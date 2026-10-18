#include "punto/xorg_input_transport.hpp"

#include "punto_ingress_stream.h"

#include <openssl/crypto.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace punto {
namespace {
using Clock = std::chrono::steady_clock;

bool wait_fd(int fd, short events, Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    pollfd descriptor{fd, events, 0};
    const auto timeout = static_cast<int>(std::max<std::int64_t>(
        1, std::min<std::int64_t>(remaining.count(),
                                  std::numeric_limits<int>::max())));
    const int result = poll(&descriptor, 1, timeout);
    if (result > 0)
      return (descriptor.revents & events) != 0;
    if (result < 0 && errno != EINTR)
      return false;
  }
  return false;
}

bool same_identity(const PuntoIngressControl &control, std::uint32_t policy_hash,
                   std::uint32_t build_id) {
  return control.policy_hash == policy_hash && control.build_id == build_id;
}
} // namespace

XorgInputTransport::XorgInputTransport(int fd,
                                       const PuntoIngressBootstrap &bootstrap)
    : fd_(fd), policy_hash_(bootstrap.policy_hash),
      build_id_(bootstrap.build_id) {
  std::memcpy(secret_.data(), bootstrap.secret, secret_.size());
}

XorgInputTransport::~XorgInputTransport() {
  if (fd_ >= 0) {
    if (healthy_ && epoch_ != 0) {
      PuntoIngressControl close{};
      close.kind = PUNTO_CONTROL_CLOSE;
      close.policy_hash = policy_hash_;
      close.build_id = build_id_;
      close.stream_epoch = epoch_;
      if (punto_ingress_control_sign(&close, secret_.data()))
        (void)send(fd_, &close, sizeof(close), MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    ::close(fd_);
  }
  OPENSSL_cleanse(secret_.data(), secret_.size());
}

std::unique_ptr<XorgInputTransport> XorgInputTransport::connect_authenticated(
    std::string_view socket_path, int bootstrap_fd, std::uint32_t policy_hash,
    std::uint32_t build_id, std::chrono::milliseconds timeout) {
  PuntoIngressBootstrap bootstrap{};
  if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path) ||
      timeout <= std::chrono::milliseconds::zero() || !policy_hash ||
      !build_id || !punto_ingress_bootstrap_read(bootstrap_fd, &bootstrap))
    return nullptr;
  const bool authorized = bootstrap.policy_hash == policy_hash &&
                          bootstrap.build_id == build_id &&
                          bootstrap.producer_uid == geteuid();
  if (!authorized) {
    OPENSSL_cleanse(&bootstrap, sizeof(bootstrap));
    return nullptr;
  }

  const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    OPENSSL_cleanse(&bootstrap, sizeof(bootstrap));
    return nullptr;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
  address.sun_path[socket_path.size()] = '\0';
  const auto deadline = Clock::now() + timeout;
  const int connected = connect(fd, reinterpret_cast<sockaddr *>(&address),
                                sizeof(address));
  if (connected != 0 &&
      (errno != EINPROGRESS || !wait_fd(fd, POLLOUT, deadline))) {
    ::close(fd);
    OPENSSL_cleanse(&bootstrap, sizeof(bootstrap));
    return nullptr;
  }
  int socket_error = 0;
  socklen_t error_size = sizeof(socket_error);
  ucred peer{};
  socklen_t peer_size = sizeof(peer);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0 ||
      socket_error != 0 ||
      getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
      peer_size != sizeof(peer) || peer.pid <= 0 ||
      peer.uid != bootstrap.server_uid) {
    ::close(fd);
    OPENSSL_cleanse(&bootstrap, sizeof(bootstrap));
    return nullptr;
  }
  auto transport = std::unique_ptr<XorgInputTransport>(
      new XorgInputTransport(fd, bootstrap));
  OPENSSL_cleanse(&bootstrap, sizeof(bootstrap));
  if (!transport->authenticate(timeout))
    return nullptr;
  return transport;
}

bool XorgInputTransport::send_bytes(const void *bytes, std::size_t count,
                                    Clock::time_point deadline) {
  while (wait_fd(fd_, POLLOUT, deadline)) {
    const ssize_t result = send(fd_, bytes, count, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (result == static_cast<ssize_t>(count))
      return true;
    if (result < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    return false;
  }
  return false;
}

XorgInputTransport::ControlRead XorgInputTransport::receive_control(
    PuntoIngressControl &control, Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    pollfd descriptor{fd_, POLLIN, 0};
    const int ready = poll(&descriptor, 1, static_cast<int>(
        std::max<std::int64_t>(1, std::min<std::int64_t>(
            remaining.count(), std::numeric_limits<int>::max()))));
    if (ready == 0)
      return ControlRead::Timeout;
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      return ControlRead::Invalid;
    }
    if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0 ||
        (descriptor.revents & POLLIN) == 0)
      return ControlRead::Invalid;
    const ssize_t result = recv(fd_, &control, sizeof(control),
                                MSG_DONTWAIT | MSG_TRUNC);
    if (result == static_cast<ssize_t>(sizeof(control)))
      return punto_ingress_control_verify(&control, sizeof(control),
                                          secret_.data()) &&
                     same_identity(control, policy_hash_, build_id_)
                 ? ControlRead::Valid
                 : ControlRead::Invalid;
    if (result < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    return ControlRead::Invalid;
  }
  return ControlRead::Timeout;
}

bool XorgInputTransport::send_control(PuntoIngressControl &control,
                                       Clock::time_point deadline) {
  return punto_ingress_control_sign(&control, secret_.data()) &&
         send_bytes(&control, sizeof(control), deadline);
}

bool XorgInputTransport::authenticate(std::chrono::milliseconds timeout) {
  PuntoIngressControl hello{};
  if (receive_control(hello, Clock::now() + timeout) != ControlRead::Valid ||
      hello.kind != PUNTO_CONTROL_HELLO || hello.stream_epoch == 0)
    return false;
  server_nonce_ = hello.stream_epoch;
  std::uint64_t nonce = 0;
  if (getrandom(&nonce, sizeof(nonce), 0) != sizeof(nonce) || !nonce)
    return false;
  PuntoIngressControl registration{};
  registration.kind = PUNTO_CONTROL_REGISTER;
  registration.policy_hash = policy_hash_;
  registration.build_id = build_id_;
  registration.sequence = nonce;
  std::memcpy(registration.challenge, hello.challenge,
              sizeof(registration.challenge));
  if (!send_control(registration, Clock::now() + timeout))
    return false;
  PuntoIngressControl ready{};
  if (receive_control(ready, Clock::now() + timeout) != ControlRead::Valid ||
      ready.kind != PUNTO_CONTROL_READY || ready.sequence != nonce ||
      std::memcmp(ready.challenge, hello.challenge,
                  sizeof(ready.challenge)) != 0)
    return false;
  epoch_ = ready.stream_epoch;
  healthy_ = true;
  return true;
}

bool XorgInputTransport::submit(const OrderedIngressPacket &packet,
                                 std::chrono::milliseconds timeout) {
  if (!healthy_ || timeout <= std::chrono::milliseconds::zero() ||
      packet.header.stream_epoch != epoch_ ||
      packet.header.sequence != next_sequence_ ||
      !verify_ordered_ingress_frame(packet))
    return false;
  std::vector<std::uint8_t> bytes(packet.header.byte_length);
  std::memcpy(bytes.data(), &packet.header, sizeof(packet.header));
  std::memcpy(bytes.data() + sizeof(packet.header), packet.records.data(),
              packet.records.size() * sizeof(PuntoIngressRecord));
  if (!send_bytes(bytes.data(), bytes.size(), Clock::now() + timeout)) {
    healthy_ = false;
    return false;
  }
  PuntoIngressControl result{};
  auto read = receive_control(result, Clock::now() + timeout);
  if (read == ControlRead::Valid && next_sequence_ > 1 &&
      result.kind == PUNTO_CONTROL_QUERY_RESULT &&
      result.stream_epoch == epoch_ &&
      result.sequence == next_sequence_ - 1) {
    // A query answer can follow a late original ACK. It belongs only to the
    // already committed frame and must not be mistaken for this frame's ACK.
    read = receive_control(result, Clock::now() + timeout);
  }
  if (read == ControlRead::Invalid) {
    healthy_ = false;
    return false;
  }
  if (read == ControlRead::Timeout) {
    PuntoIngressControl query{};
    query.kind = PUNTO_CONTROL_QUERY;
    query.policy_hash = policy_hash_;
    query.build_id = build_id_;
    query.stream_epoch = epoch_;
    query.sequence = next_sequence_;
    if (!send_control(query, Clock::now() + timeout) ||
        receive_control(result, Clock::now() + timeout) !=
            ControlRead::Valid) {
      healthy_ = false;
      return false;
    }
  }
  if ((result.kind != PUNTO_CONTROL_FRAME_RESULT &&
       result.kind != PUNTO_CONTROL_QUERY_RESULT) ||
      result.stream_epoch != epoch_ || result.sequence != next_sequence_ ||
      result.status < PUNTO_FRAME_ACCEPTED ||
      result.status >= PUNTO_FRAME_REJECTED) {
    healthy_ = false;
    return false;
  }
  ++next_sequence_;
  return true;
}

bool XorgInputTransport::terminal_failure() {
  if (!healthy_ || fd_ < 0)
    return true;
  pollfd descriptor{fd_, POLLIN, 0};
  const int ready = poll(&descriptor, 1, 0);
  if (ready < 0) {
    if (errno == EINTR)
      return false;
    healthy_ = false;
    return true;
  }
  if (ready == 0)
    return false;
  if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
    healthy_ = false;
    return true;
  }
  if (descriptor.revents & POLLIN) {
    PuntoIngressControl message{};
    const ssize_t length = recv(fd_, &message, sizeof(message),
                                MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC);
    if (length != sizeof(message) ||
        !punto_ingress_control_verify(&message, sizeof(message),
                                      secret_.data()) ||
        !same_identity(message, policy_hash_, build_id_) ||
        message.stream_epoch != epoch_ ||
        (message.kind == PUNTO_CONTROL_ERROR ||
         ((message.kind == PUNTO_CONTROL_FRAME_RESULT ||
           message.kind == PUNTO_CONTROL_QUERY_RESULT) &&
          message.status >= PUNTO_FRAME_REJECTED))) {
      healthy_ = false;
      return true;
    }
  }
  return false;
}

} // namespace punto
