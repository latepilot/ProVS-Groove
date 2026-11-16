#include "acidgen.h"

#include <cstring>
#include <initializer_list>

namespace pvg {
namespace acid {

// ------------------------------------------------------------------ names, scales
static const char* kStyleNames[ST_COUNT] = {"CLASSIC", "ROLLING", "HYPNO", "STAB", "SYNCO", "DEEP", "RAVE", "WILD"};
static const char* kScaleNames[SC_COUNT] = {"MINOR", "PHRYG", "DORIAN", "HARMON", "PENTA", "BLUES", "MAJOR", "CHROM"};
static const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* styleName(int s) { return (s >= 0 && s < ST_COUNT) ? kStyleNames[s] : "?"; }
const char* scaleName(int s) { return (s >= 0 && s < SC_COUNT) ? kScaleNames[s] : "?"; }
const char* noteName(int pc) { return kNoteNames[((pc % 12) + 12) % 12]; }

static const uint8_t kScales[SC_COUNT][12] = {
    {0, 2, 3, 5, 7, 8, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 11},
    {0, 3, 5, 7, 10},       {0, 3, 5, 6, 7, 10},    {0, 2, 4, 5, 7, 9, 11}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
static const uint8_t kScaleSize[SC_COUNT] = {7, 7, 7, 7, 5, 6, 7, 12};

int scaleSize(int sc) { return kScaleSize[sc % SC_COUNT]; }
int scaleSemi(int sc, int deg) {
  int n = scaleSize(sc);
  int oct = deg >= 0 ? deg / n : -((-deg + n - 1) / n);
  return kScales[sc % SC_COUNT][deg - oct * n] + 12 * oct;
}
int baseNote(const GenCtx& c) { return 36 + (c.gen.root % 12) + (c.line ? 12 : 0); }  // A: C2.., B: an octave up

// How acid lines use each interval above the root (the root itself is set per style)
static const float kSemiWeight[12] = {0, 0.45f, 0.3f, 0.65f, 0.35f, 0.4f, 0.3f, 0.75f, 0.35f, 0.3f, 0.6f, 0.3f};

// ------------------------------------------------------------------ styles
struct StyleDef {
  float density, tie, accent, slide, octUp, octDn;
  int cell;          // motif length in steps
  float rootBias;    // how often lines return to the root
  float leap;        // 0 = stepwise, 1 = jumpy
  float offbeat;     // syncopation
  float firstHit;    // chance step 1 is a note
};
static const StyleDef kStyles[ST_COUNT] = {
    //  dens  tie   acc   slide octUp octDn cell root  leap  off   first
    {0.62f, 0.08f, 0.28f, 0.24f, 0.18f, 0.05f, 8, 0.45f, 0.35f, 0.45f, 0.92f},  // CLASSIC
    {0.92f, 0.02f, 0.24f, 0.12f, 0.26f, 0.04f, 4, 0.70f, 0.25f, 0.20f, 1.00f},  // ROLLING
    {0.48f, 0.26f, 0.16f, 0.48f, 0.12f, 0.08f, 8, 0.55f, 0.15f, 0.35f, 0.95f},  // HYPNO
    {0.42f, 0.05f, 0.55f, 0.10f, 0.22f, 0.10f, 8, 0.40f, 0.45f, 0.85f, 0.55f},  // STAB
    {0.58f, 0.10f, 0.32f, 0.22f, 0.22f, 0.06f, 16, 0.40f, 0.35f, 0.80f, 0.65f}, // SYNCO
    {0.44f, 0.32f, 0.14f, 0.36f, 0.05f, 0.22f, 8, 0.62f, 0.12f, 0.30f, 1.00f},  // DEEP
    {0.76f, 0.05f, 0.40f, 0.26f, 0.42f, 0.08f, 4, 0.40f, 0.55f, 0.40f, 0.95f},  // RAVE
    {0.64f, 0.10f, 0.36f, 0.34f, 0.30f, 0.14f, 16, 0.22f, 0.75f, 0.50f, 0.70f}, // WILD
};

static float d01(const GenCtx& c) { return c.gen.density / 127.f; }
static float w01(const GenCtx& c) { return c.gen.wild / 127.f; }
static const StyleDef& sty(const GenCtx& c) { return kStyles[c.gen.style % ST_COUNT]; }
static bool chance(Rng& r, float p) { return r.uni() < p; }
static int len(const Line& l) { return l.length < 1 ? 16 : (l.length > 16 ? 16 : l.length); }

// metric weight of a 16th position (1 = downbeat), bent towards the off-beats by `off`
static float metric(int s, float off) {
  float w;
  if (s % 8 == 0) w = 1.05f - off * 0.5f;
  else if (s % 4 == 0) w = 1.f - off * 0.6f;
  else if (s % 2 == 0) w = 0.72f + off * 0.2f;
  else w = 0.5f + off * 0.8f;
  return w;
}

// ------------------------------------------------------------------ pitches
// scale degree of a pitch relative to the line's base (snaps to the nearest)
static int degreeOf(int pitch, const GenCtx& c) {
  int rel = pitch - baseNote(c);
  int best = 0, bestD = 99;
  for (int d = -8; d <= 16; d++) {
    int dd = rel - scaleSemi(c.gen.scale, d);
    if (dd < 0) dd = -dd;
    if (dd < bestD) { bestD = dd; best = d; }
  }
  return best;
}

static int pickPoolDegree(const GenCtx& c, Rng& r) {
  const StyleDef& st = sty(c);
  int n = scaleSize(c.gen.scale);
  float w[13], sum = 0;
  for (int d = 0; d < n; d++) {
    int semi = scaleSemi(c.gen.scale, d);
    w[d] = d == 0 ? 1.f + 3.f * st.rootBias : kSemiWeight[semi % 12];
    if (c.gen.scale == SC_CHROM && (semi == 1 || semi == 6)) w[d] *= 0.5f;
    sum += w[d];
  }
  w[n] = 0.3f * st.leap;  // the root an octave up
  sum += w[n];
  float x = r.uni() * sum;
  for (int d = 0; d <= n; d++) { x -= w[d]; if (x <= 0) return d; }
  return 0;
}

static int pickDegree(const GenCtx& c, Rng& r, int prev, bool havePrev) {
  const StyleDef& st = sty(c);
  float leap = clamp01(st.leap * (0.6f + 0.8f * w01(c)));
  if (havePrev && prev != 0 && chance(r, st.rootBias * 0.45f)) return 0;  // back to the pedal
  if (havePrev && chance(r, (1.f - leap) * 0.7f)) {  // stepwise from the previous note
    int step = chance(r, 0.7f) ? 1 : 2;
    float up = prev > 3 ? 0.3f : (prev < 0 ? 0.7f : 0.5f);  // drift back towards the root
    int d = prev + (chance(r, up) ? step : -step);
    int n = scaleSize(c.gen.scale);
    if (d < -2) d = -2;
    if (d > n + 1) d = n + 1;
    return d;
  }
  return pickPoolDegree(c, r);
}

// ------------------------------------------------------------------ building blocks
static void genRhythm(bool on[kSteps], int L, const GenCtx& c, Rng& r) {
  const StyleDef& st = sty(c);
  float dens = clampf(st.density * (0.35f + 1.3f * d01(c)), 0.08f, 0.98f);
  float var = 0.06f + 0.22f * w01(c);
  int cell = st.cell < L ? st.cell : L;
  bool cellOn[kSteps];
  for (int s = 0; s < cell; s++) {
    float p = dens * metric(s, st.offbeat);
    if (c.line == 1 && c.other) {  // B answers A: fewer notes where A plays
      const Step& o = c.other->steps[s % kSteps];
      p *= o.gate ? 0.55f : 1.25f;
    }
    cellOn[s] = chance(r, clamp01(p));
  }
  if (c.line == 0) cellOn[0] = chance(r, st.firstHit);
  // keep the style recognisable: a minimum number of notes per cell
  int need = int(dens * cell * (0.3f + 0.5f * st.density) + 0.5f), have = 0;
  for (int s = 0; s < cell; s++) have += cellOn[s];
  for (int guard = 0; have < need && guard < 64; guard++) {
    int s = 1 + int(r.next() % unsigned(cell > 1 ? cell - 1 : 1));
    if (s < cell && !cellOn[s] && chance(r, metric(s, st.offbeat) * 0.9f)) { cellOn[s] = true; have++; }
  }
  for (int s = 0; s < L; s++) {
    on[s] = cellOn[s % cell];
    if (s >= cell && s != 0 && chance(r, var)) on[s] = !on[s];
  }
  // turnaround: re-roll the tail of the pattern
  if (L >= 8 && chance(r, 0.35f + 0.35f * w01(c))) {
    int tail = cell < 4 ? cell : 4;
    for (int s = L - tail; s < L; s++) on[s] = chance(r, clamp01(dens * metric(s, st.offbeat)));
  }
  for (int s = L; s < kSteps; s++) on[s] = false;
  int count = 0;
  for (int s = 0; s < L; s++) count += on[s];
  int minNotes = c.line == 0 ? (L / 5 > 3 ? L / 5 : 3) : 2;
  if (minNotes > L) minNotes = L;
  for (int guard = 0; count < minNotes && guard < 64; guard++) {
    int s = int(r.next() % unsigned(L));
    if (!on[s] && chance(r, metric(s, st.offbeat) * 0.8f)) { on[s] = true; count++; }
  }
  if (count == L && c.gen.style != ST_ROLLING && L > 4) {  // leave room to breathe
    int s = 1 + 2 * int(r.next() % unsigned(L / 2));
    if (s < L) on[s] = false;
  }
}

// motif-based pitches for the steps that are on
static void genPitches(Line& l, const GenCtx& c, Rng& r) {
  const StyleDef& st = sty(c);
  int L = len(l);
  int cell = st.cell < 8 ? 8 : st.cell;  // short rhythm cells still get an 8-step melody
  if (cell > L) cell = L;
  int base = baseNote(c);
  int deg[kSteps];
  bool has[kSteps] = {};
  int prev = 0;
  bool havePrev = false;
  // the motif: one cell
  for (int s = 0; s < cell; s++) {
    if (!l.steps[s].gate) continue;
    deg[s] = pickDegree(c, r, prev, havePrev);
    if (s == 0 && chance(r, 0.9f)) deg[s] = 0;  // anchor on the root
    has[s] = true;
    prev = deg[s];
    havePrev = true;
  }
  // every motif touches the root at least once
  {
    bool root = false;
    int firstNote = -1;
    for (int s = 0; s < cell; s++) if (has[s]) { if (firstNote < 0) firstNote = s; if (deg[s] % scaleSize(c.gen.scale) == 0) root = true; }
    if (!root && firstNote >= 0) deg[firstNote] = 0;
  }
  // a motif that is one note over and over is only right for ROLLING
  int firstDeg = 0, motifNotes = 0;
  bool allSame = true;
  for (int s = 0; s < cell; s++) {
    if (!has[s]) continue;
    if (motifNotes++ == 0) firstDeg = deg[s];
    else if (deg[s] != firstDeg) allSame = false;
  }
  if (allSame && motifNotes >= 2 && c.gen.style != ST_ROLLING) {
    int tries = 0, pick;
    do { pick = pickPoolDegree(c, r); } while (pick == firstDeg && ++tries < 8);
    if (pick == firstDeg) pick = firstDeg + 2;
    for (int s = cell - 1; s > 0; s--)
      if (has[s]) { deg[s] = pick; break; }
  }
  // repetitions copy the motif, fresh notes where the rhythm changed
  for (int s = cell; s < L; s++) {
    if (!l.steps[s].gate) continue;
    int m = s % cell;
    if (has[m] && !chance(r, 0.08f + 0.2f * w01(c))) deg[s] = deg[m];
    else deg[s] = pickDegree(c, r, prev, havePrev);
    has[s] = true;
    prev = deg[s];
    havePrev = true;
  }
  // call and response: shift part of the second half
  if (L >= cell * 2 && chance(r, 0.35f + 0.4f * w01(c))) {
    static const int kShift[5] = {3, 4, -1, 2, 5};
    int sh = kShift[r.next() % 5];
    if (scaleSize(c.gen.scale) == 5 && sh > 3) sh = 3;
    int from = chance(r, 0.5f) ? L / 2 : L - cell;
    for (int s = from; s < L; s++)
      if (has[s] && !(s % 8 == 0 && chance(r, 0.5f))) deg[s] += sh;
  }
  int n = scaleSize(c.gen.scale);
  for (int s = 0; s < L; s++) {
    if (!has[s]) continue;
    while (deg[s] > n + 2) deg[s] -= n;  // keep the line within about an octave and a half
    while (deg[s] < -3) deg[s] += n;
    l.steps[s].pitch = uint8_t(base + scaleSemi(c.gen.scale, deg[s]));
  }
}

static void genOctaves(Line& l, const GenCtx& c, Rng& r) {
  const StyleDef& st = sty(c);
  int L = len(l);
  int cell = st.cell < L ? st.cell : L;
  float wild = 0.5f + w01(c);
  int8_t cellOct[kSteps];
  for (int s = 0; s < L; s++) {
    Step& x = l.steps[s];
    if (!x.gate) { x.oct = 0; continue; }
    if (s < cell) {
      float pos = (s % 2 == 1) ? 1.5f : (s % 4 == 0 ? 0.4f : 1.f);
      int8_t o = 0;
      if (chance(r, clamp01(st.octUp * wild * pos))) o = 1;
      else if (chance(r, st.octDn * wild * (s % 4 == 0 ? 1.3f : 0.7f))) o = -1;
      if (o == 1 && x.pitch >= baseNote(c) + 12) o = 0;  // already high
      cellOct[s] = o;
      x.oct = o;
    } else {
      x.oct = chance(r, 0.12f) ? int8_t(chance(r, st.octUp * wild) ? 1 : 0) : cellOct[s % cell];
      if (x.oct == 1 && x.pitch >= baseNote(c) + 12) x.oct = 0;
    }
  }
}

static int fullNote(const Step& s) { return s.pitch + 12 * s.oct; }

static void genAccents(Line& l, const GenCtx& c, Rng& r) {
  const StyleDef& st = sty(c);
  int L = len(l);
  int cell = st.cell < L ? st.cell : L;
  float amt = st.accent * (0.6f + 0.8f * w01(c));
  uint8_t cellAcc[kSteps] = {};
  int count = 0, notes = 0;
  for (int s = 0; s < L; s++) {
    Step& x = l.steps[s];
    if (!x.gate) { x.accent = 0; continue; }
    notes++;
    if (s < cell || !l.steps[s % cell].gate) {
      float pos = (s % 2 == 1) ? 1.25f : (s % 4 == 0 ? 0.7f : 1.f);
      bool afterRest = !l.steps[(s + L - 1) % L].gate;
      x.accent = chance(r, clamp01(amt * pos + (afterRest ? 0.2f : 0.f)));
      if (s < cell) cellAcc[s] = x.accent;
    } else {
      x.accent = chance(r, 0.1f) ? !cellAcc[s % cell] : cellAcc[s % cell];
    }
    count += x.accent;
  }
  for (int guard = 0; count * 2 > notes + 1 && guard < 64; guard++) {  // at most about half
    int s = int(r.next() % unsigned(L));
    if (l.steps[s].accent) { l.steps[s].accent = 0; count--; }
  }
}

static void genSlides(Line& l, const GenCtx& c, Rng& r) {
  const StyleDef& st = sty(c);
  int L = len(l);
  float amt = st.slide * (0.6f + 0.8f * w01(c));
  for (int s = 0; s < L; s++) {
    Step& x = l.steps[s];
    x.slide = 0;
    if (!x.gate) continue;
    const Step& n = l.steps[(s + 1) % L];
    float p;
    if (n.gate) {
      int a = fullNote(x), b = fullNote(n);
      p = amt * (a != b ? 1.3f : 0.5f) * (x.oct != n.oct ? 1.5f : 1.f);
    } else {
      p = amt * 0.15f;
    }
    x.slide = chance(r, clamp01(p));
  }
  // structure: a slide that worked in the first cell tends to come back
  int cell = st.cell < L ? st.cell : L;
  for (int s = cell; s < L; s++) {
    Step& x = l.steps[s];
    if (x.gate && l.steps[s % cell].slide && l.steps[(s + 1) % L].gate && chance(r, 0.5f)) x.slide = 1;
  }
}

static void genTies(Line& l, const GenCtx& c, Rng& r) {
  const StyleDef& st = sty(c);
  int L = len(l);
  for (int s = 0; s < L; s++) {
    Step& x = l.steps[s];
    x.len = 1;
    if (!x.gate || l.steps[(s + 1) % L].gate || s + 1 >= L) continue;
    if (chance(r, st.tie * 2.f)) {
      x.len = 2;
      if (s + 2 < L && !l.steps[s + 2].gate && st.tie > 0.2f && chance(r, 0.4f)) x.len = 3;
    }
  }
}

static void genProb(Line& l, const GenCtx& c, Rng& r) {
  float w = w01(c);
  for (int s = 0; s < len(l); s++) {
    Step& x = l.steps[s];
    x.prob = 8;
    if (x.gate && w > 0.55f && s % 4 != 0 && chance(r, (w - 0.55f) * 0.35f)) x.prob = uint8_t(5 + r.next() % 2);
  }
}

// ------------------------------------------------------------------ operations
void clearLine(Line& l, int rootNote) {
  for (auto& s : l.steps) {
    std::memset(&s, 0, sizeof s);
    s.pitch = uint8_t(rootNote);
    s.len = 1;
    s.prob = 8;
  }
  if (l.length < 1 || l.length > 16) l.length = 16;
}

void generateLine(Line& l, const GenCtx& c, Rng& r) {
  int L = len(l);
  clearLine(l, baseNote(c));
  l.length = uint8_t(L);
  bool on[kSteps];
  genRhythm(on, L, c, r);
  for (int s = 0; s < L; s++) l.steps[s].gate = on[s];
  genPitches(l, c, r);
  genOctaves(l, c, r);
  genAccents(l, c, r);
  genSlides(l, c, r);
  genTies(l, c, r);
  genProb(l, c, r);
}

void regenRhythm(Line& l, const GenCtx& c, Rng& r) {
  int L = len(l);
  uint8_t pitch[kSteps];
  int8_t oct[kSteps];
  int n = 0;
  for (int s = 0; s < L; s++)
    if (l.steps[s].gate) { pitch[n] = l.steps[s].pitch; oct[n] = l.steps[s].oct; n++; }
  if (n == 0) { generateLine(l, c, r); return; }
  bool on[kSteps];
  genRhythm(on, L, c, r);
  int k = 0;
  for (int s = 0; s < L; s++) {
    Step& x = l.steps[s];
    x.gate = on[s];
    if (on[s]) { x.pitch = pitch[k % n]; x.oct = oct[k % n]; k++; }
  }
  genAccents(l, c, r);
  genSlides(l, c, r);
  genTies(l, c, r);
}

void regenNotes(Line& l, const GenCtx& c, Rng& r) {
  int any = 0;
  for (int s = 0; s < len(l); s++) any += l.steps[s].gate;
  if (!any) { generateLine(l, c, r); return; }
  genPitches(l, c, r);
  genOctaves(l, c, r);
  genSlides(l, c, r);
}

void regenAccSlide(Line& l, const GenCtx& c, Rng& r) {
  genAccents(l, c, r);
  genSlides(l, c, r);
}

void mutate(Line& l, const GenCtx& c, Rng& r, float amount) {
  int L = len(l);
  int base = baseNote(c);
  for (int s = 0; s < L; s++) {
    if (!chance(r, amount)) continue;
    Step& x = l.steps[s];
    float op = r.uni();
    if (x.gate && op < 0.32f) {  // nudge the pitch along the scale
      int d = degreeOf(x.pitch, c) + (chance(r, 0.5f) ? 1 : -1);
      x.pitch = uint8_t(base + scaleSemi(c.gen.scale, d));
    } else if (x.gate && op < 0.47f) {
      x.oct = x.oct == 0 ? (chance(r, 0.75f) ? 1 : -1) : 0;
      if (x.oct == 1 && x.pitch >= base + 12) x.oct = 0;
    } else if (x.gate && op < 0.62f) {
      x.accent = !x.accent;
    } else if (x.gate && op < 0.77f) {
      x.slide = !x.slide;
    } else if (op < 0.92f && s != 0) {  // add or drop a note
      x.gate = !x.gate;
      if (x.gate) {
        x.pitch = uint8_t(base + scaleSemi(c.gen.scale, pickPoolDegree(c, r)));
        x.oct = 0;
        x.len = 1;
        x.prob = 8;
        x.accent = chance(r, sty(c).accent);
      }
    } else if (s + 1 < L) {  // swap with the neighbour
      Step t = x;
      x = l.steps[s + 1];
      l.steps[s + 1] = t;
    }
  }
}

void rotate(Line& l, int dir) {
  int L = len(l);
  Step tmp[kSteps];
  for (int s = 0; s < L; s++) tmp[((s + dir) % L + L) % L] = l.steps[s];
  for (int s = 0; s < L; s++) l.steps[s] = tmp[s];
}

// ------------------------------------------------------------------ drums
static uint16_t bits(std::initializer_list<int> s) {
  uint16_t m = 0;
  for (int x : s) m |= uint16_t(1u << x);
  return m;
}

void generateDrums(Drums& d, const GenSettings& g, Rng& r) {
  float dens = g.density / 127.f, wild = g.wild / 127.f;
  int st = g.style % ST_COUNT;
  bool four = st == ST_CLASSIC || st == ST_ROLLING || st == ST_HYPNO || st == ST_DEEP || st == ST_RAVE;
  uint16_t kick, clap, ch = 0, oh = 0;
  if (four) {
    kick = bits({0, 4, 8, 12});
    if (chance(r, 0.15f + 0.4f * wild)) { static const int kx[4] = {14, 10, 7, 15}; kick |= uint16_t(1u << kx[r.next() % 4]); }
  } else if (st == ST_WILD) {
    int k = 5 + int(r.next() % 3), rot = int(r.next() % 4);
    kick = 1;
    for (int s = 0; s < 16; s++) if (((s * k) % 16) < k) kick |= uint16_t(1u << ((s + rot) % 16));
  } else {
    static const uint16_t kBroken[4] = {0x0441, 0x2481, 0x0909, 0x0541};  // {0,6,10} {0,7,10,13} {0,3,8,11} {0,6,8,10}
    kick = kBroken[r.next() % 4];
  }
  clap = bits({4, 12});
  if ((st == ST_HYPNO || st == ST_DEEP) && chance(r, 0.5f)) clap = bits({12});
  if (st == ST_RAVE && chance(r, 0.5f)) clap |= bits({15});
  if (st == ST_SYNCO && chance(r, 0.6f)) clap |= uint16_t(1u << (chance(r, 0.5f) ? 7 : 15));
  if (st == ST_WILD) clap = uint16_t((chance(r, 0.5f) ? bits({4, 12}) : bits({4, 10, 12})));
  if (st == ST_STAB || st == ST_SYNCO) {
    for (int s = 0; s < 16; s += 2) if (chance(r, 0.6f + 0.4f * dens)) ch |= uint16_t(1u << s);
    oh = chance(r, 0.5f) ? bits({6, 14}) : bits({2, 10});
  } else {
    oh = bits({2, 6, 10, 14});
    for (int s = 0; s < 16; s++) {
      bool eighth = s % 2 == 0;
      float p = (st == ST_HYPNO || st == ST_DEEP) ? 0.25f + 0.4f * dens : 0.45f + 0.5f * dens;
      if (eighth) p += 0.2f;
      if (chance(r, clamp01(p))) ch |= uint16_t(1u << s);
    }
    if (st == ST_ROLLING || st == ST_RAVE) ch |= 0x5555;  // straight 8ths at least
  }
  ch &= uint16_t(~oh);  // the open hat chokes the closed one anyway
  d.hits[DI_KICK] = kick;
  d.hits[DI_CLAP] = clap;
  d.hits[DI_CH] = ch;
  d.hits[DI_OH] = oh;
  if (d.length < 1 || d.length > 16) d.length = 16;
  regenDrumAccents(d, g, r);
}

void regenDrumAccents(Drums& d, const GenSettings& g, Rng& r) {
  float wild = g.wild / 127.f;
  uint16_t acc = 0;
  for (int s = 0; s < 16; s++) {
    bool hit = (d.hits[DI_CH] | d.hits[DI_OH] | d.hits[DI_KICK]) & (1u << s);
    if (!hit) continue;
    float p = (s % 4 == 2) ? 0.6f : (s % 4 == 0 ? 0.3f : 0.1f + 0.3f * wild);
    if (chance(r, p)) acc |= uint16_t(1u << s);
  }
  d.accent = acc;
}

void mutateDrums(Drums& d, const GenSettings& g, Rng& r, float amount) {
  int st = g.style % ST_COUNT;
  bool four = st == ST_CLASSIC || st == ST_ROLLING || st == ST_HYPNO || st == ST_DEEP || st == ST_RAVE;
  for (int s = 0; s < 16; s++) {
    if (!chance(r, amount)) continue;
    uint16_t b = uint16_t(1u << s);
    float op = r.uni();
    if (op < 0.55f) {
      if (!(d.hits[DI_OH] & b)) d.hits[DI_CH] ^= b;
    } else if (op < 0.75f) {
      if (!(four && s % 4 == 0) && s != 0) d.hits[DI_KICK] ^= b;
    } else if (op < 0.85f) {
      if (s != 4 && s != 12) d.hits[DI_CLAP] ^= b;
    } else {
      d.accent ^= b;
    }
  }
  d.hits[DI_CH] &= uint16_t(~d.hits[DI_OH]);
}

}  // namespace acid
}  // namespace pvg
