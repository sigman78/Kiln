#include "kiln_test.h"

#include "kiln/null_adapter.h"
#include "kiln/placeholders.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

using namespace kiln;

namespace {

KILN_TEST(NullAdapter, CreateProducesValidAdapter) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create({}, &adapter);
    KILN_REQUIRE(created.ok());
    KILN_REQUIRE(created.value() != nullptr);
    KILN_CHECK(adapter_is_valid(adapter));
    null_adapter_destroy(created.value());
}

KILN_TEST(NullAdapter, NonBindlessHasNoBind) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create(NullAdapterDesc{.bindlessSlots = 0}, &adapter);
    KILN_REQUIRE(created.ok());
    KILN_CHECK(adapter.bind == nullptr);
    KILN_CHECK_EQ(adapter.bindlessSlots, 0u);
    KILN_CHECK(adapter_is_valid(adapter));
    null_adapter_destroy(created.value());
}

KILN_TEST(NullAdapter, MeshUploadCycleWritesAndReadsBytes) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create({}, &adapter);
    KILN_REQUIRE(created.ok());
    NullAdapter* na = created.value();

    MeshPayloadDesc meshDesc{.payloadDecodedSize = 64, .payloadAlignment = 256, .indexSize = 4};
    UploadDesc desc{
        .id = 100, .kind = UploadKind::MeshPayload, .size = 64, .alignment = 256, .mesh = &meshDesc};

    UploadTarget target{};
    Status s = adapter.begin_upload(adapter.user, desc, &target);
    KILN_REQUIRE(s.ok());
    KILN_REQUIRE(target.dst != nullptr);
    KILN_CHECK(target.token != u64(0));

    u8* bytes = static_cast<u8*>(target.dst);
    for (u32 i = 0; i < 64; ++i)
        bytes[i] = u8(i);

    adapter.commit_upload(adapter.user, target.token);
    Status why;
    KILN_CHECK(adapter.upload_status(adapter.user, target.token, &why) == UploadStatus::Complete);

    Span<u8 const> payload = null_adapter_payload(na, target.object);
    KILN_REQUIRE_EQ(payload.size, usize(64));
    bool matches = true;
    for (u32 i = 0; i < 64; ++i)
        if (payload.data[i] != u8(i)) matches = false;
    KILN_CHECK(matches);

    null_adapter_destroy(na);
}

KILN_TEST(NullAdapter, TextureUploadCycleWritesAndReadsBytes) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create({}, &adapter);
    KILN_REQUIRE(created.ok());
    NullAdapter* na = created.value();

    TextureDesc texDesc{.format = Format::R8G8B8A8_UNORM, .width = 2, .height = 2};
    UploadDesc desc{
        .id = 200, .kind = UploadKind::TextureLevels, .size = 16, .alignment = 16, .texture = &texDesc};

    UploadTarget target{};
    KILN_REQUIRE(adapter.begin_upload(adapter.user, desc, &target).ok());
    KILN_REQUIRE(target.dst != nullptr);

    u8* bytes = static_cast<u8*>(target.dst);
    for (u32 i = 0; i < 16; ++i)
        bytes[i] = u8(0xA0 + i);

    adapter.commit_upload(adapter.user, target.token);
    Status why;
    KILN_CHECK(adapter.upload_status(adapter.user, target.token, &why) == UploadStatus::Complete);

    Span<u8 const> payload = null_adapter_payload(na, target.object);
    KILN_REQUIRE_EQ(payload.size, usize(16));
    bool matches = true;
    for (u32 i = 0; i < 16; ++i)
        if (payload.data[i] != u8(0xA0 + i)) matches = false;
    KILN_CHECK(matches);

    null_adapter_destroy(na);
}

KILN_TEST(NullAdapter, BusyEveryNReturnsBusyOnEverySecondCall) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create(NullAdapterDesc{.busyEveryN = 2}, &adapter);
    KILN_REQUIRE(created.ok());
    NullAdapter* na = created.value();

    MeshPayloadDesc meshDesc{.payloadDecodedSize = 16};
    UploadDesc ud{.id = 1, .kind = UploadKind::MeshPayload, .size = 16, .alignment = 16, .mesh = &meshDesc};

    u32 busyCount = 0;
    for (int i = 1; i <= 6; ++i) {
        UploadTarget target{};
        Status s = adapter.begin_upload(adapter.user, ud, &target);
        if (i % 2 == 0) {
            KILN_CHECK_EQ(s.code, Code::Busy);
            ++busyCount;
        } else {
            KILN_CHECK(s.ok());
            adapter.commit_upload(adapter.user, target.token);
        }
    }
    KILN_CHECK_EQ(busyCount, u32(3));

    NullAdapterStats stats = null_adapter_stats(na);
    KILN_CHECK_EQ(stats.beginUploads, u32(6));
    KILN_CHECK_EQ(stats.busyReturned, u32(3));

    null_adapter_destroy(na);
}

KILN_TEST(NullAdapter, FailEveryNReturnsUnsupportedOnEveryThirdCall) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create(NullAdapterDesc{.failEveryN = 3}, &adapter);
    KILN_REQUIRE(created.ok());
    NullAdapter* na = created.value();

    MeshPayloadDesc meshDesc{.payloadDecodedSize = 16};
    UploadDesc ud{.id = 1, .kind = UploadKind::MeshPayload, .size = 16, .alignment = 16, .mesh = &meshDesc};

    u32 failCount = 0;
    for (int i = 1; i <= 6; ++i) {
        UploadTarget target{};
        Status s = adapter.begin_upload(adapter.user, ud, &target);
        if (i % 3 == 0) {
            KILN_CHECK_EQ(s.code, Code::Unsupported);
            ++failCount;
        } else {
            KILN_CHECK(s.ok());
            adapter.commit_upload(adapter.user, target.token);
        }
    }
    KILN_CHECK_EQ(failCount, u32(2));

    null_adapter_destroy(na);
}

UploadTarget upload_rgba_1x1(Adapter const& adapter) {
    TextureDesc texDesc{.format = Format::R8G8B8A8_UNORM, .width = 1, .height = 1};
    UploadDesc ud{
        .id = 5, .kind = UploadKind::TextureLevels, .size = 4, .alignment = 16, .texture = &texDesc};
    UploadTarget target{};
    KILN_VERIFY(adapter.begin_upload(adapter.user, ud, &target).ok());
    std::memset(target.dst, 0xAB, 4);
    adapter.commit_upload(adapter.user, target.token);
    return target;
}

KILN_TEST(NullAdapter, BindShowsObjectInSlot) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create({.bindlessSlots = 8}, &adapter);
    KILN_REQUIRE(created.ok());
    NullAdapter* na = created.value();
    KILN_CHECK_EQ(adapter.bindlessSlots, 8u);
    KILN_CHECK(null_adapter_slot(na, 3).is_null());

    UploadTarget const t = upload_rgba_1x1(adapter);
    adapter.bind(adapter.user, 3, t.object, TextureShape::Tex2D);
    KILN_CHECK_EQ(null_adapter_slot(na, 3).native, t.object.native);
    KILN_CHECK_EQ(null_adapter_stats(na).binds, 1u);

    null_adapter_destroy(na);
}

KILN_TEST(NullAdapter, DestroyFreesPayload) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create({}, &adapter);
    KILN_REQUIRE(created.ok());
    NullAdapter* na = created.value();

    UploadTarget const target = upload_rgba_1x1(adapter);
    NullAdapterStats before   = null_adapter_stats(na);
    KILN_CHECK_EQ(null_adapter_payload(na, target.object).size, usize(4));

    adapter.destroy(adapter.user, target.object);
    KILN_CHECK(null_adapter_payload(na, target.object).empty());
    NullAdapterStats after = null_adapter_stats(na);
    KILN_CHECK_EQ(after.liveObjects + 1, before.liveObjects);
    KILN_CHECK_EQ(after.destroys, 1u);

    null_adapter_destroy(na);
}

KILN_TEST(NullAdapter, ConcurrentBeginCommitCyclesProduceDistinctCompletedTokens) {
    Adapter adapter{};
    Result<NullAdapter*> created = null_adapter_create(NullAdapterDesc{.maxObjects = 1024}, &adapter);
    KILN_REQUIRE(created.ok());
    NullAdapter* na = created.value();

    constexpr int kThreads   = 4;
    constexpr int kPerThread = 100;

    std::vector<u64> tokens[kThreads];
    std::thread workers[kThreads];
    MeshPayloadDesc meshDesc{.payloadDecodedSize = 8};

    for (int t = 0; t < kThreads; ++t) {
        workers[t] = std::thread([&adapter, &tokens, &meshDesc, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                UploadDesc ud{.id        = AssetId(1000 + t),
                              .kind      = UploadKind::MeshPayload,
                              .size      = 8,
                              .alignment = 16,
                              .mesh      = &meshDesc};
                UploadTarget target{};
                Status s = adapter.begin_upload(adapter.user, ud, &target);
                KILN_REQUIRE(s.ok());
                adapter.commit_upload(adapter.user, target.token);
                tokens[t].push_back(target.token);
            }
        });
    }
    for (auto& th : workers)
        th.join();

    std::vector<u64> all;
    for (int t = 0; t < kThreads; ++t) {
        KILN_CHECK_EQ(tokens[t].size(), usize(kPerThread));
        for (u64 tok : tokens[t])
            all.push_back(tok);
    }
    KILN_REQUIRE_EQ(all.size(), usize(kThreads * kPerThread));

    std::sort(all.begin(), all.end());
    bool distinct = true;
    for (usize i = 1; i < all.size(); ++i)
        if (all[i] == all[i - 1]) distinct = false;
    KILN_CHECK(distinct);

    bool allComplete = true;
    Status why;
    for (u64 tok : all)
        if (adapter.upload_status(adapter.user, tok, &why) != UploadStatus::Complete) allComplete = false;
    KILN_CHECK(allComplete);

    null_adapter_destroy(na);
}

KILN_TEST(Placeholders, BaseColorIsMidGreySrgb) {
    PlaceholderImage img = builtin_placeholder(TextureKind::BaseColor);
    KILN_CHECK(img.format == Format::R8G8B8A8_SRGB);
    KILN_REQUIRE_EQ(img.width, u32(1));
    KILN_REQUIRE_EQ(img.height, u32(1));
    KILN_REQUIRE_EQ(img.pixels.size, usize(4));
    KILN_CHECK_EQ(img.pixels[0], u8(128));
    KILN_CHECK_EQ(img.pixels[1], u8(128));
    KILN_CHECK_EQ(img.pixels[2], u8(128));
    KILN_CHECK_EQ(img.pixels[3], u8(255));
}

KILN_TEST(Placeholders, NormalIsFlatUnorm) {
    PlaceholderImage img = builtin_placeholder(TextureKind::Normal);
    KILN_CHECK(img.format == Format::R8G8B8A8_UNORM);
    KILN_REQUIRE_EQ(img.pixels.size, usize(4));
    KILN_CHECK_EQ(img.pixels[0], u8(128));
    KILN_CHECK_EQ(img.pixels[1], u8(128));
    KILN_CHECK_EQ(img.pixels[2], u8(255));
    KILN_CHECK_EQ(img.pixels[3], u8(255));
}

KILN_TEST(Placeholders, OrmIsAoRoughOneMetalZeroUnorm) {
    PlaceholderImage img = builtin_placeholder(TextureKind::Orm);
    KILN_CHECK(img.format == Format::R8G8B8A8_UNORM);
    KILN_REQUIRE_EQ(img.pixels.size, usize(4));
    KILN_CHECK_EQ(img.pixels[0], u8(255)); // AO = 1
    KILN_CHECK_EQ(img.pixels[1], u8(255)); // roughness = 1
    KILN_CHECK_EQ(img.pixels[2], u8(0));   // metallic = 0
    KILN_CHECK_EQ(img.pixels[3], u8(255));
}

KILN_TEST(Placeholders, EmissiveIsBlackSrgb) {
    PlaceholderImage img = builtin_placeholder(TextureKind::Emissive);
    KILN_CHECK(img.format == Format::R8G8B8A8_SRGB);
    KILN_REQUIRE_EQ(img.pixels.size, usize(4));
    KILN_CHECK_EQ(img.pixels[0], u8(0));
    KILN_CHECK_EQ(img.pixels[1], u8(0));
    KILN_CHECK_EQ(img.pixels[2], u8(0));
    KILN_CHECK_EQ(img.pixels[3], u8(255));
}

KILN_TEST(Placeholders, FailedCheckerAlternatesInTwoByTwoCells) {
    PlaceholderImage img = builtin_failed_placeholder();
    KILN_CHECK(img.format == Format::R8G8B8A8_SRGB);
    KILN_REQUIRE_EQ(img.width, u32(8));
    KILN_REQUIRE_EQ(img.height, u32(8));
    KILN_REQUIRE_EQ(img.pixels.size, usize(8 * 8 * 4));

    auto texelAt   = [&](u32 x, u32 y) { return img.pixels.data + (y * 8 + x) * 4; };
    auto isMagenta = [](u8 const* p) { return p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255; };
    auto isBlack   = [](u8 const* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255; };

    u8 const* c00 = texelAt(0, 0);
    KILN_CHECK(isMagenta(c00) || isBlack(c00));

    // Every texel inside a 2x2 cell matches the cell's color; the next cell
    // along either axis is the other color (checkerboard, 2x2 cells).
    for (u32 cy = 0; cy < 4; ++cy) {
        for (u32 cx = 0; cx < 4; ++cx) {
            u8 const* ref   = texelAt(cx * 2, cy * 2);
            bool refMagenta = isMagenta(ref);
            KILN_CHECK(refMagenta || isBlack(ref));
            for (u32 dy = 0; dy < 2; ++dy)
                for (u32 dx = 0; dx < 2; ++dx) {
                    u8 const* p = texelAt(cx * 2 + dx, cy * 2 + dy);
                    KILN_CHECK(refMagenta ? isMagenta(p) : isBlack(p));
                }
            if (cx + 1 < 4) {
                u8 const* right = texelAt((cx + 1) * 2, cy * 2);
                KILN_CHECK(isMagenta(right) != refMagenta);
            }
            if (cy + 1 < 4) {
                u8 const* below = texelAt(cx * 2, (cy + 1) * 2);
                KILN_CHECK(isMagenta(below) != refMagenta);
            }
        }
    }
}

KILN_TEST(Placeholders, AssetIdsMatchReservedRange) {
    KILN_CHECK_EQ(placeholder_asset_id(TextureKind::BaseColor), AssetId(1));
    KILN_CHECK_EQ(placeholder_asset_id(TextureKind::Normal), AssetId(2));
    KILN_CHECK_EQ(placeholder_asset_id(TextureKind::Orm), AssetId(3));
    KILN_CHECK_EQ(placeholder_asset_id(TextureKind::Emissive), AssetId(4));
    KILN_CHECK_EQ(failed_placeholder_id(), AssetId(13));
    KILN_CHECK_EQ(placeholder_asset_id(TextureKind::BaseColor, TextureShape::Cube), AssetId(5));
    KILN_CHECK_EQ(placeholder_asset_id(TextureKind::Emissive, TextureShape::Array), AssetId(12));
    KILN_CHECK_EQ(failed_placeholder_id(TextureShape::Array), AssetId(15));
}

} // namespace
