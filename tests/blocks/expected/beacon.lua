function onInteract(self)
    if not (Persist.Get("beacon_lit") == 1) then
        Light.SetEnabled(Light.Find("Beacon Light"), true)
        Persist.Set("beacon_lit", 1)
        Debug.Log("lit by " .. Actor.GetName(self))
    end
end

function onButtonPress(self, button)
    if button == Input.SELECT then
        Light.SetEnabled(Light.Find("Beacon Light"), false)
        Persist.Set("beacon_lit", 0)
    end
end

function onSceneCreationEnd()
    if Persist.Get("beacon_lit") == 1 then
        Light.SetEnabled(Light.Find("Beacon Light"), true)
    else
        Light.SetEnabled(Light.Find("Beacon Light"), false)
    end
end
