# Scripting

Table of Contents
- [Entrypoints](#entrypoints)
- [Example script structure](#example-script-structure)
- [Dependencies](#dependencies)
- [Writing safe scripts](#writing-safe-scripts)
- [API Reference](#api-reference)

LJE executes units of startup and pre-init code as "scripts," which are folders in `%USERPROFILE%\.lje_scripts`.
To make it as seamless as possible, LJE provides a unified environment for scripts to run in, which mimics the global environment as closely as possible.
The only differences are:

- `lje` table: This table contains all LJE-specific functionality, such as environment management, logging, and utility functions.
- `_L` table: This is a circular reference *back* to the scripting environment.
- `_G` table: This is a reference to the actual GMod global environment, useful for detouring or accessing global functions directly.
- No Lua-based API functions included: LJE does not include any Lua-based API functions (e.g., `hook.Add`, `file.Read`, etc.) in the scripting environment. This is to prevent easy detection of LJE via these functions.

Otherwise, everything else is identical and the entire C-based API is available as normal.
You can monkeypatch/share global functions and variables, and other scripts will see the changes.
To provide library behavior, LJE includes a load order system that allows scripts to specify dependencies on other scripts.

Scripts first declare their metadata in a `info.toml` file, which is parsed by LJE to determine the script's name, version, author and dependencies.

Here is the structure of an `info.toml` file, taken from [gilbhax](https://github.com/yogwoggf/gilbhax):
```toml
# info.toml allows you to specify metadata about your script.

[script]
name = "gilbhax"
version = "1.0.0"
author = "yogwoggf"
url = "https://github.com/yogwoggf/gilbhax" # Not required, in fact not even used, but can be helpful for automation/documentation.
dependencies = [
    "Eyoko1.ljeutil"
]
```

## Entrypoints

There are two entrypoints for scripts: `main.lua` and `preinit.lua`.
- `main.lua`: This file is executed during the startup phase of LJE. This is after `init.lua` has executed, but before any other scripts or addons have loaded. This is the main entrypoint for most scripts.
- `preinit.lua`: This file is executed during the pre-init phase of LJE. This is before `init.lua` has executed, and is useful for scripts that need to set up detours or modify the environment before any other code runs.

Note, `main.lua` means any code from the server *could* have executed before it. Be cautious in what you trust from the global environment in `main.lua`. 
`preinit.lua` is safer in this regard, but not many things are available at that point.


## Example script structure

Here is an example structure of a script named `MyScript`:
```
.lje_scripts
└── MyScript
    ├── info.toml
    ├── main.lua
    └── preinit.lua
```

## Dependencies

Dependencies are specified in the `info.toml` file under the `dependencies` field.
When a script is loaded, LJE will ensure that all dependencies are loaded first.
This allows scripts to share functionality and build upon each other like GMod addons, where one script provide sharing via the global environment.

# Writing safe scripts

LJE is advanced enough to make most code undetectable by default, but there are still some pitfalls to avoid when writing scripts.
- Don't call arbitrary Lua functions from LJE code, as they may be able to inspect the call stack or trigger a flag to ban you.
- Avoid detouring random functions unless absolutely necessary, as this increases the attack surface for detection.
- Be cautious when interacting with foreign code (e.g: a custom hook.Add detour that calls user hooks)
  - This does not always lead to detection, but it increases the risk.
  - Additionally, foreign code can create unexpected assumptions for your code, leading to errors.
- Always test your scripts thoroughly in a replica of the target environment to ensure they are undetectable.

## Errors

Errors are a big part of detection. Anticheats can use the native error reporting system to log any suspicious errors that occur during execution.
They can also wrap functions in pcall/xpcall to catch errors and inspect them for suspicious content.

LJE automatically sinks any unhandled LJE errors to avoid this. Additionally (and quite helpfully), LJE automatically
determines if there is a foreign pcall/xpcall in the call stack, and if so, it will sink the error to avoid detection.
Any LJE pcall or xpcall will behave as normal.

All errors are logged to the LJE console for debugging purposes. Code should still strive to avoid errors as much as possible, as they can potentially be used to fingerprint detour behavior or other LJE-specific code paths,
leading to detection. Any LJE error that happens to surface to foreign code is a potential detection vector since any foreign code above LJE will terminate abnormally, which may or may not get
detected by anti-cheat systems. However, any LJE error which happens entirely within LJE code is not a detection vector, as it is fully contained within LJE.

As of writing, any C-handled errors tend to be unstable as LJE's error handling mechanism is inherently quite contrary to Lua's normal error handling model. Most issues are fixed,
but some edge cases remain, such as unhandled LJE errors in simple timers leading to a Lua stack leak. Do note, even in these edge cases, detection is still avoided, but stability may be compromised.

## Writing detours

Here is a simple detour that is undetectable in LJE:
```lua
_G.blah = lje.detour(_G.blah, function(...)
    print("Detoured!")
    return _G.blah(...) -- This is bad!
end)
```

The entire point of LJE is to make it so that simple code is undetectable, and you can keep writing your idioms and patterns as normal.
Things like function calls or metatable accesses are automatically remapped or silenced to avoid detection.

Of course, any kind of introspection done by foreign code is *fundamentally* blocked. It is basically impossible for foreign code to
detect LJE code using any introspection facility. That being said, very advanced anti-cheat systems may still be able to detect LJE code via side-channels or timing attacks.

It's for this reason that writing LJE code should still be done with care, and you should avoid doing anything that involves heavy interaction with foreign code as much as possible.
For example, *never* call an arbitrary Lua function from LJE code, barring detours, as that function may be able to inspect the call stack or trigger a flag to ban you.

Detouring a metatable should be done as follows:
```lua
local playerMetaTable = FindMetaTable("Player")
playerMetaTable.SetHealth = lje.detour(playerMetaTable.SetHealth, function(self, health)
    lje.con_printf("Setting health to $red{%d}", health)
    return self:SetHealth(health)
end)
```

It is possible to continue using the `:` syntax sugar here, because LJE automatically remaps metatable accesses to known-good metatables in `cloned_mts`.
It also means you don't need to store the original function yourself, as LJE handles that internally for **known-good** metatables. These are simply
just pre-init copies of the metatables that LJE knows about at that time, which are most of the base GMod metatables.

Random number generation can be freely used no matter where, as LJE automatically isolates the PRNG state for you.


# API Reference

The `lje` table contains all LJE-specific functionality for scripts.

## base functions

- `lje.include(path: string, execute: boolean = true) -> any`: Includes and optionally executes a Lua file from the script's folder. The path is relative to the script's root folder. If `execute` is false, the file is only loaded and not executed.
- `lje.con_print(message: string)`: Prints a message to the LJE console with LJE formatting.
- <font color="#2242e3">(GLua)</font> `lje.con_printf(format: string, ...)`: Prints a formatted message to the LJE console with LJE formatting. You can wrap text in color tags like `$red{this is red}`.
- <font color="#2242e3">(GLua)</font> `lje.detour(original: function, detour: function) -> function`: Detours a function. The detour function should call the original function as needed. Returns the detoured function.
- <font color="#2242e3">(GLua)</font> `lje.require(path: string) -> any`: Requires a Lua module from the script's folder. The difference between this and `lje.include` is that `lje.require` caches the module, so subsequent calls to `lje.require` with the same path will return the cached module instead of reloading it.
- <font color="#2242e3">(GLua)</font> `lje.get_global(...: vararg<string>)`: Traverses _G using the given path in the vararg, for example `lje.get_global("hook", "Call")` returns hook.Call
- <font color="#2242e3">(GLua)</font> `lje.get_global_static(paths: array<string>, count: integer)`: Has the same behaviour as lje.get_global, but instead of using a vararg, you need to pass the array of paths, as well as the number of elements to be used within that array
- <font color="#2242e3">(GLua)</font> `cloned_mts`: A table containing copies of known good metatables for various object types.

## `func` API

- `lje.func.spoof(spoof: function, target: function) -> function`: Spoofs a function to appear as another function.
- `lje.func.is_spoofed(func: function) -> boolean, function`: Checks if a function is spoofed. If it is, returns true and the original function.
- `lje.func.mark_special(func: function)`: Marks a function as special, which means it is protected from detections like debug hooks (although only the code itself, not other functions it calls) and stack inspections.

### Note

Typically `spoof` and `mark_special` are used together to create undetectable functions. There are internal reasons for splitting them up like this, but from a scripting perspective, you can think of them as a single operation.

## `hooks` API

- `lje.hooks.disable()`: Disables debug hooks.
- `lje.hooks.enable()`: Enables debug hooks.
- `lje.hooks.ignore_fn_once(func: function)`: Ignores the next call to the specified function in debug hooks. This is useful for detours that call something else at return, like `unpack` or similar.

### Note

These functions are protected from debug hooks themselves, so you can safely call them from detours or other sensitive code.

## `env` API

- `lje.env.set(tbl: table)`: Sets the scripting environment to the specified table. This is typically only used internally by LJE.
- `lje.env.get() -> table`: Gets the current scripting environment table. This is equal to `_L`.
- `lje.env.disable_metatables()`: Disables metatable resolution globally. This is useful for code that interacts with tables/userdata that may have custom metatables.
- `lje.env.enable_metatables()`: Enables metatable resolution globally. This should be called after `lje.env.disable_metatables()` to restore normal behavior.
- <font color="#e32242">(Deprecated)</font> `lje.env.save_random_state()`: Saves the current PRNG state. This function no longer does anything.
- <font color="#e32242">(Deprecated)</font> `lje.env.restore_random_state()`: Restores the previously saved PRNG state. This function no longer does anything.
- `lje.env.current_script() -> string`: Returns the name of the currently executing script.
- `lje.env.is_lua_involved(offset: integer = 0) -> boolean`: Checks if Lua code is involved in the current call stack. The optional `offset` parameter allows you to skip a number of frames from the top of the stack.

## `util` API

- `lje.util.get_bytecode_hash(func: function) -> number`: Returns the bytecode hash of a function. This is useful for verifying the integrity of functions. It is a simple 32-bit integer hash of the function's bytecode.
- `lje.util.get_call_stack() -> table`: Returns a table representing the current call stack. Each entry in the table contains information about a stack frame, similar to what `debug.getinfo` returns. Very expensive, use sparingly.
- `lje.util.get_registry() -> table`: Returns the actual Lua registry table. Use with caution.
- `lje.util.set_push_string_callback(callback: function)`: Sets a callback function that is called whenever a string is pushed onto the Lua stack. Its use is very limited, and primarily intended for advanced use cases. It is more or less like `debug.sethook` but for string pushes only.
- `lje.util.set_script_hook_callback(callback: fun(name: string, content: string) -> boolean)`: Sets a callback function that is called whenever a script is about to be executed. The callback receives the script's name and content, and should return true to allow execution or false to block it. This is useful for implementing custom script loading logic or security checks.

## `gc` API

- `lje.gc.get_total() -> number`: Returns the total memory used by Lua in bytes.
- `lje.gc.set_total(bytes: number)`: Sets the total memory used by Lua to the specified number of bytes. This is primarily useful for debugging only. Do not use in production code.
- `lje.gc.run_full_gc()`: Simple shorthand for running a full garbage collection cycle.

## `vm` API

- `lje.vm.patch_bytecodes()`: Patches relevant bytecode instructions in the current Lua VM. This is used internally, do not call it manually.
- <font color="#2242e3">(GLua)</font> `add_engine_call_hook(callback: function)`: Adds the given callback to an array of callbacks which are called whenever set_engine_call_hook fires
- <font color="#2242e3">(GLua)</font> `remove_engine_call_hook(callback: function)`: Removes the given callback from the engine hook callback array
- <font color="#2242e3">(GLua)</font> <font color="#7eb547">(Internal)</font> `set_engine_call_hook(callback: function)`: Sets the function to be called whenever the engine tries to call a lua function - Use add_engine_call_hook instead of this as incorrect usage of this function will break other scripts using it

## `data` API

Data is written to `%USERPROFILE%\.lje_data`. They are flat blobs of data, each ending in `.dat` with no subdirectories allowed.
Funky characters in names are rejected to avoid path traversal.

- `lje.data.write(name: string, data: string)`: Writes data to a file with the specified name.
- `lje.data.read(name: string) -> string | nil`: Reads data from a file with the specified name. Returns nil if the file does not exist.