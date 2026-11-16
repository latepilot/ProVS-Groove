// 128x32 1-bit framebuffer in SSD1306 page layout, so on hardware the buffer can
// be DMA'd to the OLED as is. 128x32 (0.91") is inferred from photos of the
// stock UI (two text lines + a bar); confirm the controller on the board.
#pragma once
#include <cstdint>
#include <cstring>
#include "font5x7.h"

namespace pvg {

struct Display {
  static constexpr int W = 128, H = 32;
  uint8_t buf[W * H / 8];

  void clear() { std::memset(buf, 0, sizeof buf); }
  void set(int x, int y, bool on = true) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    uint8_t& b = buf[x + (y >> 3) * W];
    uint8_t m = uint8_t(1u << (y & 7));
    if (on) b |= m; else b &= uint8_t(~m);
  }
  bool get(int x, int y) const {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return false;
    return (buf[x + (y >> 3) * W] >> (y & 7)) & 1;
  }
  void flip(int x, int y) { set(x, y, !get(x, y)); }
  void fill(int x, int y, int w, int h, bool on = true) {
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) set(x + i, y + j, on);
  }
  void invert(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) flip(x + i, y + j);
  }
  void hline(int x, int y, int w, bool on = true) { for (int i = 0; i < w; i++) set(x + i, y, on); }
  void vline(int x, int y, int h, bool on = true) { for (int j = 0; j < h; j++) set(x, y + j, on); }
  void rect(int x, int y, int w, int h, bool on = true) {
    hline(x, y, w, on); hline(x, y + h - 1, w, on);
    vline(x, y, h, on); vline(x + w - 1, y, h, on);
  }
  // returns x after the text; 6*scale px per char, 8*scale px tall
  int text(int x, int y, const char* s, bool on = true, int scale = 1) {
    for (; *s; s++) {
      int c = (unsigned char)*s;
      if (c < 32 || c > 126) c = '?';
      const uint8_t* g = kFont5x7[c - 32];
      for (int col = 0; col < 5; col++)
        for (int row = 0; row < 8; row++)
          if ((g[col] >> row) & 1) fill(x + col * scale, y + row * scale, scale, scale, on);
      x += 6 * scale;
    }
    return x;
  }
  static int textWidth(const char* s, int scale = 1) {
    int n = 0;
    while (s[n]) n++;
    return n ? n * 6 * scale - scale : 0;
  }
};

}  // namespace pvg
