#include "compiler_test_fixture.h"

#include "gti/mir_effects.h"

namespace {

[[nodiscard]] bool isOptionalIntrinsic(lang::IntrinsicKind intrinsic) {
  return intrinsic == lang::IntrinsicKind::OptionalStorageHasValue ||
         intrinsic == lang::IntrinsicKind::OptionalStorageConstruct ||
         intrinsic == lang::IntrinsicKind::OptionalStorageRead ||
         intrinsic == lang::IntrinsicKind::OptionalStorageReadMut ||
         intrinsic == lang::IntrinsicKind::OptionalStorageReset;
}

[[nodiscard]] std::size_t
optionalIntrinsicIndex(lang::IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case lang::IntrinsicKind::OptionalStorageHasValue:
    return 0;
  case lang::IntrinsicKind::OptionalStorageConstruct:
    return 1;
  case lang::IntrinsicKind::OptionalStorageRead:
    return 2;
  case lang::IntrinsicKind::OptionalStorageReadMut:
    return 3;
  case lang::IntrinsicKind::OptionalStorageReset:
    return 4;
  default:
    return 5;
  }
}

void testOptionalCompilerContract() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "optional-compiler-contract.gti", R"(
#include <std/optional>

class payload {
  mut int32_t stored;

public:
  payload(int32_t value) : stored(value) {}
  payload(payload& other) = delete;
  payload(payload&& other) = default;
  ~payload() {}
  void set(int32_t value) mut { this.stored = value; }
  int32_t get() { return this.stored; }
};

int main() {
  std::optional<payload> constructed{payload(int32_t(3))};
  if (constructed.value().get() != int32_t(3)) {
    return 2;
  }
  mut std::optional<payload> value{};
  [[discard]] value.emplace(int32_t(4));
  value.value().set(int32_t(5));
  if (!value.has_value() || value.value().get() != int32_t(5)) {
    return 1;
  }
  mut std::optional<payload> moved{std::move(value)};
  moved.reset();
  std::optional<int32_t> fixed{int32_t(7)};
  return fixed.value() - int32_t(7);
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "the installed optional surface should pass semantic analysis");
  if (!frontend.canGenerateCode()) {
    return;
  }

  std::array<bool, 5> hirIntrinsics{};
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (isOptionalIntrinsic(value.intrinsic)) {
        hirIntrinsics[optionalIntrinsicIndex(value.intrinsic)] = true;
      }
    }
  }

  std::array<bool, 5> mirIntrinsics{};
  const lang::MirClassInstance *optionalStorage = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.compilerCapability ==
        lang::CompilerCapabilityTypeKind::OptionalStorage) {
      optionalStorage = &instance;
      break;
    }
  }
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (isOptionalIntrinsic(instruction.intrinsic)) {
          mirIntrinsics[optionalIntrinsicIndex(instruction.intrinsic)] = true;
        }
      }
    }
  }
  expect(std::all_of(hirIntrinsics.begin(), hirIntrinsics.end(),
                     [](bool present) { return present; }) &&
             std::all_of(mirIntrinsics.begin(), mirIntrinsics.end(),
                         [](bool present) { return present; }) &&
             optionalStorage != nullptr &&
             optionalStorage->copyConstructor ==
                 lang::SpecialMemberStatus::Deleted &&
             optionalStorage->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             optionalStorage->copyAssignment ==
                 lang::SpecialMemberStatus::Deleted &&
             optionalStorage->moveAssignment ==
                 lang::SpecialMemberStatus::Deleted &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "all five declaration-bound operations and the move-only inline-slot "
         "shape should survive HIR, MIR, and whole-program verification");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  const std::size_t optionalBegin =
      artifact.contents.find("template <typename T>\nclass optional_storage {");
  const std::size_t optionalEnd =
      artifact.contents.find("// Prefix-initialized storage", optionalBegin);
  const std::string optionalRepresentation =
      optionalBegin == std::string::npos || optionalEnd == std::string::npos
          ? std::string{}
          : artifact.contents.substr(optionalBegin,
                                     optionalEnd - optionalBegin);
  expect(!artifact.contents.empty() && !optionalRepresentation.empty() &&
             optionalRepresentation.find(
                 "alignas(T) std::byte storage_[sizeof(T)]") !=
                 std::string::npos &&
             optionalRepresentation.find("bool engaged_ = false") !=
                 std::string::npos &&
             optionalRepresentation.find("std::construct_at") !=
                 std::string::npos &&
             optionalRepresentation.find("std::destroy_at") !=
                 std::string::npos &&
             optionalRepresentation.find("operator new") == std::string::npos &&
             optionalRepresentation.find("unique_ptr") == std::string::npos &&
             artifact.contents.find("mir_optional_construct_v1(") !=
                 std::string::npos &&
             artifact.contents.find("mir_optional_read_v1(") !=
                 std::string::npos &&
             artifact.contents.find("mir_optional_read_mut_v1(") !=
                 std::string::npos,
         "the C++ backend should select inline checked storage without "
         "smuggling optional state through allocation or std::optional");

  lang::MirProgram forged = frontend.mir;
  bool forgedRead = false;
  auto &forgedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forged.functionInstances());
  for (lang::MirFunctionInstance &function : forgedFunctions) {
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (!forgedRead &&
            instruction.intrinsic == lang::IntrinsicKind::OptionalStorageRead) {
          instruction.intrinsic = lang::IntrinsicKind::OptionalStorageHasValue;
          forgedRead = true;
        }
      }
    }
  }
  const lang::MirVerificationResult forgedVerification =
      lang::verifyMirProgram(forged);
  expect(forgedRead && !forgedVerification.valid(),
         "whole-program verification should reject an optional intrinsic "
         "whose result, loan, and failure contract were relabelled");

  const auto verifyForgedConstructLifecycle = [&](bool substituteTransfer) {
    lang::MirProgram changed = frontend.mir;
    bool changedLifecycle = false;
    for (const lang::MirBodyAddress address :
         lang::enumerateMirBodyAddresses(changed)) {
      lang::MirBody *body = lang::findMirBody(changed, address);
      if (body == nullptr) {
        continue;
      }
      for (lang::MirBlock &block : body->blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.intrinsic !=
                  lang::IntrinsicKind::OptionalStorageConstruct ||
              instruction.lifecycle.empty()) {
            continue;
          }
          if (!substituteTransfer) {
            instruction.lifecycle.clear();
            changedLifecycle = true;
            break;
          }
          const auto transfer = std::find_if(
              instruction.lifecycle.begin(), instruction.lifecycle.end(),
              [](const lang::MirLifecycleEvent &event) {
                return event.kind == lang::MirLifecycleEventKind::TransferOut;
              });
          if (transfer == instruction.lifecycle.end()) {
            continue;
          }
          const auto replacement = std::find_if(
              body->dropObligations.begin(), body->dropObligations.end(),
              [&](const lang::MirDropObligation &candidate) {
                return candidate.id != transfer->source;
              });
          if (replacement == body->dropObligations.end()) {
            continue;
          }
          transfer->source = replacement->id;
          changedLifecycle = true;
          break;
        }
        if (changedLifecycle) {
          break;
        }
      }
      if (changedLifecycle) {
        break;
      }
    }
    return std::pair{changedLifecycle, lang::verifyMirProgram(changed)};
  };

  const auto missingConstructTransfer = verifyForgedConstructLifecycle(false);
  expect(
      missingConstructTransfer.first &&
          !missingConstructTransfer.second.valid() &&
          hasMirVerificationError(missingConstructTransfer.second,
                                  "optional storage intrinsic does not retain"),
      "whole-program verification should reject optional construction "
      "whose owning input transfer was removed");

  const auto substitutedConstructTransfer =
      verifyForgedConstructLifecycle(true);
  expect(
      substitutedConstructTransfer.first &&
          !substitutedConstructTransfer.second.valid() &&
          hasMirVerificationError(substitutedConstructTransfer.second,
                                  "optional storage intrinsic does not retain"),
      "whole-program verification should reject optional construction "
      "whose owning input transfer names another live obligation");

  for (int mutation = 0; mutation != 3; ++mutation) {
    auto malformed = frontend.mir;
    bool changed = false;
    for (const auto address : lang::enumerateMirBodyAddresses(malformed)) {
      auto *body = lang::findMirBody(malformed, address);
      if (body == nullptr || changed)
        continue;
      for (auto &block : body->blocks) {
        for (auto &instruction : block.instructions) {
          if (changed || instruction.intrinsic !=
                             lang::IntrinsicKind::OptionalStorageConstruct)
            continue;
          expect(instruction.operands.size() == 2 &&
                     !instruction.constructorTarget,
                 "optional publication accepts one completed value, not a "
                 "hidden constructor");
          if (mutation == 0) {
            instruction.operands.pop_back();
            instruction.parameterTypes.pop_back();
          } else if (mutation == 1) {
            instruction.operands[1].type = lang::SemanticType::Bool;
            instruction.parameterTypes[1] = lang::SemanticType::Bool;
          } else {
            instruction.constructorTarget =
                frontend.mir.constructorInstances().front().id;
          }
          changed = true;
        }
      }
    }
    expect(changed && !lang::verifyMirProgram(malformed).valid(),
           "optional publication rejects missing/wrong payloads and hidden "
           "constructor targets");
  }

  const auto invalidEmplacement = lang::Frontend().analyze(
      "optional-invalid-payload-construction.gti", R"(
#include <std/optional>
struct Payload {
  int value;
  Payload(int value) : value(value) {}
};
int main() {
  mut std::optional<Payload> slot{};
  [[discard]] slot.emplace("invalid");
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidEmplacement.canGenerateCode() &&
             hasDiagnosticCode(invalidEmplacement.diagnostics, "GTI-S2012") &&
             hasDiagnostic(invalidEmplacement.diagnostics,
                           "No constructor of 'Payload' exactly matches") &&
             !hasDiagnosticCode(invalidEmplacement.diagnostics, "GTI-B0001"),
         "invalid emplacement must use ordinary source constructor diagnostics "
         "rather than reaching the native backend");

  const lang::FrontendResult copied = lang::Frontend().analyze(
      "optional-copy-rejected.gti", R"(
#include <std/optional>
int main() {
  std::optional<int32_t> source{int32_t(1)};
  std::optional<int32_t> copied = source;
  return copied.value();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!copied.canGenerateCode() &&
             hasDiagnostic(copied.diagnostics, "Cannot initialize 'copied'") &&
             hasDiagnosticCode(copied.diagnostics, "GTI-S2003"),
         "optional copies should fail closed before lowering");

  const lang::FrontendResult reusedAfterMove = lang::Frontend().analyze(
      "optional-source-after-move-rejected.gti", R"(
#include <std/optional>
int main() {
  std::optional<int32_t> source{int32_t(1)};
  std::optional<int32_t> destination{std::move(source)};
  return source.has_value();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!reusedAfterMove.canGenerateCode() &&
             hasDiagnosticCode(reusedAfterMove.diagnostics, "GTI-S2018") &&
             hasDiagnostic(reusedAfterMove.diagnostics,
                           "Value 'source' has already been moved"),
         "an optional source should become unavailable after move "
         "construction");

  const lang::FrontendResult aliased = lang::Frontend().analyze(
      "optional-live-borrow-rejected.gti", R"(
#include <std/optional>
int main() {
  mut std::optional<int32_t> value{int32_t(7)};
  mut int32_t& borrowed = value.value();
  value.reset();
  return borrowed;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !aliased.canGenerateCode() &&
          hasDiagnostic(aliased.diagnostics,
                        "while a reference borrowed from it may still be live"),
      "reset should reject while an element reference remains live");

  const lang::FrontendResult forgedSpelling =
      lang::Frontend().analyze("ordinary-optional-storage.gti", R"(
namespace ordinary {
class optional_storage<T> {
public:
  optional_storage() {}
};
}
int main() {
  ordinary::optional_storage<int32_t> value{};
  return 0;
}
)");
  bool ordinaryOnly = forgedSpelling.canGenerateCode();
  for (const lang::MirClassInstance &instance :
       forgedSpelling.mir.classInstances()) {
    ordinaryOnly = ordinaryOnly && instance.compilerCapability ==
                                       lang::CompilerCapabilityTypeKind::None;
  }
  expect(ordinaryOnly,
         "an ordinary class with the optional-storage spelling must not mint "
         "compiler capability identity");
}

void testOptionalResetCleanup() {
  const auto frontend = lang::Frontend().analyze(
      "optional-reset-cleanup.gti", R"(
#include <std/optional>
class Resource {
  int divisor;
public:
  Resource(int divisor) : divisor(divisor) {}
  ~Resource() { int checked = 12 / this.divisor; }
};
void destroy(int divisor) {
  mut std::optional<Resource> slot{Resource(divisor)};
  slot.reset();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "fallible payload reset must have independently verified MIR");
  if (!frontend.canGenerateCode())
    return;
  bool found = false;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = lang::findMirBody(frontend.mir, address);
    for (const auto &block : body->blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::OptionalStorageReset)
          continue;
        found = true;
        expect(instruction.elementCleanup &&
                   instruction.definedFailure.propagation ==
                       lang::FailurePropagationKind::Destructor &&
                   instruction.localFailureSites.empty() &&
                   instruction.definedFailure.localOrigins.empty() &&
                   block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "reset invokes exact payload cleanup without inventing a new "
               "failure identity");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *changedBody = lang::findMirBody(forged, address);
          auto &changedBlock = changedBody->blocks[block.id - 1];
          for (auto &changed : changedBlock.instructions) {
            if (changed.id != instruction.id)
              continue;
            if (mutation == 0)
              changed.elementCleanup.reset();
            else if (mutation == 1)
              changed.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
            else {
              changedBlock.terminator.kind = lang::MirTerminatorKind::Goto;
              changedBlock.terminator.invokeInstruction = 0;
              changedBlock.terminator.elseTarget = 0;
              changedBlock.terminator.failureRecord = 0;
            }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "reset cannot erase payload cleanup, failure propagation, "
                 "or the caller cleanup edge");
        }
      }
    }
  }
  expect(found, "the source reset must reach the trusted storage capability");
  const auto artifact = gti_test::emitCpp(frontend);
  expect(artifact.contents.find(".reset_with_failure(") != std::string::npos,
         "the backend must consume the verified element cleanup recipe");
}

void testNestedOptionalConstruction() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_nested.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = lang::Frontend().analyze(path.string(), source,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &instance : frontend.mir.classInstances()) {
      const auto derived =
          lang::mirTypeRelocationKind(frontend.mir, instance.type);
      if (derived != instance.traits.relocation)
        std::cerr << "relocation mismatch: class " << instance.id << " "
                  << "declaration=" << instance.declaration
                  << " semantic=" << int(instance.traits.relocation)
                  << " mir=" << int(derived)
                  << " move=" << int(instance.moveConstructor) << '\n';
    }
  }
  expect(input.good() && frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "nested construction-only owners retain exact lifecycle and "
         "relocation metadata without requiring assignment");
}

} // namespace

int main() {
  testOptionalCompilerContract();
  testOptionalResetCleanup();
  testNestedOptionalConstruction();
  if (failures != 0) {
    std::cerr << failures << " optional compiler test(s) failed\n";
    return 1;
  }
  std::cout << "optional compiler tests passed\n";
  return 0;
}
