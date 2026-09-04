/**
 * @file sound_manager.hpp
 * @brief Non-blocking, privilege-separated layout sound playback.
 */

#pragma once

#include <memory>

#ifdef PUNTO_TESTING
#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <sys/types.h>
#endif

namespace punto {

class X11Session;
struct X11SessionInfo;
struct SoundConfig;

#ifdef PUNTO_TESTING
enum class SoundLaunchResult {
  Completed,
  SpawnFailed,
  ExitedFailure,
  TimedOut,
  Stopped,
};

struct SoundManagerResolvedUser {
  std::string username;
  std::string home_dir;
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<gid_t> groups;
};

struct SoundLaunchRequest {
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<gid_t> groups;
  bool drop_privileges = true;
  std::string player_path;
  std::string sound_path;
  std::vector<std::string> environment;
};

struct SoundManagerTestOptions {
  bool session_valid = true;
  std::string player_path;
  std::optional<SoundManagerResolvedUser> resolved_user;
  bool drop_privileges = true;
  std::chrono::milliseconds minimum_launch_interval{100};
  std::chrono::milliseconds shutdown_wait{2500};
  std::function<std::optional<SoundManagerResolvedUser>(const X11SessionInfo &,
                                                        std::stop_token)>
      resolve_user;
  std::function<SoundLaunchResult(const SoundLaunchRequest &, std::stop_token)>
      launch;
};
#endif

class SoundManager {
public:
  SoundManager(const X11Session &x11_session, const SoundConfig &config);
  ~SoundManager();

  SoundManager(const SoundManager &) = delete;
  SoundManager &operator=(const SoundManager &) = delete;
  SoundManager(SoundManager &&) = delete;
  SoundManager &operator=(SoundManager &&) = delete;

  void set_enabled(bool enabled) noexcept;

  /// @param new_layout 0 = EN, 1 = RU; other values are ignored.
  void play_for_layout(int new_layout) noexcept;

#ifdef PUNTO_TESTING
  SoundManager(const X11SessionInfo &session, const SoundConfig &config,
               SoundManagerTestOptions options);

  [[nodiscard]] static std::optional<SoundManagerResolvedUser>
  resolve_user_for_test(const X11SessionInfo &session);

  [[nodiscard]] static SoundLaunchResult run_process_for_test(
      const SoundLaunchRequest &request, const std::string &helper_path,
      std::chrono::milliseconds maximum_runtime, std::stop_token stop);
#endif

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace punto
