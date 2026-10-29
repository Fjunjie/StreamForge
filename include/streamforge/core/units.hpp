#pragma once

#include <string>

#include "streamforge/core/error.hpp"

namespace streamforge {
namespace core_units {

// Built-in unit conversion (requirement FR-VAL-004).
//
// Every known unit belongs to one dimension and carries an affine mapping to the
// dimension's base unit: base = value * scale + offset.
//   Temperature: base kelvin   (C, F, K)
//   Pressure:    base pascal   (Pa, kPa, MPa, bar)
//   Velocity:    base m/s      (mm/s, cm/s, m/s)
//   Power:       base watt     (W, kW, MW)
//   Flow:        base m3/s     (L/min, m3/h, m3/s)
struct UnitInfo {
    const char* name;
    int dimension;
    double scale;
    double offset;
};

enum class Dimension {
    Temperature = 0,
    Pressure = 1,
    Velocity = 2,
    Power = 3,
    Flow = 4,
};

// Returns nullptr when the unit is unknown to the conversion tables.
const UnitInfo* find_unit(const std::string& unit);

// Physical validity of a value expressed in the dimension's canonical display unit used
// by convert_unit results (absolute-zero floor for temperature, non-negativity for the
// others).
bool physically_valid(Dimension dim, double base_value);

// Converts `value` expressed in `input_unit` into `canonical_unit` (both must be known
// units of the same dimension). Fails with ValidationUnitUnknown for unknown units or
// dimension mismatch and with ValidationValueInvalid for overflow or physically invalid
// results (e.g. below absolute zero).
Result<double> convert_unit(const std::string& input_unit, const std::string& canonical_unit, double value);

} // namespace core_units
} // namespace streamforge
