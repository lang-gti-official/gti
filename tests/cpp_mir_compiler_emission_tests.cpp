// Frozen C++-backend representation checks moved out of the general compiler
// suites (frontend, constexpr, diagnostics, e2e) and function_signature_tests.
// Each case asserts the generated C++ spelling of a program whose language
// behaviour is proven backend-neutrally (MIR, semantics, LLVM emission) in its
// original suite. Assertion text is kept as it was in the original test.

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/mir.h"
#include "gti/optimizer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"

#include "cpp_backend_test_support.h"
#include "mir_test_fixture.h"
#include "native_symbol_spelling.h"
#include "temporary_directory.h"

#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti";
}

std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

lang::FrontendResult analyzeWithLibrary(const std::filesystem::path &entry,
                                        const std::string &source,
                                        lang::FrontendOptions options = {}) {
  lang::FrontendResult result =
      lang::Frontend(std::move(options))
          .analyze(entry, source, {standardLibraryPrelude()}, {},
                   {standardLibraryRoot()});
  expect(result.canGenerateCode(), "the C++ representation fixture '" +
                                       entry.filename().string() +
                                       "' should pass the frontend");
  if (!result.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  return result;
}

std::string
emitAnalyzedSource(std::string_view source,
                   lang::TargetInfo target = lang::TargetInfo::host(),
                   lang::CppStandard standard = lang::CppStandard::Cpp23) {
  const lang::FrontendResult frontend =
      lang::Frontend({.target = target})
          .analyze("compiler-emission-fixture.gti", std::string(source));
  expect(frontend.canGenerateCode(),
         "a semantically valid emission fixture should complete MIR "
         "lowering");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected emission-fixture diagnostic: "
                << diagnostic.message << '\n';
    }
    return {};
  }
  return gti_test::emitCppText(frontend, standard, std::move(target));
}

std::unordered_set<std::string>
concreteBackendSymbols(std::string_view text, std::string_view marker,
                       std::string_view sourceName) {
  std::unordered_set<std::string> result;
  std::size_t search = 0;
  while ((search = text.find(marker, search)) != std::string_view::npos) {
    const std::size_t digest = search + marker.size();
    std::size_t tail = digest;
    while (tail < text.size() && tail - digest < 64 &&
           ((text[tail] >= '0' && text[tail] <= '9') ||
            (text[tail] >= 'a' && text[tail] <= 'f'))) {
      ++tail;
    }
    if (tail - digest == 64 && tail < text.size() && text[tail] == '_' &&
        text.substr(tail + 1, sourceName.size()) == sourceName) {
      const std::size_t end = tail + 1 + sourceName.size();
      result.emplace(text.substr(search, end - search));
    }
    search = digest;
  }
  return result;
}

// --- moved from compiler_e2e_tests.cpp -------------------------------------

// From testFrontendBackendAndOptimizationPipeline.
void testCppBackendConsumesOptimizedMir() {
  const std::string source = R"(
int main() {
  bool folded = (1 < 2) and !false;
  int arithmetic = 1 + 2;
  if (arithmetic == 3) {
    return 0;
  }
  return arithmetic;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("frontend.gti", source);
  expect(frontend.canGenerateCode(),
         "the shared frontend should produce a checked program");
  const lang::OptimizationResult optimized = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O1);
  const lang::OptimizedProgram optimizedProgram =
      lang::OptimizationPipeline().run(
          lang::OptimizationRequest{.hir = frontend.hir,
                                    .mir = frontend.mir,
                                    .level = lang::OptimizationLevel::O1,
                                    .compatibility = &optimized});
  expect(optimizedProgram.valid(),
         "the owned MIR pipeline should optimize the fixture");
  std::unique_ptr<lang::Backend> backend = std::make_unique<lang::CppBackend>();
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimizedProgram.sourceMir, optimizedProgram.mir,
      lang::TargetInfo::host(), optimizedProgram.report.transformations);
  const lang::BackendArtifact artifact = backend->generate(lowered);
  expect(backend->name() == "cpp" &&
             artifact.kind == lang::BackendArtifactKind::Source &&
             artifact.extension == ".cpp",
         "the C++ emitter should be available through the backend contract");
  const std::size_t mainBodyStart = artifact.contents.find("function-instance");
  const std::string_view mainOnward =
      mainBodyStart == std::string::npos
          ? std::string_view()
          : std::string_view(artifact.contents).substr(mainBodyStart);
  expect(mainOnward.find("= true;") != std::string_view::npos &&
             mainOnward.find(" < __gti_mir_v_") == std::string_view::npos,
         "the C++ backend should consume optimization results");
  expect(mainOnward.find("= static_cast<std::int32_t>(3);") !=
             std::string_view::npos,
         "proven in-range integer arithmetic should fold to a typed constant");
  expect(!mainOnward.empty() &&
             mainOnward.find("if ((") == std::string_view::npos,
         "contextual binary conditions should not gain warning-producing "
         "parentheses in C++");
}

// From testProgramArgumentsEntryPoint.
void testCppProgramArgumentsEntryAdapter() {
  const std::string source = R"(
import std.string;
import std.vector;
using arguments_type = std::vector<std::string>;

int main(mut int argc, mut arguments_type argv) {
  if (argc > 0 and argv.size() == uint64_t(argc) and
      argv[std::size_t(0)].size() > 0) {
    return 0;
  }
  return 1;
}
)";
  const lang::FrontendResult valid =
      analyzeWithLibrary("program-arguments.gti", source);
  const lang::FunctionInfo *mainInfo = nullptr;
  for (const lang::StmtPtr &declaration : valid.program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == "main")
      mainInfo = valid.semantics.findFunction(*function);
  }
  const std::string generated = gti_test::emitCppText(valid);
  const lang::FunctionInfo *append =
      mainInfo == nullptr
          ? nullptr
          : valid.semantics.findFunction(mainInfo->entryArgumentAppendFunction);
  const std::unordered_set<std::string> appendNames =
      concreteBackendSymbols(generated, "__gti_member_", "push_back");
  // Public member aliases can require the native template carrier instead of
  // a concrete owner spelling. Both representations must invoke the exact
  // selected GTI append declaration; neither may rely on native overload
  // lookup.
  const std::string appendName =
      appendNames.size() == 1 ? *appendNames.begin()
      : appendNames.empty() && append != nullptr
          ? "__gti_fn_" + std::to_string(append->id) + "_push_back"
          : std::string{};
  expect(generated.find(
             "int main(int __gti_native_argc, char **__gti_native_argv)") !=
                 std::string::npos &&
             generated.find("std::string_view("
                            "__gti_native_argv[__gti_index])") !=
                 std::string::npos &&
             !appendName.empty() && append != nullptr &&
             generated.find("__gti_arguments." + appendName +
                            "__gti_mir_failure(") != std::string::npos &&
             generated.find("std::move(__gti_arguments)") != std::string::npos,
         "the C++ backend should isolate native argc/argv in an owned GTI "
         "entry adapter");
}

// From testCompletePipeline.
void testCppCompletePipelineText() {
  const std::string source = R"(
int twice(int value) {
  return value * 2;
}

int main() {
  int result = twice(4);
  if (result == 8) {
    return 0;
  }
  return 1;
}
)";
  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find(
                 "_twice__gti_mir_failure(std::int32_t __gti_mir_arg_0") !=
                 std::string::npos &&
             generated.find("mir_checked_multiply_v1<std::int32_t>") !=
                 std::string::npos,
         "emitter should lower function signatures");
  expect(
      generated.find("_twice__gti_mir_failure(__gti_mir_v_") !=
              std::string::npos &&
          generated.find("// GTI verified-MIR body: scalar-cfg-failure-v1") !=
              std::string::npos,
      "emitter should lower immutable source bindings through MIR-owned "
      "places");
  expect(generated.find("#include <iostream>") == std::string::npos,
         "emitter should not include print runtime support");
}

// --- moved from compiler_diagnostics_tests.cpp -----------------------------

// From testDefaultImmutability.
void testCppDefaultImmutabilityText() {
  const std::string validSource = R"(
int identity(int value) { return value; }
int main() {
  int fixed = 1;
  mut int moving = 1;
  moving++;
  return identity(fixed);
}
)";
  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("_identity(std::int32_t __gti_mir_arg_0)") !=
                 std::string::npos &&
             generated.find("__gti_mir_p_1 = __gti_mir_arg_0") !=
                 std::string::npos,
         "parameters should enter verified MIR through exact typed places");
  expect(generated.find("__gti_mir_p_1 = __gti_mir_v_1") != std::string::npos,
         "immutable variables should retain distinct MIR storage after "
         "semantic immutability checks");
  expect(
      generated.find("mir_checked_add_v1<std::int32_t>") != std::string::npos &&
          generated.find("__gti_mir_p_2 = __gti_mir_v_4") != std::string::npos,
      "mut variables should lower through explicit MIR updates");
}

// From testDefaultNodiscard.
void testCppDefaultNodiscardText() {
  const std::string validSource = R"(
int calculate() { return 7; }
void perform() {}
int main() {
  [[discard]] calculate();
  perform();
  mut int count = 0;
  count++;
  return 0;
}
)";
  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("calculate();") != std::string::npos &&
             generated.find("[[discard]]") == std::string::npos,
         "discard should be a GTI-only call-site attribute");
}

// From testPrintIsAnIdentifier.
void testCppPrintIsAnIdentifierText() {
  const std::string source = R"(
int print(int value) { return value; }
int main() { return print(0); }
)";
  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find("_print(std::int32_t __gti_mir_arg_0)") !=
                 std::string::npos &&
             generated.find("_print(__gti_mir_v_") != std::string::npos,
         "print should lower as a normal function");
}

// --- moved from function_signature_tests.cpp --------------------------------

// From signatureTypes.
void testCppSignatureAliasSpelling() {
  const std::string source = R"(
using Action = void();
using Predicate = bool(int);
using Borrowing = int&(int&);
class SignatureTag<S> {
public:
  using signature = S;
  int value() { return 7; }
public:
  SignatureTag() = default;
};
class Description<T> {
public:
  using signature = T(T&);
public:
  Description() = default;
};
int identify<S>(SignatureTag<S>& value) { return value.value(); }
int main() {
  SignatureTag<void()> first{};
  SignatureTag<Action> second{};
  SignatureTag<Predicate> third{};
  SignatureTag<int&(int&)> fourth{};
  Description<int> descriptor{};
  return identify(first) + identify(second) + identify(third) +
         identify(fourth) - 28;
}
)";
  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("using Action = void();") != std::string::npos &&
             generated.find("using Predicate = bool(std::int32_t);") !=
                 std::string::npos,
         "signature aliases should lower as types without function pointers");
}

// From receiverQualifiedSignatures.
void testCppQualifiedSignatureSpelling() {
  const std::string source = R"(
using Read = int(int);
using Mutable = int(int) mut;
using Once = int(int) &&;
class Tag<S> { public: Tag() = default; };
int choose<R, Args...>(Tag<R(Args...)>& tag) { return 1; }
int choose<R, Args...>(Tag<R(Args...) mut>& tag) { return 2; }
int choose<R, Args...>(Tag<R(Args...) &&>& tag) { return 3; }
class MutablePattern<R(Args...) mut> { public: using signature = R(Args...) mut;  public: MutablePattern() = default; };
int main() {
  Tag<Read> read{};
  Tag<Mutable> write{};
  Tag<Once> once{};
  Tag<int(int) mut> same{};
  MutablePattern<Mutable> pattern{};
  return choose(read) + choose(write) + choose(once) + choose(same) - 8;
}
)";
  const auto generated = emitAnalyzedSource(source);
  expect(generated.find("using Mutable = std::int32_t(std::int32_t) &;") !=
                 std::string::npos &&
             generated.find("using Once = std::int32_t(std::int32_t) &&;") !=
                 std::string::npos,
         "the native metadata representation distinguishes qualifiers without "
         "pointers");
}

// --- moved from compiler_frontend_tests.cpp ------------------------------

// From testStandardLibraryImports.
void testCppStandardArraySpelling() {
  const std::filesystem::path root =
      gti_test::temporaryDirectory() / "gti-standard-imports";
  const std::filesystem::path entry = root / "main.gti";
  const lang::FrontendResult imported = lang::Frontend().analyze(
      entry,
      "import std.array;\n"
      "import std.array;\n"
      "class NoDefault {\n"
      "  int value;\n"
      "public:\n"
      "  NoDefault(int value) : value(value) {}\n"
      "};\n"
      "int main() {\n"
      "  int initial[3] = {1, 2, 3};\n"
      "  mut std::array<int, 3> values = std::array<int, 3>(initial);\n"
      "  std::array<int, 0> empty_values = std::array<int, 0>();\n"
      "  NoDefault objects[1] = {NoDefault(7)};\n"
      "  std::array<NoDefault, 1> non_default = "
      "std::array<NoDefault, 1>(objects);\n"
      "  std::size_t value_count = values.size();\n"
      "  std::ptrdiff_t offset = std::ptrdiff_t(-1);\n"
      "  values[1] = 4;\n"
      "  if (value_count == 3 and offset < 0 and !values.empty() and "
      "empty_values.size() == 0 and empty_values.empty() and "
      "values.at(1) == 4) { return 0; }\n"
      "  return 1;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::string generated = gti_test::emitCppText(imported);
  expect(generated.find("namespace __gti_std") != std::string::npos &&
             generated.find("_array {") != std::string::npos &&
             generated.find("std::array<std::int32_t, 3> values") !=
                 std::string::npos &&
             generated.find("::__gti_program::__gti_std::__gti_type_") !=
                 std::string::npos &&
             generated.find("template <typename T, std::uint64_t N>\n"
                            "    class array") == std::string::npos,
         "std::array should remain source-defined while its selected "
         "application emits as one concrete GTI-owned class");

  const lang::FrontendResult distinctStandardNamespace =
      lang::Frontend().analyze(
          root / "user-gti-std.gti",
          "namespace gti_std {\n"
          "class string_view {\n"
          "  int32_t value;\n"
          "public:\n"
          "  string_view(int32_t value) : value(value) {}\n"
          "  int32_t read() { return this.value; }\n"
          "};\n"
          "}\n"
          "int main() {\n"
          "  gti_std::string_view value = gti_std::string_view(7);\n"
          "  return value.read() - 7;\n"
          "}\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(distinctStandardNamespace.canGenerateCode(),
         "a user gti_std namespace should remain distinct in GTI");
  const std::string distinctNamespaceCpp =
      gti_test::emitCppText(distinctStandardNamespace);
  expect(
      distinctNamespaceCpp.find("namespace __gti_std") != std::string::npos &&
          distinctNamespaceCpp.find("namespace gti_std") != std::string::npos,
      "the C++ backend should reserve its std remap without claiming the "
      "user gti_std namespace");
}

// From testStandardLibraryImports (std::tcp).
void testCppTcpSpelling() {
  const std::filesystem::path entry =
      gti_test::temporaryDirectory() / "gti-standard-imports" / "main.gti";
  const lang::TargetInfo linuxTarget{"linux", "unknown", "x86_64"};
  const lang::FrontendOptions linuxOptions{.target = linuxTarget};
  const lang::FrontendResult tcp =
      lang::Frontend(linuxOptions)
          .analyze(entry,
                   "import std.tcp;\n"
                   "int main() {\n"
                   "  mut auto opened = std::tcp::open();\n"
                   "  if (!opened or !opened.value().is_open()) { return 1; }\n"
                   "  auto closed = opened.value().close();\n"
                   "  if (!closed or opened.value().is_open()) { return 2; }\n"
                   "  auto closed_again = opened.value().close();\n"
                   "  if (closed_again or closed_again.error() != "
                   "std::tcp::errc::not_open) { return 3; }\n"
                   "  return 0;\n"
                   "}\n",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!tcp.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : tcp.diagnostics) {
      std::cerr << "Unexpected tcp import diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(tcp.canGenerateCode(),
         "the POSIX std::tcp owner should provide typed open and close");
  const std::string tcpCpp =
      gti_test::emitCppText(tcp, lang::CppStandard::Cpp23, linuxTarget);
  expect(
      tcpCpp.find("class socket") != std::string::npos &&
          tcpCpp.find("runtime::" + gti_test::nativeSpelling(tcpCpp, "socket") +
                      "(") != std::string::npos &&
          std::regex_search(
              tcpCpp, std::regex(R"(open__gti_mir_failure\(::__gti_program::)"
                                 R"(__gti_type_[0-9a-f]+_expected)")) &&
          tcpCpp.find("runtime::" + gti_test::nativeSpelling(tcpCpp, "close") +
                      "(") != std::string::npos &&
          tcpCpp.find("socket(socket &&other) noexcept") != std::string::npos &&
          tcpCpp.find("// GTI verified-MIR body:") != std::string::npos,
      "the tcp owner should retain private POSIX acquisition and generated "
      "move cleanup");
}

// From testLoopControlStatements.
void testCppLoopControlText() {
  const std::string source = R"(
int main() {
  mut int total = 0;
  for (mut int outer = 0; outer < 4; outer++) {
    if (outer == 1) {
      continue;
    }
    while (true) {
      total += outer;
      break;
    }
  }
  return total;
}
)";
  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("switch (__gti_mir_bb)") == std::string::npos &&
             generated.find("__gti_mir_bb =") == std::string::npos &&
             generated.find("goto __gti_mir_bb_") != std::string::npos &&
             generated.find("__gti_mir_bb_1: {") != std::string::npos &&
             generated.find("// GTI verified-MIR body:") != std::string::npos,
         "the C++ backend should lower loop control through direct verified "
         "MIR CFG edges");
}

// From testDoWhileStatements.
void testCppDoWhileText() {
  const std::string source = R"(
class Marker {
public:
  Marker() {}
  ~Marker() {}
};

int run() {
  mut int value = 0;
  do {
    Marker marker{};
    value++;
    if (value < 2) {
      continue;
    }
    if (value == 4) {
      break;
    }
  } while (value < 5);
  return value;
}

int main() { return run() - 4; }
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("do-while.gti", source);
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact generated =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // The cleanup-gate narrowing admitted this body to general verified-MIR
  // emission (its scope owner's cleanup cannot raise); the body-first MIR
  // structure is pinned above, and the backend now emits the verified
  // schedule instead of the compatibility do-while text.
  expect(generated.contents.find("// GTI verified-MIR body: scalar-cfg-v1") !=
             std::string::npos,
         "the C++ backend should emit the do-while body from verified MIR");
}

// From testConditionalExpressions.
void testCppConditionalOwnerText() {
  const std::string source = R"(
import std.memory;

int choose(bool condition, int left, int right) {
  return condition ? left : right;
}

int branch_effect(bool condition) {
  mut int value = 0;
  int selected = condition ? (value = 7) : (value = 3);
  return selected + value;
}

void set_value(mut int& target, int value) { target = value; }

void choose_void(bool condition, mut int& target) {
  condition ? set_value(target, 1) : set_value(target, 2);
}

struct Item {
  int value;
  Item(int initial) : value(initial) {}
};

std::unique_ptr<Item> choose_owner(
    bool condition,
    std::unique_ptr<Item> left,
    std::unique_ptr<Item> right) {
  return condition ? std::move(left) : std::move(right);
}

int main() {
  mut int result = choose(true, 4, 9);
  result += choose(false, 1, 3);
  result += branch_effect(false);
  mut int side_effect = 0;
  choose_void(true, side_effect);
  result += side_effect;
  auto owner = choose_owner(
      true, std::make_unique<Item>(8), std::make_unique<Item>(9));
  return result - 14;
}
)";
  const lang::FrontendResult frontend =
      analyzeWithLibrary("conditional-expression.gti", source);
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact generated =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // choose_owner cannot fail, so its plain form stages the selected owner
  // in the returned value's slot exactly where a failure form would publish
  // into its result storage, then retires the moved-from sources.
  const auto publication = generated.contents.find(
      "__gti_mir_v_4.construct(std::move(__gti_mir_p_4.get()));");
  const auto retirement =
      generated.contents.find("__gti_mir_p_4.destroy();", publication);
  const auto rightCleanup =
      generated.contents.find("__gti_mir_p_3.destroy();", retirement);
  const auto leftCleanup =
      generated.contents.find("__gti_mir_p_2.destroy();", rightCleanup);
  const auto returnPublication = generated.contents.find(
      "return std::move(__gti_mir_v_4.get());", publication);
  expect(generated.contents.find("// GTI verified-MIR body: scalar-cfg-v1") !=
                 std::string::npos &&
             generated.contents.find("if (__gti_mir_v_1) {") !=
                 std::string::npos &&
             generated.contents.find("goto __gti_mir_bb_2;") !=
                 std::string::npos &&
             generated.contents.find("goto __gti_mir_bb_3;") !=
                 std::string::npos &&
             generated.contents.find("switch (__gti_mir_bb)") ==
                 std::string::npos &&
             generated.contents.find(
                 "__gti_mir_p_4.construct(std::move(__gti_mir_p_2.get()));") !=
                 std::string::npos &&
             generated.contents.find(
                 "__gti_mir_p_4.construct(std::move(__gti_mir_p_3.get()));") !=
                 std::string::npos &&
             publication != std::string::npos &&
             retirement != std::string::npos &&
             rightCleanup != std::string::npos &&
             leftCleanup != std::string::npos &&
             returnPublication != std::string::npos &&
             publication < retirement && retirement < rightCleanup &&
             rightCleanup < leftCleanup && leftCleanup < returnPublication,
         "the C++ backend should emit scalar conditional control flow from "
         "verified MIR while preserving explicit ownership transfer");
}

// From testSwitchStatements.
void testCppSwitchText() {
  const std::string source = R"(
enum class Stage : uint8_t { Boot, Ready, Running };

int classify(Stage stage) {
  switch (stage) {
  case Stage::Boot:
    return 0;
  case Stage::Ready:
  case Stage::Running:
    return 1;
  default:
    return 2;
  }
}

int main() {
  mut int total = 0;
  for (mut int index = 0; index < 3; index++) {
    switch (index) {
    case 0:
      continue;
    case 1:
      int local = 1;
      total += local;
      break;
    default:
      int local = 2;
      total += local;
      break;
    }
  }
  char marker = 'G';
  switch (marker) {
  case 'G':
    total += 1;
    break;
  default:
    break;
  }
  uint64_t wide = 7;
  switch (wide) {
  case uint64_t(7):
    total += classify(Stage::Ready);
    break;
  default:
    break;
  }
  return total;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("switch.gti", source);
  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("switch (__gti_mir_v_") != std::string::npos &&
             generated.find("case static_cast<::__gti_program::Stage>(0):") !=
                 std::string::npos &&
             generated.find("case static_cast<std::uint64_t>(7):") !=
                 std::string::npos &&
             generated.find("goto __gti_mir_bb_") != std::string::npos &&
             generated.find("switch (__gti_mir_bb)") == std::string::npos &&
             generated.find("__gti_mir_bb_1: {") != std::string::npos &&
             generated.find("// GTI verified-MIR body:") != std::string::npos,
         "the C++ backend should emit normalized labels through arm-local MIR "
         "CFG blocks");
}

// --- moved from compiler_constexpr_tests.cpp ----------------------------

// From testFixedWidthIntegers.
void testCppFixedWidthIntegerSpelling() {
  const std::string source = R"(
int8_t minimum8 = -128;
int16_t widened16 = minimum8;
int32_t maximum32 = 2147483647;
int default32 = maximum32;
int64_t maximum64 = 9223372036854775807;
int64_t minimum64 = -9223372036854775808;
uint8_t maximum_u8 = 255;
uint16_t widened_u16 = maximum_u8;
uint32_t maximum_u32 = 4294967295;
uint default_u32 = maximum_u32;
uint64_t maximum_u64 = 18446744073709551615;
int64_t signed_widening = maximum_u32;
uint32_t hexadecimal_pattern = 0xA5A5;
uint8_t binary_pattern = 0b10100101;

int64_t add_wide(int16_t left, int64_t right) {
  return left + right;
}

uint64_t add_unsigned(uint16_t left, uint64_t right) {
  return left + right;
}

int main() {
  int8_t maximum8 = 127;
  int16_t minimum16 = -32768;
  int32_t promoted = maximum8 + minimum16;
  uint8_t unsigned_left = 1;
  uint8_t unsigned_right = 2;
  int32_t promoted_unsigned = unsigned_left + unsigned_right;
  uint8_t bytes[4] = {1, 2, 3, 4};
  uint32_t counter = 1;
  uint32_t next = counter + 1;
  bool has_next = next > 0;
  return promoted;
}
)";
  const std::string generated = emitAnalyzedSource(source);
  expect(
      generated.find("#include <cstdint>") != std::string::npos &&
          generated.find("std::int8_t minimum8;") != std::string::npos &&
          generated.find("std::int16_t widened16;") != std::string::npos &&
          generated.find("std::int32_t default32;") != std::string::npos &&
          generated.find("std::int64_t maximum64;") != std::string::npos &&
          generated.find("std::int64_t minimum64;") != std::string::npos &&
          generated.find("std::uint8_t maximum_u8;") != std::string::npos &&
          generated.find("std::uint16_t widened_u16;") != std::string::npos &&
          generated.find("std::uint32_t default_u32;") != std::string::npos &&
          generated.find("std::uint64_t maximum_u64;") != std::string::npos &&
          generated.find("std::uint32_t hexadecimal_pattern;") !=
              std::string::npos &&
          generated.find("std::uint8_t binary_pattern;") != std::string::npos &&
          generated.find("std::array<std::uint8_t, 4>") != std::string::npos &&
          generated.find("static_cast<std::uint8_t>(1)") != std::string::npos &&
          generated.find("static_cast<std::uint8_t>(4)") != std::string::npos &&
          generated.find("program-initialization-v1 module-instance 0") !=
              std::string::npos &&
          generated.find("static_cast<std::int8_t>(-127 - 1)") !=
              std::string::npos &&
          generated.find("static_cast<std::int16_t>(-32767 - 1)") !=
              std::string::npos &&
          generated.find("static_cast<std::int64_t>("
                         "-9223372036854775807 - 1)") != std::string::npos &&
          generated.find("static_cast<std::uint64_t>("
                         "18446744073709551615ULL)") != std::string::npos &&
          generated.find("::__gti_program::hexadecimal_pattern = ") !=
              std::string::npos &&
          generated.find("::__gti_program::binary_pattern = ") !=
              std::string::npos &&
          generated.find("::__gti_program::__gti_program_initialize(") !=
              std::string::npos &&
          generated.find("int main()") != std::string::npos,
      "integer widths should lower to cstdint types while main stays valid");
}

// From testContextualSignedIntegerLiterals.
void testCppContextualSignedLiteralSpelling() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("contextual-signed-integer-literals.gti", R"(
bool matches_boundaries(int8_t narrow, int32_t wide, int64_t widest) {
  return narrow == -1 and (-128) <= narrow and
         wide == -2147483648 and widest == -(9223372036854775808);
}
)");
  const std::string generated = gti_test::emitCppText(valid);
  expect(generated.find("mir_checked_negate_v1<std::int8_t>") !=
                 std::string::npos &&
             generated.find("static_cast<std::int32_t>(-2147483647 - 1)") !=
                 std::string::npos &&
             generated.find(
                 "static_cast<std::int64_t>(-9223372036854775807 - 1)") !=
                 std::string::npos,
         "the C++ backend should cast the complete signed literal "
         "expression into its frontend-selected domain");
}

// From testBinaryFloatSemantics.
void testCppBinaryFloatSpelling() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("constexpr-float.gti", R"(
constexpr float sum = 0.1 + 0.2;
constexpr float rounded = float(16777217);
constexpr int truncated = int(3.9);
constexpr float negative_zero = -0.0;
constexpr float nan = 0.0 / 0.0;
constexpr bool ordered = sum > 0.2;
constexpr bool zeros_equal = negative_zero == 0.0;
constexpr bool nan_equal = nan == nan;
constexpr bool nan_not_equal = nan != nan;
constexpr float half(float value) { return value / 2.0; }
constexpr float result = half(1.0);
constexpr float advance(float value) {
  mut float current = value;
  current += 1;
  current++;
  return current;
}
constexpr float advanced = advance(0.5);
int main() {
  if constexpr (ordered and zeros_equal and !nan_equal and nan_not_equal and
                truncated == 3 and result == 0.5 and advanced == 2.5) {
    return 0;
  } else {
    return discarded_float_branch_is_not_analyzed;
  }
}
)");
  const lang::OptimizationResult optimized = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O1);
  const std::string generated = gti_test::emitCppText(
      frontend, lang::CppStandard::Cpp23, lang::TargetInfo::host(),
      lang::OptimizationLevel::O1, &optimized);
  expect(generated.find("std::bit_cast<float>(std::uint32_t{0x3e99999aU})") !=
                 std::string::npos &&
             generated.find("std::numeric_limits<float>::is_iec559") !=
                 std::string::npos &&
             generated.find("__gti_strict_ieee754 == 1") != std::string::npos &&
             generated.find("discarded_float_branch_is_not_analyzed") ==
                 std::string::npos,
         "the backend should emit exact binary32 constants, require an IEEE "
         "host, and erase unselected constexpr branches");

  const lang::FrontendResult ordinary = lang::Frontend().analyze(
      "fold-float.gti",
      "int main() { float rounded = float(16777217); "
      "float sum = 0.1 + 0.2; float mixed = 0.5 + 1; "
      "if (sum > rounded or mixed == 0.0) { return 1; } return 0; }");
  const lang::OptimizationResult ordinaryOptimized =
      lang::OptimizationPipeline().run(ordinary.hir,
                                       lang::OptimizationLevel::O1);
  const std::string ordinaryGenerated = gti_test::emitCppText(
      ordinary, lang::CppStandard::Cpp23, lang::TargetInfo::host(),
      lang::OptimizationLevel::O1, &ordinaryOptimized);
  expect(ordinary.canGenerateCode() &&
             ordinaryGenerated.find(
                 "std::bit_cast<float>(std::uint32_t{0x4b800000U})") !=
                 std::string::npos &&
             ordinaryGenerated.find(
                 "std::bit_cast<float>(std::uint32_t{0x3e99999aU})") !=
                 std::string::npos &&
             ordinaryGenerated.find(
                 "std::bit_cast<float>(std::uint32_t{0x3fc00000U})") !=
                 std::string::npos,
         "ordinary O1 folding should use APFloat for integer-to-float "
         "conversion and binary32 arithmetic, not only constexpr bindings");
}

// From testCharactersAndStringViews.
void testCppCharacterTextSpelling() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "text-values.gti", R"(
char letter = 'G';
char newline = '\n';
std::string_view text = "GTI\0text";
bool chars_match = letter == 'G' and newline == '\n';
bool literal_chars_match = 'G' == 'G';
bool text_matches = text == "GTI\0text";
std::size_t text_size = text.size();
bool text_not_empty = !text.empty();
char first = text[0];
char embedded_zero = text[3];
uint8_t letter_code = uint8_t(letter);

int main() {
  if (chars_match and text_matches and text_size == 8 and text_not_empty and
      first == 'G' and embedded_zero == '\0' and letter_code == 71) {
    return 0;
  }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::OptimizationResult optimized =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O1);
  const std::string generated = gti_test::emitCppText(
      valid, lang::CppStandard::Cpp23, lang::TargetInfo::host(),
      lang::OptimizationLevel::O1, &optimized);
  expect(generated.find("using string_view = std::string_view;") !=
                 std::string::npos &&
             generated.find("char8_t{71}") != std::string::npos &&
             generated.find("std::string_view{\"GTI\\000text\", 8}") !=
                 std::string::npos &&
             generated.find(
                 "::gti_internal::backend::mir_checked_string_view_at_v1") !=
                 std::string::npos &&
             generated.find("static_cast<std::uint8_t>(") !=
                 std::string::npos &&
             generated.find("program-initialization-v1 module-instance 0") !=
                 std::string::npos &&
             generated.find("::__gti_program::literal_chars_match = ") !=
                 std::string::npos &&
             generated.find("::__gti_program::__gti_program_initialize(") !=
                 std::string::npos,
         "the C++ backend should preserve counted text and exact code units");
}

// From testStandardString.
void testCppStandardStringSpelling() {
  const std::filesystem::path entry =
      gti_test::temporaryDirectory() / "gti-standard-string/main.gti";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry, R"(
import std.string;

int main() {
  std::string_view literal = "engine";
  mut std::string value = std::string(literal);
  value.push_back(' ');
  value.append("runtime");
  mut std::string copy = value.clone();
  std::string signed_text =
      std::to_string(int64_t(-9223372036854775807 - 1));
  std::string unsigned_text =
      std::to_string(uint64_t(18446744073709551615));
  bool cloned = value == copy;
  copy[0] = 'E';
  char first = copy.at(0);
  copy.clear();
  if (literal.size() == 6 and !literal.empty() and literal[0] == 'e' and
      cloned and value == "engine runtime" and first == 'E' and copy.empty() and
      signed_text == "-9223372036854775808" and
      unsigned_text == "18446744073709551615") {
    return 0;
  }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::string generated = gti_test::emitCppText(valid);
  bool checkedMutableCharacters = false;
  for (const auto &function : valid.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        checkedMutableCharacters |=
            instruction.intrinsic == lang::IntrinsicKind::RawBorrowMut &&
            instruction.info.type == lang::SemanticType::Char &&
            instruction.borrowAccess == lang::AccessMode::Mutable &&
            instruction.loan.has_value();
      }
    }
  }
  expect(generated.find("class string") != std::string::npos &&
             generated.find("class terminated_prefix_owner") !=
                 std::string::npos &&
             generated.find("prefix_owner<char8_t>") != std::string::npos &&
             generated.find("string(const string &other) = delete;") !=
                 std::string::npos &&
             generated.find("string(string &&) = default;") !=
                 std::string::npos &&
             checkedMutableCharacters &&
             generated.find("native_pointer_value(") != std::string::npos &&
             generated.find("::gti_internal::backend::prefix_storage<") ==
                 std::string::npos,
         "std::string lowering should retain nominal move-only lifecycle and "
         "checked mutable storage access");
}

// From testStandardString (formatting).
void testCppFormattingSpelling() {
  const std::filesystem::path entry =
      gti_test::temporaryDirectory() / "gti-standard-string/main.gti";
  const lang::FrontendResult formattedOutput = lang::Frontend().analyze(
      entry, R"(
import std.format;
import std.string;
int main() {
  int8_t left = -8;
  uint64_t right = 9;
  int count = 3;
  mut auto formatted =
      std::try_runtime_format("left={} right={} count={}", left, right, count);
  if (!formatted) { return 1; }
  if (formatted.value() != "left=-8 right=9 count=3") { return 2; }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::string formattedGenerated = gti_test::emitCppText(formattedOutput);
  expect(formattedGenerated.find("std::format(") == std::string::npos &&
             formattedGenerated.find("std::vformat") == std::string::npos &&
             formattedGenerated.find("count_format_argument") !=
                 std::string::npos &&
             formattedGenerated.find("append_selected_format_argument") !=
                 std::string::npos,
         "formatting should lower as ordinary GTI library calls rather than a "
         "native C++ formatting shortcut");
}

// From testLogicalOperatorSpellings.
void testCppLogicalOperatorSpelling() {
  const std::string validSource = R"(
bool choose(bool left, bool right, bool fallback) {
  return left && right || fallback;
}

int main() {
  bool words = true and true or false;
  if (choose(words, true, false) && !false || false) {
    return 0;
  }
  return 1;
}
)";
  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("&&") != std::string::npos &&
             generated.find("||") != std::string::npos,
         "both logical spellings should lower to short-circuit C++ operators");
}

// From testIntegerBitwiseAndModuloOperators.
void testCppBitwiseHelperSpelling() {
  const std::string validSource = R"(
int combine(int left, int right) {
  return ((left & right) | (left ^ right)) % 17;
}

int shift_small(uint8_t value) { return (value << 3) >> 1; }
int64_t mix_widths(int64_t left, uint32_t right) { return left & right; }
uint64_t unsigned_bits(uint64_t left, uint64_t right) { return left | right; }

int main() {
  int8_t small = 3;
  int promoted = ~small;
  int flags = ((5 & 3) | 8) ^ 2;
  int shifted = (flags << 2) >> 1;
  int remainder = combine(shifted, 5);
  int wrapped = 1 << 31;
  if (promoted == -4 and remainder == 6 and wrapped == -2147483648) {
    return 0;
  }
  return 1;
}
)";
  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("::gti_internal::backend::mir_checked_remainder_v1") !=
                 std::string::npos &&
             generated.find(
                 "::gti_internal::backend::mir_checked_shift_left_v1") !=
                 std::string::npos &&
             generated.find(
                 "::gti_internal::backend::mir_checked_shift_right_v1") !=
                 std::string::npos,
         "modulo and shifts should lower through checked integer helpers");
  expect(generated.find(" & __gti_mir_v_") != std::string::npos &&
             generated.find(" ^ __gti_mir_v_") != std::string::npos &&
             generated.find(" | __gti_mir_v_") != std::string::npos,
         "ordinary bitwise operators should lower directly");
  expect(generated.find("modulo by zero") != std::string::npos &&
             generated.find("std::bit_cast") != std::string::npos,
         "generated helpers should define invalid modulo and shift behavior");
}

// From testCheckedArithmeticAndCompoundAssignments.
void testCppCheckedArithmeticSpelling() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("checked-arithmetic.gti", R"(
struct Meter {
  mut int value = 1;
public:
  Meter() = default;
};

int calculate(int left, int right) {
  return ((left + right) * (left - right)) / right;
}

int folded_arithmetic() { return ((40 + 2) * 3 - 42) / 2; }
bool checked_addition() { return 2147483647 + 1 == 0; }
bool checked_unsigned_subtraction() {
  return 1 - 18446744073709551615 == 0;
}
bool checked_negation() { return -(-9223372036854775808) == 0; }

int main() {
  mut int value = 6;
  value *= 7;
  value /= 6;
  value %= 5;
  value &= 3;
  value |= 8;
  value ^= 1;
  value <<= 2;
  value >>= 1;
  value += 2;
  value -= 2;

  mut int values[2] = {3, 4};
  mut int index = 0;
  values[index++] *= 2;
  mut Meter meter{};
  meter.value += 2;
  int negative = -1;
  int arithmetic = calculate(7, 2);
  if (value == 22 and index == 1 and values[0] == 6 and
      meter.value == 3 and negative == -1 and arithmetic == 22) {
    return 0;
  }
  return 1;
}
)");
  const std::string generated = gti_test::emitCppText(valid);
  const std::vector<std::string_view> checkedHelpers = {
      "backend::mir_checked_add_v1<",
      "backend::mir_checked_subtract_v1<",
      "backend::mir_checked_multiply_v1<",
      "backend::mir_checked_divide_v1<",
      "backend::mir_checked_remainder_v1<",
      "backend::mir_checked_shift_left_v1<",
      "backend::mir_checked_shift_right_v1<",
      "backend::mir_checked_negate_v1<",
  };
  expect(std::all_of(checkedHelpers.begin(), checkedHelpers.end(),
                     [&](std::string_view helper) {
                       return generated.find(helper) != std::string::npos;
                     }) &&
             generated.find(" & __gti_mir_v_") != std::string::npos &&
             generated.find(" | __gti_mir_v_") != std::string::npos &&
             generated.find(" ^ __gti_mir_v_") != std::string::npos,
         "arithmetic and mutation should lower through checked GTI helpers");
  expect(generated.find("integer addition overflow") != std::string::npos &&
             generated.find("integer subtraction overflow") !=
                 std::string::npos &&
             generated.find("integer multiplication overflow") !=
                 std::string::npos &&
             generated.find("integer division overflow") != std::string::npos &&
             generated.find("division by zero") != std::string::npos,
         "generated arithmetic helpers should carry stable failure reasons");
}

// From testConstexprBindings.
void testCppConstexprBindingSpelling() {
  const std::string source = R"(
constexpr uint64_t factorial(mut uint64_t value) {
  mut uint64_t result = 1;
  while (value > 1) {
    result *= value;
    value--;
  }
  return result;
}

constexpr uint64_t fibonacci(uint64_t value) {
  if (value < uint64_t(2)) {
    return value;
  }
  return fibonacci(value - 1) + fibonacci(value - 2);
}

constexpr int32_t classify(int32_t value) {
  switch (value) {
  case 0:
    return 10;
  case 1:
    return 20;
  default:
    return 30;
  }
}

enum class BuildMode : uint8_t {
  debug = 1,
  release = 2,
};

class ConstantBox<T> {
  T value;

public:
  constexpr ConstantBox(T initial) : value(initial) {}
  constexpr T get() { return this.value; }
};

constexpr T preserve<T>(T value) { return value; }

constexpr uint64_t extent = uint64_t(2 + 2);
constexpr int32_t base = 6 * 7;
constexpr bool selected = base == 42 and true;
constexpr uint64_t factorial_value = factorial(uint64_t(5));
constexpr uint64_t fibonacci_value = fibonacci(uint64_t(10));
constexpr int32_t raw_values[3] = {2, 4, 6};
constexpr ConstantBox<int32_t> signed_box = ConstantBox<int32_t>(7);
constexpr ConstantBox<uint64_t> wide_box =
    ConstantBox<uint64_t>(uint64_t(9));
constexpr BuildMode selected_mode = BuildMode::release;
constexpr int32_t preserved = preserve<int32_t>(11);

static_assert(raw_values[1] == 4);
static_assert(signed_box.get() == 7);
static_assert(wide_box.get() == uint64_t(9));
static_assert(selected_mode == BuildMode::release);
static_assert(preserved == 11);

namespace values {
constexpr char marker = 'G';
}

class Limits {
public:
  static constexpr uint64_t count = extent;
  static constexpr uint64_t twice(uint64_t value) { return value * 2; }
public:
  Limits() = default;
};

constexpr uint64_t doubled = Limits::twice(uint64_t(21));

class Block<T, uint64_t N> {
public:
  T values[N] = {};
public:
  Block() = default;
};

int main() {
  constexpr auto local = selected ? base : 0;
  int32_t data[extent] = {1, 2, 3, 4};
  Block<int32_t, extent> block{};
  if constexpr (factorial_value == uint64_t(120) and
                fibonacci_value == uint64_t(55) and
                doubled == uint64_t(42)) {
    if (local == 42 and Limits::count == 4 and data[3] == 4 and
        values::marker == 'G' and raw_values[2] == 6 and
        signed_box.get() == 7 and wide_box.get() == uint64_t(9) and
        selected_mode == BuildMode::release and preserved == 11) {
      return classify(0) - 10;
    }
    return 1;
  } else {
    return inactive_branch_is_not_analyzed;
  }
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("constexpr.gti", source);
  const std::string generated = gti_test::emitCppText(frontend);
  expect(
      generated.find(
          "constexpr std::uint64_t extent = static_cast<std::uint64_t>(4)") !=
              std::string::npos &&
          generated.find("static constexpr std::uint64_t count = "
                         "static_cast<std::uint64_t>(4)") !=
              std::string::npos &&
          generated.find("constexpr std::uint64_t factorial_value = "
                         "static_cast<std::uint64_t>(120)") !=
              std::string::npos &&
          generated.find("// GTI verified-MIR body:") != std::string::npos &&
          generated.find("inactive_branch_is_not_analyzed") ==
              std::string::npos,
      "the C++ backend should emit computed constexpr values and keep "
      "static constexpr fields inline while erasing inactive branches");
}

// From testStaticAssertions.
void testCppStaticAssertionErasure() {
  const std::string validSource = R"(
#define ASSERTIONS_ENABLED

constexpr uint64_t expected_width = uint64_t(4);
static_assert(true);

namespace checks {
static_assert(expected_width == uint64_t(4),
              "must never reach native C++");
}

struct Packet {
  static_assert(sizeof(int32_t) == uint64_t(4), "packet field width");
  int32_t value = 0;
public:
  Packet() = default;
};

interface Contract {
  static_assert(true);
};

union Choice {
  static_assert(true);
  int32_t value;
};

int main() {
  constexpr bool ready = true;
  static_assert(ready, "local assertion");
#ifdef ASSERTIONS_ENABLED
  static_assert(true);
#else
  static_assert(false, "inactive configuration assertion");
#endif
  if constexpr (false) {
    static_assert(false, "discarded constexpr assertion");
  } else {
    static_assert(true);
  }
  return 0;
}
)";
  const lang::FrontendResult valid =
      lang::Frontend().analyze("static-assertions.gti", validSource);
  const std::string generated = gti_test::emitCppText(valid);
  expect(generated.find("must never reach native C++") == std::string::npos &&
             generated.find("discarded constexpr assertion") ==
                 std::string::npos,
         "source static assertions and their messages must not be emitted to "
         "the C++ backend");
}

// From testStaticStorageAndMembers.
void testCppStaticStorageSpelling() {
  const std::string source = R"(
static int file_value = 7;
static int add_file_value(int value) { return value + file_value; }

namespace detail {
static int namespace_value = 5;
static int read_namespace_value() { return namespace_value; }
}

class Counter {
public:
  static int answer = 42;
  static mut int count = 0;
  static int current() { return count; }
  int value = 3;
public:
  Counter() = default;
};

class Empty {
public:
  static Empty value = Empty();
public:
  Empty() = default;
};

class Managed {
public:
  static mut int live = 0;
  ~Managed() {}
public:
  Managed() = default;
};

int main() {
  Counter::count = add_file_value(2);
  int result = Counter::current() + Counter::answer +
               detail::read_namespace_value() + detail::namespace_value;
  if (result == 61) { return 0; }
  return 1;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("static-members.gti", source);
  const std::string generated = gti_test::emitCppText(frontend);
  expect(
      generated.find("static const std::int32_t answer;") !=
              std::string::npos &&
          generated.find("inline const std::int32_t Counter::answer = "
                         "static_cast<std::int32_t>(42)") !=
              std::string::npos &&
          generated.find("static std::int32_t count;") != std::string::npos &&
          generated.find("inline std::int32_t Counter::count = "
                         "static_cast<std::int32_t>(0)") != std::string::npos &&
          generated.find("inline const ::__gti_program::Empty Empty::value = "
                         "::__gti_program::Empty{}") != std::string::npos &&
          generated.find("inline std::int32_t Managed::live = "
                         "static_cast<std::int32_t>(0)") != std::string::npos &&
          generated.find("live(std::move(other.live))") == std::string::npos &&
          generated.find("static std::int32_t __gti_fn_") !=
              std::string::npos &&
          generated.find("detail::__gti_static_") != std::string::npos &&
          generated.find("__gti_static_") != std::string::npos,
      "the C++ backend should emit inline class statics and uniquely named "
      "qualified internal-linkage declarations");
}

// From testCompileTimeConditionals (configuration).
void testCppConfigurationSelectionSpelling() {
  const std::string selectionSource = R"(
#define LOCAL
#define LOCAL
#if target.os == "never"
#define LEAKED
#define ALTERNATE
#define EXTERNAL
#endif

#if defined(EXTERNAL) &&
    (target.os != "never" || defined(ALTERNATE))
int selected_value() { return 11; }
#else
int selected_value() { return 22; }
#endif

#undef EXTERNAL
#ifdef EXTERNAL
#error "a source undef did not remove an externally seeded flag"
#endif

#ifdef LEAKED
#error "an inactive definition leaked"
#endif

#undef LOCAL
#ifndef LOCAL
int local_state() { return 7; }
#else
int local_state() { return 9; }
#endif

int main() { return selected_value() + local_state(); }
)";
  lang::FrontendOptions configuredOptions;
  configuredOptions.configurationFlags = {"EXTERNAL"};
  const lang::FrontendResult configured =
      lang::Frontend(configuredOptions)
          .analyze("configuration-selection.gti", selectionSource);
  const std::string configuredCpp = gti_test::emitCppText(configured);
  expect(configuredCpp.find("static_cast<std::int32_t>(11)") !=
                 std::string::npos &&
             configuredCpp.find("static_cast<std::int32_t>(7)") !=
                 std::string::npos &&
             configuredCpp.find("static_cast<std::int32_t>(22)") ==
                 std::string::npos &&
             configuredCpp.find("inactive definition leaked") ==
                 std::string::npos &&
             configuredCpp.find("#define LOCAL") == std::string::npos &&
             configuredCpp.find("#define EXTERNAL") == std::string::npos,
         "only selected configuration branches should reach the backend");
  const lang::FrontendResult unconfigured =
      lang::Frontend().analyze("configuration-selection.gti", selectionSource);
  const std::string unconfiguredCpp = gti_test::emitCppText(unconfigured);
  expect(unconfiguredCpp.find("static_cast<std::int32_t>(22)") !=
                 std::string::npos &&
             unconfiguredCpp.find("static_cast<std::int32_t>(11)") ==
                 std::string::npos,
         "external define sets should deterministically change selection");
}

// From testCompileTimeConditionals (targets).
void testCppTargetSelectionSpelling() {
  const std::string source = R"(
#if target.vendor == "apple"
int platform_value() { return 101; }
#if target.arch == "arm64"
int nested_value() { return 64; }
#else
int nested_value() { return 32; }
#endif
#elif target.os == "windows"
int platform_value() { return 202; }
#else
int platform_value() { return 303; }
#endif

#if target.os == "never"
expected<int, int> inactive_error() { return missing_name; }
@runtime("stdout.write")
void inactive_runtime(gti_internal::text_view value);
#error "This target is deliberately unsupported."
#endif

class PlatformInfo {
#if target.arch == "arm64"
  int bits = 64;
#else
  int bits = 32;
#endif
public:
  int selected_bits() { return this.bits; }
public:
  PlatformInfo() = default;
};

int main() {
  PlatformInfo info{};
#if target.os != "windows"
  int value = platform_value();
#else
  int value = platform_value();
#endif
  return value + info.selected_bits();
}
)";
  const lang::TargetInfo apple{"macos", "apple", "arm64"};
  const std::string appleCpp = emitAnalyzedSource(source, apple);
  const auto appleProgramStart = appleCpp.find("namespace __gti_program {");
  expect(
      appleCpp.find("static_cast<std::int32_t>(101)") != std::string::npos &&
          appleCpp.find("static_cast<std::int32_t>(64)") != std::string::npos &&
          appleCpp.find("std::int32_t bits;") != std::string::npos &&
          appleCpp.find("PlatformInfo::PlatformInfo()") != std::string::npos &&
          appleCpp.find("missing_name") == std::string::npos &&
          appleCpp.find("#include <expected>") == std::string::npos &&
          appleCpp.find("#include <gti/runtime.hpp>") == std::string::npos &&
          appleProgramStart != std::string::npos &&
          appleCpp.find("#if", appleProgramStart) == std::string::npos,
      "Apple lowering should emit only active branches without C++ macros");
  const lang::TargetInfo windows{"windows", "pc", "x86_64"};
  const std::string windowsCpp = emitAnalyzedSource(source, windows);
  expect(windowsCpp.find("static_cast<std::int32_t>(202)") !=
                 std::string::npos &&
             windowsCpp.find("std::int32_t bits;") != std::string::npos &&
             windowsCpp.find("PlatformInfo::PlatformInfo()") !=
                 std::string::npos &&
             windowsCpp.find("static_cast<std::int32_t>(32)") !=
                 std::string::npos &&
             windowsCpp.find("nested_value") == std::string::npos &&
             windowsCpp.find("static_cast<std::int32_t>(101)") ==
                 std::string::npos,
         "target selection should distinguish vendor, OS, and architecture");
  const std::string errorSource = R"(
#if target.os == "blocked"
#error "This target is not supported."
#endif

class PlatformState {
#if target.os == "blocked"
#error "PlatformState is unavailable on this target."
#endif
  int value = 1;
public:
  PlatformState() = default;
};

int main() {
#if target.os == "blocked"
#error "No entry point is available for this target."
#endif
  return 0;
}
)";
  const lang::TargetInfo allowed{"macos", "apple", "arm64"};
  const std::string allowedCpp = emitAnalyzedSource(errorSource, allowed);
  const auto allowedProgramStart = allowedCpp.find("namespace __gti_program {");
  // Backend-private ABI guards are independent of folded source directives.
  expect(allowedProgramStart != std::string::npos &&
             allowedCpp.find("#error", allowedProgramStart) ==
                 std::string::npos,
         "source #error directives should never leak into program output");
}

// @@MORE_CASES@@

constexpr NamedTest kTests[] = {
    {"testCppBackendConsumesOptimizedMir", testCppBackendConsumesOptimizedMir},
    {"testCppProgramArgumentsEntryAdapter",
     testCppProgramArgumentsEntryAdapter},
    {"testCppCompletePipelineText", testCppCompletePipelineText},
    {"testCppDefaultImmutabilityText", testCppDefaultImmutabilityText},
    {"testCppDefaultNodiscardText", testCppDefaultNodiscardText},
    {"testCppPrintIsAnIdentifierText", testCppPrintIsAnIdentifierText},
    {"testCppSignatureAliasSpelling", testCppSignatureAliasSpelling},
    {"testCppQualifiedSignatureSpelling", testCppQualifiedSignatureSpelling},
    {"testCppStandardArraySpelling", testCppStandardArraySpelling},
    {"testCppTcpSpelling", testCppTcpSpelling},
    {"testCppLoopControlText", testCppLoopControlText},
    {"testCppDoWhileText", testCppDoWhileText},
    {"testCppConditionalOwnerText", testCppConditionalOwnerText},
    {"testCppSwitchText", testCppSwitchText},
    {"testCppFixedWidthIntegerSpelling", testCppFixedWidthIntegerSpelling},
    {"testCppContextualSignedLiteralSpelling",
     testCppContextualSignedLiteralSpelling},
    {"testCppBinaryFloatSpelling", testCppBinaryFloatSpelling},
    {"testCppCharacterTextSpelling", testCppCharacterTextSpelling},
    {"testCppStandardStringSpelling", testCppStandardStringSpelling},
    {"testCppFormattingSpelling", testCppFormattingSpelling},
    {"testCppLogicalOperatorSpelling", testCppLogicalOperatorSpelling},
    {"testCppBitwiseHelperSpelling", testCppBitwiseHelperSpelling},
    {"testCppCheckedArithmeticSpelling", testCppCheckedArithmeticSpelling},
    {"testCppConstexprBindingSpelling", testCppConstexprBindingSpelling},
    {"testCppStaticAssertionErasure", testCppStaticAssertionErasure},
    {"testCppStaticStorageSpelling", testCppStaticStorageSpelling},
    {"testCppConfigurationSelectionSpelling",
     testCppConfigurationSelectionSpelling},
    {"testCppTargetSelectionSpelling", testCppTargetSelectionSpelling},
    // @@MORE_REGISTRATIONS@@
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "cpp_mir_compiler_emission");
}
