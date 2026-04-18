/**
 * @file control_plane_state.hpp
 * @brief Shared runtime state for primary/secondary punto-daemon roles
 */

#pragma once

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace punto {

inline constexpr const char *kControlPlaneLockPath = "/var/run/punto-control.lock";
inline constexpr const char *kControlPlaneStatePath = "/var/run/punto-control.state";

struct SharedControlPlaneState {
  std::uint64_t config_generation = 0;
  std::uint64_t status_generation = 0;
  bool enabled = true;
  std::string config_path;
};

namespace detail {

inline void apply_runtime_file_permissions(const std::string &path) {
  const int chmod_rc = ::chmod(path.c_str(), 0660);
  (void)chmod_rc;
  if (group *grp = ::getgrnam("punto"); grp != nullptr) {
    const int chown_rc = ::chown(path.c_str(), 0, grp->gr_gid);
    (void)chown_rc;
  }
}

inline std::string_view trim_ascii(std::string_view value) {
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\n' || value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\n' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return value;
}

inline std::optional<std::uint64_t> parse_u64(std::string_view value) {
  value = trim_ascii(value);
  std::uint64_t parsed = 0;
  auto [ptr, ec] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (ec == std::errc{} && ptr == value.data() + value.size()) {
    return parsed;
  }
  return std::nullopt;
}

inline std::optional<bool> parse_bool(std::string_view value) {
  value = trim_ascii(value);
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return std::nullopt;
}

} // namespace detail

class ControlPlaneLease {
public:
  explicit ControlPlaneLease(std::string path = std::string{kControlPlaneLockPath})
      : path_{std::move(path)} {}

  ~ControlPlaneLease() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  [[nodiscard]] bool try_acquire() {
    if (primary_) {
      return true;
    }

    if (fd_ < 0) {
      fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0660);
      if (fd_ < 0) {
        return false;
      }
      detail::apply_runtime_file_permissions(path_);
    }

    if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
      primary_ = true;
      return true;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return false;
    }

    return false;
  }

  [[nodiscard]] bool is_primary() const noexcept { return primary_; }

private:
  std::string path_;
  int fd_ = -1;
  bool primary_ = false;
};

inline bool read_shared_control_plane_state(
    SharedControlPlaneState &out,
    const std::string &path = std::string{kControlPlaneStatePath}) {
  std::ifstream input{path};
  if (!input.is_open()) {
    return false;
  }

  SharedControlPlaneState parsed;
  std::string line;
  while (std::getline(input, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }

    const std::string_view key = detail::trim_ascii(std::string_view{line}.substr(0, pos));
    const std::string_view value =
        detail::trim_ascii(std::string_view{line}.substr(pos + 1));

    if (key == "config_generation") {
      if (auto parsed_value = detail::parse_u64(value)) {
        parsed.config_generation = *parsed_value;
      }
    } else if (key == "status_generation") {
      if (auto parsed_value = detail::parse_u64(value)) {
        parsed.status_generation = *parsed_value;
      }
    } else if (key == "enabled") {
      if (auto parsed_value = detail::parse_bool(value)) {
        parsed.enabled = *parsed_value;
      }
    } else if (key == "config_path") {
      parsed.config_path.assign(value.begin(), value.end());
    }
  }

  out = std::move(parsed);
  return true;
}

inline bool write_shared_control_plane_state(
    const SharedControlPlaneState &state,
    const std::string &path = std::string{kControlPlaneStatePath}) {
  const std::string temp_path =
      path + ".tmp." + std::to_string(::getpid());

  {
    std::ofstream output{temp_path, std::ios::trunc};
    if (!output.is_open()) {
      return false;
    }

    output << "config_generation=" << state.config_generation << '\n';
    output << "status_generation=" << state.status_generation << '\n';
    output << "enabled=" << (state.enabled ? 1 : 0) << '\n';
    output << "config_path=" << state.config_path << '\n';
    output.flush();
    if (!output.good()) {
      output.close();
      (void)::unlink(temp_path.c_str());
      return false;
    }
  }

  detail::apply_runtime_file_permissions(temp_path);
  if (::rename(temp_path.c_str(), path.c_str()) != 0) {
    (void)::unlink(temp_path.c_str());
    return false;
  }

  detail::apply_runtime_file_permissions(path);
  return true;
}

} // namespace punto
