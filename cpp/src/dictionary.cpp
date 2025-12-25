/**
 * @file dictionary.cpp
 * @brief Реализация словарного анализатора с загрузкой из hunspell
 */

#include "punto/dictionary.hpp"
#include "punto/hasher.hpp"
#include "punto/scancode_map.hpp"

#include <algorithm>
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

// Дополнительные английские словари (scowl, wamerican-huge)
constexpr const char *kEnDictPaths[] = {
    "/usr/share/hunspell/en_US.dic",
    "/usr/share/hunspell/en_GB.dic",
    "/usr/share/dict/american-english-huge",
    "/usr/share/dict/american-english-large",
    "/usr/share/dict/american-english",
    "/usr/share/dict/words",
};

// Дополнительные русские словари
constexpr const char *kRuDictPaths[] = {
    "/usr/share/hunspell/ru_RU.dic",
    "/usr/share/dict/russian",
};

// Встроенные IT-термины (не всегда есть в стандартных словарях)
// clang-format off
constexpr const char *kBuiltinITTerms[] = {
    // Containerization & Orchestration
    "docker", "dockerfile", "kubernetes", "kubectl", "helm", "podman",
    "containerd", "minikube", "kube", "pods", "deployments", "ingress",
    
    // Cloud & DevOps
    "nginx", "apache", "redis", "memcached", "elasticsearch", "kibana",
    "grafana", "prometheus", "terraform", "ansible", "jenkins", "gitlab",
    "github", "bitbucket", "circleci", "travis", "argocd", "fluxcd",
    
    // Programming Languages & Runtimes
    "python", "nodejs", "java", "golang", "rust", "typescript", "javascript",
    "kotlin", "scala", "ruby", "perl", "php", "swift", "cpp", "csharp",
    
    // Databases
    "postgres", "postgresql", "mysql", "mariadb", "mongodb", "cassandra",
    "sqlite", "dynamodb", "firestore", "cockroachdb", "tidb", "clickhouse",
    
    // Web & Frameworks
    "react", "angular", "vue", "svelte", "nextjs", "nuxt", "nestjs",
    "django", "flask", "fastapi", "express", "springboot", "laravel",
    "webpack", "vite", "rollup", "esbuild", "parcel", "tailwind",
    
    // Infrastructure & Cloud
    "aws", "gcp", "azure", "digitalocean", "linode", "vultr", "heroku",
    "netlify", "vercel", "cloudflare", "nginx", "haproxy", "traefik",
    
    // Version Control & Collaboration
    "git", "gitflow", "github", "gitlab", "bitbucket", "jira", "confluence",
    "slack", "discord", "zoom", "teams", "notion", "linear",
    
    // Monitoring & Logging
    "datadog", "newrelic", "splunk", "logstash", "fluentd", "jaeger",
    "zipkin", "opentelemetry", "pagerduty", "opsgenie",
    
    // Security
    "oauth", "jwt", "saml", "keycloak", "vault", "hashicorp",
    "ssl", "tls", "vpn", "wireguard", "ipsec",
    
    // Common IT Terms
    "api", "rest", "graphql", "grpc", "websocket", "http", "https",
    "json", "yaml", "xml", "csv", "protobuf", "avro",
    "localhost", "backend", "frontend", "fullstack", "devops", "sre",
    "microservices", "monolith", "serverless", "faas", "paas", "iaas",
    "cicd", "pipeline", "workflow", "cron", "daemon", "systemd",
    "sudo", "chmod", "chown", "grep", "awk", "sed", "curl", "wget",
    "bash", "zsh", "fish", "vim", "neovim", "emacs", "vscode",
    "linux", "ubuntu", "debian", "centos", "fedora", "alpine",
    "macos", "windows", "wsl", "homebrew", "apt", "yum", "dnf",
};
// clang-format on

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

/// Таблица обратной конвертации: ASCII QWERTY -> UTF-8 кириллица
struct QwertyToCyr {
  char qwerty;
  const char *utf8;
};

constexpr QwertyToCyr kQwertyMap[] = {
    {'f', "а"}, {',', "б"}, {'d', "в"}, {'u', "г"}, {'l', "д"},
    {'t', "е"}, {';', "ж"}, {'p', "з"}, {'b', "и"}, {'q', "й"},
    {'r', "к"}, {'k', "л"}, {'v', "м"}, {'y', "н"}, {'j', "о"},
    {'g', "п"}, {'h', "р"}, {'c', "с"}, {'n', "т"}, {'e', "у"},
    {'a', "ф"}, {'[', "х"}, {'w', "ц"}, {'x', "ч"}, {'i', "ш"},
    {'o', "щ"}, {']', "ъ"}, {'s', "ы"}, {'m', "ь"}, {'\'', "э"},
    {'.', "ю"}, {'z', "я"}, {'`', "ё"},
};
// clang-format on

// Пути к hunspell словарям (с .aff файлами)
constexpr const char *kEnAffPath = "/usr/share/hunspell/en_US.aff";
constexpr const char *kEnDicPathHunspell = "/usr/share/hunspell/en_US.dic";
constexpr const char *kRuAffPath = "/usr/share/hunspell/ru_RU.aff";
constexpr const char *kRuDicPathHunspell = "/usr/share/hunspell/ru_RU.dic";

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

bool Dictionary::hash_exists(
    std::uint64_t hash, const std::vector<std::uint64_t> &hashes) noexcept {
  // Бинарный поиск в отсортированном векторе
  // O(log N) с отличной cache locality
  return std::binary_search(hashes.begin(), hashes.end(), hash);
}

std::size_t Dictionary::load_en_dictionary(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    // Не логируем ошибку — словарь может быть опциональным
    return 0;
  }

  std::string line;
  std::size_t count = 0;

  // Резервируем память (wamerican-huge содержит ~300k слов)
  if (en_hashes_.capacity() < 350000) {
    en_hashes_.reserve(350000);
  }

  // Определяем формат файла: hunspell (.dic) или plain text
  bool is_hunspell = (path.find(".dic") != std::string::npos);

  // Первая строка hunspell — количество слов, пропускаем
  if (is_hunspell) {
    std::getline(file, line);
  }

  while (std::getline(file, line)) {
    std::string word = is_hunspell ? extract_word(line) : line;

    // Убираем пробелы в начале/конце
    while (!word.empty() &&
           (word.back() == '\r' || word.back() == '\n' || word.back() == ' ')) {
      word.pop_back();
    }
    while (!word.empty() && (word.front() == ' ')) {
      word.erase(0, 1);
    }

    // Фильтруем по длине и содержимому
    if (word.size() >= kDictMinWordLen && word.size() <= kDictMaxWordLen &&
        is_ascii_alpha_only(word)) {
      std::string lower = to_lowercase_ascii(word);

      // Вычисляем хеш и добавляем в структуры
      std::uint64_t hash = Hasher::hash_string(lower);
      en_hashes_.push_back(hash);
      en_bloom_.add(lower);

      ++count;
    }
  }

  return count;
}

std::size_t Dictionary::load_ru_dictionary(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    // Не логируем ошибку — словарь может быть опциональным
    return 0;
  }

  std::string line;
  std::size_t count = 0;

  // Резервируем память (типичный RU словарь ~150k слов)
  if (ru_hashes_.capacity() < 200000) {
    ru_hashes_.reserve(200000);
  }

  // Определяем формат файла: hunspell (.dic) или plain text
  bool is_hunspell = (path.find(".dic") != std::string::npos);

  // Первая строка hunspell — количество слов, пропускаем
  if (is_hunspell) {
    std::getline(file, line);
  }

  while (std::getline(file, line)) {
    std::string word = is_hunspell ? extract_word(line) : line;

    // Убираем пробелы в начале/конце
    while (!word.empty() &&
           (word.back() == '\r' || word.back() == '\n' || word.back() == ' ')) {
      word.pop_back();
    }
    while (!word.empty() && (word.front() == ' ')) {
      word.erase(0, 1);
    }

    // Фильтруем по длине (в символах UTF-8 это примерно word.size()/2)
    if (word.size() >= kDictMinWordLen * 2 &&
        word.size() <= kDictMaxWordLen * 2) {
      std::string qwerty = cyrillic_to_qwerty(word);

      // Проверяем, что конвертация прошла успешно
      if (qwerty.size() >= kDictMinWordLen &&
          qwerty.size() <= kDictMaxWordLen) {
        // Вычисляем хеш и добавляем в структуры
        std::uint64_t hash = Hasher::hash_string(qwerty);
        ru_hashes_.push_back(hash);
        ru_bloom_.add(qwerty);

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

  std::size_t en_count = 0;
  std::size_t ru_count = 0;

#ifdef HAVE_HUNSPELL
  // Инициализируем Hunspell для проверки словоформ (падежи, склонения, времена)
  std::ifstream test_en(kEnAffPath);
  std::ifstream test_ru(kRuAffPath);

  if (test_en.good()) {
    try {
      hunspell_en_ = std::make_unique<Hunspell>(kEnAffPath, kEnDicPathHunspell);
      std::cerr << "[punto] Hunspell EN loaded (with word forms support)\n";
    } catch (...) {
      std::cerr << "[punto] Hunspell EN init failed\n";
    }
  }

  if (test_ru.good()) {
    try {
      hunspell_ru_ = std::make_unique<Hunspell>(kRuAffPath, kRuDicPathHunspell);
      std::cerr << "[punto] Hunspell RU loaded (with word forms support)\n";
    } catch (...) {
      std::cerr << "[punto] Hunspell RU init failed\n";
    }
  }

  hunspell_available_ = (hunspell_en_ != nullptr || hunspell_ru_ != nullptr);
  if (hunspell_available_) {
    std::cerr << "[punto] Hunspell enabled: full word forms support (cases, "
                 "declensions, tenses)\n";
  }
#endif

  // Загружаем все доступные английские словари (для hash-based fallback)
  for (const char *path : kEnDictPaths) {
    std::size_t loaded = load_en_dictionary(path);
    if (loaded > 0) {
      std::cerr << "[punto] Loaded EN dict: " << path << " (+" << loaded
                << " words)\n";
      en_count += loaded;
    }
  }

  // Загружаем все доступные русские словари (для hash-based fallback)
  for (const char *path : kRuDictPaths) {
    std::size_t loaded = load_ru_dictionary(path);
    if (loaded > 0) {
      std::cerr << "[punto] Loaded RU dict: " << path << " (+" << loaded
                << " words)\n";
      ru_count += loaded;
    }
  }

  // Загружаем встроенные IT-термины (docker, kubernetes, etc)
  std::size_t it_count = 0;
  for (const char *term : kBuiltinITTerms) {
    std::string word_str(term);
    if (word_str.size() >= kDictMinWordLen &&
        word_str.size() <= kDictMaxWordLen) {
      std::string lower = to_lowercase_ascii(word_str);
      std::uint64_t hash = Hasher::hash_string(lower);
      en_hashes_.push_back(hash);
      en_bloom_.add(lower);
      ++it_count;
    }
  }
  if (it_count > 0) {
    std::cerr << "[punto] Loaded built-in IT terms: +" << it_count
              << " words\n";
    en_count += it_count;
  }

  // Сортируем и удаляем дубликаты после загрузки всех словарей
  finalize_hashes();

  initialized_ = (en_count > 0 || ru_count > 0 || hunspell_available_);

  std::cerr << "[punto] Dictionary: EN=" << en_hashes_.size()
            << " RU=" << ru_hashes_.size() << " unique words (hash-based)\n";
  std::cerr << "[punto] Bloom fill: EN="
            << static_cast<int>(en_bloom_.fill_ratio() * 100)
            << "% RU=" << static_cast<int>(ru_bloom_.fill_ratio() * 100)
            << "%\n";
  std::cerr << "[punto] Hash memory: EN=" << (en_hashes_.size() * 8 / 1024)
            << "KB RU=" << (ru_hashes_.size() * 8 / 1024) << "KB\n";

  return initialized_;
}

void Dictionary::finalize_hashes() {
  // Сортируем хеши для бинарного поиска
  std::sort(en_hashes_.begin(), en_hashes_.end());
  std::sort(ru_hashes_.begin(), ru_hashes_.end());

  // Удаляем дубликаты
  en_hashes_.erase(std::unique(en_hashes_.begin(), en_hashes_.end()),
                   en_hashes_.end());
  ru_hashes_.erase(std::unique(ru_hashes_.begin(), ru_hashes_.end()),
                   ru_hashes_.end());

  // Освобождаем лишнюю память
  en_hashes_.shrink_to_fit();
  ru_hashes_.shrink_to_fit();
}

DictResult Dictionary::lookup(std::span<const KeyEntry> entries) const {
  if (!initialized_ || entries.empty()) {
    return DictResult::Unknown;
  }

  // Конвертируем скан-коды в ASCII строку
  std::string ascii_word;
  ascii_word.reserve(entries.size());
  for (const auto &entry : entries) {
    if (entry.code < kScancodeToChar.size()) {
      char c = kScancodeToChar[entry.code];
      if (c != '\0') {
        // Приводим к нижнему регистру
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c + 32);
        }
        ascii_word += c;
      }
    }
  }

  if (ascii_word.empty() || ascii_word.size() < 2) {
    return DictResult::Unknown;
  }

  bool in_en = false;
  bool in_ru = false;

#ifdef HAVE_HUNSPELL
  // Приоритет 1: Hunspell с полной поддержкой словоформ
  if (hunspell_available_) {
    // Проверка 1: это английское слово? (ascii_word как есть)
    in_en = check_hunspell(ascii_word, true);

    // Проверка 2: это русское слово? (конвертируем QWERTY -> кириллица)
    std::string cyrillic_word = qwerty_to_cyrillic(ascii_word);
    if (!cyrillic_word.empty()) {
      in_ru = check_hunspell(cyrillic_word, false);
    }

    // Если hunspell дал результат — возвращаем
    if (in_en || in_ru) {
      if (in_en && in_ru) {
        return DictResult::Both;
      }
      if (in_en) {
        return DictResult::English;
      }
      if (in_ru) {
        return DictResult::Russian;
      }
    }
  }
#endif

  // Приоритет 2: Hash-based проверка (fallback)
  std::uint64_t h1, h2;
  Hasher::hash_entries_double(entries, h1, h2);

  if (h1 == 0) {
    return DictResult::Unknown;
  }

  // Level 0: Bloom Filter — быстрое отсечение
  bool maybe_en = en_bloom_.maybe_contains_hashes(h1, h2);
  bool maybe_ru = ru_bloom_.maybe_contains_hashes(h1, h2);

  // Если оба фильтра сказали "точно нет" — выходим немедленно
  if (!maybe_en && !maybe_ru) {
    return DictResult::Unknown;
  }

  // Level 1-2: Точная проверка в sorted vector (бинарный поиск)
  in_en = maybe_en && hash_exists(h1, en_hashes_);
  in_ru = maybe_ru && hash_exists(h1, ru_hashes_);

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

std::string Dictionary::qwerty_to_cyrillic(const std::string &qwerty) {
  std::string result;
  result.reserve(qwerty.size() * 2); // UTF-8 кириллица = 2 байта

  for (char c : qwerty) {
    // Приводим к нижнему регистру
    char lower = c;
    if (lower >= 'A' && lower <= 'Z') {
      lower = static_cast<char>(lower + 32);
    }

    bool found = false;
    for (const auto &entry : kQwertyMap) {
      if (entry.qwerty == lower) {
        result += entry.utf8;
        found = true;
        break;
      }
    }

    if (!found) {
      // Неизвестный символ — не конвертируем
      return "";
    }
  }

  return result;
}

bool Dictionary::check_hunspell(const std::string &word,
                                bool is_english) const {
#ifdef HAVE_HUNSPELL
  if (is_english && hunspell_en_) {
    return hunspell_en_->spell(word) != 0;
  }
  if (!is_english && hunspell_ru_) {
    return hunspell_ru_->spell(word) != 0;
  }
#else
  (void)word;
  (void)is_english;
#endif
  return false;
}

std::vector<std::string>
Dictionary::suggest(const std::string &word, bool is_english,
                    std::size_t max_suggestions) const {

  std::vector<std::string> result;

#ifdef HAVE_HUNSPELL
  Hunspell *hs = nullptr;
  if (is_english && hunspell_en_) {
    hs = hunspell_en_.get();
  } else if (!is_english && hunspell_ru_) {
    hs = hunspell_ru_.get();
  }

  if (hs == nullptr) {
    return result;
  }

  // Hunspell::suggest возвращает список предложений
  std::vector<std::string> suggestions = hs->suggest(word);

  // Ограничиваем количество результатов
  std::size_t count = std::min(suggestions.size(), max_suggestions);
  result.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    result.push_back(std::move(suggestions[i]));
  }
#else
  (void)word;
  (void)is_english;
  (void)max_suggestions;
#endif

  return result;
}

} // namespace punto
