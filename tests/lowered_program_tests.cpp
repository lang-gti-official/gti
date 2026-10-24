#include "gti/frontend.h"
#include "gti/lowered_program.h"
#include "gti/lowered_program_builder.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "lowered_program_contract_client.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(std::is_same_v<decltype(lang::MirCAbiRecordFieldLayout::field),
                             lang::SymbolId>);
static_assert(
    std::is_same_v<decltype(lang::MirUnionFieldLayout::field), lang::SymbolId>);

namespace lang {

struct LoweredProgramTestAccess {
  static std::vector<LoweredBody> &bodies(LoweredProgram &program) {
    return program.bodies_;
  }

  static std::vector<LoweredDeclaration> &
  declarations(LoweredProgram &program) {
    return program.declarations_;
  }

  static std::vector<LoweredGeneratedItem> &
  generatedItems(LoweredProgram &program) {
    return program.generatedItems_;
  }

  static std::vector<LoweredSymbol> &symbols(LoweredProgram &program) {
    return program.symbols_;
  }

  static std::vector<LoweredFunctionInstance> &
  functionInstances(LoweredProgram &program) {
    return program.functionInstances_;
  }

  static std::vector<LoweredConstructorInstance> &
  constructorInstances(LoweredProgram &program) {
    return program.constructorInstances_;
  }

  static std::vector<LoweredLambdaInstance> &
  lambdaInstances(LoweredProgram &program) {
    return program.lambdaInstances_;
  }

  static MirProgram &mir(LoweredProgram &program) { return program.mir_; }
};

} // namespace lang

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[nodiscard]] bool
hasIssue(const std::vector<lang::LoweredProgramIssue> &issues,
         lang::LoweredProgramIssueKind kind) {
  return std::any_of(issues.begin(), issues.end(),
                     [kind](const lang::LoweredProgramIssue &issue) {
                       return issue.kind == kind;
                     });
}

[[nodiscard]] std::size_t generatedCount(const lang::LoweredProgram &program,
                                         lang::LoweredGeneratedItemKind kind) {
  return static_cast<std::size_t>(std::count_if(
      program.generatedItems().begin(), program.generatedItems().end(),
      [kind](const lang::LoweredGeneratedItem &item) {
        return item.identity.kind == kind;
      }));
}

template <typename Payload>
[[nodiscard]] const Payload *findPayload(const lang::LoweredProgram &program,
                                         std::string_view name = {}) {
  for (const lang::LoweredDeclaration &declaration : program.declarations()) {
    if (!name.empty() && declaration.name != name) {
      continue;
    }
    if (const auto *payload = std::get_if<Payload>(&declaration.payload)) {
      return payload;
    }
  }
  return nullptr;
}

[[nodiscard]] std::optional<lang::LoweredProgram>
buildDetachedProgram(std::string_view sourceName = "lowered-program.gti",
                     bool debugInfo = false) {
  lang::FrontendResult frontend = lang::Frontend().analyze(sourceName, R"(
using Unary = (int32_t) -> int32_t;

enum class Mode : int32_t { active = 1, idle = 2 };

[[c_abi]] struct NativePair {
  int32_t left;
  int32_t right;
};

union Bits {
  int32_t signed_value;
  uint32_t unsigned_value;
};

class Counter {
public:
  mut int32_t value = 0;

  int32_t read() { return this.value; }
};

class Cursor {
  mut int32_t current = 0;
public:
  int32_t& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(Cursor& other) { return this.current != other.current; }
};

class Increment {
public:
  int32_t operator()(int32_t value) { return value + 1; }
};

class Cleanup {
  mut int32_t value = 1;
public:
  ~Cleanup() { this.value = 0; }
};

class GenericConstruction {
public:
  GenericConstruction<Left, Right>(Left left, Right right) {}
};

namespace demo {
class Marker<T> {
public:
  int32_t read() { return 1; }
};
}

class demo::Marker<int32_t> {
public:
  int32_t read() { return 2; }
};

mut int32_t process_seed = 1;

extern "C" {
  Unary install_callback(Unary callback);
}

int32_t add_one(int32_t value) { return value + 1; }

T identity<T>(T value) { return value; }

int32_t apply_offset(int32_t value) {
  int32_t offset = 1;
  auto add_offset = [offset](int32_t input) -> int32_t {
    return input + offset;
  };
  return add_offset(value);
}

int main() {
  unsafe {
    [[discard]] install_callback(add_one);
  }
  Counter counter = Counter();
  Cleanup cleanup = Cleanup();
  GenericConstruction generic = GenericConstruction(1, true);
  demo::Marker<int32_t> marker{};
  return identity<int32_t>(counter.read()) + process_seed + apply_offset(0) +
         marker.read() - 4;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return std::nullopt;
  }

  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  if (!optimized.valid()) {
    return std::nullopt;
  }
  lang::LoweredProgramBuild build = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations, lang::TargetInfo::host(),
      debugInfo);
  if (!build.valid()) {
    for (const lang::LoweredProgramIssue &issue : build.issues) {
      std::cerr << "lowered-program issue: " << issue.detail << '\n';
    }
    return std::nullopt;
  }
  return std::move(build.program);
}

void testDetachedDeterministicProgram() {
  std::optional<lang::LoweredProgram> first = buildDetachedProgram();
  std::optional<lang::LoweredProgram> second = buildDetachedProgram();
  expect(first.has_value() && second.has_value(),
         "the builder should accept a rich optimized frontend program");
  if (!first || !second) {
    return;
  }

  const std::string firstText = lang::LoweredProgramPrinter().print(*first);
  const std::string secondText = lang::LoweredProgramPrinter().print(*second);
  expect(firstText == secondText,
         "independent frontend snapshots should lower deterministically");
  expect(firstText.find("lowered-program-v25") == 0 &&
             firstText.find("\nmir\n") != std::string::npos,
         "the deterministic printer should serialize the complete contract");
  const gti_test::LoweredProgramInventory inventory =
      gti_test::inspectLoweredProgram(*first);
  expect(inventory.deterministicText == firstText &&
             inventory.bodies == first->bodies().size() &&
             inventory.declarations == first->declarations().size() &&
             inventory.symbols == first->symbols().size() &&
             inventory.classInstances == first->classInstances().size() &&
             inventory.functionInstances == first->functionInstances().size() &&
             inventory.constructorInstances ==
                 first->constructorInstances().size() &&
             inventory.destructorInstances ==
                 first->destructorInstances().size() &&
             inventory.lambdaInstances == first->lambdaInstances().size() &&
             inventory.generatedItems == first->generatedItems().size() &&
             std::accumulate(inventory.declarationKinds.begin(),
                             inventory.declarationKinds.end(),
                             std::size_t{0}) == inventory.declarations &&
             std::accumulate(inventory.generatedItemKinds.begin(),
                             inventory.generatedItemKinds.end(),
                             std::size_t{0}) == inventory.generatedItems,
         "an independent contract client should enumerate and print the "
         "complete lowered program without frontend representations");
  expect(lang::verifyLoweredProgram(*first).empty(),
         "the detached lowered program should verify after frontend owners "
         "have been destroyed");
  expect(first->bodies().size() ==
             lang::enumerateMirBodyAddresses(first->mir()).size(),
         "the lowered body census should exactly match its owned MIR");
  const bool hasValueLayouts = std::any_of(
      first->mir().classInstances().begin(),
      first->mir().classInstances().end(),
      [](const lang::MirClassInstance &instance) {
        const bool validCAbi =
            instance.cAbiLayout &&
            std::all_of(instance.cAbiLayout->fields.begin(),
                        instance.cAbiLayout->fields.end(),
                        [](const lang::MirCAbiRecordFieldLayout &field) {
                          return field.field != 0;
                        });
        const bool validUnion =
            instance.unionLayout &&
            std::all_of(instance.unionLayout->fields.begin(),
                        instance.unionLayout->fields.end(),
                        [](const lang::MirUnionFieldLayout &field) {
                          return field.field != 0;
                        });
        return validCAbi || validUnion;
      });
  expect(hasValueLayouts,
         "MIR ABI and union layouts should retain symbol identities rather "
         "than AST declaration pointers");
  expect(
      generatedCount(
          *first, lang::LoweredGeneratedItemKind::ProgramInitialization) == 1 &&
          generatedCount(*first, lang::LoweredGeneratedItemKind::HostedEntry) ==
              1 &&
          generatedCount(
              *first, lang::LoweredGeneratedItemKind::NativeInteropAdapter) ==
              1 &&
          generatedCount(
              *first,
              lang::LoweredGeneratedItemKind::StructuralOperatorAdapter) == 3 &&
          generatedCount(
              *first, lang::LoweredGeneratedItemKind::CallableAdapter) == 1 &&
          generatedCount(
              *first, lang::LoweredGeneratedItemKind::LifecycleCleanup) == 1 &&
          generatedCount(
              *first,
              lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter) == 2,
      "the lowered program should own startup, initialization, and native "
      "callback/declaration/lifecycle/concrete-instance adapter contracts "
      "without frontend pointers");
  const bool declarationAdaptersRooted = std::all_of(
      first->generatedItems().begin(), first->generatedItems().end(),
      [&](const lang::LoweredGeneratedItem &item) {
        if (item.identity.kind !=
                lang::LoweredGeneratedItemKind::StructuralOperatorAdapter &&
            item.identity.kind !=
                lang::LoweredGeneratedItemKind::CallableAdapter &&
            item.identity.kind !=
                lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter) {
          return true;
        }
        const lang::LoweredDeclaration *source =
            first->findDeclaration(item.sourceDeclaration);
        return item.sourceKind ==
                   lang::LoweredGeneratedItemSourceKind::Declaration &&
               source != nullptr &&
               std::find(source->requiredGeneratedItems.begin(),
                         source->requiredGeneratedItems.end(),
                         item.identity) != source->requiredGeneratedItems.end();
      });
  expect(declarationAdaptersRooted,
         "structural, callable, and concrete-instance adapters should be "
         "rooted by their exact lowered declarations");
  const auto lifecycle = std::find_if(
      first->generatedItems().begin(), first->generatedItems().end(),
      [](const lang::LoweredGeneratedItem &item) {
        return item.identity.kind ==
               lang::LoweredGeneratedItemKind::LifecycleCleanup;
      });
  const auto *lifecyclePayload =
      lifecycle == first->generatedItems().end()
          ? nullptr
          : std::get_if<lang::LoweredLifecycleCleanupItem>(&lifecycle->payload);
  const lang::LoweredDeclaration *lifecycleSource =
      lifecycle == first->generatedItems().end()
          ? nullptr
          : first->findDeclaration(lifecycle->sourceDeclaration);
  expect(lifecyclePayload != nullptr && lifecycleSource != nullptr &&
             lifecycle->sourceKind ==
                 lang::LoweredGeneratedItemSourceKind::Declaration &&
             std::holds_alternative<lang::LoweredDestructorDeclaration>(
                 lifecycleSource->payload) &&
             lifecyclePayload->destructorInstance ==
                 lifecycle->identity.owner &&
             lifecyclePayload->form ==
                 lang::LoweredLifecycleCleanupForm::OrdinaryClass,
         "lifecycle cleanup should retain one exact destructor-rooted "
         "ordinary-class payload");
  const auto concrete = std::find_if(
      first->generatedItems().begin(), first->generatedItems().end(),
      [&](const lang::LoweredGeneratedItem &item) {
        const auto *payload =
            std::get_if<lang::LoweredConcreteInstanceAdapterItem>(
                &item.payload);
        const lang::LoweredDeclaration *source =
            first->findDeclaration(item.sourceDeclaration);
        return item.identity.kind ==
                   lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter &&
               payload != nullptr &&
               payload->kind ==
                   lang::LoweredConcreteInstanceAdapterKind::Function &&
               source != nullptr && source->name == "identity";
      });
  const auto *concretePayload =
      concrete == first->generatedItems().end()
          ? nullptr
          : std::get_if<lang::LoweredConcreteInstanceAdapterItem>(
                &concrete->payload);
  const lang::LoweredDeclaration *concreteSource =
      concrete == first->generatedItems().end()
          ? nullptr
          : first->findDeclaration(concrete->sourceDeclaration);
  const lang::MirFunctionInstance *concreteMir =
      concretePayload == nullptr
          ? nullptr
          : first->mir().findFunctionInstance(concretePayload->body.owner);
  expect(concretePayload != nullptr && concreteSource != nullptr &&
             concreteMir != nullptr && concreteSource->name == "identity" &&
             concreteSource->generic &&
             concrete->sourceKind ==
                 lang::LoweredGeneratedItemSourceKind::Declaration &&
             concretePayload->kind ==
                 lang::LoweredConcreteInstanceAdapterKind::Function &&
             concretePayload->body.kind == lang::MirBodyKind::Function &&
             concretePayload->declaration == concreteMir->declaration &&
             concretePayload->mayRaiseDefinedFailure ==
                 concreteMir->mayRaiseDefinedFailure,
         "the concrete generic function row should preserve its exact "
         "declaration, MIR body, and failure effect");
  expect(!first->declarations().empty() &&
             std::all_of(first->declarations().begin(),
                         first->declarations().end(),
                         [](const lang::LoweredDeclaration &declaration) {
                           return declaration.id != 0;
                         }),
         "the active declaration census should use stable nonzero identities");

  const lang::LoweredClassDeclaration *nativePair =
      findPayload<lang::LoweredClassDeclaration>(*first, "NativePair");
  const lang::LoweredClassDeclaration *bits =
      findPayload<lang::LoweredClassDeclaration>(*first, "Bits");
  const lang::LoweredEnumDeclaration *mode =
      findPayload<lang::LoweredEnumDeclaration>(*first, "Mode");
  const lang::LoweredFunctionDeclaration *addOne =
      findPayload<lang::LoweredFunctionDeclaration>(*first, "add_one");
  const lang::LoweredStorageDeclaration *processSeed =
      findPayload<lang::LoweredStorageDeclaration>(*first, "process_seed");
  expect(
      nativePair != nullptr && nativePair->cAbiRecord &&
          nativePair->cAbiLayout.has_value() &&
          !nativePair->cAbiLayout->fields.empty() &&
          first->findClassDeclaration(nativePair->id) == nativePair,
      "class declarations should own resolved ABI layouts and stable lookup");
  expect(bits != nullptr && bits->kind == lang::ClassKind::Union &&
             bits->unionLayout.has_value() &&
             !bits->unionLayout->fields.empty() &&
             first->findClassDeclaration(bits->id) == bits,
         "union declarations should own resolved value layouts");
  expect(mode != nullptr && mode->enumerators.size() == 2 &&
             first->findEnumDeclaration(mode->id) == mode,
         "enum declarations should own resolved enumerator contracts");
  expect(addOne != nullptr && addOne->returnType == lang::SemanticType::Int32 &&
             addOne->parameters.size() == 1 &&
             addOne->parameters.front().symbol != 0 &&
             first->findFunctionDeclaration(addOne->id) == addOne,
         "function declarations should own resolved signatures and bindings");
  expect(processSeed != nullptr &&
             processSeed->type == lang::SemanticType::Int32 &&
             processSeed->hasInitializer &&
             first->findStorageDeclaration(processSeed->symbol) == processSeed,
         "storage declarations should own resolved type and initialization "
         "contracts");
  expect(!first->symbols().empty() &&
             first->findSymbol(processSeed->symbol) != nullptr &&
             first->findSymbol(processSeed->symbol)->name == "process_seed",
         "the lowered symbol vocabulary should resolve MIR symbol names "
         "without SemanticModel");
  expect(first->classInstances().size() ==
                 first->mir().classInstances().size() &&
             first->functionInstances().size() ==
                 first->mir().functionInstances().size() &&
             first->constructorInstances().size() ==
                 first->mir().constructorInstances().size() &&
             first->destructorInstances().size() ==
                 first->mir().destructorInstances().size() &&
             first->lambdaInstances().size() ==
                 first->mir().lambdaInstances().size() &&
             !first->lambdaInstances().empty() &&
             !first->lambdaInstances().front().captures.empty(),
         "lowered concrete-instance tables should exactly cover MIR and own "
         "lambda names");

  const lang::LoweredClassDeclaration *exactMarker = nullptr;
  const lang::LoweredDeclaration *exactMarkerRow = nullptr;
  for (const lang::LoweredDeclaration &declaration : first->declarations()) {
    const auto *candidate =
        std::get_if<lang::LoweredClassDeclaration>(&declaration.payload);
    if (candidate != nullptr && candidate->qualifiedName == "demo::Marker" &&
        candidate->exactSpecializationPrimary != 0) {
      exactMarker = candidate;
      exactMarkerRow = &declaration;
      break;
    }
  }
  expect(exactMarker != nullptr && exactMarkerRow != nullptr &&
             exactMarkerRow->namespaceScope ==
                 std::vector<std::string>{"demo"} &&
             exactMarker->exactTypeArguments ==
                 std::vector<lang::SemanticType>{lang::SemanticType::Int32} &&
             exactMarker->exactValueArguments.empty(),
         "the lowered declaration inventory should retain an exact class "
         "specialization's primary and canonical key");
}

void testBuilderRejectsForgedOptimizationEvidence() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("lowered-forged-optimization.gti", R"(
int main() {
  mut int value = 1;
  if (true) {
    value = 2;
  } else {
    value = 3;
  }
  return value;
}
)");
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  expect(frontend.canGenerateCode() && optimized.valid() &&
             !optimized.report.transformations.empty(),
         "the forged-evidence fixture should produce verified transformations");
  if (!frontend.canGenerateCode() || !optimized.valid() ||
      optimized.report.transformations.empty()) {
    return;
  }

  std::vector<lang::MirTransformRecord> forged =
      optimized.report.transformations;
  forged.front().requiredAnalyses.dominance =
      !forged.front().requiredAnalyses.dominance;
  const lang::LoweredProgramBuild rejected =
      lang::LoweredProgramBuilder().build(
          frontend.program, frontend.semantics, frontend.hir,
          optimized.sourceMir, optimized.mir, forged, lang::TargetInfo::host());
  expect(!rejected.valid() &&
             hasIssue(rejected.issues,
                      lang::LoweredProgramIssueKind::InvalidOptimization),
         "LoweredProgramBuilder should independently reject forged optimizer "
         "evidence before sealing backend input");
}

void testDetachedDebugLocations() {
  auto normal = buildDetachedProgram();
  auto debug = buildDetachedProgram("debug-source.gti", true);
  expect(normal && debug,
         "debug capture should retain a valid detached program");
  if (!normal || !debug)
    return;
  expect(std::all_of(
             normal->bodies().begin(), normal->bodies().end(),
             [](const auto &body) { return body.sourceLocations.empty(); }),
         "debug provenance should be opt-in");
  auto &bodies = lang::LoweredProgramTestAccess::bodies(*debug);
  const auto body =
      std::find_if(bodies.begin(), bodies.end(), [](const auto &body) {
        return !body.sourceLocations.empty();
      });
  expect(body != bodies.end(),
         "debug provenance should survive frontend destruction");
  if (body == bodies.end())
    return;
  expect(std::all_of(body->sourceLocations.begin(), body->sourceLocations.end(),
                     [](const auto &row) {
                       return row.source.source.ends_with("debug-source.gti") &&
                              row.source.line > 0 &&
                              row.source.start < row.source.end;
                     }),
         "debug rows should own real source statement spans");
  body->sourceLocations.front().block = 999999;
  expect(hasIssue(lang::verifyLoweredProgram(*debug),
                  lang::LoweredProgramIssueKind::InvalidBodyInventory),
         "debug stages must identify exact optimized MIR operations");
}

void testVerificationEvidenceOwnsItsProgram() {
  auto source = buildDetachedProgram();
  expect(source.has_value(), "verification evidence fixture should lower");
  if (!source)
    return;
  std::vector<lang::LoweredProgramIssue> issues;
  const auto verified = lang::verifiedLoweredProgram(*source, issues);
  expect(verified.has_value() && issues.empty(),
         "a valid program should produce verification evidence");
  if (!verified)
    return;
  // Public value assignment is legal even though individual rows are exposed
  // read-only. It must not change the value a consumer has already verified.
  auto invalid = *source;
  auto &declarations = lang::LoweredProgramTestAccess::declarations(invalid);
  expect(!declarations.empty(), "evidence fixture needs declarations");
  if (declarations.empty())
    return;
  declarations.front().name += "_unverified_replacement";
  *source = std::move(invalid);
  expect(!lang::verifyLoweredProgram(*source).empty(),
         "the replacement must invalidate the original construction seal");
  expect(lang::verifyLoweredProgram(verified->program()).empty(),
         "verification evidence must retain its exact accepted value across "
         "public assignment of the source object");
  expect(!lang::verifiedLoweredProgram(*source, issues).has_value() &&
             !issues.empty(),
         "a newly acquired proof must reject the invalid replacement");
  source.reset();
  auto shared = *verified;
  auto transferred = std::move(shared);
  expect(&transferred.program() == &verified->program() &&
             &shared.program() == &verified->program(),
         "evidence copies and transfers share one live immutable snapshot");
  expect(lang::verifyLoweredProgram(transferred.program()).empty(),
         "the snapshot must outlive the original source object");

  auto owned = buildDetachedProgram();
  expect(owned.has_value(), "the owning handoff fixture should lower");
  if (!owned)
    return;
  const auto *rows = owned->declarations().data();
  const auto accepted = lang::verifiedLoweredProgram(std::move(*owned), issues);
  expect(accepted.has_value() && issues.empty(),
         "a moved valid value should produce verification evidence");
  if (accepted) {
    expect(accepted->program().declarations().data() == rows,
           "owning handoff must preserve the declaration storage without "
           "copying the complete lowered program");
  }
}

void testMutationRejection() {
  std::optional<lang::LoweredProgram> built = buildDetachedProgram();
  expect(built.has_value(),
         "the mutation fixture should produce a valid lowered program");
  if (!built) {
    return;
  }
  const lang::LoweredProgram original = *built;

  lang::LoweredProgram missing = original;
  lang::LoweredProgramTestAccess::generatedItems(missing).pop_back();
  expect(hasIssue(lang::verifyLoweredProgram(missing),
                  lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
         "deleting a generated item should invalidate the exact inventory");

  lang::LoweredProgram duplicated = original;
  auto &duplicatedItems =
      lang::LoweredProgramTestAccess::generatedItems(duplicated);
  duplicatedItems.push_back(duplicatedItems.front());
  expect(hasIssue(lang::verifyLoweredProgram(duplicated),
                  lang::LoweredProgramIssueKind::DuplicateGeneratedItem),
         "duplicating a generated identity should be diagnosed");

  lang::LoweredProgram reordered = original;
  auto &reorderedItems =
      lang::LoweredProgramTestAccess::generatedItems(reordered);
  std::reverse(reorderedItems.begin(), reorderedItems.end());
  expect(hasIssue(lang::verifyLoweredProgram(reordered),
                  lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
         "reordering generated contracts should invalidate deterministic "
         "construction");

  lang::LoweredProgram derooted = original;
  for (lang::LoweredBody &body :
       lang::LoweredProgramTestAccess::bodies(derooted)) {
    body.requiredGeneratedItems.clear();
  }
  const std::vector<lang::LoweredProgramIssue> derootedIssues =
      lang::verifyLoweredProgram(derooted);
  expect(hasIssue(derootedIssues,
                  lang::LoweredProgramIssueKind::OrphanGeneratedItem),
         "de-rooting generated contracts should expose orphan items");

  lang::LoweredProgram declarationDerooted = original;
  for (lang::LoweredDeclaration &declaration :
       lang::LoweredProgramTestAccess::declarations(declarationDerooted)) {
    declaration.requiredGeneratedItems.clear();
  }
  const std::vector<lang::LoweredProgramIssue> declarationDerootedIssues =
      lang::verifyLoweredProgram(declarationDerooted);
  expect(
      hasIssue(declarationDerootedIssues,
               lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory) &&
          hasIssue(declarationDerootedIssues,
                   lang::LoweredProgramIssueKind::OrphanGeneratedItem),
      "de-rooting declaration adapters should fail the exact inventory and "
      "reachability checks");

  lang::LoweredProgram cyclic = original;
  auto &cyclicItems = lang::LoweredProgramTestAccess::generatedItems(cyclic);
  expect(!cyclicItems.empty(),
         "the mutation fixture should contain generated items");
  if (!cyclicItems.empty()) {
    cyclicItems.front().dependencies = {cyclicItems.front().identity};
    expect(
        hasIssue(lang::verifyLoweredProgram(cyclic),
                 lang::LoweredProgramIssueKind::CyclicGeneratedItemDependency),
        "a generated-item dependency cycle should be diagnosed");
  }

  lang::LoweredProgram invalidLifecycle = original;
  auto &lifecycleItems =
      lang::LoweredProgramTestAccess::generatedItems(invalidLifecycle);
  const auto lifecycle =
      std::find_if(lifecycleItems.begin(), lifecycleItems.end(),
                   [](const lang::LoweredGeneratedItem &item) {
                     return item.identity.kind ==
                            lang::LoweredGeneratedItemKind::LifecycleCleanup;
                   });
  expect(lifecycle != lifecycleItems.end(),
         "the mutation fixture should contain lifecycle cleanup");
  if (lifecycle != lifecycleItems.end()) {
    auto &payload =
        std::get<lang::LoweredLifecycleCleanupItem>(lifecycle->payload);
    ++payload.classInstance;
    expect(
        hasIssue(lang::verifyLoweredProgram(invalidLifecycle),
                 lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
        "forging a lifecycle class instance should invalidate its exact "
        "generated-item contract");
  }

  lang::LoweredProgram invalidConcrete = original;
  auto &concreteItems =
      lang::LoweredProgramTestAccess::generatedItems(invalidConcrete);
  const auto concrete = std::find_if(
      concreteItems.begin(), concreteItems.end(),
      [](const lang::LoweredGeneratedItem &item) {
        return item.identity.kind ==
               lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter;
      });
  expect(concrete != concreteItems.end(),
         "the mutation fixture should contain a concrete-instance adapter");
  if (concrete != concreteItems.end()) {
    auto &payload =
        std::get<lang::LoweredConcreteInstanceAdapterItem>(concrete->payload);
    ++payload.declaration;
    expect(
        hasIssue(lang::verifyLoweredProgram(invalidConcrete),
                 lang::LoweredProgramIssueKind::InvalidGeneratedItemInventory),
        "forging a concrete-instance declaration should invalidate its exact "
        "generated-item contract");
  }

  lang::LoweredProgram staleSeal = original;
  auto &declarations = lang::LoweredProgramTestAccess::declarations(staleSeal);
  expect(!declarations.empty(),
         "the mutation fixture should contain declarations");
  if (!declarations.empty()) {
    declarations.front().name += "_forged";
    expect(hasIssue(lang::verifyLoweredProgram(staleSeal),
                    lang::LoweredProgramIssueKind::InvalidConstructionSeal),
           "mutating pointer-free payload should invalidate the construction "
           "seal");
  }

  lang::LoweredProgram invalidPayload = original;
  auto &invalidDeclarations =
      lang::LoweredProgramTestAccess::declarations(invalidPayload);
  const auto function = std::find_if(
      invalidDeclarations.begin(), invalidDeclarations.end(),
      [](const lang::LoweredDeclaration &declaration) {
        return std::holds_alternative<lang::LoweredFunctionDeclaration>(
            declaration.payload);
      });
  expect(function != invalidDeclarations.end(),
         "the mutation fixture should contain a function payload");
  if (function != invalidDeclarations.end()) {
    std::get<lang::LoweredFunctionDeclaration>(function->payload).returnType =
        lang::SemanticType::Unknown;
    const std::vector<lang::LoweredProgramIssue> payloadIssues =
        lang::verifyLoweredProgram(invalidPayload);
    expect(hasIssue(payloadIssues,
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "an unresolved declaration payload should be rejected");
    expect(hasIssue(payloadIssues,
                    lang::LoweredProgramIssueKind::InvalidConstructionSeal),
           "declaration payload mutation should invalidate the construction "
           "seal");
  }

  lang::LoweredProgram duplicateIdentity = original;
  lang::LoweredProgram missingSourceIdentity = original;
  for (auto &declaration :
       lang::LoweredProgramTestAccess::declarations(missingSourceIdentity)) {
    if (auto *payload =
            std::get_if<lang::LoweredClassDeclaration>(&declaration.payload)) {
      payload->sourceUnit = 999999;
      break;
    }
  }
  const auto sourceIssues = lang::verifyLoweredProgram(missingSourceIdentity);
  expect(std::any_of(
             sourceIssues.begin(), sourceIssues.end(),
             [](const auto &issue) {
               return issue.kind == lang::LoweredProgramIssueKind::
                                        InvalidDeclarationInventory &&
                      issue.detail ==
                          "lowered declaration has no logical source identity";
             }),
         "a declaration without a logical source identity must fail before "
         "emission");
  auto &duplicateDeclarations =
      lang::LoweredProgramTestAccess::declarations(duplicateIdentity);
  lang::LoweredFunctionDeclaration *firstFunction = nullptr;
  lang::LoweredDeclaration *secondFunctionRow = nullptr;
  for (lang::LoweredDeclaration &declaration : duplicateDeclarations) {
    auto *payload =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    if (payload == nullptr) {
      continue;
    }
    if (firstFunction == nullptr) {
      firstFunction = payload;
    } else {
      secondFunctionRow = &declaration;
      break;
    }
  }

  lang::LoweredProgram invalidSpecialization = original;
  auto &specializationDeclarations =
      lang::LoweredProgramTestAccess::declarations(invalidSpecialization);
  const auto exactSpecialization = std::find_if(
      specializationDeclarations.begin(), specializationDeclarations.end(),
      [](const lang::LoweredDeclaration &declaration) {
        const auto *payload =
            std::get_if<lang::LoweredClassDeclaration>(&declaration.payload);
        return payload != nullptr && payload->exactSpecializationPrimary != 0;
      });
  expect(exactSpecialization != specializationDeclarations.end(),
         "the mutation fixture should contain an exact specialization");
  if (exactSpecialization != specializationDeclarations.end()) {
    const std::size_t exactIndex = static_cast<std::size_t>(
        std::distance(specializationDeclarations.begin(), exactSpecialization));
    std::get<lang::LoweredClassDeclaration>(exactSpecialization->payload)
        .exactSpecializationPrimary = 999999;
    expect(hasIssue(lang::verifyLoweredProgram(invalidSpecialization),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "an exact specialization with a missing primary should be "
           "rejected");

    lang::LoweredProgram wrongSpecializationScope = original;
    lang::LoweredProgramTestAccess::declarations(
        wrongSpecializationScope)[exactIndex]
        .namespaceScope = {"wrong"};
    expect(hasIssue(lang::verifyLoweredProgram(wrongSpecializationScope),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "an exact specialization outside its primary's canonical namespace "
           "should be rejected");
  }
  expect(firstFunction != nullptr && secondFunctionRow != nullptr,
         "the mutation fixture should contain multiple function identities");
  if (firstFunction != nullptr && secondFunctionRow != nullptr) {
    auto &secondFunction =
        std::get<lang::LoweredFunctionDeclaration>(secondFunctionRow->payload);
    secondFunction.id = firstFunction->id;
    secondFunctionRow->semanticIdentity = firstFunction->id;
    expect(hasIssue(lang::verifyLoweredProgram(duplicateIdentity),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "duplicate semantic declaration identities should be rejected");
  }

  lang::LoweredProgram missingInstance = original;
  auto &functionInstances =
      lang::LoweredProgramTestAccess::functionInstances(missingInstance);
  expect(!functionInstances.empty(),
         "the mutation fixture should contain concrete function instances");
  if (!functionInstances.empty()) {
    functionInstances.pop_back();
    expect(hasIssue(lang::verifyLoweredProgram(missingInstance),
                    lang::LoweredProgramIssueKind::InvalidInstanceInventory),
           "deleting concrete-instance metadata should be rejected");
  }

  lang::LoweredProgram forgedConstructorArgument = original;
  lang::LoweredProgram forgedConstructorConstexpr = original;
  auto &constantMir =
      lang::LoweredProgramTestAccess::mir(forgedConstructorConstexpr);
  auto &constantConstructors =
      const_cast<std::vector<lang::MirConstructorInstance> &>(
          constantMir.constructorInstances());
  expect(!constantConstructors.empty(),
         "fixture has constructor eligibility metadata");
  if (!constantConstructors.empty()) {
    constantConstructors.front().constexprConstructor =
        !constantConstructors.front().constexprConstructor;
    expect(hasIssue(lang::verifyLoweredProgram(forgedConstructorConstexpr),
                    lang::LoweredProgramIssueKind::InvalidInstanceInventory),
           "constructor constexpr eligibility cannot diverge from its semantic "
           "declaration");
  }
  auto &constructorInstances =
      lang::LoweredProgramTestAccess::constructorInstances(
          forgedConstructorArgument);
  const auto genericConstructor =
      std::find_if(constructorInstances.begin(), constructorInstances.end(),
                   [](const lang::LoweredConstructorInstance &instance) {
                     return instance.typeArguments.size() == 2;
                   });
  expect(genericConstructor != constructorInstances.end(),
         "the mutation fixture should contain a constructor-local generic "
         "instance");
  if (genericConstructor != constructorInstances.end()) {
    std::swap(genericConstructor->typeArguments[0],
              genericConstructor->typeArguments[1]);
    expect(hasIssue(lang::verifyLoweredProgram(forgedConstructorArgument),
                    lang::LoweredProgramIssueKind::InvalidInstanceInventory),
           "reordering constructor-local type arguments without changing "
           "the MIR identity should be rejected");

    lang::LoweredProgram forgedConstructorParameter = original;
    lang::MirProgram &mir =
        lang::LoweredProgramTestAccess::mir(forgedConstructorParameter);
    auto &mirConstructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            mir.constructorInstances());
    const auto concrete =
        std::find_if(mirConstructors.begin(), mirConstructors.end(),
                     [](const lang::MirConstructorInstance &instance) {
                       return instance.typeArguments.size() == 2;
                     });
    if (concrete != mirConstructors.end() &&
        !concrete->parameterTypes.empty()) {
      concrete->parameterTypes.front() = lang::SemanticType::Bool;
    }
    expect(
        concrete != mirConstructors.end() &&
            !concrete->parameterTypes.empty() &&
            hasIssue(lang::verifyLoweredProgram(forgedConstructorParameter),
                     lang::LoweredProgramIssueKind::InvalidInstanceInventory),
        "substituting a constructor parameter type without changing its "
        "concrete generic identity should be rejected");
  }

  lang::LoweredProgram duplicateSymbol = original;
  auto &symbols = lang::LoweredProgramTestAccess::symbols(duplicateSymbol);
  expect(symbols.size() > 1,
         "the mutation fixture should contain multiple lowered symbols");
  if (symbols.size() > 1) {
    symbols[1].id = symbols.front().id;
    expect(hasIssue(lang::verifyLoweredProgram(duplicateSymbol),
                    lang::LoweredProgramIssueKind::InvalidSymbolInventory),
           "duplicate lowered symbol identities should be rejected");
  }

  // Static method invariants are structural, not an exception for one owner
  // kind. Exercise both a normal class and an unrelated interface factory.
  auto staticFrontend = lang::Frontend().analyze("static-method-contracts.gti",
                                                 R"gti(
class Factory {
public:
  static int Build(int value) { return value; }
};
interface FactoryApi {
  static int Select(int value) { return Factory::Build(value); }
  int value();
};
class Signal : public FactoryApi {
public:
  int value() override { return 3; }
};
int main() {
  Signal signal{};
  return Factory::Build(1) + FactoryApi::Select(2) + signal.value() - 6;
}
)gti");
  expect(staticFrontend.canGenerateCode(),
         "class and interface static methods share ordinary executable rules");
  if (!staticFrontend.canGenerateCode()) {
    for (const auto &diagnostic : staticFrontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  auto staticBuild = lang::LoweredProgramBuilder().build(
      staticFrontend.program, staticFrontend.semantics, staticFrontend.hir,
      staticFrontend.mir, staticFrontend.mir, {}, lang::TargetInfo::host());
  expect(staticBuild.valid(),
         "class and interface static contracts cross the lowered boundary");
  if (!staticBuild.valid()) {
    for (const auto &issue : staticBuild.issues)
      std::cerr << issue.detail << '\n';
    return;
  }
  const lang::LoweredProgram staticOriginal = *staticBuild.program;
  std::size_t staticTargets = 0;
  for (const auto &candidate : staticOriginal.mir().functionInstances()) {
    if (!candidate.staticMember)
      continue;
    ++staticTargets;
    expect(
        candidate.owner && !candidate.virtualMethod && !candidate.pureVirtual &&
            !candidate.overrideMethod && candidate.virtualRoots.empty() &&
            candidate.receiverMutability == lang::ReceiverMutability::ReadOnly,
        "static methods preserve an owner without a receiver or dispatch "
        "contract");
    const std::size_t functionIndex = candidate.id - 1;
    for (int mutation = 0; mutation < 8; ++mutation) {
      lang::MirProgram forged = staticOriginal.mir();
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      auto &function = functions.at(functionIndex);
      switch (mutation) {
      case 0:
        function.virtualMethod = true;
        break;
      case 1:
        function.pureVirtual = true;
        break;
      case 2:
        function.overrideMethod = true;
        break;
      case 3:
        function.virtualRoots.push_back(function.declaration);
        break;
      case 4:
        function.receiverMutability = lang::ReceiverMutability::Mutable;
        break;
      case 5:
        function.receiverMutability = lang::ReceiverMutability::Consuming;
        break;
      case 6:
        function.owner.reset();
        break;
      case 7: {
        lang::MirPlace receiver;
        receiver.id = function.body.places.size() + 1;
        receiver.root = lang::MirPlaceRootKind::This;
        receiver.type = forged.findClassInstance(*candidate.owner)->type;
        function.body.places.push_back(std::move(receiver));
        break;
      }
      }
      const auto verification = lang::verifyMirProgram(forged);
      expect(!verification.valid() &&
                 std::any_of(
                     verification.errors.begin(), verification.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.owner == candidate.id &&
                              error.message.find("static method must") !=
                                  std::string::npos;
                     }),
             "MIR must reject forged static dispatch, receiver or owner "
             "metadata for every owner kind");
    }
    for (int mutation = 0; mutation < 6; ++mutation) {
      lang::LoweredProgram forged = staticOriginal;
      auto &declarations = lang::LoweredProgramTestAccess::declarations(forged);
      const auto row =
          std::find_if(declarations.begin(), declarations.end(),
                       [&](const lang::LoweredDeclaration &declaration) {
                         const auto *payload =
                             std::get_if<lang::LoweredFunctionDeclaration>(
                                 &declaration.payload);
                         return payload && payload->id == candidate.declaration;
                       });
      expect(row != declarations.end(),
             "static method has one exact lowered declaration");
      if (row == declarations.end())
        continue;
      auto &payload = std::get<lang::LoweredFunctionDeclaration>(row->payload);
      switch (mutation) {
      case 0:
        payload.staticMember = false;
        break;
      case 1:
        payload.virtualMethod = true;
        break;
      case 2:
        payload.pureVirtual = true;
        break;
      case 3:
        payload.overrideMethod = true;
        break;
      case 4:
        payload.virtualRoots.push_back(payload.id);
        break;
      case 5:
        payload.receiverMutability = lang::ReceiverMutability::Mutable;
        break;
      }
      expect(hasIssue(lang::verifyLoweredProgram(forged),
                      lang::LoweredProgramIssueKind::InvalidInstanceInventory),
             "lowered signatures cannot disagree with static-method MIR "
             "dispatch or receiver authority");
    }
  }
  expect(staticTargets == 2,
         "mutations cover both class and interface static method instances");
}

void testConcreteBranchSymbols() {
  constexpr std::string_view source = R"gti(
T select<T, uint64_t Mode>(T value) {
  if constexpr (Mode == 0) {
    T local = value;
    { T local = value; }
    return local;
  } else {
    T alternative = value;
    return alternative;
  }
}
int main() {
  return select<int, 0>(7) == 7 && select<bool, 0>(true) &&
         select<int, 1>(9) == 9 ? 0 : 1;
}
)gti";
  auto frontend = lang::Frontend().analyze("concrete-branch-symbols.gti",
                                           std::string(source));
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "selected branches retain semantic locals");
  if (!frontend.canGenerateCode())
    return;
  const auto &symbols = frontend.hir.instanceSymbols();
  expect(symbols.size() == 5 &&
             std::count_if(symbols.begin(), symbols.end(),
                           [](const auto &s) {
                             return s.name == "local" &&
                                    s.type == lang::SemanticType::Bool;
                           }) == 2 &&
             std::count_if(symbols.begin(), symbols.end(),
                           [](const auto &s) {
                             return s.name == "local" &&
                                    s.type == lang::SemanticType::Int32;
                           }) == 2,
         "two instances and shadowed locals retain separate typed identities");
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "concrete-only locals cross the lowered inventory");
  if (!built.valid())
    return;
  for (const auto &symbol : symbols) {
    const auto *lowered = built.program->findSymbol(symbol.id);
    expect(lowered && lowered->type == symbol.type &&
               lowered->nameSource == symbol.nameSpan,
           "concrete symbol type and source identity survive lowering");
  }
  auto missing = *built.program;
  std::erase_if(lang::LoweredProgramTestAccess::symbols(missing),
                [&](const auto &symbol) { return symbol.id == symbols[0].id; });
  expect(hasIssue(lang::verifyLoweredProgram(missing),
                  lang::LoweredProgramIssueKind::InvalidSymbolInventory),
         "missing selected-arm symbol still fails verification");
  auto wrong = *built.program;
  auto &wrongSymbols = lang::LoweredProgramTestAccess::symbols(wrong);
  const auto record = std::find_if(
      wrongSymbols.begin(), wrongSymbols.end(),
      [&](const auto &symbol) { return symbol.id == symbols[0].id; });
  record->type = lang::SemanticType::Unknown;
  expect(hasIssue(lang::verifyLoweredProgram(wrong),
                  lang::LoweredProgramIssueKind::InvalidSymbolInventory),
         "a forged unresolved concrete local fails verification");
  auto inactive = lang::Frontend().analyze("inactive-branch.gti", R"gti(
int choose<uint64_t N>() {
  if constexpr (N == 0) { int local = 2; return local; }
  else { Missing type{}; return unknown_name; }
}
int main() { return choose<0>() - 2; }
)gti");
  expect(inactive.canGenerateCode(), "discarded arms remain unanalyzed");
  auto invalidBorrow =
      lang::Frontend().analyze("selected-borrow-escape.gti", R"gti(
int& borrow<uint64_t N>(int& source) {
  if constexpr (N == 0) { int local = 2; return local; }
}
int main() { int value = 1; return borrow<0>(value); }
)gti");
  expect(!invalidBorrow.canGenerateCode() &&
             std::any_of(invalidBorrow.diagnostics.begin(),
                         invalidBorrow.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2017"; }),
         "selected-arm locals still cannot escape by reference");
  auto forged = frontend.hir;
  auto &forgedSymbols =
      const_cast<std::vector<lang::SymbolRecord> &>(forged.instanceSymbols());
  forgedSymbols.front().type = lang::SemanticType::Bool;
  expect(!lang::verifyHirProgramPlans(frontend.semantics, forged).valid(),
         "a forged concrete symbol type fails HIR provenance verification");
}

void testConcreteLambdaPacks() {
  auto frontend = lang::Frontend().analyze("concrete-lambda-packs.gti", R"gti(
int execute<Args...>(Args... args) {
  auto callback = [](int tag, Args... forwarded) -> int { return tag; };
  return callback(3, args...);
}
int main() { return execute() + execute(4) + execute(7, true) - 9; }
)gti");
  expect(
      frontend.canGenerateCode(),
      "concrete lambda packs analyze for empty, single and multiple elements");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(),
         "detached lambda inventory preserves lexical packs and flattened ABI");
  if (!built.valid()) {
    for (const auto &issue : built.issues)
      std::cerr << issue.detail << '\n';
    return;
  }
  const auto &original = *built.program;
  std::vector<std::size_t> sizes;
  for (const auto &lambda : original.lambdaInstances()) {
    expect(lambda.parameters.size() == 2 && lambda.parameters.back().pack &&
               lambda.parameters.back().type.kind ==
                   lang::SemanticType::TypePack &&
               lambda.parameters.back().type.concretePack,
           "lambda inventory keeps the exact lexical pack binding");
    if (lambda.parameters.size() != 2)
      continue;
    sizes.push_back(lambda.parameters.back().type.arguments.size());
    for (int mutation = 0; mutation != 8; ++mutation) {
      auto changed = original;
      auto &target = lang::LoweredProgramTestAccess::lambdaInstances(
          changed)[lambda.id - 1];
      auto &parameter = target.parameters.back();
      if (mutation == 0)
        parameter.pack = false;
      else if (mutation == 1)
        parameter.type.concretePack = false;
      else if (mutation == 2)
        parameter.type.arguments.push_back(lang::SemanticType::Bool);
      else if (mutation == 3) {
        if (parameter.type.arguments.empty())
          parameter.type.arguments.push_back(lang::SemanticType::Int32);
        else
          parameter.type.arguments.pop_back();
      } else if (mutation == 4)
        parameter.symbol = target.parameters.front().symbol;
      else if (mutation == 5)
        target.parameters.pop_back();
      else if (mutation == 6)
        parameter.access = lang::AccessMode::Mutable;
      else
        target.parameters.front().pack = true;
      expect(hasIssue(lang::verifyLoweredProgram(changed),
                      lang::LoweredProgramIssueKind::InvalidInstanceInventory),
             "malformed lambda pack shape, element, identity and access must "
             "fail detached verification");
    }
  }
  std::sort(sizes.begin(), sizes.end());
  expect(
      sizes == std::vector<std::size_t>{0, 1, 2},
      "all three pack arities survive without invented or dropped parameters");
}

void testConcreteClassAliases() {
  auto frontend = lang::Frontend().analyze("concrete-class-aliases.gti", R"gti(
namespace shapes {
struct Layer<uint64_t N> { Layer<N - 1> inner; int value = 1; };
}
struct shapes::Layer<0> { int value = 100; };
struct Box<uint64_t N> {
  using Previous = shapes::Layer<N - 1>;
  Previous inner;
};
int main() { Box<1> first{}; Box<2> second{}; Box<3> third{}; return 0; }
)gti");
  expect(frontend.canGenerateCode(),
         "concrete class aliases analyze independently");
  if (!frontend.canGenerateCode()) {
    for (const auto &d : frontend.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return;
  }
  std::vector<const lang::HirClassInstance *> owners;
  for (const auto &owner : frontend.hir.classInstances())
    if (!owner.typeAliases.empty())
      owners.push_back(&owner);
  expect(owners.size() == 3 && owners[0]->typeAliases[0].declaration != 0 &&
             owners[0]->typeAliases[0].declaration ==
                 owners[1]->typeAliases[0].declaration &&
             owners[0]->typeAliases[0].type != owners[1]->typeAliases[0].type &&
             owners[1]->typeAliases[0].type != owners[2]->typeAliases[0].type,
         "one primary alias identity retains three exact owner-specific types");
  if (owners.size() != 3)
    return;
  bool testedImplicitSource = false;
  for (const auto &constructor : frontend.hir.constructorInstances()) {
    if (!constructor.generatedDefault)
      continue;
    const auto &owner = frontend.hir.classInstances()[constructor.owner - 1];
    for (const auto &field : owner.fields) {
      if (field.declaration == nullptr || field.declaration->initializer())
        continue;
      const auto initialized = std::find_if(
          constructor.initializers.begin(), constructor.initializers.end(),
          [&](const auto &initializer) {
            return initializer.field == field.info.symbol &&
                   initializer.arguments.size() == 1;
          });
      if (initialized == constructor.initializers.end())
        continue;
      auto forged = frontend.hir;
      auto &constructors =
          const_cast<std::vector<lang::HirConstructorInstance> &>(
              forged.constructorInstances());
      auto &body = constructors[constructor.id - 1].body;
      auto value = std::find_if(
          body.values.begin(), body.values.end(), [&](const auto &candidate) {
            return candidate.id == initialized->arguments.front();
          });
      if (value == body.values.end())
        continue;
      value->source = nullptr;
      expect(!lang::verifyHirProgramPlans(frontend.semantics, forged).valid(),
             "implicit nominal default retains exact semantic anchor identity");
      testedImplicitSource = true;
    }
  }
  expect(testedImplicitSource,
         "alias fixture exercises generated implicit nominal construction");
  auto forgedHir = frontend.hir;
  auto &hirClasses = const_cast<std::vector<lang::HirClassInstance> &>(
      forgedHir.classInstances());
  hirClasses[owners[0]->id - 1].typeAliases[0].type =
      owners[1]->typeAliases[0].type;
  expect(!lang::verifyHirProgramPlans(frontend.semantics, forgedHir).valid(),
         "another owner's closed alias type fails HIR verification");
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "concrete aliases survive detached lowering");
  if (!built.valid())
    return;
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto forged = *built.program;
    auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
        lang::LoweredProgramTestAccess::mir(forged).classInstances());
    auto &aliases = classes[owners[0]->id - 1].typeAliases;
    if (mutation == 0)
      aliases.clear();
    if (mutation == 1)
      aliases.push_back(aliases.front());
    if (mutation == 2)
      aliases[0].declaration += 10000;
    if (mutation == 3)
      aliases[0].type = owners[1]->typeAliases[0].type;
    expect(!lang::verifyLoweredProgram(forged).empty(),
           "missing, duplicate, foreign or another owner's alias facts fail "
           "detached verification");
  }
}

void testGeneralRequirementMetadata() {
  auto frontend = lang::Frontend().analyze("lowered-requirements.gti", R"(
constexpr bool accepted<T>() { return true; }
concept selected<T> = accepted<T>();
void named<T>() requires selected<T> {}
void predicate<T>() requires accepted<T>() {}
void expression<T>() requires requires(T& value) {
  { value + value } -> T;
} {}
void nullary<T>() requires requires { { T() } -> T; } {}
class Reader {
public:
  int32_t read() { return 7; }
};
void member<T>() requires requires(T& value) {
  { value.read() } -> int32_t;
} {}
class Box {
public:
  Box<T>(T value) requires requires(T& input) { { input + input } -> T; } {}
};
int main() {
  named<int32_t>();
  predicate<int32_t>();
  expression<int32_t>();
  nullary<int32_t>();
  member<Reader>();
  Box box = Box(1);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "general requirement metadata fixture is semantically valid");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << "  requirement fixture: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    return;
  }
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(),
         "concept/member/expression/predicate requirements cross the lowered "
         "boundary without AST pointers or fabricated member names");
  if (!built.valid()) {
    for (const auto &issue : built.issues)
      std::cerr << "  requirement boundary: " << issue.detail << '\n';
    return;
  }
  const auto &program = *built.program;
  const auto *named =
      findPayload<lang::LoweredFunctionDeclaration>(program, "named");
  const auto *member =
      findPayload<lang::LoweredFunctionDeclaration>(program, "member");
  const auto *expression =
      findPayload<lang::LoweredFunctionDeclaration>(program, "expression");
  const auto *nullary =
      findPayload<lang::LoweredFunctionDeclaration>(program, "nullary");
  const auto *predicate =
      findPayload<lang::LoweredFunctionDeclaration>(program, "predicate");
  expect(named && member && expression && nullary && predicate,
         "all requirement declaration rows are retained");
  if (!named || !member || !expression || !nullary || !predicate)
    return;
  expect(
      named->requirements.size() == 1 &&
          named->requirements[0].kind ==
              lang::LoweredRequirementKind::Concept &&
          named->requirements[0].conceptId != 0 &&
          member->requirements.size() == 1 &&
          member->requirements[0].kind ==
              lang::LoweredRequirementKind::Member &&
          member->requirements[0].memberName == "read",
      "named concept and exact member leaves keep their distinct identities");
  expect(expression->requirements.size() == 1 &&
             expression->requirements[0].kind ==
                 lang::LoweredRequirementKind::Expression &&
             expression->requirements[0].expressionParameterCount == 1 &&
             expression->requirements[0].resultArgument == 1 &&
             expression->requirements[0].arguments.size() == 2 &&
             expression->requirements[0].memberName.empty() &&
             nullary->requirements.size() == 1 &&
             nullary->requirements[0].resultArgument == 0 &&
             nullary->requirements[0].expressionParameterCount == 0 &&
             predicate->requirements.size() == 1 &&
             predicate->requirements[0].kind ==
                 lang::LoweredRequirementKind::Predicate &&
             !predicate->requirements[0].resultArgument &&
             predicate->requirements[0].memberName.empty(),
         "general expressions retain exact result slots and predicates never "
         "masquerade as member requirements");
  const auto text = lang::LoweredProgramPrinter().print(program);
  expect(text.find("kind=expression;concept=0;expression-parameters=1;result-"
                   "argument=1") != std::string::npos &&
             text.find("kind=predicate;concept=0") != std::string::npos,
         "deterministic serialization exposes the explicit requirement kinds");

  std::size_t checkedFunctions = 0;
  std::size_t checkedConstructors = 0;
  for (std::size_t row = 0; row < program.declarations().size(); ++row) {
    const auto &declaration = program.declarations()[row];
    const auto *function =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    const auto *constructor =
        std::get_if<lang::LoweredConstructorDeclaration>(&declaration.payload);
    const auto *requirements = function      ? &function->requirements
                               : constructor ? &constructor->requirements
                                             : nullptr;
    if (!requirements || requirements->empty())
      continue;
    if (function)
      ++checkedFunctions;
    else
      ++checkedConstructors;
    for (int mutation = 0; mutation < 7; ++mutation) {
      auto forged = program;
      auto &payload =
          lang::LoweredProgramTestAccess::declarations(forged)[row].payload;
      auto &requirement =
          function ? std::get<lang::LoweredFunctionDeclaration>(payload)
                         .requirements[0]
                   : std::get<lang::LoweredConstructorDeclaration>(payload)
                         .requirements[0];
      if (mutation == 0)
        requirement.kind = lang::LoweredRequirementKind::Count;
      else if (mutation == 1)
        requirement.resultArgument = requirement.arguments.size();
      else if (mutation == 2)
        requirement.expressionParameterCount = requirement.arguments.size() + 1;
      else if (mutation == 3)
        requirement.arguments.push_back(lang::SemanticType::Unknown);
      else if (mutation == 4)
        requirement.conceptId = requirement.conceptId == 0 ? 999999 : 0;
      else if (mutation == 5) {
        requirement.kind = lang::LoweredRequirementKind::Expression;
        requirement.resultArgument.reset();
      } else {
        requirement.kind = lang::LoweredRequirementKind::Predicate;
        requirement.requiresUnsafe = true;
      }
      expect(
          hasIssue(lang::verifyLoweredProgram(forged),
                   lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
          "invalid requirement kinds, mixed payloads, result slots and "
          "unresolved types are rejected before backend consumption");
    }
    if ((*requirements)[0].kind == lang::LoweredRequirementKind::Expression ||
        (*requirements)[0].kind == lang::LoweredRequirementKind::Predicate) {
      auto forged = program;
      auto &payload =
          lang::LoweredProgramTestAccess::declarations(forged)[row].payload;
      auto &requirement =
          function ? std::get<lang::LoweredFunctionDeclaration>(payload)
                         .requirements[0]
                   : std::get<lang::LoweredConstructorDeclaration>(payload)
                         .requirements[0];
      requirement.source = {};
      expect(
          hasIssue(lang::verifyLoweredProgram(forged),
                   lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
          "general requirement leaves must retain source provenance");
    }
    if ((*requirements)[0].kind == lang::LoweredRequirementKind::Expression) {
      auto forged = program;
      auto &payload =
          lang::LoweredProgramTestAccess::declarations(forged)[row].payload;
      auto &requirement =
          function ? std::get<lang::LoweredFunctionDeclaration>(payload)
                         .requirements[0]
                   : std::get<lang::LoweredConstructorDeclaration>(payload)
                         .requirements[0];
      auto &result = requirement.arguments[*requirement.resultArgument];
      result = lang::SemanticType::referenceTo(result);
      expect(
          hasIssue(lang::verifyLoweredProgram(forged),
                   lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
          "general expression result slots must not silently become borrows");
    }
  }
  expect(checkedFunctions >= 5 && checkedConstructors >= 1,
         "requirement mutation coverage includes functions and constructors");
}

void testClassDefinitionFamilyIdentity() {
  const auto frontend = lang::Frontend().analyze("definition-family.gti", R"(
class Record<T, E> { public: int kind() { return 1; } };
class Record<void, E> { public: int kind() { return 2; } };
class Unrelated<T, E> {};
int main() {
  Record<int, int> value{};
  Record<void, int> empty{};
  return value.kind() + empty.kind() - 3;
}
)");
  expect(frontend.canGenerateCode(), "definition family fixture is valid");
  if (!frontend.canGenerateCode())
    return;
  auto built = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(built.valid(), "definition family crosses the lowered boundary");
  if (!built.valid())
    return;
  std::size_t memberIndex = built.program->declarations().size();
  lang::ClassId unrelated = 0;
  for (std::size_t index = 0; index < built.program->declarations().size();
       ++index) {
    const auto &declaration = built.program->declarations()[index];
    const auto *type =
        std::get_if<lang::LoweredClassDeclaration>(&declaration.payload);
    if (!type)
      continue;
    if (type->definitionFamilyPrimary != 0)
      memberIndex = index;
    if (declaration.name == "Unrelated")
      unrelated = type->id;
  }
  expect(memberIndex < built.program->declarations().size() && unrelated != 0,
         "family fixture retains its member and unrelated identity");
  if (memberIndex >= built.program->declarations().size() || unrelated == 0)
    return;
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto forged = *built.program;
    auto &member = std::get<lang::LoweredClassDeclaration>(
        lang::LoweredProgramTestAccess::declarations(forged)[memberIndex]
            .payload);
    member.definitionFamilyPrimary = mutation == 0   ? member.id
                                     : mutation == 1 ? unrelated
                                                     : 999999;
    expect(hasIssue(lang::verifyLoweredProgram(forged),
                    lang::LoweredProgramIssueKind::InvalidDeclarationInventory),
           "self, unrelated and missing class family identities are rejected");
  }
}

} // namespace

int main() {
  testDetachedDeterministicProgram();
  testBuilderRejectsForgedOptimizationEvidence();
  testDetachedDebugLocations();
  testVerificationEvidenceOwnsItsProgram();
  testMutationRejection();
  testConcreteBranchSymbols();
  testConcreteLambdaPacks();
  testConcreteClassAliases();
  testGeneralRequirementMetadata();
  testClassDefinitionFamilyIdentity();
  if (failures != 0) {
    std::cerr << failures << " lowered-program test(s) failed\n";
    return 1;
  }
  std::cout << "lowered-program contract tests passed\n";
  return 0;
}
