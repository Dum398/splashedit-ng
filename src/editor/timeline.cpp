#include "editor/timeline.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <span>

#include "editor/document.hh"
#include "editor/gizmo.hh"
#include "splashpack.hh"

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

const std::vector<splash::CutsceneAudioEvent>* seqAudio(const Document& doc, SeqRef ref) {
    if (!valid(doc, ref) || ref.kind != SeqKind::Cutscene) return nullptr;
    return &doc.scene().cutscenes[size_t(ref.index)].audioEvents;
}

const std::vector<splash::SkinAnimEvent>* seqSkin(const Document& doc, SeqRef ref) {
    if (!valid(doc, ref)) return nullptr;
    return ref.kind == SeqKind::Cutscene ? &doc.scene().cutscenes[size_t(ref.index)].skinEvents
                                         : &doc.scene().animations[size_t(ref.index)].skinEvents;
}

std::vector<std::string> audioClips(const Document& doc) {
    // splashpack.cpp: audio sources are the active (in hierarchy) objects with an audio component.
    std::vector<std::string> out;
    for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
        if (fo.activeInHierarchy && fo.object->active && fo.object->audio && !fo.object->audio->clipName.empty() &&
            std::find(out.begin(), out.end(), fo.object->audio->clipName) == out.end())
            out.push_back(fo.object->audio->clipName);
    return out;
}

std::vector<SkinTarget> skinTargets(const Document& doc) {
    std::vector<SkinTarget> out;
    for (const splash::FlatObject& fo : splash::flatten(doc.scene())) {
        const splash::Object& o = *fo.object;
        if (!fo.activeInHierarchy || !o.active || !o.mesh || !o.skin) continue;
        SkinTarget t{o.name, {}};
        for (const std::string& path : o.skin->clips) {
            try {
                const std::string n = splash::loadAnim(doc.resolve(path)).name;
                if (!n.empty()) t.clips.push_back(n);
            } catch (const std::exception&) {
            }
        }
        out.push_back(std::move(t));
    }
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

// The fields a timeline edits; `audio` is null for an animation.
struct SeqMut {
    int* length;
    std::vector<CutsceneTrack>* tracks;
    std::vector<splash::CutsceneAudioEvent>* audio;
    std::vector<splash::SkinAnimEvent>* skin;
};

// Runs `fn` on a copy of the sequence; when it returns true the copy goes in as one step.
template <class F>
bool editSeq(Document& doc, SeqRef ref, F&& fn, const std::string& mergeKey = {}) {
    if (!valid(doc, ref)) return false;
    std::vector<splash::Cutscene> cs = doc.scene().cutscenes;
    std::vector<splash::Animation> as = doc.scene().animations;
    SeqMut m = ref.kind == SeqKind::Cutscene
                   ? SeqMut{&cs[size_t(ref.index)].durationFrames, &cs[size_t(ref.index)].tracks, &cs[size_t(ref.index)].audioEvents,
                            &cs[size_t(ref.index)].skinEvents}
                   : SeqMut{&as[size_t(ref.index)].durationFrames, &as[size_t(ref.index)].tracks, nullptr,
                            &as[size_t(ref.index)].skinEvents};
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

bool frameOk(int f) { return f >= 0 && f <= kMaxTimelineFrame; }

// Moves `keys` by `delta` in place; false (the copy half done, so dropped) when
// one leaves the frame range or lands on a key that stays. `result` gets the
// keys' new addresses in the order given.
bool moveKeysIn(SeqMut& m, const std::vector<KeyRef>& keys, int delta, std::vector<KeyRef>& result) {
    std::set<KeyRef> moving(keys.begin(), keys.end());
    for (const KeyRef& k : moving)
        if (!keyOk(m, k)) return false;
    result.assign(keys.size(), KeyRef{});
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
                if (!frameOk(k.frame)) return false;
            }
            frames.insert(k.frame);
            tagged.push_back({std::move(k), i});
        }
        for (const auto& [k, i] : tagged)
            if (moving.count({ti, i}) && frames.count(k.frame) > 1) return false;
        std::stable_sort(tagged.begin(), tagged.end(), [](const auto& a, const auto& b) { return a.first.frame < b.first.frame; });
        ks.clear();
        for (int n = 0; n < int(tagged.size()); ++n) {
            ks.push_back(tagged[size_t(n)].first);
            for (size_t j = 0; j < keys.size(); ++j)
                if (keys[j].track == ti && keys[j].key == tagged[size_t(n)].second) result[j] = {ti, n};
        }
    }
    return true;
}

// The selection's events that exist, each once.
std::set<int> eventsOk(const std::vector<int>& idx, size_t n) {
    std::set<int> out;
    for (int i : idx)
        if (i >= 0 && i < int(n)) out.insert(i);
    return out;
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

// String.Substring's count, as splashpack.cpp's truncateUtf16: UTF-16 code
// units, so a character outside the BMP counts two.
static int utf16Units(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = uint8_t(s[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        n += len == 4 ? 2 : 1;
        i += len;
    }
    return n;
}

std::string seqNameProblem(const Document& doc, SeqRef ref, const std::string& name) {
    if (!valid(doc, ref)) return "No such sequence";
    const bool cs = ref.kind == SeqKind::Cutscene;
    if (name.empty()) return "A name is needed: Lua plays it by name";
    if (utf16Units(name) > kMaxSeqName) return "At most " + std::to_string(kMaxSeqName) + " characters (the runtime's limit)";
    const int n = cs ? int(doc.scene().cutscenes.size()) : int(doc.scene().animations.size());
    for (int i = 0; i < n; ++i)
        if (i != ref.index && *seqName(doc, {ref.kind, i}) == name)
            return std::string("Another ") + (cs ? "cutscene" : "animation") + " is named '" + name + "'";
    return {};
}

namespace {

bool identChar(char c) { return std::isalnum(uint8_t(c)) || c == '_'; }
bool spaceChar(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// Past a long bracket's opening `[==[` at `i`: the level, or -1 when it is not one.
int longOpen(const std::string& s, size_t i, size_t* after) {
    if (i >= s.size() || s[i] != '[') return -1;
    size_t j = i + 1;
    while (j < s.size() && s[j] == '=') ++j;
    if (j >= s.size() || s[j] != '[') return -1;
    *after = j + 1;
    return int(j - i - 1);
}

size_t longClose(const std::string& s, size_t from, int level) {
    const std::string close = "]" + std::string(size_t(level), '=') + "]";
    const size_t at = s.find(close, from);
    return at == std::string::npos ? s.size() : at + close.size();
}

// A Lua quoted string literal holding `name`.
std::string quoteLua(const std::string& name, char q) {
    std::string out;
    for (char c : name) {
        if (c == '\\' || c == q) out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        if (c == '\r') {
            out += "\\r";
            continue;
        }
        out += c;
    }
    return out;
}

std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Written beside and renamed over, as lua::Script::save, so a failed write leaves the old file.
bool writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) std::filesystem::remove(tmp, ec);
    return !ec;
}

// The rename and the scripts it rewrote, one step. A file goes back (or
// forward again) only while it holds what this step left in it.
class RenameCommand : public Command {
public:
    struct File {
        std::string path;  // project relative
        std::string before, after;
    };
    RenameCommand(TimelineCommand seq, std::vector<File> files) : m_seq(std::move(seq)), m_files(std::move(files)) {}
    void apply(Document& doc) override {
        m_seq.apply(doc);
        write(doc, false);
    }
    void revert(Document& doc) override {
        m_seq.revert(doc);
        write(doc, true);
    }
    std::vector<std::string> written, failed;  // by the last apply or revert

private:
    void write(Document& doc, bool back) {
        written.clear();
        failed.clear();
        for (const File& f : m_files) {
            const std::filesystem::path p = doc.resolve(f.path);
            const std::optional<std::string> now = readFile(p);
            if (!now || *now != (back ? f.after : f.before)) {
                failed.push_back(f.path);
                continue;
            }
            (writeFile(p, back ? f.before : f.after) ? written : failed).push_back(f.path);
        }
        if (!written.empty()) doc.refreshAssets(written);  // open script tabs reload from this
    }
    TimelineCommand m_seq;
    std::vector<File> m_files;
};

void collectScripts(const std::vector<splash::Object>& objs, std::vector<std::string>& out) {
    auto add = [&](const std::string& p) {
        if (!p.empty() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    for (const splash::Object& o : objs) {
        if (o.script) add(o.script->lua);
        if (o.trigger) add(o.trigger->lua);
        collectScripts(o.children, out);
    }
}

}  // namespace

std::string renameSeqInLua(const std::string& s, SeqKind kind, const std::string& from, const std::string& to, int* count) {
    static const char* const cutsceneFns[] = {"Play"};
    static const char* const animationFns[] = {"Play", "Stop", "IsPlaying"};
    const bool cs = kind == SeqKind::Cutscene;
    const std::string table = cs ? "Cutscene" : "Animation";
    std::string out;
    size_t copied = 0;  // s[copied..] is not in `out` yet
    int n = 0;
    auto skipSpace = [&](size_t i) {
        while (i < s.size() && spaceChar(s[i])) ++i;
        return i;
    };
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        size_t after = 0;
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            const int level = longOpen(s, i + 2, &after);
            if (level >= 0) {
                i = longClose(s, after, level);
            } else {
                const size_t nl = s.find('\n', i);
                i = nl == std::string::npos ? s.size() : nl + 1;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            for (++i; i < s.size() && s[i] != c && s[i] != '\n'; ++i)
                if (s[i] == '\\') ++i;
            ++i;
            continue;
        }
        if (c == '[') {
            const int level = longOpen(s, i, &after);
            i = level >= 0 ? longClose(s, after, level) : i + 1;
            continue;
        }
        if (!identChar(c) || std::isdigit(uint8_t(c))) {
            if (std::isdigit(uint8_t(c)))
                while (i < s.size() && (identChar(s[i]) || s[i] == '.')) ++i;
            else
                ++i;
            continue;
        }
        size_t end = i;
        while (end < s.size() && identChar(s[end])) ++end;
        const std::string word = s.substr(i, end - i);
        // Not a field of something else (x.Cutscene, x:Cutscene).
        size_t back = i;
        while (back > 0 && spaceChar(s[back - 1])) --back;
        const bool member = back > 0 && (s[back - 1] == ':' || (s[back - 1] == '.' && (back < 2 || s[back - 2] != '.')));
        i = end;
        if (word != table || member) continue;
        size_t j = skipSpace(end);
        if (j >= s.size() || s[j] != '.') continue;
        j = skipSpace(j + 1);
        size_t fe = j;
        while (fe < s.size() && identChar(s[fe])) ++fe;
        const std::string fn = s.substr(j, fe - j);
        bool known = false;
        for (const char* f : cs ? std::span<const char* const>(cutsceneFns) : std::span<const char* const>(animationFns))
            known |= fn == f;
        if (!known) continue;
        j = skipSpace(fe);
        if (j < s.size() && s[j] == '(') j = skipSpace(j + 1);  // Cutscene.Play "intro" is a call too
        if (j >= s.size() || (s[j] != '"' && s[j] != '\'')) continue;
        const char q = s[j];
        size_t k = j + 1;
        while (k < s.size() && s[k] != q && s[k] != '\\' && s[k] != '\n') ++k;
        if (k >= s.size() || s[k] != q) continue;  // escapes or unfinished: the scanner above takes it
        if (s.compare(j + 1, k - j - 1, from) == 0 && k - j - 1 == from.size()) {
            out.append(s, copied, j + 1 - copied);
            out += quoteLua(to, q);
            copied = k;
            ++n;
        }
        i = k + 1;
    }
    out.append(s, copied, std::string::npos);
    if (count) *count = n;
    return out;
}

std::vector<std::string> sceneScripts(const Document& doc) {
    std::vector<std::string> out;
    collectScripts(doc.scene().objects, out);
    const std::string& scene = doc.scene().settings.script;
    if (!scene.empty() && std::find(out.begin(), out.end(), scene) == out.end()) out.push_back(scene);
    return out;
}

bool renameSequence(Document& doc, SeqRef ref, const std::string& name, ScriptRenames* scripts) {
    if (!valid(doc, ref) || *seqName(doc, ref) == name || !seqNameProblem(doc, ref, name).empty()) return false;
    const std::string old = *seqName(doc, ref);
    std::vector<splash::Cutscene> cs = doc.scene().cutscenes;
    std::vector<splash::Animation> as = doc.scene().animations;
    (ref.kind == SeqKind::Cutscene ? cs[size_t(ref.index)].name : as[size_t(ref.index)].name) = name;
    ScriptRenames result;
    std::vector<RenameCommand::File> files;
    std::map<std::string, int> refs;
    // An unsaved scene has no project to find its scripts in.
    if (!doc.projectRoot().empty())
        for (const std::string& p : sceneScripts(doc)) {
            const std::filesystem::path abs = doc.resolve(p);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(abs, ec)) continue;  // missing: the inspector warns already
            const std::optional<std::string> text = readFile(abs);
            if (!text) {
                result.failed.push_back(p);
                continue;
            }
            int n = 0;
            std::string after = renameSeqInLua(*text, ref.kind, old, name, &n);
            if (!n) continue;
            refs[p] = n;
            files.push_back({p, *text, std::move(after)});
        }
    auto cmd = std::make_unique<RenameCommand>(
        TimelineCommand(doc.scene().cutscenes, doc.scene().animations, std::move(cs), std::move(as), std::string()),
        std::move(files));
    RenameCommand* raw = cmd.get();
    doc.execute(std::move(cmd));
    result.files = raw->written;
    for (const std::string& f : raw->written) result.refs += refs[f];
    for (const std::string& f : raw->failed) result.failed.push_back(f);
    if (scripts) *scripts = std::move(result);
    return true;
}

bool removeSequence(Document& doc, SeqRef ref) {
    if (!valid(doc, ref)) return false;
    std::vector<splash::Cutscene> cs = doc.scene().cutscenes;
    std::vector<splash::Animation> as = doc.scene().animations;
    if (ref.kind == SeqKind::Cutscene)
        cs.erase(cs.begin() + ref.index);
    else
        as.erase(as.begin() + ref.index);
    doc.execute(std::make_unique<TimelineCommand>(doc.scene().cutscenes, doc.scene().animations, std::move(cs), std::move(as),
                                                  std::string()));
    return true;
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

bool setTrackTarget(Document& doc, SeqRef ref, int track, const std::string& target) {
    return editSeq(doc, ref, [&](SeqMut& m) {
        if (!trackOk(m, track) || target.empty()) return false;
        CutsceneTrack& t = (*m.tracks)[size_t(track)];
        if (!needsTarget(t.type) || t.target == target) return false;
        for (const CutsceneTrack& o : *m.tracks)
            if (o.type == t.type && o.target == target) return false;
        t.target = target;
        return true;
    });
}

std::vector<std::string> targetsFor(const Document& doc, TrackType type) {
    std::vector<std::string> out;
    if (!needsTarget(type)) return out;
    const int ti = int(type);
    if (ti >= 5 && ti <= 9) {
        for (const splash::UICanvas& c : doc.scene().canvases) {
            if (type == TrackType::UICanvasVisible)
                out.push_back(c.name);
            else
                for (const splash::UIElement& e : c.elements) out.push_back(c.name + "/" + e.name);
        }
        return out;
    }
    const bool light = ti >= 14;
    for (const splash::FlatObject& fo : splash::flatten(doc.scene())) {
        if (light ? (fo.object->light && fo.object->light->runtime) : (fo.object->mesh && fo.object->active))
            out.push_back(fo.object->name);
    }
    return out;
}

std::optional<KeyRef> addKey(Document& doc, SeqRef ref, int track, int frame, std::optional<KeyValue> value) {
    frame = std::clamp(frame, 0, kMaxTimelineFrame);
    // Sampled before the edit: the value the track has there now.
    const std::vector<CutsceneTrack>* now = seqTracks(doc, ref);
    if (!now || track < 0 || track >= int(now->size())) return std::nullopt;
    // The authored curve, not the runtime's rounding of it: the new key reads
    // as the values the user typed (the export rounds it the same way anyway).
    const CutsceneTrack& cur = (*now)[size_t(track)];
    const KeyValue v = value ? *value : sample(cur, float(frame), initialValue(doc, cur));
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
            std::vector<KeyRef> result;
            if (!moveKeysIn(m, keys, delta, result)) return false;
            out = std::move(result);
            return true;
        },
        mergeKey);
    return out;
}

std::optional<Selection> moveSelection(Document& doc, SeqRef ref, const Selection& sel, int delta, const std::string& mergeKey) {
    std::optional<Selection> out;
    editSeq(
        doc, ref,
        [&](SeqMut& m) {
            if (sel.empty() || delta == 0) return false;
            Selection result = sel;
            if (!sel.keys.empty() && !moveKeysIn(m, sel.keys, delta, result.keys)) return false;
            if (!sel.audio.empty()) {
                if (!m.audio) return false;
                const std::set<int> ok = eventsOk(sel.audio, m.audio->size());
                if (ok.size() != std::set<int>(sel.audio.begin(), sel.audio.end()).size()) return false;
                for (int i : ok) {
                    int& f = (*m.audio)[size_t(i)].frame;
                    if (!frameOk(f + delta)) return false;
                    f += delta;
                }
            }
            if (!sel.skin.empty()) {
                const std::set<int> ok = eventsOk(sel.skin, m.skin->size());
                if (ok.size() != std::set<int>(sel.skin.begin(), sel.skin.end()).size()) return false;
                for (int i : ok) {
                    int& f = (*m.skin)[size_t(i)].frame;
                    if (!frameOk(f + delta)) return false;
                    f += delta;
                }
            }
            out = std::move(result);
            return true;
        },
        mergeKey);
    return out;
}

int deleteSelection(Document& doc, SeqRef ref, const Selection& sel) {
    int removed = 0;
    editSeq(doc, ref, [&](SeqMut& m) {
        std::set<KeyRef> gone;
        for (const KeyRef& k : sel.keys)
            if (keyOk(m, k)) gone.insert(k);
        const std::set<int> audio = m.audio ? eventsOk(sel.audio, m.audio->size()) : std::set<int>();
        const std::set<int> skin = eventsOk(sel.skin, m.skin->size());
        if (gone.empty() && audio.empty() && skin.empty()) return false;
        // Back to front, so earlier indices stay valid.
        for (auto it = gone.rbegin(); it != gone.rend(); ++it) {
            std::vector<Keyframe>& ks = (*m.tracks)[size_t(it->track)].keyframes;
            ks.erase(ks.begin() + it->key);
        }
        for (auto it = audio.rbegin(); it != audio.rend(); ++it) m.audio->erase(m.audio->begin() + *it);
        for (auto it = skin.rbegin(); it != skin.rend(); ++it) m.skin->erase(m.skin->begin() + *it);
        removed = int(gone.size() + audio.size() + skin.size());
        return true;
    });
    return removed;
}

TimelineClipboard copySelection(const Document& doc, SeqRef ref, const Selection& sel) {
    TimelineClipboard clip;
    const std::vector<CutsceneTrack>* tracks = seqTracks(doc, ref);
    if (!tracks) return clip;
    int first = std::numeric_limits<int>::max();
    for (const KeyRef& k : std::set<KeyRef>(sel.keys.begin(), sel.keys.end()))
        if (const Keyframe* kf = key(doc, ref, k)) {
            const CutsceneTrack& t = (*tracks)[size_t(k.track)];
            clip.keys.push_back({t.type, t.target, *kf});
            first = std::min(first, kf->frame);
        }
    if (const auto* audio = seqAudio(doc, ref))
        for (int i : eventsOk(sel.audio, audio->size())) {
            clip.audio.push_back((*audio)[size_t(i)]);
            first = std::min(first, clip.audio.back().frame);
        }
    const auto* skin = seqSkin(doc, ref);
    for (int i : eventsOk(sel.skin, skin->size())) {
        clip.skin.push_back((*skin)[size_t(i)]);
        first = std::min(first, clip.skin.back().frame);
    }
    for (auto& k : clip.keys) k.key.frame -= first;
    for (auto& a : clip.audio) a.frame -= first;
    for (auto& s : clip.skin) s.frame -= first;
    return clip;
}

std::optional<Selection> pasteClipboard(Document& doc, SeqRef ref, const TimelineClipboard& clip, int frame, int* skipped) {
    std::optional<Selection> out;
    int skip = 0;
    editSeq(doc, ref, [&](SeqMut& m) {
        skip = 0;
        // Keys: into the track with the same type and target, replacing a key on that frame.
        std::vector<std::pair<int, int>> placed;  // track, frame
        for (const TimelineClipboard::Key& ck : clip.keys) {
            auto t = std::find_if(m.tracks->begin(), m.tracks->end(),
                                  [&](const CutsceneTrack& tr) { return tr.type == ck.type && tr.target == ck.target; });
            if (t == m.tracks->end()) {
                ++skip;
                continue;
            }
            Keyframe k = ck.key;
            k.frame += frame;
            if (!frameOk(k.frame)) return false;
            std::vector<Keyframe>& ks = t->keyframes;
            auto same = std::find_if(ks.begin(), ks.end(), [&](const Keyframe& e) { return e.frame == k.frame; });
            if (same != ks.end()) {
                *same = k;
            } else {
                if (int(ks.size()) >= kMaxTimelineKeys) return false;
                ks.insert(std::upper_bound(ks.begin(), ks.end(), k.frame, [](int f, const Keyframe& e) { return f < e.frame; }), k);
            }
            placed.push_back({int(t - m.tracks->begin()), k.frame});
        }
        Selection sel;
        // Inserting shifts indices: address the pasted keys once all are in.
        for (auto [ti, f] : placed) {
            const auto& ks = (*m.tracks)[size_t(ti)].keyframes;
            for (int i = 0; i < int(ks.size()); ++i)
                if (ks[size_t(i)].frame == f && std::find(sel.keys.begin(), sel.keys.end(), KeyRef{ti, i}) == sel.keys.end())
                    sel.keys.push_back({ti, i});
        }
        if (!clip.audio.empty()) {
            if (!m.audio) {
                skip += int(clip.audio.size());
            } else {
                if (int(m.audio->size() + clip.audio.size()) > kMaxAudioEvents) return false;
                for (splash::CutsceneAudioEvent a : clip.audio) {
                    a.frame += frame;
                    if (!frameOk(a.frame)) return false;
                    sel.audio.push_back(int(m.audio->size()));
                    m.audio->push_back(std::move(a));
                }
            }
        }
        if (!clip.skin.empty()) {
            if (int(m.skin->size() + clip.skin.size()) > kMaxSkinEvents) return false;
            for (splash::SkinAnimEvent e : clip.skin) {
                e.frame += frame;
                if (!frameOk(e.frame)) return false;
                sel.skin.push_back(int(m.skin->size()));
                m.skin->push_back(std::move(e));
            }
        }
        if (sel.empty()) return false;
        out = std::move(sel);
        return true;
    });
    if (skipped) *skipped = skip;
    return out;
}

int addAudioEvent(Document& doc, SeqRef ref, int frame, const std::string& clip) {
    int added = -1;
    editSeq(doc, ref, [&](SeqMut& m) {
        if (!m.audio || clip.empty() || int(m.audio->size()) >= kMaxAudioEvents) return false;
        splash::CutsceneAudioEvent a;
        a.frame = std::clamp(frame, 0, kMaxTimelineFrame);
        a.clip = clip;
        m.audio->push_back(std::move(a));
        added = int(m.audio->size()) - 1;
        return true;
    });
    return added;
}

bool setAudioEvent(Document& doc, SeqRef ref, int index, splash::CutsceneAudioEvent ev, const std::string& mergeKey) {
    ev.frame = std::clamp(ev.frame, 0, kMaxTimelineFrame);
    ev.volume = std::clamp(ev.volume, 0, kMaxEventVolume);
    ev.pan = std::clamp(ev.pan, 0, kMaxEventPan);
    return editSeq(
        doc, ref,
        [&](SeqMut& m) {
            if (!m.audio || index < 0 || index >= int(m.audio->size()) || ev.clip.empty()) return false;
            splash::CutsceneAudioEvent& a = (*m.audio)[size_t(index)];
            if (a.frame == ev.frame && a.clip == ev.clip && a.volume == ev.volume && a.pan == ev.pan) return false;
            a.frame = ev.frame;
            a.clip = ev.clip;
            a.volume = ev.volume;
            a.pan = ev.pan;
            return true;
        },
        mergeKey);
}

int addSkinEvent(Document& doc, SeqRef ref, int frame, const std::string& object, const std::string& clip, bool loop) {
    int added = -1;
    editSeq(doc, ref, [&](SeqMut& m) {
        if (object.empty() || clip.empty() || int(m.skin->size()) >= kMaxSkinEvents) return false;
        splash::SkinAnimEvent e;
        e.frame = std::clamp(frame, 0, kMaxTimelineFrame);
        e.object = object;
        e.clip = clip;
        e.loop = loop;
        m.skin->push_back(std::move(e));
        added = int(m.skin->size()) - 1;
        return true;
    });
    return added;
}

bool setSkinEvent(Document& doc, SeqRef ref, int index, splash::SkinAnimEvent ev, const std::string& mergeKey) {
    ev.frame = std::clamp(ev.frame, 0, kMaxTimelineFrame);
    return editSeq(
        doc, ref,
        [&](SeqMut& m) {
            if (index < 0 || index >= int(m.skin->size()) || ev.object.empty() || ev.clip.empty()) return false;
            splash::SkinAnimEvent& e = (*m.skin)[size_t(index)];
            if (e.frame == ev.frame && e.object == ev.object && e.clip == ev.clip && e.loop == ev.loop) return false;
            e.frame = ev.frame;
            e.object = ev.object;
            e.clip = ev.clip;
            e.loop = ev.loop;
            return true;
        },
        mergeKey);
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

// The runtime's numbers: keys packed as the exporter packs them, then
// interpolation.cpp's integer arithmetic.

namespace {

int roundInt(float f) { return int(std::nearbyint(double(f))); }  // splash::roundToInt
int16_t sat16(int v) { return int16_t(std::clamp(v, -32768, 32767)); }

int32_t applyCurveFixed(int32_t t, Interp mode) {
    switch (mode) {
        case Interp::Linear: return t;
        case Interp::Step: return 0;
        case Interp::EaseIn: return int32_t(int64_t(t) * t >> 12);
        case Interp::EaseOut: return int32_t((int64_t(t) * (8192 - t)) >> 12);
        case Interp::EaseInOut: {
            const int64_t t2 = int64_t(t) * t;
            const int64_t t3 = t2 * t;
            return int32_t((3 * t2 - 2 * (t3 >> 12)) >> 12);
        }
    }
    return t;
}

}  // namespace

FixedValue encodeValue(TrackType t, const KeyValue& v, float gte) {
    auto coord = [&](float x) { return sat16(roundInt((x / gte) * 4096.f)); };
    auto angle = [](float deg) { return sat16(roundInt(deg * 1024.f / 180.f)); };
    auto byteOf = [](float x, int lo, int hi) { return int16_t(std::clamp(roundInt(x), lo, hi)); };
    FixedValue o{0, 0, 0};
    switch (t) {
        case TrackType::CameraPosition:
        case TrackType::ObjectPosition:
        case TrackType::LightPosition: o = {coord(v[0]), coord(-v[1]), coord(v[2])}; break;
        case TrackType::CameraRotation:
        case TrackType::ObjectRotation: o = {angle(-v[0]), angle(v[1]), angle(-v[2])}; break;
        case TrackType::ObjectActive:
        case TrackType::UICanvasVisible:
        case TrackType::UIElementVisible:
        case TrackType::RumbleSmall:
        case TrackType::LightEnabled: o[0] = v[0] > 0.5f ? 1 : 0; break;
        case TrackType::UIProgress: o[0] = byteOf(v[0], 0, 100); break;
        // The exporter adds the anchor's rounding correction to every key;
        // initialValue() is already less it, and a blend carries a shared
        // offset through unchanged, so it is left out here.
        case TrackType::UIPosition: o = {sat16(roundInt(v[0])), sat16(roundInt(v[1])), 0}; break;
        case TrackType::UIColor:
        case TrackType::LightColor:
            for (int i = 0; i < 3; ++i) o[size_t(i)] = byteOf(v[size_t(i)] * 255.f, 0, 255);
            break;
        case TrackType::CameraH: o[0] = byteOf(v[0], 1, 1024); break;
        case TrackType::RumbleLarge: o[0] = byteOf(v[0], 0, 255); break;
        case TrackType::ObjectUVOffset: o = {byteOf(v[0], 0, 255), byteOf(v[1], 0, 255), 0}; break;
        case TrackType::LightIntensity: o[0] = byteOf(v[0] * 4096.f, 0, 32767); break;
        case TrackType::LightRadius: o[0] = std::max<int16_t>(0, coord(v[0])); break;
    }
    return o;
}

KeyValue decodeValue(TrackType t, const FixedValue& o, float gte) {
    const float c = gte / 4096.f, a = 180.f / 1024.f;
    switch (t) {
        case TrackType::CameraPosition:
        case TrackType::ObjectPosition:
        case TrackType::LightPosition: return {o[0] * c, -o[1] * c, o[2] * c};
        case TrackType::CameraRotation:
        case TrackType::ObjectRotation: return {-o[0] * a, o[1] * a, -o[2] * a};
        case TrackType::UIColor:
        case TrackType::LightColor: return {o[0] / 255.f, o[1] / 255.f, o[2] / 255.f};
        case TrackType::LightIntensity: return {o[0] / 4096.f, 0, 0};
        case TrackType::LightRadius: return {o[0] * c, 0, 0};
        default: return {float(o[0]), float(o[1]), float(o[2])};
    }
}

FixedValue sampleFixedRaw(const CutsceneTrack& track, float frame, const KeyValue& initial, float gte) {
    const TrackType ty = track.type;
    if (isDiscrete(ty)) return encodeValue(ty, sample(track, frame, initial), gte);
    struct K {
        int frame;
        FixedValue v;
        Interp interp;
    };
    // Sorted stably by frame, as the export writes them.
    std::vector<K> kf;
    for (const Keyframe& k : track.keyframes) kf.push_back({k.frame & 0x1FFF, encodeValue(ty, k.value, gte), k.interp});
    std::stable_sort(kf.begin(), kf.end(), [](const K& a, const K& b) { return a.frame < b.frame; });
    const FixedValue init = encodeValue(ty, initial, gte);
    if (kf.empty()) return init;  // the runtime leaves the target alone
    // CutscenePlayer's m_frame and m_subFrame (0.12).
    frame = std::max(frame, 0.0f);
    const uint32_t fr = uint32_t(std::floor(frame));
    const uint32_t sub = std::min(uint32_t((frame - float(fr)) * 4096.f), 4095u);
    const size_t count = kf.size();
    FixedValue out{0, 0, 0};
    // lerpKeyframesSub: findKfPairSub, then the blend from the initial value
    // before the first key.
    if (fr < uint32_t(kf[0].frame) || (fr == uint32_t(kf[0].frame) && sub == 0)) {
        out = kf[0].v;
        if (kf[0].frame > 0 && fr < uint32_t(kf[0].frame)) {
            const uint32_t num = fr * 4096u + sub;
            const int32_t rawT = std::min<int32_t>(int32_t(num / uint32_t(kf[0].frame)), 4096);
            const int32_t ct = applyCurveFixed(rawT, kf[0].interp);
            for (size_t i = 0; i < 3; ++i) {
                const int32_t delta = int32_t(kf[0].v[i]) - int32_t(init[i]);
                out[i] = int16_t(int32_t(init[i]) + ((delta * ct) >> 12));
            }
        }
    } else if (fr > uint32_t(kf[count - 1].frame) || (fr == uint32_t(kf[count - 1].frame) && (sub > 0 || count == 1))) {
        out = kf[count - 1].v;
    } else if (fr == uint32_t(kf[count - 1].frame)) {
        // On the last key with no sub-frame the runtime's search runs one past
        // the end, but its distance into that segment is 0: the last key.
        out = kf[count - 1].v;
    } else {
        size_t b = 1;
        while (b < count && uint32_t(kf[b].frame) <= fr) ++b;
        const size_t a = b - 1;
        const uint32_t span = uint32_t(kf[b].frame - kf[a].frame);
        if (span == 0) {
            out = kf[a].v;
        } else {
            const uint32_t num = (fr - uint32_t(kf[a].frame)) * 4096u + sub;
            const int32_t t = applyCurveFixed(std::min<int32_t>(int32_t(num / span), 4096), kf[b].interp);
            for (size_t i = 0; i < 3; ++i) {
                const int32_t delta = int32_t(kf[b].v[i]) - int32_t(kf[a].v[i]);
                out[i] = int16_t(int32_t(kf[a].v[i]) + ((delta * t) >> 12));
            }
        }
    }
    // What the apply step clamps (cutscene.cpp).
    switch (ty) {
        case TrackType::CameraH: out[0] = int16_t(std::clamp<int>(out[0], 1, 1024)); break;
        case TrackType::LightColor:
            for (int16_t& c : out) c = int16_t(std::clamp<int>(c, 0, 255));
            break;
        case TrackType::LightIntensity:
        case TrackType::LightRadius: out[0] = std::max<int16_t>(out[0], 0); break;
        default: break;
    }
    return out;
}

KeyValue sampleFixed(const CutsceneTrack& track, float frame, const KeyValue& initial, float gte) {
    if (isDiscrete(track.type)) return sample(track, frame, initial);
    return decodeValue(track.type, sampleFixedRaw(track, frame, initial, gte), gte);
}

KeyValue initialValue(const Document& doc, const CutsceneTrack& track) {
    const int ti = int(track.type);
    if (ti >= 14 && track.type != TrackType::LightEnabled) {
        // captureLightTrack: the light as the scene ships it.
        for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
            if (fo.object->name == track.target && fo.object->light) {
                const splash::LightComponent& l = *fo.object->light;
                switch (track.type) {
                    case TrackType::LightPosition: return {fo.worldPosition.x, fo.worldPosition.y, fo.worldPosition.z};
                    case TrackType::LightColor: return {l.color[0], l.color[1], l.color[2]};
                    case TrackType::LightIntensity: return {l.intensity, 0, 0};
                    case TrackType::LightRadius: return {l.range, 0, 0};
                    default: break;
                }
            }
        return {0, 0, 0};
    }
    switch (track.type) {
        case TrackType::CameraH: return {120, 0, 0};  // psxsplash Camera's m_projH
        case TrackType::LightEnabled:
            for (const splash::FlatObject& fo : splash::flatten(doc.scene()))
                if (fo.object->name == track.target && fo.object->light) return {fo.object->light->enabled ? 1.0f : 0.0f, 0, 0};
            break;
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
        case TrackType::UIPosition: {
            // The element where it sits on screen (its anchor's pixel plus its
            // offset), in the units the exporter packs keys from.
            const size_t slash = track.target.find('/');
            if (slash == std::string::npos) break;
            for (const splash::UICanvas& c : doc.scene().canvases)
                if (c.name == track.target.substr(0, slash))
                    for (const splash::UIElement& e : c.elements)
                        if (e.name == track.target.substr(slash + 1)) {
                            const std::array<int, 2> p = splash::uiPositionTrackStart(e);
                            return {float(p[0]), float(p[1]), 0};
                        }
            break;
        }
        default: break;
    }
    return {0, 0, 0};
}

KeyValue sampleAt(const Document& doc, SeqRef ref, int track, float frame) {
    const std::vector<CutsceneTrack>* tracks = seqTracks(doc, ref);
    if (!tracks || track < 0 || track >= int(tracks->size())) return {0, 0, 0};
    const CutsceneTrack& t = (*tracks)[size_t(track)];
    return sampleFixed(t, frame, initialValue(doc, t), doc.scene().settings.gteScaling);
}

}  // namespace editor

namespace editor {

Preview samplePreview(const Document& doc, SeqRef ref, float frame) {
    Preview p;
    const std::vector<CutsceneTrack>* tracks = seqTracks(doc, ref);
    if (!tracks) return p;
    for (int i = 0; i < int(tracks->size()); ++i) {
        const CutsceneTrack& t = (*tracks)[size_t(i)];
        if (t.keyframes.empty()) continue;  // nothing to pose: the target shows as it is
        const KeyValue v = sampleAt(doc, ref, i, frame);
        const splash::Vec3 v3{v[0], v[1], v[2]};
        switch (t.type) {
            case TrackType::ObjectPosition: p.objects[t.target].position = v3; break;
            case TrackType::ObjectRotation: p.objects[t.target].rotation = v3; break;
            case TrackType::ObjectActive: p.objects[t.target].active = v[0] > 0.5f; break;
            case TrackType::ObjectUVOffset: p.objects[t.target].uvOffset = std::array<float, 2>{v[0], v[1]}; break;
            case TrackType::CameraPosition: p.cameraPosition = v3; break;
            case TrackType::CameraRotation: p.cameraRotation = v3; break;
            case TrackType::CameraH: p.cameraH = v[0]; break;
            case TrackType::LightPosition: p.lights[t.target].position = v3; break;
            case TrackType::LightColor: p.lights[t.target].color = v; break;
            case TrackType::LightIntensity: p.lights[t.target].intensity = v[0]; break;
            case TrackType::LightRadius: p.lights[t.target].radius = v[0]; break;
            case TrackType::LightEnabled: p.lights[t.target].enabled = v[0] > 0.5f; break;
            // UI and rumble: nothing in the Scene view to pose.
            default: break;
        }
    }
    return p;
}

splash::Quat quatEuler(splash::Vec3 deg) {
    const double h = 3.14159265358979323846 / 360.0;  // half angle, in radians per degree
    const double cx = std::cos(deg.x * h), sx = std::sin(deg.x * h);
    const double cy = std::cos(deg.y * h), sy = std::sin(deg.y * h);
    const double cz = std::cos(deg.z * h), sz = std::sin(deg.z * h);
    // qY * qX * qZ
    return {float(cy * sx * cz + sy * cx * sz), float(sy * cx * cz - cy * sx * sz), float(cy * cx * sz - sy * sx * cz),
            float(cy * cx * cz + sy * sx * sz)};
}

void applyPreview(splash::Scene& scene, const Preview& preview) {
    // Objects, then lights: a light's pose is an object pose (its position)
    // plus the light component's own fields.
    std::vector<std::pair<std::string, ObjectPose>> poses(preview.objects.begin(), preview.objects.end());
    for (const auto& [name, lp] : preview.lights) {
        ObjectPose op;
        op.position = lp.position;
        poses.push_back({name, op});
    }
    for (size_t pi = 0; pi < poses.size(); ++pi) {
        const std::string& name = poses[pi].first;
        const ObjectPose& pose = poses[pi].second;
        const bool isLight = pi >= preview.objects.size();
        // Flattened again per target: an earlier target may be this one's parent.
        const std::vector<splash::FlatObject> flats = splash::flatten(scene);
        const splash::FlatObject* fo = nullptr;
        const splash::FlatObject* parent = nullptr;
        for (const splash::FlatObject& f : flats)
            if (f.object->name == name && (!isLight || f.object->light)) {
                fo = &f;
                break;
            }
        if (!fo) continue;
        for (const splash::FlatObject& f : flats)
            for (const splash::Object& c : f.object->children)
                if (&c == fo->object) parent = &f;
        // The scene is the caller's own copy; flatten only hands out const pointers into it.
        splash::Object& ob = const_cast<splash::Object&>(*fo->object);
        const splash::Quat pr = parent ? parent->worldRotation : splash::Quat{};
        const splash::Quat prInv{-pr.x, -pr.y, -pr.z, pr.w};
        if (pose.position) {
            splash::Vec3 local = *pose.position;
            if (parent) {
                local = splash::rotate(prInv, local - parent->worldPosition);
                const splash::Vec3 s = parent->lossyScale;
                local = {s.x != 0 ? local.x / s.x : 0, s.y != 0 ? local.y / s.y : 0, s.z != 0 ? local.z / s.z : 0};
            }
            ob.transform.position = local;
        }
        if (pose.rotation) ob.transform.rotation = quatNormalize(quatMul(prInv, quatEuler(*pose.rotation)));
        if (pose.active && !*pose.active) ob.mesh.reset();
        if (isLight) {
            const LightPose& lp = preview.lights.at(name);
            splash::LightComponent& l = *ob.light;
            if (lp.color) l.color = *lp.color;
            if (lp.intensity) l.intensity = *lp.intensity;
            if (lp.radius) l.range = *lp.radius;
            if (lp.enabled) l.enabled = *lp.enabled;
        }
    }
}

}  // namespace editor
