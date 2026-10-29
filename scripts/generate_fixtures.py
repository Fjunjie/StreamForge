#!/usr/bin/env python3
"""Generates the fixed three-format consistency fixtures (audit requirement:
tests must use committed binary samples, not only programmatically built frames).

Writes tests/fixtures/m2/consistency.{csv,jsonl,tlm} describing the SAME four
logical records. Run from the repo root:  python3 scripts/generate_fixtures.py
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "tests", "fixtures", "m2")


def tlv(t: int, v: bytes) -> bytes:
    return bytes([t]) + struct.pack("<H", len(v)) + v


def s(x: str) -> bytes:
    return x.encode("utf-8")


def frame(seq: int, tlvs: list) -> bytes:
    payload = b"".join(tlvs)
    head = struct.pack("<HBB IQ", 0xA55A, 1, 0, len(payload), seq)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    return head + payload + struct.pack("<I", crc)


# The same four logical records in all three formats. Event times are epoch
# milliseconds of 2026-09-01T00:16:40Z..00:17:00Z (inside the 365d history range
# of any run after 2026-09-01).
RECORDS = [
    # (device, metric, t_ms, value_or_None, unit, quality, sequence, tags)
    ("dev-01", "temp", 1788221800000, 76.4, "C", 0, 1, {"line": "A"}),
    ("dev-01", "temp", 1788221810000, None, "C", 0, 2, {}),
    ("dev-02", "temp", 1788221800000, 10.0, "C", 0, None, {"zone": "w"}),
    ("dev-01", "pressure", 1788221820000, 300.0, "kPa", 1, 9, {}),
]

ISO_TIMES = ["2026-09-01T00:16:40Z", "2026-09-01T00:16:50Z", "2026-09-01T00:16:40Z",
             "2026-09-01T00:17:00Z"]


def main() -> None:
    os.makedirs(OUT, exist_ok=True)

    # --- CSV ---------------------------------------------------------------
    csv_lines = ["device_id,metric,event_time,value,unit,quality,sequence,tags"]
    for i, (dev, metric, t_ms, value, unit, quality, seq, tags) in enumerate(RECORDS):
        tag_str = "|".join(f"{k}={v}" for k, v in tags.items())
        csv_lines.append(",".join([
            dev, metric, ISO_TIMES[i], "" if value is None else repr(value),
            unit, str(quality), "" if seq is None else str(seq), tag_str]))
    with open(os.path.join(OUT, "consistency.csv"), "w", newline="\n") as f:
        f.write("\n".join(csv_lines) + "\n")

    # --- JSON Lines ----------------------------------------------------------
    json_lines = []
    for i, (dev, metric, t_ms, value, unit, quality, seq, tags) in enumerate(RECORDS):
        obj = {"device_id": dev, "metric": metric, "event_time": ISO_TIMES[i],
               "value": value, "unit": unit, "tags": tags}
        if quality != 0:
            obj["quality"] = quality
        if seq is not None:
            obj["sequence"] = seq
        json_lines.append(obj)
    with open(os.path.join(OUT, "consistency.jsonl"), "w", newline="\n") as f:
        for obj in json_lines:
            import json
            f.write(json.dumps(obj, separators=(",", ":")) + "\n")

    # --- TLM -----------------------------------------------------------------
    def tag_tlv(tags: dict) -> bytes:
        out = bytearray()
        for k, v in tags.items():
            kb = k.encode()
            vb = v.encode()
            out += struct.pack("<H", len(kb)) + kb + struct.pack("<H", len(vb)) + vb
        return tlv(0x09, bytes(out))

    frames = []
    for seq_no, (dev, metric, t_ms, value, unit, quality, seq, tags) in enumerate(RECORDS, start=1):
        tlvs = [tlv(0x01, s(dev)), tlv(0x02, s(metric)), tlv(0x03, struct.pack("<q", t_ms))]
        if value is None:
            tlvs.append(tlv(0x05, b""))
        else:
            tlvs.append(tlv(0x04, struct.pack("<d", value)))
        tlvs.append(tlv(0x06, s(unit)))
        if quality:
            tlvs.append(tlv(0x07, bytes([quality])))
        if seq is not None:
            tlvs.append(tlv(0x08, struct.pack("<Q", seq)))
        if tags:
            tlvs.append(tag_tlv(tags))
        frames.append(frame(seq_no, tlvs))

    header = b"TLM1" + struct.pack("<HHQI", 1, 24, 1788221800000, 0)
    header += struct.pack("<I", zlib.crc32(header) & 0xFFFFFFFF)
    with open(os.path.join(OUT, "consistency.tlm"), "wb") as f:
        f.write(header + b"".join(frames))

    print("fixtures written to", OUT)


if __name__ == "__main__":
    main()
