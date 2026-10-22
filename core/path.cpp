#include "path.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace splash {

namespace {

constexpr float kPi = 3.14159265358979f;

Vec3 lerp3(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

// Barry and Goldman's pyramid for one centripetal Catmull-Rom segment p1..p2.
Vec3 crPoint(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float u01) {
    auto knot = [](Vec3 a, Vec3 b) { return std::max(std::sqrt(magnitude(b - a)), 1e-4f); };
    const float t0 = 0, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
    const float u = t1 + (t2 - t1) * u01;
    auto mix = [](Vec3 a, Vec3 b, float ta, float tb, float t) { return a * ((tb - t) / (tb - ta)) + b * ((t - ta) / (tb - ta)); };
    const Vec3 a1 = mix(p0, p1, t0, t1, u), a2 = mix(p1, p2, t1, t2, u), a3 = mix(p2, p3, t2, t3, u);
    const Vec3 b1 = mix(a1, a2, t0, t2, u), b2 = mix(a2, a3, t1, t3, u);
    return mix(b1, b2, t1, t2, u);
}

// Index of the dense segment holding arc length s.
size_t segmentAt(const PathCurve& c, float s) {
    auto it = std::upper_bound(c.acc.begin(), c.acc.end(), s);
    size_t k = it == c.acc.begin() ? 0 : size_t(it - c.acc.begin()) - 1;
    return std::min(k, c.dense.size() - 2);
}

std::string cm(float m) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f cm", double(m * 100.f));
    return b;
}

const FlatObject* findPath(const std::vector<FlatObject>& flat, const std::string& name) {
    for (const FlatObject& fo : flat)
        if (fo.object->path && fo.object->name == name) return &fo;
    return nullptr;
}

std::string luaStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\', o += char(ch);
        else if (ch < 32 || ch == 127) {
            char b[8];
            std::snprintf(b, sizeof b, "\\%03d", ch);
            o += b;
        } else o += char(ch);
    }
    return o + "\"";
}

}  // namespace

PathCurve buildCurve(const std::vector<Vec3>& pts, bool closed, int perSegment) {
    PathCurve c;
    c.closed = closed && pts.size() > 2;
    const size_t n = pts.size();
    if (n == 0) return c;
    if (n == 1) {
        c.dense = {pts[0], pts[0]};
        c.acc = {0, 0};
        return c;
    }
    auto at = [&](long i) -> Vec3 {
        if (c.closed) return pts[size_t(((i % long(n)) + long(n)) % long(n))];
        if (i < 0) return pts[0] * 2.f - pts[1];
        if (i >= long(n)) return pts[n - 1] * 2.f - pts[n - 2];
        return pts[size_t(i)];
    };
    const long segs = c.closed ? long(n) : long(n) - 1;
    for (long i = 0; i < segs; i++)
        for (int j = 0; j < perSegment; j++)
            c.dense.push_back(crPoint(at(i - 1), at(i), at(i + 1), at(i + 2), float(j) / float(perSegment)));
    c.dense.push_back(c.closed ? pts[0] : pts[n - 1]);
    c.acc.push_back(0);
    for (size_t i = 1; i < c.dense.size(); i++) c.acc.push_back(c.acc.back() + magnitude(c.dense[i] - c.dense[i - 1]));
    c.length = c.acc.back();
    return c;
}

Vec3 curvePoint(const PathCurve& c, float t) {
    if (c.dense.empty()) return {};
    if (c.length <= 0) return c.dense[0];
    const float s = std::clamp(t, 0.f, 1.f) * c.length;
    const size_t k = segmentAt(c, s);
    const float len = c.acc[k + 1] - c.acc[k];
    return len > 0 ? lerp3(c.dense[k], c.dense[k + 1], (s - c.acc[k]) / len) : c.dense[k];
}

Vec3 curveDirection(const PathCurve& c, float t) {
    if (c.dense.size() < 2 || c.length <= 0) return {};
    size_t k = segmentAt(c, std::clamp(t, 0.f, 1.f) * c.length);
    // Skip zero-length pieces (repeated points).
    for (size_t i = k; i + 1 < c.dense.size(); i++)
        if (sqrMagnitude(c.dense[i + 1] - c.dense[i]) > 1e-12f) return normalized(c.dense[i + 1] - c.dense[i]);
    for (size_t i = k + 1; i-- > 0;)
        if (sqrMagnitude(c.dense[i + 1] - c.dense[i]) > 1e-12f) return normalized(c.dense[i + 1] - c.dense[i]);
    return {};
}

std::vector<Vec3> worldPoints(const FlatObject& fo) {
    std::vector<Vec3> out;
    if (fo.object->path)
        for (const Vec3& p : fo.object->path->points) out.push_back(fo.localToWorld.point(p));
    return out;
}

PathCurve worldCurve(const FlatObject& fo) {
    return buildCurve(worldPoints(fo), fo.object->path && fo.object->path->closed);
}

float modeProgress(PathMode m, float laps) {
    if (!(laps > 0)) return 0;
    switch (m) {
        case PathMode::Once: return std::min(laps, 1.f);
        case PathMode::Loop: return laps - std::floor(laps);
        case PathMode::PingPong: {
            const float x = std::fmod(laps, 2.f);
            return x <= 1.f ? x : 2.f - x;
        }
    }
    return 0;
}

float yawDegrees(Vec3 d) { return std::atan2(d.x, d.z) * 180.f / kPi; }
float pitchDegrees(Vec3 d) { return -std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z)) * 180.f / kPi; }

std::vector<int> fitKeys(const std::vector<std::vector<float>>& v, int maxKeys, float* maxError) {
    std::vector<int> keys;
    const int n = int(v.size());
    if (maxError) *maxError = 0;
    if (n == 0) return keys;
    if (n == 1 || maxKeys < 2) return {0};
    keys = {0, n - 1};
    auto err = [&](int a, int b, int i) {
        const float t = float(i - a) / float(b - a);
        float d2 = 0;
        for (size_t k = 0; k < v[size_t(i)].size(); k++) {
            const float e = v[size_t(a)][k] + (v[size_t(b)][k] - v[size_t(a)][k]) * t - v[size_t(i)][k];
            d2 += e * e;
        }
        return std::sqrt(d2);
    };
    for (;;) {
        float worst = 0;
        int at = -1;
        for (size_t s = 0; s + 1 < keys.size(); s++)
            for (int i = keys[s] + 1; i < keys[s + 1]; i++) {
                const float e = err(keys[s], keys[s + 1], i);
                if (e > worst) worst = e, at = i;
            }
        if (at < 0 || worst < 1e-4f || int(keys.size()) >= maxKeys) {
            if (maxError) *maxError = at < 0 ? 0 : worst;
            break;
        }
        keys.insert(std::upper_bound(keys.begin(), keys.end(), at), at);
    }
    return keys;
}

static float curveAt(const std::vector<Keyframe>& ks, float frame) {
    // timeline.cpp sample(): between keys the later key's curve, after the last it holds.
    auto curve = [](float t, Interp m) {
        switch (m) {
            case Interp::Linear: return t;
            case Interp::Step: return 0.f;
            case Interp::EaseIn: return t * t;
            case Interp::EaseOut: return t * (2 - t);
            case Interp::EaseInOut: return t * t * (3 - 2 * t);
        }
        return t;
    };
    if (frame <= float(ks.front().frame) || ks.size() == 1) return ks.front().value[0];
    if (frame >= float(ks.back().frame)) return ks.back().value[0];
    size_t b = 1;
    while (b < ks.size() && float(ks[b].frame) <= frame) ++b;
    const Keyframe &ka = ks[b - 1], &kb = ks[b];
    const int span = kb.frame - ka.frame;
    if (span <= 0) return ka.value[0];
    const float t = std::clamp((frame - float(ka.frame)) / float(span), 0.f, 1.f);
    return ka.value[0] + (kb.value[0] - ka.value[0]) * curve(t, kb.interp);
}

CutsceneTrack bakeFollowTrack(const Scene& scene, const CutsceneTrack& track, std::string* problem) {
    CutsceneTrack out;
    out.type = track.target.empty() ? TrackType::CameraPosition : TrackType::ObjectPosition;
    out.target = track.target;
    const std::vector<FlatObject> flat = flatten(scene);
    const FlatObject* po = findPath(flat, track.path);
    if (!po) {
        if (problem) *problem = track.path.empty() ? "no path chosen" : "no path named '" + track.path + "'";
        return out;
    }
    if (track.keyframes.empty()) {
        if (problem) *problem = "no keys";
        return out;
    }
    std::vector<Keyframe> ks = track.keyframes;
    std::stable_sort(ks.begin(), ks.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
    const PathCurve c = worldCurve(*po);
    const int f0 = ks.front().frame, f1 = ks.back().frame;
    std::vector<std::vector<float>> per;
    for (int f = f0; f <= f1; f++) {
        const Vec3 p = curvePoint(c, std::clamp(curveAt(ks, float(f)), 0.f, 1.f));
        per.push_back({p.x, p.y, p.z});
    }
    float maxErr = 0;
    for (int i : fitKeys(per, kPathMaxKeys, &maxErr)) {
        Keyframe k;
        k.frame = f0 + i;
        k.value = {per[size_t(i)][0], per[size_t(i)][1], per[size_t(i)][2]};
        out.keyframes.push_back(k);
    }
    if (problem && maxErr > 0.05f)
        *problem = "the " + std::to_string(kPathMaxKeys) + "-keyframe cap cuts corners by up to " + cm(maxErr);
    return out;
}

bool sceneUsesPaths(const Scene& scene) {
    std::function<bool(const Object&)> any = [&](const Object& o) {
        if (o.path || o.followPath) return true;
        for (const Object& c : o.children)
            if (any(c)) return true;
        return false;
    };
    for (const Object& o : scene.objects)
        if (any(o)) return true;
    auto hasTrack = [](const std::vector<CutsceneTrack>& ts) {
        return std::any_of(ts.begin(), ts.end(), [](const CutsceneTrack& t) { return t.type == TrackType::FollowPath; });
    };
    for (const Cutscene& c : scene.cutscenes)
        if (hasTrack(c.tracks)) return true;
    for (const Animation& a : scene.animations)
        if (hasTrack(a.tracks)) return true;
    return false;
}

namespace {

// One path as the generated Lua and the bake see it.
struct PathInfo {
    const FlatObject* fo;
    PathCurve curve;
    int lapFrames = 0;  // at the path's own speed
    std::vector<int> groups;  // indices into PathBake::groups
    struct Plain {
        std::string name;
        int group;
        int offsetFrames;
    };
    std::vector<Plain> plain;
    struct Agent {
        std::string name;
        int stride, start, speedRaw;
    };
    std::vector<Agent> agents;
    bool table = false;  // a sample table for the camera and agents
};

constexpr int kLuaSamples = 64;
constexpr int kLuaOne = 1 << 22;  // progress units along a path in the Lua driver

int lapFramesFor(float length, float speed) {
    return std::max(1, roundToInt(length / speed * 30.f));
}

std::string followerLabel(const FlatObject& fo) { return "'" + fo.object->name + "'"; }

}  // namespace

PathBake bakePaths(const Scene& in, const std::function<std::string(const std::string&)>& readSource) {
    PathBake b;
    b.scene = in;
    Scene& sc = b.scene;
    const float gte = sc.settings.gteScaling;
    std::vector<FlatObject> flat = flatten(sc);

    // Timeline tracks first: they need only the curves.
    auto bakeTracks = [&](std::vector<CutsceneTrack>& tracks, const std::string& where, bool animation) {
        std::vector<CutsceneTrack> keep;
        for (const CutsceneTrack& t : tracks) {
            if (t.type != TrackType::FollowPath) {
                keep.push_back(t);
                continue;
            }
            const std::string label = where + ": follow path track (" + (t.target.empty() ? "camera" : t.target) + ")";
            if (animation && t.target.empty()) {
                b.warnings.push_back(label + " left out: animations have no camera tracks");
                continue;
            }
            std::string problem;
            CutsceneTrack baked = bakeFollowTrack(sc, t, &problem);
            if (!problem.empty()) b.warnings.push_back(label + ": " + problem);
            for (const CutsceneTrack& o : tracks)
                if (o.type == baked.type && o.target == baked.target)
                    b.warnings.push_back(label + " and a position track drive the same " +
                                         (t.target.empty() ? std::string("camera") : "object") + "; the later one wins");
            if (!baked.keyframes.empty()) keep.push_back(std::move(baked));
        }
        tracks = std::move(keep);
    };
    for (Cutscene& c : sc.cutscenes) bakeTracks(c.tracks, "cutscene '" + c.name + "'", false);
    for (Animation& a : sc.animations) bakeTracks(a.tracks, "animation '" + a.name + "'", true);

    // Paths.
    std::vector<PathInfo> paths;
    std::set<std::string> names;
    for (const FlatObject& fo : flat) {
        if (!fo.object->path || !fo.activeInHierarchy) continue;
        if (!names.insert(fo.object->name).second) {
            b.errors.push_back("two paths are named '" + fo.object->name + "' (scripts and followers name a path)");
            continue;
        }
        PathInfo pi{&fo, worldCurve(fo)};
        const float sp = fo.object->path->speed;
        pi.lapFrames = sp > 0 && pi.curve.length > 0 ? lapFramesFor(pi.curve.length, sp) : 0;
        if (fo.object->path->points.size() < 2)
            b.warnings.push_back("path '" + fo.object->name + "' has fewer than 2 points: nothing rides it");
        paths.push_back(std::move(pi));
    }
    auto pathIndex = [&](const std::string& name) -> int {
        for (size_t i = 0; i < paths.size(); i++)
            if (paths[i].fo->object->name == name) return int(i);
        return -1;
    };

    // Followers. Animation-driven ones group by (path, speed); a group is one animation of at most 8 tracks.
    std::set<std::string> animated;  // targets the user's own position tracks drive
    for (const Cutscene& c : in.cutscenes)
        for (const CutsceneTrack& t : c.tracks)
            if (t.type == TrackType::ObjectPosition) animated.insert(t.target);
    for (const Animation& a : in.animations)
        for (const CutsceneTrack& t : a.tracks)
            if (t.type == TrackType::ObjectPosition) animated.insert(t.target);
    struct Pending {
        const FlatObject* fo;
        int path;
        float speed;
    };
    std::vector<Pending> plain;
    std::set<std::string> followerNames;
    for (const FlatObject& fo : flat) {
        if (!fo.object->followPath || !fo.activeInHierarchy) continue;
        const FollowPathComponent& f = *fo.object->followPath;
        const std::string who = followerLabel(fo);
        const int pi = pathIndex(f.path);
        if (pi < 0) {
            b.warnings.push_back(who + " follows " + (f.path.empty() ? std::string("no path") : "path '" + f.path + "', which is not in the scene"));
            continue;
        }
        if (!followerNames.insert(fo.object->name).second)
            b.warnings.push_back("two followers are named " + who + ": the console finds the first by name");
        PathInfo& p = paths[size_t(pi)];
        const float speed = f.speed > 0 ? f.speed : p.fo->object->path->speed;
        if (fo.object->agent) {
            if (!fo.object->agent->waypoints.empty())
                b.warnings.push_back(who + " follows path '" + f.path + "' and has patrol waypoints: they fight over where it walks");
            const float spacing = p.curve.length / float(kLuaSamples - 1);
            const float want = std::max(1.0f, fo.object->agent->stopDistance * 3.f);
            PathInfo::Agent a;
            a.name = fo.object->name;
            a.stride = spacing > 0 ? std::clamp(int(std::ceil(want / spacing)), 1, kLuaSamples - 1) : 1;
            a.start = roundToInt(f.startOffset * float(kLuaSamples - 1));
            a.speedRaw = f.speed > 0 ? clampv(roundToInt(f.speed / 30.f / gte * 4096.f), 1, 65535) : 0;
            p.agents.push_back(a);
            p.table = true;
            continue;
        }
        if (!fo.object->mesh)
            b.warnings.push_back(who + " follows path '" + f.path + "' but has no mesh: the console has nothing to move");
        if (!fo.object->children.empty())
            b.warnings.push_back(who + " follows path '" + f.path + "': its children stay where they are on the console");
        if (animated.count(fo.object->name))
            b.warnings.push_back(who + " follows path '" + f.path + "' and an animation or cutscene also moves it");
        if (!(speed > 0) || p.curve.length <= 0) {
            b.warnings.push_back(who + " follows path '" + f.path + "' at speed 0 or along no length: it does not move");
            continue;
        }
        plain.push_back({&fo, pi, speed});
    }

    // Bake the animation groups.
    std::set<std::string> taken;
    for (const Animation& a : sc.animations) taken.insert(a.name);
    int serial = 0;
    std::vector<Animation> generated;
    std::vector<bool> done(plain.size(), false);
    for (size_t i = 0; i < plain.size(); i++) {
        if (done[i]) continue;
        const PathInfo& p = paths[size_t(plain[i].path)];
        const PathComponent& pc = *p.fo->object->path;
        Animation anim;
        do anim.name = "~path" + std::to_string(++serial);
        while (taken.count(anim.name));
        taken.insert(anim.name);
        PathBake::Group g;
        g.animation = anim.name;
        g.path = p.fo->object->name;
        int lap = lapFramesFor(p.curve.length, plain[i].speed);
        const int cycleMul = pc.mode == PathMode::PingPong ? 2 : 1;
        if (lap * cycleMul > kPathMaxFrames) {
            b.warnings.push_back("path '" + g.path + "': a lap at that speed is longer than " +
                                 std::to_string(kPathMaxFrames) + " frames, the longest an animation plays; it rides faster");
            lap = kPathMaxFrames / cycleMul;
        }
        g.lapFrames = lap;
        int tracks = 0, duration = 0;
        const int gi = int(b.groups.size());
        for (size_t j = i; j < plain.size(); j++) {
            if (done[j] || plain[j].path != plain[i].path || std::fabs(plain[j].speed - plain[i].speed) > 1e-4f) continue;
            const FollowPathComponent& f = *plain[j].fo->object->followPath;
            const int need = f.faceAlong ? 2 : 1;
            if (tracks + need > kPathMaxAnimTracks) continue;
            tracks += need;
            done[j] = true;
            const std::string& name = plain[j].fo->object->name;
            g.followers.push_back(name);
            // Frames of this follower's keys.
            const float off = f.startOffset;
            const int frames = pc.mode == PathMode::Once ? std::max(1, roundToInt((1.f - off) * float(lap))) : lap * cycleMul;
            duration = std::max(duration, frames);
            std::vector<std::vector<float>> per;
            std::vector<Vec3> pos;
            std::vector<float> yaw;
            float prevYaw = 0;
            for (int fr = 0; fr <= frames; fr++) {
                const float laps = float(fr) / float(lap) + off;
                const float t = modeProgress(pc.mode, laps);
                Vec3 d = curveDirection(p.curve, t);
                if (pc.mode == PathMode::PingPong && std::fmod(laps, 2.f) > 1.f) d = d * -1.f;
                float y = sqrMagnitude(d) > 0 ? yawDegrees(d) : prevYaw;
                if (fr > 0) {
                    while (y - prevYaw > 180.f) y -= 360.f;
                    while (y - prevYaw < -180.f) y += 360.f;
                }
                prevYaw = y;
                const Vec3 q = curvePoint(p.curve, t);
                pos.push_back(q);
                yaw.push_back(y);
                std::vector<float> row{q.x, q.y, q.z};
                if (f.faceAlong) row.push_back(y * 0.005f);  // a degree weighs as 5 mm
                per.push_back(row);
            }
            float err = 0;
            const std::vector<int> keys = fitKeys(per, kPathMaxKeys, &err);
            g.keys = std::max(g.keys, int(keys.size()));
            g.maxError = std::max(g.maxError, err);
            CutsceneTrack tp;
            tp.type = TrackType::ObjectPosition;
            tp.target = name;
            CutsceneTrack tr;
            tr.type = TrackType::ObjectRotation;
            tr.target = name;
            bool outOfRange = false, spun = false;
            for (int k : keys) {
                Keyframe kp;
                kp.frame = k;
                kp.value = {pos[size_t(k)].x, pos[size_t(k)].y, pos[size_t(k)].z};
                for (float v : kp.value)
                    if (std::fabs(v / gte * 4096.f) > 32767.f) outOfRange = true;
                tp.keyframes.push_back(kp);
                Keyframe kr;
                kr.frame = k;
                kr.value = {0, yaw[size_t(k)], 0};
                if (std::fabs(yaw[size_t(k)] * 1024.f / 180.f) > 32767.f) spun = true;
                tr.keyframes.push_back(kr);
            }
            anim.tracks.push_back(std::move(tp));
            if (f.faceAlong) anim.tracks.push_back(std::move(tr));
            if (outOfRange)
                b.warnings.push_back("'" + name + "' rides path '" + g.path + "' further than " + std::to_string(int(32767.f / 4096.f * gte)) +
                                     " m from the origin, more than an animation key holds (16 bits at this GTE scaling)");
            if (spun) b.warnings.push_back("'" + name + "' turns more than 16 times a lap on path '" + g.path + "'; its turning is cut");
            if (err > 0.05f)
                b.warnings.push_back("'" + name + "' on path '" + g.path + "': " + std::to_string(kPathMaxKeys) +
                                     " keys a lap cut corners by up to " + cm(err) + "; shorten the path or use more paths");
            paths[size_t(plain[j].path)].plain.push_back({name, gi, roundToInt(off * float(lap))});
        }
        anim.durationFrames = duration;
        paths[size_t(plain[i].path)].groups.push_back(gi);
        b.groups.push_back(g);
        generated.push_back(std::move(anim));
    }
    // psxsplash loads at most 16 animations: the user's come first.
    const size_t room = sc.animations.size() < size_t(kPathMaxAnimations) ? kPathMaxAnimations - sc.animations.size() : 0;
    if (generated.size() > room) {
        std::string left;
        for (size_t k = room; k < generated.size(); k++)
            for (const std::string& n : b.groups[k].followers) left += (left.empty() ? "" : ", ") + n;
        b.warnings.push_back("paths need " + std::to_string(generated.size()) + " animations and the scene has " +
                             std::to_string(sc.animations.size()) + " of its own; psxsplash holds " +
                             std::to_string(kPathMaxAnimations) + ", so these do not ride: " + left);
        for (PathInfo& p : paths) {
            p.plain.erase(std::remove_if(p.plain.begin(), p.plain.end(), [&](const PathInfo::Plain& x) { return x.group >= int(room); }),
                          p.plain.end());
            p.groups.erase(std::remove_if(p.groups.begin(), p.groups.end(), [&](int x) { return x >= int(room); }), p.groups.end());
        }
        generated.resize(room);
        b.groups.resize(room);
    }
    if (generated.size() > size_t(kPathMaxPlaying))
        b.warnings.push_back("paths make " + std::to_string(generated.size()) + " animations; psxsplash plays " +
                             std::to_string(kPathMaxPlaying) + " at once (yours included), so not all of them can run together");
    else if (!generated.empty() && generated.size() + sc.animations.size() > size_t(kPathMaxPlaying))
        b.warnings.push_back("paths make " + std::to_string(generated.size()) + " animations and the scene has " +
                             std::to_string(sc.animations.size()) + ": psxsplash plays " + std::to_string(kPathMaxPlaying) +
                             " at once, and Animation.Play does nothing when all are busy");
    for (Animation& a : generated) sc.animations.push_back(std::move(a));

    if (paths.empty()) return b;

    // The camera can ride any path from Lua: give the paths a sample table
    // when a script asks for it.
    bool camera = false;
    if (readSource) {
        std::set<std::string> files;
        std::function<void(const Object&)> collect = [&](const Object& o) {
            if (o.script) files.insert(o.script->lua);
            if (o.trigger) files.insert(o.trigger->lua);
            for (const Object& c : o.children) collect(c);
        };
        for (const Object& o : in.objects) collect(o);
        if (!in.settings.script.empty()) files.insert(in.settings.script);
        for (const std::string& f : files) {
            if (f.empty()) continue;
            try {
                const std::string src = readSource(f);
                if (src.find("RideCamera") != std::string::npos || src.find("camera_ride") != std::string::npos) camera = true;
            } catch (...) {
            }
        }
    }
    if (camera)
        for (PathInfo& p : paths) p.table = true;

    // The host: a path object that starts active (onUpdate runs on active
    // objects only), one without a script of its own when there is one.
    const PathInfo* host = nullptr;
    for (const PathInfo& p : paths)
        if (p.fo->object->startActive && !p.fo->object->script) {
            host = &p;
            break;
        }
    if (!host)
        for (const PathInfo& p : paths)
            if (p.fo->object->startActive) {
                host = &p;
                break;
            }
    if (!host) {
        b.warnings.push_back("every path starts hidden: the Path table has no object to run on, so agents and the camera do not ride");
        host = &paths[0];
    }

    // The library.
    std::string L;
    L += "-- Generated by SplashEdit from the scene's paths when it exported. Do not edit.\n";
    L += "local P = {}\n_G.Path = P\nlocal ONE = " + std::to_string(kLuaOne) + "\n";
    L += "local list, byObj, cam, warned = {}, nil, nil, {}\n";
    L += "local function def(p) list[#list + 1] = p end\n";
    for (const PathInfo& p : paths) {
        const PathComponent& pc = *p.fo->object->path;
        L += "def({ name = " + luaStr(p.fo->object->name) + ", mode = " + std::to_string(int(pc.mode)) +
             ", auto = " + (pc.autostart ? "true" : "false") + ", closed = " + (p.curve.closed ? "true" : "false") +
             ", lap = " + std::to_string(p.lapFrames) + ",\n  groups = {";
        for (int g : p.groups)
            L += " { " + luaStr(b.groups[size_t(g)].animation) + ", " + std::to_string(b.groups[size_t(g)].lapFrames) + " },";
        L += " },\n  plain = {";
        for (const PathInfo::Plain& f : p.plain) {
            int gl = 0;
            for (size_t k = 0; k < p.groups.size(); k++)
                if (p.groups[k] == f.group) gl = int(k) + 1;
            L += " { " + luaStr(f.name) + ", " + std::to_string(gl) + ", " + std::to_string(f.offsetFrames) + " },";
        }
        L += " },\n  agents = {";
        for (const PathInfo::Agent& a : p.agents)
            L += " { " + luaStr(a.name) + ", " + std::to_string(a.stride) + ", " + std::to_string(a.start) + ", " +
                 std::to_string(a.speedRaw) + " },";
        L += " },\n";
        if (p.table && p.curve.length > 0) {
            const float sp = pc.speed > 0 ? pc.speed : 1.f;
            const double inc = double(kLuaOne) * double(sp) / double(p.curve.length) / 30.0;
            L += "  n = " + std::to_string(kLuaSamples) + ", inc = " + std::to_string(std::clamp(int(std::lround(inc)), 1, 8000000)) +
                 ",\n  pos = {";
            std::string rot;
            float prevYaw = 0, prevPitch = 0;
            for (int i = 0; i < kLuaSamples; i++) {
                const float t = float(i) / float(kLuaSamples - 1);
                const Vec3 q = curvePoint(p.curve, t);
                L += std::to_string(roundToInt(q.x / gte * 4096.f)) + "," + std::to_string(roundToInt(-q.y / gte * 4096.f)) + "," +
                     std::to_string(roundToInt(q.z / gte * 4096.f)) + (i + 1 < kLuaSamples ? "," : "");
                const Vec3 d = curveDirection(p.curve, t);
                float y = sqrMagnitude(d) > 0 ? yawDegrees(d) : prevYaw, x = sqrMagnitude(d) > 0 ? pitchDegrees(d) : prevPitch;
                if (i > 0) {
                    while (y - prevYaw > 180.f) y -= 360.f;
                    while (y - prevYaw < -180.f) y += 360.f;
                }
                prevYaw = y, prevPitch = x;
                // Camera.SetRotation: pi units in 4.12, X negated as the exporter's camera keys.
                rot += std::to_string(roundToInt(-x / 180.f * 4096.f)) + "," + std::to_string(roundToInt(y / 180.f * 4096.f)) +
                       (i + 1 < kLuaSamples ? "," : "");
            }
            L += "},\n  rot = {" + rot + "} })\n";
        } else {
            L += "  n = 0 })\n";
        }
    }
    L += R"LUA(
local function pct(mode, tf, lap, off)
  if lap <= 0 then return 0 end
  local x = tf + off
  if mode == 0 then
    if x >= lap then return 100 end
    return x * 100 / lap
  elseif mode == 1 then
    x = x % (lap + 1)
    if x > lap then x = lap end
    return x * 100 / lap
  end
  x = x % (2 * lap + 1)
  if x > lap then x = 2 * lap - x end
  if x < 0 then x = 0 end
  return x * 100 / lap
end
local function vec()
  local nf = FixedPoint.newFromRaw
  return { x = nf(0), y = nf(0), z = nf(0) }
end
local function init()
  if byObj then return end
  byObj = {}
  for _, p in ipairs(list) do
    p.on, p.t = false, 0
    local e = Entity.Find(p.name)
    if e then byObj[e] = { path = p } end
    for _, f in ipairs(p.plain) do
      local o = Entity.Find(f[1])
      if o then byObj[o] = { path = p, plain = f } end
    end
    p.ag = {}
    for _, a in ipairs(p.agents) do
      local r = { actor = Actor.Find(a[1]), stride = a[2], idx = a[3], dir = 1, speed = a[4], on = false, v = vec() }
      if r.idx > p.n - 1 then r.idx = p.n - 1 end
      if r.idx < 0 then r.idx = 0 end
      p.ag[#p.ag + 1] = r
      local o = Entity.Find(a[1])
      if o then byObj[o] = { path = p, agent = r } end
    end
  end
end
local function resolve(x)
  init()
  if x == Camera then return { camera = true } end
  if type(x) == "string" then
    for _, p in ipairs(list) do if p.name == x then return { path = p } end end
    return nil
  end
  if x == nil then return nil end
  return byObj[x]
end
local function moveAgent(p, r)
  if not r.actor or p.n == 0 then return end
  local a = r.idx * 3 + 1
  r.v.x._raw, r.v.y._raw, r.v.z._raw = p.pos[a], p.pos[a + 1], p.pos[a + 2]
  Agent.MoveTo(r.actor, r.v)
end
local function stepAgent(p, r)
  if not r.actor or Agent.IsMoving(r.actor) then return end
  local last = p.n - 1
  local nx = r.idx + r.dir * r.stride
  if p.mode == 1 then
    if nx > last then
      if p.closed then nx = nx - last else nx = 0 end
    end
  elseif p.mode == 2 then
    if nx > last then r.dir = -1; nx = 2 * last - nx elseif nx < 0 then r.dir = 1; nx = -nx end
    if nx < 0 then nx = 0 end
    if nx > last then nx = last end
  else
    if r.idx >= last then r.on = false; return end
    if nx > last then nx = last end
  end
  r.idx = nx
  moveAgent(p, r)
end
local function startPath(p)
  if p.on then return end
  p.on, p.t = true, 0
  for _, g in ipairs(p.groups) do
    if not Animation.IsPlaying(g[1]) then Animation.Play(g[1], { loop = p.mode ~= 0 }) end
  end
  for _, r in ipairs(p.ag) do
    r.on = true
    if r.speed > 0 and r.actor then Agent.SetSpeed(r.actor, FixedPoint.newFromRaw(r.speed)) end
    moveAgent(p, r)
  end
end
local function stopPath(p)
  p.on = false
  for _, g in ipairs(p.groups) do Animation.Stop(g[1]) end
  for _, r in ipairs(p.ag) do
    if r.on then r.on = false; if r.actor then Agent.Stop(r.actor) end end
  end
end
local function camPlace()
  local p = cam.path
  local q = cam.p
  local back = false
  if q >= ONE then q = 2 * ONE - q; back = true end
  if q < 0 then q = 0 end
  local n1 = p.n - 1
  local s = q * n1
  local i = s / ONE
  if i >= n1 then i = n1 - 1 end
  local f = (s - i * ONE) / 1024
  local a = i * 3 + 1
  local ps = p.pos
  local v = cam.v
  v.x._raw = ps[a] + (ps[a + 3] - ps[a]) * f / 4096
  v.y._raw = ps[a + 1] + (ps[a + 4] - ps[a + 1]) * f / 4096
  v.z._raw = ps[a + 2] + (ps[a + 5] - ps[a + 2]) * f / 4096
  Camera.SetPosition(v)
  if cam.face then
    local b = i * 2 + 1
    local rs = p.rot
    local rx = rs[b] + (rs[b + 2] - rs[b]) * f / 4096
    local ry = rs[b + 1] + (rs[b + 3] - rs[b + 1]) * f / 4096
    if back then rx = -rx; ry = ry + 4096 end
    cam.r.x._raw, cam.r.y._raw = rx, ry
    Camera.SetRotation(cam.r)
  end
end
local function camStep(dt)
  local p = cam.path
  local q = cam.p + p.inc * (dt / 64) / 64
  if p.mode == 0 then
    if q >= ONE then q = ONE; cam.on = false end
  elseif p.mode == 1 then
    while q >= ONE do q = q - ONE end
  else
    while q >= 2 * ONE do q = q - 2 * ONE end
  end
  cam.p = q
  camPlace()
end
function P._create()
  init()
  for _, p in ipairs(list) do if p.auto then startPath(p) end end
end
function P._tick(dt)
  if not byObj then return end
  for _, p in ipairs(list) do
    if p.on then
      p.t = p.t + dt
      local busy = false
      if #p.groups > 0 then
        if p.mode ~= 0 then busy = true
        else
          for _, g in ipairs(p.groups) do if Animation.IsPlaying(g[1]) then busy = true end end
        end
      end
      for _, r in ipairs(p.ag) do
        if r.on then stepAgent(p, r) end
        if r.on then busy = true end
      end
      if not busy and #p.groups + #p.ag > 0 then p.on = false end
    end
  end
  if cam and cam.on then camStep(dt) end
end
function P.Start(x)
  local r = resolve(x)
  if r and r.path then startPath(r.path) end
end
function P.Stop(x)
  local r = resolve(x)
  if r and r.path then stopPath(r.path) end
end
function P.IsRunning(x)
  local r = resolve(x)
  if not r then return false end
  if r.camera then return cam ~= nil and cam.on end
  if r.agent then return r.agent.on end
  return r.path.on
end
function P.GetProgress(x)
  local r = resolve(x)
  if not r then return nil end
  if r.camera then
    if not cam then return nil end
    local q = cam.p
    if q >= ONE then q = 2 * ONE - q end
    return q / (ONE / 100)
  end
  local p = r.path
  if r.agent then
    if p.n < 2 then return 0 end
    return r.agent.idx * 100 / (p.n - 1)
  end
  local tf = p.t / 4096
  if r.plain then
    local g = p.groups[r.plain[2]]
    return pct(p.mode, tf, g[2], r.plain[3])
  end
  if #p.groups == 0 and #p.ag > 0 then
    local a = p.ag[1]
    if p.n < 2 then return 0 end
    return a.idx * 100 / (p.n - 1)
  end
  return pct(p.mode, tf, p.lap, 0)
end
function P.SetProgress(x, v)
  local r = resolve(x)
  if not r then return end
  if v < 0 then v = 0 end
  if v > 100 then v = 100 end
  if r.camera then
    if cam then cam.p = v * (ONE / 100); camPlace() end
    return
  end
  local p = r.path
  local function setAgent(a)
    if p.n < 2 then return end
    a.idx = v * (p.n - 1) / 100
    if a.on then moveAgent(p, a) end
  end
  if r.agent then setAgent(r.agent); return end
  if #p.groups > 0 and not warned[p.name] then
    warned[p.name] = true
    Debug.Log("Path.SetProgress: '" .. p.name .. "' moves its followers by animation, which the PS1 cannot jump; they carry on")
  end
  for _, a in ipairs(p.ag) do setAgent(a) end
end
function P.RideCamera(x, face)
  local r = resolve(x)
  if not r or not r.path then return end
  local p = r.path
  if p.n == 0 then
    Debug.Log("Path.RideCamera: '" .. p.name .. "' has no camera samples (export with a script that calls RideCamera)")
    return
  end
  Camera.FollowPsxPlayer(false)
  cam = { path = p, p = 0, on = true, face = face ~= false, v = vec(), r = vec() }
  camPlace()
end
function P.StopCamera()
  if not cam then return end
  cam = nil
  Camera.FollowPsxPlayer(true)
end
)LUA";
    b.libraryPath = "<paths>/library.lua";
    b.files[b.libraryPath] = L;

    // The host's tick, after the host's own script.
    Object* hostObj = const_cast<Object*>(host->fo->object);
    b.hostUserScript = hostObj->script ? hostObj->script->lua : "";
    b.hostPath = "<paths>/host:" + hostObj->name + ".lua";
    b.files[b.hostPath] = R"LUA(
local __path_onCreate, __path_onUpdate = onCreate, onUpdate
function onCreate(self)
  Path._create()
  if __path_onCreate then return __path_onCreate(self) end
end
function onUpdate(self, dt)
  Path._tick(dt)
  if __path_onUpdate then return __path_onUpdate(self, dt) end
end
)LUA";
    if (!hostObj->script) hostObj->script.emplace();
    hostObj->script->lua = b.hostPath;
    return b;
}

}  // namespace splash
