#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The function route dissolved into analysis-driven per-body admission:
// slot-vocabulary bodies publish under the general route's label while the
// destructor-definition route keeps the family label below.
constexpr std::string_view functionMarker =
    "// GTI verified-MIR body: scalar-cfg-v1 function-instance ";
constexpr std::string_view failureFunctionMarker =
    "// GTI verified-MIR body: scalar-cfg-failure-v1 function-instance ";
constexpr std::string_view destructorMarker =
    "// GTI verified-MIR body: scalar-cfg-v1 destructor-instance ";
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::size_t count(std::string_view text, std::string_view needle) {
  std::size_t result = 0;
  for (std::size_t position = text.find(needle);
       position != std::string_view::npos;
       position = text.find(needle, position + needle.size())) {
    ++result;
  }
  return result;
}

std::string_view definitionContaining(std::string_view generated,
                                      std::string_view needle) {
  for (std::size_t position = generated.find(needle);
       position != std::string_view::npos;
       position = generated.find(needle, position + needle.size())) {
    const std::size_t lineEnd = generated.find('\n', position);
    const std::size_t opening = generated.find('{', position + needle.size());
    if (opening == std::string_view::npos ||
        (lineEnd != std::string_view::npos && opening > lineEnd)) {
      continue;
    }
    std::size_t depth = 0;
    for (std::size_t cursor = opening; cursor < generated.size(); ++cursor) {
      if (generated[cursor] == '{') {
        ++depth;
      } else if (generated[cursor] == '}' && --depth == 0) {
        return generated.substr(opening, cursor + 1 - opening);
      }
    }
  }
  return {};
}

std::string_view functionDefinition(std::string_view generated,
                                    std::string_view sourceName) {
  return definitionContaining(generated,
                              std::string{"_"} + std::string{sourceName} + "(");
}

std::string_view lifecycleDefinition(std::string_view generated,
                                     std::string_view className) {
  return definitionContaining(generated, std::string{className} +
                                             "::__gti_lifecycle_cleanup_");
}

const lang::HirFunctionInstance *
findHirFunction(const lang::HirProgram &program, std::string_view name) {
  const auto found = std::find_if(
      program.functionInstances().begin(), program.functionInstances().end(),
      [name](const lang::HirFunctionInstance &function) {
        return function.source != nullptr &&
               function.source->name().lexeme == name;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
}

const lang::HirClassInstance *findHirClass(const lang::HirProgram &program,
                                           std::string_view name) {
  const auto found = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [name](const lang::HirClassInstance &instance) {
        return instance.source != nullptr &&
               instance.source->name().lexeme == name;
      });
  return found == program.classInstances().end() ? nullptr : &*found;
}

const lang::MirFunctionInstance *findMirFunction(const lang::HirProgram &hir,
                                                 const lang::MirProgram &mir,
                                                 std::string_view name) {
  const lang::HirFunctionInstance *source = findHirFunction(hir, name);
  return source == nullptr ? nullptr : mir.findFunctionInstance(source->id);
}

const lang::MirClassInstance *findMirClass(const lang::HirProgram &hir,
                                           const lang::MirProgram &mir,
                                           std::string_view name) {
  const lang::HirClassInstance *source = findHirClass(hir, name);
  return source == nullptr ? nullptr : mir.findClassInstance(source->id);
}

const lang::MirDestructorInstance *
findMirDestructor(const lang::HirProgram &hir, const lang::MirProgram &mir,
                  std::string_view className) {
  const lang::HirClassInstance *source = findHirClass(hir, className);
  return source == nullptr || !source->destructor
             ? nullptr
             : mir.findDestructorInstance(*source->destructor);
}

lang::OptimizedProgram optimize(const lang::FrontendResult &frontend,
                                lang::OptimizationLevel level,
                                const lang::OptimizationResult &compatibility) {
  return lang::OptimizationPipeline().run({.hir = frontend.hir,
                                           .mir = frontend.mir,
                                           .level = level,
                                           .compatibility = &compatibility});
}

lang::BackendArtifact emit(const lang::FrontendResult &frontend,
                           const lang::MirProgram &mir,
                           const lang::OptimizationResult &compatibility) {
  static_cast<void>(compatibility);
  return gti_test::emitCpp(frontend, frontend.mir, mir);
}

std::vector<const lang::MirInstruction *>
instructionsOfKind(const lang::MirBody &body, lang::MirInstructionKind kind) {
  std::vector<const lang::MirInstruction *> result;
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == kind) {
        result.push_back(&instruction);
      }
    }
  }
  return result;
}

bool isEmptyCleanupClass(const lang::HirClassInstance *hirClass,
                         const lang::MirClassInstance *mirClass) {
  return hirClass != nullptr && mirClass != nullptr &&
         hirClass->kind == lang::ClassKind::Class &&
         mirClass->kind == lang::ClassKind::Class &&
         hirClass->typeArguments.empty() && hirClass->valueArguments.empty() &&
         hirClass->bases.empty() && hirClass->fields.empty() &&
         !hirClass->abstract && !hirClass->polymorphic &&
         hirClass->destructor && hirClass->requiresActiveDropState &&
         hirClass->requiresActiveCleanup && mirClass->bases.empty() &&
         mirClass->structuralBases.empty() &&
         mirClass->declaredFields.empty() && mirClass->fields.empty() &&
         mirClass->fieldDropOrder.empty() && !mirClass->abstract &&
         !mirClass->polymorphic && mirClass->destructor &&
         mirClass->requiresActiveDropState && mirClass->requiresActiveCleanup;
}

bool destructorIsBounded(const lang::MirDestructorInstance *destructor) {
  if (destructor == nullptr ||
      destructor->definitionKind != lang::MirDefinitionKind::Source ||
      destructor->mayRaiseDefinedFailure ||
      destructor->body.kind != lang::MirBodyKind::Destructor ||
      !destructor->body.loans.empty() ||
      !destructor->body.dropObligations.empty() ||
      !destructor->body.cleanupBoundaries.empty() ||
      !destructor->body.failureRecords.empty()) {
    return false;
  }
  std::size_t assignments = 0;
  std::size_t literals = 0;
  for (const lang::MirBlock &block : destructor->body.blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Return) {
      return false;
    }
    for (const lang::MirInstruction &instruction : block.instructions) {
      assignments += instruction.kind == lang::MirInstructionKind::Assign;
      literals += instruction.kind == lang::MirInstructionKind::Compute &&
                  instruction.operation == lang::MirOperation::Literal;
      if (instruction.kind == lang::MirInstructionKind::Call ||
          instruction.kind == lang::MirInstructionKind::Construct ||
          instruction.kind == lang::MirInstructionKind::Drop ||
          instruction.kind == lang::MirInstructionKind::Borrow ||
          instruction.synchronization.kind !=
              lang::SynchronizationOperationKind::None ||
          !instruction.definedFailure.empty()) {
        return false;
      }
    }
  }
  return assignments == 1 && literals == 1;
}

void expectSelectedDefinitions(std::string_view generated) {
  expect(count(generated, destructorMarker) == 4,
         "the Early, Late, ExplicitDefault, and FieldOwner destructors "
         "should be MIR-emitted");

  const std::string_view selected =
      functionDefinition(generated, "selected_default_cleanup");
  expect(selected.find(functionMarker) != std::string_view::npos,
         "selected_default_cleanup should carry MIR body authority");
  expect(count(selected, "::gti_internal::backend::mir_lifetime_slot<") == 2,
         "each selected class local should use the non-RAII MIR lifetime slot");
  expect(count(selected, ".construct()") == 2,
         "each generated-default Construct should construct its final slot");
  expect(count(selected, ".destroy()") == 2,
         "each normal MIR Drop should explicitly destroy its final slot");
  expect(selected.find(".destroy_if_engaged()") == std::string_view::npos,
         "cleanup-owning slots must not replace a missing semantic Drop with "
         "representation-only rearming");
  expect(selected.find("std::optional<") == std::string_view::npos &&
             selected.find("std::move(") == std::string_view::npos &&
             selected.find("Early early") == std::string_view::npos &&
             selected.find("Late late") == std::string_view::npos,
         "selected cleanup must not rely on optional, an unmodeled move, or "
         "native automatic RAII");

  expect(lifecycleDefinition(generated, "Early").find(destructorMarker) !=
                 std::string_view::npos &&
             lifecycleDefinition(generated, "Late").find(destructorMarker) !=
                 std::string_view::npos,
         "both selected lifecycle helpers should execute destructor MIR");

  // Ordinary lifetimes the whole-family contract had to reject now emit
  // per body through the same slot vocabulary.
  constexpr std::array<std::string_view, 3> migratedFunctions = {
      "compatibility_declared_constructor", "compatibility_nested_scope",
      "compatibility_branch"};
  for (const std::string_view name : migratedFunctions) {
    const std::string_view body = functionDefinition(generated, name);
    expect(body.find(functionMarker) != std::string_view::npos &&
               body.find(".construct()") != std::string_view::npos &&
               body.find(".destroy()") != std::string_view::npos,
           std::string{"the migrated function should emit through the slot "
                       "vocabulary: "} +
               std::string{name});
  }
  const std::string_view fieldOwner = functionDefinition(
      generated, "compatibility_field_owner__gti_mir_failure");
  const auto construction = fieldOwner.find(".construction_address()");
  const auto successGuard = fieldOwner.find("if (__gti_mir_construct_success_");
  const auto activation = fieldOwner.find(".mark_constructed()");
  const auto destruction = fieldOwner.find(".destroy()");
  expect(fieldOwner.find(failureFunctionMarker) != std::string_view::npos &&
             construction != std::string_view::npos &&
             successGuard != std::string_view::npos &&
             activation != std::string_view::npos &&
             destruction != std::string_view::npos &&
             construction < successGuard && successGuard < activation &&
             activation < destruction &&
             fieldOwner.find("::new (") != std::string_view::npos &&
             fieldOwner.find("mir_failure_constructor_tag_v1<") !=
                 std::string_view::npos &&
             fieldOwner.find("return false;") != std::string_view::npos &&
             functionDefinition(generated, "compatibility_field_owner").empty(),
         "the field-owning function must placement-construct its selected "
         "generated body, activate only on success, and drain its slot only "
         "through the explicit failure-form body");
  expect(functionDefinition(generated, "compatibility_checked_cleanup")
                 .find(".construct()") == std::string_view::npos,
         "the checked-cleanup near miss should remain compatible");
  // Per-body admission also emits the ordinary destructors the dissolved
  // route's whole-family contract had to reject.
  constexpr std::array<std::string_view, 2> migratedDestructors = {
      "ExplicitDefault", "FieldOwner"};
  for (const std::string_view name : migratedDestructors) {
    expect(lifecycleDefinition(generated, name).find(destructorMarker) !=
               std::string_view::npos,
           std::string{"the migrated destructor should execute MIR: "} +
               std::string{name});
  }
  expect(
      lifecycleDefinition(generated, "CheckedCleanup").find(destructorMarker) ==
          std::string_view::npos,
      "the checked-arithmetic destructor near miss should remain "
      "compatible: CheckedCleanup");

  expect(generated.find("verified MIR lifetime slot escaped without Drop") !=
             std::string_view::npos,
         "the generated lifetime slot must abort instead of hiding a missing "
         "MIR Drop");
}

void testFamily(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the class-default-cleanup fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirClassInstance *earlyHir = findHirClass(frontend.hir, "Early");
  const lang::HirClassInstance *lateHir = findHirClass(frontend.hir, "Late");
  expect(isEmptyCleanupClass(
             earlyHir, findMirClass(frontend.hir, frontend.mir, "Early")) &&
             isEmptyCleanupClass(
                 lateHir, findMirClass(frontend.hir, frontend.mir, "Late")),
         "the selected classes should be exact empty concrete cleanup owners");
  expect(destructorIsBounded(
             findMirDestructor(frontend.hir, frontend.mir, "Early")) &&
             destructorIsBounded(
                 findMirDestructor(frontend.hir, frontend.mir, "Late")),
         "the selected destructor bodies should be failure-free scalar global "
         "assignments");

  const lang::HirFunctionInstance *hirSelected =
      findHirFunction(frontend.hir, "selected_default_cleanup");
  const lang::MirFunctionInstance *selected =
      findMirFunction(frontend.hir, frontend.mir, "selected_default_cleanup");
  expect(hirSelected != nullptr && selected != nullptr &&
             selected->body.kind == lang::MirBodyKind::Function &&
             !selected->mayRaiseDefinedFailure,
         "the selected source function should have a no-failure MIR body");
  if (hirSelected == nullptr || selected == nullptr) {
    return;
  }

  const auto constructs =
      instructionsOfKind(selected->body, lang::MirInstructionKind::Construct);
  const auto initializes =
      instructionsOfKind(selected->body, lang::MirInstructionKind::Initialize);
  const auto drops =
      instructionsOfKind(selected->body, lang::MirInstructionKind::Drop);
  expect(constructs.size() == 2 && initializes.size() == 2 && drops.size() == 2,
         "two generated-default locals should retain two Construct, "
         "Initialize, and Drop instructions");
  expect(std::all_of(constructs.begin(), constructs.end(),
                     [](const auto *item) {
                       return item->result && !item->constructorTarget &&
                              item->constructorKind ==
                                  lang::ConstructorKind::Ordinary &&
                              item->operands.empty() &&
                              item->parameterTypes.empty();
                     }),
         "the selected construction must be operand-free generated default, "
         "not an ordinary constructor call");

  std::vector<const lang::MirDropObligation *> bindings;
  for (const lang::MirDropObligation &obligation :
       selected->body.dropObligations) {
    if (obligation.kind == lang::MirDropObligationKind::Binding) {
      bindings.push_back(&obligation);
    }
  }
  expect(bindings.size() == 2 &&
             std::all_of(bindings.begin(), bindings.end(),
                         [](const auto *item) {
                           return item->hirObligation != 0 &&
                                  item->binding != 0 && item->place != 0 &&
                                  item->dropType.classInstance &&
                                  item->dropType.destructor &&
                                  item->dropType.requiresActiveCleanup;
                         }),
         "the final local slots should retain exact binding cleanup "
         "obligations");
  expect(selected->body.cleanupBoundaries.size() == 1 &&
             selected->body.cleanupBoundaries.front().kind ==
                 lang::MirCleanupBoundaryKind::Normal &&
             selected->body.cleanupBoundaries.front().obligations.size() == 2,
         "the return path should carry one normal reverse-order cleanup "
         "boundary");
  if (selected->body.cleanupBoundaries.size() == 1) {
    const auto &obligations =
        selected->body.cleanupBoundaries.front().obligations;
    const lang::MirDropObligation *first =
        obligations.empty()
            ? nullptr
            : selected->body.findDropObligation(obligations.front());
    const lang::MirDropObligation *last =
        obligations.empty()
            ? nullptr
            : selected->body.findDropObligation(obligations.back());
    expect(first != nullptr && last != nullptr &&
               first->constructionOrder > last->constructionOrder,
           "normal cleanup should destroy Late before Early");
  }

  const lang::OptimizationPipeline pipeline;
  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    const lang::OptimizationResult compatibility =
        pipeline.run(frontend.hir, level);
    const lang::OptimizedProgram optimized =
        optimize(frontend, level, compatibility);
    expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
           "class-default-cleanup MIR should verify at O0, O1, and O3");
    if (optimized.valid() && lang::verifyMirProgram(optimized.mir).valid()) {
      expectSelectedDefinitions(
          emit(frontend, optimized.mir, compatibility).contents);
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gti_mir_backend_class_default_cleanup_tests "
                 "<fixture>\n";
    return 2;
  }
  testFamily(argv[1]);
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  return 0;
}
