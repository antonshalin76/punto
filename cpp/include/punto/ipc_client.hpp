/**
 * @file ipc_client.hpp
 * @brief IPC клиент для связи с punto сервисом
 *
 * Используется tray-приложением для отправки команд и получения статуса.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace punto {

/// Статус сервиса punto
enum class ServiceStatus {
  Unknown, // Не удалось получить статус
  Enabled, // Автопереключение включено
  Disabled // Автопереключение выключено
};

/// Классификация отказа одного IPC-обмена.
enum class IpcClientError {
  None,
  InvalidRequest,
  PathTooLong,
  PermissionDenied,
  Unavailable,
  TimedOut,
  IoError,
  ProtocolError,
  ServerRejected,
};

/// Результат одного запроса к ровно одному Unix-сокету.
struct IpcClientResult {
  IpcClientError error = IpcClientError::None;
  ServiceStatus status = ServiceStatus::Unknown;
  std::string response;

  [[nodiscard]] bool ok() const noexcept {
    return error == IpcClientError::None;
  }
};

/**
 * @brief IPC клиент для связи с punto сервисом через Unix Domain Socket
 */
class IpcClient {
public:
  /**
   * @brief Путь к сокету сервиса
   */
  static constexpr const char *kSocketPath = "/var/run/punto.sock";

  /**
   * @brief Timeout для операций (в миллисекундах)
   */
  static constexpr int kTimeoutMs = 1000;

  /// Максимальный размер ответа без завершающего LF.
  static constexpr std::size_t kMaxResponseBytes = 4096U;

  /**
   * @brief Получает текущий статус сервиса
   * @return Статус сервиса или Unknown при ошибке
   */
  static ServiceStatus get_status();

  /**
   * @brief Отправляет команду перезагрузки конфигурации
   * @param config_path Абсолютный путь к конфигу; если пусто — сервер сам решит
   * @return true при успехе
   */
  static bool reload_config(const std::string &config_path = {});

  /**
   * @brief Проверяет, доступен ли сервис
   * @return true если сервис отвечает
   */
  static bool is_service_available();

  /**
   * @brief Выполняет read-only GET_STATUS для явно заданного сокета.
   *
   * Этот API предназначен для диагностики fallback-инстансов. Обычные методы
   * tray всегда обращаются только к kSocketPath.
   */
  [[nodiscard]] static IpcClientResult
  diagnose_socket(const std::string &socket_path);

  /**
   * @brief Перечисляет fallback-сокеты только для явной диагностики.
   */
  [[nodiscard]] static std::vector<std::string> list_diagnostic_socket_paths();

#if defined(PUNTO_IPC_CLIENT_INTERNAL_TESTING)
  [[nodiscard]] static IpcClientResult
  exchange_for_test(const std::string &command, const std::string &socket_path);
#endif

private:
  /**
   * @brief Отправляет команду и получает ответ
   * @param command Команда для отправки
   * @return Типизированный результат IPC-обмена
   */
  [[nodiscard]] static IpcClientResult send_command(const std::string &command);

  [[nodiscard]] static IpcClientResult
  send_command_to_socket(const std::string &command,
                         const std::string &socket_path);
};

} // namespace punto
