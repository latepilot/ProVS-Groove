// The PRO-VS MINI front panel as the firmware sees it. Shared by every
// firmware in this repo (ProVS Groove, ProVS Acid) and by the simulator.
#pragma once
#include <cstdint>

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

// Outgoing timing events, timestamped in samples (engine sample clock).
// OUT_MIDI: 1..3 MIDI bytes (clock 0xF8, start 0xFA, stop 0xFC) for USB MIDI.
// OUT_SYNC: rising edge for the SYNC jack (HAL keeps it high ~5 ms).
enum OutType : uint8_t { OUT_MIDI, OUT_SYNC };
struct OutEvent { uint64_t time; uint8_t type, len; uint8_t b[3]; };

enum ClockSrc : uint8_t { CLK_INT, CLK_MIDI, CLK_SYNC };
static const uint8_t kSyncPpqn[4] = {1, 2, 4, 24};  // pulses per quarter on the SYNC jack

constexpr int kNumKeys = 27;      // capacitive strip F..G (16 white + 11 black pads)
constexpr int kKeyBaseNote = 41;  // lowest key at octave 0 = F2
inline bool keyIsBlack(int k) { int s = (k + 5) % 12; return s == 1 || s == 3 || s == 6 || s == 8 || s == 10; }  // strip starts at F

}  // namespace pvg
