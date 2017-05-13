// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "base/basictypes.h"
#include "profiler/profiler.h"

#include "Common/ThreadPools.h"
#include "Common/ColorConv.h"
#include "Core/Config.h"
#include "Core/MemMap.h"
#include "Core/Reporting.h"
#include "GPU/GPUState.h"

#include "GPU/Common/TextureDecoder.h"
#include "GPU/Software/SoftGpu.h"
#include "GPU/Software/Rasterizer.h"

#include <algorithm>

#if defined(_M_SSE)
#include <emmintrin.h>
#endif

extern FormatBuffer fb;
extern FormatBuffer depthbuf;

extern u32 clut[4096];

namespace Rasterizer {

//static inline int orient2d(const DrawingCoords& v0, const DrawingCoords& v1, const DrawingCoords& v2)
static inline int orient2d(const ScreenCoords& v0, const ScreenCoords& v1, const ScreenCoords& v2)
{
	return ((int)v1.x-(int)v0.x)*((int)v2.y-(int)v0.y) - ((int)v1.y-(int)v0.y)*((int)v2.x-(int)v0.x);
}

static inline int orient2dIncX(int dY01)
{
	return dY01;
}

static inline int orient2dIncY(int dX01)
{
	return -dX01;
}

// Only OK on x64 where our stack is aligned
#if defined(_M_SSE) && !defined(_M_IX86)
static inline __m128 Interpolate(const __m128 &c0, const __m128 &c1, const __m128 &c2, int w0, int w1, int w2, float wsum) {
	__m128 v = _mm_mul_ps(c0, _mm_cvtepi32_ps(_mm_set1_epi32(w0)));
	v = _mm_add_ps(v, _mm_mul_ps(c1, _mm_cvtepi32_ps(_mm_set1_epi32(w1))));
	v = _mm_add_ps(v, _mm_mul_ps(c2, _mm_cvtepi32_ps(_mm_set1_epi32(w2))));
	return _mm_mul_ps(v, _mm_set_ps1(wsum));
}

static inline __m128i Interpolate(const __m128i &c0, const __m128i &c1, const __m128i &c2, int w0, int w1, int w2, float wsum) {
	return _mm_cvtps_epi32(Interpolate(_mm_cvtepi32_ps(c0), _mm_cvtepi32_ps(c1), _mm_cvtepi32_ps(c2), w0, w1, w2, wsum));
}
#endif

// NOTE: When not casting color0 and color1 to float vectors, this code suffers from severe overflow issues.
// Not sure if that should be regarded as a bug or if casting to float is a valid fix.

static inline Vec4<int> Interpolate(const Vec4<int> &c0, const Vec4<int> &c1, const Vec4<int> &c2, int w0, int w1, int w2, float wsum) {
#if defined(_M_SSE) && !defined(_M_IX86)
	return Vec4<int>(Interpolate(c0.ivec, c1.ivec, c2.ivec, w0, w1, w2, wsum));
#else
	return ((c0.Cast<float>() * w0 + c1.Cast<float>() * w1 + c2.Cast<float>() * w2) * wsum).Cast<int>();
#endif
}

static inline Vec3<int> Interpolate(const Vec3<int> &c0, const Vec3<int> &c1, const Vec3<int> &c2, int w0, int w1, int w2, float wsum) {
#if defined(_M_SSE) && !defined(_M_IX86)
	return Vec3<int>(Interpolate(c0.ivec, c1.ivec, c2.ivec, w0, w1, w2, wsum));
#else
	return ((c0.Cast<float>() * w0 + c1.Cast<float>() * w1 + c2.Cast<float>() * w2) * wsum).Cast<int>();
#endif
}

static inline Vec2<float> Interpolate(const Vec2<float> &c0, const Vec2<float> &c1, const Vec2<float> &c2, int w0, int w1, int w2, float wsum) {
#if defined(_M_SSE) && !defined(_M_IX86)
	return Vec2<float>(Interpolate(c0.vec, c1.vec, c2.vec, w0, w1, w2, wsum));
#else
	return (c0 * w0 + c1 * w1 + c2 * w2) * wsum;
#endif
}

static inline Vec4<float> Interpolate(const float &c0, const float &c1, const float &c2, const Vec4<float> &w0, const Vec4<float> &w1, const Vec4<float> &w2, const Vec4<float> &wsum_recip) {
#if defined(_M_SSE) && !defined(_M_IX86)
	__m128 v = _mm_mul_ps(w0.vec, _mm_set1_ps(c0));
	v = _mm_add_ps(v, _mm_mul_ps(w1.vec, _mm_set1_ps(c1)));
	v = _mm_add_ps(v, _mm_mul_ps(w2.vec, _mm_set1_ps(c2)));
	return _mm_mul_ps(v, wsum_recip.vec);
#else
	return (w0 * c0 + w1 * c1 + w2 * c2) * wsum_recip;
#endif
}

static inline Vec4<float> Interpolate(const float &c0, const float &c1, const float &c2, const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2, const Vec4<float> &wsum_recip) {
	return Interpolate(c0, c1, c2, w0.Cast<float>(), w1.Cast<float>(), w2.Cast<float>(), wsum_recip);
}

template <unsigned int texel_size_bits>
static inline int GetPixelDataOffset(unsigned int row_pitch_bytes, unsigned int u, unsigned int v)
{
	if (!gstate.isTextureSwizzled())
		return (v * (row_pitch_bytes * texel_size_bits >> 3)) + (u * texel_size_bits >> 3);

	const int tile_size_bits = 32;
	const int tiles_in_block_horizontal = 4;
	const int tiles_in_block_vertical = 8;

	int texels_per_tile = tile_size_bits / texel_size_bits;
	int tile_u = u / texels_per_tile;
	int tile_idx = (v % tiles_in_block_vertical) * (tiles_in_block_horizontal) +
	// TODO: not sure if the *texel_size_bits/8 factor is correct
					(v / tiles_in_block_vertical) * ((row_pitch_bytes*texel_size_bits/(tile_size_bits))*tiles_in_block_vertical) +
					(tile_u % tiles_in_block_horizontal) +
					(tile_u / tiles_in_block_horizontal) * (tiles_in_block_horizontal*tiles_in_block_vertical);

	return tile_idx * (tile_size_bits / 8) + ((u % texels_per_tile) * texel_size_bits) / 8;
}

static inline u32 LookupColor(unsigned int index, unsigned int level)
{
	const bool mipmapShareClut = gstate.isClutSharedForMipmaps();
	const int clutSharingOffset = mipmapShareClut ? 0 : level * 16;

	switch (gstate.getClutPaletteFormat()) {
	case GE_CMODE_16BIT_BGR5650:
		return RGB565ToRGBA8888(reinterpret_cast<u16*>(clut)[index + clutSharingOffset]);

	case GE_CMODE_16BIT_ABGR5551:
		return RGBA5551ToRGBA8888(reinterpret_cast<u16*>(clut)[index + clutSharingOffset]);

	case GE_CMODE_16BIT_ABGR4444:
		return RGBA4444ToRGBA8888(reinterpret_cast<u16*>(clut)[index + clutSharingOffset]);

	case GE_CMODE_32BIT_ABGR8888:
		return clut[index + clutSharingOffset];

	default:
		ERROR_LOG_REPORT(G3D, "Software: Unsupported palette format: %x", gstate.getClutPaletteFormat());
		return 0;
	}
}

static inline u8 ClampFogDepth(float fogdepth) {
	if (fogdepth <= 0.0f)
		return 0;
	else if (fogdepth >= 1.0f)
		return 255;
	else
		return (u8)(u32)(fogdepth * 255.0f);
}

static inline int ClampUV(int v, int height) {
	if (v >= height - 1)
		return height - 1;
	else if (v < 0)
		return 0;
	return v;
}

static inline int WrapUV(int v, int height) {
	return v & (height - 1);
}

template <int N>
static inline void ApplyTexelClamp(int out_u[N], int out_v[N], const int u[N], const int v[N], int width, int height) {
	if (gstate.isTexCoordClampedS()) {
		for (int i = 0; i < N; ++i) {
			out_u[i] = ClampUV(u[i], width);
		}
	} else {
		for (int i = 0; i < N; ++i) {
			out_u[i] = WrapUV(u[i], width);
		}
	}
	if (gstate.isTexCoordClampedT()) {
		for (int i = 0; i < N; ++i) {
			out_v[i] = ClampUV(v[i], height);
		}
	} else {
		for (int i = 0; i < N; ++i) {
			out_v[i] = WrapUV(v[i], height);
		}
	}
}

template <int N>
static inline void ApplyTexelClampQuad(int out_u[N * 4], int out_v[N * 4], const int u[N], const int v[N], int width, int height) {
	if (gstate.isTexCoordClampedS()) {
		for (int i = 0; i < N * 4; ++i) {
			out_u[i] = ClampUV(u[i >> 2] + (i & 1), width);
		}
	} else {
		for (int i = 0; i < N * 4; ++i) {
			out_u[i] = WrapUV(u[i >> 2] + (i & 1), width);
		}
	}
	if (gstate.isTexCoordClampedT()) {
		for (int i = 0; i < N * 4; ++i) {
			out_v[i] = ClampUV(v[i >> 2] + ((i >> 1) & 1), height);
		}
	} else {
		for (int i = 0; i < N * 4; ++i) {
			out_v[i] = WrapUV(v[i >> 2] + ((i >> 1) & 1), height);
		}
	}
}

static inline void GetTexelCoordinates(int level, float s, float t, int& out_u, int& out_v)
{
	int width = gstate.getTextureWidth(level);
	int height = gstate.getTextureHeight(level);

	int base_u = (int)(s * width * 256.0f + 0.375f);
	int base_v = (int)(t * height * 256.0f + 0.375f);

	base_u >>= 8;
	base_v >>= 8;

	ApplyTexelClamp<1>(&out_u, &out_v, &base_u, &base_v, width, height);
}

static inline void GetTexelCoordinatesQuad(int level, float in_s, float in_t, int u[4], int v[4], int &frac_u, int &frac_v)
{
	// 8 bits of fractional UV
	int width = gstate.getTextureWidth(level);
	int height = gstate.getTextureHeight(level);

	int base_u = (int)(in_s * width * 256.0f + 0.375f) - 128;
	int base_v = (int)(in_t * height * 256.0f + 0.375f) - 128;

	frac_u = (int)(base_u) & 0xff;
	frac_v = (int)(base_v) & 0xff;

	base_u >>= 8;
	base_v >>= 8;

	// Need to generate and individually wrap/clamp the four sample coordinates. Ugh.
	ApplyTexelClampQuad<1>(u, v, &base_u, &base_v, width, height);
}

static inline void GetTextureCoordinates(const VertexData& v0, const VertexData& v1, const VertexData& v2, const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2, const Vec4<float> &wsum_recip, Vec4<float> &s, Vec4<float> &t)
{
	switch (gstate.getUVGenMode()) {
	case GE_TEXMAP_TEXTURE_COORDS:
	case GE_TEXMAP_UNKNOWN:
	case GE_TEXMAP_ENVIRONMENT_MAP:
		{
			// TODO: What happens if vertex has no texture coordinates?
			// Note that for environment mapping, texture coordinates have been calculated during lighting
			float q0 = 1.f / v0.clippos.w;
			float q1 = 1.f / v1.clippos.w;
			float q2 = 1.f / v2.clippos.w;
			Vec4<float> wq0 = w0.Cast<float>() * q0;
			Vec4<float> wq1 = w1.Cast<float>() * q1;
			Vec4<float> wq2 = w2.Cast<float>() * q2;

			Vec4<float> q_recip = (wq0 + wq1 + wq2).Reciprocal();
			s = Interpolate(v0.texturecoords.s(), v1.texturecoords.s(), v2.texturecoords.s(), wq0, wq1, wq2, q_recip);
			t = Interpolate(v0.texturecoords.t(), v1.texturecoords.t(), v2.texturecoords.t(), wq0, wq1, wq2, q_recip);
		}
		break;
	case GE_TEXMAP_TEXTURE_MATRIX:
		for (int i = 0; i < 4; ++i) {
			// projection mapping, TODO: Move this code to TransformUnit!
			Vec3<float> source;
			switch (gstate.getUVProjMode()) {
			case GE_PROJMAP_POSITION:
				source = (v0.modelpos * w0[i] + v1.modelpos * w1[i] + v2.modelpos * w2[i]) * wsum_recip[i];
				break;

			case GE_PROJMAP_UV:
				source = Vec3f((v0.texturecoords * w0[i] + v1.texturecoords * w1[i] + v2.texturecoords * w2[i]) * wsum_recip[i], 0.0f);
				break;

			case GE_PROJMAP_NORMALIZED_NORMAL:
				source = (v0.normal.Normalized() * w0[i] + v1.normal.Normalized() * w1[i] + v2.normal.Normalized() * w2[i]) * wsum_recip[i];
				break;

			case GE_PROJMAP_NORMAL:
				source = (v0.normal * w0[i] + v1.normal * w1[i] + v2.normal * w2[i]) * wsum_recip[i];
				break;

			default:
				ERROR_LOG_REPORT(G3D, "Software: Unsupported UV projection mode %x", gstate.getUVProjMode());
				break;
			}

			Mat3x3<float> tgen(gstate.tgenMatrix);
			Vec3<float> stq = tgen * source + Vec3<float>(gstate.tgenMatrix[9], gstate.tgenMatrix[10], gstate.tgenMatrix[11]);
			float z_recip = 1.0f / stq.z;
			s[i] = stq.x * z_recip;
			t[i] = stq.y * z_recip;
		}
		break;
	default:
		ERROR_LOG_REPORT(G3D, "Software: Unsupported texture mapping mode %x!", gstate.getUVGenMode());
		s = Vec4<float>::AssignToAll(0.0f);
		t = Vec4<float>::AssignToAll(0.0f);
		break;
	}	
}

struct Nearest4 {
	MEMORY_ALIGNED16(u32 v[4]);

	operator u32() const {
		return v[0];
	}
};

template <int N>
inline static Nearest4 SampleNearest(int level, int u[N], int v[N], const u8 *srcptr, int texbufwidthbytes)
{
	Nearest4 res;
	if (!srcptr) {
		memset(res.v, 0, sizeof(res.v));
		return res;
	}

	GETextureFormat texfmt = gstate.getTextureFormat();

	// TODO: Should probably check if textures are aligned properly...

	switch (texfmt) {
	case GE_TFMT_4444:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<16>(texbufwidthbytes, u[i], v[i]);
			res.v[i] = RGBA4444ToRGBA8888(*(const u16 *)src);
		}
		return res;
	
	case GE_TFMT_5551:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<16>(texbufwidthbytes, u[i], v[i]);
			res.v[i] = RGBA5551ToRGBA8888(*(const u16 *)src);
		}
		return res;

	case GE_TFMT_5650:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<16>(texbufwidthbytes, u[i], v[i]);
			res.v[i] = RGB565ToRGBA8888(*(const u16 *)src);
		}
		return res;

	case GE_TFMT_8888:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<32>(texbufwidthbytes, u[i], v[i]);
			res.v[i] = *(const u32 *)src;
		}
		return res;

	case GE_TFMT_CLUT32:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<32>(texbufwidthbytes, u[i], v[i]);
			u32 val = src[0] + (src[1] << 8) + (src[2] << 16) + (src[3] << 24);
			res.v[i] = LookupColor(gstate.transformClutIndex(val), 0);
		}
		return res;

	case GE_TFMT_CLUT16:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<16>(texbufwidthbytes, u[i], v[i]);
			u16 val = src[0] + (src[1] << 8);
			res.v[i] = LookupColor(gstate.transformClutIndex(val), 0);
		}
		return res;

	case GE_TFMT_CLUT8:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<8>(texbufwidthbytes, u[i], v[i]);
			u8 val = *src;
			res.v[i] = LookupColor(gstate.transformClutIndex(val), 0);
		}
		return res;

	case GE_TFMT_CLUT4:
		for (int i = 0; i < N; ++i) {
			const u8 *src = srcptr + GetPixelDataOffset<4>(texbufwidthbytes, u[i], v[i]);
			u8 val = (u[i] & 1) ? (src[0] >> 4) : (src[0] & 0xF);
			// Only CLUT4 uses separate mipmap palettes.
			res.v[i] = LookupColor(gstate.transformClutIndex(val), level);
		}
		return res;

	case GE_TFMT_DXT1:
		for (int i = 0; i < N; ++i) {
			const DXT1Block *block = (const DXT1Block *)srcptr + (v[i] / 4) * (texbufwidthbytes / 4) + (u[i] / 4);
			u32 data[4 * 4];
			DecodeDXT1Block(data, block, 4, 4, false);
			res.v[i] = data[4 * (v[i] % 4) + (u[i] % 4)];
		}
		return res;

	case GE_TFMT_DXT3:
		for (int i = 0; i < N; ++i) {
			const DXT3Block *block = (const DXT3Block *)srcptr + (v[i] / 4) * (texbufwidthbytes / 4) + (u[i] / 4);
			u32 data[4 * 4];
			DecodeDXT3Block(data, block, 4, 4);
			res.v[i] = data[4 * (v[i] % 4) + (u[i] % 4)];
		}
		return res;

	case GE_TFMT_DXT5:
		for (int i = 0; i < N; ++i) {
			const DXT5Block *block = (const DXT5Block *)srcptr + (v[i] / 4) * (texbufwidthbytes / 4) + (u[i] / 4);
			u32 data[4 * 4];
			DecodeDXT5Block(data, block, 4, 4);
			res.v[i] = data[4 * (v[i] % 4) + (u[i] % 4)];
		}
		return res;

	default:
		ERROR_LOG_REPORT(G3D, "Software: Unsupported texture format: %x", texfmt);
		memset(res.v, 0, sizeof(res.v));
		return res;
	}
}

// NOTE: These likely aren't endian safe
static inline u32 GetPixelColor(int x, int y)
{
	switch (gstate.FrameBufFormat()) {
	case GE_FORMAT_565:
		return RGB565ToRGBA8888(fb.Get16(x, y, gstate.FrameBufStride()));

	case GE_FORMAT_5551:
		return RGBA5551ToRGBA8888(fb.Get16(x, y, gstate.FrameBufStride()));

	case GE_FORMAT_4444:
		return RGBA4444ToRGBA8888(fb.Get16(x, y, gstate.FrameBufStride()));

	case GE_FORMAT_8888:
		return fb.Get32(x, y, gstate.FrameBufStride());

	case GE_FORMAT_INVALID:
		_dbg_assert_msg_(G3D, false, "Software: invalid framebuf format.");
	}
	return 0;
}

static inline void SetPixelColor(int x, int y, u32 value)
{
	switch (gstate.FrameBufFormat()) {
	case GE_FORMAT_565:
		fb.Set16(x, y, gstate.FrameBufStride(), RGBA8888ToRGB565(value));
		break;

	case GE_FORMAT_5551:
		fb.Set16(x, y, gstate.FrameBufStride(), RGBA8888ToRGBA5551(value));
		break;

	case GE_FORMAT_4444:
		fb.Set16(x, y, gstate.FrameBufStride(), RGBA8888ToRGBA4444(value));
		break;

	case GE_FORMAT_8888:
		fb.Set32(x, y, gstate.FrameBufStride(), value);
		break;

	case GE_FORMAT_INVALID:
		_dbg_assert_msg_(G3D, false, "Software: invalid framebuf format.");
	}
}

static inline u16 GetPixelDepth(int x, int y)
{
	return depthbuf.Get16(x, y, gstate.DepthBufStride());
}

static inline void SetPixelDepth(int x, int y, u16 value)
{
	depthbuf.Set16(x, y, gstate.DepthBufStride(), value);
}

static inline u8 GetPixelStencil(int x, int y)
{
	if (gstate.FrameBufFormat() == GE_FORMAT_565) {
		// Always treated as 0 for comparison purposes.
		return 0;
	} else if (gstate.FrameBufFormat() == GE_FORMAT_5551) {
		return ((fb.Get16(x, y, gstate.FrameBufStride()) & 0x8000) != 0) ? 0xFF : 0;
	} else if (gstate.FrameBufFormat() == GE_FORMAT_4444) {
		return Convert4To8(fb.Get16(x, y, gstate.FrameBufStride()) >> 12);
	} else {
		return fb.Get32(x, y, gstate.FrameBufStride()) >> 24;
	}
}

static inline void SetPixelStencil(int x, int y, u8 value)
{
	// TODO: This seems like it maybe respects the alpha mask (at least in some scenarios?)

	if (gstate.FrameBufFormat() == GE_FORMAT_565) {
		// Do nothing
	} else if (gstate.FrameBufFormat() == GE_FORMAT_5551) {
		u16 pixel = fb.Get16(x, y, gstate.FrameBufStride()) & ~0x8000;
		pixel |= value != 0 ? 0x8000 : 0;
		fb.Set16(x, y, gstate.FrameBufStride(), pixel);
	} else if (gstate.FrameBufFormat() == GE_FORMAT_4444) {
		u16 pixel = fb.Get16(x, y, gstate.FrameBufStride()) & ~0xF000;
		pixel |= (u16)value << 12;
		fb.Set16(x, y, gstate.FrameBufStride(), pixel);
	} else {
		u32 pixel = fb.Get32(x, y, gstate.FrameBufStride()) & ~0xFF000000;
		pixel |= (u32)value << 24;
		fb.Set32(x, y, gstate.FrameBufStride(), pixel);
	}
}

static inline bool DepthTestPassed(int x, int y, u16 z)
{
	u16 reference_z = GetPixelDepth(x, y);

	switch (gstate.getDepthTestFunction()) {
	case GE_COMP_NEVER:
		return false;

	case GE_COMP_ALWAYS:
		return true;

	case GE_COMP_EQUAL:
		return (z == reference_z);

	case GE_COMP_NOTEQUAL:
		return (z != reference_z);

	case GE_COMP_LESS:
		return (z < reference_z);

	case GE_COMP_LEQUAL:
		return (z <= reference_z);

	case GE_COMP_GREATER:
		return (z > reference_z);

	case GE_COMP_GEQUAL:
		return (z >= reference_z);

	default:
		return 0;
	}
}

static inline bool IsRightSideOrFlatBottomLine(const Vec2<int>& vertex, const Vec2<int>& line1, const Vec2<int>& line2)
{
	if (line1.y == line2.y) {
		// just check if vertex is above us => bottom line parallel to x-axis
		return vertex.y < line1.y;
	} else {
		// check if vertex is on our left => right side
		return vertex.x < line1.x + (line2.x - line1.x) * (vertex.y - line1.y) / (line2.y - line1.y);
	}
}

static inline bool StencilTestPassed(u8 stencil)
{
	// TODO: Does the masking logic make any sense?
	stencil &= gstate.getStencilTestMask();
	u8 ref = gstate.getStencilTestRef() & gstate.getStencilTestMask();
	switch (gstate.getStencilTestFunction()) {
		case GE_COMP_NEVER:
			return false;

		case GE_COMP_ALWAYS:
			return true;

		case GE_COMP_EQUAL:
			return ref == stencil;

		case GE_COMP_NOTEQUAL:
			return ref != stencil;

		case GE_COMP_LESS:
			return ref < stencil;

		case GE_COMP_LEQUAL:
			return ref <= stencil;

		case GE_COMP_GREATER:
			return ref > stencil;

		case GE_COMP_GEQUAL:
			return ref >= stencil;
	}
	return true;
}

static inline u8 ApplyStencilOp(int op, int x, int y)
{
	u8 old_stencil = GetPixelStencil(x, y); // TODO: Apply mask?
	u8 reference_stencil = gstate.getStencilTestRef(); // TODO: Apply mask?

	switch (op) {
		case GE_STENCILOP_KEEP:
			return old_stencil;

		case GE_STENCILOP_ZERO:
			return 0;

		case GE_STENCILOP_REPLACE:
			return reference_stencil;

		case GE_STENCILOP_INVERT:
			return ~old_stencil;

		case GE_STENCILOP_INCR:
			switch (gstate.FrameBufFormat()) {
			case GE_FORMAT_8888:
				if (old_stencil != 0xFF) {
					return old_stencil + 1;
				}
				return old_stencil;
			case GE_FORMAT_5551:
				return 0xFF;
			case GE_FORMAT_4444:
				if (old_stencil < 0xF0) {
					return old_stencil + 0x10;
				}
				return old_stencil;
			default:
				return old_stencil;
			}
			break;

		case GE_STENCILOP_DECR:
			switch (gstate.FrameBufFormat()) {
			case GE_FORMAT_4444:
				if (old_stencil >= 0x10)
					return old_stencil - 0x10;
				break;
			default:
				if (old_stencil != 0)
					return old_stencil - 1;
				return old_stencil;
			}
			break;
	}

	return old_stencil;
}

static inline u32 ApplyLogicOp(GELogicOp op, u32 old_color, u32 new_color)
{
	switch (op) {
	case GE_LOGIC_CLEAR:
		new_color = 0;
		break;

	case GE_LOGIC_AND:
		new_color = new_color & old_color;
		break;

	case GE_LOGIC_AND_REVERSE:
		new_color = new_color & ~old_color;
		break;

	case GE_LOGIC_COPY:
		//new_color = new_color;
		break;

	case GE_LOGIC_AND_INVERTED:
		new_color = ~new_color & old_color;
		break;

	case GE_LOGIC_NOOP:
		new_color = old_color;
		break;

	case GE_LOGIC_XOR:
		new_color = new_color ^ old_color;
		break;

	case GE_LOGIC_OR:
		new_color = new_color | old_color;
		break;

	case GE_LOGIC_NOR:
		new_color = ~(new_color | old_color);
		break;

	case GE_LOGIC_EQUIV:
		new_color = ~(new_color ^ old_color);
		break;

	case GE_LOGIC_INVERTED:
		new_color = ~old_color;
		break;

	case GE_LOGIC_OR_REVERSE:
		new_color = new_color | ~old_color;
		break;

	case GE_LOGIC_COPY_INVERTED:
		new_color = ~new_color;
		break;

	case GE_LOGIC_OR_INVERTED:
		new_color = ~new_color | old_color;
		break;

	case GE_LOGIC_NAND:
		new_color = ~(new_color & old_color);
		break;

	case GE_LOGIC_SET:
		new_color = 0xFFFFFFFF;
		break;
	}

	return new_color;
}

static inline Vec4<int> GetTextureFunctionOutput(const Vec4<int>& prim_color, const Vec4<int>& texcolor)
{
	Vec3<int> out_rgb;
	int out_a;

	bool rgba = gstate.isTextureAlphaUsed();

	switch (gstate.getTextureFunction()) {
	case GE_TEXFUNC_MODULATE:
	{
#if defined(_M_SSE)
		// We can be accurate up to 24 bit integers, should be enough.
		const __m128 p = _mm_cvtepi32_ps(prim_color.ivec);
		const __m128 t = _mm_cvtepi32_ps(texcolor.ivec);
		out_rgb.ivec = _mm_cvtps_epi32(_mm_div_ps(_mm_mul_ps(p, t), _mm_set_ps1(255.0f)));

		if (rgba) {
			return Vec4<int>(out_rgb.ivec);
		} else {
			out_a = prim_color.a();
		}
#else
		out_rgb = prim_color.rgb() * texcolor.rgb() / 255;
		out_a = (rgba) ? (prim_color.a() * texcolor.a() / 255) : prim_color.a();
#endif
		break;
	}

	case GE_TEXFUNC_DECAL:
	{
		int t = (rgba) ? texcolor.a() : 255;
		int invt = (rgba) ? 255 - t : 0;
		out_rgb = (prim_color.rgb() * invt + texcolor.rgb() * t) / 255;
		out_a = prim_color.a();
		break;
	}

	case GE_TEXFUNC_BLEND:
	{
		const Vec3<int> const255(255, 255, 255);
		const Vec3<int> texenv(gstate.getTextureEnvColR(), gstate.getTextureEnvColG(), gstate.getTextureEnvColB());
		out_rgb = ((const255 - texcolor.rgb()) * prim_color.rgb() + texcolor.rgb() * texenv) / 255;
		out_a = prim_color.a() * ((rgba) ? texcolor.a() : 255) / 255;
		break;
	}

	case GE_TEXFUNC_REPLACE:
		out_rgb = texcolor.rgb();
		out_a = (rgba) ? texcolor.a() : prim_color.a();
		break;

	case GE_TEXFUNC_ADD:
		out_rgb = prim_color.rgb() + texcolor.rgb();
		if (out_rgb.r() > 255) out_rgb.r() = 255;
		if (out_rgb.g() > 255) out_rgb.g() = 255;
		if (out_rgb.b() > 255) out_rgb.b() = 255;
		out_a = prim_color.a() * ((rgba) ? texcolor.a() : 255) / 255;
		break;

	default:
		ERROR_LOG_REPORT(G3D, "Software: Unknown texture function %x", gstate.getTextureFunction());
		out_rgb = Vec3<int>::AssignToAll(0);
		out_a = 0;
	}

	return Vec4<int>(out_rgb.r(), out_rgb.g(), out_rgb.b(), out_a);
}

static inline bool ColorTestPassed(const Vec3<int> &color)
{
	const u32 mask = gstate.getColorTestMask();
	const u32 c = color.ToRGB() & mask;
	const u32 ref = gstate.getColorTestRef() & mask;
	switch (gstate.getColorTestFunction()) {
		case GE_COMP_NEVER:
			return false;

		case GE_COMP_ALWAYS:
			return true;

		case GE_COMP_EQUAL:
			return c == ref;

		case GE_COMP_NOTEQUAL:
			return c != ref;

		default:
			ERROR_LOG_REPORT(G3D, "Software: Invalid colortest function: %d", gstate.getColorTestFunction());
			break;
	}
	return true;
}

static inline bool AlphaTestPassed(int alpha)
{
	const u8 mask = gstate.getAlphaTestMask() & 0xFF;
	const u8 ref = gstate.getAlphaTestRef() & mask;
	alpha &= mask;

	switch (gstate.getAlphaTestFunction()) {
		case GE_COMP_NEVER:
			return false;

		case GE_COMP_ALWAYS:
			return true;

		case GE_COMP_EQUAL:
			return (alpha == ref);

		case GE_COMP_NOTEQUAL:
			return (alpha != ref);

		case GE_COMP_LESS:
			return (alpha < ref);

		case GE_COMP_LEQUAL:
			return (alpha <= ref);

		case GE_COMP_GREATER:
			return (alpha > ref);

		case GE_COMP_GEQUAL:
			return (alpha >= ref);
	}
	return true;
}

static inline Vec3<int> GetSourceFactor(const Vec4<int>& source, const Vec4<int>& dst)
{
	switch (gstate.getBlendFuncA()) {
	case GE_SRCBLEND_DSTCOLOR:
		return dst.rgb();

	case GE_SRCBLEND_INVDSTCOLOR:
		return Vec3<int>::AssignToAll(255) - dst.rgb();

	case GE_SRCBLEND_SRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3)));
#else
		return Vec3<int>::AssignToAll(source.a());
#endif

	case GE_SRCBLEND_INVSRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_sub_epi32(_mm_set1_epi32(255), _mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3))));
#else
		return Vec3<int>::AssignToAll(255 - source.a());
#endif

	case GE_SRCBLEND_DSTALPHA:
		return Vec3<int>::AssignToAll(dst.a());

	case GE_SRCBLEND_INVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - dst.a());

	case GE_SRCBLEND_DOUBLESRCALPHA:
		return Vec3<int>::AssignToAll(2 * source.a());

	case GE_SRCBLEND_DOUBLEINVSRCALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * source.a(), 255));

	case GE_SRCBLEND_DOUBLEDSTALPHA:
		return Vec3<int>::AssignToAll(2 * dst.a());

	case GE_SRCBLEND_DOUBLEINVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * dst.a(), 255));

	case GE_SRCBLEND_FIXA:
	default:
		// All other dest factors (> 10) are treated as FIXA.
		return Vec3<int>::FromRGB(gstate.getFixA());
	}
}

static inline Vec3<int> GetDestFactor(const Vec4<int>& source, const Vec4<int>& dst)
{
	switch (gstate.getBlendFuncB()) {
	case GE_DSTBLEND_SRCCOLOR:
		return source.rgb();

	case GE_DSTBLEND_INVSRCCOLOR:
		return Vec3<int>::AssignToAll(255) - source.rgb();

	case GE_DSTBLEND_SRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3)));
#else
		return Vec3<int>::AssignToAll(source.a());
#endif

	case GE_DSTBLEND_INVSRCALPHA:
#if defined(_M_SSE)
		return Vec3<int>(_mm_sub_epi32(_mm_set1_epi32(255), _mm_shuffle_epi32(source.ivec, _MM_SHUFFLE(3, 3, 3, 3))));
#else
		return Vec3<int>::AssignToAll(255 - source.a());
#endif

	case GE_DSTBLEND_DSTALPHA:
		return Vec3<int>::AssignToAll(dst.a());

	case GE_DSTBLEND_INVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - dst.a());

	case GE_DSTBLEND_DOUBLESRCALPHA:
		return Vec3<int>::AssignToAll(2 * source.a());

	case GE_DSTBLEND_DOUBLEINVSRCALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * source.a(), 255));

	case GE_DSTBLEND_DOUBLEDSTALPHA:
		return Vec3<int>::AssignToAll(2 * dst.a());

	case GE_DSTBLEND_DOUBLEINVDSTALPHA:
		return Vec3<int>::AssignToAll(255 - std::min(2 * dst.a(), 255));

	case GE_DSTBLEND_FIXB:
	default:
		// All other dest factors (> 10) are treated as FIXB.
		return Vec3<int>::FromRGB(gstate.getFixB());
	}
}

static inline Vec3<int> AlphaBlendingResult(const Vec4<int> &source, const Vec4<int> &dst)
{
	// Note: These factors cannot go below 0, but they can go above 255 when doubling.
	Vec3<int> srcfactor = GetSourceFactor(source, dst);
	Vec3<int> dstfactor = GetDestFactor(source, dst);

	switch (gstate.getBlendEq()) {
	case GE_BLENDMODE_MUL_AND_ADD:
	{
#if defined(_M_SSE)
		const __m128 s = _mm_mul_ps(_mm_cvtepi32_ps(source.ivec), _mm_cvtepi32_ps(srcfactor.ivec));
		const __m128 d = _mm_mul_ps(_mm_cvtepi32_ps(dst.ivec), _mm_cvtepi32_ps(dstfactor.ivec));
		return Vec3<int>(_mm_cvtps_epi32(_mm_mul_ps(_mm_add_ps(s, d), _mm_set_ps1(1.0f / 255.0f))));
#else
		return (source.rgb() * srcfactor + dst.rgb() * dstfactor) / 255;
#endif
	}

	case GE_BLENDMODE_MUL_AND_SUBTRACT:
	{
#if defined(_M_SSE)
		const __m128 s = _mm_mul_ps(_mm_cvtepi32_ps(source.ivec), _mm_cvtepi32_ps(srcfactor.ivec));
		const __m128 d = _mm_mul_ps(_mm_cvtepi32_ps(dst.ivec), _mm_cvtepi32_ps(dstfactor.ivec));
		return Vec3<int>(_mm_cvtps_epi32(_mm_mul_ps(_mm_sub_ps(s, d), _mm_set_ps1(1.0f / 255.0f))));
#else
		return (source.rgb() * srcfactor - dst.rgb() * dstfactor) / 255;
#endif
	}

	case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE:
	{
#if defined(_M_SSE)
		const __m128 s = _mm_mul_ps(_mm_cvtepi32_ps(source.ivec), _mm_cvtepi32_ps(srcfactor.ivec));
		const __m128 d = _mm_mul_ps(_mm_cvtepi32_ps(dst.ivec), _mm_cvtepi32_ps(dstfactor.ivec));
		return Vec3<int>(_mm_cvtps_epi32(_mm_mul_ps(_mm_sub_ps(d, s), _mm_set_ps1(1.0f / 255.0f))));
#else
		return (dst.rgb() * dstfactor - source.rgb() * srcfactor) / 255;
#endif
	}

	case GE_BLENDMODE_MIN:
		return Vec3<int>(std::min(source.r(), dst.r()),
						std::min(source.g(), dst.g()),
						std::min(source.b(), dst.b()));

	case GE_BLENDMODE_MAX:
		return Vec3<int>(std::max(source.r(), dst.r()),
						std::max(source.g(), dst.g()),
						std::max(source.b(), dst.b()));

	case GE_BLENDMODE_ABSDIFF:
		return Vec3<int>(::abs(source.r() - dst.r()),
						::abs(source.g() - dst.g()),
						::abs(source.b() - dst.b()));

	default:
		ERROR_LOG_REPORT(G3D, "Software: Unknown blend function %x", gstate.getBlendEq());
		return Vec3<int>();
	}
}

template <bool clearMode>
inline void DrawSinglePixel(const DrawingCoords &p, u16 z, u8 fog, const Vec4<int> &color_in) {
	Vec4<int> prim_color = color_in;
	// Depth range test
	// TODO: Clear mode?
	if (!gstate.isModeThrough())
		if (z < gstate.getDepthRangeMin() || z > gstate.getDepthRangeMax())
			return;

	if (gstate.isColorTestEnabled() && !clearMode)
		if (!ColorTestPassed(prim_color.rgb()))
			return;

	// TODO: Does a need to be clamped?
	if (gstate.isAlphaTestEnabled() && !clearMode)
		if (!AlphaTestPassed(prim_color.a()))
			return;

	// In clear mode, it uses the alpha color as stencil.
	u8 stencil = clearMode ? prim_color.a() : GetPixelStencil(p.x, p.y);
	// TODO: Is it safe to ignore gstate.isDepthTestEnabled() when clear mode is enabled? Probably yes
	if (!clearMode && (gstate.isStencilTestEnabled() || gstate.isDepthTestEnabled())) {
		if (gstate.isStencilTestEnabled() && !StencilTestPassed(stencil)) {
			stencil = ApplyStencilOp(gstate.getStencilOpSFail(), p.x, p.y);
			SetPixelStencil(p.x, p.y, stencil);
			return;
		}

		// Also apply depth at the same time.  If disabled, same as passing.
		if (gstate.isDepthTestEnabled() && !DepthTestPassed(p.x, p.y, z)) {
			if (gstate.isStencilTestEnabled()) {
				stencil = ApplyStencilOp(gstate.getStencilOpZFail(), p.x, p.y);
				SetPixelStencil(p.x, p.y, stencil);
			}
			return;
		} else if (gstate.isStencilTestEnabled()) {
			stencil = ApplyStencilOp(gstate.getStencilOpZPass(), p.x, p.y);
		}

		if (gstate.isDepthTestEnabled() && gstate.isDepthWriteEnabled()) {
			SetPixelDepth(p.x, p.y, z);
		}
	} else if (clearMode && gstate.isClearModeDepthMask()) {
		SetPixelDepth(p.x, p.y, z);
	}

	// Doubling happens only when texturing is enabled, and after tests.
	if (gstate.isTextureMapEnabled() && gstate.isColorDoublingEnabled() && !clearMode) {
		// TODO: Does this need to be clamped before blending?
		prim_color.r() <<= 1;
		prim_color.g() <<= 1;
		prim_color.b() <<= 1;
	}

	if (gstate.isFogEnabled() && !gstate.isModeThrough() && !clearMode) {
		Vec3<int> fogColor = Vec3<int>::FromRGB(gstate.fogcolor);
		fogColor = (prim_color.rgb() * (int)fog + fogColor * (255 - (int)fog)) / 255;
		prim_color.r() = fogColor.r();
		prim_color.g() = fogColor.g();
		prim_color.b() = fogColor.b();
	}

	const u32 old_color = GetPixelColor(p.x, p.y);
	u32 new_color;

	if (gstate.isAlphaBlendEnabled() && !clearMode) {
		const Vec4<int> dst = Vec4<int>::FromRGBA(old_color);
		// ToRGBA() always automatically clamps.
		new_color = AlphaBlendingResult(prim_color, dst).ToRGB();
		new_color |= stencil << 24;
	} else {
#if defined(_M_SSE)
		new_color = Vec3<int>(prim_color.ivec).ToRGB();
		new_color |= stencil << 24;
#else
		new_color = Vec4<int>(prim_color.r(), prim_color.g(), prim_color.b(), stencil).ToRGBA();
#endif
	}

	// TODO: Is alpha blending still performed if logic ops are enabled?
	if (gstate.isLogicOpEnabled() && !clearMode) {
		// Logic ops don't affect stencil.
		new_color = (stencil << 24) | (ApplyLogicOp(gstate.getLogicOp(), old_color, new_color) & 0x00FFFFFF);
	}

	if (clearMode) {
		new_color = (new_color & ~gstate.getClearModeColorMask()) | (old_color & gstate.getClearModeColorMask());
	} else {
		new_color = (new_color & ~gstate.getColorMask()) | (old_color & gstate.getColorMask());
	}

	// TODO: Dither before or inside SetPixelColor
	SetPixelColor(p.x, p.y, new_color);
}

static inline Vec4<int> SampleLinear(int texlevel, int u[4], int v[4], int frac_u, int frac_v, const u8 *tptr, int bufwbytes) {
#if defined(_M_SSE)
	Nearest4 c = SampleNearest<4>(texlevel, u, v, tptr, bufwbytes);

	const __m128i z = _mm_setzero_si128();

	__m128i cvec = _mm_load_si128((const __m128i *)c.v);
	__m128i tvec = _mm_unpacklo_epi8(cvec, z);
	tvec = _mm_mullo_epi16(tvec, _mm_set1_epi16(0x100 - frac_v));
	__m128i bvec = _mm_unpackhi_epi8(cvec, z);
	bvec = _mm_mullo_epi16(bvec, _mm_set1_epi16(frac_v));

	// This multiplies the left and right sides.  We shift right after, although this may round down...
	__m128i rowmult = _mm_set_epi16(frac_u, frac_u, frac_u, frac_u, 0x100 - frac_u, 0x100 - frac_u, 0x100 - frac_u, 0x100 - frac_u);
	__m128i tmp = _mm_mulhi_epu16(_mm_add_epi16(tvec, bvec), rowmult);

	// Now we need to add the left and right sides together.
	__m128i res = _mm_add_epi16(tmp, _mm_shuffle_epi32(tmp, _MM_SHUFFLE(3, 2, 3, 2)));
	return Vec4<int>(_mm_unpacklo_epi16(res, z));
#else
	Nearest4 nearest = SampleNearest<4>(texlevel, u, v, tptr, bufwbytes);
	Vec4<int> texcolor_tl = Vec4<int>::FromRGBA(nearest.v[0]);
	Vec4<int> texcolor_tr = Vec4<int>::FromRGBA(nearest.v[1]);
	Vec4<int> texcolor_bl = Vec4<int>::FromRGBA(nearest.v[2]);
	Vec4<int> texcolor_br = Vec4<int>::FromRGBA(nearest.v[3]);
	// 0x100 causes a slight bias to tl, but without it we'd have to divide by 255 * 255.
	Vec4<int> t = texcolor_tl * (0x100 - frac_u) + texcolor_tr * frac_u;
	Vec4<int> b = texcolor_bl * (0x100 - frac_u) + texcolor_br * frac_u;
	return (t * (0x100 - frac_v) + b * frac_v) / (256 * 256);
#endif
}

static inline void ApplyTexturing(Vec4<int> &prim_color, float s, float t, int texlevel, int frac_texlevel, int filt, u8 *texptr[], int texbufwidthbytes[]) {
	int u[8] = {0}, v[8] = {0};   // 1.23.8 fixed point
	int frac_u[2], frac_v[2];

	Vec4<int> texcolor0;
	Vec4<int> texcolor1;
	const u8 *tptr0 = texptr[texlevel];
	int bufwbytes0 = texbufwidthbytes[texlevel];
	const u8 *tptr1 = texptr[texlevel + 1];
	int bufwbytes1 = texbufwidthbytes[texlevel + 1];

	bool bilinear = filt != 0;
	if (!bilinear) {
		// Nearest filtering only.  Round texcoords.
		GetTexelCoordinates(texlevel, s, t, u[0], v[0]);
		if (frac_texlevel) {
			GetTexelCoordinates(texlevel + 1, s, t, u[1], v[1]);
		}

		texcolor0 = Vec4<int>::FromRGBA(SampleNearest<1>(texlevel, u, v, tptr0, bufwbytes0));
		if (frac_texlevel) {
			texcolor1 = Vec4<int>::FromRGBA(SampleNearest<1>(texlevel + 1, u + 1, v + 1, tptr1, bufwbytes1));
		}
	} else {
		GetTexelCoordinatesQuad(texlevel, s, t, u, v, frac_u[0], frac_v[0]);
		if (frac_texlevel) {
			GetTexelCoordinatesQuad(texlevel + 1, s, t, u + 4, v + 4, frac_u[1], frac_v[1]);
		}

		texcolor0 = SampleLinear(texlevel, u, v, frac_u[0], frac_v[0], tptr0, bufwbytes0);
		if (frac_texlevel) {
			texcolor1 = SampleLinear(texlevel + 1, u + 4, v + 4, frac_u[1], frac_v[1], tptr1, bufwbytes1);
		}
	}

	if (frac_texlevel) {
		texcolor0 = (texcolor1 * frac_texlevel + texcolor0 * (256 - frac_texlevel)) / 256;
	}
	prim_color = GetTextureFunctionOutput(prim_color, texcolor0);
}

// Produces a signed 1.23.8 value.
static int TexLog2(float delta) {
	union FloatBits {
		float f;
		u32 u;
	};
	FloatBits f;
	f.f = delta;
	// Use the exponent as the tex level, and the top mantissa bits for a frac.
	// We can't support more than 8 bits of frac, so truncate.
	int useful = (f.u >> 15) & 0xFFFF;
	// Now offset so the exponent aligns with log2f (exp=127 is 0.)
	return useful - 127 * 256;
}

static inline void ApplyTexturing(Vec4<int> *prim_color, const Vec4<float> &s, const Vec4<float> &t, int maxTexLevel, u8 *texptr[], int texbufwidthbytes[]) {
	int width = gstate.getTextureWidth(0);
	int height = gstate.getTextureHeight(0);

	float ds = (s[1] - s[0]) * width;
	float dt = (t[2] - t[0]) * height;

	// With 8 bits of fraction (because texslope can be fairly precise.)
	int detail;
	switch (gstate.getTexLevelMode()) {
	case GE_TEXLEVEL_MODE_AUTO:
		detail = TexLog2(std::max(ds, dt));
		break;
	case GE_TEXLEVEL_MODE_SLOPE:
		// This is always offset by an extra texlevel.
		detail = 1 * 256 + TexLog2(gstate.getTextureLodSlope());
		break;
	case GE_TEXLEVEL_MODE_CONST:
	default:
		// Unused value 3 operates the same as CONST.
		detail = 0;
		break;
	}

	// Add in the bias (used in all modes), expanding to 8 bits of fraction.
	detail += (int)(s8)((gstate.texlevel >> 16) & 0xFF) << 4;

	int minFilt = (gstate.texfilter >> 0) & 1;
	int mipFilt = (gstate.texfilter >> 1) & 1;
	int magFilt = (gstate.texfilter >> 8) & 1;

	int level = 0;
	int levelFrac = 0;
	if (detail > 0 && maxTexLevel > 0) {
		int level8 = std::min(detail, maxTexLevel * 256);
		if (!mipFilt) {
			// Round up at 1.5.
			level8 += 128;
		}
		level = level8 >> 8;
		levelFrac = mipFilt ? level8 & 0xFF : 0;
	}
	int filt = detail > 0 ? minFilt : magFilt;
	if (g_Config.iTexFiltering == 3) {
		filt = 1;
	} else if (g_Config.iTexFiltering == 2) {
		filt = 0;
	}

	for (int i = 0; i < 4; ++i) {
		ApplyTexturing(prim_color[i], s[i], t[i], level, levelFrac, filt, texptr, texbufwidthbytes);
	}
}

struct TriangleEdge {
	Vec4<int> Start(const ScreenCoords &v0, const ScreenCoords &v1, const ScreenCoords &origin);
	inline Vec4<int> StepX(const Vec4<int> &w);
	inline Vec4<int> StepY(const Vec4<int> &w);

	Vec4<int> stepX;
	Vec4<int> stepY;
};

Vec4<int> TriangleEdge::Start(const ScreenCoords &v0, const ScreenCoords &v1, const ScreenCoords &origin) {
	Vec4<int> initX = Vec4<int>::AssignToAll(origin.x) + Vec4<int>(0, 16, 0, 16);
	Vec4<int> initY = Vec4<int>::AssignToAll(origin.y) + Vec4<int>(0, 0, 16, 16);

	// orient2d refactored.
	int xf = v0.y - v1.y;
	int yf = v1.x - v0.x;
	int c = v1.y * v0.x - v1.x * v0.y;

	stepX = Vec4<int>::AssignToAll(xf * 16 * 2);
	stepY = Vec4<int>::AssignToAll(yf * 16 * 2);

	return Vec4<int>::AssignToAll(xf) * initX + Vec4<int>::AssignToAll(yf) * initY + Vec4<int>::AssignToAll(c);
}

inline Vec4<int> TriangleEdge::StepX(const Vec4<int> &w) {
#if defined(_M_SSE) && !defined(_M_IX86)
	return _mm_add_epi32(w.ivec, stepX.ivec);
#else
	return w + stepX;
#endif
}

inline Vec4<int> TriangleEdge::StepY(const Vec4<int> &w) {
#if defined(_M_SSE) && !defined(_M_IX86)
	return _mm_add_epi32(w.ivec, stepY.ivec);
#else
	return w + stepY;
#endif
}

static inline Vec4<int> MakeMask(const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2, const Vec4<int> &bias0, const Vec4<int> &bias1, const Vec4<int> &bias2) {
#if defined(_M_SSE) && !defined(_M_IX86)
	__m128i biased0 = _mm_add_epi32(w0.ivec, bias0.ivec);
	__m128i biased1 = _mm_add_epi32(w1.ivec, bias1.ivec);
	__m128i biased2 = _mm_add_epi32(w2.ivec, bias2.ivec);

	return _mm_or_si128(biased0, _mm_or_si128(biased1, biased2));
#else
	return (w0 + bias0) | (w1 + bias1) | (w2 + bias2);
#endif
}

static inline bool AnyMask(const Vec4<int> &mask) {
#if defined(_M_SSE) && !defined(_M_IX86)
	// In other words: !(mask.x < 0 && mask.y < 0 && mask.z < 0 && mask.w < 0)
	__m128i low2 = _mm_and_si128(mask.ivec, _mm_shuffle_epi32(mask.ivec, _MM_SHUFFLE(3, 2, 3, 2)));
	__m128i low1 = _mm_and_si128(low2, _mm_shuffle_epi32(low2, _MM_SHUFFLE(1, 1, 1, 1)));
	// Now we only need to check one sign bit.
	return _mm_cvtsi128_si32(low1) >= 0;
#else
	return mask.x >= 0 || mask.y >= 0 || mask.z >= 0 || mask.w >= 0;
#endif
}

static inline Vec4<float> EdgeRecip(const Vec4<int> &w0, const Vec4<int> &w1, const Vec4<int> &w2) {
#if defined(_M_SSE) && !defined(_M_IX86)
	__m128i wsum = _mm_add_epi32(w0.ivec, _mm_add_epi32(w1.ivec, w2.ivec));
	return _mm_rcp_ps(_mm_cvtepi32_ps(wsum));
#else
	return (w0 + w1 + w2).Cast<float>().Reciprocal();
#endif
}

template <bool clearMode>
void DrawTriangleSlice(
	const VertexData& v0, const VertexData& v1, const VertexData& v2,
	int minX, int minY, int maxX, int maxY,
	int hy1, int hy2)
{
	Vec4<int> bias0 = Vec4<int>::AssignToAll(IsRightSideOrFlatBottomLine(v0.screenpos.xy(), v1.screenpos.xy(), v2.screenpos.xy()) ? -1 : 0);
	Vec4<int> bias1 = Vec4<int>::AssignToAll(IsRightSideOrFlatBottomLine(v1.screenpos.xy(), v2.screenpos.xy(), v0.screenpos.xy()) ? -1 : 0);
	Vec4<int> bias2 = Vec4<int>::AssignToAll(IsRightSideOrFlatBottomLine(v2.screenpos.xy(), v0.screenpos.xy(), v1.screenpos.xy()) ? -1 : 0);

	int texbufwidthbytes[8] = {0};

	int maxTexLevel = gstate.getTextureMaxLevel();
	u8 *texptr[8] = {NULL};

	if ((gstate.texfilter & 4) == 0 || !g_Config.bMipMap) {
		// No mipmapping enabled
		maxTexLevel = 0;
	}

	if (gstate.isTextureMapEnabled() && !clearMode) {
		// TODO: Always using level 0.
		GETextureFormat texfmt = gstate.getTextureFormat();
		for (int i = 0; i <= maxTexLevel; i++) {
			u32 texaddr = gstate.getTextureAddress(i);
			texbufwidthbytes[i] = GetTextureBufw(i, texaddr, texfmt);
			if (Memory::IsValidAddress(texaddr))
				texptr[i] = Memory::GetPointerUnchecked(texaddr);
			else
				texptr[i] = 0;
		}
	}

	TriangleEdge e0;
	TriangleEdge e1;
	TriangleEdge e2;

	ScreenCoords pprime(minX, minY, 0);
	Vec4<int> w0_base = e0.Start(v1.screenpos, v2.screenpos, pprime);
	Vec4<int> w1_base = e1.Start(v2.screenpos, v0.screenpos, pprime);
	Vec4<int> w2_base = e2.Start(v0.screenpos, v1.screenpos, pprime);

	// Step forward to y1 (slice..)
	w0_base += e0.stepY * hy1;
	w1_base += e1.stepY * hy1;
	w2_base += e2.stepY * hy1;

	// All the z values are the same, no interpolation required.
	// This is common, and when we interpolate, we lose accuracy.
	const bool flatZ = v0.screenpos.z == v1.screenpos.z && v0.screenpos.z == v2.screenpos.z;

	for (pprime.y = minY + hy1 * 32; pprime.y < minY + hy2 * 32; pprime.y += 32,
										w0_base = e0.StepY(w0_base),
										w1_base = e1.StepY(w1_base),
										w2_base = e2.StepY(w2_base)) {
		Vec4<int> w0 = w0_base;
		Vec4<int> w1 = w1_base;
		Vec4<int> w2 = w2_base;

		pprime.x = minX;
		DrawingCoords p = TransformUnit::ScreenToDrawing(pprime);

		for (; pprime.x <= maxX; pprime.x += 32,
			w0 = e0.StepX(w0),
			w1 = e1.StepX(w1),
			w2 = e2.StepX(w2),
			p.x = (p.x + 2) & 0x3FF) {

			// If p is on or inside all edges, render pixel
			Vec4<int> mask = MakeMask(w0, w1, w2, bias0, bias1, bias2);
			if (AnyMask(mask)) {
				Vec4<float> wsum_recip = EdgeRecip(w0, w1, w2);

				Vec4<int> prim_color[4];
				Vec3<int> sec_color[4];
				if (gstate.getShadeMode() == GE_SHADE_GOURAUD && !clearMode) {
					// Does the PSP do perspective-correct color interpolation? The GC doesn't.
					for (int i = 0; i < 4; ++i) {
						prim_color[i] = Interpolate(v0.color0, v1.color0, v2.color0, w0[i], w1[i], w2[i], wsum_recip[i]);
						sec_color[i] = Interpolate(v0.color1, v1.color1, v2.color1, w0[i], w1[i], w2[i], wsum_recip[i]);
					}
				} else {
					for (int i = 0; i < 4; ++i) {
						prim_color[i] = v2.color0;
						sec_color[i] = v2.color1;
					}
				}

				if (gstate.isTextureMapEnabled() && !clearMode) {
					Vec4<float> s, t;
					if (gstate.isModeThrough()) {
						s = Interpolate(v0.texturecoords.s(), v1.texturecoords.s(), v2.texturecoords.s(), w0, w1, w2, wsum_recip);
						t = Interpolate(v0.texturecoords.t(), v1.texturecoords.t(), v2.texturecoords.t(), w0, w1, w2, wsum_recip);

						// For levels > 0, mipmapping is always based on level 0.  Simpler to scale first.
						s *= 1.0f / (float)gstate.getTextureWidth(0);
						t *= 1.0f / (float)gstate.getTextureHeight(0);
					} else {
						// Texture coordinate interpolation must definitely be perspective-correct.
						GetTextureCoordinates(v0, v1, v2, w0, w1, w2, wsum_recip, s, t);
					}

					ApplyTexturing(prim_color, s, t, maxTexLevel, texptr, texbufwidthbytes);
				}

				if (!clearMode) {
					for (int i = 0; i < 4; ++i) {
#if defined(_M_SSE)
						// TODO: Tried making Vec4 do this, but things got slower.
						const __m128i sec = _mm_and_si128(sec_color[i].ivec, _mm_set_epi32(0, -1, -1, -1));
						prim_color[i].ivec = _mm_add_epi32(prim_color[i].ivec, sec);
#else
						prim_color[i] += Vec4<int>(sec_color[i], 0);
#endif
					}
				}

				Vec4<int> fog = Vec4<int>::AssignToAll(255);
				if (gstate.isFogEnabled() && !clearMode) {
					Vec4<float> fogdepths = w0.Cast<float>() * v0.fogdepth + w1.Cast<float>() * v1.fogdepth + w2.Cast<float>() * v2.fogdepth;
					fogdepths = fogdepths * wsum_recip;
					for (int i = 0; i < 4; ++i) {
						fog[i] = ClampFogDepth(fogdepths[i]);
					}
				}

				Vec4<int> z;
				if (flatZ) {
					z = Vec4<int>::AssignToAll(v2.screenpos.z);
				} else {
					// TODO: Is that the correct way to interpolate?
					Vec4<float> zfloats = w0.Cast<float>() * v0.screenpos.z + w1.Cast<float>() * v1.screenpos.z + w2.Cast<float>() * v2.screenpos.z;
					z = (zfloats * wsum_recip).Cast<int>();
				}

				DrawingCoords subp = p;
				for (int i = 0; i < 4; ++i) {
					if (mask[i] < 0) {
						continue;
					}
					subp.x = p.x + (i & 1);
					subp.y = p.y + (i / 2);

					DrawSinglePixel<clearMode>(subp, (u16)z[i], fog[i], prim_color[i]);
				}
			}
		}
	}
}

// Draws triangle, vertices specified in counter-clockwise direction
void DrawTriangle(const VertexData& v0, const VertexData& v1, const VertexData& v2)
{
	PROFILE_THIS_SCOPE("draw_tri");

	Vec2<int> d01((int)v0.screenpos.x - (int)v1.screenpos.x, (int)v0.screenpos.y - (int)v1.screenpos.y);
	Vec2<int> d02((int)v0.screenpos.x - (int)v2.screenpos.x, (int)v0.screenpos.y - (int)v2.screenpos.y);
	Vec2<int> d12((int)v1.screenpos.x - (int)v2.screenpos.x, (int)v1.screenpos.y - (int)v2.screenpos.y);

	// Drop primitives which are not in CCW order by checking the cross product
	if (d01.x * d02.y - d01.y * d02.x < 0)
		return;

	int minX = std::min(std::min(v0.screenpos.x, v1.screenpos.x), v2.screenpos.x) & ~0xF;
	int minY = std::min(std::min(v0.screenpos.y, v1.screenpos.y), v2.screenpos.y) & ~0xF;
	int maxX = std::max(std::max(v0.screenpos.x, v1.screenpos.x), v2.screenpos.x) & ~0xF;
	int maxY = std::max(std::max(v0.screenpos.y, v1.screenpos.y), v2.screenpos.y) & ~0xF;

	DrawingCoords scissorTL(gstate.getScissorX1(), gstate.getScissorY1(), 0);
	DrawingCoords scissorBR(gstate.getScissorX2(), gstate.getScissorY2(), 0);
	minX = std::max(minX, (int)TransformUnit::DrawingToScreen(scissorTL).x);
	maxX = std::min(maxX, (int)TransformUnit::DrawingToScreen(scissorBR).x);
	minY = std::max(minY, (int)TransformUnit::DrawingToScreen(scissorTL).y);
	maxY = std::min(maxY, (int)TransformUnit::DrawingToScreen(scissorBR).y);

	// 32 because we do two pixels at once, and we don't want overlap.
	int range = (maxY - minY) / 32 + 1;
	if (gstate.isModeClear()) {
		if (range >= 12 && (maxX - minX) >= 24 * 16) {
			auto bound = [&](int a, int b) -> void {
				DrawTriangleSlice<true>(v0, v1, v2, minX, minY, maxX, maxY, a, b);
			};
			GlobalThreadPool::Loop(bound, 0, range);
		} else {
			DrawTriangleSlice<true>(v0, v1, v2, minX, minY, maxX, maxY, 0, range);
		}
	} else {
		if (range >= 12 && (maxX - minX) >= 24 * 16) {
			auto bound = [&](int a, int b) -> void {
				DrawTriangleSlice<false>(v0, v1, v2, minX, minY, maxX, maxY, a, b);
			};
			GlobalThreadPool::Loop(bound, 0, range);
		} else {
			DrawTriangleSlice<false>(v0, v1, v2, minX, minY, maxX, maxY, 0, range);
		}
	}
}

void DrawPoint(const VertexData &v0)
{
	ScreenCoords pos = v0.screenpos;
	Vec4<int> prim_color = v0.color0;
	Vec3<int> sec_color = v0.color1;
	// TODO: UVGenMode?
	float s = v0.texturecoords.s();
	float t = v0.texturecoords.t();

	ScreenCoords scissorTL(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX1(), gstate.getScissorY1(), 0)));
	ScreenCoords scissorBR(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX2(), gstate.getScissorY2(), 0)));

	if (pos.x < scissorTL.x || pos.y < scissorTL.y || pos.x >= scissorBR.x || pos.y >= scissorBR.y)
		return;

	bool clearMode = gstate.isModeClear();

	if (gstate.isTextureMapEnabled() && !clearMode) {
		int texbufwidthbytes[8] = {0};

		int maxTexLevel = gstate.getTextureMaxLevel();
		u8 *texptr[8] = {NULL};

		int magFilt = (gstate.texfilter>>8) & 1;
		if (g_Config.iTexFiltering > 1) {
			if (g_Config.iTexFiltering == 2) {
				magFilt = 0;
			} else if (g_Config.iTexFiltering == 3) {
				magFilt = 1;
			}
		}
		if ((gstate.texfilter & 4) == 0 || !g_Config.bMipMap) {
			// No mipmapping enabled
			maxTexLevel = 0;
		}

		if (gstate.isTextureMapEnabled() && !clearMode) {
			// TODO: Always using level 0.
			maxTexLevel = 0;
			GETextureFormat texfmt = gstate.getTextureFormat();
			for (int i = 0; i <= maxTexLevel; i++) {
				u32 texaddr = gstate.getTextureAddress(i);
				texbufwidthbytes[i] = GetTextureBufw(i, texaddr, texfmt);
				texptr[i] = Memory::GetPointer(texaddr);
			}
		}

		if (gstate.isModeThrough()) {
			s *= 1.0f / (float)gstate.getTextureWidth(0);
			t *= 1.0f / (float)gstate.getTextureHeight(0);
		}

		ApplyTexturing(prim_color, s, t, 0, 0, magFilt, texptr, texbufwidthbytes);
	}

	if (!clearMode)
		prim_color += Vec4<int>(sec_color, 0);

	ScreenCoords pprime = pos;

	DrawingCoords p = TransformUnit::ScreenToDrawing(pprime);
	u16 z = pos.z;

	u8 fog = 255;
	if (gstate.isFogEnabled() && !clearMode) {
		fog = ClampFogDepth(v0.fogdepth);
	}

	if (clearMode) {
		DrawSinglePixel<true>(p, z, fog, prim_color);
	} else {
		DrawSinglePixel<false>(p, z, fog, prim_color);
	}
}

void DrawLine(const VertexData &v0, const VertexData &v1)
{
	// TODO: Use a proper line drawing algorithm that handles fractional endpoints correctly.
	Vec3<int> a(v0.screenpos.x, v0.screenpos.y, v0.screenpos.z);
	Vec3<int> b(v1.screenpos.x, v1.screenpos.y, v0.screenpos.z);

	int dx = b.x - a.x;
	int dy = b.y - a.y;
	int dz = b.z - a.z;

	int steps;
	if (abs(dx) < abs(dy))
		steps = abs(dy) / 16;
	else
		steps = abs(dx) / 16;

	float xinc = (float)dx / steps;
	float yinc = (float)dy / steps;
	float zinc = (float)dz / steps;

	ScreenCoords scissorTL(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX1(), gstate.getScissorY1(), 0)));
	ScreenCoords scissorBR(TransformUnit::DrawingToScreen(DrawingCoords(gstate.getScissorX2(), gstate.getScissorY2(), 0)));
	bool clearMode = gstate.isModeClear();

	int texbufwidthbytes[8] = {0};

	int maxTexLevel = gstate.getTextureMaxLevel();
	u8 *texptr[8] = {NULL};

	int magFilt = (gstate.texfilter>>8) & 1;
	if (g_Config.iTexFiltering > 1) {
		if (g_Config.iTexFiltering == 2) {
			magFilt = 0;
		} else if (g_Config.iTexFiltering == 3) {
			magFilt = 1;
		}
	}
	if ((gstate.texfilter & 4) == 0 || !g_Config.bMipMap) {
		// No mipmapping enabled
		maxTexLevel = 0;
	}

	if (gstate.isTextureMapEnabled() && !clearMode) {
		// TODO: Always using level 0.
		GETextureFormat texfmt = gstate.getTextureFormat();
		for (int i = 0; i <= maxTexLevel; i++) {
			u32 texaddr = gstate.getTextureAddress(i);
			texbufwidthbytes[i] = GetTextureBufw(i, texaddr, texfmt);
			texptr[i] = Memory::GetPointer(texaddr);
		}
	}

	float x = a.x;
	float y = a.y;
	float z = a.z;
	const int steps1 = steps == 0 ? 1 : steps;
	for (int i = 0; i <= steps; i++) {
		if (x < scissorTL.x || y < scissorTL.y || x >= scissorBR.x || y >= scissorBR.y)
			continue;

		// Interpolate between the two points.
		Vec4<int> c0 = (v0.color0 * (steps - i) + v1.color0 * i) / steps1;
		Vec3<int> sec_color = (v0.color1 * (steps - i) + v1.color1 * i) / steps1;
		// TODO: UVGenMode?
		Vec2<float> tc = (v0.texturecoords * (float)(steps - i) + v1.texturecoords * (float)i) / steps1;
		Vec4<int> prim_color = c0;

		u8 fog = 255;
		if (gstate.isFogEnabled() && !clearMode) {
			fog = ClampFogDepth((v0.fogdepth * (float)(steps - i) + v1.fogdepth * (float)i) / steps1);
		}

		float s = tc.s();
		float t = tc.t();

		if (gstate.isTextureMapEnabled() && !clearMode) {
			if (gstate.isModeThrough()) {
				s *= 1.0f / (float)gstate.getTextureWidth(0);
				t *= 1.0f / (float)gstate.getTextureHeight(0);
			}
			ApplyTexturing(prim_color, s, t, 0, 0, magFilt, texptr, texbufwidthbytes);
		}

		if (!clearMode)
			prim_color += Vec4<int>(sec_color, 0);

		ScreenCoords pprime = ScreenCoords(x, y, z);

		DrawingCoords p = TransformUnit::ScreenToDrawing(pprime);
		if (clearMode) {
			DrawSinglePixel<true>(p, z, fog, prim_color);
		} else {
			DrawSinglePixel<false>(p, z, fog, prim_color);
		}

		x = x + xinc;
		y = y + yinc;
		z = z + zinc;
	}
}

bool GetCurrentStencilbuffer(GPUDebugBuffer &buffer)
{
	int w = gstate.getRegionX2() - gstate.getRegionX1() + 1;
	int h = gstate.getRegionY2() - gstate.getRegionY1() + 1;
	buffer.Allocate(w, h, GPU_DBG_FORMAT_8BIT);

	u8 *row = buffer.GetData();
	for (int y = gstate.getRegionY1(); y <= gstate.getRegionY2(); ++y) {
		for (int x = gstate.getRegionX1(); x <= gstate.getRegionX2(); ++x) {
			row[x - gstate.getRegionX1()] = GetPixelStencil(x, y);
		}
		row += w;
	}
	return true;
}

bool GetCurrentTexture(GPUDebugBuffer &buffer, int level)
{
	if (!gstate.isTextureMapEnabled()) {
		return false;
	}

	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);
	buffer.Allocate(w, h, GE_FORMAT_8888, false);

	GETextureFormat texfmt = gstate.getTextureFormat();
	u32 texaddr = gstate.getTextureAddress(level);
	int texbufwidthbytes = GetTextureBufw(level, texaddr, texfmt);
	u8 *texptr = Memory::GetPointer(texaddr);

	u32 *row = (u32 *)buffer.GetData();
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			row[x] = SampleNearest<1>(level, &x, &y, texptr, texbufwidthbytes);
		}
		row += w;
	}
	return true;
}

} // namespace
