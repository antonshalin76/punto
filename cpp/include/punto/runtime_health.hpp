/**
 * @file runtime_health.hpp
 * @brief Clock-driven health policy for in-flight runtime work.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace punto {

enum class ComponentHealth { Ready, Degraded, Failed };

[[nodiscard]] constexpr std::string_view
component_health_name(ComponentHealth health) noexcept {
  switch (health) {
  case ComponentHealth::Ready:
    return "ready";
  case ComponentHealth::Degraded:
    return "degraded";
  case ComponentHealth::Failed:
    return "failed";
  }
  return "failed";
}

struct StallHealthSnapshot {
  ComponentHealth health = ComponentHealth::Ready;
  std::uint64_t last_progress_ms = 0;
  bool in_flight = false;
};

namespace detail {

[[nodiscard]] inline std::uint64_t
elapsed_ms(std::chrono::steady_clock::time_point origin,
           std::chrono::steady_clock::time_point value) {
  if (value <= origin) {
    return 0;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(value - origin)
          .count();
  using Count = std::remove_cv_t<decltype(elapsed)>;
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if constexpr (sizeof(Count) > sizeof(std::uint64_t)) {
    if (elapsed > static_cast<Count>(maximum)) {
      return maximum;
    }
  }
  return static_cast<std::uint64_t>(elapsed);
}

} // namespace detail

class ExplicitHealthPolicy {
public:
  using Clock = std::chrono::steady_clock;
  using SteadyNow = std::function<Clock::time_point()>;

  explicit ExplicitHealthPolicy(
      ComponentHealth initial, SteadyNow now = [] { return Clock::now(); })
      : now_{std::move(now)}, origin_{now_()}, last_progress_{origin_},
        health_{initial} {}

  void ready() noexcept {
    if (health_ != ComponentHealth::Failed) {
      health_ = ComponentHealth::Ready;
    }
  }

  void degrade() noexcept {
    if (health_ != ComponentHealth::Failed) {
      health_ = ComponentHealth::Degraded;
    }
  }

  void fail() noexcept { health_ = ComponentHealth::Failed; }

  void mark_progress() {
    if (health_ != ComponentHealth::Failed) {
      last_progress_ = now_();
    }
  }

  [[nodiscard]] StallHealthSnapshot snapshot() const {
    return {health_, detail::elapsed_ms(origin_, last_progress_), false};
  }

private:
  SteadyNow now_;
  Clock::time_point origin_;
  Clock::time_point last_progress_;
  ComponentHealth health_;
};

class StallHealthPolicy {
public:
  using Clock = std::chrono::steady_clock;
  using SteadyNow = std::function<Clock::time_point()>;

  explicit StallHealthPolicy(
      SteadyNow now = [] { return Clock::now(); },
      std::chrono::milliseconds stall_after = std::chrono::milliseconds{2000})
      : now_{std::move(now)}, stall_after_{stall_after}, origin_{now_()},
        last_progress_{origin_} {}

  void begin(Clock::time_point accepted_at) noexcept {
    in_flight_since_ = accepted_at;
  }

  void clear_in_flight() noexcept { in_flight_since_.reset(); }

  void mark_progress() {
    if (!failed_) {
      last_progress_ = now_();
    }
  }

  void fail() noexcept { failed_ = true; }

  [[nodiscard]] StallHealthSnapshot snapshot() const {
    const Clock::time_point current = now_();
    ComponentHealth health = ComponentHealth::Ready;
    if (failed_) {
      health = ComponentHealth::Failed;
    } else if (in_flight_since_.has_value() && current >= *in_flight_since_ &&
               current - *in_flight_since_ >= stall_after_) {
      health = ComponentHealth::Degraded;
    }
    return {health, detail::elapsed_ms(origin_, last_progress_),
            in_flight_since_.has_value()};
  }

private:
  SteadyNow now_;
  std::chrono::milliseconds stall_after_;
  Clock::time_point origin_;
  Clock::time_point last_progress_;
  std::optional<Clock::time_point> in_flight_since_;
  bool failed_ = false;
};

} // namespace punto
