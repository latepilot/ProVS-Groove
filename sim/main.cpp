// ProVS Groove simulator. Panel layout traced from a photo of the real Pro VS
// Mini (landscape, keys in front). The OLED is drawn 1 screen pixel per OLED
// pixel: on a Retina MacBook that is roughly its real 22 mm width. TAB opens
// a sim-only magnifier.
//
//   bin/provs-groove-sim                    interactive
//   bin/provs-groove-sim --shot out.bmp     headless screenshot (demo playing)
//   bin/provs-groove-sim --video out/demo   scripted demo video -> out/demo.mp4 (needs ffmpeg)
#include <CoreMIDI/CoreMIDI.h>
#include <SDL.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef PVA_ACID
#include "../engine/acidbox.h"
using Engine = pvg::AcidBox;
#define FW_FUNC_NAME acidFuncName
static const char* kFwTitle = "ProVS Acid - simulator";
static const char* kFwSub = "+ ACID fw (sim)";
static const char* kStateFile = "/../state/acid.pva";
#else
#include "../engine/groovebox.h"
using Engine = pvg::Groovebox;
#define FW_FUNC_NAME funcName
static const char* kFwTitle = "ProVS Groove - simulator";
static const char* kFwSub = "+ GROOVE fw (sim)";
static const char* kStateFile = "/../state/project.pvg";
#endif

using namespace pvg;

static Engine g_gb;
static SDL_AudioDeviceID g_dev = 0;
static const int kW = 1200, kH = 740;
static SDL_Renderer* R = nullptr;
static std::mutex g_ctl;  // engine input queue is single-producer: main thread + CoreMIDI thread share it

// ---------------------------------------------------------------- CoreMIDI
// In: every MIDI source on the Mac + a virtual destination "ProVS Groove In".
// Out: a virtual source "ProVS Groove" (clock / start / stop), scheduled with
// host timestamps so the clock does not inherit audio-buffer jitter.
static MIDIClientRef g_midi = 0;
static MIDIPortRef g_inPort = 0;
static MIDIEndpointRef g_outSrc = 0, g_virtIn = 0;
static std::vector<MIDIEndpointRef> g_connected;
static std::vector<std::string> g_srcNames;
static std::atomic<bool> g_midiChanged{false};
static std::atomic<uint32_t> g_midiInFlash{0}, g_syncFlash{0};
static double g_hostTicksPerSec = 1e9;
static float g_sr = 48000.f;

struct MidiParser { uint8_t status = 0, data[2] = {0, 0}; int cnt = 0; bool sysex = false; };
static MidiParser g_parsers[64];

static void parseBytes(MidiParser& p, const Byte* d, int n) {
  for (int i = 0; i < n; i++) {
    uint8_t b = d[i];
    if (b >= 0xF8) { g_gb.midi(b, 0, 0); continue; }  // realtime, may interleave
    if (b == 0xF0) { p.sysex = true; continue; }
    if (b == 0xF7) { p.sysex = false; continue; }
    if (p.sysex) continue;
    if (b & 0x80) { p.status = b; p.cnt = 0; continue; }
    if (!p.status) continue;
    p.data[p.cnt++] = b;
    uint8_t hi = p.status & 0xF0;
    int need = (hi == 0xC0 || hi == 0xD0 || p.status == 0xF1 || p.status == 0xF3) ? 1 : 2;
    if (p.cnt >= need) {
      g_gb.midi(p.status, p.data[0], need > 1 ? p.data[1] : 0);
      p.cnt = 0;  // running status
    }
  }
}

static void midiRead(const MIDIPacketList* pl, void*, void* srcRef) {
  int idx = int(reinterpret_cast<intptr_t>(srcRef)) & 63;
  std::lock_guard<std::mutex> lk(g_ctl);
  const MIDIPacket* pk = &pl->packet[0];
  for (UInt32 i = 0; i < pl->numPackets; i++) {
    parseBytes(g_parsers[idx], pk->data, pk->length);
    pk = MIDIPacketNext(pk);
  }
  g_midiInFlash = SDL_GetTicks();
}

static void midiNotify(const MIDINotification* n, void*) {
  if (n->messageID == kMIDIMsgSetupChanged) g_midiChanged = true;
}

static std::string endpointName(MIDIEndpointRef e) {
  CFStringRef cf = nullptr;
  MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &cf);
  char buf[128] = "?";
  if (cf) { CFStringGetCString(cf, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(cf); }
  return buf;
}

static void midiConnectAll() {
  for (auto e : g_connected) MIDIPortDisconnectSource(g_inPort, e);
  g_connected.clear();
  std::vector<std::string> names;
  ItemCount n = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < n && i < 62; i++) {
    MIDIEndpointRef e = MIDIGetSource(i);
    if (e == g_outSrc) continue;  // never listen to ourselves
    MIDIPortConnectSource(g_inPort, e, reinterpret_cast<void*>(intptr_t(i + 1)));
    g_connected.push_back(e);
    names.push_back(endpointName(e));
  }
  std::lock_guard<std::mutex> lk(g_ctl);
  g_srcNames = names;
}

static bool midiInit() {
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  g_hostTicksPerSec = 1e9 * double(tb.denom) / double(tb.numer);
  if (MIDIClientCreate(CFSTR("ProVS Groove"), midiNotify, nullptr, &g_midi) != noErr) return false;
  MIDIInputPortCreate(g_midi, CFSTR("in"), midiRead, nullptr, &g_inPort);
  MIDISourceCreate(g_midi, CFSTR("ProVS Groove"), &g_outSrc);
  MIDIDestinationCreate(g_midi, CFSTR("ProVS Groove In"), midiRead, reinterpret_cast<void*>(intptr_t(63)), &g_virtIn);
  midiConnectAll();
  return true;
}

// called from the audio thread right after process(); blockStart = engine clock before it
static void midiSendOutput(uint64_t blockStart, int frames) {
  OutEvent e;
  uint64_t hostNow = mach_absolute_time();
  double latency = double(frames) / g_sr;  // the block we just rendered plays ~one buffer later
  while (g_gb.popOutput(e)) {
    if (e.type == OUT_SYNC) { g_syncFlash = SDL_GetTicks(); continue; }
    if (!g_outSrc) continue;
    double dt = (double(e.time) - double(blockStart)) / g_sr + latency;
    if (dt < 0) dt = 0;
    Byte buf[64];
    MIDIPacketList* pl = reinterpret_cast<MIDIPacketList*>(buf);
    MIDIPacket* pk = MIDIPacketListInit(pl);
    MIDIPacketListAdd(pl, sizeof buf, pk, hostNow + MIDITimeStamp(dt * g_hostTicksPerSec), e.len, e.b);
    MIDIReceived(g_outSrc, pl);
  }
}

// ---------------------------------------------------------------- drawing
static void color(Uint32 rgb) { SDL_SetRenderDrawColor(R, rgb >> 16, (rgb >> 8) & 255, rgb & 255, 255); }
static void fillRect(int x, int y, int w, int h) { SDL_Rect r{x, y, w, h}; SDL_RenderFillRect(R, &r); }
static void drawRect(int x, int y, int w, int h) { SDL_Rect r{x, y, w, h}; SDL_RenderDrawRect(R, &r); }
static int textW(const char* s, int sc) { return int(std::strlen(s)) * 6 * sc - sc; }
static void text(int x, int y, const char* s, int sc = 1) {
  std::vector<SDL_Rect> px;
  for (; *s; s++) {
    int c = (unsigned char)*s;
    if (c < 32 || c > 126) c = '?';
    const uint8_t* g = kFont5x7[c - 32];
    for (int col = 0; col < 5; col++)
      for (int row = 0; row < 8; row++)
        if ((g[col] >> row) & 1) px.push_back({x + col * sc, y + row * sc, sc, sc});
    x += 6 * sc;
  }
  if (!px.empty()) SDL_RenderFillRects(R, px.data(), int(px.size()));
}
static void textC(int cx, int y, const char* s, int sc = 1) { text(cx - textW(s, sc) / 2, y, s, sc); }
static void disc(int cx, int cy, int r) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = int(std::sqrt(float(r * r - dy * dy)));
    SDL_RenderDrawLine(R, cx - dx, cy + dy, cx + dx, cy + dy);
  }
}
static void ring(int cx, int cy, int r) {
  for (int a = 0; a < 360; a += 2) {
    float t = a * kPi / 180.f;
    SDL_RenderDrawPoint(R, cx + int(std::cos(t) * r), cy + int(std::sin(t) * r));
  }
}

// ---------------------------------------------------------------- layout (from the photo)
struct Knob { int pot, x, y, r; const char* label; float v; };
struct Btn { int b, x, y; const char* label; const char* sub; };
static std::vector<Knob> knobs = {
    {K_R1, 541, 216, 22, "R1", 0.3f},       {K_R2, 646, 216, 22, "R2", 0.4f},
    {K_R3, 758, 216, 22, "R3", 0.4f},       {K_R4, 863, 216, 22, "R4", 0.85f},
    {K_LFO_RATE, 541, 307, 22, "RATE", 0.5f}, {K_LFO_AMT, 646, 307, 22, "AMT", 0.4f},
    {K_CUTOFF, 758, 307, 22, "CUTOFF", 0.47f}, {K_RESO, 863, 307, 22, "RESO", 0.4f},
    {K_WAVE, 541, 398, 22, "WAVE", 0.1f},   {K_FREQ, 646, 398, 22, "FREQ", 0.5f},
    {K_CH_RATE, 758, 398, 22, "RATE", 0.4f}, {K_CH_AMT, 863, 398, 22, "AMT", 0.3f},
    {K_TEMPO, 968, 216, 22, "", 0.43f},
};
static std::vector<Btn> btns = {
    {B_PROG_UP, 317, 216, "+", ""},          {B_PROG_DN, 317, 398, "-", ""},
    {B_ENV_SEL, 408, 216, "", ""},           {B_LFO_SEL, 408, 307, "", ""},
    {B_OSC_SEL, 408, 398, "", ""},           {B_MEMWRITE, 968, 307, "MEMORY", "WRITE"},
    {B_PLAY, 968, 398, "PLAY", "ACTIVE"},    {B_FUNC, 1059, 307, "FUNC", ""},
    {B_REC, 1059, 398, "RECORD", "CLEAR"},
};
static const int kBtnHalf = 24;
struct SelLed { int led, y; const char* label; };
#ifdef PVA_ACID  // sim-only stickers: what the selector LEDs mean in this firmware
static const SelLed kSelLeds[] = {{LED_ENV_FIL, 198, "A"},   {LED_ENV_AMP, 218, "B"},   {LED_ENV_MIX, 238, "DR"},
                                  {LED_LFO1, 296, "PLY"},    {LED_LFO2, 316, "TRS"},
                                  {LED_OSC_A, 368, "GAT"},   {LED_OSC_B, 386, "ACC"},   {LED_OSC_C, 404, "SLD"}, {LED_OSC_D, 422, "OCT"}};
#else
static const SelLed kSelLeds[] = {{LED_ENV_FIL, 198, "FIL"}, {LED_ENV_AMP, 218, "AMP"}, {LED_ENV_MIX, 238, "MIX"},
                                  {LED_LFO1, 296, "1"},      {LED_LFO2, 316, "2"},
                                  {LED_OSC_A, 368, "A"},     {LED_OSC_B, 386, "B"},   {LED_OSC_C, 404, "C"}, {LED_OSC_D, 422, "D"}};
#endif
static const int kJoyCX = 150, kJoyCY = 300, kJoyR = 72;
static const int kOledX = 303, kOledY = 86;
static const int kKbX = 80, kKbY = 520, kKbH = 92;
static const float kKeyW = 63.8f;
static const int kStepY = 622, kStepW = 54, kStepH = 62;

static int whiteIndexOf(int k) { int w = 0; for (int i = 0; i < k; i++) if (!keyIsBlack(i)) w++; return w; }
static int keyX(int k) {  // centre x
  int w = whiteIndexOf(k);
  return keyIsBlack(k) ? int(kKbX + w * kKeyW) : int(kKbX + w * kKeyW + kKeyW / 2);
}
static int stepX(int i) { return int(kKbX + i * kKeyW + kKeyW / 2); }

// ---------------------------------------------------------------- input state
static bool stepDownS[16] = {}, stepLatched[16] = {};
static bool keyDownS[kNumKeys] = {}, keyLatched[kNumKeys] = {};
static bool btnDownS[B_COUNT] = {}, btnLatched[B_COUNT] = {};
static int mouseStep = -1, mouseKey = -1, mouseBtn = -1, dragKnob = -1;
static bool dragJoy = false, zoom = false;
static float joyX = 0, joyY = 0;

static void setStep(int s, bool d) { if (s < 0 || s > 15 || stepDownS[s] == d) return; stepDownS[s] = d; if (d) g_gb.stepDown(s); else g_gb.stepUp(s); }
static void setKey(int k, bool d) { if (k < 0 || k >= kNumKeys || keyDownS[k] == d) return; keyDownS[k] = d; if (d) g_gb.keyDown(k); else g_gb.keyUp(k); }
static void setBtn(int b, bool d) { if (b < 0 || b >= B_COUNT || btnDownS[b] == d) return; btnDownS[b] = d; if (d) g_gb.buttonDown(b); else g_gb.buttonUp(b); }

static int hitStep(int mx, int my) {
  for (int i = 0; i < 16; i++)
    if (std::abs(mx - stepX(i)) <= kStepW / 2 && my >= kStepY && my < kStepY + kStepH) return i;
  return -1;
}
static int hitKey(int mx, int my) {
  if (my < kKbY || my >= kKbY + kKbH) return -1;
  for (int k = 0; k < kNumKeys; k++)
    if (keyIsBlack(k) && std::abs(mx - keyX(k)) <= 18 && my >= kKbY + 14 && my < kKbY + kKbH - 14) return k;
  for (int k = 0; k < kNumKeys; k++)
    if (!keyIsBlack(k) && std::abs(mx - keyX(k)) <= int(kKeyW / 2)) return k;
  return -1;
}
static int hitBtn(int mx, int my) {
  for (auto& b : btns) if (std::abs(mx - b.x) <= kBtnHalf && std::abs(my - b.y) <= kBtnHalf) return b.b;
  return -1;
}
static int hitKnob(int mx, int my) {
  for (size_t i = 0; i < knobs.size(); i++) {
    int dx = mx - knobs[i].x, dy = my - knobs[i].y;
    if (dx * dx + dy * dy <= 28 * 28) return int(i);
  }
  return -1;
}
static void joyFromMouse(int mx, int my) {
  joyX = clampf((mx - kJoyCX) / 55.f, -1.f, 1.f);
  joyY = clampf(-(my - kJoyCY) / 55.f, -1.f, 1.f);
  g_gb.joystick(joyX, joyY);
}

static int scanToStep(SDL_Scancode s) {
  static const SDL_Scancode m[16] = {SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y,
                                     SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F,
                                     SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K};
  for (int i = 0; i < 16; i++) if (m[i] == s) return i;
  return -1;
}
static int scanToKey(SDL_Scancode s) {  // bottom row = white keys F G A B C D E F G A
  static const SDL_Scancode m[10] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B,
                                     SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH};
  for (int i = 0; i < 10; i++)
    if (m[i] == s) { int w = 0; for (int k = 0; k < kNumKeys; k++) if (!keyIsBlack(k) && w++ == i) return k; }
  return -1;
}
static int scanToBtn(SDL_Scancode s) {
  switch (s) {
    case SDL_SCANCODE_SPACE: return B_PLAY;
    case SDL_SCANCODE_RETURN: return B_REC;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return B_FUNC;
    case SDL_SCANCODE_LEFTBRACKET: return B_PROG_DN;
    case SDL_SCANCODE_RIGHTBRACKET: return B_PROG_UP;
    case SDL_SCANCODE_1: return B_ENV_SEL;
    case SDL_SCANCODE_2: return B_LFO_SEL;
    case SDL_SCANCODE_3: return B_OSC_SEL;
    case SDL_SCANCODE_0: return B_MEMWRITE;
    default: return -1;
  }
}

// ---------------------------------------------------------------- render
static SDL_Texture* oledTex = nullptr;
#ifdef PVA_ACID  // acid theme: lime instead of blue
static const Uint32 kBlue = 0xb4ff1e, kBarText = 0x142000, kOledOn = 0xFFB4FF1E;
#else
static const Uint32 kBlue = 0x2b8fe0, kBarText = 0x0b1a2a, kOledOn = 0xFF45E6FF;
#endif
static const Uint32 kPanel = 0x1c1d20, kLabel = 0xe8e8e8, kDim = 0x8a9099;

static void sectionBar(int x, int y, int w, const char* s) {
  color(kBlue); fillRect(x, y, w, 11);
  color(kBarText); text(x + 4, y + 2, s);
}

static void drawPanel() {
  color(0xd9d6cf); SDL_RenderClear(R);
  color(kPanel); fillRect(20, 20, kW - 40, kH - 40);           // body
  color(0x121214); fillRect(30, kKbY - 6, kW - 60, kKbH + 12);  // key strip
  color(kBlue); fillRect(44, 160, kW - 120, 2);                 // blue line

  // engine snapshot
  Display d;
  SDL_LockAudioDevice(g_dev);
  g_gb.drawDisplay(d);
  uint16_t leds = g_gb.stepLeds();
  bool led[LED_COUNT];
  for (int i = 0; i < LED_COUNT; i++) led[i] = g_gb.led(i);
  SDL_UnlockAudioDevice(g_dev);

  // OLED (1:1) + bezel
  static uint32_t px[Display::W * Display::H];
  for (int y = 0; y < Display::H; y++)
    for (int x = 0; x < Display::W; x++) px[y * Display::W + x] = d.get(x, y) ? kOledOn : 0xFF04080A;
  SDL_UpdateTexture(oledTex, nullptr, px, Display::W * 4);
  color(0x3a3c40); drawRect(kOledX - 10, kOledY - 9, Display::W + 20, Display::H + 18);
  color(0x04080a); fillRect(kOledX - 9, kOledY - 8, Display::W + 18, Display::H + 16);
  SDL_Rect dst{kOledX, kOledY, Display::W, Display::H};
  SDL_RenderCopy(R, oledTex, nullptr, &dst);
  color(kDim); text(kOledX - 8, kOledY + Display::H + 12, "OLED 1:1   TAB = zoom");

  // logo, jacks
  color(kBlue); text(560, 62, "Pro VS", 3); color(0xffffff); text(680, 70, "MINI", 2);
  color(kLabel); text(560, 92, "HYBRID VECTOR SYNTHESIZER");
  color(0x8f8f8f); text(560, 106, kFwSub);
  const struct { int x; const char* n; int r; } jacks[] = {{800, "VOLUME", 13}, {880, "PHONES", 12}, {950, "SYNC", 12}, {1050, "MIDI IN", 26}};
  uint32_t nowMs = SDL_GetTicks();
  for (auto& j : jacks) {
    bool flash = (!std::strcmp(j.n, "SYNC") && nowMs - g_syncFlash < 40) || (!std::strcmp(j.n, "MIDI IN") && nowMs - g_midiInFlash < 40);
    color(0x050505); disc(j.x, 92, j.r);
    color(flash ? 0x40ff70 : 0x6a6a6a); ring(j.x, 92, j.r); ring(j.x, 92, j.r - 1);
    color(kLabel); textC(j.x, 124, j.n);
  }
  {
    std::string list;
    { std::lock_guard<std::mutex> lk(g_ctl); for (auto& n : g_srcNames) { if (!list.empty()) list += ", "; list += n; } }
    if (list.size() > 60) list = list.substr(0, 57) + "...";
    color(kDim);
    text(560, 138, ("MIDI in: " + (list.empty() ? std::string("none (port 'ProVS Groove In')") : list)).c_str());
    text(560, 148, "MIDI out: virtual port 'ProVS Groove'   SETUP = FUNC+WRITE");
  }
  color(kLabel); text(60, 52, "USB");

  // joystick
  color(0x0d0d0e); disc(kJoyCX, kJoyCY, kJoyR);
  color(0x3a3a3c); ring(kJoyCX, kJoyCY, kJoyR);
  int jx = kJoyCX + int(joyX * 32), jy = kJoyCY - int(joyY * 32);
  color(0x3b3b3e); disc(jx, jy, 40); color(0x55555a); ring(jx, jy, 40);
  color(kLabel);
  text(kJoyCX - kJoyR - 22, kJoyCY - 4, "A", 2);
  text(kJoyCX - 5, kJoyCY - kJoyR - 22, "B", 2);
  text(kJoyCX + kJoyR + 10, kJoyCY - 4, "C", 2);
  text(kJoyCX - 5, kJoyCY + kJoyR + 8, "D", 2);
  color(kDim); textC(kJoyCX, kJoyCY + kJoyR + 30, "WAVEFORM MIX");

  // section bars + knobs
#ifdef PVA_ACID
  if (g_gb.lane() == AcidBox::LANE_DRUMS) {  // the knobs follow the drum picked with OSC
    sectionBar(520, 178, 370, "ATTACK  DECAY  SNAP  LEVEL   (drum picked with OSC)");
    sectionBar(520, 269, 150, "PAN");
    sectionBar(735, 269, 155, "-");
    sectionBar(520, 360, 150, "TONE   TUNE");
    sectionBar(735, 360, 155, "DELAY TIME/SEND");
  } else {
    sectionBar(520, 178, 370, "CUTOFF  RESO  ENV MOD  DECAY  (FUNC: DRIVE SUB LEVEL)");
    sectionBar(520, 269, 150, "LFO > CUTOFF");
    sectionBar(735, 269, 155, "ACCENT  SLIDE");
    sectionBar(520, 360, 150, "WAVE   TUNE");
    sectionBar(735, 360, 155, "DELAY TIME/SEND");
  }
#else
  sectionBar(520, 178, 370, "ENV: AMP OSC FIL VCF MIX  (drums: TUNE DECAY TONE LVL)");
  sectionBar(520, 269, 150, "LFO");
  sectionBar(735, 269, 155, "ANALOG FILTER");
  sectionBar(520, 360, 150, "OSC");
  sectionBar(735, 360, 155, "CHORUS");
#endif
  for (auto& k : knobs) {
    color(0x0a0a0b); disc(k.x, k.y, k.r);
    color(0x4a4a4e); ring(k.x, k.y, k.r);
    for (int t = 0; t <= 10; t++) {  // scale ticks
      float a = (-135.f + 27.f * t) * kPi / 180.f;
      color(0x9a9a9a);
      SDL_RenderDrawLine(R, k.x + int(std::sin(a) * (k.r + 4)), k.y - int(std::cos(a) * (k.r + 4)),
                         k.x + int(std::sin(a) * (k.r + 7)), k.y - int(std::cos(a) * (k.r + 7)));
    }
    float a = (-135.f + 270.f * k.v) * kPi / 180.f;
    color(0xf2f2f2);
    for (int w = -1; w <= 1; w++)
      SDL_RenderDrawLine(R, k.x + w, k.y, k.x + w + int(std::sin(a) * (k.r - 4)), k.y - int(std::cos(a) * (k.r - 4)));
    color(kLabel); textC(k.x, k.y + k.r + 10, k.label);
  }
  color(kLabel); textC(968, 182, "TEMPO");
  textC(317, 300, "PROG/OCT"); color(kDim); textC(317, 440, "DATA ENTRY");

  // buttons
  for (auto& b : btns) {
    bool on = btnDownS[b.b];
    bool red = b.b == B_PROG_UP || b.b == B_PROG_DN || (b.b == B_REC && led[LED_REC]);
    bool lit = (b.b == B_PLAY && led[LED_PLAY]) || (b.b == B_FUNC && led[LED_FUNC]);
    color(0x0a0a0b); fillRect(b.x - kBtnHalf - 3, b.y - kBtnHalf - 3, 2 * kBtnHalf + 6, 2 * kBtnHalf + 6);
    color(red ? 0xff3a3a : (lit ? 0xf0f0f0 : 0x8c8c90)); fillRect(b.x - kBtnHalf, b.y - kBtnHalf, 2 * kBtnHalf, 2 * kBtnHalf);
    color(on ? 0x5a5a60 : 0x232326); fillRect(b.x - kBtnHalf + 4, b.y - kBtnHalf + 4, 2 * kBtnHalf - 8, 2 * kBtnHalf - 8);
    if (*b.label) {
      color(kLabel);
      int ly = b.b == B_PROG_UP || b.b == B_PROG_DN ? b.y - 4 : b.y - kBtnHalf - 22;
      if (b.b == B_PROG_UP || b.b == B_PROG_DN) textC(b.x - kBtnHalf - 14, ly, b.label, 2);
      else { textC(b.x, ly, b.label); color(kBlue); textC(b.x, ly + 10, b.sub); }
    }
  }
  // selector LEDs
  for (auto& s : kSelLeds) {
    color(led[s.led] ? 0xff2020 : 0x2a1a1a); disc(486, s.y, 5);
    color(kBlue); text(452, s.y - 4, s.label);
  }

  // key strip
  for (int k = 0; k < kNumKeys; k++) {
    if (keyIsBlack(k)) continue;
    int cx = keyX(k);
    color(keyDownS[k] ? 0x2c4a70 : 0x121214); fillRect(int(cx - kKeyW / 2) + 1, kKbY, int(kKeyW) - 2, kKbH);
    color(0x2e2e32); SDL_RenderDrawLine(R, int(cx + kKeyW / 2), kKbY, int(cx + kKeyW / 2), kKbY + kKbH);
    if (keyLatched[k]) { color(0x3d7bd9); fillRect(int(cx - kKeyW / 2) + 2, kKbY + kKbH - 5, int(kKeyW) - 4, 4); }
  }
  for (int k = 0; k < kNumKeys; k++) {
    char l[2] = {char(k < 26 ? 'A' + k : '_'), 0};
    int cx = keyX(k);
    if (keyIsBlack(k)) {
      color(keyDownS[k] ? 0x9fc6ff : 0xbdbdbd); fillRect(cx - 17, kKbY + 14, 34, kKbH - 28);
      color(0x7a7a7a);
      for (int y = kKbY + 18; y < kKbY + kKbH - 16; y += 6) SDL_RenderDrawLine(R, cx - 14, y, cx + 14, y + 4);
      if (keyLatched[k]) { color(0x3d7bd9); fillRect(cx - 17, kKbY + kKbH - 18, 34, 4); }
      color(0xbdbdbd); textC(cx, kKbY + 2, l);
    } else {
      color(0xbdbdbd); textC(cx, kKbY + 2, l);
    }
  }
  // step buttons with LEDs + groove-firmware FUNC legend (sticker)
  for (int i = 0; i < 16; i++) {
    int cx = stepX(i);
    color(0xe8e8e8); drawRect(cx - kStepW / 2, kStepY, kStepW, kStepH);
    color(stepDownS[i] ? 0x3c3c42 : 0x141416); fillRect(cx - kStepW / 2 + 2, kStepY + 2, kStepW - 4, kStepH - 4);
    bool on = (leds >> i) & 1;
    color(on ? 0xff3030 : 0x351818); disc(cx, kStepY + kStepH - 12, 5);
    char n[4]; std::snprintf(n, sizeof n, "%d", i + 1);
    color(kLabel); textC(cx, kStepY + 18, n, 2);
    color(0xffc75a); textC(cx, kStepY + 6, FW_FUNC_NAME(i));
    if (stepLatched[i]) { color(0x3d7bd9); fillRect(cx - kStepW / 2 + 2, kStepY + kStepH - 4, kStepW - 4, 3); }
  }
  color(0x55565c);
  text(40, kH - 34, "steps Q-I A-K | keys Z../ (white) | space play | enter rec | shift func | [ ] prog | 1 env 2 lfo 3 osc | 0 write | RMB latch | amber = FUNC+step");

  if (zoom) {  // sim-only magnifier
    const int S = 4, zx = 352, zy = 140;
    color(0x000000); fillRect(zx - 12, zy - 12, Display::W * S + 24, Display::H * S + 36);
    SDL_Rect zd{zx, zy, Display::W * S, Display::H * S};
    SDL_RenderCopy(R, oledTex, nullptr, &zd);
    color(kDim); text(zx, zy + Display::H * S + 8, "x4 magnifier (sim only) - the real screen is the small one");
  }
}

// ---------------------------------------------------------------- audio / state
static void audioCb(void*, Uint8* stream, int len) {
  float* out = reinterpret_cast<float*>(stream);
  int frames = len / int(sizeof(float) * 2);
  static float L[4096], Rr[4096];
  int done = 0;
  uint64_t blockStart = g_gb.sampleClock();
  while (done < frames) {
    int n = frames - done > 4096 ? 4096 : frames - done;
    g_gb.process(L, Rr, n);
    for (int i = 0; i < n; i++) { out[(done + i) * 2] = L[i]; out[(done + i) * 2 + 1] = Rr[i]; }
    done += n;
  }
  midiSendOutput(blockStart, frames);
}

static std::string statePath(const char* argv0) {
  std::string p(argv0);
  size_t s = p.find_last_of('/');
  return (s == std::string::npos ? std::string(".") : p.substr(0, s)) + kStateFile;
}
static void loadState(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return;
  std::vector<uint8_t> buf(sizeof(Engine::ProjectType));
  size_t n = std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  if (g_gb.loadProject(buf.data(), n)) std::printf("loaded %s\n", path.c_str());
}
static void saveState(const std::string& path) {
  std::vector<uint8_t> buf(sizeof(Engine::ProjectType));
  SDL_LockAudioDevice(g_dev);
  std::memcpy(buf.data(), &g_gb.project(), buf.size());
  SDL_UnlockAudioDevice(g_dev);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::printf("cannot save %s\n", path.c_str()); return; }
  std::fwrite(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  std::printf("saved %s\n", path.c_str());
}

// ---------------------------------------------------------------- scripted demo video
// --video out/demo   renders out/demo_video.rgb pipe -> ffmpeg (video only) and
// out/demo.wav offline (faster than real time), then muxes out/demo.mp4.
// Frame = 160 px caption strip with a x4 OLED view on top of the 1200x740 panel.
static const int kStripH = 160, kVidH = kH + kStripH, kFps = 30;
static std::string g_cap1, g_cap2;

static int knobIndex(int pot) { for (size_t i = 0; i < knobs.size(); i++) if (knobs[i].pot == pot) return int(i); return -1; }
// make the on-screen knobs match the current track's stored values (no jumps)
#ifdef PVA_ACID
static void syncKnobs() {
  int l = g_gb.lane();
  if (l > 1) {
    const uint8_t* q = g_gb.project().drum[g_gb.layer()];
    const int pots[8] = {K_R1, K_R2, K_R3, K_R4, K_FREQ, K_WAVE, K_LFO_RATE, K_CH_AMT};
    const int dp[8] = {acid::DP_ATTACK, acid::DP_DECAY, acid::DP_SNAP, acid::DP_LEVEL, acid::DP_TUNE, acid::DP_TONE, acid::DP_PAN, acid::DP_SEND};
    for (int i = 0; i < 8; i++) knobs[knobIndex(pots[i])].v = q[dp[i]] / 127.f;
    return;
  }
  const uint8_t* q = g_gb.project().line[l];
  const int pots[11] = {K_R1, K_R2, K_R3, K_R4, K_LFO_RATE, K_LFO_AMT, K_CUTOFF, K_RESO, K_WAVE, K_FREQ, K_CH_AMT};
  const int lp[11] = {acid::LP_CUTOFF, acid::LP_RESO, acid::LP_ENVMOD, acid::LP_DECAY, acid::LP_LFO_RATE, acid::LP_LFO_AMT,
                      acid::LP_ACCENT, acid::LP_SLIDE, acid::LP_WAVE, acid::LP_TUNE, acid::LP_SEND};
  for (int i = 0; i < 11; i++) knobs[knobIndex(pots[i])].v = q[lp[i]] / 127.f;
}
#else
static void syncKnobs() {
  const Project& p = g_gb.project();
  int t = g_gb.currentTrack();
  const Track& tr = p.patterns[p.current].tracks[t];
  if (t == kSynthTrack) return;
  const int pots[9] = {K_R1, K_R2, K_R3, K_R4, K_FREQ, K_WAVE, K_LFO_RATE, K_LFO_AMT, K_CH_AMT};
  const int slots[9] = {0, 1, 2, 3, 4, 5, 6, 7, P_SEND};
  for (int i = 0; i < 9; i++) knobs[knobIndex(pots[i])].v = tr.p[slots[i]] / 127.f;
}
#endif

struct Anim { double t0, t1; int pot; float v0, v1; };  // pot -1 = joystick circle
struct Cue { double t; int kind, a; float x; const char* s1; const char* s2; };
enum { C_BTN_DN, C_BTN_UP, C_STEP_DN, C_STEP_UP, C_KEY_DN, C_KEY_UP, C_CAPTION, C_SYNC, C_ANIM_DUMMY };

static void drawStrip() {
  SDL_RenderSetViewport(R, nullptr);
  color(0x0e0f12); fillRect(0, 0, kW, kStripH);
  color(kBlue); fillRect(0, kStripH - 3, kW, 3);
  const int S = 4, ox = 24, oy = 16;
  color(0x3a3c40); drawRect(ox - 6, oy - 6, Display::W * S + 12, Display::H * S + 12);
  SDL_Rect zd{ox, oy, Display::W * S, Display::H * S};
  SDL_RenderCopy(R, oledTex, nullptr, &zd);
  int tx = ox + Display::W * S + 34;
  color(0x6a707a); text(tx, kStripH - 18, "<- the 128x32 OLED, magnified x4 (real size is on the panel below)");
  color(0xffffff); text(tx, 34, g_cap1.c_str(), 3);
  color(0xc8ccd4);
  // wrap the second line at ~48 chars (scale 2 = 12 px per char)
  std::string s = g_cap2;
  int y = 78;
  while (!s.empty() && y < kStripH - 20) {
    size_t cut = s.size() <= 48 ? s.size() : s.rfind(' ', 48);
    if (cut == std::string::npos || cut == 0) cut = std::min<size_t>(48, s.size());
    text(tx, y, s.substr(0, cut).c_str(), 2);
    s = cut < s.size() ? s.substr(cut + 1) : "";
    y += 22;
  }
}

static std::vector<Cue> cues;
static std::vector<Anim> anims;
static void cap(double t, const char* a, const char* b) { cues.push_back({t, C_CAPTION, 0, 0, a, b}); }
static void btn(double t, int b, double hold = 0.18) { cues.push_back({t, C_BTN_DN, b, 0, 0, 0}); cues.push_back({t + hold, C_BTN_UP, b, 0, 0, 0}); }
static void step(double t, int s, double hold = 0.18) { cues.push_back({t, C_STEP_DN, s, 0, 0, 0}); cues.push_back({t + hold, C_STEP_UP, s, 0, 0, 0}); }
static void key(double t, int k, double hold = 0.25) { cues.push_back({t, C_KEY_DN, k, 0, 0, 0}); cues.push_back({t + hold, C_KEY_UP, k, 0, 0, 0}); }
static void func(double t, int s) { cues.push_back({t, C_BTN_DN, B_FUNC, 0, 0, 0}); step(t + 0.15, s, 0.15); cues.push_back({t + 0.4, C_BTN_UP, B_FUNC, 0, 0, 0}); }
static void knob(double t0, double t1, int pot, float v1) { anims.push_back({t0, t1, pot, -1.f, v1}); }
static void syncK(double t) { cues.push_back({t, C_SYNC, 0, 0, 0, 0}); }

#ifndef PVA_ACID
static double script() {  // ProVS Groove (120 BPM: one bar = 2 s)
  cap(0.0, "ProVS Groove", "Groovebox firmware for the Behringer PRO-VS MINI, running in a desktop simulator of the real panel");
  func(1.3, 6); step(1.9, 5); btn(2.2, B_FUNC, 0.1);           // mute the acid track for the intro
  btn(2.6, B_PLAY);
  cap(2.6, "Drums + vector synth", "4 synth drum tracks and the vector synth, 16-step sequencer with p-locks, ratchets and probability");
  func(6.6, 2); syncK(7.1);
  cap(6.6, "One idea per screen", "Turn a knob: the tiny OLED shows just that parameter, big");
  knob(7.4, 8.6, K_R2, 0.85f); knob(8.8, 10.0, K_R2, 0.25f);
  cap(10.6, "Enter the ACID track", "Track 6: mono acid bass with its own resonant filter, accent and slide");
  func(10.6, 6); step(11.2, 5); btn(11.5, B_FUNC, 0.1);         // unmute
  func(11.9, 5); syncK(12.4);                                     // select ACID
  cap(14.6, "Cutoff, resonance, env mod", "R1-R4 = cutoff / reso / env mod / decay. Consecutive accents push the filter higher");
  knob(14.8, 18.6, K_R1, 0.62f); knob(17.0, 19.0, K_R3, 0.95f); knob(19.0, 22.2, K_R4, 0.75f);
  knob(20.0, 22.4, K_R1, 0.38f);
  cap(22.6, "Drive", "LFO RATE = drive on the acid track");
  knob(22.8, 25.0, K_LFO_RATE, 0.85f);
  cap(26.6, "Joystick", "Live cutoff (X) and env mod (Y)");
  anims.push_back({26.8, 30.4, -1, 0, 0});                       // joystick circle
  cap(30.6, "Edit a step", "Hold a step: ENV = accent, LFO = slide, a key = the note");
  cues.push_back({31.0, C_STEP_DN, 3, 0, 0, 0});
  btn(31.4, B_ENV_SEL); btn(31.8, B_LFO_SEL); key(32.2, 14, 0.3);
  cues.push_back({32.9, C_STEP_UP, 3, 0, 0, 0});
  cues.push_back({33.4, C_STEP_DN, 8, 0, 0, 0});
  btn(33.8, B_LFO_SEL); key(34.2, 19, 0.3);
  cues.push_back({34.9, C_STEP_UP, 8, 0, 0, 0});
  cap(36.6, "DICE", "FUNC + 13 rolls a new line: notes, accents and slides");
  func(36.8, 12);
  cap(42.6, "Saw / square", "OSC button switches the oscillator");
  btn(42.8, B_OSC_SEL); knob(43.4, 46.4, K_R1, 0.55f);
  cap(46.6, "Engine done", "Next step: Behringer's firmware packer, so it can run on the real synth");
  knob(47.0, 50.6, K_R1, 0.2f);
  cap(50.6, "github.com/latepilot/ProVS-Groove", "Vibe-coded with Claude (Anthropic). Open source, AGPL-3.0");
  return 54.0;
}
#else
static double script() {  // ProVS Acid (126 BPM: one bar = 1.9 s)
  cap(0.0, "ProVS Acid", "A dedicated acid firmware for the PRO-VS MINI: two acid lines, drums and a pattern generator");
  btn(2.0, B_PLAY);
  cap(2.0, "Line A + drums", "Starter pattern 1: CLASSIC style in A minor, all written by the generator");
  cap(6.2, "Twist the filter", "R1-R4 = cutoff / resonance / env mod / decay, with accent and slide on their own knobs");
  knob(6.4, 9.6, K_R1, 0.62f); knob(8.0, 10.4, K_R3, 0.95f); knob(10.0, 12.0, K_R1, 0.36f);
  cap(12.4, "Generate", "FUNC + 1 writes a new line in the current style, scale and key");
  func(12.6, 0); func(16.4, 0);
  cap(20.0, "Style, scale, key", "FUNC + 7 = style (8 kinds), FUNC + 8 = scale, the keyboard sets the key");
  func(20.2, 6); step(20.8, 1);
  func(21.6, 7); key(22.2, 12); step(22.8, 1);
  func(23.4, 0);
  cap(27.6, "Density and wildness", "Hold FUNC and push the joystick: X = how many notes, Y = how wild");
  cues.push_back({27.8, C_BTN_DN, B_FUNC, 0, 0, 0});
  anims.push_back({28.0, 29.4, -2, 0.75f, 0.85f});
  cues.push_back({29.6, C_BTN_UP, B_FUNC, 0, 0, 0});
  func(30.0, 0);
  cap(33.4, "Mutate, rhythm, notes", "FUNC + 2 mutates a few steps, FUNC + 3 = new rhythm, FUNC + 4 = new notes, FUNC + 6 = undo");
  func(33.6, 1); func(35.5, 2); func(37.4, 3); func(39.3, 5);
  cap(41.0, "Line B", "ENV switches to line B: a counter line that answers line A");
  btn(41.2, B_ENV_SEL); syncK(41.5); func(41.8, 0);
  cap(45.4, "Accent and slide layers", "OSC flips the step buttons between gate, accent, slide and octave");
  btn(45.6, B_OSC_SEL); step(46.2, 2); step(46.7, 10); btn(47.2, B_OSC_SEL); step(47.7, 6);
  btn(48.4, B_OSC_SEL); btn(48.6, B_OSC_SEL);
  cap(49.4, "Evolve + synced LFO", "FUNC + 9: the pattern mutates by itself every few bars. LFO sweeps the cutoff in time");
  btn(49.6, B_ENV_SEL); btn(49.9, B_ENV_SEL); syncK(50.2);
  func(50.4, 8);
  knob(51.0, 53.0, K_LFO_RATE, 0.62f); knob(51.2, 54.0, K_LFO_AMT, 0.5f);
  cap(58.6, "github.com/latepilot/ProVS-Groove", "Vibe-coded with Claude (Anthropic). Open source, AGPL-3.0");
  return 63.0;
}
#endif

static int runVideo(const std::string& base, float sr) {
  const double kLen = script(), kFade = 2.5;
  std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.t < b.t; });

  std::string vid = base + "_video.mp4", wav = base + ".wav", mp4 = base + ".mp4";
  char cmd[1024];
  std::snprintf(cmd, sizeof cmd,
                "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i - -c:v libx264 -preset slow -crf 20 "
                "-tune animation -pix_fmt yuv420p \"%s\"", kW, kVidH, kFps, vid.c_str());
  FILE* ff = popen(cmd, "w");
  if (!ff) { std::printf("cannot start ffmpeg\n"); return 1; }
  std::vector<int16_t> pcm;
  SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, kW, kVidH, 32, SDL_PIXELFORMAT_ARGB8888);
  size_t ci = 0;
  uint64_t total = uint64_t(kLen * sr);
  int frames = int(kLen * kFps);
  float L[256], Rr[256];
  for (int f = 0; f < frames; f++) {
    uint64_t end = uint64_t(double(f + 1) / kFps * sr);
    while (g_gb.sampleClock() < end) {
      double now = double(g_gb.sampleClock()) / sr;
      while (ci < cues.size() && cues[ci].t <= now + 1e-9) {
        const Cue& c = cues[ci++];
        switch (c.kind) {
          case C_BTN_DN: setBtn(c.a, true); break;
          case C_BTN_UP: setBtn(c.a, false); break;
          case C_STEP_DN: setStep(c.a, true); break;
          case C_STEP_UP: setStep(c.a, false); break;
          case C_KEY_DN: setKey(c.a, true); break;
          case C_KEY_UP: setKey(c.a, false); break;
          case C_CAPTION: g_cap1 = c.s1; g_cap2 = c.s2; break;
          case C_SYNC: syncKnobs(); break;
        }
      }
      for (auto& a : anims) {
        if (now < a.t0 || now > a.t1 + 0.01) continue;
        float u = float(clamp01((now - a.t0) / (a.t1 - a.t0)));
        float e = u * u * (3.f - 2.f * u);  // ease in/out
        if (a.pot == -2) {  // push towards (v0, v1) and spring back
          float r = std::sin(u * kPi);
          joyX = a.v0 * r; joyY = a.v1 * r;
          if (u >= 1.f) joyX = joyY = 0;
          g_gb.joystick(joyX, joyY);
          continue;
        }
        if (a.pot < 0) {
          float ang = e * 2.f * kTwoPi, r = std::sin(u * kPi) * 0.95f;
          joyX = std::cos(ang) * r; joyY = std::sin(ang) * r;
          if (u >= 1.f) joyX = joyY = 0;
          g_gb.joystick(joyX, joyY);
          continue;
        }
        Knob& k = knobs[knobIndex(a.pot)];
        if (a.v0 < 0) a.v0 = k.v;  // start from wherever the knob is
        k.v = a.v0 + (a.v1 - a.v0) * e;
        g_gb.pot(a.pot, k.v);
      }
      uint64_t next = end;
      if (ci < cues.size()) next = std::min<uint64_t>(next, uint64_t(cues[ci].t * sr) + 1);
      int n = int(std::min<uint64_t>(256, next > g_gb.sampleClock() ? next - g_gb.sampleClock() : 1));
      g_gb.process(L, Rr, n);
      OutEvent oe;
      while (g_gb.popOutput(oe)) {}
      for (int i = 0; i < n; i++) {
        double t = double(pcm.size() / 2) / sr;
        float g = t > kLen - kFade ? float(std::max(0.0, (kLen - t) / kFade)) : 1.f;
        if (t < 0.05) g *= float(t / 0.05);
        pcm.push_back(int16_t(clampf(L[i] * g, -1.f, 1.f) * 32000.f));
        pcm.push_back(int16_t(clampf(Rr[i] * g, -1.f, 1.f) * 32000.f));
      }
    }
    SDL_RenderSetViewport(R, nullptr);
    color(0xd9d6cf); SDL_RenderClear(R);
    SDL_Rect vp{0, kStripH, kW, kH};
    SDL_RenderSetViewport(R, &vp);
    drawPanel();
    drawStrip();
    SDL_RenderReadPixels(R, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    for (int y = 0; y < kVidH; y++) fwrite(static_cast<uint8_t*>(surf->pixels) + y * surf->pitch, 4, kW, ff);
    if (f % (kFps * 5) == 0) { std::printf("  %d / %d s\n", f / kFps, int(kLen)); std::fflush(stdout); }
  }
  pclose(ff);
  (void)total;
  // wav
  FILE* w = std::fopen(wav.c_str(), "wb");
  uint32_t ds = uint32_t(pcm.size() * 2), srI = uint32_t(sr);
  uint32_t hdr[11] = {0x46464952, 36 + ds, 0x45564157, 0x20746d66, 16, 0x00020001, srI, srI * 4, 0x00100004, 0x61746164, ds};
  std::fwrite(hdr, 4, 11, w);
  std::fwrite(pcm.data(), 2, pcm.size(), w);
  std::fclose(w);
  std::snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k -movflags +faststart -shortest \"%s\"",
                vid.c_str(), wav.c_str(), mp4.c_str());
  int rc = std::system(cmd);
  std::remove(vid.c_str());
  std::printf("wrote %s\n", mp4.c_str());
  return rc;
}

int main(int argc, char** argv) {
  const char* shot = nullptr;
  const char* video = nullptr;
  for (int i = 1; i < argc; i++)
    if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!std::strcmp(argv[i], "--video") && i + 1 < argc) shot = video = argv[++i];
    else if (!std::strcmp(argv[i], "--zoom")) zoom = true;

  if (SDL_Init(SDL_INIT_VIDEO | (shot ? 0 : SDL_INIT_AUDIO)) != 0) { std::printf("SDL: %s\n", SDL_GetError()); return 1; }
  float sr = 48000.f;
  if (!shot) {
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 256;
    want.callback = audioCb;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_dev) { std::printf("audio: %s\n", SDL_GetError()); return 1; }
    sr = float(have.freq);
    g_sr = sr;
  }
  g_gb.init(sr);
  std::string state = statePath(argv[0]);
  if (!video) loadState(state);  // the video always uses the built-in demo
#ifdef PVA_ACID
  syncKnobs();
#endif

  SDL_Window* win = SDL_CreateWindow(kFwTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kW,
                                     video ? kVidH : kH, shot ? SDL_WINDOW_HIDDEN : 0);
  R = SDL_CreateRenderer(win, -1, shot ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
  if (!video) SDL_RenderSetLogicalSize(R, kW, kH);
  oledTex = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Display::W, Display::H);
  if (video) { int rc = runVideo(video, sr); SDL_Quit(); return rc; }

  if (shot) {
    float L[256], Rr[256];
    g_gb.buttonDown(B_PLAY);
    g_gb.buttonUp(B_PLAY);
    for (int i = 0; i < int(sr * 3.05f) / 256; i++) g_gb.process(L, Rr, 256);
    drawPanel();
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(R, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, shot);
    std::printf("wrote %s\n", shot);
    SDL_Quit();
    return 0;
  }

  if (!midiInit()) std::printf("CoreMIDI unavailable\n");
  SDL_PauseAudioDevice(g_dev, 0);
  bool quit = false;
  while (!quit) {
    if (g_midiChanged.exchange(false)) midiConnectAll();
    SDL_Event e;
    std::unique_lock<std::mutex> lk(g_ctl);
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: quit = true; break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          if (e.key.repeat) break;
          bool down = e.type == SDL_KEYDOWN;
          SDL_Scancode sc = e.key.keysym.scancode;
          if (down && sc == SDL_SCANCODE_ESCAPE) { quit = true; break; }
          if (down && sc == SDL_SCANCODE_TAB) { zoom = !zoom; break; }
          int s = scanToStep(sc);
          if (s >= 0) { if (!stepLatched[s]) setStep(s, down); break; }
          int k = scanToKey(sc);
          if (k >= 0) { if (!keyLatched[k]) setKey(k, down); break; }
          int b = scanToBtn(sc);
          if (b >= 0 && !btnLatched[b]) setBtn(b, down);
          break;
        }
        case SDL_MOUSEBUTTONDOWN: {
          int mx = e.button.x, my = e.button.y;
          int s = hitStep(mx, my), k = hitKey(mx, my), b = hitBtn(mx, my), kn = hitKnob(mx, my);
          if (e.button.button == SDL_BUTTON_RIGHT) {  // latch toggle for two-handed combos
            if (s >= 0) { stepLatched[s] = !stepLatched[s]; setStep(s, stepLatched[s]); }
            else if (k >= 0) { keyLatched[k] = !keyLatched[k]; setKey(k, keyLatched[k]); }
            else if (b >= 0) { btnLatched[b] = !btnLatched[b]; setBtn(b, btnLatched[b]); }
            break;
          }
          if (s >= 0 && !stepLatched[s]) { mouseStep = s; setStep(s, true); }
          else if (k >= 0 && !keyLatched[k]) { mouseKey = k; setKey(k, true); }
          else if (b >= 0 && !btnLatched[b]) { mouseBtn = b; setBtn(b, true); }
          else if (kn >= 0) dragKnob = kn;
          else if ((mx - kJoyCX) * (mx - kJoyCX) + (my - kJoyCY) * (my - kJoyCY) <= kJoyR * kJoyR) { dragJoy = true; joyFromMouse(mx, my); }
          break;
        }
        case SDL_MOUSEBUTTONUP:
          if (e.button.button != SDL_BUTTON_LEFT) break;
          if (mouseStep >= 0) { setStep(mouseStep, false); mouseStep = -1; }
          if (mouseKey >= 0) { setKey(mouseKey, false); mouseKey = -1; }
          if (mouseBtn >= 0) { setBtn(mouseBtn, false); mouseBtn = -1; }
          dragKnob = -1;
          if (dragJoy) { dragJoy = false; joyX = joyY = 0; g_gb.joystick(0, 0); }  // spring return
          break;
        case SDL_MOUSEMOTION:
          if (dragKnob >= 0) {
            Knob& kn = knobs[dragKnob];
            kn.v = clamp01(kn.v - e.motion.yrel * 0.006f + e.motion.xrel * 0.002f);
            g_gb.pot(kn.pot, kn.v);
          } else if (dragJoy) {
            joyFromMouse(e.motion.x, e.motion.y);
          } else if (mouseKey >= 0 && (e.motion.state & SDL_BUTTON_LMASK)) {  // glide along the strip
            int k = hitKey(e.motion.x, e.motion.y);
            if (k != mouseKey && k >= 0 && !keyLatched[k]) { setKey(mouseKey, false); mouseKey = k; setKey(k, true); }
          }
          break;
        case SDL_MOUSEWHEEL: {
          int mx, my;
          SDL_GetMouseState(&mx, &my);
          int kn = hitKnob(mx, my);
          if (kn >= 0) { knobs[kn].v = clamp01(knobs[kn].v + e.wheel.y * 0.01f); g_gb.pot(knobs[kn].pot, knobs[kn].v); }
          break;
        }
      }
    }
    lk.unlock();
    if (g_gb.takeSaveRequest()) saveState(state);
#ifdef PVA_ACID
    {  // knobs follow the selected lane's sound
      static int lastSel = -1;
      SDL_LockAudioDevice(g_dev);
      int sel = g_gb.lane() * 4 + (g_gb.lane() == AcidBox::LANE_DRUMS ? g_gb.layer() : 0);
      if (sel != lastSel) { lastSel = sel; syncKnobs(); }
      SDL_UnlockAudioDevice(g_dev);
    }
#endif
    drawPanel();
    SDL_RenderPresent(R);
  }
  SDL_CloseAudioDevice(g_dev);
  if (g_midi) MIDIClientDispose(g_midi);
  SDL_Quit();
  return 0;
}
