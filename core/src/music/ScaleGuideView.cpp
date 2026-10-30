#include "music/ScaleGuideView.h"

#include <algorithm>
#include <array>

namespace guitarfx::music
{
namespace
{
using nlohmann::json;

int IntField(const json& object, const char* key, int fallback, int lowest, int highest)
{
    const auto it = object.find(key);

    if (it == object.end() || !it->is_number())
    {
        return fallback;
    }

    return std::clamp(it->get<int>(), lowest, highest);
}

std::string StringField(const json& object, const char* key, std::string fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

/// The neck the request plays on: its tuning (or the player's own), cut off at the capo.
struct ResolvedNeck
{
    Neck neck;
    std::string tuningId;
    std::string tuningName;
    bool flats = false;
};

ResolvedNeck Resolve(const ScaleGuideRequest& request)
{
    ResolvedNeck resolved;
    const auto count = static_cast<int>(request.strings.size());

    if (request.tuning == "custom" && count >= kMinStrings && count <= kMaxStrings)
    {
        resolved.neck.strings = request.strings;
        resolved.neck.instrument = request.instrument;
        resolved.tuningId = "custom";
        resolved.tuningName = "Custom";
    }
    else
    {
        const auto* tuning = FindTuning(request.tuning);

        if (tuning == nullptr)
        {
            tuning = &Tunings().front();
        }

        resolved.neck.strings = tuning->strings;
        resolved.neck.instrument = tuning->instrument;
        resolved.tuningId = tuning->id;
        resolved.tuningName = tuning->name;
        resolved.flats = tuning->flats;
    }

    resolved.neck.frets = request.frets;
    return resolved;
}

/// The neck above a capo: its strings sound `capo` higher and it has that many fewer frets.
Neck AboveCapo(const Neck& neck, int capo)
{
    Neck shifted = neck;

    for (auto& note : shifted.strings)
    {
        note += capo;
    }

    shifted.frets = neck.frets - capo;
    return shifted;
}

json IntervalLabels(const SpelledSet& set)
{
    std::string formula;

    for (const auto& note : set.notes)
    {
        formula += (formula.empty() ? "" : " ") + note.interval.label;
    }

    return formula;
}

json TonesJson(const SpelledSet& set)
{
    json tones = json::array();

    for (const auto& note : set.notes)
    {
        tones.push_back({{"name", note.name},
                         {"interval", note.interval.label},
                         {"degree", note.interval.degree},
                         {"pitchClass", note.pitchClass},
                         {"root", note.interval.degree == 1 && note.interval.semitones == 0}});
    }

    return tones;
}

json ShapeJson(const ChordShape& shape, int capo)
{
    json frets = json::array();

    for (const int fret : shape.frets)
    {
        frets.push_back(fret < 0 ? -1 : fret + capo);
    }

    json barres = json::array();

    for (const auto& barre : shape.barres)
    {
        barres.push_back({{"fret", barre.fret + capo},
                          {"fromString", barre.fromString},
                          {"toString", barre.toString},
                          {"finger", barre.finger}});
    }

    return {{"frets", frets},
            {"fingers", shape.fingers},
            {"barres", barres},
            {"position", shape.position + capo},
            {"caged", shape.caged}};
}

std::vector<int> ShiftedFrets(const std::vector<int>& frets, int capo)
{
    std::vector<int> out;

    for (const int fret : frets)
    {
        out.push_back(fret < 0 ? -1 : std::max(0, fret - capo));
    }

    return out;
}
} // namespace

ScaleGuideRequest ParseScaleGuideRequest(const json& object)
{
    ScaleGuideRequest request;

    if (!object.is_object())
    {
        return request;
    }

    request.mode = StringField(object, "mode", request.mode) == "chord" ? "chord" : "scale";
    request.key = PitchClass(IntField(object, "key", request.key, -1000, 1000));
    request.scale = StringField(object, "scale", request.scale);
    request.chord = StringField(object, "chord", request.chord);
    request.progression = StringField(object, "progression", "");
    request.progressionName = StringField(object, "progressionName", "");
    request.step = IntField(object, "step", 0, 0, 1000);
    request.tuning = StringField(object, "tuning", request.tuning);
    request.frets = IntField(object, "frets", kDefaultFrets, kMinFrets, kMaxFrets);
    request.capo = IntField(object, "capo", 0, 0, kMaxCapo);
    request.position = IntField(object, "position", 0, 0, 100);
    request.notesPerString = IntField(object, "notesPerString", 0, 0, 4);
    request.allowInversions = object.value("allowInversions", false);
    request.instrument = ParseInstrument(StringField(object, "instrument", "guitar")).value_or(Instrument::Guitar);

    if (const auto it = object.find("progressionChords"); it != object.end() && it->is_array())
    {
        for (const auto& token : *it)
        {
            if (!token.is_string() || static_cast<int>(request.progressionChords.size()) >= kMaxProgressionChords)
            {
                continue;
            }

            if (const auto chord = ParseRoman(token.get<std::string>()))
            {
                request.progressionChords.push_back(*chord);
            }
        }
    }

    if (const auto it = object.find("strings"); it != object.end() && it->is_array())
    {
        for (const auto& note : *it)
        {
            if (note.is_number_integer() && note.get<int>() >= 0 && note.get<int>() <= 127)
            {
                request.strings.push_back(note.get<int>());
            }
        }
    }

    if (const auto it = object.find("previousFrets"); it != object.end() && it->is_array())
    {
        for (const auto& fret : *it)
        {
            request.previousFrets.push_back(fret.is_number_integer() ? fret.get<int>() : -1);
        }
    }

    return request;
}

json BuildScaleGuideView(const ScaleGuideRequest& request)
{
    const auto resolved = Resolve(request);
    const auto& neck = resolved.neck;
    const int capo = std::min(request.capo, neck.frets - kMinFrets / 2);
    const Neck playing = AboveCapo(neck, capo);
    const bool chordMode = request.mode == "chord";

    json view;
    view["mode"] = chordMode ? "chord" : "scale";
    view["key"] = request.key;
    view["instrument"] = std::string(InstrumentId(neck.instrument));
    view["tuning"] = {{"id", resolved.tuningId}, {"name", resolved.tuningName}};
    view["frets"] = neck.frets;
    view["capo"] = capo;

    json strings = json::array();

    for (const int note : neck.strings)
    {
        strings.push_back({{"openMidi", note}, {"name", PlainNoteName(note, resolved.flats)}});
    }

    view["strings"] = strings;

    SpelledSet tones;
    std::vector<std::vector<FretPos>> positionCells;
    int position = request.position;

    if (!chordMode)
    {
        const auto* scale = FindScale(request.scale);

        if (scale == nullptr)
        {
            scale = &Scales().front();
        }

        tones = SpellBest(request.key, scale->intervals);
        const auto positions = ScalePositions(playing, *scale, request.key, request.notesPerString);
        json positionsJson = json::array();

        for (const auto& p : positions)
        {
            std::vector<FretPos> cells;

            for (const auto& cell : p.cells)
            {
                cells.push_back({cell.string, cell.fret + capo});
            }

            positionCells.push_back(std::move(cells));
            positionsJson.push_back({{"kind", p.kind}, {"fretFrom", p.fretFrom + capo}, {"fretTo", p.fretTo + capo}});
        }

        json diatonic = json::array();

        for (const auto& chord : DiatonicTriads(tones))
        {
            diatonic.push_back({{"name", chord.name},
                                {"numeral", chord.numeral},
                                {"quality", chord.quality},
                                {"rootPitchClass", chord.rootPitchClass}});
        }

        view["title"] = tones.notes.front().name + " " + scale->name;
        view["positions"] = positionsJson;
        view["scale"] = {{"id", scale->id},
                         {"name", scale->name},
                         {"steps", StepPattern(scale->intervals)},
                         {"formula", IntervalLabels(tones)},
                         {"diatonic", diatonic}};
    }
    else
    {
        SpelledChord chord;
        json progressionJson;
        std::vector<RomanChord> romans = request.progressionChords;
        std::string progressionId = romans.empty() ? std::string() : "custom";
        std::string progressionName = request.progressionName;

        if (romans.empty())
        {
            if (const auto* progression = FindProgression(request.progression))
            {
                romans = progression->chords;
                progressionId = progression->id;
                progressionName = progression->name;
            }
        }

        if (!romans.empty())
        {
            const auto chords = ChordsInKey(request.key, romans);
            const int step = request.step % static_cast<int>(chords.size());
            chord = chords[step];

            json steps = json::array();

            for (const auto& c : chords)
            {
                steps.push_back({{"name", c.name}, {"numeral", RomanNumeral(*c.roman)}});
            }

            progressionJson = {{"id", progressionId},
                               {"name", progressionName},
                               {"step", step},
                               {"chords", steps},
                               {"next", chords[(step + 1) % chords.size()].name}};
        }
        else
        {
            const auto* def = FindChord(request.chord);
            chord = SpellChord(request.key, def != nullptr ? *def : Chords().front());
        }

        tones = chord.notes;

        ChordSearch search;
        search.rootPitchClass = chord.rootPitchClass;
        search.intervals = chord.def->intervals;
        search.allowInversions = request.allowInversions;
        const auto shapes = ChordShapes(playing, search);

        if (!request.previousFrets.empty() && !shapes.empty())
        {
            ChordShape previous;
            previous.frets = ShiftedFrets(request.previousFrets, capo);
            position = NearestShape(shapes, previous) + 1;
        }

        json shapesJson = json::array();

        for (const auto& shape : shapes)
        {
            std::vector<FretPos> cells;

            for (int s = 0; s < static_cast<int>(shape.frets.size()); ++s)
            {
                if (shape.frets[s] >= 0)
                {
                    cells.push_back({s, shape.frets[s] + capo});
                }
            }

            positionCells.push_back(std::move(cells));
            shapesJson.push_back(ShapeJson(shape, capo));
        }

        view["title"] = chord.name;
        view["shapes"] = shapesJson;
        view["chord"] = {{"id", chord.def->id},
                         {"name", chord.name},
                         {"quality", chord.def->name},
                         {"formula", IntervalLabels(tones)}};

        if (!progressionJson.is_null())
        {
            view["progression"] = progressionJson;
        }
    }

    if (position < 0 || position > static_cast<int>(positionCells.size()))
    {
        position = 0;
    }

    view["position"] = position;
    view["flats"] = tones.UsesFlats();
    view["tones"] = TonesJson(tones);

    std::array<int, kPitchClasses> toneIndex{};
    toneIndex.fill(-1);

    for (int i = 0; i < static_cast<int>(tones.notes.size()); ++i)
    {
        if (toneIndex[tones.notes[i].pitchClass] < 0)
        {
            toneIndex[tones.notes[i].pitchClass] = i;
        }
    }

    const std::vector<FretPos>* selected = position > 0 ? &positionCells[position - 1] : nullptr;
    json dots = json::array();

    for (int s = 0; s < static_cast<int>(neck.strings.size()); ++s)
    {
        for (int fret = capo; fret <= neck.frets; ++fret)
        {
            const int index = toneIndex[PitchClass(neck.strings[s] + fret)];

            if (index < 0)
            {
                continue;
            }

            const auto& note = tones.notes[index];
            const bool inPosition = selected == nullptr ||
                                    std::find(selected->begin(), selected->end(), FretPos{s, fret}) != selected->end();
            dots.push_back({{"string", s},
                            {"fret", fret},
                            {"tone", index},
                            {"name", note.name},
                            {"interval", note.interval.label},
                            {"degree", note.interval.degree},
                            {"root", index == 0},
                            {"inPosition", inPosition}});
        }
    }

    view["dots"] = dots;
    return view;
}

json ScaleGuideCatalogJson()
{
    json scales = json::array();

    for (const auto& scale : Scales())
    {
        std::string formula;

        for (const auto& interval : scale.intervals)
        {
            formula += (formula.empty() ? "" : " ") + interval.label;
        }

        scales.push_back({{"id", scale.id}, {"name", scale.name}, {"group", scale.group}, {"formula", formula}});
    }

    json chords = json::array();

    for (const auto& chord : Chords())
    {
        chords.push_back({{"id", chord.id}, {"name", chord.name}, {"symbol", chord.symbol}});
    }

    json progressions = json::array();

    for (const auto& progression : Progressions())
    {
        json numerals = json::array();

        for (const auto& chord : progression.chords)
        {
            numerals.push_back(RomanNumeral(chord));
        }

        progressions.push_back({{"id", progression.id}, {"name", progression.name}, {"numerals", numerals}});
    }

    json tunings = json::array();

    for (const auto& tuning : Tunings())
    {
        json names = json::array();

        for (const int note : tuning.strings)
        {
            names.push_back(PlainNoteName(note, tuning.flats));
        }

        tunings.push_back({{"id", tuning.id},
                           {"name", tuning.name},
                           {"instrument", std::string(InstrumentId(tuning.instrument))},
                           {"strings", tuning.strings},
                           {"stringNames", names}});
    }

    json keys = json::array();

    for (int pc = 0; pc < kPitchClasses; ++pc)
    {
        const auto spellings = KeySpellings(pc);
        keys.push_back(spellings.size() > 1 ? PlainNoteName(pc, false) + "/" + PlainNoteName(pc, true)
                                            : PlainNoteName(pc, false));
    }

    return {
        {"keys", keys}, {"scales", scales}, {"chords", chords}, {"progressions", progressions}, {"tunings", tunings}};
}
} // namespace guitarfx::music
