// Synthesized drum voices (no samples -> fits the MCU's flash easily).
#pragma once
#include "dsp.h"

namespace pvg {

struct DrumParams {
  float tune = 0.5f, decay = 0.5f, tone = 0.5f, level = 0.8f;
  float drive = 0, crush = 0, pan = 0.5f, sweep = 0.5f, send = 0;
  float vel = 1;
  int variant = 0;
};

inline float squareOsc(float& ph, float inc) {
  ph += inc;
  if (ph >= 1.f) ph -= 1.f;
  return ph < 0.5f ? 1.f : -1.f;
}

class Kick {
 public:
  void init(float sr) { sr_ = sr; hp_.setLP(800.f, sr); }
  void trigger(const DrumParams& p) {
    int v = p.variant;
    f0_ = 32.f + p.tune * 60.f;
    float pdec = v == 0 ? 0.03f + p.sweep * 0.09f : 0.008f + p.sweep * 0.03f;
    sweep_ = v == 0 ? 2.5f + p.sweep * 4.f : 5.f + p.sweep * 9.f;
    pc_ = decayCoef(pdec, sr_);
    penv_ = 1.f;
    ac_ = decayCoef(0.12f + p.decay * 1.4f, sr_);
    amp_ = 1.f;
    click_ = p.tone * (v == 0 ? 0.6f : 1.2f);
    cc_ = decayCoef(0.005f, sr_);
    drive_ = 1.f + p.drive * 6.f;
    norm_ = 1.f / softclip(drive_);
    gain_ = p.level * p.vel;
    ph_ = 0;
    active_ = true;
  }
  float tick() {
    if (!active_) return 0;
    float f = f0_ * (1.f + sweep_ * penv_);
    penv_ *= pc_;
    ph_ += f / sr_;
    if (ph_ >= 1.f) ph_ -= 1.f;
    float s = std::sin(kTwoPi * ph_) * amp_;
    s += hp_.hp(rng_.bi()) * click_;
    click_ *= cc_;
    amp_ *= ac_;
    if (amp_ < 1e-4f) active_ = false;
    return softclip(s * drive_) * norm_ * gain_;
  }
  bool active() const { return active_; }

 private:
  float sr_ = 48000, f0_ = 50, sweep_ = 3, pc_ = 0.99f, penv_ = 0, ac_ = 0.999f, amp_ = 0;
  float click_ = 0, cc_ = 0.9f, drive_ = 1, norm_ = 1, gain_ = 1, ph_ = 0;
  bool active_ = false;
  Rng rng_;
  OnePole hp_;
};

class Snare {
 public:
  void init(float sr) { sr_ = sr; }
  void trigger(const DrumParams& p) {
    rim_ = p.variant == 1;
    if (rim_) {
      f1_ = 420.f + p.tune * 700.f;
      f2_ = f1_ * 2.31f;
      tc_ = decayCoef(0.012f + p.decay * 0.03f, sr_);
      nc_ = decayCoef(0.008f + p.decay * 0.02f, sr_);
      bp_.set(2500.f + p.tone * 3000.f, 2.f, sr_);
      toneAmt_ = 0.9f;
      noiseAmt_ = 0.35f + p.sweep * 0.4f;
    } else {
      f1_ = 150.f + p.tune * 150.f;
      f2_ = f1_ * 1.53f;
      tc_ = decayCoef(0.05f + p.decay * 0.15f, sr_);
      nc_ = decayCoef(0.08f + p.decay * 0.45f, sr_);
      bp_.set(1200.f + p.tone * 7000.f, 0.8f, sr_);
      toneAmt_ = 0.9f - p.sweep * 0.55f;
      noiseAmt_ = 0.35f + p.sweep * 0.7f;
    }
    pc_ = decayCoef(0.01f, sr_);
    penv_ = 1.f;
    tenv_ = nenv_ = 1.f;
    ph1_ = ph2_ = 0;
    drive_ = 1.f + p.drive * 5.f;
    norm_ = 1.f / softclip(drive_);
    gain_ = p.level * p.vel;
    active_ = true;
  }
  float tick() {
    if (!active_) return 0;
    float pm = 1.f + 0.4f * penv_;
    penv_ *= pc_;
    ph1_ += f1_ * pm / sr_; if (ph1_ >= 1.f) ph1_ -= 1.f;
    ph2_ += f2_ * pm / sr_; if (ph2_ >= 1.f) ph2_ -= 1.f;
    float t = (std::sin(kTwoPi * ph1_) + 0.6f * std::sin(kTwoPi * ph2_)) * tenv_ * toneAmt_;
    float n = (rim_ ? bp_.bp(rng_.bi()) : bp_.hp(rng_.bi())) * nenv_ * noiseAmt_;
    tenv_ *= tc_;
    nenv_ *= nc_;
    if (tenv_ < 1e-4f && nenv_ < 1e-4f) active_ = false;
    return softclip((t + n) * drive_) * norm_ * gain_;
  }

 private:
  float sr_ = 48000, f1_ = 180, f2_ = 280, tc_ = 0.99f, nc_ = 0.99f, pc_ = 0.99f;
  float penv_ = 0, tenv_ = 0, nenv_ = 0, ph1_ = 0, ph2_ = 0, toneAmt_ = 0.5f, noiseAmt_ = 0.5f;
  float drive_ = 1, norm_ = 1, gain_ = 1;
  bool active_ = false, rim_ = false;
  Rng rng_{0x1234567u};
  Svf bp_;
};

class Hat {
 public:
  void init(float sr) { sr_ = sr; hp_.setLP(7000.f, sr); }
  void trigger(const DrumParams& p) {
    static const float kRatios[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.f, 800.f};
    float scale = 0.6f + p.tune * 1.4f;
    for (int i = 0; i < 6; i++) inc_[i] = kRatios[i] * scale / sr_;
    float dec = p.variant == 1 ? 0.12f + p.decay * 0.9f : 0.015f + p.decay * 0.12f;
    ec_ = decayCoef(dec, sr_);
    env_ = 1.f;
    bp_.set(6000.f + p.tone * 6000.f, 1.2f, sr_);
    noise_ = p.sweep;
    gain_ = p.level * p.vel * 1.6f;
    drive_ = 1.f + p.drive * 4.f;
    norm_ = 1.f / softclip(drive_);
    active_ = true;
  }
  float tick() {
    if (!active_) return 0;
    float m = 0;
    for (int i = 0; i < 6; i++) m += squareOsc(ph_[i], inc_[i]);
    m *= (1.f / 6.f);
    float x = m * (1.f - noise_) + rng_.bi() * noise_;
    float y = hp_.hp(bp_.bp(x)) * env_;
    env_ *= ec_;
    if (env_ < 1e-4f) active_ = false;
    return softclip(y * drive_) * norm_ * gain_;
  }

 private:
  float sr_ = 48000, inc_[6] = {}, ph_[6] = {}, ec_ = 0.99f, env_ = 0, noise_ = 0;
  float gain_ = 1, drive_ = 1, norm_ = 1;
  bool active_ = false;
  Rng rng_{0xBEEF1234u};
  Svf bp_;
  OnePole hp_;
};

// Multi-model percussion: 0 clap, 1 tom, 2 cowbell, 3 zap
class Perc {
 public:
  void init(float sr) { sr_ = sr; }
  void trigger(const DrumParams& p) {
    model_ = p.variant;
    t_ = 0;
    env_ = 1.f;
    env2_ = 1.f;
    ph1_ = ph2_ = 0;
    gain_ = p.level * p.vel;
    drive_ = 1.f + p.drive * 5.f;
    norm_ = 1.f / softclip(drive_);
    switch (model_) {
      case 0:  // clap
        bp_.set(900.f + p.tone * 1800.f, 1.5f, sr_);
        ec_ = decayCoef(0.008f, sr_);
        tailC_ = decayCoef(0.08f + p.decay * 0.5f, sr_);
        burstGap_ = int((0.008f + p.sweep * 0.01f) * sr_);
        gain_ *= 2.2f;
        break;
      case 1:  // tom
        f1_ = 70.f + p.tune * 250.f;
        sweep_ = 0.3f + p.sweep * 1.5f;
        pc_ = decayCoef(0.05f, sr_);
        ec_ = decayCoef(0.1f + p.decay * 0.9f, sr_);
        noise_ = p.tone * 0.3f;
        bp_.set(2000.f, 0.7f, sr_);
        break;
      case 2:  // cowbell
        f1_ = 540.f * (0.6f + p.tune * 0.8f);
        f2_ = f1_ * 1.4815f;
        bp_.set(f2_ * (1.5f + p.tone * 2.f), 3.f, sr_);
        ec_ = decayCoef(0.05f + p.decay * 0.5f, sr_);
        pc_ = decayCoef(0.015f, sr_);
        gain_ *= 2.f;
        break;
      default:  // zap
        f1_ = 100.f + p.tune * 600.f;
        sweep_ = 6.f + p.sweep * 20.f;
        pc_ = decayCoef(0.01f + p.sweep * 0.08f, sr_);
        ec_ = decayCoef(0.05f + p.decay * 0.4f, sr_);
        break;
    }
    active_ = true;
  }
  float tick() {
    if (!active_) return 0;
    float y = 0;
    switch (model_) {
      case 0: {
        int burst = burstGap_ > 0 ? t_ / burstGap_ : 4;
        if (burst < 4) {
          if (t_ % burstGap_ == 0) env_ = 1.f;
          env_ *= ec_;
          y = bp_.bp(rng_.bi()) * env_;
        } else {
          env2_ *= tailC_;
          y = bp_.bp(rng_.bi()) * env2_ * 0.6f;
          if (env2_ < 1e-4f) active_ = false;
        }
        t_++;
        break;
      }
      case 1: {
        float f = f1_ * (1.f + sweep_ * env2_);
        env2_ *= pc_;
        ph1_ += f / sr_; if (ph1_ >= 1.f) ph1_ -= 1.f;
        y = (std::sin(kTwoPi * ph1_) + bp_.bp(rng_.bi()) * noise_) * env_;
        env_ *= ec_;
        if (env_ < 1e-4f) active_ = false;
        break;
      }
      case 2: {
        float a = squareOsc(ph1_, f1_ / sr_) + squareOsc(ph2_, f2_ / sr_);
        float e = 0.55f * env2_ + 0.45f * env_;
        env2_ *= pc_;
        env_ *= ec_;
        y = bp_.bp(a * 0.5f) * e;
        if (env_ < 1e-4f) active_ = false;
        break;
      }
      default: {
        float f = f1_ * (1.f + sweep_ * env2_);
        env2_ *= pc_;
        ph1_ += f / sr_; if (ph1_ >= 1.f) ph1_ -= 1.f;
        y = std::sin(kTwoPi * ph1_) * env_;
        env_ *= ec_;
        if (env_ < 1e-4f) active_ = false;
        break;
      }
    }
    return softclip(y * drive_) * norm_ * gain_;
  }

 private:
  float sr_ = 48000, env_ = 0, env2_ = 0, ec_ = 0.99f, pc_ = 0.99f, tailC_ = 0.99f;
  float f1_ = 200, f2_ = 300, ph1_ = 0, ph2_ = 0, sweep_ = 1, noise_ = 0;
  float gain_ = 1, drive_ = 1, norm_ = 1;
  int model_ = 0, t_ = 0, burstGap_ = 400;
  bool active_ = false;
  Rng rng_{0xC0FFEEu};
  Svf bp_;
};

}  // namespace pvg
