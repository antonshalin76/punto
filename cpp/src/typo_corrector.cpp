/**
 * @file typo_corrector.cpp
 * @brief Реализация алгоритмов исправления опечаток
 */

#include "punto/typo_corrector.hpp"
#include "punto/scancode_map.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace punto {

namespace {

/// Проверяет, является ли буквенный скан-код гласной (EN или RU)
[[nodiscard]] bool is_vowel_key(ScanCode code) noexcept {
  // EN гласные: a, e, i, o, u
  // RU гласные: а(f), е(t), ё(`), и(b), о(j), у(e), ы(s), э('), ю(.), я(z)
  switch (code) {
  // EN гласные
  case KEY_A:
  case KEY_E:
  case KEY_I:
  case KEY_O:
  case KEY_U:
  // RU гласные (QWERTY коды)
  case KEY_F:          // а
  case KEY_T:          // е
  case KEY_B:          // и
  case KEY_J:          // о
  case KEY_S:          // ы
  case KEY_APOSTROPHE: // э
  case KEY_DOT:        // ю
  case KEY_Z:          // я
  case KEY_GRAVE:      // ё
    return true;
  default:
    return false;
  }
}

/// Подсчитывает количество гласных в слове
[[nodiscard]] std::size_t
count_vowels(std::span<const KeyEntry> word) noexcept {
  std::size_t count = 0;
  for (const auto &entry : word) {
    if (is_vowel_key(entry.code)) {
      ++count;
    }
  }
  return count;
}

/// Проверяет, похоже ли слово на аббревиатуру
/// Аббревиатуры обычно короткие (2-5 букв) и содержат мало гласных
[[nodiscard]] bool
is_likely_abbreviation(std::span<const KeyEntry> word) noexcept {
  // Только короткие слова (2-5 символов)
  if (word.size() < 2 || word.size() > 5) {
    return false;
  }

  // Подсчитываем гласные и заглавные
  std::size_t vowel_count = count_vowels(word);
  std::size_t upper_count = 0;
  std::size_t letter_count = 0;

  for (const auto &entry : word) {
    if (is_typeable_letter(entry.code)) {
      ++letter_count;
      if (entry.shifted) {
        ++upper_count;
      }
    }
  }

  // Если слово короткое (2-4 буквы) и мало гласных (0-1) — вероятно
  // аббревиатура Примеры: ДНК (0 гласных), СНГ (0), API (1), URL (1), СНиП (1)
  if (letter_count >= 2 && letter_count <= 4 && vowel_count <= 1) {
    // Дополнительная проверка: большинство букв заглавные
    if (upper_count >= letter_count / 2) {
      return true;
    }
  }

  // Также аббревиатуры: 2-3 буквы и все заглавные
  if (letter_count >= 2 && letter_count <= 3 && upper_count == letter_count) {
    return true;
  }

  return false;
}

} // namespace

// ===========================================================================
// Определение паттернов регистра
// ===========================================================================

CasePattern detect_case_pattern(std::span<const KeyEntry> word) {
  if (word.empty()) {
    return CasePattern::Unknown;
  }

  // ВАЖНО: Пропускаем вероятные аббревиатуры (СНиП, ДНК, API)
  if (is_likely_abbreviation(word)) {
    return CasePattern::Mixed; // Mixed означает "не исправлять"
  }

  // Подсчитываем позиции заглавных и строчных букв
  std::size_t upper_count = 0;
  std::size_t lower_count = 0;
  std::size_t first_lower_pos = word.size(); // Позиция первой строчной
  std::size_t first_upper_pos = word.size(); // Позиция первой заглавной
  std::size_t last_upper_pos = 0;

  for (std::size_t i = 0; i < word.size(); ++i) {
    const auto &entry = word[i];

    // Пропускаем не-буквы (используем is_typeable_letter для корректной
    // работы с русскими б и ю)
    if (!is_typeable_letter(entry.code)) {
      continue;
    }

    if (entry.shifted) {
      ++upper_count;
      if (first_upper_pos == word.size()) {
        first_upper_pos = i;
      }
      last_upper_pos = i;
    } else {
      ++lower_count;
      if (first_lower_pos == word.size()) {
        first_lower_pos = i;
      }
    }
  }

  const std::size_t total = upper_count + lower_count;
  if (total == 0) {
    return CasePattern::Unknown;
  }

  // Диагностика
  std::cerr << "[punto] detect_case_pattern: upper=" << upper_count
            << " lower=" << lower_count << " first_upper=" << first_upper_pos
            << " last_upper=" << last_upper_pos
            << " first_lower=" << first_lower_pos << std::endl;

  // Все строчные
  if (upper_count == 0) {
    return CasePattern::AllLower;
  }

  // Все заглавные
  if (lower_count == 0) {
    return CasePattern::AllUpper;
  }

  // Title case: первая заглавная, остальные строчные
  if (upper_count == 1 && first_upper_pos == 0) {
    return CasePattern::TitleCase;
  }

  // Sticky Shift (UU+L+): несколько заглавных в начале, затем строчные
  // Паттерн: заглавные идут подряд в начале, затем строчные
  // Пример: ПРивет (U=0,1 L=2,3,4,5) -> first_upper=0, last_upper=1,
  // first_lower=2
  if (first_upper_pos == 0 && last_upper_pos < first_lower_pos &&
      first_lower_pos < word.size() && upper_count >= 2) {
    // Проверяем, что заглавные идут подряд в начале
    bool consecutive_upper = true;
    for (std::size_t i = 0; i <= last_upper_pos; ++i) {
      if (i < word.size() && !word[i].shifted) {
        // Найдена строчная буква среди заглавных
        if (is_typeable_letter(word[i].code)) {
          consecutive_upper = false;
          break;
        }
      }
    }
    // Проверяем, что строчные идут подряд после заглавных
    bool consecutive_lower = true;
    for (std::size_t i = first_lower_pos; i < word.size(); ++i) {
      if (word[i].shifted) {
        // Найдена заглавная буква среди строчных
        if (is_typeable_letter(word[i].code)) {
          consecutive_lower = false;
          break;
        }
      }
    }

    if (consecutive_upper && consecutive_lower) {
      return CasePattern::StickyShiftUU;
    }
  }

  // Sticky Shift (L+U+): первая строчная, остальные заглавные (caps lock)
  // Пример: кОЛБАСА (L=0, U=1,2,3,4,5,6)
  if (lower_count == 1 && first_lower_pos == 0 && upper_count >= 2) {
    // Проверяем, что все заглавные идут подряд после первой строчной
    bool all_upper_after_first = true;
    for (std::size_t i = 1; i < word.size(); ++i) {
      if (!word[i].shifted) {
        // Найдена строчная буква после первой
        if (is_typeable_letter(word[i].code)) {
          all_upper_after_first = false;
          break;
        }
      }
    }

    if (all_upper_after_first) {
      return CasePattern::StickyShiftLU;
    }
  }

  // Смешанный регистр (СНиП) — НЕ исправляем
  return CasePattern::Mixed;
}

// ===========================================================================
// Исправление залипшего Shift
// ===========================================================================

StickyShiftResult detect_sticky_shift(std::span<const KeyEntry> word) {
  StickyShiftResult result;
  result.detected = false;
  result.needs_layout_fix = false;

  if (word.size() < 2) {
    return result;
  }

  CasePattern pattern = detect_case_pattern(word);

  if (pattern == CasePattern::StickyShiftUU) {
    // ПРивет -> Привет
    // Оставляем только первую букву заглавной
    result.detected = true;
    result.corrected.reserve(word.size());

    bool first_letter = true;
    for (const auto &entry : word) {
      if (is_typeable_letter(entry.code)) {
        // Первая буква — заглавная, остальные — строчные
        result.corrected.emplace_back(entry.code, first_letter);
        first_letter = false;
      } else {
        // Не-буквы оставляем как есть
        result.corrected.push_back(entry);
      }
    }

    return result;
  }

  if (pattern == CasePattern::StickyShiftLU) {
    // кОЛБАСА -> Колбаса
    // Инвертируем: первая заглавная, остальные строчные
    result.detected = true;
    result.corrected.reserve(word.size());

    bool first_letter = true;
    for (const auto &entry : word) {
      if (is_typeable_letter(entry.code)) {
        result.corrected.emplace_back(entry.code, first_letter);
        first_letter = false;
      } else {
        result.corrected.push_back(entry);
      }
    }

    return result;
  }

  return result;
}

StickyShiftResult
detect_sticky_shift_with_layout(std::span<const KeyEntry> word,
                                int current_layout) {

  // Сначала проверяем sticky shift в текущей раскладке
  StickyShiftResult result = detect_sticky_shift(word);
  if (result.detected) {
    return result;
  }

  // Если не нашли, пробуем с инверсией раскладки (для случая GHbdtn -> Привет)
  // Конвертируем слово в другую раскладку и проверяем паттерн
  //
  // GHbdtn (EN layout, нажат Shift для G,H) -> при инверсии -> ПРивет
  // После инверсии проверяем sticky shift

  // Примечание: инверсия раскладки не меняет shifted флаг,
  // только интерпретацию scancode в другой раскладке.
  // Поэтому GH (shifted) в EN = ПР (shifted) в RU

  // Проверяем паттерн как если бы слово было в другой раскладке
  CasePattern pattern = detect_case_pattern(word);

  if (pattern == CasePattern::StickyShiftUU ||
      pattern == CasePattern::StickyShiftLU) {
    // Нужна и смена раскладки, и исправление регистра
    result.detected = true;
    result.needs_layout_fix = true;
    result.corrected.reserve(word.size());

    bool first_letter = true;
    for (const auto &entry : word) {
      if (entry.code < kScancodeToChar.size()) {
        char c = kScancodeToChar[entry.code];
        if (c != '\0' && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
          result.corrected.emplace_back(entry.code, first_letter);
          first_letter = false;
          continue;
        }
      }
      result.corrected.push_back(entry);
    }
  }

  return result;
}

// ===========================================================================
// Расстояние Дамерау-Левенштейна
// ===========================================================================

std::size_t damerau_levenshtein_distance(std::string_view s1,
                                         std::string_view s2) {
  const std::size_t len1 = s1.size();
  const std::size_t len2 = s2.size();

  // Оптимизация для пустых строк
  if (len1 == 0)
    return len2;
  if (len2 == 0)
    return len1;

  // Оптимизация для одинаковых строк
  if (s1 == s2)
    return 0;

  // Создаём матрицу расстояний
  // Используем 2D вектор для простоты (O(n*m) память)
  std::vector<std::vector<std::size_t>> d(len1 + 1,
                                          std::vector<std::size_t>(len2 + 1));

  // Инициализация первой строки и столбца
  for (std::size_t i = 0; i <= len1; ++i) {
    d[i][0] = i;
  }
  for (std::size_t j = 0; j <= len2; ++j) {
    d[0][j] = j;
  }

  // Заполнение матрицы
  for (std::size_t i = 1; i <= len1; ++i) {
    for (std::size_t j = 1; j <= len2; ++j) {
      const std::size_t cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;

      // Минимум из: удаление, вставка, замена
      d[i][j] = std::min({
          d[i - 1][j] + 1,       // Удаление
          d[i][j - 1] + 1,       // Вставка
          d[i - 1][j - 1] + cost // Замена
      });

      // Транспозиция (перестановка соседних символов)
      if (i > 1 && j > 1 && s1[i - 1] == s2[j - 2] && s1[i - 2] == s2[j - 1]) {
        d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + cost);
      }
    }
  }

  return d[len1][len2];
}

std::size_t damerau_levenshtein_distance(std::span<const KeyEntry> word1,
                                         std::span<const KeyEntry> word2) {

  // Конвертируем в строки для сравнения (без учёта регистра)
  std::string s1 = keys_to_ascii(word1);
  std::string s2 = keys_to_ascii(word2);

  return damerau_levenshtein_distance(s1, s2);
}

// ===========================================================================
// Генерация кандидатов
// ===========================================================================

std::vector<std::string> generate_typo_candidates(std::string_view word,
                                                  std::size_t max_distance) {

  std::vector<std::string> candidates;

  // Для простоты пока не используем Hunspell suggest здесь,
  // т.к. это будет вызываться из Dictionary::suggest() в будущем.
  //
  // Здесь генерируем простые кандидаты для коротких расстояний.

  if (max_distance == 0 || word.empty()) {
    return candidates;
  }

  // Генерируем кандидатов с одной правкой (расстояние 1)
  std::string base{word};

  // 1. Удаление одного символа (исправление дубля)
  for (std::size_t i = 0; i < base.size(); ++i) {
    std::string candidate = base.substr(0, i) + base.substr(i + 1);
    if (!candidate.empty()) {
      candidates.push_back(std::move(candidate));
    }
  }

  // 2. Перестановка соседних символов (транспозиция)
  for (std::size_t i = 0; i + 1 < base.size(); ++i) {
    std::string candidate = base;
    std::swap(candidate[i], candidate[i + 1]);
    candidates.push_back(std::move(candidate));
  }

  // 3. Вставка одного символа (исправление пропуска)
  // Ограничиваем алфавитом a-z для скорости
  const std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz";
  for (std::size_t i = 0; i <= base.size(); ++i) {
    for (char c : alphabet) {
      std::string candidate = base.substr(0, i) + c + base.substr(i);
      candidates.push_back(std::move(candidate));
    }
  }

  // 4. Замена одного символа
  for (std::size_t i = 0; i < base.size(); ++i) {
    for (char c : alphabet) {
      if (c != base[i]) {
        std::string candidate = base;
        candidate[i] = c;
        candidates.push_back(std::move(candidate));
      }
    }
  }

  // Убираем дубликаты
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());

  return candidates;
}

// ===========================================================================
// Утилиты преобразования
// ===========================================================================

std::string keys_to_ascii(std::span<const KeyEntry> word) {
  std::string result;
  result.reserve(word.size());

  for (const auto &entry : word) {
    if (entry.code < kScancodeToChar.size()) {
      char c = kScancodeToChar[entry.code];
      if (c != '\0') {
        // Приводим к нижнему регистру
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c + 32);
        }
        result += c;
      }
    }
  }

  return result;
}

std::vector<KeyEntry> ascii_to_keys(std::string_view ascii, bool preserve_case,
                                    std::span<const KeyEntry> original_word) {

  std::vector<KeyEntry> result;
  result.reserve(ascii.size());

  for (std::size_t i = 0; i < ascii.size(); ++i) {
    char c = ascii[i];
    bool shifted = false;

    // Определяем регистр
    if (preserve_case && i < original_word.size()) {
      shifted = original_word[i].shifted;
    } else if (c >= 'A' && c <= 'Z') {
      shifted = true;
      c = static_cast<char>(c + 32); // К нижнему регистру для поиска scancode
    }

    // Ищем scancode для символа
    ScanCode code = 0;
    for (std::size_t sc = 0; sc < kScancodeToChar.size(); ++sc) {
      if (kScancodeToChar[sc] == c) {
        code = static_cast<ScanCode>(sc);
        break;
      }
    }

    if (code != 0) {
      result.emplace_back(code, shifted);
    }
  }

  return result;
}

// ===========================================================================
// Таблица соответствия QWERTY <-> Кириллица
// ===========================================================================

namespace {

/// Таблица соответствия: QWERTY (ASCII lower) -> UTF-8 кириллица (lower)
struct QwertyToCyrillic {
  char qwerty;
  const char *utf8;
};

// clang-format off
constexpr QwertyToCyrillic kQwertyToCyrMap[] = {
    {'f', "а"}, {',', "б"}, {'d', "в"}, {'u', "г"}, {'l', "д"},
    {'t', "е"}, {';', "ж"}, {'p', "з"}, {'b', "и"}, {'q', "й"},
    {'r', "к"}, {'k', "л"}, {'v', "м"}, {'y', "н"}, {'j', "о"},
    {'g', "п"}, {'h', "р"}, {'c', "с"}, {'n', "т"}, {'e', "у"},
    {'a', "ф"}, {'[', "х"}, {'w', "ц"}, {'x', "ч"}, {'i', "ш"},
    {'o', "щ"}, {']', "ъ"}, {'s', "ы"}, {'m', "ь"}, {'\'', "э"},
    {'.', "ю"}, {'z', "я"}, {'`', "ё"},
};
// clang-format on

/// Таблица соответствия: UTF-8 кириллица (lower) -> QWERTY (ASCII lower)
struct CyrillicToQwerty {
  const char *utf8;
  char qwerty;
};

// clang-format off
constexpr CyrillicToQwerty kCyrToQwertyMap[] = {
    {"а", 'f'}, {"б", ','}, {"в", 'd'}, {"г", 'u'}, {"д", 'l'},
    {"е", 't'}, {"ж", ';'}, {"з", 'p'}, {"и", 'b'}, {"й", 'q'},
    {"к", 'r'}, {"л", 'k'}, {"м", 'v'}, {"н", 'y'}, {"о", 'j'},
    {"п", 'g'}, {"р", 'h'}, {"с", 'c'}, {"т", 'n'}, {"у", 'e'},
    {"ф", 'a'}, {"х", '['}, {"ц", 'w'}, {"ч", 'x'}, {"ш", 'i'},
    {"щ", 'o'}, {"ъ", ']'}, {"ы", 's'}, {"ь", 'm'}, {"э", '\''},
    {"ю", '.'}, {"я", 'z'}, {"ё", '`'},
};
// clang-format on

} // anonymous namespace

std::string keys_to_utf8(std::span<const KeyEntry> word, bool is_english) {
  std::string result;
  // UTF-8 кириллица = 2 байта на символ
  result.reserve(word.size() * 2);

  for (const auto &entry : word) {
    if (entry.code >= kScancodeToChar.size()) {
      continue;
    }

    char c = kScancodeToChar[entry.code];
    if (c == '\0') {
      continue;
    }

    // Приводим к нижнему регистру
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }

    if (is_english) {
      // Для английского — просто ASCII
      if ((c >= 'a' && c <= 'z')) {
        result += c;
      }
    } else {
      // Для русского — конвертируем в кириллицу
      bool found = false;
      for (const auto &map : kQwertyToCyrMap) {
        if (map.qwerty == c) {
          result += map.utf8;
          found = true;
          break;
        }
      }
      // Если не нашли в таблице — пропускаем
      (void)found;
    }
  }

  return result;
}

std::vector<KeyEntry> utf8_to_keys(std::string_view utf8, bool is_english,
                                   bool preserve_case,
                                   std::span<const KeyEntry> original_word) {
  std::vector<KeyEntry> result;
  result.reserve(utf8.size());

  std::size_t key_idx = 0;
  std::size_t i = 0;

  while (i < utf8.size()) {
    char qwerty_char = '\0';
    std::size_t char_len = 1;

    if (is_english) {
      // Для английского — просто ASCII
      char c = utf8[i];
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c + 32);
      }
      if (c >= 'a' && c <= 'z') {
        qwerty_char = c;
      }
    } else {
      // Для русского — ищем UTF-8 кириллицу
      // UTF-8 кириллица = 2 байта (0xD0 или 0xD1 + второй байт)
      if (i + 1 < utf8.size()) {
        for (const auto &map : kCyrToQwertyMap) {
          std::size_t len = std::strlen(map.utf8);
          if (i + len <= utf8.size() &&
              std::memcmp(utf8.data() + i, map.utf8, len) == 0) {
            qwerty_char = map.qwerty;
            char_len = len;
            break;
          }
        }
      }

      // Если не нашли кириллицу — пробуем ASCII
      if (qwerty_char == '\0') {
        char c = utf8[i];
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c + 32);
        }
        if (c >= 'a' && c <= 'z') {
          qwerty_char = c;
        }
      }
    }

    if (qwerty_char != '\0') {
      // Ищем scancode для QWERTY символа
      ScanCode code = 0;
      for (std::size_t sc = 0; sc < kScancodeToChar.size(); ++sc) {
        char table_c = kScancodeToChar[sc];
        if (table_c >= 'A' && table_c <= 'Z') {
          table_c = static_cast<char>(table_c + 32);
        }
        if (table_c == qwerty_char) {
          code = static_cast<ScanCode>(sc);
          break;
        }
      }

      if (code != 0) {
        bool shifted = false;
        if (preserve_case && key_idx < original_word.size()) {
          shifted = original_word[key_idx].shifted;
        }
        result.emplace_back(code, shifted);
        ++key_idx;
      }
    }

    i += char_len;
  }

  return result;
}

std::vector<KeyEntry> apply_case_pattern(std::span<const KeyEntry> corrected,
                                         CasePattern target_pattern) {

  std::vector<KeyEntry> result;
  result.reserve(corrected.size());

  switch (target_pattern) {
  case CasePattern::AllLower:
    for (const auto &entry : corrected) {
      result.emplace_back(entry.code, false);
    }
    break;

  case CasePattern::AllUpper:
    for (const auto &entry : corrected) {
      result.emplace_back(entry.code, true);
    }
    break;

  case CasePattern::TitleCase:
  case CasePattern::StickyShiftUU:
  case CasePattern::StickyShiftLU: {
    bool first_letter = true;
    for (const auto &entry : corrected) {
      if (is_typeable_letter(entry.code)) {
        result.emplace_back(entry.code, first_letter);
        first_letter = false;
      } else {
        result.push_back(entry);
      }
    }
    break;
  }

  default:
    // Unknown или Mixed — копируем как есть
    result.assign(corrected.begin(), corrected.end());
    break;
  }

  return result;
}

} // namespace punto
