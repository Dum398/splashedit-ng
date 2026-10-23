// Paths (splashedit-ng#208): the one sampler the Scene view, the timeline
// preview and the exporter share, and the export bake.
//
// A PathComponent's points (object space) go through the object's
// localToWorld and make a centripetal Catmull-Rom curve (alpha 0.5: no cusps
// or loops between close points). The curve is walked by arc length: t = 0 is
// the first point, t = 1 the last (a closed path's first again).
//
// On the PS1 there is no path runtime. The exporter turns paths into what
// psxsplash already plays (measured in Redux, ng-deps/paths/probe):
//   - a follower without an Agent: a looping Animation (Object Position and,
//     facing along, Object Rotation keys at most 64 a lap, the frames chosen
//     to keep closest to the curve), up to 4 followers to an animation;
//   - a follower with an Agent: Agent.MoveTo to points along the curve,
//     from a generated Lua driver;
//   - the camera (Path.RideCamera from Lua): the same driver;
//   - a timeline Follow Path track: a Camera or Object Position track.
// The generated Lua (a library file plus a tick on one path object's script)
// gives scripts the Path table: Start, Stop, IsRunning, GetProgress,
// SetProgress, RideCamera, StopCamera.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "scene.hh"

namespace splash {

// psxsplash limits the bake respects (src/animation.hh, src/cutscene.hh, the
// exporter's 64-keyframe check).
constexpr int kPathMaxKeys = 64;
constexpr int kPathMaxAnimations = 16;
constexpr int kPathMaxPlaying = 8;
constexpr int kPathMaxAnimTracks = 8;
constexpr int kPathMaxFrames = 8191;

// The curve as a dense polyline with its arc length. An empty curve (no
// points) has length 0 and samples to the origin.
struct PathCurve {
    std::vector<Vec3> dense;
    std::vector<float> acc;  // arc length to dense[i]
    float length = 0;
    bool closed = false;
};
PathCurve buildCurve(const std::vector<Vec3>& points, bool closed, int perSegment = 32);
// The point at t (0..1 of the arc length, clamped).
Vec3 curvePoint(const PathCurve& c, float t);
// The direction of travel at t (unit; zero for a curve without length).
Vec3 curveDirection(const PathCurve& c, float t);
// The curve of a path object: its points through localToWorld.
PathCurve worldCurve(const FlatObject& fo);
// The control points in world space.
std::vector<Vec3> worldPoints(const FlatObject& fo);

// Where on the path a rider is after `laps` laps (0 = its start): once
// clamps at 1, loop wraps, ping-pong runs 0..1..0 every two laps.
float modeProgress(PathMode m, float laps);
// Quaternion.Euler angles (degrees) that turn +Z onto dir: yaw about Y,
// pitch about X (positive looks down).
float yawDegrees(Vec3 dir);
float pitchDegrees(Vec3 dir);

// Picks keys from a value per frame (frame i = values[i]): the first and
// last frames always, at most maxKeys, chosen so that linear interpolation
// between them strays least from the values (each value a point; its
// distance is the error). Returns the frames, ascending; *maxError is the
// largest distance left (0 when every frame is a key).
std::vector<int> fitKeys(const std::vector<std::vector<float>>& values, int maxKeys, float* maxError);

// A Follow Path timeline track baked into the Camera Position (no target) or
// Object Position track the console plays, from the scene's path of that
// name: keys at most kPathMaxKeys, from the first progress key's frame to the
// last. problem (when not null) says why nothing could be baked (no such
// path, no keys), or that the cap cut corners by more than 5 cm.
CutsceneTrack bakeFollowTrack(const Scene& scene, const CutsceneTrack& track, std::string* problem = nullptr);

// The export's view of a scene with paths: `scene` with the generated
// animations added, follow-path tracks baked and the tick host's script
// replaced by `hostScript`; the Lua files the export adds.
struct PathBake {
    Scene scene;
    // Virtual Lua files by path (never on disk). The library goes in the
    // pack whether or not an object names it; the host file is the epilogue
    // that follows the host's own script (hostUserScript, may be empty).
    std::string libraryPath, hostPath, hostUserScript;
    std::map<std::string, std::string> files;
    std::vector<std::string> warnings, errors;
    // Facts for --stats and tests.
    struct Group {
        std::string animation, path;
        std::vector<std::string> followers;
        int lapFrames = 0, keys = 0;
        float maxError = 0;
    };
    std::vector<Group> groups;
    // Each agent follower and the points it walks to (world metres), for the
    // export's check against the baked nav regions.
    std::vector<std::pair<std::string, std::vector<Vec3>>> agentRoutes;
};
bool sceneUsesPaths(const Scene& scene);
// readSource reads a project script (for the RideCamera scan); may be empty.
PathBake bakePaths(const Scene& scene, const std::function<std::string(const std::string&)>& readSource = {});

}  // namespace splash
