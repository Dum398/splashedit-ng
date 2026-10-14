-- Boots to frame BOOT_FRAME, writes the GPU's 1 MB VRAM to BOOT_OUT/vram.raw
-- (a PCSX-Redux save state, from which quantpreviewcheck.py lifts the VRAM
-- field), takes a screenshot too, prints their sizes and quits.
local outdir = os.getenv('BOOT_OUT')
local target = tonumber(os.getenv('BOOT_FRAME') or '600')
local frame = 0
function DrawImguiFrame()
  frame = frame + 1
  if frame == target then
    local ss = PCSX.GPU.takeScreenShot()
    local sf = Support.File.open(outdir .. '/shot.raw', 'TRUNCATE')
    sf:writeMoveSlice(ss.data); sf:close()
    print('SHOT ' .. ss.width .. ' ' .. ss.height)
    local st = PCSX.createSaveState()
    local f = Support.File.open(outdir .. '/state.bin', 'TRUNCATE')
    f:writeMoveSlice(st); f:close()
    print('STATE ' .. Support.File.open(outdir .. '/state.bin'):size())
    PCSX.quit(0)
  end
end
