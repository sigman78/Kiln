// examples/viewer/viewer_math.h — the few vector and matrix helpers the viewer needs.
// Matrices are column-major (m[column * 4 + row]) like GLSL; right-handed, +Y up, Vulkan
// clip space (depth 0..1, +Y down in NDC).
#pragma once

#include <kiln/core.h>

#include <cmath>

namespace kiln::vkx {

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
};

[[nodiscard]] inline Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] inline Vec3 operator*(Vec3 a, f32 s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] inline f32 dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] inline Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline f32 length(Vec3 a) noexcept { return std::sqrt(dot(a, a)); }
[[nodiscard]] inline Vec3 normalize(Vec3 a) noexcept {
    f32 const l = length(a);
    return l > 0 ? a * (1.0f / l) : a;
}

struct Mat4 {
    f32 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

[[nodiscard]] inline Mat4 operator*(Mat4 const& a, Mat4 const& b) noexcept {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i) {
            f32 s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + i] * b.m[c * 4 + k];
            r.m[c * 4 + i] = s;
        }
    return r;
}

[[nodiscard]] inline Mat4 translation(Vec3 t) noexcept {
    Mat4 r;
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

/// Rotation by the unit quaternion (x, y, z, w), then translation by t.
[[nodiscard]] inline Mat4 from_rt(f32 const t[3], f32 const q[4]) noexcept {
    f32 const x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 r;
    r.m[0]  = 1 - 2 * (y * y + z * z);
    r.m[1]  = 2 * (x * y + w * z);
    r.m[2]  = 2 * (x * z - w * y);
    r.m[4]  = 2 * (x * y - w * z);
    r.m[5]  = 1 - 2 * (x * x + z * z);
    r.m[6]  = 2 * (y * z + w * x);
    r.m[8]  = 2 * (x * z + w * y);
    r.m[9]  = 2 * (y * z - w * x);
    r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[12] = t[0];
    r.m[13] = t[1];
    r.m[14] = t[2];
    return r;
}

[[nodiscard]] inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    Vec3 const f = normalize(target - eye);
    Vec3 const s = normalize(cross(f, up));
    Vec3 const u = cross(s, f);
    Mat4 r;
    r.m[0]  = s.x;
    r.m[4]  = s.y;
    r.m[8]  = s.z;
    r.m[1]  = u.x;
    r.m[5]  = u.y;
    r.m[9]  = u.z;
    r.m[2]  = -f.x;
    r.m[6]  = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

/// Right-handed perspective into Vulkan clip space: depth 0 at `nearZ`, 1 at `farZ`, +Y down.
[[nodiscard]] inline Mat4 perspective(f32 fovyRadians, f32 aspect, f32 nearZ, f32 farZ) noexcept {
    f32 const f = 1.0f / std::tan(fovyRadians * 0.5f);
    Mat4 r;
    r.m[0]  = f / aspect;
    r.m[5]  = -f;
    r.m[10] = farZ / (nearZ - farZ);
    r.m[11] = -1;
    r.m[14] = nearZ * farZ / (nearZ - farZ);
    r.m[15] = 0;
    return r;
}

} // namespace kiln::vkx
