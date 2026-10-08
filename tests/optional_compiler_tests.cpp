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
             optionalRepresentation.find("construct_representation_at") !=
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

  for (int mutation = 0; mutation != 4; ++mutation) {
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
          } else if (mutation == 2) {
            instruction.constructorTarget =
                frontend.mir.constructorInstances().front().id;
          } else {
            const auto payloadValue = instruction.operands[1].value;
            for (auto &producerBlock : body->blocks)
              for (auto &producer : producerBlock.instructions)
                if (producer.result == payloadValue)
                  producer.info.traits.containsBorrowedState = true;
          }
          changed = true;
        }
      }
    }
    expect(changed && !lang::verifyMirProgram(malformed).valid(),
           "optional publication rejects missing/wrong payloads and hidden "
           "constructor targets, and owner-dependent payload expression state");
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

void testStoragePayloadAdmission() {
  const std::string prefix = R"(
#include <std/optional>
#include <std/vector>
class View {
  int& source;
public:
  View(int& value) : source(value) {}
};
)";
  for (const std::string declaration :
       {"std::optional<View> item{};",
        "std::optional<View> item{View(source)};",
        "std::vector<View> item{};"}) {
    const std::string source =
        prefix + "int main() { int source = 7; " + declaration + " return 0; }";
    const auto frontend = lang::Frontend().analyze(
        "borrowed-storage-admission.gti", source, {standardLibraryPrelude()},
        {}, {standardLibraryRoot()});
    expect(!frontend.canGenerateCode() &&
               hasDiagnosticCode(frontend.diagnostics, "GTI-S2019") &&
               hasDiagnostic(frontend.diagnostics, "borrowed state") &&
               !hasDiagnosticCode(frontend.diagnostics, "GTI-B0001"),
           "trusted owning storage rejects a reference-carrying element even "
           "when no payload accessor or mutation is used");
    if (declaration.find("optional") == std::string::npos)
      continue;
    expect(countDiagnosticCode(frontend.diagnostics, "GTI-S2019") == 1,
           "class-owned optional payload validation is published once, not "
           "again for the constructor's field-model reanalysis");
    bool preciseDiagnostic = false;
    for (const auto &diagnostic : frontend.diagnostics) {
      if (diagnostic.code != "GTI-S2019" ||
          diagnostic.message.find("payload 'View'") == std::string::npos)
        continue;
      const auto userRequest = [&](const lang::SourceSpan &span) {
        return std::filesystem::path(span.source).filename() ==
                   "borrowed-storage-admission.gti" &&
               span.start == source.find("item") && span.end == span.start + 4;
      };
      preciseDiagnostic =
          diagnostic.phase == lang::DiagnosticPhase::Semantics &&
          diagnostic.severity == lang::DiagnosticSeverity::Error &&
          diagnostic.fixes.empty() && !diagnostic.hints.empty() &&
          (userRequest(diagnostic.primary) ||
           std::any_of(
               diagnostic.related.begin(), diagnostic.related.end(),
               [&](const auto &related) { return userRequest(related.span); }));
    }
    expect(preciseDiagnostic,
           "optional rejection names the concrete payload and exact user "
           "instantiation without suggesting an ownership-changing fix-it");
  }

  const std::string constructorSource = R"(
class Broken<uint64_t N> {
  static_assert(N > 2, "class constraint");
public:
  Broken() {
    static_assert(N > 0, "first constructor constraint");
    static_assert(N > 1, "second constructor constraint");
  }
};
int main() {
  Broken<0> first = Broken<0>();
  return 0;
}
)";
  const auto constructorDiagnostics = lang::Frontend().analyze(
      "class-and-constructor-diagnostics.gti", constructorSource);
  expect(!constructorDiagnostics.canGenerateCode() &&
             countDiagnosticCode(constructorDiagnostics.diagnostics,
                                 "GTI-S2079") == 3,
         "class-owned diagnostics publish once without suppressing distinct "
         "concrete constructor-body errors");
  for (const std::string_view condition : {"N > 2", "N > 0", "N > 1"}) {
    const auto start = constructorSource.find(condition);
    const auto request = condition == "N > 2"
                             ? constructorSource.find("first =")
                             : constructorSource.find("Broken<0>()");
    expect(std::count_if(
               constructorDiagnostics.diagnostics.begin(),
               constructorDiagnostics.diagnostics.end(),
               [&](const lang::Diagnostic &diagnostic) {
                 return diagnostic.code == "GTI-S2079" &&
                        diagnostic.primary.start == start &&
                        diagnostic.primary.end == start + condition.size() &&
                        diagnostic.related.size() == 2 &&
                        diagnostic.related.front().message ==
                            "Concrete substitution: 'N' = '0'." &&
                        diagnostic.related.back().span.start == request &&
                        diagnostic.related.back().span.end ==
                            request + (condition == "N > 2" ? 5 : 6) &&
                        diagnostic.related.back().message ==
                            "Concrete generic instance requested here.";
               }) == 1,
           "class and constructor diagnostics retain their distinct primary "
           "and instantiation sites with concrete substitution notes");
  }

  std::string multipleInstances = constructorSource;
  multipleInstances.insert(multipleInstances.find("  return 0;"),
                           "  Broken<1> second = Broken<1>();\n");
  const auto distinctInstances = lang::Frontend().analyze(
      "distinct-instance-diagnostics.gti", multipleInstances);
  const auto *entry = distinctInstances.sourceGraph.findUnit(
      distinctInstances.sourceGraph.entryUnit());
  expect(!distinctInstances.canGenerateCode() && entry != nullptr &&
             countDiagnosticCode(distinctInstances.diagnostics, "GTI-S2079") ==
                 5,
         "class-owned publication does not merge errors from distinct "
         "concrete instances that share the same generic source span");
  for (const int value : {0, 1}) {
    for (const std::string_view condition : {"N > 2", "N > 0", "N > 1"}) {
      const bool fails = value == 0 || condition != "N > 0";
      const auto start = multipleInstances.find(condition);
      const std::string construction =
          "Broken<" + std::to_string(value) + ">()";
      const auto request =
          condition == "N > 2"
              ? multipleInstances.find(value == 0 ? "first =" : "second =")
              : multipleInstances.find(construction);
      const auto matching = std::count_if(
          distinctInstances.diagnostics.begin(),
          distinctInstances.diagnostics.end(),
          [&](const lang::Diagnostic &diagnostic) {
            return diagnostic.code == "GTI-S2079" && entry != nullptr &&
                   diagnostic.primary.source == entry->path.string() &&
                   diagnostic.primary.start == start &&
                   diagnostic.primary.end == start + condition.size() &&
                   diagnostic.related.size() == 2 &&
                   diagnostic.related.front().message ==
                       "Concrete substitution: 'N' = '" +
                           std::to_string(value) + "'." &&
                   diagnostic.related.back().span.source ==
                       entry->path.string() &&
                   diagnostic.related.back().span.start == request &&
                   diagnostic.related.back().span.end ==
                       request +
                           (condition == "N > 2" ? (value == 0 ? 5 : 6) : 6) &&
                   diagnostic.related.back().message ==
                       "Concrete generic instance requested here.";
          });
      expect(matching == (fails ? 1 : 0),
             "each failing concrete substitution retains its own diagnostic "
             "and requesting location without repeating class diagnostics");
    }
  }

  for (const std::string type : {"int*", "int&", "void"}) {
    const auto frontend = lang::Frontend().analyze(
        "optional-unsupported-payload.gti",
        "#include <std/optional>\nint main() { std::optional<" + type +
            "> item{}; return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!frontend.canGenerateCode() &&
               hasDiagnosticCode(frontend.diagnostics,
                                 type == "void" ? "GTI-S2029" : "GTI-S2019") &&
               !hasDiagnosticCode(frontend.diagnostics, "GTI-B0001"),
           "unsupported optional payload shapes must fail at source instead "
           "of reaching the MIR/backend contract");
  }

  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_borrowed_escape_rejected.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto escaped = lang::Frontend().analyze(path.string(), source,
                                                {standardLibraryPrelude()}, {},
                                                {standardLibraryRoot()});
  expect(input.good() && !escaped.canGenerateCode() &&
             hasDiagnosticCode(escaped.diagnostics, "GTI-S2019") &&
             hasDiagnostic(escaped.diagnostics, "payload 'View'") &&
             !hasDiagnosticCode(escaped.diagnostics, "GTI-B0001"),
         "an unrelated generic wrapper and destructor cannot hide a borrowed "
         "optional payload escaping its local owner");

  const auto admitted = lang::Frontend().analyze(
      "optional-admitted-payloads.gti", R"(
#include <std/optional>
#include <std/string>
int main() {
  std::optional<std::string_view> text{};
  std::optional<int[2]> array{};
  std::optional<expected<int, int>> result{};
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(admitted.canGenerateCode() &&
             lang::verifyMirProgram(admitted.mir).valid(),
         "optional admission preserves static text views, fixed arrays, and "
         "expected payloads rather than borrowing sparse-storage exclusions");

  for (const std::string argument : {"owner.view()", "\"static\""}) {
    const auto text = lang::Frontend().analyze(
        "optional-text-parameter-storage.gti",
        "#include <std/optional>\n#include <std/string>\n"
        "std::optional<std::string_view> escape() { "
        "std::string owner{\"owned\"}; "
        "return std::optional<std::string_view>(" +
            argument + "); }\nint main() { auto result = escape(); return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!text.canGenerateCode() &&
               hasDiagnosticCode(text.diagnostics, "GTI-S2045") &&
               hasDiagnostic(text.diagnostics, "nested in optional storage") &&
               !hasDiagnosticCode(text.diagnostics, "GTI-B0001"),
           "optional publication cannot store a potentially owner-dependent "
           "text parameter; literal arguments do not bypass that generic "
           "parameter boundary");
  }

  for (const std::string storage :
       {"Holder value{.text = std::move(view)};",
        "std::string_view values[1] = {std::move(view)};",
        "mut std::vector<std::string_view> values{}; "
        "values.push_back(std::move(view));"}) {
    const auto moved = lang::Frontend().analyze(
        "moved-text-storage-rejected.gti",
        "#include <std/string>\n#include <std/vector>\n"
        "struct Holder { std::string_view text; };\n"
        "int main() { std::string owner{\"owned\"}; "
        "std::string_view view = owner.view(); " +
            storage + " return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!moved.canGenerateCode() &&
               hasDiagnosticCode(moved.diagnostics, "GTI-S2045") &&
               !hasDiagnosticCode(moved.diagnostics, "GTI-B0001"),
           "moving owner-dependent text does not erase its loan at a field, "
           "fixed-array, or dynamic-storage boundary");
  }

  const auto localText = lang::Frontend().analyze(
      "moved-text-local-loan.gti", R"(
#include <std/string>
int main() {
  std::string owner{"owned"};
  std::string_view view = owner.view();
  std::string_view moved = std::move(view);
  return moved.size() == std::size_t(5) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(localText.canGenerateCode() &&
             lang::verifyMirProgram(localText.mir).valid(),
         "moving a text borrow into a local binding retains the original "
         "owner loan and remains valid within that owner's lifetime");
  const auto ownerMutation = lang::Frontend().analyze(
      "moved-text-owner-mutation.gti", R"(
#include <std/string>
int main() {
  mut std::string owner{"owned"};
  std::string_view view = owner.view();
  std::string_view moved = std::move(view);
  owner.clear();
  return moved.size() == std::size_t(5) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!ownerMutation.canGenerateCode() &&
             hasDiagnosticCode(ownerMutation.diagnostics, "GTI-S2017"),
         "a moved local text borrow still prevents mutation of its original "
         "owner rather than merely retaining a borrowed-state flag");
  const auto cText = lang::Frontend().analyze(
      "optional-c-string-escape.gti", R"(
#include <std/optional>
#include <std/string>
std::optional<c_string> escape() {
  std::string owner{"owned"};
  return std::optional<c_string>(owner.c_string());
}
int main() { auto result = escape(); return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!cText.canGenerateCode() &&
             hasDiagnosticCode(cText.diagnostics, "GTI-S2045") &&
             hasDiagnostic(cText.diagnostics,
                           "owner-dependent c_string cannot be nested") &&
             !hasDiagnosticCode(cText.diagnostics, "GTI-B0001"),
         "optional storage applies the same dependency rule to C-string "
         "views without relying on a public library spelling");
}

void testStoragePayloadMirAdmission() {
  const auto frontend = lang::Frontend().analyze(
      "storage-payload-mir-admission.gti", R"(
#include <std/optional>
#include <std/vector>
struct Payload { int value = 7; };
int main() {
  std::optional<Payload> optional{};
  std::vector<Payload> prefix{};
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "the trusted-storage mutation fixture starts as valid MIR");
  if (!frontend.canGenerateCode())
    return;
  const auto storage = std::find_if(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(), [](const auto &instance) {
        return instance.compilerCapability ==
               lang::CompilerCapabilityTypeKind::OptionalStorage;
      });
  expect(storage != frontend.mir.classInstances().end() &&
             storage->type.arguments.size() == 1,
         "the fixture has an exact optional payload identity");
  if (storage == frontend.mir.classInstances().end() ||
      storage->type.arguments.size() != 1)
    return;
  const auto payload = storage->type.arguments.front();
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto forged = frontend.mir;
    bool changed = false;
    for (auto &instance : const_cast<std::vector<lang::MirClassInstance> &>(
             forged.classInstances())) {
      if (mutation == 0 && instance.id == storage->id) {
        instance.traits.containsBorrowedState = true;
        changed = true;
      } else if (mutation != 0 && instance.type == payload) {
        if (mutation == 1)
          instance.traits.movable = false;
        else if (mutation == 2)
          instance.traits.containsBorrowedState = true;
        else if (!instance.declaredFields.empty()) {
          instance.traits.containsBorrowedState = false;
          instance.declaredFields.front().type =
              lang::SemanticType::referenceTo(lang::SemanticType::Int32);
        } else {
          continue;
        }
        changed = true;
      }
    }
    const auto verification = lang::verifyMirProgram(forged);
    expect(changed && !verification.valid() &&
               hasMirVerificationError(
                   verification,
                   "optional storage capability does not retain its exact"),
           "independent verification rejects a borrowed slot, nonmoving "
           "payload, and borrowed payload even when its class flag is erased");
    if (mutation >= 2)
      expect(hasMirVerificationError(
                 verification, "storage field payload retains borrowed state"),
             "prefix storage independently checks the same concrete payload "
             "instead of trusting its owning handle or an allocation call");
  }
}

void testValueOrContract() {
  const std::string source = R"(
#include <std/optional>
struct Resource {
  Resource() {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
};
int main() {
  std::optional<Resource> value{};
  auto result = value.value_or(Resource());
  return 0;
}
)";
  const auto invalid = lang::Frontend().analyze("optional-value-or.gti", source,
                                                {standardLibraryPrelude()}, {},
                                                {standardLibraryRoot()});
  const auto *diagnostic =
      findDiagnosticByCode(invalid.diagnostics, "GTI-S2029");
  const auto call = source.find("));") + 1;
  expect(!invalid.canGenerateCode() && diagnostic != nullptr &&
             diagnostic->primary.source.ends_with("optional-value-or.gti") &&
             diagnostic->primary.start == call &&
             diagnostic->primary.end == call + 1 &&
             diagnostic->message.find("std::copy_constructible") !=
                 std::string::npos &&
             diagnostic->fixes.empty() &&
             hasRelatedDiagnostic(
                 invalid.diagnostics,
                 "Required application 'std::copy_constructible<Resource>'") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "copy assignment is not required"),
         "value_or diagnoses its source API contract at the call, with the "
         "copy-construction requirement and no misleading fix");

  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_value_or_contract.gti";
  std::ifstream input(path);
  expect(input.good(), "optional API contract fixture exists");
  const std::string fixture{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
  const auto valid = lang::Frontend().analyze(
      "optional-value-or-valid.gti", fixture, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "value_or permits a copyable user payload and leaves "
         "move-only optional's other members usable");
  bool recordChecked = false;
  for (const auto &instance : valid.mir.classInstances()) {
    const auto hir = std::find_if(
        valid.hir.classInstances().begin(), valid.hir.classInstances().end(),
        [&](const auto &item) { return item.id == instance.id; });
    if (hir != valid.hir.classInstances().end() && hir->source &&
        hir->source->name().lexeme == "Record") {
      recordChecked = true;
      expect(instance.copyConstructor == lang::SpecialMemberStatus::Generated,
             "the positive payload retains generated copy construction");
    }
  }
  expect(recordChecked, "the positive record is retained in concrete MIR");
}

} // namespace

int main() {
  testOptionalCompilerContract();
  testValueOrContract();
  testOptionalResetCleanup();
  testNestedOptionalConstruction();
  testStoragePayloadAdmission();
  testStoragePayloadMirAdmission();
  if (failures != 0) {
    std::cerr << failures << " optional compiler test(s) failed\n";
    return 1;
  }
  std::cout << "optional compiler tests passed\n";
  return 0;
}
