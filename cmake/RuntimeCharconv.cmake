# Keep floating parsing available with SDKs predating libc++ 20. The public
# runtime archive remains self-contained; neither Abseil headers nor a second
# library are required by installed GTI programs.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
option(GTI_FORCE_PORTABLE_CHARCONV
       "Use the pinned portable floating parser even when std::from_chars exists"
       OFF)
# Reconfiguring an existing tree for an older deployment target must not reuse
# an overload result obtained for the build machine's newer system library.
string(SHA256 probe_context
  "${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_FLAGS};${CMAKE_CXX_COMPILER_TARGET};${CMAKE_SYSROOT};${CMAKE_OSX_SYSROOT};${CMAKE_OSX_DEPLOYMENT_TARGET};${CMAKE_CXX_STANDARD}")
if(DEFINED GTI_FLOAT_FROM_CHARS_PROBE_CONTEXT AND
   NOT GTI_FLOAT_FROM_CHARS_PROBE_CONTEXT STREQUAL probe_context)
  unset(GTI_HAS_STD_FLOAT_FROM_CHARS CACHE)
endif()
set(GTI_FLOAT_FROM_CHARS_PROBE_CONTEXT "${probe_context}" CACHE INTERNAL
    "Configuration used for the floating from_chars availability probe" FORCE)
cmake_push_check_state()
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Linking against a new SDK alone does not establish availability on the
  # requested deployment target. Also reject non-strict availability warnings.
  string(APPEND CMAKE_REQUIRED_FLAGS " -Werror=unguarded-availability-new")
endif()
check_cxx_source_compiles([=[
  #include <charconv>
  int main(int argc, char **argv) {
    float f = 0;
    double d = 0;
    const char *first = argv[0];
    const char *last = first + argc;
    auto a = std::from_chars(first, last, f, std::chars_format::general);
    auto b = std::from_chars(first, last, d, std::chars_format::hex);
    return static_cast<int>(a.ec) + static_cast<int>(b.ec);
  }
]=] GTI_HAS_STD_FLOAT_FROM_CHARS)
cmake_pop_check_state()

function(gti_configure_runtime_charconv)
  if(GTI_HAS_STD_FLOAT_FROM_CHARS AND NOT GTI_FORCE_PORTABLE_CHARCONV)
    return()
  endif()

  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  # SOURCE_SUBDIR deliberately names no upstream CMake project: only the four
  # parser sources below are built, not Abseil's unrelated libraries or tests.
  FetchContent_Declare(gti_charconv_abseil
    URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20250814.1.tar.gz
    URL_HASH SHA256=1692f77d1739bacf3f94337188b78583cf09bab7e420d2dc6c5605a4f86785a1
    SOURCE_SUBDIR gti-private-charconv-only)
  FetchContent_MakeAvailable(gti_charconv_abseil)

  # Use Abseil's documented binary-packager namespace option in an include
  # overlay. The downloaded source stays untouched, and client Abseil copies
  # cannot collide with the private parser's symbols.
  set(config_root "${CMAKE_CURRENT_BINARY_DIR}/charconv-private")
  file(READ "${gti_charconv_abseil_SOURCE_DIR}/absl/base/options.h" options)
  string(REPLACE
    "#define ABSL_OPTION_INLINE_NAMESPACE_NAME lts_20250814"
    "#define ABSL_OPTION_INLINE_NAMESPACE_NAME gti_charconv_20250814"
    options "${options}")
  file(MAKE_DIRECTORY "${config_root}/absl/base")
  file(CONFIGURE OUTPUT "${config_root}/absl/base/options.h"
       CONTENT "${options}" @ONLY)

  add_library(gti_charconv_portable OBJECT
    "${gti_charconv_abseil_SOURCE_DIR}/absl/strings/charconv.cc"
    "${gti_charconv_abseil_SOURCE_DIR}/absl/strings/internal/charconv_parse.cc"
    "${gti_charconv_abseil_SOURCE_DIR}/absl/strings/internal/charconv_bigint.cc"
    "${gti_charconv_abseil_SOURCE_DIR}/absl/strings/internal/memutil.cc")
  target_compile_features(gti_charconv_portable PRIVATE cxx_std_20)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Do not impose first-party warning-as-error policy on pinned upstream code.
    target_compile_options(gti_charconv_portable PRIVATE -Wno-error)
  endif()
  target_include_directories(gti_charconv_portable SYSTEM PRIVATE
    "${config_root}" "${gti_charconv_abseil_SOURCE_DIR}")
  set_target_properties(gti_charconv_portable PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_sources(gti_runtime PRIVATE $<TARGET_OBJECTS:gti_charconv_portable>)
  target_include_directories(gti_runtime SYSTEM PRIVATE
    "${config_root}" "${gti_charconv_abseil_SOURCE_DIR}")
  target_compile_definitions(gti_runtime PRIVATE GTI_USE_PORTABLE_CHARCONV=1)
  install(FILES "${gti_charconv_abseil_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/gti"
    RENAME "abseil-charconv-LICENSE.txt" COMPONENT gti_toolchain)
  message(STATUS "GTI floating parser: private Abseil 20250814.1")
endfunction()
