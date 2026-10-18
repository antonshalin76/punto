#include "punto/session_resolver.hpp"

#include <systemd/sd-login.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace punto {
namespace {

constexpr std::size_t kMaxInventorySessions = 64;

bool disappeared(int result) { return result == -ENXIO || result == -ENOENT; }

class SystemdSessionQuery final : public SessionQuery {
public:
  int session_for_pid(pid_t pid, std::string &session) override {
    char *value = nullptr;
    const int result = sd_pid_get_session(pid, &value);
    if (result >= 0 && value != nullptr)
      session = value;
    std::free(value);
    return result;
  }

  int sessions_for_uid(uid_t uid, std::vector<std::string> &sessions) override {
    char **values = nullptr;
    const int result = sd_uid_get_sessions(uid, 0, &values);
    if (result < 0)
      return result;
    if (static_cast<std::size_t>(result) > kMaxInventorySessions) {
      release(values);
      return -E2BIG;
    }
    for (int index = 0; index < result; ++index) {
      if (values == nullptr || values[index] == nullptr) {
        release(values);
        return -EBADMSG;
      }
      sessions.emplace_back(values[index]);
    }
    release(values);
    return result;
  }

  int session_active(const std::string &session) override {
    return sd_session_is_active(session.c_str());
  }
  int session_remote(const std::string &session) override {
    return sd_session_is_remote(session.c_str());
  }
  int session_uid(const std::string &session, uid_t &uid) override {
    return sd_session_get_uid(session.c_str(), &uid);
  }
  int session_type(const std::string &session, std::string &type) override {
    return string_query(sd_session_get_type, session, type);
  }
  int session_class(const std::string &session,
                    std::string &session_class) override {
    return string_query(sd_session_get_class, session, session_class);
  }
  int session_seat(const std::string &session, std::string &seat) override {
    return string_query(sd_session_get_seat, session, seat);
  }

private:
  static void release(char **values) {
    if (values == nullptr)
      return;
    for (char **cursor = values; *cursor != nullptr; ++cursor)
      std::free(*cursor);
    std::free(values);
  }

  static int string_query(int (*query)(const char *, char **),
                          const std::string &session, std::string &value) {
    char *native = nullptr;
    const int result = query(session.c_str(), &native);
    if (result >= 0 && native != nullptr)
      value = native;
    std::free(native);
    return result;
  }
};

#ifdef PUNTO_TEST_SD_LOGIN
class TestSessionQuery final : public SessionQuery {
public:
  int session_for_pid(pid_t, std::string &session) override {
    const std::string mode = current_mode();
    if (mode == "lookup-error")
      return -EACCES;
    if (mode == "direct" || mode == "direct-contradictory") {
      session = "punto-direct";
      return 0;
    }
    return -ENODATA;
  }

  int sessions_for_uid(uid_t, std::vector<std::string> &sessions) override {
    const std::string mode = current_mode();
    if (mode == "inventory-error")
      return -EIO;
    if (mode == "zero" || mode == "inactive")
      sessions = {"punto-test-session"};
    else if (mode == "ambiguous")
      sessions = {"punto-test-session", "punto-second-session"};
    else if (mode == "metadata-error")
      sessions = {"punto-test-session", "punto-broken-session"};
    else
      sessions = {"punto-stale-session", "punto-test-session"};
    if (mode == "zero")
      sessions.clear();
    return static_cast<int>(sessions.size());
  }

  int session_active(const std::string &session) override {
    if (current_mode() == "disappeared" && session == "punto-test-session")
      return -ENXIO;
    if (error_for(session) != 0)
      return error_for(session);
    return current_mode() == "inactive" ? 0 : 1;
  }
  int session_remote(const std::string &session) override {
    if (error_for(session) != 0)
      return error_for(session);
    return current_mode() == "remote" ? 1 : 0;
  }
  int session_uid(const std::string &session, uid_t &uid) override {
    if (error_for(session) != 0)
      return error_for(session);
    uid = current_mode() == "wrong-uid" ||
                  current_mode() == "direct-contradictory"
              ? static_cast<uid_t>(getuid() + 1)
              : getuid();
    return 0;
  }
  int session_type(const std::string &session, std::string &type) override {
    if (error_for(session) != 0)
      return error_for(session);
    type = current_mode() == "wayland" ? "wayland" : "x11";
    return 0;
  }
  int session_class(const std::string &session,
                    std::string &session_class) override {
    if (error_for(session) != 0)
      return error_for(session);
    session_class = current_mode() == "greeter" ? "greeter" : "user";
    return 0;
  }
  int session_seat(const std::string &session, std::string &seat) override {
    if (error_for(session) != 0)
      return error_for(session);
    seat = current_mode() == "wrong-seat" ? "seat1" : "seat0";
    return 0;
  }

private:
  static std::string current_mode() {
    const char *path = std::getenv("PUNTO_TEST_SESSION_STATE_FILE");
    if (path != nullptr) {
      FILE *stream = std::fopen(path, "r");
      if (stream != nullptr) {
        char buffer[64] = {};
        const std::size_t size =
            std::fread(buffer, 1, sizeof(buffer) - 1, stream);
        std::fclose(stream);
        std::string value(buffer, size);
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
          value.pop_back();
        if (!value.empty())
          return value;
      }
    }
    const char *mode = std::getenv("PUNTO_TEST_SESSION_MODE");
    return mode == nullptr ? "user-manager" : mode;
  }

  static int error_for(const std::string &session) {
    if (session == "punto-stale-session")
      return -ENXIO;
    if (session == "punto-broken-session")
      return -EIO;
    return 0;
  }
};
#endif

} // namespace

std::unique_ptr<SessionQuery> make_systemd_session_query() {
#ifdef PUNTO_TEST_SD_LOGIN
  return std::make_unique<TestSessionQuery>();
#else
  return std::make_unique<SystemdSessionQuery>();
#endif
}

SessionResolver::Eligibility
SessionResolver::eligibility(uid_t authenticated_uid,
                             const std::string &session, bool direct,
                             const char **stage) {
  uid_t session_uid = static_cast<uid_t>(-1);
  int result = query_.session_uid(session, session_uid);
  if (disappeared(result))
    return Eligibility::disappeared;
  if (result < 0) {
    *stage = "session-uid-query";
    return Eligibility::query_error;
  }
  if (session_uid != authenticated_uid) {
    *stage = direct ? "direct-session-uid" : "inventory-session-uid";
    return Eligibility::ineligible;
  }

  result = query_.session_active(session);
  if (disappeared(result))
    return Eligibility::disappeared;
  if (result < 0) {
    *stage = "session-active-query";
    return Eligibility::query_error;
  }
  if (result == 0) {
    *stage = direct ? "direct-session-inactive" : "inventory-session-inactive";
    return Eligibility::ineligible;
  }

  result = query_.session_remote(session);
  if (disappeared(result))
    return Eligibility::disappeared;
  if (result < 0) {
    *stage = "session-remote-query";
    return Eligibility::query_error;
  }
  if (result != 0) {
    *stage = direct ? "direct-session-remote" : "inventory-session-remote";
    return Eligibility::ineligible;
  }

  std::string value;
  result = query_.session_type(session, value);
  if (disappeared(result))
    return Eligibility::disappeared;
  if (result == -ENODATA) {
    *stage = direct ? "direct-session-type" : "inventory-session-type";
    return Eligibility::ineligible;
  }
  if (result < 0 || value.empty()) {
    *stage = "session-type-query";
    return Eligibility::query_error;
  }
  if (value != "x11") {
    *stage = direct ? "direct-session-type" : "inventory-session-type";
    return Eligibility::ineligible;
  }

  value.clear();
  result = query_.session_class(session, value);
  if (disappeared(result))
    return Eligibility::disappeared;
  if (result == -ENODATA) {
    *stage = direct ? "direct-session-class" : "inventory-session-class";
    return Eligibility::ineligible;
  }
  if (result < 0 || value.empty()) {
    *stage = "session-class-query";
    return Eligibility::query_error;
  }
  if (value != "user") {
    *stage = direct ? "direct-session-class" : "inventory-session-class";
    return Eligibility::ineligible;
  }

  value.clear();
  result = query_.session_seat(session, value);
  if (disappeared(result))
    return Eligibility::disappeared;
  if (result == -ENODATA) {
    *stage = direct ? "direct-session-seat" : "inventory-session-seat";
    return Eligibility::ineligible;
  }
  if (result < 0 || value.empty()) {
    *stage = "session-seat-query";
    return Eligibility::query_error;
  }
  if (value != "seat0") {
    *stage = direct ? "direct-session-seat" : "inventory-session-seat";
    return Eligibility::ineligible;
  }
  return Eligibility::eligible;
}

SessionSelection SessionResolver::inventory(uid_t authenticated_uid) {
  std::vector<std::string> sessions;
  const int result = query_.sessions_for_uid(authenticated_uid, sessions);
  if (result < 0 || sessions.size() > kMaxInventorySessions)
    return {SessionResolutionStatus::retryable_failure,
            SessionRoute::user_inventory,
            {},
            "inventory-query"};

  std::set<std::string> distinct(sessions.begin(), sessions.end());
  std::vector<std::string> eligible;
  for (const std::string &candidate : distinct) {
    if (candidate.empty())
      return {SessionResolutionStatus::retryable_failure,
              SessionRoute::user_inventory,
              {},
              "inventory-malformed"};
    const char *stage = "inventory-eligibility";
    const Eligibility state =
        eligibility(authenticated_uid, candidate, false, &stage);
    if (state == Eligibility::query_error)
      return {SessionResolutionStatus::retryable_failure,
              SessionRoute::user_inventory,
              {},
              stage};
    if (state == Eligibility::eligible)
      eligible.push_back(candidate);
  }
  if (eligible.size() != 1)
    return {SessionResolutionStatus::retryable_failure,
            SessionRoute::user_inventory,
            {},
            eligible.empty() ? "inventory-no-match" : "inventory-ambiguous"};
  return {SessionResolutionStatus::ready, SessionRoute::user_inventory,
          eligible.front(), "inventory-selected"};
}

SessionSelection SessionResolver::resolve(uid_t authenticated_uid,
                                          pid_t shell_pid) {
  std::string direct_session;
  const int result = query_.session_for_pid(shell_pid, direct_session);
  if (result == -ENODATA)
    return inventory(authenticated_uid);
  if (result < 0 || direct_session.empty())
    return {SessionResolutionStatus::retryable_failure,
            SessionRoute::direct,
            {},
            "pid-session-query"};

  const char *stage = "direct-eligibility";
  const Eligibility state =
      eligibility(authenticated_uid, direct_session, true, &stage);
  if (state == Eligibility::eligible)
    return {SessionResolutionStatus::ready, SessionRoute::direct,
            direct_session, "direct-selected"};
  const bool permanent = state == Eligibility::ineligible &&
                         stage != std::string_view("direct-session-inactive");
  return {permanent ? SessionResolutionStatus::permanent_failure
                    : SessionResolutionStatus::retryable_failure,
          SessionRoute::direct,
          {},
          stage};
}

bool SessionResolver::revalidate(uid_t authenticated_uid, pid_t shell_pid,
                                 const SessionSelection &selection) {
  if (selection.status != SessionResolutionStatus::ready ||
      selection.session.empty())
    return false;
  std::string direct_session;
  const int direct_result = query_.session_for_pid(shell_pid, direct_session);
  if (selection.route == SessionRoute::direct) {
    if (direct_result < 0 || direct_session != selection.session)
      return false;
    const char *stage = "direct-revalidation";
    return eligibility(authenticated_uid, selection.session, true, &stage) ==
           Eligibility::eligible;
  }
  if (direct_result != -ENODATA)
    return false;
  const SessionSelection current = inventory(authenticated_uid);
  return current.status == SessionResolutionStatus::ready &&
         current.route == SessionRoute::user_inventory &&
         current.session == selection.session;
}

} // namespace punto
