#include "streamforge/core/uuid.hpp"

#include <uuid/uuid.h>

namespace streamforge {

std::string uuid_v4() {
    uuid_t id{};
    uuid_generate_random(id);
    char buf[37] = {}; // 36 chars + NUL
    uuid_unparse_lower(id, buf);
    return std::string{buf};
}

} // namespace streamforge
