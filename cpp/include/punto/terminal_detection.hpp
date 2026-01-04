/**
 * @file terminal_detection.hpp
 * @brief Утилиты детекции терминала по данным активного окна
 */

#pragma once

#include <string_view>

namespace punto {

/// Определяет, является ли окно терминалом по WM_CLASS (instance/class).
///
/// @param res_name  WM_CLASS instance (XClassHint::res_name)
/// @param res_class WM_CLASS class    (XClassHint::res_class)
/// @return true если строка содержит известные токены терминалов (case-insensitive)
[[nodiscard]] bool is_terminal_wm_class(std::string_view res_name,
                                       std::string_view res_class) noexcept;

} // namespace punto
