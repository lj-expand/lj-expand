#pragma once

#include <stddef.h>

#include "lj_expand_script.h"
#include "lj_expand_binary_module.h"

/* Dependency resolution for LJE scripts: builds a load order where every script
 * comes after the scripts it depends on (transitively), and reports problems.
 */

typedef enum LJEScriptDepErrorKind
{
    /* A script depends on a script (name + author) that is not loaded. */
    LJE_SCRIPT_DEP_MISSING_SCRIPT,
    /* A script depends on a binary module that is not loaded. */
    LJE_SCRIPT_DEP_MISSING_BINARY,
    /* A script's dependency leads back to itself; the edge was ignored. */
    LJE_SCRIPT_DEP_CYCLE
} LJEScriptDepErrorKind;

/* All strings are borrowed from the scripts' LJEScriptInfo and stay valid as long
 * as the scripts do. For cycles, dep_name/dep_author name the script that closes
 * the cycle. For missing binaries, dep_author is NULL. */
typedef struct LJEScriptDepError
{
    LJEScriptDepErrorKind kind;
    LJEScript* script;
    const char* dep_name;
    const char* dep_author;
} LJEScriptDepError;

typedef struct LJEScriptDepErrors
{
    LJEScriptDepError* items;
    size_t count;
} LJEScriptDepErrors;

/* Resolves the load order of `scripts`. `*out_order` receives a newly allocated
 * array of exactly `script_count` pointers into `scripts` (every script appears once,
 * even when part of a cycle). `*out_errors` receives the detected problems; free it with
 * lje_script_deps_free_errors. `modules` may be NULL when no binary modules are loaded.
 * Returns 0 on success, -1 on allocation failure (outputs are then NULL/empty). */
int lje_script_deps_resolve(
    LJEScript* scripts,
    size_t script_count,
    const LJEBinaryModule* modules,
    size_t module_count,
    LJEScript*** out_order,
    LJEScriptDepErrors* out_errors
);

void lje_script_deps_free_errors(LJEScriptDepErrors* errors);

/* Logs every error with LJE_WARN. */
void lje_script_deps_log_errors(const LJEScriptDepErrors* errors);
