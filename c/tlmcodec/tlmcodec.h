/*
 * tlmcodec - StreamForge TLM binary protocol codec (C11).
 *
 * Implements the wire format documented in docs/tlm-protocol.md:
 *   - 24-byte file header (magic "TLM1", CRC-32/ISO-HDLC over the first 20 bytes)
 *   - frames: sync 0xA55A, frame_type, flags, payload_size, sequence, payload, payload CRC
 *   - data-frame payload: TLV dictionary (section 4 of the protocol document)
 *   - zlib-compressed payloads (flags bit0), decompressed size capped at 4 MiB
 *   - strict file-internal sequence (uint64, strictly increasing)
 *   - resync scanning for the sync word after corrupt frames
 *
 * The decoder performs no unaligned or unbounded accesses: every integer is assembled
 * byte-by-byte (little endian) and every length is bounds-checked before use.
 */
#ifndef TLMCODEC_H
#define TLMCODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (see docs/tlm-protocol.md section 7). */
typedef enum tlm_status {
    TLM_OK = 0,
    TLM_ERR_TRUNCATED = 1,          /* buffer ends before a complete structure */
    TLM_ERR_BAD_MAGIC = 2,          /* file magic is not "TLM1" */
    TLM_ERR_BAD_HEADER_CRC = 3,     /* file header CRC mismatch */
    TLM_ERR_BAD_HEADER_SIZE = 4,    /* header_size != 24 */
    TLM_ERR_UNSUPPORTED_VERSION = 5,/* file version not parseable by this codec */
    TLM_ERR_BAD_FLAGS = 6,          /* reserved flag bits set (header or frame) */
    TLM_ERR_BAD_SYNC = 7,           /* frame does not start with 0xA55A */
    TLM_ERR_PAYLOAD_TOO_LARGE = 8,  /* payload beyond 4 MiB (stored or inflated) */
    TLM_ERR_PAYLOAD_CRC = 9,        /* payload CRC mismatch */
    TLM_ERR_SEQUENCE_REGRESSION = 10, /* frame sequence not strictly increasing */
    TLM_ERR_BAD_COMPRESSION = 11,   /* zlib stream corrupt or not self-terminated */
    TLM_ERR_BAD_TLV = 12,           /* TLV truncated, duplicated, unknown-required, ... */
    TLM_ERR_NO_MEMORY = 13,         /* decompression buffer allocation failed */
    TLM_ERR_INVALID_ARGUMENT = 14   /* NULL or otherwise invalid arguments */
} tlm_status;

#define TLM_FRAME_HEADER_SIZE 16u
#define TLM_FILE_HEADER_SIZE 24u
#define TLM_MAX_PAYLOAD (4u * 1024u * 1024u)
#define TLM_MAX_TAGS 16u
#define TLM_TAG_KEY_MAX 128u
#define TLM_TAG_VALUE_MAX 128u
#define TLM_ID_MAX 64u
#define TLM_UNIT_MAX 32u
#define TLM_MAX_TLV_COUNT 256u
#define TLM_MAX_TLV_VALUE 4096u

/* Frame types. */
#define TLM_FRAME_DATA 1u
#define TLM_FRAME_DEVICE_META 2u
#define TLM_FRAME_HEARTBEAT 3u

/* Frame flags. */
#define TLM_FRAME_FLAG_COMPRESSED 0x01u

/* Cross-process parser state: tracks the last accepted frame sequence. */
typedef struct tlm_parser_state {
    int has_last_sequence;   /* 0 before the first accepted frame */
    uint64_t last_sequence;
    int suspicious;          /* set to 1 after a resync recovery */
} tlm_parser_state;

typedef struct tlm_file_header {
    uint16_t version;
    uint16_t header_size;
    int64_t created_at_ms;
    uint32_t flags;
} tlm_file_header;

typedef struct tlm_frame_info {
    uint8_t frame_type;    /* 1 data, 2 device metadata, 3 heartbeat, other = unknown */
    uint8_t flags;
    uint32_t payload_size; /* stored (possibly compressed) payload length in bytes */
    uint64_t sequence;
    size_t total_len;      /* bytes consumed: frame header + payload + CRC */
    int suspicious;        /* 1 when the frame follows a resync recovery */
} tlm_frame_info;

typedef struct tlm_tag {
    char key[129];
    char value[129];
    uint16_t key_len;
    uint16_t value_len;
} tlm_tag;

/* Decoded data-frame record (TLV dictionary, protocol section 4.2). */
typedef struct tlm_record {
    char device_id[TLM_ID_MAX + 1];
    char metric[TLM_ID_MAX + 1];
    int64_t event_time_ms;
    int has_value;      /* value/value_null TLV present */
    int value_is_null;  /* value_null TLV (0x05) present */
    double value;
    char unit[TLM_UNIT_MAX + 1];
    int has_quality;
    uint8_t quality;
    int has_sequence;
    uint64_t sequence;
    tlm_tag tags[TLM_MAX_TAGS];
    uint16_t tag_count;
    int suspicious; /* propagated from parser state after resync recovery */
} tlm_record;

/* Parses the 24-byte file header; validates magic, version, header_size, flags and CRC. */
tlm_status tlm_decode_file_header(const uint8_t* buf, size_t len, tlm_file_header* out);

/* Parses one frame starting at buf[0].
 * On TLM_OK: *consumed holds the total frame length; *info is filled; for data frames
 * *record is filled. Unknown frame types (not 1/2/3) parse their framing but leave the
 * record untouched. Sequence state is advanced only on accepted frames; a regression
 * returns TLM_ERR_SEQUENCE_REGRESSION with *consumed set so the caller can skip the
 * offending frame and continue with the next one. */
tlm_status tlm_parse_frame(const uint8_t* buf, size_t len, tlm_parser_state* st,
                           tlm_frame_info* info, size_t* consumed, tlm_record* record);

/* Scans forward up to max_scan bytes for the sync word (0x5A 0xA5 byte pair).
 * On success returns TLM_OK with *offset = number of bytes skipped before the sync word. */
tlm_status tlm_find_sync(const uint8_t* buf, size_t len, size_t max_scan, size_t* offset);

/* CRC-32/ISO-HDLC (zlib parameters: poly 0x04C11DB7 reflected, init/xorout 0xFFFFFFFF).
 * Exposed so tests and tools can verify payloads without a second implementation. */
uint32_t tlm_crc32(const uint8_t* data, size_t len);

/* Reference little-endian encoding helpers, used by tools and tests to build frames that
 * match the decoder byte-for-byte. */
void tlm_write_frame_header(uint8_t* buf, uint8_t frame_type, uint8_t flags, uint32_t payload_size,
                            uint64_t sequence);
void tlm_write_u16(uint8_t* p, uint16_t v);
void tlm_write_u32(uint8_t* p, uint32_t v);
void tlm_write_u64(uint8_t* p, uint64_t v);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TLMCODEC_H */
