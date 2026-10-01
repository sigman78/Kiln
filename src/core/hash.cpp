#include "kiln/hash.h"

namespace kiln {

void Xxh64State::reset(u64 seed) {
    using namespace detail;
    seed_   = seed;
    acc_[0] = seed + kXxhP1 + kXxhP2;
    acc_[1] = seed + kXxhP2;
    acc_[2] = seed + 0;
    acc_[3] = seed - kXxhP1;
    bufLen_ = 0;
    total_  = 0;
}

void Xxh64State::update(void const* data, usize len) {
    using namespace detail;
    u8 const* p   = static_cast<u8 const*>(data);
    u8 const* end = p + len;
    total_ += len;

    if (bufLen_ + len < 32) {
        if (len) std::memcpy(buf_ + bufLen_, p, len);
        bufLen_ += u32(len);
        return;
    }
    if (bufLen_) {
        usize fill = 32 - bufLen_;
        std::memcpy(buf_ + bufLen_, p, fill);
        p += fill;
        acc_[0] = xxh_round(acc_[0], xxh_read64(buf_ + 0));
        acc_[1] = xxh_round(acc_[1], xxh_read64(buf_ + 8));
        acc_[2] = xxh_round(acc_[2], xxh_read64(buf_ + 16));
        acc_[3] = xxh_round(acc_[3], xxh_read64(buf_ + 24));
        bufLen_ = 0;
    }
    if (p + 32 <= end) {
        u8 const* const limit = end - 32;
        u64 v1 = acc_[0], v2 = acc_[1], v3 = acc_[2], v4 = acc_[3];
        do {
            v1 = xxh_round(v1, xxh_read64(p));
            v2 = xxh_round(v2, xxh_read64(p + 8));
            v3 = xxh_round(v3, xxh_read64(p + 16));
            v4 = xxh_round(v4, xxh_read64(p + 24));
            p += 32;
        } while (p <= limit);
        acc_[0] = v1;
        acc_[1] = v2;
        acc_[2] = v3;
        acc_[3] = v4;
    }
    if (p < end) {
        bufLen_ = u32(end - p);
        std::memcpy(buf_, p, bufLen_);
    }
}

u64 Xxh64State::digest() const {
    using namespace detail;
    u64 h;
    if (total_ >= 32) {
        h = std::rotl(acc_[0], 1) + std::rotl(acc_[1], 7) + std::rotl(acc_[2], 12) + std::rotl(acc_[3], 18);
        h = xxh_merge(h, acc_[0]);
        h = xxh_merge(h, acc_[1]);
        h = xxh_merge(h, acc_[2]);
        h = xxh_merge(h, acc_[3]);
    } else {
        h = seed_ + kXxhP5;
    }
    h += total_;

    u8 const* p   = buf_;
    u8 const* end = buf_ + bufLen_;
    while (end - p >= 8) {
        h ^= xxh_round(0, xxh_read64(p));
        h = std::rotl(h, 27) * kXxhP1 + kXxhP4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= u64(xxh_read32(p)) * kXxhP1;
        h = std::rotl(h, 23) * kXxhP2 + kXxhP3;
        p += 4;
    }
    while (p < end) {
        h ^= u64(*p) * kXxhP5;
        h = std::rotl(h, 11) * kXxhP1;
        ++p;
    }
    return xxh_avalanche(h);
}

} // namespace kiln
