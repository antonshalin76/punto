/**
 * @file ipc_client.cpp
 * @brief Реализация IPC клиента для связи с punto сервисом
 */

#include "punto/ipc_client.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <vector>

namespace punto {

namespace {

/// Создаёт подключение к серверу
int connect_to_server(const char* socket_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

} // namespace

std::vector<std::string> IpcClient::list_socket_paths() {
  std::vector<std::string> sockets;
  sockets.emplace_back(kSocketPath);

  std::vector<std::string> extra;

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/var/run", ec)) {
    if (ec) {
      break;
    }

    std::error_code type_ec;
    if (!entry.is_socket(type_ec) || type_ec) {
      continue;
    }

    const std::string name = entry.path().filename().string();
    if (!name.starts_with("punto-") || !name.ends_with(".sock")) {
      continue;
    }

    extra.push_back(entry.path().string());
  }

  std::sort(extra.begin(), extra.end());
  extra.erase(std::unique(extra.begin(), extra.end()), extra.end());

  for (auto& p : extra) {
    if (p != kSocketPath) {
      sockets.push_back(std::move(p));
    }
  }

  return sockets;
}

std::optional<std::string>
IpcClient::send_command_to_socket(const std::string& command,
                                 const std::string& socket_path) {
  int fd = connect_to_server(socket_path.c_str());
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

std::optional<std::string> IpcClient::send_command(const std::string& command) {
  return send_command_to_socket(command, kSocketPath);
}

ServiceStatus IpcClient::get_status() {
  bool saw_enabled = false;
  bool saw_disabled = false;

  for (const auto& socket_path : list_socket_paths()) {
    auto response = send_command_to_socket("GET_STATUS", socket_path);
    if (!response) {
      continue;
    }

    // Ответ: "OK ENABLED" или "OK DISABLED"
    if (response->find("ENABLED") != std::string::npos) {
      saw_enabled = true;
      continue;
    }
    if (response->find("DISABLED") != std::string::npos) {
      saw_disabled = true;
      continue;
    }
  }

  if (saw_enabled && saw_disabled) {
    return ServiceStatus::Unknown;
  }
  if (saw_enabled) {
    return ServiceStatus::Enabled;
  }
  if (saw_disabled) {
    return ServiceStatus::Disabled;
  }

  return ServiceStatus::Unknown;
}

bool IpcClient::set_status(bool enabled) {
  std::string command = enabled ? "SET_STATUS 1" : "SET_STATUS 0";

  int ok_count = 0;
  for (const auto& socket_path : list_socket_paths()) {
    auto response = send_command_to_socket(command, socket_path);
    if (!response) {
      continue;
    }

    if (response->find("OK") != std::string::npos ||
        response->find("ENABLED") != std::string::npos ||
        response->find("DISABLED") != std::string::npos) {
      ok_count++;
    }
  }

  return ok_count > 0;
}

bool IpcClient::reload_config(const std::string& config_path) {
  std::string cmd = "RELOAD";
  if (!config_path.empty()) {
    cmd += " ";
    cmd += config_path;
  }

  int ok_count = 0;
  for (const auto& socket_path : list_socket_paths()) {
    auto response = send_command_to_socket(cmd, socket_path);
    if (!response) {
      continue;
    }

    if (response->find("OK") != std::string::npos) {
      ok_count++;
    }
  }

  return ok_count > 0;
}

bool IpcClient::is_service_available() {
  for (const auto& socket_path : list_socket_paths()) {
    int fd = connect_to_server(socket_path.c_str());
    if (fd < 0) {
      continue;
    }
    close(fd);
    return true;
  }
  return false;
}

} // namespace punto
