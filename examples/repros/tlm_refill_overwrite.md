# Issue #2: incomplete TLM frame overwritten during refill

Source: `src/ingest/tlm/tlm_parser.cpp`, `TlmParser::refill()` (around lines 45–60).

The adjacent C++ sample extracts the buffer logic into a standalone program.
It demonstrates the actual overwrite and compares it with an append operation.
It does not invoke the production parser or validate CRC/TLV decoding, so it is
evidence for the buffer defect, not a full ingestion regression test.

## Run

```powershell
clang++ -std=c++17 -Wall -Wextra -pedantic examples/repros/tlm_refill_overwrite.cpp -o build/tlm-refill-repro.exe
./build/tlm-refill-repro.exe
```

Expected output:

```text
buggy: retained=65512, resulting_size=100, prefix_preserved=false
fixed: retained=65512, resulting_size=65612, prefix_preserved=true
PASS: overwrite reproduced; append preserves the frame prefix
```

The first read fills a 64 KiB window without reaching EOF. After consuming the
24-byte file header, 65,512 bytes of an incomplete frame remain. Compaction
moves that frame to index zero. The buggy read writes the final 100 stream bytes
at index zero and then resizes the buffer to 100 bytes, destroying the retained
frame prefix, including its sync bytes. The reference operation appends instead.

The precise trigger is refill with retained, unconsumed bytes. File size above
64 KiB alone does not guarantee failure: complete frames may be consumed before
refill. Small inputs that reach EOF on their first read often avoid the path.

## Questions for CodeHawk's missed-detection analysis

Review the production implementation independently, then trace these invariants:

1. Before compaction, which bytes are consumed and which are still live?
2. After `buf_begin_ = 0`, does zero denote the parse cursor or the write cursor?
3. Does `buf_.size()` represent valid data length or writable spare capacity?
4. Does the destination of `in_.read()` overlap the retained frame?
5. Does the final `resize(buf_begin_ + got)` discard retained data?
6. Did the original review inspect `refill()` and its incomplete-frame caller,
   or only codec validation and small-file tests?
7. Did EOF handling or the “Compact” comment lead to an assumption that this
   buffer operation was safe without tracking live byte ranges?

Separate confirmed evidence about the original review (files read, tool traces,
reasoning recorded) from hypotheses. This sample cannot establish why a model
missed the defect. A useful follow-up is a production-parser regression using a
valid TLM frame spanning the 64 KiB boundary, asserting recovery of its record,
accurate offsets, and finite EOF termination.
