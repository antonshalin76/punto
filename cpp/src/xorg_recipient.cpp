#include "punto/xorg_recipient.hpp"

#include <X11/Xlibint.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xproto.h>
#include <xcb/xcbext.h>
#include <xcb/xproto.h>

#undef min
#undef max

#include "puntoRecipientProto.h"

#include <cstring>
#include <cstdlib>
#include <limits>
#include <utility>

namespace punto {
namespace {

static_assert(sizeof(xPuntoQueryVersionReq) == 4);
static_assert(sizeof(xPuntoQueryVersionReply) == 32);
static_assert(sizeof(xPuntoRegisterReq) == 4);
static_assert(sizeof(xPuntoUnregisterReq) == 8);
static_assert(sizeof(xPuntoRegisterReply) == 32);
static_assert(sizeof(xPuntoClaimReq) == 24);
static_assert(sizeof(xPuntoClaimReply) == 32);
static_assert(sizeof(xPuntoRecipientEvent) == 32);

Bool recipient_wire_to_event(Display *display, XEvent *event, xEvent *wire) {
  if (display == nullptr || event == nullptr || wire == nullptr) {
    return False;
  }
  xPuntoRecipientEvent message{};
  std::memcpy(&message, wire, sizeof(message));
  return decode_recipient_wire_event(
             message, display,
             _XSetLastRequestRead(display,
                                  reinterpret_cast<xGenericReply *>(wire)),
             event)
             ? True
             : False;
}

} // namespace

bool valid_recipient_event_base(int event_base, int major_opcode) {
  return event_base >= 64 && event_base <= 127 &&
         major_opcode >= 128 && major_opcode <= 255;
}

bool decode_recipient_wire_event(const xPuntoRecipientEvent &message,
                                 Display *display, unsigned long serial,
                                 XEvent *event) {
  if (display == nullptr || event == nullptr ||
      !valid_recipient_event_base(message.type & 0x7fU, 128) ||
      message.detail != 0) {
    return false;
  }
  std::memset(event, 0, sizeof(*event));
  event->xclient.type = message.type & 0x7fU;
  event->xclient.serial = serial;
  event->xclient.send_event = (message.type & 0x80U) != 0 ? True : False;
  event->xclient.display = display;
  event->xclient.window = message.routingWindow;
  event->xclient.format = 32;
  event->xclient.data.l[0] = static_cast<long>(message.endpoint);
  event->xclient.data.l[1] = static_cast<long>(
      (static_cast<std::uint64_t>(message.serverNonceHi) << 32) |
      message.serverNonceLo);
  event->xclient.data.l[2] = static_cast<long>(
      (static_cast<std::uint64_t>(message.occurrenceHi) << 32) |
      message.occurrenceLo);
  event->xclient.data.l[3] = static_cast<long>(message.flags);
  event->xclient.data.l[4] = static_cast<long>(message.routingWindow);
  return true;
}

bool XorgRecipient::start_registration(Display *display,
                                       std::uint32_t policy_hash,
                                       std::uint32_t build_id,
                                       Clock::time_point deadline) {
  if (state_ != State::Unavailable || display == nullptr || !policy_hash ||
      !build_id || sizeof(long) < sizeof(std::uint64_t) ||
      deadline <= Clock::now())
    return false;
  xcb_connection_t *connection = XGetXCBConnection(display);
  if (connection == nullptr || xcb_connection_has_error(connection) ||
      XTryBoundedEnable_v3(display) != XCB_BOUNDED_COMPLETE)
    return false;
  constexpr std::size_t name_size = sizeof(PUNTO_RECIPIENT_NAME) - 1;
  constexpr std::size_t query_size = sizeof(xcb_query_extension_request_t) +
      ((name_size + 3U) & ~std::size_t{3});
  std::array<std::uint8_t, query_size> query{};
  query[0] = XCB_QUERY_EXTENSION;
  const auto wire_name_size = static_cast<std::uint16_t>(name_size);
  std::memcpy(query.data() + 4, &wire_name_size, sizeof(wire_name_size));
  std::memcpy(query.data() + sizeof(xcb_query_extension_request_t),
              PUNTO_RECIPIENT_NAME, name_size);
  display_ = display;
  connection_ = connection;
  policy_hash_ = policy_hash;
  build_id_ = build_id;
  deadline_ = deadline;
  query_step_ = QueryStep::Extension;
  state_ = State::QueryPending;
  if (send_request(query.data(), query.size(), true) == Submit::Rejected) {
    state_ = State::Unavailable;
    display_ = nullptr;
    connection_ = nullptr;
    return false;
  }
  return true;
}

XorgRecipient::Submit XorgRecipient::send_request(
    const void *request, std::size_t size, bool has_reply) {
  if (display_ == nullptr || connection_ == nullptr || request == nullptr ||
      size < 4 || size > deferred_request_.size() || (size & 3U) ||
      deferred_size_ || pending_sequence_)
    return Submit::Rejected;
  std::memcpy(deferred_request_.data(), request, size);
  const auto units = static_cast<std::uint16_t>(size / 4);
  std::memcpy(deferred_request_.data() + 2, &units, sizeof(units));
  deferred_size_ = size;
  deferred_has_reply_ = has_reply;
  return retry_request();
}

XorgRecipient::Submit XorgRecipient::retry_request() {
  if (!deferred_size_)
    return Submit::Rejected;
  std::uint64_t sequence = 0;
  const int status = XTryBoundedRequest_v1(
      display_, deferred_request_.data(), deferred_size_,
      deferred_has_reply_ ? XCB_REQUEST_CHECKED : 0,
      deferred_has_reply_ ? 0 : 1, &sequence);
  if (status == XCB_BOUNDED_BUSY)
    return Submit::Busy;
  if (status != XCB_BOUNDED_PROGRESS || !sequence) {
    deferred_size_ = 0;
    return Submit::Rejected;
  }
  pending_sequence_ = deferred_has_reply_ ? sequence : 0;
  deferred_size_ = 0;
  return Submit::Accepted;
}

void XorgRecipient::expire() {
  late_register_reply_ = state_ == State::RegisterPending && pending_sequence_;
  state_ = State::Expired;
  claim_result_.reset();
  deferred_size_ = 0;
}

XorgRecipient::State XorgRecipient::poll(Clock::time_point now) {
  if (connection_ == nullptr)
    return state_;
  if (xcb_connection_has_error(connection_)) {
    state_ = State::Unavailable;
    pending_sequence_ = 0;
    return state_;
  }
  const bool pending = state_ == State::QueryPending ||
                       state_ == State::RegisterPending ||
                       state_ == State::ClaimPending;
  if (pending && now >= deadline_)
    expire();
  if (deferred_size_ && retry_request() == Submit::Rejected &&
      state_ != State::Expired)
    state_ = State::Rejected;
  if (!pending_sequence_)
    return state_;
  void *raw = nullptr;
  xcb_generic_error_t *error = nullptr;
  const int reply_status = xcb_bounded_poll_reply_v1(
      connection_, pending_sequence_, &raw, &error);
  if (reply_status == XCB_BOUNDED_BUSY ||
      reply_status == XCB_BOUNDED_INCOMPLETE)
    return state_;
  if (reply_status != XCB_BOUNDED_COMPLETE) {
    state_ = State::Rejected;
    pending_sequence_ = 0;
    return state_;
  }
  pending_sequence_ = 0;
  const bool valid_reply = raw != nullptr && error == nullptr;
  std::free(error);
  if (state_ == State::Expired) {
    if (valid_reply && late_register_reply_) {
      // A late registration response can only be cleaned up, never admitted.
      const auto *reply = static_cast<const xPuntoRegisterReply *>(raw);
      if (reply->endpoint && reply->policyHash == policy_hash_ &&
          reply->buildId == build_id_) {
        xPuntoUnregisterReq cleanup{};
        cleanup.reqType = static_cast<CARD8>(opcode_);
        cleanup.puntoReqType = X_PuntoUnregister;
        cleanup.endpoint = reply->endpoint;
        (void)send_request(&cleanup, sizeof(cleanup), false);
      }
    }
    late_register_reply_ = false;
    std::free(raw);
    return state_;
  }
  if (!valid_reply) {
    std::free(raw);
    state_ = State::Rejected;
    return state_;
  }
  if (state_ == State::QueryPending && query_step_ == QueryStep::Extension) {
    const auto *reply = static_cast<const xcb_query_extension_reply_t *>(raw);
    const bool extension_ok = reply->present &&
        valid_recipient_event_base(reply->first_event, reply->major_opcode);
    if (extension_ok) {
      event_type_ = reply->first_event;
      opcode_ = reply->major_opcode;
      xPuntoQueryVersionReq query{};
      query.reqType = static_cast<CARD8>(opcode_);
      query.puntoReqType = X_PuntoQueryVersion;
      query_step_ = QueryStep::Version;
      const auto submit = send_request(&query, sizeof(query), true);
      if (submit == Submit::Rejected)
        state_ = State::Rejected;
    }
    if (!extension_ok)
      state_ = State::Rejected;
  } else if (state_ == State::QueryPending) {
    const auto *reply = static_cast<const xPuntoQueryVersionReply *>(raw);
    server_nonce_ = (static_cast<std::uint64_t>(reply->serverNonceHi) << 32) |
                    reply->serverNonceLo;
    if (reply->majorVersion != PUNTO_RECIPIENT_MAJOR ||
        reply->minorVersion != PUNTO_RECIPIENT_MINOR ||
        reply->policyHash != policy_hash_ || reply->buildId != build_id_ ||
        !server_nonce_) {
      state_ = State::Rejected;
    } else {
      xPuntoRegisterReq registration{};
      registration.reqType = static_cast<CARD8>(opcode_);
      registration.puntoReqType = X_PuntoRegister;
      state_ = send_request(&registration, sizeof(registration), true) ==
                       Submit::Rejected ? State::Rejected : State::RegisterPending;
    }
  } else if (state_ == State::RegisterPending) {
    const auto *reply = static_cast<const xPuntoRegisterReply *>(raw);
    if (reply->endpoint == 0 || reply->policyHash != policy_hash_ ||
        reply->buildId != build_id_) {
      state_ = State::Rejected;
    } else {
      endpoint_ = reply->endpoint;
      XESetWireToEvent(display_, event_type_, recipient_wire_to_event);
      state_ = State::Ready;
    }
  } else if (state_ == State::ClaimPending) {
    const auto *reply = static_cast<const xPuntoClaimReply *>(raw);
    if (reply->endpoint == endpoint_ &&
        reply->routingWindow == claim_notification_.routing_window &&
        reply->flags == claim_notification_.flags &&
        reply->policyHash == policy_hash_ && reply->buildId == build_id_) {
      claim_result_ = XorgClaim{reply->routingWindow, reply->flags};
      state_ = State::Claimed;
    } else {
      state_ = State::Rejected;
    }
  }
  std::free(raw);
  return state_;
}

std::optional<XorgNotification>
XorgRecipient::notification(const XEvent &event) const {
  auto result = decode_recipient_notification(event, display_, event_type_,
                                               endpoint_);
  if (!result || result->server_nonce != server_nonce_)
    return std::nullopt;
  return result;
}

std::optional<XorgNotification> decode_recipient_notification(
    const XEvent &event, Display *display, int event_type,
    std::uint32_t endpoint) {
  if (display == nullptr || endpoint == 0 || event_type <= 0 ||
      event.type != event_type || event.xclient.send_event != False ||
      event.xclient.display != display || event.xclient.format != 32 ||
      event.xclient.window == 0 ||
      event.xclient.window > std::numeric_limits<std::uint32_t>::max() ||
      event.xclient.data.l[0] != static_cast<long>(endpoint) ||
      event.xclient.data.l[4] != static_cast<long>(event.xclient.window) ||
      event.xclient.data.l[1] == 0 || event.xclient.data.l[2] == 0 ||
      event.xclient.data.l[3] < 0 ||
      static_cast<unsigned long>(event.xclient.data.l[3]) >
          std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  const auto flags = static_cast<std::uint32_t>(event.xclient.data.l[3]);
  constexpr std::uint32_t allowed = PUNTO_RECIPIENT_SHIFT_MASK |
      (0xffU << PUNTO_RECIPIENT_ACTION_SHIFT) |
      (0xffU << PUNTO_RECIPIENT_PHYSICAL_SHIFT);
  if (flags & ~allowed) {
    return std::nullopt;
  }
  const auto action =
      flags >> PUNTO_RECIPIENT_ACTION_SHIFT & 0xffU;
  if (action < 1 || action > 5) {
    return std::nullopt;
  }
  return XorgNotification{
      endpoint, static_cast<std::uint32_t>(event.xclient.window),
      static_cast<std::uint64_t>(event.xclient.data.l[1]),
      static_cast<std::uint64_t>(event.xclient.data.l[2]),
      flags};
}

bool XorgRecipient::start_claim(const XorgNotification &notification,
                                Clock::time_point deadline) {
  if (!ready() || deadline <= Clock::now() ||
      notification.endpoint != endpoint_ ||
      notification.server_nonce != server_nonce_ ||
      !notification.occurrence)
    return false;
  xPuntoClaimReq request{};
  request.reqType = static_cast<CARD8>(opcode_);
  request.puntoReqType = X_PuntoClaim;
  request.endpoint = endpoint_;
  request.serverNonceLo = static_cast<CARD32>(notification.server_nonce);
  request.serverNonceHi = static_cast<CARD32>(notification.server_nonce >> 32);
  request.occurrenceLo = static_cast<CARD32>(notification.occurrence);
  request.occurrenceHi = static_cast<CARD32>(notification.occurrence >> 32);
  state_ = State::ClaimPending;
  if (send_request(&request, sizeof(request), true) == Submit::Rejected) {
    state_ = State::Rejected;
    return false;
  }
  claim_notification_ = notification;
  deadline_ = deadline;
  return true;
}

std::optional<XorgClaim> XorgRecipient::take_claim() {
  if (state_ != State::Claimed)
    return std::nullopt;
  auto result = std::exchange(claim_result_, std::nullopt);
  state_ = State::Ready;
  return result;
}

void XorgRecipient::clear_claim() {
  if (state_ != State::ClaimPending && state_ != State::Claimed)
    return;
  // The accepted request and any late reply still belong to the shared XCB
  // stream; only the local claim intent is cancelled.
  expire();
}

} // namespace punto
