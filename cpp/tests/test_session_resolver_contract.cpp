#include "punto/session_resolver.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Facts {
  uid_t uid = 1000;
  int active = 1;
  int remote = 0;
  std::string type = "x11";
  std::string session_class = "user";
  std::string seat = "seat0";
  int error = 0;
};

class FakeQuery final : public punto::SessionQuery {
public:
  int pid_result = -ENODATA;
  std::string pid_session;
  int inventory_result = 0;
  std::vector<std::string> inventory;
  std::map<std::string, Facts> facts;
  int inventory_calls = 0;

  int session_for_pid(pid_t, std::string &session) override {
    session = pid_session;
    return pid_result;
  }
  int sessions_for_uid(uid_t, std::vector<std::string> &sessions) override {
    ++inventory_calls;
    sessions = inventory;
    return inventory_result < 0 ? inventory_result
                                : static_cast<int>(sessions.size());
  }
  int session_active(const std::string &session) override {
    return get(session).error != 0 ? get(session).error : get(session).active;
  }
  int session_remote(const std::string &session) override {
    return get(session).error != 0 ? get(session).error : get(session).remote;
  }
  int session_uid(const std::string &session, uid_t &uid) override {
    const Facts &value = get(session);
    if (value.error != 0)
      return value.error;
    uid = value.uid;
    return 0;
  }
  int session_type(const std::string &session, std::string &type) override {
    const Facts &value = get(session);
    if (value.error != 0)
      return value.error;
    type = value.type;
    return 0;
  }
  int session_class(const std::string &session,
                    std::string &session_class) override {
    const Facts &value = get(session);
    if (value.error != 0)
      return value.error;
    session_class = value.session_class;
    return 0;
  }
  int session_seat(const std::string &session, std::string &seat) override {
    const Facts &value = get(session);
    if (value.error != 0)
      return value.error;
    seat = value.seat;
    return 0;
  }

private:
  const Facts &get(const std::string &session) const {
    const auto found = facts.find(session);
    if (found == facts.end()) {
      std::cerr << "missing fake session " << session << '\n';
      std::abort();
    }
    return found->second;
  }
};

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void test_user_manager_fallback_and_revalidation() {
  FakeQuery query;
  query.inventory = {"stale", "valid", "valid", "remote"};
  query.facts["stale"] = Facts{.error = -ENXIO};
  query.facts["valid"] = Facts{};
  query.facts["remote"] = Facts{.remote = 1};
  punto::SessionResolver resolver(query);
  const auto selected = resolver.resolve(1000, 42);
  require(selected.status == punto::SessionResolutionStatus::ready &&
              selected.route == punto::SessionRoute::user_inventory &&
              selected.session == "valid",
          "ENODATA must select the only distinct eligible session");
  require(resolver.revalidate(1000, 42, selected),
          "unchanged fallback identity must revalidate");
  query.inventory.push_back("second");
  query.facts["second"] = Facts{};
  require(!resolver.revalidate(1000, 42, selected),
          "new ambiguity must revoke a pinned fallback identity");
  query.inventory = {"second"};
  require(!resolver.revalidate(1000, 42, selected),
          "fallback revalidation must not retarget to another session");
}

void test_filtering_is_order_independent() {
  FakeQuery query;
  query.inventory = {"wrong-seat", "greeter",  "wayland",
                     "wrong-uid",  "inactive", "valid"};
  query.facts["wrong-seat"] = Facts{.seat = "seat1"};
  query.facts["greeter"] = Facts{.session_class = "greeter"};
  query.facts["wayland"] = Facts{.type = "wayland"};
  query.facts["wrong-uid"] = Facts{.uid = 1001};
  query.facts["inactive"] = Facts{.active = 0};
  query.facts["valid"] = Facts{};
  punto::SessionResolver resolver(query);
  require(resolver.resolve(1000, 42).session == "valid",
          "ineligible sessions must be filtered");
  std::reverse(query.inventory.begin(), query.inventory.end());
  require(resolver.resolve(1000, 42).session == "valid",
          "inventory order must not affect selection");
}

void test_zero_ambiguity_and_query_error_reject() {
  FakeQuery query;
  punto::SessionResolver resolver(query);
  require(resolver.resolve(1000, 42).status ==
              punto::SessionResolutionStatus::retryable_failure,
          "zero eligible sessions must reject");
  query.inventory = {"a", "b"};
  query.facts["a"] = Facts{};
  query.facts["b"] = Facts{};
  require(resolver.resolve(1000, 42).status ==
              punto::SessionResolutionStatus::retryable_failure,
          "two eligible sessions must reject");
  query.inventory = {"a", "broken"};
  query.facts["broken"] = Facts{.error = -EIO};
  require(resolver.resolve(1000, 42).status ==
              punto::SessionResolutionStatus::retryable_failure,
          "metadata errors must not manufacture uniqueness");
}

void test_direct_route_cannot_be_rescued() {
  FakeQuery query;
  query.pid_result = 0;
  query.pid_session = "direct";
  query.inventory = {"fallback"};
  query.facts["fallback"] = Facts{};
  query.facts["direct"] = Facts{};
  punto::SessionResolver resolver(query);
  const auto selected = resolver.resolve(1000, 42);
  require(selected.status == punto::SessionResolutionStatus::ready &&
              selected.route == punto::SessionRoute::direct &&
              query.inventory_calls == 0,
          "valid direct session must remain the primary route");
  query.facts["direct"].uid = 1001;
  require(resolver.resolve(1000, 42).status ==
                  punto::SessionResolutionStatus::permanent_failure &&
              query.inventory_calls == 0,
          "contradictory direct session must fail without fallback");
  query.pid_result = -EACCES;
  require(resolver.resolve(1000, 42).status ==
                  punto::SessionResolutionStatus::retryable_failure &&
              query.inventory_calls == 0,
          "non-ENODATA PID errors must not enter inventory fallback");
}

} // namespace

int main() {
  test_user_manager_fallback_and_revalidation();
  test_filtering_is_order_independent();
  test_zero_ambiguity_and_query_error_reject();
  test_direct_route_cannot_be_rescued();
  std::cout << "test_session_resolver_contract: OK\n";
  return 0;
}
