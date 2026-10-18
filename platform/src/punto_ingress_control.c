#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "punto_ingress_control.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
punto_ingress_bootstrap_valid(const PuntoIngressBootstrap *bootstrap)
{
    uint8_t zero[PUNTO_INGRESS_SECRET_BYTES] = {0};

    return bootstrap && bootstrap->magic == PUNTO_INGRESS_BOOTSTRAP_MAGIC &&
           bootstrap->policy_hash && bootstrap->build_id &&
           CRYPTO_memcmp(bootstrap->secret, zero, sizeof(zero)) != 0;
}

int
punto_ingress_bootstrap_read(int fd, PuntoIngressBootstrap *bootstrap)
{
    PuntoIngressBootstrap candidate;
    struct stat metadata;
    size_t offset = 0;
    int seals;

    if (fd < 0 || !bootstrap)
        return 0;
    memset(bootstrap, 0, sizeof(*bootstrap));
    seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 ||
        (seals & (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) !=
            (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) ||
        fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size != sizeof(candidate))
        return 0;
    while (offset < sizeof(candidate)) {
        ssize_t count = pread(fd, (uint8_t *)&candidate + offset,
                              sizeof(candidate) - offset, (off_t)offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            OPENSSL_cleanse(&candidate, sizeof(candidate));
            return 0;
        }
        offset += (size_t)count;
    }
    if (!punto_ingress_bootstrap_valid(&candidate)) {
        OPENSSL_cleanse(&candidate, sizeof(candidate));
        return 0;
    }
    *bootstrap = candidate;
    OPENSSL_cleanse(&candidate, sizeof(candidate));
    return 1;
}

static int
punto_ingress_control_shape(const PuntoIngressControl *control)
{
    uint8_t zero[PUNTO_INGRESS_SECRET_BYTES] = {0};
    int challenge_present;

    if (!control || control->magic != PUNTO_INGRESS_MAGIC ||
        control->version != PUNTO_INGRESS_VERSION ||
        control->byte_length != sizeof(*control) ||
        !control->policy_hash || !control->build_id)
        return 0;
    challenge_present = CRYPTO_memcmp(control->challenge, zero,
                                      sizeof(zero)) != 0;
    switch (control->kind) {
    case PUNTO_CONTROL_HELLO:
        return control->stream_epoch && !control->sequence &&
               !control->status && challenge_present;
    case PUNTO_CONTROL_REGISTER:
        return !control->stream_epoch && control->sequence &&
               !control->status && challenge_present;
    case PUNTO_CONTROL_READY:
        return control->stream_epoch && control->sequence &&
               !control->status && challenge_present;
    case PUNTO_CONTROL_FRAME_RESULT:
    case PUNTO_CONTROL_QUERY_RESULT:
        return control->stream_epoch && control->sequence &&
               control->status && !challenge_present;
    case PUNTO_CONTROL_QUERY:
        return control->stream_epoch && control->sequence &&
               !control->status && !challenge_present;
    case PUNTO_CONTROL_CLOSE:
        return control->stream_epoch && !control->sequence &&
               !control->status && !challenge_present;
    case PUNTO_CONTROL_ERROR:
        return control->status && !challenge_present;
    default:
        return 0;
    }
}

static int
punto_ingress_control_proof(
    const PuntoIngressControl *control,
    const uint8_t secret[PUNTO_INGRESS_SECRET_BYTES],
    uint8_t proof[PUNTO_INGRESS_SECRET_BYTES])
{
    unsigned int proof_length = 0;

    return HMAC(EVP_sha256(), secret, PUNTO_INGRESS_SECRET_BYTES,
                (const unsigned char *)control,
                offsetof(PuntoIngressControl, proof), proof,
                &proof_length) != NULL &&
           proof_length == PUNTO_INGRESS_SECRET_BYTES;
}

int
punto_ingress_control_sign(
    PuntoIngressControl *control,
    const uint8_t secret[PUNTO_INGRESS_SECRET_BYTES])
{
    if (!control || !secret)
        return 0;
    control->magic = PUNTO_INGRESS_MAGIC;
    control->version = PUNTO_INGRESS_VERSION;
    control->byte_length = sizeof(*control);
    if (!punto_ingress_control_shape(control))
        return 0;
    return punto_ingress_control_proof(control, secret, control->proof);
}

int
punto_ingress_control_verify(
    const PuntoIngressControl *control, size_t length,
    const uint8_t secret[PUNTO_INGRESS_SECRET_BYTES])
{
    uint8_t expected[PUNTO_INGRESS_SECRET_BYTES];

    if (length != sizeof(*control) || !secret ||
        !punto_ingress_control_shape(control) ||
        !punto_ingress_control_proof(control, secret, expected))
        return 0;
    return CRYPTO_memcmp(expected, control->proof, sizeof(expected)) == 0;
}
