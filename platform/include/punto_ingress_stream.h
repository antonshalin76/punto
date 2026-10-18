#ifndef PUNTO_INGRESS_STREAM_H
#define PUNTO_INGRESS_STREAM_H

#include "punto_ingress_wire.h"

#define PUNTO_INGRESS_RECEIPT_CAPACITY 4096U
#define PUNTO_INGRESS_RECEIPT_NS UINT64_C(10000000000)

enum PuntoIngressFrameStatus {
    PUNTO_FRAME_ACCEPTED = 1,
    PUNTO_FRAME_BLOCKED = 2,
    PUNTO_FRAME_DISPATCHED = 3,
    PUNTO_FRAME_RECIPIENT_ISSUED = 4,
    PUNTO_FRAME_PRIVATE_REJECTED = 5,
    PUNTO_FRAME_REJECTED = 6,
    PUNTO_FRAME_FAILED_BEFORE = 7,
    PUNTO_FRAME_FAILED_PARTIAL = 8
};

enum PuntoIngressStreamResult {
    PUNTO_STREAM_NEW = 0,
    PUNTO_STREAM_DUPLICATE = 1,
    PUNTO_STREAM_BAD_FRAME = 2,
    PUNTO_STREAM_BAD_SEQUENCE = 3,
    PUNTO_STREAM_CONFLICT = 4,
    PUNTO_STREAM_BAD_LIFECYCLE = 5,
    PUNTO_STREAM_FULL = 6
};

typedef struct PuntoIngressPauseState {
    uint64_t request_id;
    uint64_t deadline_monotonic_ns;
    uint32_t action;
    uint32_t physical_modifiers;
    uint8_t active;
} PuntoIngressPauseState;

typedef struct PuntoIngressReceipt {
    uint64_t sequence;
    uint64_t received_monotonic_ns;
    uint8_t digest[PUNTO_INGRESS_DIGEST_BYTES];
    uint32_t record_count;
    uint32_t status;
    uint32_t dispatched_count;
} PuntoIngressReceipt;

typedef struct PuntoIngressStream {
    uint64_t epoch;
    uint64_t next_sequence;
    uint64_t last_request_id;
    PuntoIngressPauseState pause;
    PuntoIngressReceipt receipts[PUNTO_INGRESS_RECEIPT_CAPACITY];
    size_t receipt_count;
} PuntoIngressStream;

typedef struct PuntoIngressPreview {
    uint64_t sequence;
    uint64_t last_request_id_after;
    PuntoIngressPauseState pause_after;
    size_t receipt_slot;
    uint32_t record_count;
    uint32_t new_request_count;
    uint8_t digest[PUNTO_INGRESS_DIGEST_BYTES];
} PuntoIngressPreview;

#ifdef __cplusplus
extern "C" {
#endif

void punto_ingress_stream_init(PuntoIngressStream *stream, uint64_t epoch);
enum PuntoIngressStreamResult punto_ingress_stream_preview(
    const PuntoIngressStream *stream, const PuntoIngressFrameHeader *header,
    const PuntoIngressRecord *records, size_t count, uint64_t now_ns,
    PuntoIngressPreview *preview);
int punto_ingress_stream_commit(PuntoIngressStream *stream,
                                const PuntoIngressPreview *preview,
                                uint64_t now_ns);
int punto_ingress_stream_query(const PuntoIngressStream *stream,
                               uint64_t sequence, PuntoIngressReceipt *receipt);
int punto_ingress_stream_set_status(PuntoIngressStream *stream,
                                    uint64_t sequence, uint32_t status,
                                    uint32_t dispatched_count);

#ifdef __cplusplus
}
#endif

#endif
