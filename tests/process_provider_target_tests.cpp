#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/process_memory_runtime.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Operation = lang::ProcessMemoryRuntimeOperation;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const std::filesystem::path root =
    std::filesystem::path(__FILE__).parent_path().parent_path();

lang::FrontendResult analyze(const lang::FrontendOptions &options,
                             const std::filesystem::path &path,
                             std::optional<std::string> source,
                             const std::filesystem::path &stdlib) {
  auto result = lang::Frontend(options).analyze(
      path, std::move(source), {stdlib / "prelude.gti"}, {}, {stdlib});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  return result;
}

lang::FrontendOptions targetOptions(std::string_view os) {
  lang::FrontendOptions options;
  options.target.os = std::string(os);
  options.target.vendor = os == "macos" ? "apple" : "pc";
  options.target.arch = "x86_64";
  return options;
}

const lang::FunctionInfo *info(const lang::FrontendResult &result,
                               const lang::MirFunctionInstance &function) {
  return result.semantics.findFunction(function.declaration);
}

lang::MirFunctionInstance *mutableFunction(lang::MirProgram &program,
                                           std::string_view symbol) {
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      program.functionInstances());
  const auto found = std::find_if(
      functions.begin(), functions.end(),
      [&](const auto &function) { return function.externalSymbol == symbol; });
  return found == functions.end() ? nullptr : &*found;
}

bool rejectsRuntimeContract(const lang::MirProgram &program) {
  const auto verification = lang::verifyMirProgram(program);
  return std::any_of(verification.errors.begin(), verification.errors.end(),
                     [](const auto &error) {
                       return error.message ==
                              "process-memory runtime declaration contract is "
                              "invalid";
                     });
}

void testTable() {
  const auto address =
      lang::SemanticType::rawPointerTo(lang::SemanticType::Void);
  const auto observed = lang::SemanticType::rawPointerTo(
      lang::SemanticType::Void, lang::AccessMode::ReadOnly);
  const auto status =
      lang::SemanticType::rawPointerTo(lang::SemanticType::Int32);
  const std::vector<lang::SemanticType> memalign{
      lang::SemanticType::UInt64, lang::SemanticType::UInt64, status};
  expect(lang::exactProcessMemoryRuntimeOperation("gti_rt_posix_memalign_v1",
                                                  address, memalign) ==
             Operation::PosixAllocate,
         "exact POSIX allocation signature classifies");
  expect(lang::exactProcessMemoryRuntimeOperation(
             "gti_rt_posix_memalign_v1", address,
             std::vector<lang::SemanticType>{lang::SemanticType::UInt64,
                                             lang::SemanticType::UInt64}) ==
                 Operation::None &&
             lang::exactProcessMemoryRuntimeOperation(
                 "gti_rt_posix_memalign_v1", lang::SemanticType::UInt64,
                 memalign) == Operation::None &&
             lang::exactProcessMemoryRuntimeOperation(
                 "posix_memalign", address, memalign) == Operation::None,
         "a wrong arity, return type or symbol does not classify");
  expect(lang::exactProcessMemoryRuntimeOperation(
             "gti_rt_address_is_aligned_v1", lang::SemanticType::Int32,
             std::vector<lang::SemanticType>{observed,
                                             lang::SemanticType::UInt64}) ==
                 Operation::AddressIsAligned &&
             lang::exactProcessMemoryRuntimeOperation(
                 "gti_rt_address_is_aligned_v1", lang::SemanticType::Int32,
                 std::vector<lang::SemanticType>{
                     address, lang::SemanticType::UInt64}) == Operation::None &&
             lang::exactProcessMemoryRuntimeOperation(
                 "gti_rt_posix_free_v1", lang::SemanticType::Void,
                 std::vector<lang::SemanticType>{observed}) == Operation::None,
         "pointer access is part of the exact signature");
  for (auto value = static_cast<unsigned>(Operation::None) + 1;
       value < static_cast<unsigned>(Operation::Count); ++value) {
    const auto operation = static_cast<Operation>(value);
    expect(!lang::processMemoryRuntimeSymbol(operation).empty(),
           "every operation names its exact runtime symbol");
    expect(lang::processMemoryRuntimeSelectedForTarget(operation, "windows") !=
                   lang::processMemoryRuntimeSelectedForTarget(operation,
                                                               "linux") ||
               operation == Operation::NativeSizeMax ||
               operation == Operation::AddressIsAligned,
           "only the common queries exist in both target families");
  }
  expect(lang::processMemoryRuntimeSymbol(Operation::None).empty() &&
             !lang::processMemoryRuntimeSelectedForTarget(Operation::None,
                                                          "linux"),
         "None names no runtime declaration");
}

void testTarget(std::string_view os) {
  const auto options = targetOptions(os);
  const auto result =
      analyze(options, root / "tests/fixtures/process_resource_policy.gti",
              std::nullopt, root / "stdlib");
  expect(result.canGenerateCode(), "process provider fixture analyzes");
  if (!result.canGenerateCode()) {
    return;
  }
  const auto text =
      gti_test::emitCppText(result, lang::CppStandard::Cpp23, options.target);
  const bool windows = os == "windows";
  const auto selected =
      windows ? "gti_rt_aligned_malloc_v1" : "gti_rt_posix_memalign_v1";
  const auto absent =
      windows ? "gti_rt_posix_memalign_v1" : "gti_rt_aligned_malloc_v1";
  expect(text.find(selected) != std::string::npos &&
             text.find(absent) == std::string::npos &&
             text.find("gti_rt_memory_allocate_v1") == std::string::npos,
         "target selects its source provider");

  // Semantics, HIR and MIR agree on the exact trusted declarations.
  std::set<Operation> expected{Operation::NativeSizeMax,
                               Operation::AddressIsAligned};
  if (windows) {
    expected.insert({Operation::AlignedAllocate, Operation::AlignedRelease});
  } else {
    expected.insert({Operation::PosixPointerSize,
                     Operation::PosixOutOfMemoryStatus,
                     Operation::PosixInvalidArgumentStatus,
                     Operation::PosixAllocate, Operation::PosixRelease});
  }
  std::set<Operation> classified;
  const auto &functions = result.mir.functionInstances();
  for (std::size_t index = 0; index < functions.size(); ++index) {
    const auto &function = functions[index];
    const auto *declaration = info(result, function);
    const auto &hir = result.hir.functionInstances()[index];
    expect(declaration != nullptr &&
               declaration->processMemoryOperation ==
                   function.processMemoryOperation &&
               hir.processMemoryOperation == function.processMemoryOperation,
           "semantic, HIR and MIR classifications agree");
    if (function.processMemoryOperation == Operation::None) {
      expect(!function.externalSymbol.starts_with("gti_rt_posix_") &&
                 !function.externalSymbol.starts_with("gti_rt_aligned_") &&
                 function.externalSymbol != "gti_rt_native_size_max_v1" &&
                 function.externalSymbol != "gti_rt_address_is_aligned_v1",
             "every trusted allocator declaration classifies");
      continue;
    }
    classified.insert(function.processMemoryOperation);
    expect(function.externalSymbol == lang::processMemoryRuntimeSymbol(
                                          function.processMemoryOperation) &&
               function.definitionKind ==
                   lang::MirDefinitionKind::Declaration &&
               declaration != nullptr && declaration->compilerPrivate,
           "classification names its exact private declaration");
    const auto &summary = function.effectSummary;
    const bool allocation =
        function.processMemoryOperation == Operation::PosixAllocate ||
        function.processMemoryOperation == Operation::AlignedAllocate;
    const bool release =
        function.processMemoryOperation == Operation::PosixRelease ||
        function.processMemoryOperation == Operation::AlignedRelease;
    expect(!summary.traits.maySynchronize && !summary.traits.invokesUserCode &&
               !summary.traits.mayTrap && !summary.traits.readsUnknownMemory &&
               !summary.traits.readsPlace && !summary.traits.writesPlace &&
               !summary.receiverEscapes &&
               std::none_of(summary.argumentsEscape.begin(),
                            summary.argumentsEscape.end(),
                            [](bool escapes) { return escapes; }) &&
               summary.argumentsEscape.size() == function.parameterTypes.size(),
           "declared runtime contract is a local, non-synchronizing summary");
    expect(summary.traits.allocates == allocation &&
               summary.traits.writesUnknownMemory ==
                   (release || function.processMemoryOperation ==
                                   Operation::PosixAllocate) &&
               !summary.traits.removableWhenUnused &&
               !summary.traits.reorderable && !summary.traits.speculatable,
           "allocation, status-write and release effects are retained");
  }
  expect(classified == expected,
         "exactly the target-selected runtime family classifies");
  expect(lang::verifyHirProgramPlans(result.semantics, result.hir).valid() &&
             lang::verifyMirProgram(result.mir).valid(),
         "classified HIR and MIR verify");
  const auto dump = lang::MirPrinter().print(result.mir);
  expect(dump.starts_with("mir-v98 ") &&
             dump.find(windows
                           ? "process-memory-operation=8"
                           : "process-memory-operation=6") != std::string::npos,
         "MIR serializes the runtime classification");

  // The provider's source bodies no longer inherit an unknown boundary from
  // the runtime declarations. Failure-only panic paths may still synchronize.
  for (const auto &function : functions) {
    const auto *declaration = info(result, function);
    if (declaration == nullptr ||
        (declaration->qualifiedName !=
             "gti_internal::process_resource::allocate" &&
         declaration->qualifiedName !=
             "gti_internal::process_resource::release")) {
      continue;
    }
    const auto &summary = function.effectSummary;
    // Only the POSIX status pointee and release write memory; the Windows
    // aligned allocation is a pure allocation.
    const bool release =
        declaration->qualifiedName == "gti_internal::process_resource::release";
    expect(!summary.traits.maySynchronize && summary.traits.invokesRuntime &&
               summary.traits.writesUnknownMemory == (release || !windows) &&
               summary.traits.allocates == !release,
           "process_resource summaries are precise above the runtime");
    expect(std::none_of(summary.argumentsEscape.begin(),
                        summary.argumentsEscape.end(),
                        [](bool escapes) { return escapes; }),
           "process_resource arguments do not escape through the runtime");
  }

  // Detached lowered metadata retains the private identity.
  const auto lowered =
      gti_test::lowerProgram(result, result.mir, result.mir, options.target);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "detached lowered contract verifies");
  auto forgedLowered = lowered;
  for (auto &row : const_cast<std::vector<lang::LoweredDeclaration> &>(
           forgedLowered.declarations())) {
    auto *function =
        std::get_if<lang::LoweredFunctionDeclaration>(&row.payload);
    if (function && function->processMemoryOperation != Operation::None) {
      function->compilerPrivate = false;
    }
  }
  expect(!lang::verifyLoweredProgram(forgedLowered).empty(),
         "runtime contracts cannot lose private declaration identity");

  auto forgedHir = result.hir;
  for (auto &function : const_cast<std::vector<lang::HirFunctionInstance> &>(
           forgedHir.functionInstances())) {
    if (function.externalSymbol == "gti_rt_write_stdout") {
      function.processMemoryOperation = Operation::NativeSizeMax;
    }
  }
  expect(!lang::verifyHirProgramPlans(result.semantics, forgedHir).valid(),
         "HIR rejects a classification its semantic declaration lacks");

  // Forged MIR classifications fail closed.
  const std::string allocator =
      windows ? "gti_rt_aligned_malloc_v1" : "gti_rt_posix_memalign_v1";
  for (int mutation = 0; mutation < 7; ++mutation) {
    auto changed = result.mir;
    auto *function = mutableFunction(changed, allocator);
    auto *unrelated = mutableFunction(changed, "gti_rt_write_stdout");
    if (function == nullptr || unrelated == nullptr) {
      expect(false, "fixture retains the allocator and an unrelated native");
      break;
    }
    switch (mutation) {
    case 0:
      function->externalSymbol = "malloc";
      break;
    case 1:
      function->parameterTypes.pop_back();
      break;
    case 2:
      function->linkage = lang::LanguageLinkage::Gti;
      break;
    case 3:
      function->definitionKind = lang::MirDefinitionKind::RuntimeBinding;
      break;
    case 4:
      function->processMemoryOperation =
          windows ? Operation::PosixAllocate : Operation::AlignedAllocate;
      break;
    case 5:
      unrelated->processMemoryOperation = Operation::NativeSizeMax;
      unrelated->effectSummary.traits = {};
      unrelated->effectSummary.argumentsEscape.assign(
          unrelated->parameterTypes.size(), false);
      break;
    case 6:
      const_cast<lang::TargetInfo &>(changed.target()).os =
          windows ? "linux" : "windows";
      break;
    }
    expect(rejectsRuntimeContract(changed),
           "forged symbol, signature, linkage, definition, operation, "
           "declaration or target family is rejected");
  }
  auto erased = result.mir;
  if (auto *function = mutableFunction(erased, allocator)) {
    function->processMemoryOperation = Operation::None;
  }
  expect(!lang::verifyMirProgram(erased).valid(),
         "an erased classification cannot keep its precise summary");
}

void testUntrustedDeclarations() {
  const lang::FrontendOptions options;
  const auto application = analyze(options, "application.gti", R"(
extern "C" {
uint64_t gti_rt_native_size_max_v1();
}
int main() { return gti_rt_native_size_max_v1() == uint64_t(0) ? 1 : 0; }
)",
                                   root / "stdlib");
  expect(application.canGenerateCode(),
         "an application may declare the runtime symbol itself");
  for (const auto &function : application.mir.functionInstances()) {
    if (function.externalSymbol == "gti_rt_native_size_max_v1") {
      expect(function.processMemoryOperation == Operation::None &&
                 function.effectSummary.traits.maySynchronize &&
                 function.effectSummary.traits.writesUnknownMemory,
             "an application declaration stays an unknown native boundary");
    }
  }

  // A trusted unit grants the contract only to exact private declarations.
  const auto copy =
      std::filesystem::temp_directory_path() /
      ("gti-process-memory-runtime-" + std::to_string(std::random_device{}()));
  std::filesystem::remove_all(copy);
  std::filesystem::create_directories(copy);
  std::filesystem::copy(root / "stdlib", copy / "stdlib",
                        std::filesystem::copy_options::recursive);
  const auto memory = copy / "stdlib/std/memory.gti";
  std::string text;
  {
    std::ifstream input(memory);
    text.assign(std::istreambuf_iterator<char>(input), {});
  }
  const auto replace = [&](std::string_view from, std::string_view to) {
    const auto position = text.find(from);
    expect(position != std::string::npos, "patched stdlib text exists");
    if (position != std::string::npos) {
      text.replace(position, from.size(), to);
    }
  };
  // Wrong pointer access on release; still callable from its source body.
  replace("void gti_rt_posix_free_v1(void* address);",
          "void gti_rt_posix_free_v1(const void* address);");
  // Not compiler-private: the same declaration outside gti_internal.
  replace("uint64_t gti_rt_posix_pointer_size_v1();", "");
  replace("namespace gti_internal {\n// Source policy seal",
          "namespace std::detail::process_probe {\nextern \"C\" {\nuint64_t "
          "gti_rt_posix_pointer_size_v1();\n}\n}\nnamespace gti_internal {\n"
          "// Source policy seal");
  replace("gti_internal::runtime::gti_rt_posix_pointer_size_v1()",
          "std::detail::process_probe::gti_rt_posix_pointer_size_v1()");
  {
    std::ofstream output(memory, std::ios::trunc);
    output << text;
  }
  const auto patched =
      analyze(targetOptions("linux"),
              root / "tests/fixtures/process_resource_policy.gti", std::nullopt,
              copy / "stdlib");
  expect(patched.canGenerateCode(), "patched trusted unit still analyzes");
  bool sawRelease = false;
  bool sawQuery = false;
  bool sawAllocation = false;
  for (const auto &function : patched.mir.functionInstances()) {
    if (function.externalSymbol == "gti_rt_posix_free_v1") {
      sawRelease = true;
      expect(function.processMemoryOperation == Operation::None,
             "a wrong signature in the trusted unit does not classify");
    } else if (function.externalSymbol == "gti_rt_posix_pointer_size_v1") {
      sawQuery = true;
      expect(function.processMemoryOperation == Operation::None,
             "a non-private declaration in the trusted unit does not classify");
    } else if (function.externalSymbol == "gti_rt_posix_memalign_v1") {
      sawAllocation = true;
      expect(function.processMemoryOperation == Operation::PosixAllocate,
             "unchanged exact declarations still classify");
    }
  }
  expect(!patched.canGenerateCode() ||
             (sawRelease && sawQuery && sawAllocation &&
              lang::verifyMirProgram(patched.mir).valid()),
         "patched declarations remain ordinary verified natives");
  std::filesystem::remove_all(copy);
}

} // namespace

int main() {
  testTable();
  for (const auto *os : {"macos", "linux", "windows"}) {
    testTarget(os);
  }
  testUntrustedDeclarations();
  return failures == 0 ? 0 : 1;
}
