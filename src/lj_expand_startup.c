#include "lj_expand_startup.h"

#include "lauxlib.h"
#include "lj_expand_globals.h"
#include "lj_expand_lib.h"
#include "lj_expand_log.h"
#include "lj_expand_module.h"
#include "lj_expand_platform.h"
#include "lj_expand_settings.h"
#include "stdio.h"

#include "generated/lje_secure_preinit.h"
#include "generated/lje_helpers.h"
#include "generated/lje_shadow_init.h"

static char* load_lua_file(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file) {
        LJE_ERROR("Failed to open script file: %s", path);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = (char*)malloc(length + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    fread(buffer, 1, length, file);
    buffer[length] = '\0';
    fclose(file);
    return buffer;
}

// Bit of a hack, so we want to run with GMod's loadbufferx because their bytecode compiler
// is heavily modified to support stupid shit like C operators and continue.
typedef int (*luaL_loadbufferx_t)(lua_State* L, const char* buff, size_t size, const char* name, const char* mode);
// Same thing for pcall, unfortunately
typedef int (*lua_pcall_t)(lua_State* L, int nargs, int nresults, int errfunc);

static int resolve_original_functions(luaL_loadbufferx_t* out_loadbufferx, lua_pcall_t* out_pcall)
{
    lje_Module* mod = lje_module_find(LJE_LUA_MODULE);
    if (mod)
    {
        *out_loadbufferx = (luaL_loadbufferx_t)lje_module_get_func(mod, "luaL_loadbufferx");
        if (out_pcall)
            *out_pcall = (lua_pcall_t)lje_module_get_func(mod, "lua_pcall");

        return 1;
    }

    return 0;
}

void lje_startup_execute(lua_State* L, LJEScript* script, const char* path) {

    LJEG()->current_script = script;

    luaL_loadbufferx_t original_loadbufferx = NULL;
    lua_pcall_t original_pcall = NULL;
    if (!resolve_original_functions(&original_loadbufferx, &original_pcall))
    {
        LJE_ERROR("Failed to resolve original startup functions necessary...");
        return;
    }

    path = path ? path : script->main_path;
    char* script_file = load_lua_file(path);

    char chunkname[LUA_IDSIZE] = { 0 };
    lje_strlcat(chunkname, "@lje_script:", LUA_IDSIZE);
    lje_strlcat(chunkname, script->info->name, LUA_IDSIZE);
    chunkname[LUA_IDSIZE - 1] = '\0';

    if (script_file)
    {
        LJE_INFO("Executing script '%s'...", script->name);
        if (original_loadbufferx(L, script_file, strlen(script_file), chunkname, NULL) == 0)
        {
            if (lua_pcall(L, 0, 0, 0) != 0) /* mental note: figure out why this seems to randomly not work? */
            {
                LJE_ERROR("Error executing script: %s", lua_tostring(L, -1));
                lua_pop(L, 1); // Pop error message
            }
        }
        else
        {
            LJE_ERROR("Error loading script: %s", lua_tostring(L, -1));
            lua_pop(L, 1); // Pop error message
        }

        free(script_file);
    }

    LJEG()->current_script = NULL;
}

int lje_startup_include(lua_State* L, const char* relative_path, int execute) {
    luaL_loadbufferx_t original_loadbufferx = NULL;
    lua_pcall_t original_pcall = NULL;
    if (!resolve_original_functions(&original_loadbufferx, &original_pcall))
    {
        LJE_ERROR("Failed to resolve original startup functions necessary...");
        return 0;
    }

    if (!LJEG()->current_script)
    {
        LJE_ERROR("No current script context for include!");
        return 0;
    }

    // compute relative to current script
    char full_path[512] = { 0 };
    lje_strlcat(full_path, LJEG()->current_script->folder, 512);
    lje_strlcat(full_path, relative_path, 512);

    char chunkname[LUA_IDSIZE] = { 0 };
    lje_strlcat(chunkname, "@lje_include:", LUA_IDSIZE);
    lje_strlcat(chunkname, relative_path, LUA_IDSIZE);

    char* buffer = load_lua_file(full_path);
    if (original_loadbufferx(L, buffer, strlen(buffer), chunkname, NULL) == 0)
    {
        if (!execute)
        {
            // Just return the loaded function
            free(buffer);
            return 1;
        }

        if (original_pcall(L, 0, LUA_MULTRET, 0) != 0)
        {
            LJE_ERROR("Error executing include script: %s", lua_tostring(L, -1));
            lua_pop(L, 1); // Pop error message
        } else
        {
            free(buffer);
            return lua_gettop(L) - 1;
        }
    }
    else
    {
        LJE_ERROR("Error loading include script: %s", lua_tostring(L, -1));
        lua_pop(L, 1); // Pop error message
    }

    free(buffer);
    return 0;
}

// Load and execute a builtin Lua chunk using GMod's original loadbufferx/pcall.
static void run_secure_chunk(lua_State* L, luaL_loadbufferx_t original_loadbufferx,
                             lua_pcall_t original_pcall, const char* source, const char* name)
{
    if (original_loadbufferx(L, source, strlen(source), name, NULL) == 0)
    {
        if (original_pcall(L, 0, 0, 0) != 0)
        {
            LJE_ERROR("Error executing %s script: %s", name, lua_tostring(L, -1));
            lua_pop(L, 1); // Pop error message
        } else
        {
            LJE_SUCCESS("%s script executed successfully.", name);
        }
    }
    else
    {
        LJE_ERROR("Error loading %s script: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1); // Pop error message
    }
}

void lje_startup_secure_preinit(lua_State* L) {
    LJE_INFO("Running secure pre-initialization script...");

    luaL_loadbufferx_t original_loadbufferx = NULL;
    lua_pcall_t original_pcall = NULL;
    if (!resolve_original_functions(&original_loadbufferx, &original_pcall))
    {
        LJE_ERROR("Failed to resolve original startup functions necessary...");
        return;
    }

    run_secure_chunk(L, original_loadbufferx, original_pcall,
                     lje_secure_preinit_data, "@lje_secure_preinit");

    return;
}

// Seeds the shadow registries.
void lje_startup_shadow_stubs(lua_State* L) {
    luaL_loadbufferx_t original_loadbufferx = NULL;
    lua_pcall_t original_pcall = NULL;
    if (!resolve_original_functions(&original_loadbufferx, &original_pcall))
    {
        LJE_ERROR("Failed to resolve original startup functions necessary...");
        return;
    }

    run_secure_chunk(L, original_loadbufferx, original_pcall,
                     lje_shadow_init_data, "@lje_shadow_init");
}

// Loads the pure-Lua helpers chunk into the given state. Kept separate from preinit
// so it can be loaded early (before boot scripts run); it only depends on lje.util
// and lje.con_print, which the isolated state already provides.
void lje_startup_secure_helpers(lua_State* L) {
    LJE_INFO("Running secure helpers script...");

    luaL_loadbufferx_t original_loadbufferx = NULL;
    lua_pcall_t original_pcall = NULL;
    if (!resolve_original_functions(&original_loadbufferx, &original_pcall))
    {
        LJE_ERROR("Failed to resolve original startup functions necessary...");
        return;
    }

    run_secure_chunk(L, original_loadbufferx, original_pcall,
                     lje_helpers_data, "@lje_helpers");

    return;
}

int lje_startup_compile(lua_State* L, const char* source, char error_buffer[512]) {
    luaL_loadbufferx_t original_loadbufferx = NULL;
    if (!resolve_original_functions(&original_loadbufferx, NULL))
    {
        LJE_ERROR("Failed to resolve original startup functions necessary...");
        strncpy(error_buffer, "Failed to resolve original startup functions necessary...", 512);
        error_buffer[511] = '\0'; // Ensure null termination
        return 0;
    }

    if (original_loadbufferx(L, source, strlen(source), "@lje_dynamic_compile", NULL) == 0)
    {
        return 1; // success, function is on top of stack
    }
    else
    {
        LJE_ERROR("Error compiling dynamic script: %s", lua_tostring(L, -1));
        strncpy(error_buffer, lua_tostring(L, -1), 512);
        error_buffer[511] = '\0';
        lua_pop(L, 1); // Pop error message
    }

    return 0;
}

int lje_startup_run(lua_State* L, const char* source) {
    luaL_loadbufferx_t original_loadbufferx = NULL;
    lua_pcall_t original_pcall = NULL;
    if (!resolve_original_functions(&original_loadbufferx, &original_pcall))
    {
        LJE_ERROR("Failed to resolve original startup functions necessary...");
        return -1;
    }

    int status = original_loadbufferx(L, source, strlen(source), "@lje_run", NULL);

    if (status != 0)
    {
        LJE_ERROR("Error loading script: %s", lua_tostring(L, -1));
        lua_pop(L, 1); // Pop error message
        return status;
    }

    status = original_pcall(L, 0, 0, 0);
    if (status != 0)
    {
        LJE_ERROR("Error executing script: %s", lua_tostring(L, -1));
        lua_pop(L, 1); // Pop error message
    }

    return status;
}

LJ_FUNC void lje_startup_reload(lua_State* L, LJEScript* script)
{
    LJE_INFO("Reloading script '%s'...", script->name);
    // run `lje.includeCache[script] = {}`
    char cacheInvalidationSource[512] = { 0 };
    lje_strlcat(cacheInvalidationSource, "lje.includeCache[\"", 512);
    lje_strlcat(cacheInvalidationSource, script->info->name, 512);
    lje_strlcat(cacheInvalidationSource, "\"] = {}", 512);

    if (lje_startup_run(L, cacheInvalidationSource) != LUA_OK)
    {
      LJE_WARN("Failed to invalidate include cache for script %s. This may cause includes to not reload properly. Error: %s", script->info->name, lua_tostring(L, -1));
      return;
    }

    lje_settings_clear_cache(L);
    // Clear this script's engine hooks, unrefing as we go since it has GC implications.
    // Boot-scoped hooks stay: only main.lua re-runs below, so nothing would re-register them.
    lje_script_drop_transient_hooks(script, L);
    lje_startup_execute(L, script, NULL);
}
