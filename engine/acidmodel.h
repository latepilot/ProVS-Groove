// ProVS Acid data model: 16 patterns, each with two acid lines (A, B) and a
// small drum lane, plus the generator settings used to (re)build them.
// Plain POD so it can be memcpy'd to flash / a file.
#pragma once
#include <cstdint>

namespace pvg {
namespace acid {

constexpr int kSteps = 16;
constexpr int kPatterns = 16;
constexpr int kLines = 2;         // acid lines A and B
constexpr int kDrumInst = 4;      // kick, clap, closed hat, open hat
constexpr uint32_t kMagic = 0x50564131;  // "PVA1"
constexpr uint32_t kVersion = 2;

enum DrumInst : uint8_t { DI_KICK, DI_CLAP, DI_CH, DI_OH };

struct Step {
  uint8_t gate;    // 0 rest, 1 note
  uint8_t pitch;   // MIDI note (octave 0)
  int8_t oct;      // -1 .. +2
  uint8_t accent;
  uint8_t slide;   // glide into the next step
  uint8_t len;     // 1..4 steps (tie)
  uint8_t prob;    // 1..8 eighths
  uint8_t pad;
};

struct Line {
  Step steps[kSteps];
  uint8_t length;  // 1..16
  uint8_t pad[3];
};

struct Drums {
  uint16_t hits[kDrumInst];
  uint16_t accent;
  uint8_t length;
  uint8_t pad;
};

enum Style : uint8_t { ST_CLASSIC, ST_ROLLING, ST_HYPNO, ST_STAB, ST_SYNCO, ST_DEEP, ST_RAVE, ST_WILD, ST_COUNT };
enum Scale : uint8_t { SC_MINOR, SC_PHRYG, SC_DORIAN, SC_HARM, SC_PENTA, SC_BLUES, SC_MAJOR, SC_CHROM, SC_COUNT };

struct GenSettings {
  uint8_t style;    // Style
  uint8_t scale;    // Scale
  uint8_t root;     // 0..11 (C..B)
  uint8_t density;  // 0..127
  uint8_t wild;     // 0..127
  uint8_t pad[3];
};

struct Pattern {
  Line lines[kLines];
  Drums drums;
  GenSettings gen;
  uint8_t swing;    // 50..75
  uint8_t pad[3];
};

// Sound of one acid line (global, not per pattern: the knobs are live)
enum LineParam : uint8_t {
  LP_CUTOFF, LP_RESO, LP_ENVMOD, LP_DECAY, LP_ACCENT, LP_SLIDE, LP_WAVE, LP_TUNE,
  LP_DRIVE, LP_SUB, LP_LEVEL, LP_DIST, LP_LFO_RATE, LP_LFO_AMT, LP_SEND, LP_COUNT
};
// Sound of one drum (per instrument: kick, clap/snare, closed hat, open hat)
enum DrumParam : uint8_t {
  DP_ATTACK, DP_DECAY, DP_SNAP, DP_LEVEL,  // amplitude: R1-R4
  DP_TUNE, DP_TONE, DP_PAN, DP_SEND,       // FREQ, WAVE, LFO RATE, CHORUS AMT
  DP_MODEL,                                // kick DEEP/PUNCH, slot 2 CLAP/SNARE/RIM
  DP_COUNT
};
enum DrumModel2 : uint8_t { DM_CLAP, DM_SNARE, DM_RIM };

struct Project {
  uint32_t magic;
  uint32_t version;
  Pattern patterns[kPatterns];
  uint8_t line[kLines][16];   // LineParam values 0..127
  uint8_t drum[kDrumInst][10];  // DrumParam values per instrument
  uint8_t kitDrive;
  uint8_t bpm;
  uint8_t delayTime;
  uint8_t delayFb;
  uint8_t current;
  uint8_t clockSrc;           // pvg::ClockSrc
  uint8_t syncPpqn;
  uint8_t clockOut;
  uint8_t syncOut;
  uint8_t evolve;             // 0 off, else bars between mutations
  uint8_t analogA;            // 1: line A goes through the analog filter (V3320)
  uint8_t pad;
};

const char* styleName(int s);
const char* scaleName(int s);
const char* noteName(int pc);

}  // namespace acid
}  // namespace pvg
