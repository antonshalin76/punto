#include "punto/ordered_ingress_frame.hpp"

namespace punto {

OrderedIngressFrame::OrderedIngressFrame(std::uint64_t stream_epoch,
                                         std::uint64_t sequence)
    : stream_epoch_(stream_epoch), sequence_(sequence) {
  records_.reserve(PUNTO_INGRESS_MAX_RECORDS);
}

bool OrderedIngressFrame::advance(std::uint32_t source_index) {
  if (closed_ || records_.size() >= PUNTO_INGRESS_MAX_RECORDS ||
      (last_source_index_ && source_index <= *last_source_index_))
    return false;
  last_source_index_ = source_index;
  return true;
}

bool OrderedIngressFrame::append_input(const input_event &event,
                                       std::uint32_t source_index) {
  if (event.type == EV_MSC && event.code == MSC_SCAN) {
    // Scan metadata is not a native X key, but consumes a source index.
    return advance(source_index);
  }
  if (event.type == EV_KEY) {
    if (event.code == KEY_PAUSE || event.code >= KEY_CNT || event.value < 0 ||
        event.value > 2)
      return false;
  } else if (event.type != EV_SYN || event.code != SYN_REPORT ||
             event.value != 0) {
    return false;
  }
  if (!advance(source_index))
    return false;
  PuntoIngressRecord record{};
  record.source_index = source_index;
  record.kind = event.type == EV_SYN ? PUNTO_INGRESS_SYN : PUNTO_INGRESS_KEY;
  record.code = event.type == EV_SYN ? 0 : event.code;
  record.value = event.value;
  records_.push_back(record);
  if (event.type == EV_SYN)
    closed_ = true;
  return true;
}

bool OrderedIngressFrame::append_pause(std::uint32_t source_index, int value,
                                       std::uint32_t action,
                                       std::uint32_t physical_modifiers,
                                       std::uint64_t request_id,
                                       std::uint64_t deadline_monotonic_ns) {
  if ((value != 0 && value != 1 && value != 2) ||
      action < PUNTO_ACTION_LAYOUT_WORD ||
      action > PUNTO_ACTION_TRANSLIT_SELECTION ||
      (physical_modifiers & ~PUNTO_MOD_ALL) != 0 || request_id == 0 ||
      deadline_monotonic_ns == 0 || !advance(source_index))
    return false;
  PuntoIngressRecord record{};
  record.source_index = source_index;
  record.kind = value == 1   ? PUNTO_INGRESS_PAUSE_BEGIN
                : value == 2 ? PUNTO_INGRESS_PAUSE_REPEAT
                             : PUNTO_INGRESS_PAUSE_END;
  record.action = action;
  record.physical_modifiers = physical_modifiers;
  record.request_id = request_id;
  record.deadline_monotonic_ns = deadline_monotonic_ns;
  records_.push_back(record);
  return true;
}

std::optional<OrderedIngressPacket> OrderedIngressFrame::finish() {
  if (!closed_ || records_.empty() ||
      records_.back().kind != PUNTO_INGRESS_SYN || stream_epoch_ == 0 ||
      sequence_ == 0)
    return std::nullopt;
  OrderedIngressPacket packet;
  packet.records = std::move(records_);
  packet.header.magic = PUNTO_INGRESS_MAGIC;
  packet.header.version = PUNTO_INGRESS_VERSION;
  packet.header.kind = PUNTO_INGRESS_FRAME;
  packet.header.byte_length = static_cast<std::uint32_t>(
      sizeof(PuntoIngressFrameHeader) +
      packet.records.size() * sizeof(PuntoIngressRecord));
  packet.header.record_count =
      static_cast<std::uint32_t>(packet.records.size());
  packet.header.stream_epoch = stream_epoch_;
  packet.header.sequence = sequence_;
  punto_ingress_digest_records(
      reinterpret_cast<const std::uint8_t *>(packet.records.data()),
      packet.records.size() * sizeof(PuntoIngressRecord), packet.header.digest);
  if (!verify_ordered_ingress_frame(packet))
    return std::nullopt;
  return packet;
}

bool verify_ordered_ingress_frame(const OrderedIngressPacket &packet) {
  return punto_ingress_validate_frame(&packet.header, packet.records.data(),
                                      packet.records.size()) ==
         PUNTO_INGRESS_DECODE_OK;
}

} // namespace punto
