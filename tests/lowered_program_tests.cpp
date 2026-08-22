#include "gti/frontend.h"
#include "gti/lowered_program.h"
#include "gti/lowered_program_printer.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <iostream>
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

mut int32_t process_seed = 1;

extern "C" {
  Unary install_callback(Unary callback);
}

int32_t add_one(int32_t value) { return value + 1; }

int main() {
  unsafe {
    [[discard]] install_callback(add_one);
  }
  Counter counter = Counter();
  return counter.read() + process_seed - 1;
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
              1,
      "the lowered program should own startup, initialization, and native "
      "callback contracts without frontend pointers");
  expect(!first->declarations().empty() &&
             std::all_of(first->declarations().begin(),
                         first->declarations().end(),
                         [](const lang::LoweredDeclaration &declaration) {
                           return declaration.id != 0;
                         }),
         "the active declaration census should use stable nonzero identities");
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
