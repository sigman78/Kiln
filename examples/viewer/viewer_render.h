// examples/viewer/viewer_render.h — the viewer's render target and frame loop plumbing on
// raw Vulkan 1.4: a swapchain or an offscreen image, depth, frames in flight, the frame
// uniform buffer, and one pipeline per mesh::VertexLayout (spec §8 model A). What is drawn
// is up to main.cpp. Design: docs/design/viewer.md.
#pragma once

#include <kiln/alloc.h>
#include <kiln/containers.h>
#include <kiln/mesh.h>

#include "vk_adapter.h"
#include "vk_device.h"

namespace kiln::vkx {

inline constexpr u32 kFramesInFlight = 2;

/// Set 1, binding 0 of both shaders (std140).
struct FrameUniforms {
    f32 viewProj[16];
    f32 cameraPos[4];
    f32 lightDir[4]; ///< towards the light, xyz
    f32 tonemap[4];  ///< x: exposure multiplier (2^EV), y: 0 none, 1 ACES
};
static_assert(sizeof(FrameUniforms) == 112);

/// The shaders' push constant block `Draw` (std430 push constant layout).
struct DrawPush {
    f32 model[16];
    f32 posScale[4];
    f32 posBias[4];
    u32 baseColorSlot;
    u32 flags; ///< kDrawBaseColor | kDrawVertexColor
    u32 pad[2];
};
static_assert(sizeof(DrawPush) == 112); // mat4 + 2 vec4 + 4 uint, as laid out by the shaders

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

enum DrawFlags : u32 {
    kDrawBaseColor   = 1u << 0, ///< baseColorSlot holds a bindless slot
    kDrawVertexColor = 1u << 1, ///< the material uses vertex color
};

struct RendererDesc {
    Device const* device   = nullptr; ///< required
    VkAdapter* adapter     = nullptr; ///< required: bindless set, timeline, retire
    Allocator const* alloc = nullptr; ///< nullptr = default allocator
    /// Window mode: the surface to present to (the caller owns it). Null = offscreen.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    u32 width            = 1280; ///< offscreen image size; window mode asks framebufferSize
    u32 height           = 720;
    /// Window mode: the current framebuffer size in pixels (0 x 0 while minimized).
    void (*framebufferSize)(void* user, u32* width, u32* height) = nullptr;
    void* user                                                   = nullptr;
    /// A host with descriptor sets of its own (kiln-vk-basic) passes its SPIR-V and the layout of
    /// its material set: set 0 is then the frame set and set 1 the material set, which the host
    /// binds per draw (and before renderer_draw_sky). Empty: the viewer's bindless shaders, with
    /// the adapter's bindless set at 0 and the frame set at 1.
    Span<u32 const> meshVert = {}, meshFrag = {}, skyVert = {}, skyFrag = {};
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
};

/// One graphics pipeline per distinct vertex layout. Bindings 0..streamCount-1 are the
/// layout's streams; bindings streamCount.. feed the zero buffer to locations the layout lacks.
struct LayoutPipeline {
    mesh::VertexLayout layout{};
    VkPipeline pipeline = VK_NULL_HANDLE;
    u32 streamCount     = 0;
    u32 zeroBindings    = 0;
};

struct Renderer;

[[nodiscard]] Result<Renderer*> renderer_create(RendererDesc const& desc) noexcept;
/// Waits for the device to go idle first.
void renderer_destroy(Renderer* r) noexcept;

/// The pipeline for `layout`, created on first use. Null if creation failed (logged once).
[[nodiscard]] LayoutPipeline const* renderer_pipeline(Renderer* r, mesh::VertexLayout const& layout) noexcept;
[[nodiscard]] VkPipelineLayout renderer_pipeline_layout(Renderer* r) noexcept;
/// 64 zero bytes; bound at instance rate for vertex inputs a layout does not provide.
[[nodiscard]] VkBuffer renderer_zero_buffer(Renderer* r) noexcept;

/// Step 1 of a frame: waits for the frame slot's fence and retires the adapter's deferred
/// objects up to the frame that used the slot last. Pump kiln after this.
void renderer_wait_frame(Renderer* r) noexcept;
/// Step 2: acquires the target, begins the command buffer and dynamic rendering, binds the
/// bindless and frame sets. VK_NULL_HANDLE means skip this frame (minimized window).
[[nodiscard]] VkCommandBuffer renderer_begin(Renderer* r) noexcept;
[[nodiscard]] VkExtent2D renderer_extent(Renderer* r) noexcept;
/// This frame's uniform block, host-visible; write it between begin and end.
[[nodiscard]] FrameUniforms* renderer_uniforms(Renderer* r) noexcept;
/// Draws the cube in `push.cubeSlot` behind everything: call right after renderer_begin(),
/// before the meshes. No depth test and no depth write.
void renderer_draw_sky(Renderer* r, VkCommandBuffer cmd, SkyPush const& push) noexcept;
/// Step 3: ends rendering and submits, waiting on the adapter's upload watermark; presents in
/// window mode. `readback` (offscreen only) also copies the color image into the readback
/// buffer for renderer_read_back().
void renderer_end(Renderer* r, bool readback) noexcept;
/// Window mode: the framebuffer changed size; the swapchain is rebuilt before the next frame.
void renderer_resize(Renderer* r) noexcept;
void renderer_wait_idle(Renderer* r) noexcept;

/// After a renderer_end(r, true): waits for the device and returns the image as RGBA8,
/// top row first (the color image is BGRA; this swizzles).
[[nodiscard]] Status renderer_read_back(Renderer* r, Vec<u8>* rgba, u32* width, u32* height) noexcept;

[[nodiscard]] char const* renderer_color_format_name(Renderer* r) noexcept;

} // namespace kiln::vkx
