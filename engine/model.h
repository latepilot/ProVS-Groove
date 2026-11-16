// Data model: project -> 16 patterns -> 5 tracks -> 16 steps.
// Plain POD so it can be memcpy'd to flash / a file.
#pragma once
#include <cstdint>

namespace pvg {

constexpr int kNumSteps = 16;
constexpr int kNumTracks = 6;
constexpr int kNumPatterns = 16;
constexpr int kNumParams = 32;
constexpr int kMaxNotes = 3;  // notes per synth step (paraphonic chords)
constexpr int kSynthTrack = 4;
constexpr int kAcidTrack = 5;
constexpr uint8_t kNoNote = 0xFF;
constexpr uint32_t kProjectMagic = 0x50564731;  // "PVG1"
constexpr uint32_t kProjectVersion = 4;

enum ClockSrc : uint8_t { CLK_INT, CLK_MIDI, CLK_SYNC };
static const uint8_t kSyncPpqn[4] = {1, 2, 4, 24};  // pulses per quarter on the SYNC jack

enum TrackType : uint8_t { TT_KICK, TT_SNARE, TT_HAT, TT_PERC, TT_SYNTH, TT_ACID };
inline bool isDrum(int t) { return t < TT_SYNTH; }
inline bool isMelodic(int t) { return t >= TT_SYNTH; }  // tracks that store notes

// Synth parameter slots
enum : uint8_t {
  S_AMP_A = 0, S_AMP_D, S_AMP_S, S_AMP_R,   // R1-R4 with ENV=AMP
  S_FIL_A, S_FIL_D, S_FIL_S, S_FIL_R,       // R1-R4 with ENV=FIL
  S_WAVE_A, S_WAVE_B, S_WAVE_C, S_WAVE_D,   // WAVE knob, OSC selector
  S_TUNE_A, S_TUNE_B, S_TUNE_C, S_TUNE_D,   // FREQ knob (64 = 0 semitones)
  S_DETUNE,                                 // FUNC+FREQ
  S_LFO1_RATE, S_LFO1_AMT,                  // LFO 1 spins the vector
  S_LFO2_RATE, S_LFO2_AMT,                  // LFO 2 -> cutoff
  P_SEND,                                   // CHORUS AMT, every track
  S_CUTOFF, S_RESO, S_FENV,                 // analog filter (+FUNC+CUTOFF = env amount)
  S_VEC_X, S_VEC_Y,                         // vector position (joystick latch / ENV=MIX R1,R2)
};
// Drum parameter slots
enum : uint8_t {
  D_TUNE = 0, D_DECAY, D_TONE, D_LEVEL,     // R1-R4
  D_DRIVE,                                  // FREQ
  D_CRUSH,                                  // WAVE
  D_PAN,                                    // LFO RATE
  D_SWEEP,                                  // LFO AMT
};
// Acid bass slots (same knobs as the drum slots above)
enum : uint8_t {
  A_CUTOFF = 0, A_RESO, A_ENVMOD, A_DECAY,  // R1-R4
  A_TUNE,                                   // FREQ (64 = 0, +-12 semitones)
  A_ACCENT,                                 // WAVE
  A_DRIVE,                                  // LFO RATE
  A_SLIDE,                                  // LFO AMT (glide time)
};

struct Step {
  uint8_t on;
  uint8_t accent;
  uint8_t prob;      // 1..8 eighths (8 = always)
  uint8_t ratchet;   // 1..4 hits per step
  uint8_t gate;      // synth: length in steps 1..16
  uint8_t notes[kMaxNotes];
  uint8_t slide;     // acid: glide into the next step
  uint8_t pad[3];
  uint32_t lockMask; // bit per param slot
  uint8_t lock[kNumParams];
};

struct Track {
  Step steps[kNumSteps];
  uint8_t length;    // 1..16 (per-track length -> polymeter)
  uint8_t variant;   // drum model 0..3
  uint8_t p[kNumParams];
  uint8_t pad[2];
};

struct Pattern {
  Track tracks[kNumTracks];
  uint8_t swing;     // 50..75 %
  uint8_t pad[3];
};

struct Project {
  uint32_t magic;
  uint32_t version;
  Pattern patterns[kNumPatterns];
  uint8_t bpm;       // 60..200
  uint8_t delayTime; // index into delay divisions
  uint8_t delayFb;   // 0..127
  uint8_t current;   // current pattern
  uint8_t clockSrc;  // ClockSrc
  uint8_t syncPpqn;  // index into kSyncPpqn
  uint8_t clockOut;  // send MIDI clock / start / stop
  uint8_t syncOut;   // pulse the SYNC jack (when it is not the clock source)
};

}  // namespace pvg
