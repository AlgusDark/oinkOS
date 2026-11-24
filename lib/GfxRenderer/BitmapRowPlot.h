#pragma once

#include <cstdint>

#include "BitmapHelpers.h"

// Unscaled 2-bit BMP rows. The caller passes the physical position of the first
// pixel and the step to the next logical x, both from rotateCoordinates.
// Bit phyY * strideBytes * 8 + phyX is MSB-first in that byte, matching drawPixel.
namespace bitmapRow {

enum class Plane : uint8_t { Bw, GrayLsb, GrayMsb };

struct Span {
  int bmpX0 = 0;
  int count = 0;
};

// Screen reject (x < 0 or x >= screenWidth) intersected with the drawPixel clip.
inline Span clipSpan(int bmpWidth, int cropPixX, int originX, int screenY, int screenWidth, int clipLeft, int clipTop,
                     int clipRight, int clipBottom) {
  if (screenY < clipTop || screenY >= clipBottom) return {};
  const int content0 = cropPixX;
  const int content1 = bmpWidth - cropPixX;
  if (content0 >= content1) return {};

  const int screenLo = clipLeft > 0 ? clipLeft : 0;
  const int screenHi = clipRight < screenWidth ? clipRight : screenWidth;
  if (screenLo >= screenHi) return {};

  int bmp0 = screenLo - originX + cropPixX;
  int bmp1 = screenHi - originX + cropPixX;
  if (bmp0 < content0) bmp0 = content0;
  if (bmp1 > content1) bmp1 = content1;
  if (bmp0 >= bmp1) return {};
  return {bmp0, bmp1 - bmp0};
}

inline void plot(uint8_t* buffer, int strideBytes, int phyX, int phyY, int stepX, int stepY, const uint8_t* row,
                 int bmpX0, int count, Plane plane, bool absoluteGray, bool transparentWhite) {
  const int strideBits = strideBytes * 8;
  int destination = phyY * strideBits + phyX;
  const int stepBits = stepY * strideBits + stepX;
  for (int i = 0; i < count; ++i, destination += stepBits) {
    const int bmpX = bmpX0 + i;
    const uint8_t val = static_cast<uint8_t>((row[bmpX >> 2] >> (6 - ((bmpX & 3) << 1))) & 0x3);
    if (transparentWhite && val == 3) continue;

    bool write = false;
    bool clearBit = false;
    switch (plane) {
      case Plane::Bw:
        write = val < 3;
        clearBit = true;
        break;
      case Plane::GrayLsb:
      case Plane::GrayMsb: {
        const GrayPlanePixel pixel = grayPlanePixel(val, plane == Plane::GrayMsb, absoluteGray);
        write = pixel.write;
        clearBit = pixel.black;
        break;
      }
    }
    if (!write) continue;

    const uint8_t mask = static_cast<uint8_t>(0x80u >> (destination & 7));
    uint8_t& stored = buffer[destination >> 3];
    if (clearBit) {
      stored = static_cast<uint8_t>(stored & static_cast<uint8_t>(~mask));
    } else {
      stored = static_cast<uint8_t>(stored | mask);
    }
  }
}

}  // namespace bitmapRow
