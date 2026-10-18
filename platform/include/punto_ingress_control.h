#ifndef PUNTO_INGRESS_CONTROL_H
#define PUNTO_INGRESS_CONTROL_H

#include "punto_ingress_wire.h"

#define PUNTO_INGRESS_BOOTSTRAP_MAGIC UINT32_C(0x50554231)
#define PUNTO_INGRESS_SECRET_BYTES 32U

enum PuntoIngressControlKind {
    PUNTO_CONTROL_HELLO = 1,
    PUNTO_CONTROL_REGISTER = 2,
    PUNTO_CONTROL_READY = 4,
    PUNTO_CONTROL_FRAME_RESULT = 5,
    PUNTO_CONTROL_QUERY = 6,
    PUNTO_CONTROL_QUERY_RESULT = 7,
    PUNTO_CONTROL_CLOSE = 8,
    PUNTO_CONTROL_ERROR = 9
};

/* Read from a private, inherited and sealed descriptor by both peers. */
typedef struct PuntoIngressBootstrap {
    uint32_t magic;
    uint32_t policy_hash;
    uint32_t build_id;
    uint32_t producer_uid;
    uint32_t server_uid;
    uint8_t secret[PUNTO_INGRESS_SECRET_BYTES];
} PuntoIngressBootstrap;

/* Fixed little-endian x86_64 SOCK_SEQPACKET control ABI. */
typedef struct PuntoIngressControl {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t byte_length;
    uint32_t policy_hash;
    uint32_t build_id;
    uint32_t status;
    uint64_t stream_epoch; /* HELLO: server nonce; READY onward: stream epoch */
    uint64_t sequence;
    uint8_t challenge[PUNTO_INGRESS_SECRET_BYTES];
    uint8_t proof[PUNTO_INGRESS_SECRET_BYTES];
} PuntoIngressControl;

#if defined(__cplusplus)
static_assert(sizeof(PuntoIngressBootstrap) == 52);
static_assert(sizeof(PuntoIngressControl) == 104);
static_assert(offsetof(PuntoIngressControl, proof) == 72);
#else
_Static_assert(sizeof(PuntoIngressBootstrap) == 52, "bootstrap ABI");
_Static_assert(sizeof(PuntoIngressControl) == 104, "control ABI");
_Static_assert(offsetof(PuntoIngressControl, proof) == 72, "proof ABI");
#endif

#ifdef __cplusplus
extern "C" {
#endif

int punto_ingress_bootstrap_valid(const PuntoIngressBootstrap *bootstrap);
int punto_ingress_bootstrap_read(int fd, PuntoIngressBootstrap *bootstrap);
int punto_ingress_control_sign(
    PuntoIngressControl *control,
    const uint8_t secret[PUNTO_INGRESS_SECRET_BYTES]);
int punto_ingress_control_verify(
    const PuntoIngressControl *control, size_t length,
    const uint8_t secret[PUNTO_INGRESS_SECRET_BYTES]);

#ifdef __cplusplus
}
#endif

#endif
