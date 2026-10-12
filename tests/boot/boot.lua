-- Takes a screenshot at frame BOOT_FRAME, writes BOOT_OUT/shot.raw, prints its size, quits.
local outdir = os.getenv('BOOT_OUT')
local target = tonumber(os.getenv('BOOT_FRAME') or '600')
-- BOOT_HOLD=<bit>:<frame> holds one pad 1 button from that frame on (bit 4 = Up), to move the player.
local holdBit, holdFrom = (os.getenv('BOOT_HOLD') or ''):match('^(%d+):(%d+)$')
local frame = 0
function DrawImguiFrame()
  frame = frame + 1
  if holdBit and frame == tonumber(holdFrom) then PCSX.SIO0.slots[1].pads[1].setOverride(tonumber(holdBit)) end
  if frame == target then
    local ss = PCSX.GPU.takeScreenShot()
    local f = Support.File.open(outdir .. '/shot.raw', 'TRUNCATE')
    f:writeMoveSlice(ss.data); f:close()
    print('SHOT ' .. ss.width .. ' ' .. ss.height)
    PCSX.quit(0)
  end
end
