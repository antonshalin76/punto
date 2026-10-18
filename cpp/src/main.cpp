/**
 * @file main.cpp
 * @brief Точка входа Punto Switcher
 *
 * Punto Switcher для Linux (C++20 версия)
 * Высокопроизводительный плагин для interception-tools
 *
 * Запуск: sudo intercept -g /dev/input/eventX | punto | uinput -d
 * /dev/input/eventX
 */

#include "punto/config.hpp"
#include "punto/event_loop.hpp"
#include "punto/logger.hpp"
#include "punto/xorg_input_transport.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <charconv>
#include <optional>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

#ifndef PUNTO_POLICY_HASH
#define PUNTO_POLICY_HASH 0
#endif
#ifndef PUNTO_BUILD_ID
#define PUNTO_BUILD_ID 0
#endif

volatile sig_atomic_t g_stop_pipe_write_fd = -1;

void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    const int saved_errno = errno;
    const int fd = static_cast<int>(g_stop_pipe_write_fd);
    if (fd >= 0) {
      constexpr char kStopByte = 'x';
      const ssize_t ignored = ::write(fd, &kStopByte, sizeof(kStopByte));
      (void)ignored;
    }
    errno = saved_errno;
  }
}

void print_version() {
  std::cout << "Punto Switcher " PUNTO_VERSION " (C++20)\n"
            << "Высокопроизводительный плагин для interception-tools\n"
            << "https://github.com/antonshalin76/punto\n";
}

std::optional<int> inherited_fd(const char *name) {
  const char *raw = std::getenv(name);
  if (!raw || !*raw)
    return std::nullopt;
  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(raw, &end, 10);
  if (errno != 0 || *end != '\0' || value < 3 ||
      value > std::numeric_limits<int>::max() ||
      ::fcntl(static_cast<int>(value), F_GETFD) < 0)
    return std::nullopt;
  return static_cast<int>(value);
}

void print_usage(std::ostream &output, const char *argv0) {
  output << "Использование: " << argv0 << " [опции]\n"
         << "\n"
         << "Опции:\n"
         << "  -h, --help     Показать эту справку\n"
         << "  -v, --version  Показать версию\n"
         << "\n"
         << "Рабочий режим v" PUNTO_VERSION ":\n"
         << "  Pause — смена раскладки текущего слова\n"
         << "  Shift/Ctrl/Alt+Pause — операции с выделением и регистром\n"
         << "  Ctrl+Z — отмена последнего исправления Punto\n"
         << "  Автокоррекция применяется при пробеле или Tab\n"
         << "\n"
         << "Конфигурация: /etc/punto/config.yaml\n";
}

} // namespace

int main(int argc, char *argv[]) {
  bool logging_started = false;
  try {
    bool show_help = false;
    bool show_version = false;
    bool xorg_ingress = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        show_help = true;
      } else if (arg == "-v" || arg == "--version") {
        show_version = true;
      } else if (arg == "--xorg-ingress") {
        xorg_ingress = true;
      } else {
        std::cerr << "[punto] Unknown option: " << arg << "\n";
        print_usage(std::cerr, argv[0]);
        return 2;
      }
    }
    if (show_help) {
      print_usage(std::cout, argv[0]);
      return 0;
    }
    if (show_version) {
      print_version();
      return 0;
    }

    punto::init_logging("punto", punto::LogLevel::Info);
    logging_started = true;

#ifdef PUNTO_ENABLE_TEST_SEAMS
    if (const char *inject = std::getenv("PUNTO_TEST_MAIN_EXCEPTION");
        inject != nullptr && std::string_view{inject} == "1") {
      throw std::runtime_error{"injected main exception"};
    }
#endif

    int stop_pipe[2] = {-1, -1};
    if (::pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
      std::cerr << "[punto] Failed to create stop pipe: "
                << std::strerror(errno) << "\n";
      punto::shutdown_logging();
      return 2;
    }

    g_stop_pipe_write_fd = stop_pipe[1];

    // Установка обработчиков сигналов
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    struct sigaction ignore_sigpipe {};
    ignore_sigpipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sigpipe.sa_mask);
    ignore_sigpipe.sa_flags = 0;

    if (::sigaction(SIGPIPE, &ignore_sigpipe, nullptr) != 0 ||
        ::sigaction(SIGINT, &sa, nullptr) != 0 ||
        ::sigaction(SIGTERM, &sa, nullptr) != 0) {
      const int error = errno;
      g_stop_pipe_write_fd = -1;
      ::close(stop_pipe[0]);
      ::close(stop_pipe[1]);
      std::cerr << "[punto] Failed to install signal handlers: "
                << std::strerror(error) << "\n";
      punto::shutdown_logging();
      return 2;
    }

    // Startup runs in the udevmon service account. Its HOME is not an authority
    // for the active desktop user, so only the system config is trusted here.
    // EventLoop applies a session user's config after bounded session
    // discovery.
    auto loaded_config =
        punto::load_config_checked(std::filesystem::path{punto::kConfigPath});
    if (loaded_config.result != punto::ConfigResult::Ok) {
      g_stop_pipe_write_fd = -1;
      ::close(stop_pipe[0]);
      ::close(stop_pipe[1]);
      std::cerr << "[punto] FATAL: " << loaded_config.error << "\n";
      punto::shutdown_logging();
      return 2;
    }
    auto config = std::move(loaded_config.config);
    punto::update_log_level(config.logging.level);

    std::unique_ptr<punto::XorgInputTransport> ingress_transport;
    int ingress_ready_fd = -1;
    std::string ingress_display;
    std::uint32_t ingress_desktop_uid = 0;
    if (xorg_ingress) {
      const auto bootstrap_fd = inherited_fd("PUNTO_INGRESS_BOOTSTRAP_FD");
      const auto ready_fd = inherited_fd("PUNTO_INGRESS_READY_FD");
      const char *socket_path = std::getenv("PUNTO_INGRESS_SOCKET");
      const char *display = std::getenv("PUNTO_INGRESS_TARGET_DISPLAY");
      const char *uid_text = std::getenv("PUNTO_INGRESS_DESKTOP_UID");
      if (display && uid_text) {
        ingress_display = display;
        const char *end = uid_text + std::strlen(uid_text);
        auto parsed = std::from_chars(uid_text, end, ingress_desktop_uid);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
          ingress_desktop_uid = 0;
        }
      }
      if (!bootstrap_fd || !ready_fd || *bootstrap_fd == *ready_fd ||
          !socket_path || !*socket_path || ingress_display.empty() ||
          ingress_desktop_uid == 0 || PUNTO_POLICY_HASH == 0 ||
          PUNTO_BUILD_ID == 0) {
        std::cerr << "[punto] ingress pre-capture configuration refused\n";
        punto::shutdown_logging();
        return 2;
      }
      ingress_transport = punto::XorgInputTransport::connect_authenticated(
          socket_path, *bootstrap_fd, PUNTO_POLICY_HASH, PUNTO_BUILD_ID,
          std::chrono::milliseconds{1000});
      ::close(*bootstrap_fd);
      if (!ingress_transport) {
        std::cerr << "[punto] ingress pre-capture authentication refused\n";
        ::close(*ready_fd);
        punto::shutdown_logging();
        return 2;
      }
      ingress_ready_fd = *ready_fd;
    }

    // Логирование настроек auto_switch для отладки
    std::cerr << "[punto] auto_switch: enabled=" << config.auto_switch.enabled
              << ", threshold=" << config.auto_switch.threshold
              << ", min_word_len=" << config.auto_switch.min_word_len
              << ", min_score=" << config.auto_switch.min_score
              << ", max_rollback_words="
              << config.auto_switch.max_rollback_words << '\n';

    int result = 2;
    {
      // All asynchronous producers owned by EventLoop must stop before the
      // process-wide logger is torn down.
      punto::EventLoop loop{std::move(config)};
      loop.set_stop_signal_fd(stop_pipe[0]);
      if (ingress_transport &&
          !loop.set_ingress_transport(std::move(ingress_transport),
                                      ingress_ready_fd,
                                      std::move(ingress_display),
                                      ingress_desktop_uid)) {
        std::cerr << "[punto] ingress target binding refused\n";
        ::close(ingress_ready_fd);
        return 2;
      }
      result = loop.run();
    }

    g_stop_pipe_write_fd = -1;
    if (stop_pipe[0] >= 0) {
      ::close(stop_pipe[0]);
    }
    if (stop_pipe[1] >= 0) {
      ::close(stop_pipe[1]);
    }
    punto::shutdown_logging();
    logging_started = false;

    return result;
  } catch (const std::exception &error) {
    g_stop_pipe_write_fd = -1;
    if (logging_started) {
      std::cerr << "[punto] FATAL: unhandled exception: " << error.what()
                << "\n";
      punto::shutdown_logging();
    }
    return 3;
  } catch (...) {
    g_stop_pipe_write_fd = -1;
    if (logging_started) {
      std::cerr << "[punto] FATAL: unhandled non-standard exception\n";
      punto::shutdown_logging();
    }
    return 3;
  }
}
