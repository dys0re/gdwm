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

} // namespace KWin
