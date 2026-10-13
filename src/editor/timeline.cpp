#include "editor/timeline.hh"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

#include "editor/document.hh"

namespace editor {

using splash::CutsceneTrack;
using splash::Interp;
using splash::Keyframe;
using splash::TrackType;

const char* trackTypeName(TrackType t) {
    switch (t) {
        case TrackType::CameraPosition: return "Camera Position";
        case TrackType::CameraRotation: return "Camera Rotation";
        case TrackType::ObjectPosition: return "Object Position";
        case TrackType::ObjectRotation: return "Object Rotation";
        case TrackType::ObjectActive: return "Object Active";
        case TrackType::UICanvasVisible: return "Canvas Visible";
        case TrackType::UIElementVisible: return "Element Visible";
        case TrackType::UIProgress: return "UI Progress";
        case TrackType::UIPosition: return "UI Position";
        case TrackType::UIColor: return "UI Color";
        case TrackType::CameraH: return "Camera H";
        case TrackType::RumbleSmall: return "Rumble Small";
        case TrackType::RumbleLarge: return "Rumble Large";
        case TrackType::ObjectUVOffset: return "UV Offset";
        case TrackType::LightPosition: return "Light Position";
        case TrackType::LightColor: return "Light Color";
        case TrackType::LightIntensity: return "Light Intensity";
        case TrackType::LightRadius: return "Light Radius";
        case TrackType::LightEnabled: return "Light Enabled";
    }
    return "Track";
}

// The property alone, for a label that already names the target.
static const char* propertyName(TrackType t) {
    switch (t) {
        case TrackType::CameraPosition:
        case TrackType::ObjectPosition:
        case TrackType::UIPosition:
        case TrackType::LightPosition: return "Position";
        case TrackType::CameraRotation:
        case TrackType::ObjectRotation: return "Rotation";
        case TrackType::ObjectActive: return "Active";
        case TrackType::UICanvasVisible:
        case TrackType::UIElementVisible: return "Visible";
        case TrackType::UIProgress: return "Progress";
        case TrackType::UIColor:
        case TrackType::LightColor: return "Color";
        case TrackType::CameraH: return "H (FOV)";
        case TrackType::RumbleSmall: return "Small motor";
        case TrackType::RumbleLarge: return "Large motor";
        case TrackType::ObjectUVOffset: return "UV Offset";
        case TrackType::LightIntensity: return "Intensity";
        case TrackType::LightRadius: return "Radius";
        case TrackType::LightEnabled: return "Enabled";
    }
    return "Track";
}

int valueCount(TrackType t) {
    switch (t) {
        case TrackType::CameraPosition:
        case TrackType::CameraRotation:
        case TrackType::ObjectPosition:
        case TrackType::ObjectRotation:
        case TrackType::UIColor:
        case TrackType::LightPosition:
        case TrackType::LightColor: return 3;
        case TrackType::UIPosition:
        case TrackType::ObjectUVOffset: return 2;
        default: return 1;
    }
}

bool isDiscrete(TrackType t) {
    return t == TrackType::ObjectActive || t == TrackType::UICanvasVisible || t == TrackType::UIElementVisible ||
           t == TrackType::RumbleSmall || t == TrackType::LightEnabled;
}

bool needsTarget(TrackType t) {
    return !(t == TrackType::CameraPosition || t == TrackType::CameraRotation || t == TrackType::CameraH ||
             t == TrackType::RumbleSmall || t == TrackType::RumbleLarge);
}

bool allowedIn(SeqKind kind, TrackType t) {
    if (kind == SeqKind::Cutscene) return true;
    return !(t == TrackType::CameraPosition || t == TrackType::CameraRotation || t == TrackType::CameraH);
}

std::string trackLabel(const CutsceneTrack& t) {
    std::string who = t.target;
    if (who.empty()) who = (t.type == TrackType::RumbleSmall || t.type == TrackType::RumbleLarge) ? "Rumble" : "Camera";
    return who + ": " + propertyName(t.type);
}

// Reading.

bool valid(const Document& doc, SeqRef ref) {
    if (ref.index < 0) return false;
    const splash::Scene& s = doc.scene();
    return ref.kind == SeqKind::Cutscene ? ref.index < int(s.cutscenes.size()) : ref.index < int(s.animations.size());
}

const std::string* seqName(const Document& doc, SeqRef ref) {
    if (!valid(doc, ref)) return nullptr;
    return ref.kind == SeqKind::Cutscene ? &doc.scene().cutscenes[size_t(ref.index)].name
                                         : &doc.scene().animations[size_t(ref.index)].name;
}

int seqLength(const Document& doc, SeqRef ref) {
    if (!valid(doc, ref)) return 0;
    return ref.kind == SeqKind::Cutscene ? doc.scene().cutscenes[size_t(ref.index)].durationFrames
                                         : doc.scene().animations[size_t(ref.index)].durationFrames;
}

const std::vector<CutsceneTrack>* seqTracks(const Document& doc, SeqRef ref) {
    if (!valid(doc, ref)) return nullptr;
    return ref.kind == SeqKind::Cutscene ? &doc.scene().cutscenes[size_t(ref.index)].tracks
                                         : &doc.scene().animations[size_t(ref.index)].tracks;
}

const Keyframe* key(const Document& doc, SeqRef ref, KeyRef k) {
    const std::vector<CutsceneTrack>* tracks = seqTracks(doc, ref);
    if (!tracks || k.track < 0 || k.track >= int(tracks->size())) return nullptr;
    const std::vector<Keyframe>& keys = (*tracks)[size_t(k.track)].keyframes;
    if (k.key < 0 || k.key >= int(keys.size())) return nullptr;
    return &keys[size_t(k.key)];
}

std::vector<SeqRef> allSequences(const Document& doc) {
    std::vector<SeqRef> out;
    for (int i = 0; i < int(doc.scene().cutscenes.size()); ++i) out.push_back({SeqKind::Cutscene, i});
    for (int i = 0; i < int(doc.scene().animations.size()); ++i) out.push_back({SeqKind::Animation, i});
    return out;
}

// Edits. Every one snapshots the scene's cutscenes and animations before and
// after; they are small (the runtime caps them at 16 x 8 tracks x 64 keys).

namespace {

class TimelineCommand : public Command {
public:
    TimelineCommand(std::vector<splash::Cutscene> beforeC, std::vector<splash::Animation> beforeA,
                    std::vector<splash::Cutscene> afterC, std::vector<splash::Animation> afterA, std::string key)
        : m_beforeC(std::move(beforeC)), m_afterC(std::move(afterC)), m_beforeA(std::move(beforeA)),
          m_afterA(std::move(afterA)), m_key(std::move(key)) {}
    void apply(Document& doc) override {
        doc.cutscenesMut() = m_afterC;
        doc.animationsMut() = m_afterA;
    }
    void revert(Document& doc) override {
        doc.cutscenesMut() = m_beforeC;
        doc.animationsMut() = m_beforeA;
    }
    bool merge(const Command& next) override {
        auto* n = dynamic_cast<const TimelineCommand*>(&next);
        if (!n || m_key.empty() || n->m_key != m_key) return false;
        m_afterC = n->m_afterC;
        m_afterA = n->m_afterA;
        return true;
    }

private:
    std::vector<splash::Cutscene> m_beforeC, m_afterC;
    std::vector<splash::Animation> m_beforeA, m_afterA;
    std::string m_key;
};

// The fields a timeline edits, the same for both kinds.
struct SeqMut {
    int* length;
    std::vector<CutsceneTrack>* tracks;
};

// Runs `fn` on a copy of the sequence; when it returns true the copy goes in as one step.
template <class F>
bool editSeq(Document& doc, SeqRef ref, F&& fn, const std::string& mergeKey = {}) {
    if (!valid(doc, ref)) return false;
    std::vector<splash::Cutscene> cs = doc.scene().cutscenes;
    std::vector<splash::Animation> as = doc.scene().animations;
    SeqMut m = ref.kind == SeqKind::Cutscene
                   ? SeqMut{&cs[size_t(ref.index)].durationFrames, &cs[size_t(ref.index)].tracks}
                   : SeqMut{&as[size_t(ref.index)].durationFrames, &as[size_t(ref.index)].tracks};
    if (!fn(m)) return false;
    doc.execute(std::make_unique<TimelineCommand>(doc.scene().cutscenes, doc.scene().animations, std::move(cs), std::move(as),
                                                  mergeKey),
                !mergeKey.empty());
    return true;
}

bool trackOk(const SeqMut& m, int track) { return track >= 0 && track < int(m.tracks->size()); }

bool keyOk(const SeqMut& m, KeyRef k) {
    return trackOk(m, k.track) && k.key >= 0 && k.key < int((*m.tracks)[size_t(k.track)].keyframes.size());
}

}  // namespace

std::optional<SeqRef> addSequence(Document& doc, SeqKind kind, const std::string& name) {
    auto taken = [&](const std::string& n) {
        if (kind == SeqKind::Cutscene)
            return std::any_of(doc.scene().cutscenes.begin(), doc.scene().cutscenes.end(), [&](auto& c) { return c.name == n; });
        return std::any_of(doc.scene().animations.begin(), doc.scene().animations.end(), [&](auto& a) { return a.name == n; });
    };
    const std::string base = name.empty() ? (kind == SeqKind::Cutscene ? "cutscene" : "animation") : name;
    std::string n = base;
    for (int i = 2; taken(n); ++i) n = base + " (" + std::to_string(i) + ")";
    std::vector<splash::Cutscene> cs = doc.scene().cutscenes;
    std::vector<splash::Animation> as = doc.scene().animations;
    SeqRef ref{kind, -1};
    if (kind == SeqKind::Cutscene) {
        cs.push_back({});
        cs.back().name = n;
        ref.index = int(cs.size()) - 1;
    } else {
        as.push_back({});
        as.back().name = n;
        ref.index = int(as.size()) - 1;
    }
    doc.execute(std::make_unique<TimelineCommand>(doc.scene().cutscenes, doc.scene().animations, std::move(cs), std::move(as),
                                                  std::string()));
    return ref;
}

int addTrack(Document& doc, SeqRef ref, TrackType type, const std::string& target) {
    int added = -1;
    editSeq(doc, ref, [&](SeqMut& m) {
        if (!allowedIn(ref.kind, type)) return false;
        if (needsTarget(type) && target.empty()) return false;
        if (int(m.tracks->size()) >= kMaxTimelineTracks) return false;
        const std::string t = needsTarget(type) ? target : std::string();
        for (const CutsceneTrack& tr : *m.tracks)
            if (tr.type == type && tr.target == t) return false;
        CutsceneTrack tr;
        tr.type = type;
        tr.target = t;
        m.tracks->push_back(std::move(tr));
        added = int(m.tracks->size()) - 1;
        return true;
    });
    return added;
}

bool removeTrack(Document& doc, SeqRef ref, int track) {
    return editSeq(doc, ref, [&](SeqMut& m) {
        if (!trackOk(m, track)) return false;
        m.tracks->erase(m.tracks->begin() + track);
        return true;
    });
}

std::optional<KeyRef> addKey(Document& doc, SeqRef ref, int track, int frame, std::optional<KeyValue> value) {
    frame = std::clamp(frame, 0, kMaxTimelineFrame);
    // Sampled before the edit: the value the track has there now.
    const std::vector<CutsceneTrack>* now = seqTracks(doc, ref);
    if (!now || track < 0 || track >= int(now->size())) return std::nullopt;
    const KeyValue v = value ? *value : sampleAt(doc, ref, track, float(frame));
    std::optional<KeyRef> added;
    editSeq(doc, ref, [&](SeqMut& m) {
        std::vector<Keyframe>& keys = (*m.tracks)[size_t(track)].keyframes;
        if (int(keys.size()) >= kMaxTimelineKeys) return false;
        if (std::any_of(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.frame == frame; })) return false;
        Keyframe k;
        k.frame = frame;
        k.value = v;
        // On/off tracks have no curve; the runtime ignores it.
        k.interp = Interp::Linear;
        auto at = std::upper_bound(keys.begin(), keys.end(), frame, [](int f, const Keyframe& e) { return f < e.frame; });
        at = keys.insert(at, std::move(k));
        added = KeyRef{track, int(at - keys.begin())};
        return true;
    });
    return added;
}

std::optional<std::vector<KeyRef>> moveKeys(Document& doc, SeqRef ref, const std::vector<KeyRef>& keys, int delta,
                                            const std::string& mergeKey) {
    std::optional<std::vector<KeyRef>> out;
    editSeq(
        doc, ref,
        [&](SeqMut& m) {
            if (keys.empty()) return false;
            std::set<KeyRef> moving(keys.begin(), keys.end());
            for (const KeyRef& k : moving)
                if (!keyOk(m, k)) return false;
            std::vector<KeyRef> result(keys.size());
            std::set<int> tracksTouched;
            for (const KeyRef& k : moving) tracksTouched.insert(k.track);
            for (int ti : tracksTouched) {
                std::vector<Keyframe>& ks = (*m.tracks)[size_t(ti)].keyframes;
                // Each key with its old index, so the new order can be traced back.
                std::vector<std::pair<Keyframe, int>> tagged;
                std::multiset<int> frames;
                for (int i = 0; i < int(ks.size()); ++i) {
                    Keyframe k = ks[size_t(i)];
                    if (moving.count({ti, i})) {
                        k.frame += delta;
                        if (k.frame < 0 || k.frame > kMaxTimelineFrame) return false;
                    }
                    frames.insert(k.frame);
                    tagged.push_back({std::move(k), i});
                }
                for (const auto& [k, i] : tagged)
                    if (moving.count({ti, i}) && frames.count(k.frame) > 1) return false;
                std::stable_sort(tagged.begin(), tagged.end(),
                                 [](const auto& a, const auto& b) { return a.first.frame < b.first.frame; });
                ks.clear();
                for (int n = 0; n < int(tagged.size()); ++n) {
                    ks.push_back(tagged[size_t(n)].first);
                    for (size_t j = 0; j < keys.size(); ++j)
                        if (keys[j].track == ti && keys[j].key == tagged[size_t(n)].second) result[j] = {ti, n};
                }
            }
            out = std::move(result);
            return true;
        },
        mergeKey);
    return out;
}

int deleteKeys(Document& doc, SeqRef ref, const std::vector<KeyRef>& keys) {
    int removed = 0;
    editSeq(doc, ref, [&](SeqMut& m) {
        std::set<KeyRef> gone;
        for (const KeyRef& k : keys)
            if (keyOk(m, k)) gone.insert(k);
        if (gone.empty()) return false;
        // Back to front, so earlier indices stay valid.
        for (auto it = gone.rbegin(); it != gone.rend(); ++it) {
            std::vector<Keyframe>& ks = (*m.tracks)[size_t(it->track)].keyframes;
            ks.erase(ks.begin() + it->key);
        }
        removed = int(gone.size());
        return true;
    });
    return removed;
}

bool setKeyValue(Document& doc, SeqRef ref, KeyRef k, KeyValue value, const std::string& mergeKey) {
    return editSeq(
        doc, ref,
        [&](SeqMut& m) {
            if (!keyOk(m, k)) return false;
            Keyframe& kf = (*m.tracks)[size_t(k.track)].keyframes[size_t(k.key)];
            if (kf.value == value) return false;
            kf.value = value;
            return true;
        },
        mergeKey);
}

bool setKeyInterp(Document& doc, SeqRef ref, KeyRef k, Interp interp) {
    return editSeq(doc, ref, [&](SeqMut& m) {
        if (!keyOk(m, k)) return false;
        Keyframe& kf = (*m.tracks)[size_t(k.track)].keyframes[size_t(k.key)];
        if (kf.interp == interp) return false;
        kf.interp = interp;
        return true;
    });
}

bool setLength(Document& doc, SeqRef ref, int frames, const std::string& mergeKey) {
    frames = std::clamp(frames, 1, kMaxTimelineFrame);
    return editSeq(
        doc, ref,
        [&](SeqMut& m) {
            if (*m.length == frames) return false;
            *m.length = frames;
            return true;
        },
        mergeKey);
}

// Sampling: psxsplash's interpolation.cpp in floating point.

float applyCurve(float t, Interp mode) {
    switch (mode) {
        case Interp::Linear: return t;
        case Interp::Step: return 0;
        case Interp::EaseIn: return t * t;
        case Interp::EaseOut: return t * (2 - t);
        case Interp::EaseInOut: return t * t * (3 - 2 * t);
    }
    return t;
}

static KeyValue mix(const KeyValue& a, const KeyValue& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

KeyValue sample(const CutsceneTrack& track, float frame, const KeyValue& initial) {
    // The pack sorts keys by frame (stably); so does this.
    std::vector<const Keyframe*> ks;
    for (const Keyframe& k : track.keyframes) ks.push_back(&k);
    std::stable_sort(ks.begin(), ks.end(), [](const Keyframe* a, const Keyframe* b) { return a->frame < b->frame; });
    if (ks.empty()) return initial;  // the runtime leaves the target alone

    if (isDiscrete(track.type)) {
        // Whole frames only: the runtime compares the integer frame.
        const int f = int(std::floor(frame));
        KeyValue v = f < ks[0]->frame ? initial : ks[0]->value;
        for (const Keyframe* k : ks) {
            if (k->frame > f) break;
            v = k->value;
        }
        return v;
    }

    const Keyframe& first = *ks.front();
    if (frame < float(first.frame)) {
        // Before the first key: from where the target was into it.
        if (first.frame <= 0) return first.value;
        float t = std::clamp(frame / float(first.frame), 0.0f, 1.0f);
        return mix(initial, first.value, applyCurve(t, first.interp));
    }
    if (ks.size() == 1 || frame >= float(ks.back()->frame)) return ks.size() == 1 ? first.value : ks.back()->value;
    size_t b = 1;
    while (b < ks.size() && float(ks[b]->frame) <= frame) ++b;
    const Keyframe& ka = *ks[b - 1];
    const Keyframe& kb = *ks[b];
    const int span = kb.frame - ka.frame;
    if (span <= 0) return ka.value;
    const float t = std::clamp((frame - float(ka.frame)) / float(span), 0.0f, 1.0f);
    // The curve of a segment is the later key's.
    return mix(ka.value, kb.value, applyCurve(t, kb.interp));
}

KeyValue initialValue(const Document& doc, const CutsceneTrack& track) {
    switch (track.type) {
        case TrackType::ObjectPosition:
            for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
                if (fo.object->name == track.target) return {fo.worldPosition.x, fo.worldPosition.y, fo.worldPosition.z};
            break;
        case TrackType::ObjectActive:
            for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
                if (fo.object->name == track.target) return {fo.object->startActive ? 1.0f : 0.0f, 0, 0};
            break;
        case TrackType::UICanvasVisible:
            for (const splash::UICanvas& c : doc.scene().canvases)
                if (c.name == track.target) return {c.visible ? 1.0f : 0.0f, 0, 0};
            break;
        default: break;
    }
    return {0, 0, 0};
}

KeyValue sampleAt(const Document& doc, SeqRef ref, int track, float frame) {
    const std::vector<CutsceneTrack>* tracks = seqTracks(doc, ref);
    if (!tracks || track < 0 || track >= int(tracks->size())) return {0, 0, 0};
    const CutsceneTrack& t = (*tracks)[size_t(track)];
    return sample(t, frame, initialValue(doc, t));
}

}  // namespace editor
