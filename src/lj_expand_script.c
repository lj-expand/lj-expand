#include "lj_expand_script.h"
#include "tomlc17.h"

#include <stdio.h>

#include "lauxlib.h"
#include "lj_expand_dirs.h"
#include "lj_expand_globals.h"
#include "lj_expand_log.h"
#include "lj_expand_platform.h"
#include "lj_expand_script_deps.h"

void lje_script_resolve_base(char* out_buffer, size_t buffer_size) {
    if (!lje_directory_get(LJE_DIR_SCRIPTS, out_buffer, buffer_size))
    {
        LJE_ERROR("Failed to resolve script directory path.");
    }
}

void lje_script_data_resolve_base(char* out_buffer, size_t buffer_size)
{
    if (!lje_directory_get(LJE_DIR_SCRIPT_DATA, out_buffer, buffer_size))
    {
        LJE_ERROR("Failed to resolve script data directory path.");
    }
}

int lje_script_folder_exists() {
    char path[LJE_PATH_MAX];
    lje_script_resolve_base(path, sizeof(path));
    return lje_plat_fs_kind(path) == LJE_FS_DIR;
}

int lje_script_data_folder_exists()
{
    char path[LJE_PATH_MAX];
    lje_script_data_resolve_base(path, sizeof(path));
    return lje_plat_fs_kind(path) == LJE_FS_DIR;
}

int lje_script_folder_create()
{
    char path[LJE_PATH_MAX];
    lje_script_resolve_base(path, sizeof(path));
    return lje_plat_mkdir(path);
}

int lje_script_data_folder_create()
{
    char path[LJE_PATH_MAX];
    lje_script_data_resolve_base(path, sizeof(path));
    return lje_plat_mkdir(path);
}

static char is_data_path_safe(const char* relative_path)
{
    if (strlen(relative_path) == 0 || strlen(relative_path) > 128)
    {
        return 0; // Too long or empty
    }
    // Only accept alphanumeric, underscore and hyphen characters
    // We specifically forbid subdirectories or path weaseling (e.g. ../)
    // It might seem excessive, but having a completely flat namespace for script data
    // is much safer and easier to manage.
    for (const char* p = relative_path; *p; p++)
    {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              (*p == '_') ||
              (*p == '-') ))
        {
            return 0; // Unsafe character found
        }
    }

    return 1; // Safe
}

int lje_script_data_write_file(const char* relative_path, const char* data, size_t data_size)
{
    if (!is_data_path_safe(relative_path))
    {
        return 0; // Unsafe path
    }

    char full_path[LJE_PATH_MAX];
    lje_script_data_resolve_base(full_path, sizeof(full_path));
    if (!lje_path_join(full_path, sizeof(full_path), full_path, relative_path))
        return 0;
    lje_strlcat(full_path, ".dat", sizeof(full_path));

    FILE* file = fopen(full_path, "wb");
    if (!file)
    {
        return 0; // Failed to open file
    }

    size_t written = fwrite(data, 1, data_size, file);
    fclose(file);

    return written == data_size;
}

char* lje_script_data_read_file(const char* relative_path, size_t* out_data_size)
{
    if (!is_data_path_safe(relative_path))
    {
        return NULL; // Unsafe path
    }

    char full_path[LJE_PATH_MAX];
    lje_script_data_resolve_base(full_path, sizeof(full_path));
    if (!lje_path_join(full_path, sizeof(full_path), full_path, relative_path))
        return NULL;
    lje_strlcat(full_path, ".dat", sizeof(full_path));

    FILE* file = fopen(full_path, "rb");
    if (!file)
    {
        return NULL; // Failed to open file
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = (char*)malloc(length + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, length, file);
    fclose(file);

    if (read_size != length)
    {
        free(buffer);
        return NULL; // Read error
    }

    buffer[length] = '\0';
    if (out_data_size)
    {
        *out_data_size = length;
    }
    return buffer;
}

static void script_info_parse_error(const char* property, const char* details)
{
    LJE_ERROR("Error parsing script info file: '%s' - %s", property, details);
}

static LJEScriptDependency* script_info_parse_dep(const char* dep_str)
{
    // Deps are formatted like:
    // author.name

    char* dot_pos = strchr(dep_str, '.');
    if (!dot_pos)
        return NULL;

    size_t author_start = 0;
    size_t author_length = dot_pos - dep_str;
    size_t name_start = author_length + 1;
    size_t name_length = strlen(dep_str) - name_start;

    LJEScriptDependency* dep = (LJEScriptDependency*)malloc(sizeof(LJEScriptDependency));
    dep->author = (char*)malloc(author_length + 1);
    dep->name = (char*)malloc(name_length + 1);

    lje_strlcpy((char*)dep->author, dep_str + author_start, author_length + 1);
    lje_strlcpy((char*)dep->name, dep_str + name_start, name_length + 1);

    return dep;
}
#define TYPE_CHECK_PROPERTY(prop, ctype) if (prop.type != ctype) { script_info_parse_error(#prop, "Invalid type or missing"); return NULL; }
LJEScriptInfo* lje_script_parse_info(const char* info_path)
{
    // Check if file exists
    if (lje_plat_fs_kind(info_path) == LJE_FS_MISSING)
    {
        return NULL;
    }

    toml_result_t res = toml_parse_file_ex(info_path);
    if (!res.ok)
    {
        script_info_parse_error("file", "Failed to parse TOML file");
        return NULL;
    }

    toml_datum_t name = toml_seek(res.toptab, "script.name");
    toml_datum_t version = toml_seek(res.toptab, "script.version");
    toml_datum_t author = toml_seek(res.toptab, "script.author");
    toml_datum_t dependencies = toml_seek(res.toptab, "script.dependencies");
    toml_datum_t binary_dependencies = toml_seek(res.toptab, "script.binaries");

    TYPE_CHECK_PROPERTY(name, TOML_STRING);
    TYPE_CHECK_PROPERTY(version, TOML_STRING);
    TYPE_CHECK_PROPERTY(author, TOML_STRING);
    TYPE_CHECK_PROPERTY(dependencies, TOML_ARRAY);

    LJEScriptInfo* info = (LJEScriptInfo*)malloc(sizeof(LJEScriptInfo));
    info->name = lje_strdup(name.u.s);
    info->version = lje_strdup(version.u.s);
    info->author = lje_strdup(author.u.s);

    // Parse dependencies
    info->dependency_count = dependencies.u.arr.size;
    info->dependencies = (LJEScriptDependency*)malloc(sizeof(LJEScriptDependency) * info->dependency_count);
    for (size_t i = 0; i < info->dependency_count; i++)
    {
        toml_datum_t dep_item = dependencies.u.arr.elem[i];
        TYPE_CHECK_PROPERTY(dep_item, TOML_STRING);
        LJEScriptDependency* dep = script_info_parse_dep(dep_item.u.s);
        if (!dep)
        {
            script_info_parse_error("script.dependencies", "Invalid dependency format");
            free(info);
            return NULL;
        }

        info->dependencies[i] = *dep;
        free(dep);
    }

    // Binary dependencies are optional
    if (binary_dependencies.type == TOML_ARRAY)
    {
        info->binary_dependency_count = binary_dependencies.u.arr.size;
        info->binary_dependencies = (LJEBinaryModuleDependency*)malloc(sizeof(LJEBinaryModuleDependency) * info->binary_dependency_count);
        for (size_t i = 0; i < info->binary_dependency_count; i++)
        {
            toml_datum_t dep_item = binary_dependencies.u.arr.elem[i];
            TYPE_CHECK_PROPERTY(dep_item, TOML_STRING);
            LJEBinaryModuleDependency* dep = info->binary_dependencies + i;
            dep->name = lje_strdup(dep_item.u.s);
        }
    } else
    {
        info->binary_dependency_count = 0;
        info->binary_dependencies = NULL;
    }

    toml_free(res);
    return info;
}

LJEScript* lje_script_load_all_scripts(size_t* out_script_count) {
    char base_path[LJE_PATH_MAX];
    lje_script_resolve_base(base_path, sizeof(base_path));

    LJEPlatDir* d = lje_plat_dir_open(base_path, NULL);
    if (!d) {
        LJE_INFO("No scripts found in %s", base_path);
        *out_script_count = 0;
        return NULL;
    }

    LJEScript* scripts = NULL;
    size_t script_count = 0;

    LJEPlatDirEntry entry;
    while (lje_plat_dir_next(d, &entry))
    {
        if (!entry.is_dir)
            continue;

        LJE_INFO("Found script folder: %s", entry.name);

        char folder_path[LJE_PATH_MAX];
        if (!lje_path_join(folder_path, sizeof(folder_path), base_path, entry.name))
            continue;
        /* script->folder must carry a trailing separator: lje_startup_include and
         * user Lua code concatenate relative paths onto it directly. lje_path_join
         * below will not double the separator. */
        lje_strlcat(folder_path, LJE_PATH_SEP_STR, sizeof(folder_path));

        char main_lua_path[LJE_PATH_MAX];
        if (!lje_path_join(main_lua_path, sizeof(main_lua_path), folder_path, LJE_SCRIPT_MAIN))
            continue;

        char info_path[LJE_PATH_MAX];
        if (!lje_path_join(info_path, sizeof(info_path), folder_path, "info.toml"))
            continue;

        char boot_lua_path[LJE_PATH_MAX];
        if (!lje_path_join(boot_lua_path, sizeof(boot_lua_path), folder_path, LJE_SCRIPT_BOOT))
          continue;

        char uses_main_entrypoint = 1;
        if (lje_plat_fs_kind(main_lua_path) != LJE_FS_FILE)
        {
          uses_main_entrypoint = 0;
          // Not all scripts operate in the client state, some only have boot.lua, so check that quickly
          if (lje_plat_fs_kind(boot_lua_path) != LJE_FS_FILE)
          {
            LJE_INFO("Skipping script %s: no entrypoints found", entry.name);
            continue;
          }
        }

        // Found a valid script
        LJEScriptInfo* info = lje_script_parse_info(info_path);
        if (!info)
        {
            LJE_WARN("Script '%s' is missing a valid info.toml file. Skipping.", entry.name);
            continue;
        }

        // Before continuing, check if it has binaries. Scripts likely wont function at all without
        // their required binaries, so we skip them entirely if they are missing.
        if (info->binary_dependency_count > 0)
        {
            int found_binaries = 0;
            for (size_t i = 0; i < info->binary_dependency_count; i++)
            {
                const char* binary_name = info->binary_dependencies[i].name;
                for (size_t j = 0; j < LJEG()->loaded_binary_module_count; j++)
                {
                    if (strcmp(LJEG()->loaded_binary_modules[j].name, binary_name) == 0)
                    {
                        found_binaries++;
                        break;
                    }
                }
            }

            if (found_binaries != info->binary_dependency_count)
            {
                LJE_WARN("Script '%s' is missing required binary dependencies. Skipping.", entry.name);
                continue;
            }
        }

        scripts = (LJEScript*)realloc(scripts, sizeof(LJEScript) * (script_count + 1));
        scripts[script_count].folder = lje_strdup(folder_path);
        scripts[script_count].main_path = uses_main_entrypoint ? lje_strdup(main_lua_path) : NULL;
        scripts[script_count].name = lje_strdup(entry.name);
        scripts[script_count].info = info;

        LJEScriptExtraInfo* extra = (LJEScriptExtraInfo*)malloc(sizeof(LJEScriptExtraInfo));
        extra->cleanup_ref_id = LUA_NOREF;
        extra->engine_call_hook_count = 0;
        memset(extra->engine_call_hooks, 0, sizeof(LJEEngineCallHook) * LJE_SCRIPT_MAX_ENGINE_CALL_HOOKS);
        scripts[script_count].extra = extra;

        // Check for preinit.lua
        char preinit_lua_path[LJE_PATH_MAX];
        if (lje_path_join(preinit_lua_path, sizeof(preinit_lua_path), folder_path, LJE_SCRIPT_PREINIT) &&
            lje_plat_fs_kind(preinit_lua_path) == LJE_FS_FILE)
        {
            scripts[script_count].preinit_path = lje_strdup(preinit_lua_path);
        }
        else
        {
            scripts[script_count].preinit_path = NULL;
        }

        // Check for boot.lua
        if (lje_plat_fs_kind(boot_lua_path) == LJE_FS_FILE)
        {
            scripts[script_count].boot_path = lje_strdup(boot_lua_path);
        } else
        {
            scripts[script_count].boot_path = NULL;
        }

        script_count++;
    }
    lje_plat_dir_close(d);

    *out_script_count = script_count;
    return scripts;
}

LJEScript** lje_script_compute_load_order(size_t script_count, LJEScript* scripts)
{
    LJE_INFO("Computing script load order for %llu scripts...", script_count);

    LJEScript** load_order = NULL;
    LJEScriptDepErrors errors;
    if (lje_script_deps_resolve(scripts, script_count,
                                LJEG()->loaded_binary_modules, LJEG()->loaded_binary_module_count,
                                &load_order, &errors) != 0)
    {
        LJE_ERROR("Failed to compute script load order (out of memory).");
        return NULL;
    }

    lje_script_deps_log_errors(&errors);
    lje_script_deps_free_errors(&errors);

    for (size_t i = 0; i < script_count; i++)
    {
        LJE_DEBUG("Load order %llu: %s by %s", i, load_order[i]->info->name, load_order[i]->info->author);
    }

    return load_order;
}

char** lje_script_find(LJEScript* script, const char* relative_path, size_t* out_path_count)
{
    if (strstr(relative_path, ".."))
    {
        // Forbid path weaseling
        LJE_WARN("lje_script_find: Forbidding path weaseling in path '%s'", relative_path);
        *out_path_count = 0;
        return NULL;
    }

    // Split relative_path into directory part and glob part.
    // e.g. "modules/*"  → dir="modules", glob="*"
    //      "*.lua"      → dir="",       glob="*.lua"
    const char* last_sep = NULL;
    for (const char* p = relative_path; *p; p++) {
        if (*p == '/' || *p == '\\')
            last_sep = p;
    }

    char dir_part[LJE_PATH_MAX];
    char glob_part[LJE_NAME_MAX];
    if (last_sep) {
        size_t dir_len = (size_t)(last_sep - relative_path);
        memcpy(dir_part, relative_path, dir_len);
        dir_part[dir_len] = '\0';
        lje_strlcpy(glob_part, last_sep + 1, sizeof(glob_part));
    } else {
        dir_part[0] = '\0';
        lje_strlcpy(glob_part, relative_path, sizeof(glob_part));
    }

    // Build the search directory = script->folder + dir_part
    char search_dir[LJE_PATH_MAX];
    if (dir_part[0]) {
        if (!lje_path_join(search_dir, sizeof(search_dir), script->folder, dir_part)) {
            *out_path_count = 0;
            return NULL;
        }
    } else {
        lje_strlcpy(search_dir, script->folder, sizeof(search_dir));
    }

    LJEPlatDir* d = lje_plat_dir_open(search_dir, glob_part);
    if (!d) {
        *out_path_count = 0;
        return NULL;
    }

    char** found_paths = NULL;
    size_t found_count = 0;

    LJEPlatDirEntry entry;
    while (lje_plat_dir_next(d, &entry))
    {
        if (entry.is_dir)
            continue;

        // Build relative path: dir_part + entry.name
        char* full_path = (char*)malloc(LJE_PATH_MAX);
        if (!full_path) continue;
        full_path[0] = '\0';

        if (dir_part[0]) {
            lje_strlcpy(full_path, dir_part, LJE_PATH_MAX);
            lje_strlcat(full_path, "/", LJE_PATH_MAX);
        }
        lje_strlcat(full_path, entry.name, LJE_PATH_MAX);

        found_paths = (char**)realloc(found_paths, sizeof(char*) * (found_count + 1));
        found_paths[found_count++] = full_path;
    }
    lje_plat_dir_close(d);

    *out_path_count = found_count;
    return found_paths;
}

char* lje_script_read(LJEScript* script, const char* relative_path, size_t* out_size)
{
  // Literally just read from the script folder
  char path[LJE_PATH_MAX];
  if (!lje_path_join(path, sizeof(path), script->folder, relative_path))
    return NULL;

  FILE* file = NULL;
  file = fopen(path, "rb");
  if (file == NULL)
    return NULL;

  fseek(file, 0, SEEK_END);
  size_t file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  char* buffer = (char*)malloc(file_size + 1);
  fread(buffer, 1, file_size, file);
  buffer[file_size] = '\0';
  fclose(file);

  *out_size = file_size; // For binary data, incase the file isn't null-terminated.
  return buffer;
}

