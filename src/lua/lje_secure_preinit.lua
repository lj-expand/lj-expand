-- Secure preinit script. No need for a custom environment as this takes place entirely within a
-- separate Lua universe. All we do here is ensure that all the C API is pulled in, add a hook library,
-- ensure metatables exist in registry and perform some GMod-specific registry hacks.
-- Pure-Lua helpers live in lje_helpers.lua.

lje.con_print("Running secure preinit script...")

local registry = lje.util.get_registry()

-- Shadow registry stubs live in lje_shadow_init.lua

-- Only a subset of necessary GMod C APIs are pulled in. We have our own versions of any base library as well.
achievements = lje.secure.pull("achievements")
cam = lje.secure.pull("cam")
chat = lje.secure.pull("chat")
engine = lje.secure.pull("engine")
ents = lje.secure.pull("ents")
file = lje.secure.pull("file")
game = lje.secure.pull("game")
gameevent = lje.secure.pull("gameevent")
gmod = lje.secure.pull("gmod")
gui = lje.secure.pull("gui")
input = lje.secure.pull("input")
net = lje.secure.pull("net")
render = lje.secure.pull("render")
sound = lje.secure.pull("sound")
sql = lje.secure.pull("sql")
surface = lje.secure.pull("surface")
steamworks = lje.secure.pull("steamworks")
system = lje.secure.pull("system")
timer = lje.secure.pull("timer")
util = lje.secure.pull("util")
player = lje.secure.pull("player")
vgui = lje.secure.pull("vgui")

-- Globals
local globalEnv = lje.secure.pull("_G")
for k, v in pairs(globalEnv) do
  if _G[k] == nil then
    _G[k] = v     -- Merge
  end
end

-- Classes
registry.Entity = lje.secure.pull("_R.Entity")
registry.Player = lje.secure.pull("_R.Player")
registry.Vector = lje.secure.pull("_R.Vector")
registry.Angle = lje.secure.pull("_R.Angle")
registry.CUserCmd = lje.secure.pull("_R.CUserCmd")
registry.File = lje.secure.pull("_R.File")
registry.ConVar = lje.secure.pull("_R.ConVar")
registry.VMatrix = lje.secure.pull("_R.VMatrix")
registry.Weapon = lje.secure.pull("_R.Weapon")
registry.IMaterial = lje.secure.pull("_R.IMaterial")
registry.ITexture = lje.secure.pull("_R.ITexture")
registry.PhysObj = lje.secure.pull("_R.PhysObj")
registry.Vehicle = lje.secure.pull("_R.Vehicle")
registry.Panel = lje.secure.pull("_R.Panel")
registry.CSEnt = lje.secure.pull("_R.CSEnt")
registry.NPC = lje.secure.pull("_R.NPC")

NULL = lje.secure.pull("NULL") -- For some reason, the null entity is a special userdata..?
registry.NULL = NULL
-- Polyfills for common things
local cam2D = { type = "2D" }
cam.Start2D = function()
  cam.Start(cam2D)
end

-- Small hook library replacement, no returns for now.
hook = {}
hook._listeners = {}

function hook.Add(event, identifier, func)
  local script = lje.env.current_script()
  hook._listeners[script] = hook._listeners[script] or {}
  if not hook._listeners[script][event] then
    hook._listeners[script][event] = {}
  end
  hook._listeners[script][event][identifier] = func
end

function hook.Listen(post)
  post = post or false
  local script = lje.env.current_script()
  if not hook._listeners[script] then
    hook._listeners[script] = {}
  end

  lje.vm.add_engine_call_hook(function(func, nargs, nresults, name, gm, ...)
    if name and hook._listeners[script][name] then
      for _, listener in pairs(hook._listeners[script][name]) do
        listener(...)
      end
    end
  end, post)
end

function hook.Call() --[[no-op]] end
