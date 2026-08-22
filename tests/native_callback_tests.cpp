#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/native_header.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

lang::FrontendResult
analyze(std::string_view name, std::string source,
        std::optional<lang::ExecutionProfile> profile = std::nullopt) {
  lang::FrontendOptions options;
  if (profile) {
    options.target.executionProfile = *profile;
  }
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

void testNativeCallbackPipeline() {
  const lang::FrontendResult frontend = analyze("native-callback.gti", R"(
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
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
  }
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "exact named native callbacks should complete frontend analysis");
  if (!frontend.canGenerateCode()) {
    return;
  }

  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "HIR should verify callback adapter identity and exact target types");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "MIR should verify callback adapter policy and target identity");
  expect(frontend.hir.nativeCallbackAdapters().size() == 4 &&
             frontend.mir.nativeCallbackAdapters().size() == 4,
         "repeated conversions should deduplicate to one adapter per exact "
         "function and callback type");

  const bool hasFailureAdapter = std::any_of(
      frontend.mir.nativeCallbackAdapters().begin(),
      frontend.mir.nativeCallbackAdapters().end(),
      [](const lang::MirNativeCallbackAdapter &adapter) {
        return adapter.targetMayRaiseDefinedFailure &&
               adapter.failurePolicy ==
                   lang::MirNativeCallbackFailurePolicy::TerminateInvocation &&
               adapter.catchesNativeExceptions;
      });
  expect(hasFailureAdapter,
         "a failure-capable GTI target should retain explicit no-unwind "
         "callback containment policy in MIR");

  lang::MirProgram forged = frontend.mir;
  auto &forgedAdapters =
      const_cast<std::vector<lang::MirNativeCallbackAdapter> &>(
          forged.nativeCallbackAdapters());
  if (!forgedAdapters.empty()) {
    forgedAdapters.front().target = 0;
  }
  expect(!forgedAdapters.empty() && !lang::verifyMirProgram(forged).valid(),
         "MIR verification should reject a callback adapter detached from its "
         "source function");

  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, frontend.mir, frontend.mir, lang::TargetInfo::host());
  const std::string cpp = lang::CppBackend().generate(lowered).contents;
  const std::string header =
      lang::NativeHeaderBackend().generate(lowered).contents;

  expect(cpp.find("extern \"C\" std::int32_t "
                  "__gti_native_callback_") != std::string::npos &&
             cpp.find("gti_rt_failure_terminate_v1") != std::string::npos &&
             cpp.find("catch (...)") != std::string::npos &&
             cpp.find("std::terminate()") != std::string::npos &&
             cpp.find("(&::__gti_program::__gti_native_callback_") !=
                 std::string::npos,
         "the C++ backend should emit typed noexcept adapters that contain "
         "both defined failure and native exceptions without pointer casts");
  expect(
      header.find("typedef int32_t (*Unary)(int32_t);") != std::string::npos &&
          header.find("typedef Unary (*Factory)(void);") != std::string::npos &&
          header.find("Unary set_callback(Unary callback);") !=
              std::string::npos &&
          header.find("Factory set_factory(Factory factory);") !=
              std::string::npos &&
          header.find("Resolver set_resolver(Resolver resolver);") !=
              std::string::npos &&
          header.find("using Factory = ::__gti_program::Unary (*)();") !=
              std::string::npos &&
          header.find("using Resolver = ::__gti_program::Unary (*)(void*, "
                      "const char*);") != std::string::npos,
      "the native header should order nested callback aliases and preserve "
      "readable C and C++ declarations");
}

void expectCallbackFailure(std::string_view name, std::string source) {
  const lang::FrontendResult result = analyze(name, std::move(source));
  expect(!result.canGenerateCode() && hasCode(result, "GTI-S2076"),
         std::string(name) +
             " should be rejected by the native callback semantic boundary");
}

void testNativeCallbackRejections() {
  expectCallbackFailure("anonymous-callback-parameter.gti", R"(
void consume((int32_t) -> int32_t callback) {}
int main() { return 0; }
)");

  expectCallbackFailure("callback-signature-mismatch.gti", R"(
using Callback = (int32_t) -> int32_t;
int64_t wrong(int64_t value) { return value; }
int main() { Callback callback = wrong; return 0; }
)");

  expectCallbackFailure("generic-callback.gti", R"(
using Callback = (int32_t) -> int32_t;
T identity<T>(T value) { return value; }
int main() { Callback callback = identity; return 0; }
)");

  expectCallbackFailure("member-callback.gti", R"(
using Callback = (int32_t) -> int32_t;
class Owner {
public:
  static int32_t transform(int32_t value) { return value; }
};
int main() { Callback callback = Owner::transform; return 0; }
)");

  expectCallbackFailure("uninitialized-callback.gti", R"(
using Callback = (int32_t) -> int32_t;
int main() { Callback callback; return 0; }
)");

  expectCallbackFailure("owned-callback-boundary.gti", R"(
class Owner {};
using Callback = (Owner) -> void;
int main() { return 0; }
)");

  const lang::FrontendResult lambda = analyze("lambda-callback.gti", R"(
using Callback = (int32_t) -> int32_t;
int main() {
  Callback callback = [](int32_t value) -> int32_t { return value; };
  return 0;
}
)");
  expect(!lambda.canGenerateCode() && hasCode(lambda, "GTI-S2003"),
         "capturing or non-capturing lambdas should not silently acquire a C "
         "callback lifetime or calling convention");
}

void testExecutionProfileBoundary() {
  const std::string declarations = R"(
using Callback = (int32_t) -> int32_t;
extern "C" { Callback set_callback(Callback callback); }
int main() { return 0; }
)";
  const lang::FrontendResult passive =
      analyze("concurrent-callback-type.gti", declarations,
              lang::ExecutionProfile::Concurrent);
  expect(passive.canGenerateCode() && passive.diagnostics.empty() &&
             passive.mir.nativeCallbackAdapters().empty(),
         "the concurrent profile may describe and transport foreign callback "
         "values when no GTI function is exposed as a callback");

  const lang::FrontendResult converted =
      analyze("concurrent-gti-callback.gti", R"(
using Callback = (int32_t) -> int32_t;
int32_t add_one(int32_t value) { return value + 1; }
int main() { Callback callback = add_one; return 0; }
)",
              lang::ExecutionProfile::Concurrent);
  expect(!converted.canGenerateCode() && hasCode(converted, "GTI-S2076"),
         "the concurrent profile should reject GTI callback entry until its "
         "thread attachment and synchronization contract exists");
}

void testFormatterSurface() {
  const std::string source = "using Factory=()->(int32_t,c_string)->int32_t;\n";
  const std::string first = lang::Formatter().format(source);
  const std::string second = lang::Formatter().format(first);
  expect(first == "using Factory = () -> (int32_t, c_string) -> int32_t;\n" &&
             second == first,
         "the formatter should apply stable C++-style spacing to nested "
         "native function types");
}

} // namespace

int main() {
  testNativeCallbackPipeline();
  testNativeCallbackRejections();
  testExecutionProfileBoundary();
  testFormatterSurface();
  if (failures != 0) {
    std::cerr << failures << " native callback test(s) failed\n";
    return 1;
  }
  std::cout << "All native callback tests passed\n";
  return 0;
}
