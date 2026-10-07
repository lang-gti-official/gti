#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
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
#include <utility>
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
  std::size_t orderedNullCallbacks = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.info.type.kind != lang::SemanticType::NativeFunction ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().type != lang::SemanticType::NullPtr) {
          continue;
        }
        ++orderedNullCallbacks;
        auto invalid = frontend.mir;
        auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
            invalid.functionInstances());
        for (auto &changedFunction : functions) {
          if (changedFunction.id != function.id) {
            continue;
          }
          for (auto &changedBlock : changedFunction.body.blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id == instruction.id) {
                changed.operands.front().type = lang::SemanticType::Int32;
              }
            }
          }
        }
        expect(!lang::verifyMirProgram(invalid).valid(),
               "native callback inputs must not admit an integer as null");
      }
    }
  }
  expect(orderedNullCallbacks == 1,
         "the native null callback argument must retain an ordered MIR input");
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

// Native indirect targets retain their native signature instead of inventing
// a selected GTI declaration or callback adapter target.
void testNativeCallableGroundwork() {
  const lang::FrontendResult transported = analyze("foreign-table.gti", R"(
using Transform = (int32_t) -> int32_t;
[[c_abi]] struct DispatchTable { Transform transform; };
Transform select_transform(DispatchTable table) { return table.transform; }
Transform forward_transform(Transform value) { return value; }
int main() { Transform inert = nullptr; return inert == nullptr ? 0 : 1; }
)");
  if (!transported.canGenerateCode()) {
    printDiagnostics(transported);
  }
  expect(transported.canGenerateCode() &&
             lang::verifyMirProgram(transported.mir).valid() &&
             transported.mir.nativeCallbackAdapters().empty(),
         "foreign callback values retain exact shapes through fields and "
         "ordinary forwarding without inventing a GTI adapter target");

  const std::vector<std::pair<std::string, std::string>> invocations = {
      {"foreign-local-invoke.gti", R"(
using Transform = (int32_t) -> int32_t;
int main() {
  Transform callback = nullptr;
  unsafe { return callback(7); }
}

)"},
      {"foreign-field-invoke.gti", R"(
using Transform = (int32_t) -> int32_t;
[[c_abi]] struct DispatchTable { Transform transform; };
int32_t invoke(DispatchTable table) {
  unsafe { return table.transform(7); }
}
int main() { return 0; }
)"},
      {"foreign-result-invoke.gti", R"(
using Transform = (int32_t) -> int32_t;
extern "C" { Transform obtain_transform(); }
int main() { unsafe { return obtain_transform()(7); } }
)"},
      {"generic-native-invoke.gti", R"(
using Transform = (int32_t) -> int32_t;
extern "C" { Transform obtain_transform(); }
int32_t invoke<F>(F callback, int32_t value) {
  unsafe { return callback(value); }
}
int main() { unsafe { return invoke(obtain_transform(), 7); } }
)"},
  };
  for (const auto &[name, source] : invocations) {
    const lang::FrontendResult invoked = analyze(name, source);
    if (!invoked.canGenerateCode())
      printDiagnostics(invoked);
    expect(invoked.canGenerateCode() &&
               lang::verifyMirProgram(invoked.mir).valid(),
           name + " must retain a verified native indirect invocation");
  }

  for (const std::string declaration : {
           "int32_t foreign_counter;",
           "const int32_t foreign_limit;",
           "Transform foreign_transform;",
       }) {
    const lang::FrontendResult rejected =
        analyze("foreign-variable.gti",
                "using Transform = (int32_t) -> int32_t;\nextern \"C\" { " +
                    declaration + " }\nint main() { return 0; }\n");
    expect(!rejected.canGenerateCode() &&
               std::any_of(rejected.diagnostics.begin(),
                           rejected.diagnostics.end(),
                           [](const lang::Diagnostic &diagnostic) {
                             return diagnostic.message.find(
                                        "only function declarations") !=
                                    std::string::npos;
                           }),
           declaration + " must not become owned GTI global storage through "
                         "foreign-declaration parser recovery");
  }
}
void testNativeIndirectVerificationAndQueries() {
  const std::string source = R"(
using Combine = (int32_t, int32_t) -> int32_t;
extern "C" { Combine obtain(); }
int main() {
  unsafe {
    Combine callback = obtain();
    return callback(2, 3);
  }
}
)";
  const auto frontend = analyze("native-indirect-contract.gti", source);
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    expect(false, "native indirect contract fixture must complete lowering");
    return;
  }
  const lang::LanguageQueries queries;
  const auto callOffset = source.rfind("callback(2, 3)");
  const auto hover =
      queries.hover(frontend, frontend.sourceGraph.entryUnit(), callOffset);
  const auto signature = queries.signatureHelp(
      frontend, frontend.sourceGraph.entryUnit(), callOffset + 12);
  const auto definition = queries.definition(
      frontend, frontend.sourceGraph.entryUnit(), callOffset);
  expect(hover &&
             hover->signature == "unsafe int32_t callback(int32_t, int32_t)" &&
             signature && signature->activeParameter == 1 &&
             signature->parameterLabels.size() == 2 && definition &&
             definition->target.start == source.find("callback ="),
         "native call queries must retain the signature and underlying "
         "callback binding identity");
  std::size_t calls = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.nativeCalleeType)
          continue;
        ++calls;
        expect(lang::isMirNativeIndirectCall(frontend.mir, function.body,
                                             instruction),
               "native call must satisfy the shared exact MIR contract");
        const auto effect = lang::effects(frontend.mir, instruction);
        expect(!effect.speculatable && !effect.removableWhenUnused &&
                   !effect.reorderable && effect.readsUnknownMemory &&
                   effect.writesUnknownMemory,
               "native address invocation must retain conservative foreign "
               "effects rather than a guessed GTI body summary");
        for (int mutation = 0; mutation < 10; ++mutation) {
          auto forged = frontend.mir;
          auto &functions =
              const_cast<std::vector<lang::MirFunctionInstance> &>(
                  forged.functionInstances());
          for (auto &changedFunction : functions) {
            if (changedFunction.id != function.id)
              continue;
            for (auto &changedBlock : changedFunction.body.blocks) {
              for (auto &changed : changedBlock.instructions) {
                if (changed.id != instruction.id)
                  continue;
                switch (mutation) {
                case 0:
                  changed.nativeCalleeType.reset();
                  break;
                case 1:
                  changed.unsafeOperation = lang::UnsafeOperationKind::None;
                  break;
                case 2:
                  changed.receiver.reset();
                  break;
                case 3:
                  changed.functionTarget = function.id;
                  break;
                case 4:
                  changed.parameterTypes[0] = lang::SemanticType::Int64;
                  break;
                case 5:
                  changed.info.type = lang::SemanticType::Int64;
                  break;
                case 6:
                  changed.receiver->type = lang::SemanticType::Int32;
                  break;
                case 7:
                  changed.callSite = 0;
                  break;
                case 8:
                  changed.definedFailure.propagation =
                      lang::FailurePropagationKind::Callable;
                  break;
                case 9:
                  changed.operands[0] = *changed.receiver;
                  break;
                }
              }
            }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "forged native invocation identity/signature/permission "
                 "must fail MIR verification");
        }
      }
    }
  }
  expect(calls == 1, "fixture must exercise one exact native indirect call");
  const auto emitted = gti_test::emitCpp(frontend).contents;
  expect(emitted.find("GTI verified-MIR body") != std::string::npos,
         "native invocation must be emitted through verified production MIR");

  const auto unsafeMissing = analyze("native-safe-call.gti", R"(
using Unary = (int32_t) -> int32_t;
int main() { Unary callback = nullptr; return callback(7); }
)");
  expect(!unsafeMissing.canGenerateCode() &&
             hasCode(unsafeMissing, "GTI-S2055"),
         "scalar-only indirect native calls still require lexical unsafe");
  const auto arity = analyze("native-arity.gti", R"(
using Unary = (int32_t) -> int32_t;
int main() { Unary callback = nullptr; unsafe { return callback(); } }
)");
  expect(!arity.canGenerateCode() && hasCode(arity, "GTI-S2005"),
         "native invocation arity is established before MIR");
  expectCallbackFailure("native-generic-arguments.gti", R"(
using Unary = (int32_t) -> int32_t;
int main() { Unary callback = nullptr; unsafe { return callback<int32_t>(7); } }
)");
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

void testConcreteFailureOverlay() {
  const auto ordinary = analyze("generic-gti-callable.gti", R"(
int32_t identity(int32_t value) { return value; }
int32_t invoke<F>(F callback, int32_t value) { return callback(value); }
int main() { return invoke(identity, 0); }
)");
  expect(ordinary.canGenerateCode() &&
             lang::verifyMirProgram(ordinary.mir).valid(),
         "ordinary GTI callable specialization must retain its selected "
         "function failure contract beside native specialization");
}

void testNativeViewReturnBoundary() {
  for (const std::string declaration : {
           "using Bad = () -> std::string_view;",
           "using Text = std::string_view; using Bad = () -> Text;",
           "using Bad = (std::string_view) -> std::string_view;",
       }) {
    const std::string source = declaration + "\nint main() { return 0; }\n";
    const auto result = analyze("native-view-return.gti", source);
    const std::size_t begin = source.find("-> ") + 3;
    const std::size_t tokenBegin = source.find("::", begin) == std::string::npos
                                       ? begin
                                       : source.find("::", begin) + 2;
    const std::size_t tokenEnd = source.find(';', tokenBegin);
    expect(!result.canGenerateCode() && !hasCode(result, "GTI-B0001") &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2076" &&
                                    diagnostic.phase ==
                                        lang::DiagnosticPhase::Semantics &&
                                    diagnostic.primary.start == tokenBegin &&
                                    diagnostic.primary.end == tokenEnd &&
                                    diagnostic.message.find(
                                        "native function return") !=
                                        std::string::npos;
                           }),
           "counted-view return must fail at its semantic return token");
  }
  const auto factory = analyze("native-view-factory.gti", R"(
using Reader = (std::string_view) -> c_int;
using Factory = () -> Reader;
extern "C" { Factory obtain(); }
int main() { unsafe { return obtain()()("hello"); } }
)");
  expect(factory.canGenerateCode() &&
             lang::verifyMirProgram(factory.mir).valid(),
         "a callback factory may return a callback accepting counted input");
  if (factory.canGenerateCode()) {
    expect(!gti_test::emitCpp(factory).contents.empty(),
           "counted-input callback factories must reach production emission");
  }
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
  testNativeCallableGroundwork();
  testNativeIndirectVerificationAndQueries();
  testExecutionProfileBoundary();
  testConcreteFailureOverlay();
  testNativeViewReturnBoundary();
  testFormatterSurface();
  if (failures != 0) {
    std::cerr << failures << " native callback test(s) failed\n";
    return 1;
  }
  std::cout << "All native callback tests passed\n";
  return 0;
}
