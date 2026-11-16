#include "groovebox.h"

#include <cstdio>
#include <cstring>

namespace pvg {

// ------------------------------------------------------------------ tables
static const char* kButtonNames[B_COUNT] = {"-", "+", "ENV", "LFO", "OSC", "MEMORY WRITE", "PLAY", "FUNC", "RECORD"};
static const char* kPotNames[K_COUNT] = {"R1", "R2", "R3", "R4", "RATE", "AMT", "CUTOFF", "RESO",
                                         "WAVE", "FREQ", "RATE", "AMT", "TEMPO"};
static const char* kFuncNames[16] = {"KICK", "SNARE", "HAT", "PERC", "SYNTH", "ACID", "MUTE", "CLR ALL",
                                     "COPY", "PASTE", "LEN", "EUCLID", "DICE", "PATTERN", "KEYS", "SAVE"};
static const char* kTrackNames[kNumTracks] = {"KICK", "SNARE", "HAT", "PERC", "SYNTH", "ACID"};
static const char* kVariantNames[kNumTracks][4] = {
    {"DEEP", "PUNCH", "", ""}, {"SNARE", "RIM", "", ""}, {"CLOSED", "OPEN", "", ""}, {"CLAP", "TOM", "COWBELL", "ZAP"},
    {"", "", "", ""}, {"SAW", "SQUARE", "", ""}};
static const int kVariantCount[kNumTracks] = {2, 2, 2, 4, 1, 2};
static const int kDelayDiv[6] = {1, 2, 3, 4, 6, 8};  // in 16th notes
static const char* kDelayDivNames[6] = {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"};
static const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
// ENV pages for the synth. Order when pressing ENV: AMP > OSC ENV > FIL > VOICE FLT > MIX
enum EnvPage { EP_FIL, EP_AMP, EP_MIX, EP_OSC, EP_VCF };
static const char* kEnvNames[5] = {"FIL", "AMP", "MIX", "OSC ENV", "VOICE FLT"};
static const int kNextEnv[5] = {EP_VCF, EP_OSC, EP_AMP, EP_FIL, EP_MIX};
static const char* kVcfModeNames[kVcfModes] = {"OFF", "LP12", "LP24", "BP", "HP"};
static int envGroup(int slot) {  // first slot of the ADSR group a slot belongs to, -1 = none
  if (slot >= S_AMP_A && slot <= S_FIL_R) return slot & ~3;
  if (slot >= S_OENV && slot < S_OENV + 16) return S_OENV + ((slot - S_OENV) & ~3);
  return -1;
}

const char* buttonName(int b) { return (b >= 0 && b < B_COUNT) ? kButtonNames[b] : "?"; }
const char* potName(int p) { return (p >= 0 && p < K_COUNT) ? kPotNames[p] : "?"; }
const char* funcName(int s) { return (s >= 0 && s < 16) ? kFuncNames[s] : "?"; }
const char* trackName(int t) { return (t >= 0 && t < kNumTracks) ? kTrackNames[t] : "?"; }

const char* paramName(int t, int slot) {
  if (t == kSynthTrack) {
    static const char* n[kNumParams] = {
        "AMP ATK", "AMP DEC", "AMP SUS", "AMP REL", "FIL ATK", "FIL DEC", "FIL SUS", "FIL REL",
        "WAVE A", "WAVE B", "WAVE C", "WAVE D", "FREQ A", "FREQ B", "FREQ C", "FREQ D",
        "DETUNE", "LFO1 RATE", "LFO1>VEC", "LFO2 RATE", "LFO2>CUT", "SEND", "CUTOFF", "RESO",
        "FIL ENV", "VECTOR X", "VECTOR Y", "VCF CUTOFF", "VCF RESO", "VCF ENV", "VCF MODE", "-",
        "A ATTACK", "A DECAY", "A SUSTAIN", "A RELEASE", "B ATTACK", "B DECAY", "B SUSTAIN", "B RELEASE",
        "C ATTACK", "C DECAY", "C SUSTAIN", "C RELEASE", "D ATTACK", "D DECAY", "D SUSTAIN", "D RELEASE"};
    return n[slot];
  }
  if (t == kAcidTrack) {
    static const char* n[8] = {"CUTOFF", "RESO", "ENV MOD", "DECAY", "TUNE", "ACCENT", "DRIVE", "SLIDE"};
    if (slot < 8) return n[slot];
    return slot == P_SEND ? "SEND" : "-";
  }
  switch (slot) {
    case D_TUNE: return "TUNE";
    case D_DECAY: return "DECAY";
    case D_TONE: return "TONE";
    case D_LEVEL: return "LEVEL";
    case D_DRIVE: return "DRIVE";
    case D_CRUSH: return "CRUSH";
    case D_PAN: return "PAN";
    case D_SWEEP: return t == TT_SNARE ? "SNAP" : (t == TT_HAT ? "NOISE" : "SWEEP");
    case P_SEND: return "SEND";
    default: return "-";
  }
}

static void noteStr(int n, char* out, int len) {
  if (n == kNoNote || n < 0) { std::snprintf(out, len, "--"); return; }
  std::snprintf(out, len, "%s%d", kNoteNames[n % 12], n / 12 - 1);
}
static int tuneSemis(int v) { return int(std::lround((v - 64) * 24.f / 63.f)); }
static int acidSemis(int v) { return int(std::lround((v - 64) * 12.f / 63.f)); }

// ------------------------------------------------------------------ queue
void Groovebox::push(const Event& e) {
  uint32_t h = qHead_.load(std::memory_order_relaxed);
  uint32_t n = (h + 1) % kQ;
  if (n == qTail_.load(std::memory_order_acquire)) return;  // full: drop
  q_[h] = e;
  qHead_.store(n, std::memory_order_release);
}

void Groovebox::drain() {
  uint32_t t = qTail_.load(std::memory_order_relaxed);
  while (t != qHead_.load(std::memory_order_acquire)) {
    Event e = q_[t];
    t = (t + 1) % kQ;
    qTail_.store(t, std::memory_order_release);
    handle(e);
  }
}

void Groovebox::handle(const Event& e) {
  switch (e.type) {
    case EV_STEP_DOWN: if (e.a < kNumSteps) onStepDown(e.a); break;
    case EV_STEP_UP: if (e.a < kNumSteps) onStepUp(e.a); break;
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
void Groovebox::init(float sr) {
  sr_ = sr;
  kick_.init(sr);
  snare_.init(sr);
  hat_.init(sr);
  perc_.init(sr);
  synth_.init(sr);
  acid_.init(sr);
  delay_.init(sr);
  initProject();
  loadDemo();
  computeStepDur();
}

void Groovebox::resetStep(Step& s) {
  std::memset(&s, 0, sizeof s);
  s.prob = 8;
  s.ratchet = 1;
  s.gate = 1;
  s.notes[0] = uint8_t(lastNote_);
  s.notes[1] = s.notes[2] = kNoNote;
}

void Groovebox::clearTrack(Track& tr) {
  for (auto& s : tr.steps) resetStep(s);
}

void Groovebox::initProject() {
  std::memset(&proj_, 0, sizeof proj_);
  proj_.magic = kProjectMagic;
  proj_.version = kProjectVersion;
  proj_.bpm = 120;
  proj_.delayTime = 2;
  proj_.delayFb = 50;
  proj_.clockSrc = CLK_INT;
  proj_.syncPpqn = 1;  // 2 PPQN: Volca / Pocket Operator standard
  proj_.clockOut = 1;
  proj_.syncOut = 1;
  //                                  R1   R2  R3   R4  DRV CRU PAN SWP
  static const uint8_t kDrum[4][8] = {{40, 50, 50, 115, 20, 0, 64, 50},
                                      {50, 45, 60, 95, 10, 0, 58, 64},
                                      {64, 30, 70, 80, 0, 0, 76, 30},
                                      {64, 50, 60, 85, 0, 0, 52, 50}};
  static const uint8_t kSend[4] = {0, 18, 10, 30};
  for (auto& p : proj_.patterns) {
    p.swing = 50;
    for (int t = 0; t < kNumTracks; t++) {
      Track& tr = p.tracks[t];
      clearTrack(tr);
      tr.length = 16;
      tr.variant = 0;
      std::memset(tr.p, 0, kNumParams);
      if (isDrum(t)) {
        std::memcpy(tr.p, kDrum[t], 8);
        tr.p[P_SEND] = kSend[t];
      } else if (t == kAcidTrack) {
        //                          CUT RES ENV DEC TUN ACC DRV SLD
        static const uint8_t kAcid[8] = {38, 88, 72, 40, 64, 85, 35, 45};
        std::memcpy(tr.p, kAcid, 8);
        tr.p[P_SEND] = 22;
      } else {
        const uint8_t amp[4] = {4, 60, 50, 40}, fil[4] = {2, 55, 20, 40};
        for (int i = 0; i < 4; i++) { tr.p[S_AMP_A + i] = amp[i]; tr.p[S_FIL_A + i] = fil[i]; }
        const uint8_t waves[4] = {2, 3, 12, 15};
        for (int i = 0; i < 4; i++) { tr.p[S_WAVE_A + i] = waves[i]; tr.p[S_TUNE_A + i] = 64; }
        tr.p[S_TUNE_D] = 95;  // +12
        tr.p[S_DETUNE] = 25;
        tr.p[S_LFO1_RATE] = 40;
        tr.p[S_LFO2_RATE] = 30;
        tr.p[P_SEND] = 40;
        tr.p[S_CUTOFF] = 60;
        tr.p[S_RESO] = 50;
        tr.p[S_FENV] = 70;
        tr.p[S_VEC_X] = 64;
        tr.p[S_VEC_Y] = 64;
        tr.p[S_VCF_CUT] = 90;
        tr.p[S_VCF_RES] = 25;
        tr.p[S_VCF_ENV] = 45;
        tr.p[S_VCF_MODE] = 0;  // off: the classic paraphonic sound
        for (int o = 0; o < 4; o++) {  // transparent until you shape them
          tr.p[S_OENV + o * 4 + 0] = 0;
          tr.p[S_OENV + o * 4 + 1] = 60;
          tr.p[S_OENV + o * 4 + 2] = 127;
          tr.p[S_OENV + o * 4 + 3] = 127;
        }
      }
    }
  }
}

void Groovebox::loadDemo() {
  Pattern& p = proj_.patterns[0];
  auto on = [&](int t, int s) -> Step& { Step& st = p.tracks[t].steps[s]; st.on = 1; return st; };
  for (int s = 0; s < 16; s += 4) on(TT_KICK, s);
  on(TT_KICK, 4).accent = 1;
  on(TT_SNARE, 4).accent = 1;
  on(TT_SNARE, 12).accent = 1;
  { Step& g = on(TT_SNARE, 15); g.prob = 4; setLock(g, D_LEVEL, 50); }
  for (int s = 0; s < 16; s += 2) {
    Step& h = on(TT_HAT, s);
    if (s % 4 == 2) { h.accent = 1; setLock(h, D_DECAY, 80); }
  }
  { Step& r = on(TT_HAT, 7); r.ratchet = 3; r.prob = 5; }
  { Step& c = on(TT_PERC, 12); c.prob = 6; }
  { Step& c = on(TT_PERC, 14); setLock(c, D_TONE, 110); c.prob = 3; }
  const int notes[16] = {45, -1, -1, 45, -1, -1, 48, -1, 45, -1, 43, 45, -1, -1, 52, -1};
  for (int s = 0; s < 16; s++) {
    if (notes[s] < 0) continue;
    Step& st = on(TT_SYNTH, s);
    st.notes[0] = uint8_t(notes[s]);
    st.notes[1] = st.notes[2] = kNoNote;
  }
  on(TT_SYNTH, 0).accent = 1;
  on(TT_SYNTH, 8).accent = 1;
  on(TT_SYNTH, 0).gate = 2;
  setLock(on(TT_SYNTH, 6), S_CUTOFF, 85);
  setLock(on(TT_SYNTH, 10), S_VEC_X, 127);
  setLock(on(TT_SYNTH, 14), S_VEC_Y, 127);
  setLock(on(TT_SYNTH, 14), S_CUTOFF, 80);
  // acid line: A minor, slides into the octave jumps
  const int acid[16] = {33, 33, 45, 33, -1, 36, 33, 31, 33, -1, 45, 43, 33, 33, 48, 45};
  const uint8_t aAcc[16] = {1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0};
  const uint8_t aSld[16] = {0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0};
  for (int s = 0; s < 16; s++) {
    if (acid[s] < 0) continue;
    Step& st = on(TT_ACID, s);
    st.notes[0] = uint8_t(acid[s]);
    st.notes[1] = st.notes[2] = kNoNote;
    st.accent = aAcc[s];
    st.slide = aSld[s];
  }
  setLock(on(TT_ACID, 12), A_CUTOFF, 60);
  Pattern& b = proj_.patterns[1];
  b.tracks[TT_KICK].steps[0].on = 1;
  b.tracks[TT_KICK].steps[10].on = 1;
  b.tracks[TT_SNARE].steps[8].on = 1;
  for (int s = 0; s < 16; s++) b.tracks[TT_HAT].steps[s].on = 1;
  b.tracks[TT_HAT].length = 12;
  b.tracks[TT_PERC].variant = 1;
  b.tracks[TT_PERC].steps[6].on = 1;
  b.tracks[TT_PERC].steps[7].on = 1;
}

// Older layouts, migrated on load. v3: 5 tracks, no slide flag. v4: 6 tracks.
// Both have 32 parameter slots and a 32-bit lock mask.
namespace v3 {
struct Step { uint8_t on, accent, prob, ratchet, gate, notes[kMaxNotes]; uint32_t lockMask; uint8_t lock[32]; };
struct Track { Step steps[kNumSteps]; uint8_t length, variant, p[32], pad[2]; };
struct Pattern { Track tracks[5]; uint8_t swing, pad[3]; };
struct Project { uint32_t magic, version; Pattern patterns[kNumPatterns];
                 uint8_t bpm, delayTime, delayFb, current, clockSrc, syncPpqn, clockOut, syncOut; };
}  // namespace v3
namespace v4 {
struct Step { uint8_t on, accent, prob, ratchet, gate, notes[kMaxNotes], slide, pad[3]; uint32_t lockMask; uint8_t lock[32]; };
struct Track { Step steps[kNumSteps]; uint8_t length, variant, p[32], pad[2]; };
struct Pattern { Track tracks[6]; uint8_t swing, pad[3]; };
struct Project { uint32_t magic, version; Pattern patterns[kNumPatterns];
                 uint8_t bpm, delayTime, delayFb, current, clockSrc, syncPpqn, clockOut, syncOut; };
}  // namespace v4
static uint8_t oldSlide(const v3::Step&) { return 0; }
static uint8_t oldSlide(const v4::Step& s) { return s.slide; }

// copies an old project over a freshly initialised one (new slots keep their defaults)
template <class OP>
static void migrate(Project& np, const OP* o, int nTracks) {
  for (int pi = 0; pi < kNumPatterns; pi++) {
    Pattern& pt = np.patterns[pi];
    pt.swing = o->patterns[pi].swing;
    for (int t = 0; t < nTracks; t++) {
      Track& nt = pt.tracks[t];
      const auto& ot = o->patterns[pi].tracks[t];
      nt.length = ot.length;
      nt.variant = ot.variant;
      std::memcpy(nt.p, ot.p, S_VCF_CUT);  // slots 27..31 were unused before
      for (int s = 0; s < kNumSteps; s++) {
        Step& ns = nt.steps[s];
        const auto& os = ot.steps[s];
        ns.on = os.on; ns.accent = os.accent; ns.prob = os.prob; ns.ratchet = os.ratchet; ns.gate = os.gate;
        std::memcpy(ns.notes, os.notes, kMaxNotes);
        ns.slide = oldSlide(os);
        ns.lockMask = os.lockMask & ((1u << S_VCF_CUT) - 1);
        std::memcpy(ns.lock, os.lock, 32);
      }
    }
  }
  np.bpm = o->bpm; np.delayTime = o->delayTime; np.delayFb = o->delayFb;
  np.current = o->current < kNumPatterns ? o->current : 0;
  np.clockSrc = o->clockSrc; np.syncPpqn = o->syncPpqn; np.clockOut = o->clockOut; np.syncOut = o->syncOut;
}

bool Groovebox::loadProject(const void* data, size_t n) {
  if (n == sizeof(v3::Project) || n == sizeof(v4::Project)) {
    bool isV3 = n == sizeof(v3::Project);
    const uint32_t* hdr = static_cast<const uint32_t*>(data);
    if (hdr[0] != kProjectMagic || hdr[1] != (isV3 ? 3u : 4u)) return false;
    stop();
    initProject();
    if (isV3) migrate(proj_, static_cast<const v3::Project*>(data), 5);
    else migrate(proj_, static_cast<const v4::Project*>(data), 6);
    if (cur_ >= kNumTracks) cur_ = 0;
    return true;
  }
  if (n != sizeof(Project)) return false;
  const Project* p = static_cast<const Project*>(data);
  if (p->magic != kProjectMagic || p->version != kProjectVersion) return false;
  stop();  // caller must not run process() concurrently
  std::memcpy(&proj_, data, n);
  if (proj_.current >= kNumPatterns) proj_.current = 0;
  return true;
}

// ------------------------------------------------------------------ helpers
void Groovebox::msg(const char* title, const char* value, int bar, const char* badge) {
  std::snprintf(title_, sizeof title_, "%s", title);
  std::snprintf(value_, sizeof value_, "%s", value);
  std::snprintf(badge_, sizeof badge_, "%s", badge);
  bar_ = bar;
  envGraphHi_ = -1;
  msgUntil_ = clock_ + uint64_t(sr_ * 1.2f);
}

void Groovebox::setLock(Step& s, int slot, int v) {
  s.lockMask |= uint64_t(1) << slot;
  s.lock[slot] = uint8_t(v < 0 ? 0 : (v > 127 ? 127 : v));
}

void Groovebox::showStep(int t, int s) {
  const Step& st = pat().tracks[t].steps[s];
  char title[24], val[16];
  int n = std::snprintf(title, sizeof title, "STEP %d", s + 1);
  if (st.prob < 8) n += std::snprintf(title + n, sizeof title - n, " %d%%", st.prob * 100 / 8);
  if (st.ratchet > 1) n += std::snprintf(title + n, sizeof title - n, " x%d", st.ratchet);
  if (isMelodic(t) && st.gate > 1) n += std::snprintf(title + n, sizeof title - n, " G%d", st.gate);
  if (t == kAcidTrack && st.slide) std::snprintf(title + n, sizeof title - n, " SLD");
  if (isMelodic(t)) {
    noteStr(st.notes[0], val, sizeof val);
    if (st.notes[1] != kNoNote) std::strncat(val, "+", sizeof val - std::strlen(val) - 1);
  } else {
    std::snprintf(val, sizeof val, "%s", st.on ? "ON" : "OFF");
  }
  const char* badge = st.accent ? (st.lockMask ? "ACC L" : "ACC") : (st.lockMask ? "LOCK" : "");
  msg(title, val, -1, badge);
}

int Groovebox::nearestStep(int t) const {
  int len = pat().tracks[t].length;
  int pos = stepPos_[t];
  if (sampleInStep_ > curStepDur_ / 2) pos = (pos + 1) % len;
  return pos;
}

void Groovebox::selectPattern(int p) {
  if (p < 0 || p >= kNumPatterns) return;
  char v[8];
  std::snprintf(v, sizeof v, "P%02d", p + 1);
  if (playing_) {
    queuedPattern_ = p;
    msg("NEXT PATTERN", v);
  } else {
    proj_.current = uint8_t(p);
    queuedPattern_ = -1;
    msg("PATTERN", v);
  }
}

void Groovebox::selectTrack(int t) {
  if (cur_ != t) { heldSteps_ = createdOnPress_ = stepEdited_ = 0; }
  cur_ = t;
  msg("TRACK", kTrackNames[t]);
}

void Groovebox::dice(int t) {
  Track& tr = pat().tracks[t];
  static const float kDensity[kNumTracks] = {0.3f, 0.2f, 0.6f, 0.2f, 0.45f, 0.7f};
  static const int kPenta[5] = {0, 3, 5, 7, 10};
  int root = lastNote_ - (lastNote_ % 12) + 9;  // A of the current octave
  if (root > lastNote_ + 6) root -= 12;
  for (int s = 0; s < tr.length; s++) {
    Step& st = tr.steps[s];
    resetStep(st);
    float d = kDensity[t];
    if (t == TT_KICK && s % 4 == 0) d = 0.85f;
    if (t == TT_SNARE && (s == 4 || s == 12)) d = 0.9f;
    st.on = rng_.uni() < d;
    st.accent = rng_.uni() < 0.2f;
    if (rng_.uni() < 0.1f) st.ratchet = 2;
    if (t == kAcidTrack) {
      int r = root - 12 * ((root - 24) / 12);  // keep the bass in octave 1-2
      if (r > 40) r -= 12;
      float u = rng_.uni();
      st.notes[0] = uint8_t(r + (u < 0.55f ? 0 : kPenta[rng_.next() % 5]) + (rng_.uni() < 0.25f ? 12 : 0));
      st.accent = rng_.uni() < 0.3f;
      st.slide = rng_.uni() < 0.22f;
      st.ratchet = 1;
    } else if (t == kSynthTrack) {
      st.notes[0] = uint8_t(root + kPenta[rng_.next() % 5] + (rng_.uni() < 0.2f ? 12 : 0));
      st.gate = rng_.uni() < 0.2f ? 2 : 1;
    } else if (rng_.uni() < 0.15f) {
      setLock(st, D_TUNE, int(tr.p[D_TUNE]) + int(rng_.bi() * 30));
    }
  }
  msg("DICE", kTrackNames[t]);
}

void Groovebox::euclid(int t, int hits) {
  Track& tr = pat().tracks[t];
  int L = tr.length;
  if (hits > L) hits = L;
  for (int s = 0; s < L; s++) {
    Step& st = tr.steps[s];
    bool on = ((s * hits) % L) < hits;
    if (on && !st.on) { resetStep(st); st.on = 1; }
    if (!on) st.on = 0;
  }
  char v[8];
  std::snprintf(v, sizeof v, "%d/%d", hits, L);
  msg("EUCLID", v);
}

// ------------------------------------------------------------------ step buttons
void Groovebox::onStepDown(int w) {
  if (held(B_FUNC)) { funcUsed_ = true; onFunc(w); return; }
  Track& tr = pat().tracks[cur_];
  switch (mode_) {
    case M_MUTE:
      if (w < kNumTracks) { mute_[w] = !mute_[w]; msg(kTrackNames[w], mute_[w] ? "MUTED" : "ON"); }
      return;
    case M_PATTERN: selectPattern(w); mode_ = M_NONE; return;
    case M_LEN: {
      tr.length = uint8_t(w + 1);
      char v[4];
      std::snprintf(v, sizeof v, "%d", w + 1);
      msg("LENGTH", v);
      mode_ = M_NONE;
      return;
    }
    case M_EUCLID: euclid(cur_, w + 1); mode_ = M_NONE; return;
    case M_SETUP: onSetupStep(w); return;
    default: break;
  }
  if (w >= tr.length) {
    char v[4];
    std::snprintf(v, sizeof v, "%d", tr.length);
    msg("TRACK LENGTH", v);
    return;
  }
  uint16_t bit = uint16_t(1u << w);
  heldSteps_ |= bit;
  stepEdited_ &= uint16_t(~bit);
  Step& st = tr.steps[w];
  if (!st.on) {
    resetStep(st);
    st.on = 1;
    createdOnPress_ |= bit;
  }
  if (heldSteps_ == bit) joyLatch_ = 0;
  showStep(cur_, w);
}

void Groovebox::onStepUp(int w) {
  uint16_t bit = uint16_t(1u << w);
  if (!(heldSteps_ & bit)) return;
  heldSteps_ &= uint16_t(~bit);
  Step& st = pat().tracks[cur_].steps[w];
  if (!(createdOnPress_ & bit) && !(stepEdited_ & bit)) {
    st.on = 0;
    st.lockMask = 0;
  }
  createdOnPress_ &= uint16_t(~bit);
}

static const char* kClockNames[3] = {"INTERNAL", "MIDI", "SYNC IN"};

void Groovebox::onSetupStep(int s) {
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

void Groovebox::onFunc(int s) {
  Track& tr = pat().tracks[cur_];
  switch (s) {
    case F_TRK1: case F_TRK2: case F_TRK3: case F_TRK4: case F_TRK5: case F_TRK6: selectTrack(s); break;
    case F_MUTE:
      mode_ = mode_ == M_MUTE ? M_NONE : M_MUTE;
      msg("MUTE MODE", mode_ == M_MUTE ? "ON" : "OFF");
      break;
    case F_CLEAR_ALL:
      for (auto& t : pat().tracks) clearTrack(t);
      msg("CLEAR ALL", "DONE");
      break;
    case F_COPY: clipTrack_ = tr; clipTrackValid_ = true; clipType_ = cur_; msg("COPY", kTrackNames[cur_]); break;
    case F_PASTE:
      if (clipTrackValid_) {
        uint8_t v = tr.variant;
        uint8_t keep[kNumParams];
        std::memcpy(keep, tr.p, kNumParams);
        bool sameType = (isDrum(cur_) && isDrum(clipType_)) || cur_ == clipType_;  // other kind: keep the sound
        tr = clipTrack_;
        if (!sameType) { std::memcpy(tr.p, keep, kNumParams); tr.variant = v; }
        msg("PASTE", kTrackNames[cur_]);
      } else {
        msg("PASTE", "EMPTY");
      }
      break;
    case F_LEN: mode_ = M_LEN; break;
    case F_EUCLID: mode_ = M_EUCLID; break;
    case F_DICE: dice(cur_); break;
    case F_PATTERN: mode_ = M_PATTERN; break;
    case F_KEYS: keysMode_ = !keysMode_; msg("KEYS", keysMode_ ? "SYNTH" : "TRACK"); break;
    case F_STORE: saveReq_.store(true); msg("MEMORY", "SAVED"); break;
  }
}

// ------------------------------------------------------------------ keyboard
void Groovebox::onKeyDown(int k) {
  bool chord = heldKeys_ != 0;
  heldKeys_ |= 1u << k;
  int note = keyNote(k);
  // tune lock for drums: key 7 (the first C) = no change, 4 units per semitone
  int tuneLock = int(pat().tracks[cur_].p[D_TUNE]) + (k - kDrumBaseKey) * 4;
  Track& tr = pat().tracks[cur_];

  if (heldSteps_) {  // hold step(s) + key = set note / chord / drum pitch
    char v[8];
    for (int s = 0; s < kNumSteps; s++) {
      if (!(heldSteps_ & (1u << s))) continue;
      Step& st = tr.steps[s];
      if (cur_ == kAcidTrack) {
        st.notes[0] = uint8_t(note);
        st.notes[1] = st.notes[2] = kNoNote;
      } else if (cur_ == kSynthTrack) {
        if (!chord) { st.notes[0] = uint8_t(note); st.notes[1] = st.notes[2] = kNoNote; }
        else for (auto& n : st.notes) if (n == kNoNote) { n = uint8_t(note); break; }
      } else {
        setLock(st, D_TUNE, tuneLock);
      }
      stepEdited_ |= uint16_t(1u << s);
    }
    if (cur_ == kSynthTrack) { synth_.noteOn(note, 0.8f, int(sr_ * 0.25f)); lastNote_ = note; noteStr(note, v, sizeof v); }
    else if (cur_ == kAcidTrack) { acid_.allOff(); acid_.noteOn(note, 0.8f, false, int(sr_ * 0.2f)); lastNote_ = note; noteStr(note, v, sizeof v); }
    else { Step tmp; resetStep(tmp); setLock(tmp, D_TUNE, tuneLock); fire(cur_, &tmp, 0.8f, false); std::snprintf(v, sizeof v, "%+d", k - kDrumBaseKey); }
    msg(isMelodic(cur_) ? "STEP NOTE" : "STEP PITCH", v, -1, "LOCK");
    return;
  }

  bool synthTarget = keysMode_ || cur_ == kSynthTrack;
  if (synthTarget) {
    synth_.noteOn(note, 0.9f, -1);
    lastNote_ = note;
  } else if (cur_ == kAcidTrack) {
    acid_.noteOn(note, 0.9f, false, -1);  // legato while another key is held
    lastNote_ = note;
  } else {
    Step tmp;
    resetStep(tmp);
    if (k != kDrumBaseKey) setLock(tmp, D_TUNE, tuneLock);
    fire(cur_, &tmp, 0.9f, false);
  }
  if (!(recMode_ && playing_) || (keysMode_ && cur_ != kSynthTrack)) return;

  int s = nearestStep(cur_);
  Step& st = tr.steps[s];
  bool addToChord = chord && lastRecStep_ == s && cur_ == kSynthTrack;
  if (!addToChord) {
    uint64_t mask = st.lockMask;
    uint8_t locks[kNumParams];
    std::memcpy(locks, st.lock, kNumParams);
    resetStep(st);
    st.lockMask = mask;
    std::memcpy(st.lock, locks, kNumParams);
  }
  st.on = 1;
  if (isMelodic(cur_)) {
    if (addToChord) { for (auto& n : st.notes) if (n == kNoNote) { n = uint8_t(note); break; } }
    else st.notes[0] = uint8_t(note);
    if (cur_ == kAcidTrack && chord && lastRecStep_ >= 0 && lastRecStep_ != s)
      tr.steps[lastRecStep_].slide = 1;  // legato playing records a slide
  } else if (k != kDrumBaseKey) {
    setLock(st, D_TUNE, tuneLock);
  }
  lastRecStep_ = s;
  if (s != stepPos_[cur_]) skipStep_[cur_] = s;  // already heard it live
}

void Groovebox::onKeyUp(int k) {
  heldKeys_ &= ~(1u << k);
  for (int o = -3; o <= 4; o++) {  // octave may have changed
    synth_.noteOff(kKeyBaseNote + 12 * o + k);
    if (heldKeys_ == 0) acid_.noteOff(kKeyBaseNote + 12 * o + k);
  }
}

// ------------------------------------------------------------------ buttons
void Groovebox::onButtonDown(int b) {
  if (b < 0 || b >= B_COUNT) return;
  heldButtons_ |= 1u << b;
  Track& tr = pat().tracks[cur_];
  bool fn = held(B_FUNC) && b != B_FUNC;
  if (fn) funcUsed_ = true;
  int firstHeld = -1;
  for (int s = 0; s < kNumSteps; s++) if (heldSteps_ & (1u << s)) { firstHeld = s; break; }
  auto forHeld = [&](auto f) {
    for (int s = 0; s < kNumSteps; s++)
      if (heldSteps_ & (1u << s)) { f(tr.steps[s]); stepEdited_ |= uint16_t(1u << s); }
  };
  bool synth = cur_ == kSynthTrack;
  bool acid = cur_ == kAcidTrack;

  switch (b) {
    case B_FUNC: funcUsed_ = false; break;
    case B_PLAY:
      if (playing_) stop();
      else { start(); if (ext()) msg("WAITING FOR", proj_.clockSrc == CLK_MIDI ? "MIDI CLK" : "SYNC"); }
      break;
    case B_REC:
      if (fn) { clearTrack(tr); msg("CLEAR", kTrackNames[cur_]); break; }
      recMode_ = !recMode_;
      lastRecStep_ = -1;
      msg("RECORD", recMode_ ? (playing_ ? "LIVE" : "ARMED") : "OFF");
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
        if (fn && isMelodic(cur_)) forHeld([&](Step& s) { int g = s.gate + d; s.gate = uint8_t(g < 1 ? 1 : (g > 16 ? 16 : g)); });
        else forHeld([&](Step& s) { int p = s.prob + d; s.prob = uint8_t(p < 1 ? 1 : (p > 8 ? 8 : p)); });
        showStep(cur_, firstHeld);
      } else if (fn) {
        octave_ += d;
        if (octave_ < -2) octave_ = -2;
        if (octave_ > 3) octave_ = 3;
        char v[8];
        std::snprintf(v, sizeof v, "%+d", octave_);
        msg("OCTAVE", v);
      } else {
        int p = (queuedPattern_ >= 0 ? queuedPattern_ : proj_.current) + d;
        selectPattern((p + kNumPatterns) % kNumPatterns);
      }
      break;
    }
    case B_ENV_SEL:
      if (firstHeld >= 0) { forHeld([](Step& s) { s.accent = !s.accent; }); showStep(cur_, firstHeld); }
      else if (synth) {
        envSel_ = kNextEnv[envSel_ % 5];
        if (envSel_ == EP_OSC) {
          char t[16];
          std::snprintf(t, sizeof t, "OSC %c ENV", 'A' + oscSel_);
          msg("R1-R4: OSC = WHICH", t);
          showEnvGraph(kSynthTrack, S_OENV + oscSel_ * 4);
          envGraphHi_ = -2;  // graph, nothing highlighted
        } else if (envSel_ == EP_VCF) {
          msg("R1-R4 CUT RES ENV MODE", "VOICE FLT");
        } else {
          msg("R1-R4 ENVELOPE", kEnvNames[envSel_]);
        }
      }
      else if (acid) msg("R1 R2 R3 R4", "CUT RES ENV DEC");
      else msg("R1 R2 R3 R4", "TUN DEC TON LVL");
      break;
    case B_LFO_SEL:
      if (firstHeld >= 0 && acid && !fn) { forHeld([](Step& s) { s.slide = !s.slide; }); showStep(cur_, firstHeld); }
      else if (firstHeld >= 0) { forHeld([](Step& s) { s.ratchet = uint8_t(s.ratchet % 4 + 1); }); showStep(cur_, firstHeld); }
      else if (acid) msg("RATE / AMT", "DRIVE SLIDE");
      else if (synth) { lfoSel_ = 1 - lfoSel_; msg(lfoSel_ ? "LFO 2" : "LFO 1", lfoSel_ ? "> CUTOFF" : "> VECTOR"); }
      else msg("RATE / AMT", "PAN SWEEP");
      break;
    case B_OSC_SEL:
      if (firstHeld >= 0) { forHeld([](Step& s) { s.lockMask = 0; }); msg("STEP LOCKS", "CLEARED"); }
      else if (synth) {
        oscSel_ = (oscSel_ + 1) % 4;
        char t[8];
        std::snprintf(t, sizeof t, "OSC %c", 'A' + oscSel_);
        if (envSel_ == EP_OSC) {  // on the OSC ENV page: show that oscillator's envelope
          char v[16];
          std::snprintf(v, sizeof v, "OSC %c ENV", 'A' + oscSel_);
          msg("R1-R4", v);
          showEnvGraph(kSynthTrack, S_OENV + oscSel_ * 4);
          envGraphHi_ = -2;
        } else {
          msg(t, waveName(tr.p[S_WAVE_A + oscSel_] % kNumWaves));
        }
      } else {
        tr.variant = uint8_t((tr.variant + 1) % kVariantCount[cur_]);
        msg(kTrackNames[cur_], kVariantNames[cur_][tr.variant]);
      }
      break;
  }
}

void Groovebox::onButtonUp(int b) {
  if (b < 0 || b >= B_COUNT) return;
  heldButtons_ &= ~(1u << b);
  if (b == B_FUNC) {
    joyLatch_ = 0;
    if (!funcUsed_ && mode_ != M_NONE) { mode_ = M_NONE; msg("MODE", "EXIT"); }
  }
}

// ------------------------------------------------------------------ pots
int Groovebox::potSlot(int p, int& track) const {
  bool fn = held(B_FUNC);
  track = cur_;
  if (p == K_CUTOFF) { track = kSynthTrack; return fn ? S_FENV : S_CUTOFF; }
  if (p == K_RESO) { track = kSynthTrack; return S_RESO; }
  if (p == K_CH_AMT) return P_SEND;
  if (cur_ != kSynthTrack) {
    switch (p) {
      case K_R1: return D_TUNE;
      case K_R2: return D_DECAY;
      case K_R3: return D_TONE;
      case K_R4: return D_LEVEL;
      case K_FREQ: return D_DRIVE;
      case K_WAVE: return D_CRUSH;
      case K_LFO_RATE: return D_PAN;
      case K_LFO_AMT: return D_SWEEP;
      default: return -1;
    }
  }
  if (p >= K_R1 && p <= K_R4) {
    int i = p - K_R1;
    if (envSel_ == EP_AMP) return S_AMP_A + i;
    if (envSel_ == EP_FIL) return S_FIL_A + i;
    if (envSel_ == EP_OSC) return S_OENV + oscSel_ * 4 + i;
    if (envSel_ == EP_VCF) return S_VCF_CUT + i;
    static const int kMix[4] = {S_VEC_X, S_VEC_Y, S_DETUNE, S_FENV};
    return kMix[i];
  }
  switch (p) {
    case K_LFO_RATE: return lfoSel_ ? S_LFO2_RATE : S_LFO1_RATE;
    case K_LFO_AMT: return lfoSel_ ? S_LFO2_AMT : S_LFO1_AMT;
    case K_WAVE: return S_WAVE_A + oscSel_;
    case K_FREQ: return fn ? S_DETUNE : S_TUNE_A + oscSel_;
    default: return -1;
  }
}

void Groovebox::formatValue(int track, int slot, int v, char* out, int n) const {
  if (track == kSynthTrack && slot >= S_WAVE_A && slot <= S_WAVE_D) std::snprintf(out, n, "%s", waveName(v % kNumWaves));
  else if (track == kSynthTrack && slot >= S_TUNE_A && slot <= S_TUNE_D) std::snprintf(out, n, "%+d", tuneSemis(v));
  else if (track == kSynthTrack && slot == S_VCF_MODE) std::snprintf(out, n, "%s", kVcfModeNames[v % kVcfModes]);
  else if (track == kSynthTrack && envGroup(slot) >= 0 && (slot & 3) == 2) std::snprintf(out, n, "%d%%", v * 100 / 127);
  else if (track == kAcidTrack && slot == A_TUNE) std::snprintf(out, n, "%+d", acidSemis(v));
  else if (isDrum(track) && slot == D_PAN) std::snprintf(out, n, v == 64 ? "C" : (v < 64 ? "L%d" : "R%d"), v < 64 ? 64 - v : v - 64);
  else std::snprintf(out, n, "%d", v);
}

void Groovebox::onPot(int p, float v) {
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
  int track;
  int slot = potSlot(p, track);
  if (slot < 0) return;
  int val = v7;
  if (track == kSynthTrack && slot >= S_WAVE_A && slot <= S_WAVE_D) val = int(v * (kNumWaves - 0.01f));
  if (track == kSynthTrack && slot == S_VCF_MODE) val = int(v * (kVcfModes - 0.01f));
  Track& tr = pat().tracks[track];
  formatValue(track, slot, val, vs, sizeof vs);
  int bar = (track == kSynthTrack && slot >= S_WAVE_A && slot <= S_WAVE_D) ? val * 127 / (kNumWaves - 1) : val;
  if (track == kSynthTrack && slot == S_VCF_MODE) bar = val * 127 / (kVcfModes - 1);
  const char* name = paramName(track, slot);

  if (track == cur_ && heldSteps_) {
    for (int s = 0; s < kNumSteps; s++)
      if (heldSteps_ & (1u << s)) { setLock(tr.steps[s], slot, val); stepEdited_ |= uint16_t(1u << s); }
    msg(name, vs, bar, "LOCK");
  } else if (track == cur_ && recMode_ && playing_ && tr.steps[stepPos_[track]].on) {
    setLock(tr.steps[stepPos_[track]], slot, val);
    msg(name, vs, bar, "REC");
  } else {
    tr.p[slot] = uint8_t(val);
    msg(name, vs, bar);
  }
  if (track == kSynthTrack && envGroup(slot) >= 0) showEnvGraph(track, slot);
}

// ADSR graph next to the value: what you hear, including a lock on the held step
void Groovebox::showEnvGraph(int track, int slot) {
  int g = envGroup(slot);
  if (g < 0) return;
  const Track& tr = pat().tracks[track];
  int held = -1;
  for (int s = 0; s < kNumSteps; s++) if (heldSteps_ & (1u << s)) { held = s; break; }
  for (int i = 0; i < 4; i++) {
    int v = tr.p[g + i];
    if (held >= 0 && track == cur_ && ((tr.steps[held].lockMask >> (g + i)) & 1)) v = tr.steps[held].lock[g + i];
    envGraph_[i] = uint8_t(v);
  }
  envGraphHi_ = slot - g;
  bar_ = -1;
}

void Groovebox::onJoy(float x, float y) {
  joyX_ = clampf(x, -1.f, 1.f);
  joyY_ = clampf(y, -1.f, 1.f);
  float mag = std::fabs(joyX_) > std::fabs(joyY_) ? std::fabs(joyX_) : std::fabs(joyY_);
  // The stick springs back to centre, so writes only happen while the
  // deflection grows: on release the furthest position stays (latch).
  bool fn = held(B_FUNC);
  if (!(heldSteps_ || fn) || mag < joyLatch_) return;
  joyLatch_ = mag;
  Track& tr = pat().tracks[cur_];
  bool syn = cur_ == kSynthTrack;
  bool acid = cur_ == kAcidTrack;
  int sx = syn ? S_VEC_X : (acid ? A_CUTOFF : D_TUNE), sy = syn ? S_VEC_Y : (acid ? A_ENVMOD : D_TONE);
  int vx, vy;
  if (syn) {
    vx = int((joyX_ + 1.f) * 63.5f + 0.5f);
    vy = int((joyY_ + 1.f) * 63.5f + 0.5f);
  } else {
    vx = int(tr.p[sx]) + int(joyX_ * 63.f);
    vy = int(tr.p[sy]) + int(joyY_ * 63.f);
  }
  vx = vx < 0 ? 0 : (vx > 127 ? 127 : vx);
  vy = vy < 0 ? 0 : (vy > 127 ? 127 : vy);
  char vs[16];
  std::snprintf(vs, sizeof vs, "%d %d", vx, vy);
  if (heldSteps_) {
    for (int s = 0; s < kNumSteps; s++)
      if (heldSteps_ & (1u << s)) {
        setLock(tr.steps[s], sx, vx);
        setLock(tr.steps[s], sy, vy);
        stepEdited_ |= uint16_t(1u << s);
      }
    msg(syn ? "VECTOR X Y" : (acid ? "CUT ENVMOD" : "TUNE TONE"), vs, -1, "LOCK");
  } else if (syn) {
    funcUsed_ = true;
    tr.p[sx] = uint8_t(vx);
    tr.p[sy] = uint8_t(vy);
    msg("VECTOR X Y", vs);
  }
}

void Groovebox::onMidi(uint8_t st, uint8_t d1, uint8_t d2) {
  uint8_t type = st & 0xF0;
  bool midiClk = proj_.clockSrc == CLK_MIDI;
  if (type == 0x90 && d2 > 0) {
    synth_.noteOn(d1, 0.3f + d2 / 181.f, -1);
    lastNote_ = d1;
  } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
    synth_.noteOff(d1);
  } else if (st == 0xF8) {
    if (midiClk) onRef(1);
  } else if (st == 0xFA) {  // start
    if (midiClk) startExt(true);
  } else if (st == 0xFB) {  // continue (from the song position)
    if (midiClk) startExt(false);
  } else if (st == 0xFC) {
    if (midiClk && playing_) stop();
  } else if (st == 0xF2) {  // song position pointer, in 16ths
    if (midiClk && !playing_) globalStep_ = uint32_t(d1 | (d2 << 7)) % 720720u;
  }
}

// ------------------------------------------------------------------ clock in / out
bool Groovebox::popOutput(OutEvent& e) {
  uint32_t t = oTail_.load(std::memory_order_relaxed);
  if (t == oHead_.load(std::memory_order_acquire)) return false;
  e = out_[t];
  oTail_.store((t + 1) % kOutQ, std::memory_order_release);
  return true;
}

void Groovebox::emit(uint64_t t, uint8_t type, uint8_t b0) {
  uint32_t h = oHead_.load(std::memory_order_relaxed);
  uint32_t n = (h + 1) % kOutQ;
  if (n == oTail_.load(std::memory_order_acquire)) return;  // HAL not draining: drop
  out_[h] = OutEvent{t, type, uint8_t(type == OUT_MIDI ? 1 : 0), {b0, 0, 0}};
  oHead_.store(n, std::memory_order_release);
}

void Groovebox::outTick(uint64_t t, bool sendMidi) {
  if (proj_.clockOut && sendMidi) emit(t, OUT_MIDI, 0xF8);
  if (proj_.syncOut && proj_.clockSrc != CLK_SYNC) {
    uint32_t div = 24u / kSyncPpqn[proj_.syncPpqn & 3];
    if (outTicks_ % div == 0) emit(t, OUT_SYNC);
  }
  outTicks_++;
}

float Groovebox::tempo() const {
  if (!ext()) return proj_.bpm;
  if (refPeriod_ <= 0) return 0;
  return sr_ * 60.f / (refPeriod_ / refN_ * 24.f);
}

// A reference pulse worth `ticks` 24-PPQN ticks arrived (MIDI F8 = 1, a
// 2-PPQN sync pulse = 12). Ticks between pulses are interpolated from the
// measured period, never running ahead of the next pulse; a late pulse
// first flushes the ticks still owed, so the grid never drifts.
void Groovebox::onRef(int ticks) {
  uint64_t now = clock_;
  if (haveRef_) {
    float d = float(now - lastRefTime_);
    if (d > sr_ * 2.f) refPeriod_ = 0;  // clock was stopped: forget the tempo
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

void Groovebox::extTick(uint64_t t) {
  outTick(t, proj_.clockSrc != CLK_MIDI);  // convert: MIDI in -> sync out, sync in -> MIDI out (no MIDI echo)
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
int Groovebox::stepSamples() const {
  if (ext() && refPeriod_ > 0) return int(refPeriod_ / refN_ * 6.f);
  return int(sr_ * 15.f / proj_.bpm);
}

void Groovebox::computeStepDur() {
  int sps = stepSamples();
  float sw = ext() ? 0.f : (pat().swing - 50) / 100.f;  // 0 .. 0.25; external: swing is a delay
  curStepDur_ = int(sps * ((globalStep_ & 1) ? 1.f - 2.f * sw : 1.f + 2.f * sw));
  if (curStepDur_ < 16) curStepDur_ = 16;
}

void Groovebox::start() {
  if (ext()) { startExt(true); return; }
  playing_ = true;
  globalStep_ = 0;
  sampleInStep_ = 0;
  lastRecStep_ = -1;
  for (int t = 0; t < kNumTracks; t++) { ratchLeft_[t] = 0; skipStep_[t] = -1; }
  if (queuedPattern_ >= 0) { proj_.current = uint8_t(queuedPattern_); queuedPattern_ = -1; }
  if (proj_.clockOut) emit(clock_, OUT_MIDI, 0xFA);
  outTicks_ = 0;
  intTickAcc_ = 0;
  outTick(clock_);
  computeStepDur();
  startStep();
}

void Groovebox::startExt(bool fromTop) {
  playing_ = true;
  extArmed_ = true;  // the next incoming tick plays the step
  pendingStep_ = -1;
  lastRecStep_ = -1;
  if (fromTop) globalStep_ = 0;
  for (int t = 0; t < kNumTracks; t++) { ratchLeft_[t] = 0; skipStep_[t] = -1; }
  if (queuedPattern_ >= 0) { proj_.current = uint8_t(queuedPattern_); queuedPattern_ = -1; }
  if (proj_.clockOut && proj_.clockSrc != CLK_MIDI) emit(clock_, OUT_MIDI, fromTop ? 0xFA : 0xFB);
  outTicks_ = 0;
}

void Groovebox::stop() {
  if (playing_ && proj_.clockOut && proj_.clockSrc != CLK_MIDI) emit(clock_, OUT_MIDI, 0xFC);
  playing_ = false;
  extArmed_ = false;
  pendingStep_ = -1;
  synth_.allOff();
  acid_.allOff();
  synthLockMask_ = 0;
  acidLockMask_ = 0;
  for (int t = 0; t < kNumTracks; t++) { ratchLeft_[t] = 0; stepPos_[t] = 0; }
  if (queuedPattern_ >= 0) { proj_.current = uint8_t(queuedPattern_); queuedPattern_ = -1; }
}

void Groovebox::startStep() {
  if (globalStep_ % 16 == 0 && queuedPattern_ >= 0) {
    proj_.current = uint8_t(queuedPattern_);
    queuedPattern_ = -1;
    char v[8];
    std::snprintf(v, sizeof v, "P%02d", proj_.current + 1);
    msg("PATTERN", v);
  }
  computeStepDur();
  for (int t = 0; t < kNumTracks; t++) {
    Track& tr = pat().tracks[t];
    int len = tr.length ? tr.length : 16;
    int pos = int(globalStep_ % uint32_t(len));
    stepPos_[t] = pos;
    ratchLeft_[t] = 0;
    if (skipStep_[t] == pos) { skipStep_[t] = -1; continue; }
    const Step& s = tr.steps[pos];
    if (!s.on || mute_[t]) continue;
    if (s.prob < 8 && rng_.uni() * 8.f >= s.prob) continue;
    float vel = s.accent ? 1.f : 0.72f;
    if (s.ratchet > 1) {
      ratchLeft_[t] = s.ratchet - 1;
      ratchInterval_[t] = curStepDur_ / s.ratchet;
      ratchCounter_[t] = ratchInterval_[t];
      ratchStep_[t] = pos;
      ratchVel_[t] = vel;
    }
    fire(t, &s, vel, false);
  }
}

void Groovebox::controlTick(int blk) {
  if (playing_) {
    sampleInStep_ += blk;
    for (int t = 0; t < kNumTracks; t++) {
      if (ratchLeft_[t] <= 0) continue;
      ratchCounter_[t] -= blk;
      if (ratchCounter_[t] <= 0) {
        ratchCounter_[t] += ratchInterval_[t];
        ratchLeft_[t]--;
        ratchVel_[t] *= 0.85f;
        fire(t, &pat().tracks[t].steps[ratchStep_[t]], ratchVel_[t], true);
      }
    }
    if (!ext()) {
      while (sampleInStep_ >= curStepDur_) {
        sampleInStep_ -= curStepDur_;
        globalStep_ = (globalStep_ + 1) % 720720u;  // lcm(1..16): keeps polymeters aligned
        startStep();
      }
      // straight (unswung) 24-PPQN clock out
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
  if (ext() && haveRef_ && segTicks_ < refN_ && refPeriod_ > 0) {  // interpolate between sync pulses
    segTimer_ += blk;
    float iv = refPeriod_ / refN_;
    while (segTicks_ < refN_ && segTimer_ >= iv) {
      segTimer_ -= iv;
      segTicks_++;
      extTick(clock_ + uint64_t(blk - segTimer_));
    }
  }
  if (synthLockLeft_ > 0) {
    synthLockLeft_ -= blk;
    if (synthLockLeft_ <= 0) synthLockMask_ = 0;
  }
  if (acidLockLeft_ > 0) {
    acidLockLeft_ -= blk;
    if (acidLockLeft_ <= 0) acidLockMask_ = 0;
  }
  synth_.control(synthParams(), blk);
  acid_.control(acidParams(), blk);
  delay_.setTime(stepSamples() * kDelayDiv[proj_.delayTime % 6]);
  delay_.setFeedback(proj_.delayFb / 127.f * 0.9f);
}

DrumParams Groovebox::drumParams(int t, const Step* st, float vel) const {
  const Track& tr = pat().tracks[t];
  auto val = [&](int s) -> float {
    int v = tr.p[s];
    if (st && ((st->lockMask >> s) & 1)) v = st->lock[s];
    return v / 127.f;
  };
  DrumParams p;
  p.tune = val(D_TUNE);
  p.decay = val(D_DECAY);
  p.tone = val(D_TONE);
  p.level = val(D_LEVEL);
  p.drive = val(D_DRIVE);
  p.crush = val(D_CRUSH);
  p.pan = val(D_PAN);
  p.sweep = val(D_SWEEP);
  p.send = val(P_SEND);
  if (t == cur_ && !keysMode_ && !heldSteps_) {  // joystick = live tune / tone
    p.tune = clamp01(p.tune + joyX_ * 0.5f);
    p.tone = clamp01(p.tone + joyY_ * 0.5f);
  }
  p.vel = vel;
  p.variant = tr.variant;
  return p;
}

SynthParams Groovebox::synthParams() const {
  const Track& tr = pat().tracks[kSynthTrack];
  auto raw = [&](int s) -> int {
    int v = tr.p[s];
    if ((synthLockMask_ >> s) & 1) v = synthLock_[s];
    return v;
  };
  auto val = [&](int s) -> float { return raw(s) / 127.f; };
  auto atk = [&](int s) { return 0.0005f * std::exp2(val(s) * 13.f); };
  auto tm = [&](int s) { return 0.003f * std::exp2(val(s) * 11.5f); };
  SynthParams p;
  p.att = atk(S_AMP_A); p.dec = tm(S_AMP_D); p.sus = val(S_AMP_S); p.rel = tm(S_AMP_R);
  p.fatt = atk(S_FIL_A); p.fdec = tm(S_FIL_D); p.fsus = val(S_FIL_S); p.frel = tm(S_FIL_R);
  p.detune = val(S_DETUNE);
  for (int o = 0; o < 4; o++) {
    p.wave[o] = raw(S_WAVE_A + o) % kNumWaves;
    p.tune[o] = float(tuneSemis(raw(S_TUNE_A + o)));
  }
  p.lfoRate = 0.05f * std::exp2(val(S_LFO1_RATE) * 8.6f);
  p.lfoAmt = val(S_LFO1_AMT);
  p.lfo2Rate = 0.05f * std::exp2(val(S_LFO2_RATE) * 8.6f);
  p.lfo2Amt = val(S_LFO2_AMT);
  p.cutoff = val(S_CUTOFF);
  p.reso = val(S_RESO);
  p.fenv = val(S_FENV);
  for (int o = 0; o < 4; o++) {
    int b = S_OENV + o * 4;
    p.oatt[o] = atk(b);
    p.odec[o] = tm(b + 1);
    p.osus[o] = val(b + 2);
    p.orel[o] = raw(b + 3) >= 127 ? 20.f : tm(b + 3);  // full release = "until the amp envelope ends"
  }
  p.vcfMode = raw(S_VCF_MODE) % kVcfModes;
  p.vcfCut = val(S_VCF_CUT);
  p.vcfRes = val(S_VCF_RES);
  p.vcfEnv = val(S_VCF_ENV);
  bool live = (cur_ == kSynthTrack || keysMode_) && !heldSteps_ && !held(B_FUNC);
  p.vx = clampf(val(S_VEC_X) * 2.f - 1.f + (live ? joyX_ : 0.f), -1.f, 1.f);
  p.vy = clampf(val(S_VEC_Y) * 2.f - 1.f + (live ? joyY_ : 0.f), -1.f, 1.f);
  return p;
}

AcidParams Groovebox::acidParams() const {
  const Track& tr = pat().tracks[kAcidTrack];
  auto val = [&](int s) -> float {
    int v = tr.p[s];
    if ((acidLockMask_ >> s) & 1) v = acidLock_[s];
    return v / 127.f;
  };
  AcidParams p;
  p.cutoff = val(A_CUTOFF);
  p.reso = val(A_RESO);
  p.envMod = val(A_ENVMOD);
  p.decay = val(A_DECAY);
  p.accent = val(A_ACCENT);
  p.drive = val(A_DRIVE);
  p.slide = val(A_SLIDE);
  int tv = ((acidLockMask_ >> A_TUNE) & 1) ? acidLock_[A_TUNE] : tr.p[A_TUNE];
  p.tune = float(acidSemis(tv));
  p.square = tr.variant == 1;
  if (cur_ == kAcidTrack && !keysMode_ && !heldSteps_ && !held(B_FUNC)) {  // joystick = live cutoff / env mod
    p.cutoff = clamp01(p.cutoff + joyX_ * 0.5f);
    p.envMod = clamp01(p.envMod + joyY_ * 0.5f);
  }
  return p;
}

void Groovebox::fire(int t, const Step* st, float vel, bool ratchet) {
  if (t == kAcidTrack) {
    if (!st || st->notes[0] == kNoNote) return;
    int sps = stepSamples();
    int gate;
    if (ratchet || st->ratchet > 1) gate = ratchInterval_[t] / 2;
    else if (st->slide) gate = st->gate * sps + sps / 4;  // overlaps the next step: it will glide
    else gate = st->gate * sps - sps / 2;
    if (gate < 64) gate = 64;
    acidLockMask_ = st->lockMask;
    std::memcpy(acidLock_, st->lock, kNumParams);
    acidLockLeft_ = gate;
    acid_.control(acidParams(), 0);
    acid_.noteOn(st->notes[0], vel, st->accent != 0, gate);
    return;
  }
  if (t == kSynthTrack) {
    if (!st) return;
    int gate = (ratchet || st->ratchet > 1) ? ratchInterval_[t] * 7 / 10 : st->gate * stepSamples() - stepSamples() / 8;
    if (gate < 64) gate = 64;
    synthLockMask_ = st->lockMask;
    std::memcpy(synthLock_, st->lock, kNumParams);
    synthLockLeft_ = gate;
    synth_.control(synthParams(), 0);  // apply locks before the attack
    for (int i = 0; i < kMaxNotes; i++)
      if (st->notes[i] != kNoNote) synth_.noteOn(st->notes[i], vel, gate);
    return;
  }
  DrumParams p = drumParams(t, st, vel);
  Mix& m = mix_[t];
  m.gl = std::cos(p.pan * kPi * 0.5f);
  m.gr = std::sin(p.pan * kPi * 0.5f);
  m.send = p.send * p.send;
  m.crush = p.crush;
  switch (t) {
    case TT_KICK: kick_.trigger(p); break;
    case TT_SNARE: snare_.trigger(p); break;
    case TT_HAT: hat_.trigger(p); break;
    case TT_PERC: perc_.trigger(p); break;
  }
}

// ------------------------------------------------------------------ audio
void Groovebox::process(float* outL, float* outR, int n) {
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

void Groovebox::render(float* L, float* R, int n) {
  float syn[16], acd[16];
  synth_.render(syn, n);
  acid_.render(acd, n);
  float acidSend = pat().tracks[kAcidTrack].p[P_SEND] / 127.f;
  acidSend *= acidSend;
  float synSend = pat().tracks[kSynthTrack].p[P_SEND] / 127.f;
  synSend *= synSend;
  for (int i = 0; i < n; i++) {
    float d[4] = {kick_.tick(), snare_.tick(), hat_.tick(), perc_.tick()};
    float l = 0, r = 0, send = 0;
    for (int t = 0; t < 4; t++) {
      float x = mix_[t].cr.apply(d[t], mix_[t].crush);
      l += x * mix_[t].gl;
      r += x * mix_[t].gr;
      send += x * mix_[t].send;
    }
    float s = syn[i];
    l += s * 0.707f;
    r += s * 0.707f;
    send += s * synSend;
    float a = acd[i] * 0.8f;
    l += a * 0.707f;
    r += a * 0.707f;
    send += a * acidSend;
    float dl, dr;
    delay_.tick(send * 0.7f, dl, dr);
    L[i] = softclip((l + dl) * 0.8f);
    R[i] = softclip((r + dr) * 0.8f);
  }
}

// ------------------------------------------------------------------ LEDs
uint16_t Groovebox::stepLeds() const {
  bool blink = (clock_ / uint64_t(sr_ * 0.15f)) & 1;
  const Track& tr = pat().tracks[cur_];
  uint16_t m = 0;
  if (held(B_FUNC)) {
    m = uint16_t(1u << cur_);
    if (mode_ == M_MUTE) m |= 1u << F_MUTE;
    if (keysMode_) m |= 1u << F_KEYS;
    return m;
  }
  switch (mode_) {
    case M_SETUP:
      m = uint16_t(1u << proj_.clockSrc) | uint16_t(1u << (4 + (proj_.syncPpqn & 3)));
      if (proj_.clockOut) m |= 1u << 8;
      if (proj_.syncOut) m |= 1u << 9;
      return m;
    case M_MUTE:
      for (int t = 0; t < kNumTracks; t++) if (!mute_[t]) m |= 1u << t;
      return m;
    case M_PATTERN:
      m = uint16_t(1u << proj_.current);
      if (queuedPattern_ >= 0 && blink) m |= 1u << queuedPattern_;
      return m;
    case M_LEN:
      return uint16_t((1u << tr.length) - 1);
    case M_EUCLID:
      return blink ? uint16_t((1u << tr.length) - 1) : 0;
    default: break;
  }
  for (int s = 0; s < tr.length; s++) if (tr.steps[s].on) m |= 1u << s;
  if (playing_) m ^= uint16_t(1u << stepPos_[cur_]);  // running light
  return m;
}

bool Groovebox::led(int l) const {
  bool synth = cur_ == kSynthTrack;
  switch (l) {
    case LED_ENV_FIL: case LED_ENV_AMP: case LED_ENV_MIX: {
      // OSC ENV blinks the AMP LED, VOICE FLT blinks the FIL LED
      bool blink = (clock_ / uint64_t(sr_ * 0.2f)) & 1;
      int page = envSel_;
      if (page == EP_OSC) page = blink ? EP_AMP : -1;
      if (page == EP_VCF) page = blink ? EP_FIL : -1;
      return synth && page == l - LED_ENV_FIL;
    }
    case LED_LFO1: case LED_LFO2: return synth && lfoSel_ == l - LED_LFO1;
    case LED_OSC_A: case LED_OSC_B: case LED_OSC_C: case LED_OSC_D:
      return (synth ? oscSel_ : pat().tracks[cur_].variant) == l - LED_OSC_A;
    case LED_PLAY: return playing_;
    case LED_REC: return recMode_;
    case LED_FUNC: return held(B_FUNC) || mode_ != M_NONE;
    default: return false;
  }
}

// ------------------------------------------------------------------ display (128x32)
// Layout rule for a 22 mm wide screen: one idea per screen, the thing you are
// changing in 2x font, labels in 1x, no grids (the step LEDs show the pattern).
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

static void drawLine(Display& d, int x0, int y0, int x1, int y1) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    d.set(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// title + big value on the left, the ADSR shape on the right (hi = stage being edited)
static void drawEnvMsg(Display& d, const char* title, const char* value, const char* badge, const uint8_t* e, int hi) {
  d.text(0, 0, title);
  if (badge && *badge) {
    int w = Display::textWidth(badge) + 4;
    d.fill(Display::W - w, 0, w, 8);
    d.text(Display::W - w + 2, 0, badge, false);
  }
  int scale = Display::textWidth(value, 2) <= 58 ? 2 : 1;
  d.text(0, scale == 2 ? 13 : 16, value, true, scale);
  const int gx = 62, top = 11, bot = 31;
  int a = 1 + e[0] * 16 / 127, dcy = 1 + e[1] * 16 / 127, r = 1 + e[3] * 16 / 127, hold = 10;
  int sy = bot - e[2] * (bot - top) / 127;
  int x0 = gx, x1 = x0 + a, x2 = x1 + dcy, x3 = x2 + hold, x4 = x3 + r;
  int xs[5] = {x0, x1, x2, x3, x4}, ys[5] = {bot, top, sy, sy, bot};
  for (int i = 0; i < 4; i++) {
    drawLine(d, xs[i], ys[i], xs[i + 1], ys[i + 1]);
    int stage = i;  // 0 attack, 1 decay, 2 sustain, 3 release
    if (stage == hi) { drawLine(d, xs[i], ys[i] - 1, xs[i + 1], ys[i + 1] - 1); drawLine(d, xs[i] + 1, ys[i], xs[i + 1] + 1, ys[i + 1]); }
  }
  d.hline(gx, bot, x4 - gx + 1);
}

void Groovebox::drawDisplay(Display& d) const {
  d.clear();
  if (clock_ < uint64_t(sr_ * 1.2f)) {  // boot splash
    d.text(28, 0, "PRO VS", true, 2);
    d.text(16, 20, "GROOVE  fw 0.2");
    return;
  }
  if (clock_ < msgUntil_) {
    if (envGraphHi_ != -1) { drawEnvMsg(d, title_, value_, badge_, envGraph_, envGraphHi_); return; }
    drawTitleValue(d, title_, value_, bar_, badge_);
    return;
  }

  const Track& tr = pat().tracks[cur_];
  char buf[24];
  if (held(B_FUNC)) {  // legend for FUNC + step
    d.text(0, 0, "1-6 TRACK  7 MUTE");
    d.text(0, 8, "8 CLR ALL  9 COPY");
    d.text(0, 16, "10 PST 11 LEN 12 EUC");
    d.text(0, 24, "13 RND 14 PAT 15 KEY");
    return;
  }
  switch (mode_) {
    case M_MUTE: {
      d.text(0, 0, "MUTE: STEP 1-6");
      static const char kL[kNumTracks] = {'K', 'S', 'H', 'P', 'Y', 'A'};
      for (int t = 0; t < kNumTracks; t++) {
        char c[2] = {kL[t], 0};
        int x = t * 21;
        d.text(x + 4, 12, c, true, 2);
        if (mute_[t]) d.invert(x + 1, 10, 19, 18);
      }
      return;
    }
    case M_PATTERN:
      std::snprintf(buf, sizeof buf, "P%02d", proj_.current + 1);
      drawTitleValue(d, "PATTERN: PRESS STEP", buf, -1, "");
      return;
    case M_LEN:
      std::snprintf(buf, sizeof buf, "%d", tr.length);
      drawTitleValue(d, "LENGTH: PRESS STEP", buf, -1, "");
      return;
    case M_EUCLID:
      drawTitleValue(d, "EUCLID: HITS = STEP", "?", -1, "");
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

  // home: big track name + pattern, status line, position bar
  bool blink = (clock_ / uint64_t(sr_ * 0.25f)) & 1;
  d.text(0, 0, kTrackNames[cur_], true, 2);
  int shownPat = (queuedPattern_ >= 0 && blink) ? queuedPattern_ : proj_.current;
  std::snprintf(buf, sizeof buf, "P%02d", shownPat + 1);
  d.text(Display::W - Display::textWidth(buf, 2), 0, buf, true, 2);
  if (queuedPattern_ >= 0) d.hline(Display::W - Display::textWidth(buf, 2), 16, Display::textWidth(buf, 2));

  int n;
  if (!ext()) n = std::snprintf(buf, sizeof buf, "%d", proj_.bpm);
  else if (waitingForClock()) n = std::snprintf(buf, sizeof buf, "WAIT %s", proj_.clockSrc == CLK_MIDI ? "MIDI" : "SYNC");
  else if (tempo() > 0) n = std::snprintf(buf, sizeof buf, "%s %.0f", proj_.clockSrc == CLK_MIDI ? "MIDI" : "SYNC", tempo());
  else n = std::snprintf(buf, sizeof buf, "NO %s", proj_.clockSrc == CLK_MIDI ? "MIDI" : "SYNC");
  if (pat().swing > 50) std::snprintf(buf + n, sizeof buf - n, " S%d", pat().swing);
  d.text(0, 18, buf);
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
  if (keysMode_) flag("KEYS", false);
  if (mute_[cur_]) flag("MUTE", false);
  if (cur_ != kSynthTrack) flag(kVariantNames[cur_][tr.variant], false);
  else { char o[8]; std::snprintf(o, sizeof o, "OCT%+d", octave_); flag(o, false); }

  for (int s = 0; s < tr.length; s++) {  // 16 x 8 px position bar
    int bx = s * 8;
    if (playing_ && stepPos_[cur_] == s) d.fill(bx, 28, 7, 4);
    else if (tr.steps[s].on) d.fill(bx + 2, 30, 3, 2);
    else d.set(bx + 3, 31);
  }
}

}  // namespace pvg
