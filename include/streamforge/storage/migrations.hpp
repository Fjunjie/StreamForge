#pragma once

#include "streamforge/core/error.hpp"

namespace streamforge {
namespace storage {

class Db;

// Applies pending forward migrations inside transactions and verifies checksums of already
// applied ones. Returns the current schema version. Forward-only; no automatic downgrade
// (requirement FR-DB-001).
Result<int> migrate(Db& db);

// Reads the current schema version (0 when no migrations applied; -1 on query error).
Result<int> current_version(Db& db);

} // namespace storage
} // namespace streamforge
