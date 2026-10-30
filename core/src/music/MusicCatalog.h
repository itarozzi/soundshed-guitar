#pragma once

/**
 * MusicCatalog.h - What the Scale Guide offers (docs/plans/scale-guide.md): its scales, chord
 * qualities, progressions and tunings, written as interval formulas and roman numerals and parsed
 * once on first use. Ids are stable: the UIs keep them in app settings.
 */

#include "music/MusicTheory.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx::music
{
struct ScaleDef
{
    std::string id;
    std::string name;
    std::string group;
    std::vector<Interval> intervals;
    /// The scale whose boxes this one borrows (a blues scale draws its pentatonic's), or empty.
    std::string boxParent;
    /// A chord's tones as a scale: positions are hand-position windows, never n-notes-a-string.
    bool arpeggio = false;
};

struct ChordDef
{
    std::string id;
    std::string name;
    /// What follows the root in a chord name: "", "m", "7", "m7♭5".
    std::string symbol;
    std::vector<Interval> intervals;
};

struct ProgressionDef
{
    std::string id;
    std::string name;
    std::vector<RomanChord> chords;
};

enum class Instrument
{
    Guitar,
    Bass,
    Ukulele
};

struct TuningDef
{
    std::string id;
    std::string name;
    Instrument instrument = Instrument::Guitar;
    /// Open strings as MIDI notes in the order they lie across the neck, from the side a guitar's
    /// low string is on. Not sorted by pitch: a re-entrant ukulele's high G comes first.
    std::vector<int> strings;
    /// Name the open strings with flats (E♭ standard).
    bool flats = false;
};

[[nodiscard]] const std::vector<ScaleDef>& Scales();
[[nodiscard]] const ScaleDef* FindScale(std::string_view id);

[[nodiscard]] const std::vector<ChordDef>& Chords();
[[nodiscard]] const ChordDef* FindChord(std::string_view id);

[[nodiscard]] const std::vector<ProgressionDef>& Progressions();
[[nodiscard]] const ProgressionDef* FindProgression(std::string_view id);

[[nodiscard]] const std::vector<TuningDef>& Tunings();
[[nodiscard]] const TuningDef* FindTuning(std::string_view id);

[[nodiscard]] std::string_view InstrumentId(Instrument instrument);
[[nodiscard]] std::optional<Instrument> ParseInstrument(std::string_view id);

/// A chord with its root and tones spelled.
struct SpelledChord
{
    const ChordDef* def = nullptr;
    int rootPitchClass = 0;
    SpelledSet notes;
    /// "E7", "D♭maj7".
    std::string name;
    /// Its place in a progression, when it is in one.
    std::optional<RomanChord> roman;
};

/// One chord on a root, spelled whichever way reads better.
[[nodiscard]] SpelledChord SpellChord(int rootPitchClass, const ChordDef& def);

/// How a progression's key is written: the spelling that needs the fewest accidentals over every
/// tone of every chord, so ♭VII in E♭ is D♭. A tie goes to the flat key, except F♯.
[[nodiscard]] Spelling ProgressionKeySpelling(int keyPitchClass, const std::vector<RomanChord>& chords);

/// A progression's chords in a key, each root spelled from the key's letters. A chord whose
/// quality is unknown comes back as a major triad.
[[nodiscard]] std::vector<SpelledChord> ChordsInKey(int keyPitchClass, const std::vector<RomanChord>& chords);
} // namespace guitarfx::music
