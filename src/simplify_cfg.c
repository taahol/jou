#include "jou_compiler.h"
#include <limits.h>

static int find_block_index(const CfGraph *cfg, const CfBlock *b)
{
    for (int i = 0; i < cfg->all_blocks.len; i++)
        if (cfg->all_blocks.ptr[i] == b)
            return i;
    assert(0);
}

static int find_var_index(const CfGraph *cfg, const LocalVariable *v)
{
    for (int i = 0; i < cfg->locals.len; i++)
        if (cfg->locals.ptr[i] == v)
            return i;
    assert(0);
}

enum VarStatus {
    VS_UNVISITED = 0,  // Don't know anything about this variable yet.
    VS_TRUE,  // This is a boolean variable that is set to True.
    VS_FALSE,  // This is a boolean variable that is set to False.
    VS_DEFINED,  // This variable (boolean or other) has been set to some non-garbage value.
    VS_POSSIBLY_UNDEFINED,  // Could hold a garbage value or a non-garbage value.
    VS_UNDEFINED,  // No value has been set to the variable. Holds a garbage value.
    VS_UNPREDICTABLE,  // Address of variable (&foo) has been used. Give up analzing it.
    /*
    Longer description of VS_UNPREDICTABLE: The value of an unpredictable variable can
    change in lines of code that seem to have nothing to do with the variable. For
    example, a function that doesn't take &foo as an argument can change the value of foo,
    because the pointer &foo can be stored in some place that the function can access.
    */
};

/*
a and b are statuses from different branches that both jump to the same block.
Should have these properties:

    merge(a, VS_UNVISITED) == a
    merge(a, a) == a
    merge(a, b) == merge(b, a)
    merge(a, merge(b, c)) == merge(merge(a, b), c)

In other words:
- It makes sense to merge an unordered collection of statuses.
- VS_UNVISITED corresponds with merging an empty set of statuses.
- Having the same status several times doesn't affect anything.
*/
static enum VarStatus merge(enum VarStatus a, enum VarStatus b)
{
    // Unvisited --> use the other status
    if (a == VS_UNVISITED) return b;
    if (b == VS_UNVISITED) return a;

    // If any value in a merge is unpredictable or undefined, then the result is also
    // unpredictable/undefined.
    // If there are unpredictable and undefined values, the merge is unpredictable.
    if (a == VS_UNPREDICTABLE || b == VS_UNPREDICTABLE) return VS_UNPREDICTABLE;
    if (a == VS_UNDEFINED && b == VS_UNDEFINED) return VS_UNDEFINED;
    if (a == VS_UNDEFINED || b == VS_UNDEFINED || a == VS_POSSIBLY_UNDEFINED || b == VS_POSSIBLY_UNDEFINED) return VS_POSSIBLY_UNDEFINED;

    // At this point we know that the value is set to something. We may or may not know
    // what it is set to.
    assert(a == VS_TRUE || a == VS_FALSE || a == VS_DEFINED);
    assert(b == VS_TRUE || b == VS_FALSE || b == VS_DEFINED);
    if (a == VS_TRUE && b == VS_TRUE) return VS_TRUE;
    if (a == VS_FALSE && b == VS_FALSE) return VS_FALSE;
    return VS_DEFINED;
}

static bool merge_arrays_in_place(enum VarStatus *dest, const enum VarStatus *src, int n)
{
    bool did_something = false;
    for (int i = 0; i < n; i++) {
        enum VarStatus m = merge(src[i], dest[i]);
        if (dest[i] != m) {
            dest[i] = m;
            did_something = true;
        }
    }

    return did_something;
}

// Figure out how an instruction affects variables when it runs.
static void update_statuses_with_instruction(const CfGraph *cfg, enum VarStatus *statuses, const CfInstruction *ins)
{
    for (int i = 0; i < cfg->locals.len; i++)
        assert(statuses[i] != VS_UNVISITED);

    if (!ins->destvar)
        return;

    int destidx = find_var_index(cfg, ins->destvar);
    if (statuses[destidx] == VS_UNPREDICTABLE)
        return;

    switch(ins->kind) {
    case CF_VARCPY:
        statuses[destidx] = statuses[find_var_index(cfg, ins->operands[0])];
        if (statuses[destidx] == VS_UNPREDICTABLE) {
            // Assume that unpredictable variables always yield non-garbage values.
            // Otherwise using functions like scanf() would be annoying.
            statuses[destidx] = VS_DEFINED;
        }
        break;
    case CF_ADDRESS_OF_LOCAL_VAR:
        statuses[find_var_index(cfg, ins->operands[0])] = VS_UNPREDICTABLE;
        statuses[destidx] = VS_DEFINED;
        break;
    case CF_CONSTANT:
        if (ins->data.constant.kind == CONSTANT_BOOL)
            statuses[destidx] = ins->data.constant.data.boolean ? VS_TRUE : VS_FALSE;
        else
            statuses[destidx] = VS_DEFINED;
        break;
    default:
        statuses[destidx] = VS_DEFINED;
        break;
    }
}

#define DebugPrint 0  // change to 1 to see VarStatuses

#if DebugPrint
static const char * vs_to_string(enum VarStatus vs)
{
    switch(vs){
        case VS_UNVISITED: return "unvisited";
        case VS_TRUE: return "true";
        case VS_FALSE: return "false";
        case VS_DEFINED: return "defined";
        case VS_POSSIBLY_UNDEFINED: return "possibly undef";
        case VS_UNDEFINED: return "undef";
        case VS_UNPREDICTABLE: return "unpredictable";
    }
    assert(0);
}
static void print_var_statuses(const CfGraph *cfg, enum VarStatus **statuses, const enum VarStatus *temp, const char *description)
{
    int nblocks = cfg->all_blocks.len;
    int nvars = cfg->variables.len;

    puts(description);
    for (int blockidx = 0; blockidx < nblocks; blockidx++) {
        printf("  block %d:\n", blockidx);
        for (int i = 0; i < nvars; i++)
            printf("    %-15s  %s\n", cfg->variables.ptr[i]->name, vs_to_string(statuses[blockidx][i]));
    }
    if(temp) {
        printf("  temp:\n");
        for (int i = 0; i < nvars; i++)
            printf("    %-15s  %s\n", cfg->variables.ptr[i]->name, vs_to_string(temp[i]));
    }
    printf("\n");
}
#endif  // DebugPrint

static bool all_zero(const char *ptr, int n)
{
    for (int i = 0; i < n; i++)
        if (ptr[i])
            return false;
    return true;
}

/*
Figure out whether boolean variables are true or false.
return_value[block_index][variable_index] = status of variable at END of block
variable_index must be so that the variable has type bool.

Idea: Initially mark everything as possible, so all variables can be true or false.
Loop through instructions of start block, filtering out impossible options: for
example, if a variable is set to True, then it can be True and cannot be False.
Repeat for blocks where execution jumps from the current block, unless we got same
result as last time, then we know that we don't have to reanalyze blocks where
execution jumps from the current block.
*/
static enum VarStatus **determine_var_statuses(const CfGraph *cfg)
{
#if DebugPrint
    print_control_flow_graph(cfg);
    printf("\n");
#endif

    int nblocks = cfg->all_blocks.len;
    int nvars = cfg->locals.len;

    enum VarStatus **result = malloc(sizeof(result[0]) * nblocks);  // NOLINT
    for (int i = 0; i < nblocks; i++)
        result[i] = calloc(sizeof(result[i][0]), nvars);

    char *blocks_to_visit = calloc(1, nblocks);
    blocks_to_visit[0] = true;  // visit initial block

    enum VarStatus *tempstatus = malloc(nvars*sizeof(tempstatus[0]));

    while(!all_zero(blocks_to_visit, nblocks)){
        // Find a block to visit.
        int visiting = 0;
        while (!blocks_to_visit[visiting]) visiting++;
#if DebugPrint
        printf("=== Visit block %d ===\n", visiting);
#endif
        blocks_to_visit[visiting] = false;
        const CfBlock *visitingblock = cfg->all_blocks.ptr[visiting];

        // Determine initial values based on other blocks that jump here.
        for (int i = 0; i < nvars; i++) {
            if (visiting == 0) {
                // start block
                tempstatus[i] = cfg->locals.ptr[i]->is_argument ? VS_DEFINED : VS_UNDEFINED;
            } else {
                // What is possible in other blocks is determined based on only how
                // they are jumped into.
                tempstatus[i] = VS_UNVISITED;
            }
        }

#if DebugPrint
        print_var_statuses(cfg, result, tempstatus, "Initial");
#endif

        for (int i = 0; i < nblocks; i++) {
            if (cfg->all_blocks.ptr[i]->iftrue == visitingblock
                || cfg->all_blocks.ptr[i]->iffalse == visitingblock)
            {
                // TODO: If we only get here from the true jump, or only from false
                // jump, we could assume that the variable used in the jump was true/false.
                merge_arrays_in_place(tempstatus, result[i], nvars);
            }
        }

#if DebugPrint
        print_var_statuses(cfg, result, tempstatus, "After adding from other blocks to temp");
#endif

        // Turn the initial status into status at end of the block.
        const CfInstruction *ins;
        for (ins = visitingblock->instructions.ptr; ins < End(visitingblock->instructions); ins++)
        {
            update_statuses_with_instruction(cfg, tempstatus, ins);
        }

        // Update what we learned about variable status at end of this block.
        bool result_affected = merge_arrays_in_place(result[visiting], tempstatus, nvars);
#if DebugPrint
        print_var_statuses(cfg, result, NULL, "At end");
#endif

        if (result_affected && visitingblock != &cfg->end_block) {
            // Also need to update blocks where we jump from here.
#if DebugPrint
            printf("  Will visit %d and %d\n", find_block_index(cfg, visitingblock->iftrue), find_block_index(cfg, visitingblock->iffalse));
#endif
            blocks_to_visit[find_block_index(cfg, visitingblock->iftrue)] = true;
            blocks_to_visit[find_block_index(cfg, visitingblock->iffalse)] = true;
        }
    }

    free(blocks_to_visit);
    free(tempstatus);
    return result;
}

static void free_var_statuses(const CfGraph *cfg, enum VarStatus** statuses)
{
    for (int i = 0; i < cfg->all_blocks.len; i++)
        free(statuses[i]);
    free(statuses);
}

static void clean_jumps_where_condition_always_true_or_always_false(CfGraph *cfg)
{
    enum VarStatus **statuses = determine_var_statuses(cfg);

    for (int blockidx = 0; blockidx < cfg->all_blocks.len; blockidx++) {
        CfBlock *block = cfg->all_blocks.ptr[blockidx];
        if (block == &cfg->end_block || block->iftrue == block->iffalse)
            continue;

        switch(statuses[blockidx][find_var_index(cfg, block->branchvar)]) {
        case VS_TRUE:
            // Always jump to true case.
            block->iffalse = block->iftrue;
            break;
        case VS_FALSE:
            // Always jump to false case.
            block->iftrue = block->iffalse;
            break;
        default:
            break;
        }
    }
    free_var_statuses(cfg, statuses);
}

/*
Two blocks will end up in the same group, if there is an execution path from one block to another.
Return value: array of groups, each group is an array of CfBlock pointers.
All returned arrays are NULL terminated.

This is not very efficient code, but it's only used for unreachable blocks.
In a typical program, I don't expect to have many unreachable blocks.
*/
static CfBlock ***group_blocks(CfBlock **blocks, int nblocks)
{
    CfBlock ***groups = calloc(sizeof(groups[0]), nblocks+1);  // NOLINT

    // Initially there is a separate group for each block.
    int ngroups = nblocks;
    for (int i = 0; i < nblocks; i++) {
        groups[i] = calloc(sizeof(groups[i][0]), nblocks + 1); // NOLINT
        groups[i][0] = blocks[i];
    }

    // For each block, we need to check whether that block can jump outside its
    // group. When that does, merge the two groups together.
    for (int block1index = 0; block1index < nblocks; block1index++) {
        CfBlock *block1 = blocks[block1index];
        if (block1->iffalse==NULL && block1->iftrue==NULL)
            continue;  // the end block

        for (int m = 0; m < 2; m++) {
            CfBlock *block2 = m ? block1->iffalse : block1->iftrue;
            assert(block2);

            // Find groups of block1 and block2.
            CfBlock ***g1 = NULL, ***g2 = NULL;
            for (int i = 0; groups[i]; i++) {
                for (int k = 0; groups[i][k]; k++) {
                    if (groups[i][k] == block1) g1=&groups[i];
                    if (groups[i][k] == block2) g2=&groups[i];
                }
            }

            if (g1 && g2 && *g1!=*g2) {
                // Append all blocks from group 2 to group 1.
                CfBlock **dest = *g1, **src = *g2;
                while (*dest) dest++;
                while ((*dest++ = *src++)) ;

                // Delete group 2.
                free(*g2);
                *g2 = groups[--ngroups];
                groups[ngroups] = NULL;
            }
        }
    }

    return groups;
}

static void show_unreachable_warnings(CfBlock **unreachable_blocks, int n_unreachable_blocks)
{
    // Show a warning in the beginning of each group of blocks.
    // Can't show a warning for each block, that would be too noisy.
    CfBlock ***groups = group_blocks(unreachable_blocks, n_unreachable_blocks);

    // Prevent showing two errors on the same line, even if from different groups
    int prev_lineno = -1;

    for (int groupidx = 0; groups[groupidx]; groupidx++) {
        Location first_location = { .lineno = INT_MAX };
        for (int blockidx = 0; groups[groupidx][blockidx]; blockidx++) {
            CfBlock *block = groups[groupidx][blockidx];
            for (const CfInstruction *ins = block->instructions.ptr; ins < End(block->instructions); ins++)
                if (!ins->hide_unreachable_warning && ins->location.lineno < first_location.lineno)
                    first_location = ins->location;
        }

        if (first_location.lineno != INT_MAX && first_location.lineno != prev_lineno) {
            show_warning(first_location, "this code will never run");
            prev_lineno = first_location.lineno;
        }
    }

    for (int i = 0; groups[i]; i++)
        free(groups[i]);
    free(groups);
}

static void remove_given_blocks(CfGraph *cfg, CfBlock **blocks_to_remove, int n_blocks_to_remove)
{
    for (int i = cfg->all_blocks.len - 1; i >= 0; i--) {
        bool shouldgo = false;
        for (CfBlock **b = blocks_to_remove; b < &blocks_to_remove[n_blocks_to_remove]; b++) {
            if(*b==cfg->all_blocks.ptr[i]){
                shouldgo=true;
                break;
            }
        }
        if(shouldgo)
        {
            free_control_flow_graph_block(cfg, cfg->all_blocks.ptr[i]);
            cfg->all_blocks.ptr[i]=Pop(&cfg->all_blocks);
        }
    }
}

static void remove_unreachable_blocks(CfGraph *cfg)
{
    bool *reachable = calloc(sizeof(reachable[0]), cfg->all_blocks.len);
    List(int) todo = {0};
    Append(&todo, 0);  // start block

    while (todo.len != 0) {
        int i = Pop(&todo);
        if (reachable[i])
            continue;
        reachable[i] = true;

        if (cfg->all_blocks.ptr[i] != &cfg->end_block) {
            Append(&todo, find_block_index(cfg, cfg->all_blocks.ptr[i]->iftrue));
            Append(&todo, find_block_index(cfg, cfg->all_blocks.ptr[i]->iffalse));
        }
    }
    free(todo.ptr);

    List(CfBlock *) blocks_to_remove = {0};
    for (int i = 0; i < cfg->all_blocks.len; i++)
        if (!reachable[i] && cfg->all_blocks.ptr[i] != &cfg->end_block)
            Append(&blocks_to_remove, cfg->all_blocks.ptr[i]);
    free(reachable);

    show_unreachable_warnings(blocks_to_remove.ptr, blocks_to_remove.len);
    remove_given_blocks(cfg, blocks_to_remove.ptr, blocks_to_remove.len);
    free(blocks_to_remove.ptr);
}

static void remove_unused_variables(CfGraph *cfg)
{
    char *used = calloc(1, cfg->locals.len);

    for (CfBlock **b = cfg->all_blocks.ptr; b < End(cfg->all_blocks); b++) {
        for (CfInstruction *ins = (*b)->instructions.ptr; ins < End((*b)->instructions); ins++) {
            if (ins->destvar)
                used[find_var_index(cfg, ins->destvar)] = true;
            for (int i = 0; i < ins->noperands; i++)
                used[find_var_index(cfg, ins->operands[i])] = true;
        }
    }

    for (int i = cfg->locals.len - 1; i>=0; i--) {
        if (!used[i] && !cfg->locals.ptr[i]->is_argument) {
            free(cfg->locals.ptr[i]);
            cfg->locals.ptr[i] = Pop(&cfg->locals);
        }
    }

    free(used);
}

static void warn_about_undefined_variables(CfGraph *cfg)
{
    enum VarStatus **statuses = determine_var_statuses(cfg);

    for (int blockidx = 0; blockidx < cfg->all_blocks.len; blockidx++) {
        const CfBlock *b = cfg->all_blocks.ptr[blockidx];
        enum VarStatus *status = statuses[blockidx];
        for (CfInstruction *ins = b->instructions.ptr; ins < End(b->instructions); ins++) {
            for (int i = 0; i < ins->noperands; i++) {
                switch(status[find_var_index(cfg, ins->operands[i])]) {
                case VS_UNVISITED:
                    assert(0);
                case VS_TRUE:
                case VS_FALSE:
                case VS_DEFINED:
                case VS_UNPREDICTABLE:
                    break;
                case VS_POSSIBLY_UNDEFINED:
                    /*
                    The compiler internally creates anonymous variables.
                    They can be undefined if a user's undefined variable is copied to them.
                    But in that case we get a warning from the user's variable anyway.
                    */
                    if (ins->operands[i]->name[0])
                        show_warning(ins->location, "the value of '%s' may be undefined", ins->operands[i]->name);
                    break;
                case VS_UNDEFINED:
                    if (ins->operands[i]->name[0])
                        show_warning(ins->location, "the value of '%s' is undefined", ins->operands[i]->name);
                    break;
                }
            }
            update_statuses_with_instruction(cfg, status, ins);
        }
    }

    free_var_statuses(cfg, statuses);
}

static void error_about_missing_return(CfGraph *cfg)
{
    if (!cfg->signature.returntype)
        return;

    enum VarStatus **statuses = determine_var_statuses(cfg);

    // When a function returns a value, it is stored in a variable named "return".
    int varidx = -1;
    for (int i = 0; i < cfg->locals.len; i++) {
        if (!strcmp(cfg->locals.ptr[i]->name, "return")) {
            varidx = i;
            break;
        }
    }
    assert(varidx != -1);

    enum VarStatus s = statuses[find_block_index(cfg, &cfg->end_block)][varidx];
    if (s == VS_POSSIBLY_UNDEFINED) {
        show_warning(
            cfg->signature.returntype_location,
            "function '%s' doesn't seem to return a value in all cases", cfg->signature.name);
    }
    if (s == VS_UNDEFINED) {
        fail_with_error(
            cfg->signature.returntype_location,
            "function '%s' must return a value, because it is defined with '-> %s'",
            cfg->signature.name, cfg->signature.returntype->name);
    }

    free_var_statuses(cfg, statuses);
}

static void simplify_cfg(CfGraph *cfg)
{
    clean_jumps_where_condition_always_true_or_always_false(cfg);
    remove_unreachable_blocks(cfg);
    error_about_missing_return(cfg);
    remove_unused_variables(cfg);
    warn_about_undefined_variables(cfg);
}

void simplify_control_flow_graphs(const CfGraphFile *cfgfile)
{
    for (CfGraph **cfg = cfgfile->graphs.ptr; cfg < End(cfgfile->graphs); cfg++)
        simplify_cfg(*cfg);
}
