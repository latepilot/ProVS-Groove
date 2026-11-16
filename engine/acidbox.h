// ProVS Acid: a dedicated acid firmware for the PRO-VS MINI.
//
// Two monophonic acid lines (A = bass, B = counter line), a small drum lane
// (kick, clap, closed / open hat) and a pattern generator that writes lines
// by style, scale and key. Same panel and HAL contract as ProVS Groove:
//   - call process() from the audio DMA callback
//   - forward panel events; they are queued lock-free and applied in process()
//   - drawDisplay() -> OLED, stepLeds()/led() -> LEDs, takeSaveRequest() -> flash
#pragma once
#include <atomic>
#include <cstddef>
#include "acid.h"
#include "acidgen.h"
#include "acidmodel.h"
#include "display.h"
#include "drums.h"
#include "fx.h"
#include "panel.h"

namespace pvg {

// FUNC + step commands
enum AcidFunc : uint8_t {
  AF_GEN, AF_MUTATE, AF_RHYTHM, AF_NOTES, AF_ACCSLIDE, AF_UNDO, AF_STYLE, AF_SCALE,
  AF_EVOLVE, AF_ROTATE, AF_LEN, AF_COPY, AF_PASTE, AF_MUTE, AF_PATTERN, AF_SAVE
};
const char* acidFuncName(int step);

class AcidBox {
 public:
  using ProjectType = acid::Project;
  enum Lane : uint8_t { LANE_A, LANE_B, LANE_DRUMS };
  enum Layer : uint8_t { L_GATE, L_ACCENT, L_SLIDE, L_OCT };  // step-button layers (OSC selector)

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
  void syncPulse() { push({EV_SYNC, 0, 0, 0}); }
  bool popOutput(OutEvent& e);

  void drawDisplay(Display& d) const;
  uint16_t stepLeds() const;
  bool led(int l) const;
  bool takeSaveRequest() { return saveReq_.exchange(false); }
  const acid::Project& project() const { return proj_; }
  bool loadProject(const void* data, size_t n);

  // introspection for tests / simulator
  bool playing() const { return playing_; }
  int lane() const { return lane_; }
  int layer() const { return layer_; }
  int currentPattern() const { return proj_.current; }
  int queuedPattern() const { return queuedPattern_; }
  int stepPos(int lane) const { return stepPos_[lane]; }
  int transpose() const { return transpose_; }
  bool recMode() const { return recMode_; }
  int recPos() const { return recPos_; }
  bool muted(int lane) const { return mute_[lane]; }
  float tempo() const;
  bool waitingForClock() const { return playing_ && extArmed_; }
  uint64_t sampleClock() const { return clock_; }
  const acid::Line& line(int pattern, int l) const { return proj_.patterns[pattern].lines[l]; }
  const acid::Drums& drums(int pattern) const { return proj_.patterns[pattern].drums; }
  int lastNote(int l) const { return lastFired_[l]; }

 private:
  enum EvType : uint8_t { EV_STEP_DOWN, EV_STEP_UP, EV_KEY_DOWN, EV_KEY_UP, EV_BTN_DOWN, EV_BTN_UP, EV_POT, EV_JOY, EV_MIDI, EV_SYNC };
  enum Mode : uint8_t { M_NONE, M_MUTE, M_PATTERN, M_LEN, M_STYLE, M_SCALE, M_SETUP };
  struct Event { uint8_t type, a; float x, y; };
  static constexpr int kQ = 512;
  Event q_[kQ];
  std::atomic<uint32_t> qHead_{0}, qTail_{0};
  void push(const Event& e);
  void drain();
  void handle(const Event& e);

  void initProject();
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
  void onSetupStep(int s);
  bool held(int b) const { return (heldButtons_ >> b) & 1; }
  void msg(const char* title, const char* value = "", int bar = -1, const char* badge = "");
  void showStep(int s);
  int keyNote(int k) const { int n = kKeyBaseNote + 12 * octave_ + k - 12; return n < 0 ? 0 : (n > 127 ? 127 : n); }

  acid::Pattern& pat() { return proj_.patterns[proj_.current]; }
  const acid::Pattern& pat() const { return proj_.patterns[proj_.current]; }
  acid::GenCtx genCtx(int l) const;
  void backup();
  void genOp(int op);
  void selectPattern(int p);
  void selectLane(int l);
  int laneLength(int l) const;

  // transport / clock (same behaviour as ProVS Groove)
  void start();
  void startExt(bool fromTop);
  void stop();
  bool ext() const { return proj_.clockSrc != CLK_INT; }
  void emit(uint64_t t, uint8_t type, uint8_t b0 = 0);
  void outTick(uint64_t t, bool sendMidi = true);
  void onRef(int ticks);
  void extTick(uint64_t t);
  void controlTick(int blk);
  void startStep();
  void onBar();
  void computeStepDur();
  int stepSamples() const;
  void fireLine(int l, const acid::Step& s);
  void fireDrum(int inst, bool accent);
  AcidParams voiceParams(int l) const;
  void render(float* L, float* R, int n);

  float sr_ = 48000.f;
  acid::Project proj_;
  Acid voice_[acid::kLines];
  Kick kick_;
  Perc clap_;
  Hat hat_;
  Delay delay_;
  Rng rng_;
  uint64_t clock_ = 0;

  // sequencer
  bool playing_ = false;
  uint32_t globalStep_ = 0;
  int sampleInStep_ = 0, curStepDur_ = 6000;
  int stepPos_[3] = {};
  int queuedPattern_ = -1;
  bool mute_[3] = {};
  int transpose_ = 0;
  int barCount_ = 0;
  int lastFired_[acid::kLines] = {-1, -1};

  // UI
  uint32_t heldKeys_ = 0, heldButtons_ = 0;
  uint16_t heldSteps_ = 0, createdOnPress_ = 0, stepEdited_ = 0;
  bool funcUsed_ = false, recMode_ = false, keysTranspose_ = false;
  Mode mode_ = M_NONE;
  int lane_ = LANE_A, layer_ = L_GATE, octave_ = 0, recPos_ = 0, lastRecStep_ = -1;
  float joyX_ = 0, joyY_ = 0, joyLatch_ = 0;
  int skipStep_[acid::kLines] = {-1, -1};
  char title_[24] = {}, value_[16] = {}, badge_[8] = {};
  int bar_ = -1;
  uint64_t msgUntil_ = 0;
  // undo (one level, swaps) and clipboard
  acid::Line undoLine_[acid::kLines];
  acid::Drums undoDrums_;
  int undoPattern_ = -1;
  acid::Line clipLine_;
  acid::Drums clipDrums_;
  bool clipLineValid_ = false, clipDrumsValid_ = false;

  // clock in / out
  static constexpr int kOutQ = 512;
  OutEvent out_[kOutQ];
  std::atomic<uint32_t> oHead_{0}, oTail_{0};
  double intTickAcc_ = 0;
  uint32_t outTicks_ = 0;
  bool extArmed_ = false;
  uint32_t tickCount_ = 0;
  bool haveRef_ = false;
  uint64_t lastRefTime_ = 0;
  float refPeriod_ = 0;
  int refN_ = 1, segTicks_ = 1;
  float segTimer_ = 0;
  int pendingStep_ = -1;
  std::atomic<bool> saveReq_{false};
};

}  // namespace pvg
