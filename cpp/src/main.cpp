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

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

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

void print_usage(std::ostream &output, const char *argv0) {
  output << "Использование: " << argv0 << " [опции]\n"
         << "\n"
         << "Опции:\n"
         << "  -h, --help     Показать эту справку\n"
         << "  -v, --version  Показать версию\n"
         << "\n"
         << "Безопасный режим v" PUNTO_VERSION ":\n"
         << "  Pause и комбинации с Pause поглощаются без изменения текста\n"
         << "  Ctrl+Z остаётся обычным undo приложения\n"
         << "  Автоматические решения только анализируются и не применяются\n"
         << "\n"
         << "Конфигурация: /etc/punto/config.yaml\n";
}

} // namespace

int main(int argc, char *argv[]) {
  bool logging_started = false;
  try {
    bool show_help = false;
    bool show_version = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        show_help = true;
      } else if (arg == "-v" || arg == "--version") {
        show_version = true;
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
