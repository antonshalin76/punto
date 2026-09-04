#include "punto/dictionary.hpp"

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TempDirectory {
public:
  TempDirectory() {
    std::string pattern = "/tmp/punto-dictionary-contract-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char *created = ::mkdtemp(writable.data());
    if (created == nullptr) {
      throw std::runtime_error{"mkdtemp failed"};
    }
    path_ = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class TestRunner {
public:
  void expect(bool condition, const std::string &message) {
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL: " << message << '\n';
    }
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

private:
  int failures_ = 0;
};

void write_file(const std::filesystem::path &path,
                const std::string &contents) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

punto::DictionaryLoadResult load_english(
    const std::filesystem::path &path,
    punto::DictionaryLoadLimits limits = punto::DictionaryLoadLimits{}) {
  punto::DictionaryLoadSpec spec;
  spec.english_paths = {path};
  spec.limits = limits;
  punto::Dictionary dictionary;
  return dictionary.initialize_bounded(spec);
}

void test_valid_and_declared_count(TestRunner &runner,
                                   const TempDirectory &directory) {
  const auto valid = directory.path() / "valid.dic";
  const auto valid_ru = directory.path() / "valid-ru.dic";
  write_file(valid, "2\nhello\nworld\n");
  write_file(valid_ru, "1\nпривет\n");

  punto::DictionaryLoadSpec spec;
  spec.english_paths = {valid};
  spec.russian_paths = {valid_ru};
  punto::Dictionary dictionary;
  runner.expect(dictionary.initialize_bounded(spec) ==
                    punto::DictionaryLoadResult::Ok,
                "bounded valid dictionary loads");
  runner.expect(dictionary.is_ready(), "valid dictionary becomes ready");
  runner.expect(dictionary.en_size() >= 2,
                "valid dictionary publishes its entries");
  runner.expect(dictionary.ru_size() >= 1,
                "valid dictionary publishes Russian entries");

  punto::DictionaryLoadSpec english_only;
  english_only.english_paths = {valid};
  punto::Dictionary english_only_dictionary;
  runner.expect(english_only_dictionary.initialize_bounded(english_only) ==
                    punto::DictionaryLoadResult::NoUsableSource,
                "English-only source fails closed for bidirectional analysis");

  punto::DictionaryLoadSpec russian_only;
  russian_only.russian_paths = {valid_ru};
  punto::Dictionary russian_only_dictionary;
  runner.expect(russian_only_dictionary.initialize_bounded(russian_only) ==
                    punto::DictionaryLoadResult::NoUsableSource,
                "Russian-only source fails closed for bidirectional analysis");

  const auto wrong_alphabet_ru = directory.path() / "wrong-ru.txt";
  write_file(wrong_alphabet_ru, "hello\nworld\n");
  punto::DictionaryLoadSpec wrong_alphabet_ru_spec;
  wrong_alphabet_ru_spec.english_paths = {valid};
  wrong_alphabet_ru_spec.russian_paths = {wrong_alphabet_ru};
  punto::Dictionary wrong_alphabet_ru_dictionary;
  runner.expect(
      wrong_alphabet_ru_dictionary.initialize_bounded(wrong_alphabet_ru_spec) ==
          punto::DictionaryLoadResult::NoUsableSource,
      "ASCII-only Russian fallback does not satisfy Russian readiness");

  const auto wrong_alphabet_ru_dic = directory.path() / "wrong-ru.dic";
  write_file(wrong_alphabet_ru_dic, "1\nhello\n");
  wrong_alphabet_ru_spec.russian_paths = {wrong_alphabet_ru_dic};
  punto::Dictionary wrong_alphabet_ru_dic_dictionary;
  runner.expect(wrong_alphabet_ru_dic_dictionary.initialize_bounded(
                    wrong_alphabet_ru_spec) ==
                    punto::DictionaryLoadResult::NoUsableSource,
                "ASCII-only Russian .dic does not satisfy Russian readiness");

  const auto empty_affix = directory.path() / "empty.aff";
  const auto empty_hunspell = directory.path() / "empty.dic";
  write_file(empty_affix, "SET UTF-8\n");
  write_file(empty_hunspell, "0\n");
  punto::DictionaryLoadSpec empty_hunspell_spec;
  empty_hunspell_spec.english_affix = empty_affix;
  empty_hunspell_spec.english_hunspell_dictionary = empty_hunspell;
  empty_hunspell_spec.russian_paths = {valid_ru};
  punto::Dictionary empty_hunspell_dictionary;
  runner.expect(
      empty_hunspell_dictionary.initialize_bounded(empty_hunspell_spec) ==
          punto::DictionaryLoadResult::NoUsableSource,
      "zero-entry Hunspell pair does not satisfy per-language readiness");

  const auto blank_hunspell = directory.path() / "blank.dic";
  write_file(blank_hunspell, "1\n\n");
  punto::DictionaryLoadSpec blank_hunspell_spec;
  blank_hunspell_spec.english_affix = empty_affix;
  blank_hunspell_spec.english_hunspell_dictionary = blank_hunspell;
  blank_hunspell_spec.russian_paths = {valid_ru};
  punto::Dictionary blank_hunspell_dictionary;
  runner.expect(
      blank_hunspell_dictionary.initialize_bounded(blank_hunspell_spec) ==
          punto::DictionaryLoadResult::NoUsableSource,
      "blank Hunspell entry does not satisfy English readiness");

  const auto invalid_hunspell = directory.path() / "invalid-entry.dic";
  write_file(invalid_hunspell, "1\n1234\n");
  punto::DictionaryLoadSpec invalid_hunspell_spec;
  invalid_hunspell_spec.english_affix = empty_affix;
  invalid_hunspell_spec.english_hunspell_dictionary = invalid_hunspell;
  invalid_hunspell_spec.russian_paths = {valid_ru};
  punto::Dictionary invalid_hunspell_dictionary;
  runner.expect(
      invalid_hunspell_dictionary.initialize_bounded(invalid_hunspell_spec) ==
          punto::DictionaryLoadResult::NoUsableSource,
      "non-word Hunspell entry does not satisfy English readiness");

  const auto wrong_language_hunspell = directory.path() / "wrong-language.dic";
  write_file(wrong_language_hunspell, "1\nhello\n");
  punto::DictionaryLoadSpec wrong_language_hunspell_spec;
  wrong_language_hunspell_spec.russian_affix = empty_affix;
  wrong_language_hunspell_spec.russian_hunspell_dictionary =
      wrong_language_hunspell;
  wrong_language_hunspell_spec.english_paths = {valid};
  punto::Dictionary wrong_language_hunspell_dictionary;
  runner.expect(
      wrong_language_hunspell_dictionary.initialize_bounded(
          wrong_language_hunspell_spec) ==
          punto::DictionaryLoadResult::NoUsableSource,
      "wrong-alphabet Hunspell entry does not satisfy Russian readiness");

  const auto mismatch = directory.path() / "mismatch.dic";
  write_file(mismatch, "2\nhello\n");
  runner.expect(load_english(mismatch) ==
                    punto::DictionaryLoadResult::Malformed,
                "declared count must match bounded observed entries");
}

void test_malformed_and_oversize(TestRunner &runner,
                                 const TempDirectory &directory) {
  const auto malformed = directory.path() / "malformed.dic";
  write_file(malformed, "not-a-count\nhello\n");
  runner.expect(load_english(malformed) ==
                    punto::DictionaryLoadResult::Malformed,
                "malformed Hunspell header is deterministic");

  const auto huge_header = directory.path() / "huge-header.dic";
  write_file(huge_header, "2000001\nhello\n");
  runner.expect(load_english(huge_header) ==
                    punto::DictionaryLoadResult::Oversize,
                "declared Hunspell count is capped before construction");

  const auto large_file = directory.path() / "large.dic";
  write_file(large_file, "2\nhello\nworld\n");
  punto::DictionaryLoadLimits file_limits;
  file_limits.max_file_bytes = 8;
  runner.expect(load_english(large_file, file_limits) ==
                    punto::DictionaryLoadResult::Oversize,
                "individual dictionary byte cap is enforced");

  punto::DictionaryLoadLimits line_limits;
  line_limits.max_line_bytes = 4;
  runner.expect(load_english(large_file, line_limits) ==
                    punto::DictionaryLoadResult::Oversize,
                "dictionary line cap is enforced");

  const auto first = directory.path() / "first.txt";
  const auto second = directory.path() / "second.txt";
  write_file(first, "hello\n");
  write_file(second, "world\n");
  punto::DictionaryLoadSpec aggregate_spec;
  aggregate_spec.english_paths = {first, second};
  aggregate_spec.limits.max_aggregate_bytes = 10;
  punto::Dictionary aggregate_dictionary;
  runner.expect(aggregate_dictionary.initialize_bounded(aggregate_spec) ==
                    punto::DictionaryLoadResult::Oversize,
                "aggregate dictionary byte cap spans all sources");
}

void test_special_files_never_block(TestRunner &runner,
                                    const TempDirectory &directory) {
  const auto fifo = directory.path() / "blocked.fifo";
  if (::mkfifo(fifo.c_str(), 0600) != 0) {
    throw std::runtime_error{"mkfifo failed"};
  }
  const auto started = std::chrono::steady_clock::now();
  runner.expect(load_english(fifo) == punto::DictionaryLoadResult::Malformed,
                "FIFO dictionary is rejected as malformed");
  runner.expect(std::chrono::steady_clock::now() - started <
                    std::chrono::milliseconds{200},
                "FIFO dictionary rejection is nonblocking");

  const auto target = directory.path() / "target.txt";
  const auto symlink = directory.path() / "dictionary.txt";
  write_file(target, "hello\n");
  std::filesystem::create_symlink(target, symlink);
  runner.expect(load_english(symlink) ==
                    punto::DictionaryLoadResult::NoUsableSource,
                "symlink source is ignored without following it");
}

} // namespace

int main() {
  try {
    TestRunner runner;
    TempDirectory directory;
    test_valid_and_declared_count(runner, directory);
    test_malformed_and_oversize(runner, directory);
    test_special_files_never_block(runner, directory);
    if (runner.failures() != 0) {
      std::cerr << runner.failures()
                << " dictionary contract assertion(s) failed\n";
      return 1;
    }
    std::cout << "dictionary contract tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FATAL: " << error.what() << '\n';
    return 2;
  }
}
