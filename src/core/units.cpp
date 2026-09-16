#include "streamforge/core/units.hpp"

#include <cmath>

namespace streamforge {
namespace core_units {

namespace {

// base = value * scale + offset (base units per dimension, see units.hpp)
const UnitInfo kUnits[] = {
    {"C", static_cast<int>(Dimension::Temperature), 1.0, 273.15},
    {"F", static_cast<int>(Dimension::Temperature), 5.0 / 9.0, 255.3722222222222222},
    {"K", static_cast<int>(Dimension::Temperature), 1.0, 0.0},
    {"Pa", static_cast<int>(Dimension::Pressure), 1.0, 0.0},
    {"kPa", static_cast<int>(Dimension::Pressure), 1e3, 0.0},
    {"MPa", static_cast<int>(Dimension::Pressure), 1e6, 0.0},
    {"bar", static_cast<int>(Dimension::Pressure), 1e5, 0.0},
    {"mm/s", static_cast<int>(Dimension::Velocity), 1e-3, 0.0},
    {"cm/s", static_cast<int>(Dimension::Velocity), 1e-2, 0.0},
    {"m/s", static_cast<int>(Dimension::Velocity), 1.0, 0.0},
    {"W", static_cast<int>(Dimension::Power), 1.0, 0.0},
    {"kW", static_cast<int>(Dimension::Power), 1e3, 0.0},
    {"MW", static_cast<int>(Dimension::Power), 1e6, 0.0},
    {"L/min", static_cast<int>(Dimension::Flow), 1.0 / 60.0, 0.0},
    {"m3/h", static_cast<int>(Dimension::Flow), 1000.0 / 3600.0, 0.0},
    {"m3/s", static_cast<int>(Dimension::Flow), 1000.0, 0.0},
};

} // namespace

const UnitInfo* find_unit(const std::string& unit) {
    for (const auto& u : kUnits) {
        if (unit == u.name)
            return &u;
    }
    return nullptr;
}

bool physically_valid(Dimension dim, double base_value) {
    if (!std::isfinite(base_value))
        return false;
    // All dimensions share the non-negativity floor (kelvin for temperature, zero for
    // the others); a single check keeps them in lockstep.
    (void)dim;
    return base_value >= 0.0;
}

Result<double> convert_unit(const std::string& input_unit, const std::string& canonical_unit, double value) {
    const UnitInfo* from = find_unit(input_unit);
    const UnitInfo* to = find_unit(canonical_unit);
    if (from == nullptr || to == nullptr) {
        return Result<double>::Err(
            Error::make(ErrorCode::ValidationUnitUnknown, "unit is not part of the conversion tables")
                .ctx("input", input_unit)
                .ctx("canonical", canonical_unit));
    }
    if (from->dimension != to->dimension) {
        return Result<double>::Err(Error::make(ErrorCode::ValidationUnitUnknown,
                                               "input unit and canonical unit belong to different dimensions")
                                       .ctx("input", input_unit)
                                       .ctx("canonical", canonical_unit));
    }
    double base = value * from->scale + from->offset;
    if (!std::isfinite(base)) {
        return Result<double>::Err(
            Error::make(ErrorCode::ValidationValueInvalid, "unit conversion overflowed the base unit")
                .ctx("input", input_unit));
    }
    double target = (base - to->offset) / to->scale;
    if (!std::isfinite(target)) {
        return Result<double>::Err(
            Error::make(ErrorCode::ValidationValueInvalid, "unit conversion overflowed the canonical unit")
                .ctx("canonical", canonical_unit));
    }
    if (!physically_valid(static_cast<Dimension>(from->dimension), base)) {
        return Result<double>::Err(
            Error::make(ErrorCode::ValidationValueInvalid, "value is physically invalid after conversion")
                .ctx("canonical", canonical_unit));
    }
    return Result<double>::Ok(target);
}

} // namespace core_units
} // namespace streamforge
