// Small DSP toolkit. Header-only, no allocation, float math only:
// portable to Cortex-M7 (STM32H750, single-precision FPU).
#pragma once
#include <cmath>
#include <cstdint>

namespace pvg {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float clamp01(float x) { return clampf(x, 0.f, 1.f); }
// tanh-like soft clipper (Pade), exact 1.0 at |x| >= 3
inline float softclip(float x) {
  x = clampf(x, -3.f, 3.f);
  return x * (27.f + x * x) / (27.f + 9.f * x * x);
}
inline float mtof(float n) { return 440.f * std::exp2((n - 69.f) * (1.f / 12.f)); }
// per-sample multiplier that decays to -60 dB in `sec` seconds
inline float decayCoef(float sec, float sr) { return std::exp(-6.9078f / (sec * sr)); }
inline float expMap(float x, float lo, float hi) { return lo * std::pow(hi / lo, x); }

struct Rng {
  uint32_t s = 0x9E3779B9u;
  uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  float uni() { return (next() >> 8) * (1.f / 16777216.f); }
  float bi() { return uni() * 2.f - 1.f; }
};

struct OnePole {
  float z = 0, a = 1;
  void setLP(float fc, float sr) { a = 1.f - std::exp(-kTwoPi * fc / sr); }
  float lp(float x) { z += a * (x - z); return z; }
  float hp(float x) { return x - lp(x); }
  void reset() { z = 0; }
};

// Topology-preserving state variable filter (Zavalishin / Simper)
struct Svf {
  float ic1 = 0, ic2 = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
  void set(float fc, float q, float sr) {
    fc = clampf(fc, 10.f, sr * 0.45f);
    float g = std::tan(kPi * fc / sr);
    k = 1.f / q;
    a1 = 1.f / (1.f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  void tick(float v0, float& lp, float& bp, float& hp) {
    float v3 = v0 - ic2;
    float v1 = a1 * ic1 + a2 * v3;
    float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.f * v1 - ic1;
    ic2 = 2.f * v2 - ic2;
    lp = v2; bp = v1; hp = v0 - k * v1 - v2;
  }
  float bp(float x) { float l, b, h; tick(x, l, b, h); return b; }
  float hp(float x) { float l, b, h; tick(x, l, b, h); return h; }
  float lp(float x) { float l, b, h; tick(x, l, b, h); return l; }
  void reset() { ic1 = ic2 = 0; }
};

struct Adsr {
  enum St : uint8_t { Idle, Att, Dec, Rel };
  St st = Idle;
  float v = 0, aInc = 0.01f, dCoef = 0.999f, sus = 0.5f, rCoef = 0.999f;
  void set(float a, float d, float s, float r, float sr) {
    aInc = 1.f / (a * sr);
    dCoef = decayCoef(d, sr);
    sus = s;
    rCoef = decayCoef(r, sr);
  }
  void gateOn() { st = Att; }
  void gateOff() { if (st != Idle) st = Rel; }
  bool active() const { return st != Idle; }
  float tick() {
    switch (st) {
      case Att: v += aInc; if (v >= 1.f) { v = 1.f; st = Dec; } break;
      case Dec: v = sus + (v - sus) * dCoef; break;
      case Rel: v *= rCoef; if (v < 1e-4f) { v = 0; st = Idle; } break;
      default: break;
    }
    return v;
  }
};

// 4-pole zero-delay-feedback ladder: stands in for the Pro VS Mini's analog
// low-pass in the simulator. On hardware the real analog filter is used and
// this block is replaced by cutoff/resonance CV outputs.
struct Ladder {
  float s[4] = {0, 0, 0, 0};
  float G = 0.5f, g1 = 1, k = 0;
  void set(float fc, float reso01, float sr) {
    fc = clampf(fc, 15.f, sr * 0.42f);
    float g = std::tan(kPi * fc / sr);
    G = g / (1.f + g);
    g1 = 1.f / (1.f + g);
    k = reso01 * 3.9f;
  }
  float tick(float x) {
    float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
    float S = (G3 * s[0] + G2 * s[1] + G * s[2] + s[3]) * g1;
    float u = (x * (1.f + 0.35f * k) - k * S) / (1.f + k * G4);
    u = softclip(u);
    for (int i = 0; i < 4; i++) {
      float v = (u - s[i]) * G;
      float y = v + s[i];
      s[i] = y + v;
      u = y;
    }
    return u;
  }
  void reset() { s[0] = s[1] = s[2] = s[3] = 0; }
};

}  // namespace pvg
