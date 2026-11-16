// Acid bass voice: monophonic saw/square through a resonant 4-pole low-pass
// with a decaying filter envelope, accent and slide.
//
// The voice has its own DIGITAL filter. It can also hand its signal to the
// Pro VS Mini's analog filter (CoolAudio V3320): render() with `pre` / `cvOct`
// then also writes the signal before the filter (VCA and drive already
// applied) and the cutoff as a CV, in octaves above 20 Hz.
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
  bool square = false;   // force square (ProVS Groove's OSC button)
  // extended controls (ProVS Acid)
  float morph = 0;       // 0 saw .. 1 square
  float sub = 0;         // square one octave down, 0..1
  int dist = 0;          // 0 soft, 1 hard, 2 fold, 3 fuzz
  float level = 1;       // output gain
  float cutMod = 0;      // extra cutoff in octaves (LFO, joystick)
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
  float reso() const { return resoNow_; }  // resonance the filter used last, 0..1

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

  // pre / cvOct (optional): signal for an external filter and its cutoff
  // (fc = 20 Hz * 2^cvOct). `out` is always the digitally filtered voice.
  void render(float* out, int n, float* pre = nullptr, float* cvOct = nullptr) {
    float amAtk = 1.f - std::exp(-1.f / (0.003f * sr_));
    float amRel = 1.f - std::exp(-1.f / (0.008f * sr_));
    float capUp = 1.f - std::exp(-1.f / (0.025f * sr_));
    float capDn = 1.f - std::exp(-1.f / (0.35f * sr_));
    float drive = 1.f + p_.drive * p_.drive * 10.f;
    float accAmt = accentOn_ ? p_.accent : 0.f;
    float shape = p_.square ? 1.f : clamp01(p_.morph);
    float sub = clamp01(p_.sub);
    float cutMul = std::exp2(p_.cutMod);
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
        float fc = baseHz_ * cutMul * std::exp2(oct);
        resoNow_ = clamp01(p_.reso * 0.93f + accCap_ * 0.05f);
        flt_.set(fc, resoNow_, sr_);
        cvNow_ = std::log2(clampf(fc, 20.f, 20000.f) / 20.f);
      }
      if (cvOct) cvOct[i] = cvNow_;
      // oscillator: polyBLEP saw or square
      phase_ += inc_;
      if (phase_ >= 1.f) phase_ -= 1.f;
      float saw = 2.f * phase_ - 1.f - blep(phase_, inc_);
      float x = saw;
      if (shape > 0) {
        float p2 = phase_ + 0.5f;
        if (p2 >= 1.f) p2 -= 1.f;
        float sq = (saw - (2.f * p2 - 1.f - blep(p2, inc_))) * 0.7f;
        x = saw + (sq - saw) * shape;
      }
      if (sub > 0) {  // sub square, one octave down
        subPh_ += inc_ * 0.5f;
        if (subPh_ >= 1.f) subPh_ -= 1.f;
        float s1 = 2.f * subPh_ - 1.f - blep(subPh_, inc_ * 0.5f);
        float p3 = subPh_ + 0.5f;
        if (p3 >= 1.f) p3 -= 1.f;
        x += (s1 - (2.f * p3 - 1.f - blep(p3, inc_ * 0.5f))) * 0.45f * sub;
      }
      x = dcHp_.hp(x);
      // analog route: VCA and drive come before the filter (the V3320 input)
      if (pre) pre[i] = shapeOut(x * 0.8f * drive, p_.dist) * amp_ * p_.level * 0.8f;
      float y = flt_.tick(x * 0.8f);
      y = hpOut_.hp(y) * amp_;
      out[i] = shapeOut(y * drive, p_.dist) * (1.1f / (0.6f + 0.5f * drive)) * p_.level;
    }
    if (!gated_ && amp_ < 1e-4f) accentOn_ = false;
  }

 private:
  static float shapeOut(float x, int mode) {
    switch (mode) {
      case 1: return clampf(x * 1.3f, -0.8f, 0.8f) * 1.2f;                     // hard clip
      case 2: return std::sin(clampf(x, -6.f, 6.f) * 1.4f) * 0.95f;             // wavefolder
      case 3: { float a = softclip(x * 1.6f + 0.35f) - softclip(0.35f);         // asymmetric fuzz
                return softclip(a * 1.4f) * 0.95f; }
      default: return softclip(x);
    }
  }
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
  float targetNote_ = 45, curNote_ = 45, slideK_ = 0, inc_ = 0, phase_ = 0, subPh_ = 0;
  float amp_ = 0, level_ = 0.7f, menv_ = 0, menvK_ = 0.999f, vegDec_ = 1, vegK_ = 0.9999f;
  float accCap_ = 0, baseHz_ = 500.f, resoNow_ = 0, cvNow_ = 5;
};

}  // namespace pvg
