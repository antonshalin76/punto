#ifndef PUNTO_RECIPIENT_PROTO_H
#define PUNTO_RECIPIENT_PROTO_H

#include <X11/Xmd.h>

#define PUNTO_RECIPIENT_NAME "PuntoRecipient"
#define PUNTO_RECIPIENT_MAJOR 1
#define PUNTO_RECIPIENT_MINOR 1

#define X_PuntoQueryVersion 0
#define X_PuntoRegister 1
#define X_PuntoUnregister 2
#define X_PuntoClaim 3

typedef struct {
    CARD8 reqType;
    CARD8 puntoReqType;
    CARD16 length;
} xPuntoQueryVersionReq;

typedef struct {
    CARD8 type;
    CARD8 status;
    CARD16 sequenceNumber;
    CARD32 length;
    CARD16 majorVersion;
    CARD16 minorVersion;
    CARD32 policyHash;
    CARD32 buildId;
    CARD32 serverNonceLo;
    CARD32 serverNonceHi;
    CARD32 pad2;
} xPuntoQueryVersionReply;

typedef struct {
    CARD8 reqType;
    CARD8 puntoReqType;
    CARD16 length;
} xPuntoRegisterReq;

typedef struct {
    CARD8 type;
    CARD8 status;
    CARD16 sequenceNumber;
    CARD32 length;
    CARD32 endpoint;
    CARD32 policyHash;
    CARD32 buildId;
    CARD32 pad0;
    CARD32 pad1;
    CARD32 pad2;
} xPuntoRegisterReply;

typedef struct {
    CARD8 reqType;
    CARD8 puntoReqType;
    CARD16 length;
    CARD32 endpoint;
} xPuntoUnregisterReq;

typedef struct {
    CARD8 reqType;
    CARD8 puntoReqType;
    CARD16 length;
    CARD32 endpoint;
    CARD32 serverNonceLo;
    CARD32 serverNonceHi;
    CARD32 occurrenceLo;
    CARD32 occurrenceHi;
} xPuntoClaimReq;

typedef struct {
    CARD8 type;
    CARD8 status;
    CARD16 sequenceNumber;
    CARD32 length;
    CARD32 endpoint;
    CARD32 routingWindow;
    CARD32 flags;
    CARD32 policyHash;
    CARD32 buildId;
    CARD32 pad;
} xPuntoClaimReply;

/* Server-to-one-client event, never selected via another client's window. */
typedef struct {
    CARD8 type;
    CARD8 detail;
    CARD16 sequenceNumber;
    CARD32 endpoint;
    CARD32 routingWindow;
    CARD32 serverNonceLo;
    CARD32 serverNonceHi;
    CARD32 occurrenceLo;
    CARD32 occurrenceHi;
    CARD32 flags;
} xPuntoRecipientEvent;

#define PUNTO_RECIPIENT_SHIFT_MASK 1U
#define PUNTO_RECIPIENT_ACTION_SHIFT 8U
#define PUNTO_RECIPIENT_PHYSICAL_SHIFT 16U

#endif
