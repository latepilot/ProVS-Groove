// Effects: tempo-synced ping-pong delay (int16 buffer to save SRAM) and a crusher.
#pragma once
#include "dsp.h"

namespace pvg {

class Delay {
 public:
  static constexpr int kMax = 48000;  // 1 s at 48 kHz, 2 x 96 KB
  void init(float sr) {
    sr_ = sr;
    for (int i = 0; i < kMax; i++) l_[i] = r_[i] = 0;
    damp_.setLP(4500.f, sr);
    damp2_.setLP(4500.f, sr);
  }
  void setTime(int samples) { d_ = samples < 1 ? 1 : (samples >= kMax ? kMax - 1 : samples); }
  void setFeedback(float fb) { fb_ = clampf(fb, 0.f, 0.92f); }
  void tick(float in, float& outL, float& outR) {
    int rp = w_ - d_;
    if (rp < 0) rp += kMax;
    float rl = l_[rp] * (1.f / 32767.f);
    float rr = r_[rp] * (1.f / 32767.f);
    float nl = in + damp_.lp(rr) * fb_;
    float nr = damp2_.lp(rl) * fb_;
    l_[w_] = int16_t(clampf(nl, -1.f, 1.f) * 32767.f);
    r_[w_] = int16_t(clampf(nr, -1.f, 1.f) * 32767.f);
    if (++w_ >= kMax) w_ = 0;
    outL = rl;
    outR = rr;
  }

 private:
  int16_t l_[kMax], r_[kMax];
  int w_ = 0, d_ = 12000;
  float sr_ = 48000, fb_ = 0.4f;
  OnePole damp_, damp2_;
};

struct Crusher {
  float hold = 0;
  int cnt = 0;
  float apply(float x, float amount) {
    if (amount < 0.02f) return x;
    int factor = 1 + int(amount * 20.f);
    if (++cnt >= factor) { cnt = 0; hold = x; }
    float q = std::exp2(11.f - amount * 8.f);
    return std::round(hold * q) / q;
  }
};

}  // namespace pvg
