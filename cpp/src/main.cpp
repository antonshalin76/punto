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

#include <cstdlib>
#include <iostream>
#include <signal.h>

namespace {

volatile sig_atomic_t g_running = 1;

void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    g_running = 0;
  }
}

void print_version() {
  std::cout << "Punto Switcher 2.0.0 (C++20)\n"
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

  // Установка обработчиков сигналов
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // Загрузка конфигурации
  auto config = punto::load_config();

  // Запуск event loop
  punto::EventLoop loop{std::move(config)};

  return loop.run();
}
