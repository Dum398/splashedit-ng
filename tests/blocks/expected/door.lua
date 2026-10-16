local opened = false

function onInteract(self)
    if not opened then
        opened = true
        Entity.SetActive(Entity.Find("Door"), false)
        Audio.Play("creak")
    end
end
