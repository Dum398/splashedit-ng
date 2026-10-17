-- Lever, written in Lua. Walk up to the lever and press Square: each pull
-- switches the lamp next to it on or off and counts the pulls.
-- The other lever runs scripts/lever_blocks.blocks, the same thing as blocks.

local lit = true
local pulls = 0

-- Called when the player presses the Interactable's button near this object.
function onInteract(self)
    lit = not lit
    pulls = pulls + 1
    Entity.SetActive(Entity.Find("Lamp (Lua)"), lit)
    Debug.Log("Lua lever pulled " .. pulls .. " times")
end
