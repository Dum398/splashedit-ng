local score = 0
local speed = FixedPoint.new(1) / 2
local label = "Score: "
local done = false

function onCreate(self)
    for i = 1, 3 do
        score = score + 1
        for j = 1, 2 do
            speed = speed - FixedPoint.new(1) / 4
        end
    end
    if score > (score + 2) * 3 % 4 then
        speed = score - (1 - FixedPoint.new(1) / 10)
    else
        done = not (score == -3)
    end
    Debug.Log(label .. score .. "!")

    label = "Count \"x\"\n"
end

function onUpdate(self, dt)
    while not (done or score > 100) do
        score = score + speed / (FixedPoint.new(5) / 4)
    end
    while score < 0 and not done do
        score = Random.Range(1, 10)
    end
end
