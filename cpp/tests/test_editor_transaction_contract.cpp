#include "punto/editor_transaction.hpp"

#include <json-glib/json-glib.h>
#include <xcb/xcb.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

const char *status_name(punto::EditorTransactionStatus status) {
  switch (status) {
  case punto::EditorTransactionStatus::Unavailable:
    return "UNAVAILABLE";
  case punto::EditorTransactionStatus::Prepared:
    return "PREPARED";
  case punto::EditorTransactionStatus::Applied:
    return "APPLIED";
  case punto::EditorTransactionStatus::Rejected:
    return "REJECTED";
  case punto::EditorTransactionStatus::Unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

void emit(bool ok, std::string_view status, std::string_view reason = {},
          std::string_view fragment = {}, std::uint32_t start = 0,
          std::uint32_t end = 0, std::string_view operation_id = {}) {
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "ok");
  json_builder_add_boolean_value(builder, ok);
  json_builder_set_member_name(builder, "status");
  json_builder_add_string_value(builder, std::string{status}.c_str());
  json_builder_set_member_name(builder, "reason");
  json_builder_add_string_value(builder, std::string{reason}.c_str());
  json_builder_set_member_name(builder, "fragment");
  json_builder_add_string_value(builder, std::string{fragment}.c_str());
  json_builder_set_member_name(builder, "start");
  json_builder_add_int_value(builder, start);
  json_builder_set_member_name(builder, "end");
  json_builder_add_int_value(builder, end);
  json_builder_set_member_name(builder, "operation_id");
  json_builder_add_string_value(builder, std::string{operation_id}.c_str());
  json_builder_end_object(builder);
  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar *data = json_generator_to_data(generator, nullptr);
  std::cout << (data == nullptr ? "{}" : data) << std::endl;
  g_free(data);
  json_node_free(root);
  g_object_unref(generator);
  g_object_unref(builder);
}

} // namespace

int main() {
  int screen = 0;
  std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection{
      xcb_connect(nullptr, &screen), &xcb_disconnect};
  if (connection == nullptr ||
      xcb_connection_has_error(connection.get()) != 0) {
    emit(false, "UNAVAILABLE", "x11_connection");
    return 1;
  }
  const auto focus_cookie = xcb_get_input_focus(connection.get());
  std::unique_ptr<xcb_get_input_focus_reply_t, decltype(&std::free)> focus{
      xcb_get_input_focus_reply(connection.get(), focus_cookie, nullptr),
      &std::free};
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  const std::string runtime_directory =
      runtime == nullptr
          ? "/run/user/" +
                std::to_string(static_cast<std::uint32_t>(::getuid()))
          : std::string{runtime};
  if (!focus) {
    emit(false, "UNAVAILABLE", "runtime_context");
    return 1;
  }
  const auto deadline = Clock::now() + std::chrono::seconds{2};
  auto target = punto::EditorTransactionClient::discover(
      connection.get(), screen, focus->focus,
      static_cast<std::uint32_t>(::getuid()), runtime_directory, deadline);
  if (!target) {
    emit(false, "UNAVAILABLE", "adapter_discovery");
    return 1;
  }
  punto::EditorTransactionClient client{*target};
  std::optional<punto::PreparedEditorTransaction> prepared;
  std::map<std::string, Clock::time_point> keyed_deadlines;
  emit(true, "READY");

  std::string line;
  while (std::getline(std::cin, line)) {
    const auto split = line.find(' ');
    const std::string command = line.substr(0, split);
    const std::string argument =
        split == std::string::npos ? std::string{} : line.substr(split + 1);
    const auto command_deadline = Clock::now() + std::chrono::seconds{2};
    if (command == "PREPARE_WORD" || command == "PREPARE_SELECTION" ||
        command == "PREPARE_SHORT" || command == "PREPARE_KEY") {
      const auto key_end = argument.find(' ');
      const std::string request_key =
          command == "PREPARE_KEY" && key_end != std::string::npos
              ? argument.substr(0, key_end)
              : std::string{};
      const std::string expected = command == "PREPARE_KEY"
                                       ? argument.substr(key_end + 1)
                                       : argument;
      const auto operation_deadline =
          command == "PREPARE_SHORT"
              ? Clock::now() + std::chrono::seconds{1}
              : request_key.empty()
                    ? command_deadline
                    : keyed_deadlines.emplace(request_key, command_deadline)
                          .first->second;
      prepared = client.prepare(command == "PREPARE_SELECTION"
                                    ? punto::EditorTransactionMode::Selection
                                    : punto::EditorTransactionMode::Word,
                                expected,
                                command == "PREPARE_SHORT" || !request_key.empty()
                                    ? operation_deadline
                                    : command_deadline,
                                operation_deadline, request_key);
      if (!prepared) {
        emit(false, "REJECTED", "prepare_failed");
      } else {
        emit(true, "PREPARED", {}, prepared->fragment, prepared->start,
             prepared->end, prepared->operation_id);
      }
      continue;
    }
    if (!prepared) {
      emit(false, "REJECTED", "not_prepared");
      continue;
    }
    if (command == "APPLY_LOST") {
      const bool sent = client.compare_apply_without_read_for_test(
          *prepared, argument, command_deadline);
      emit(sent, sent ? "SENT" : "UNKNOWN", sent ? "" : "send_failed");
      continue;
    }
    if (command == "RETRY_UNKNOWN") {
      auto unknown = *prepared;
      unknown.operation_id += "-unknown";
      const auto result =
          client.compare_apply(unknown, argument, command_deadline);
      emit(result.status == punto::EditorTransactionStatus::Applied,
           status_name(result.status), result.reason, {},
           result.resulting_start, result.resulting_end);
      continue;
    }
    if (command == "APPLY" || command == "RETRY") {
      const auto result =
          client.compare_apply(*prepared, argument, command_deadline);
      emit(result.status == punto::EditorTransactionStatus::Applied,
           status_name(result.status), result.reason, {},
           result.resulting_start, result.resulting_end);
      continue;
    }
    emit(false, "REJECTED", "unknown_command");
  }
  return 0;
}
