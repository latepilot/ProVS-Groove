#include "acidbox.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace pvg {
using namespace acid;

// ------------------------------------------------------------------ tables
static const char* kFuncNames[16] = {"GEN", "MUTATE", "RHYTHM", "NOTES", "ACC/SLD", "UNDO", "STYLE", "SCALE",
                                     "EVOLVE", "ROTATE", "LEN", "COPY", "PASTE", "MUTE", "PATTERN", "SAVE"};
const char* acidFuncName(int s) { return (s >= 0 && s < 16) ? kFuncNames[s] : "?"; }

static const char* kLaneNames[3] = {"LINE A", "LINE B", "DRUMS"};
static const char* kLayerNames[4] = {"GATE", "ACCENT", "SLIDE", "OCTAVE"};
static const char* kDrumNames[4] = {"KICK", "CLAP", "CL HAT", "OP HAT"};
static const char* kStyleShort[ST_COUNT] = {"CLAS", "ROLL", "HYPN", "STAB", "SYNC", "DEEP", "RAVE", "WILD"};
static const char* kScaleShort[SC_COUNT] = {"MIN", "PHR", "DOR", "HRM", "PEN", "BLU", "MAJ", "CHR"};
static const char* kDistNames[4] = {"SOFT", "HARD", "FOLD", "FUZZ"};
static const int kDelayDiv[6] = {1, 2, 3, 4, 6, 8};
static const char* kDelayDivNames[6] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
static const int kLfoDiv[8] = {1, 2, 4, 8, 16, 32, 64, 128};  // in 16ths
static const char* kLfoDivNames[8] = {"1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BARS", "4 BARS", "8 BARS"};
static const char* kLineParamNames[LP_COUNT] = {"CUTOFF", "RESONANCE", "ENV MOD", "DECAY", "ACCENT", "SLIDE TIME",
                                                "WAVE", "TUNE", "DRIVE", "SUB OSC", "LEVEL", "DISTORTION",
                                                "LFO RATE", "LFO>CUTOFF", "DELAY SEND"};
static const char* kDrumParamNames[DP_COUNT] = {"KICK TUNE", "KICK DECAY", "HAT DECAY", "DRUM LEVEL", "DRUM DRIVE", "DRUM SEND"};
static const char* kClockNames[3] = {"INTERNAL", "MIDI", "SYNC IN"};

static void noteStr(int n, char* out, int len) { std::snprintf(out, len, "%s%d", noteName(n), n / 12 - 1); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ------------------------------------------------------------------ queue
void AcidBox::push(const Event& e) {
  uint32_t h = qHead_.load(std::memory_order_relaxed);
  uint32_t n = (h + 1) % kQ;
  if (n == qTail_.load(std::memory_order_acquire)) return;
  q_[h] = e;
  qHead_.store(n, std::memory_order_release);
}

void AcidBox::drain() {
  uint32_t t = qTail_.load(std::memory_order_relaxed);
  while (t != qHead_.load(std::memory_order_acquire)) {
    Event e = q_[t];
    t = (t + 1) % kQ;
    qTail_.store(t, std::memory_order_release);
    handle(e);
  }
}

void AcidBox::handle(const Event& e) {
  switch (e.type) {
    case EV_STEP_DOWN: if (e.a < kSteps) onStepDown(e.a); break;
    case EV_STEP_UP: if (e.a < kSteps) onStepUp(e.a); break;
    case EV_KEY_DOWN: if (e.a < kNumKeys) onKeyDown(e.a); break;
    case EV_KEY_UP: if (e.a < kNumKeys) onKeyUp(e.a); break;
    case EV_BTN_DOWN: onButtonDown(e.a); break;
    case EV_BTN_UP: onButtonUp(e.a); break;
    case EV_POT: onPot(e.a, e.x); break;
    case EV_JOY: onJoy(e.x, e.y); break;
    case EV_MIDI: onMidi(e.a, uint8_t(e.x), uint8_t(e.y)); break;
    case EV_SYNC: if (proj_.clockSrc == CLK_SYNC) onRef(24 / kSyncPpqn[proj_.syncPpqn & 3]); break;
  }
}

// ------------------------------------------------------------------ setup
void AcidBox::init(float sr) {
  sr_ = sr;
  for (auto& v : voice_) v.init(sr);
  kick_.init(sr);
  clap_.init(sr);
  hat_.init(sr);
  delay_.init(sr);
  initProject();
  computeStepDur();
}

void AcidBox::initProject() {
  std::memset(&proj_, 0, sizeof proj_);
  proj_.magic = kMagic;
  proj_.version = kVersion;
  proj_.bpm = 126;
  proj_.delayTime = 2;
  proj_.delayFb = 45;
  proj_.clockSrc = CLK_INT;
  proj_.syncPpqn = 1;
  proj_.clockOut = 1;
  proj_.syncOut = 1;
  //                                   CUT RES ENV DEC ACC SLD WAV TUN DRV SUB LVL DST LFR LFA SND
  static const uint8_t kA[LP_COUNT] = {38, 96, 76, 44, 92, 45, 0, 64, 34, 0, 100, 0, 72, 0, 18};
  static const uint8_t kB[LP_COUNT] = {58, 82, 58, 34, 80, 40, 127, 64, 22, 0, 78, 0, 80, 0, 46};
  std::memcpy(proj_.line[0], kA, LP_COUNT);
  std::memcpy(proj_.line[1], kB, LP_COUNT);
  static const uint8_t kD[DP_COUNT] = {38, 58, 30, 100, 12, 8};
  std::memcpy(proj_.drum, kD, DP_COUNT);

  // starter patterns: one per style, each in its own key and scale
  struct Seed { uint8_t style, scale, root, dens, wild, swing; bool lineB; };
  static const Seed kSeeds[8] = {
      {ST_CLASSIC, SC_MINOR, 9, 64, 40, 50, false}, {ST_ROLLING, SC_PHRYG, 5, 70, 35, 54, false},
      {ST_HYPNO, SC_DORIAN, 2, 60, 30, 50, true},   {ST_RAVE, SC_HARM, 4, 72, 60, 50, true},
      {ST_STAB, SC_MINOR, 7, 64, 50, 52, false},    {ST_SYNCO, SC_BLUES, 0, 64, 45, 58, true},
      {ST_DEEP, SC_PENTA, 9, 56, 25, 50, false},    {ST_WILD, SC_CHROM, 6, 64, 90, 50, false}};
  Rng r;
  r.s = 0xAC1D0303u;
  for (int p = 0; p < kPatterns; p++) {
    acid::Pattern& pt = proj_.patterns[p];
    pt.swing = 50;
    pt.gen = GenSettings{ST_CLASSIC, SC_MINOR, 9, 64, 40, {0, 0, 0}};
    pt.drums.length = 16;
    for (int l = 0; l < kLines; l++) {
      pt.lines[l].length = 16;
      GenCtx c;
      c.gen = pt.gen;
      c.line = l;
      clearLine(pt.lines[l], baseNote(c));
    }
    if (p >= 8) continue;
    const Seed& s = kSeeds[p];
    pt.gen = GenSettings{s.style, s.scale, s.root, s.dens, s.wild, {0, 0, 0}};
    pt.swing = s.swing;
    GenCtx a;
    a.gen = pt.gen;
    generateLine(pt.lines[0], a, r);
    if (s.lineB) {
      GenCtx b;
      b.gen = pt.gen;
      b.line = 1;
      b.other = &pt.lines[0];
      generateLine(pt.lines[1], b, r);
    } else {
      GenCtx b;
      b.gen = pt.gen;
      b.line = 1;
      clearLine(pt.lines[1], baseNote(b));
    }
    generateDrums(pt.drums, pt.gen, r);
  }
  rng_.s = 0x5EED0303u;
}

bool AcidBox::loadProject(const void* data, size_t n) {
  if (n != sizeof(acid::Project)) return false;
  const acid::Project* p = static_cast<const acid::Project*>(data);
  if (p->magic != kMagic || p->version != kVersion) return false;
  stop();
  std::memcpy(&proj_, data, n);
  if (proj_.current >= kPatterns) proj_.current = 0;
  return true;
}

// ------------------------------------------------------------------ helpers
void AcidBox::msg(const char* title, const char* value, int bar, const char* badge) {
  std::snprintf(title_, sizeof title_, "%s", title);
  std::snprintf(value_, sizeof value_, "%s", value);
  std::snprintf(badge_, sizeof badge_, "%s", badge);
  bar_ = bar;
  msgUntil_ = clock_ + uint64_t(sr_ * 1.2f);
}

int AcidBox::laneLength(int l) const {
  int n = l < kLines ? pat().lines[l].length : pat().drums.length;
  return n < 1 || n > 16 ? 16 : n;
}

GenCtx AcidBox::genCtx(int l) const {
  GenCtx c;
  c.gen = pat().gen;
  c.line = l;
  c.other = &pat().lines[1 - l];
  return c;
}

void AcidBox::backup() {
  for (int l = 0; l < kLines; l++) undoLine_[l] = pat().lines[l];
  undoDrums_ = pat().drums;
  undoPattern_ = proj_.current;
}

void AcidBox::showStep(int s) {
  char title[24], val[16];
  if (lane_ == LANE_DRUMS) {
    const Drums& d = pat().drums;
    std::snprintf(title, sizeof title, "STEP %d%s", s + 1, (d.accent >> s) & 1 ? " ACC" : "");
    int n = 0;
    static const char kL[4] = {'K', 'C', 'H', 'O'};
    for (int i = 0; i < kDrumInst; i++)
      if ((d.hits[i] >> s) & 1) { val[n++] = kL[i]; val[n++] = ' '; }
    val[n ? n - 1 : 0] = 0;
    if (!n) std::snprintf(val, sizeof val, "-");
    msg(title, val);
    return;
  }
  const acid::Step& st = pat().lines[lane_].steps[s];
  int n = std::snprintf(title, sizeof title, "STEP %d", s + 1);
  if (st.accent) n += std::snprintf(title + n, sizeof title - n, " ACC");
  if (st.slide) n += std::snprintf(title + n, sizeof title - n, " SLD");
  if (st.len > 1) n += std::snprintf(title + n, sizeof title - n, " T%d", st.len);
  if (st.prob < 8) std::snprintf(title + n, sizeof title - n, " %d%%", st.prob * 100 / 8);
  if (!st.gate) std::snprintf(val, sizeof val, "REST");
  else {
    noteStr(st.pitch, val, sizeof val);
    if (st.oct) std::strncat(val, st.oct > 0 ? (st.oct > 1 ? " ++" : " UP") : " DN", sizeof val - std::strlen(val) - 1);
  }
  msg(title, val);
}

void AcidBox::selectPattern(int p) {
  if (p < 0 || p >= kPatterns) return;
  char v[8];
  std::snprintf(v, sizeof v, "P%02d", p + 1);
  if (playing_) { queuedPattern_ = p; msg("NEXT PATTERN", v); }
  else { proj_.current = uint8_t(p); queuedPattern_ = -1; msg("PATTERN", v); }
}

void AcidBox::selectLane(int l) {
  if (lane_ != l) heldSteps_ = createdOnPress_ = stepEdited_ = 0;
  lane_ = l;
  msg("LANE", kLaneNames[l]);
}

// ------------------------------------------------------------------ generator ops
void AcidBox::genOp(int op) {
  static const char* kOpNames[5] = {"GENERATE", "MUTATE", "NEW RHYTHM", "NEW NOTES", "ACC + SLIDE"};
  backup();
  char v[16];
  if (lane_ == LANE_DRUMS) {
    Drums& d = pat().drums;
    switch (op) {
      case AF_GEN: case AF_RHYTHM: generateDrums(d, pat().gen, rng_); break;
      case AF_MUTATE: mutateDrums(d, pat().gen, rng_, 0.2f); break;
      case AF_ACCSLIDE: regenDrumAccents(d, pat().gen, rng_); break;
      default: msg("DRUMS", "NO NOTES"); return;
    }
    std::snprintf(v, sizeof v, "DR %s", kStyleShort[pat().gen.style % ST_COUNT]);
  } else {
    Line& l = pat().lines[lane_];
    GenCtx c = genCtx(lane_);
    switch (op) {
      case AF_GEN: generateLine(l, c, rng_); break;
      case AF_MUTATE: mutate(l, c, rng_, 0.18f); break;
      case AF_RHYTHM: regenRhythm(l, c, rng_); break;
      case AF_NOTES: regenNotes(l, c, rng_); break;
      case AF_ACCSLIDE: regenAccSlide(l, c, rng_); break;
    }
    std::snprintf(v, sizeof v, "%c %s %s", 'A' + lane_, kStyleShort[pat().gen.style % ST_COUNT], noteName(pat().gen.root));
  }
  msg(kOpNames[op], v);
}

// ------------------------------------------------------------------ step buttons
void AcidBox::onStepDown(int w) {
  if (held(B_FUNC)) { funcUsed_ = true; onFunc(w); return; }
  char v[16];
  switch (mode_) {
    case M_MUTE:
      if (w < 3) { mute_[w] = !mute_[w]; msg(kLaneNames[w], mute_[w] ? "MUTED" : "ON"); }
      return;
    case M_PATTERN: selectPattern(w); mode_ = M_NONE; return;
    case M_LEN:
      if (lane_ == LANE_DRUMS) pat().drums.length = uint8_t(w + 1);
      else pat().lines[lane_].length = uint8_t(w + 1);
      std::snprintf(v, sizeof v, "%d", w + 1);
      msg("LENGTH", v);
      mode_ = M_NONE;
      return;
    case M_STYLE:
      if (w < ST_COUNT) { pat().gen.style = uint8_t(w); msg("STYLE", styleName(w)); mode_ = M_NONE; }
      return;
    case M_SCALE:
      if (w < SC_COUNT) {
        pat().gen.scale = uint8_t(w);
        std::snprintf(v, sizeof v, "%s %s", noteName(pat().gen.root), scaleName(w));
        msg("SCALE", v);
        mode_ = M_NONE;
      }
      return;
    case M_SETUP: onSetupStep(w); return;
    default: break;
  }
  if (recMode_ && !playing_ && lane_ != LANE_DRUMS) {  // step entry: jump the cursor
    recPos_ = w % laneLength(lane_);
    std::snprintf(v, sizeof v, "STEP %d", recPos_ + 1);
    msg("STEP ENTRY", v);
    return;
  }
  if (w >= laneLength(lane_)) {
    std::snprintf(v, sizeof v, "%d", laneLength(lane_));
    msg("LENGTH", v);
    return;
  }
  uint16_t bit = uint16_t(1u << w);
  heldSteps_ |= bit;
  stepEdited_ &= uint16_t(~bit);
  if (lane_ == LANE_DRUMS) {
    uint16_t& h = pat().drums.hits[layer_];
    if (!(h & bit)) { h |= bit; createdOnPress_ |= bit; }
    if (layer_ == DI_OH) pat().drums.hits[DI_CH] &= uint16_t(~bit);
    showStep(w);
    return;
  }
  acid::Step& st = pat().lines[lane_].steps[w];
  switch (layer_) {
    case L_GATE:
      if (!st.gate) { st.gate = 1; createdOnPress_ |= bit; }
      break;
    case L_ACCENT: st.accent = !st.accent; st.gate = 1; stepEdited_ |= bit; break;
    case L_SLIDE: st.slide = !st.slide; st.gate = 1; stepEdited_ |= bit; break;
    case L_OCT: st.oct = int8_t(st.oct == 0 ? 1 : (st.oct > 0 ? -1 : 0)); st.gate = 1; stepEdited_ |= bit; break;
  }
  showStep(w);
}

void AcidBox::onStepUp(int w) {
  uint16_t bit = uint16_t(1u << w);
  if (!(heldSteps_ & bit)) return;
  heldSteps_ &= uint16_t(~bit);
  bool tap = !(createdOnPress_ & bit) && !(stepEdited_ & bit);
  createdOnPress_ &= uint16_t(~bit);
  if (!tap) return;
  if (lane_ == LANE_DRUMS) pat().drums.hits[layer_] &= uint16_t(~bit);
  else if (layer_ == L_GATE) pat().lines[lane_].steps[w].gate = 0;
}

void AcidBox::onSetupStep(int s) {
  char v[16];
  if (s <= 2) {
    if (playing_) stop();
    proj_.clockSrc = uint8_t(s);
    refPeriod_ = 0;
    haveRef_ = false;
    msg("CLOCK SOURCE", kClockNames[s]);
  } else if (s >= 4 && s <= 7) {
    proj_.syncPpqn = uint8_t(s - 4);
    std::snprintf(v, sizeof v, "%d PPQN", kSyncPpqn[s - 4]);
    msg("SYNC JACK", v);
  } else if (s == 8) {
    proj_.clockOut = !proj_.clockOut;
    msg("MIDI CLOCK OUT", proj_.clockOut ? "ON" : "OFF");
  } else if (s == 9) {
    proj_.syncOut = !proj_.syncOut;
    msg("SYNC OUT", proj_.syncOut ? "ON" : "OFF");
  }
}

void AcidBox::onFunc(int s) {
  char v[16];
  switch (s) {
    case AF_GEN: case AF_MUTATE: case AF_RHYTHM: case AF_NOTES: case AF_ACCSLIDE: genOp(s); break;
    case AF_UNDO:
      if (undoPattern_ < 0) { msg("UNDO", "NOTHING"); break; }
      for (int l = 0; l < kLines; l++) {
        Line t = proj_.patterns[undoPattern_].lines[l];
        proj_.patterns[undoPattern_].lines[l] = undoLine_[l];
        undoLine_[l] = t;
      }
      { Drums t = proj_.patterns[undoPattern_].drums; proj_.patterns[undoPattern_].drums = undoDrums_; undoDrums_ = t; }
      msg("UNDO", "PRESS AGAIN=REDO");
      break;
    case AF_STYLE: mode_ = M_STYLE; break;
    case AF_SCALE: mode_ = M_SCALE; break;
    case AF_EVOLVE:
      proj_.evolve = proj_.evolve == 0 ? 2 : (proj_.evolve == 2 ? 4 : (proj_.evolve == 4 ? 8 : 0));
      if (proj_.evolve) std::snprintf(v, sizeof v, "EVERY %d BARS", proj_.evolve);
      else std::snprintf(v, sizeof v, "OFF");
      msg("EVOLVE", v);
      barCount_ = 0;
      break;
    case AF_ROTATE:
      backup();
      if (lane_ == LANE_DRUMS) {
        Drums& d = pat().drums;
        int L = laneLength(LANE_DRUMS);
        auto rot = [L](uint16_t m) {
          uint16_t r = 0;
          for (int i = 0; i < L; i++) if ((m >> i) & 1) r |= uint16_t(1u << ((i + 1) % L));
          return uint16_t(r | (m & ~((1u << L) - 1)));
        };
        for (auto& h : d.hits) h = rot(h);
        d.accent = rot(d.accent);
      } else {
        rotate(pat().lines[lane_], 1);
      }
      msg("ROTATE", kLaneNames[lane_]);
      break;
    case AF_LEN: mode_ = M_LEN; break;
    case AF_COPY:
      if (lane_ == LANE_DRUMS) { clipDrums_ = pat().drums; clipDrumsValid_ = true; }
      else { clipLine_ = pat().lines[lane_]; clipLineValid_ = true; }
      msg("COPY", kLaneNames[lane_]);
      break;
    case AF_PASTE:
      if (lane_ == LANE_DRUMS ? !clipDrumsValid_ : !clipLineValid_) { msg("PASTE", "EMPTY"); break; }
      backup();
      if (lane_ == LANE_DRUMS) pat().drums = clipDrums_;
      else pat().lines[lane_] = clipLine_;
      msg("PASTE", kLaneNames[lane_]);
      break;
    case AF_MUTE: mode_ = mode_ == M_MUTE ? M_NONE : M_MUTE; msg("MUTE MODE", mode_ == M_MUTE ? "ON" : "OFF"); break;
    case AF_PATTERN: mode_ = M_PATTERN; break;
    case AF_SAVE: saveReq_.store(true); msg("MEMORY", "SAVED"); break;
  }
}

// ------------------------------------------------------------------ keyboard
void AcidBox::onKeyDown(int k) {
  bool legato = heldKeys_ != 0;
  heldKeys_ |= 1u << k;
  int note = keyNote(k);
  char v[16];
  if (mode_ == M_SCALE) {
    pat().gen.root = uint8_t(note % 12);
    std::snprintf(v, sizeof v, "%s %s", noteName(note), scaleName(pat().gen.scale));
    msg("KEY", v);
    return;
  }
  if (lane_ == LANE_DRUMS) {
    int inst = k % kDrumInst;
    fireDrum(inst, held(B_FUNC));
    if (recMode_ && playing_) {
      int s = stepPos_[2];
      if (sampleInStep_ > curStepDur_ / 2) s = (s + 1) % laneLength(2);
      pat().drums.hits[inst] |= uint16_t(1u << s);
    }
    return;
  }
  Line& ln = pat().lines[lane_];
  Acid& vc = voice_[lane_];
  if (heldSteps_) {  // hold step + key = the step's note
    for (int s = 0; s < kSteps; s++) {
      if (!(heldSteps_ & (1u << s))) continue;
      acid::Step& st = ln.steps[s];
      st.gate = 1;
      st.pitch = uint8_t(note);
      st.oct = 0;
      stepEdited_ |= uint16_t(1u << s);
    }
    vc.allOff();
    vc.noteOn(note, 0.8f, false, int(sr_ * 0.2f));
    noteStr(note, v, sizeof v);
    msg("STEP NOTE", v);
    return;
  }
  if (keysTranspose_) {
    transpose_ = k - 7;  // the first C is 0
    std::snprintf(v, sizeof v, "%+d", transpose_);
    msg("TRANSPOSE", v);
    return;
  }
  if (recMode_ && !playing_) {  // step entry
    int L = laneLength(lane_);
    acid::Step& st = ln.steps[recPos_ % L];
    if (legato && recPos_ > 0) ln.steps[(recPos_ - 1) % L].slide = 1;
    st.gate = 1;
    st.pitch = uint8_t(note);
    st.oct = 0;
    st.accent = held(B_FUNC);
    st.slide = 0;
    st.len = 1;
    st.prob = 8;
    vc.allOff();
    vc.noteOn(note, 0.8f, st.accent, int(sr_ * 0.18f));
    char t[16];
    std::snprintf(t, sizeof t, "STEP %d%s", recPos_ % L + 1, st.accent ? " ACC" : "");
    noteStr(note, v, sizeof v);
    msg(t, v);
    recPos_ = (recPos_ + 1) % L;
    return;
  }
  vc.noteOn(note, 0.9f, held(B_FUNC), -1);  // legato: glides while another key is held
  if (!(recMode_ && playing_)) return;
  int L = laneLength(lane_);
  int s = stepPos_[lane_];
  if (sampleInStep_ > curStepDur_ / 2) s = (s + 1) % L;
  acid::Step& st = ln.steps[s];
  st.gate = 1;
  st.pitch = uint8_t(note);
  st.oct = 0;
  st.accent = held(B_FUNC);
  st.len = 1;
  st.prob = 8;
  if (legato && lastRecStep_ >= 0 && lastRecStep_ != s) ln.steps[lastRecStep_].slide = 1;
  lastRecStep_ = s;
  if (s != stepPos_[lane_]) skipStep_[lane_] = s;
}

void AcidBox::onKeyUp(int k) {
  heldKeys_ &= ~(1u << k);
  if (heldKeys_ == 0 && lane_ != LANE_DRUMS) voice_[lane_].noteOff(voice_[lane_].note());
}

// ------------------------------------------------------------------ buttons
void AcidBox::onButtonDown(int b) {
  if (b < 0 || b >= B_COUNT) return;
  heldButtons_ |= 1u << b;
  bool fn = held(B_FUNC) && b != B_FUNC;
  if (fn) funcUsed_ = true;
  int firstHeld = -1;
  for (int s = 0; s < kSteps; s++) if (heldSteps_ & (1u << s)) { firstHeld = s; break; }
  bool lineLane = lane_ != LANE_DRUMS;
  auto forHeld = [&](auto f) {
    for (int s = 0; s < kSteps; s++)
      if (heldSteps_ & (1u << s)) { f(s); stepEdited_ |= uint16_t(1u << s); }
  };
  char v[16];
  switch (b) {
    case B_FUNC: funcUsed_ = false; break;
    case B_PLAY:
      if (playing_) stop();
      else { start(); if (ext()) msg("WAITING FOR", proj_.clockSrc == CLK_MIDI ? "MIDI CLK" : "SYNC"); }
      break;
    case B_REC:
      if (fn) {
        backup();
        if (lineLane) { GenCtx c = genCtx(lane_); clearLine(pat().lines[lane_], baseNote(c)); }
        else { for (auto& h : pat().drums.hits) h = 0; pat().drums.accent = 0; }
        msg("CLEAR", kLaneNames[lane_]);
        break;
      }
      recMode_ = !recMode_;
      recPos_ = 0;
      lastRecStep_ = -1;
      msg("RECORD", recMode_ ? (playing_ ? "LIVE" : (lineLane ? "STEP ENTRY" : "PLAY TO REC")) : "OFF");
      break;
    case B_MEMWRITE:
      if (fn) { mode_ = mode_ == M_SETUP ? M_NONE : M_SETUP; break; }
      if (mode_ == M_SETUP) { mode_ = M_NONE; break; }
      saveReq_.store(true);
      msg("MEMORY", "SAVED");
      break;
    case B_PROG_DN:
    case B_PROG_UP: {
      int d = b == B_PROG_UP ? 1 : -1;
      if (firstHeld >= 0) {
        if (!lineLane) break;
        Line& ln = pat().lines[lane_];
        if (fn) forHeld([&](int s) { ln.steps[s].len = uint8_t(clampi(ln.steps[s].len + d, 1, 4)); });
        else forHeld([&](int s) { ln.steps[s].oct = int8_t(clampi(ln.steps[s].oct + d, -1, 2)); ln.steps[s].gate = 1; });
        showStep(firstHeld);
      } else if (recMode_ && !playing_ && lineLane) {  // step entry: + = rest, - = back
        int L = laneLength(lane_);
        if (d > 0) { pat().lines[lane_].steps[recPos_ % L].gate = 0; recPos_ = (recPos_ + 1) % L; }
        else recPos_ = (recPos_ + L - 1) % L;
        std::snprintf(v, sizeof v, "STEP %d", recPos_ + 1);
        msg(d > 0 ? "REST" : "BACK", v);
      } else if (fn) {
        octave_ = clampi(octave_ + d, -1, 2);
        std::snprintf(v, sizeof v, "%+d", octave_);
        msg("OCTAVE", v);
      } else {
        int p = (queuedPattern_ >= 0 ? queuedPattern_ : proj_.current) + d;
        selectPattern((p + kPatterns) % kPatterns);
      }
      break;
    }
    case B_ENV_SEL:
      if (firstHeld >= 0) {
        if (lineLane) forHeld([&](int s) { acid::Step& st = pat().lines[lane_].steps[s]; st.accent = !st.accent; st.gate = 1; });
        else forHeld([&](int s) { pat().drums.accent ^= uint16_t(1u << s); });
        showStep(firstHeld);
      } else {
        selectLane((lane_ + 1) % 3);
      }
      break;
    case B_LFO_SEL:
      if (firstHeld >= 0 && lineLane) {
        forHeld([&](int s) { acid::Step& st = pat().lines[lane_].steps[s]; st.slide = !st.slide; st.gate = 1; });
        showStep(firstHeld);
      } else {
        keysTranspose_ = !keysTranspose_;
        msg("KEYBOARD", keysTranspose_ ? "TRANSPOSE" : "PLAY");
      }
      break;
    case B_OSC_SEL:
      if (firstHeld >= 0 && lineLane) {
        forHeld([&](int s) { acid::Step& st = pat().lines[lane_].steps[s]; st.prob = uint8_t(st.prob <= 2 ? 8 : st.prob - 2); });
        showStep(firstHeld);
      } else {
        layer_ = (layer_ + 1) % 4;
        msg(lineLane ? "STEP LAYER" : "DRUM", lineLane ? kLayerNames[layer_] : kDrumNames[layer_]);
      }
      break;
  }
}

void AcidBox::onButtonUp(int b) {
  if (b < 0 || b >= B_COUNT) return;
  heldButtons_ &= ~(1u << b);
  if (b == B_FUNC) {
    joyLatch_ = 0;
    if (!funcUsed_ && mode_ != M_NONE) { mode_ = M_NONE; msg("MODE", "EXIT"); }
  }
}

// ------------------------------------------------------------------ pots
void AcidBox::onPot(int p, float v) {
  v = clamp01(v);
  int v7 = int(v * 127.f + 0.5f);
  bool fn = held(B_FUNC);
  if (fn) funcUsed_ = true;
  char vs[16];
  switch (p) {
    case K_TEMPO:
      if (!fn && ext()) { msg("TEMPO", "EXT CLOCK"); return; }
      if (fn) { pat().swing = uint8_t(50 + v * 25.f + 0.5f); std::snprintf(vs, sizeof vs, "%d%%", pat().swing); msg("SWING", vs, (pat().swing - 50) * 127 / 25); }
      else { proj_.bpm = uint8_t(60 + v * 140.f + 0.5f); std::snprintf(vs, sizeof vs, "%d", proj_.bpm); msg("TEMPO", vs, (proj_.bpm - 60) * 127 / 140); }
      return;
    case K_CH_RATE:
      if (fn) { proj_.delayFb = uint8_t(v7); std::snprintf(vs, sizeof vs, "%d", v7); msg("DELAY FEEDBACK", vs, v7); }
      else { proj_.delayTime = uint8_t(v * 5.99f); msg("DELAY TIME", kDelayDivNames[proj_.delayTime], proj_.delayTime * 127 / 5); }
      return;
    default: break;
  }
  if (lane_ == LANE_DRUMS) {
    int dp = -1;
    switch (p) {
      case K_R1: dp = DP_KICK_TUNE; break;
      case K_R2: dp = DP_KICK_DECAY; break;
      case K_R3: dp = DP_HAT_DECAY; break;
      case K_R4: dp = DP_LEVEL; break;
      case K_WAVE: dp = DP_DRIVE; break;
      case K_CH_AMT: dp = DP_SEND; break;
      default: msg("DRUMS", "NO SUCH KNOB"); return;
    }
    proj_.drum[dp] = uint8_t(v7);
    std::snprintf(vs, sizeof vs, "%d", v7);
    msg(kDrumParamNames[dp], vs, v7);
    return;
  }
  int lp = -1;
  switch (p) {
    case K_R1: lp = fn ? LP_DRIVE : LP_CUTOFF; break;
    case K_R2: lp = fn ? LP_SUB : LP_RESO; break;
    case K_R3: lp = fn ? LP_LEVEL : LP_ENVMOD; break;
    case K_R4: lp = LP_DECAY; break;
    case K_LFO_RATE: lp = LP_LFO_RATE; break;
    case K_LFO_AMT: lp = LP_LFO_AMT; break;
    case K_CUTOFF: lp = LP_ACCENT; break;
    case K_RESO: lp = LP_SLIDE; break;
    case K_WAVE: lp = fn ? LP_DIST : LP_WAVE; break;
    case K_FREQ: lp = LP_TUNE; break;
    case K_CH_AMT: lp = LP_SEND; break;
    default: return;
  }
  proj_.line[lane_][lp] = uint8_t(v7);
  int bar = v7;
  switch (lp) {
    case LP_TUNE: std::snprintf(vs, sizeof vs, "%+d", int(std::lround((v7 - 64) * 12.f / 63.f))); break;
    case LP_WAVE: std::snprintf(vs, sizeof vs, v7 < 4 ? "SAW" : (v7 > 123 ? "SQUARE" : "MIX %d"), v7 * 100 / 127); break;
    case LP_DIST: std::snprintf(vs, sizeof vs, "%s", kDistNames[v7 * 4 / 128]); bar = -1; break;
    case LP_LFO_RATE: std::snprintf(vs, sizeof vs, "%s", kLfoDivNames[v7 * 8 / 128]); break;
    default: std::snprintf(vs, sizeof vs, "%d", v7); break;
  }
  char title[24];
  std::snprintf(title, sizeof title, "%c %s", 'A' + lane_, kLineParamNames[lp]);
  msg(title, vs, bar);
}

void AcidBox::onJoy(float x, float y) {
  joyX_ = clampf(x, -1.f, 1.f);
  joyY_ = clampf(y, -1.f, 1.f);
  if (!held(B_FUNC)) return;
  // FUNC + joystick = generator density (X) and wildness (Y); the furthest
  // deflection stays when the stick springs back
  float mag = std::fabs(joyX_) > std::fabs(joyY_) ? std::fabs(joyX_) : std::fabs(joyY_);
  if (mag < 0.1f || mag < joyLatch_) return;
  joyLatch_ = mag;
  funcUsed_ = true;
  pat().gen.density = uint8_t(clampi(int((joyX_ + 1.f) * 63.5f + 0.5f), 0, 127));
  pat().gen.wild = uint8_t(clampi(int((joyY_ + 1.f) * 63.5f + 0.5f), 0, 127));
  char v[16];
  std::snprintf(v, sizeof v, "D%d W%d", pat().gen.density * 100 / 127, pat().gen.wild * 100 / 127);
  msg("GEN DENSITY WILD", v);
}

void AcidBox::onMidi(uint8_t st, uint8_t d1, uint8_t d2) {
  uint8_t type = st & 0xF0;
  bool midiClk = proj_.clockSrc == CLK_MIDI;
  int l = lane_ == LANE_DRUMS ? 0 : lane_;
  if (type == 0x90 && d2 > 0) voice_[l].noteOn(d1, 0.5f + d2 / 254.f, d2 > 100, -1);
  else if (type == 0x80 || (type == 0x90 && d2 == 0)) voice_[l].noteOff(d1);
  else if (st == 0xF8) { if (midiClk) onRef(1); }
  else if (st == 0xFA) { if (midiClk) startExt(true); }
  else if (st == 0xFB) { if (midiClk) startExt(false); }
  else if (st == 0xFC) { if (midiClk && playing_) stop(); }
  else if (st == 0xF2) { if (midiClk && !playing_) globalStep_ = uint32_t(d1 | (d2 << 7)) % 720720u; }
}

// ------------------------------------------------------------------ clock in / out
bool AcidBox::popOutput(OutEvent& e) {
  uint32_t t = oTail_.load(std::memory_order_relaxed);
  if (t == oHead_.load(std::memory_order_acquire)) return false;
  e = out_[t];
  oTail_.store((t + 1) % kOutQ, std::memory_order_release);
  return true;
}

void AcidBox::emit(uint64_t t, uint8_t type, uint8_t b0) {
  uint32_t h = oHead_.load(std::memory_order_relaxed);
  uint32_t n = (h + 1) % kOutQ;
  if (n == oTail_.load(std::memory_order_acquire)) return;
  out_[h] = OutEvent{t, type, uint8_t(type == OUT_MIDI ? 1 : 0), {b0, 0, 0}};
  oHead_.store(n, std::memory_order_release);
}

void AcidBox::outTick(uint64_t t, bool sendMidi) {
  if (proj_.clockOut && sendMidi) emit(t, OUT_MIDI, 0xF8);
  if (proj_.syncOut && proj_.clockSrc != CLK_SYNC) {
    uint32_t div = 24u / kSyncPpqn[proj_.syncPpqn & 3];
    if (outTicks_ % div == 0) emit(t, OUT_SYNC);
  }
  outTicks_++;
}

float AcidBox::tempo() const {
  if (!ext()) return proj_.bpm;
  if (refPeriod_ <= 0) return 0;
  return sr_ * 60.f / (refPeriod_ / refN_ * 24.f);
}

void AcidBox::onRef(int ticks) {
  uint64_t now = clock_;
  if (haveRef_) {
    float d = float(now - lastRefTime_);
    if (d > sr_ * 2.f) refPeriod_ = 0;
    else if (d > 0) refPeriod_ = refPeriod_ > 0 ? refPeriod_ * 0.85f + d * 0.15f : d;
    while (segTicks_ < refN_) { segTicks_++; extTick(now); }
  }
  haveRef_ = true;
  lastRefTime_ = now;
  refN_ = ticks;
  segTicks_ = 1;
  segTimer_ = 0;
  extTick(now);
}

void AcidBox::extTick(uint64_t t) {
  outTick(t, proj_.clockSrc != CLK_MIDI);
  if (!playing_) return;
  if (extArmed_) {
    extArmed_ = false;
    tickCount_ = 0;
    sampleInStep_ = 0;
    startStep();
    return;
  }
  if (++tickCount_ % 6) return;
  globalStep_ = (globalStep_ + 1) % 720720u;
  sampleInStep_ = 0;
  float sw = (pat().swing - 50) / 100.f;
  if ((globalStep_ & 1) && sw > 0) pendingStep_ = int(stepSamples() * 2.f * sw);
  else startStep();
}

// ------------------------------------------------------------------ sequencer
int AcidBox::stepSamples() const {
  if (ext() && refPeriod_ > 0) return int(refPeriod_ / refN_ * 6.f);
  return int(sr_ * 15.f / proj_.bpm);
}

void AcidBox::computeStepDur() {
  int sps = stepSamples();
  float sw = ext() ? 0.f : (pat().swing - 50) / 100.f;
  curStepDur_ = int(sps * ((globalStep_ & 1) ? 1.f - 2.f * sw : 1.f + 2.f * sw));
  if (curStepDur_ < 16) curStepDur_ = 16;
}

void AcidBox::start() {
  if (ext()) { startExt(true); return; }
  playing_ = true;
  globalStep_ = 0;
  sampleInStep_ = 0;
  barCount_ = 0;
  lastRecStep_ = -1;
  skipStep_[0] = skipStep_[1] = -1;
  if (queuedPattern_ >= 0) { proj_.current = uint8_t(queuedPattern_); queuedPattern_ = -1; }
  if (proj_.clockOut) emit(clock_, OUT_MIDI, 0xFA);
  outTicks_ = 0;
  intTickAcc_ = 0;
  outTick(clock_);
  computeStepDur();
  startStep();
}

void AcidBox::startExt(bool fromTop) {
  playing_ = true;
  extArmed_ = true;
  pendingStep_ = -1;
  lastRecStep_ = -1;
  barCount_ = 0;
  if (fromTop) globalStep_ = 0;
  skipStep_[0] = skipStep_[1] = -1;
  if (queuedPattern_ >= 0) { proj_.current = uint8_t(queuedPattern_); queuedPattern_ = -1; }
  if (proj_.clockOut && proj_.clockSrc != CLK_MIDI) emit(clock_, OUT_MIDI, fromTop ? 0xFA : 0xFB);
  outTicks_ = 0;
}

void AcidBox::stop() {
  if (playing_ && proj_.clockOut && proj_.clockSrc != CLK_MIDI) emit(clock_, OUT_MIDI, 0xFC);
  playing_ = false;
  extArmed_ = false;
  pendingStep_ = -1;
  for (auto& v : voice_) v.allOff();
  for (int& p : stepPos_) p = 0;
  if (queuedPattern_ >= 0) { proj_.current = uint8_t(queuedPattern_); queuedPattern_ = -1; }
}

void AcidBox::onBar() {
  barCount_++;
  if (!proj_.evolve || barCount_ % proj_.evolve) return;
  backup();  // UNDO steps back one evolution
  for (int l = 0; l < kLines; l++) {
    if (mute_[l]) continue;
    Line& ln = pat().lines[l];
    int notes = 0;
    for (int s = 0; s < laneLength(l); s++) notes += ln.steps[s].gate;
    if (notes) mutate(ln, genCtx(l), rng_, 0.1f);
  }
  if (!mute_[2]) mutateDrums(pat().drums, pat().gen, rng_, 0.06f);
}

void AcidBox::startStep() {
  if (globalStep_ % 16 == 0) {
    if (queuedPattern_ >= 0) {
      proj_.current = uint8_t(queuedPattern_);
      queuedPattern_ = -1;
      char v[8];
      std::snprintf(v, sizeof v, "P%02d", proj_.current + 1);
      msg("PATTERN", v);
    }
    if (globalStep_ > 0) onBar();
  }
  computeStepDur();
  for (int l = 0; l < kLines; l++) {
    int L = laneLength(l);
    int pos = int(globalStep_ % uint32_t(L));
    stepPos_[l] = pos;
    if (skipStep_[l] == pos) { skipStep_[l] = -1; continue; }
    const acid::Step& s = pat().lines[l].steps[pos];
    if (!s.gate || mute_[l]) continue;
    if (s.prob < 8 && rng_.uni() * 8.f >= s.prob) continue;
    fireLine(l, s);
  }
  const Drums& d = pat().drums;
  int pos = int(globalStep_ % uint32_t(laneLength(2)));
  stepPos_[2] = pos;
  if (!mute_[2]) {
    bool acc = (d.accent >> pos) & 1;
    for (int i : {DI_KICK, DI_CLAP, DI_CH, DI_OH})
      if ((d.hits[i] >> pos) & 1) fireDrum(i, acc);
  }
}

void AcidBox::fireLine(int l, const acid::Step& s) {
  int note = clampi(s.pitch + 12 * s.oct + transpose_, 12, 108);
  int sps = stepSamples();
  int len = s.len < 1 ? 1 : s.len;
  int gate = s.slide ? len * sps + sps / 4 : len * sps - sps / 2;
  if (gate < 64) gate = 64;
  voice_[l].noteOn(note, s.accent ? 1.f : 0.75f, s.accent != 0, gate);
  lastFired_[l] = note;
}

void AcidBox::fireDrum(int inst, bool accent) {
  const uint8_t* dp = proj_.drum;
  DrumParams p;
  p.vel = accent ? 1.f : 0.72f;
  p.level = dp[DP_LEVEL] / 127.f;
  p.drive = dp[DP_DRIVE] / 127.f;
  switch (inst) {
    case DI_KICK:
      p.tune = dp[DP_KICK_TUNE] / 127.f;
      p.decay = dp[DP_KICK_DECAY] / 127.f;
      p.tone = 0.45f;
      p.sweep = 0.5f;
      kick_.trigger(p);
      break;
    case DI_CLAP:
      p.variant = 0;
      p.tone = 0.5f;
      p.decay = 0.4f;
      p.sweep = 0.5f;
      p.level *= 0.8f;
      clap_.trigger(p);
      break;
    default:
      p.variant = inst == DI_OH ? 1 : 0;
      p.decay = dp[DP_HAT_DECAY] / 127.f;
      p.tune = 0.55f;
      p.tone = 0.6f;
      p.sweep = 0.35f;
      p.level *= 0.55f;
      hat_.trigger(p);
      break;
  }
}

AcidParams AcidBox::voiceParams(int l) const {
  const uint8_t* q = proj_.line[l];
  auto v = [&](int i) { return q[i] / 127.f; };
  AcidParams p;
  p.cutoff = v(LP_CUTOFF);
  p.reso = v(LP_RESO);
  p.envMod = v(LP_ENVMOD);
  p.decay = v(LP_DECAY);
  p.accent = v(LP_ACCENT);
  p.slide = v(LP_SLIDE);
  p.morph = v(LP_WAVE);
  p.tune = float(std::lround((q[LP_TUNE] - 64) * 12.f / 63.f));
  p.drive = v(LP_DRIVE);
  p.sub = v(LP_SUB);
  p.level = v(LP_LEVEL) * 1.25f;
  p.dist = q[LP_DIST] * 4 / 128;
  float mod = 0;
  if (playing_ && q[LP_LFO_AMT] > 0) {  // tempo-synced triangle LFO on the cutoff
    int div = kLfoDiv[q[LP_LFO_RATE] * 8 / 128];
    float ph = (float(globalStep_ % uint32_t(div)) + float(sampleInStep_) / float(curStepDur_)) / float(div);
    float tri = 1.f - 4.f * std::fabs(ph - 0.5f);
    mod += v(LP_LFO_AMT) * 2.5f * tri;
  }
  if (lane_ == l && !held(B_FUNC) && !heldSteps_) {  // joystick: live cutoff (X) / env mod (Y)
    mod += joyX_ * 2.f;
    p.envMod = clamp01(p.envMod + joyY_ * 0.5f);
  }
  p.cutMod = mod;
  return p;
}

void AcidBox::controlTick(int blk) {
  if (playing_) {
    sampleInStep_ += blk;
    if (!ext()) {
      while (sampleInStep_ >= curStepDur_) {
        sampleInStep_ -= curStepDur_;
        globalStep_ = (globalStep_ + 1) % 720720u;
        startStep();
      }
      intTickAcc_ += blk;
      double per = stepSamples() / 6.0;
      while (intTickAcc_ >= per) {
        intTickAcc_ -= per;
        outTick(clock_ + uint64_t(blk - intTickAcc_));
      }
    } else if (pendingStep_ >= 0) {
      pendingStep_ -= blk;
      if (pendingStep_ <= 0) { pendingStep_ = -1; startStep(); }
    }
  }
  if (ext() && haveRef_ && segTicks_ < refN_ && refPeriod_ > 0) {
    segTimer_ += blk;
    float iv = refPeriod_ / refN_;
    while (segTicks_ < refN_ && segTimer_ >= iv) {
      segTimer_ -= iv;
      segTicks_++;
      extTick(clock_ + uint64_t(blk - segTimer_));
    }
  }
  for (int l = 0; l < kLines; l++) voice_[l].control(voiceParams(l), blk);
  delay_.setTime(stepSamples() * kDelayDiv[proj_.delayTime % 6]);
  delay_.setFeedback(proj_.delayFb / 127.f * 0.9f);
}

// ------------------------------------------------------------------ audio
void AcidBox::process(float* outL, float* outR, int n) {
  drain();
  int done = 0;
  while (done < n) {
    int blk = n - done < 16 ? n - done : 16;
    controlTick(blk);
    render(outL + done, outR + done, blk);
    done += blk;
    clock_ += uint64_t(blk);
  }
}

void AcidBox::render(float* L, float* R, int n) {
  float a[16], b[16];
  voice_[0].render(a, n);
  voice_[1].render(b, n);
  float sa = proj_.line[0][LP_SEND] / 127.f, sb = proj_.line[1][LP_SEND] / 127.f, sd = proj_.drum[DP_SEND] / 127.f;
  sa *= sa; sb *= sb; sd *= sd;
  for (int i = 0; i < n; i++) {
    float k = kick_.tick(), c = clap_.tick(), h = hat_.tick();
    float drums = k + c * 0.9f + h;
    float l = a[i] * 0.72f + b[i] * 0.5f + drums * 0.7f + h * 0.08f;
    float r = a[i] * 0.72f + b[i] * 0.66f + drums * 0.7f - h * 0.08f;
    float send = a[i] * sa + b[i] * sb + (c + h) * sd;
    float dl, dr;
    delay_.tick(send * 0.7f, dl, dr);
    L[i] = softclip((l + dl) * 0.85f);
    R[i] = softclip((r + dr) * 0.85f);
  }
}

// ------------------------------------------------------------------ LEDs
uint16_t AcidBox::stepLeds() const {
  bool blink = (clock_ / uint64_t(sr_ * 0.15f)) & 1;
  uint16_t m = 0;
  if (held(B_FUNC)) {
    if (proj_.evolve) m |= 1u << AF_EVOLVE;
    if (mode_ == M_MUTE) m |= 1u << AF_MUTE;
    if (undoPattern_ == proj_.current) m |= 1u << AF_UNDO;
    return m;
  }
  int L = laneLength(lane_);
  switch (mode_) {
    case M_SETUP:
      m = uint16_t(1u << proj_.clockSrc) | uint16_t(1u << (4 + (proj_.syncPpqn & 3)));
      if (proj_.clockOut) m |= 1u << 8;
      if (proj_.syncOut) m |= 1u << 9;
      return m;
    case M_MUTE:
      for (int t = 0; t < 3; t++) if (!mute_[t]) m |= 1u << t;
      return m;
    case M_PATTERN:
      m = uint16_t(1u << proj_.current);
      if (queuedPattern_ >= 0 && blink) m |= 1u << queuedPattern_;
      return m;
    case M_LEN: return uint16_t((1u << L) - 1);
    case M_STYLE: return uint16_t(1u << (pat().gen.style % ST_COUNT));
    case M_SCALE: return uint16_t(1u << (pat().gen.scale % SC_COUNT));
    default: break;
  }
  if (lane_ == LANE_DRUMS) {
    m = pat().drums.hits[layer_] & uint16_t((1u << L) - 1);
  } else {
    const Line& ln = pat().lines[lane_];
    for (int s = 0; s < L; s++) {
      const acid::Step& st = ln.steps[s];
      bool on = false;
      switch (layer_) {
        case L_GATE: on = st.gate; break;
        case L_ACCENT: on = st.gate && st.accent; break;
        case L_SLIDE: on = st.gate && st.slide; break;
        case L_OCT: on = st.gate && (st.oct > 0 || (st.oct < 0 && blink)); break;
      }
      if (on) m |= 1u << s;
    }
    if (recMode_ && !playing_) { if (blink) m ^= uint16_t(1u << recPos_); return m; }
  }
  if (playing_) m ^= uint16_t(1u << stepPos_[lane_]);
  return m;
}

bool AcidBox::led(int l) const {
  switch (l) {
    case LED_ENV_FIL: return lane_ == LANE_A;
    case LED_ENV_AMP: return lane_ == LANE_B;
    case LED_ENV_MIX: return lane_ == LANE_DRUMS;
    case LED_LFO1: return !keysTranspose_;
    case LED_LFO2: return keysTranspose_;
    case LED_OSC_A: case LED_OSC_B: case LED_OSC_C: case LED_OSC_D: return layer_ == l - LED_OSC_A;
    case LED_PLAY: return playing_;
    case LED_REC: return recMode_;
    case LED_FUNC: return held(B_FUNC) || mode_ != M_NONE;
    default: return false;
  }
}

// ------------------------------------------------------------------ display (128x32)
static void drawTitleValue(Display& d, const char* title, const char* value, int bar, const char* badge) {
  d.text(0, 0, title);
  if (badge && *badge) {
    int w = Display::textWidth(badge) + 4;
    d.fill(Display::W - w, 0, w, 8);
    d.text(Display::W - w + 2, 0, badge, false);
  }
  int scale = Display::textWidth(value, 2) <= Display::W ? 2 : 1;
  d.text(0, scale == 2 ? 10 : 14, value, true, scale);
  if (bar >= 0) {
    d.rect(0, 28, Display::W, 4);
    d.fill(1, 29, bar * (Display::W - 2) / 127, 2);
  }
}

void AcidBox::drawDisplay(Display& d) const {
  d.clear();
  if (clock_ < uint64_t(sr_ * 1.2f)) {
    d.text(28, 0, "PRO VS", true, 2);
    d.text(22, 20, "ACID   fw 0.1");
    return;
  }
  if (clock_ < msgUntil_) { drawTitleValue(d, title_, value_, bar_, badge_); return; }
  char buf[32];
  if (held(B_FUNC) && mode_ == M_NONE) {
    d.text(0, 0, "1GEN 2MUT 3RHY 4NOTE");
    d.text(0, 8, "5A+S 6UNDO 7STY 8SCL");
    d.text(0, 16, "9EVO 10ROT 11LEN 12C");
    d.text(0, 24, "13P 14MUT 15PAT 16SV");
    return;
  }
  switch (mode_) {
    case M_MUTE: {
      d.text(0, 0, "MUTE: STEP 1-3");
      static const char* kL[3] = {"A", "B", "DR"};
      for (int t = 0; t < 3; t++) {
        int x = t * 42;
        d.text(x + 8, 12, kL[t], true, 2);
        if (mute_[t]) d.invert(x + 2, 10, 36, 18);
      }
      return;
    }
    case M_PATTERN:
      std::snprintf(buf, sizeof buf, "P%02d", proj_.current + 1);
      drawTitleValue(d, "PATTERN: PRESS STEP", buf, -1, "");
      return;
    case M_LEN:
      std::snprintf(buf, sizeof buf, "%d", laneLength(lane_));
      drawTitleValue(d, "LENGTH: PRESS STEP", buf, -1, "");
      return;
    case M_STYLE:
      drawTitleValue(d, "STYLE: STEP 1-8", styleName(pat().gen.style), -1, "");
      return;
    case M_SCALE:
      std::snprintf(buf, sizeof buf, "%s %s", noteName(pat().gen.root), scaleName(pat().gen.scale));
      drawTitleValue(d, "SCALE 1-8, KEY=KEYS", buf, -1, "");
      return;
    case M_SETUP: {
      static const char* src[3] = {"INT", "MIDI", "SYNC"};
      std::snprintf(buf, sizeof buf, "CLK %s  SYNC %dPPQN", src[proj_.clockSrc % 3], kSyncPpqn[proj_.syncPpqn & 3]);
      d.text(0, 0, buf);
      std::snprintf(buf, sizeof buf, "OUT: MIDI %s SYNC %s", proj_.clockOut ? "ON" : "--", proj_.syncOut ? "ON" : "--");
      d.text(0, 8, buf);
      d.text(0, 16, "1-3 CLK  5-8 PPQN");
      d.text(0, 24, "9 MIDIOUT 10 SYNCOUT");
      return;
    }
    default: break;
  }

  // home: lane + pattern, status line, mini roll
  bool blink = (clock_ / uint64_t(sr_ * 0.25f)) & 1;
  d.text(0, 0, kLaneNames[lane_], true, 2);
  int shownPat = (queuedPattern_ >= 0 && blink) ? queuedPattern_ : proj_.current;
  std::snprintf(buf, sizeof buf, "P%02d", shownPat + 1);
  d.text(Display::W - Display::textWidth(buf, 2), 0, buf, true, 2);

  int x = Display::W - 9;
  if (playing_) { for (int c = 0; c < 4; c++) d.vline(x + c, 18 + c, 7 - 2 * c); }
  else d.fill(x, 18, 6, 6);
  x -= 4;
  auto flag = [&](const char* s, bool inv) {
    int w = Display::textWidth(s);
    x -= w + 3;
    if (inv) { d.fill(x - 1, 17, w + 2, 9); d.text(x, 18, s, false); }
    else d.text(x, 18, s);
  };
  if (recMode_) flag("REC", true);
  if (proj_.evolve) flag("EVO", blink && playing_);
  if (transpose_) { char t[8]; std::snprintf(t, sizeof t, "T%+d", transpose_); flag(t, false); }
  if (mute_[lane_]) flag("M", true);
  int n;
  if (!ext()) n = std::snprintf(buf, sizeof buf, "%d", proj_.bpm);
  else if (waitingForClock()) n = std::snprintf(buf, sizeof buf, "WAIT");
  else if (tempo() > 0) n = std::snprintf(buf, sizeof buf, "%.0f", tempo());
  else n = std::snprintf(buf, sizeof buf, "EXT");
  std::snprintf(buf + n, sizeof buf - n, " %s%s %s", noteName(pat().gen.root), kScaleShort[pat().gen.scale % SC_COUNT],
                kStyleShort[pat().gen.style % ST_COUNT]);
  int maxChars = (x - 3) / 6;
  if (maxChars < int(sizeof buf) && maxChars >= 0) buf[maxChars] = 0;
  d.text(0, 18, buf);

  int L = laneLength(lane_);
  for (int s = 0; s < L; s++) {  // mini roll: note / accent (tall) / slide (tie line)
    int bx = s * 8;
    bool on = false, acc = false, sld = false;
    if (lane_ == LANE_DRUMS) {
      const Drums& dr = pat().drums;
      on = ((dr.hits[DI_CLAP] | dr.hits[DI_CH] | dr.hits[DI_OH]) >> s) & 1;
      acc = (dr.hits[DI_KICK] >> s) & 1;
    } else {
      const acid::Step& st = pat().lines[lane_].steps[s];
      on = st.gate;
      acc = st.gate && st.accent;
      sld = st.gate && st.slide;
    }
    if (acc) d.fill(bx + 1, 28, 5, 4);
    else if (on) d.fill(bx + 1, 30, 5, 2);
    else d.set(bx + 3, 31);
    if (sld) d.hline(bx + 6, 30, 3);
    if (playing_ && stepPos_[lane_] == s) d.invert(bx, 27, 7, 5);
    if (recMode_ && !playing_ && lane_ != LANE_DRUMS && recPos_ == s && blink) d.rect(bx, 27, 7, 5);
  }
}

}  // namespace pvg
