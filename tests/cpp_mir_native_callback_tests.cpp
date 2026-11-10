// Frozen C++-backend coverage moved from native_callback_tests.cpp: the C++
// spelling of native callback adapters and thread-binding runtime calls.
#include "cpp_backend_test_support.h"
#include "mir_test_fixture.h"

using namespace gti_test::mir_fixture;

namespace {
lang::FrontendResult
analyzeCallbacks(std::string_view name, std::string source,
                 std::optional<lang::ExecutionProfile> profile = std::nullopt) {
  lang::FrontendOptions options;
  if (profile) {
    options.target.executionProfile = *profile;
    options.target.runtimeCapabilities.threads = true;
  }
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

void testAdapterSpelling() {
  const lang::FrontendResult frontend =
      analyzeCallbacks("native-callback.gti", R"(
[[c_opaque]] struct NativeHandle;

using Unary = (int32_t) -> int32_t;
using Notify = (NativeHandle*, c_string) -> void;
using Factory = () -> Unary;
using Resolver = (void*, c_string) -> Unary;

[[c_abi]] struct CallbackTable {
  Unary unary;
  Notify notify;
  void* user;
};

extern "C" {
  Unary set_callback(Unary callback);
  Factory set_factory(Factory factory);
  Resolver set_resolver(Resolver resolver);
  void install_callbacks(const CallbackTable* callbacks);
}

int32_t add_one(int32_t value) { return value + 1; }
int32_t checked_divide(int32_t value) { return 100 / value; }
Unary make_callback() { return add_one; }
Unary resolve_callback(void* instance, c_string name) { return nullptr; }

int main() {
  mut Unary current = nullptr;
  current = add_one;
  unsafe {
    current = set_callback(add_one);
    [[discard]] set_callback(checked_divide);
    [[discard]] set_callback(nullptr);
    [[discard]] set_factory(make_callback);
    [[discard]] set_resolver(resolve_callback);
  }
  return current == nullptr ? 0 : 0;
}
)");
  expect(frontend.canGenerateCode(),
         "exact named native callbacks should complete frontend analysis");
  if (!frontend.canGenerateCode())
    return;
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, lang::TargetInfo::host());
  const std::string cpp = lang::CppBackend().generate(lowered).contents;

  expect(cpp.find("extern \"C\" std::int32_t "
                  "__gti_native_callback_") != std::string::npos &&
             cpp.find("gti_rt_failure_terminate_v2") != std::string::npos &&
             cpp.find("catch (...)") != std::string::npos &&
             cpp.find("std::terminate()") != std::string::npos &&
             cpp.find("(&::__gti_program::__gti_native_callback_") !=
                 std::string::npos,
         "the C++ backend should emit typed noexcept adapters that contain "
         "both defined failure and native exceptions without pointer casts");
  expect(cpp.find("_resolve_callback("
                  "::gti_internal::backend::raw_address<void> instance, "
                  "const char* name);") != std::string::npos &&
             cpp.find("const const char*") == std::string::npos,
         "c_string parameters should retain their pointee constness without "
         "adding a duplicate top-level const qualifier");
}

void testThreadConfinedSpelling() {
  const std::string source = R"(
import std.cthread;
using Transform = (int32_t) -> int32_t;
[[thread_confined_callback]] int32_t transform(int32_t value) { return value; }
Transform obtain_transform() { return transform; }
int main() {
  Transform first = transform;
  Transform second = transform;
  unsafe { return first(second(0)); }
}
)";
  auto frontend = analyzeCallbacks("confined.gti", source,
                                   lang::ExecutionProfile::Concurrent);
  expect(frontend.canGenerateCode(),
         "explicit confined native callbacks compile concurrently");
  if (!frontend.canGenerateCode())
    return;
  const auto cpp = gti_test::emitCpp(frontend, lang::CppStandard::Cpp23,
                                     frontend.mir.target())
                       .contents;
  expect(cpp.find("__gti_bind_native_callback_") != std::string::npos &&
             cpp.find("gti_rt_confined_callback_enter_v1") != std::string::npos,
         "production MIR emission consumes thread binding and entry policy");
}
} // namespace

int main() {
  testAdapterSpelling();
  testThreadConfinedSpelling();
  return failures == 0 ? 0 : 1;
}
