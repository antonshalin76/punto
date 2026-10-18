#include "punto/ordered_ingress_frame.hpp"
#include "punto_ingress_stream.h"
#include "punto_ingress_control.h"

#include <linux/input.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
void require(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) {
    std::fprintf(stderr, "contract failure at %s:%u\n", at.file_name(),
                 at.line());
    std::abort();
  }
}
} // namespace

int main() {
  PuntoIngressBootstrap bootstrap{};
  bootstrap.magic = PUNTO_INGRESS_BOOTSTRAP_MAGIC;
  bootstrap.policy_hash = 0x12345678;
  bootstrap.build_id = 0x9abcdef0;
  bootstrap.producer_uid = 0;
  bootstrap.server_uid = 0;
  std::memset(bootstrap.secret, 0x5a, sizeof(bootstrap.secret));
  require(punto_ingress_bootstrap_valid(&bootstrap));
  const int secret_fd = memfd_create("punto-contract", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  require(secret_fd >= 0);
  require(write(secret_fd, &bootstrap, sizeof(bootstrap)) == sizeof(bootstrap));
  PuntoIngressBootstrap read_bootstrap{};
  require(!punto_ingress_bootstrap_read(secret_fd, &read_bootstrap));
  require(fcntl(secret_fd, F_ADD_SEALS,
                F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0);
  require(punto_ingress_bootstrap_read(secret_fd, &read_bootstrap));
  require(read_bootstrap.producer_uid == bootstrap.producer_uid);
  close(secret_fd);
  PuntoIngressControl hello{};
  hello.kind = PUNTO_CONTROL_HELLO;
  hello.policy_hash = bootstrap.policy_hash;
  hello.build_id = bootstrap.build_id;
  hello.challenge[0] = 7;
  require(!punto_ingress_control_sign(&hello, bootstrap.secret));
  hello.stream_epoch = 0x98abcdef12345678ULL;
  require(punto_ingress_control_sign(&hello, bootstrap.secret));
  require(punto_ingress_control_verify(&hello, sizeof(hello),
                                      bootstrap.secret));
  auto forged_hello = hello;
  forged_hello.challenge[0] ^= 1;
  require(!punto_ingress_control_verify(&forged_hello,
                                       sizeof(forged_hello),
                                       bootstrap.secret));
  require(!punto_ingress_control_verify(&hello, sizeof(hello) - 1,
                                       bootstrap.secret));

  punto::OrderedIngressFrame frame{0x1234, 7};
  input_event key_down{};
  key_down.type = EV_KEY;
  key_down.code = KEY_A;
  key_down.value = 1;
  require(frame.append_input(key_down, 0));
  require(frame.append_pause(1, 1, 1, 0, 0x55, 1000));
  require(frame.append_pause(2, 2, 1, 0, 0x55, 1000));
  require(frame.append_pause(3, 0, 1, 0, 0x55, 1000));
  input_event key_up = key_down;
  key_up.value = 0;
  require(frame.append_input(key_up, 4));
  input_event syn{};
  syn.type = EV_SYN;
  syn.code = SYN_REPORT;
  require(frame.append_input(syn, 5));

  const auto packet = frame.finish();
  require(packet.has_value());
  require(packet->header.stream_epoch == 0x1234);
  require(packet->header.sequence == 7);
  require(packet->header.record_count == 6);
  require(packet->records[0].kind == PUNTO_INGRESS_KEY);
  require(packet->records[1].kind == PUNTO_INGRESS_PAUSE_BEGIN);
  require(packet->records[2].kind == PUNTO_INGRESS_PAUSE_REPEAT);
  require(packet->records[3].kind == PUNTO_INGRESS_PAUSE_END);
  require(packet->records[4].kind == PUNTO_INGRESS_KEY);
  require(packet->records[5].kind == PUNTO_INGRESS_SYN);
  for (std::uint32_t i = 0; i < packet->header.record_count; ++i) {
    require(packet->records[i].source_index == i);
    require(packet->records[i].kind != PUNTO_INGRESS_KEY ||
            packet->records[i].code != KEY_PAUSE);
  }
  require(punto::verify_ordered_ingress_frame(*packet));
  std::vector<std::uint8_t> bytes(packet->header.byte_length);
  std::memcpy(bytes.data(), &packet->header, sizeof(packet->header));
  std::memcpy(bytes.data() + sizeof(packet->header), packet->records.data(),
              packet->records.size() * sizeof(PuntoIngressRecord));
  PuntoIngressFrameHeader decoded{};
  PuntoIngressRecord decoded_records[PUNTO_INGRESS_MAX_RECORDS]{};
  require(punto_ingress_decode_frame(bytes.data(), bytes.size(), &decoded,
                                     decoded_records,
                                     PUNTO_INGRESS_MAX_RECORDS) ==
          PUNTO_INGRESS_DECODE_OK);
  require(decoded.sequence == packet->header.sequence);
  require(decoded_records[1].kind == PUNTO_INGRESS_PAUSE_BEGIN);
  require(punto_ingress_decode_frame(bytes.data(), sizeof(PuntoIngressFrameHeader) - 1,
                                     &decoded, decoded_records,
                                     PUNTO_INGRESS_MAX_RECORDS) !=
          PUNTO_INGRESS_DECODE_OK);
  require(punto_ingress_decode_frame(bytes.data(), bytes.size() - 1, &decoded,
                                     decoded_records,
                                     PUNTO_INGRESS_MAX_RECORDS) !=
          PUNTO_INGRESS_DECODE_OK);
  auto extra = bytes;
  extra.push_back(0);
  require(punto_ingress_decode_frame(extra.data(), extra.size(), &decoded,
                                     decoded_records,
                                     PUNTO_INGRESS_MAX_RECORDS) !=
          PUNTO_INGRESS_DECODE_OK);
  auto invalid_action = bytes;
  PuntoIngressRecord invalid_record{};
  const auto record_offset = sizeof(PuntoIngressFrameHeader) +
                             sizeof(PuntoIngressRecord);
  std::memcpy(&invalid_record, invalid_action.data() + record_offset,
              sizeof(invalid_record));
  invalid_record.action = 0xffff;
  std::memcpy(invalid_action.data() + record_offset, &invalid_record,
              sizeof(invalid_record));
  PuntoIngressFrameHeader invalid_header{};
  std::memcpy(&invalid_header, invalid_action.data(), sizeof(invalid_header));
  punto_ingress_digest_records(
      invalid_action.data() + sizeof(PuntoIngressFrameHeader),
      invalid_action.size() - sizeof(PuntoIngressFrameHeader),
      invalid_header.digest);
  std::memcpy(invalid_action.data(), &invalid_header, sizeof(invalid_header));
  require(punto_ingress_decode_frame(invalid_action.data(), invalid_action.size(),
                                     &decoded, decoded_records,
                                     PUNTO_INGRESS_MAX_RECORDS) !=
          PUNTO_INGRESS_DECODE_OK);
  auto tampered = *packet;
  tampered.records[0].code = KEY_B;
  require(!punto::verify_ordered_ingress_frame(tampered));
  tampered = *packet;
  tampered.records[0].kind = 0xffff;
  require(!punto::verify_ordered_ingress_frame(tampered));
  require(!frame.append_input(key_down, 6));

  punto::OrderedIngressFrame rejects{0x1234, 8};
  input_event pause = key_down;
  pause.code = KEY_PAUSE;
  require(!rejects.append_input(pause, 0));
  require(rejects.append_input(key_down, 0));
  require(!rejects.append_input(key_down, 0));
  require(rejects.append_input(syn, 1));
  const auto valid = rejects.finish();
  require(valid.has_value());
  require(valid->header.record_count == 2);

  punto::OrderedIngressFrame incomplete{0x1234, 9};
  require(incomplete.append_input(key_down, 0));
  require(!incomplete.finish().has_value());

  PuntoIngressStream stream{};
  punto_ingress_stream_init(&stream, 0x1234);
  punto::OrderedIngressFrame first{0x1234, 1};
  require(first.append_input(key_down, 0));
  require(first.append_pause(1, 1, PUNTO_ACTION_LAYOUT_WORD, 0, 1, 1000));
  require(first.append_input(syn, 2));
  const auto first_packet = first.finish();
  require(first_packet.has_value());
  PuntoIngressPreview preview{};
  require(punto_ingress_stream_preview(
              &stream, &first_packet->header, first_packet->records.data(),
              first_packet->records.size(), 100, &preview) ==
          PUNTO_STREAM_NEW);
  require(preview.new_request_count == 1);
  require(punto_ingress_stream_commit(&stream, &preview, 100));
  PuntoIngressReceipt receipt{};
  require(punto_ingress_stream_query(&stream, 1, &receipt));
  require(receipt.record_count == first_packet->records.size());
  require(receipt.status == PUNTO_FRAME_ACCEPTED);
  require(punto_ingress_stream_preview(
              &stream, &first_packet->header, first_packet->records.data(),
              first_packet->records.size(), 101, &preview) ==
          PUNTO_STREAM_DUPLICATE);
  require(stream.next_sequence == 2);

  punto::OrderedIngressFrame repeat{0x1234, 2};
  require(repeat.append_pause(0, 2, PUNTO_ACTION_LAYOUT_WORD, 0, 1, 1000));
  require(repeat.append_input(syn, 1));
  const auto repeat_packet = repeat.finish();
  require(repeat_packet.has_value());
  require(punto_ingress_stream_preview(
              &stream, &repeat_packet->header, repeat_packet->records.data(),
              repeat_packet->records.size(), 102, &preview) ==
          PUNTO_STREAM_NEW);
  require(preview.new_request_count == 0);
  require(punto_ingress_stream_commit(&stream, &preview, 102));

  punto::OrderedIngressFrame wrong_repeat{0x1234, 3};
  require(wrong_repeat.append_pause(0, 2, PUNTO_ACTION_CASE_WORD, 0, 1, 1000));
  require(wrong_repeat.append_input(syn, 1));
  const auto wrong_packet = wrong_repeat.finish();
  require(wrong_packet.has_value());
  require(punto_ingress_stream_preview(
              &stream, &wrong_packet->header, wrong_packet->records.data(),
              wrong_packet->records.size(), 103, &preview) ==
          PUNTO_STREAM_BAD_LIFECYCLE);
  require(stream.next_sequence == 3);

  punto::OrderedIngressFrame release{0x1234, 3};
  require(release.append_pause(0, 0, PUNTO_ACTION_LAYOUT_WORD, 0, 1, 1000));
  require(release.append_input(syn, 1));
  const auto release_packet = release.finish();
  require(release_packet.has_value());
  require(punto_ingress_stream_preview(
              &stream, &release_packet->header, release_packet->records.data(),
              release_packet->records.size(), 104, &preview) ==
          PUNTO_STREAM_NEW);
  require(punto_ingress_stream_commit(&stream, &preview, 104));
  require(!stream.pause.active);

  punto::OrderedIngressFrame orphan{0x1234, 4};
  require(orphan.append_pause(0, 0, PUNTO_ACTION_LAYOUT_WORD, 0, 1, 1000));
  require(orphan.append_input(syn, 1));
  const auto orphan_packet = orphan.finish();
  require(orphan_packet.has_value());
  require(punto_ingress_stream_preview(
              &stream, &orphan_packet->header, orphan_packet->records.data(),
              orphan_packet->records.size(), 105, &preview) ==
          PUNTO_STREAM_NEW);
  require(preview.new_request_count == 0);
  require(punto_ingress_stream_commit(&stream, &preview, 105));

  punto::OrderedIngressFrame second{0x1234, 5};
  require(second.append_pause(0, 1, PUNTO_ACTION_LAYOUT_WORD, 0, 2, 2000));
  require(second.append_input(syn, 1));
  const auto second_packet = second.finish();
  require(second_packet.has_value());
  require(punto_ingress_stream_preview(
              &stream, &second_packet->header, second_packet->records.data(),
              second_packet->records.size(), 106, &preview) ==
          PUNTO_STREAM_NEW);
  require(preview.new_request_count == 1);
  require(punto_ingress_stream_commit(&stream, &preview, 106));
  require(punto_ingress_stream_set_status(&stream, 1, PUNTO_FRAME_BLOCKED, 1));
  require(punto_ingress_stream_query(&stream, 1, &receipt));
  require(receipt.status == PUNTO_FRAME_BLOCKED);
  require(receipt.dispatched_count == 1);
  require(!punto_ingress_stream_set_status(&stream, 1,
                                          PUNTO_FRAME_ACCEPTED, 0));
}
