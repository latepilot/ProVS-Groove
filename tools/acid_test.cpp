// ProVS Acid: self-tests (generator rules, panel, audio), demo WAV, CPU benchmark.
// Usage: acid-test [out_dir]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../engine/acidbox.h"

using namespace pvg;
using namespace pvg::acid;

static AcidBox ab;
static int failures = 0;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (cond) std::printf("  ok   " __VA_ARGS__);           \
    else { std::printf("  FAIL " __VA_ARGS__); failures++; } \
    std::printf("\n");                                      \
  } while (0)

static const float SR = 48000.f;
struct Stats { float peak = 0; double sum2 = 0; long n = 0; bool nan = false; };

static void run(int frames, std::vector<float>* wav = nullptr, Stats* st = nullptr) {
  float L[256], R[256];
  while (frames > 0) {
    int n = frames < 256 ? frames : 256;
    ab.process(L, R, n);
    for (int i = 0; i < n; i++) {
      if (st) {
        if (!std::isfinite(L[i]) || !std::isfinite(R[i])) st->nan = true;
        float a = std::fabs(L[i]) > std::fabs(R[i]) ? std::fabs(L[i]) : std::fabs(R[i]);
        if (a > st->peak) st->peak = a;
        st->sum2 += 0.5 * (L[i] * L[i] + R[i] * R[i]);
        st->n++;
      }
      if (wav) { wav->push_back(L[i]); wav->push_back(R[i]); }
    }
    frames -= n;
  }
}
static void tick() { run(16); }
static void stepTap(int s) { ab.stepDown(s); tick(); ab.stepUp(s); tick(); }
static void press(int b) { ab.buttonDown(b); tick(); ab.buttonUp(b); tick(); }
static void func(int s) { ab.buttonDown(B_FUNC); tick(); stepTap(s); ab.buttonUp(B_FUNC); tick(); }
static void key(int k) { ab.keyDown(k); tick(); ab.keyUp(k); tick(); }

static void writeWav(const std::string& path, const std::vector<float>& s) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  uint32_t dataBytes = uint32_t(s.size() * 2), sr = 48000, byteRate = sr * 4, riff = 36 + dataBytes, fmtLen = 16;
  uint16_t ch = 2, bits = 16, align = 4, fmt = 1;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&sr, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
  for (float x : s) { int16_t v = int16_t(std::lround(clampf(x, -1.f, 1.f) * 32767.f)); std::fwrite(&v, 2, 1, f); }
  std::fclose(f);
}

static void printOled() {
  Display d;
  ab.drawDisplay(d);
  for (int y = 0; y < Display::H; y++) {
    std::printf("    ");
    for (int x = 0; x < Display::W; x++) std::putchar(d.get(x, y) ? '#' : '.');
    std::putchar('\n');
  }
}

static bool inScale(int pitch, const GenCtx& c) {
  int rel = ((pitch - baseNote(c)) % 12 + 12) % 12;
  for (int d = 0; d < scaleSize(c.gen.scale); d++) if (scaleSemi(c.gen.scale, d) == rel) return true;
  return false;
}
static std::string lineStr(const Line& l) {
  std::string s;
  for (int i = 0; i < l.length; i++) {
    const Step& x = l.steps[i];
    if (!x.gate) { s += " .   "; continue; }
    char b[8];
    std::snprintf(b, sizeof b, "%s%d", noteName(x.pitch), x.pitch / 12 - 1);
    std::string n = b;
    n += x.oct > 0 ? '^' : (x.oct < 0 ? 'v' : ' ');
    n += x.accent ? '!' : ' ';
    n += x.slide ? '~' : ' ';
    while (n.size() < 6) n += ' ';
    s += n.substr(0, 5);
  }
  return s;
}

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : "out";

  std::printf("[1] generator rules (8 styles x 8 scales x 12 seeds)\n");
  {
    int bad = 0, total = 0, accOver = 0, slideOnRest = 0, firstMiss = 0;
    double repeatSim = 0;
    int repeatN = 0;
    double rootShare = 0;
    for (int st = 0; st < ST_COUNT; st++)
      for (int sc = 0; sc < SC_COUNT; sc++)
        for (int seed = 0; seed < 12; seed++) {
          GenCtx c;
          c.gen = GenSettings{uint8_t(st), uint8_t(sc), uint8_t((seed * 5) % 12), 64, 50, {0, 0, 0}};
          Rng r;
          r.s = 1234u + uint32_t(seed) * 7919u + uint32_t(st) * 31u + uint32_t(sc);
          Line l;
          l.length = 16;
          generateLine(l, c, r);
          int notes = 0, acc = 0, roots = 0;
          for (int i = 0; i < 16; i++) {
            const Step& x = l.steps[i];
            if (!x.gate) { if (x.slide) slideOnRest++; continue; }
            notes++;
            acc += x.accent;
            if (!inScale(x.pitch, c)) bad++;
            if (((x.pitch - baseNote(c)) % 12 + 12) % 12 == 0) roots++;
          }
          total++;
          if (notes < 2) bad++;
          if (acc * 2 > notes + 1) accOver++;
          if ((st == ST_ROLLING || st == ST_DEEP) && !l.steps[0].gate) firstMiss++;
          if (notes) rootShare += double(roots) / notes;
          if (st == ST_CLASSIC || st == ST_HYPNO || st == ST_DEEP) {  // cell = 8: halves should rhyme
            int same = 0;
            for (int i = 0; i < 8; i++) same += l.steps[i].gate == l.steps[i + 8].gate;
            repeatSim += same / 8.0;
            repeatN++;
          }
        }
    CHECK(bad == 0, "every line has notes and every pitch is in its scale (%d bad of %d)", bad, total);
    CHECK(slideOnRest == 0, "no slides on rests");
    CHECK(accOver == 0, "accents on at most about half of the notes (%d over)", accOver);
    CHECK(firstMiss == 0, "ROLLING / DEEP always start on step 1");
    CHECK(repeatSim / repeatN > 0.7, "motif repeats: halves share %.0f%% of the rhythm", 100 * repeatSim / repeatN);
    CHECK(rootShare / total > 0.2, "lines come home: %.0f%% of notes are the root", 100 * rootShare / total);

    auto avgNotes = [](int dens) {
      double sum = 0;
      for (int seed = 0; seed < 40; seed++) {
        GenCtx c;
        c.gen = GenSettings{ST_CLASSIC, SC_MINOR, 9, uint8_t(dens), 50, {0, 0, 0}};
        Rng r; r.s = 99u + uint32_t(seed) * 13u;
        Line l; l.length = 16;
        generateLine(l, c, r);
        for (auto& x : l.steps) sum += x.gate;
      }
      return sum / 40;
    };
    double lo = avgNotes(15), hi = avgNotes(115);
    CHECK(lo + 3 < hi, "density knob works: %.1f notes at low, %.1f at high", lo, hi);

    // determinism
    GenCtx c; c.gen = GenSettings{ST_RAVE, SC_PHRYG, 5, 70, 70, {0, 0, 0}};
    Rng r1; r1.s = 42; Rng r2; r2.s = 42;
    Line a{}, b{}; a.length = b.length = 16;
    generateLine(a, c, r1);
    generateLine(b, c, r2);
    CHECK(!std::memcmp(&a, &b, sizeof a), "same seed, same line");
    for (int st = 0; st < ST_COUNT; st++) {
      GenCtx e; e.gen = GenSettings{uint8_t(st), SC_MINOR, 9, 64, 50, {0, 0, 0}};
      Rng re; re.s = 2024u + uint32_t(st);
      Line x{}; x.length = 16;
      generateLine(x, e, re);
      std::printf("    %-7s %s\n", styleName(st), lineStr(x).c_str());
    }

    // operations
    Line keep = a;
    Rng r3; r3.s = 7;
    regenNotes(a, c, r3);
    bool rhythmKept = true, notesChanged = false;
    for (int i = 0; i < 16; i++) {
      rhythmKept &= a.steps[i].gate == keep.steps[i].gate;
      notesChanged |= a.steps[i].gate && a.steps[i].pitch != keep.steps[i].pitch;
    }
    CHECK(rhythmKept && notesChanged, "NOTES keeps the rhythm, changes pitches");
    a = keep;
    regenRhythm(a, c, r3);
    std::vector<int> p0, p1;
    for (int i = 0; i < 16; i++) { if (keep.steps[i].gate) p0.push_back(keep.steps[i].pitch); if (a.steps[i].gate) p1.push_back(a.steps[i].pitch); }
    bool order = !p1.empty();
    for (size_t i = 0; i < p1.size(); i++) order &= p1[i] == p0[i % p0.size()];
    CHECK(order, "RHYTHM keeps the order of the notes");
    a = keep;
    mutate(a, c, r3, 0.f);
    CHECK(!std::memcmp(&a, &keep, sizeof a), "mutate 0 changes nothing");
    mutate(a, c, r3, 0.18f);
    int diff = 0;
    for (int i = 0; i < 16; i++) diff += std::memcmp(&a.steps[i], &keep.steps[i], sizeof(Step)) != 0;
    CHECK(diff >= 1 && diff <= 8, "MUTATE changes a few steps (%d)", diff);
    a = keep;
    rotate(a, 1);
    CHECK(!std::memcmp(&a.steps[1], &keep.steps[0], sizeof(Step)) && !std::memcmp(&a.steps[0], &keep.steps[15], sizeof(Step)), "rotate");

    // B answers A
    double overlapB = 0, overlapRand = 0;
    for (int seed = 0; seed < 40; seed++) {
      GenCtx ca; ca.gen = GenSettings{ST_CLASSIC, SC_MINOR, 9, 64, 40, {0, 0, 0}};
      Rng r; r.s = 500u + uint32_t(seed);
      Line la, lb, lx; la.length = lb.length = lx.length = 16;
      generateLine(la, ca, r);
      GenCtx cb = ca; cb.line = 1; cb.other = &la;
      generateLine(lb, cb, r);
      GenCtx cx = ca; cx.line = 1;
      generateLine(lx, cx, r);
      int nb = 0, ob = 0, nx = 0, ox = 0;
      for (int i = 0; i < 16; i++) {
        if (lb.steps[i].gate) { nb++; ob += la.steps[i].gate; }
        if (lx.steps[i].gate) { nx++; ox += la.steps[i].gate; }
      }
      if (nb) overlapB += double(ob) / nb;
      if (nx) overlapRand += double(ox) / nx;
    }
    CHECK(overlapB < overlapRand, "line B avoids line A's notes (%.0f%% vs %.0f%% unaware)", 100 * overlapB / 40, 100 * overlapRand / 40);

    // drums
    bool four = true, choke = true;
    for (int seed = 0; seed < 20; seed++) {
      Drums d{}; d.length = 16;
      Rng r; r.s = 77u + uint32_t(seed);
      GenSettings g{ST_CLASSIC, SC_MINOR, 9, 64, 50, {0, 0, 0}};
      generateDrums(d, g, r);
      four &= (d.hits[DI_KICK] & 0x1111) == 0x1111;
      choke &= (d.hits[DI_CH] & d.hits[DI_OH]) == 0;
      mutateDrums(d, g, r, 0.5f);
      four &= (d.hits[DI_KICK] & 0x1111) == 0x1111;
      choke &= (d.hits[DI_CH] & d.hits[DI_OH]) == 0;
    }
    CHECK(four, "house styles keep four on the floor, even after mutation");
    CHECK(choke, "closed and open hat never on the same step");
  }

  ab.init(SR);
  run(int(SR * 1.3f));

  std::printf("[2] panel: generate / undo / style / scale\n");
  {
    int p = ab.currentPattern();
    Line before = ab.line(p, 0);
    func(AF_GEN);
    bool changed = std::memcmp(&before, &ab.line(p, 0), sizeof(Line)) != 0;
    CHECK(changed, "FUNC+1 generates a new line A");
    Line gen = ab.line(p, 0);
    func(AF_UNDO);
    CHECK(!std::memcmp(&before, &ab.line(p, 0), sizeof(Line)), "FUNC+6 undoes");
    func(AF_UNDO);
    CHECK(!std::memcmp(&gen, &ab.line(p, 0), sizeof(Line)), "FUNC+6 again redoes");
    func(AF_STYLE); stepTap(ST_HYPNO);
    CHECK(ab.project().patterns[p].gen.style == ST_HYPNO, "STYLE mode + step 3 = HYPNO");
    func(AF_SCALE); key(7 + 5);  // keys start at F; key 12 = F
    stepTap(SC_PHRYG);
    CHECK(ab.project().patterns[p].gen.scale == SC_PHRYG && ab.project().patterns[p].gen.root == 5, "SCALE mode: key sets root F, step 2 = phrygian");
    ab.buttonDown(B_FUNC); tick();
    ab.joystick(0.8f, -0.6f); tick();
    ab.joystick(0.f, 0.f); tick();
    ab.buttonUp(B_FUNC); tick();
    CHECK(ab.project().patterns[p].gen.density > 105 && ab.project().patterns[p].gen.wild < 30, "FUNC + joystick sets density / wild (latched: %d %d)",
          ab.project().patterns[p].gen.density, ab.project().patterns[p].gen.wild);
  }

  std::printf("[3] panel: lanes, layers, step edit, step entry, transpose\n");
  {
    int p = ab.currentPattern();
    press(B_ENV_SEL);
    CHECK(ab.lane() == AcidBox::LANE_B && ab.led(LED_ENV_AMP), "ENV selects line B (AMP LED)");
    func(AF_GEN);
    int notesB = 0;
    for (auto& s : ab.line(p, 1).steps) notesB += s.gate;
    CHECK(notesB > 0, "line B generated (%d notes)", notesB);
    press(B_ENV_SEL); press(B_ENV_SEL);
    CHECK(ab.lane() == AcidBox::LANE_A, "back to line A");
    // layers
    ab.stepDown(5); tick(); ab.stepUp(5); tick();  // GATE layer: toggles
    bool g5 = ab.line(p, 0).steps[5].gate;
    press(B_OSC_SEL);
    CHECK(ab.layer() == AcidBox::L_ACCENT && ab.led(LED_OSC_B), "OSC = accent layer");
    int a3 = ab.line(p, 0).steps[3].accent;
    stepTap(3);
    CHECK(ab.line(p, 0).steps[3].accent != a3 && ab.line(p, 0).steps[3].gate, "accent layer: tap toggles accent");
    press(B_OSC_SEL);
    int s3 = ab.line(p, 0).steps[3].slide;
    stepTap(3);
    CHECK(ab.line(p, 0).steps[3].slide != s3, "slide layer: tap toggles slide");
    press(B_OSC_SEL);
    stepTap(3);
    CHECK(ab.line(p, 0).steps[3].oct == 1 || ab.line(p, 0).steps[3].oct == -1 || ab.line(p, 0).steps[3].oct == 0, "octave layer cycles");
    press(B_OSC_SEL);
    CHECK(ab.layer() == AcidBox::L_GATE && ab.line(p, 0).steps[5].gate == g5, "back to gate layer");
    // hold step + key / PROG / ENV / LFO
    ab.stepDown(9); tick();
    ab.keyDown(13); tick(); ab.keyUp(13); tick();  // F#? key 13 from F
    press(B_PROG_UP);
    press(B_ENV_SEL);
    press(B_LFO_SEL);
    ab.stepUp(9); tick();
    const Step& s9 = ab.line(p, 0).steps[9];
    CHECK(s9.gate && s9.pitch == 41 - 12 + 13 && s9.oct == 1, "hold step + key = note %d, + PROG+ = octave up", s9.pitch);
    CHECK(s9.accent != 0 || s9.slide != 0, "hold step + ENV / LFO = accent / slide");
    // step entry
    func(AF_LEN); stepTap(7);
    CHECK(ab.line(p, 0).length == 8, "FUNC+11 + step 8 = length 8");
    press(B_REC);
    CHECK(ab.recMode() && !ab.playing(), "REC while stopped = step entry");
    key(7); key(10); press(B_PROG_UP); key(14);
    CHECK(ab.line(p, 0).steps[0].pitch == 36 && ab.line(p, 0).steps[1].pitch == 39 && !ab.line(p, 0).steps[2].gate &&
              ab.line(p, 0).steps[3].pitch == 43 && ab.recPos() == 4,
          "step entry: C, D#, rest, G -> cursor at 5");
    ab.keyDown(5); tick(); ab.keyDown(7); tick(); ab.keyUp(5); ab.keyUp(7); tick();  // legato
    CHECK(ab.line(p, 0).steps[4].slide, "legato in step entry writes a slide");
    press(B_REC);
    // transpose
    press(B_LFO_SEL);
    key(12);
    CHECK(ab.transpose() == 5 && ab.led(LED_LFO2), "transpose mode: key F = +5");
    key(7);
    CHECK(ab.transpose() == 0, "first C = no transpose");
    press(B_LFO_SEL);
    // drums lane
    press(B_ENV_SEL); press(B_ENV_SEL);
    CHECK(ab.lane() == AcidBox::LANE_DRUMS, "drum lane");
    uint16_t k0 = ab.drums(p).hits[DI_KICK];
    stepTap(2);
    CHECK(ab.drums(p).hits[DI_KICK] == (k0 ^ 4), "drum lane: tap toggles kick on step 3");
    press(B_OSC_SEL);  // clap
    ab.pot(K_R2, 0.9f); tick();
    CHECK(ab.project().drum[DI_CLAP][DP_DECAY] == 114 && ab.project().drum[DI_KICK][DP_DECAY] == 58, "drum knobs edit only the drum picked with OSC");
    ab.buttonDown(B_FUNC); tick(); press(B_OSC_SEL); ab.buttonUp(B_FUNC); tick();
    CHECK(ab.project().drum[DI_CLAP][DP_MODEL] == DM_SNARE, "FUNC + OSC: clap -> snare");
    press(B_OSC_SEL);  // closed hat
    auto early = [&](float atk) {
      ab.pot(K_R1, atk); tick();
      run(int(SR * 0.4f));
      Stats st;
      ab.keyDown(2); ab.keyUp(2);
      run(192, nullptr, &st);  // the first 4 ms
      return st.peak;
    };
    float sharp = early(0.f), soft = early(1.f);
    CHECK(soft < sharp * 0.3f, "hat attack 100 ms softens the hit (%.3f vs %.3f in the first 4 ms)", soft, sharp);
    ab.pot(K_R1, 0.f); tick();
    press(B_OSC_SEL); press(B_OSC_SEL);  // back to the first layer
    press(B_ENV_SEL);
  }

  std::printf("[4] audio: all starter patterns, evolve, mute\n");
  {
    std::vector<float> wav;
    Stats s;
    for (int pt = 0; pt < 8; pt++) {
      // fresh project so the starter patterns are untouched
      if (pt == 0) { ab.init(SR); run(int(SR * 1.3f)); press(B_PLAY); }
      else press(B_PROG_UP);
      run(int(SR * 60.f / 126.f * 4.f * 2.f), &wav, &s);  // two bars each (switch is queued to the bar)
    }
    double rms = std::sqrt(s.sum2 / s.n);
    CHECK(!s.nan, "no NaN/Inf");
    CHECK(s.peak > 0.3f && s.peak <= 1.0f, "peak %.3f", s.peak);
    CHECK(rms > 0.05 && rms < 0.5, "rms %.3f", rms);
    writeWav(out + "/acid_demo.wav", wav);
    int p = ab.currentPattern();
    Line before = ab.line(p, 0);
    func(AF_EVOLVE);  // every 2 bars
    run(int(SR * 60.f / 126.f * 4.f * 4.2f));
    CHECK(std::memcmp(&before, &ab.line(p, 0), sizeof(Line)) != 0, "EVOLVE mutates the line while playing");
    func(AF_EVOLVE); func(AF_EVOLVE); func(AF_EVOLVE);
    CHECK(ab.project().evolve == 0, "EVOLVE cycles back to off");
    func(AF_MUTE); stepTap(0); press(B_FUNC);
    CHECK(ab.muted(0), "mute line A");
    func(AF_MUTE); stepTap(0); press(B_FUNC);
    press(B_PLAY);
  }

  std::printf("[5] save / load\n");
  {
    func(AF_SAVE);
    CHECK(ab.takeSaveRequest(), "save requested");
    std::vector<uint8_t> blob(sizeof(Project));
    std::memcpy(blob.data(), &ab.project(), blob.size());
    static AcidBox b2;
    b2.init(SR);
    CHECK(b2.loadProject(blob.data(), blob.size()), "load ok (%zu bytes)", blob.size());
    CHECK(!std::memcmp(&b2.project(), &ab.project(), blob.size()), "identical");
  }

  std::printf("[6] OLED\n");
  {
    press(B_PLAY);
    run(int(SR * 1.5f));
    printOled();
    ab.buttonDown(B_FUNC); tick();
    printOled();
    ab.buttonUp(B_FUNC); tick();
    press(B_PLAY);
  }

  std::printf("[7] CPU benchmark (60 s, P04: two lines + drums)\n");
  {
    static AcidBox bb;
    bb.init(SR);
    float L[256], R[256];
    for (int i = 0; i < 3; i++) { bb.buttonDown(B_PROG_UP); bb.buttonUp(B_PROG_UP); bb.process(L, R, 16); }
    bb.buttonDown(B_PLAY); bb.buttonUp(B_PLAY);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < int(SR * 60) / 256; i++) bb.process(L, R, 256);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  60 s rendered in %.3f s -> %.0fx realtime (%.1f ns per stereo sample)\n", sec, 60.0 / sec, sec / (SR * 60) * 1e9);
  }

  std::printf("\n%s\n", failures ? "FAILURES" : "ALL TESTS PASSED");
  if (failures) std::printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
