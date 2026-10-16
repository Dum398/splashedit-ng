function onUpdate(self, dt)
    if Input.IsHeldPlayer1(Input.SQUARE) then
        Entity.SetPosition(self, Vec3.add(Entity.GetPosition(self), Vec3.new(FixedPoint.new(1) / 2, 0, 0)))
    end
    if Entity.GetPosition(self).x < -10 then
        Entity.SetPosition(self, Vec3.new(-10, Entity.GetPosition(self).y, Entity.GetPosition(Entity.Find("Crate")).z))
    end
end

function onButtonPress(self, button)
    if button == Input.CROSS then
        Entity.SetPosition(self, Vec3.add(Entity.GetPosition(self), Vec3.new(0, 1, 0)))

        Cutscene.Play("intro")
    elseif button == Input.START then
        Scene.Load(1)
    end
end

function onButtonRelease(self, button)
    if button == Input.CIRCLE then
        Entity.SetPosition(Entity.Find("Crate"), Vec3.new(0, 0, -3))
    end
end
