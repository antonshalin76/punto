#include "punto_ingress_wire.h"

#include <linux/input-event-codes.h>
#include <openssl/sha.h>
#include <string.h>

void
punto_ingress_digest_records(const uint8_t *records, size_t length,
                             uint8_t digest[PUNTO_INGRESS_DIGEST_BYTES])
{
    if (!digest)
        return;
    if ((!records && length != 0) || !SHA256(records, length, digest))
        memset(digest, 0, PUNTO_INGRESS_DIGEST_BYTES);
}

static int
punto_valid_action(uint32_t action)
{
    return action >= PUNTO_ACTION_LAYOUT_WORD &&
           action <= PUNTO_ACTION_TRANSLIT_SELECTION;
}

enum PuntoIngressDecodeResult
punto_ingress_validate_frame(const PuntoIngressFrameHeader *header,
                             const PuntoIngressRecord *records, size_t count)
{
    size_t i;
    uint8_t digest[PUNTO_INGRESS_DIGEST_BYTES];

    if (!header || !records || !count || count > PUNTO_INGRESS_MAX_RECORDS)
        return PUNTO_INGRESS_DECODE_LENGTH;
    if (header->magic != PUNTO_INGRESS_MAGIC ||
        header->version != PUNTO_INGRESS_VERSION ||
        header->kind != PUNTO_INGRESS_FRAME ||
        header->stream_epoch == 0 || header->sequence == 0 ||
        header->record_count != count ||
        header->byte_length != sizeof(*header) + count * sizeof(*records))
        return PUNTO_INGRESS_DECODE_HEADER;
    for (i = 0; i < count; ++i) {
        const PuntoIngressRecord *record = &records[i];

        if (record->reserved ||
            (i && record->source_index <= records[i - 1].source_index))
            return PUNTO_INGRESS_DECODE_RECORD;
        switch (record->kind) {
        case PUNTO_INGRESS_KEY:
            if (record->code >= KEY_CNT || record->code == KEY_PAUSE ||
                record->value < 0 || record->value > 2 || record->action ||
                record->physical_modifiers || record->request_id ||
                record->deadline_monotonic_ns)
                return PUNTO_INGRESS_DECODE_RECORD;
            break;
        case PUNTO_INGRESS_SYN:
            if (i + 1 != count || record->code || record->value ||
                record->action || record->physical_modifiers ||
                record->request_id || record->deadline_monotonic_ns)
                return PUNTO_INGRESS_DECODE_RECORD;
            break;
        case PUNTO_INGRESS_PAUSE_BEGIN:
        case PUNTO_INGRESS_PAUSE_REPEAT:
        case PUNTO_INGRESS_PAUSE_END:
            if (record->code || record->value ||
                !punto_valid_action(record->action) ||
                (record->physical_modifiers & ~PUNTO_MOD_ALL) ||
                !record->request_id || !record->deadline_monotonic_ns)
                return PUNTO_INGRESS_DECODE_RECORD;
            break;
        default:
            return PUNTO_INGRESS_DECODE_RECORD;
        }
    }
    if (records[count - 1].kind != PUNTO_INGRESS_SYN)
        return PUNTO_INGRESS_DECODE_RECORD;
    punto_ingress_digest_records((const uint8_t *)records,
                                 count * sizeof(*records), digest);
    if (memcmp(header->digest, digest, sizeof(digest)) != 0)
        return PUNTO_INGRESS_DECODE_DIGEST;
    return PUNTO_INGRESS_DECODE_OK;
}

enum PuntoIngressDecodeResult
punto_ingress_decode_frame(const uint8_t *packet, size_t length,
                           PuntoIngressFrameHeader *header,
                           PuntoIngressRecord *records, size_t capacity)
{
    PuntoIngressFrameHeader candidate;
    PuntoIngressRecord candidate_records[PUNTO_INGRESS_MAX_RECORDS];
    enum PuntoIngressDecodeResult result;

    if (!packet || !header || !records || length < sizeof(candidate) ||
        length > PUNTO_INGRESS_FRAME_MAX_BYTES)
        return PUNTO_INGRESS_DECODE_LENGTH;
    memcpy(&candidate, packet, sizeof(candidate));
    if (candidate.record_count == 0 ||
        candidate.record_count > PUNTO_INGRESS_MAX_RECORDS ||
        candidate.record_count > capacity ||
        candidate.byte_length != length ||
        length != sizeof(candidate) +
                  (size_t)candidate.record_count * sizeof(PuntoIngressRecord))
        return PUNTO_INGRESS_DECODE_LENGTH;
    memcpy(candidate_records, packet + sizeof(candidate),
           candidate.record_count * sizeof(PuntoIngressRecord));
    result = punto_ingress_validate_frame(&candidate, candidate_records,
                                          candidate.record_count);
    if (result != PUNTO_INGRESS_DECODE_OK)
        return result;
    *header = candidate;
    memcpy(records, candidate_records,
           candidate.record_count * sizeof(PuntoIngressRecord));
    return PUNTO_INGRESS_DECODE_OK;
}
