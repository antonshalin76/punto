#pragma once

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace punto {

inline constexpr int kAccessibleRangeCharacters = 4096;
inline constexpr int kAccessibleRangeBytes = 4096;

enum class AccessibleStatus : std::uint32_t {
  Observed,
  Unavailable,
  Stale,
  Expired,
  Unknown
};

enum class AccessibleScope { Child, Range };

struct AccessibleRange {
  std::int32_t start = -1, end = -1;
  std::string sha256;
};

struct AccessibleRequest {
  std::uint32_t provider_pid =
      0; // Caller proves the active application/session.
  std::uint64_t deadline_us = 0; // Original CLOCK_MONOTONIC deadline.
  std::string expected_token;
  std::optional<AccessibleRange> expected_range;
  AccessibleScope scope = AccessibleScope::Range;
  bool preparation = false;
};

struct AccessibleObservation {
  AccessibleObservation() = default;
  explicit AccessibleObservation(AccessibleStatus value) : status(value) {}
  AccessibleStatus status = AccessibleStatus::Unavailable;
  std::uint64_t bus_epoch = 0, focus_generation = 0, observed_us = 0;
  std::uint32_t provider_pid = 0;
  std::string registry_owner, provider, object = "/", token;
  std::int32_t caret = -1, selection_start = -1, selection_end = -1;
  AccessibleRange range;
  GVariant *wire(std::string_view helper) const;
};

// Desktop-user GMainContext only; never initializes libatspi or mutates text.
class AccessibleContext {
public:
  using Completion = std::function<void(AccessibleObservation)>;
  AccessibleContext(GDBusConnection *session, std::string helper,
                    std::uint32_t uid);
  ~AccessibleContext();
  AccessibleContext(const AccessibleContext &) = delete;
  AccessibleContext &operator=(const AccessibleContext &) = delete;
  void observe(AccessibleRequest request, Completion completion);
  void cancel();
  void cancel_observation();

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace punto
