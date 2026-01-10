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

// Указатель на EventLoop для корректной остановки из signal handler
punto::EventLoop *g_event_loop = nullptr;

void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    if (g_event_loop) {
      g_event_loop->request_stop();
    }
  }
}

void print_version() {
  std::cout << "Punto Switcher 2.8.3 (C++20)\n"
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

  // Установка обработчиков сигналов
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // Загрузка конфигурации
  auto config = punto::load_config();

  // Логирование настроек auto_switch для отладки
  std::cerr << "[punto] auto_switch: enabled=" << config.auto_switch.enabled
            << ", threshold=" << config.auto_switch.threshold
            << ", min_word_len=" << config.auto_switch.min_word_len
            << ", min_score=" << config.auto_switch.min_score
            << ", max_rollback_words=" << config.auto_switch.max_rollback_words
            << '\n';

  // Запуск event loop
  punto::EventLoop loop{std::move(config)};

  // Регистрируем EventLoop для signal handler
  g_event_loop = &loop;

  int result = loop.run();

  // Сбрасываем указатель после завершения
  g_event_loop = nullptr;

  return result;
}
