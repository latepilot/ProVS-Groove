// Vector synth: 5 voices x 4 wavetable oscillators (A,B,C,D) mixed by the
// joystick position. Each oscillator has its own envelope (the mix moves over
// time) and each voice can have its own digital filter; then everything goes
// through ONE shared filter, like the Pro VS Mini's analog filter.
#pragma once
#include "dsp.h"

namespace pvg {

constexpr int kWaveLen = 256;
constexpr int kNumWaves = 32;
constexpr int kMips = 4;

void initWavetables();
const float* wavetable(int wave, int mip);
const char* waveName(int wave);

struct SynthParams {
  float att = 0.005f, dec = 0.3f, sus = 0.5f, rel = 0.2f;      // amp env (s / level)
  float fatt = 0.005f, fdec = 0.3f, fsus = 0.f, frel = 0.2f;   // filter env
  float detune = 0;        // 0..1
  int wave[4] = {2, 3, 12, 15};
  float tune[4] = {0, 0, 0, 0};  // semitones per oscillator
  float lfoRate = 1.f;     // Hz, LFO 1
  float lfoAmt = 0;        // 0..1, rotates the vector
  float lfo2Rate = 1.f;    // Hz, LFO 2
  float lfo2Amt = 0;       // 0..1, cutoff +-3 oct
  float cutoff = 0.5f;     // 0..1
  float reso = 0;          // 0..1
  float vx = 0, vy = 0;    // -1..1
  float fenv = 0.5f;       // 0..1
  // per-oscillator envelopes (s / level); the defaults leave the sound unchanged
  float oatt[4] = {0.0005f, 0.0005f, 0.0005f, 0.0005f};
  float odec[4] = {0.3f, 0.3f, 0.3f, 0.3f};
  float osus[4] = {1.f, 1.f, 1.f, 1.f};
  float orel[4] = {20.f, 20.f, 20.f, 20.f};
  // per-voice digital filter (its envelope uses the FIL ADSR)
  int vcfMode = 0;         // 0 off, 1 LP12, 2 LP24, 3 BP, 4 HP
  float vcfCut = 0.7f;     // 0..1
  float vcfRes = 0.2f;     // 0..1
  float vcfEnv = 0.3f;     // 0..1
};
constexpr int kVcfModes = 5;

class Synth {
 public:
  static constexpr int kVoices = 5;
  void init(float sr);
  // gateSamples < 0: held until noteOff()
  void noteOn(int note, float vel, int gateSamples);
  void noteOff(int note);
  void allOff();
  void control(const SynthParams& p, int blk);  // call once per control block
  void render(float* out, int n);
  int activeVoices() const;
  float vectorX() const { return curX_; }
  float vectorY() const { return curY_; }

 private:
  struct Voice {
    bool on = false, gated = false;
    int note = -1;
    float vel = 0;
    uint32_t ph[4] = {0, 0, 0, 0};
    uint32_t inc[4] = {0, 0, 0, 0};
    int mip = 0;
    int gateLeft = -1;
    uint32_t age = 0;
    Adsr env;
    Adsr oenv[4];   // per-oscillator envelopes
    Adsr venv;      // per-voice filter envelope
    Svf svf;
    Ladder lad;
  };
  void setPitch(Voice& v);
  void release(Voice& v);
  Voice v_[kVoices];
  Adsr fenv_;
  Ladder flt_;
  float sr_ = 48000.f;
  uint32_t ageCounter_ = 0;
  float lfoPh_ = 0, lfo2Ph_ = 0;
  float w_[4] = {0.25f, 0.25f, 0.25f, 0.25f};
  float ratio_[4] = {1, 1, 1, 1};
  int wave_[4] = {2, 3, 12, 15};
  float curX_ = 0, curY_ = 0;
  SynthParams p_;
};

}  // namespace pvg
