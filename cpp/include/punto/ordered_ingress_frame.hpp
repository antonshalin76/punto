#pragma once

#include "punto_ingress_wire.h"

#include <linux/input.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace punto {

struct OrderedIngressPacket {
  PuntoIngressFrameHeader header{};
  std::vector<PuntoIngressRecord> records;
};

// A source frame is only publishable after SYN_REPORT. This class has no
// connection, editor or recipient authority.
class OrderedIngressFrame {
public:
  OrderedIngressFrame(std::uint64_t stream_epoch, std::uint64_t sequence);

  [[nodiscard]] bool append_input(const input_event &event,
                                  std::uint32_t source_index);
  [[nodiscard]] bool append_pause(std::uint32_t source_index, int value,
                                  std::uint32_t action,
                                  std::uint32_t physical_modifiers,
                                  std::uint64_t request_id,
                                  std::uint64_t deadline_monotonic_ns);
  [[nodiscard]] std::optional<OrderedIngressPacket> finish();

private:
  [[nodiscard]] bool advance(std::uint32_t source_index);

  std::uint64_t stream_epoch_;
  std::uint64_t sequence_;
  std::vector<PuntoIngressRecord> records_;
  std::optional<std::uint32_t> last_source_index_;
  bool closed_ = false;
};

[[nodiscard]] bool
verify_ordered_ingress_frame(const OrderedIngressPacket &packet);

} // namespace punto
