---
id: info-toml
title: info.toml Reference
---

# `info.toml` Reference

`info.toml` contains the metadata LJE needs to load a script: its identity, version, and what it depends on. LJE reads this file before running anything, and **skips any script folder that is missing a valid `info.toml`**.

This is the mechanism through which load ordering is established. Dependencies declared here let you build familiar GMod-esque global libraries. LJE guarantees that every dependency has already run before your script starts.

## Fields

All fields live under the `[script]` table.

| Field          | Type              | Required | Description                                                                          |
|----------------|-------------------|----------|--------------------------------------------------------------------------------------|
| `name`         | string            | Yes      | The script's name. Used to identify it as a dependency target.                       |
| `version`      | string            | Yes      | The script's version string (e.g. `"1.0.0"`). Not currently enforced, but required.  |
| `author`       | string            | Yes      | The script's author. Combined with `name` to form the unique identity `author.name`. |
| `URL`          | string            | No       | The script's repository.                                                             |
| `dependencies` | array of strings  | Yes      | Scripts this script depends on. Use `[]` if there are none.                          |
| `binaries`     | array of strings  | No       | Binary module names (excluding `.dll`) this script requires.                         |

## Example

```toml
[script]
name = "myscript"
version = "1.0.0"
author = "yourname"
dependencies = []
```

With dependencies, binary module, and url.:

```toml
[script]
name = "myscript"
version = "1.2.0"
author = "yourname"
url = "https://github.com/yourname/myscript"
dependencies = ["yogwoggf.ljeutil", "someone.otherscript"]
binaries = ["lje-mymodule"]
```

## Dependencies

Dependencies are strings formatted as `<author>.<name>`. LJE uses these to build a load order guaranteeing that every dependency runs before the script that requires it.

If a listed dependency is not installed, LJE prints a warning but still loads the script — it just won't have that dependency available.

```toml
dependencies = ["yogwoggf.ljeutil", "Eyoko1.ljeutil"]
```

## Binary Dependencies (`binaries`)

`binaries` is optional. It lists the names of binary modules (loaded from `.lje_modules`) that the script requires. Names must match the `lje-<name>` portion of the DLL filename, without the extension.

```toml
binaries = ["lje-mymodule"]
```

If any listed binary module is not loaded, **the entire script is skipped**. LJE assumes the script cannot function without it.
