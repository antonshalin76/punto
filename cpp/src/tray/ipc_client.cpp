/**
 * @file ipc_client.cpp
 * @brief Реализация IPC клиента для связи с punto сервисом
 */

#include "punto/ipc_client.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <cerrno>
#include <cstring>

namespace punto {

namespace {

/// Создаёт подключение к серверу
int connect_to_server() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, IpcClient::kSocketPath, sizeof(addr.sun_path) - 1);

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

} // namespace

std::optional<std::string> IpcClient::send_command(const std::string& command) {
  int fd = connect_to_server();
  if (fd < 0) {
    return std::nullopt;
  }

  // Отправляем команду
  std::string cmd_with_newline = command + "\n";
  ssize_t written = write(fd, cmd_with_newline.c_str(), cmd_with_newline.size());
  if (written != static_cast<ssize_t>(cmd_with_newline.size())) {
    close(fd);
    return std::nullopt;
  }

  // Ждём ответ с таймаутом
  pollfd pfd = {fd, POLLIN, 0};
  int ret = poll(&pfd, 1, kTimeoutMs);
  
  if (ret <= 0) {
    close(fd);
    return std::nullopt;
  }

  // Читаем ответ
  char buffer[256] = {};
  ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);

  if (bytes_read <= 0) {
    return std::nullopt;
  }

  // Удаляем trailing newline
  std::string response{buffer, static_cast<size_t>(bytes_read)};
  while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
    response.pop_back();
  }

  return response;
}

ServiceStatus IpcClient::get_status() {
  auto response = send_command("GET_STATUS");
  if (!response) {
    return ServiceStatus::Unknown;
  }

  // Ответ: "OK ENABLED" или "OK DISABLED"
  if (response->find("ENABLED") != std::string::npos) {
    return ServiceStatus::Enabled;
  }
  if (response->find("DISABLED") != std::string::npos) {
    return ServiceStatus::Disabled;
  }

  return ServiceStatus::Unknown;
}

bool IpcClient::set_status(bool enabled) {
  std::string command = enabled ? "SET_STATUS 1" : "SET_STATUS 0";
  auto response = send_command(command);
  
  if (!response) {
    return false;
  }

  return response->find("OK") != std::string::npos ||
         response->find("ENABLED") != std::string::npos ||
         response->find("DISABLED") != std::string::npos;
}

ServiceStatus IpcClient::toggle_status() {
  ServiceStatus current = get_status();
  
  if (current == ServiceStatus::Unknown) {
    return ServiceStatus::Unknown;
  }

  bool new_state = (current == ServiceStatus::Disabled);
  if (set_status(new_state)) {
    return new_state ? ServiceStatus::Enabled : ServiceStatus::Disabled;
  }

  return ServiceStatus::Unknown;
}

bool IpcClient::reload_config() {
  auto response = send_command("RELOAD");
  return response && response->find("OK") != std::string::npos;
}

bool IpcClient::is_service_available() {
  int fd = connect_to_server();
  if (fd < 0) {
    return false;
  }
  close(fd);
  return true;
}

} // namespace punto
