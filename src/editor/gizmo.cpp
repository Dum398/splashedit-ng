#include "editor/gizmo.hh"

#include <cmath>

namespace editor {

splash::Quat quatAxisAngle(splash::Vec3 axis, float radians) {
    float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (len < 1e-12f) return {};
    float s = std::sin(radians * 0.5f) / len;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f)};
}

splash::Quat quatMul(splash::Quat a, splash::Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

splash::Quat quatNormalize(splash::Quat q) {
    float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-12f) return {};
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

float screenAngle(float cx, float cy, float x, float y) { return std::atan2(y - cy, x - cx); }

float angleStep(float prev, float now) {
    float d = now - prev;
    while (d > kPi) d -= 2 * kPi;
    while (d <= -kPi) d += 2 * kPi;
    return d;
}

float snapTo(float v, float step) { return step > 0 ? std::round(v / step) * step : v; }

float rotateDragDegrees(float radians, bool snap) {
    float deg = radians * (180.0f / kPi);
    return snap ? snapTo(deg, kRotateSnapDeg) : deg;
}

float clampScale(float v, float reference) {
    float mag = std::fabs(v);
    // A value that crossed zero has collapsed: hold it at the minimum.
    if ((reference < 0) != (v < 0) && v != 0) mag = 0;
    if (!(mag >= kMinScale)) mag = kMinScale;  // also catches NaN
    return reference < 0 ? -mag : mag;
}

float scaleAxis(float start, float factor, bool snap) {
    float v = start * factor;
    if (snap) v = snapTo(v, kScaleSnap);
    return clampScale(v, start);
}

splash::Vec3 scaleUniform(splash::Vec3 start, float factor, bool snap) {
    if (snap) factor = snapTo(factor, kScaleSnap);
    return {clampScale(start.x * factor, start.x), clampScale(start.y * factor, start.y), clampScale(start.z * factor, start.z)};
}

static float& comp(splash::Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

splash::Vec3 scalePlane(splash::Vec3 start, int a, int b, float factor, bool snap) {
    const splash::Vec3 u = scaleUniform(start, factor, snap);
    splash::Vec3 out = start;
    comp(out, a) = u[a];
    comp(out, b) = u[b];
    return out;
}

splash::Vec3 lockedScale(splash::Vec3 start, int axis, float value) {
    const float s0 = start[axis];
    splash::Vec3 out = start;
    if (s0 == 0) {
        if (start.x == 0 && start.y == 0 && start.z == 0) return {value, value, value};
        comp(out, axis) = value;
        return out;
    }
    const float factor = value / s0;
    out = {start.x * factor, start.y * factor, start.z * factor};
    comp(out, axis) = value;  // exactly what was typed, not s0 * (value / s0)
    return out;
}

}  // namespace editor
