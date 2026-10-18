#pragma once

#include "punto/accessible_context.hpp"
#include "punto/x11_session.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace punto {

enum class DesktopLayoutStatus {
  Rejected,
  RequestedUnresolved,
  Verified,
  Applied,
};

struct DesktopLayoutResult {
  DesktopLayoutStatus status = DesktopLayoutStatus::Rejected;
  int observed_layout = -1;
  std::string_view stage = "invalid_request";
};

class DesktopLayoutClient {
public:
  using WaitFunction =
      std::function<bool(std::chrono::steady_clock::time_point)>;

  DesktopLayoutClient();
  ~DesktopLayoutClient();
  DesktopLayoutClient(const DesktopLayoutClient &) = delete;
  DesktopLayoutClient &operator=(const DesktopLayoutClient &) = delete;

  [[nodiscard]] DesktopLayoutResult
  switch_to(const X11SessionInfo &session, int source_layout, int target_layout,
            std::chrono::steady_clock::time_point hard_deadline,
            const WaitFunction &wait);
  [[nodiscard]] AccessibleObservation observe_editor(
      const X11SessionInfo &session, std::uint32_t provider_pid,
      std::chrono::steady_clock::time_point hard_deadline,
      const WaitFunction &wait, std::string_view expected_token = {},
      const std::optional<AccessibleRange> &expected_range = std::nullopt,
      AccessibleScope scope = AccessibleScope::Range, bool preparation = false);
  void reset() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace punto
