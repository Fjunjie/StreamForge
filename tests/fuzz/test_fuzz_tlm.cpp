// libFuzzer entry for the C11 TLM codec (requirement 13.3): the file-header and frame
// parsers must survive arbitrary byte soup with an error result, never undefined
// behavior. Built only with -DSTREAMFORGE_BUILD_FUZZ=ON.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "tlmcodec.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 24)
        return 0;
    tlm_file_header header;
    if (tlm_decode_file_header(data, size, &header) != TLM_OK)
        return 0; // header soup rejected cleanly

    tlm_parser_state state{};
    size_t offset = 24;
    while (offset < size) {
        tlm_frame_info info{};
        tlm_record record{};
        size_t consumed = 0;
        tlm_status rc = tlm_parse_frame(data + offset, size - offset, &state, &info, &consumed, &record);
        if (rc != TLM_OK)
            break;
        if (consumed == 0)
            break; // zero-progress guard for hostile input
        offset += consumed;
    }
    return 0;
}
