// examples/vk/vk_background.h — camera rays and cube binding for the fullscreen background.
#pragma once

#include "../common/example_math.h"
#include "vk_render.h"

namespace kiln::vkx {

/// The sky shaders' push constant block `Sky`: the camera basis, right and up scaled by the half
/// extents of the view at distance 1, and the cube's bindless slot.
struct SkyPush {
    f32 forward[4];
    f32 right[4];
    f32 up[4];
    u32 cubeSlot;
    u32 pad[3];
};
static_assert(sizeof(SkyPush) <= sizeof(DrawPush)); // shares the pipeline layout's push range

/// The caller checks that the cube is present. Bindless shaders read `cubeSlot`; shaders that
/// sample the scene set (vk_environment.h) ignore it.
inline void draw_background(Renderer* r, VkCommandBuffer cmd, ex::ViewRays const& rays, u32 cubeSlot = 0) {
    SkyPush const push{
        .forward  = {rays.forward.x, rays.forward.y, rays.forward.z, 0},
        .right    = {rays.right.x, rays.right.y, rays.right.z, 0},
        .up       = {rays.up.x, rays.up.y, rays.up.z, 0},
        .cubeSlot = cubeSlot,
        .pad      = {},
    };
    renderer_draw_fullscreen(r, cmd, {reinterpret_cast<u8 const*>(&push), sizeof push});
}

} // namespace kiln::vkx
