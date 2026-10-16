local started = false

function onSceneCreationEnd()
    started = true
    Cutscene.Play("intro")
    Entity.SetActive(Entity.Find("Door"), true)
end

function onTriggerEnter(index)
    Debug.Log(Timer.GetFrameCount())
end

function onTriggerExit(index)
    if started then
        return
    end
end
