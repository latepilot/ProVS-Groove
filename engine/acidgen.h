// Acid pattern generator.
//
// The aim is lines that sound written, not random: a short motif (rhythm cell
// + pitch cell) is built first and then repeated across the pattern with small
// variations, a "response" in the second half and a turnaround at the end.
// Pitches come from a scale with the root, fifth, minor third and seventh
// weighted the way acid lines use them; octave jumps prefer off-beat 16ths;
// slides go where the pitch changes; accents lean on syncopation.
//
// Every operation takes the RNG explicitly, so the same seed gives the same
// pattern (handy for tests and for "undo").
#pragma once
#include "acidmodel.h"
#include "dsp.h"

namespace pvg {
namespace acid {

struct GenCtx {
  GenSettings gen;
  int line = 0;               // 0 = A (bass), 1 = B (counter line, higher, sparser)
  const Line* other = nullptr;  // the other line, so B can answer A
};

int scaleSemi(int scale, int degree);  // degree may be negative or > 7
int scaleSize(int scale);
int baseNote(const GenCtx& c);        // root MIDI note of the line's register

void generateLine(Line& l, const GenCtx& c, Rng& r);
void regenRhythm(Line& l, const GenCtx& c, Rng& r);     // new rhythm, keeps the note order
void regenNotes(Line& l, const GenCtx& c, Rng& r);      // new notes, keeps the rhythm
void regenAccSlide(Line& l, const GenCtx& c, Rng& r);   // new accents + slides only
void mutate(Line& l, const GenCtx& c, Rng& r, float amount);
void rotate(Line& l, int dir);
void clearLine(Line& l, int rootNote);

void generateDrums(Drums& d, const GenSettings& g, Rng& r);
void mutateDrums(Drums& d, const GenSettings& g, Rng& r, float amount);
void regenDrumAccents(Drums& d, const GenSettings& g, Rng& r);

}  // namespace acid
}  // namespace pvg
