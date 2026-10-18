#include "punto/x11_session.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::abort();
  }
}

punto::X11SessionInfo session_info(const char *display,
                                   std::uint32_t uid) {
  punto::X11SessionInfo info;
  info.session_id = "isolated-session";
  info.username = "desktop";
  info.display = display;
  info.uid = uid;
  return info;
}

} // namespace

int main() {
  auto observed = session_info(":42", 1000);
  punto::X11Session session{[&] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, observed};
  }};
  require(!session.constrain_to_target("localhost:42", 1000),
          "remote X display cannot authorize ingress");
  require(!session.constrain_to_target(":42", 0),
          "root is not a desktop target");
  require(session.constrain_to_target(":42", 1000),
          "exact target can be fixed before discovery");
  require(session.initialize() && session.acquire_write_lease().has_value(),
          "matching session admits a write lease");
  require(!session.constrain_to_target(":43", 1000),
          "target cannot change after admission");

  observed = session_info(":43", 1000);
  require(session.refresh() == punto::X11Session::RefreshResult::Failed &&
              !session.acquire_write_lease().has_value(),
          "display migration revokes authority");
  observed = session_info(":42", 1001);
  require(session.refresh() == punto::X11Session::RefreshResult::Failed &&
              !session.acquire_write_lease().has_value(),
          "same display with another desktop uid remains denied");
  observed = session_info(":42", 1000);
  require(session.refresh() ==
              punto::X11Session::RefreshResult::HealthyUpdated &&
              session.acquire_write_lease().has_value(),
          "only original target can recover");
  std::cout << "punto-x11-target-binding: OK\n";
}
