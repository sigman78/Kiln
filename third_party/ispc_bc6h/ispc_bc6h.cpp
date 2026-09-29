// Scalar C++ port of the BC6H encoder of the ISPC Texture Compressor (kernel.ispc, commit 79ddbc9,
// MIT, Copyright 2017 Intel Corporation; see LICENSE). Each ISPC program instance is plain C, so
// the port keeps its arithmetic exactly: float literals, truncations, x86 float-to-int conversion.
#include "ispc_bc6h.h"

#include <climits>
#include <cmath>
#include <initializer_list>

namespace ispc_bc6h {

namespace {

// ---------------------------------------------------------------------------
// Helpers with the original's x86 semantics
// ---------------------------------------------------------------------------

/// cvttss2si: truncates; NaN and out-of-range values give INT_MIN (C++ leaves them undefined).
int cvt(float f) noexcept {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return INT_MIN;
    return static_cast<int>(f);
}
/// minps / maxps operand order: the second operand wins ties and NaN.
float fmin_x86(float a, float b) noexcept { return a < b ? a : b; }
float fmax_x86(float a, float b) noexcept { return a > b ? a : b; }
int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
float sq(float v) noexcept { return v * v; }

// ---------------------------------------------------------------------------
// Format tables (the 32 two-region partitions BC6H shares with BC7)
// ---------------------------------------------------------------------------

constexpr uint32_t kPattern[32] = {
    0x50505050u, 0x40404040u, 0x54545454u, 0x54505040u, 0x50404000u, 0x55545450u, 0x55545040u, 0x54504000u,
    0x50400000u, 0x55555450u, 0x55544000u, 0x54400000u, 0x55555440u, 0x55550000u, 0x55555500u, 0x55000000u,
    0x55150100u, 0x00004054u, 0x15010000u, 0x00405054u, 0x00004050u, 0x15050100u, 0x05010000u, 0x40505054u,
    0x00404050u, 0x05010100u, 0x14141414u, 0x05141450u, 0x01155440u, 0x00555500u, 0x15014054u, 0x05414150u,
};
constexpr uint32_t kPatternMask[32] = {
    0xCCCC3333u, 0x88887777u, 0xEEEE1111u, 0xECC81337u, 0xC880377Fu, 0xFEEC0113u, 0xFEC80137u, 0xEC80137Fu,
    0xC80037FFu, 0xFFEC0013u, 0xFE80017Fu, 0xE80017FFu, 0xFFE80017u, 0xFF0000FFu, 0xFFF0000Fu, 0xF0000FFFu,
    0xF71008EFu, 0x008EFF71u, 0x71008EFFu, 0x08CEF731u, 0x008CFF73u, 0x73108CEFu, 0x3100CEFFu, 0x8CCE7331u,
    0x088CF773u, 0x3110CEEFu, 0x66669999u, 0x366CC993u, 0x17E8E817u, 0x0FF0F00Fu, 0x718E8E71u, 0x399CC663u,
};
constexpr int kSkip[32] = {
    0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
    0xf0, 0x20, 0x80, 0x20, 0x20, 0x80, 0x80, 0xf0, 0x20, 0x80, 0x20, 0x20, 0x80, 0x80, 0x20, 0x20,
};
constexpr int kUnquant3[8]  = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kUnquant4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

constexpr int kModePrefix[14] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15};
// The original's float table, truncated to int as its `uniform int span = span_table[mode]` does.
constexpr int kSpan[14]     = {921, 14745, 204, -1, -1, 1843, 3686, -1, -1, 65535, 65535, 7782, 1945, 6};
constexpr int kModeBits[14] = {10, 7, 11, -1, -1, 9, 8, -1, -1, 6, 10, 11, 12, 16};

int pattern_mask(int part_id, int j) noexcept {
    uint32_t const packed = kPatternMask[part_id];
    int const mask0       = int(packed & 0xFFFF);
    int const mask1       = int(packed >> 16);
    return j == 0 ? mask0 : mask1;
}

struct State {
    float block[64];
    float bestErr;
    uint32_t bestData[5];
    float rgbBounds[6];
    float maxSpan;
    int maxSpanIdx;
    int mode;
    int epb;
    int qbounds[8];
    Settings s;
};

// ---------------------------------------------------------------------------
// PCA
// ---------------------------------------------------------------------------

void compute_stats_masked(float stats[15], float const block[64], int mask) noexcept {
    for (int i = 0; i < 15; i++) stats[i] = 0;
    int shifted = mask << 1;
    for (int k = 0; k < 16; k++) {
        shifted >>= 1;
        int const flag = shifted & 1;
        float rgb[3]   = {block[k], block[k + 16], block[k + 32]};
        for (int p = 0; p < 3; p++) rgb[p] *= float(flag);
        stats[14] += float(flag);
        stats[10] += rgb[0];
        stats[11] += rgb[1];
        stats[12] += rgb[2];
        stats[0] += rgb[0] * rgb[0];
        stats[1] += rgb[0] * rgb[1];
        stats[2] += rgb[0] * rgb[2];
        stats[4] += rgb[1] * rgb[1];
        stats[5] += rgb[1] * rgb[2];
        stats[7] += rgb[2] * rgb[2];
    }
}

void covar_from_stats(float covar[10], float const stats[15]) noexcept {
    covar[0] = stats[0] - stats[10] * stats[10] / stats[14];
    covar[1] = stats[1] - stats[10] * stats[11] / stats[14];
    covar[2] = stats[2] - stats[10] * stats[12] / stats[14];
    covar[4] = stats[4] - stats[11] * stats[11] / stats[14];
    covar[5] = stats[5] - stats[11] * stats[12] / stats[14];
    covar[7] = stats[7] - stats[12] * stats[12] / stats[14];
}

void ssymv3(float a[4], float const covar[10], float const b[4]) noexcept {
    a[0] = covar[0] * b[0] + covar[1] * b[1] + covar[2] * b[2];
    a[1] = covar[1] * b[0] + covar[4] * b[1] + covar[5] * b[2];
    a[2] = covar[2] * b[0] + covar[5] * b[1] + covar[7] * b[2];
}

void compute_axis(float axis[4], float const covar[10], int iterations) noexcept {
    float vec[4] = {1, 1, 1, 1};
    for (int i = 0; i < iterations; i++) {
        ssymv3(axis, covar, vec);
        for (int p = 0; p < 3; p++) vec[p] = axis[p];
        if (i % 2 == 1) {
            float normSq = 0;
            for (int p = 0; p < 3; p++) normSq += axis[p] * axis[p];
            float const rnorm = 1.0f / std::sqrt(normSq);
            for (int p = 0; p < 3; p++) vec[p] *= rnorm;
        }
    }
    for (int p = 0; p < 3; p++) axis[p] = vec[p];
}

void block_segment_core(float ep[8], float const block[64], int mask) noexcept {
    float stats[15];
    compute_stats_masked(stats, block, mask);
    float covar[10] = {};
    covar_from_stats(covar, stats);
    float dc[4] = {};
    for (int p = 0; p < 3; p++) dc[p] = stats[10 + p] / stats[14];
    float const invVar = 1.0f / (256 * 256);
    for (int k = 0; k < 10; k++) covar[k] *= invVar;
    float const eps = sq(0.001f);
    covar[0] += eps;
    covar[4] += eps;
    covar[7] += eps;
    covar[9] += eps;
    float axis[4] = {};
    compute_axis(axis, covar, 8);

    float ext[2] = {INFINITY, -INFINITY};
    int shifted  = mask << 1;
    for (int k = 0; k < 16; k++) {
        shifted >>= 1;
        if ((shifted & 1) == 0) continue;
        float dot = 0;
        for (int p = 0; p < 3; p++) dot += axis[p] * (block[16 * p + k] - dc[p]);
        ext[0] = fmin_x86(ext[0], dot);
        ext[1] = fmax_x86(ext[1], dot);
    }
    if (ext[1] - ext[0] < 1.0f) {
        ext[0] -= 0.5f;
        ext[1] += 0.5f;
    }
    for (int i = 0; i < 2; i++)
        for (int p = 0; p < 3; p++) ep[4 * i + p] = ext[i] * axis[p] + dc[p];
}

float get_pca_bound(float covar[10]) noexcept {
    float const invVar = 1.0f / (256 * 256);
    for (int k = 0; k < 10; k++) covar[k] *= invVar;
    float const eps = sq(0.001f);
    covar[0] += eps;
    covar[4] += eps;
    covar[7] += eps;
    float axis[4] = {};
    compute_axis(axis, covar, 4);
    float vec[4] = {};
    ssymv3(vec, covar, axis);
    float sqSum = 0.0f;
    for (int p = 0; p < 3; p++) sqSum += sq(vec[p]);
    float const lambda = std::sqrt(sqSum);
    float bound        = covar[0] + covar[4] + covar[7];
    bound -= lambda;
    return fmax_x86(bound, 0.0f);
}

float block_pca_bound_split(float const block[64], int mask, float const fullStats[15]) noexcept {
    float stats[15];
    compute_stats_masked(stats, block, mask);
    float covar1[10] = {};
    covar_from_stats(covar1, stats);
    for (int i = 0; i < 15; i++) stats[i] = fullStats[i] - stats[i];
    float covar2[10] = {};
    covar_from_stats(covar2, stats);
    float bound = 0.0f;
    bound += get_pca_bound(covar1);
    bound += get_pca_bound(covar2);
    return std::sqrt(bound) * 256;
}

// ---------------------------------------------------------------------------
// Quantization and refinement
// ---------------------------------------------------------------------------

float block_quant(uint32_t qblock[2], float const block[64], int bits, float const ep[], uint32_t pattern) noexcept {
    float totalErr       = 0;
    int const* unquant   = bits == 3 ? kUnquant3 : kUnquant4;
    int const levels     = 1 << bits;
    qblock[0] = qblock[1] = 0;
    int shifted           = int(pattern);
    for (int k = 0; k < 16; k++) {
        int const j = shifted & 3;
        shifted >>= 2;
        float proj = 0;
        float div  = 0;
        for (int p = 0; p < 3; p++) {
            float const a = ep[8 * j + p];
            float const b = ep[8 * j + 4 + p];
            proj += (block[k + p * 16] - a) * (b - a);
            div += sq(b - a);
        }
        proj /= div;
        int q1 = cvt(proj * float(levels) + 0.5f);
        q1     = clampi(q1, 1, levels - 1);

        float err0   = 0;
        float err1   = 0;
        int const w0 = unquant[q1 - 1];
        int const w1 = unquant[q1];
        for (int p = 0; p < 3; p++) {
            float const a    = ep[8 * j + p];
            float const b    = ep[8 * j + 4 + p];
            float const dec0 = float(cvt((float(64 - w0) * a + float(w0) * b + 32.0f) / 64.0f));
            float const dec1 = float(cvt((float(64 - w1) * a + float(w1) * b + 32.0f) / 64.0f));
            err0 += sq(dec0 - block[k + p * 16]);
            err1 += sq(dec1 - block[k + p * 16]);
        }
        int bestErr = cvt(err1); // the original truncates the error to int here
        int bestQ   = q1;
        if (err0 < err1) {
            bestErr = cvt(err0);
            bestQ   = q1 - 1;
        }
        qblock[k / 8] += uint32_t(bestQ) << (4 * (k % 8));
        totalErr += float(bestErr);
    }
    return totalErr;
}

void opt_endpoints(float ep[8], float const block[64], int bits, uint32_t const qblock[2], int mask) noexcept {
    int const levels = 1 << bits;
    float atb1[4]    = {0, 0, 0, 0};
    float sumQ       = 0;
    float sumQQ      = 0;
    float sum[5]     = {0, 0, 0, 0, 0};
    int shifted      = mask << 1;
    for (int k1 = 0; k1 < 2; k1++) {
        uint32_t qbits = qblock[k1];
        for (int k2 = 0; k2 < 8; k2++) {
            int const k   = k1 * 8 + k2;
            float const q = float(int(qbits & 15));
            qbits >>= 4;
            shifted >>= 1;
            if ((shifted & 1) == 0) continue;
            int const x = cvt(float(levels - 1) - q);
            sumQ += q;
            sumQQ += q * q;
            sum[4] += 1;
            for (int p = 0; p < 3; p++) sum[p] += block[k + p * 16];
            for (int p = 0; p < 3; p++) atb1[p] += float(x) * block[k + p * 16];
        }
    }
    float atb2[4];
    for (int p = 0; p < 3; p++) atb2[p] = float(levels - 1) * sum[p] - atb1[p];

    float const cxx   = sum[4] * sq(float(levels - 1)) - float(2 * (levels - 1)) * sumQ + sumQQ;
    float const cyy   = sumQQ;
    float const cxy   = float(levels - 1) * sumQ - sumQQ;
    float const scale = float(levels - 1) / (cxx * cyy - cxy * cxy);
    for (int p = 0; p < 3; p++) {
        ep[0 + p] = (atb1[p] * cyy - atb2[p] * cxy) * scale;
        ep[4 + p] = (atb2[p] * cxx - atb1[p] * cxy) * scale;
    }
    if (std::fabs(cxx * cyy - cxy * cxy) < 0.001f) {
        for (int p = 0; p < 3; p++) {
            ep[0 + p] = sum[p] / sum[4];
            ep[4 + p] = ep[0 + p];
        }
    }
}

void partial_sort_list(int list[], int length, int partialCount) noexcept {
    for (int k = 0; k < partialCount; k++) {
        int bestIdx   = k;
        int bestValue = list[k];
        for (int i = k + 1; i < length; i++) {
            if (bestValue > list[i]) {
                bestValue = list[i];
                bestIdx   = i;
            }
        }
        list[bestIdx] = list[k];
        list[k]       = bestValue;
    }
}

int unpack_to_uf16(uint32_t v, int bits) noexcept {
    if (bits >= 15) return int(v);
    if (v == 0) return 0;
    if (v == (1u << bits) - 1) return 0xFFFF;
    return int((v * 2 + 1) << (15 - bits));
}

void ep_quant_bc6h(int qep[], float const ep[], int bits, int pairs) noexcept {
    int const levels = 1 << bits;
    for (int i = 0; i < 8 * pairs; i++) {
        int const v = cvt(ep[i] / (256 * 256.0f - 1) * float(levels - 1) + 0.5f);
        qep[i]      = clampi(v, 0, levels - 1);
    }
}

void ep_quant_dequant_bc6h(State const& st, int qep[], float ep[], int pairs) noexcept {
    ep_quant_bc6h(qep, ep, st.epb, pairs);
    for (int i = 0; i < 2 * pairs; i++)
        for (int p = 0; p < 3; p++) qep[i * 4 + p] = clampi(qep[i * 4 + p], st.qbounds[p], st.qbounds[4 + p]);
    for (int i = 0; i < 8 * pairs; i++) ep[i] = float(unpack_to_uf16(uint32_t(qep[i]), st.epb));
}

// ---------------------------------------------------------------------------
// Bitstream coding
// ---------------------------------------------------------------------------

int bit_at(int v, int pos) noexcept { return (v >> pos) & 1; }

uint32_t reverse_bits(uint32_t v, int bits) noexcept {
    if (bits == 2) return (v >> 1) + (v & 1) * 2;
    v = (v & 0x5555) * 2 + ((v >> 1) & 0x5555); // bits == 6
    return (v >> 4) + ((v >> 2) & 3) * 4 + (v & 3) * 16;
}

void put_bits(uint32_t data[5], int* pos, int bits, int v) noexcept {
    data[*pos / 32] |= uint32_t(v) << (*pos % 32);
    if (*pos % 32 + bits > 32) data[*pos / 32 + 1] |= uint32_t(v) >> (32 - *pos % 32);
    *pos += bits;
}

void bc6h_pack(uint32_t packed[4], int const qep[], int mode) noexcept {
    if (mode == 0) {
        int pred[16] = {};
        for (int p = 0; p < 3; p++) {
            pred[p]      = qep[p];
            pred[4 + p]  = (qep[4 + p] - qep[p]) & 31;
            pred[8 + p]  = (qep[8 + p] - qep[p]) & 31;
            pred[12 + p] = (qep[12 + p] - qep[p]) & 31;
        }
        uint32_t pq[10] = {};
        pq[4]           = uint32_t(pred[4] + (pred[8 + 1] & 15) * 64);
        pq[5]           = uint32_t(pred[5] + (pred[12 + 1] & 15) * 64);
        pq[6]           = uint32_t(pred[6] + (pred[8 + 2] & 15) * 64);
        pq[4] += uint32_t(bit_at(pred[12 + 1], 4) << 5);
        pq[5] += uint32_t(bit_at(pred[12 + 2], 0) << 5);
        pq[6] += uint32_t(bit_at(pred[12 + 2], 1) << 5);
        pq[8] = uint32_t(pred[8] + bit_at(pred[12 + 2], 2) * 32);
        pq[9] = uint32_t(pred[12] + bit_at(pred[12 + 2], 3) * 32);

        packed[0] = uint32_t(kModePrefix[0]);
        packed[0] += uint32_t(bit_at(pred[8 + 1], 4) << 2);
        packed[0] += uint32_t(bit_at(pred[8 + 2], 4) << 3);
        packed[0] += uint32_t(bit_at(pred[12 + 2], 4) << 4);
        packed[1] = uint32_t((pred[2] << 20) + (pred[1] << 10) + pred[0]);
        packed[2] = (pq[6] << 20) + (pq[5] << 10) + pq[4];
        packed[3] = (pq[9] << 6) + pq[8];
    } else if (mode == 1) {
        int pred[16] = {};
        for (int p = 0; p < 3; p++) {
            pred[p]      = qep[p];
            pred[4 + p]  = (qep[4 + p] - qep[p]) & 63;
            pred[8 + p]  = (qep[8 + p] - qep[p]) & 63;
            pred[12 + p] = (qep[12 + p] - qep[p]) & 63;
        }
        uint32_t pq[8] = {};
        pq[0]          = uint32_t(pred[0]);
        pq[0] += uint32_t(bit_at(pred[12 + 2], 0) << 7);
        pq[0] += uint32_t(bit_at(pred[12 + 2], 1) << 8);
        pq[0] += uint32_t(bit_at(pred[8 + 2], 4) << 9);
        pq[1] = uint32_t(pred[1]);
        pq[1] += uint32_t(bit_at(pred[8 + 2], 5) << 7);
        pq[1] += uint32_t(bit_at(pred[12 + 2], 2) << 8);
        pq[1] += uint32_t(bit_at(pred[8 + 1], 4) << 9);
        pq[2] = uint32_t(pred[2]);
        pq[2] += uint32_t(bit_at(pred[12 + 2], 3) << 7);
        pq[2] += uint32_t(bit_at(pred[12 + 2], 5) << 8);
        pq[2] += uint32_t(bit_at(pred[12 + 2], 4) << 9);
        pq[4] = uint32_t(pred[4] + (pred[8 + 1] & 15) * 64);
        pq[5] = uint32_t(pred[5] + (pred[12 + 1] & 15) * 64);
        pq[6] = uint32_t(pred[6] + (pred[8 + 2] & 15) * 64);

        packed[0] = uint32_t(kModePrefix[1]);
        packed[0] += uint32_t(bit_at(pred[8 + 1], 5) << 2);
        packed[0] += uint32_t(bit_at(pred[12 + 1], 4) << 3);
        packed[0] += uint32_t(bit_at(pred[12 + 1], 5) << 4);
        packed[1] = (pq[2] << 20) + (pq[1] << 10) + pq[0];
        packed[2] = (pq[6] << 20) + (pq[5] << 10) + pq[4];
        packed[3] = uint32_t((pred[12] << 6) + pred[8]);
    } else if (mode == 2 || mode == 3 || mode == 4) {
        int dq[16] = {};
        for (int p = 0; p < 3; p++) {
            int const mask = p == mode - 2 ? 31 : 15;
            dq[p]          = qep[p];
            dq[4 + p]      = (qep[4 + p] - qep[p]) & mask;
            dq[8 + p]      = (qep[8 + p] - qep[p]) & mask;
            dq[12 + p]     = (qep[12 + p] - qep[p]) & mask;
        }
        uint32_t pq[10] = {};
        pq[0]           = uint32_t(dq[0] & 1023);
        pq[1]           = uint32_t(dq[1] & 1023);
        pq[2]           = uint32_t(dq[2] & 1023);
        pq[4]           = uint32_t(dq[4] + (dq[8 + 1] & 15) * 64);
        pq[5]           = uint32_t(dq[5] + (dq[12 + 1] & 15) * 64);
        pq[6]           = uint32_t(dq[6] + (dq[8 + 2] & 15) * 64);
        pq[8]           = uint32_t(dq[8]);
        pq[9]           = uint32_t(dq[12]);
        if (mode == 2) {
            packed[0] = uint32_t(kModePrefix[2]);
            pq[5] += uint32_t(bit_at(dq[0 + 1], 10) << 4);
            pq[6] += uint32_t(bit_at(dq[0 + 2], 10) << 4);
            pq[4] += uint32_t(bit_at(dq[0 + 0], 10) << 5);
            pq[5] += uint32_t(bit_at(dq[12 + 2], 0) << 5);
            pq[6] += uint32_t(bit_at(dq[12 + 2], 1) << 5);
            pq[8] += uint32_t(bit_at(dq[12 + 2], 2) << 5);
            pq[9] += uint32_t(bit_at(dq[12 + 2], 3) << 5);
        }
        if (mode == 3) {
            packed[0] = uint32_t(kModePrefix[3]);
            pq[4] += uint32_t(bit_at(dq[0 + 0], 10) << 4);
            pq[6] += uint32_t(bit_at(dq[0 + 2], 10) << 4);
            pq[8] += uint32_t(bit_at(dq[12 + 2], 0) << 4);
            pq[9] += uint32_t(bit_at(dq[8 + 1], 4) << 4);
            pq[4] += uint32_t(bit_at(dq[12 + 1], 4) << 5);
            pq[5] += uint32_t(bit_at(dq[0 + 1], 10) << 5);
            pq[6] += uint32_t(bit_at(dq[12 + 2], 1) << 5);
            pq[8] += uint32_t(bit_at(dq[12 + 2], 2) << 5);
            pq[9] += uint32_t(bit_at(dq[12 + 2], 3) << 5);
        }
        if (mode == 4) {
            packed[0] = uint32_t(kModePrefix[4]);
            pq[4] += uint32_t(bit_at(dq[0 + 0], 10) << 4);
            pq[5] += uint32_t(bit_at(dq[0 + 1], 10) << 4);
            pq[8] += uint32_t(bit_at(dq[12 + 2], 1) << 4);
            pq[9] += uint32_t(bit_at(dq[12 + 2], 4) << 4);
            pq[4] += uint32_t(bit_at(dq[8 + 2], 4) << 5);
            pq[5] += uint32_t(bit_at(dq[12 + 2], 0) << 5);
            pq[6] += uint32_t(bit_at(dq[0 + 2], 10) << 5);
            pq[8] += uint32_t(bit_at(dq[12 + 2], 2) << 5);
            pq[9] += uint32_t(bit_at(dq[12 + 2], 3) << 5);
        }
        packed[1] = (pq[2] << 20) + (pq[1] << 10) + pq[0];
        packed[2] = (pq[6] << 20) + (pq[5] << 10) + pq[4];
        packed[3] = (pq[9] << 6) + pq[8];
    } else if (mode == 5) {
        int dq[16] = {};
        for (int p = 0; p < 3; p++) {
            dq[p]      = qep[p];
            dq[4 + p]  = (qep[4 + p] - qep[p]) & 31;
            dq[8 + p]  = (qep[8 + p] - qep[p]) & 31;
            dq[12 + p] = (qep[12 + p] - qep[p]) & 31;
        }
        uint32_t pq[10] = {};
        pq[0]           = uint32_t(dq[0]);
        pq[1]           = uint32_t(dq[1]);
        pq[2]           = uint32_t(dq[2]);
        pq[4]           = uint32_t(dq[4] + (dq[8 + 1] & 15) * 64);
        pq[5]           = uint32_t(dq[5] + (dq[12 + 1] & 15) * 64);
        pq[6]           = uint32_t(dq[6] + (dq[8 + 2] & 15) * 64);
        pq[8]           = uint32_t(dq[8]);
        pq[9]           = uint32_t(dq[12]);
        pq[0] += uint32_t(bit_at(dq[8 + 2], 4) << 9);
        pq[1] += uint32_t(bit_at(dq[8 + 1], 4) << 9);
        pq[2] += uint32_t(bit_at(dq[12 + 2], 4) << 9);
        pq[4] += uint32_t(bit_at(dq[12 + 1], 4) << 5);
        pq[5] += uint32_t(bit_at(dq[12 + 2], 0) << 5);
        pq[6] += uint32_t(bit_at(dq[12 + 2], 1) << 5);
        pq[8] += uint32_t(bit_at(dq[12 + 2], 2) << 5);
        pq[9] += uint32_t(bit_at(dq[12 + 2], 3) << 5);
        packed[0] = uint32_t(kModePrefix[5]);
        packed[1] = (pq[2] << 20) + (pq[1] << 10) + pq[0];
        packed[2] = (pq[6] << 20) + (pq[5] << 10) + pq[4];
        packed[3] = (pq[9] << 6) + pq[8];
    } else if (mode == 6 || mode == 7 || mode == 8) {
        int dq[16] = {};
        for (int p = 0; p < 3; p++) {
            int const mask = p == mode - 6 ? 63 : 31;
            dq[p]          = qep[p];
            dq[4 + p]      = (qep[4 + p] - qep[p]) & mask;
            dq[8 + p]      = (qep[8 + p] - qep[p]) & mask;
            dq[12 + p]     = (qep[12 + p] - qep[p]) & mask;
        }
        uint32_t pq[10] = {};
        pq[0]           = uint32_t(dq[0]);
        pq[0] += uint32_t(bit_at(dq[8 + 2], 4) << 9);
        pq[1] = uint32_t(dq[1]);
        pq[1] += uint32_t(bit_at(dq[8 + 1], 4) << 9);
        pq[2] = uint32_t(dq[2]);
        pq[2] += uint32_t(bit_at(dq[12 + 2], 4) << 9);
        pq[4] = uint32_t(dq[4] + (dq[8 + 1] & 15) * 64);
        pq[5] = uint32_t(dq[5] + (dq[12 + 1] & 15) * 64);
        pq[6] = uint32_t(dq[6] + (dq[8 + 2] & 15) * 64);
        pq[8] = uint32_t(dq[8]);
        pq[9] = uint32_t(dq[12]);
        if (mode == 6) {
            packed[0] = uint32_t(kModePrefix[6]);
            pq[0] += uint32_t(bit_at(dq[12 + 1], 4) << 8);
            pq[1] += uint32_t(bit_at(dq[12 + 2], 2) << 8);
            pq[2] += uint32_t(bit_at(dq[12 + 2], 3) << 8);
            pq[5] += uint32_t(bit_at(dq[12 + 2], 0) << 5);
            pq[6] += uint32_t(bit_at(dq[12 + 2], 1) << 5);
        }
        if (mode == 7) {
            packed[0] = uint32_t(kModePrefix[7]);
            pq[0] += uint32_t(bit_at(dq[12 + 2], 0) << 8);
            pq[1] += uint32_t(bit_at(dq[8 + 1], 5) << 8);
            pq[2] += uint32_t(bit_at(dq[12 + 1], 5) << 8);
            pq[4] += uint32_t(bit_at(dq[12 + 1], 4) << 5);
            pq[6] += uint32_t(bit_at(dq[12 + 2], 1) << 5);
            pq[8] += uint32_t(bit_at(dq[12 + 2], 2) << 5);
            pq[9] += uint32_t(bit_at(dq[12 + 2], 3) << 5);
        }
        if (mode == 8) {
            packed[0] = uint32_t(kModePrefix[8]);
            pq[0] += uint32_t(bit_at(dq[12 + 2], 1) << 8);
            pq[1] += uint32_t(bit_at(dq[8 + 2], 5) << 8);
            pq[2] += uint32_t(bit_at(dq[12 + 2], 5) << 8);
            pq[4] += uint32_t(bit_at(dq[12 + 1], 4) << 5);
            pq[5] += uint32_t(bit_at(dq[12 + 2], 0) << 5);
            pq[8] += uint32_t(bit_at(dq[12 + 2], 2) << 5);
            pq[9] += uint32_t(bit_at(dq[12 + 2], 3) << 5);
        }
        packed[1] = (pq[2] << 20) + (pq[1] << 10) + pq[0];
        packed[2] = (pq[6] << 20) + (pq[5] << 10) + pq[4];
        packed[3] = (pq[9] << 6) + pq[8];
    } else if (mode == 9) {
        uint32_t pq[10] = {};
        pq[0]           = uint32_t(qep[0]);
        pq[0] += uint32_t(bit_at(qep[12 + 1], 4) << 6);
        pq[0] += uint32_t(bit_at(qep[12 + 2], 0) << 7);
        pq[0] += uint32_t(bit_at(qep[12 + 2], 1) << 8);
        pq[0] += uint32_t(bit_at(qep[8 + 2], 4) << 9);
        pq[1] = uint32_t(qep[1]);
        pq[1] += uint32_t(bit_at(qep[8 + 1], 5) << 6);
        pq[1] += uint32_t(bit_at(qep[8 + 2], 5) << 7);
        pq[1] += uint32_t(bit_at(qep[12 + 2], 2) << 8);
        pq[1] += uint32_t(bit_at(qep[8 + 1], 4) << 9);
        pq[2] = uint32_t(qep[2]);
        pq[2] += uint32_t(bit_at(qep[12 + 1], 5) << 6);
        pq[2] += uint32_t(bit_at(qep[12 + 2], 3) << 7);
        pq[2] += uint32_t(bit_at(qep[12 + 2], 5) << 8);
        pq[2] += uint32_t(bit_at(qep[12 + 2], 4) << 9);
        pq[4]     = uint32_t(qep[4] + (qep[8 + 1] & 15) * 64);
        pq[5]     = uint32_t(qep[5] + (qep[12 + 1] & 15) * 64);
        pq[6]     = uint32_t(qep[6] + (qep[8 + 2] & 15) * 64);
        packed[0] = uint32_t(kModePrefix[9]);
        packed[1] = (pq[2] << 20) + (pq[1] << 10) + pq[0];
        packed[2] = (pq[6] << 20) + (pq[5] << 10) + pq[4];
        packed[3] = uint32_t((qep[12] << 6) + qep[8]);
    } else if (mode == 10) {
        packed[0] = uint32_t(kModePrefix[10]);
        packed[1] = uint32_t((qep[2] << 20) + (qep[1] << 10) + qep[0]);
        packed[2] = uint32_t((qep[6] << 20) + (qep[5] << 10) + qep[4]);
    } else if (mode == 11 || mode == 12 || mode == 13) {
        int const deltaMask = mode == 11 ? 511 : mode == 12 ? 255 : 15;
        int dq[8]           = {};
        for (int p = 0; p < 3; p++) {
            dq[p]     = qep[p];
            dq[4 + p] = (qep[4 + p] - qep[p]) & deltaMask;
        }
        uint32_t pq[8] = {};
        for (int p = 0; p < 3; p++) {
            pq[p] = uint32_t(dq[p] & 1023);
            if (mode == 11) pq[4 + p] = uint32_t(dq[4 + p] + (dq[p] >> 10) * 512);
            if (mode == 12) pq[4 + p] = uint32_t(dq[4 + p]) + reverse_bits(uint32_t(dq[p] >> 10), 2) * 256;
            if (mode == 13) pq[4 + p] = uint32_t(dq[4 + p]) + reverse_bits(uint32_t(dq[p] >> 10), 6) * 16;
        }
        packed[0] = uint32_t(kModePrefix[mode]);
        packed[1] = (pq[2] << 20) + (pq[1] << 10) + pq[0];
        packed[2] = (pq[6] << 20) + (pq[5] << 10) + pq[4];
    }
}

void swap_ints(int u[], int v[], int n) noexcept {
    for (int i = 0; i < n; i++) {
        int const t = u[i];
        u[i]        = v[i];
        v[i]        = t;
    }
}

void code_qblock(uint32_t data[5], int* pos, uint32_t const qblock[2], int bits, int flips) noexcept {
    int const levels = 1 << bits;
    int flipsShifted = flips;
    for (int k1 = 0; k1 < 2; k1++) {
        uint32_t qbits = qblock[k1];
        for (int k2 = 0; k2 < 8; k2++) {
            int q = int(qbits & 15);
            if ((flipsShifted & 1) > 0) q = (levels - 1) - q;
            put_bits(data, pos, k1 == 0 && k2 == 0 ? bits - 1 : bits, q);
            qbits >>= 4;
            flipsShifted >>= 1;
        }
    }
}

void data_shl_1bit_from(uint32_t data[5], int from) noexcept {
    if (from < 96) {
        uint32_t const shifted = (data[2] >> 1) | (data[3] << 31);
        uint32_t const mask    = ((1u << (from - 64)) - 1) >> 1;
        data[2]                = (mask & data[2]) | (~mask & shifted);
        data[3]                = (data[3] >> 1) | (data[4] << 31);
        data[4]                = data[4] >> 1;
    } else if (from < 128) {
        uint32_t const shifted = (data[3] >> 1) | (data[4] << 31);
        uint32_t const mask    = ((1u << (from - 96)) - 1) >> 1;
        data[3]                = (mask & data[3]) | (~mask & shifted);
        data[4]                = data[4] >> 1;
    }
}

void code_2p(uint32_t data[5], int qep[], uint32_t qblock[2], int partId, int mode) noexcept {
    // BC7 mode 1 layout: 3-bit indices, two pairs; the anchor of each pair gets index < 4.
    int const skips[2] = {0, kSkip[partId] >> 4};
    int flips          = 0;
    for (int j = 0; j < 2; j++) {
        int const k0 = skips[j];
        int const q  = int((qblock[k0 >> 3] << (28 - (k0 & 7) * 4)) >> 28);
        if (q >= 4) {
            swap_ints(&qep[8 * j], &qep[8 * j + 4], 4);
            flips |= pattern_mask(partId, j);
        }
    }
    for (int k = 0; k < 5; k++) data[k] = 0;
    int pos            = 0;
    uint32_t packed[4] = {};
    bc6h_pack(packed, qep, mode);
    put_bits(data, &pos, 5, int(packed[0]));
    put_bits(data, &pos, 30, int(packed[1]));
    put_bits(data, &pos, 30, int(packed[2]));
    put_bits(data, &pos, 12, int(packed[3]));
    put_bits(data, &pos, 5, partId);
    code_qblock(data, &pos, qblock, 3, flips);
    data_shl_1bit_from(data, 128 + 1 - (15 - skips[1]) * 3);
}

void code_1p(uint32_t data[5], int qep[], uint32_t qblock[2], int mode) noexcept {
    if ((qblock[0] & 15) >= 8) {
        swap_ints(&qep[0], &qep[4], 4);
        for (int k = 0; k < 2; k++) qblock[k] = 0x11111111u * 15u - qblock[k];
    }
    for (int k = 0; k < 5; k++) data[k] = 0;
    int pos            = 0;
    uint32_t packed[4] = {};
    bc6h_pack(packed, qep, mode);
    put_bits(data, &pos, 5, int(packed[0]));
    put_bits(data, &pos, 30, int(packed[1]));
    put_bits(data, &pos, 30, int(packed[2]));
    code_qblock(data, &pos, qblock, 4, 0);
}

// ---------------------------------------------------------------------------
// Mode search
// ---------------------------------------------------------------------------

float enc_2p_part_fast(State const& st, int qep[24], uint32_t qblock[2], int partId) noexcept {
    float ep[16] = {};
    for (int j = 0; j < 2; j++) block_segment_core(&ep[j * 8], st.block, pattern_mask(partId, j));
    ep_quant_dequant_bc6h(st, qep, ep, 2);
    return block_quant(qblock, st.block, 3, ep, kPattern[partId]);
}

void enc_2p_list(State& st, int const partList[], int partCount) noexcept {
    if (partCount == 0) return;
    int bestQep[24]         = {};
    uint32_t bestQblock[2]  = {};
    int bestPartId          = -1;
    float bestErr           = INFINITY;
    for (int part = 0; part < partCount; part++) {
        int const partId  = partList[part] & 31;
        int qep[24]       = {};
        uint32_t qblock[2] = {};
        float const err   = enc_2p_part_fast(st, qep, qblock, partId);
        if (err < bestErr) {
            for (int i = 0; i < 16; i++) bestQep[i] = qep[i];
            bestQblock[0] = qblock[0];
            bestQblock[1] = qblock[1];
            bestPartId    = partId;
            bestErr       = err;
        }
    }
    if (bestPartId < 0) return; // only when every error is NaN

    for (int it = 0; it < st.s.refineIterations2p; it++) {
        float ep[24] = {};
        for (int j = 0; j < 2; j++) opt_endpoints(&ep[j * 8], st.block, 3, bestQblock, pattern_mask(bestPartId, j));
        int qep[24]        = {};
        uint32_t qblock[2] = {};
        ep_quant_dequant_bc6h(st, qep, ep, 2);
        float const err = block_quant(qblock, st.block, 3, ep, kPattern[bestPartId]);
        if (err < bestErr) {
            for (int i = 0; i < 16; i++) bestQep[i] = qep[i];
            bestQblock[0] = qblock[0];
            bestQblock[1] = qblock[1];
            bestErr       = err;
        }
    }
    if (bestErr < st.bestErr) {
        st.bestErr = bestErr;
        code_2p(st.bestData, bestQep, bestQblock, bestPartId, st.mode);
    }
}

void enc_2p(State& st) noexcept {
    float fullStats[15];
    compute_stats_masked(fullStats, st.block, -1);
    int partList[32];
    for (int part = 0; part < 32; part++) {
        int const bound = cvt(block_pca_bound_split(st.block, pattern_mask(part, 0), fullStats));
        partList[part]  = part + bound * 64;
    }
    partial_sort_list(partList, 32, st.s.fastSkipThreshold);
    enc_2p_list(st, partList, st.s.fastSkipThreshold);
}

void enc_1p(State& st) noexcept {
    float ep[8] = {};
    block_segment_core(ep, st.block, -1);
    int qep[8] = {};
    ep_quant_dequant_bc6h(st, qep, ep, 1);
    uint32_t qblock[2] = {};
    float err          = block_quant(qblock, st.block, 4, ep, 0);
    for (int i = 0; i < st.s.refineIterations1p; i++) {
        opt_endpoints(ep, st.block, 4, qblock, -1);
        ep_quant_dequant_bc6h(st, qep, ep, 1);
        err = block_quant(qblock, st.block, 4, ep, 0);
    }
    if (err < st.bestErr) {
        st.bestErr = err;
        code_1p(st.bestData, qep, qblock, st.mode);
    }
}

void compute_qbounds(State& st, float const rgbSpan[3]) noexcept {
    float bounds[8] = {};
    for (int p = 0; p < 3; p++) {
        float const middle = (st.rgbBounds[p] + st.rgbBounds[3 + p]) / 2;
        bounds[p]          = middle - rgbSpan[p] / 2;
        bounds[4 + p]      = middle + rgbSpan[p] / 2;
    }
    ep_quant_bc6h(st.qbounds, bounds, st.epb, 1);
}

/// Sets up `mode` if the block's span fits it (with `margin`); with `enc`, also encodes.
void test_mode(State& st, int mode, bool enc, float margin) noexcept {
    float const span = float(kSpan[mode]);
    if (st.maxSpan * margin > span) return;
    st.epb = kModeBits[mode];
    if (mode >= 10) {
        st.mode             = mode;
        float const rgb[3] = {span, span, span};
        compute_qbounds(st, rgb);
        if (enc) enc_1p(st);
    } else if (mode <= 1 || mode == 5 || mode == 9) {
        st.mode             = mode;
        float const rgb[3] = {span, span, span};
        compute_qbounds(st, rgb);
        if (enc) enc_2p(st);
    } else {
        st.mode      = mode + st.maxSpanIdx;
        float rgb[3] = {span, span, span};
        rgb[st.maxSpanIdx] *= 2;
        compute_qbounds(st, rgb);
        if (enc) enc_2p(st);
    }
}

void setup(State& st) noexcept {
    for (int p = 0; p < 3; p++) {
        st.rgbBounds[p]     = 0xFFFF;
        st.rgbBounds[3 + p] = 0;
    }
    for (int p = 0; p < 3; p++)
        for (int k = 0; k < 16; k++) {
            float& v            = st.block[p * 16 + k];
            v                   = (v / 31) * 64; // half bits to the uf16 scale
            st.rgbBounds[p]     = fmin_x86(st.rgbBounds[p], v);
            st.rgbBounds[3 + p] = fmax_x86(st.rgbBounds[3 + p], v);
        }
    st.maxSpan    = 0;
    st.maxSpanIdx = 0;
    for (int p = 0; p < 3; p++) {
        float const span = st.rgbBounds[3 + p] - st.rgbBounds[p];
        if (span > st.maxSpan) {
            st.maxSpanIdx = p;
            st.maxSpan    = span;
        }
    }
}

} // namespace

void encode_block(uint8_t out[16], uint16_t const rgb[48], Settings const& s) noexcept {
    State st{};
    st.s = s;
    for (int k = 0; k < 16; k++)
        for (int p = 0; p < 3; p++) st.block[16 * p + k] = float(rgb[k * 3 + p]);
    st.bestErr = INFINITY;
    setup(st);
    if (s.slowMode) {
        for (int mode : {0, 1, 2, 5, 6, 9, 10, 11, 12, 13}) test_mode(st, mode, true, 0);
    } else {
        if (s.fastSkipThreshold > 0) {
            test_mode(st, 9, false, 0);
            if (s.fastMode) test_mode(st, 1, false, 1);
            test_mode(st, 6, false, 1 / 1.2f);
            test_mode(st, 5, false, 1 / 1.2f);
            test_mode(st, 0, false, 1 / 1.2f);
            test_mode(st, 2, false, 1);
            enc_2p(st);
            if (!s.fastMode) test_mode(st, 1, true, 0);
        }
        test_mode(st, 10, false, 0);
        test_mode(st, 11, false, 1);
        test_mode(st, 12, false, 1);
        test_mode(st, 13, false, 1);
        enc_1p(st);
    }
    for (int k = 0; k < 4; k++) {
        out[k * 4 + 0] = uint8_t(st.bestData[k]);
        out[k * 4 + 1] = uint8_t(st.bestData[k] >> 8);
        out[k * 4 + 2] = uint8_t(st.bestData[k] >> 16);
        out[k * 4 + 3] = uint8_t(st.bestData[k] >> 24);
    }
}

} // namespace ispc_bc6h
