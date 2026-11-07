#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace gti_test {

// Creation, rather than a clock or process identifier, establishes ownership.
// A collision must never let one test overwrite or remove another's fixtures.
class TemporaryDirectory final {
public:
  explicit TemporaryDirectory(std::string_view prefix = "gti-test") {
    const auto parent =
        std::filesystem::canonical(std::filesystem::temp_directory_path());
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt != 1024; ++attempt) {
      auto candidate =
          parent /
          (std::string(prefix) + '-' + std::to_string(stamp) + '-' +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        directory = std::move(candidate);
        return;
      }
      if (error && error != std::errc::file_exists)
        throw std::filesystem::filesystem_error("create private test directory",
                                                candidate, error);
    }
    throw std::filesystem::filesystem_error(
        "exhausted private test directory names", parent,
        std::make_error_code(std::errc::file_exists));
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }

  [[nodiscard]] const std::filesystem::path &root() const { return directory; }

private:
  inline static std::atomic<std::uint64_t> sequence{0};
  std::filesystem::path directory;
};

// Existing fixtures can retain their descriptive relative names beneath one
// private root for the lifetime of this test executable.
[[nodiscard]] inline const std::filesystem::path &temporaryDirectory() {
  static const TemporaryDirectory directory;
  return directory.root();
}

} // namespace gti_test
