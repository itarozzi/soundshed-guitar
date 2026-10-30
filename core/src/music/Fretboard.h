#pragma once

/**
 * Fretboard.h - Where scales and chords fall on a neck, for any tuning of 4 to 8 strings
 * (docs/plans/scale-guide.md). Nothing here is tabled: positions are generated and chord shapes
 * searched, so a drop tuning, an 8-string or a re-entrant ukulele gets them without a table of
 * its own.
 *
 * Strings are indexed in the order they lie across the neck (TuningDef::strings), which on a
 * re-entrant tuning is not pitch order; wherever "lowest" matters, it means lowest in pitch.
 */

#include "music/MusicCatalog.h"

#include <string>
#include <vector>

namespace guitarfx::music
{
constexpr int kDefaultFrets = 24;
constexpr int kMaxFingers = 4;
/// A hand position: four frets, one finger each.
constexpr int kHandFrets = 4;

struct Neck
{
    std::vector<int> strings;
    int frets = kDefaultFrets;
    Instrument instrument = Instrument::Guitar;

    /// Every string higher than the one before it (no re-entrant string).
    [[nodiscard]] bool Ascending() const;
    /// The string with the lowest open note.
    [[nodiscard]] int LowestString() const;
};

struct FretPos
{
    int string = 0;
    int fret = 0;

    auto operator<=>(const FretPos&) const = default;
};

/// One position of a scale: a box (n notes a string) or a hand-position window.
struct ScalePosition
{
    /// "box" or "window".
    std::string kind;
    int fretFrom = 0;
    int fretTo = 0;
    /// Sorted, no duplicates.
    std::vector<FretPos> cells;
};

/// A scale's positions on this neck, rooted on `rootPitchClass`; numbered from the one that starts
/// on the scale's first degree. Boxes on an ascending tuning for five- and seven-note scales (and
/// those that borrow such a box); windows otherwise. `notesPerString` 0 picks 2 for five notes and
/// 3 for seven; 3 for a pentatonic gives the three-a-string shapes.
[[nodiscard]] std::vector<ScalePosition> ScalePositions(const Neck& neck, const ScaleDef& scale, int rootPitchClass,
                                                        int notesPerString = 0);

struct Barre
{
    int fret = 0;
    int fromString = 0;
    int toString = 0;
    int finger = 1;
};

struct ChordShape
{
    /// One per string: -1 muted, 0 open, else the fret.
    std::vector<int> frets;
    /// One per string: 0 for none (muted or open), else 1 (index) … 4 (little).
    std::vector<int> fingers;
    std::vector<Barre> barres;
    /// The lowest fretted fret, or 0 when every sounding string is open.
    int position = 0;
    int highestFret = 0;
    double score = 0.0;
    /// "C", "A", "G", "E" or "D" on standard 6-string tuning; empty elsewhere.
    std::string caged;

    /// The first sounding string across the neck, or -1.
    [[nodiscard]] int FirstSoundingString() const;
    /// The mean fretted fret, or 0 for an all-open shape.
    [[nodiscard]] double AverageFret() const;
};

struct ChordSearch
{
    int rootPitchClass = 0;
    std::vector<Interval> intervals;
    /// Let a tone other than the root be lowest (inversions). A ukulele always does.
    bool allowInversions = false;
    /// The most shapes to return.
    int maxShapes = 6;
};

/// Playable shapes for a chord, spread along the neck and sorted up it. Each sounds every tone the
/// chord needs (a plain 5th may go from a chord of four or more), has its root lowest unless
/// inversions are allowed, no muted string between sounding ones, and needs at most four fingers
/// with a barre counting as one. Per instrument: a bass plays 2-3 strings (root and 5th, or root
/// and 3rd); a ukulele sounds all four; an extended-range guitar keeps a shape on its low strings
/// when one exists.
[[nodiscard]] std::vector<ChordShape> ChordShapes(const Neck& neck, const ChordSearch& search);

/// Fingers for a shape: separate fingers where four will do, else a barre at the lowest fret, then
/// small barres across neighbouring strings at one fret. False when no fingering fits.
bool AssignFingers(ChordShape& shape);

/// The shape nearest `previous` (mean fret, then first string), for stepping a progression without
/// the hand jumping. -1 when `shapes` is empty.
[[nodiscard]] int NearestShape(const std::vector<ChordShape>& shapes, const ChordShape& previous);
} // namespace guitarfx::music
