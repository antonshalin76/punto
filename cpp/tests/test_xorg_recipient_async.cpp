#include "punto/xorg_recipient.hpp"

#include "puntoRecipientProto.h"

#include <X11/Xlib-xcb.h>
#include <X11/Xlibint.h>
#include <xcb/xcbext.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using Clock = punto::XorgRecipient::Clock;
using State = punto::XorgRecipient::State;

enum class ReplyMode { Pending, Reply, Error };
ReplyMode reply_mode = ReplyMode::Pending;
std::array<unsigned char, 32> reply_bytes{};
std::uint64_t sequence = 10;
unsigned int unregister_count = 0;
unsigned int accepted_count = 0;
bool disconnected = false;
bool admission_busy = false;
int installed_event_type = 0;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "recipient async contract: %s\n", message);
    std::abort();
  }
}

template <typename Reply> void provide(const Reply &reply) {
  static_assert(sizeof(Reply) <= reply_bytes.size());
  std::memcpy(reply_bytes.data(), &reply, sizeof(reply));
  reply_mode = ReplyMode::Reply;
}

void reset_peer() {
  reply_mode = ReplyMode::Pending;
  unregister_count = 0;
  accepted_count = 0;
  disconnected = false;
  admission_busy = false;
  installed_event_type = 0;
}

void provide_extension() {
  xcb_query_extension_reply_t reply{};
  reply.present = 1;
  reply.major_opcode = 130;
  reply.first_event = 80;
  provide(reply);
}

void provide_version(std::uint32_t build_id = 0x87654321U) {
  xPuntoQueryVersionReply reply{};
  reply.majorVersion = PUNTO_RECIPIENT_MAJOR;
  reply.minorVersion = PUNTO_RECIPIENT_MINOR;
  reply.policyHash = 0x12345678U;
  reply.buildId = build_id;
  reply.serverNonceLo = 0xabcdef01U;
  reply.serverNonceHi = 0x23456789U;
  provide(reply);
}

void provide_registration() {
  xPuntoRegisterReply reply{};
  reply.endpoint = 92;
  reply.policyHash = 0x12345678U;
  reply.buildId = 0x87654321U;
  provide(reply);
}
} // namespace

extern "C" xcb_connection_t *__wrap_XGetXCBConnection(Display *) {
  return reinterpret_cast<xcb_connection_t *>(0x2000);
}

extern "C" int __wrap_xcb_connection_has_error(xcb_connection_t *) {
  return disconnected ? 1 : 0;
}

extern "C" int __wrap_XTryBoundedEnable_v3(Display *) {
  return XCB_BOUNDED_COMPLETE;
}

extern "C" int __wrap_XTryBoundedRequest_v1(
    Display *, const void *wire, std::size_t size, int flags, int isvoid,
    std::uint64_t *accepted) {
  const auto *bytes = static_cast<const std::uint8_t *>(wire);
  std::uint16_t wire_units = 0;
  std::memcpy(&wire_units, bytes + 2, sizeof(wire_units));
  require(size >= 4 && size == static_cast<std::size_t>(wire_units) * 4,
          "exact padded wire length is retained");
  require((bytes[0] == XCB_QUERY_EXTENSION && size == 24) ||
              (bytes[0] == 130 && size <= sizeof(xPuntoClaimReq)),
          "only query or registered Punto opcode uses bounded path");
  if (bytes[0] == XCB_QUERY_EXTENSION)
    require(std::memcmp(bytes + sizeof(xcb_query_extension_request_t),
                        PUNTO_RECIPIENT_NAME,
                        sizeof(PUNTO_RECIPIENT_NAME) - 1) == 0,
            "query targets only PuntoRecipient");
  if (admission_busy)
    return XCB_BOUNDED_BUSY;
  if (bytes[1] == X_PuntoUnregister)
    ++unregister_count;
  require(isvoid == (bytes[1] == X_PuntoUnregister) &&
              flags == (isvoid ? 0 : XCB_REQUEST_CHECKED),
          "reply and void flags match wire stage");
  *accepted = ++sequence;
  ++accepted_count;
  return XCB_BOUNDED_PROGRESS;
}

extern "C" int __wrap_xcb_bounded_poll_reply_v1(
    xcb_connection_t *, std::uint64_t, void **reply,
    xcb_generic_error_t **error) {
  *reply = nullptr;
  *error = nullptr;
  if (reply_mode == ReplyMode::Pending)
    return XCB_BOUNDED_INCOMPLETE;
  if (reply_mode == ReplyMode::Error) {
    *error = static_cast<xcb_generic_error_t *>(
        std::calloc(1, sizeof(xcb_generic_error_t)));
    (*error)->error_code = BadAccess;
  } else {
    *reply = std::malloc(reply_bytes.size());
    std::memcpy(*reply, reply_bytes.data(), reply_bytes.size());
  }
  reply_mode = ReplyMode::Pending;
  return XCB_BOUNDED_COMPLETE;
}

using WireConverter = Bool (*)(Display *, XEvent *, xEvent *);
extern "C" WireConverter __wrap_XESetWireToEvent(Display *, int event_type,
                                                  WireConverter) {
  installed_event_type = event_type;
  return nullptr;
}

extern "C" XExtCodes *__wrap_XInitExtension(Display *, const char *) {
  std::abort();
}

extern "C" Status __wrap__XReply(Display *, xReply *, int, Bool) {
  std::abort();
}

int main() {
  auto *display = reinterpret_cast<Display *>(0x1000);
  const auto now = Clock::now();
  reset_peer();
  punto::XorgRecipient no_reply;
  require(no_reply.start_registration(display, 0x12345678U, 0x87654321U,
                                      now + std::chrono::seconds(1)),
          "async extension query begins");
  require(no_reply.poll(now) == State::QueryPending,
          "no reply returns immediately");
  require(no_reply.poll(now + std::chrono::seconds(2)) == State::Expired,
          "one absolute deadline expires without reply");
  provide_extension();
  require(no_reply.poll(now + std::chrono::seconds(3)) == State::Expired,
          "late extension reply cannot revive registration");

  reset_peer();
  admission_busy = true;
  punto::XorgRecipient never_admitted;
  require(never_admitted.start_registration(
              display, 0x12345678U, 0x87654321U,
              now + std::chrono::seconds(1)),
          "busy pre-admission is retained locally");
  require(never_admitted.poll(now) == State::QueryPending &&
              accepted_count == 0,
          "busy retry does not emit bytes");
  admission_busy = false;
  require(never_admitted.poll(now + std::chrono::seconds(2)) ==
              State::Expired && accepted_count == 0,
          "deadline before admission cancels only local intent");

  reset_peer();
  punto::XorgRecipient recipient;
  require(recipient.start_registration(display, 0x12345678U, 0x87654321U,
                                       now + std::chrono::seconds(10)),
          "registration begins on the supplied display");
  provide_extension();
  admission_busy = true;
  require(recipient.poll(now) == State::QueryPending,
          "valid extension retains busy version request");
  require(accepted_count == 1,
          "busy version request does not mutate sequence");
  admission_busy = false;
  require(recipient.poll(now) == State::QueryPending && accepted_count == 2,
          "version request retries after contention");
  provide_version();
  require(recipient.poll(now) == State::RegisterPending,
          "version, policy, build and nonce verified");
  provide_registration();
  require(recipient.poll(now) == State::Ready && installed_event_type == 80,
          "server-assigned endpoint and event base 80 register");
  require(recipient.server_nonce() == 0x23456789abcdef01ULL,
          "server incarnation retained from version reply");

  punto::XorgNotification notification{92, 73, recipient.server_nonce(),
                                        1002, 1U << PUNTO_RECIPIENT_ACTION_SHIFT};
  require(recipient.start_claim(notification, now + std::chrono::seconds(1)),
          "claim request enqueued without a reply wait");
  require(recipient.poll(now) == State::ClaimPending,
          "missing claim reply returns immediately");
  xPuntoClaimReply claim{};
  claim.endpoint = 92;
  claim.routingWindow = 73;
  claim.flags = notification.flags;
  claim.policyHash = 0x12345678U;
  claim.buildId = 0x87654321U;
  provide(claim);
  require(recipient.poll(now) == State::Claimed,
          "matching server reply claims only the original occurrence");
  require(recipient.take_claim().has_value() && recipient.ready(),
          "claim consumed once and recipient returns ready");
  require(!recipient.take_claim(), "claim cannot be consumed twice");

  require(recipient.start_claim(notification, now + std::chrono::seconds(1)),
          "second claim request can be observed before completion");
  recipient.clear_claim();
  require(recipient.state() == State::Expired && !recipient.ready(),
          "unknown claim outcome cannot restore READY");
  provide(claim);
  require(recipient.poll(now) == State::Expired && !recipient.take_claim(),
          "late claim reply cannot revive expired occurrence");

  reset_peer();
  punto::XorgRecipient bad_version;
  require(bad_version.start_registration(display, 0x12345678U, 0x87654321U,
                                         now + std::chrono::seconds(10)),
          "negative version test begins");
  provide_extension();
  require(bad_version.poll(now) == State::QueryPending,
          "extension response accepted");
  provide_version(0x1U);
  require(bad_version.poll(now) == State::Rejected,
          "wrong build identity rejected before registration");

  reset_peer();
  punto::XorgRecipient late_registration;
  require(late_registration.start_registration(
              display, 0x12345678U, 0x87654321U,
              now + std::chrono::seconds(1)),
          "late registration test begins");
  provide_extension();
  require(late_registration.poll(now) == State::QueryPending,
          "extension accepted");
  provide_version();
  require(late_registration.poll(now) == State::RegisterPending,
          "register request enqueued");
  require(late_registration.poll(now + std::chrono::seconds(2)) ==
              State::Expired,
          "register deadline expired");
  provide_registration();
  admission_busy = true;
  require(late_registration.poll(now + std::chrono::seconds(3)) ==
              State::Expired &&
              unregister_count == 0,
          "late cleanup can be deferred without reviving admission");
  admission_busy = false;
  require(late_registration.poll(now + std::chrono::seconds(3)) ==
              State::Expired && unregister_count == 1,
          "deferred cleanup retries and never makes recipient ready");

  reset_peer();
  punto::XorgRecipient transport_error;
  require(transport_error.start_registration(
              display, 0x12345678U, 0x87654321U,
              now + std::chrono::seconds(1)),
          "transport error test begins");
  disconnected = true;
  require(transport_error.poll(now) == State::Unavailable,
          "disconnected X connection fails closed");
  transport_error.clear_claim();
  require(transport_error.state() == State::Unavailable &&
              !transport_error.ready(),
          "claim cleanup cannot revive a disconnected endpoint");
  std::puts("punto-xorg-recipient-async: OK");
}
