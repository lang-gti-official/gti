#include "gti/filesystem.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {
namespace fs = std::filesystem;

uint32_t error(const std::error_code &code) {
  if (!code)
    return GTI_FS_OK_V1;
  if (code == std::errc::no_such_file_or_directory)
    return GTI_FS_NOT_FOUND_V1;
  if (code == std::errc::permission_denied ||
      code == std::errc::operation_not_permitted)
    return GTI_FS_PERMISSION_DENIED_V1;
  if (code == std::errc::file_exists)
    return GTI_FS_ALREADY_EXISTS_V1;
  if (code == std::errc::not_a_directory)
    return GTI_FS_NOT_DIRECTORY_V1;
  if (code == std::errc::is_a_directory)
    return GTI_FS_IS_DIRECTORY_V1;
  if (code == std::errc::directory_not_empty)
    return GTI_FS_DIRECTORY_NOT_EMPTY_V1;
  if (code == std::errc::too_many_symbolic_link_levels)
    return GTI_FS_TOO_MANY_SYMLINKS_V1;
  if (code == std::errc::no_space_on_device)
    return GTI_FS_NO_SPACE_V1;
  if (code == std::errc::read_only_file_system)
    return GTI_FS_READ_ONLY_V1;
  if (code == std::errc::cross_device_link)
    return GTI_FS_CROSS_DEVICE_V1;
  if (code == std::errc::filename_too_long)
    return GTI_FS_NAME_TOO_LONG_V1;
  if (code == std::errc::not_enough_memory)
    return GTI_FS_OUT_OF_MEMORY_V1;
  if (code == std::errc::operation_not_supported ||
      code == std::errc::function_not_supported)
    return GTI_FS_UNSUPPORTED_V1;
  if (code == std::errc::invalid_argument ||
      code == std::errc::illegal_byte_sequence)
    return GTI_FS_INVALID_ARGUMENT_V1;
  return GTI_FS_IO_ERROR_V1;
}

template <typename Operation> uint32_t contain(Operation operation) noexcept {
  try {
    return operation();
  } catch (const std::bad_alloc &) {
    return GTI_FS_OUT_OF_MEMORY_V1;
  } catch (const fs::filesystem_error &failure) {
    return error(failure.code());
  } catch (const std::system_error &failure) {
    return error(failure.code());
  } catch (...) {
    return GTI_FS_IO_ERROR_V1;
  }
}

bool decode(gti_c_string_view input, fs::path &output) {
  if (input.data == nullptr || input.length == 0 || input.length > SIZE_MAX ||
      std::memchr(input.data, '\0', static_cast<size_t>(input.length)) !=
          nullptr)
    return false;
#if defined(_WIN32)
  const auto *begin = reinterpret_cast<const char8_t *>(input.data);
  output = fs::path(std::u8string(begin, begin + input.length));
#else
  output = fs::path(std::string(input.data, static_cast<size_t>(input.length)));
#endif
  return true;
}

std::string encode(const fs::path &path) {
#if defined(_WIN32)
  auto value = path.u8string();
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
#else
  return path.native();
#endif
}

struct Result {
  std::string text;
  fs::directory_iterator iterator;
  bool directory = false;
  bool started = false;
};
Result *result(int64_t handle) {
  return reinterpret_cast<Result *>(static_cast<intptr_t>(handle));
}
int64_t handle(Result *value) {
  return static_cast<int64_t>(reinterpret_cast<intptr_t>(value));
}
} // namespace

extern "C" uint32_t gti_rt_fs_status_v1(gti_c_string_view input,
                                        uint32_t follow, uint32_t *type) {
  return contain([&]() -> uint32_t {
    fs::path path;
    if (!type || follow > 1 || !decode(input, path))
      return GTI_FS_INVALID_ARGUMENT_V1;
    std::error_code code;
    const auto status =
        follow ? fs::status(path, code) : fs::symlink_status(path, code);
    // An absent path is an observed state, including a dangling followed link.
    if (code && code != std::errc::no_such_file_or_directory)
      return error(code);
    uint32_t value = 8;
    switch (status.type()) {
    case fs::file_type::not_found:
      value = 0;
      break;
    case fs::file_type::regular:
      value = 1;
      break;
    case fs::file_type::directory:
      value = 2;
      break;
    case fs::file_type::symlink:
      value = 3;
      break;
    case fs::file_type::block:
      value = 4;
      break;
    case fs::file_type::character:
      value = 5;
      break;
    case fs::file_type::fifo:
      value = 6;
      break;
    case fs::file_type::socket:
      value = 7;
      break;
    default:
      break;
    }
    *type = value;
    return GTI_FS_OK_V1;
  });
}

extern "C" uint32_t gti_rt_fs_measure_v1(gti_c_string_view input,
                                         uint32_t operation, uint64_t *value) {
  return contain([&]() -> uint32_t {
    fs::path path;
    if (!value || operation > 1 || !decode(input, path))
      return GTI_FS_INVALID_ARGUMENT_V1;
    std::error_code code;
    const uint64_t measured =
        operation == 0 ? fs::file_size(path, code) : fs::is_empty(path, code);
    if (code)
      return error(code);
    *value = measured;
    return GTI_FS_OK_V1;
  });
}

extern "C" uint32_t gti_rt_fs_equivalent_v1(gti_c_string_view left,
                                            gti_c_string_view right,
                                            uint32_t *value) {
  return contain([&]() -> uint32_t {
    fs::path a, b;
    if (!value || !decode(left, a) || !decode(right, b))
      return GTI_FS_INVALID_ARGUMENT_V1;
    std::error_code code;
    const bool same = fs::equivalent(a, b, code);
    if (code)
      return error(code);
    *value = same ? 1 : 0;
    return GTI_FS_OK_V1;
  });
}

extern "C" uint32_t gti_rt_fs_mutate_v1(uint32_t operation,
                                        gti_c_string_view first,
                                        gti_c_string_view second,
                                        uint32_t option, uint64_t *value) {
  return contain([&]() -> uint32_t {
    fs::path a, b;
    const bool two = (operation >= 4 && operation <= 7) || operation == 9;
    if (!value || operation > 9 ||
        (operation == 5 ? option > 2 : option != 0) || !decode(first, a) ||
        (two && !decode(second, b)))
      return GTI_FS_INVALID_ARGUMENT_V1;
    std::error_code code;
    uint64_t changed = 1;
    switch (operation) {
    case 0:
      changed = fs::create_directory(a, code);
      break;
    case 1:
      changed = fs::create_directories(a, code);
      break;
    case 2:
      changed = fs::remove(a, code);
      break;
    case 3:
      changed = fs::remove_all(a, code);
      break;
    case 4:
      fs::rename(a, b, code);
      break;
    case 5: {
      const auto options = option == 1   ? fs::copy_options::skip_existing
                           : option == 2 ? fs::copy_options::overwrite_existing
                                         : fs::copy_options::none;
      changed = fs::copy_file(a, b, options, code);
      break;
    }
    case 6:
      fs::create_symlink(a, b, code);
      break;
    case 7:
      fs::create_directory_symlink(a, b, code);
      break;
    case 8:
      fs::current_path(a, code);
      break;
    case 9:
      fs::create_hard_link(a, b, code);
      break;
    }
    if (code)
      return error(code);
    *value = changed;
    return GTI_FS_OK_V1;
  });
}

extern "C" uint32_t gti_rt_fs_path_v1(uint32_t operation,
                                      gti_c_string_view input,
                                      int64_t *output) {
  return contain([&]() -> uint32_t {
    fs::path path, resolved;
    if (!output || operation > 5 ||
        (operation != 0 && operation != 4 && !decode(input, path)))
      return GTI_FS_INVALID_ARGUMENT_V1;
    std::error_code code;
    switch (operation) {
    case 0:
      resolved = fs::current_path(code);
      break;
    case 1:
      resolved = fs::absolute(path, code);
      break;
    case 2:
      resolved = fs::canonical(path, code);
      break;
    case 3:
      resolved = fs::weakly_canonical(path, code);
      break;
    case 4:
      resolved = fs::temp_directory_path(code);
      break;
    case 5:
      resolved = fs::read_symlink(path, code);
      break;
    }
    if (code)
      return error(code);
    auto owned = std::make_unique<Result>();
    owned->text = encode(resolved);
    *output = handle(owned.release());
    return GTI_FS_OK_V1;
  });
}

extern "C" uint32_t gti_rt_fs_directory_open_v1(gti_c_string_view input,
                                                int64_t *output) {
  return contain([&]() -> uint32_t {
    fs::path path;
    if (!output || !decode(input, path))
      return GTI_FS_INVALID_ARGUMENT_V1;
    auto owned = std::make_unique<Result>();
    std::error_code code;
    owned->iterator = fs::directory_iterator(path, code);
    if (code)
      return error(code);
    owned->directory = true;
    *output = handle(owned.release());
    return GTI_FS_OK_V1;
  });
}

extern "C" uint32_t gti_rt_fs_directory_next_v1(int64_t input,
                                                uint32_t *available) {
  return contain([&]() -> uint32_t {
    auto *owned = result(input);
    if (!owned || !owned->directory || !available)
      return GTI_FS_INVALID_ARGUMENT_V1;
    owned->text.clear();
    if (owned->started && owned->iterator != fs::directory_iterator()) {
      std::error_code code;
      owned->iterator.increment(code);
      if (code) {
        owned->iterator = fs::directory_iterator();
        return error(code);
      }
    }
    owned->started = true;
    if (owned->iterator == fs::directory_iterator()) {
      *available = 0;
      return GTI_FS_OK_V1;
    }
    owned->text = encode(owned->iterator->path());
    *available = 1;
    return GTI_FS_OK_V1;
  });
}

extern "C" uint64_t gti_rt_fs_result_size_v1(int64_t input) {
  const auto *owned = result(input);
  return owned ? owned->text.size() : 0;
}
extern "C" uint64_t gti_rt_fs_result_read_v1(int64_t input, uint64_t offset,
                                             uint8_t *output,
                                             uint64_t capacity) {
  const auto *owned = result(input);
  if (!owned || !output || offset > owned->text.size())
    return 0;
  const auto count = std::min<uint64_t>(capacity, owned->text.size() - offset);
  if (count)
    std::memcpy(output, owned->text.data() + offset,
                static_cast<size_t>(count));
  return count;
}
extern "C" void gti_rt_fs_result_destroy_v1(int64_t input) {
  delete result(input);
}
