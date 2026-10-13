// The timeline's model: edits to a cutscene's or an animation's tracks and
// keys, each one undo step through the Document, and sampling a track at a
// frame the way psxsplash's CutscenePlayer and AnimationPlayer do. No ImGui.
#pragma once

#include <array>
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
// The new track's index, or -1: the type is not allowed in this kind of
// sequence, the target is missing where needed, the same type and target is
// already there, or the sequence has kMaxTimelineTracks.
int addTrack(Document& doc, SeqRef ref, splash::TrackType type, const std::string& target);
bool removeTrack(Document& doc, SeqRef ref, int track);
// Adds a key at `frame` (clamped to 0..kMaxTimelineFrame). Without `value`
// it takes what the track samples there, so adding a key leaves the motion as
// it was. Returns the key, or nullopt when the frame already has one or the
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
// to the runtime's fixed point.
KeyValue sample(const splash::CutsceneTrack& track, float frame, const KeyValue& initial = {0, 0, 0});
// The curve psxsplash applies to a 0..1 segment position.
float applyCurve(float t, splash::Interp mode);
// The target's state when playback starts, as far as the editor knows it:
// an object's world position or its start-active flag; zero otherwise
// (object rotation starts from 0,0,0 in the runtime too).
KeyValue initialValue(const Document& doc, const splash::CutsceneTrack& track);
// sample() with initialValue().
KeyValue sampleAt(const Document& doc, SeqRef ref, int track, float frame);

}  // namespace editor
