// basisu_bc7f.h: the bc7f real-time BC7 encoder of Basis Universal, as a standalone library.
// Modified by the kiln project from basisu_transcoder_internal.h; see NOTICE.
//
// Copyright (C) 2019-2026 Binomial LLC. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kiln_bc7f
{
	enum
	{
		// Low-level BC7 encoder configuration flags.
		cPackBC7FlagUse2SubsetsRGB = 1, // use mode 1/3 for RGB blocks
		cPackBC7FlagUse2SubsetsRGBA = 2, // use mode 7 for RGBA blocks

		cPackBC7FlagUse3SubsetsRGB = 4, // also use mode 0/2, cPackBC7FlagUse2SubsetsRGB MUST be enabled too

		cPackBC7FlagUseDualPlaneRGB = 8, // enable mode 4/5 usage for RGB blocks
		cPackBC7FlagUseDualPlaneRGBA = 16, // enable mode 4/5 usage for RGBA blocks

		cPackBC7FlagPBitOpt = 32, // enable to disable usage of fixed p-bits on some modes; slower
		cPackBC7FlagPBitOptMode6 = 64, // enable to disable usage of fixed p-bits on mode 6, alpha on fully opaque blocks may be 254 however; slower

		cPackBC7FlagUseTrivialMode6 = 128, // enable trivial fast mode 6 encoder on blocks with very low variances (highly recommended)

		cPackBC7FlagPartiallyAnalyticalRGB = 256, // partially analytical mode for RGB blocks, slower but higher quality, computes actual SSE's on complex blocks to resolve which mode to use vs. predictions
		cPackBC7FlagPartiallyAnalyticalRGBA = 512, // partially analytical mode for RGBA blocks, slower but higher quality, computes actual SSE's on complex blocks to resolve which mode to use vs. predictions

		// Non-analytical is really still partially analytical on the mode pairs (0 vs. 2, 1 vs 3, 4 vs. 5).
		cPackBC7FlagNonAnalyticalRGB = 1024, // very slow/brute force, totally abuses the encoder, MUST use with cPackBC7FlagPartiallyAnalyticalRGB flag
		cPackBC7FlagNonAnalyticalRGBA = 2048, // very slow/brute force, totally abuses the encoder, MUST use with cPackBC7FlagPartiallyAnalyticalRGBA flag

		cPackBC7FlagASTCCompatible = 4096, // disallow 2/3 partition patterns not in common with ASTC LDR 4x4

		cPackBC7FlagDisableRGBDualPlane = 8192, // don't use RGB dual plane channels (but A is OK)

		// Default to use first:

		// Decent analytical BC7 defaults
		cPackBC7FlagDefaultFastest = cPackBC7FlagUseTrivialMode6, // very weak particularly on alpha, mode 6 only for RGB/RGBA,

		// Mode 6 with pbits for RGB, Modes 4,5,6 for alpha.
		cPackBC7FlagDefaultFaster = cPackBC7FlagPBitOpt | cPackBC7FlagUseDualPlaneRGBA | cPackBC7FlagUseTrivialMode6,

		cPackBC7FlagDefaultFast = cPackBC7FlagUse2SubsetsRGB | cPackBC7FlagUse2SubsetsRGBA | cPackBC7FlagUseDualPlaneRGBA |
			cPackBC7FlagPBitOpt | cPackBC7FlagUseTrivialMode6,

		// Reasonable defaults, but not highest quality but fast
		cPackBC7FlagDefault = (cPackBC7FlagUse2SubsetsRGB | cPackBC7FlagUse2SubsetsRGBA | cPackBC7FlagUse3SubsetsRGB) |
			(cPackBC7FlagUseDualPlaneRGB | cPackBC7FlagUseDualPlaneRGBA) |
			(cPackBC7FlagPBitOpt | cPackBC7FlagPBitOptMode6) |
			cPackBC7FlagUseTrivialMode6,

		// Default partially analytical BC7 defaults (slower)
		cPackBC7FlagDefaultPartiallyAnalytical = cPackBC7FlagDefault | (cPackBC7FlagPartiallyAnalyticalRGB | cPackBC7FlagPartiallyAnalyticalRGBA),

		// Default non-analytical BC7 defaults (very slow). In reality the encoder is still analytical on the mode pairs, but at the highest level is non-analytical.
		cPackBC7FlagDefaultNonAnalytical = (cPackBC7FlagDefaultPartiallyAnalytical | (cPackBC7FlagNonAnalyticalRGB | cPackBC7FlagNonAnalyticalRGBA)) & ~cPackBC7FlagUseTrivialMode6
	};

	// Fills the encoder's tables. Call exactly once, before the first encode.
	// After it returns, any number of threads may encode at the same time.
	void init();

	// Encodes one 4x4 block of 16 RGBA8 texels (row by row, R G B A bytes) to 16 BC7 bytes.
	// Checks the block for alpha < 255 and picks the RGB or RGBA encoder.
	// Returns upstream's diagnostic squared error: 0 on the analytical paths and for solid blocks.
	uint32_t fast_pack_bc7_auto_rgba(uint8_t block[16], const uint8_t rgba[64], uint32_t flags);

} // namespace kiln_bc7f
