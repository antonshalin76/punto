#pragma once

#include <sys/types.h>

#include <memory>
#include <string>
#include <vector>

namespace punto {

enum class SessionRoute { direct, user_inventory };
enum class SessionResolutionStatus {
  ready,
  retryable_failure,
  permanent_failure
};

struct SessionSelection {
  SessionResolutionStatus status = SessionResolutionStatus::retryable_failure;
  SessionRoute route = SessionRoute::direct;
  std::string session;
  const char *stage = "session-unresolved";
};

class SessionQuery {
public:
  virtual ~SessionQuery() = default;
  virtual int session_for_pid(pid_t pid, std::string &session) = 0;
  virtual int sessions_for_uid(uid_t uid,
                               std::vector<std::string> &sessions) = 0;
  virtual int session_active(const std::string &session) = 0;
  virtual int session_remote(const std::string &session) = 0;
  virtual int session_uid(const std::string &session, uid_t &uid) = 0;
  virtual int session_type(const std::string &session, std::string &type) = 0;
  virtual int session_class(const std::string &session,
                            std::string &session_class) = 0;
  virtual int session_seat(const std::string &session, std::string &seat) = 0;
};

std::unique_ptr<SessionQuery> make_systemd_session_query();

class SessionResolver {
public:
  explicit SessionResolver(SessionQuery &query) : query_(query) {}

  SessionSelection resolve(uid_t authenticated_uid, pid_t shell_pid);
  bool revalidate(uid_t authenticated_uid, pid_t shell_pid,
                  const SessionSelection &selection);

private:
  enum class Eligibility { eligible, ineligible, disappeared, query_error };

  Eligibility eligibility(uid_t authenticated_uid, const std::string &session,
                          bool direct, const char **stage);
  SessionSelection inventory(uid_t authenticated_uid);

  SessionQuery &query_;
};

} // namespace punto
