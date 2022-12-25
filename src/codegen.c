#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <llvm-c/Core.h>
#include <llvm-c/Types.h>
#include "jou_compiler.h"
#include "util.h"

struct LocalVariable {
    char name[100];
    LLVMValueRef pointer;
};

struct State {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    const struct AstFunctionSignature *current_func_signature;
    LLVMValueRef current_func;
    List(struct LocalVariable) local_vars;
};

static LLVMTypeRef codegen_type(const struct State *st, const struct Type *type, struct Location location)
{
    switch(type->kind) {
    case TYPE_POINTER:
        return LLVMPointerType(codegen_type(st, type->data.valuetype, location), 0);
    case TYPE_SIGNED_INTEGER:
    case TYPE_UNSIGNED_INTEGER:
        return LLVMIntType(type->data.width_in_bits);
    case TYPE_BOOL:
        return LLVMInt1Type();
    case TYPE_UNKNOWN:
        assert(0);
    }
    assert(0);
}

static LLVMValueRef codegen_function_decl(const struct State *st, const struct AstFunctionSignature *sig)
{
    LLVMTypeRef *argtypes = malloc(sig->nargs * sizeof(argtypes[0]));  // NOLINT
    for (int i = 0; i < sig->nargs; i++)
        argtypes[i] = codegen_type(st, &sig->argtypes[i], sig->location);

    LLVMTypeRef returntype;
    if (sig->returntype == NULL)
        returntype = LLVMVoidType();
    else
        returntype = codegen_type(st, sig->returntype, sig->location);

    LLVMTypeRef functype = LLVMFunctionType(returntype, argtypes, sig->nargs, false);
    free(argtypes);

    return LLVMAddFunction(st->module, sig->funcname, functype);
}

// forward-declare
LLVMValueRef codegen_call(const struct State *st, const struct AstCall *call, struct Location location);

LLVMValueRef codegen_expression(const struct State *st, const struct AstExpression *expr)
{
    switch(expr->kind) {
    case AST_EXPR_CALL:
        return codegen_call(st, &expr->data.call, expr->location);
    case AST_EXPR_GET_VARIABLE:
        for (struct LocalVariable *v = st->local_vars.ptr; v < End(st->local_vars); v++)
            if (!strcmp(v->name, expr->data.varname))
                return LLVMBuildLoad(st->builder, v->pointer, v->name);
        fail_with_error(expr->location, "no local variable named \"%s\"", expr->data.varname);
    case AST_EXPR_ADDRESS_OF_VARIABLE:
        for (struct LocalVariable *v = st->local_vars.ptr; v < End(st->local_vars); v++)
            if (!strcmp(v->name, expr->data.varname))
                return v->pointer;
        fail_with_error(expr->location, "no local variable named \"%s\"", expr->data.varname);
    case AST_EXPR_DEREFERENCE:
    {
        LLVMValueRef pointer = codegen_expression(st, expr->data.pointerexpr);
        if (LLVMGetTypeKind(LLVMTypeOf(pointer)) != LLVMPointerTypeKind)
            fail_with_error(expr->location, "the dereference operator '*' is only for pointers");
        return LLVMBuildLoad(st->builder, pointer, "deref");
    }
    case AST_EXPR_INT_CONSTANT:
        return LLVMConstInt(LLVMInt32Type(), expr->data.int_value, false);
    case AST_EXPR_TRUE:
        return LLVMConstInt(LLVMInt1Type(), 1, false);
    case AST_EXPR_FALSE:
        return LLVMConstInt(LLVMInt1Type(), 0, false);
    }
    assert(0);
}

LLVMValueRef codegen_call(const struct State *st, const struct AstCall *call, struct Location location)
{
    // TODO: do something with the location, debug info?
    (void)location;

    LLVMValueRef function = LLVMGetNamedFunction(st->module, call->funcname);
    assert(function);
    assert(LLVMGetTypeKind(LLVMTypeOf(function)) == LLVMPointerTypeKind);
    LLVMTypeRef function_type = LLVMGetElementType(LLVMTypeOf(function));
    assert(LLVMGetTypeKind(function_type) == LLVMFunctionTypeKind);

    LLVMValueRef *args = malloc(call->nargs * sizeof(args[0]));  // NOLINT
    for (int i = 0; i < call->nargs; i++) 
        args[i] = codegen_expression(st, &call->args[i]);

    char debug_name[100] = {0};
    if (LLVMGetTypeKind(LLVMGetReturnType(function_type)) != LLVMVoidTypeKind)
        snprintf(debug_name, sizeof debug_name, "%s_return_value", call->funcname);

    LLVMValueRef return_value = LLVMBuildCall2(st->builder, function_type, function, args, call->nargs, debug_name);
    free(args);
    return return_value;
}

static void codegen_body(const struct State *st, const struct AstBody *body);

static void codegen_if_statement(const struct State *st, const struct AstIfStatement *ifstatement)
{
    LLVMValueRef condition = codegen_expression(st, &ifstatement->condition);
    assert(LLVMTypeOf(condition) == LLVMInt1Type());

    LLVMBasicBlockRef then = LLVMAppendBasicBlock(st->current_func, "then");
    LLVMBasicBlockRef after_if = LLVMAppendBasicBlock(st->current_func, "after_if");
    LLVMBuildCondBr(st->builder, condition, then, after_if);

    LLVMPositionBuilderAtEnd(st->builder, then);
    codegen_body(st, &ifstatement->body);
    LLVMBuildBr(st->builder, after_if);

    LLVMPositionBuilderAtEnd(st->builder, after_if);
}

static void codegen_statement(const struct State *st, const struct AstStatement *stmt)
{
    switch(stmt->kind) {
    case AST_STMT_IF:
        codegen_if_statement(st, &stmt->data.ifstatement);
        break;

    case AST_STMT_CALL:
        codegen_call(st, &stmt->data.call, stmt->location);
        break;

    case AST_STMT_RETURN_VALUE:
        LLVMBuildRet(st->builder, codegen_expression(st, &stmt->data.returnvalue));
        break;

    case AST_STMT_RETURN_WITHOUT_VALUE:
        LLVMBuildRetVoid(st->builder);
        break;
    }
}

static void codegen_body(const struct State *st, const struct AstBody *body)
{
    for (int i = 0; i < body->nstatements; i++)
        codegen_statement(st, &body->statements[i]);
}

static void codegen_function_def(struct State *st, const struct AstFunctionDef *funcdef)
{
    assert(st->current_func_signature == NULL);
    assert(st->current_func == NULL);
    assert(st->local_vars.len == 0);

    st->current_func_signature = &funcdef->signature;
    st->current_func = codegen_function_decl(st, &funcdef->signature);

    LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(LLVMGetGlobalContext(), st->current_func, "function_start");
    LLVMPositionBuilderAtEnd(st->builder, block);

    for (int i = 0; i < funcdef->signature.nargs; i++) {
        LLVMValueRef value = LLVMGetParam(st->current_func, i);
        LLVMValueRef pointer = LLVMBuildAlloca(st->builder, LLVMTypeOf(value), funcdef->signature.argnames[i]);
        LLVMBuildStore(st->builder, value, pointer);
        struct LocalVariable local = { .pointer=pointer };
        safe_strcpy(local.name, funcdef->signature.argnames[i]);
        Append(&st->local_vars, local);
    }

    codegen_body(st, &funcdef->body);

    if (funcdef->signature.returntype == NULL)
        LLVMBuildRetVoid(st->builder);
    else
        LLVMBuildUnreachable(st->builder);  // TODO: compiler error if this is reachable

    assert(st->current_func_signature == &funcdef->signature);
    st->current_func_signature = NULL;
    st->current_func = NULL;
    st->local_vars.len = 0;
}

LLVMModuleRef codegen(const struct AstToplevelNode *ast)
{
    struct State st = {
        .module = LLVMModuleCreateWithName(""),  // TODO: pass module name?
        .builder = LLVMCreateBuilder(),
    };
    LLVMSetSourceFileName(st.module, ast->location.filename, strlen(ast->location.filename));

    for(;;ast++){
        switch(ast->kind) {
        case AST_TOPLEVEL_CDECL_FUNCTION:
            codegen_function_decl(&st, &ast->data.decl_signature);
            break;

        case AST_TOPLEVEL_DEFINE_FUNCTION:
            codegen_function_def(&st, &ast->data.funcdef);
            break;

        case AST_TOPLEVEL_END_OF_FILE:
            LLVMDisposeBuilder(st.builder);
            free(st.local_vars.ptr);
            return st.module;
        }
    }
}
