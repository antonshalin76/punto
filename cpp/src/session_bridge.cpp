#include <gio/gio.h>
#include <glib-unix.h>

#include "punto/accessible_context.hpp"
#include "punto/session_resolver.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#ifndef PUNTO_PROTOCOL_PATH
#error "PUNTO_PROTOCOL_PATH is required"
#endif

namespace {

constexpr guint kProtocolVersion = 1;
constexpr guint kOutcomeState = 0;
constexpr guint kOutcomeLocked = 15;
constexpr guint kOutcomeFailed = 17;
constexpr guint kOutcomeHelperStale = 20;
constexpr guint kOutcomeDeadlineExpired = 21;
constexpr guint kOutcomeHopUnknown = 22;
constexpr guchar kUnknownRole = 255;
constexpr guint64 kSwitchHorizonUs = 1'000'000;
constexpr guint64 kObserverHorizonUs = 100'000;
constexpr std::size_t kMaxConnections = 4;
constexpr gsize kMaxMessageBytes = 1024;

constexpr char kBusName[] = "org.punto.Switcher.SessionBridge1";
constexpr char kShellName[] = "org.gnome.Shell";
constexpr char kExtensionPath[] = "/org/punto/Switcher/InputSource1";
constexpr char kExtensionInterface[] = "org.punto.Switcher.InputSource1";
constexpr char kPeerPath[] = "/org/punto/Switcher/SessionBridge1";
constexpr char kPeerInterface[] = "org.punto.Switcher.SessionBridge1";
constexpr char kObserverPath[] = "/org/punto/Switcher/DesktopObserver1";
constexpr char kObserverInterface[] = "org.punto.Switcher.DesktopObserver1";
constexpr char kDaemonExe[] = "/usr/bin/punto-daemon";
constexpr char kDaemonComm[] = "punto-daemon";
constexpr char kDaemonCgroup[] = "0::/system.slice/udevmon.service";
constexpr std::size_t kProcFileLimit = 4096;

uid_t helper_uid() {
#ifdef PUNTO_TEST_SD_LOGIN
  const char *offset = g_getenv("PUNTO_TEST_HELPER_UID_OFFSET");
  if (offset != nullptr && std::strcmp(offset, "1") == 0)
    return static_cast<uid_t>(getuid() + 1);
#endif
  return getuid();
}

bool is_lower_hex(std::string_view value, std::size_t size) {
  if (value.size() != size)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f')))
      return false;
  }
  return true;
}

std::string make_id() {
  gchar *uuid = g_uuid_string_random();
  if (uuid == nullptr)
    return {};
  std::string result;
  result.reserve(32);
  for (const char *cursor = uuid; *cursor != '\0'; ++cursor) {
    if (*cursor != '-')
      result.push_back(*cursor);
  }
  g_free(uuid);
  return is_lower_hex(result, 32) ? result : std::string{};
}

guint64 now_us() {
  const gint64 value = g_get_monotonic_time();
  return value < 0 ? 0 : static_cast<guint64>(value);
}

bool valid_deadline(guint64 deadline, guint64 now) {
  return deadline > now && deadline - now <= kSwitchHorizonUs;
}

bool wait_test_pre_publish_barrier() {
#ifdef PUNTO_TEST_SD_LOGIN
  const char *ready = g_getenv("PUNTO_TEST_PRE_PUBLISH_READY");
  const char *release = g_getenv("PUNTO_TEST_PRE_PUBLISH_RELEASE");
  if (ready == nullptr || release == nullptr)
    return true;
  if (!g_file_set_contents(ready, "ready\n", -1, nullptr))
    return false;
  const guint64 deadline = now_us() + 2'000'000;
  while (!g_file_test(release, G_FILE_TEST_EXISTS)) {
    if (now_us() >= deadline)
      return false;
    g_usleep(1'000);
  }
#endif
  return true;
}

bool valid_outcome(guint outcome) {
  return outcome <= 2 || (outcome >= 10 && outcome <= kOutcomeHopUnknown);
}

int remaining_timeout_ms(guint64 deadline) {
  const guint64 now = now_us();
  if (deadline <= now)
    return 0;
  const guint64 remaining = deadline - now;
  return static_cast<int>((remaining + 999) / 1000);
}

std::optional<bool> enroll_accessibility(uid_t uid) {
  const char *directory = g_getenv("STATE_DIRECTORY");
  if (directory == nullptr || directory[0] != '/' ||
      std::strchr(directory, ':') != nullptr)
    return std::nullopt;
  const int directory_fd =
      open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_fd < 0)
    return std::nullopt;
  struct stat directory_stat {};
  if (fstat(directory_fd, &directory_stat) != 0 ||
      directory_stat.st_uid != uid || (directory_stat.st_mode & 0777) != 0700) {
    close(directory_fd);
    return std::nullopt;
  }
  constexpr char marker[] = "accessibility-enrolled";
  int fd = openat(directory_fd, marker,
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  const bool first_activation = fd >= 0;
  if (fd < 0 && errno == EEXIST)
    fd = openat(directory_fd, marker,
                O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    close(directory_fd);
    return std::nullopt;
  }
  struct stat marker_stat {};
  bool ok = fstat(fd, &marker_stat) == 0 && S_ISREG(marker_stat.st_mode) &&
            marker_stat.st_uid == uid && (marker_stat.st_mode & 0777) == 0600 &&
            marker_stat.st_size == 0 && marker_stat.st_nlink == 1;
  // Consume permission before Set. Unknown enablement must not renew permission
  // on restart and override a subsequent user opt-out.
  if (ok && first_activation)
    ok = fsync(fd) == 0 && fsync(directory_fd) == 0;
  const int close_result = close(fd);
  const int directory_close_result = close(directory_fd);
  if (!ok || close_result != 0 || directory_close_result != 0)
    return std::nullopt;
  return first_activation;
}

bool write_all(int fd, std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t written =
        write(fd, value.data() + offset, value.size() - offset);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

std::optional<unsigned long long> process_start_time() {
  std::ifstream stream("/proc/self/stat");
  std::string line;
  if (!std::getline(stream, line))
    return std::nullopt;
  const std::size_t end = line.rfind(')');
  if (end == std::string::npos || end + 2 >= line.size())
    return std::nullopt;
  std::istringstream fields(line.substr(end + 2));
  std::string token;
  for (int field = 3; field <= 22; ++field) {
    if (!(fields >> token))
      return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const auto result = std::stoull(token, &consumed, 10);
    if (consumed != token.size())
      return std::nullopt;
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::string proc_root_path() {
#ifdef PUNTO_TEST_SD_LOGIN
  const char *override = g_getenv("PUNTO_TEST_PROC_ROOT");
  if (override != nullptr) {
    if (override[0] == '/' && std::strchr(override, '\n') == nullptr &&
        std::strchr(override, '\r') == nullptr)
      return override;
    return {};
  }
#endif
  return "/proc";
}

const char *daemon_exe_path() {
#ifdef PUNTO_TEST_SD_LOGIN
  const char *override = g_getenv("PUNTO_TEST_DAEMON_EXE");
  if (override != nullptr && override[0] == '/' &&
      std::strchr(override, '\n') == nullptr &&
      std::strchr(override, '\r') == nullptr)
    return override;
#endif
  return kDaemonExe;
}

std::string_view trim_one_newline(std::string_view value) {
  if (!value.empty() && value.back() == '\n')
    value.remove_suffix(1);
  return value;
}

bool skip_horizontal_space(std::string_view &cursor) {
  while (!cursor.empty() && (cursor.front() == ' ' || cursor.front() == '\t'))
    cursor.remove_prefix(1);
  return true;
}

bool parse_u64_token(std::string_view &cursor, unsigned long long &value) {
  skip_horizontal_space(cursor);
  if (cursor.empty() || cursor.front() < '0' || cursor.front() > '9')
    return false;
  const char *begin = cursor.data();
  const char *end = begin + cursor.size();
  unsigned long long parsed = 0;
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr == begin)
    return false;
  cursor.remove_prefix(static_cast<std::size_t>(result.ptr - begin));
  value = parsed;
  return true;
}

bool finish_uid_map_line(std::string_view &cursor) {
  skip_horizontal_space(cursor);
  if (cursor.empty())
    return true;
  if (cursor.front() != '\n')
    return false;
  cursor.remove_prefix(1);
  return true;
}

bool parse_uid_map_line(std::string_view &cursor, unsigned long long &ns_start,
                        unsigned long long &host_start,
                        unsigned long long &count) {
  if (!parse_u64_token(cursor, ns_start) ||
      !parse_u64_token(cursor, host_start) || !parse_u64_token(cursor, count) ||
      count == 0 || !finish_uid_map_line(cursor))
    return false;
  return true;
}

bool uid_fits(unsigned long long value) {
  return value <= std::numeric_limits<uid_t>::max();
}

bool uid_map_maps_ns0_to_host_root(std::string_view text) {
  if (text.empty())
    return false;
  bool mapped = false;
  while (!text.empty()) {
    unsigned long long ns_start = 0;
    unsigned long long host_start = 0;
    unsigned long long count = 0;
    if (!parse_uid_map_line(text, ns_start, host_start, count))
      return false;
    if (ns_start == 0 && host_start == 0)
      mapped = true;
  }
  return mapped;
}

bool uid_map_is_exact_single_self(std::string_view text, uid_t self_uid) {
  unsigned long long ns_start = 0;
  unsigned long long host_start = 0;
  unsigned long long count = 0;
  if (text.empty() || !parse_uid_map_line(text, ns_start, host_start, count) ||
      !text.empty() || !uid_fits(ns_start) || !uid_fits(host_start) ||
      count != 1)
    return false;
  return static_cast<uid_t>(ns_start) == self_uid &&
         static_cast<uid_t>(host_start) == self_uid;
}

bool read_bounded_file(const std::string &path, std::size_t max_bytes,
                       std::string &out) {
  out.clear();
  int fd = -1;
  do {
    fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0)
    return false;
  struct stat metadata {};
  if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    close(fd);
    return false;
  }
  std::string buffer(max_bytes + 1, '\0');
  std::size_t filled = 0;
  while (filled <= max_bytes) {
    const ssize_t count =
        read(fd, buffer.data() + filled, buffer.size() - filled);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      return false;
    }
    if (count == 0)
      break;
    filled += static_cast<std::size_t>(count);
    if (filled > max_bytes) {
      close(fd);
      return false;
    }
  }
  close(fd);
  buffer.resize(filled);
  out = std::move(buffer);
  return true;
}

std::optional<uid_t> read_overflow_uid(const std::string &proc_root) {
  if (proc_root.empty())
    return std::nullopt;
  std::string text;
  if (!read_bounded_file(proc_root + "/sys/kernel/overflowuid", 32, text))
    return std::nullopt;
  const std::string_view trimmed = trim_one_newline(text);
  if (trimmed.empty() || (!trimmed.empty() && trimmed.back() == '\r'))
    return std::nullopt;
  std::string_view cursor = trimmed;
  unsigned long long value = 0;
  if (!parse_u64_token(cursor, value) || !uid_fits(value))
    return std::nullopt;
  skip_horizontal_space(cursor);
  if (!cursor.empty())
    return std::nullopt;
  const uid_t overflow = static_cast<uid_t>(value);
  if (overflow == 0)
    return std::nullopt;
  return overflow;
}

bool cmdline_is_daemon_exe(std::string_view cmdline,
                           std::string_view daemon_path) {
  const std::size_t path_size = daemon_path.size();
  if (cmdline.size() < path_size || cmdline.substr(0, path_size) != daemon_path)
    return false;
  for (std::size_t index = path_size; index < cmdline.size(); ++index) {
    if (cmdline[index] != '\0')
      return false;
  }
  return true;
}

bool daemon_file_protected(const struct stat &metadata, uid_t overflow) {
  if (!S_ISREG(metadata.st_mode) || (metadata.st_mode & 022) != 0)
    return false;
  if (metadata.st_uid == 0)
    return true;
  if (metadata.st_uid == overflow)
    return true;
#ifdef PUNTO_TEST_SD_LOGIN
  if (g_getenv("PUNTO_TEST_DAEMON_EXE") != nullptr &&
      metadata.st_uid == getuid())
    return true;
#endif
  return false;
}

bool peer_exe_identity_ok(const std::string &exe_path,
                          std::string_view daemon_path) {
  char target[4096];
  const ssize_t length = readlink(exe_path.c_str(), target, sizeof(target));
  if (length < 0)
    return errno == EACCES || errno == EPERM;
  if (static_cast<std::size_t>(length) >= sizeof(target))
    return false;
  return std::string_view(target, static_cast<std::size_t>(length)) ==
         daemon_path;
}

bool peer_is_genuine_root_daemon(pid_t peer_pid, uid_t overflow) {
  if (peer_pid <= 0)
    return false;
  const std::string proc_root = proc_root_path();
  if (proc_root.empty())
    return false;
  const std::string peer_dir =
      proc_root + "/" +
      std::to_string(static_cast<unsigned long long>(peer_pid));
  const std::string_view daemon_path = daemon_exe_path();
  if (daemon_path.empty())
    return false;
  struct stat peer_dir_stat {};
  if (lstat(peer_dir.c_str(), &peer_dir_stat) != 0 ||
      !S_ISDIR(peer_dir_stat.st_mode))
    return false;

  std::string cgroup;
  std::string comm;
  std::string cmdline;
  if (!read_bounded_file(peer_dir + "/cgroup", kProcFileLimit, cgroup) ||
      trim_one_newline(cgroup) != kDaemonCgroup)
    return false;
  if (!read_bounded_file(peer_dir + "/comm", 64, comm) ||
      trim_one_newline(comm) != kDaemonComm)
    return false;
  if (!read_bounded_file(peer_dir + "/cmdline", kProcFileLimit, cmdline) ||
      !cmdline_is_daemon_exe(cmdline, daemon_path))
    return false;
  if (!peer_exe_identity_ok(peer_dir + "/exe", daemon_path))
    return false;

  int fd = -1;
  do {
    fd = open(std::string{daemon_path}.c_str(),
              O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0)
    return false;
  struct stat daemon_stat {};
  const bool ok = fstat(fd, &daemon_stat) == 0 &&
                  daemon_file_protected(daemon_stat, overflow);
  close(fd);
  return ok;
}

bool authorize_root_peer(uid_t peer_uid, pid_t peer_pid) {
  const std::string proc_root = proc_root_path();
  if (proc_root.empty())
    return false;
  std::string uid_map;
  if (!read_bounded_file(proc_root + "/self/uid_map", kProcFileLimit, uid_map))
    return false;
  if (peer_uid == 0)
    return uid_map_maps_ns0_to_host_root(uid_map);

  const std::optional<uid_t> overflow = read_overflow_uid(proc_root);
  if (!overflow || peer_uid != *overflow)
    return false;
  const uid_t self_uid = getuid();
  if (self_uid == *overflow)
    return false;
  if (!uid_map_is_exact_single_self(uid_map, self_uid))
    return false;
  return peer_is_genuine_root_daemon(peer_pid, *overflow);
}

bool peer_credentials_authorized(GCredentials *credentials) {
  if (credentials == nullptr)
    return false;
  GError *error = nullptr;
  const uid_t peer_uid = g_credentials_get_unix_user(credentials, &error);
  if (error != nullptr) {
    g_clear_error(&error);
    return false;
  }
  error = nullptr;
  const pid_t peer_pid = g_credentials_get_unix_pid(credentials, &error);
  const bool have_pid = error == nullptr && peer_pid > 0;
  g_clear_error(&error);
  return authorize_root_peer(peer_uid,
                             have_pid ? peer_pid : static_cast<pid_t>(-1));
}

GVariant *get_state_failure(guint outcome, const std::string &helper) {
  return g_variant_new("(usssttty)", outcome, helper.c_str(), "", "",
                       guint64{0}, guint64{0}, guint64{0}, kUnknownRole);
}

GVariant *operation_failure(guint outcome, std::string_view extension,
                            std::string_view digest) {
  const std::string extension_value(extension);
  const std::string digest_value(digest);
  return g_variant_new("(usstty)", outcome, extension_value.c_str(),
                       digest_value.c_str(), guint64{0}, guint64{0},
                       kUnknownRole);
}

bool valid_operation_reply(GVariant *reply, std::string_view expected_extension,
                           std::string_view expected_digest) {
  guint outcome = kOutcomeFailed;
  const char *extension = nullptr;
  const char *digest = nullptr;
  guint64 source_generation = 0;
  guint64 state_generation = 0;
  guchar role = kUnknownRole;
  g_variant_get(reply, "(u&s&stty)", &outcome, &extension, &digest,
                &source_generation, &state_generation, &role);
  const std::string_view extension_value =
      extension == nullptr ? "" : extension;
  const std::string_view digest_value = digest == nullptr ? "" : digest;
  const bool helper_stale = outcome == kOutcomeHelperStale;
  const bool failed_without_digest =
      outcome == kOutcomeFailed && digest_value.empty();
  return outcome != kOutcomeState && valid_outcome(outcome) &&
         is_lower_hex(extension_value, 32) &&
         (helper_stale || expected_extension == extension_value) &&
         ((helper_stale && digest_value.empty()) || failed_without_digest ||
          expected_digest == digest_value) &&
         (role == 0 || role == 1 || role == kUnknownRole);
}

bool operation_requires_restart(GVariant *reply,
                                std::string_view expected_extension) {
  guint outcome = kOutcomeFailed;
  const char *extension = nullptr;
  const char *digest = nullptr;
  guint64 source_generation = 0;
  guint64 state_generation = 0;
  guchar role = kUnknownRole;
  g_variant_get(reply, "(u&s&stty)", &outcome, &extension, &digest,
                &source_generation, &state_generation, &role);
  return outcome == kOutcomeHelperStale || extension == nullptr ||
         expected_extension != std::string_view(extension);
}

class SessionBridge {
public:
  SessionBridge() = default;
  SessionBridge(const SessionBridge &) = delete;
  SessionBridge &operator=(const SessionBridge &) = delete;
  ~SessionBridge() { stop(); }

  bool start();
  void run();
  void stop();
  bool restart_required() const { return restart_required_; }
  bool permanent_failure() const { return permanent_failure_; }
  static gboolean authorize_peer(GDBusAuthObserver *, GIOStream *,
                                 GCredentials *, gpointer);
  static gboolean new_connection(GDBusServer *, GDBusConnection *, gpointer);
  static void connection_closed(GDBusConnection *, gboolean, GError *,
                                gpointer);
  static void method_call(GDBusConnection *, const gchar *, const gchar *,
                          const gchar *, const gchar *, GVariant *,
                          GDBusMethodInvocation *, gpointer);
  static void observer_method_call(GDBusConnection *, const gchar *,
                                   const gchar *, const gchar *, const gchar *,
                                   GVariant *, GDBusMethodInvocation *,
                                   gpointer);
  static void name_owner_changed(GDBusConnection *, const gchar *,
                                 const gchar *, const gchar *, const gchar *,
                                 GVariant *, gpointer);
  static gboolean stop_signal(gpointer);

private:
  struct ObserverCall {
    enum class Step {
      CallerUid,
      PreOwner,
      PrePid,
      PreUid,
      State,
      PostOwner,
      PostPid,
      PostUid,
      CallerAgain
    };
    SessionBridge *owner;
    GDBusMethodInvocation *invocation;
    GCancellable *cancel = g_cancellable_new();
    GVariant *reply = nullptr;
    std::string sender;
    guint64 deadline;
    Step step = Step::CallerUid;
    guint timer = 0, subscription = 0;
    bool done = false;
    ObserverCall(SessionBridge *self, GDBusMethodInvocation *request,
                 const char *caller, guint64 expires)
        : owner(self), invocation(request), sender(caller), deadline(expires) {
      g_object_ref(invocation);
    }
    ~ObserverCall() {
      if (timer != 0)
        g_source_remove(timer);
      if (subscription != 0)
        g_dbus_connection_signal_unsubscribe(owner->session_, subscription);
      if (owner->observer_call_ == this)
        owner->observer_call_ = nullptr;
      if (reply != nullptr)
        g_variant_unref(reply);
      g_object_unref(cancel);
      g_object_unref(invocation);
    }
  };
  struct Forward {
    SessionBridge *owner;
    GDBusConnection *connection;
    GDBusMethodInvocation *invocation;
    GCancellable *cancel = g_cancellable_new();
    bool state;
    guint64 deadline;
    std::string extension, digest;
    Forward(SessionBridge *self, GDBusConnection *peer,
            GDBusMethodInvocation *request, bool is_state, guint64 expires,
            std::string_view instance, std::string_view payload)
        : owner(self), connection(peer), invocation(request), state(is_state),
          deadline(expires), extension(instance), digest(payload) {
      g_object_ref(connection);
      g_object_ref(invocation);
    }
    ~Forward() {
      g_object_unref(cancel);
      g_object_unref(invocation);
      g_object_unref(connection);
    }
  };
  bool acquire_name();
  bool pin_shell_owner_and_session();
  bool query_shell_identity(std::string &owner, guint &pid, uid_t &uid,
                            guint64 deadline = 0);
  bool authority_current(guint64 deadline = 0);
  bool initialize_accessibility();
  bool bind_extension();
  bool create_runtime();
  bool acquire_runtime_lock();
  bool publish_descriptor();
  void request_stop(bool restart_required);
  void remove_owned_endpoint();
  void remove_connection(GDBusConnection *connection);
  void handle_method(GDBusConnection *connection, const gchar *method,
                     GVariant *parameters, GDBusMethodInvocation *invocation);
  void call_extension(GDBusConnection *connection, const char *method,
                      GVariant *parameters, guint64 deadline,
                      GDBusMethodInvocation *invocation,
                      std::string_view extension = {},
                      std::string_view digest = {});
  void complete_extension(const Forward &call, GVariant *reply);
  void handle_observer(const gchar *sender, const gchar *method,
                       GVariant *parameters, GDBusMethodInvocation *invocation);
  bool call_observer_step(ObserverCall &call);
  bool advance_observer(ObserverCall &call, GVariant *reply);
  void finish_observer(ObserverCall &call, guint outcome);

  uid_t uid_ = helper_uid();
  std::string helper_id_;
  std::string observer_client_id_;
  std::string shell_owner_;
  std::string extension_instance_;
  guint shell_pid_ = 0;
  punto::SessionSelection session_selection_;
  std::unique_ptr<punto::SessionQuery> session_query_ =
      punto::make_systemd_session_query();
  punto::SessionResolver session_resolver_{*session_query_};
  std::string runtime_dir_;
  std::string socket_path_;
  std::string descriptor_path_;
  std::string descriptor_payload_;
  ino_t socket_inode_ = 0;
  ino_t descriptor_inode_ = 0;
  int lock_fd_ = -1;
  GMainLoop *loop_ = nullptr;
  GDBusConnection *session_ = nullptr;
  std::unique_ptr<punto::AccessibleContext> accessible_;
  GDBusNodeInfo *node_info_ = nullptr;
  GDBusServer *server_ = nullptr;
  GDBusAuthObserver *observer_ = nullptr;
  struct Peer {
    guint registration = 0;
    bool inflight = false;
    bool observing_editor = false;
    GCancellable *forward_cancel = nullptr;
  };
  std::map<GDBusConnection *, Peer> connections_;
  std::size_t pending_forwards_ = 0;
  ObserverCall *observer_call_ = nullptr;
  guint observer_registration_ = 0;
  guint sigterm_source_ = 0;
  guint sigint_source_ = 0;
  guint shell_owner_subscription_ = 0;
  bool stopping_ = false;
  bool restart_required_ = false;
  bool permanent_failure_ = false;
};

const GDBusInterfaceVTable kVtable = {
    &SessionBridge::method_call,
    nullptr,
    nullptr,
    {0},
};
const GDBusInterfaceVTable kObserverVtable = {
    &SessionBridge::observer_method_call,
    nullptr,
    nullptr,
    {0},
};

bool SessionBridge::acquire_name() {
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(
      session_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", kBusName, guint{4}), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, &error);
  if (reply == nullptr) {
    g_clear_error(&error);
    return false;
  }
  guint result = 0;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  if (result != 1)
    permanent_failure_ = true;
  return result == 1;
}

bool SessionBridge::pin_shell_owner_and_session() {
  std::string owner;
  guint shell_pid = 0;
  uid_t shell_uid = static_cast<uid_t>(-1);
  if (!query_shell_identity(owner, shell_pid, shell_uid)) {
    g_printerr("punto-session-bridge: shell-identity-query\n");
    return false;
  }
  if (shell_uid != uid_) {
    g_printerr("punto-session-bridge: shell-uid-mismatch\n");
    permanent_failure_ = true;
    return false;
  }
  shell_owner_ = std::move(owner);
  shell_pid_ = shell_pid;
  session_selection_ =
      session_resolver_.resolve(uid_, static_cast<pid_t>(shell_pid_));
  if (session_selection_.status != punto::SessionResolutionStatus::ready) {
    g_printerr("punto-session-bridge: %s\n", session_selection_.stage);
    permanent_failure_ = session_selection_.status ==
                         punto::SessionResolutionStatus::permanent_failure;
    return false;
  }

  shell_owner_subscription_ = g_dbus_connection_signal_subscribe(
      session_, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "NameOwnerChanged", "/org/freedesktop/DBus", kShellName,
      G_DBUS_SIGNAL_FLAGS_NONE, &SessionBridge::name_owner_changed, this,
      nullptr);
  return shell_owner_subscription_ != 0;
}

bool SessionBridge::query_shell_identity(std::string &owner, guint &pid,
                                         uid_t &uid, guint64 deadline) {
  const auto timeout = [deadline] {
    return deadline == 0 ? 500 : std::min(500, remaining_timeout_ms(deadline));
  };
  int remaining = timeout();
  if (remaining <= 0)
    return false;
  GError *error = nullptr;
  GVariant *owner_reply = g_dbus_connection_call_sync(
      session_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", kShellName),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, remaining,
      nullptr, &error);
  if (owner_reply == nullptr) {
    g_clear_error(&error);
    return false;
  }
  const char *owner_value = nullptr;
  g_variant_get(owner_reply, "(&s)", &owner_value);
  if (owner_value != nullptr && g_dbus_is_unique_name(owner_value))
    owner = owner_value;
  g_variant_unref(owner_reply);
  if (owner.empty())
    return false;

  remaining = timeout();
  if (remaining <= 0)
    return false;

  GVariant *pid_reply = g_dbus_connection_call_sync(
      session_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetConnectionUnixProcessID",
      g_variant_new("(s)", owner.c_str()), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, remaining, nullptr, &error);
  if (pid_reply == nullptr) {
    g_clear_error(&error);
    return false;
  }
  g_variant_get(pid_reply, "(u)", &pid);
  g_variant_unref(pid_reply);
  if (pid == 0)
    return false;

  remaining = timeout();
  if (remaining <= 0)
    return false;

  GVariant *uid_reply = g_dbus_connection_call_sync(
      session_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetConnectionUnixUser",
      g_variant_new("(s)", owner.c_str()), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, remaining, nullptr, &error);
  if (uid_reply == nullptr) {
    g_clear_error(&error);
    return false;
  }
  guint owner_uid = 0;
  g_variant_get(uid_reply, "(u)", &owner_uid);
  g_variant_unref(uid_reply);
  uid = static_cast<uid_t>(owner_uid);
  return true;
}

bool SessionBridge::authority_current(guint64 deadline) {
  std::string owner;
  guint pid = 0;
  uid_t owner_uid = static_cast<uid_t>(-1);
  return query_shell_identity(owner, pid, owner_uid, deadline) &&
         owner == shell_owner_ && pid == shell_pid_ && owner_uid == uid_ &&
         session_resolver_.revalidate(uid_, static_cast<pid_t>(shell_pid_),
                                      session_selection_);
}

bool SessionBridge::initialize_accessibility() {
  const guint64 deadline = now_us() + 1'000'000;
  const auto read_enabled = [this, deadline]() -> std::optional<bool> {
    const int timeout = remaining_timeout_ms(deadline);
    if (timeout <= 0)
      return std::nullopt;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(
        session_, "org.a11y.Bus", "/org/a11y/bus",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.a11y.Status", "IsEnabled"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, timeout, nullptr,
        &error);
    g_clear_error(&error);
    if (reply == nullptr)
      return std::nullopt;
    GVariant *value = nullptr;
    g_variant_get(reply, "(v)", &value);
    std::optional<bool> result;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
      result = g_variant_get_boolean(value) != FALSE;
    g_variant_unref(value);
    g_variant_unref(reply);
    return result;
  };
  const auto enabled = read_enabled();
  if (!enabled || !authority_current(deadline) || now_us() >= deadline)
    return false;
  const auto first_activation = enroll_accessibility(uid_);
  if (!first_activation)
    return false;
  if (*enabled)
    return authority_current(deadline) && now_us() < deadline;
  if (!*first_activation) {
    g_printerr("punto-session-bridge: accessibility-disabled (layout-only; "
               "enable toolkit accessibility explicitly)\n");
    return authority_current(deadline) && now_us() < deadline;
  }
  if (!authority_current(deadline))
    return false;
  const int timeout = remaining_timeout_ms(deadline);
  if (timeout <= 0)
    return false;
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(
      session_, "org.a11y.Bus", "/org/a11y/bus",
      "org.freedesktop.DBus.Properties", "Set",
      g_variant_new("(ssv)", "org.a11y.Status", "IsEnabled",
                    g_variant_new_boolean(TRUE)),
      G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, timeout, nullptr, &error);
  g_clear_error(&error);
  if (reply == nullptr)
    return false;
  g_variant_unref(reply);
  if (!authority_current(deadline))
    return false;
  const auto readback = read_enabled();
  return readback && *readback && authority_current(deadline) &&
         now_us() < deadline;
}

bool SessionBridge::bind_extension() {
  for (int attempt = 0; attempt < 10; ++attempt) {
    const guint64 deadline = now_us() + 100'000;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(
        session_, shell_owner_.c_str(), kExtensionPath, kExtensionInterface,
        "BindHelper",
        g_variant_new("(sut)", helper_id_.c_str(), kProtocolVersion, deadline),
        G_VARIANT_TYPE("(us)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 100, nullptr,
        &error);
    if (reply != nullptr) {
      guint outcome = kOutcomeFailed;
      const char *extension = nullptr;
      g_variant_get(reply, "(u&s)", &outcome, &extension);
      const bool accepted = outcome == kOutcomeState && extension != nullptr &&
                            is_lower_hex(extension, 32);
      if (accepted)
        extension_instance_ = extension;
      g_variant_unref(reply);
      if (accepted)
        return true;
    }
    g_clear_error(&error);
    g_usleep(25'000);
  }
  return false;
}

bool SessionBridge::create_runtime() {
  const char *base = g_get_user_runtime_dir();
  if (base == nullptr || base[0] != '/' || std::strchr(base, '\n') != nullptr ||
      std::strchr(base, '\r') != nullptr || std::strchr(base, '"') != nullptr ||
      std::strchr(base, '\\') != nullptr)
    return false;
  runtime_dir_ = std::string(base) + "/punto";
  if (runtime_dir_.size() > 70 ||
      g_mkdir_with_parents(runtime_dir_.c_str(), 0700) != 0)
    return false;
  struct stat directory {};
  if (lstat(runtime_dir_.c_str(), &directory) != 0 ||
      !S_ISDIR(directory.st_mode) || directory.st_uid != uid_ ||
      (directory.st_mode & 0777) != 0700)
    return false;
  if (!acquire_runtime_lock())
    return false;

  socket_path_ = runtime_dir_ + "/session-bridge-" + helper_id_ + ".sock";
  descriptor_path_ = runtime_dir_ + "/session-bridge.json";
  const std::string address = "unix:path=" + socket_path_;
  gchar *guid = g_dbus_generate_guid();
  GError *error = nullptr;
  observer_ = g_dbus_auth_observer_new();
  g_signal_connect(observer_, "authorize-authenticated-peer",
                   G_CALLBACK(&SessionBridge::authorize_peer), this);
  server_ = g_dbus_server_new_sync(address.c_str(), G_DBUS_SERVER_FLAGS_NONE,
                                   guid, observer_, nullptr, &error);
  g_free(guid);
  if (server_ == nullptr) {
    g_clear_error(&error);
    return false;
  }
  g_signal_connect(server_, "new-connection",
                   G_CALLBACK(&SessionBridge::new_connection), this);
  g_dbus_server_start(server_);
  struct stat socket_stat {};
  if (lstat(socket_path_.c_str(), &socket_stat) != 0 ||
      !S_ISSOCK(socket_stat.st_mode) || socket_stat.st_uid != uid_)
    return false;
  socket_inode_ = socket_stat.st_ino;
  if (chmod(socket_path_.c_str(), 0600) != 0 ||
      lstat(socket_path_.c_str(), &socket_stat) != 0 ||
      socket_stat.st_ino != socket_inode_ ||
      (socket_stat.st_mode & 0777) != 0600)
    return false;
  return true;
}

bool SessionBridge::acquire_runtime_lock() {
  const std::string path = runtime_dir_ + "/session-bridge.lock";
  lock_fd_ =
      open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_fd_ < 0)
    return false;
  struct stat lock_stat {};
  if (fstat(lock_fd_, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) ||
      lock_stat.st_uid != uid_ || (lock_stat.st_mode & 0777) != 0600 ||
      flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    close(lock_fd_);
    lock_fd_ = -1;
    return false;
  }
  return true;
}

bool SessionBridge::publish_descriptor() {
  const auto start_time = process_start_time();
  if (!start_time)
    return false;
  std::ostringstream payload;
  payload << "{\"protocol_version\":" << kProtocolVersion
          << ",\"socket_path\":\"" << socket_path_
          << "\",\"helper_instance\":\"" << helper_id_
          << "\",\"pid\":" << getpid() << ",\"start_time\":" << *start_time
          << "}\n";
  descriptor_payload_ = payload.str();
  const std::string temporary = descriptor_path_ + "." + helper_id_;
  const int fd =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return false;
  bool ok = write_all(fd, descriptor_payload_) && fsync(fd) == 0;
  const int close_result = close(fd);
  ok = ok && close_result == 0;
  if (!ok || rename(temporary.c_str(), descriptor_path_.c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  struct stat descriptor {};
  if (lstat(descriptor_path_.c_str(), &descriptor) != 0 ||
      !S_ISREG(descriptor.st_mode) || descriptor.st_uid != uid_ ||
      (descriptor.st_mode & 0777) != 0600)
    return false;
  descriptor_inode_ = descriptor.st_ino;
  const int directory_fd =
      open(runtime_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd >= 0) {
    ok = fsync(directory_fd) == 0;
    close(directory_fd);
  } else {
    ok = false;
  }
  if (!ok)
    return false;
  return true;
}

bool SessionBridge::start() {
#if !defined(PUNTO_SESSION_BRIDGE_APP_E2E)
  if (uid_ == 0) {
    permanent_failure_ = true;
    return false;
  }
#endif
  helper_id_ = make_id();
  observer_client_id_ = make_id();
  if (helper_id_.empty() || observer_client_id_.empty())
    return false;
  gchar *xml = nullptr;
  gsize xml_size = 0;
  GError *error = nullptr;
  if (!g_file_get_contents(PUNTO_PROTOCOL_PATH, &xml, &xml_size, &error)) {
    g_clear_error(&error);
    permanent_failure_ = true;
    return false;
  }
  node_info_ = g_dbus_node_info_new_for_xml(xml, &error);
  g_free(xml);
  if (node_info_ == nullptr ||
      g_dbus_node_info_lookup_interface(node_info_, kPeerInterface) ==
          nullptr ||
      g_dbus_node_info_lookup_interface(node_info_, kObserverInterface) ==
          nullptr) {
    g_clear_error(&error);
    permanent_failure_ = true;
    return false;
  }
  session_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (session_ == nullptr) {
    g_clear_error(&error);
    return false;
  }
  g_dbus_connection_set_exit_on_close(session_, FALSE);
  loop_ = g_main_loop_new(nullptr, FALSE);
  g_signal_connect(session_, "closed",
                   G_CALLBACK(&SessionBridge::connection_closed), this);
  sigterm_source_ =
      g_unix_signal_add(SIGTERM, &SessionBridge::stop_signal, this);
  sigint_source_ = g_unix_signal_add(SIGINT, &SessionBridge::stop_signal, this);
  if (!acquire_name()) {
    g_printerr("punto-session-bridge: acquire-name\n");
    return false;
  }
  if (!pin_shell_owner_and_session())
    return false;
  if (!bind_extension()) {
    g_printerr("punto-session-bridge: bind-extension\n");
    return false;
  }
  if (!authority_current()) {
    g_printerr("punto-session-bridge: authority-after-bind\n");
    return false;
  }
  if (!create_runtime()) {
    g_printerr("punto-session-bridge: create-runtime\n");
    permanent_failure_ = true;
    return false;
  }
  if (!wait_test_pre_publish_barrier()) {
    g_printerr("punto-session-bridge: pre-publish-barrier\n");
    return false;
  }
  if (!authority_current()) {
    g_printerr("punto-session-bridge: authority-before-publish\n");
    return false;
  }
  if (!initialize_accessibility()) {
    g_printerr("punto-session-bridge: accessibility-startup\n");
    return false;
  }
  accessible_ =
      std::make_unique<punto::AccessibleContext>(session_, helper_id_, uid_);
  observer_registration_ = g_dbus_connection_register_object(
      session_, kObserverPath,
      g_dbus_node_info_lookup_interface(node_info_, kObserverInterface),
      &kObserverVtable, this, nullptr, &error);
  if (observer_registration_ == 0) {
    g_clear_error(&error);
    return false;
  }
  if (!publish_descriptor()) {
    g_printerr("punto-session-bridge: publish-descriptor\n");
    permanent_failure_ = true;
    return false;
  }
  return true;
}

void SessionBridge::run() {
  if (loop_ != nullptr)
    g_main_loop_run(loop_);
}

gboolean SessionBridge::authorize_peer(GDBusAuthObserver *, GIOStream *,
                                       GCredentials *credentials, gpointer) {
  if (credentials == nullptr) {
    g_printerr("punto-session-bridge: peer-auth missing-credentials\n");
    return FALSE;
  }
  GError *error = nullptr;
  const uid_t peer_uid = g_credentials_get_unix_user(credentials, &error);
  pid_t peer_pid = -1;
  if (error == nullptr) {
    GError *pid_error = nullptr;
    const pid_t parsed_pid =
        g_credentials_get_unix_pid(credentials, &pid_error);
    if (pid_error == nullptr)
      peer_pid = parsed_pid;
    g_clear_error(&pid_error);
  }
  const bool accepted =
      error == nullptr && authorize_root_peer(peer_uid, peer_pid);
  if (!accepted)
    g_printerr("punto-session-bridge: peer-auth rejected uid=%u\n",
               static_cast<unsigned int>(peer_uid));
  g_clear_error(&error);
  return accepted ? TRUE : FALSE;
}

gboolean SessionBridge::new_connection(GDBusServer *,
                                       GDBusConnection *connection,
                                       gpointer data) {
  auto *self = static_cast<SessionBridge *>(data);
  if (self->stopping_ || self->connections_.size() >= kMaxConnections ||
      self->pending_forwards_ >= kMaxConnections) {
    g_printerr("punto-session-bridge: peer-connect unavailable count=%zu\n",
               self->connections_.size());
    return FALSE;
  }
  GCredentials *credentials =
      g_dbus_connection_get_peer_credentials(connection);
  GError *error = nullptr;
  const uid_t peer_uid = credentials == nullptr
                             ? static_cast<uid_t>(-1)
                             : g_credentials_get_unix_user(credentials, &error);
  if (error != nullptr || !peer_credentials_authorized(credentials)) {
    g_printerr("punto-session-bridge: peer-connect uid=%u rejected\n",
               static_cast<unsigned int>(peer_uid));
    g_clear_error(&error);
    return FALSE;
  }
  g_clear_error(&error);
  GDBusInterfaceInfo *interface_info =
      g_dbus_node_info_lookup_interface(self->node_info_, kPeerInterface);
  const guint registration = g_dbus_connection_register_object(
      connection, kPeerPath, interface_info, &kVtable, self, nullptr, &error);
  if (registration == 0) {
    g_printerr("punto-session-bridge: peer-connect register-failed\n");
    g_clear_error(&error);
    return FALSE;
  }
  g_object_ref(connection);
  self->connections_.emplace(connection, Peer{registration, false});
  g_signal_connect(connection, "closed",
                   G_CALLBACK(&SessionBridge::connection_closed), self);
  return TRUE;
}

void SessionBridge::connection_closed(GDBusConnection *connection, gboolean,
                                      GError *, gpointer data) {
  auto *self = static_cast<SessionBridge *>(data);
  if (connection == self->session_) {
    self->request_stop(true);
    return;
  }
  self->remove_connection(connection);
}

void SessionBridge::name_owner_changed(GDBusConnection *, const gchar *,
                                       const gchar *, const gchar *,
                                       const gchar *, GVariant *parameters,
                                       gpointer data) {
  auto *self = static_cast<SessionBridge *>(data);
  const char *name = nullptr;
  const char *old_owner = nullptr;
  const char *new_owner = nullptr;
  g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (name != nullptr && std::strcmp(name, kShellName) == 0 &&
      old_owner != nullptr && self->shell_owner_ == old_owner &&
      (new_owner == nullptr || self->shell_owner_ != new_owner))
    self->request_stop(true);
}

void SessionBridge::request_stop(bool restart_required) {
  restart_required_ = restart_required_ || restart_required;
  stopping_ = true;
  if (observer_call_ != nullptr)
    finish_observer(*observer_call_, kOutcomeLocked);
  if (server_ != nullptr)
    g_dbus_server_stop(server_);
  if (loop_ != nullptr)
    g_main_loop_quit(loop_);
}

void SessionBridge::remove_connection(GDBusConnection *connection) {
  const auto found = connections_.find(connection);
  if (found == connections_.end())
    return;
  // A disconnected peer cannot keep a read-only observation occupying the
  // single-flight slot. Preserve the continuous provider/focus event watch.
  if (found->second.observing_editor && accessible_)
    accessible_->cancel_observation();
  // Cancellation only retires our reply receiver, not the accepted operation.
  g_cancellable_cancel(found->second.forward_cancel);
  g_signal_handlers_disconnect_by_data(connection, this);
  g_dbus_connection_unregister_object(connection, found->second.registration);
  connections_.erase(found);
  g_object_unref(connection);
}

gboolean SessionBridge::stop_signal(gpointer data) {
  auto *self = static_cast<SessionBridge *>(data);
  self->request_stop(false);
  return G_SOURCE_REMOVE;
}

void SessionBridge::call_extension(GDBusConnection *connection,
                                   const char *method, GVariant *parameters,
                                   guint64 deadline,
                                   GDBusMethodInvocation *invocation,
                                   std::string_view extension,
                                   std::string_view digest) {
  if (g_variant_is_floating(parameters))
    g_variant_ref_sink(parameters);
  const bool state = g_strcmp0(method, "GetState") == 0;
  const int timeout = remaining_timeout_ms(deadline);
  if (timeout <= 0) {
    g_variant_unref(parameters);
    g_dbus_method_invocation_return_value(
        invocation,
        state ? get_state_failure(kOutcomeDeadlineExpired, helper_id_)
              : operation_failure(kOutcomeDeadlineExpired, extension, digest));
    return;
  }
  if (pending_forwards_ >= kMaxConnections) {
    g_variant_unref(parameters);
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.punto.Switcher.Error.Busy", "forward limit reached");
    return;
  }
  auto *call = new Forward(this, connection, invocation, state, deadline,
                           extension, digest);
  auto &peer = connections_.at(connection);
  peer.inflight = true;
  peer.forward_cancel = call->cancel;
  ++pending_forwards_;
  g_dbus_connection_call(
      session_, shell_owner_.c_str(), kExtensionPath, kExtensionInterface,
      method, parameters, G_VARIANT_TYPE(state ? "(ussttty)" : "(usstty)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout, call->cancel,
      [](GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<Forward> call(static_cast<Forward *>(data));
        GError *error = nullptr;
        GVariant *reply = g_dbus_connection_call_finish(
            G_DBUS_CONNECTION(source), result, &error);
        call->owner->complete_extension(*call, reply);
        --call->owner->pending_forwards_;
        if (reply != nullptr)
          g_variant_unref(reply);
        g_clear_error(&error);
      },
      call);
  g_variant_unref(parameters);
}

void SessionBridge::complete_extension(const Forward &call, GVariant *reply) {
  const auto peer = connections_.find(call.connection);
  if (peer == connections_.end() || peer->second.forward_cancel != call.cancel)
    return;
  peer->second.forward_cancel = nullptr;
  peer->second.inflight = false;
  if (stopping_ || g_dbus_connection_is_closed(call.connection))
    return;
  if (reply == nullptr) {
    g_dbus_method_invocation_return_value(
        call.invocation, call.state
                             ? get_state_failure(kOutcomeHopUnknown, helper_id_)
                             : operation_failure(kOutcomeHopUnknown,
                                                 call.extension, call.digest));
    return;
  }
  if (call.state) {
    if (!authority_current()) {
      g_dbus_method_invocation_return_value(
          call.invocation, get_state_failure(kOutcomeLocked, helper_id_));
      request_stop(true);
      return;
    }
    if (now_us() >= call.deadline) {
      g_dbus_method_invocation_return_value(
          call.invocation,
          get_state_failure(kOutcomeDeadlineExpired, helper_id_));
      return;
    }
    guint outcome = kOutcomeFailed;
    const char *extension = nullptr;
    const char *lease = nullptr;
    guint64 lease_expiry = 0, source_generation = 0, state_generation = 0;
    guchar role = kUnknownRole;
    g_variant_get(reply, "(u&s&sttty)", &outcome, &extension, &lease,
                  &lease_expiry, &source_generation, &state_generation, &role);
    const bool valid =
        valid_outcome(outcome) &&
        is_lower_hex(extension == nullptr ? "" : extension, 32) &&
        ((outcome == kOutcomeState && extension_instance_ == extension &&
          is_lower_hex(lease == nullptr ? "" : lease, 32) &&
          (role == 0 || role == 1)) ||
         (outcome != kOutcomeState && lease != nullptr && lease[0] == '\0' &&
          role == kUnknownRole &&
          (outcome == kOutcomeHelperStale ||
           extension_instance_ == extension)));
    g_dbus_method_invocation_return_value(
        call.invocation,
        valid ? g_variant_new("(usssttty)", outcome, helper_id_.c_str(),
                              extension, lease, lease_expiry, source_generation,
                              state_generation, role)
              : get_state_failure(kOutcomeFailed, helper_id_));
    if (outcome == kOutcomeHelperStale || extension_instance_ != extension)
      request_stop(true);
    return;
  }
  // Applied is an operation fact, not permission for a new request or lease.
  const bool restart = operation_requires_restart(reply, extension_instance_);
  g_dbus_method_invocation_return_value(
      call.invocation,
      valid_operation_reply(reply, extension_instance_, call.digest)
          ? reply
          : operation_failure(kOutcomeHopUnknown, call.extension, call.digest));
  if (restart)
    request_stop(true);
}

void SessionBridge::method_call(GDBusConnection *connection, const gchar *,
                                const gchar *, const gchar *,
                                const gchar *method, GVariant *parameters,
                                GDBusMethodInvocation *invocation,
                                gpointer data) {
  static_cast<SessionBridge *>(data)->handle_method(connection, method,
                                                    parameters, invocation);
}

void SessionBridge::observer_method_call(GDBusConnection *, const gchar *sender,
                                         const gchar *, const gchar *,
                                         const gchar *method,
                                         GVariant *parameters,
                                         GDBusMethodInvocation *invocation,
                                         gpointer data) {
  static_cast<SessionBridge *>(data)->handle_observer(sender, method,
                                                      parameters, invocation);
}

void SessionBridge::handle_observer(const gchar *sender, const gchar *method,
                                    GVariant *parameters,
                                    GDBusMethodInvocation *invocation) {
  if (g_strcmp0(method, "ReadState") != 0 || sender == nullptr ||
      !g_dbus_is_unique_name(sender) ||
      !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(t)")) ||
      g_variant_get_size(parameters) > kMaxMessageBytes) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.punto.Switcher.Error.Invalid", "invalid observation");
    return;
  }
  guint64 deadline = 0;
  g_variant_get(parameters, "(t)", &deadline);
  const guint64 now = now_us();
  if (stopping_ || deadline <= now || deadline - now > kObserverHorizonUs) {
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new(
            "(usstty)", stopping_ ? kOutcomeLocked : kOutcomeDeadlineExpired,
            helper_id_.c_str(), "", guint64{0}, guint64{0}, kUnknownRole));
    return;
  }
  if (observer_call_ != nullptr) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.punto.Switcher.Error.Busy", "observation active");
    return;
  }
  auto call =
      std::make_unique<ObserverCall>(this, invocation, sender, deadline);
  observer_call_ = call.get();
  GSource *timer = g_timeout_source_new(100);
  g_source_set_ready_time(timer, static_cast<gint64>(deadline));
  g_source_set_callback(
      timer,
      [](gpointer data) -> gboolean {
        auto *call = static_cast<ObserverCall *>(data);
        call->timer = 0;
        call->owner->finish_observer(*call, kOutcomeDeadlineExpired);
        return G_SOURCE_REMOVE;
      },
      call.get(), nullptr);
  call->timer = g_source_attach(timer, nullptr);
  g_source_unref(timer);
  call->subscription = g_dbus_connection_signal_subscribe(
      session_, "org.freedesktop.DBus", "org.freedesktop.DBus",
      "NameOwnerChanged", "/org/freedesktop/DBus", sender,
      G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection *, const gchar *, const gchar *, const gchar *,
         const gchar *, GVariant *parameters, gpointer data) {
        auto *call = static_cast<ObserverCall *>(data);
        const gchar *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (name != nullptr && call->sender == name &&
            (new_owner == nullptr || new_owner[0] == '\0'))
          call->owner->finish_observer(*call, kOutcomeLocked);
      },
      call.get(), nullptr);
  if (call_observer_step(*call))
    call.release();
}

void SessionBridge::finish_observer(ObserverCall &call, guint outcome) {
  if (call.done)
    return;
  call.done = true;
  g_cancellable_cancel(call.cancel);
  if (outcome == kOutcomeState && now_us() < call.deadline && !stopping_) {
    guint ignored = 0;
    const gchar *extension = nullptr, *lease = nullptr;
    guint64 expiry = 0, source_generation = 0, state_generation = 0;
    guchar role = kUnknownRole;
    g_variant_get(call.reply, "(u&s&sttty)", &ignored, &extension, &lease,
                  &expiry, &source_generation, &state_generation, &role);
    const guint64 now = now_us();
    if (expiry > now && now < call.deadline) {
      g_dbus_method_invocation_return_value(
          call.invocation,
          g_variant_new("(usstty)", outcome, helper_id_.c_str(), extension,
                        source_generation, state_generation, role));
      return;
    }
    outcome = now >= call.deadline ? kOutcomeDeadlineExpired : kOutcomeFailed;
  } else if (now_us() >= call.deadline) {
    outcome = kOutcomeDeadlineExpired;
  }
  g_dbus_method_invocation_return_value(
      call.invocation, g_variant_new("(usstty)", outcome, helper_id_.c_str(),
                                     "", guint64{0}, guint64{0}, kUnknownRole));
}

bool SessionBridge::call_observer_step(ObserverCall &call) {
  if (call.done)
    return false;
  const int timeout = remaining_timeout_ms(call.deadline);
  if (stopping_ || timeout <= 0) {
    finish_observer(call, stopping_ ? kOutcomeLocked : kOutcomeDeadlineExpired);
    return false;
  }
  const char *destination = "org.freedesktop.DBus";
  const char *path = "/org/freedesktop/DBus";
  const char *interface = "org.freedesktop.DBus";
  const char *method = "GetConnectionUnixUser";
  const GVariantType *reply_type = G_VARIANT_TYPE("(u)");
  GVariant *parameters = nullptr;
  using Step = ObserverCall::Step;
  switch (call.step) {
  case Step::CallerUid:
  case Step::CallerAgain:
    parameters = g_variant_new("(s)", call.sender.c_str());
    break;
  case Step::PreOwner:
  case Step::PostOwner:
    method = "GetNameOwner";
    reply_type = G_VARIANT_TYPE("(s)");
    parameters = g_variant_new("(s)", kShellName);
    break;
  case Step::PrePid:
  case Step::PostPid:
    method = "GetConnectionUnixProcessID";
    parameters = g_variant_new("(s)", shell_owner_.c_str());
    break;
  case Step::PreUid:
  case Step::PostUid:
    parameters = g_variant_new("(s)", shell_owner_.c_str());
    break;
  case Step::State:
    destination = shell_owner_.c_str();
    path = kExtensionPath;
    interface = kExtensionInterface;
    method = "GetState";
    reply_type = G_VARIANT_TYPE("(ussttty)");
    parameters = g_variant_new("(sst)", helper_id_.c_str(),
                               observer_client_id_.c_str(), call.deadline);
    break;
  }
  g_dbus_connection_call(
      session_, destination, path, interface, method, parameters, reply_type,
      G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout, call.cancel,
      [](GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<ObserverCall> call(static_cast<ObserverCall *>(data));
        GError *error = nullptr;
        GVariant *reply = g_dbus_connection_call_finish(
            G_DBUS_CONNECTION(source), result, &error);
        const bool next = call->owner->advance_observer(*call, reply);
        if (reply != nullptr)
          g_variant_unref(reply);
        g_clear_error(&error);
        if (next && call->owner->call_observer_step(*call))
          call.release();
      },
      &call);
  return true;
}

bool SessionBridge::advance_observer(ObserverCall &call, GVariant *reply) {
  if (call.done)
    return false;
  if (stopping_ || now_us() >= call.deadline || reply == nullptr) {
    finish_observer(call, stopping_ ? kOutcomeLocked : kOutcomeHopUnknown);
    return false;
  }
  using Step = ObserverCall::Step;
  if (call.step == Step::CallerUid || call.step == Step::CallerAgain) {
    guint uid = 0;
    g_variant_get(reply, "(u)", &uid);
    if (uid != uid_) {
      call.done = true;
      g_cancellable_cancel(call.cancel);
      g_dbus_method_invocation_return_dbus_error(
          call.invocation, "org.punto.Switcher.Error.Unauthorized",
          "caller is not the active session user");
      return false;
    }
    if (call.step == Step::CallerAgain) {
      finish_observer(call, kOutcomeState);
      return false;
    }
    call.step = Step::PreOwner;
    return true;
  }
  if (call.step == Step::State) {
    guint outcome = kOutcomeFailed;
    const gchar *extension = nullptr, *lease = nullptr;
    guint64 expiry = 0, source_generation = 0, state_generation = 0;
    guchar role = kUnknownRole;
    g_variant_get(reply, "(u&s&sttty)", &outcome, &extension, &lease, &expiry,
                  &source_generation, &state_generation, &role);
    const guint64 now = now_us();
    const bool stale =
        outcome == kOutcomeHelperStale ||
        (is_lower_hex(extension, 32) && extension_instance_ != extension);
    const bool valid =
        is_lower_hex(extension, 32) &&
        (stale || extension_instance_ == extension) &&
        ((outcome == kOutcomeState && !stale && is_lower_hex(lease, 32) &&
          expiry > now && expiry - now <= 30'000'000 &&
          (role == 0 || role == 1)) ||
         (outcome >= 10 && valid_outcome(outcome) && lease[0] == '\0' &&
          expiry == 0 && role == kUnknownRole));
    if (!valid || outcome != kOutcomeState) {
      finish_observer(call, valid ? outcome : kOutcomeFailed);
      if (stale)
        request_stop(true);
      return false;
    }
    call.reply = g_variant_ref(reply);
    call.step = Step::PostOwner;
    return true;
  }
  bool current = false;
  if (call.step == Step::PreOwner || call.step == Step::PostOwner) {
    const gchar *owner = nullptr;
    g_variant_get(reply, "(&s)", &owner);
    current = owner != nullptr && shell_owner_ == owner;
    call.step = call.step == Step::PreOwner ? Step::PrePid : Step::PostPid;
  } else {
    guint identity = 0;
    g_variant_get(reply, "(u)", &identity);
    if (call.step == Step::PrePid || call.step == Step::PostPid) {
      current = identity == shell_pid_;
      call.step = call.step == Step::PrePid ? Step::PreUid : Step::PostUid;
    } else {
      if (now_us() >= call.deadline) {
        finish_observer(call, kOutcomeDeadlineExpired);
        return false;
      }
      current = identity == uid_ &&
                session_resolver_.revalidate(
                    uid_, static_cast<pid_t>(shell_pid_), session_selection_);
      call.step = call.step == Step::PreUid ? Step::State : Step::CallerAgain;
    }
  }
  if (now_us() >= call.deadline) {
    finish_observer(call, kOutcomeDeadlineExpired);
    return false;
  }
  if (!current) {
    finish_observer(call, kOutcomeLocked);
    request_stop(true);
    return false;
  }
  return true;
}

void SessionBridge::handle_method(GDBusConnection *connection,
                                  const gchar *method, GVariant *parameters,
                                  GDBusMethodInvocation *invocation) {
  g_printerr("punto-session-bridge: peer-method %s\n",
             method == nullptr ? "unknown" : method);
  const auto found = connections_.find(connection);
  if (found == connections_.end()) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.punto.Switcher.Error.Unauthorized",
        "unauthorized peer");
    return;
  }
  const bool preparation = g_strcmp0(method, "PrepareEditorOrigin") == 0;
  const bool origin_only =
      preparation || g_strcmp0(method, "ObserveEditorOrigin") == 0;
  const bool selection = g_strcmp0(method, "ObserveSelection") == 0;
  if (origin_only || selection || g_strcmp0(method, "ObserveEditor") == 0) {
    const gchar *helper = nullptr, *token = nullptr, *sha = "", *identity = "";
    guint pid = 0;
    guint64 deadline = 0;
    gint begin = -1, end = -1;
    if (g_variant_get_size(parameters) > kMaxMessageBytes ||
        !g_variant_is_of_type(parameters,
                              G_VARIANT_TYPE(origin_only ? "(suts)"
                                             : selection ? "(sutsiiss)"
                                                         : "(sutsiis)"))) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.punto.Switcher.Error.Invalid",
          "invalid observation");
      return;
    }
    if (origin_only)
      g_variant_get(parameters, "(&sut&s)", &helper, &pid, &deadline, &token);
    else if (selection)
      g_variant_get(parameters, "(&sut&sii&s&s)", &helper, &pid, &deadline,
                    &token, &begin, &end, &sha, &identity);
    else
      g_variant_get(parameters, "(&sut&sii&s)", &helper, &pid, &deadline,
                    &token, &begin, &end, &sha);
    punto::AccessibleObservation refused;
    if (found->second.inflight) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.punto.Switcher.Error.Busy", "call already active");
      return;
    }
    const bool range_valid =
        (begin == -1 && end == -1 && sha[0] == '\0') ||
        (begin >= 0 && end >= begin &&
         end - begin <= punto::kAccessibleRangeCharacters &&
         is_lower_hex(sha, 64));
    const bool current =
        !stopping_ && helper_id_ == helper && pid > 0 &&
        (token[0] == '\0' || is_lower_hex(token, 64)) &&
        (!selection ||
         (begin >= 0 ? is_lower_hex(identity, 64) : identity[0] == '\0')) &&
        (!preparation || token[0] == '\0') && range_valid &&
        session_resolver_.revalidate(uid_, static_cast<pid_t>(shell_pid_),
                                     session_selection_);
    if (!valid_deadline(deadline, now_us()) || !current || !accessible_) {
      refused.status = !valid_deadline(deadline, now_us())
                           ? punto::AccessibleStatus::Expired
                           : punto::AccessibleStatus::Stale;
      g_dbus_method_invocation_return_value(
          invocation, refused.wire(helper_id_, selection));
      return;
    }
    punto::AccessibleRequest request{pid, deadline, token, std::nullopt};
    request.preparation = preparation;
    if (origin_only)
      request.scope = punto::AccessibleScope::Child;
    else if (selection)
      request.scope = punto::AccessibleScope::Selection;
    if (begin >= 0)
      request.expected_range =
          punto::AccessibleRange{begin, end, sha, identity};
    found->second.inflight = true;
    found->second.observing_editor = true;
    g_object_ref(connection);
    g_object_ref(invocation);
    accessible_->observe(
        std::move(request), [this, connection, invocation, deadline, selection](
                                punto::AccessibleObservation observation) {
          const auto peer = connections_.find(connection);
          if (peer != connections_.end()) {
            peer->second.inflight = false;
            peer->second.observing_editor = false;
          }
          if (peer == connections_.end() || stopping_ ||
              !session_resolver_.revalidate(
                  uid_, static_cast<pid_t>(shell_pid_), session_selection_))
            observation =
                punto::AccessibleObservation{punto::AccessibleStatus::Stale};
          if (now_us() >= deadline)
            observation =
                punto::AccessibleObservation{punto::AccessibleStatus::Expired};
          g_dbus_method_invocation_return_value(
              invocation, observation.wire(helper_id_, selection));
          g_object_unref(invocation);
          g_object_unref(connection);
        });
    return;
  }
  if (g_variant_get_size(parameters) > kMaxMessageBytes) {
    if (g_strcmp0(method, "GetState") == 0)
      g_dbus_method_invocation_return_value(
          invocation, get_state_failure(kOutcomeFailed, helper_id_));
    else
      g_dbus_method_invocation_return_value(
          invocation, operation_failure(kOutcomeFailed, "", ""));
    return;
  }
  if (found->second.inflight) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.punto.Switcher.Error.Busy", "call already active");
    return;
  }
  const bool active = authority_current();
  if (!active)
    request_stop(true);
  if (g_strcmp0(method, "GetState") == 0) {
    const char *client = nullptr;
    guint64 deadline = 0;
    g_variant_get(parameters, "(&st)", &client, &deadline);
    if (!is_lower_hex(client == nullptr ? "" : client, 32)) {
      g_dbus_method_invocation_return_value(
          invocation, get_state_failure(kOutcomeFailed, helper_id_));
      return;
    }
    if (!valid_deadline(deadline, now_us())) {
      g_dbus_method_invocation_return_value(
          invocation, get_state_failure(kOutcomeDeadlineExpired, helper_id_));
      return;
    }
    if (!active) {
      g_dbus_method_invocation_return_value(
          invocation, get_state_failure(kOutcomeLocked, helper_id_));
      return;
    }
    call_extension(connection, "GetState",
                   g_variant_new("(sst)", helper_id_.c_str(), client, deadline),
                   deadline, invocation);
    return;
  }

  const char *helper = nullptr;
  const char *extension = nullptr;
  const char *client = nullptr;
  const char *lease = nullptr;
  const char *digest = nullptr;
  guint64 sequence = 0;
  guint64 deadline = 0;
  if (g_strcmp0(method, "Switch") == 0) {
    guint64 source_generation = 0;
    guint64 state_generation = 0;
    guchar expected_role = kUnknownRole;
    guchar target_role = kUnknownRole;
    g_variant_get(parameters, "(&s&s&s&st&stttyy)", &helper, &extension,
                  &client, &lease, &sequence, &digest, &deadline,
                  &source_generation, &state_generation, &expected_role,
                  &target_role);
    const bool helper_valid = helper != nullptr && helper_id_ == helper;
    const bool payload_valid =
        is_lower_hex(extension == nullptr ? "" : extension, 32) &&
        is_lower_hex(client == nullptr ? "" : client, 32) &&
        is_lower_hex(lease == nullptr ? "" : lease, 32) && sequence > 0 &&
        is_lower_hex(digest == nullptr ? "" : digest, 64) &&
        (expected_role == 0 || expected_role == 1) &&
        (target_role == 0 || target_role == 1);
    const bool deadline_valid = valid_deadline(deadline, now_us());
    if (!helper_valid || !payload_valid || !deadline_valid || !active) {
      const guint outcome = !helper_valid     ? kOutcomeHelperStale
                            : !payload_valid  ? kOutcomeFailed
                            : !deadline_valid ? kOutcomeDeadlineExpired
                                              : kOutcomeLocked;
      g_dbus_method_invocation_return_value(
          invocation,
          operation_failure(outcome, extension == nullptr ? "" : extension,
                            digest == nullptr ? "" : digest));
      return;
    }
    call_extension(connection, "Switch", g_variant_ref(parameters), deadline,
                   invocation, extension, digest);
    return;
  }

  if (g_strcmp0(method, "GetOperation") == 0) {
    g_variant_get(parameters, "(&s&s&s&st&st)", &helper, &extension, &client,
                  &lease, &sequence, &digest, &deadline);
    const bool helper_valid = helper != nullptr && helper_id_ == helper;
    const bool payload_valid =
        is_lower_hex(extension == nullptr ? "" : extension, 32) &&
        is_lower_hex(client == nullptr ? "" : client, 32) &&
        is_lower_hex(lease == nullptr ? "" : lease, 32) && sequence > 0 &&
        is_lower_hex(digest == nullptr ? "" : digest, 64);
    const bool deadline_valid = valid_deadline(deadline, now_us());
    if (!helper_valid || !payload_valid || !deadline_valid || !active) {
      const guint outcome = !helper_valid     ? kOutcomeHelperStale
                            : !payload_valid  ? kOutcomeFailed
                            : !deadline_valid ? kOutcomeDeadlineExpired
                                              : kOutcomeLocked;
      g_dbus_method_invocation_return_value(
          invocation,
          operation_failure(outcome, extension == nullptr ? "" : extension,
                            digest == nullptr ? "" : digest));
      return;
    }
    call_extension(connection, "GetOperation", g_variant_ref(parameters),
                   deadline, invocation, extension, digest);
    return;
  }

  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.punto.Switcher.Error.UnknownMethod", "unknown method");
}

void SessionBridge::remove_owned_endpoint() {
  if (!descriptor_path_.empty() && descriptor_inode_ != 0) {
    struct stat value {};
    gchar *contents = nullptr;
    gsize size = 0;
    if (lstat(descriptor_path_.c_str(), &value) == 0 &&
        S_ISREG(value.st_mode) && value.st_ino == descriptor_inode_ &&
        g_file_get_contents(descriptor_path_.c_str(), &contents, &size,
                            nullptr) &&
        std::string_view(contents, size) == descriptor_payload_)
      unlink(descriptor_path_.c_str());
    g_free(contents);
  }
  if (!socket_path_.empty() && socket_inode_ != 0) {
    struct stat value {};
    if (lstat(socket_path_.c_str(), &value) == 0 && S_ISSOCK(value.st_mode) &&
        value.st_ino == socket_inode_)
      unlink(socket_path_.c_str());
  }
}

void SessionBridge::stop() {
  if (stopping_ && loop_ == nullptr && session_ == nullptr)
    return;
  stopping_ = true;
  if (observer_call_ != nullptr) {
    finish_observer(*observer_call_, kOutcomeLocked);
    g_cancellable_cancel(observer_call_->cancel);
  }
  accessible_.reset();
  if (server_ != nullptr)
    g_dbus_server_stop(server_);
  while (!connections_.empty()) {
    GDBusConnection *connection = connections_.begin()->first;
    g_dbus_connection_flush_sync(connection, nullptr, nullptr);
    g_dbus_connection_close_sync(connection, nullptr, nullptr);
    remove_connection(connection);
  }
  // GIO cancellation completes asynchronously on this owning main context.
  // Drain the receivers while their callback owner is still alive.
  while (pending_forwards_ != 0 || observer_call_ != nullptr)
    g_main_context_iteration(nullptr, TRUE);
  remove_owned_endpoint();
  if (lock_fd_ >= 0) {
    flock(lock_fd_, LOCK_UN);
    close(lock_fd_);
    lock_fd_ = -1;
  }
  if (session_ != nullptr && !g_dbus_connection_is_closed(session_)) {
    if (observer_registration_ != 0)
      g_dbus_connection_unregister_object(session_, observer_registration_);
    observer_registration_ = 0;
    GVariant *release_reply = g_dbus_connection_call_sync(
        session_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", kBusName),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr,
        nullptr);
    if (release_reply != nullptr)
      g_variant_unref(release_reply);
    g_signal_handlers_disconnect_by_data(session_, this);
    if (shell_owner_subscription_ != 0)
      g_dbus_connection_signal_unsubscribe(session_, shell_owner_subscription_);
    shell_owner_subscription_ = 0;
    g_dbus_connection_flush_sync(session_, nullptr, nullptr);
    g_dbus_connection_close_sync(session_, nullptr, nullptr);
  }
  if (sigterm_source_ != 0 &&
      g_main_context_find_source_by_id(nullptr, sigterm_source_) != nullptr)
    g_source_remove(sigterm_source_);
  if (sigint_source_ != 0 &&
      g_main_context_find_source_by_id(nullptr, sigint_source_) != nullptr)
    g_source_remove(sigint_source_);
  sigterm_source_ = 0;
  sigint_source_ = 0;
  if (server_ != nullptr) {
    g_object_unref(server_);
    server_ = nullptr;
  }
  if (observer_ != nullptr) {
    g_object_unref(observer_);
    observer_ = nullptr;
  }
  if (session_ != nullptr) {
    g_object_unref(session_);
    session_ = nullptr;
  }
  if (node_info_ != nullptr) {
    g_dbus_node_info_unref(node_info_);
    node_info_ = nullptr;
  }
  if (loop_ != nullptr) {
    g_main_loop_unref(loop_);
    loop_ = nullptr;
  }
}

bool desktop_ready() {
  GError *error = nullptr;
  GDBusConnection *connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (connection == nullptr) {
    g_clear_error(&error);
    return false;
  }
  const guint64 deadline = now_us() + kObserverHorizonUs;
  GVariant *reply = g_dbus_connection_call_sync(
      connection, kBusName, kObserverPath, kObserverInterface, "ReadState",
      g_variant_new("(t)", deadline), G_VARIANT_TYPE("(usstty)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 100, nullptr, &error);
  g_object_unref(connection);
  if (reply == nullptr) {
    g_clear_error(&error);
    return false;
  }
  guint outcome = kOutcomeFailed;
  const char *helper = nullptr;
  const char *extension = nullptr;
  guint64 source_generation = 0;
  guint64 state_generation = 0;
  guchar role = kUnknownRole;
  g_variant_get(reply, "(u&s&stty)", &outcome, &helper, &extension,
                &source_generation, &state_generation, &role);
  const bool ready = outcome == kOutcomeState && is_lower_hex(helper, 32) &&
                     is_lower_hex(extension, 32) && role <= 1 &&
                     now_us() < deadline;
  g_variant_unref(reply);
  return ready;
}

} // namespace

int main(int argc, char **argv) {
  umask(0077);
  if (argc == 2 && std::strcmp(argv[1], "--check-ready") == 0)
    return desktop_ready() ? 0 : 75;
#ifdef PUNTO_TEST_SD_LOGIN
  if (argc >= 2 && std::strcmp(argv[1], "--authorize-peer") == 0) {
    if (argc != 4) {
      g_printerr("punto-session-bridge: authorize-peer-usage\n");
      return 64;
    }
    unsigned long long peer_uid_value = 0;
    unsigned long long peer_pid_value = 0;
    std::string_view uid_text = argv[2];
    std::string_view pid_text = argv[3];
    if (!parse_u64_token(uid_text, peer_uid_value) || !uid_text.empty() ||
        !uid_fits(peer_uid_value) ||
        !parse_u64_token(pid_text, peer_pid_value) || !pid_text.empty() ||
        peer_pid_value > static_cast<unsigned long long>(
                             std::numeric_limits<pid_t>::max())) {
      g_printerr("punto-session-bridge: authorize-peer-usage\n");
      return 64;
    }
    const bool accepted = authorize_root_peer(
        static_cast<uid_t>(peer_uid_value), static_cast<pid_t>(peer_pid_value));
    g_print("accepted=%u\n", accepted ? 1U : 0U);
    return accepted ? 0 : 2;
  }
#else
  (void)argc;
  (void)argv;
#endif
  SessionBridge bridge;
  if (!bridge.start())
    return bridge.permanent_failure() ? 64 : 75;
  bridge.run();
  return bridge.restart_required() ? 1 : 0;
}
