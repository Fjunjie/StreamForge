// Reproduction of issue #2 in src/ingest/tlm/tlm_parser.cpp: TlmParser::refill.
// This is an extracted buffer algorithm, not a complete TLM decoder.
// No project libraries are needed: clang++ -std=c++17 tlm_refill_overwrite.cpp -o repro
// Run: repro (Windows: .\repro.exe). Exit 0 means the defect was reproduced.
//
// Defect: after compaction, retained bytes occupy [0, buf.size()). The buggy
// read starts at buf_begin_ (now zero), overwriting them instead of appending.
// A frame crossing a read-window boundary can therefore lose its header/body.
// Small streams hide this because the first read reaches EOF and no refill runs.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

constexpr size_t kWindow = 64 * 1024;

bool reproduce(bool fixed) {
    // Model the first 64 KiB read: 24-byte file header, then a frame whose
    // payload extends into the next window. The sync bytes must survive refill.
    std::string input(kWindow + 100, 'P');
    input[24] = static_cast<char>(0x5A);
    input[25] = static_cast<char>(0xA5);
    std::istringstream in(input);
    std::vector<uint8_t> buf(kWindow);
    in.read(reinterpret_cast<char*>(buf.data()), buf.size());
    buf.resize(static_cast<size_t>(in.gcount()));
    size_t buf_begin = 24; // file header consumed; frame remains incomplete

    // Same compaction as TlmParser::refill(): retain the incomplete frame.
    buf.erase(buf.begin(), buf.begin() + static_cast<long>(buf_begin));
    buf_begin = 0;
    const size_t retained = buf.size();
    const auto before = buf;

    if (!fixed) {
        // Extracted faulty refill: size is treated as writable capacity and
        // the retained prefix is overwritten. 100 remaining bytes replace it.
        if (buf.size() == buf_begin || buf.size() - buf_begin < 4096)
            buf.resize(std::max(kWindow, buf.size() * 2));
        in.read(reinterpret_cast<char*>(buf.data() + buf_begin),
                static_cast<std::streamsize>(buf.size() - buf_begin));
        buf.resize(buf_begin + static_cast<size_t>(in.gcount()));
    } else {
        // Reference repair: reserve writable space AFTER the retained bytes.
        buf.resize(retained + kWindow);
        in.read(reinterpret_cast<char*>(buf.data() + retained), kWindow);
        buf.resize(retained + static_cast<size_t>(in.gcount()));
    }

    const bool preserved = buf.size() >= retained &&
        std::equal(before.begin(), before.end(), buf.begin());
    std::cout << (fixed ? "fixed" : "buggy") << ": retained=" << retained
              << ", resulting_size=" << buf.size()
              << ", prefix_preserved=" << std::boolalpha << preserved << '\n';
    return preserved;
}

int main() {
    const bool buggy_preserved = reproduce(false);
    const bool fixed_preserved = reproduce(true);
    if (buggy_preserved || !fixed_preserved) {
        std::cerr << "Unexpected result: reproduction failed\n";
        return 1;
    }
    std::cout << "PASS: overwrite reproduced; append preserves the frame prefix\n";
}
