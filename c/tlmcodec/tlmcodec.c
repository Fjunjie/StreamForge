/*
 * tlmcodec - StreamForge TLM binary protocol codec (C11).
 * See tlmcodec.h and docs/tlm-protocol.md for the wire format.
 */
#include "tlmcodec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------ */
/* Little-endian byte access (alignment-safe by construction)          */
/* ------------------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t* p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

static int64_t rd_i64(const uint8_t* p) { return (int64_t)rd_u64(p); }

static void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wr_u64(uint8_t* p, uint64_t v) {
    wr_u32(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr_u32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

uint32_t tlm_crc32(const uint8_t* data, size_t len) {
    /* zlib crc32() implements CRC-32/ISO-HDLC exactly as specified by the protocol. */
    uLong crc = crc32(0L, Z_NULL, 0);
    if (len > 0 && data != NULL) {
        crc = crc32(crc, data, (uInt)len);
    }
    return (uint32_t)crc;
}

static int is_id_char(uint8_t c) {
    return (c >= (uint8_t)'A' && c <= (uint8_t)'Z') || (c >= (uint8_t)'a' && c <= (uint8_t)'z') ||
           (c >= (uint8_t)'0' && c <= (uint8_t)'9') || c == (uint8_t)'-' || c == (uint8_t)'_' ||
           c == (uint8_t)'.';
}

static int valid_identifier(const uint8_t* v, size_t len) {
    if (len == 0 || len > TLM_ID_MAX) return 0;
    for (size_t i = 0; i < len; ++i) {
        if (!is_id_char(v[i])) return 0;
    }
    return 1;
}

static int valid_unit_value(const uint8_t* v, size_t len) {
    if (len == 0 || len > TLM_UNIT_MAX) return 0;
    for (size_t i = 0; i < len; ++i) {
        if (v[i] <= 0x20u || v[i] >= 0x7Fu) return 0;
    }
    return 1;
}

static void copy_str(char* dst, size_t dst_cap, const uint8_t* v, size_t len) {
    if (len > dst_cap - 1) len = dst_cap - 1; /* callers validated lengths beforehand */
    memcpy(dst, v, len);
    dst[len] = '\0';
}

/* ------------------------------------------------------------------ */
/* File header                                                         */
/* ------------------------------------------------------------------ */

tlm_status tlm_decode_file_header(const uint8_t* buf, size_t len, tlm_file_header* out) {
    if (buf == NULL || out == NULL) return TLM_ERR_INVALID_ARGUMENT;
    if (len < TLM_FILE_HEADER_SIZE) return TLM_ERR_TRUNCATED;
    if (!(buf[0] == 'T' && buf[1] == 'L' && buf[2] == 'M' && buf[3] == '1')) {
        return TLM_ERR_BAD_MAGIC;
    }
    uint32_t stored_crc = rd_u32(buf + 20);
    if (tlm_crc32(buf, 20) != stored_crc) return TLM_ERR_BAD_HEADER_CRC;

    uint16_t version = rd_u16(buf + 4);
    uint16_t header_size = rd_u16(buf + 6);
    if (header_size != TLM_FILE_HEADER_SIZE) return TLM_ERR_BAD_HEADER_SIZE;
    if (version != 1u) return TLM_ERR_UNSUPPORTED_VERSION;
    if (rd_u32(buf + 16) != 0u) return TLM_ERR_BAD_FLAGS;

    out->version = version;
    out->header_size = header_size;
    out->created_at_ms = rd_i64(buf + 8);
    out->flags = 0;
    return TLM_OK;
}

/* ------------------------------------------------------------------ */
/* TLV payload decoding (data frames)                                  */
/* ------------------------------------------------------------------ */

/* Tags payload layout (protocol section 4.3), repeated up to 16 entries:
 *   klen (u16 LE) | key (klen bytes) | vlen (u16 LE) | value (vlen bytes) */
static tlm_status parse_tags_tlv(const uint8_t* v, size_t len, tlm_record* rec) {
    size_t i = 0;
    while (i < len) {
        if (len - i < 2) return TLM_ERR_BAD_TLV;
        uint16_t klen = rd_u16(v + i);
        i += 2;
        if (klen == 0 || klen > TLM_TAG_KEY_MAX || (size_t)klen > len - i) return TLM_ERR_BAD_TLV;
        const uint8_t* key = v + i;
        i += klen;
        if (len - i < 2) return TLM_ERR_BAD_TLV;
        uint16_t vlen = rd_u16(v + i);
        i += 2;
        if (vlen > TLM_TAG_VALUE_MAX || (size_t)vlen > len - i) return TLM_ERR_BAD_TLV;
        if (rec->tag_count >= TLM_MAX_TAGS) return TLM_ERR_BAD_TLV;
        tlm_tag* tag = &rec->tags[rec->tag_count];
        memcpy(tag->key, key, klen);
        tag->key[klen] = '\0';
        tag->key_len = klen;
        memcpy(tag->value, v + i, vlen);
        tag->value[vlen] = '\0';
        tag->value_len = vlen;
        rec->tag_count++;
        i += vlen;
    }
    return TLM_OK;
}

static tlm_status parse_data_payload(const uint8_t* p, size_t len, tlm_record* rec) {
    int has_dev = 0, has_metric = 0, has_time = 0, has_value = 0, has_null = 0, has_unit = 0;
    int has_quality = 0, has_sequence = 0, has_tags = 0;
    size_t count = 0;
    size_t i = 0;

    memset(rec, 0, sizeof(*rec));
    while (i < len) {
        if (len - i < 3) return TLM_ERR_BAD_TLV;
        uint8_t type = p[i];
        uint16_t vlen = rd_u16(p + i + 1);
        i += 3;
        if ((size_t)vlen > len - i) return TLM_ERR_BAD_TLV;
        const uint8_t* v = p + i;
        i += vlen;
        if (++count > TLM_MAX_TLV_COUNT) return TLM_ERR_BAD_TLV;
        if (vlen > TLM_MAX_TLV_VALUE) return TLM_ERR_BAD_TLV;

        switch (type) {
            case 0x01: /* device_id */
                if (has_dev) return TLM_ERR_BAD_TLV;
                if (!valid_identifier(v, vlen)) return TLM_ERR_BAD_TLV;
                copy_str(rec->device_id, sizeof(rec->device_id), v, vlen);
                has_dev = 1;
                break;
            case 0x02: /* metric */
                if (has_metric) return TLM_ERR_BAD_TLV;
                if (!valid_identifier(v, vlen)) return TLM_ERR_BAD_TLV;
                copy_str(rec->metric, sizeof(rec->metric), v, vlen);
                has_metric = 1;
                break;
            case 0x03: /* event_time_ms */
                if (has_time) return TLM_ERR_BAD_TLV;
                if (vlen != 8) return TLM_ERR_BAD_TLV;
                rec->event_time_ms = rd_i64(v);
                has_time = 1;
                break;
            case 0x04: /* value (IEEE 754 double LE) */
                if (has_value || has_null) return TLM_ERR_BAD_TLV;
                if (vlen != 8) return TLM_ERR_BAD_TLV;
                {
                    uint64_t bits = rd_u64(v);
                    memcpy(&rec->value, &bits, sizeof(rec->value));
                }
                rec->has_value = 1;
                has_value = 1;
                break;
            case 0x05: /* value_null */
                if (has_value || has_null) return TLM_ERR_BAD_TLV;
                if (vlen != 0) return TLM_ERR_BAD_TLV;
                rec->has_value = 1;
                rec->value_is_null = 1;
                has_null = 1;
                break;
            case 0x06: /* unit */
                if (has_unit) return TLM_ERR_BAD_TLV;
                if (!valid_unit_value(v, vlen)) return TLM_ERR_BAD_TLV;
                copy_str(rec->unit, sizeof(rec->unit), v, vlen);
                has_unit = 1;
                break;
            case 0x07: /* quality */
                if (has_quality) return TLM_ERR_BAD_TLV;
                if (vlen != 1) return TLM_ERR_BAD_TLV;
                rec->quality = v[0];
                rec->has_quality = 1;
                has_quality = 1;
                break;
            case 0x08: /* sequence */
                if (has_sequence) return TLM_ERR_BAD_TLV;
                if (vlen != 8) return TLM_ERR_BAD_TLV;
                rec->sequence = rd_u64(v);
                rec->has_sequence = 1;
                has_sequence = 1;
                break;
            case 0x09: /* tags */
                if (has_tags) return TLM_ERR_BAD_TLV;
                if (parse_tags_tlv(v, vlen, rec) != TLM_OK) return TLM_ERR_BAD_TLV;
                has_tags = 1;
                break;
            default:
                break; /* unknown TLV: skipped per protocol section 4.1 */
        }
    }

    if (!has_dev || !has_metric || !has_time || !has_unit || (!has_value && !has_null)) {
        return TLM_ERR_BAD_TLV; /* required fields missing */
    }
    rec->suspicious = 0;
    return TLM_OK;
}

/* ------------------------------------------------------------------ */
/* Decompression                                                       */
/* ------------------------------------------------------------------ */

static tlm_status inflate_payload(const uint8_t* in, uint32_t in_len, uint8_t** out,
                                  size_t* out_len) {
    *out = NULL;
    *out_len = 0;
    uint8_t* buf = malloc(TLM_MAX_PAYLOAD);
    if (buf == NULL) return TLM_ERR_NO_MEMORY;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) {
        free(buf);
        return TLM_ERR_BAD_COMPRESSION;
    }
    zs.next_in = (Bytef*)in; /* zlib API lacks const; it does not modify next_in */
    zs.avail_in = in_len;
    zs.next_out = buf;
    zs.avail_out = TLM_MAX_PAYLOAD;

    int rc = inflate(&zs, Z_FINISH);
    int ok = (rc == Z_STREAM_END);
    size_t produced = TLM_MAX_PAYLOAD - zs.avail_out;
    inflateEnd(&zs);
    if (!ok || produced > TLM_MAX_PAYLOAD) {
        free(buf);
        if (rc == Z_OK || rc == Z_BUF_ERROR) return TLM_ERR_PAYLOAD_TOO_LARGE;
        return TLM_ERR_BAD_COMPRESSION;
    }
    *out = buf;
    *out_len = produced;
    return TLM_OK;
}

/* ------------------------------------------------------------------ */
/* Frame parsing                                                       */
/* ------------------------------------------------------------------ */

tlm_status tlm_parse_frame(const uint8_t* buf, size_t len, tlm_parser_state* st,
                           tlm_frame_info* info, size_t* consumed, tlm_record* record) {
    if (buf == NULL || info == NULL || consumed == NULL || st == NULL) {
        return TLM_ERR_INVALID_ARGUMENT;
    }
    if (len < TLM_FRAME_HEADER_SIZE) return TLM_ERR_TRUNCATED;

    uint16_t sync = rd_u16(buf);
    if (sync != 0xA55Au) return TLM_ERR_BAD_SYNC;

    uint8_t ftype = buf[2];
    uint8_t fflags = buf[3];
    uint32_t payload_size = rd_u32(buf + 4);
    uint64_t sequence = rd_u64(buf + 8);

    if ((fflags & ~TLM_FRAME_FLAG_COMPRESSED) != 0u) return TLM_ERR_BAD_FLAGS;
    if (payload_size > TLM_MAX_PAYLOAD) return TLM_ERR_PAYLOAD_TOO_LARGE;
    if (len - TLM_FRAME_HEADER_SIZE < (size_t)payload_size + 4u) return TLM_ERR_TRUNCATED;

    /* The frame framing is now fully determined: report the consumed length and frame
     * info on EVERY outcome from here on (CRC/sequence/TLV/compression errors included)
     * so the caller can always skip past the frame and make progress. */
    *consumed = TLM_FRAME_HEADER_SIZE + (size_t)payload_size + 4u;
    info->frame_type = ftype;
    info->flags = fflags;
    info->payload_size = payload_size;
    info->sequence = sequence;
    info->total_len = *consumed;
    info->suspicious = st->suspicious;

    const uint8_t* payload = buf + TLM_FRAME_HEADER_SIZE;
    uint32_t stored_crc = rd_u32(payload + payload_size);
    if (tlm_crc32(payload, payload_size) != stored_crc) return TLM_ERR_PAYLOAD_CRC;

    if (st->has_last_sequence && sequence <= st->last_sequence) {
        return TLM_ERR_SEQUENCE_REGRESSION;
    }

    tlm_status status = TLM_OK;
    if (ftype == TLM_FRAME_DATA) {
        /* TLV validation happens regardless of whether the caller wants the decoded
         * record: malformed data frames must be rejected either way. */
        tlm_record local_record;
        tlm_record* target = record != NULL ? record : &local_record;
        if ((fflags & TLM_FRAME_FLAG_COMPRESSED) != 0u) {
            uint8_t* inflated = NULL;
            size_t inflated_len = 0;
            status = inflate_payload(payload, payload_size, &inflated, &inflated_len);
            if (status == TLM_OK) {
                status = parse_data_payload(inflated, inflated_len, target);
                free(inflated);
            }
        } else {
            status = parse_data_payload(payload, payload_size, target);
        }
        if (status == TLM_OK && record != NULL) {
            record->suspicious = st->suspicious;
        }
    }

    if (status == TLM_OK) {
        st->has_last_sequence = 1;
        st->last_sequence = sequence;
    }
    return status;
}

tlm_status tlm_find_sync(const uint8_t* buf, size_t len, size_t max_scan, size_t* offset) {
    if (buf == NULL || offset == NULL) return TLM_ERR_INVALID_ARGUMENT;
    if (len < 2) return TLM_ERR_BAD_SYNC;
    size_t limit = max_scan < len - 1 ? max_scan : len - 1;
    for (size_t i = 0; i < limit; ++i) {
        if (buf[i] == 0x5Au && buf[i + 1] == 0xA5u) {
            *offset = i;
            return TLM_OK;
        }
    }
    return TLM_ERR_BAD_SYNC;
}

/* Encoding helpers are used by tools and tests; kept here so the reference encoder
 * matches the decoder byte-for-byte. */
void tlm_write_frame_header(uint8_t* buf, uint8_t frame_type, uint8_t flags, uint32_t payload_size,
                            uint64_t sequence) {
    wr_u16(buf, 0xA55Au);
    buf[2] = frame_type;
    buf[3] = flags;
    wr_u32(buf + 4, payload_size);
    wr_u64(buf + 8, sequence);
}

void tlm_write_u16(uint8_t* p, uint16_t v) { wr_u16(p, v); }
void tlm_write_u32(uint8_t* p, uint32_t v) { wr_u32(p, v); }
void tlm_write_u64(uint8_t* p, uint64_t v) { wr_u64(p, v); }
