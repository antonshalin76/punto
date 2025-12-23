/**
 * @file ipc_client.hpp
 * @brief IPC клиент для связи с punto сервисом
 *
 * Используется tray-приложением для отправки команд и получения статуса.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace punto {

/// Статус сервиса punto
enum class ServiceStatus {
  Unknown,    // Не удалось получить статус
  Enabled,    // Автопереключение включено
  Disabled    // Автопереключение выключено
};

/**
 * @brief IPC клиент для связи с punto сервисом через Unix Domain Socket
 */
class IpcClient {
public:
  /**
   * @brief Путь к сокету сервиса
   */
  static constexpr const char* kSocketPath = "/var/run/punto.sock";

  /**
   * @brief Timeout для операций (в миллисекундах)
   */
  static constexpr int kTimeoutMs = 1000;

  /**
   * @brief Получает текущий статус сервиса
   * @return Статус сервиса или Unknown при ошибке
   */
  static ServiceStatus get_status();

  /**
   * @brief Устанавливает статус сервиса (вкл/выкл)
   * @param enabled true = включить, false = выключить
   * @return true при успехе
   */
  static bool set_status(bool enabled);

  /**
   * @brief Отправляет команду перезагрузки конфигурации
   * @param config_path Абсолютный путь к конфигу; если пусто — сервер сам решит
   * @return true при успехе
   */
  static bool reload_config(const std::string& config_path = {});

  /**
   * @brief Проверяет, доступен ли сервис
   * @return true если сервис отвечает
   */
  static bool is_service_available();

private:
  [[nodiscard]] static std::vector<std::string> list_socket_paths();

  /**
   * @brief Отправляет команду и получает ответ
   * @param command Команда для отправки
   * @return Ответ сервиса или nullopt при ошибке
   */
  static std::optional<std::string> send_command(const std::string& command);

  [[nodiscard]] static std::optional<std::string>
  send_command_to_socket(const std::string& command, const std::string& socket_path);
};

} // namespace punto
