#include <assert.h>
#include <libgen.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "jou_compiler.h"
#include "util.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Transforms/PassManagerBuilder.h>


static void optimize(LLVMModuleRef module, int level)
{
    assert(1 <= level && level <= 3);

    LLVMPassManagerRef pm = LLVMCreatePassManager();

    /*
    The default settings should be fine for Jou because they work well for
    C and C++, and Jou is quite similar to C.
    */
    LLVMPassManagerBuilderRef pmbuilder = LLVMPassManagerBuilderCreate();
    LLVMPassManagerBuilderSetOptLevel(pmbuilder, level);
    LLVMPassManagerBuilderPopulateModulePassManager(pmbuilder, pm);
    LLVMPassManagerBuilderDispose(pmbuilder);

    LLVMRunPassManager(pm, module);
    LLVMDisposePassManager(pm);
}

static const char help_fmt[] =
    "Usage:\n"
    "  <argv0> [-o OUTFILE] [-O0|-O1|-O2|-O3] [--verbose] [--linker-flags \"...\"] FILENAME\n"
    "  <argv0> --help       # This message\n"
    "  <argv0> --update     # Download and install the latest Jou\n"
    "\n"
    "Options:\n"
    "  -o OUTFILE       output an executable file, don't run the code\n"
    "  -O0/-O1/-O2/-O3  set optimization level (0 = default, 3 = runs fastest)\n"
    "  -v / --verbose   display some progress information\n"
    "  -vv              display a lot of information about all compilation steps\n"
    "  --tokenize-only  display only the output of the tokenizer, don't do anything else\n"
    "  --parse-only     display only the AST (parse tree), don't do anything else\n"
    "  --linker-flags   appended to the linker command, so you can use external libraries\n"
    ;

struct CommandLineArgs command_line_args;

void parse_arguments(int argc, char **argv)
{
    memset(&command_line_args, 0, sizeof command_line_args);
    command_line_args.argv0 = argv[0];
    command_line_args.optlevel = 1; /* Set default optimize to O1
                            User sets optimize will overwrite the default flag
                         */

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        // Print help.
        const char *s = help_fmt;
        while (*s) {
            if (!strncmp(s, "<argv0>", 7)) {
                printf("%s", argv[0]);
                s += 7;
            } else {
                putchar(*s++);
            }
        }
        exit(0);
    }

    if (argc == 2 && !strcmp(argv[1], "--update")) {
        update_jou_compiler();
        exit(0);
    }

    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "--update")) {
            fprintf(stderr, "%s: \"%s\" cannot be used with other arguments", argv[0], argv[i]);
            goto wrong_usage;
        } else if (!strcmp(argv[i], "--verbose")) {
            command_line_args.verbosity++;
            i++;
        } else if (strncmp(argv[i], "-v", 2) == 0 && strspn(argv[i] + 1, "v") == strlen(argv[i])-1) {
            command_line_args.verbosity += strlen(argv[i]) - 1;
            i++;
        } else if (!strcmp(argv[i], "--tokenize-only")) {
            if (argc > 3) {
                fprintf(stderr, "%s: --tokenize-only cannot be used together with other flags", argv[0]);
                goto wrong_usage;
            }
            command_line_args.tokenize_only = true;
            i++;
        } else if (!strcmp(argv[i], "--parse-only")) {
            if (argc > 3) {
                fprintf(stderr, "%s: --parse-only cannot be used together with other flags", argv[0]);
                goto wrong_usage;
            }
            command_line_args.parse_only = true;
            i++;
        } else if (!strcmp(argv[i], "--linker-flags")) {
            if (command_line_args.linker_flags) {
                fprintf(stderr, "%s: --linker-flags cannot be given multiple times", argv[0]);
                goto wrong_usage;
            }
            if (argc-i < 2) {
                fprintf(stderr, "%s: there must be a string of flags after --linker-flags", argv[0]);
                goto wrong_usage;
            }
            command_line_args.linker_flags = argv[i+1];
            i += 2;
        } else if (strlen(argv[i]) == 3
                && !strncmp(argv[i], "-O", 2)
                && argv[i][2] >= '0'
                && argv[i][2] <= '3')
        {
            command_line_args.optlevel = argv[i][2] - '0';
            i++;
        } else if (!strcmp(argv[i], "-o")) {
            if (argc-i < 2) {
                fprintf(stderr, "%s: there must be a file name after -o", argv[0]);
                goto wrong_usage;
            }
            command_line_args.outfile = argv[i+1];
            if (strlen(command_line_args.outfile) > 4 && !strcmp(&command_line_args.outfile[strlen(command_line_args.outfile)-4], ".jou")) {
                fprintf(stderr, "%s: the filename after -o should be an executable, not a Jou file", argv[0]);
                goto wrong_usage;
            }
            i += 2;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown argument \"%s\"", argv[0], argv[i]);
            goto wrong_usage;
        } else if (command_line_args.infile) {
            fprintf(stderr, "%s: you can only pass one Jou file", argv[0]);
            goto wrong_usage;
        } else {
            command_line_args.infile = argv[i++];
        }
    }

    if (!command_line_args.infile) {
        fprintf(stderr, "%s: missing Jou file name", argv[0]);
        goto wrong_usage;
    }
    return;

wrong_usage:
    fprintf(stderr, " (try \"%s --help\")\n", argv[0]);
    exit(2);
}


struct FileState {
    char *path;
    AstToplevelNode *ast;
    FileTypes types;
    LLVMModuleRef module;
    ExportSymbol *pending_exports;
};

struct ParseQueueItem {
    const char *filename;
    Location import_location;
};

struct CompileState {
    const char *stdlib_path;
    List(struct FileState) files;
    List(struct ParseQueueItem) parse_queue;
};

static struct FileState *find_file(const struct CompileState *compst, const char *path)
{
    for (struct FileState *fs = compst->files.ptr; fs < End(compst->files); fs++)
        if (!strcmp(fs->path, path))
            return fs;
    return NULL;
}

static FILE *open_the_file(const char *path, const Location *import_location)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (import_location)
            fail_with_error(*import_location, "cannot import from \"%s\": %s", path, strerror(errno));
        else
            fail_with_error((Location){.filename=path}, "cannot open file: %s", strerror(errno));
    }
    return f;
}

static void parse_file(struct CompileState *compst, const char *filename, const Location *import_location)
{
    if (find_file(compst, filename))
        return;  // already parsed this file

    struct FileState fs = { .path = strdup(filename) };

    if(command_line_args.verbosity >= 2)
        printf("Tokenizing %s\n", filename);
    FILE *f = open_the_file(fs.path, import_location);
    Token *tokens = tokenize(f, fs.path);
    fclose(f);
    if(command_line_args.verbosity >= 2)
        print_tokens(tokens);

    if(command_line_args.verbosity >= 2)
        printf("Parsing %s\n", filename);
    fs.ast = parse(tokens, compst->stdlib_path);
    free_tokens(tokens);
    if(command_line_args.verbosity >= 2)
        print_ast(fs.ast);

    for (AstToplevelNode *impnode = fs.ast; impnode->kind == AST_TOPLEVEL_IMPORT; impnode++) {
        Append(&compst->parse_queue, (struct ParseQueueItem){
            .filename = impnode->data.import.path,
            .import_location = impnode->location,
        });
    }

    Append(&compst->files, fs);
}

static void parse_all_pending_files(struct CompileState *compst)
{
    while (compst->parse_queue.len > 0) {
        struct ParseQueueItem it = Pop(&compst->parse_queue);
        parse_file(compst, it.filename, &it.import_location);
    }
    free(compst->parse_queue.ptr);
}

static void compile_ast_to_llvm(struct FileState *fs)
{
    if (command_line_args.verbosity >= 1)
        printf("Compile to LLVM IR: %s\n", fs->path);

    if (command_line_args.verbosity >= 2)
        printf("Building CFG: %s\n", fs->path);

    CfGraphFile cfgfile = build_control_flow_graphs(fs->ast, &fs->types);
    for (AstToplevelNode *imp = fs->ast; imp->kind == AST_TOPLEVEL_IMPORT; imp++)
        if (!imp->data.import.used)
            show_warning(imp->location, "'%s' imported but not used", imp->data.import.symbolname);

    if(command_line_args.verbosity >= 2)
        print_control_flow_graphs(&cfgfile);

    simplify_control_flow_graphs(&cfgfile);
    if(command_line_args.verbosity >= 2)
        print_control_flow_graphs(&cfgfile);

    if (command_line_args.verbosity >= 2)
        printf("Build LLVM IR: %s\n", fs->path);

    fs->module = codegen(&cfgfile, &fs->types);
    free_control_flow_graphs(&cfgfile);

    if (command_line_args.verbosity >= 2)
        print_llvm_ir(fs->module, false);

    /*
    If this fails, it is not just users writing dumb code, it is a bug in this compiler.
    This compiler should always fail with an error elsewhere, or generate valid LLVM IR.
    */
    LLVMVerifyModule(fs->module, LLVMAbortProcessAction, NULL);

    if (command_line_args.optlevel) {
        if (command_line_args.verbosity >= 2)
            printf("Optimizing %s (level %d)\n", fs->path, command_line_args.optlevel);
        optimize(fs->module, command_line_args.optlevel);
        if(command_line_args.verbosity >= 2)
            print_llvm_ir(fs->module, true);
    }
}

static char *find_stdlib()
{
    char *exe = find_current_executable();
#ifdef _WIN32
    simplify_path(exe);
#endif
    const char *exedir = dirname(exe);

    char *path = malloc(strlen(exedir) + 10);
    strcpy(path, exedir);
    strcat(path, "/stdlib");
    free(exe);

    char *iojou = malloc(strlen(path) + 10);
    sprintf(iojou, "%s/io.jou", path);
    struct stat st;
    if (stat(iojou, &st) != 0) {
        fprintf(stderr, "error: cannot find the Jou standard library in %s\n", path);
        exit(1);
    }
    free(iojou);

    return path;
}

static bool astnode_conflicts_with_an_import(const AstToplevelNode *astnode, const ExportSymbol *import)
{
    switch(astnode->kind) {
    case AST_TOPLEVEL_DECLARE_FUNCTION:
    case AST_TOPLEVEL_DEFINE_FUNCTION:
        return import->kind == EXPSYM_FUNCTION && !strcmp(import->name, astnode->data.funcdef.signature.name);
    case AST_TOPLEVEL_DECLARE_GLOBAL_VARIABLE:
    case AST_TOPLEVEL_DEFINE_GLOBAL_VARIABLE:
        return import->kind == EXPSYM_GLOBAL_VAR && !strcmp(import->name, astnode->data.globalvar.name);
    case AST_TOPLEVEL_DEFINE_CLASS:
        return import->kind == EXPSYM_TYPE && !strcmp(import->name, astnode->data.classdef.name);
    case AST_TOPLEVEL_DEFINE_ENUM:
        return import->kind == EXPSYM_TYPE && !strcmp(import->name, astnode->data.enumdef.name);
    case AST_TOPLEVEL_IMPORT:
        return false;
    case AST_TOPLEVEL_END_OF_FILE:
        assert(0);
    }
}

static void add_imported_symbol(struct FileState *fs, const ExportSymbol *es, AstImport *imp)
{
    for (AstToplevelNode *ast = fs->ast; ast->kind != AST_TOPLEVEL_END_OF_FILE; ast++) {
        if (astnode_conflicts_with_an_import(ast, es)) {
            const char *wat;
            switch(es->kind) {
                case EXPSYM_FUNCTION: wat = "function"; break;
                case EXPSYM_GLOBAL_VAR: wat = "global variable"; break;
                case EXPSYM_TYPE: wat = "type"; break;
            }
            fail_with_error(ast->location, "a %s named '%s' already exists", wat, es->name);
        }
    }

    struct GlobalVariable *g;

    switch(es->kind) {
    case EXPSYM_FUNCTION:
        Append(&fs->types.functions, (struct SignatureAndUsedPtr){
            .signature = copy_signature(&es->data.funcsignature),
            .usedptr = &imp->used,
        });
        break;
    case EXPSYM_TYPE:
        Append(&fs->types.types, (struct TypeAndUsedPtr){
            .type=es->data.type,
            .usedptr=&imp->used,
        });
        break;
    case EXPSYM_GLOBAL_VAR:
        g = calloc(1, sizeof(*g));
        g->type = es->data.type;
        g->usedptr = &imp->used;
        assert(strlen(es->name) < sizeof g->name);
        strcpy(g->name, es->name);
        Append(&fs->types.globals, g);
        break;
    }
}

static bool exportsymbol_matches_import(const ExportSymbol *es, const AstImport *imp)
{
    // Method bar in struct Foo appears as a function with name "Foo.bar". Match it when importing Foo.
    return !strcmp(es->name, imp->symbolname) || (strlen(es->name) > strlen(imp->symbolname) && es->name[strlen(imp->symbolname)] == '.');
}

static void add_imported_symbols(struct CompileState *compst)
{
    // TODO: should it be possible for a file to import from itself?
    // Should fail with error?
    for (struct FileState *to = compst->files.ptr; to < End(compst->files); to++) {
        for (AstToplevelNode *ast = to->ast; ast->kind == AST_TOPLEVEL_IMPORT; ast++) {
            AstImport *imp = &ast->data.import;
            struct FileState *from = find_file(compst, imp->path);
            assert(from);

            for (struct ExportSymbol *es = from->pending_exports; es->name[0]; es++) {
                if (exportsymbol_matches_import(es, imp)) {
                    if (command_line_args.verbosity >= 2) {
                        const char *kindstr;
                        switch(es->kind) {
                            case EXPSYM_FUNCTION: kindstr="function"; break;
                            case EXPSYM_GLOBAL_VAR: kindstr="global var"; break;
                            case EXPSYM_TYPE: kindstr="type"; break;
                        }
                        printf("Adding imported %s %s: %s --> %s\n",
                            kindstr, es->name, from->path, to->path);
                    }
                    imp->found = true;
                    add_imported_symbol(to, es, imp);
                }
            }
        }
    }

    // Mark all exports as no longer pending.
    for (struct FileState *fs = compst->files.ptr; fs < End(compst->files); fs++) {
        for (struct ExportSymbol *es = fs->pending_exports; es->name[0]; es++)
            free_export_symbol(es);
        free(fs->pending_exports);
        fs->pending_exports = NULL;
    }
}

/*
Check whether each import statement in AST actually imported something.

This is trickier than you would expect, because multiple passes over
the AST look at the imports, and any of them could provide the symbol to import.
*/
static void check_for_404_imports(const struct CompileState *compst)
{
    for (struct FileState *fs = compst->files.ptr; fs < End(compst->files); fs++) {
        for (const AstToplevelNode *imp = fs->ast; imp->kind == AST_TOPLEVEL_IMPORT; imp++) {
            if (!imp->data.import.found) {
                fail_with_error(
                    imp->location, "file \"%s\" does not export a symbol named '%s'",
                    imp->data.import.path, imp->data.import.symbolname);
            }
        }
    }
}

int main(int argc, char **argv)
{
    init_target();
    init_types();
    char *stdlib = find_stdlib();
    parse_arguments(argc, argv);

    struct CompileState compst = { .stdlib_path = stdlib };
    if (command_line_args.verbosity >= 2) {
        printf("Target triple: %s\n", get_target()->triple);
        printf("Data layout: %s\n", get_target()->data_layout);
    }

    if (command_line_args.tokenize_only || command_line_args.parse_only) {
        FILE *f = open_the_file(command_line_args.infile, NULL);
        Token *tokens = tokenize(f, command_line_args.infile);
        fclose(f);
        if (command_line_args.tokenize_only) {
            print_tokens(tokens);
        } else {
            AstToplevelNode *ast = parse(tokens, compst.stdlib_path);
            print_ast(ast);
            free_ast(ast);
        }
        free_tokens(tokens);
        return 0;
    }

    if (command_line_args.verbosity >= 1)
        printf("Parsing Jou files...\n");

#ifdef _WIN32
    char *startup_path = malloc(strlen(compst.stdlib_path) + 50);
    sprintf(startup_path, "%s/_windows_startup.jou", compst.stdlib_path);
    parse_file(&compst, startup_path, NULL);
    free(startup_path);
#endif

    parse_file(&compst, command_line_args.infile, NULL);
    parse_all_pending_files(&compst);

    if (command_line_args.verbosity >= 1)
        printf("Type-checking...\n");

    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++) {
        if (command_line_args.verbosity >= 2)
            printf("Typecheck stage 1: %s\n", fs->path);
        fs->pending_exports = typecheck_stage1_create_types(&fs->types, fs->ast);
    }
    add_imported_symbols(&compst);
    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++) {
        if (command_line_args.verbosity >= 2)
            printf("Typecheck stage 2: %s\n", fs->path);
        fs->pending_exports = typecheck_stage2_signatures_globals_structbodies(&fs->types, fs->ast);
    }
    add_imported_symbols(&compst);
    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++) {
        if (command_line_args.verbosity >= 2)
            printf("Typecheck stage 3: %s\n", fs->path);
        typecheck_stage3_function_and_method_bodies(&fs->types, fs->ast);
    }

    check_for_404_imports(&compst);

    char **objpaths = calloc(sizeof objpaths[0], compst.files.len + 1);
    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++) {
        compile_ast_to_llvm(fs);
        objpaths[fs - compst.files.ptr] = compile_to_object_file(fs->module);
    }

    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++) {
        LLVMDisposeModule(fs->module);
        free_ast(fs->ast);
        fs->ast = NULL;
        free(fs->path);
        free_file_types(&fs->types);
    }
    free(compst.files.ptr);
    free(stdlib);

    char *exepath;
    if (command_line_args.outfile)
        exepath = strdup(command_line_args.outfile);
    else
        exepath = get_default_exe_path();

    run_linker((const char *const*)objpaths, exepath);
    for (int i = 0; objpaths[i]; i++)
        free(objpaths[i]);
    free(objpaths);

    int ret = 0;
    if (!command_line_args.outfile) {
        if(command_line_args.verbosity >= 1)
            printf("Run: %s\n", exepath);
        ret = run_exe(exepath);
    }

    free(exepath);

    return ret;
}
