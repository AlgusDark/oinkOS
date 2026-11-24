#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "lib/GfxRenderer/BitmapRowPlot.h"

namespace {

constexpr int kPanelW = 32;
constexpr int kPanelH = 24;
constexpr int kStride = kPanelW / 8;
constexpr int kBmpW = 17;
constexpr int kBmpH = 11;
constexpr int kRowBytes = (kBmpW + 3) / 4;
constexpr int kGuard = 16;

// Same mapping as rotateCoordinates in GfxRenderer.cpp.
std::pair<int, int> physical(int orientation, int x, int y) {
  switch (orientation) {
    case 0:
      return {y, kPanelH - 1 - x};
    case 1:
      return {kPanelW - 1 - x, kPanelH - 1 - y};
    case 2:
      return {kPanelW - 1 - y, x};
    case 3:
      return {x, y};
    default:
      return {0, 0};
  }
}

void screenSize(int orientation, int& width, int& height) {
  if (orientation == 0 || orientation == 2) {
    width = kPanelH;
    height = kPanelW;
  } else {
    width = kPanelW;
    height = kPanelH;
  }
}

uint8_t sourceValue(const uint8_t* row, int bmpX) {
  return static_cast<uint8_t>((row[bmpX / 4] >> (6 - ((bmpX * 2) % 8))) & 0x3);
}

void writePixel(uint8_t* frame, int phyX, int phyY, bool clearBit) {
  const int byteIndex = phyY * kStride + phyX / 8;
  const uint8_t mask = static_cast<uint8_t>(1u << (7 - (phyX % 8)));
  if (clearBit) {
    frame[byteIndex] = static_cast<uint8_t>(frame[byteIndex] & static_cast<uint8_t>(~mask));
  } else {
    frame[byteIndex] = static_cast<uint8_t>(frame[byteIndex] | mask);
  }
}

std::vector<uint8_t> makeImage() {
  std::vector<uint8_t> image(static_cast<size_t>(kBmpH * kRowBytes), 0);
  for (int y = 0; y < kBmpH; ++y) {
    uint8_t* row = image.data() + static_cast<size_t>(y * kRowBytes);
    for (int x = 0; x < kBmpW; ++x) {
      const uint8_t val = static_cast<uint8_t>((x * 3 + y * 5) & 3);
      const int shift = 6 - ((x & 3) << 1);
      row[x >> 2] = static_cast<uint8_t>(row[x >> 2] | (val << shift));
    }
  }
  return image;
}

struct Paint {
  int orientation;
  int originX;
  int originY;
  int cropPixX;
  int cropPixY;
  bool topDown;
  int clipLeft;
  int clipTop;
  int clipRight;
  int clipBottom;
  bitmapRow::Plane plane;
  bool absolute;
  bool transparent;
};

std::vector<uint8_t> render(const std::vector<uint8_t>& image, const Paint& paint, bool fast) {
  int screenW = 0;
  int screenH = 0;
  screenSize(paint.orientation, screenW, screenH);
  const auto origin = physical(paint.orientation, 0, 0);
  const auto next = physical(paint.orientation, 1, 0);
  const int stepX = next.first - origin.first;
  const int stepY = next.second - origin.second;

  std::vector<uint8_t> buffer(static_cast<size_t>(kStride * kPanelH + 2 * kGuard), 0xA5);
  uint8_t* frame = buffer.data() + kGuard;

  for (int bmpY = 0; bmpY < kBmpH - paint.cropPixY; ++bmpY) {
    int screenY = -paint.cropPixY + (paint.topDown ? bmpY : kBmpH - 1 - bmpY);
    screenY += paint.originY;
    if (screenY >= screenH) break;
    const uint8_t* row = image.data() + static_cast<size_t>(bmpY * kRowBytes);
    if (screenY < 0 || bmpY < paint.cropPixY) continue;

    if (fast) {
      const bitmapRow::Span span =
          bitmapRow::clipSpan(kBmpW, paint.cropPixX, paint.originX, screenY, screenW, paint.clipLeft, paint.clipTop,
                              paint.clipRight, paint.clipBottom);
      if (span.count <= 0) continue;
      const int screenX0 = span.bmpX0 - paint.cropPixX + paint.originX;
      const auto phy = physical(paint.orientation, screenX0, screenY);
      bitmapRow::plot(frame, kStride, phy.first, phy.second, stepX, stepY, row, span.bmpX0, span.count, paint.plane,
                      paint.absolute, paint.transparent);
      continue;
    }

    for (int bmpX = paint.cropPixX; bmpX < kBmpW - paint.cropPixX; ++bmpX) {
      const int screenX = bmpX - paint.cropPixX + paint.originX;
      if (screenX >= screenW) break;
      if (screenX < 0) continue;
      if (screenX < paint.clipLeft || screenY < paint.clipTop || screenX >= paint.clipRight ||
          screenY >= paint.clipBottom) {
        continue;
      }
      const auto phy = physical(paint.orientation, screenX, screenY);
      if (phy.first < 0 || phy.first >= kPanelW || phy.second < 0 || phy.second >= kPanelH) continue;

      const uint8_t val = sourceValue(row, bmpX);
      if (paint.transparent && val == 3) continue;
      bool write = false;
      bool clearBit = false;
      switch (paint.plane) {
        case bitmapRow::Plane::Bw:
          write = val < 3;
          clearBit = true;
          break;
        case bitmapRow::Plane::GrayLsb:
        case bitmapRow::Plane::GrayMsb: {
          const GrayPlanePixel pixel = grayPlanePixel(val, paint.plane == bitmapRow::Plane::GrayMsb, paint.absolute);
          write = pixel.write;
          clearBit = pixel.black;
          break;
        }
      }
      if (!write) continue;
      writePixel(frame, phy.first, phy.second, clearBit);
    }
  }
  return buffer;
}

}  // namespace

TEST(BitmapRowPlot, StepIsConstantForEveryOrientation) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    const auto origin = physical(orientation, 0, 0);
    const auto next = physical(orientation, 1, 0);
    const int stepX = next.first - origin.first;
    const int stepY = next.second - origin.second;
    for (int y = -3; y < kPanelW + 3; ++y) {
      for (int x = -3; x < kPanelH + 3; ++x) {
        const auto at = physical(orientation, x, y);
        const auto ahead = physical(orientation, x + 6, y);
        EXPECT_EQ(ahead.first, at.first + 6 * stepX);
        EXPECT_EQ(ahead.second, at.second + 6 * stepY);
      }
    }
  }
}

TEST(BitmapRowPlot, FastPathMatchesDrawPixel) {
  const std::vector<uint8_t> image = makeImage();
  const bitmapRow::Plane planes[] = {bitmapRow::Plane::Bw, bitmapRow::Plane::GrayLsb, bitmapRow::Plane::GrayMsb};

  for (int orientation = 0; orientation < 4; ++orientation) {
    int screenW = 0;
    int screenH = 0;
    screenSize(orientation, screenW, screenH);
    const int origins[][2] = {{0, 0}, {-4, 3}, {screenW - 2, -2}, {0, screenH}};
    const int crops[] = {0, 2};
    const int clips[][4] = {
        {0, 0, 32767, 32767},
        {3, 2, screenW - 2, screenH - 1},
        {5, 5, 5, screenH},
        {-5, -3, screenW + 4, screenH + 2},
    };

    for (int crop : crops) {
      for (const auto& origin : origins) {
        for (const auto& clip : clips) {
          for (bitmapRow::Plane plane : planes) {
            for (bool absolute : {false, true}) {
              for (bool transparent : {false, true}) {
                for (bool topDown : {true, false}) {
                  const Paint paint{orientation, origin[0], origin[1], crop,  crop,     topDown,    clip[0],
                                    clip[1],     clip[2],   clip[3],   plane, absolute, transparent};
                  const std::vector<uint8_t> expected = render(image, paint, false);
                  const std::vector<uint8_t> actual = render(image, paint, true);
                  ASSERT_EQ(actual, expected) << orientation << " origin " << origin[0] << "," << origin[1] << " crop "
                                              << crop << " plane " << static_cast<int>(plane) << " absolute "
                                              << absolute << " transparent " << transparent << " topDown " << topDown;
                }
              }
            }
          }
        }
      }
    }
  }
}
