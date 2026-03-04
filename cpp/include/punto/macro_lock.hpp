/**
 * @file macro_lock.hpp
 * @brief Межпроцессная блокировка для сериализации макросов коррекции
 *
 * udevmon запускает отдельный punto-daemon для каждой клавиатуры.
 * Без координации несколько экземпляров могут одновременно:
 * - перезаписывать clipboard друг друга,
 * - переключать раскладку, ломая чужие коррекции,
 * - вставлять backspace/paste в одно и то же приложение.
 *
 * MacroLock использует flock() на /var/run/punto-macro.lock для
 * сериализации всех макросов (коррекций, hotkey-действий, undo).
 */

#pragma once

#include <chrono>

namespace punto {

/**
 * @brief Межпроцессная блокировка для макросов коррекции
 *
 * Один экземпляр на процесс. Использует flock(LOCK_EX) / flock(LOCK_UN).
 * Файл создаётся при первом вызове и НЕ удаляется (stale-safe).
 */
class MacroLock {
public:
  MacroLock();
  ~MacroLock();

  MacroLock(const MacroLock &) = delete;
  MacroLock &operator=(const MacroLock &) = delete;

  /**
   * @brief Пытается захватить эксклюзивную блокировку с таймаутом.
   * @param timeout Максимальное время ожидания
   * @return true если блокировка успешно захвачена
   */
  [[nodiscard]] bool try_lock(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

  /**
   * @brief Освобождает блокировку.
   *
   * Безопасно вызывать, если блокировка не была захвачена (no-op).
   */
  void unlock();

  /**
   * @brief Проверяет, захвачена ли блокировка текущим процессом.
   */
  [[nodiscard]] bool is_locked() const noexcept { return locked_; }

private:
  bool ensure_fd();

  int fd_ = -1;
  bool locked_ = false;

  static constexpr const char *kLockPath = "/var/run/punto-macro.lock";
};

/**
 * @brief RAII-обёртка для MacroLock
 *
 * Захватывает блокировку в конструкторе, освобождает в деструкторе.
 * Если блокировка не была получена (таймаут), owns_lock() == false.
 */
class MacroLockGuard {
public:
  explicit MacroLockGuard(
      MacroLock &lock,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{2000})
      : lock_(lock), owns_(lock.try_lock(timeout)) {}

  ~MacroLockGuard() {
    if (owns_) {
      lock_.unlock();
    }
  }

  MacroLockGuard(const MacroLockGuard &) = delete;
  MacroLockGuard &operator=(const MacroLockGuard &) = delete;

  [[nodiscard]] bool owns_lock() const noexcept { return owns_; }

private:
  MacroLock &lock_;
  bool owns_;
};

} // namespace punto
