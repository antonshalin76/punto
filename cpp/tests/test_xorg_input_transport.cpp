#include "punto/xorg_input_transport.hpp"
#include "punto_ingress_stream.h"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <thread>

namespace {
void require(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) {
    std::fprintf(stderr, "transport contract failure at %s:%u\n",
                 at.file_name(), at.line());
    std::abort();
  }
}
} // namespace

int main() {
  char directory[] = "/tmp/punto-ingress-contract-XXXXXX";
  require(mkdtemp(directory) != nullptr);
  const std::string path = std::string(directory) + "/input.sock";
  const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  require(listener >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  require(path.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  require(bind(listener, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == 0);
  require(listen(listener, 1) == 0);

  PuntoIngressBootstrap bootstrap{};
  bootstrap.magic = PUNTO_INGRESS_BOOTSTRAP_MAGIC;
  bootstrap.policy_hash = 0x12345678;
  bootstrap.build_id = 0x9abcdef0;
  bootstrap.producer_uid = getuid();
  bootstrap.server_uid = getuid();
  std::memset(bootstrap.secret, 0x5a, sizeof(bootstrap.secret));
  const int bootstrap_fd = memfd_create("punto-contract", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  require(bootstrap_fd >= 0);
  require(write(bootstrap_fd, &bootstrap, sizeof(bootstrap)) ==
          sizeof(bootstrap));
  require(fcntl(bootstrap_fd, F_ADD_SEALS,
                F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0);

  std::thread server([&] {
    const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    require(client >= 0);
    PuntoIngressControl hello{};
    hello.kind = PUNTO_CONTROL_HELLO;
    hello.policy_hash = bootstrap.policy_hash;
    hello.build_id = bootstrap.build_id;
    hello.challenge[0] = 7;
    require(!punto_ingress_control_sign(&hello, bootstrap.secret));
    hello.stream_epoch = 0x98abcdef12345678ULL;
    require(punto_ingress_control_sign(&hello, bootstrap.secret));
    require(send(client, &hello, sizeof(hello), 0) == sizeof(hello));
    PuntoIngressControl registration{};
    require(recv(client, &registration, sizeof(registration), 0) ==
            sizeof(registration));
    require(registration.kind == PUNTO_CONTROL_REGISTER);
    require(punto_ingress_control_verify(&registration, sizeof(registration),
                                        bootstrap.secret));
    require(std::memcmp(registration.challenge, hello.challenge,
                        sizeof(hello.challenge)) == 0);
    PuntoIngressControl ready{};
    ready.kind = PUNTO_CONTROL_READY;
    ready.policy_hash = bootstrap.policy_hash;
    ready.build_id = bootstrap.build_id;
    ready.stream_epoch = 0x1234;
    ready.sequence = registration.sequence;
    std::memcpy(ready.challenge, hello.challenge, sizeof(hello.challenge));
    require(punto_ingress_control_sign(&ready, bootstrap.secret));
    require(send(client, &ready, sizeof(ready), 0) == sizeof(ready));
    std::uint8_t packet[PUNTO_INGRESS_FRAME_MAX_BYTES]{};
    const ssize_t received = recv(client, packet, sizeof(packet), 0);
    PuntoIngressFrameHeader header{};
    PuntoIngressRecord records[PUNTO_INGRESS_MAX_RECORDS]{};
    require(received > 0);
    require(punto_ingress_decode_frame(packet, static_cast<std::size_t>(received),
                                       &header, records,
                                       PUNTO_INGRESS_MAX_RECORDS) ==
            PUNTO_INGRESS_DECODE_OK);
    require(header.sequence == 1);
    // Drop the frame ACK. The producer must query, not send the keys again.
    PuntoIngressControl query{};
    require(recv(client, &query, sizeof(query), 0) == sizeof(query));
    require(query.kind == PUNTO_CONTROL_QUERY);
    require(query.stream_epoch == ready.stream_epoch && query.sequence == 1);
    require(punto_ingress_control_verify(&query, sizeof(query),
                                        bootstrap.secret));
    PuntoIngressControl result{};
    result.kind = PUNTO_CONTROL_QUERY_RESULT;
    result.policy_hash = bootstrap.policy_hash;
    result.build_id = bootstrap.build_id;
    result.stream_epoch = ready.stream_epoch;
    result.sequence = 1;
    result.status = PUNTO_FRAME_ACCEPTED;
    require(punto_ingress_control_sign(&result, bootstrap.secret));
    PuntoIngressControl late_ack = result;
    late_ack.kind = PUNTO_CONTROL_FRAME_RESULT;
    require(punto_ingress_control_sign(&late_ack, bootstrap.secret));
    require(send(client, &late_ack, sizeof(late_ack), 0) == sizeof(late_ack));
    require(send(client, &result, sizeof(result), 0) == sizeof(result));
    const ssize_t second = recv(client, packet, sizeof(packet), 0);
    require(second > 0);
    require(punto_ingress_decode_frame(packet, static_cast<std::size_t>(second),
                                       &header, records,
                                       PUNTO_INGRESS_MAX_RECORDS) ==
            PUNTO_INGRESS_DECODE_OK);
    require(header.sequence == 2);
    result.kind = PUNTO_CONTROL_FRAME_RESULT;
    result.sequence = 2;
    require(punto_ingress_control_sign(&result, bootstrap.secret));
    require(send(client, &result, sizeof(result), 0) == sizeof(result));
    close(client);
  });

  auto transport = punto::XorgInputTransport::connect_authenticated(
      path, bootstrap_fd, bootstrap.policy_hash, bootstrap.build_id,
      std::chrono::milliseconds{1000});
  require(transport && transport->stream_epoch() == 0x1234);
  require(transport->server_nonce() == 0x98abcdef12345678ULL);
  punto::OrderedIngressFrame frame{transport->stream_epoch(), 1};
  input_event key{};
  key.type = EV_KEY;
  key.code = KEY_A;
  key.value = 1;
  input_event syn{};
  syn.type = EV_SYN;
  syn.code = SYN_REPORT;
  require(frame.append_input(key, 0));
  require(frame.append_input(syn, 1));
  const auto packet = frame.finish();
  require(packet.has_value());
  require(transport->submit(*packet, std::chrono::milliseconds{30}));
  punto::OrderedIngressFrame following{transport->stream_epoch(), 2};
  require(following.append_input(key, 0));
  require(following.append_input(syn, 1));
  const auto next_packet = following.finish();
  require(next_packet.has_value());
  require(transport->submit(*next_packet, std::chrono::milliseconds{100}));
  server.join();
  require(transport->terminal_failure());
  transport.reset();
  close(bootstrap_fd);
  close(listener);
  require(unlink(path.c_str()) == 0);
  require(rmdir(directory) == 0);
}
