#pragma once

#include <cstdint>

namespace streamforge {

// Sample flag bits stored in samples.flags (requirement 9.1).
namespace sample_flags {
constexpr uint32_t kDuplicate = 1u << 0;   // dropped as duplicate (recorded, not inserted)
constexpr uint32_t kLate = 1u << 1;        // arrived after the watermark
constexpr uint32_t kSynthetic = 1u << 2;   // produced by interpolation
constexpr uint32_t kForcedFlush = 1u << 3; // emitted because the reorder buffer hit its limit
constexpr uint32_t kSuspicious = 1u << 4;  // TLM resync recovery, quality downgraded
} // namespace sample_flags

// Record quality codes (requirement FR-FMT-001).
enum class Quality : int {
    Normal = 0,
    Suspicious = 1,
    DeviceError = 2,
};

// Sample flag bit for staging rows (see storage schema).
enum class StagedStatus : int {
    Accepted = 0,
    FormatError = 1,
    BusinessError = 2,
};

} // namespace streamforge
