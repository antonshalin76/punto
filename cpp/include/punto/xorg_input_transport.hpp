#pragma once

#include "punto/ordered_ingress_frame.hpp"
#include "punto_ingress_control.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace punto {

// Owns exactly one authenticated producer stream; never writes raw input.
class XorgInputTransport {
public:
  static std::unique_ptr<XorgInputTransport> connect_authenticated(
      std::string_view socket_path, int bootstrap_fd, std::uint32_t policy_hash,
      std::uint32_t build_id, std::chrono::milliseconds timeout);

  ~XorgInputTransport();
  XorgInputTransport(const XorgInputTransport &) = delete;
  XorgInputTransport &operator=(const XorgInputTransport &) = delete;

  [[nodiscard]] std::uint64_t stream_epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::uint64_t server_nonce() const noexcept {
    return server_nonce_;
  }
  [[nodiscard]] bool submit(const OrderedIngressPacket &packet,
                            std::chrono::milliseconds timeout);
  [[nodiscard]] bool terminal_failure();

private:
  enum class ControlRead { Valid, Timeout, Invalid };
  XorgInputTransport(int fd, const PuntoIngressBootstrap &bootstrap);
  [[nodiscard]] bool authenticate(std::chrono::milliseconds timeout);
  [[nodiscard]] bool send_bytes(const void *bytes, std::size_t count,
                                std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] ControlRead receive_control(
      PuntoIngressControl &control,
      std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] bool send_control(
      PuntoIngressControl &control,
      std::chrono::steady_clock::time_point deadline);

  int fd_ = -1;
  std::array<std::uint8_t, PUNTO_INGRESS_SECRET_BYTES> secret_{};
  std::uint32_t policy_hash_ = 0;
  std::uint32_t build_id_ = 0;
  std::uint64_t epoch_ = 0;
  std::uint64_t server_nonce_ = 0;
  std::uint64_t next_sequence_ = 1;
  bool healthy_ = false;
};

} // namespace punto
