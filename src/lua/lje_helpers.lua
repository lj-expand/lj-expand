-- Pure-Lua helpers for the secure environment. This file contains no initialization
-- logic (no C API pulls, registry hacks or metatable setup) -- only self-contained
-- helper functions. It runs alongside lje_secure_preinit.lua.

lje.includeCache = {}
lje.require = function(path)
  local currentScript = lje.env.current_script()
  if not currentScript then
    lje.con_print("Error: lje.require called outside of a script context!")
    return
  end

  lje.includeCache[currentScript] = lje.includeCache[currentScript] or {}
  local scriptCache = lje.includeCache[currentScript]
  if scriptCache[path] then
    return scriptCache[path]
  end

  local result = lje.include(path)
  scriptCache[path] = result
  return result
end

-- Neatly print out any value using lje.con_print. Handles nested tables with
-- indentation and guards against recursive (cyclic) references.
function lje.util.inspect(x)
    local seen = {}

    local function valueToString(v, indent)
        local t = type(v)

        if t == "string" then
            return '"' .. v .. '"'
        elseif t == "number" or t == "boolean" then
            return tostring(v)
        elseif t == "nil" then
            return "nil"
        elseif t ~= "table" then
            -- function, userdata, thread, cdata, etc.
            return "<" .. t .. ": " .. tostring(v) .. ">"
        end

        -- It's a table from here on.
        if seen[v] then
            return "<recursion: " .. tostring(v) .. ">"
        end
        seen[v] = true

        local childIndent = indent .. "  "
        local out = "{\n"
        local isEmpty = true

        for k, val in pairs(v) do
            isEmpty = false

            local keyStr
            if type(k) == "string" then
                keyStr = k
            else
                keyStr = "[" .. tostring(k) .. "]"
            end

            out = out .. childIndent .. keyStr .. " = " .. valueToString(val, childIndent) .. ",\n"
        end

        -- Allow this table to be visited again on sibling branches once we're done with it.
        seen[v] = nil

        if isEmpty then
            return "{}"
        end

        return out .. indent .. "}"
    end

    lje.con_print(valueToString(x, ""))
end

-- Recursively deep copy a value. Tables are duplicated key-by-key; everything
-- else (functions, userdata, cdata, etc.) is referenced as-is since those can't
-- be meaningfully duplicated. Cyclic references are preserved via the `seen` map,
-- and metatables are kept (by reference, as they're shared).
local function deepCopy(value, seen)
    if type(value) ~= "table" then
        return value
    end

    seen = seen or {}
    if seen[value] then
        return seen[value]
    end

    local copy = {}
    seen[value] = copy

    for k, v in pairs(value) do
        copy[k] = deepCopy(v, seen)
    end

    local mt = getmetatable(value)
    if mt ~= nil then
        setmetatable(copy, mt)
    end

    return copy
end

-- Little printf console helper with color parsing.
-- Usage: lje.con_printf("$red{Error}: Something happened!")
--        lje.con_printf("$$red{$white{ FATAL }} disk on fire")   -- $$ = background
--        lje.con_printf("$#ff8800{orange-ish} $$#202020{dark bg}")
--
-- `$name{...}` colors the foreground, `$$name{...}` the background. Names can be
-- one of the base ANSI colors, a bright/extended color, a `#rrggbb` (or `#rgb`)
-- hex triplet, or a raw 0-255 xterm-256 index. Blocks nest, and a nested block
-- restores whatever was active around it when it closes -- so combining a
-- background with a foreground is just `$$blue{$white{text}}`.

-- Base ANSI 8: foreground is 30+n / bright 90+n, background 40+n / bright 100+n.
local ANSI_BASE = {
    black = 0,
    red = 1,
    green = 2,
    yellow = 3,
    blue = 4,
    magenta = 5,
    cyan = 6,
    white = 7,
}

-- Everything past the base 8 lives in the xterm-256 cube. Keeps the palette wide
-- without needing truecolor support on ancient consoles.
local XTERM_COLORS = {
    orange = 208,
    pink = 218,
    hotpink = 205,
    purple = 93,
    violet = 135,
    indigo = 54,
    lime = 154,
    mint = 121,
    teal = 30,
    turquoise = 45,
    navy = 18,
    maroon = 88,
    olive = 100,
    brown = 130,
    gold = 220,
    tan = 180,
    salmon = 209,
    coral = 203,
    lavender = 183,
    silver = 250,
    slate = 244,
    charcoal = 236,
    cream = 230,
    darkgreen = 22,
    darkblue = 19,
    darkred = 52,
}

-- Non-color attributes, usable with the same `$name{...}` syntax.
local ANSI_STYLES = {
    bold = "1",
    dim = "2",
    italic = "3",
    underline = "4",
    blink = "5",
    reverse = "7",
    invert = "7",
    hidden = "8",
    strike = "9",
}

local RESET = "\x1b[0m"

-- Turn a color name into the SGR parameters for the requested layer.
-- `layer` is 0 for foreground, 10 for background (the ANSI offset between them).
local function resolveColor(name, layer)
    local lowered = string.lower(name)

    -- #rrggbb / #rgb -> truecolor
    local hex = string.match(lowered, "^#(%x+)$")
    if hex then
        local r, g, b
        if #hex == 6 then
            r = tonumber(string.sub(hex, 1, 2), 16)
            g = tonumber(string.sub(hex, 3, 4), 16)
            b = tonumber(string.sub(hex, 5, 6), 16)
        elseif #hex == 3 then
            -- #abc expands to #aabbcc
            r = tonumber(string.rep(string.sub(hex, 1, 1), 2), 16)
            g = tonumber(string.rep(string.sub(hex, 2, 2), 2), 16)
            b = tonumber(string.rep(string.sub(hex, 3, 3), 2), 16)
        else
            return nil
        end
        return (38 + layer) .. ";2;" .. r .. ";" .. g .. ";" .. b
    end

    -- Raw xterm-256 index
    local index = string.match(lowered, "^%d+$")
    if index then
        index = tonumber(index)
        if index < 0 or index > 255 then
            return nil
        end
        return (38 + layer) .. ";5;" .. index
    end

    if lowered == "default" then
        return tostring(39 + layer)
    end

    -- gray/grey are the bright black slot; `light`/`bright` prefixes both work.
    if lowered == "gray" or lowered == "grey" then
        lowered = "brightblack"
    end

    local bare = string.match(lowered, "^bright(%a+)$") or string.match(lowered, "^light(%a+)$")
    if bare and ANSI_BASE[bare] then
        return tostring(90 + layer + ANSI_BASE[bare])
    end

    if ANSI_BASE[lowered] then
        return tostring(30 + layer + ANSI_BASE[lowered])
    end

    if XTERM_COLORS[lowered] then
        return (38 + layer) .. ";5;" .. XTERM_COLORS[lowered]
    end

    -- Styles are foreground-only; `$$bold{}` makes no sense so it falls through.
    if layer == 0 and ANSI_STYLES[lowered] then
        return ANSI_STYLES[lowered]
    end

    return nil
end

-- `$` or `$$`, a color/style name, then a balanced {...} block.
local COLOR_PATTERN = "(%$%$?)([%w#]+)(%b{})"

-- Recursively expand color blocks. `active` is the sequence that re-establishes
-- the state of the enclosing block, so a closing tag doesn't wipe its parent.
local function expandColors(text, active)
    return (string.gsub(text, COLOR_PATTERN, function(sigil, name, braced)
        local layer = (sigil == "$$") and 10 or 0
        local body = string.sub(braced, 2, -2)

        local code = resolveColor(name, layer)
        if not code then
            -- Unknown name: leave the source text alone rather than eating it,
            -- so typos are visible instead of silently swallowing the braces.
            return sigil .. name .. "{" .. expandColors(body, active) .. "}"
        end

        -- Opening only needs the new attribute; `nested` is the full state that
        -- children replay after they reset.
        local open = "\x1b[" .. code .. "m"
        local nested = active .. open
        return open .. expandColors(body, nested) .. RESET .. active
    end))
end

lje.con_printf = function(fmt, ...)
    local result = string.format(fmt, ...)
    lje.con_print(expandColors(result, "") .. RESET) -- reset color at the end
end

-- GMod's print is messed up. Just redirect to ours
function print(...)
    lje.con_print(table.concat({ ... }, "\t"))
end

function Color(r, g, b, a)
    return { r = r, g = g, b = b, a = a or 255 }
end

-- Binds a settings object to the calling script. Call at load time (when there is
-- a current script) and reuse the returned object anywhere, including deferred
-- callbacks like hooks/render/timers where there is no current script context.
function lje.settings.open()
    local name = lje.env.current_script()
    if not name then
        lje.con_print("lje.settings.open() called with no current script context!")
        return nil
    end
    return lje.settings.bind(name)
end

-- Easy helpers for adding engine call hooks based on order. Defaults to client.
function lje.vm.add_pre_engine_call_hook(hook, state)
    lje.vm.add_engine_call_hook(hook, false, state)
end

function lje.vm.add_post_engine_call_hook(hook, state)
    lje.vm.add_engine_call_hook(hook, true, state)
end

-- Legacy helper, assumes pre-engine call hook. Use add_pre_engine_call_hook or add_post_engine_call_hook instead.
function lje.vm.set_engine_call_hook(hook)
    lje.vm.add_engine_call_hook(hook, false)
end