// Headless runner: self-tests, demo WAV render, OLED dump, CPU benchmark.
// Usage: render [out_dir]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../engine/groovebox.h"

using namespace pvg;

static Groovebox gb;
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
    gb.process(L, R, n);
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
static void tick() { run(16); }  // lets queued events reach the engine

static void stepTap(int s) { gb.stepDown(s); tick(); gb.stepUp(s); tick(); }
static void press(int b) { gb.buttonDown(b); tick(); gb.buttonUp(b); tick(); }
static void func(int s) { gb.buttonDown(B_FUNC); tick(); stepTap(s); gb.buttonUp(B_FUNC); tick(); }

static void writeWav(const std::string& path, const std::vector<float>& s) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::printf("cannot write %s\n", path.c_str()); return; }
  uint32_t dataBytes = uint32_t(s.size() * 2), sr = 48000, byteRate = sr * 4;
  uint16_t ch = 2, bits = 16, align = 4, fmt = 1;
  uint32_t riff = 36 + dataBytes, fmtLen = 16;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&sr, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
  for (float x : s) { int16_t v = int16_t(std::lrint(clampf(x, -1.f, 1.f) * 32767.f)); std::fwrite(&v, 2, 1, f); }
  std::fclose(f);
}

static void dumpOled(const std::string& pgm, bool ascii) {
  Display d;
  gb.drawDisplay(d);
  if (ascii) {
    for (int y = 0; y < Display::H; y++) {
      std::string line;
      for (int x = 0; x < Display::W; x++) line += d.get(x, y) ? '#' : '.';
      std::printf("    %s\n", line.c_str());
    }
  }
  FILE* f = std::fopen(pgm.c_str(), "wb");
  if (!f) return;
  const int S = 4;
  std::fprintf(f, "P5\n%d %d\n255\n", Display::W * S, Display::H * S);
  for (int y = 0; y < Display::H * S; y++)
    for (int x = 0; x < Display::W * S; x++) std::fputc(d.get(x / S, y / S) ? 230 : 10, f);
  std::fclose(f);
}

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : "out";
  gb.init(SR);

  std::printf("[1] idle is silent\n");
  { Stats s; run(int(SR), nullptr, &s); CHECK(s.peak < 1e-4f && !s.nan, "idle peak %.6f", s.peak); }

  std::printf("[2] demo pattern renders cleanly (8 bars @120)\n");
  std::vector<float> wav;
  {
    press(B_PLAY);
    CHECK(gb.playing(), "transport running");
    Stats s;
    run(int(SR * 16), &wav, &s);
    double rms = std::sqrt(s.sum2 / s.n);
    CHECK(!s.nan, "no NaN/Inf");
    CHECK(s.peak > 0.2f && s.peak <= 1.0f, "peak %.3f in (0.2, 1.0]", s.peak);
    CHECK(rms > 0.03 && rms < 0.5, "rms %.3f", rms);
  }

  std::printf("[3] step buttons / p-lock / step attributes on HAT\n");
  {
    press(B_PLAY);  // stop
    func(2);
    CHECK(gb.currentTrack() == TT_HAT, "FUNC+3 selects hat");
    int p = gb.currentPattern();
    bool before = gb.step(p, TT_HAT, 1).on;
    stepTap(1);
    CHECK(gb.step(p, TT_HAT, 1).on != before, "tap toggles step 2 (now %d)", gb.step(p, TT_HAT, 1).on);
    stepTap(1);
    CHECK(gb.step(p, TT_HAT, 1).on == before, "tap again toggles back");
    CHECK(!(gb.stepLeds() & 2) && (gb.stepLeds() & 1), "step LEDs mirror the track");
    gb.stepDown(2); tick();
    gb.pot(K_R1, 0.9f); tick();
    press(B_LFO_SEL); press(B_LFO_SEL);   // ratchet 1->2->3
    press(B_PROG_DN);                     // prob 7/8
    press(B_ENV_SEL);                     // accent toggle (was on)
    gb.stepUp(2); tick();
    const Step& st = gb.step(p, TT_HAT, 2);
    CHECK(st.on && (st.lockMask & (1u << D_TUNE)) && st.lock[D_TUNE] == 114, "hold+knob -> lock TUNE=%d, step kept", st.lock[D_TUNE]);
    CHECK(st.ratchet == 3 && st.prob == 7 && !st.accent, "ratchet 3, prob 7/8, accent off");
    gb.stepDown(4); tick(); gb.keyDown(9); tick(); gb.keyUp(9); gb.stepUp(4); tick();
    CHECK(gb.step(p, TT_HAT, 4).lock[D_TUNE] == gb.project().patterns[p].tracks[TT_HAT].p[D_TUNE] + 8, "hold step + key = pitch lock");
  }

  std::printf("[4] synth: hold step + keys = notes/chords, live record\n");
  {
    func(4);
    gb.buttonDown(B_FUNC); tick(); press(B_REC); gb.buttonUp(B_FUNC); tick();  // clear track
    int p = gb.currentPattern();
    CHECK(!gb.step(p, kSynthTrack, 0).on, "FUNC+RECORD cleared synth track");
    gb.stepDown(0); tick(); gb.keyDown(4); tick(); gb.keyUp(4); gb.stepUp(0); tick();
    gb.stepDown(1); tick(); gb.keyDown(7); tick(); gb.keyDown(11); tick(); gb.keyDown(14); tick();
    gb.keyUp(7); gb.keyUp(11); gb.keyUp(14); gb.stepUp(1); tick();
    gb.stepDown(3); tick(); gb.buttonDown(B_FUNC); tick(); press(B_PROG_UP); gb.buttonUp(B_FUNC); gb.stepUp(3); tick();
    const Step& a = gb.step(p, kSynthTrack, 0);
    const Step& c = gb.step(p, kSynthTrack, 1);
    CHECK(a.on && a.notes[0] == 45, "step1 = A2 (%d)", a.notes[0]);
    CHECK(c.on && c.notes[0] == 48 && c.notes[1] == 52 && c.notes[2] == 55, "step2 chord C3-E3-G3");
    CHECK(gb.step(p, kSynthTrack, 3).gate == 2, "hold step + FUNC + '+' = gate 2");
    press(B_REC);
    press(B_PLAY);
    run(int(SR * 0.5f));
    gb.keyDown(11); tick(); gb.keyUp(11); tick();
    int hits = 0;
    for (int s = 0; s < 16; s++) hits += gb.step(p, kSynthTrack, s).on && gb.step(p, kSynthTrack, s).notes[0] == 52;
    CHECK(hits == 1, "live-recorded E3 into one step");
    press(B_REC);
    press(B_PLAY);
  }

  std::printf("[4b] acid: notes, slide, accent, sound\n");
  {
    func(5);
    CHECK(gb.currentTrack() == kAcidTrack, "FUNC+6 selects acid");
    gb.buttonDown(B_FUNC); tick(); press(B_REC); gb.buttonUp(B_FUNC); tick();  // clear the demo line
    int p = gb.currentPattern();
    gb.stepDown(1); tick(); gb.keyDown(4); tick(); gb.keyDown(9); tick(); gb.keyUp(4); gb.keyUp(9); tick();
    press(B_LFO_SEL); press(B_ENV_SEL);
    gb.stepUp(1); tick();
    const Step& a = gb.step(p, kAcidTrack, 1);
    CHECK(a.on && a.notes[0] == 50 && a.notes[1] == kNoNote, "acid is mono: last key wins (%d)", a.notes[0]);
    CHECK(a.slide && a.accent && a.ratchet == 1, "hold step + LFO = slide, + ENV = accent");
    gb.stepDown(1); tick(); gb.buttonDown(B_FUNC); tick(); press(B_LFO_SEL); gb.buttonUp(B_FUNC); gb.stepUp(1); tick();
    CHECK(gb.step(p, kAcidTrack, 1).ratchet == 2 && gb.step(p, kAcidTrack, 1).slide, "hold step + FUNC + LFO = ratchet");
    // the voice itself
    Acid v; v.init(SR); AcidParams ap; float buf[16]; float pk = 0;
    v.control(ap, 0);
    bool l1 = v.noteOn(33, 1.f, true, 6000);
    for (int i = 0; i < 300; i++) { v.control(ap, 16); v.render(buf, 16); for (float x : buf) pk = std::max(pk, std::fabs(x)); }
    bool l2 = v.noteOn(45, 1.f, false, 6000);  // still gated: glides
    CHECK(!l1 && l2, "note during an open gate slides (legato), first note retriggers");
    for (int i = 0; i < 400; i++) { v.control(ap, 16); v.render(buf, 16); }
    bool l3 = v.noteOn(40, 1.f, false, 3000);  // gate closed by now
    CHECK(!l3 && pk > 0.1f && pk <= 1.2f, "retrigger after gate end, peak %.2f", pk);
  }

  std::printf("[5] pattern queue switches on the bar\n");
  {
    press(B_PLAY);
    run(int(SR * 0.6f));
    func(13); stepTap(1);
    CHECK(gb.queuedPattern() == 1 && gb.currentPattern() == 0, "FUNC+14 then step 2 queues P2");
    run(int(SR * 2.0f));
    CHECK(gb.currentPattern() == 1 && gb.queuedPattern() < 0, "switched at bar end");
    press(B_PROG_UP);
    CHECK(gb.queuedPattern() == 2, "PROG+ queues next pattern");
    press(B_PLAY);
    CHECK(gb.currentPattern() == 2, "stop applies the queued pattern");
    press(B_PROG_DN);
  }

  std::printf("[6] mute mode / euclid / length\n");
  {
    func(6); stepTap(0);
    CHECK(gb.muted(TT_KICK), "mute mode + step1 mutes kick");
    CHECK((gb.stepLeds() & 0x3F) == 0x3E, "LEDs show unmuted tracks");
    stepTap(0); press(B_FUNC);  // unmute, tap FUNC exits the mode
    CHECK(!gb.muted(TT_KICK) && (gb.stepLeds() & 0x3F) != 0x3E, "unmuted, mode exited");
    func(0);
    func(10); stepTap(11);
    int p = gb.currentPattern();
    CHECK(gb.project().patterns[p].tracks[TT_KICK].length == 12, "FUNC+11, step12 -> length 12");
    func(11); stepTap(4);
    int hits = 0;
    for (int s = 0; s < 12; s++) hits += gb.step(p, TT_KICK, s).on;
    CHECK(hits == 5, "FUNC+12, step5 -> euclid 5/12 (%d hits)", hits);
  }

  std::printf("[7] save / load round trip\n");
  {
    func(15);
    CHECK(gb.takeSaveRequest(), "save requested");
    std::vector<uint8_t> blob(sizeof(Project));
    std::memcpy(blob.data(), &gb.project(), blob.size());
    static Groovebox gb2;
    gb2.init(SR);
    CHECK(gb2.loadProject(blob.data(), blob.size()), "load ok (%zu bytes)", blob.size());
    CHECK(std::memcmp(&gb2.project(), &gb.project(), blob.size()) == 0, "identical");
    uint8_t junk[16] = {};
    CHECK(!gb2.loadProject(junk, sizeof junk), "rejects junk");
  }

  std::printf("[8] OLED 128x32\n");
  {
    func(4);
    press(B_PLAY);
    run(int(SR * 3.1f));
    dumpOled(out + "/oled_home.pgm", true);
    gb.pot(K_CUTOFF, 0.7f);
    run(int(SR * 0.1f));
    dumpOled(out + "/oled_param.pgm", true);
    gb.buttonDown(B_FUNC); run(int(SR * 1.5f));
    dumpOled(out + "/oled_func.pgm", false);
    gb.buttonUp(B_FUNC); tick();
    Display d; gb.drawDisplay(d);
    int lit = 0;
    for (int y = 0; y < Display::H; y++) for (int x = 0; x < Display::W; x++) lit += d.get(x, y);
    CHECK(lit > 100, "home screen draws (%d px lit)", lit);
    press(B_PLAY);
  }

  std::printf("[10] clock: internal out, MIDI in, SYNC in\n");
  {
    static Groovebox c;
    c.init(SR);
    float L[256], R[256];
    auto runc = [&](int frames) { while (frames > 0) { int n = frames < 256 ? frames : 256; c.process(L, R, n); frames -= n; } };
    auto drainOut = [&](std::vector<OutEvent>& v) { OutEvent e; while (c.popOutput(e)) v.push_back(e); };
    std::vector<OutEvent> ev;
    // internal @120: 48 ticks/s, 1000 samples apart, starts with FA
    c.buttonDown(B_PLAY); c.buttonUp(B_PLAY);
    for (int i = 0; i < 2 * 48000 / 256; i++) { c.process(L, R, 256); drainOut(ev); }
    int f8 = 0, fa = 0, sync = 0; double maxErr = 0; uint64_t prev = 0;
    for (auto& e : ev) {
      if (e.type == OUT_SYNC) { sync++; continue; }
      if (e.b[0] == 0xFA) fa++;
      if (e.b[0] == 0xF8) { if (f8) maxErr = std::max(maxErr, std::fabs(double(e.time - prev) - 1000.0)); prev = e.time; f8++; }
    }
    CHECK(fa == 1 && ev[0].b[0] == 0xFA, "clock out starts with 0xFA");
    CHECK(f8 >= 95 && f8 <= 97, "%d clock ticks in 2 s @120 (96 expected)", f8);
    CHECK(maxErr <= 1.0, "tick spacing 1000 +- %.1f samples", maxErr);
    CHECK(sync >= 7 && sync <= 9, "%d sync pulses at 2 PPQN (8 expected)", sync);
    c.buttonDown(B_PLAY); c.buttonUp(B_PLAY); runc(256); ev.clear(); drainOut(ev);
    CHECK(!ev.empty() && ev.back().b[0] == 0xFC, "stop sends 0xFC");

    // switch to MIDI clock via SETUP (FUNC + MEMORY WRITE, step 2)
    c.buttonDown(B_FUNC); c.buttonDown(B_MEMWRITE); c.buttonUp(B_MEMWRITE); c.buttonUp(B_FUNC);
    c.stepDown(1); c.stepUp(1); c.buttonDown(B_MEMWRITE); c.buttonUp(B_MEMWRITE); runc(64);
    CHECK(c.clockSource() == CLK_MIDI, "SETUP step 2 -> MIDI clock");
    c.midi(0xFA, 0, 0); runc(64);
    CHECK(c.waitingForClock(), "0xFA arms, waits for first tick");
    int samplesPerTick = 48000 * 60 / (132 * 24);  // 132 BPM from outside
    for (int t = 0; t < 24 * 8; t++) { c.midi(0xF8, 0, 0); runc(samplesPerTick); }  // 8 beats = 32 steps
    CHECK(!c.waitingForClock() && c.playing(), "running on MIDI clock");
    CHECK(std::fabs(c.tempo() - 132.f) < 1.5f, "measured tempo %.1f (132)", c.tempo());
    CHECK(c.stepPos(TT_KICK) == 15, "kick at step %d after 32 steps (15 = last of 2nd bar)", c.stepPos(TT_KICK) + 1);
    runc(48000);  // clock pauses: sequencer must hold
    CHECK(c.stepPos(TT_KICK) == 15, "holds while clock pauses");
    c.midi(0xFC, 0, 0); runc(64);
    CHECK(!c.playing(), "0xFC stops");
    { std::vector<OutEvent> echo; drainOut(echo); int n = 0; for (auto& e : echo) n += e.type == OUT_MIDI; CHECK(n == 0, "no MIDI clock echo when slaved to MIDI (%d)", n); }
    c.midi(0xF2, 8, 0); c.midi(0xFB, 0, 0); c.midi(0xF8, 0, 0); runc(64);
    CHECK(c.stepPos(TT_KICK) == 8, "song position 8 + continue -> step %d", c.stepPos(TT_KICK) + 1);
    c.midi(0xFC, 0, 0); runc(64);

    // SYNC in at 2 PPQN, 100 BPM: one pulse per 8th, 16ths interpolated
    c.buttonDown(B_FUNC); c.buttonDown(B_MEMWRITE); c.buttonUp(B_MEMWRITE); c.buttonUp(B_FUNC);
    c.stepDown(2); c.stepUp(2); c.buttonDown(B_MEMWRITE); c.buttonUp(B_MEMWRITE); runc(64);
    CHECK(c.clockSource() == CLK_SYNC, "SETUP step 3 -> SYNC in");
    c.buttonDown(B_PLAY); c.buttonUp(B_PLAY); runc(64);
    int pulse = 48000 * 60 / (100 * 2);
    drainOut(ev); ev.clear();
    for (int i = 0; i < 16; i++) { c.syncPulse(); for (int k = 0; k < pulse; k += 256) { c.process(L, R, std::min(256, pulse - k)); } }
    drainOut(ev);
    int midiTicks = 0; for (auto& e : ev) midiTicks += e.type == OUT_MIDI && e.b[0] == 0xF8;
    CHECK(c.stepPos(TT_KICK) == 15, "16 pulses = 32 steps (at step %d)", c.stepPos(TT_KICK) + 1);
    CHECK(std::fabs(c.tempo() - 100.f) < 1.5f, "sync tempo %.1f (100)", c.tempo());
    CHECK(midiTicks >= 190 && midiTicks <= 192, "sync->MIDI converter: %d ticks (192)", midiTicks);
  }

  std::printf("[9] CPU benchmark (60 s audio, demo pattern, all voices)\n");
  {
    static Groovebox gb4;
    gb4.init(SR);
    gb4.buttonDown(B_PLAY);
    gb4.pot(K_LFO_AMT, 0.5f);
    float L[64], R[64];
    auto t0 = std::chrono::steady_clock::now();
    int blocks = int(SR * 60) / 64;
    for (int i = 0; i < blocks; i++) {
      if (i % 3000 == 0) for (int n = 0; n < 5; n++) gb4.midi(0x90, uint8_t(48 + n * 4), 100);
      gb4.process(L, R, 64);
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  60 s rendered in %.3f s -> %.0fx realtime on this Mac\n", sec, 60.0 / sec);
    std::printf("  (%.1f ns per stereo sample)\n", sec * 1e9 / (SR * 60));
  }

  writeWav(out + "/demo.wav", wav);
  std::printf("\nwrote %s/demo.wav (16 s), %s/oled_*.pgm\n", out.c_str(), out.c_str());
  std::printf(failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", failures);
  return failures ? 1 : 0;
}
