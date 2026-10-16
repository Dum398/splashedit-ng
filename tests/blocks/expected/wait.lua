-- Blocks that wait run as tasks (coroutines); onUpdate resumes them every frame.
local tasks = {}

local function resumeTask(name, ...)
    local ok, err = coroutine.resume(tasks[name], ...)
    if coroutine.status(tasks[name]) == "dead" then
        tasks[name] = nil
    end
    if not ok then
        error(err, 0)
    end
end

-- Starts fn as the task called name, unless that task is still waiting.
local function startTask(name, fn)
    if tasks[name] == nil then
        tasks[name] = coroutine.create(fn)
        resumeTask(name)
    end
end

-- dt counts 4096 per 30fps frame, so a second is 30 * 4096. Always lets a frame pass.
local function wait(seconds)
    local left
    if type(seconds) == "table" then
        left = seconds._raw * 30  -- a FixedPoint keeps 4096 per unit in _raw
    else
        left = seconds * 122880
    end
    repeat
        left = left - coroutine.yield()
    until left <= 0
end

local frames = 0
local lit = false

function onCreate(self)
    startTask("onCreate", function()
        Debug.Log("lamp on")
        wait(2)
        Debug.Log("two seconds, frame " .. frames)
    end)

    Debug.Log("second stack")
end

function onUpdate(self, dt)
    if tasks.onCreate then
        resumeTask("onCreate", dt)
    end
    if tasks.onInteract then
        resumeTask("onInteract", dt)
    end
    if tasks.onButtonPress_CROSS then
        resumeTask("onButtonPress_CROSS", dt)
    end

    frames = frames + 1
end

function onInteract(self)
    startTask("onInteract", function()
        for i = 1, 3 do
            Debug.Log("tick")
            wait(1)
        end
        while not Input.IsHeldPlayer1(Input.SQUARE) do
            coroutine.yield()
        end
        return
    end)
end

function onButtonPress(self, button)
    if button == Input.CROSS then
        startTask("onButtonPress_CROSS", function()
            Entity.SetActive(self, true)
            lit = true
            wait(FixedPoint.new(1) / 2)
            Entity.SetActive(self, false)
        end)
    elseif button == Input.CIRCLE then
        Debug.Log("circle")
    end
end
