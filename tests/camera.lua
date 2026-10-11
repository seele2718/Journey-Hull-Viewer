local active, angle, debugTerrain, normalTerrain = false, false, true, true
local debug = {
  SetAngleLock = function(self, v) angle = v end,
  GetAngleLock = function(self) return angle end,
}
local camera = {
  StartDebugCam = function(self) active = true end,
  StopDebugCam = function(self) active = false end,
  GetDebugCam = function(self) return debug end,
  UsingDebugCam = function(self) return active end,
}
game = { cameraSystem = function(self) return camera end }
Vars = {
  DebugCamera = { collideTerrain = function(v)
    if v ~= nil then debugTerrain = v end
    return debugTerrain
  end },
  CameraSystem = { defCollideTerrain = function(v)
    if v ~= nil then normalTerrain = v end
    return normalTerrain
  end },
}
assert(commands.HV_CAM_READ() == '0110')
assert(commands.HV_CAM_START() == '1110')
assert(commands.HV_CAM_IGNORE() == '1000')
assert(commands.HV_CAM_LOCK() == '1001')
assert(commands.HV_CAM_UNLOCK() == '1000')
assert(commands.HV_CAM_COLLIDE() == '1110')
assert(commands.HV_CAM_STOP() == '0110')
debugTerrain = false
assert(commands.HV_CAM_READ() == '0010') -- Mixed collision states stay distinguishable.
debug = nil
assert(not pcall(commands.HV_CAM_LOCK))
game = nil
assert(not pcall(commands.HV_CAM_START))
print('PASS: all seven exact embedded camera chunks, getters/setters and unavailable-state errors')
