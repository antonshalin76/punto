/**
 * @file main.cpp
 * @brief Точка входа punto-tray
 *
 * Punto Tray - приложение для управления сервисом punto
 * через иконку в системном трее.
 */

#include "punto/tray_app.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void print_version() {
  std::cout << "Punto Tray 1.0.0\n"
            << "Приложение для управления Punto Switcher\n";
}

void print_usage(const char* argv0) {
  std::cout << "Использование: " << argv0 << " [опции]\n"
            << "\n"
            << "Опции:\n"
            << "  -h, --help     Показать эту справку\n"
            << "  -v, --version  Показать версию\n"
            << "\n"
            << "Приложение отображает иконку в системном трее\n"
            << "для управления сервисом Punto Switcher.\n";
}

} // namespace

int main(int argc, char* argv[]) {
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

  // Инициализация GTK
  gtk_init(&argc, &argv);

  // Проверяем доступность сервиса (silent; сервис может появиться позже)
  (void)punto::IpcClient::is_service_available();

  // Создаём и запускаем приложение
  punto::TrayApp app;
  
  if (!app.initialize()) {
    return 1;
  }

  return app.run();
}
