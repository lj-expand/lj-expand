#include "lj_expand_script_deps.h"

#include <stdlib.h>
#include <string.h>

#include "lj_expand_log.h"

enum
{
    DEP_UNVISITED = 0,
    DEP_VISITING,
    DEP_DONE
};

typedef struct DepResolver
{
    LJEScript* scripts;
    size_t script_count;
    unsigned char* state;
    LJEScript** order;
    size_t order_count;
    LJEScriptDepErrors* errors;
    size_t error_capacity;
    int failed;
} DepResolver;

static void add_error(DepResolver* r, LJEScriptDepErrorKind kind, LJEScript* script,
                      const char* dep_name, const char* dep_author)
{
    LJEScriptDepErrors* errors = r->errors;
    if (errors->count == r->error_capacity)
    {
        size_t capacity = r->error_capacity ? r->error_capacity * 2 : 8;
        LJEScriptDepError* items = (LJEScriptDepError*)realloc(errors->items,
                                                               capacity * sizeof(LJEScriptDepError));
        if (!items)
        {
            r->failed = 1;
            return;
        }
        errors->items = items;
        r->error_capacity = capacity;
    }

    LJEScriptDepError* err = &errors->items[errors->count++];
    err->kind = kind;
    err->script = script;
    err->dep_name = dep_name;
    err->dep_author = dep_author;
}

static LJEScript* find_script(DepResolver* r, const LJEScriptDependency* dep, size_t* out_index)
{
    for (size_t k = 0; k < r->script_count; k++)
    {
        LJEScriptInfo* info = r->scripts[k].info;
        if (strcmp(info->name, dep->name) == 0 && strcmp(info->author, dep->author) == 0)
        {
            *out_index = k;
            return &r->scripts[k];
        }
    }
    return NULL;
}

static void visit(DepResolver* r, size_t index)
{
    LJEScript* script = &r->scripts[index];
    r->state[index] = DEP_VISITING;

    for (size_t j = 0; j < script->info->dependency_count; j++)
    {
        LJEScriptDependency* dep = &script->info->dependencies[j];
        size_t dep_index;
        if (!find_script(r, dep, &dep_index))
        {
            add_error(r, LJE_SCRIPT_DEP_MISSING_SCRIPT, script, dep->name, dep->author);
            continue;
        }

        if (r->state[dep_index] == DEP_VISITING)
        {
            /* Back edge: ignore it so every script still gets a slot. */
            add_error(r, LJE_SCRIPT_DEP_CYCLE, script, dep->name, dep->author);
            continue;
        }

        if (r->state[dep_index] == DEP_UNVISITED)
            visit(r, dep_index);
    }

    r->state[index] = DEP_DONE;
    r->order[r->order_count++] = script;
}

static void check_binaries(DepResolver* r, const LJEBinaryModule* modules, size_t module_count)
{
    for (size_t i = 0; i < r->script_count; i++)
    {
        LJEScript* script = &r->scripts[i];
        for (size_t j = 0; j < script->info->binary_dependency_count; j++)
        {
            const char* name = script->info->binary_dependencies[j].name;
            int found = 0;
            for (size_t m = 0; m < module_count; m++)
            {
                if (modules[m].name && strcmp(modules[m].name, name) == 0)
                {
                    found = 1;
                    break;
                }
            }

            if (!found)
                add_error(r, LJE_SCRIPT_DEP_MISSING_BINARY, script, name, NULL);
        }
    }
}

int lje_script_deps_resolve(
    LJEScript* scripts,
    size_t script_count,
    const LJEBinaryModule* modules,
    size_t module_count,
    LJEScript*** out_order,
    LJEScriptDepErrors* out_errors
)
{
    out_errors->items = NULL;
    out_errors->count = 0;
    *out_order = NULL;

    DepResolver r;
    memset(&r, 0, sizeof(r));
    r.scripts = scripts;
    r.script_count = script_count;
    r.errors = out_errors;
    /* +1 keeps malloc(0) from returning NULL when there are no scripts. */
    r.order = (LJEScript**)calloc(script_count + 1, sizeof(LJEScript*));
    r.state = (unsigned char*)calloc(script_count + 1, 1);
    if (!r.order || !r.state)
    {
        free(r.order);
        free(r.state);
        return -1;
    }

    /* Visit in discovery order so independent scripts keep their relative order. */
    for (size_t i = 0; i < script_count && !r.failed; i++)
    {
        if (r.state[i] == DEP_UNVISITED)
            visit(&r, i);
    }

    if (!r.failed)
        check_binaries(&r, modules, module_count);

    free(r.state);

    if (r.failed)
    {
        free(r.order);
        lje_script_deps_free_errors(out_errors);
        return -1;
    }

    *out_order = r.order;
    return 0;
}

void lje_script_deps_free_errors(LJEScriptDepErrors* errors)
{
    if (!errors) return;
    free(errors->items);
    errors->items = NULL;
    errors->count = 0;
}

void lje_script_deps_log_errors(const LJEScriptDepErrors* errors)
{
    for (size_t i = 0; i < errors->count; i++)
    {
        const LJEScriptDepError* err = &errors->items[i];
        const LJEScriptInfo* info = err->script->info;
        switch (err->kind)
        {
        case LJE_SCRIPT_DEP_MISSING_SCRIPT:
            LJE_WARN("Script '%s' by '%s' is missing dependency '%s' by '%s'",
                info->name, info->author, err->dep_name, err->dep_author);
            break;
        case LJE_SCRIPT_DEP_MISSING_BINARY:
            LJE_WARN("Script '%s' by '%s' is missing binary module dependency '%s'",
                info->name, info->author, err->dep_name);
            break;
        case LJE_SCRIPT_DEP_CYCLE:
            LJE_WARN("Script '%s' by '%s' has a circular dependency through '%s' by '%s'; "
                     "that dependency will not be ordered before it",
                info->name, info->author, err->dep_name, err->dep_author);
            break;
        }
    }
}
