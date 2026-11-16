// ProVS Groove: groovebox firmware core. Hardware-independent.
//
// Panel (as on the real Pro VS Mini):
//   16 step buttons with LEDs, 27-key capacitive keyboard, joystick,
//   PROG/OCTAVE -/+, selector buttons ENV (FIL/AMP/MIX), LFO (1/2), OSC (A-D)
//   with LEDs, MEMORY WRITE, PLAY/ACTIVE, FUNC, RECORD/CLEAR,
//   12 pots + TEMPO, 0.91" 128x32 OLED.
//
// HAL contract (what the real firmware / the simulator must provide):
//   - call process() from the audio DMA callback (stereo float, any block size)
//   - forward panel events (stepDown/Up, keyDown/Up, buttonDown/Up, pot,
//     joystick, midi); they are queued lock-free and applied in process()
//   - periodically: drawDisplay() -> OLED, stepLeds()/led() -> LED drivers
//   - when takeSaveRequest() is true, write project() to flash
#pragma once
#include <atomic>
#include <cstddef>
#include "display.h"
#include "drums.h"
#include "fx.h"
#include "model.h"
#include "synth.h"

namespace pvg {

enum Button : uint8_t {
  B_PROG_DN, B_PROG_UP, B_ENV_SEL, B_LFO_SEL, B_OSC_SEL,
  B_MEMWRITE, B_PLAY, B_FUNC, B_REC, B_COUNT
};
enum Pot : uint8_t {
  K_R1, K_R2, K_R3, K_R4, K_LFO_RATE, K_LFO_AMT, K_CUTOFF, K_RESO,
  K_WAVE, K_FREQ, K_CH_RATE, K_CH_AMT, K_TEMPO, K_COUNT
};
enum Led : uint8_t {
  LED_ENV_FIL, LED_ENV_AMP, LED_ENV_MIX, LED_LFO1, LED_LFO2,
  LED_OSC_A, LED_OSC_B, LED_OSC_C, LED_OSC_D, LED_PLAY, LED_REC, LED_FUNC, LED_COUNT
};
// FUNC + step button commands
enum FuncCmd : uint8_t {
  F_TRK1, F_TRK2, F_TRK3, F_TRK4, F_TRK5, F_MUTE, F_CLEAR_ALL, F_CLEAR_TRK,
  F_COPY, F_PASTE, F_LEN, F_EUCLID, F_DICE, F_PATTERN, F_KEYS, F_STORE
};
// Outgoing timing events, timestamped in samples (engine sample clock).
// OUT_MIDI: 1..3 MIDI bytes (clock 0xF8, start 0xFA, stop 0xFC) for USB MIDI.
// OUT_SYNC: rising edge for the SYNC jack (HAL keeps it high ~5 ms).
enum OutType : uint8_t { OUT_MIDI, OUT_SYNC };
struct OutEvent { uint64_t time; uint8_t type, len; uint8_t b[3]; };

constexpr int kNumKeys = 27;      // capacitive strip F..G (16 white + 11 black pads)
constexpr int kKeyBaseNote = 41;  // lowest key at octave 0 = F2
constexpr int kDrumBaseKey = 7;   // first C: plays a drum unpitched

const char* buttonName(int b);
const char* potName(int p);
const char* funcName(int step);
const char* trackName(int t);
const char* paramName(int track, int slot);
bool keyIsBlack(int k);

class Groovebox {
 public:
  void init(float sampleRate);
  void process(float* outL, float* outR, int n);

  void stepDown(int s) { push({EV_STEP_DOWN, uint8_t(s), 0, 0}); }
  void stepUp(int s) { push({EV_STEP_UP, uint8_t(s), 0, 0}); }
  void keyDown(int k) { push({EV_KEY_DOWN, uint8_t(k), 0, 0}); }
  void keyUp(int k) { push({EV_KEY_UP, uint8_t(k), 0, 0}); }
  void buttonDown(int b) { push({EV_BTN_DOWN, uint8_t(b), 0, 0}); }
  void buttonUp(int b) { push({EV_BTN_UP, uint8_t(b), 0, 0}); }
  void pot(int p, float v) { push({EV_POT, uint8_t(p), v, 0}); }
  void joystick(float x, float y) { push({EV_JOY, 0, x, y}); }
  void midi(uint8_t st, uint8_t d1, uint8_t d2) { push({EV_MIDI, st, float(d1), float(d2)}); }
  void syncPulse() { push({EV_SYNC, 0, 0, 0}); }  // rising edge on the SYNC jack
  bool popOutput(OutEvent& e);                     // drain from the HAL / sim

  void drawDisplay(Display& d) const;
  uint16_t stepLeds() const;
  bool led(int l) const;
  bool takeSaveRequest() { return saveReq_.exchange(false); }
  const Project& project() const { return proj_; }
  bool loadProject(const void* data, size_t n);

  // introspection for tests / simulator
  bool playing() const { return playing_; }
  int currentTrack() const { return cur_; }
  int currentPattern() const { return proj_.current; }
  int queuedPattern() const { return queuedPattern_; }
  int stepPos(int t) const { return stepPos_[t]; }
  bool recMode() const { return recMode_; }
  bool keysMode() const { return keysMode_; }
  bool muted(int t) const { return mute_[t]; }
  int bpm() const { return proj_.bpm; }
  float tempo() const;  // effective BPM (measured when synced externally), 0 = unknown
  int clockSource() const { return proj_.clockSrc; }
  bool waitingForClock() const { return playing_ && extArmed_; }
  const Step& step(int pattern, int track, int s) const { return proj_.patterns[pattern].tracks[track].steps[s]; }
  uint64_t sampleClock() const { return clock_; }

 private:
  enum EvType : uint8_t { EV_STEP_DOWN, EV_STEP_UP, EV_KEY_DOWN, EV_KEY_UP, EV_BTN_DOWN, EV_BTN_UP, EV_POT, EV_JOY, EV_MIDI, EV_SYNC };
  enum Mode : uint8_t { M_NONE, M_MUTE, M_PATTERN, M_LEN, M_EUCLID, M_SETUP };
  struct Event { uint8_t type, a; float x, y; };
  static constexpr int kQ = 512;
  Event q_[kQ];
  std::atomic<uint32_t> qHead_{0}, qTail_{0};
  void push(const Event& e);
  void drain();
  void handle(const Event& e);

  void initProject();
  void resetStep(Step& s);
  void loadDemo();

  void onStepDown(int s);
  void onStepUp(int s);
  void onFunc(int s);
  void onKeyDown(int k);
  void onKeyUp(int k);
  void onButtonDown(int b);
  void onButtonUp(int b);
  void onPot(int p, float v);
  void onJoy(float x, float y);
  void onMidi(uint8_t st, uint8_t d1, uint8_t d2);
  bool held(int b) const { return (heldButtons_ >> b) & 1; }
  void msg(const char* title, const char* value = "", int bar = -1, const char* badge = "");
  void showStep(int t, int s);
  int nearestStep(int t) const;
  void selectPattern(int p);
  void selectTrack(int t);
  void dice(int t);
  void euclid(int t, int hits);
  void clearTrack(Track& tr);
  void setLock(Step& s, int slot, int v);
  int potSlot(int p, int& track) const;
  void formatValue(int track, int slot, int v, char* out, int n) const;
  int keyNote(int k) const { int n = kKeyBaseNote + 12 * octave_ + k; return n < 0 ? 0 : (n > 127 ? 127 : n); }

  Pattern& pat() { return proj_.patterns[proj_.current]; }
  const Pattern& pat() const { return proj_.patterns[proj_.current]; }
  void start();
  void startExt(bool fromTop);
  void stop();
  bool ext() const { return proj_.clockSrc != CLK_INT; }
  void emit(uint64_t t, uint8_t type, uint8_t b0 = 0);
  void outTick(uint64_t t, bool sendMidi = true);  // one 24-PPQN tick of the outgoing clock
  void onRef(int ticks);           // external reference: MIDI F8 = 1 tick, sync pulse = 24/ppqn
  void extTick(uint64_t t);        // one incoming 24-PPQN tick
  void onSetupStep(int s);
  void controlTick(int blk);
  void startStep();
  void computeStepDur();
  int stepSamples() const;
  void fire(int t, const Step* st, float vel, bool ratchet);
  DrumParams drumParams(int t, const Step* st, float vel) const;
  SynthParams synthParams() const;
  void render(float* L, float* R, int n);

  float sr_ = 48000.f;
  Project proj_;
  Kick kick_;
  Snare snare_;
  Hat hat_;
  Perc perc_;
  Synth synth_;
  Delay delay_;
  struct Mix { float gl = 0.7f, gr = 0.7f, send = 0, crush = 0; Crusher cr; } mix_[4];
  Rng rng_;
  uint64_t clock_ = 0;

  bool playing_ = false;
  uint32_t globalStep_ = 0;
  int sampleInStep_ = 0, curStepDur_ = 6000;
  int stepPos_[kNumTracks] = {};
  int queuedPattern_ = -1;
  int ratchLeft_[kNumTracks] = {}, ratchInterval_[kNumTracks] = {}, ratchCounter_[kNumTracks] = {}, ratchStep_[kNumTracks] = {};
  float ratchVel_[kNumTracks] = {};
  int skipStep_[kNumTracks] = {-1, -1, -1, -1, -1};
  uint32_t synthLockMask_ = 0;
  uint8_t synthLock_[kNumParams] = {};
  int synthLockLeft_ = 0;
  bool mute_[kNumTracks] = {};

  uint32_t heldKeys_ = 0, heldButtons_ = 0;
  uint16_t heldSteps_ = 0, createdOnPress_ = 0, stepEdited_ = 0;
  bool funcUsed_ = false, keysMode_ = false, recMode_ = false;
  Mode mode_ = M_NONE;
  int cur_ = 0, envSel_ = 1, lfoSel_ = 0, oscSel_ = 0, octave_ = 0, lastNote_ = 45, lastRecStep_ = -1;
  float joyX_ = 0, joyY_ = 0, joyLatch_ = 0;
  char title_[24] = {}, value_[16] = {}, badge_[8] = {};
  int bar_ = -1;
  uint64_t msgUntil_ = 0;
  Track clipTrack_;
  // clock in / out
  static constexpr int kOutQ = 512;
  OutEvent out_[kOutQ];
  std::atomic<uint32_t> oHead_{0}, oTail_{0};
  double intTickAcc_ = 0;      // internal 24-PPQN clock-out accumulator
  uint32_t outTicks_ = 0;      // ticks sent since start (for sync pulse division)
  bool extArmed_ = false;      // playing, waiting for the first external tick
  uint32_t tickCount_ = 0;     // external ticks since the current step grid start
  bool haveRef_ = false;
  uint64_t lastRefTime_ = 0;
  float refPeriod_ = 0;        // samples between reference pulses (EMA), 0 = unknown
  int refN_ = 1, segTicks_ = 1;
  float segTimer_ = 0;
  int pendingStep_ = -1;       // swung step waiting to fire (samples), -1 = none
  bool clipTrackValid_ = false;
  int clipType_ = -1;
  std::atomic<bool> saveReq_{false};
};

}  // namespace pvg
