-- Sprite boot check: both sheets and the animations resolve by name to the
-- indices the exporter wrote (hero 0, fx 1; walk 0, jump 1, spark 2), a sprite
-- is made from each sheet and plays an animation, and Sprite.Count sees both.
-- Prints SPRITE OK, else SPRITE FAIL with the values (and BOOT FAIL, which
-- stops boot.sh early).
function onSceneCreationEnd()
    local hero, fx = Sprite.SheetIndex("hero"), Sprite.SheetIndex("fx")
    local walk, jump, spark = Sprite.AnimIndex("walk"), Sprite.AnimIndex("jump"), Sprite.AnimIndex("spark")
    local none = Sprite.AnimIndex("nope")
    local a, b = Sprite.Create("hero"), Sprite.Create(fx)
    local ok = hero == 0 and fx == 1 and walk == 0 and jump == 1 and spark == 2 and none == -1 and a >= 0 and b >= 0
    if ok then
        Sprite.SetPos(a, 120, 96)
        Sprite.PlayAnim(a, "walk")
        Sprite.SetPos(b, 184, 100)
        Sprite.PlayAnim(b, spark)
        ok = Sprite.Count() == 2
    end
    local got = "sheets " .. tostring(hero) .. "," .. tostring(fx) .. " anims " .. tostring(walk) .. "," .. tostring(jump) ..
                "," .. tostring(spark) .. "," .. tostring(none) .. " ids " .. tostring(a) .. "," .. tostring(b) ..
                " count " .. tostring(Sprite.Count())
    if ok then
        Debug.Log("SPRITE OK " .. got)
    else
        Debug.Log("SPRITE FAIL " .. got)
        Debug.Log("BOOT FAIL sprites")
    end
end
