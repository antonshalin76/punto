/**
 * @file ipc_server.cpp
 * @brief Реализация IPC сервера на Unix Domain Socket
 */

#include "punto/ipc_server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace punto {

namespace {

/// Удаляет пробелы с начала и конца строки
std::string_view trim(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
    sv.remove_suffix(1);
  }
  return sv;
}

/// Парсит команду из строки
IpcCommand parse_command(std::string_view cmd) {
  cmd = trim(cmd);
  
  if (cmd.starts_with("GET_STATUS")) {
    return IpcCommand::GetStatus;
  }
  if (cmd.starts_with("SET_STATUS")) {
    return IpcCommand::SetStatus;
  }
  if (cmd.starts_with("RELOAD")) {
    return IpcCommand::Reload;
  }
  if (cmd.starts_with("SHUTDOWN")) {
    return IpcCommand::Shutdown;
  }
  
  return IpcCommand::Unknown;
}

} // namespace

IpcServer::IpcServer(std::atomic<bool>& enabled_flag, ReloadCallback reload_callback)
    : enabled_flag_(enabled_flag)
    , reload_callback_(std::move(reload_callback)) {}

IpcServer::~IpcServer() {
  stop();
}

bool IpcServer::start() {
  if (running_.load()) {
    return true;
  }

  server_fd_ = create_socket();
  if (server_fd_ < 0) {
    return false;
  }

  running_.store(true);
  server_thread_ = std::jthread([this](std::stop_token st) {
    server_loop();
  });

  std::cerr << "[punto-ipc] Server started on " << socket_path_ << "\n";
  return true;
}

void IpcServer::stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);

  // Закрываем сокет, чтобы разблокировать poll()
  if (server_fd_ >= 0) {
    shutdown(server_fd_, SHUT_RDWR);
    close(server_fd_);
    server_fd_ = -1;
  }

  // Ждём завершения потока
  if (server_thread_.joinable()) {
    server_thread_.request_stop();
    server_thread_.join();
  }

  // Удаляем файл сокета
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
    socket_path_.clear();
  }

  std::cerr << "[punto-ipc] Server stopped\n";
}

bool IpcServer::is_running() const noexcept {
  return running_.load();
}

int IpcServer::create_socket() {
  auto is_socket_active = [](const char* socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      // Консервативно: если не можем проверить — считаем сокет "живым",
      // чтобы не удалить чужой/рабочий путь.
      return true;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    bool active = false;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      active = true;
    } else {
      // ECONNREFUSED/ENOENT — вероятно, стейл-файл без слушателя.
      // Остальные ошибки считаем "живым" (не удаляем).
      active = !(errno == ECONNREFUSED || errno == ENOENT);
    }

    close(fd);
    return active;
  };

  auto create_bound_socket = [&](const std::string& socket_path,
                                 bool unlink_first,
                                 int* out_errno) -> int {
    if (unlink_first) {
      (void)unlink(socket_path.c_str());
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      if (out_errno) {
        *out_errno = errno;
      }
      std::cerr << "[punto-ipc] Failed to create socket: " << strerror(errno)
                << "\n";
      return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      const int err = errno;
      if (out_errno) {
        *out_errno = err;
      }
      std::cerr << "[punto-ipc] Failed to bind socket (" << socket_path
                << "): " << strerror(err) << "\n";
      close(fd);
      return -1;
    }

    // Устанавливаем права доступа: rw для всех (0666)
    // Это безопасно для локального сокета — позволяет пользователю писать в сокет root'а
    if (chmod(socket_path.c_str(), 0666) < 0) {
      std::cerr << "[punto-ipc] Warning: failed to chmod socket (" << socket_path
                << "): " << strerror(errno) << "\n";
    }

    if (listen(fd, 5) < 0) {
      const int err = errno;
      if (out_errno) {
        *out_errno = err;
      }
      std::cerr << "[punto-ipc] Failed to listen (" << socket_path
                << "): " << strerror(err) << "\n";
      close(fd);
      (void)unlink(socket_path.c_str());
      return -1;
    }

    socket_path_ = socket_path;
    if (out_errno) {
      *out_errno = 0;
    }
    return fd;
  };

  // 1) Пытаемся поднять основной сокет.
  int bind_errno = 0;
  int fd = create_bound_socket(kIpcSocketPath, /*unlink_first=*/false, &bind_errno);
  if (fd >= 0) {
    return fd;
  }

  // 2) Если основной сокет занят — не трогаем его (multi-instance),
  // а поднимаем отдельный сокет для текущего процесса.
  if (bind_errno == EADDRINUSE) {
    // Но если файл стейл (нет слушателя), можно безопасно заменить.
    if (!is_socket_active(kIpcSocketPath)) {
      std::cerr << "[punto-ipc] Stale primary socket detected, replacing: "
                << kIpcSocketPath << "\n";
      (void)unlink(kIpcSocketPath);
      bind_errno = 0;
      fd = create_bound_socket(kIpcSocketPath, /*unlink_first=*/false, &bind_errno);
      if (fd >= 0) {
        return fd;
      }
    }

    const std::string fallback =
        std::string("/var/run/punto-") + std::to_string(::getpid()) + ".sock";
    std::cerr << "[punto-ipc] Primary socket busy, using: " << fallback << "\n";

    bind_errno = 0;
    fd = create_bound_socket(fallback, /*unlink_first=*/true, &bind_errno);
    if (fd >= 0) {
      return fd;
    }
  }

  return -1;
}

void IpcServer::server_loop() {
  std::cerr << "[punto-ipc] Server thread started\n";

  while (running_.load()) {
    pollfd pfd = {server_fd_, POLLIN, 0};
    int ret = poll(&pfd, 1, 500); // Timeout 500ms для проверки running_

    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "[punto-ipc] Poll error: " << strerror(errno) << "\n";
      break;
    }

    if (ret == 0) {
      // Timeout — проверяем running_ и продолжаем
      continue;
    }

    if (pfd.revents & POLLIN) {
      sockaddr_un client_addr{};
      socklen_t client_len = sizeof(client_addr);
      
      int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
      if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && running_.load()) {
          std::cerr << "[punto-ipc] Accept error: " << strerror(errno) << "\n";
        }
        continue;
      }

      handle_client(client_fd);
      close(client_fd);
    }
  }

  std::cerr << "[punto-ipc] Server thread exiting\n";
}

void IpcServer::handle_client(int client_fd) {
  char buffer[256] = {};
  ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
  
  if (bytes_read <= 0) {
    return;
  }

  std::string_view cmd{buffer, static_cast<size_t>(bytes_read)};
  cmd = trim(cmd);

  std::cerr << "[punto-ipc] Received command: " << cmd << "\n";

  IpcResult result = execute_command(cmd);

  std::string response = result.success ? "OK" : "ERROR";
  if (!result.message.empty()) {
    response += " ";
    response += result.message;
  }
  response += "\n";

  ssize_t written = write(client_fd, response.c_str(), response.size());
  (void)written; // Игнорируем ошибки записи
}

IpcResult IpcServer::execute_command(std::string_view cmd) {
  IpcCommand command = parse_command(cmd);

  switch (command) {
  case IpcCommand::GetStatus: {
    bool enabled = enabled_flag_.load();
    return {true, enabled ? "ENABLED" : "DISABLED"};
  }

  case IpcCommand::SetStatus: {
    // Парсим аргумент: SET_STATUS 0|1
    auto space_pos = cmd.find(' ');
    if (space_pos == std::string_view::npos) {
      return {false, "Missing argument"};
    }
    
    std::string_view arg = trim(cmd.substr(space_pos + 1));
    if (arg == "1" || arg == "true" || arg == "on") {
      enabled_flag_.store(true);
      std::cerr << "[punto-ipc] Status set to ENABLED\n";
      return {true, "ENABLED"};
    } else if (arg == "0" || arg == "false" || arg == "off") {
      enabled_flag_.store(false);
      std::cerr << "[punto-ipc] Status set to DISABLED\n";
      return {true, "DISABLED"};
    }
    return {false, "Invalid argument"};
  }

  case IpcCommand::Reload: {
    if (!reload_callback_) {
      return {false, "Reload not supported"};
    }

    std::string reload_path;
    auto space_pos = cmd.find(' ');
    if (space_pos != std::string_view::npos) {
      std::string_view arg = trim(cmd.substr(space_pos + 1));
      reload_path.assign(arg.begin(), arg.end());
    }

    IpcResult res = reload_callback_(reload_path);

    if (res.success) {
      std::cerr << "[punto-ipc] Config reloaded successfully\n";
    } else {
      std::cerr << "[punto-ipc] Config reload failed";
      if (!res.message.empty()) {
        std::cerr << ": " << res.message;
      }
      std::cerr << "\n";
    }

    if (res.message.empty()) {
      res.message = res.success ? "Config reloaded" : "Reload failed";
    }

    return res;
  }

  case IpcCommand::Shutdown: {
    std::cerr << "[punto-ipc] Shutdown requested\n";
    // Сервис не должен выключаться по IPC команде от пользователя
    // Это может нарушить работу udevmon
    return {false, "Shutdown not allowed via IPC"};
  }

  case IpcCommand::Unknown:
  default:
    return {false, "Unknown command"};
  }
}

} // namespace punto
