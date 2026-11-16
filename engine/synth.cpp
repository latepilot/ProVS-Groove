#include "synth.h"

namespace pvg {

// ---------------------------------------------------------------- wavetables
// 32 original single-cycle waves, generated at boot (no copied ROM data).
// Each wave is band-limited into 4 mip levels via DFT.
static float g_tab[kNumWaves][kMips][kWaveLen];
static const int kMipHarm[kMips] = {64, 24, 10, 4};

static const char* kWaveNames[kNumWaves] = {
    "SINE", "TRI", "SAW", "SQR", "PW25", "PW10", "ORGN", "DRAW",
    "FOLD", "FLD2", "SYN1", "SYN2", "FM1", "FM2", "FM3", "VOXA",
    "VOXE", "VOXO", "PLS3", "STEP", "SQRT", "HALF", "CHEB", "SAW2",
    "BELL", "GLAS", "NOIS", "NOI2", "ODD", "BUZZ", "EVEN", "SPEC"};

const char* waveName(int w) { return (w >= 0 && w < kNumWaves) ? kWaveNames[w] : "?"; }
const float* wavetable(int w, int mip) { return g_tab[w][mip]; }

static float frac(float x) { return x - std::floor(x); }

static float harmonicSum(float x, int count, float (*amp)(int, uint32_t&), uint32_t seed) {
  uint32_t s = seed;
  float y = 0;
  for (int h = 1; h <= count; h++) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    float a = amp(h, s);
    float phase = (s >> 8) * (1.f / 16777216.f) * kTwoPi * (seed ? 1.f : 0.f);
    y += a * std::sin(h * x + phase);
  }
  return y;
}
static float gauss(float h, float c, float w) { float d = (h - c) / w; return std::exp(-d * d); }

static float rawWave(int w, float ph) {
  float x = kTwoPi * ph;
  float s = std::sin(x);
  switch (w) {
    case 0: return s;
    case 1: return 1.f - 4.f * std::fabs(ph - 0.5f);
    case 2: return 2.f * ph - 1.f;
    case 3: return ph < 0.5f ? 1.f : -1.f;
    case 4: return ph < 0.25f ? 1.f : -1.f;
    case 5: return ph < 0.1f ? 1.f : -1.f;
    case 6: return s + 0.5f * std::sin(2 * x) + 0.33f * std::sin(3 * x) + 0.25f * std::sin(4 * x) + 0.2f * std::sin(6 * x);
    case 7: return s + 0.7f * std::sin(3 * x) + 0.4f * std::sin(8 * x);
    case 8: return std::sin(3.f * s);
    case 9: return std::sin(7.f * s);
    case 10: return 2.f * frac(ph * 2.5f) - 1.f;
    case 11: return std::sin(kTwoPi * frac(ph * 3.3f)) * (1.f - ph);
    case 12: return std::sin(x + 2.f * std::sin(2 * x));
    case 13: return std::sin(x + 3.f * std::sin(3 * x));
    case 14: return std::sin(x + 1.5f * std::sin(5 * x));
    case 15: return harmonicSum(x, 40, [](int h, uint32_t&) { return gauss(h, 7, 2.5f) + 0.5f * gauss(h, 15, 3) + 0.3f / h; }, 0);
    case 16: return harmonicSum(x, 40, [](int h, uint32_t&) { return gauss(h, 3, 1.5f) + 0.6f * gauss(h, 18, 3) + 0.3f / h; }, 0);
    case 17: return harmonicSum(x, 40, [](int h, uint32_t&) { return gauss(h, 4, 1.5f) + 0.7f * gauss(h, 8, 2) + 0.3f / h; }, 0);
    case 18: return frac(ph * 3.f) < 0.08f ? 1.f : -0.1f;
    case 19: return std::floor(ph * 4.f) / 1.5f - 1.f;
    case 20: return (s < 0 ? -1.f : 1.f) * std::sqrt(std::fabs(s));
    case 21: return (s > 0 ? s : 0.f) * 2.f - 0.6f;
    case 22: return 16 * s * s * s * s * s - 20 * s * s * s + 5 * s;
    case 23: { float a = 2.f * ph - 1.f; return a * std::fabs(a); }
    case 24: return s + 0.6f * std::sin(4 * x) + 0.4f * std::sin(7 * x) + 0.3f * std::sin(11 * x);
    case 25: return s + 0.3f * std::sin(2 * x) + 0.5f * std::sin(5 * x) + 0.4f * std::sin(9 * x) + 0.3f * std::sin(13 * x);
    case 26: return harmonicSum(x, 48, [](int h, uint32_t& r) { return ((r >> 9) & 1023) / 1023.f / std::sqrt((float)h); }, 0x1111u);
    case 27: return harmonicSum(x, 48, [](int, uint32_t& r) { return ((r >> 9) & 1023) / 1023.f * 0.5f; }, 0x2222u);
    case 28: return harmonicSum(x, 31, [](int h, uint32_t&) { return (h & 1) ? 1.f / std::sqrt((float)h) : 0.f; }, 0);
    case 29: return harmonicSum(x, 40, [](int, uint32_t&) { return 1.f; }, 0);
    case 30: return harmonicSum(x, 32, [](int h, uint32_t&) { return (h == 1 || !(h & 1)) ? 1.f / h : 0.f; }, 0);
    default: return harmonicSum(x, 48, [](int, uint32_t& r) { return ((r >> 4) & 7) == 0 ? ((r >> 12) & 255) / 255.f : 0.f; }, 0x3333u) + 0.4f * s;
  }
}

void initWavetables() {
  static bool done = false;
  if (done) return;
  done = true;
  static float cosT[kWaveLen], sinT[kWaveLen];
  for (int i = 0; i < kWaveLen; i++) {
    cosT[i] = std::cos(kTwoPi * i / kWaveLen);
    sinT[i] = std::sin(kTwoPi * i / kWaveLen);
  }
  float raw[kWaveLen], re[65], im[65];
  for (int w = 0; w < kNumWaves; w++) {
    for (int i = 0; i < kWaveLen; i++) raw[i] = rawWave(w, float(i) / kWaveLen);
    for (int h = 1; h <= 64; h++) {
      float a = 0, b = 0;
      for (int i = 0; i < kWaveLen; i++) {
        int k = (h * i) & (kWaveLen - 1);
        a += raw[i] * cosT[k];
        b += raw[i] * sinT[k];
      }
      re[h] = a * 2.f / kWaveLen;
      im[h] = b * 2.f / kWaveLen;
    }
    float peak = 1e-6f;
    for (int m = 0; m < kMips; m++) {
      int H = kMipHarm[m];
      for (int i = 0; i < kWaveLen; i++) {
        float y = 0;
        for (int h = 1; h <= H; h++) {
          float sigma = 1.f;  // Lanczos sigma: tames Gibbs ringing
          float t = kPi * h / (H + 1);
          sigma = std::sin(t) / t;
          int k = (h * i) & (kWaveLen - 1);
          y += sigma * (re[h] * cosT[k] + im[h] * sinT[k]);
        }
        g_tab[w][m][i] = y;
        if (m == 0 && std::fabs(y) > peak) peak = std::fabs(y);
      }
    }
    float g = 1.f / peak;
    for (int m = 0; m < kMips; m++)
      for (int i = 0; i < kWaveLen; i++) g_tab[w][m][i] *= g;
  }
}

// ---------------------------------------------------------------- synth
void Synth::init(float sr) {
  sr_ = sr;
  initWavetables();
  for (auto& v : v_) v = Voice();
  flt_.reset();
}

void Synth::setPitch(Voice& v) {
  float f = mtof(float(v.note));
  float rmax = ratio_[0];
  for (int o = 1; o < 4; o++) if (ratio_[o] > rmax) rmax = ratio_[o];
  float ft = f * rmax;
  v.mip = ft < 300.f ? 0 : (ft < 800.f ? 1 : (ft < 1900.f ? 2 : 3));
  for (int o = 0; o < 4; o++) {
    double inc = double(f * ratio_[o]) / sr_ * 4294967296.0;
    v.inc[o] = uint32_t(inc);
  }
}

void Synth::noteOn(int note, float vel, int gateSamples) {
  Voice* tgt = nullptr;
  for (auto& v : v_) if (v.on && v.note == note) { tgt = &v; break; }
  if (!tgt) for (auto& v : v_) if (!v.on) { tgt = &v; break; }
  if (!tgt) {  // steal oldest
    tgt = &v_[0];
    for (auto& v : v_) if (v.age < tgt->age) tgt = &v;
  }
  Voice& v = *tgt;
  if (!v.on) for (int o = 0; o < 4; o++) v.ph[o] = uint32_t(o) * 0x40000000u;
  v.on = true;
  v.gated = true;
  v.note = note;
  v.vel = vel;
  v.gateLeft = gateSamples;
  v.age = ++ageCounter_;
  v.env.set(p_.att, p_.dec, p_.sus, p_.rel, sr_);
  v.env.gateOn();
  setPitch(v);
  fenv_.gateOn();
}

void Synth::noteOff(int note) {
  for (auto& v : v_)
    if (v.on && v.gated && v.note == note && v.gateLeft < 0) { v.gated = false; v.env.gateOff(); }
}

void Synth::allOff() {
  for (auto& v : v_) { v.gated = false; v.env.gateOff(); }
  fenv_.gateOff();
}

int Synth::activeVoices() const {
  int n = 0;
  for (auto& v : v_) n += v.on ? 1 : 0;
  return n;
}

void Synth::control(const SynthParams& p, int blk) {
  p_ = p;
  // detune spread in cents: A 0, B +d, C -d, D +2d
  float c = p.detune * 25.f;
  const float cents[4] = {0.f, c, -c, 2.f * c};
  for (int o = 0; o < 4; o++) {
    ratio_[o] = std::exp2((p.tune[o] * 100.f + cents[o]) / 1200.f);
    wave_[o] = p.wave[o];
  }
  // LFO spins the vector around the joystick position
  lfoPh_ += p.lfoRate * blk / sr_;
  if (lfoPh_ >= 1.f) lfoPh_ -= std::floor(lfoPh_);
  float x = clampf(p.vx + p.lfoAmt * std::sin(kTwoPi * lfoPh_), -1.f, 1.f);
  float y = clampf(p.vy + p.lfoAmt * std::cos(kTwoPi * lfoPh_), -1.f, 1.f);
  curX_ = x; curY_ = y;
  // diamond A(left) B(top) C(right) D(bottom) -> bilinear weights
  float s = clamp01((x + y + 1.f) * 0.5f);
  float t = clamp01((x - y + 1.f) * 0.5f);
  w_[0] = (1 - s) * (1 - t);
  w_[1] = s * (1 - t);
  w_[2] = s * t;
  w_[3] = (1 - s) * t;

  bool anyGate = false;
  for (auto& v : v_) {
    if (!v.on) continue;
    if (v.gated && v.gateLeft > 0) {
      v.gateLeft -= blk;
      if (v.gateLeft <= 0) { v.gated = false; v.env.gateOff(); }
    }
    v.env.set(p.att, p.dec, p.sus, p.rel, sr_);
    setPitch(v);
    anyGate |= v.gated;
  }
  if (!anyGate) fenv_.gateOff();
  fenv_.set(p.fatt, p.fdec, p.fsus, p.frel, sr_);
  float fe = 0;
  for (int i = 0; i < blk; i++) fe = fenv_.tick();
  lfo2Ph_ += p.lfo2Rate * blk / sr_;
  if (lfo2Ph_ >= 1.f) lfo2Ph_ -= std::floor(lfo2Ph_);
  float lfo2 = p.lfo2Amt * 3.f * std::sin(kTwoPi * lfo2Ph_);
  float cut = 20.f * std::exp2(p.cutoff * 10.f + p.fenv * 6.f * fe + lfo2);
  flt_.set(cut, p.reso, sr_);
}

void Synth::render(float* out, int n) {
  for (int i = 0; i < n; i++) {
    float sum = 0;
    for (auto& v : v_) {
      if (!v.on) continue;
      float m = 0;
      for (int o = 0; o < 4; o++) {
        if (w_[o] < 1e-4f) { v.ph[o] += v.inc[o]; continue; }
        const float* t = g_tab[wave_[o]][v.mip];
        uint32_t ph = v.ph[o];
        uint32_t idx = ph >> 24;
        float fr = (ph & 0xFFFFFF) * (1.f / 16777216.f);
        float a = t[idx], b = t[(idx + 1) & (kWaveLen - 1)];
        m += w_[o] * (a + (b - a) * fr);
        v.ph[o] = ph + v.inc[o];
      }
      float e = v.env.tick();
      if (!v.env.active()) { v.on = false; v.note = -1; }
      sum += m * e * v.vel;
    }
    out[i] = flt_.tick(sum * 0.32f);
  }
}

}  // namespace pvg
