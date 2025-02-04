#define BH_DEBUG

#include "onyxutils.h"
#include "onyxlex.h"
#include "onyxastnodes.h"
#include "onyxerrors.h"
#include "onyxparser.h"
#include "onyxastnodes.h"
#include "onyxsempass.h"

bh_scratch global_scratch;
bh_allocator global_scratch_allocator;

bh_managed_heap global_heap;
bh_allocator global_heap_allocator;

void program_info_init(ProgramInfo* prog, bh_allocator alloc) {
    prog->global_scope = scope_create(alloc, NULL, (OnyxFilePos) { 0 });

    bh_table_init(alloc, prog->packages, 16);

    // NOTE: This will be initialized upon the first call to entity_heap_insert.
    prog->entities.entities = NULL;
}

Package* program_info_package_lookup(ProgramInfo* prog, char* package_name) {
    if (bh_table_has(Package *, prog->packages, package_name)) {
        return bh_table_get(Package *, prog->packages, package_name);
    } else {
        return NULL;
    }
}

Package* program_info_package_lookup_or_create(ProgramInfo* prog, char* package_name, Scope* parent_scope, bh_allocator alloc) {
    if (bh_table_has(Package *, prog->packages, package_name)) {
        return bh_table_get(Package *, prog->packages, package_name);

    } else {
        Package* package = bh_alloc_item(alloc, Package);

        char* pac_name = bh_alloc_array(alloc, char, strlen(package_name) + 1);
        memcpy(pac_name, package_name, strlen(package_name) + 1);

        package->name = pac_name;
        package->scope = scope_create(alloc, parent_scope, (OnyxFilePos) { 0 });
        package->private_scope = scope_create(alloc, package->scope, (OnyxFilePos) { 0 });

        bh_table_put(Package *, prog->packages, pac_name, package);

        return package;
    }
}

Scope* scope_create(bh_allocator a, Scope* parent, OnyxFilePos created_at) {
    Scope* scope = bh_alloc_item(a, Scope);
    scope->parent = parent;
    scope->created_at = created_at;
    scope->symbols = NULL;

    bh_table_init(global_heap_allocator, scope->symbols, 64);

    return scope;
}

void scope_include(Scope* target, Scope* source, OnyxFilePos pos) {
    bh_table_each_start(AstNode *, source->symbols);
        symbol_raw_introduce(target, (char *) key, pos, value);
    bh_table_each_end;
}

b32 symbol_introduce(Scope* scope, OnyxToken* tkn, AstNode* symbol) {
    token_toggle_end(tkn);

    b32 ret = symbol_raw_introduce(scope, tkn->text, tkn->pos, symbol);

    token_toggle_end(tkn);
    return ret;
}

b32 symbol_raw_introduce(Scope* scope, char* name, OnyxFilePos pos, AstNode* symbol) {
    if (strcmp(name, "_")) {
        if (bh_table_has(AstNode *, scope->symbols, name)) {
            if (bh_table_get(AstNode *, scope->symbols, name) != symbol) {
                onyx_report_error(pos, "Redeclaration of symbol '%s'.", name);
                return 0;
            }
            return 1;
        }
    }

    bh_table_put(AstNode *, scope->symbols, name, symbol);
    return 1;
}

void symbol_builtin_introduce(Scope* scope, char* sym, AstNode *node) {
    bh_table_put(AstNode *, scope->symbols, sym, node);
}

void symbol_subpackage_introduce(Scope* scope, OnyxToken* sym, AstPackage* package) {
    token_toggle_end(sym);

    if (bh_table_has(AstNode *, scope->symbols, sym->text)) {
        AstNode* maybe_package = bh_table_get(AstNode *, scope->symbols, sym->text);
        assert(maybe_package->kind == Ast_Kind_Package);
    } else {
        bh_table_put(AstNode *, scope->symbols, sym->text, (AstNode *) package);
    }

    token_toggle_end(sym);
}

AstNode* symbol_raw_resolve(Scope* start_scope, char* sym) {
    AstNode* res = NULL;
    Scope* scope = start_scope;

    while (res == NULL && scope != NULL) {
        if (bh_table_has(AstNode *, scope->symbols, sym)) {
            res = bh_table_get(AstNode *, scope->symbols, sym);
        } else {
            scope = scope->parent;
        }
    }

    if (res == NULL) return NULL;

    if (res->kind == Ast_Kind_Symbol) {
        return symbol_resolve(start_scope, res->token);
    }

    return res;
}

AstNode* symbol_resolve(Scope* start_scope, OnyxToken* tkn) {
    token_toggle_end(tkn);
    AstNode* res = symbol_raw_resolve(start_scope, tkn->text);

    if (res == NULL) {
        onyx_report_error(tkn->pos, "Unable to resolve symbol '%s'.", tkn->text);
        token_toggle_end(tkn);
        return &empty_node;
    }

    token_toggle_end(tkn);
    return res;
}

void scope_clear(Scope* scope) {
    bh_table_clear(scope->symbols);
}

static void insert_poly_slns_into_scope(Scope* scope, bh_arr(AstPolySolution) slns) {
    bh_arr_each(AstPolySolution, sln, slns) {
        AstNode *node = NULL;

        switch (sln->kind) {
            case PSK_Type:
                node = onyx_ast_node_new(semstate.node_allocator, sizeof(AstTypeRawAlias), Ast_Kind_Type_Raw_Alias);
                ((AstTypeRawAlias *) node)->token = sln->poly_sym->token;
                ((AstTypeRawAlias *) node)->to = sln->type;
                break;

            case PSK_Value:
                // CLEANUP: Maybe clone this?
                node = (AstNode *) sln->value;
                break;
        }

        symbol_introduce(scope, sln->poly_sym->token, node);
    }
}

typedef struct PolySolveResult {
    PolySolutionKind kind;
    union {
        AstTyped* value;
        Type*     actual;
    };
} PolySolveResult;

typedef struct PolySolveElem {
    AstType* type_expr;

    PolySolutionKind kind;
    union {
        AstTyped* value;
        Type*     actual;
    };
} PolySolveElem;

static PolySolveResult solve_poly_type(AstNode* target, AstType* type_expr, Type* actual) {
    bh_arr(PolySolveElem) elem_queue = NULL;
    bh_arr_new(global_heap_allocator, elem_queue, 4);

    PolySolveResult result = { PSK_Undefined, { NULL } };

    bh_arr_push(elem_queue, ((PolySolveElem) {
        .type_expr = type_expr,
        .kind = PSK_Type,
        .actual    = actual
    }));

    while (!bh_arr_is_empty(elem_queue)) {
        PolySolveElem elem = elem_queue[0];
        bh_arr_deleten(elem_queue, 0, 1);

        if (elem.type_expr == (AstType *) target) {
            result.kind = elem.kind;

            assert(elem.kind != PSK_Undefined);
            if (result.kind == PSK_Type)  result.actual = elem.actual;
            if (result.kind == PSK_Value) result.value = elem.value;
            break;
        }

        if (elem.kind != PSK_Type) continue;

        switch (elem.type_expr->kind) {
            case Ast_Kind_Pointer_Type: {
                if (elem.actual->kind != Type_Kind_Pointer) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstPointerType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->Pointer.elem,
                }));
                break;
            }

            case Ast_Kind_Array_Type: {
                if (elem.actual->kind != Type_Kind_Array) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = (AstType*) ((AstArrayType *) elem.type_expr)->count_expr,
                    .kind = PSK_Value,
                    .value = (AstTyped *) make_int_literal(semstate.node_allocator, elem.actual->Array.count)
                }));

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstArrayType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->Array.elem,
                }));
                break;
            }

            case Ast_Kind_Slice_Type: {
                if (elem.actual->kind != Type_Kind_Slice) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstSliceType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->Slice.ptr_to_data->Pointer.elem,
                }));
                break;
            }

            case Ast_Kind_DynArr_Type: {
                if (elem.actual->kind != Type_Kind_DynArray) break;

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstDynArrType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = elem.actual->DynArray.ptr_to_data->Pointer.elem,
                }));
                break;
            }

            case Ast_Kind_VarArg_Type:
                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ((AstVarArgType *) elem.type_expr)->elem,
                    .kind = PSK_Type,
                    .actual = actual,
                }));
                break;

            case Ast_Kind_Function_Type: {
                if (elem.actual->kind != Type_Kind_Function) break;

                AstFunctionType* ft = (AstFunctionType *) elem.type_expr;

                fori (i, 0, (i64) ft->param_count) {
                    bh_arr_push(elem_queue, ((PolySolveElem) {
                        .type_expr = ft->params[i],
                        .kind = PSK_Type,
                        .actual = elem.actual->Function.params[i],
                    }));
                }

                bh_arr_push(elem_queue, ((PolySolveElem) {
                    .type_expr = ft->return_type,
                    .kind = PSK_Type,
                    .actual = elem.actual->Function.return_type,
                }));

                break;
            }

            case Ast_Kind_Poly_Call_Type: {
                if (elem.actual->kind != Type_Kind_Struct) break;
                if (bh_arr_length(elem.actual->Struct.poly_sln) != bh_arr_length(((AstPolyCallType *) elem.type_expr)->params)) break;

                AstPolyCallType* pt = (AstPolyCallType *) elem.type_expr;

                fori (i, 0, bh_arr_length(pt->params)) {
                    PolySolutionKind kind = elem.actual->Struct.poly_sln[i].kind;
                    if (kind == PSK_Type) {
                        bh_arr_push(elem_queue, ((PolySolveElem) {
                            .kind = kind,
                            .type_expr = (AstType *) pt->params[i],
                            .actual = elem.actual->Struct.poly_sln[i].type,
                        }));
                    } else {
                        bh_arr_push(elem_queue, ((PolySolveElem) {
                            .kind = kind,
                            .type_expr = (AstType *) pt->params[i],
                            .value = elem.actual->Struct.poly_sln[i].value,
                        }));
                    }
                }

                break;
            }

            case Ast_Kind_Type_Compound: {
                if (elem.actual->kind != Type_Kind_Compound) break;
                if (bh_arr_length(elem.actual->Compound.types) != bh_arr_length(((AstCompoundType *) elem.type_expr)->types)) break;

                AstCompoundType* ct = (AstCompoundType *) elem.type_expr;

                fori (i, 0, bh_arr_length(ct->types)) {
                    bh_arr_push(elem_queue, ((PolySolveElem) {
                        .kind = PSK_Type,
                        .type_expr = ct->types[i],
                        .actual = elem.actual->Compound.types[i],
                    }));
                }

                break;
            }

            default: break;
        }
    }

    bh_arr_free(elem_queue);

    return result;
}

static bh_arr(AstPolySolution) find_polymorphic_slns(AstPolyProc* pp, PolyProcLookupMethod pp_lookup, ptr actual, char** err_msg) {
    bh_arr(AstPolySolution) slns = NULL;
    bh_arr_new(global_heap_allocator, slns, bh_arr_length(pp->poly_params));

    bh_arr_each(AstPolySolution, known_sln, pp->known_slns) bh_arr_push(slns, *known_sln);

    bh_arr_each(AstPolyParam, param, pp->poly_params) {
        b32 already_solved = 0;
        bh_arr_each(AstPolySolution, known_sln, pp->known_slns) {
            if (token_equals(param->poly_sym->token, known_sln->poly_sym->token)) {
                already_solved = 1;
                break;
            }
        }
        if (already_solved) continue;

        Type* actual_type;

        if (pp_lookup == PPLM_By_Call) {
            if (param->idx >= ((AstCall *) actual)->arg_count) {
                if (err_msg) *err_msg = "Not enough arguments to polymorphic procedure.";
                goto sln_not_found;
            }

            bh_arr(AstArgument *) arg_arr = ((AstCall *) actual)->arg_arr;
            actual_type = resolve_expression_type(arg_arr[param->idx]->value);
        }

        else if (pp_lookup == PPLM_By_Value_Array) {
            bh_arr(AstTyped *) arg_arr = (bh_arr(AstTyped *)) actual;

            if ((i32) param->idx >= bh_arr_length(arg_arr)) {
                if (err_msg) *err_msg = "Not enough arguments to polymorphic procedure.";
                goto sln_not_found;
            }

            actual_type = resolve_expression_type(arg_arr[param->idx]);
        }

        else if (pp_lookup == PPLM_By_Function_Type) {
            Type* ft = (Type*) actual;
            if (param->idx >= ft->Function.param_count) {
                if (err_msg) *err_msg = "Incompatible polymorphic argument to function paramter.";
                goto sln_not_found;
            }

            actual_type = ft->Function.params[param->idx];
        }

        else {
            if (err_msg) *err_msg = "Cannot resolve polymorphic function type.";
            goto sln_not_found;
        }

        PolySolveResult resolved = solve_poly_type(param->poly_sym, param->type_expr, actual_type);

        switch (resolved.kind) {
            case PSK_Undefined:
                if (err_msg) *err_msg = bh_aprintf(global_heap_allocator,
                    "Unable to solve for polymoprhic variable '%b', using the type '%s'.",
                    param->poly_sym->token->text,
                    param->poly_sym->token->length,
                    type_get_name(actual_type));
                goto sln_not_found;

            case PSK_Type:
                bh_arr_push(slns, ((AstPolySolution) {
                    .kind     = PSK_Type,
                    .poly_sym = param->poly_sym,
                    .type     = resolved.actual,
                }));
                break;

            case PSK_Value:
                bh_arr_push(slns, ((AstPolySolution) {
                    .kind     = PSK_Value,
                    .poly_sym = param->poly_sym,
                    .value    = resolved.value,
                }));
                break;
        }
    }

    return slns;

sln_not_found:
    bh_arr_free(slns);
    return NULL;
}

AstFunction* polymorphic_proc_lookup(AstPolyProc* pp, PolyProcLookupMethod pp_lookup, ptr actual, OnyxFilePos pos) {
    if (pp->concrete_funcs == NULL) {
        bh_table_init(global_heap_allocator, pp->concrete_funcs, 8);
    }

    char *err_msg = NULL;
    bh_arr(AstPolySolution) slns = find_polymorphic_slns(pp, pp_lookup, actual, &err_msg);
    if (slns == NULL) {
        if (err_msg != NULL) onyx_report_error(pos, err_msg);
        else                 onyx_report_error(pos, "Some kind of error occured when generating a polymorphic procedure. You hopefully will not see this");
    }

    AstFunction* result = polymorphic_proc_solidify(pp, slns, pos);
    
    bh_arr_free(slns);
    return result;
}

// NOTE: This might return a volatile string. Do not store it without copying it.
static char* build_poly_solution_key(AstPolySolution* sln) {
    static char buffer[128];

    if (sln->kind == PSK_Type) {
        return (char *) type_get_unique_name(sln->type);
    }

    else if (sln->kind == PSK_Value) {
        fori (i, 0, 128) buffer[i] = 0;

        if (sln->value->kind == Ast_Kind_NumLit) {
            strncat(buffer, "NUMLIT:", 127);
            strncat(buffer, bh_bprintf("%l", ((AstNumLit *) sln->value)->value.l), 127);

        } else {
            // HACK: For now, the value pointer is just used. This means that
            // sometimes, even through the solution is the same, it won't be
            // stored the same.
            bh_snprintf(buffer, 128, "%p", sln->value);
        }

        return buffer;
    }

    return NULL;
}

// NOTE: This returns a volatile string. Do not store it without copying it.
static char* build_poly_slns_unique_key(bh_arr(AstPolySolution) slns) {
    static char key_buf[1024];
    fori (i, 0, 1024) key_buf[i] = 0;

    bh_arr_each(AstPolySolution, sln, slns) {
        token_toggle_end(sln->poly_sym->token);

        strncat(key_buf, sln->poly_sym->token->text, 1023);
        strncat(key_buf, "=", 1023);
        strncat(key_buf, build_poly_solution_key(sln), 1023);
        strncat(key_buf, ";", 1023);

        token_toggle_end(sln->poly_sym->token);
    }

    return key_buf;
}

AstFunction* polymorphic_proc_solidify(AstPolyProc* pp, bh_arr(AstPolySolution) slns, OnyxFilePos pos) {
    if (pp->concrete_funcs == NULL) {
        bh_table_init(global_heap_allocator, pp->concrete_funcs, 8);
    }

    // NOTE: Check if a version of this polyproc has already been created.
    char* unique_key = build_poly_slns_unique_key(slns);
    if (bh_table_has(AstFunction *, pp->concrete_funcs, unique_key)) {
        return bh_table_get(AstFunction *, pp->concrete_funcs, unique_key);
    }

    Scope* poly_scope = scope_create(semstate.node_allocator, pp->poly_scope, pos);
    insert_poly_slns_into_scope(poly_scope, slns);

    AstFunction* func = (AstFunction *) ast_clone(semstate.node_allocator, pp->base_func);
    bh_table_put(AstFunction *, pp->concrete_funcs, unique_key, func);

    func->flags |= Ast_Flag_Function_Used;
    func->flags |= Ast_Flag_From_Polymorphism;

    Entity func_header_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Function_Header,
        .function = func,
        .package = NULL,
        .scope = poly_scope,
    };

    entity_bring_to_state(&func_header_entity, Entity_State_Code_Gen);
    if (onyx_has_errors()) {
        onyx_report_error(pos, "Error in polymorphic procedure header generated from this call site.");
        return NULL;
    }

    Entity func_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Function,
        .function = func,
        .package = NULL,
        .scope = poly_scope,
    };

    entity_heap_insert(&semstate.program->entities, func_header_entity);
    entity_heap_insert(&semstate.program->entities, func_entity);
    return func;
}

// NOTE: This can return either a AstFunction or an AstPolyProc, depending if enough parameters were
// supplied to remove all the polymorphic variables from the function.
AstNode* polymorphic_proc_try_solidify(AstPolyProc* pp, bh_arr(AstPolySolution) slns, OnyxFilePos pos) {
    i32 valid_argument_count = 0;

    bh_arr_each(AstPolySolution, sln, slns) {
        b32 found_match = 0;

        bh_arr_each(AstPolyParam, param, pp->poly_params) {
            if (token_equals(sln->poly_sym->token, param->poly_sym->token)) {
                found_match = 1;
                break;
            }
        }

        if (found_match) {
            valid_argument_count++;
        } else {
            onyx_report_error(pos, "'%b' is not a type variable of '%b'.",
                sln->poly_sym->token->text, sln->poly_sym->token->length,
                pp->token->text, pp->token->length);
            return (AstNode *) pp;
        }
    }

    if (valid_argument_count == bh_arr_length(pp->poly_params)) {
        return (AstNode *) polymorphic_proc_solidify(pp, slns, pos);

    } else {
        // HACK: Some of these initializations assume that the entity for this polyproc has
        // made it through the symbol resolution phase.
        //                                                    - brendanfh 2020/12/25
        AstPolyProc* new_pp = onyx_ast_node_new(semstate.node_allocator, sizeof(AstPolyProc), Ast_Kind_Polymorphic_Proc);
        new_pp->token = pp->token;                            // TODO: Change this to be the solidify->token
        new_pp->base_func = pp->base_func;
        new_pp->poly_scope = new_pp->poly_scope;
        new_pp->flags = pp->flags;
        new_pp->poly_params = pp->poly_params;

        // POTENTIAL BUG: Copying this doesn't feel right...
        if (pp->concrete_funcs == NULL) {
            bh_table_init(global_heap_allocator, pp->concrete_funcs, 8);
        }
        new_pp->concrete_funcs = pp->concrete_funcs;

        new_pp->known_slns = NULL;
        bh_arr_new(global_heap_allocator, new_pp->known_slns, bh_arr_length(pp->known_slns) + bh_arr_length(slns));

        bh_arr_each(AstPolySolution, sln, pp->known_slns) bh_arr_push(new_pp->known_slns, *sln);
        bh_arr_each(AstPolySolution, sln, slns)           bh_arr_push(new_pp->known_slns, *sln);

        return (AstNode *) new_pp;
    }
}

AstFunction* polymorphic_proc_build_only_header(AstPolyProc* pp, PolyProcLookupMethod pp_lookup, ptr actual) {
    bh_arr(AstPolySolution) slns = find_polymorphic_slns(pp, pp_lookup, actual, NULL);
    if (slns == NULL) return NULL;

    Scope* poly_scope = scope_create(semstate.node_allocator, pp->poly_scope, (OnyxFilePos) { 0 });
    insert_poly_slns_into_scope(poly_scope, slns);

    // NOTE: This function is only going to have the header of it correctly created.
    // Nothing should happen to this function's body or else the original will be corrupted.
    //                                                      - brendanfh 2021/01/10
    AstFunction* new_func = clone_function_header(semstate.node_allocator, pp->base_func);
    new_func->flags |= Ast_Flag_From_Polymorphism;

    Entity func_header_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Function_Header,
        .function = new_func,
        .package = NULL,
        .scope = poly_scope,
    };

    entity_bring_to_state(&func_header_entity, Entity_State_Code_Gen);
    if (onyx_has_errors()) {
        onyx_clear_errors();
        return NULL;
    }

    return new_func;
}

char* build_poly_struct_name(AstPolyStructType* ps_type, Type* cs_type) {
    char name_buf[256];
    fori (i, 0, 256) name_buf[i] = 0;

    strncat(name_buf, ps_type->name, 255);
    strncat(name_buf, "(", 255);
    bh_arr_each(AstPolySolution, ptype, cs_type->Struct.poly_sln) {
        if (ptype != cs_type->Struct.poly_sln)
            strncat(name_buf, ", ", 255);

        // This logic will have to be other places as well.

        switch (ptype->kind) {
            case PSK_Undefined: assert(0); break;
            case PSK_Type:      strncat(name_buf, type_get_name(ptype->type), 255); break;
            case PSK_Value: {
                // FIX
                if (ptype->value->kind == Ast_Kind_NumLit) {
                    AstNumLit* nl = (AstNumLit *) ptype->value;
                    if (type_is_integer(nl->type)) {
                        strncat(name_buf, bh_bprintf("%l", nl->value.l), 127);
                    } else {
                        strncat(name_buf, "numlit (FIX ME)", 127);
                    }
                } else {
                    strncat(name_buf, "<expr>", 127);
                }

                break;
            }
        }
    }
    strncat(name_buf, ")", 255);

    return bh_aprintf(global_heap_allocator, "%s", name_buf);
}

AstStructType* polymorphic_struct_lookup(AstPolyStructType* ps_type, bh_arr(AstPolySolution) slns, OnyxFilePos pos) {
    // @Cleanup
    assert(ps_type->scope != NULL);

    if (ps_type->concrete_structs == NULL) {
        bh_table_init(global_heap_allocator, ps_type->concrete_structs, 16);
    }

    if (bh_arr_length(slns) != bh_arr_length(ps_type->poly_params)) {
        onyx_report_error(pos, "Wrong number of arguments for '%s'. Expected %d, got %d",
            ps_type->name,
            bh_arr_length(ps_type->poly_params),
            bh_arr_length(slns));

        return NULL;
    }

    i32 i = 0;
    bh_arr_each(AstPolySolution, sln, slns) {
        sln->poly_sym = (AstNode *) &ps_type->poly_params[i];
        
        PolySolutionKind expected_kind = PSK_Undefined;
        if ((AstNode *) ps_type->poly_params[i].type_node == &type_expr_symbol) {
            expected_kind = PSK_Type;
        } else {
            expected_kind = PSK_Value;
        }

        if (sln->kind != expected_kind) {
            if (expected_kind == PSK_Type) 
                onyx_report_error(pos, "Expected type expression for %d%s argument.", i + 1, bh_num_suffix(i + 1));

            if (expected_kind == PSK_Value)
                onyx_report_error(pos, "Expected value expression of type '%s' for %d%s argument.",
                    type_get_name(ps_type->poly_params[i].type),
                    i + 1, bh_num_suffix(i + 1));

            return NULL;
        }

        if (sln->kind == PSK_Value) {
            if ((sln->value->flags & Ast_Flag_Comptime) == 0) {
                onyx_report_error(pos,
                    "Expected compile-time known argument for '%b'.",
                    sln->poly_sym->token->text,
                    sln->poly_sym->token->length);
                return NULL;
            }

            if (!types_are_compatible(sln->value->type, ps_type->poly_params[i].type)) {
                onyx_report_error(pos, "Expected compile-time argument of type '%s', got '%s'.",
                    type_get_name(ps_type->poly_params[i].type),
                    type_get_name(sln->value->type));
                return NULL;
            }
        }

        i++;
    }

    char* unique_key = build_poly_slns_unique_key(slns);
    if (bh_table_has(AstStructType *, ps_type->concrete_structs, unique_key)) {
        return bh_table_get(AstStructType *, ps_type->concrete_structs, unique_key);
    }

    scope_clear(ps_type->scope);
    insert_poly_slns_into_scope(ps_type->scope, slns);

    AstStructType* concrete_struct = (AstStructType *) ast_clone(semstate.node_allocator, ps_type->base_struct);
    bh_table_put(AstStructType *, ps_type->concrete_structs, unique_key, concrete_struct);

    Entity struct_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Type_Alias,
        .type_alias = (AstType *) concrete_struct,
        .package = NULL,
        .scope = ps_type->scope,
    };
    Entity struct_default_entity = {
        .state = Entity_State_Resolve_Symbols,
        .type = Entity_Type_Struct_Member_Default,
        .type_alias = (AstType *) concrete_struct,
        .package = NULL,
        .scope = ps_type->scope,
    };

    entity_bring_to_state(&struct_entity, Entity_State_Code_Gen);
    entity_bring_to_state(&struct_default_entity, Entity_State_Code_Gen);
 
    if (onyx_has_errors()) {
        onyx_report_error(pos, "Error in creating polymoprhic struct instantiation here.");
        return NULL;
    }

    Type* cs_type = type_build_from_ast(semstate.node_allocator, (AstType *) concrete_struct);
    cs_type->Struct.poly_sln = NULL;
    bh_arr_new(global_heap_allocator, cs_type->Struct.poly_sln, bh_arr_length(slns));

    fori (i, 0, bh_arr_length(slns)) bh_arr_push(cs_type->Struct.poly_sln, slns[i]);

    cs_type->Struct.name = build_poly_struct_name(ps_type, cs_type);
    return concrete_struct;
}

void entity_bring_to_state(Entity* ent, EntityState state) {
    while (ent->state != state) {
        switch (ent->state) {
            case Entity_State_Resolve_Symbols: symres_entity(ent); break;
            case Entity_State_Check_Types:     check_entity(ent);  break;
            case Entity_State_Code_Gen:        emit_entity(ent);   break;

            default: return;
        }

        if (onyx_has_errors()) return;
    }
}
