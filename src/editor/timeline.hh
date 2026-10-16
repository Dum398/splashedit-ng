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
// writeSequences' event limits: a cutscene's audio events, a sequence's skin
// clip events. Each packs the frame as u16 (unchecked); the editor holds them
// to kMaxTimelineFrame like keys. Volume and pan pack as u8: the runtime reads
// volume 0..128 and pan 0..127.
constexpr int kMaxAudioEvents = 64;
constexpr int kMaxSkinEvents = 16;
constexpr int kMaxEventVolume = 128;
constexpr int kMaxEventPan = 127;

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

// What the timeline has selected: keys, and events by index (audio events
// only in cutscenes). Event indices stay put when events move.
struct Selection {
    std::vector<KeyRef> keys;
    std::vector<int> audio;
    std::vector<int> skin;
    bool empty() const { return keys.empty() && audio.empty() && skin.empty(); }
    size_t size() const { return keys.size() + audio.size() + skin.size(); }
    bool operator==(const Selection&) const = default;
};

// Copied keys and events, frames relative to the earliest of them. Keys
// remember their track by type and target, so they paste into the track that
// drives the same thing (in this sequence or another).
struct TimelineClipboard {
    struct Key {
        splash::TrackType type;
        std::string target;
        splash::Keyframe key;
    };
    std::vector<Key> keys;
    std::vector<splash::CutsceneAudioEvent> audio;
    std::vector<splash::SkinAnimEvent> skin;
    bool empty() const { return keys.empty() && audio.empty() && skin.empty(); }
};

// A skinned object the export can address, with its clips' names in clip order.
struct SkinTarget {
    std::string object;
    std::vector<std::string> clips;
};

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
// A cutscene's audio events (null for an animation: it has none) and a
// sequence's skin clip events, in file order.
const std::vector<splash::CutsceneAudioEvent>* seqAudio(const Document& doc, SeqRef ref);
const std::vector<splash::SkinAnimEvent>* seqSkin(const Document& doc, SeqRef ref);
// The export's audio clip table: the clip names of the active objects with an
// audio component, in flatten order (unnamed ones left out).
std::vector<std::string> audioClips(const Document& doc);
// Active objects with a mesh and a skin component, clip names read from their
// .anim files (one that does not load is left out).
std::vector<SkinTarget> skinTargets(const Document& doc);
// Why the export refuses skin event `e` against `targets` (skinTargets()), or
// empty when it plays: no skinned object of that name (deleted, renamed,
// inactive or its skin removed), two of them, or no clip of that name on it.
std::string skinEventProblem(const std::vector<SkinTarget>& targets, const splash::SkinAnimEvent& e);

// Edits. Each is one undo step; a refused edit changes nothing and adds no
// step. Keys stay sorted by frame within a track, and no two share a frame.
//
// Appends an empty sequence named `name` (made unique with " (2)"...).
std::optional<SeqRef> addSequence(Document& doc, SeqKind kind, const std::string& name);
// Why `name` cannot name the sequence, or empty when it can: empty, longer
// than kMaxSeqName, or taken by another sequence of the same kind.
std::string seqNameProblem(const Document& doc, SeqRef ref, const std::string& name);
// Lua plays a sequence by name: Cutscene.Play(name) for a cutscene, and
// Animation.Play(name), Animation.Stop(name) and Animation.IsPlaying(name)
// for an animation (psxsplash luaapi.cpp; Cutscene.Stop and IsPlaying take
// no name). `lua` with each such call whose first argument is a quoted
// string literal spelling exactly `from` (no escapes) changed to spell `to`;
// so is such a literal assigned to a plain variable (local x = "intro",
// x = "intro", local a, x = 1, "intro") that the same file passes as the
// first argument of such a call, the same variable by Lua scope
// (lua::definitions) or both global. `count` gets how many, both kinds.
// Calls through another name (local P = Cutscene.Play), names computed,
// long-bracket, held in tables or assigned in table constructors are left alone.
std::string renameSeqInLua(const std::string& lua, SeqKind kind, const std::string& from, const std::string& to,
                           int* count = nullptr);
// The same for a block script (.blocks): every play_cutscene block whose
// field is `from` gets `to` (a cutscene rename; animations have no block),
// written back with saveBlocks. Text that is not a block script, or names
// nothing, comes back as it was with `count` 0.
std::string renameSeqInBlocks(const std::string& text, SeqKind kind, const std::string& from, const std::string& to,
                              int* count = nullptr);
// The scripts the scene exports, project relative: object scripts, trigger
// scripts and the scene script, each once.
std::vector<std::string> sceneScripts(const Document& doc);
// What a rename did to the scene's scripts on disk.
struct ScriptRenames {
    int refs = 0;                      // calls rewritten
    std::vector<std::string> files;    // project paths rewritten
    std::vector<std::string> failed;   // project paths that could not be read or written
    // Each string literal still spelling the old name in the scripts scanned,
    // "path:line" (line from 1): what the rename could not tie to a call.
    std::vector<std::string> leftovers;
};
// Refused (no step) when seqNameProblem() says so or the name is unchanged.
// Also rewrites the sequence's name in sceneScripts() (renameSeqInLua; renameSeqInBlocks for a .blocks) on
// disk, in the same undo step: undo and redo write each file back only when
// it still holds what the step left there, so a file edited since is left
// alone. `scripts` gets what was rewritten.
bool renameSequence(Document& doc, SeqRef ref, const std::string& name, ScriptRenames* scripts = nullptr);
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
// The same for a whole selection, keys and events by one delta, one step.
// Refused when a key would be refused by moveKeys() or an event would leave
// 0..kMaxTimelineFrame. Returns the selection's new addresses.
std::optional<Selection> moveSelection(Document& doc, SeqRef ref, const Selection& sel, int delta,
                                       const std::string& mergeKey = {});
// Removes every key and event in `sel`, one step. Returns how many it removed.
int deleteSelection(Document& doc, SeqRef ref, const Selection& sel);
// The selection's keys and events, frames relative to the earliest.
TimelineClipboard copySelection(const Document& doc, SeqRef ref, const Selection& sel);
// Pastes `clip` with its earliest item at `frame`, one step. A key goes to the
// track with its type and target (replacing a key already on that frame); when
// the sequence has none, the same step adds one (at the end, as addTrack()).
// A key whose track type the sequence cannot hold (camera in an animation) is
// skipped, and so are audio events in an animation. Refused (nullopt, no
// step) when nothing would land, an item would pass kMaxTimelineFrame, or the
// tracks, a track's keys or an event list would pass the runtime's limits.
// `skipped` gets how many items had nowhere to go.
std::optional<Selection> pasteClipboard(Document& doc, SeqRef ref, const TimelineClipboard& clip, int frame,
                                        int* skipped = nullptr);
// An audio event of `clip` at `frame` (clamped to 0..kMaxTimelineFrame),
// volume 100, centre pan. Its index, or -1: an animation, an empty clip name,
// or kMaxAudioEvents already.
int addAudioEvent(Document& doc, SeqRef ref, int frame, const std::string& clip);
// Replaces event `index`: frame clamped as above, volume to 0..kMaxEventVolume,
// pan to 0..kMaxEventPan. Refused for an empty clip or no change.
bool setAudioEvent(Document& doc, SeqRef ref, int index, splash::CutsceneAudioEvent ev, const std::string& mergeKey = {});
// A skin clip event: plays `clip` on skinned object `object` from `frame`.
// Its index, or -1: an empty object or clip, or kMaxSkinEvents already.
int addSkinEvent(Document& doc, SeqRef ref, int frame, const std::string& object, const std::string& clip, bool loop = false);
// Replaces skin event `index`, frame clamped. Refused for an empty object or clip or no change.
bool setSkinEvent(Document& doc, SeqRef ref, int index, splash::SkinAnimEvent ev, const std::string& mergeKey = {});
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
// an object's world position or its start-active flag, a canvas's or UI
// element's visible flag, a UI element's on-screen position
// (splash::uiPositionTrackStart), its progress value (0..100) and its colour
// (0..1, the fill of a progress bar), as psxsplash's CutscenePlayer reads them
// from the UI system when the track starts; zero otherwise (object rotation
// starts from 0,0,0 in the runtime too). So the first key added to an empty
// track takes the target's current value.
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
