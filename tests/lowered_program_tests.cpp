#include "gti/frontend.h"
#include "gti/lowered_program.h"
#include "gti/lowered_program_builder.h"
#include "gti/lowered_program_printer.h"
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
buildDetachedProgram(std::string_view sourceName = "lowered-program.gti") {
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
  return identity<int32_t>(counter.read()) + process_seed + apply_offset(0) - 2;
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
      optimized.mir, lang::TargetInfo::host());
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
  expect(firstText.find("lowered-program-v1") == 0 &&
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
              lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter) == 1,
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
      [](const lang::LoweredGeneratedItem &item) {
        return item.identity.kind ==
               lang::LoweredGeneratedItemKind::ConcreteInstanceAdapter;
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
}

} // namespace

int main() {
  testDetachedDeterministicProgram();
  testMutationRejection();
  if (failures != 0) {
    std::cerr << failures << " lowered-program test(s) failed\n";
    return 1;
  }
  std::cout << "lowered-program contract tests passed\n";
  return 0;
}
