/**
 * @file dictionary.cpp
 * @brief Реализация словарного анализатора с загрузкой из hunspell
 */

#include "punto/dictionary.hpp"
#include "punto/scancode_map.hpp"

#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <linux/input.h>

namespace punto {

namespace {

// Пути к hunspell словарям
constexpr const char *kEnDictPath = "/usr/share/hunspell/en_US.dic";
constexpr const char *kRuDictPath = "/usr/share/hunspell/ru_RU.dic";

// Минимальная и максимальная длина слов для загрузки
constexpr std::size_t kDictMinWordLen = 2;
constexpr std::size_t kDictMaxWordLen = 20;

/// Таблица конвертации UTF-8 кириллицы -> ASCII QWERTY
/// Ключ: 2-байтовый UTF-8 код (без учёта первого байта 0xD0/0xD1)
/// Значение: ASCII символ на QWERTY клавиатуре
struct CyrToQwerty {
  const char *utf8;
  char qwerty;
};

// clang-format off
constexpr CyrToQwerty kCyrillicMap[] = {
    // Строчные
    {"а", 'f'}, {"б", ','}, {"в", 'd'}, {"г", 'u'}, {"д", 'l'},
    {"е", 't'}, {"ж", ';'}, {"з", 'p'}, {"и", 'b'}, {"й", 'q'},
    {"к", 'r'}, {"л", 'k'}, {"м", 'v'}, {"н", 'y'}, {"о", 'j'},
    {"п", 'g'}, {"р", 'h'}, {"с", 'c'}, {"т", 'n'}, {"у", 'e'},
    {"ф", 'a'}, {"х", '['}, {"ц", 'w'}, {"ч", 'x'}, {"ш", 'i'},
    {"щ", 'o'}, {"ъ", ']'}, {"ы", 's'}, {"ь", 'm'}, {"э", '\''},
    {"ю", '.'}, {"я", 'z'}, {"ё", '`'},
    // Заглавные -> в нижний регистр
    {"А", 'f'}, {"Б", ','}, {"В", 'd'}, {"Г", 'u'}, {"Д", 'l'},
    {"Е", 't'}, {"Ж", ';'}, {"З", 'p'}, {"И", 'b'}, {"Й", 'q'},
    {"К", 'r'}, {"Л", 'k'}, {"М", 'v'}, {"Н", 'y'}, {"О", 'j'},
    {"П", 'g'}, {"Р", 'h'}, {"С", 'c'}, {"Т", 'n'}, {"У", 'e'},
    {"Ф", 'a'}, {"Х", '['}, {"Ц", 'w'}, {"Ч", 'x'}, {"Ш", 'i'},
    {"Щ", 'o'}, {"Ъ", ']'}, {"Ы", 's'}, {"Ь", 'm'}, {"Э", '\''},
    {"Ю", '.'}, {"Я", 'z'}, {"Ё", '`'},
};
// clang-format on

/// Извлекает слово из строки hunspell (формат: word/flags)
std::string extract_word(const std::string &line) {
  auto slash_pos = line.find('/');
  if (slash_pos != std::string::npos) {
    return line.substr(0, slash_pos);
  }
  return line;
}

/// Приводит к нижнему регистру (только ASCII)
std::string to_lowercase_ascii(const std::string &s) {
  std::string result;
  result.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      result += static_cast<char>(c + 32);
    } else {
      result += c;
    }
  }
  return result;
}

/// Проверяет, содержит ли строка только ASCII буквы
bool is_ascii_alpha_only(const std::string &s) {
  for (char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
      return false;
    }
  }
  return true;
}

} // namespace

std::string Dictionary::cyrillic_to_qwerty(const std::string &cyrillic) {
  std::string result;
  result.reserve(cyrillic.size());

  std::size_t i = 0;
  while (i < cyrillic.size()) {
    bool found = false;

    // Проверяем 2-байтовые UTF-8 последовательности
    if (i + 1 < cyrillic.size()) {
      for (const auto &entry : kCyrillicMap) {
        std::size_t len = std::strlen(entry.utf8);
        if (i + len <= cyrillic.size() &&
            std::memcmp(cyrillic.data() + i, entry.utf8, len) == 0) {
          result += entry.qwerty;
          i += len;
          found = true;
          break;
        }
      }
    }

    if (!found) {
      // Неизвестный символ — пропускаем или указываем на ошибку
      char c = cyrillic[i];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        // ASCII буква — оставляем как есть (в нижнем регистре)
        result +=
            static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      ++i;
    }
  }

  return result;
}

std::string Dictionary::entries_to_key(std::span<const KeyEntry> entries) {
  std::string key;
  key.reserve(entries.size());

  for (const auto &entry : entries) {
    if (entry.code < 256) {
      char c = kScancodeToChar[entry.code];
      if (c != '\0') {
        // Приводим к нижнему регистру
        key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
    }
  }

  return key;
}

std::size_t Dictionary::load_en_dictionary(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "[punto] Не удалось открыть EN словарь: " << path << "\n";
    return 0;
  }

  std::string line;
  std::size_t count = 0;

  // Первая строка — количество слов, пропускаем
  std::getline(file, line);

  while (std::getline(file, line)) {
    std::string word = extract_word(line);

    // Фильтруем по длине и содержимому
    if (word.size() >= kDictMinWordLen && word.size() <= kDictMaxWordLen &&
        is_ascii_alpha_only(word)) {
      en_words_.insert(to_lowercase_ascii(word));
      ++count;
    }
  }

  return count;
}

std::size_t Dictionary::load_ru_dictionary(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "[punto] Не удалось открыть RU словарь: " << path << "\n";
    return 0;
  }

  std::string line;
  std::size_t count = 0;

  // Первая строка — количество слов, пропускаем
  std::getline(file, line);

  while (std::getline(file, line)) {
    std::string word = extract_word(line);

    // Фильтруем по длине (в символах UTF-8 это примерно word.size()/2)
    if (word.size() >= kDictMinWordLen * 2 &&
        word.size() <= kDictMaxWordLen * 2) {
      std::string qwerty = cyrillic_to_qwerty(word);

      // Проверяем, что конвертация прошла успешно
      if (qwerty.size() >= kDictMinWordLen &&
          qwerty.size() <= kDictMaxWordLen) {
        ru_words_.insert(qwerty);
        ++count;
      }
    }
  }

  return count;
}

bool Dictionary::initialize() {
  if (initialized_) {
    return true;
  }

  std::size_t en_count = load_en_dictionary(kEnDictPath);
  std::size_t ru_count = load_ru_dictionary(kRuDictPath);

  initialized_ = (en_count > 0 || ru_count > 0);

  std::cerr << "[punto] Dictionary: EN=" << en_count << " RU=" << ru_count
            << " words loaded\n";

  return initialized_;
}

DictResult Dictionary::lookup(std::span<const KeyEntry> entries) const {
  if (!initialized_ || entries.empty()) {
    return DictResult::Unknown;
  }

  std::string key = entries_to_key(entries);
  if (key.empty()) {
    return DictResult::Unknown;
  }

  bool in_en = en_words_.find(key) != en_words_.end();
  bool in_ru = ru_words_.find(key) != ru_words_.end();

  if (in_en && in_ru) {
    return DictResult::Both;
  }
  if (in_en) {
    return DictResult::English;
  }
  if (in_ru) {
    return DictResult::Russian;
  }

  return DictResult::Unknown;
}

} // namespace punto
