#ifndef _LJ_EXPAND_STARTUP_H
#define _LJ_EXPAND_STARTUP_H
#include "lj_expand_script.h"
#include "lj_obj.h"

LJ_FUNC void lje_startup_execute(lua_State* L, LJEScript* script, const char* path);
LJ_FUNC void lje_startup_secure_preinit(lua_State* L);
/* Loads the pure-Lua helpers chunk (split out of preinit) */
LJ_FUNC void lje_startup_secure_helpers(lua_State* L);
/* Seeds the per-host shadow registries with the stubs GMod's C API expects */
LJ_FUNC void lje_startup_shadow_stubs(lua_State* L);
LJ_FUNC int lje_startup_include(lua_State* L, const char* relative_path, int execute);
/* Used for any dynamic compilation since we need to make sure their protos are tagged */
LJ_FUNC int lje_startup_compile(lua_State* L, const char* source, char error_buffer[512]);
/* Loads and runs a source string, returning the Lua status (0 on success) */
LJ_FUNC int lje_startup_run(lua_State* L, const char* source);
LJ_FUNC void lje_startup_reload(lua_State* L, LJEScript* script);
#endif