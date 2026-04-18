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

#include <X11/Xlib.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <unistd.h>

namespace {

std::atomic<int> g_stop_pipe_write_fd{-1};

void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    const int fd = g_stop_pipe_write_fd.load(std::memory_order_relaxed);
    if (fd >= 0) {
      constexpr char kStopByte = 'x';
      const ssize_t ignored = ::write(fd, &kStopByte, sizeof(kStopByte));
      (void)ignored;
    }
  }
}

void print_version() {
  std::cout << "Punto Switcher 2.8.4 (C++20)\n"
            << "Высокопроизводительный плагин для interception-tools\n"
            << "https://github.com/antonshalin76/punto\n";
}

void print_usage(const char *argv0) {
  std::cout << "Использование: " << argv0 << " [опции]\n"
            << "\n"
            << "Опции:\n"
            << "  -h, --help     Показать эту справку\n"
            << "  -v, --version  Показать версию\n"
            << "\n"
            << "Горячие клавиши:\n"
            << "  Pause              Инвертировать раскладку слова\n"
            << "  Shift+Pause        Инвертировать раскладку выделения\n"
            << "  Ctrl+Pause         Инвертировать регистр слова\n"
            << "  Alt+Pause          Инвертировать регистр выделения\n"
            << "  LCtrl+LAlt+Pause   Транслитерировать выделение\n"
            << "  Ctrl+Z             Отменить последнее исправление Punto (короткое окно после исправления)\n"
            << "\n"
            << "Конфигурация: /etc/punto/config.yaml\n";
}

} // namespace

int main(int argc, char *argv[]) {
  // Обработка аргументов командной строки
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      print_version();
      return 0;
    }
  }

  if (XInitThreads() == 0) {
    std::cerr << "[punto] Failed to initialize Xlib thread support\n";
    return 1;
  }

  punto::init_logging("punto", punto::LogLevel::Info);

  int stop_pipe[2] = {-1, -1};
  if (::pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    std::cerr << "[punto] Failed to create stop pipe: " << std::strerror(errno)
              << "\n";
    return 1;
  }

  g_stop_pipe_write_fd.store(stop_pipe[1], std::memory_order_relaxed);

  // Установка обработчиков сигналов
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // Загрузка конфигурации
  auto config = punto::load_config();
  punto::update_log_level(config.logging.level);

  // Логирование настроек auto_switch для отладки
  std::cerr << "[punto] auto_switch: enabled=" << config.auto_switch.enabled
            << ", threshold=" << config.auto_switch.threshold
            << ", min_word_len=" << config.auto_switch.min_word_len
            << ", min_score=" << config.auto_switch.min_score
            << ", max_rollback_words=" << config.auto_switch.max_rollback_words
            << '\n';

  // Запуск event loop
  punto::EventLoop loop{std::move(config)};
  loop.set_stop_signal_fd(stop_pipe[0]);

  int result = loop.run();

  g_stop_pipe_write_fd.store(-1, std::memory_order_relaxed);
  if (stop_pipe[0] >= 0) {
    ::close(stop_pipe[0]);
  }
  if (stop_pipe[1] >= 0) {
    ::close(stop_pipe[1]);
  }
  punto::shutdown_logging();

  return result;
}
