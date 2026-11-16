// Acid bass voice: monophonic saw/square through a resonant 4-pole low-pass
// with a decaying filter envelope, accent and slide.
//
// Unlike the vector synth this voice has its own DIGITAL filter: the Pro VS
// Mini's single analog filter belongs to the vector synth, so on hardware the
// acid line is rendered fully in software and mixed after it.
//
// Slide works like on the classic acid boxes: a slid note keeps its gate open
// into the next step, and a note that arrives while the gate is still open
// glides to the new pitch without retriggering the envelopes.
#pragma once
#include "dsp.h"

namespace pvg {

struct AcidParams {
  float cutoff = 0.4f;   // 0..1
  float reso = 0.6f;     // 0..1
  float envMod = 0.5f;   // 0..1
  float decay = 0.4f;    // 0..1
  float accent = 0.6f;   // 0..1
  float drive = 0.2f;    // 0..1
  float slide = 0.4f;    // 0..1 glide time
  float tune = 0;        // semitones
  bool square = false;
};

class Acid {
 public:
  void init(float sr) {
    sr_ = sr;
    dcHp_.setLP(25.f, sr);
    hpOut_.setLP(40.f, sr);
    reset();
  }
  void reset() {
    flt_.reset();
    gateLeft_ = 0;
    gated_ = false;
    amp_ = 0;
    menv_ = 0;
    accCap_ = 0;
    phase_ = 0;
  }
  // gateSamples < 0: held until noteOff(). Returns true when it slid (legato).
  bool noteOn(int note, float vel, bool accent, int gateSamples) {
    float target = float(note);
    bool legato = gated_;
    targetNote_ = target;
    if (!legato) {
      curNote_ = target;
      menv_ = 1.f;
      accentOn_ = accent;
      level_ = 0.55f + 0.45f * vel;
      vegDec_ = 1.f;
    } else if (accent) {
      accentOn_ = true;  // slid accents still push the accent circuit
    }
    note_ = note;
    gated_ = true;
    gateLeft_ = gateSamples;
    return legato;
  }
  void noteOff(int note) {
    if (note == note_ && gateLeft_ < 0) gated_ = false;
  }
  void allOff() { gated_ = false; gateLeft_ = 0; }
  bool gated() const { return gated_; }
  int note() const { return note_; }

  void control(const AcidParams& p, int blk) {
    p_ = p;
    if (gated_ && gateLeft_ >= 0) {
      gateLeft_ -= blk;
      if (gateLeft_ <= 0) { gated_ = false; gateLeft_ = 0; }
    }
    float glide = 0.012f + p.slide * p.slide * 0.25f;  // 12 ms .. 260 ms
    slideK_ = 1.f - std::exp(-float(blk) / (glide * sr_));
    curNote_ += (targetNote_ - curNote_) * slideK_;
    if (std::fabs(targetNote_ - curNote_) < 0.002f) curNote_ = targetNote_;
    inc_ = mtof(curNote_ + p.tune) / sr_;
    // filter envelope: accented notes always use the shortest decay
    float dsec = accentOn_ ? 0.18f : 0.18f + p.decay * p.decay * 2.3f;
    menvK_ = decayCoef(dsec, sr_);
    vegK_ = decayCoef(3.5f, sr_);
    float base = 45.f * std::exp2(p.cutoff * 7.2f);  // 45 Hz .. ~6.6 kHz
    baseHz_ = base;
    flt_.set(base, p.reso * 0.93f, sr_);
  }

  void render(float* out, int n) {
    float amAtk = 1.f - std::exp(-1.f / (0.003f * sr_));
    float amRel = 1.f - std::exp(-1.f / (0.008f * sr_));
    float capUp = 1.f - std::exp(-1.f / (0.025f * sr_));
    float capDn = 1.f - std::exp(-1.f / (0.35f * sr_));
    float drive = 1.f + p_.drive * p_.drive * 10.f;
    float accAmt = accentOn_ ? p_.accent : 0.f;
    for (int i = 0; i < n; i++) {
      // envelopes
      menv_ *= menvK_;
      vegDec_ *= vegK_;
      float ampT = gated_ ? vegDec_ * level_ * (1.f + 0.6f * accAmt) : 0.f;
      amp_ += (ampT - amp_) * (ampT > amp_ ? amAtk : amRel);
      // accent sweep: a capacitor fed by the envelope, slow to discharge, so
      // consecutive accents climb higher
      float capT = menv_ * accAmt;
      accCap_ += (capT - accCap_) * (capT > accCap_ ? capUp : capDn);
      // filter cutoff modulation every 8 samples (tan() is not free)
      if ((i & 7) == 0) {
        float oct = menv_ * p_.envMod * 4.5f + accCap_ * 3.f;
        float fc = baseHz_ * std::exp2(oct);
        flt_.set(fc, clamp01(p_.reso * 0.93f + accCap_ * 0.05f), sr_);
      }
      // oscillator: polyBLEP saw or square
      phase_ += inc_;
      if (phase_ >= 1.f) phase_ -= 1.f;
      float x = 2.f * phase_ - 1.f - blep(phase_, inc_);
      if (p_.square) {
        float p2 = phase_ + 0.5f;
        if (p2 >= 1.f) p2 -= 1.f;
        x = x - (2.f * p2 - 1.f - blep(p2, inc_));
        x *= 0.7f;
      }
      x = dcHp_.hp(x);
      float y = flt_.tick(x * 0.8f);
      y = hpOut_.hp(y) * amp_;
      out[i] = softclip(y * drive) * (1.1f / (0.6f + 0.5f * drive));
    }
    if (!gated_ && amp_ < 1e-4f) accentOn_ = false;
  }

 private:
  static float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
  }
  float sr_ = 48000.f;
  AcidParams p_;
  Ladder flt_;
  OnePole dcHp_, hpOut_;
  bool gated_ = false, accentOn_ = false;
  int gateLeft_ = 0, note_ = -1;
  float targetNote_ = 45, curNote_ = 45, slideK_ = 0, inc_ = 0, phase_ = 0;
  float amp_ = 0, level_ = 0.7f, menv_ = 0, menvK_ = 0.999f, vegDec_ = 1, vegK_ = 0.9999f;
  float accCap_ = 0, baseHz_ = 500.f;
};

}  // namespace pvg
