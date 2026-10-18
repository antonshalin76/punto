#pragma once

#include <X11/Xlib.h>
#include <xcb/xcb.h>

#include "puntoRecipientProto.h"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace punto {

struct XorgNotification {
  std::uint32_t endpoint = 0;
  std::uint32_t routing_window = 0;
  std::uint64_t server_nonce = 0;
  std::uint64_t occurrence = 0;
  std::uint32_t flags = 0;
};

struct XorgClaim {
  std::uint32_t routing_window = 0;
  std::uint32_t flags = 0;
};

bool valid_recipient_event_base(int event_base, int major_opcode);
bool decode_recipient_wire_event(const xPuntoRecipientEvent &wire,
                                 Display *display, unsigned long serial,
                                 XEvent *event);

std::optional<XorgNotification> decode_recipient_notification(
    const XEvent &event, Display *display, int event_type,
    std::uint32_t endpoint);

class XorgRecipient {
public:
  using Clock = std::chrono::steady_clock;
  enum class State {
    Unavailable,
    QueryPending,
    RegisterPending,
    Ready,
    ClaimPending,
    Claimed,
    Rejected,
    Expired,
  };

  bool start_registration(Display *display, std::uint32_t policy_hash,
                          std::uint32_t build_id, Clock::time_point deadline);
  State poll(Clock::time_point now);
  bool start_claim(const XorgNotification &notification,
                   Clock::time_point deadline);
  std::optional<XorgClaim> take_claim();
  void clear_claim();
  std::optional<XorgNotification> notification(const XEvent &event) const;

  bool ready() const { return state_ == State::Ready; }
  State state() const { return state_; }
  int event_type() const { return event_type_; }
  std::uint64_t server_nonce() const { return server_nonce_; }

private:
  enum class QueryStep { Extension, Version };
  enum class Submit { Accepted, Busy, Rejected };
  Submit send_request(const void *request, std::size_t size, bool has_reply);
  Submit retry_request();
  void expire();
  Display *display_ = nullptr;
  xcb_connection_t *connection_ = nullptr;
  int event_type_ = 0;
  int opcode_ = 0;
  std::uint32_t endpoint_ = 0;
  std::uint32_t policy_hash_ = 0;
  std::uint32_t build_id_ = 0;
  std::uint64_t server_nonce_ = 0;
  std::uint64_t pending_sequence_ = 0;
  std::array<std::uint8_t, 64> deferred_request_{};
  std::size_t deferred_size_ = 0;
  bool deferred_has_reply_ = false;
  Clock::time_point deadline_{};
  State state_ = State::Unavailable;
  QueryStep query_step_ = QueryStep::Extension;
  XorgNotification claim_notification_{};
  std::optional<XorgClaim> claim_result_;
  bool late_register_reply_ = false;
};

} // namespace punto
