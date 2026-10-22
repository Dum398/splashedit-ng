// The Path table: not psxsplash's, the exporter writes it (core/path.cpp) for
// scenes with paths. Hand-kept, unlike lua_api.inc. A path is named by its
// object's name, or passed as the object (Entity.Find, self); a follower
// stands for its path, Camera for the camera's ride. Progress is in percent.
{"Path", "", ApiKind::Table, "Path", "Paths of this scene (the export generates it when the scene has a Path component)."},
{"Path", "Start", ApiKind::Function, "Path.Start(path)", "Starts the path's followers; nothing while they run. Followers moved by animation start again from their start offset."},
{"Path", "Stop", ApiKind::Function, "Path.Stop(path)", "Stops the path's followers where they are."},
{"Path", "IsRunning", ApiKind::Function, "Path.IsRunning(path|follower|Camera) -> boolean", "Whether the path's followers (or that follower, or the camera's ride) are riding."},
{"Path", "GetProgress", ApiKind::Function, "Path.GetProgress(path|follower|Camera) -> number or nil", "How far along, in percent (0..100). Exact for agents and the camera; for followers moved by animation it is counted from the time since Start and can drift by up to a frame a lap. nil for an unknown path, or Camera when it rides none."},
{"Path", "SetProgress", ApiKind::Function, "Path.SetProgress(path|follower|Camera, percent)", "Puts agent followers or the camera's ride that far along. Followers moved by animation cannot jump on the PS1: they carry on, and the first try logs a warning."},
{"Path", "RideCamera", ApiKind::Function, "Path.RideCamera(path [, faceAlong])", "Takes the camera off the player and moves it along the path at the path's speed, looking along it unless faceAlong is false."},
{"Path", "StopCamera", ApiKind::Function, "Path.StopCamera()", "Gives the camera back to the player."},
