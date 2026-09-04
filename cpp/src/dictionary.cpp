/**
 * @file dictionary.cpp
 * @brief Реализация словарного анализатора с загрузкой из hunspell
 */

#include "punto/dictionary.hpp"
#include "punto/hasher.hpp"
#include "punto/scancode_map.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>

namespace punto {

namespace {

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

struct BoundedReadResult {
  DictionaryLoadResult result = DictionaryLoadResult::Ok;
  bool present = false;
};

template <typename Consumer>
BoundedReadResult read_bounded_lines(const std::filesystem::path &path,
                                     const DictionaryLoadLimits &limits,
                                     std::uint64_t &aggregate_bytes,
                                     std::size_t &aggregate_lines,
                                     Consumer consumer) {
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) {
    return {(errno == ENOENT || errno == ELOOP) ? DictionaryLoadResult::Ok
                                                : DictionaryLoadResult::IoError,
            false};
  }

  struct CloseFd {
    int value;
    ~CloseFd() { ::close(value); }
  } close_fd{fd};

  struct stat metadata {};
  if (::fstat(fd, &metadata) != 0) {
    return {DictionaryLoadResult::IoError, true};
  }
  if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
    return {DictionaryLoadResult::Malformed, true};
  }
  const auto advertised_size = static_cast<std::uint64_t>(metadata.st_size);
  if (advertised_size > limits.max_file_bytes ||
      aggregate_bytes > limits.max_aggregate_bytes ||
      advertised_size > limits.max_aggregate_bytes - aggregate_bytes) {
    return {DictionaryLoadResult::Oversize, true};
  }
  aggregate_bytes += advertised_size;

  std::array<char, 8192> buffer{};
  std::string line;
  line.reserve(std::min<std::size_t>(limits.max_line_bytes, 256));
  std::uint64_t actual_size = 0;
  std::size_t line_number = 0;
  bool last_was_newline = true;

  const auto finish_line = [&]() -> DictionaryLoadResult {
    if (aggregate_lines >= limits.max_entries) {
      return DictionaryLoadResult::Oversize;
    }
    ++aggregate_lines;
    const DictionaryLoadResult result = consumer(line, line_number++);
    line.clear();
    return result;
  };

  while (true) {
    const ssize_t count = ::read(fd, buffer.data(), buffer.size());
    if (count == 0) {
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return {DictionaryLoadResult::IoError, true};
    }
    actual_size += static_cast<std::uint64_t>(count);
    if (actual_size > limits.max_file_bytes ||
        (actual_size > advertised_size &&
         actual_size - advertised_size >
             limits.max_aggregate_bytes - aggregate_bytes)) {
      return {DictionaryLoadResult::Oversize, true};
    }

    for (ssize_t i = 0; i < count; ++i) {
      const char ch = buffer[static_cast<std::size_t>(i)];
      if (ch == '\0') {
        return {DictionaryLoadResult::Malformed, true};
      }
      if (ch == '\n') {
        const DictionaryLoadResult result = finish_line();
        if (result != DictionaryLoadResult::Ok) {
          return {result, true};
        }
        last_was_newline = true;
        continue;
      }
      if (line.size() >= limits.max_line_bytes) {
        return {DictionaryLoadResult::Oversize, true};
      }
      line.push_back(ch);
      last_was_newline = false;
    }
  }

  if (actual_size > advertised_size) {
    aggregate_bytes += actual_size - advertised_size;
  }
  if (!last_was_newline) {
    const DictionaryLoadResult result = finish_line();
    if (result != DictionaryLoadResult::Ok) {
      return {result, true};
    }
  }
  return {DictionaryLoadResult::Ok, true};
}

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

bool is_cyrillic_alpha_only(std::string_view word) {
  std::size_t offset = 0;
  std::size_t letters = 0;
  while (offset < word.size()) {
    bool matched = false;
    for (const auto &entry : kCyrillicMap) {
      const std::string_view letter{entry.utf8};
      if (word.substr(offset).starts_with(letter)) {
        offset += letter.size();
        ++letters;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return letters >= kDictMinWordLen && letters <= kDictMaxWordLen;
}

} // namespace

DictionaryLoadSpec DictionaryLoadSpec::system_default() {
  DictionaryLoadSpec spec;
  for (const char *path : kEnDictPaths) {
    spec.english_paths.emplace_back(path);
  }
  for (const char *path : kRuDictPaths) {
    spec.russian_paths.emplace_back(path);
  }
  spec.english_affix = kEnAffPath;
  spec.english_hunspell_dictionary = kEnDicPathHunspell;
  spec.russian_affix = kRuAffPath;
  spec.russian_hunspell_dictionary = kRuDicPathHunspell;
  return spec;
}

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

DictionaryLoadResult
Dictionary::load_en_dictionary(const std::filesystem::path &path,
                               const DictionaryLoadLimits &limits,
                               LoadBudget &budget, std::size_t &loaded) {
  loaded = 0;
  const bool hunspell_format = path.extension() == ".dic";
  bool header_seen = !hunspell_format;
  std::optional<std::uint64_t> declared_count;
  std::uint64_t observed_count = 0;
  const BoundedReadResult read = read_bounded_lines(
      path, limits, budget.aggregate_bytes, budget.lines,
      [&](const std::string &line, std::size_t line_number) {
        if (hunspell_format && line_number == 0) {
          std::string_view header{line};
          while (!header.empty() &&
                 (header.back() == '\r' || header.back() == ' ')) {
            header.remove_suffix(1);
          }
          std::uint64_t parsed_count = 0;
          const auto parsed = std::from_chars(
              header.data(), header.data() + header.size(), parsed_count);
          if (header.empty() || parsed.ec != std::errc{} ||
              parsed.ptr != header.data() + header.size()) {
            return DictionaryLoadResult::Malformed;
          }
          if (parsed_count > limits.max_entries) {
            return DictionaryLoadResult::Oversize;
          }
          declared_count = parsed_count;
          header_seen = true;
          return DictionaryLoadResult::Ok;
        }

        ++observed_count;

        std::string word = hunspell_format ? extract_word(line) : line;
        while (!word.empty() && (word.back() == '\r' || word.back() == ' ')) {
          word.pop_back();
        }
        while (!word.empty() && word.front() == ' ') {
          word.erase(0, 1);
        }

        if (word.size() < kDictMinWordLen || word.size() > kDictMaxWordLen ||
            !is_ascii_alpha_only(word)) {
          return DictionaryLoadResult::Ok;
        }
        if (budget.entries >= limits.max_entries) {
          return DictionaryLoadResult::Oversize;
        }
        const std::string lower = to_lowercase_ascii(word);
        en_hashes_.push_back(Hasher::hash_string(lower));
        en_bloom_.add(lower);
        ++budget.entries;
        ++loaded;
        return DictionaryLoadResult::Ok;
      });

  if (read.result != DictionaryLoadResult::Ok) {
    return read.result;
  }
  if (read.present && !header_seen) {
    return DictionaryLoadResult::Malformed;
  }
  if (read.present && declared_count && *declared_count != observed_count) {
    return DictionaryLoadResult::Malformed;
  }
  return DictionaryLoadResult::Ok;
}

DictionaryLoadResult
Dictionary::load_ru_dictionary(const std::filesystem::path &path,
                               const DictionaryLoadLimits &limits,
                               LoadBudget &budget, std::size_t &loaded) {
  loaded = 0;
  const bool hunspell_format = path.extension() == ".dic";
  bool header_seen = !hunspell_format;
  std::optional<std::uint64_t> declared_count;
  std::uint64_t observed_count = 0;
  const BoundedReadResult read = read_bounded_lines(
      path, limits, budget.aggregate_bytes, budget.lines,
      [&](const std::string &line, std::size_t line_number) {
        if (hunspell_format && line_number == 0) {
          std::string_view header{line};
          while (!header.empty() &&
                 (header.back() == '\r' || header.back() == ' ')) {
            header.remove_suffix(1);
          }
          std::uint64_t parsed_count = 0;
          const auto parsed = std::from_chars(
              header.data(), header.data() + header.size(), parsed_count);
          if (header.empty() || parsed.ec != std::errc{} ||
              parsed.ptr != header.data() + header.size()) {
            return DictionaryLoadResult::Malformed;
          }
          if (parsed_count > limits.max_entries) {
            return DictionaryLoadResult::Oversize;
          }
          declared_count = parsed_count;
          header_seen = true;
          return DictionaryLoadResult::Ok;
        }

        ++observed_count;

        std::string word = hunspell_format ? extract_word(line) : line;
        while (!word.empty() && (word.back() == '\r' || word.back() == ' ')) {
          word.pop_back();
        }
        while (!word.empty() && word.front() == ' ') {
          word.erase(0, 1);
        }
        if (!is_cyrillic_alpha_only(word)) {
          return DictionaryLoadResult::Ok;
        }

        const std::string qwerty = cyrillic_to_qwerty(word);
        if (qwerty.size() < kDictMinWordLen ||
            qwerty.size() > kDictMaxWordLen) {
          return DictionaryLoadResult::Ok;
        }
        if (budget.entries >= limits.max_entries) {
          return DictionaryLoadResult::Oversize;
        }
        ru_hashes_.push_back(Hasher::hash_string(qwerty));
        ru_bloom_.add(qwerty);
        ++budget.entries;
        ++loaded;
        return DictionaryLoadResult::Ok;
      });

  if (read.result != DictionaryLoadResult::Ok) {
    return read.result;
  }
  if (read.present && !header_seen) {
    return DictionaryLoadResult::Malformed;
  }
  if (read.present && declared_count && *declared_count != observed_count) {
    return DictionaryLoadResult::Malformed;
  }
  return DictionaryLoadResult::Ok;
}

bool Dictionary::initialize() {
  return initialize_bounded(DictionaryLoadSpec::system_default()) ==
         DictionaryLoadResult::Ok;
}

DictionaryLoadResult
Dictionary::initialize_bounded(const DictionaryLoadSpec &spec) {
  if (initialized_) {
    return DictionaryLoadResult::Ok;
  }
  if (spec.limits.max_file_bytes == 0 || spec.limits.max_aggregate_bytes == 0 ||
      spec.limits.max_line_bytes == 0 || spec.limits.max_entries == 0) {
    return DictionaryLoadResult::Oversize;
  }

  en_bloom_.clear();
  ru_bloom_.clear();
  en_hashes_.clear();
  ru_hashes_.clear();
#ifdef HAVE_HUNSPELL
  hunspell_en_.reset();
  hunspell_ru_.reset();
#endif
  hunspell_available_ = false;

  LoadBudget budget;
  std::size_t fallback_en_count = 0;
  std::size_t fallback_ru_count = 0;

  const auto fail = [this](DictionaryLoadResult result) {
    en_bloom_.clear();
    ru_bloom_.clear();
    en_hashes_.clear();
    ru_hashes_.clear();
#ifdef HAVE_HUNSPELL
    hunspell_en_.reset();
    hunspell_ru_.reset();
#endif
    hunspell_available_ = false;
    initialized_ = false;
    return result;
  };

#ifdef HAVE_HUNSPELL
  const auto validate_hunspell_pair =
      [&](const std::optional<std::filesystem::path> &affix,
          const std::optional<std::filesystem::path> &dictionary, bool english,
          std::unique_ptr<Hunspell> &destination) {
        if (!affix || !dictionary) {
          return DictionaryLoadResult::Ok;
        }

        const BoundedReadResult affix_read = read_bounded_lines(
            *affix, spec.limits, budget.aggregate_bytes, budget.lines,
            [](const std::string &, std::size_t) {
              return DictionaryLoadResult::Ok;
            });
        if (affix_read.result != DictionaryLoadResult::Ok) {
          return affix_read.result;
        }

        bool header_seen = false;
        std::optional<std::uint64_t> declared_count;
        std::uint64_t observed_count = 0;
        std::uint64_t usable_count = 0;
        const BoundedReadResult dictionary_read = read_bounded_lines(
            *dictionary, spec.limits, budget.aggregate_bytes, budget.lines,
            [&](const std::string &line, std::size_t line_number) {
              if (line_number != 0) {
                ++observed_count;
                std::string word = extract_word(line);
                while (!word.empty() &&
                       (word.back() == '\r' || word.back() == ' ')) {
                  word.pop_back();
                }
                while (!word.empty() && word.front() == ' ') {
                  word.erase(0, 1);
                }
                const bool usable = english
                                        ? word.size() >= kDictMinWordLen &&
                                              word.size() <= kDictMaxWordLen &&
                                              is_ascii_alpha_only(word)
                                        : is_cyrillic_alpha_only(word);
                if (usable) {
                  ++usable_count;
                }
                return DictionaryLoadResult::Ok;
              }
              std::string_view header{line};
              while (!header.empty() &&
                     (header.back() == '\r' || header.back() == ' ')) {
                header.remove_suffix(1);
              }
              std::uint64_t parsed_count = 0;
              const auto parsed = std::from_chars(
                  header.data(), header.data() + header.size(), parsed_count);
              if (header.empty() || parsed.ec != std::errc{} ||
                  parsed.ptr != header.data() + header.size()) {
                return DictionaryLoadResult::Malformed;
              }
              if (parsed_count > spec.limits.max_entries) {
                return DictionaryLoadResult::Oversize;
              }
              declared_count = parsed_count;
              header_seen = true;
              return DictionaryLoadResult::Ok;
            });
        if (dictionary_read.result != DictionaryLoadResult::Ok) {
          return dictionary_read.result;
        }
        if (!affix_read.present || !dictionary_read.present) {
          return DictionaryLoadResult::Ok;
        }
        if (!header_seen) {
          return DictionaryLoadResult::Malformed;
        }
        if (!declared_count || *declared_count != observed_count) {
          return DictionaryLoadResult::Malformed;
        }
        if (*declared_count == 0 || usable_count == 0) {
          return DictionaryLoadResult::Ok;
        }

        try {
          destination =
              std::make_unique<Hunspell>(affix->c_str(), dictionary->c_str());
        } catch (...) {
          return DictionaryLoadResult::IoError;
        }
        return DictionaryLoadResult::Ok;
      };

  DictionaryLoadResult result = validate_hunspell_pair(
      spec.english_affix, spec.english_hunspell_dictionary, true, hunspell_en_);
  if (result != DictionaryLoadResult::Ok) {
    return fail(result);
  }
  result = validate_hunspell_pair(spec.russian_affix,
                                  spec.russian_hunspell_dictionary, false,
                                  hunspell_ru_);
  if (result != DictionaryLoadResult::Ok) {
    return fail(result);
  }
  hunspell_available_ = hunspell_en_ != nullptr || hunspell_ru_ != nullptr;
#endif

  for (const auto &path : spec.english_paths) {
    std::size_t loaded = 0;
    const DictionaryLoadResult result =
        load_en_dictionary(path, spec.limits, budget, loaded);
    if (result != DictionaryLoadResult::Ok) {
      return fail(result);
    }
    fallback_en_count += loaded;
  }
  for (const auto &path : spec.russian_paths) {
    std::size_t loaded = 0;
    const DictionaryLoadResult result =
        load_ru_dictionary(path, spec.limits, budget, loaded);
    if (result != DictionaryLoadResult::Ok) {
      return fail(result);
    }
    fallback_ru_count += loaded;
  }

  for (const char *term : kBuiltinITTerms) {
    const std::string word{term};
    if (word.size() < kDictMinWordLen || word.size() > kDictMaxWordLen) {
      continue;
    }
    if (budget.entries >= spec.limits.max_entries) {
      return fail(DictionaryLoadResult::Oversize);
    }
    const std::string lower = to_lowercase_ascii(word);
    en_hashes_.push_back(Hasher::hash_string(lower));
    en_bloom_.add(lower);
    ++budget.entries;
  }

  finalize_hashes();
  bool english_ready = fallback_en_count > 0;
  bool russian_ready = fallback_ru_count > 0;
#ifdef HAVE_HUNSPELL
  english_ready = english_ready || hunspell_en_ != nullptr;
  russian_ready = russian_ready || hunspell_ru_ != nullptr;
#endif
  initialized_ = english_ready && russian_ready;
  if (!initialized_) {
    return fail(DictionaryLoadResult::NoUsableSource);
  }

  std::cerr << "[punto] Dictionary: EN=" << en_hashes_.size()
            << " RU=" << ru_hashes_.size() << " unique words (hash-based)\n";
  return DictionaryLoadResult::Ok;
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
  std::uint64_t h1 = 0;
  std::uint64_t h2 = 0;
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
    std::lock_guard<std::mutex> lock(hunspell_mutex_);
    return hunspell_en_->spell(word) != 0;
  }
  if (!is_english && hunspell_ru_) {
    std::lock_guard<std::mutex> lock(hunspell_mutex_);
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
  std::vector<std::string> suggestions;
  {
    std::lock_guard<std::mutex> lock(hunspell_mutex_);
    suggestions = hs->suggest(word);
  }

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
