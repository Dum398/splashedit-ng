-- SplashEdit frame capture, loaded into pcsx-redux by Play (-dofile).
--
-- Every 10 vsyncs it looks for capture.flag in the PCdrv directory (the one
-- Play passes as -pcdrvbase). When the editor writes it, this records one
-- frame with redux's GPU dump recorder into capture.gpd.part, renames it to
-- capture.gpd once complete and deletes the flag. When it cannot, it writes
-- the reason to capture.err instead.
--
-- The recorder arms on startDump() and begins at the first vsync where the
-- GPU is between commands, then writes each vsync's traffic at that vsync
-- (redux runs the recorder before Lua listeners). A game drawing at 20 or 30
-- fps leaves most vsync intervals empty, so recording stops at the end of the
-- first interval that carried drawing (more than kFrameBytes), or after
-- kMaxVsyncs intervals without any.

local function base()
    -- One of 2 linked consoles: capture files in its own directory
    -- (capturedir.lua, run first), as both share the PCdrv one.
    if type(SplashEditCaptureDir) == 'string' and SplashEditCaptureDir ~= '' then return SplashEditCaptureDir end
    local ok, dir = pcall(function() return PCSX.settings.emulator.Debug.PCdrvBase end)
    if not ok or type(dir) ~= 'string' or dir == '' then return nil end
    return dir
end

local function at(name)
    local dir = base()
    if not dir then return nil end
    local last = dir:sub(-1)
    if last ~= '/' and last ~= '\\' then dir = dir .. '/' end
    return dir .. name
end

local function exists(path)
    local f = io.open(path, 'rb')
    if f then
        f:close()
        return true
    end
    return false
end

local function fail(message)
    os.remove(at('capture.gpd.part'))
    local f = io.open(at('capture.err'), 'wb')
    if f then
        f:write(message)
        f:close()
    end
    os.remove(at('capture.flag'))
    print('framecapture: ' .. message)
end

local kFrameBytes, kMaxVsyncs = 256, 120
local vsyncs, armedAt, state, file, lastPos = 0, 0, 'idle', nil, nil

local function finish()
    PCSX.GPU.stopDump()
    state, file, lastPos = 'idle', nil, nil
    os.remove(at('capture.gpd'))
    local ok, err = os.rename(at('capture.gpd.part'), at('capture.gpd'))
    if not ok then
        fail('cannot rename the capture: ' .. tostring(err))
        return
    end
    os.remove(at('capture.flag'))
    print('framecapture: wrote ' .. at('capture.gpd'))
end

SplashEditFrameCapture = PCSX.Events.createEventListener('GPU::Vsync', function()
    vsyncs = vsyncs + 1
    if state == 'idle' then
        if vsyncs % 10 ~= 0 or not base() or not exists(at('capture.flag')) then return end
        os.remove(at('capture.gpd'))
        os.remove(at('capture.err'))
        if type(PCSX.GPU.startDump) ~= 'function' then
            fail('this pcsx-redux has no GPU dump recorder; update it to capture frames')
            return
        end
        local ok, err = pcall(function()
            file = Support.File.open(at('capture.gpd.part'), 'TRUNCATE')
            PCSX.GPU.startDump(file)
        end)
        if not ok then
            fail('cannot start the GPU dump: ' .. tostring(err))
            return
        end
        state, armedAt = 'armed', vsyncs
    elseif PCSX.GPU.isDumping() then
        -- The write position after this vsync's flush: the header and VRAM at
        -- the start, then one interval's traffic per vsync.
        local pos = tonumber(file:wTell())
        if lastPos and PCSX.GPU.getDumpFrames() >= 1 and
            (pos - lastPos > kFrameBytes or PCSX.GPU.getDumpFrames() >= kMaxVsyncs) then
            finish()
            return
        end
        lastPos = pos
    elseif vsyncs - armedAt > 600 then
        PCSX.GPU.stopDump()
        state = 'idle'
        fail('the GPU never went idle at a vsync; no frame was captured')
    end
end)
