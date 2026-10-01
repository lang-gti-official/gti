#include "gti/standard_library.h"

#include "gti/executable_path.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

namespace lang {

StandardLibraryLayout standardLibraryLayout(std::filesystem::path configured) {
  if (configured.extension() == ".gti") {
    return {.root = configured.parent_path(), .prelude = std::move(configured)};
  }
  return {.root = configured, .prelude = configured / "prelude.gti"};
}

StandardLibraryLayout
discoverStandardLibrary(const char *driver,
                        const std::filesystem::path &buildRoot) {
  if (const char *configured = std::getenv("GTI_STDLIB_PATH");
      configured != nullptr && *configured != '\0') {
    return standardLibraryLayout(configured);
  }

  const std::filesystem::path executable = executablePath(driver);
  const std::filesystem::path installedRoot =
      executable.parent_path().parent_path() / "share/gti/stdlib";
  std::error_code error;
  if (std::filesystem::is_regular_file(installedRoot / "prelude.gti", error)) {
    return standardLibraryLayout(installedRoot);
  }
  return standardLibraryLayout(buildRoot.empty() ? installedRoot : buildRoot);
}

std::optional<std::string_view>
standardLibraryIncludeForType(std::string_view qualifiedName) {
  struct Entry {
    std::string_view qualifiedName;
    std::string_view include;
  };
  static constexpr std::array entries{
      Entry{"std::FILE", "std/cstdio"},
      Entry{"std::alignment", "std/memory"},
      Entry{"std::alloc_errc", "std/memory"},
      Entry{"std::allocator", "std/memory"},
      Entry{"std::arena", "std/memory"},
      Entry{"std::arena_errc", "std/memory"},
      Entry{"std::arithmetic_errc", "std/numeric"},
      Entry{"std::array", "std/array"},
      Entry{"std::atomic", "std/atomic"},
      Entry{"std::atomic_compare_exchange_result", "std/atomic"},
      Entry{"std::bidirectional_iterator_tag", "std/iterator"},
      Entry{"std::chars_format", "std/charconv"},
      Entry{"std::chrono::clock_errc", "std/chrono"},
      Entry{"std::chrono::nanoseconds", "std/chrono"},
      Entry{"std::chrono::steady_clock", "std/chrono"},
      Entry{"std::chrono::steady_time_point", "std/chrono"},
      Entry{"std::chrono::system_clock", "std/chrono"},
      Entry{"std::chrono::system_time_point", "std/chrono"},
      Entry{"std::cnd_t", "std/cthread"},
      Entry{"std::contiguous_iterator_tag", "std/iterator"},
      Entry{"std::deque", "std/deque"},
      Entry{"std::equal_to", "std/functional"},
      Entry{"std::file_mode", "std/cstdio"},
      Entry{"std::filesystem::copy_options", "std/filesystem"},
      Entry{"std::filesystem::directory_iterator", "std/filesystem"},
      Entry{"std::filesystem::errc", "std/filesystem"},
      Entry{"std::filesystem::file_status", "std/filesystem"},
      Entry{"std::filesystem::file_type", "std/filesystem"},
      Entry{"std::filesystem::path", "std/filesystem"},
      Entry{"std::format_errc", "std/format"},
      Entry{"std::forward_iterator_tag", "std/iterator"},
      Entry{"std::forward_list", "std/forward_list"},
      Entry{"std::from_chars_result", "std/charconv"},
      Entry{"std::function", "std/functional"},
      Entry{"std::greater", "std/functional"},
      Entry{"std::hash", "std/functional"},
      Entry{"std::input_iterator_tag", "std/iterator"},
      Entry{"std::io_errc", "std/cstdio"},
      Entry{"std::less", "std/functional"},
      Entry{"std::list", "std/list"},
      Entry{"std::map", "std/map"},
      Entry{"std::memory_order_acq_rel", "std/atomic"},
      Entry{"std::memory_order_acquire", "std/atomic"},
      Entry{"std::memory_order_relaxed", "std/atomic"},
      Entry{"std::memory_order_release", "std/atomic"},
      Entry{"std::memory_order_seq_cst", "std/atomic"},
      Entry{"std::move_only_function", "std/functional"},
      Entry{"std::mtx_t", "std/cthread"},
      Entry{"std::mutex", "std/mutex"},
      Entry{"std::not_equal_to", "std/functional"},
      Entry{"std::once_flag", "std/cthread"},
      Entry{"std::once_flag_init", "std/cthread"},
      Entry{"std::once_initializer_t", "std/cthread"},
      Entry{"std::optional", "std/optional"},
      Entry{"std::output_iterator_tag", "std/iterator"},
      Entry{"std::pair", "std/utility"},
      Entry{"std::parse_errc", "std/charconv"},
      Entry{"std::pool", "std/memory"},
      Entry{"std::pool_errc", "std/memory"},
      Entry{"std::process::command", "std/process"},
      Entry{"std::process::errc", "std/process"},
      Entry{"std::process::exit_status", "std/process"},
      Entry{"std::queue", "std/queue"},
      Entry{"std::random_access_iterator_tag", "std/iterator"},
      Entry{"std::random_device", "std/random"},
      Entry{"std::random_engine", "std/random"},
      Entry{"std::seek_origin", "std/cstdio"},
      Entry{"std::set", "std/set"},
      Entry{"std::shared_ptr", "std/memory"},
      Entry{"std::simd", "std/simd"},
      Entry{"std::simd_mask", "std/simd"},
      Entry{"std::source_location", "std/source_location"},
      Entry{"std::span", "std/span"},
      Entry{"std::storage", "std/memory"},
      Entry{"std::string", "std/string"},
      Entry{"std::tcp::errc", "std/tcp"},
      Entry{"std::tcp::socket", "std/tcp"},
      Entry{"std::thread", "std/thread"},
      Entry{"std::thread_errc", "std/thread"},
      Entry{"std::thread_result", "std/thread"},
      Entry{"std::thrd_start_t", "std/cthread"},
      Entry{"std::thrd_t", "std/cthread"},
      Entry{"std::timespec", "std/cthread"},
      Entry{"std::unique_ptr", "std/memory"},
      Entry{"std::unordered_map", "std/unordered_map"},
      Entry{"std::unordered_set", "std/unordered_set"},
      Entry{"std::vector", "std/vector"},
      Entry{"std::weak_ptr", "std/memory"},
  };
  const auto found =
      std::find_if(entries.begin(), entries.end(), [&](const Entry &entry) {
        return entry.qualifiedName == qualifiedName;
      });
  return found == entries.end()
             ? std::nullopt
             : std::optional<std::string_view>(found->include);
}

} // namespace lang
