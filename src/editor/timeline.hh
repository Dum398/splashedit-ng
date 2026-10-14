// The timeline's model: edits to a cutscene's or an animation's tracks and
// keys, each one undo step through the Document, and sampling a track at a
// frame the way psxsplash's CutscenePlayer and AnimationPlayer do. No ImGui.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "scene.hh"

namespace editor {

class Document;

// psxsplash's limits (cutscene.hh) and frame rate.
constexpr int kTimelineFps = 30;
constexpr int kMaxTimelineTracks = 8;
constexpr int kMaxTimelineKeys = 64;
constexpr int kMaxTimelineFrame = 8191;  // 13 bits of the packed key
// The exporter's name check (splashpack.cpp writeSequences): 1..24 UTF-16
// code units, as Lua plays a sequence by name.
constexpr int kMaxSeqName = 24;

enum class SeqKind { Cutscene, Animation };

// A cutscene or an animation of the document's scene, by index.
struct SeqRef {
    SeqKind kind = SeqKind::Cutscene;
    int index = -1;
    bool operator==(const SeqRef&) const = default;
};

// A key of a sequence: track index, then key index in that track.
struct KeyRef {
    int track = -1;
    int key = -1;
    bool operator==(const KeyRef&) const = default;
    bool operator<(const KeyRef& o) const { return track != o.track ? track < o.track : key < o.key; }
};

using KeyValue = std::array<float, 3>;

// Track type facts.
// "Object Position", "Light Color", ...
const char* trackTypeName(splash::TrackType t);
// How many of a key's three values the type uses (1, 2 or 3).
int valueCount(splash::TrackType t);
// The runtime holds the last key's value (on/off tracks) instead of interpolating.
bool isDiscrete(splash::TrackType t);
// Camera and rumble tracks drive no named target.
bool needsTarget(splash::TrackType t);
// Animations have no camera tracks.
bool allowedIn(SeqKind kind, splash::TrackType t);
// "Crate: Position", "Camera: Rotation", "hud/bar: Progress".
std::string trackLabel(const splash::CutsceneTrack& t);

// Reading.
bool valid(const Document& doc, SeqRef ref);
const std::string* seqName(const Document& doc, SeqRef ref);
int seqLength(const Document& doc, SeqRef ref);  // frames; 0 when invalid
const std::vector<splash::CutsceneTrack>* seqTracks(const Document& doc, SeqRef ref);
const splash::Keyframe* key(const Document& doc, SeqRef ref, KeyRef k);
// Every cutscene, then every animation.
std::vector<SeqRef> allSequences(const Document& doc);

// Edits. Each is one undo step; a refused edit changes nothing and adds no
// step. Keys stay sorted by frame within a track, and no two share a frame.
//
// Appends an empty sequence named `name` (made unique with " (2)"...).
std::optional<SeqRef> addSequence(Document& doc, SeqKind kind, const std::string& name);
// Why `name` cannot name the sequence, or empty when it can: empty, longer
// than kMaxSeqName, or taken by another sequence of the same kind.
std::string seqNameProblem(const Document& doc, SeqRef ref, const std::string& name);
// Refused (no step) when seqNameProblem() says so or the name is unchanged.
bool renameSequence(Document& doc, SeqRef ref, const std::string& name);
// Removes the sequence; the ones after it move down an index.
bool removeSequence(Document& doc, SeqRef ref);
// The new track's index, or -1: the type is not allowed in this kind of
// sequence, the target is missing where needed, the same type and target is
// already there, or the sequence has kMaxTimelineTracks.
int addTrack(Document& doc, SeqRef ref, splash::TrackType type, const std::string& target);
bool removeTrack(Document& doc, SeqRef ref, int track);
// Points the track at another target, keys unchanged. Refused for tracks
// with no target (camera, rumble), an empty or unchanged target, or one the
// sequence already drives with this type.
bool setTrackTarget(Document& doc, SeqRef ref, int track, const std::string& target);
// What a track of this type can drive, as the export addresses it: objects
// with a mesh, runtime point lights, canvases, "canvas/element". Empty for
// camera and rumble tracks.
std::vector<std::string> targetsFor(const Document& doc, splash::TrackType type);
// Adds a key at `frame` (clamped to 0..kMaxTimelineFrame). Without `value`
// it takes what the track's curve gives there (sample(), before the runtime's
// rounding), so adding a key leaves the motion as it was. Returns the key, or nullopt when the frame already has one or the
// track is full.
std::optional<KeyRef> addKey(Document& doc, SeqRef ref, int track, int frame, std::optional<KeyValue> value = std::nullopt);
// Moves every key in `keys` by `delta` frames. Refused when one would leave
// 0..kMaxTimelineFrame or land on a key that is not moving. Returns the keys'
// new addresses, in the order given. Consecutive moves with the same
// non-empty `mergeKey` are one undo step until Document::endMerge().
std::optional<std::vector<KeyRef>> moveKeys(Document& doc, SeqRef ref, const std::vector<KeyRef>& keys, int delta,
                                            const std::string& mergeKey = {});
// Removes every key in `keys`. Returns how many it removed.
int deleteKeys(Document& doc, SeqRef ref, const std::vector<KeyRef>& keys);
bool setKeyValue(Document& doc, SeqRef ref, KeyRef k, KeyValue value, const std::string& mergeKey = {});
bool setKeyInterp(Document& doc, SeqRef ref, KeyRef k, splash::Interp interp);
// Clamped to 1..kMaxTimelineFrame. Keys past the end stay (the runtime stops
// at the length; they show greyed).
bool setLength(Document& doc, SeqRef ref, int frames, const std::string& mergeKey = {});

// Sampling, in the track's own units. `frame` may be fractional (a sub-frame).
// As the runtime: before the first key the track blends from `initial` (the
// target's state when playback starts) into it with the first key's curve,
// between keys it uses the later key's curve, after the last it holds; on/off
// tracks hold the last key at or before the frame. Values are not quantised
// to the runtime's fixed point (sampleFixed() is).
KeyValue sample(const splash::CutsceneTrack& track, float frame, const KeyValue& initial = {0, 0, 0});

// The runtime's own numbers. A key's value as the exporter packs it
// (splashpack.cpp: positions round(v / gte * 4096) with Y negated, angles
// round(deg * 1024 / 180) with X and Z negated, colours 0..255, ...), and back.
using FixedValue = std::array<int16_t, 3>;
FixedValue encodeValue(splash::TrackType t, const KeyValue& v, float gte);
KeyValue decodeValue(splash::TrackType t, const FixedValue& v, float gte);
// psxsplash's lerpKeyframesSub (interpolation.cpp) on the packed keys: the
// whole frame plus a 0.12 sub-frame, the 0.12 curve, and `>> 12` rounding
// towards minus infinity. On/off tracks as sample(). In the packed units.
FixedValue sampleFixedRaw(const splash::CutsceneTrack& track, float frame, const KeyValue& initial, float gte);
// The same, decoded to the track's units.
KeyValue sampleFixed(const splash::CutsceneTrack& track, float frame, const KeyValue& initial, float gte);
// The curve psxsplash applies to a 0..1 segment position.
float applyCurve(float t, splash::Interp mode);
// The target's state when playback starts, as far as the editor knows it:
// an object's world position or its start-active flag; zero otherwise
// (object rotation starts from 0,0,0 in the runtime too).
KeyValue initialValue(const Document& doc, const splash::CutsceneTrack& track);
// sampleFixed() with initialValue() and the scene's GTE scaling: what the
// console shows at that frame.
KeyValue sampleAt(const Document& doc, SeqRef ref, int track, float frame);

// The Scene view's preview: what a sequence poses at a frame. Built from a
// const Document and drawn from a copy of the scene, so scrubbing never
// edits the document, adds no undo step and leaves the saved scene alone.
struct ObjectPose {
    std::optional<splash::Vec3> position;  // world units, world space
    std::optional<splash::Vec3> rotation;  // degrees, Quaternion.Euler, world space
    std::optional<bool> active;
    std::optional<std::array<float, 2>> uvOffset;  // texels, on the mesh's uvOffsetMaterial
    bool operator==(const ObjectPose&) const = default;
};
// A runtime point light's tracks.
struct LightPose {
    std::optional<splash::Vec3> position;        // world units, world space
    std::optional<std::array<float, 3>> color;   // 0..1
    std::optional<float> intensity;
    std::optional<float> radius;                 // world units: the light's range
    std::optional<bool> enabled;
    bool operator==(const LightPose&) const = default;
};
struct Preview {
    std::map<std::string, ObjectPose> objects;  // by track target (object name)
    std::map<std::string, LightPose> lights;    // by track target (the light's object name)
    std::optional<splash::Vec3> cameraPosition;  // world units
    std::optional<splash::Vec3> cameraRotation;  // degrees
    std::optional<float> cameraH;                // projection H, 1..1024
    bool empty() const { return objects.empty() && lights.empty() && !cameraPosition && !cameraRotation && !cameraH; }
    bool operator==(const Preview&) const = default;
};
// Object position, rotation, active and UV offset tracks, light tracks and
// the camera's position, rotation and H, sampled at `frame` with sampleAt().
// UI and rumble tracks have nothing in the Scene view to pose; the lane
// labels show their value. Empty for an invalid sequence.
Preview samplePreview(const Document& doc, SeqRef ref, float frame);
// Poses a scene (a copy: the caller's, never the document's). Each target,
// the first object of that name in flatten order, gets the sampled world
// position and rotation, its children following; one an active track
// switches off loses its mesh, so it draws nothing. A light target gets its
// colour, intensity, range and enabled flag, and its position moves the
// light's object (the Scene view lights from the scene it draws). UV offsets
// are the renderer's to apply.
void applyPreview(splash::Scene& scene, const Preview& preview);
// Quaternion.Euler: about Z, then X, then Y (degrees).
splash::Quat quatEuler(splash::Vec3 deg);

}  // namespace editor
