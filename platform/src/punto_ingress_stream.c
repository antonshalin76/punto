#include "punto_ingress_stream.h"

#include <string.h>

void
punto_ingress_stream_init(PuntoIngressStream *stream, uint64_t epoch)
{
    if (!stream)
        return;
    memset(stream, 0, sizeof(*stream));
    stream->epoch = epoch;
    stream->next_sequence = 1;
}

static int
punto_same_request(const PuntoIngressPauseState *pause,
                   const PuntoIngressRecord *record)
{
    return pause->request_id == record->request_id &&
           pause->deadline_monotonic_ns == record->deadline_monotonic_ns &&
           pause->action == record->action &&
           pause->physical_modifiers == record->physical_modifiers;
}

enum PuntoIngressStreamResult
punto_ingress_stream_preview(const PuntoIngressStream *stream,
                             const PuntoIngressFrameHeader *header,
                             const PuntoIngressRecord *records, size_t count,
                             uint64_t now_ns, PuntoIngressPreview *preview)
{
    PuntoIngressPauseState pause;
    uint64_t last_request_id;
    size_t i, slot;
    uint32_t begins = 0;

    if (!stream || !header || !records || !preview || !stream->epoch ||
        !now_ns || header->stream_epoch != stream->epoch ||
        punto_ingress_validate_frame(header, records, count) !=
            PUNTO_INGRESS_DECODE_OK)
        return PUNTO_STREAM_BAD_FRAME;
    if (header->sequence < stream->next_sequence) {
        for (i = 0; i < stream->receipt_count; ++i) {
            const PuntoIngressReceipt *receipt = &stream->receipts[i];
            if (receipt->sequence != header->sequence)
                continue;
            if (receipt->record_count != count ||
                memcmp(receipt->digest, header->digest,
                       PUNTO_INGRESS_DIGEST_BYTES) != 0)
                return PUNTO_STREAM_CONFLICT;
            return PUNTO_STREAM_DUPLICATE;
        }
        return PUNTO_STREAM_BAD_SEQUENCE;
    }
    if (header->sequence != stream->next_sequence ||
        stream->next_sequence == UINT64_MAX)
        return PUNTO_STREAM_BAD_SEQUENCE;

    pause = stream->pause;
    last_request_id = stream->last_request_id;
    for (i = 0; i < count; ++i) {
        const PuntoIngressRecord *record = &records[i];
        switch (record->kind) {
        case PUNTO_INGRESS_PAUSE_BEGIN:
            if (pause.active || record->request_id <= last_request_id)
                return PUNTO_STREAM_BAD_LIFECYCLE;
            pause.active = 1;
            pause.request_id = record->request_id;
            pause.deadline_monotonic_ns = record->deadline_monotonic_ns;
            pause.action = record->action;
            pause.physical_modifiers = record->physical_modifiers;
            last_request_id = record->request_id;
            ++begins;
            break;
        case PUNTO_INGRESS_PAUSE_REPEAT:
            if (pause.active && !punto_same_request(&pause, record))
                return PUNTO_STREAM_BAD_LIFECYCLE;
            break;
        case PUNTO_INGRESS_PAUSE_END:
            if (pause.active) {
                if (!punto_same_request(&pause, record))
                    return PUNTO_STREAM_BAD_LIFECYCLE;
                pause.active = 0;
            }
            break;
        default:
            break;
        }
    }
    if (stream->receipt_count < PUNTO_INGRESS_RECEIPT_CAPACITY) {
        slot = stream->receipt_count;
    }
    else {
        for (slot = 0; slot < stream->receipt_count; ++slot) {
            const PuntoIngressReceipt *receipt = &stream->receipts[slot];
            if (receipt->status >= PUNTO_FRAME_DISPATCHED &&
                now_ns >= receipt->received_monotonic_ns &&
                now_ns - receipt->received_monotonic_ns >=
                    PUNTO_INGRESS_RECEIPT_NS)
                break;
        }
        if (slot == stream->receipt_count)
            return PUNTO_STREAM_FULL;
    }
    memset(preview, 0, sizeof(*preview));
    preview->sequence = header->sequence;
    preview->last_request_id_after = last_request_id;
    preview->pause_after = pause;
    preview->receipt_slot = slot;
    preview->record_count = (uint32_t)count;
    preview->new_request_count = begins;
    memcpy(preview->digest, header->digest, PUNTO_INGRESS_DIGEST_BYTES);
    return PUNTO_STREAM_NEW;
}

int
punto_ingress_stream_commit(PuntoIngressStream *stream,
                            const PuntoIngressPreview *preview,
                            uint64_t now_ns)
{
    PuntoIngressReceipt *receipt;

    if (!stream || !preview || preview->sequence != stream->next_sequence ||
        preview->receipt_slot > stream->receipt_count ||
        preview->receipt_slot >= PUNTO_INGRESS_RECEIPT_CAPACITY || !now_ns)
        return 0;
    receipt = &stream->receipts[preview->receipt_slot];
    memset(receipt, 0, sizeof(*receipt));
    receipt->sequence = preview->sequence;
    receipt->received_monotonic_ns = now_ns;
    receipt->record_count = preview->record_count;
    memcpy(receipt->digest, preview->digest, PUNTO_INGRESS_DIGEST_BYTES);
    receipt->status = PUNTO_FRAME_ACCEPTED;
    if (preview->receipt_slot == stream->receipt_count)
        ++stream->receipt_count;
    stream->pause = preview->pause_after;
    stream->last_request_id = preview->last_request_id_after;
    ++stream->next_sequence;
    return 1;
}

int
punto_ingress_stream_query(const PuntoIngressStream *stream,
                           uint64_t sequence, PuntoIngressReceipt *receipt)
{
    size_t i;

    if (!stream || !receipt || !sequence)
        return 0;
    for (i = 0; i < stream->receipt_count; ++i) {
        if (stream->receipts[i].sequence == sequence) {
            *receipt = stream->receipts[i];
            return 1;
        }
    }
    return 0;
}

int
punto_ingress_stream_set_status(PuntoIngressStream *stream,
                                 uint64_t sequence, uint32_t status,
                                 uint32_t dispatched_count)
{
    size_t i;

    if (!stream || !sequence || status <= PUNTO_FRAME_ACCEPTED ||
        status > PUNTO_FRAME_FAILED_PARTIAL)
        return 0;
    for (i = 0; i < stream->receipt_count; ++i) {
        PuntoIngressReceipt *receipt = &stream->receipts[i];
        if (receipt->sequence != sequence)
            continue;
        if (dispatched_count > receipt->record_count ||
            dispatched_count < receipt->dispatched_count ||
            status < receipt->status ||
            receipt->status >= PUNTO_FRAME_DISPATCHED ||
            (status == PUNTO_FRAME_FAILED_BEFORE && dispatched_count != 0) ||
            (status == PUNTO_FRAME_FAILED_PARTIAL &&
             (dispatched_count == 0 ||
              dispatched_count >= receipt->record_count)) ||
            ((status == PUNTO_FRAME_DISPATCHED ||
              status == PUNTO_FRAME_RECIPIENT_ISSUED ||
              status == PUNTO_FRAME_PRIVATE_REJECTED) &&
             dispatched_count != receipt->record_count))
            return 0;
        receipt->status = status;
        receipt->dispatched_count = dispatched_count;
        return 1;
    }
    return 0;
}
