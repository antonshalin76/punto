/**
 * @file control_plane_state.hpp
 * @brief Shared runtime state for primary/secondary punto-daemon roles.
 */

#pragma once

#include "punto/runtime_file.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <limits.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace punto {

inline constexpr const char *kControlPlaneLockPath =
    "/var/run/punto-control.lock";
inline constexpr const char *kControlPlaneStatePath =
    "/var/run/punto-control.state";

struct SharedControlPlaneState {
  std::uint64_t config_generation = 0;
  std::uint64_t status_generation = 0;
  bool enabled = true;
  std::string config_path;
};

enum class ControlPlanePromotionAction {
  Ready,
  ReloadAuthoritativePath,
  ReloadCurrentAuthority,
};

inline ControlPlanePromotionAction
plan_control_plane_promotion(const SharedControlPlaneState &authoritative,
                             std::uint64_t applied_config_generation,
                             std::string_view current_config_path,
                             bool authoritative_path_allowed,
                             bool current_path_allowed,
                             bool authority_fallback_applied = false) noexcept {
  if (!current_path_allowed) {
    return ControlPlanePromotionAction::ReloadCurrentAuthority;
  }
  if (applied_config_generation != authoritative.config_generation) {
    return authoritative_path_allowed && !authoritative.config_path.empty()
               ? ControlPlanePromotionAction::ReloadAuthoritativePath
               : ControlPlanePromotionAction::ReloadCurrentAuthority;
  }
  if (authority_fallback_applied) {
    return ControlPlanePromotionAction::Ready;
  }
  if (authoritative.config_path.empty() || !authoritative_path_allowed ||
      current_config_path == authoritative.config_path) {
    return ControlPlanePromotionAction::Ready;
  }
  return ControlPlanePromotionAction::ReloadAuthoritativePath;
}

namespace detail {

inline constexpr std::size_t kMaxControlPlaneStateBytes = 8192U;
inline constexpr std::size_t kMaxControlPlanePathBytes = PATH_MAX;

inline std::optional<std::uint64_t> parse_u64(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  const auto [ptr, ec] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (ec != std::errc{} || ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

inline bool valid_control_plane_path(std::string_view value) noexcept {
  if (value.size() > kMaxControlPlanePathBytes) {
    return false;
  }
  for (const char raw_byte : value) {
    const auto byte = static_cast<unsigned char>(raw_byte);
    if (byte == 0 || byte == '\n' || byte == '\r' || byte < 0x20U ||
        byte == 0x7fU) {
      return false;
    }
  }
  return true;
}

struct RuntimePath {
  std::string directory;
  std::string name;
};

inline std::optional<RuntimePath> split_runtime_path(std::string_view path) {
  if (path.empty() || path.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t separator = path.find_last_of('/');
  RuntimePath result;
  if (separator == std::string_view::npos) {
    result.directory = ".";
    result.name.assign(path);
  } else {
    result.directory =
        separator == 0 ? "/" : std::string{path.substr(0, separator)};
    result.name.assign(path.substr(separator + 1));
  }
  if (result.name.empty() || result.name == "." || result.name == ".." ||
      result.name.size() > 192U) {
    return std::nullopt;
  }
  return result;
}

inline int
open_runtime_directory(const RuntimePath &path,
                       const RuntimeFileSecurity &security) noexcept {
  int fd = -1;
  do {
    fd = ::open(path.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return -1;
  }

  struct stat metadata {};
  if (::fstat(fd, &metadata) != 0 || !S_ISDIR(metadata.st_mode) ||
      metadata.st_uid != security.owner_uid || (metadata.st_mode & 0022) != 0) {
    (void)::close(fd);
    return -1;
  }
  return fd;
}

inline bool write_all(int fd, std::string_view payload) noexcept {
  std::size_t written = 0;
  while (written < payload.size()) {
    const ssize_t count =
        ::write(fd, payload.data() + written, payload.size() - written);
    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

inline std::optional<std::string> read_bounded(int fd) {
  std::string payload;
  payload.reserve(512U);
  char chunk[1024];
  while (true) {
    const ssize_t count = ::read(fd, chunk, sizeof(chunk));
    if (count > 0) {
      const std::size_t size = static_cast<std::size_t>(count);
      if (payload.size() > kMaxControlPlaneStateBytes - size) {
        return std::nullopt;
      }
      payload.append(chunk, size);
      continue;
    }
    if (count == 0) {
      return payload;
    }
    if (errno != EINTR) {
      return std::nullopt;
    }
  }
}

inline std::optional<SharedControlPlaneState>
parse_control_plane_state(std::string_view payload) {
  if (payload.empty() || payload.size() > kMaxControlPlaneStateBytes ||
      payload.back() != '\n' || payload.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }

  std::string_view lines[4];
  std::size_t line_count = 0;
  std::size_t begin = 0;
  while (begin < payload.size()) {
    const std::size_t end = payload.find('\n', begin);
    if (end == std::string_view::npos || line_count == 4U) {
      return std::nullopt;
    }
    lines[line_count++] = payload.substr(begin, end - begin);
    begin = end + 1U;
  }
  if (line_count != 4U) {
    return std::nullopt;
  }

  constexpr std::string_view config_prefix = "config_generation=";
  constexpr std::string_view status_prefix = "status_generation=";
  constexpr std::string_view enabled_prefix = "enabled=";
  constexpr std::string_view path_prefix = "config_path=";
  if (!lines[0].starts_with(config_prefix) ||
      !lines[1].starts_with(status_prefix) ||
      !lines[2].starts_with(enabled_prefix) ||
      !lines[3].starts_with(path_prefix)) {
    return std::nullopt;
  }

  const auto config = parse_u64(lines[0].substr(config_prefix.size()));
  const auto status = parse_u64(lines[1].substr(status_prefix.size()));
  const std::string_view enabled = lines[2].substr(enabled_prefix.size());
  const std::string_view config_path = lines[3].substr(path_prefix.size());
  if (!config || !status || (enabled != "0" && enabled != "1") ||
      !valid_control_plane_path(config_path)) {
    return std::nullopt;
  }

  return SharedControlPlaneState{*config, *status, enabled == "1",
                                 std::string{config_path}};
}

inline std::string
serialize_control_plane_state(const SharedControlPlaneState &state) {
  std::string payload;
  payload.reserve(96U + state.config_path.size());
  payload += "config_generation=";
  payload += std::to_string(state.config_generation);
  payload += "\nstatus_generation=";
  payload += std::to_string(state.status_generation);
  payload += "\nenabled=";
  payload += state.enabled ? '1' : '0';
  payload += "\nconfig_path=";
  payload += state.config_path;
  payload += '\n';
  return payload;
}

} // namespace detail

class ControlPlaneLease {
public:
  explicit ControlPlaneLease(
      std::string path = std::string{kControlPlaneLockPath})
      : path_{std::move(path)}, security_{default_runtime_file_security()} {}

  ~ControlPlaneLease() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  ControlPlaneLease(const ControlPlaneLease &) = delete;
  ControlPlaneLease &operator=(const ControlPlaneLease &) = delete;

  [[nodiscard]] bool try_acquire() {
    if (primary_) {
      return true;
    }

    if (fd_ < 0) {
      do {
        fd_ = ::open(path_.c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                     security_.mode);
      } while (fd_ < 0 && errno == EINTR);
      if (fd_ < 0 || !apply_runtime_file_security(fd_, security_)) {
        if (fd_ >= 0) {
          (void)::close(fd_);
          fd_ = -1;
        }
        return false;
      }
    }

    int rc = -1;
    do {
      rc = ::flock(fd_, LOCK_EX | LOCK_NB);
    } while (rc != 0 && errno == EINTR);
    if (rc == 0) {
      primary_ = true;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool is_primary() const noexcept { return primary_; }

private:
  std::string path_;
  RuntimeFileSecurity security_;
  int fd_ = -1;
  bool primary_ = false;
};

inline bool read_shared_control_plane_state(
    SharedControlPlaneState &out,
    const std::string &path = std::string{kControlPlaneStatePath}) {
  const RuntimeFileSecurity security = default_runtime_file_security();
  const auto runtime_path = detail::split_runtime_path(path);
  if (!runtime_path) {
    return false;
  }

  const int directory_fd =
      detail::open_runtime_directory(*runtime_path, security);
  if (directory_fd < 0) {
    return false;
  }
  int fd = -1;
  do {
    fd = ::openat(directory_fd, runtime_path->name.c_str(),
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0 || !verify_runtime_file_security(fd, security)) {
    if (fd >= 0) {
      (void)::close(fd);
    }
    (void)::close(directory_fd);
    return false;
  }

  const auto payload = detail::read_bounded(fd);
  const bool close_ok = ::close(fd) == 0;
  (void)::close(directory_fd);
  if (!payload || !close_ok) {
    return false;
  }
  const auto parsed = detail::parse_control_plane_state(*payload);
  if (!parsed) {
    return false;
  }
  out = *parsed;
  return true;
}

inline SharedControlPlaneState seed_control_plane_generations(
    SharedControlPlaneState state,
    const std::string &path = std::string{kControlPlaneStatePath}) {
  SharedControlPlaneState previous;
  if (read_shared_control_plane_state(previous, path)) {
    state.config_generation = previous.config_generation;
    state.status_generation = previous.status_generation;
  }
  return state;
}

inline bool write_shared_control_plane_state(
    const SharedControlPlaneState &state,
    const std::string &path = std::string{kControlPlaneStatePath}) {
  if (!detail::valid_control_plane_path(state.config_path)) {
    return false;
  }
  const std::string payload = detail::serialize_control_plane_state(state);
  if (payload.size() > detail::kMaxControlPlaneStateBytes) {
    return false;
  }

  const RuntimeFileSecurity security = default_runtime_file_security();
  const auto runtime_path = detail::split_runtime_path(path);
  if (!runtime_path) {
    return false;
  }
  const int directory_fd =
      detail::open_runtime_directory(*runtime_path, security);
  if (directory_fd < 0) {
    return false;
  }

  int existing_fd = -1;
  do {
    existing_fd = ::openat(directory_fd, runtime_path->name.c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (existing_fd < 0 && errno == EINTR);
  if (existing_fd >= 0) {
    const bool existing_safe =
        verify_runtime_file_security(existing_fd, security);
    (void)::close(existing_fd);
    if (!existing_safe) {
      (void)::close(directory_fd);
      return false;
    }
  } else if (errno != ENOENT) {
    (void)::close(directory_fd);
    return false;
  }

  static std::atomic<std::uint64_t> sequence{0};
  int temp_fd = -1;
  std::string temp_name;
  for (unsigned int attempt = 0; attempt < 64U && temp_fd < 0; ++attempt) {
    const std::uint64_t nonce =
        sequence.fetch_add(1, std::memory_order_relaxed);
    temp_name = "." + runtime_path->name + ".tmp." +
                std::to_string(::getpid()) + "." + std::to_string(nonce);
    do {
      temp_fd =
          ::openat(directory_fd, temp_name.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (temp_fd < 0 && errno == EINTR);
    if (temp_fd < 0 && errno != EEXIST) {
      break;
    }
  }
  if (temp_fd < 0) {
    (void)::close(directory_fd);
    return false;
  }

  bool ok = apply_runtime_file_security(temp_fd, security) &&
            detail::write_all(temp_fd, payload) && ::fsync(temp_fd) == 0;
  if (::close(temp_fd) != 0) {
    ok = false;
  }
  if (ok) {
    ok = ::renameat(directory_fd, temp_name.c_str(), directory_fd,
                    runtime_path->name.c_str()) == 0;
  }
  if (ok) {
    ok = ::fsync(directory_fd) == 0;
  }
  if (!ok) {
    (void)::unlinkat(directory_fd, temp_name.c_str(), 0);
  }
  (void)::close(directory_fd);
  return ok;
}

} // namespace punto
