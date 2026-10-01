// examples/vk/vk_adapter.cpp — the example kiln::Adapter on raw Vulkan 1.4
// (docs/design/viewer.md, "Adapter design"). Keep it readable: this file is the reference
// for adapters written outside kiln.
#include "vk_adapter.h"

#include <kiln/containers.h>
#include <kiln/log.h>

#include <atomic>
#include <cstring>
#include <mutex>

// kiln::Format values equal VkFormat (docs/design/adapter.md); this is the check that says so.
#define KILN_VK_SAME(name) static_assert(::kiln::u32(::kiln::Format::name) == VK_FORMAT_##name)
#define KILN_VK_BLOCK(name) static_assert(::kiln::u32(::kiln::Format::name) == VK_FORMAT_##name##_BLOCK)
static_assert(::kiln::u32(::kiln::Format::Undefined) == VK_FORMAT_UNDEFINED);
KILN_VK_SAME(R8_UNORM);
KILN_VK_SAME(R8_SNORM);
KILN_VK_SAME(R8G8_UNORM);
KILN_VK_SAME(R8G8_SNORM);
KILN_VK_SAME(R8G8B8_UNORM);
KILN_VK_SAME(R8G8B8_SRGB);
KILN_VK_SAME(R8G8B8A8_UNORM);
KILN_VK_SAME(R8G8B8A8_SNORM);
KILN_VK_SAME(R8G8B8A8_SRGB);
KILN_VK_SAME(R16_UNORM);
KILN_VK_SAME(R16_SNORM);
KILN_VK_SAME(R16_SFLOAT);
KILN_VK_SAME(R16G16_UNORM);
KILN_VK_SAME(R16G16_SNORM);
KILN_VK_SAME(R16G16_SFLOAT);
KILN_VK_SAME(R16G16B16A16_UNORM);
KILN_VK_SAME(R16G16B16A16_SNORM);
KILN_VK_SAME(R16G16B16A16_SFLOAT);
KILN_VK_SAME(R32_SFLOAT);
KILN_VK_SAME(R32G32_SFLOAT);
KILN_VK_SAME(R32G32B32_SFLOAT);
KILN_VK_SAME(R32G32B32A32_SFLOAT);
KILN_VK_BLOCK(BC1_RGB_UNORM);
KILN_VK_BLOCK(BC1_RGB_SRGB);
KILN_VK_BLOCK(BC1_RGBA_UNORM);
KILN_VK_BLOCK(BC1_RGBA_SRGB);
KILN_VK_BLOCK(BC2_UNORM);
KILN_VK_BLOCK(BC2_SRGB);
KILN_VK_BLOCK(BC3_UNORM);
KILN_VK_BLOCK(BC3_SRGB);
KILN_VK_BLOCK(BC4_UNORM);
KILN_VK_BLOCK(BC4_SNORM);
KILN_VK_BLOCK(BC5_UNORM);
KILN_VK_BLOCK(BC5_SNORM);
KILN_VK_BLOCK(BC6H_UFLOAT);
KILN_VK_BLOCK(BC6H_SFLOAT);
KILN_VK_BLOCK(BC7_UNORM);
KILN_VK_BLOCK(BC7_SRGB);
KILN_VK_BLOCK(ETC2_R8G8B8_UNORM);
KILN_VK_BLOCK(ETC2_R8G8B8_SRGB);
KILN_VK_BLOCK(ETC2_R8G8B8A1_UNORM);
KILN_VK_BLOCK(ETC2_R8G8B8A1_SRGB);
KILN_VK_BLOCK(ETC2_R8G8B8A8_UNORM);
KILN_VK_BLOCK(ETC2_R8G8B8A8_SRGB);
KILN_VK_BLOCK(EAC_R11_UNORM);
KILN_VK_BLOCK(EAC_R11_SNORM);
KILN_VK_BLOCK(EAC_R11G11_UNORM);
KILN_VK_BLOCK(EAC_R11G11_SNORM);
KILN_VK_BLOCK(ASTC_4x4_UNORM);
KILN_VK_BLOCK(ASTC_4x4_SRGB);
KILN_VK_BLOCK(ASTC_5x4_UNORM);
KILN_VK_BLOCK(ASTC_5x4_SRGB);
KILN_VK_BLOCK(ASTC_5x5_UNORM);
KILN_VK_BLOCK(ASTC_5x5_SRGB);
KILN_VK_BLOCK(ASTC_6x5_UNORM);
KILN_VK_BLOCK(ASTC_6x5_SRGB);
KILN_VK_BLOCK(ASTC_6x6_UNORM);
KILN_VK_BLOCK(ASTC_6x6_SRGB);
KILN_VK_BLOCK(ASTC_8x5_UNORM);
KILN_VK_BLOCK(ASTC_8x5_SRGB);
KILN_VK_BLOCK(ASTC_8x6_UNORM);
KILN_VK_BLOCK(ASTC_8x6_SRGB);
KILN_VK_BLOCK(ASTC_8x8_UNORM);
KILN_VK_BLOCK(ASTC_8x8_SRGB);
KILN_VK_BLOCK(ASTC_10x5_UNORM);
KILN_VK_BLOCK(ASTC_10x5_SRGB);
KILN_VK_BLOCK(ASTC_10x6_UNORM);
KILN_VK_BLOCK(ASTC_10x6_SRGB);
KILN_VK_BLOCK(ASTC_10x8_UNORM);
KILN_VK_BLOCK(ASTC_10x8_SRGB);
KILN_VK_BLOCK(ASTC_10x10_UNORM);
KILN_VK_BLOCK(ASTC_10x10_SRGB);
KILN_VK_BLOCK(ASTC_12x10_UNORM);
KILN_VK_BLOCK(ASTC_12x10_SRGB);
KILN_VK_BLOCK(ASTC_12x12_UNORM);
KILN_VK_BLOCK(ASTC_12x12_SRGB);
#undef KILN_VK_SAME
#undef KILN_VK_BLOCK

namespace kiln::vkx {

namespace {

constexpr u64 kRowPitchAlign     = 1;
constexpr u64 kOffsetAlign       = 16;
constexpr u64 kBufferOffsetAlign = 256;
constexpr CopyConstraints kCopyConstraints{.optimalRowPitchAlign = kRowPitchAlign,
                                           .optimalOffsetAlign   = kOffsetAlign,
                                           .bufferOffsetAlign    = kBufferOffsetAlign};
constexpr u32 kMaxLevels      = 32;
constexpr u32 kMaxSubmitBatch = 16;

enum class ObjectState : u8 {
    Free,      ///< on the free list
    Begun,     ///< begin_upload returned it; kiln is writing the staging bytes
    Recorded,  ///< commit_upload recorded it and gave it a value; submitted at once or on the next poll
    Submitted, ///< on the transfer queue; complete once the timeline reaches `value`
};

/// One image (+ view) or buffer. GpuObject::native is its 1-based index in VkAdapter::objects.
struct Object {
    VkImage image           = VK_NULL_HANDLE;
    VkImageView view        = VK_NULL_HANDLE;
    VkBuffer buffer         = VK_NULL_HANDLE;
    VkDeviceMemory memory   = VK_NULL_HANDLE;
    VkDeviceSize size       = 0; ///< bytes kiln wrote (the mesh payload size for buffers)
    VkDeviceAddress address = 0;
    u64 value               = 0; ///< timeline value that completes it, given at commit_upload (0 before)
    u64 stagingOffset       = 0;
    VkCommandBuffer cmd     = VK_NULL_HANDLE; ///< recorded, not yet submitted
    TextureDesc texture{};                    ///< copy of the upload's TextureDesc
    UploadKind kind   = UploadKind::MeshPayload;
    ObjectState state = ObjectState::Free;
    u32 generation    = 0; ///< bumped when the object is freed; part of the upload token
    u32 ringItem      = 0; ///< its staging reservation in VkAdapter::ring.items
};

/// A staging ring reservation, released when the timeline reaches `value`. Reservations are
/// made in begin order and get their value at commit, so `value` is 0 until then; a discarded one
/// gets kDiscarded and is released as soon as the entries before it are.
constexpr u64 kDiscarded = ~u64(0);
struct RingEntry {
    u64 value = 0;
    u64 end   = 0; ///< ring head after this reservation
};

struct CmdEntry {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    u64 value           = 0;
};

/// A fixed-capacity FIFO over a Vec that is sized once at create.
template <class T> struct Fifo {
    Vec<T> items;
    usize head  = 0;
    usize count = 0;

    void init(Allocator const* a, usize cap) {
        items.init(a, Tag::Payload);
        items.resize(cap);
    }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
    [[nodiscard]] T& front() noexcept { return items[head]; }
    void push(T const& v) noexcept {
        KILN_VERIFY(count < items.size());
        items[(head + count) % items.size()] = v;
        ++count;
    }
    void pop() noexcept {
        head = (head + 1) % items.size();
        --count;
    }
};

} // namespace

// One mutex guards the ring, the command pool, the object table and the descriptor set.
struct VkAdapter {
    AdapterDesc desc{};
    Allocator const* alloc = nullptr;
    Device const* dev      = nullptr;
    VkDevice device        = VK_NULL_HANDLE;
    bool concurrent        = false; ///< the graphics and transfer families differ
    /// The transfer queue is the graphics queue: submit on the pump thread (inside
    /// upload_status), which is the render thread, so the queue is never used by two threads.
    bool submitOnPoll        = false;
    bool transferHasGraphics = false; ///< vertex-input stages are legal in transfer barriers

    std::mutex mutex;

    // Staging ring: one host-visible, host-coherent buffer, mapped for its lifetime.
    VkBuffer staging          = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    u8* mapped                = nullptr;
    u64 ringSize              = 0;
    u64 ringHead              = 0;
    u64 ringTail              = 0;
    Fifo<RingEntry> ring;

    // Transfer submission: values are handed out in begin_upload and submitted in order.
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkCommandPool pool   = VK_NULL_HANDLE;
    u64 lastValue        = 0; ///< last value given at commit_upload; values follow commit order
    std::atomic<u64> submittedValue{0};
    Vec<u32> byValue; ///< object index for value v at [v % maxObjects]
    Fifo<CmdEntry> cmdsInFlight;
    Vec<VkCommandBuffer> freeCmds;

    // Objects.
    Vec<Object> objects;
    Vec<u32> freeObjects;
    u32 liveObjects = 0;

    // Bindless.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool       = VK_NULL_HANDLE;
    VkDescriptorSet set             = VK_NULL_HANDLE;
    VkSampler sampler               = VK_NULL_HANDLE;

    std::atomic<u64> watermark{0};
    ex::AdapterStats stats; ///< the counters; the rest is read from the tables
};

namespace {

[[nodiscard]] VkAdapter* self(void* user) noexcept { return static_cast<VkAdapter*>(user); }

[[nodiscard]] u64 timeline_value(VkAdapter* a) noexcept {
    u64 v = 0;
    VKX_CHECK(vkGetSemaphoreCounterValue(a->device, a->timeline, &v));
    return v;
}

/// An upload token: the object's generation and its 1-based index. Stable from begin_upload on,
/// unlike its timeline value, which commit_upload gives.
[[nodiscard]] u64 token_of(u32 index, Object const& o) noexcept {
    return (u64(o.generation) << 32) | (u64(index) + 1);
}
[[nodiscard]] Object* object_of_token(VkAdapter* a, u64 token) noexcept {
    u64 const index = (token & 0xFFFFFFFFu);
    if (index == 0 || index > a->objects.size()) return nullptr;
    Object& o = a->objects[usize(index - 1)];
    if (o.state == ObjectState::Free || o.generation != u32(token >> 32)) return nullptr;
    return &o;
}

[[nodiscard]] Object* object_of(VkAdapter* a, GpuObject obj) noexcept {
    if (obj.native == 0 || obj.native > a->objects.size()) return nullptr;
    Object& o = a->objects[usize(obj.native - 1)];
    return o.state == ObjectState::Free ? nullptr : &o;
}

// --- Staging ring -----------------------------------------------------------------------------

/// Releases every reservation whose upload has completed. Caller holds the mutex.
void ring_reclaim(VkAdapter* a, u64 completed) noexcept {
    while (!a->ring.empty() && a->ring.front().value != 0 &&
           (a->ring.front().value == kDiscarded || a->ring.front().value <= completed)) {
        a->ringTail = a->ring.front().end;
        a->ring.pop();
    }
    if (a->ring.empty()) a->ringHead = a->ringTail = 0;
}

/// Finds `n` bytes at `align`; false when the ring cannot fit them now. Caller holds the mutex.
bool ring_find(VkAdapter const* a, u64 n, u64 align, u64* start) noexcept {
    u64 const s = align_up(a->ringHead, align);
    // With live reservations and head <= tail the free space is [head, tail); otherwise it
    // is [head, size) followed by [0, tail).
    bool const wrapped = !a->ring.empty() && a->ringHead <= a->ringTail;
    if (wrapped) {
        if (s + n > a->ringTail) return false;
        *start = s;
        return true;
    }
    if (s + n <= a->ringSize) {
        *start = s;
        return true;
    }
    if (n > a->ringTail) return false;
    *start = 0;
    return true;
}

[[nodiscard]] u64 ring_used(VkAdapter const* a) noexcept {
    if (a->ring.empty()) return 0;
    return a->ringHead > a->ringTail ? a->ringHead - a->ringTail : a->ringSize - a->ringTail + a->ringHead;
}

// --- Command buffers and submission -----------------------------------------------------------

void cmds_reclaim(VkAdapter* a, u64 completed) noexcept {
    while (!a->cmdsInFlight.empty() && a->cmdsInFlight.front().value <= completed) {
        a->freeCmds.push_back(a->cmdsInFlight.front().cmd);
        a->cmdsInFlight.pop();
    }
}

VkCommandBuffer cmd_get(VkAdapter* a) noexcept {
    if (!a->freeCmds.empty()) {
        VkCommandBuffer const c = a->freeCmds.back();
        a->freeCmds.pop_back();
        return c;
    }
    VkCommandBufferAllocateInfo info{};
    info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool        = a->pool;
    info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer c       = VK_NULL_HANDLE;
    VKX_CHECK(vkAllocateCommandBuffers(a->device, &info, &c));
    return c;
}

/// Submits every recorded upload whose value is next in line, in value order, because a
/// timeline semaphore only accepts increasing signal values. Values are given at commit, so a
/// small upload committed first is never held back by a larger one still being written.
/// Caller holds the mutex.
void submit_ready(VkAdapter* a) noexcept {
    u64 next = a->submittedValue.load(std::memory_order_relaxed) + 1;
    while (next <= a->lastValue) {
        VkCommandBufferSubmitInfo cmds[kMaxSubmitBatch];
        VkSemaphoreSubmitInfo signals[kMaxSubmitBatch];
        VkSubmitInfo2 submits[kMaxSubmitBatch];
        u32 n = 0;
        for (; n < kMaxSubmitBatch && next <= a->lastValue; ++n, ++next) {
            Object& o = a->objects[a->byValue[usize(next % a->byValue.size())]];
            if (o.state != ObjectState::Recorded || o.value != next) break;
            cmds[n]                             = VkCommandBufferSubmitInfo{};
            cmds[n].sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cmds[n].commandBuffer               = o.cmd;
            signals[n]                          = VkSemaphoreSubmitInfo{};
            signals[n].sType                    = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signals[n].semaphore                = a->timeline;
            signals[n].value                    = next;
            signals[n].stageMask                = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            submits[n]                          = VkSubmitInfo2{};
            submits[n].sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            submits[n].commandBufferInfoCount   = 1;
            submits[n].pCommandBufferInfos      = &cmds[n];
            submits[n].signalSemaphoreInfoCount = 1;
            submits[n].pSignalSemaphoreInfos    = &signals[n];
            a->cmdsInFlight.push(CmdEntry{.cmd = o.cmd, .value = next});
            o.cmd   = VK_NULL_HANDLE;
            o.state = ObjectState::Submitted;
        }
        if (n == 0) return;
        VKX_CHECK(vkQueueSubmit2(a->dev->transferQueue, n, submits, VK_NULL_HANDLE));
        a->submittedValue.store(next - 1, std::memory_order_release);
    }
}

// --- Objects ----------------------------------------------------------------------------------

void object_free(VkAdapter* a, u32 index) noexcept {
    Object& o = a->objects[index];
    vkDestroyImageView(a->device, o.view, nullptr);
    vkDestroyImage(a->device, o.image, nullptr);
    vkDestroyBuffer(a->device, o.buffer, nullptr);
    vkFreeMemory(a->device, o.memory, nullptr);
    u32 const generation = o.generation + 1;
    o                    = Object{};
    o.generation         = generation; // tokens of the old object no longer match
    a->freeObjects.push_back(index);
    --a->liveObjects;
}

Status allocate_memory(VkAdapter* a, VkMemoryRequirements const& req, bool deviceAddress,
                       VkDeviceMemory* out) noexcept {
    u32 const type = find_memory_type(*a->dev, req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == kInvalid) return make_status(Code::Unsupported);
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.pNext           = deviceAddress ? &flags : nullptr;
    info.allocationSize  = req.size;
    info.memoryTypeIndex = type;
    VkResult const r     = vkAllocateMemory(a->device, &info, nullptr, out);
    if (r != VK_SUCCESS) {
        KILN_WARN("vk-adapter", "vkAllocateMemory(%llu bytes) failed: %s",
                  static_cast<unsigned long long>(req.size), result_name(r));
        return make_status(r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY
                               ? Code::OutOfMemory
                               : Code::Internal);
    }
    return kOk;
}

/// The sharing mode every buffer and image uses: concurrent over both families when they differ.
struct Sharing {
    VkSharingMode mode = VK_SHARING_MODE_EXCLUSIVE;
    u32 count          = 0;
    u32 families[2]    = {};
};

Sharing sharing(VkAdapter const* a) noexcept {
    Sharing s;
    if (a->concurrent) {
        s.mode        = VK_SHARING_MODE_CONCURRENT;
        s.count       = 2;
        s.families[0] = a->dev->graphicsFamily;
        s.families[1] = a->dev->transferFamily;
    }
    return s;
}

Status create_buffer(VkAdapter* a, Object& o, u64 size) noexcept {
    Sharing const sh = sharing(a);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = size;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode           = sh.mode;
    info.queueFamilyIndexCount = sh.count;
    info.pQueueFamilyIndices   = sh.families;
    VkResult r                 = vkCreateBuffer(a->device, &info, nullptr, &o.buffer);
    if (r != VK_SUCCESS) {
        KILN_WARN("vk-adapter", "vkCreateBuffer failed: %s", result_name(r));
        return make_status(Code::Internal);
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(a->device, o.buffer, &req);
    KILN_TRY(allocate_memory(a, req, true, &o.memory));
    VKX_CHECK(vkBindBufferMemory(a->device, o.buffer, o.memory, 0));
    VkBufferDeviceAddressInfo addr{};
    addr.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addr.buffer = o.buffer;
    o.address   = vkGetBufferDeviceAddress(a->device, &addr);
    return kOk;
}

Status create_image(VkAdapter* a, Object& o, TextureDesc const& t) noexcept {
    Sharing const sh = sharing(a);
    bool const is3d  = t.depth > 1;
    VkImageCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType   = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format      = static_cast<VkFormat>(t.format);
    info.extent      = {t.width, t.height, t.depth};
    info.mipLevels   = t.levels;
    info.arrayLayers = t.layers;
    info.samples     = VK_SAMPLE_COUNT_1_BIT;
    info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC only for adapter_read_texture.
    info.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode           = sh.mode;
    info.queueFamilyIndexCount = sh.count;
    info.pQueueFamilyIndices   = sh.families;
    info.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;
    if (t.shape == TextureShape::Cube) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkResult r = vkCreateImage(a->device, &info, nullptr, &o.image);
    if (r != VK_SUCCESS) {
        KILN_WARN("vk-adapter", "vkCreateImage(%s %ux%ux%u, %u levels, %u layers) failed: %s",
                  format_name(t.format), t.width, t.height, t.depth, t.levels, t.layers, result_name(r));
        return make_status(Code::Unsupported);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(a->device, o.image, &req);
    KILN_TRY(allocate_memory(a, req, false, &o.memory));
    VKX_CHECK(vkBindImageMemory(a->device, o.image, o.memory, 0));
    VkImageViewCreateInfo view{};
    view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image            = o.image;
    view.viewType         = is3d                             ? VK_IMAGE_VIEW_TYPE_3D
                            : t.shape == TextureShape::Cube  ? VK_IMAGE_VIEW_TYPE_CUBE
                            : t.shape == TextureShape::Array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                             : VK_IMAGE_VIEW_TYPE_2D;
    view.format           = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, t.layers};
    r                     = vkCreateImageView(a->device, &view, nullptr, &o.view);
    if (r != VK_SUCCESS) {
        KILN_WARN("vk-adapter", "vkCreateImageView failed: %s", result_name(r));
        return make_status(Code::Internal);
    }
    return kOk;
}

/// Binding 0 holds 2D views, 1 cube views, 2 array views; a slot index is shared by all three.
void write_slot(VkAdapter* a, u32 slot, VkImageView view, TextureShape shape) noexcept {
    VkDescriptorImageInfo image{};
    image.sampler     = a->sampler;
    image.imageView   = view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = a->set;
    write.dstBinding      = u32(shape);
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &image;
    vkUpdateDescriptorSets(a->device, 1, &write, 0, nullptr);
}

void record_texture(VkAdapter* a, VkCommandBuffer cmd, Object const& o) noexcept {
    TextureDesc const& t = o.texture;
    FormatInfo const* fi = format_info(t.format);
    u64 offsets[kMaxLevels];
    u64 pitches[kMaxLevels];
    (void)texture_level_layout(t, kCopyConstraints, offsets, pitches);

    VkImageSubresourceRange const all{VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, t.layers};
    VkImageMemoryBarrier2 toDst{};
    toDst.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    toDst.srcStageMask     = VK_PIPELINE_STAGE_2_NONE;
    toDst.srcAccessMask    = VK_ACCESS_2_NONE;
    toDst.dstStageMask     = VK_PIPELINE_STAGE_2_COPY_BIT;
    toDst.dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toDst.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image            = o.image;
    toDst.subresourceRange = all;
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &toDst;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy2 regions[kMaxLevels];
    for (u32 i = 0; i < t.levels; ++i) {
        u32 const w        = max(t.width >> i, 1u);
        u64 const rowBytes = format_row_bytes(t.format, w);
        // bufferRowLength is in texels; 0 means tightly packed, which is the case at pitch align 1.
        u32 const rowLength =
            pitches[i] == rowBytes ? 0u : u32(pitches[i] / fi->bytesPerBlock * fi->blockWidth);
        VkBufferImageCopy2& r = regions[i];
        r                     = VkBufferImageCopy2{};
        r.sType               = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
        r.bufferOffset        = o.stagingOffset + offsets[i];
        r.bufferRowLength     = rowLength;
        r.imageSubresource    = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, t.layers};
        r.imageExtent         = {w, max(t.height >> i, 1u), max(t.depth >> i, 1u)};
    }
    VkCopyBufferToImageInfo2 copy{};
    copy.sType          = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    copy.srcBuffer      = a->staging;
    copy.dstImage       = o.image;
    copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy.regionCount    = t.levels;
    copy.pRegions       = regions;
    vkCmdCopyBufferToImage2(cmd, &copy);

    // The graphics queue waits on the timeline semaphore before sampling, which orders
    // everything here; the barrier only has to make the copy available and change the layout.
    VkImageMemoryBarrier2 toRead = toDst;
    toRead.srcStageMask          = VK_PIPELINE_STAGE_2_COPY_BIT;
    toRead.srcAccessMask         = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toRead.dstStageMask          = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toRead.dstAccessMask         = VK_ACCESS_2_NONE;
    toRead.oldLayout             = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dep.pImageMemoryBarriers     = &toRead;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void record_mesh(VkAdapter* a, VkCommandBuffer cmd, Object const& o) noexcept {
    VkBufferCopy2 region{};
    region.sType     = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
    region.srcOffset = o.stagingOffset;
    region.dstOffset = 0;
    region.size      = o.size;
    VkCopyBufferInfo2 copy{};
    copy.sType       = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
    copy.srcBuffer   = a->staging;
    copy.dstBuffer   = o.buffer;
    copy.regionCount = 1;
    copy.pRegions    = &region;
    vkCmdCopyBuffer2(cmd, &copy);
    // Vertex-input stages exist only on graphics-capable families; a dedicated transfer
    // queue uses ALL_COMMANDS and relies on the semaphore wait on the graphics side.
    bool const gfx = a->transferHasGraphics;
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask =
        gfx ? VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT
            : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask =
        gfx ? VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT : VK_ACCESS_2_NONE;
    barrier.buffer = o.buffer;
    barrier.offset = 0;
    barrier.size   = VK_WHOLE_SIZE;
    VkDependencyInfo dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// --- The adapter entry points -----------------------------------------------------------------

bool vk_supports_format(void* user, Format f, FormatUsage usage) noexcept {
    VkAdapter* a         = self(user);
    FormatInfo const* fi = format_info(f);
    if (!fi) return false;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(a->dev->physical, static_cast<VkFormat>(f), &props);
    if (usage == FormatUsage::VertexBuffer)
        return (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    // Copy offsets are multiples of kOffsetAlign, which must also be a multiple of the block size.
    if (kOffsetAlign % fi->bytesPerBlock != 0) return false;
    VkFormatFeatureFlags const need =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (props.optimalTilingFeatures & need) == need;
}

void vk_copy_constraints(void* /*user*/, CopyConstraints* out) noexcept {
    out->optimalRowPitchAlign = kRowPitchAlign;
    out->optimalOffsetAlign   = kOffsetAlign;
    out->bufferOffsetAlign    = kBufferOffsetAlign;
}

Status vk_begin_upload(void* user, UploadDesc const& desc, UploadTarget* out) noexcept {
    VkAdapter* a         = self(user);
    bool const isTexture = desc.kind == UploadKind::TextureLevels;
    if (isTexture ? !desc.texture : !desc.mesh) return make_status(Code::InvalidArgument);
    u64 const n     = desc.size ? desc.size : 1;
    u64 const align = max<u64>(desc.alignment, kOffsetAlign);
    if (n > a->ringSize) {
        KILN_WARN("vk-adapter", "upload of %llu bytes exceeds the %llu-byte staging ring",
                  static_cast<unsigned long long>(n), static_cast<unsigned long long>(a->ringSize));
        return make_status(Code::Unsupported);
    }
    if (isTexture) {
        TextureDesc const& t = *desc.texture;
        u64 offsets[kMaxLevels];
        u64 pitches[kMaxLevels];
        bool const shapeOk = t.shape == TextureShape::Array ||
                             (t.shape == TextureShape::Cube && t.layers == 6 && t.width == t.height) ||
                             (t.shape == TextureShape::Tex2D && t.layers == 1);
        if (!shapeOk || t.levels == 0 || t.levels > kMaxLevels || t.layers == 0 ||
            (t.depth > 1 && t.layers > 1) ||
            texture_level_layout(t, kCopyConstraints, offsets, pitches) > desc.size) {
            KILN_WARN("vk-adapter", "texture %016llx: unsupported shape or layout",
                      static_cast<unsigned long long>(desc.id));
            return make_status(Code::Unsupported);
        }
    }

    std::lock_guard<std::mutex> lock(a->mutex);
    u64 const completed = timeline_value(a);
    ring_reclaim(a, completed);
    cmds_reclaim(a, completed);

    u64 start = 0;
    if (!ring_find(a, n, align, &start)) {
        ++a->stats.busyStaging;
        return make_status(Code::Busy);
    }
    if (a->freeObjects.empty()) {
        KILN_WARN("vk-adapter", "all %u objects are in use", a->desc.maxObjects);
        return make_status(Code::OutOfMemory);
    }
    u32 const index = a->freeObjects.back();
    Object& o       = a->objects[index];
    o               = Object{};
    o.kind          = desc.kind;
    o.state         = ObjectState::Begun;
    o.size          = desc.size;
    o.stagingOffset = start;
    a->freeObjects.pop_back();
    ++a->liveObjects;

    Status st;
    if (isTexture) {
        o.texture = *desc.texture;
        st        = create_image(a, o, o.texture);
    } else {
        st = create_buffer(a, o, desc.mesh->payloadDecodedSize);
    }
    if (st.failed()) {
        object_free(a, index);
        return st;
    }

    o.ringItem = u32((a->ring.head + a->ring.count) % a->ring.items.size());
    a->ring.push(RingEntry{.value = 0, .end = start + n});
    a->ringHead = start + n;

    out->dst           = a->mapped + start;
    out->rowPitchAlign = kRowPitchAlign;
    out->token         = token_of(index, o);
    out->object        = GpuObject{.native = u64(index) + 1, .slot = kInvalid, .kind = u32(desc.kind)};
    return kOk;
}

void vk_commit_upload(void* user, u64 token) noexcept {
    VkAdapter* a = self(user);
    std::lock_guard<std::mutex> lock(a->mutex);
    Object* const found = object_of_token(a, token);
    KILN_VERIFY(found && found->state == ObjectState::Begun);
    Object& o       = *found;
    u32 const index = u32((token & 0xFFFFFFFFu) - 1);

    VkCommandBuffer const cmd = cmd_get(a);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKX_CHECK(vkBeginCommandBuffer(cmd, &begin));
    if (o.kind == UploadKind::TextureLevels)
        record_texture(a, cmd, o);
    else
        record_mesh(a, cmd, o);
    VKX_CHECK(vkEndCommandBuffer(cmd));
    o.cmd                                          = cmd;
    o.state                                        = ObjectState::Recorded;
    o.value                                        = ++a->lastValue;
    a->byValue[usize(o.value % a->byValue.size())] = index;
    a->ring.items[o.ringItem].value                = o.value;
    a->stats.bytesCommitted += o.size;
    if (!a->submitOnPoll) submit_ready(a);
}

/// kiln's write failed: nothing was recorded, so the image or buffer goes now, and its ring range
/// once the ranges before it are reclaimed.
void vk_discard_upload(void* user, u64 token) noexcept {
    VkAdapter* a = self(user);
    std::lock_guard<std::mutex> lock(a->mutex);
    Object* const o = object_of_token(a, token);
    if (!o || o->state != ObjectState::Begun) return;
    a->ring.items[o->ringItem].value = kDiscarded;
    object_free(a, u32((token & 0xFFFFFFFFu) - 1));
    ++a->stats.uploadsDiscarded;
    ring_reclaim(a, timeline_value(a));
}

/// A submitted copy cannot fail short of a lost device (VKX_CHECK), so never Failed for a live token.
UploadStatus vk_upload_status(void* user, u64 token, Status* failure) noexcept {
    VkAdapter* a = self(user);
    std::lock_guard<std::mutex> lock(a->mutex);
    Object const* o = object_of_token(a, token);
    if (!o) { // not an upload of this adapter
        *failure = make_status(Code::InvalidArgument);
        return UploadStatus::Failed;
    }
    if (o->state == ObjectState::Recorded && a->submitOnPoll) submit_ready(a);
    if (o->state != ObjectState::Submitted || timeline_value(a) < o->value) return UploadStatus::Pending;
    if (o->value > a->watermark.load(std::memory_order_relaxed))
        a->watermark.store(o->value, std::memory_order_relaxed);
    return UploadStatus::Complete;
}

void vk_bind(void* user, u32 slot, GpuObject obj, TextureShape shape) noexcept {
    VkAdapter* a = self(user);
    std::lock_guard<std::mutex> lock(a->mutex);
    if (Object const* o = object_of(a, obj); o && o->view) write_slot(a, slot, o->view, shape);
}

void vk_destroy(void* user, GpuObject obj) noexcept {
    VkAdapter* a = self(user);
    std::lock_guard<std::mutex> lock(a->mutex);
    if (object_of(a, obj)) object_free(a, u32(obj.native - 1));
}

} // namespace

// --- Creation and teardown --------------------------------------------------------------------

namespace {

Status create_vulkan_objects(VkAdapter* a) noexcept {
    Device const& d = *a->dev;
    VkResult r;

    // Staging ring.
    VkBufferCreateInfo sbi{};
    sbi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sbi.size        = a->ringSize;
    sbi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ((r = vkCreateBuffer(a->device, &sbi, nullptr, &a->staging)) != VK_SUCCESS)
        return make_status(Code::Internal);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(a->device, a->staging, &req);
    VkMemoryPropertyFlags const hostFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    u32 const type = find_memory_type(d, req.memoryTypeBits, hostFlags);
    if (type == kInvalid) return make_status(Code::Unsupported);
    VkMemoryAllocateInfo smi{};
    smi.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    smi.allocationSize  = req.size;
    smi.memoryTypeIndex = type;
    if ((r = vkAllocateMemory(a->device, &smi, nullptr, &a->stagingMem)) != VK_SUCCESS)
        return make_status(Code::OutOfMemory);
    VKX_CHECK(vkBindBufferMemory(a->device, a->staging, a->stagingMem, 0));
    void* mapped = nullptr;
    VKX_CHECK(vkMapMemory(a->device, a->stagingMem, 0, VK_WHOLE_SIZE, 0, &mapped));
    a->mapped = static_cast<u8*>(mapped);

    // Timeline semaphore and the transfer command pool.
    VkSemaphoreTypeCreateInfo type2{};
    type2.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type2.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type2.initialValue  = 0;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &type2;
    VKX_CHECK(vkCreateSemaphore(a->device, &sci, nullptr, &a->timeline));
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = d.transferFamily;
    VKX_CHECK(vkCreateCommandPool(a->device, &pci, nullptr, &a->pool));

    // Bindless set: bindings 0, 1, 2 = sampler2D, samplerCube, sampler2DArray [maxSlots], one per
    // TextureShape, partially bound, update after bind.
    constexpr u32 kBindings = u32(TextureShape::Count);
    VkDescriptorSetLayoutBinding bindings[kBindings]{};
    VkDescriptorBindingFlags bindingFlags[kBindings]{};
    for (u32 i = 0; i < kBindings; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = a->desc.maxSlots;
        bindings[i].stageFlags      = VK_SHADER_STAGE_ALL_GRAPHICS;
        bindingFlags[i]             = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                          VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                          VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount  = kBindings;
    flagsInfo.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.pNext        = &flagsInfo;
    lci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = kBindings;
    lci.pBindings    = bindings;
    VKX_CHECK(vkCreateDescriptorSetLayout(a->device, &lci, nullptr, &a->setLayout));
    VkDescriptorPoolSize const poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        a->desc.maxSlots * kBindings};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpci.maxSets       = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &poolSize;
    VKX_CHECK(vkCreateDescriptorPool(a->device, &dpci, nullptr, &a->descPool));
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = a->descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &a->setLayout;
    VKX_CHECK(vkAllocateDescriptorSets(a->device, &dsai, &a->set));
    VkSamplerCreateInfo sampler{};
    sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter    = VK_FILTER_LINEAR;
    sampler.minFilter    = VK_FILTER_LINEAR;
    sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.minLod       = 0.0f;
    sampler.maxLod       = VK_LOD_CLAMP_NONE;
    VKX_CHECK(vkCreateSampler(a->device, &sampler, nullptr, &a->sampler));
    return kOk;
}

} // namespace

Result<VkAdapter*> adapter_create(AdapterDesc const& desc, Adapter* out) noexcept {
    if (!out || !desc.device || !desc.device->device || desc.maxSlots == 0 || desc.maxObjects == 0 ||
        desc.stagingBytes == 0)
        return make_status(Code::InvalidArgument);
    Device const& d = *desc.device;
    VkPhysicalDeviceVulkan12Properties p12{};
    p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(d.physical, &p2);
    // Three bindings of maxSlots each (2D, cube, array).
    u64 const sampled = u64(desc.maxSlots) * u32(TextureShape::Count);
    if (sampled > p12.maxPerStageDescriptorUpdateAfterBindSampledImages ||
        sampled > p12.maxDescriptorSetUpdateAfterBindSampledImages) {
        KILN_ERROR("vk-adapter",
                   "3 x maxSlots %u exceeds the device's update-after-bind sampled image limit %u",
                   desc.maxSlots, p12.maxPerStageDescriptorUpdateAfterBindSampledImages);
        return make_status(Code::Unsupported);
    }

    Allocator const* alloc = desc.alloc ? desc.alloc : default_allocator();
    VkAdapter* a           = new_object<VkAdapter>(alloc, Tag::Payload);
    a->desc                = desc;
    a->desc.alloc          = alloc;
    a->alloc               = alloc;
    a->dev                 = &d;
    a->device              = d.device;
    a->concurrent          = d.graphicsFamily != d.transferFamily;
    a->submitOnPoll        = d.transferQueue == d.graphicsQueue;
    a->ringSize            = desc.stagingBytes;

    u32 famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &famCount, nullptr);
    VkQueueFamilyProperties fam[32];
    famCount = famCount < 32 ? famCount : 32;
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &famCount, fam);
    a->transferHasGraphics =
        d.transferFamily < famCount && (fam[d.transferFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT);

    // Every table is sized here, so nothing regrows under the lock.
    u32 const maxObj = desc.maxObjects;
    a->objects.init(alloc, Tag::Payload);
    a->objects.resize(maxObj);
    a->freeObjects.init(alloc, Tag::Payload);
    a->freeObjects.reserve(maxObj);
    for (u32 i = maxObj; i > 0; --i)
        a->freeObjects.push_back(i - 1); // pop_back hands out index 0 first
    a->byValue.init(alloc, Tag::Payload);
    a->byValue.resize(maxObj);
    a->ring.init(alloc, maxObj);
    a->cmdsInFlight.init(alloc, maxObj);
    a->freeCmds.init(alloc, Tag::Payload);
    a->freeCmds.reserve(maxObj);

    Status const st = create_vulkan_objects(a);
    if (st.failed()) {
        KILN_ERROR("vk-adapter", "creating the staging ring or the bindless set failed (%s)",
                   code_name(st.code));
        adapter_destroy(a);
        return st;
    }

    *out                  = Adapter{};
    out->supports_format  = &vk_supports_format;
    out->copy_constraints = &vk_copy_constraints;
    out->begin_upload     = &vk_begin_upload;
    out->commit_upload    = &vk_commit_upload;
    out->discard_upload   = &vk_discard_upload;
    out->upload_status    = &vk_upload_status;
    out->bind             = desc.bindless ? &vk_bind : nullptr;
    out->destroy          = &vk_destroy;
    out->caps             = kSelfSubmitting | kCubeTextures | kArrayTextures | kMeshes;
    out->bindlessSlots    = desc.bindless ? desc.maxSlots : 0;
    out->user             = a;
    KILN_ASSERT(adapter_is_valid(*out));
    return a;
}

void adapter_destroy(VkAdapter* a) noexcept {
    if (!a) return;
    if (a->device) {
        {
            std::lock_guard<std::mutex> lock(a->mutex);
            submit_ready(a); // anything recorded but held back in submit-on-poll mode
        }
        if (a->dev->transferQueue) vkQueueWaitIdle(a->dev->transferQueue);
        for (u32 i = 0; i < a->objects.size(); ++i)
            if (a->objects[i].state != ObjectState::Free) object_free(a, i);
        vkDestroySampler(a->device, a->sampler, nullptr);
        vkDestroyDescriptorPool(a->device, a->descPool, nullptr);
        vkDestroyDescriptorSetLayout(a->device, a->setLayout, nullptr);
        vkDestroyCommandPool(a->device, a->pool, nullptr);
        vkDestroySemaphore(a->device, a->timeline, nullptr);
        vkDestroyBuffer(a->device, a->staging, nullptr);
        vkFreeMemory(a->device, a->stagingMem, nullptr);
    }
    delete_object(a->alloc, a, Tag::Payload);
}

VkDescriptorSetLayout adapter_set_layout(VkAdapter* a) noexcept { return a ? a->setLayout : VK_NULL_HANDLE; }
VkDescriptorSet adapter_descriptor_set(VkAdapter* a) noexcept { return a ? a->set : VK_NULL_HANDLE; }
VkSemaphore adapter_timeline(VkAdapter* a) noexcept { return a ? a->timeline : VK_NULL_HANDLE; }

u64 adapter_upload_watermark(VkAdapter* a) noexcept {
    return a ? a->watermark.load(std::memory_order_relaxed) : 0;
}

MeshPayload adapter_mesh(VkAdapter* a, GpuObject obj) noexcept {
    if (!a) return {};
    std::lock_guard<std::mutex> lock(a->mutex);
    Object const* o = object_of(a, obj);
    if (!o || !o->buffer) return {};
    return MeshPayload{.buffer = o->buffer, .offset = 0, .size = o->size, .address = o->address};
}

TextureView adapter_texture(VkAdapter* a, GpuObject obj) noexcept {
    if (!a) return {};
    std::lock_guard<std::mutex> lock(a->mutex);
    Object const* o = object_of(a, obj);
    if (!o || !o->view) return {};
    return TextureView{.view = o->view, .shape = o->texture.shape};
}

bool adapter_read_texture(void* user, GpuObject obj, TextureDesc const& desc, Vec<u8>* out) noexcept {
    VkAdapter* const a = static_cast<VkAdapter*>(user);
    std::lock_guard<std::mutex> lock(a->mutex);
    Object const* o = object_of(a, obj);
    if (!o || !o->image || desc.levels > kMaxLevels || o->value == 0 || timeline_value(a) < o->value)
        return false;

    VkBufferImageCopy2 regions[kMaxLevels];
    u64 total = 0;
    for (u32 i = 0; i < desc.levels; ++i) {
        u32 const w           = max(desc.width >> i, 1u);
        u32 const h           = max(desc.height >> i, 1u);
        VkBufferImageCopy2& r = regions[i];
        r                     = VkBufferImageCopy2{};
        r.sType               = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
        r.bufferOffset        = total;
        r.imageSubresource    = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, desc.layers};
        r.imageExtent         = {w, h, 1};
        total += format_image_bytes(desc.format, w, h) * desc.layers;
    }

    VkBufferCreateInfo bi{};
    bi.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size         = total;
    bi.usage        = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VKX_CHECK(vkCreateBuffer(a->device, &bi, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(a->device, buffer, &req);
    VkMemoryAllocateInfo mi{};
    mi.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex =
        find_memory_type(*a->dev, req.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    bool ok =
        mi.memoryTypeIndex != kInvalid && vkAllocateMemory(a->device, &mi, nullptr, &memory) == VK_SUCCESS;
    if (ok) VKX_CHECK(vkBindBufferMemory(a->device, buffer, memory, 0));

    VkCommandBufferAllocateInfo ci{};
    ci.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ci.commandPool        = a->pool;
    ci.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    VkCommandBuffer cmd   = VK_NULL_HANDLE;
    VkFence fence         = VK_NULL_HANDLE;
    if (ok) {
        VKX_CHECK(vkAllocateCommandBuffers(a->device, &ci, &cmd));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VKX_CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkImageMemoryBarrier2 toSrc{};
        toSrc.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toSrc.srcStageMask     = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toSrc.srcAccessMask    = VK_ACCESS_2_NONE;
        toSrc.dstStageMask     = VK_PIPELINE_STAGE_2_COPY_BIT;
        toSrc.dstAccessMask    = VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSrc.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.image            = o->image;
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.levels, 0, desc.layers};
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &toSrc;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkCopyImageToBufferInfo2 copy{};
        copy.sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
        copy.srcImage       = o->image;
        copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy.dstBuffer      = buffer;
        copy.regionCount    = desc.levels;
        copy.pRegions       = regions;
        vkCmdCopyImageToBuffer2(cmd, &copy);
        VkImageMemoryBarrier2 back = toSrc;
        back.srcStageMask          = VK_PIPELINE_STAGE_2_COPY_BIT;
        back.srcAccessMask         = VK_ACCESS_2_NONE;
        back.dstStageMask          = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        back.dstAccessMask         = VK_ACCESS_2_NONE;
        back.oldLayout             = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout             = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkBufferMemoryBarrier2 toHost{};
        toHost.sType                 = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        toHost.srcStageMask          = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask         = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask          = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask         = VK_ACCESS_2_HOST_READ_BIT;
        toHost.buffer                = buffer;
        toHost.size                  = VK_WHOLE_SIZE;
        dep.pImageMemoryBarriers     = &back;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers    = &toHost;
        vkCmdPipelineBarrier2(cmd, &dep);
        VKX_CHECK(vkEndCommandBuffer(cmd));

        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VKX_CHECK(vkCreateFence(a->device, &fi, nullptr, &fence));
        VkCommandBufferSubmitInfo cbi{};
        cbi.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 submit{};
        submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos    = &cbi;
        VKX_CHECK(vkQueueSubmit2(a->dev->transferQueue, 1, &submit, fence));
        ok = vkWaitForFences(a->device, 1, &fence, VK_TRUE, ~u64(0)) == VK_SUCCESS;
    }
    if (ok) {
        void* mapped = nullptr;
        VKX_CHECK(vkMapMemory(a->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        out->resize(usize(total));
        std::memcpy(out->data(), mapped, usize(total));
        vkUnmapMemory(a->device, memory);
    }
    vkDestroyFence(a->device, fence, nullptr);
    vkFreeCommandBuffers(a->device, a->pool, 1, &cmd);
    vkDestroyBuffer(a->device, buffer, nullptr);
    vkFreeMemory(a->device, memory, nullptr);
    return ok;
}

ex::AdapterStats adapter_stats(VkAdapter* a) noexcept {
    if (!a) return {};
    std::lock_guard<std::mutex> lock(a->mutex);
    u64 const completed = timeline_value(a);
    ring_reclaim(a, completed);
    cmds_reclaim(a, completed);
    ex::AdapterStats s = a->stats;
    for (Object const& o : a->objects) // an upload is pending until its value completes
        s.uploadsPending += o.state == ObjectState::Begun || o.state == ObjectState::Recorded ||
                            (o.state == ObjectState::Submitted && o.value > completed);
    s.liveObjects = a->liveObjects;
    s.stagingUsed = ring_used(a);
    s.stagingSize = a->ringSize;
    return s;
}

} // namespace kiln::vkx
