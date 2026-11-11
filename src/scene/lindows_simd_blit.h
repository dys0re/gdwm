/*
    Lindows WARP Software Compositor SIMD Blit & Aligned Buffer
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QImage>
#include <QRect>
#include <QRegion>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace KWin
{

class AlignedShadowBuffer
{
public:
    AlignedShadowBuffer() = default;
    ~AlignedShadowBuffer()
    {
        free();
    }

    AlignedShadowBuffer(const AlignedShadowBuffer &) = delete;
    AlignedShadowBuffer &operator=(const AlignedShadowBuffer &) = delete;

    AlignedShadowBuffer(AlignedShadowBuffer &&other) noexcept
    {
        *this = std::move(other);
    }

    AlignedShadowBuffer &operator=(AlignedShadowBuffer &&other) noexcept
    {
        if (this != &other) {
            free();
            m_data = other.m_data;
            m_size = other.m_size;
            m_stride = other.m_stride;
            m_dimensions = other.m_dimensions;
            m_format = other.m_format;
            m_image = other.m_image;

            other.m_data = nullptr;
            other.m_size = 0;
            other.m_stride = 0;
            other.m_dimensions = QSize();
            other.m_format = QImage::Format_Invalid;
            other.m_image = QImage();
        }
        return *this;
    }

    bool allocate(const QSize &dimensions, QImage::Format format)
    {
        if (m_dimensions == dimensions && m_format == format && m_data && !m_image.isNull()) {
            return true;
        }

        free();

        if (dimensions.isEmpty() || format == QImage::Format_Invalid) {
            return false;
        }

        const int bpp = 4; // 32-bit ARGB/XRGB formats
        m_stride = ((dimensions.width() * bpp + 63) / 64) * 64;
        m_size = static_cast<size_t>(m_stride) * dimensions.height();
        m_dimensions = dimensions;
        m_format = format;

        void *ptr = nullptr;
        if (posix_memalign(&ptr, 64, m_size) != 0 || !ptr) {
            m_data = nullptr;
            m_size = 0;
            m_stride = 0;
            m_dimensions = QSize();
            m_format = QImage::Format_Invalid;
            return false;
        }

        m_data = static_cast<uint8_t *>(ptr);
        std::memset(m_data, 0, m_size);

        m_image = QImage(m_data, dimensions.width(), dimensions.height(), m_stride, format);
        return !m_image.isNull();
    }

    void free()
    {
        m_image = QImage();
        if (m_data) {
            ::free(m_data);
            m_data = nullptr;
        }
        m_size = 0;
        m_stride = 0;
        m_dimensions = QSize();
        m_format = QImage::Format_Invalid;
    }

    uint8_t *data() const
    {
        return m_data;
    }
    size_t size() const
    {
        return m_size;
    }
    int stride() const
    {
        return m_stride;
    }
    const QSize &dimensions() const
    {
        return m_dimensions;
    }
    QImage::Format format() const
    {
        return m_format;
    }
    QImage &image()
    {
        return m_image;
    }
    const QImage &image() const
    {
        return m_image;
    }
    bool isValid() const
    {
        return m_data != nullptr && !m_image.isNull();
    }

private:
    uint8_t *m_data = nullptr;
    size_t m_size = 0;
    int m_stride = 0;
    QSize m_dimensions;
    QImage::Format m_format = QImage::Format_Invalid;
    QImage m_image;
};

// 128-bit SIMD burst row copy
// Crucial: dst is Write-Combining or uncached memory: NEVER read from dst!
inline void lindowsCopyRow128(uint8_t *dst, const uint8_t *src, size_t bytes)
{
#if defined(__SSE2__)
    size_t i = 0;
    // 64-byte burst loop (unrolled 4x __m128i)
    for (; i + 64 <= bytes; i += 64) {
        __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
        __m128i r1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 16));
        __m128i r2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 32));
        __m128i r3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i + 48));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), r0);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i + 16), r1);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i + 32), r2);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i + 48), r3);
    }
    // 16-byte remainder loop
    for (; i + 16 <= bytes; i += 16) {
        __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), r);
    }
    // 4-byte remainder loop (single pixel)
    for (; i + 4 <= bytes; i += 4) {
        *reinterpret_cast<uint32_t *>(dst + i) = *reinterpret_cast<const uint32_t *>(src + i);
    }
    // byte remainder loop
    for (; i < bytes; ++i) {
        dst[i] = src[i];
    }
#elif defined(__aarch64__)
    size_t i = 0;
    for (; i + 64 <= bytes; i += 64) {
        uint8x16_t r0 = vld1q_u8(src + i);
        uint8x16_t r1 = vld1q_u8(src + i + 16);
        uint8x16_t r2 = vld1q_u8(src + i + 32);
        uint8x16_t r3 = vld1q_u8(src + i + 48);
        vst1q_u8(dst + i, r0);
        vst1q_u8(dst + i + 16, r1);
        vst1q_u8(dst + i + 32, r2);
        vst1q_u8(dst + i + 48, r3);
    }
    for (; i + 16 <= bytes; i += 16) {
        uint8x16_t r = vld1q_u8(src + i);
        vst1q_u8(dst + i, r);
    }
    for (; i + 4 <= bytes; i += 4) {
        *reinterpret_cast<uint32_t *>(dst + i) = *reinterpret_cast<const uint32_t *>(src + i);
    }
    for (; i < bytes; ++i) {
        dst[i] = src[i];
    }
#else
    std::memcpy(dst, src, bytes);
#endif
}

inline void lindowsBlitDirtyRegion(uint8_t *dstBytes, int dstStride,
                                  const uint8_t *srcBytes, int srcStride,
                                  int bytesPerPixel,
                                  const QRegion &dirtyRegion,
                                  const QRect &bounds)
{
    if (!dstBytes || !srcBytes || dirtyRegion.isEmpty() || bounds.isEmpty() || bytesPerPixel <= 0) {
        return;
    }

    for (const QRect &r : dirtyRegion) {
        const QRect rect = r.intersected(bounds);
        if (rect.isEmpty()) {
            continue;
        }

        const int rx = rect.x();
        const int ry = rect.y();
        const int rw = rect.width();
        const int rh = rect.height();
        const size_t rowBytes = static_cast<size_t>(rw) * bytesPerPixel;

        // Fast-path: if full-width and strides match, burst-copy entire height at once
        if (rx == 0 && rw == bounds.width() && srcStride == dstStride) {
            uint8_t *d = dstBytes + static_cast<size_t>(ry) * dstStride;
            const uint8_t *s = srcBytes + static_cast<size_t>(ry) * srcStride;
            lindowsCopyRow128(d, s, static_cast<size_t>(rh) * srcStride);
            continue;
        }

        for (int y = ry; y < ry + rh; ++y) {
            uint8_t *d = dstBytes + static_cast<size_t>(y) * dstStride + static_cast<size_t>(rx) * bytesPerPixel;
            const uint8_t *s = srcBytes + static_cast<size_t>(y) * srcStride + static_cast<size_t>(rx) * bytesPerPixel;
            lindowsCopyRow128(d, s, rowBytes);
        }
    }
}

inline void lindowsBlitDirtyRegion(QImage *dst, const QImage &src, const QRegion &dirtyRegion)
{
    if (Q_UNLIKELY(!dst || dst->isNull() || src.isNull())) {
        return;
    }
    const int bpp = dst->depth() / 8;
    const QRect bounds = dst->rect().intersected(src.rect());
    lindowsBlitDirtyRegion(dst->bits(), dst->bytesPerLine(),
                           src.constBits(), src.bytesPerLine(),
                           bpp > 0 ? bpp : 4,
                           dirtyRegion, bounds);
}

// Premultiplied alpha single-pixel blend helper (scalar fallback)
inline uint32_t blendPixelPremulScalar(uint32_t src, uint32_t dst, int opacity256, bool dstIsRGB32)
{
    if (opacity256 < 256) {
        const uint32_t a = (((src >> 24) & 0xff) * opacity256) >> 8;
        const uint32_t r = (((src >> 16) & 0xff) * opacity256) >> 8;
        const uint32_t g = (((src >> 8) & 0xff) * opacity256) >> 8;
        const uint32_t b = ((src & 0xff) * opacity256) >> 8;
        src = (a << 24) | (r << 16) | (g << 8) | b;
    }
    const uint32_t sa = (src >> 24) & 0xff;
    if (sa == 0) {
        return dstIsRGB32 ? (dst | 0xFF000000) : dst;
    }
    if (sa == 255) {
        return dstIsRGB32 ? (src | 0xFF000000) : src;
    }
    const uint32_t invA = 256 - sa;
    const uint32_t da = (dst >> 24) & 0xff;
    const uint32_t dr = (dst >> 16) & 0xff;
    const uint32_t dg = (dst >> 8) & 0xff;
    const uint32_t db = dst & 0xff;

    const uint32_t sr = (src >> 16) & 0xff;
    const uint32_t sg = (src >> 8) & 0xff;
    const uint32_t sb = src & 0xff;

    uint32_t oa = dstIsRGB32 ? 255 : (sa + ((da * invA) >> 8));
    uint32_t or_ = sr + ((dr * invA) >> 8);
    uint32_t og = sg + ((dg * invA) >> 8);
    uint32_t ob = sb + ((db * invA) >> 8);

    oa = std::min(255u, oa);
    or_ = std::min(255u, or_);
    og = std::min(255u, og);
    ob = std::min(255u, ob);

    return (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

#if defined(__SSE2__)
// 4-pixel premultiplied alpha blend with all_zero skip and all_opaque direct store
inline void lindowsBlend4PixelsSSE2(uint32_t *dPtr, __m128i vsrc, int opacity256, bool dstIsRGB32)
{
    // Fast-skip when all 4 source pixels are completely transparent (0x00000000)
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(vsrc, _mm_setzero_si128())) == 0xFFFF) {
        return;
    }

    // Scale src channels by opacity256 if opacity < 256
    if (opacity256 < 256) {
        __m128i zero = _mm_setzero_si128();
        __m128i src_lo = _mm_unpacklo_epi8(vsrc, zero);
        __m128i src_hi = _mm_unpackhi_epi8(vsrc, zero);
        __m128i vop = _mm_set1_epi16(opacity256);
        src_lo = _mm_srli_epi16(_mm_mullo_epi16(src_lo, vop), 8);
        src_hi = _mm_srli_epi16(_mm_mullo_epi16(src_hi, vop), 8);
        vsrc = _mm_packus_epi16(src_lo, src_hi);

        if (_mm_movemask_epi8(_mm_cmpeq_epi32(vsrc, zero)) == 0xFFFF) {
            return;
        }
    }

    // Fast-store when all 4 source pixels are completely opaque (alpha == 0xFF)
    __m128i alpha_mask = _mm_set1_epi32(0xFF000000);
    __m128i alphas = _mm_and_si128(vsrc, alpha_mask);
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(alphas, alpha_mask)) == 0xFFFF) {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dPtr), vsrc);
        return;
    }

    // General 4-pixel premultiplied alpha blend:
    // out = src + ((dst * (256 - srcA)) >> 8)
    __m128i zero = _mm_setzero_si128();
    __m128i src_lo = _mm_unpacklo_epi8(vsrc, zero);
    __m128i src_hi = _mm_unpackhi_epi8(vsrc, zero);

    __m128i alpha_lo = _mm_shufflehi_epi16(_mm_shufflelo_epi16(src_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
    __m128i alpha_hi = _mm_shufflehi_epi16(_mm_shufflelo_epi16(src_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

    __m128i invA_lo = _mm_sub_epi16(_mm_set1_epi16(256), alpha_lo);
    __m128i invA_hi = _mm_sub_epi16(_mm_set1_epi16(256), alpha_hi);

    __m128i vdst = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dPtr));
    __m128i dst_lo = _mm_unpacklo_epi8(vdst, zero);
    __m128i dst_hi = _mm_unpackhi_epi8(vdst, zero);

    __m128i dst_scaled_lo = _mm_srli_epi16(_mm_mullo_epi16(dst_lo, invA_lo), 8);
    __m128i dst_scaled_hi = _mm_srli_epi16(_mm_mullo_epi16(dst_hi, invA_hi), 8);

    __m128i out_lo = _mm_add_epi16(src_lo, dst_scaled_lo);
    __m128i out_hi = _mm_add_epi16(src_hi, dst_scaled_hi);

    __m128i out = _mm_packus_epi16(out_lo, out_hi);
    if (dstIsRGB32) {
        out = _mm_or_si128(out, alpha_mask);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i *>(dPtr), out);
}
#endif

// Kernel 1: 1:1 Unscaled Constant-Opacity / Premul-Alpha SourceOver
inline void lindowsBlendRegionSimd(QImage *dst,
                                   int dstX, int dstY,
                                   const QImage &src,
                                   int srcX, int srcY,
                                   int width, int height,
                                   const QRegion &clipRegion,
                                   int opacity256 = 256,
                                   bool forceOpaque = false)
{
    if (Q_UNLIKELY(!dst || dst->isNull() || src.isNull() || width <= 0 || height <= 0 || clipRegion.isEmpty())) {
        return;
    }
    if (Q_UNLIKELY(opacity256 <= 0)) {
        return;
    }

    const bool dstIsRGB32 = (dst->format() == QImage::Format_RGB32);

    const QRect srcBounds(srcX, srcY, width, height);
    const QRect validSrc = srcBounds.intersected(src.rect());
    if (validSrc.isEmpty()) {
        return;
    }
    dstX += (validSrc.x() - srcX);
    dstY += (validSrc.y() - srcY);
    srcX = validSrc.x();
    srcY = validSrc.y();
    const int validW = validSrc.width();
    const int validH = validSrc.height();
    const QRect dstBounds(dstX, dstY, validW, validH);
    const QRect validDst = dstBounds.intersected(dst->rect());
    if (validDst.isEmpty()) {
        return;
    }

    // Direct memcpy burst path when fully opaque
    if (forceOpaque && opacity256 >= 256) {
        for (const QRect &r : clipRegion) {
            const QRect cRect = r.intersected(validDst);
            if (cRect.isEmpty()) {
                continue;
            }
            const int rx = cRect.x();
            const int ry = cRect.y();
            const int rw = cRect.width();
            const int rh = cRect.height();
            for (int y = ry; y < ry + rh; ++y) {
                const int sy = srcY + (y - dstY);
                const int sx = srcX + (rx - dstX);
                uint8_t *d = dst->scanLine(y) + static_cast<size_t>(rx) * 4;
                const uint8_t *s = src.constScanLine(sy) + static_cast<size_t>(sx) * 4;
                lindowsCopyRow128(d, s, static_cast<size_t>(rw) * 4);
            }
        }
        return;
    }

    // SIMD 4-pixel premultiplied alpha blend path
    for (const QRect &r : clipRegion) {
        const QRect cRect = r.intersected(validDst);
        if (cRect.isEmpty()) {
            continue;
        }
        const int rx = cRect.x();
        const int ry = cRect.y();
        const int rw = cRect.width();
        const int rh = cRect.height();

        for (int y = ry; y < ry + rh; ++y) {
            const int sy = srcY + (y - dstY);
            const int sx = srcX + (rx - dstX);
            uint32_t *dPtr = reinterpret_cast<uint32_t *>(dst->scanLine(y)) + rx;
            const uint32_t *sPtr = reinterpret_cast<const uint32_t *>(src.constScanLine(sy)) + sx;

            int x = 0;
#if defined(__SSE2__)
            for (; x + 4 <= rw; x += 4) {
                __m128i vsrc = _mm_loadu_si128(reinterpret_cast<const __m128i *>(sPtr + x));
                lindowsBlend4PixelsSSE2(dPtr + x, vsrc, opacity256, dstIsRGB32);
            }
#endif
            for (; x < rw; ++x) {
                dPtr[x] = blendPixelPremulScalar(sPtr[x], dPtr[x], opacity256, dstIsRGB32);
            }
        }
    }
}

// Kernel 2: 16.16 Fixed-Point Axis-Aligned Scaled Blit
inline void lindowsScaleBlitSimd(QImage *dst,
                                 const QRectF &dstRectF,
                                 const QImage &src,
                                 const QRectF &srcRectF,
                                 const QRegion &deviceClip,
                                 int opacity256 = 256,
                                 bool forceOpaque = false)
{
    if (Q_UNLIKELY(!dst || dst->isNull() || src.isNull() || deviceClip.isEmpty())) {
        return;
    }
    if (Q_UNLIKELY(dstRectF.width() <= 0.0 || dstRectF.height() <= 0.0
                || srcRectF.width() <= 0.0 || srcRectF.height() <= 0.0)) {
        return;
    }
    if (Q_UNLIKELY(opacity256 <= 0)) {
        return;
    }

    const bool dstIsRGB32 = (dst->format() == QImage::Format_RGB32);
    const int srcW = src.width();
    const int srcH = src.height();

    const double invDstW = 1.0 / dstRectF.width();
    const double invDstH = 1.0 / dstRectF.height();

    const int64_t du_fp = static_cast<int64_t>((srcRectF.width() * invDstW) * 65536.0);

    const QRect dstAlignedRect = dstRectF.toAlignedRect().intersected(dst->rect());
    if (dstAlignedRect.isEmpty()) {
        return;
    }

    for (const QRect &r : deviceClip) {
        const QRect clipped = r.intersected(dstAlignedRect);
        if (clipped.isEmpty()) {
            continue;
        }

        const int top = clipped.top();
        const int bottom = clipped.bottom();
        const int left = clipped.left();
        const int rw = clipped.width();

        for (int y = top; y <= bottom; ++y) {
            const double v_rel = (y + 0.5 - dstRectF.y()) * invDstH;
            const int64_t v_fp = static_cast<int64_t>((srcRectF.y() + v_rel * srcRectF.height()) * 65536.0);
            const int sv = std::clamp(static_cast<int>(v_fp >> 16), 0, srcH - 1);

            const uint32_t *srcRow = reinterpret_cast<const uint32_t *>(src.constScanLine(sv));
            uint32_t *dstRow = reinterpret_cast<uint32_t *>(dst->scanLine(y)) + left;

            const double u_rel_start = (left + 0.5 - dstRectF.x()) * invDstW;
            int64_t u_fp = static_cast<int64_t>((srcRectF.x() + u_rel_start * srcRectF.width()) * 65536.0);

            int x = 0;
            if (forceOpaque && opacity256 >= 256) {
#if defined(__SSE2__)
                for (; x + 4 <= rw; x += 4) {
                    int su0 = std::clamp(static_cast<int>((u_fp            ) >> 16), 0, srcW - 1);
                    int su1 = std::clamp(static_cast<int>((u_fp + du_fp    ) >> 16), 0, srcW - 1);
                    int su2 = std::clamp(static_cast<int>((u_fp + du_fp * 2) >> 16), 0, srcW - 1);
                    int su3 = std::clamp(static_cast<int>((u_fp + du_fp * 3) >> 16), 0, srcW - 1);
                    u_fp += du_fp * 4;

                    uint32_t p0 = srcRow[su0] | 0xFF000000;
                    uint32_t p1 = srcRow[su1] | 0xFF000000;
                    uint32_t p2 = srcRow[su2] | 0xFF000000;
                    uint32_t p3 = srcRow[su3] | 0xFF000000;

                    __m128i v = _mm_set_epi32(p3, p2, p1, p0);
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dstRow + x), v);
                }
#endif
                for (; x < rw; ++x) {
                    int su = std::clamp(static_cast<int>(u_fp >> 16), 0, srcW - 1);
                    u_fp += du_fp;
                    dstRow[x] = srcRow[su] | 0xFF000000;
                }
            } else {
#if defined(__SSE2__)
                for (; x + 4 <= rw; x += 4) {
                    int su0 = std::clamp(static_cast<int>((u_fp            ) >> 16), 0, srcW - 1);
                    int su1 = std::clamp(static_cast<int>((u_fp + du_fp    ) >> 16), 0, srcW - 1);
                    int su2 = std::clamp(static_cast<int>((u_fp + du_fp * 2) >> 16), 0, srcW - 1);
                    int su3 = std::clamp(static_cast<int>((u_fp + du_fp * 3) >> 16), 0, srcW - 1);
                    u_fp += du_fp * 4;

                    uint32_t p0 = srcRow[su0];
                    uint32_t p1 = srcRow[su1];
                    uint32_t p2 = srcRow[su2];
                    uint32_t p3 = srcRow[su3];

                    __m128i vsrc = _mm_set_epi32(p3, p2, p1, p0);
                    lindowsBlend4PixelsSSE2(dstRow + x, vsrc, opacity256, dstIsRGB32);
                }
#endif
                for (; x < rw; ++x) {
                    int su = std::clamp(static_cast<int>(u_fp >> 16), 0, srcW - 1);
                    u_fp += du_fp;
                    dstRow[x] = blendPixelPremulScalar(srcRow[su], dstRow[x], opacity256, dstIsRGB32);
                }
            }
        }
    }
}

// Master SIMD dispatch function: automatically chooses 1:1 blend or fixed-point scale blit
inline bool lindowsBlendOrScaleBlitSimd(QImage *dst,
                                       const QRectF &dstRectF,
                                       const QImage &src,
                                       const QRectF &srcRectF,
                                       const QRegion &deviceClip,
                                       int opacity256 = 256,
                                       bool forceOpaque = false)
{
    if (Q_UNLIKELY(!dst || dst->isNull() || src.isNull() || deviceClip.isEmpty())) {
        return false;
    }
    if (Q_UNLIKELY(dstRectF.width() <= 0.0 || dstRectF.height() <= 0.0
                || srcRectF.width() <= 0.0 || srcRectF.height() <= 0.0)) {
        return true;
    }
    if (Q_UNLIKELY(opacity256 <= 0)) {
        return true;
    }

    if (dst->format() != QImage::Format_ARGB32_Premultiplied && dst->format() != QImage::Format_RGB32) {
        return false;
    }
    if (src.format() != QImage::Format_ARGB32_Premultiplied && src.format() != QImage::Format_RGB32) {
        return false;
    }

    const bool isUnscaled = std::abs(dstRectF.width() - srcRectF.width()) < 1e-3
                         && std::abs(dstRectF.height() - srcRectF.height()) < 1e-3;

    if (isUnscaled) {
        const int dstX = static_cast<int>(std::round(dstRectF.x()));
        const int dstY = static_cast<int>(std::round(dstRectF.y()));
        const int srcX = static_cast<int>(std::round(srcRectF.x()));
        const int srcY = static_cast<int>(std::round(srcRectF.y()));
        const int width = static_cast<int>(std::round(dstRectF.width()));
        const int height = static_cast<int>(std::round(dstRectF.height()));

        lindowsBlendRegionSimd(dst, dstX, dstY, src, srcX, srcY, width, height, deviceClip, opacity256, forceOpaque);
        return true;
    }

    lindowsScaleBlitSimd(dst, dstRectF, src, srcRectF, deviceClip, opacity256, forceOpaque);
    return true;
}

} // namespace KWin
