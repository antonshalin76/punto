/**
 * @file runtime_file.hpp
 * @brief Security checks shared by Punto runtime files.
 */

#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string_view>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace punto {

struct RuntimeFileSecurity {
  uid_t owner_uid = 0;
  gid_t group_gid = static_cast<gid_t>(-1);
  mode_t mode = 0660;
};

inline std::optional<gid_t> lookup_runtime_group() {
  static const std::optional<gid_t> cached = []() -> std::optional<gid_t> {
    auto parse = [](std::string_view value) -> std::optional<gid_t> {
      while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
      }
      std::uintmax_t parsed = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (value.empty() || error != std::errc{} ||
          end != value.data() + value.size() ||
          parsed >
              static_cast<std::uintmax_t>(std::numeric_limits<gid_t>::max())) {
        return std::nullopt;
      }
      return static_cast<gid_t>(parsed);
    };

    if (const char *environment = std::getenv("PUNTO_RUNTIME_GID")) {
      if (auto parsed = parse(environment)) {
        return parsed;
      }
      return std::nullopt;
    }

    constexpr const char *path = "/etc/punto/runtime-gid";
    int fd = -1;
    do {
      fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
      struct stat metadata {};
      char bytes[32]{};
      const ssize_t count = ::read(fd, bytes, sizeof(bytes));
      const bool safe = ::fstat(fd, &metadata) == 0 &&
                        S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
                        metadata.st_uid == 0 &&
                        (metadata.st_mode & 0022) == 0 && count > 0 &&
                        static_cast<std::size_t>(count) < sizeof(bytes);
      (void)::close(fd);
      if (safe) {
        return parse(std::string_view{bytes, static_cast<std::size_t>(count)});
      }
    }

    // Non-root developer/test runs cannot create privileged runtime artifacts;
    // their own effective group is the only safe local identity.
    if (::geteuid() != 0) {
      return ::getegid();
    }
    return std::nullopt;
  }();
  return cached;
}

inline RuntimeFileSecurity default_runtime_file_security() {
  RuntimeFileSecurity security;
  security.owner_uid = ::geteuid();
  security.group_gid = lookup_runtime_group().value_or(static_cast<gid_t>(-1));
  return security;
}

inline bool
runtime_file_metadata_is_safe(const struct stat &metadata,
                              const RuntimeFileSecurity &security) noexcept {
  return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
         metadata.st_uid == security.owner_uid &&
         metadata.st_gid == security.group_gid &&
         (metadata.st_mode & 0777) == security.mode;
}

inline bool
verify_runtime_file_security(int fd,
                             const RuntimeFileSecurity &security) noexcept {
  struct stat metadata {};
  return fd >= 0 && ::fstat(fd, &metadata) == 0 &&
         runtime_file_metadata_is_safe(metadata, security);
}

inline bool
apply_runtime_file_security(int fd,
                            const RuntimeFileSecurity &security) noexcept {
  if (fd < 0) {
    return false;
  }

  struct stat metadata {};
  if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_nlink != 1) {
    return false;
  }

  if ((metadata.st_uid != security.owner_uid ||
       metadata.st_gid != security.group_gid) &&
      ::fchown(fd, security.owner_uid, security.group_gid) != 0) {
    return false;
  }
  if (::fchmod(fd, security.mode) != 0) {
    return false;
  }
  return verify_runtime_file_security(fd, security);
}

} // namespace punto
