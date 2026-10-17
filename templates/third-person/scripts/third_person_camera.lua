-- Third-person camera, on the Player Body object. Blocks have no camera
-- blocks, so this one is Lua only.
--
-- psxsplash moves the player (its position is the player's eyes) and, by
-- default, puts the camera there: first person. This script takes the camera
-- over, and every frame stands this body where the player is and puts the
-- camera behind and above it, looking at the player's head.
--
-- Positions are in GTE units: one metre is 1/100 at the scene's GTE scaling
-- of 100. Y points down while the game runs, so up is -y.

local DISTANCE = FixedPoint.new(3) / 100   -- 3 m behind the player
local HEIGHT = FixedPoint.new(1) / 100     -- 1 m above the player's eyes
local EYES = FixedPoint.new(18) / 1000     -- the Player's height, 1.8 m: eyes to feet

function onCreate(self)
    Camera.FollowPsxPlayer(false)
end

function onUpdate(self, dt)
    local eyes = Player.GetPosition()
    local turn = Player.GetRotation()
    -- The body's origin is at its feet.
    Entity.SetPosition(self, Vec3.new(eyes.x, eyes.y + EYES, eyes.z))
    Entity.SetRotationY(self, turn.y)
    -- Face the way the player faces, then step back along that direction.
    Camera.SetRotation(Vec3.new(0, turn.y, 0))
    local forward = Camera.GetForward()
    Camera.SetPosition(Vec3.new(eyes.x - forward.x * DISTANCE, eyes.y - HEIGHT, eyes.z - forward.z * DISTANCE))
    Camera.LookAt(eyes)
end
