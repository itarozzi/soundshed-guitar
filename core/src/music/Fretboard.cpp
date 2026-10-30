#include "music/Fretboard.h"

#include <algorithm>
#include <array>

namespace guitarfx::music
{
namespace
{
using PitchSet = std::array<bool, kPitchClasses>;

PitchSet PitchesOf(const std::vector<Interval>& intervals, int rootPitchClass)
{
    PitchSet set{};

    for (const auto& interval : intervals)
    {
        set[PitchClass(rootPitchClass + interval.semitones)] = true;
    }

    return set;
}

void AddCell(std::vector<FretPos>& cells, int string, int fret)
{
    const FretPos cell{string, fret};

    if (std::find(cells.begin(), cells.end(), cell) == cells.end())
    {
        cells.push_back(cell);
    }
}

/// Box p starts on degree p of `box` on the lowest string and climbs, `perString` notes to a
/// string; a note the next string cannot reach (a drop tuning's gap) stays on this one. Notes of
/// the full scale that the box's own scale lacks (a blues scale's ♭5) join within its fret span.
std::vector<ScalePosition> Boxes(const Neck& neck, const std::vector<Interval>& box, const PitchSet& full,
                                 int rootPitchClass, int perString)
{
    std::vector<ScalePosition> positions;
    const auto boxSet = PitchesOf(box, rootPitchClass);
    const auto& strings = neck.strings;
    const int count = static_cast<int>(strings.size());

    for (const auto& start : box)
    {
        ScalePosition position;
        position.kind = "box";
        position.fretFrom = neck.frets;
        position.fretTo = 0;

        int note = strings[0] + PitchClass(rootPitchClass + start.semitones - strings[0]);

        for (int s = 0; s < count; ++s)
        {
            const bool last = s == count - 1;

            for (int placed = 0;; ++note)
            {
                while (!boxSet[PitchClass(note)])
                {
                    ++note;
                }

                if (placed >= perString && (last || note >= strings[s + 1]))
                {
                    break;
                }

                const int fret = note - strings[s];

                if (fret >= 0 && fret <= neck.frets)
                {
                    AddCell(position.cells, s, fret);
                    position.fretFrom = std::min(position.fretFrom, fret);
                    position.fretTo = std::max(position.fretTo, fret);
                }

                ++placed;
            }
        }

        for (int s = 0; s < count; ++s)
        {
            for (int fret = position.fretFrom; fret <= position.fretTo; ++fret)
            {
                const int pc = PitchClass(strings[s] + fret);

                if (full[pc] && !boxSet[pc])
                {
                    AddCell(position.cells, s, fret);
                }
            }
        }

        std::sort(position.cells.begin(), position.cells.end());
        positions.push_back(std::move(position));
    }

    return positions;
}

/// A position per scale degree: the four frets from where that degree sits on the lowest-pitched
/// string, every scale note in them. Works on any tuning, re-entrant ones included.
std::vector<ScalePosition> Windows(const Neck& neck, const std::vector<Interval>& intervals, int rootPitchClass)
{
    std::vector<ScalePosition> positions;
    const auto set = PitchesOf(intervals, rootPitchClass);
    const int lowest = neck.strings[neck.LowestString()];

    for (const auto& degree : intervals)
    {
        const int from = PitchClass(rootPitchClass + degree.semitones - lowest);
        const bool seen = std::any_of(positions.begin(), positions.end(),
                                      [from](const ScalePosition& p) { return p.fretFrom == from; });

        if (seen)
        {
            continue;
        }

        ScalePosition position;
        position.kind = "window";
        position.fretFrom = from;
        position.fretTo = std::min(neck.frets, from + kHandFrets - 1);

        for (int s = 0; s < static_cast<int>(neck.strings.size()); ++s)
        {
            for (int fret = position.fretFrom; fret <= position.fretTo; ++fret)
            {
                if (set[PitchClass(neck.strings[s] + fret)])
                {
                    position.cells.push_back({s, fret});
                }
            }
        }

        std::sort(position.cells.begin(), position.cells.end());
        positions.push_back(std::move(position));
    }

    return positions;
}
} // namespace

bool Neck::Ascending() const
{
    for (std::size_t i = 1; i < strings.size(); ++i)
    {
        if (strings[i] <= strings[i - 1])
        {
            return false;
        }
    }

    return true;
}

int Neck::LowestString() const
{
    return static_cast<int>(std::min_element(strings.begin(), strings.end()) - strings.begin());
}

std::vector<ScalePosition> ScalePositions(const Neck& neck, const ScaleDef& scale, int rootPitchClass,
                                          int notesPerString)
{
    if (neck.strings.empty())
    {
        return {};
    }

    const ScaleDef* box = &scale;

    if (!scale.boxParent.empty())
    {
        if (const auto* parent = FindScale(scale.boxParent))
        {
            box = parent;
        }
    }

    const auto size = box->intervals.size();
    const int natural = size == 5 ? 2 : size == 7 ? 3 : 0;
    const int perString = notesPerString > 0 ? notesPerString : natural;

    if (scale.arpeggio || natural == 0 || neck.strings.size() < 2 || !neck.Ascending())
    {
        return Windows(neck, scale.intervals, rootPitchClass);
    }

    return Boxes(neck, box->intervals, PitchesOf(scale.intervals, rootPitchClass), rootPitchClass, perString);
}
} // namespace guitarfx::music
