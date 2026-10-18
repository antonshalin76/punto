#ifndef PUNTO_INGRESS_WIRE_H
#define PUNTO_INGRESS_WIRE_H

#include <stdint.h>
#include <stddef.h>

/* This local ABI is deliberately limited to the pinned Ubuntu x86_64 build. */
#if !defined(__x86_64__) || !defined(__BYTE_ORDER__) || \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Punto ingress v1 requires little-endian x86_64
#endif

/* Ubuntu noble/x86_64 local SOCK_SEQPACKET ABI. Never expose raw input_event. */
#define PUNTO_INGRESS_MAGIC UINT32_C(0x50554e31)
#define PUNTO_INGRESS_VERSION UINT16_C(1)
#define PUNTO_INGRESS_MAX_RECORDS 128U
#define PUNTO_INGRESS_DIGEST_BYTES 32U
#define PUNTO_INGRESS_FRAME_MAX_BYTES \
    (64U + PUNTO_INGRESS_MAX_RECORDS * 40U)

enum PuntoIngressAction {
    PUNTO_ACTION_LAYOUT_WORD = 1,
    PUNTO_ACTION_LAYOUT_SELECTION = 2,
    PUNTO_ACTION_CASE_WORD = 3,
    PUNTO_ACTION_CASE_SELECTION = 4,
    PUNTO_ACTION_TRANSLIT_SELECTION = 5
};

enum PuntoIngressPhysicalModifier {
    PUNTO_MOD_LEFT_SHIFT = 1U << 0,
    PUNTO_MOD_RIGHT_SHIFT = 1U << 1,
    PUNTO_MOD_LEFT_CTRL = 1U << 2,
    PUNTO_MOD_RIGHT_CTRL = 1U << 3,
    PUNTO_MOD_LEFT_ALT = 1U << 4,
    PUNTO_MOD_RIGHT_ALT = 1U << 5,
    PUNTO_MOD_LEFT_META = 1U << 6,
    PUNTO_MOD_RIGHT_META = 1U << 7
};
#define PUNTO_MOD_ALL UINT32_C(0xff)

enum PuntoIngressMessageKind {
    PUNTO_INGRESS_FRAME = 3
};

enum PuntoIngressRecordKind {
    PUNTO_INGRESS_KEY = 1,
    PUNTO_INGRESS_SYN = 2,
    PUNTO_INGRESS_PAUSE_BEGIN = 3,
    PUNTO_INGRESS_PAUSE_REPEAT = 4,
    PUNTO_INGRESS_PAUSE_END = 5
};

typedef struct PuntoIngressRecord {
    uint32_t source_index;
    uint16_t kind;
    uint16_t code;
    int32_t value;
    uint32_t action;
    uint32_t physical_modifiers;
    uint32_t reserved;
    uint64_t request_id;
    uint64_t deadline_monotonic_ns;
} PuntoIngressRecord;

typedef struct PuntoIngressFrameHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t byte_length;
    uint32_t record_count;
    uint64_t stream_epoch;
    uint64_t sequence;
    uint8_t digest[PUNTO_INGRESS_DIGEST_BYTES];
} PuntoIngressFrameHeader;

#if defined(__cplusplus)
static_assert(sizeof(PuntoIngressRecord) == 40);
static_assert(sizeof(PuntoIngressFrameHeader) == 64);
static_assert(offsetof(PuntoIngressRecord, request_id) == 24);
static_assert(offsetof(PuntoIngressRecord, deadline_monotonic_ns) == 32);
static_assert(offsetof(PuntoIngressFrameHeader, digest) == 32);
#else
_Static_assert(sizeof(PuntoIngressRecord) == 40, "ingress record ABI");
_Static_assert(sizeof(PuntoIngressFrameHeader) == 64, "ingress frame ABI");
_Static_assert(offsetof(PuntoIngressRecord, request_id) == 24, "request ABI");
_Static_assert(offsetof(PuntoIngressRecord, deadline_monotonic_ns) == 32, "deadline ABI");
_Static_assert(offsetof(PuntoIngressFrameHeader, digest) == 32, "digest ABI");
#endif

enum PuntoIngressDecodeResult {
    PUNTO_INGRESS_DECODE_OK = 0,
    PUNTO_INGRESS_DECODE_LENGTH = 1,
    PUNTO_INGRESS_DECODE_HEADER = 2,
    PUNTO_INGRESS_DECODE_RECORD = 3,
    PUNTO_INGRESS_DECODE_DIGEST = 4
};

#ifdef __cplusplus
extern "C" {
#endif

/* Structure bytes are the v1 wire layout on the pinned target. The digest
 * covers only record bytes; stream/header identity is checked separately. */
void punto_ingress_digest_records(const uint8_t *records, size_t length,
                                  uint8_t digest[PUNTO_INGRESS_DIGEST_BYTES]);
enum PuntoIngressDecodeResult punto_ingress_validate_frame(
    const PuntoIngressFrameHeader *header, const PuntoIngressRecord *records,
    size_t count);
enum PuntoIngressDecodeResult punto_ingress_decode_frame(
    const uint8_t *packet, size_t length, PuntoIngressFrameHeader *header,
    PuntoIngressRecord *records, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
