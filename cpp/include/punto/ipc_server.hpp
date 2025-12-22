/**
 * @file ipc_server.hpp
 * @brief IPC сервер для управления punto через Unix Domain Socket
 *
 * Позволяет внешним приложениям (punto-tray) управлять сервисом:
 * - Включать/выключать автопереключение
 * - Перезагружать конфигурацию
 * - Получать текущий статус
 */

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace punto {

/// Путь к Unix Domain Socket
inline constexpr const char* kIpcSocketPath = "/var/run/punto.sock";

/// Команды IPC протокола
enum class IpcCommand {
  Unknown,
  GetStatus,   // GET_STATUS -> ENABLED|DISABLED
  SetStatus,   // SET_STATUS 0|1 -> OK
  Reload,      // RELOAD -> OK|ERROR
  Shutdown     // SHUTDOWN -> OK (graceful stop)
};

/// Результат выполнения команды
struct IpcResult {
  bool success = false;
  std::string message;
};

/**
 * @brief IPC сервер на Unix Domain Socket
 *
 * Работает в отдельном потоке, не блокирует основной event loop.
 * Использует select/poll для неблокирующего приёма соединений.
 */
class IpcServer {
public:
  /// Callback для запроса перезагрузки конфига.
  /// Аргументом передается путь к конфигурационному файлу, если он был указан
  /// в команде RELOAD; иначе пустая строка.
  /// Важно: callback выполняется в IPC-потоке, поэтому должен быть потокобезопасным.
  using ReloadCallback = std::function<IpcResult(const std::string&)>;

  /**
   * @brief Конструктор
   * @param enabled_flag Атомарный флаг включения/выключения (shared с EventLoop)
   * @param reload_callback Callback для перезагрузки конфига
   */
  IpcServer(std::atomic<bool>& enabled_flag, ReloadCallback reload_callback);

  ~IpcServer();

  // Запрет копирования
  IpcServer(const IpcServer&) = delete;
  IpcServer& operator=(const IpcServer&) = delete;

  /**
   * @brief Запускает IPC сервер в отдельном потоке
   * @return true если сервер успешно запущен
   */
  bool start();

  /**
   * @brief Останавливает IPC сервер
   *
   * Ожидает завершения потока (join).
   */
  void stop();

  /**
   * @brief Проверяет, запущен ли сервер
   */
  [[nodiscard]] bool is_running() const noexcept;

private:
  /// Основной цикл сервера (выполняется в отдельном потоке)
  void server_loop();

  /// Обрабатывает входящее соединение
  void handle_client(int client_fd);

  /// Парсит и выполняет команду
  IpcResult execute_command(std::string_view cmd);

  /// Создаёт и настраивает серверный сокет
  [[nodiscard]] int create_socket();

  // Состояние сервера
  std::atomic<bool>& enabled_flag_;
  ReloadCallback reload_callback_;

  std::atomic<bool> running_{false};
  std::jthread server_thread_;
  int server_fd_ = -1;

  // Фактический путь сокета, на котором запущен этот экземпляр.
  // Может отличаться от kIpcSocketPath, если основной сокет уже занят.
  std::string socket_path_;
};

} // namespace punto
