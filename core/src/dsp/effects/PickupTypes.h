#pragma once

namespace guitarfx::pickup_match
{
/// The kind of pickup, which groups the choices and picks their picture.
enum class PickupFamily
{
    SingleCoil,
    P90,
    Humbucker,
    Active,
    Other,
    Custom, ///< designed from its construction (PickupDesign.h)
};

/// Where on the guitar the pickup was measured, when the source says.
enum class PickupPosition
{
    Any,
    Neck,
    Middle,
    Bridge,
};

/// One pickup as its loaded electrical response: a second-order resonant low-pass.
struct PickupModel
{
    const char* id;    ///< stable slug, unique across the catalogue
    const char* label; ///< what the picker shows
    const char* brand; ///< "Generic" for the class averages
    PickupFamily family;
    PickupPosition position;
    double resonanceHz; ///< loaded resonant peak
    double q;           ///< sharpness of that peak
    double levelDb;     ///< output at low frequency; only differences between pickups matter
    double fitErrorDb;  ///< RMS error of the second-order fit to the measurement, 0 for a class
};
} // namespace guitarfx::pickup_match
