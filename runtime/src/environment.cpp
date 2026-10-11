#include "gti/environment.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace {
struct Snapshot {
  uint64_t size;
};

static_assert(sizeof(intptr_t) <= sizeof(int64_t));

Snapshot *snapshot(int64_t handle) {
  return reinterpret_cast<Snapshot *>(static_cast<intptr_t>(handle));
}

char *bytes(Snapshot *value) { return reinterpret_cast<char *>(value + 1); }
} // namespace

extern "C" uint32_t gti_rt_environment_get_v1(gti_c_string_view name,
                                              int64_t *output) {
  if (output == nullptr)
    return GTI_ENVIRONMENT_INVALID_NAME_V1;
  *output = 0;
  if (name.data == nullptr || name.length == 0 || name.length > SIZE_MAX ||
      std::memchr(name.data, '\0', static_cast<size_t>(name.length)) ||
      std::memchr(name.data, '=', static_cast<size_t>(name.length)))
    return GTI_ENVIRONMENT_INVALID_NAME_V1;

#if defined(_WIN32)
  // Do not silently pass UTF-8 to the Windows narrow CRT environment API.
  return GTI_ENVIRONMENT_UNSUPPORTED_V1;
#else
  try {
    const std::string terminatedName{name.data,
                                     static_cast<size_t>(name.length)};
    const char *value = std::getenv(terminatedName.c_str());
    if (value == nullptr)
      return GTI_ENVIRONMENT_OK_V1;

    const size_t length = std::strlen(value);
    if (length > SIZE_MAX - sizeof(Snapshot) - 1)
      return GTI_ENVIRONMENT_OUT_OF_MEMORY_V1;
    // One allocation contains metadata, exact bytes and a private final NUL.
    // Publication happens only after the entire immutable snapshot exists.
    void *allocation =
        ::operator new(sizeof(Snapshot) + length + 1, std::nothrow);
    if (allocation == nullptr)
      return GTI_ENVIRONMENT_OUT_OF_MEMORY_V1;
    auto *result = new (allocation) Snapshot{static_cast<uint64_t>(length)};
    std::memcpy(bytes(result), value, length + 1);
    *output = static_cast<int64_t>(reinterpret_cast<intptr_t>(result));
    return GTI_ENVIRONMENT_OK_V1;
  } catch (const std::bad_alloc &) {
    return GTI_ENVIRONMENT_OUT_OF_MEMORY_V1;
  } catch (...) {
    // In particular a counted name larger than string::max_size must not let
    // a C++ length_error escape the C ABI. It cannot be a valid host name.
    return GTI_ENVIRONMENT_INVALID_NAME_V1;
  }
#endif
}

extern "C" uint64_t gti_rt_environment_snapshot_size_v1(int64_t handle) {
  return handle == 0 ? 0 : snapshot(handle)->size;
}

extern "C" const void *gti_rt_environment_snapshot_data_v1(int64_t handle) {
  return handle == 0 ? nullptr : bytes(snapshot(handle));
}

extern "C" void gti_rt_environment_snapshot_destroy_v1(int64_t handle) {
  ::operator delete(snapshot(handle));
}
