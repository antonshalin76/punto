#include "punto/xorg_recipient.hpp"

#include "puntoRecipientProto.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::abort();
  }
}

} // namespace

int main() {
  auto *display = reinterpret_cast<Display *>(0x1000);
  require(punto::valid_recipient_event_base(80, 130),
          "extension event 80 and valid opcode are accepted");
  require(!punto::valid_recipient_event_base(35, 130),
          "core event cannot register as recipient extension");
  require(!punto::valid_recipient_event_base(128, 130),
          "event beyond seven-bit wire type is denied");
  require(!punto::valid_recipient_event_base(80, 127),
          "core opcode cannot register as recipient extension");
  xPuntoRecipientEvent wire{};
  wire.type = 80;
  wire.endpoint = 92;
  wire.routingWindow = 73;
  wire.serverNonceLo = 1001;
  wire.occurrenceLo = 1002;
  wire.flags = 1U << PUNTO_RECIPIENT_ACTION_SHIFT;
  XEvent event{};
  require(punto::decode_recipient_wire_event(wire, display, 42, &event),
          "server wire event decodes");
  require(event.xclient.serial == 42, "wire serial preserved");
  const auto decode = [&] {
    return punto::decode_recipient_notification(event, display, 80, 92);
  };
  require(decode().has_value(), "matching private event is admitted");
  require(!punto::decode_recipient_notification(event, display, 80, 93),
          "wrong endpoint is denied");
  wire.type |= 0x80U;
  require(punto::decode_recipient_wire_event(wire, display, 43, &event),
          "client-sent wire event decodes with provenance");
  require(!decode(), "client-sent X event cannot impersonate the server");
  wire.type = 80;
  require(punto::decode_recipient_wire_event(wire, display, 44, &event),
          "server provenance restores only from server wire");
  event.xclient.data.l[4] = 74;
  require(!decode(), "routing-window mismatch is denied");
  event.xclient.data.l[4] = 73;
  event.xclient.data.l[3] = 7L << PUNTO_RECIPIENT_ACTION_SHIFT;
  require(!decode(), "unknown action is denied");
  event.xclient.data.l[3] = 1L << 31;
  require(!decode(), "reserved flags are denied");
  event.xclient.data.l[3] = 1L << PUNTO_RECIPIENT_ACTION_SHIFT;
  event.xclient.data.l[1] = 0;
  require(!decode(), "zero server incarnation is denied");
  std::cout << "punto-xorg-recipient-notification: OK\n";
}
