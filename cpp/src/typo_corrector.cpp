/**
 * @file typo_corrector.cpp
 * @brief Реализация алгоритмов исправления опечаток
 */

#include "punto/typo_corrector.hpp"
#include "punto/scancode_map.hpp"
#include "punto/text_processor.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

namespace punto {

namespace {

constexpr std::array<std::string_view, 22> kKnownAbbreviations{
    "api",  "url",  "uuid", "json",  "yaml", "http", "https", "ssh",
    "dns",  "sql",  "cpu",  "gpu",   "ram",  "sdk",  "cli",   "gui",
    "rest", "grpc", "jwt",  "oauth", "tls",  "ssl"};

[[nodiscard]] std::string to_ascii_lower(std::span<const KeyEntry> word) {
  std::string ascii;
  ascii.reserve(word.size());

  for (const auto &entry : word) {
    if (!is_typeable_letter(entry.code) ||
        entry.code >= kScancodeToChar.size()) {
      return {};
    }

    char c = kScancodeToChar[entry.code];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
    if (c < 'a' || c > 'z') {
      return {};
    }
    ascii.push_back(c);
  }

  return ascii;
}

[[nodiscard]] bool
is_likely_abbreviation(std::span<const KeyEntry> word) noexcept {
  if (word.size() < 2 || word.size() > 8) {
    return false;
  }

  for (const auto &entry : word) {
    if (!is_typeable_letter(entry.code) || !entry.shifted) {
      return false;
    }
  }

  const std::string ascii = to_ascii_lower(word);
  for (std::string_view known : kKnownAbbreviations) {
    if (ascii == known) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::vector<std::string_view> utf8_units(std::string_view value) {
  std::vector<std::string_view> units;
  units.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    std::size_t len = utf8_char_len(static_cast<unsigned char>(value[i]));
    if (len == 0 || i + len > value.size()) {
      len = 1;
    } else {
      for (std::size_t offset = 1; offset < len; ++offset) {
        const auto byte = static_cast<unsigned char>(value[i + offset]);
        if ((byte & 0xC0U) != 0x80U) {
          len = 1;
          break;
        }
      }
    }
    units.push_back(value.substr(i, len));
    i += len;
  }
  return units;
}

template <typename Left, typename Right>
[[nodiscard]] std::size_t damerau_distance(const Left &left,
                                           const Right &right) {
  const std::size_t len1 = left.size();
  const std::size_t len2 = right.size();
  if (len1 == 0) {
    return len2;
  }
  if (len2 == 0) {
    return len1;
  }

  std::vector<std::size_t> previous_previous(len2 + 1U);
  std::vector<std::size_t> previous(len2 + 1U);
  std::vector<std::size_t> current(len2 + 1U);
  for (std::size_t j = 0; j <= len2; ++j) {
    previous[j] = j;
  }

  for (std::size_t i = 1; i <= len1; ++i) {
    current[0] = i;
    for (std::size_t j = 1; j <= len2; ++j) {
      const std::size_t cost = (left[i - 1] == right[j - 1]) ? 0U : 1U;
      current[j] = std::min(
          {previous[j] + 1U, current[j - 1] + 1U, previous[j - 1] + cost});
      if (i > 1 && j > 1 && left[i - 1] == right[j - 2] &&
          left[i - 2] == right[j - 1]) {
        current[j] = std::min(current[j], previous_previous[j - 2] + cost);
      }
    }
    previous_previous.swap(previous);
    previous.swap(current);
  }
  return previous[len2];
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
  StickyShiftResult result;
  if (current_layout != 0 && current_layout != 1) {
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
  const auto left = utf8_units(s1);
  const auto right = utf8_units(s2);
  return damerau_distance(left, right);
}

std::size_t damerau_levenshtein_distance(std::span<const KeyEntry> word1,
                                         std::span<const KeyEntry> word2) {

  struct CodeView {
    std::span<const KeyEntry> entries;
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] ScanCode operator[](std::size_t index) const noexcept {
      return entries[index].code;
    }
  };
  return damerau_distance(CodeView{word1}, CodeView{word2});
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
    const bool source_upper = c >= 'A' && c <= 'Z';
    if (source_upper) {
      c = static_cast<char>(c + 32); // К нижнему регистру для поиска scancode
    }
    if (preserve_case && i < original_word.size()) {
      shifted = original_word[i].shifted;
    } else if (source_upper) {
      shifted = true;
    }

    // Ищем scancode для символа
    ScanCode code = 0;
    for (std::size_t sc = 0; sc < kScancodeToChar.size(); ++sc) {
      if (kScancodeToChar[sc] == c) {
        code = static_cast<ScanCode>(sc);
        break;
      }
    }

    if (code == 0) {
      return {};
    }
    result.emplace_back(code, shifted);
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
    {'.', "ю"}, {'z', "я"}, {'`', "ё"}, {'/', "."},
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
    {"ю", '.'}, {"я", 'z'}, {"ё", '`'}, {".", '/'},
};
// clang-format on

} // anonymous namespace

std::string keys_to_utf8(std::span<const KeyEntry> word, bool is_english) {
  std::string result;
  // UTF-8 кириллица = 2 байта на символ
  result.reserve(word.size() * 2);

  for (const auto &entry : word) {
    if (entry.code >= kScancodeToChar.size()) {
      return {};
    }

    char c = kScancodeToChar[entry.code];
    if (c == '\0') {
      return {};
    }

    // Приводим к нижнему регистру
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }

    if (is_english) {
      // Для английского — просто ASCII
      if ((c >= 'a' && c <= 'z')) {
        result += c;
      } else {
        return {};
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
      if (!found) {
        return {};
      }
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

      if (code == 0) {
        return {};
      }
      bool shifted = false;
      if (preserve_case && key_idx < original_word.size()) {
        shifted = original_word[key_idx].shifted;
      }
      result.emplace_back(code, shifted);
      ++key_idx;
    } else {
      return {};
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
