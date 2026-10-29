#include "../src/compiler/mir_verification_queries.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
using namespace gti_test::cpp_mir_body_emitter_fixture;

std::string readFixture(std::string_view name) {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                    (std::string(name) + ".gti");
  std::ifstream stream(path);
  return {(std::istreambuf_iterator<char>(stream)), {}};
}

void checkBackend(const lang::FrontendResult &frontend,
                  std::string_view fixture) {
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
                     lang::OptimizationLevel::O3}) {
    auto optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = level,
                                          .target = lang::TargetInfo::host()});
    expect(optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
           "optimizer retains exact field, view and allocated-value authority");
    auto lowered = gti_test::lowerProgram(
        frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
        optimized.report.transformations);
    expect(lang::verifyLoweredProgram(lowered).empty(),
           "lowered snapshot is coherent");
    for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      lang::CppMirBodyEmissionMap rows(
          lang::buildCppMirBodyEmissionMapRows(lowered, standard));
      lang::CppMirBodyEmitter emitter(optimized.mir, rows);
      for (const auto &function : optimized.mir.functionInstances()) {
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        const bool admitted = emitter.analyze(address).ready() &&
                              (emitter.supportsBodyText(address) ||
                               emitter.supportsFailureBodyText(address));
        if (!admitted)
          std::cerr << "Backend rejection: " << fixture << ", function "
                    << function.id << ", declaration " << function.declaration
                    << ", level " << static_cast<int>(level) << ", standard "
                    << static_cast<int>(standard) << '\n';
        expect(admitted,
               "both production standard modes admit each exact function "
               "schedule");
      }
    }
  }
}

void checkAllocationMutations(const lang::FrontendResult &frontend) {
  std::size_t count = 0;
  std::size_t sameBlockBorrowAllocations = 0;
  bool copy = false, move = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!lang::mir_detail::generatedUniqueConstruction(instruction))
          continue;
        ++count;
        copy |= instruction.constructorKind == lang::ConstructorKind::Copy;
        move |= instruction.constructorKind == lang::ConstructorKind::Move;
        expect(instruction.objectAllocation != 0 &&
                   instruction.definedFailure.empty(),
               "generated construction follows a distinct checked allocation");
        for (const auto &operand : instruction.operands) {
          if (operand.kind != lang::MirOperandKind::Value)
            continue;
          const auto *value = function.body.findValue(operand.value);
          if (value != nullptr &&
              std::any_of(block.instructions.begin(), block.instructions.end(),
                          [&](const auto &candidate) {
                            return candidate.id == value->definition &&
                                   candidate.kind ==
                                       lang::MirInstructionKind::CallInput &&
                                   candidate.callInputKind ==
                                       lang::HirCallInputKind::ReadBorrow;
                          }))
            ++sameBlockBorrowAllocations;
        }
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = &const_cast<lang::MirFunctionInstance *>(
                            malformed.findFunctionInstance(function.id))
                            ->body;
          auto *changed = body->findBlock(block.id);
          auto producer = std::find_if(changed->instructions.begin(),
                                       changed->instructions.end(),
                                       [&](const auto &candidate) {
                                         return candidate.id == instruction.id;
                                       });
          const auto *allocation =
              body->findDropObligation(instruction.objectAllocation);
          if (mutation == 0) {
            producer->objectAllocation = 0;
          } else if (mutation == 1) {
            producer->constructorKind = lang::ConstructorKind::Ordinary;
          } else if (mutation == 2) {
            producer->parameterTypes.front() = lang::SemanticType::Int32;
          } else if (mutation == 3) {
            producer->operands.front().kind = lang::MirOperandKind::BorrowRead;
            producer->operands.front().place = allocation->place;
          } else if (mutation == 4) {
            for (auto &candidate : body->blocks)
              std::erase_if(candidate.instructions, [&](const auto &operation) {
                return operation.kind == lang::MirInstructionKind::Lifecycle &&
                       operation.objectAllocation ==
                           instruction.objectAllocation;
              });
          } else if (mutation == 5) {
            std::size_t changedAllocations = 0;
            for (auto &candidate : body->blocks)
              for (auto &operation : candidate.instructions)
                if (operation.kind ==
                        lang::MirInstructionKind::AllocateObject &&
                    operation.objectAllocation ==
                        instruction.objectAllocation) {
                  // R0011 is an immediate abort, not an Invoke predecessor.
                  // Erasing its detector contract must still fail closed.
                  operation.definedFailure.localOrigins.clear();
                  ++changedAllocations;
                }
            expect(changedAllocations == 1,
                   "allocation mutation must change its exact abort detector");
          } else if (mutation == 6) {
            auto owner = std::find_if(
                malformed.classInstances().begin(),
                malformed.classInstances().end(), [&](const auto &candidate) {
                  return candidate.type ==
                         instruction.info.type.arguments.front();
                });
            auto &changedOwner = const_cast<lang::MirClassInstance &>(*owner);
            if (instruction.constructorKind == lang::ConstructorKind::Copy)
              changedOwner.copyConstructor = lang::SpecialMemberStatus::Deleted;
            else
              changedOwner.moveConstructor = lang::SpecialMemberStatus::Deleted;
          } else {
            const auto *input =
                body->findValue(instruction.operands.front().value);
            expect(input != nullptr,
                   "generated allocation has a prepared input to corrupt");
            std::size_t changedInputs = 0;
            for (auto &candidate : body->blocks)
              for (auto &operation : candidate.instructions)
                if (input != nullptr && operation.id == input->definition &&
                    operation.kind == lang::MirInstructionKind::CallInput) {
                  operation.callSite = 0;
                  ++changedInputs;
                }
            expect(changedInputs == 1,
                   "allocation mutation detaches one exact prepared input");
          }
          lang::rebuildMirReachability(*body);
          (void)lang::rebuildMirValueUses(*body);
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "allocation rejects forged operation, input, policy, storage "
                 "escape or publication");
        }
      }
  expect(count >= 4 && copy && move,
         "unrelated passive/owning types exercise generated copy and move");
  expect(sameBlockBorrowAllocations != 0,
         "abort-only allocation consumes a borrow staged in the same block");
  std::cout << "Generated allocation: " << count << " operations, " << count * 8
            << " rejected mutations\n";
}

void checkFieldMutations(const lang::FrontendResult &frontend) {
  std::size_t count = 0;
  std::size_t accessMutations = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.body.rangeIterations.empty())
      continue;
    for (const auto &place : function.body.places) {
      if (place.root != lang::MirPlaceRootKind::Binding ||
          place.projections.size() < 2 ||
          place.projections.front().kind !=
              lang::MirProjectionKind::Dereference ||
          place.projections.back().kind != lang::MirProjectionKind::Field)
        continue;
      ++count;
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = &const_cast<lang::MirFunctionInstance *>(
                          malformed.findFunctionInstance(function.id))
                          ->body;
        auto &changed = body->places[place.id - 1];
        if (mutation == 0)
          changed.binding = 0;
        else if (mutation == 1)
          changed.projections.back().field = 0;
        else if (mutation == 2)
          changed.projections.erase(changed.projections.begin());
        else
          changed.type = lang::SemanticType::Int32;
        (void)lang::rebuildMirValueUses(*body);
        bool rejected = !lang::verifyMirProgram(malformed).valid();
        if (!rejected) {
          const auto lowered = gti_test::lowerProgram(
              frontend, malformed, malformed, lang::TargetInfo::host());
          lang::CppMirBodyEmissionMap rows(lang::buildCppMirBodyEmissionMapRows(
              lowered, lang::CppStandard::Cpp20));
          lang::CppMirBodyEmitter emitter(malformed, rows);
          rejected = !emitter
                          .analyze({.kind = lang::MirBodyKind::Function,
                                    .owner = function.id})
                          .ready();
        }
        expect(rejected, "wrong range owner, field, indirection or referent "
                         "type fails closed");
      }
    }
    for (const auto &range : function.body.rangeIterations) {
      for (int mutation = 0; mutation != 2; ++mutation) {
        auto malformed = frontend.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         malformed.findFunctionInstance(function.id))
                         ->body;
        auto &changed = body.rangeIterations[range.id - 1];
        if (mutation == 0)
          changed.element = 0;
        else
          body.loans[range.rangeLoan - 1].access = lang::AccessMode::Mutable;
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "wrong element address, carrier or owner-loan access fails closed");
      }
    }
    // Merely changing an unused place annotation is not an access grant.
    // Forge the executing receiver borrow instead: it must not gain mutable
    // authority from the enclosing read-only range loan.
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Borrow &&
            instruction.loan != 0 && instruction.operands.size() == 1 &&
            instruction.operands.front().kind ==
                lang::MirOperandKind::BorrowRead &&
            function.body.findLoan(*instruction.loan)->access ==
                lang::AccessMode::ReadOnly &&
            function.body.findPlace(instruction.operands.front().place)
                    ->access == lang::AccessMode::ReadOnly) {
          ++accessMutations;
          auto malformed = frontend.mir;
          auto &body = const_cast<lang::MirFunctionInstance *>(
                           malformed.findFunctionInstance(function.id))
                           ->body;
          auto &changed =
              *std::find_if(body.findBlock(block.id)->instructions.begin(),
                            body.findBlock(block.id)->instructions.end(),
                            [&](const auto &candidate) {
                              return candidate.id == instruction.id;
                            });
          changed.operands.front().kind = lang::MirOperandKind::BorrowWrite;
          body.loans[*instruction.loan - 1].access = lang::AccessMode::Mutable;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "executing receiver cannot upgrade a read-only range borrow");
        }
  }
  expect(count >= 2,
         "range fields retain explicit typed reference dereference paths");
  expect(
      accessMutations != 0,
      "an executing read-only borrow exercises the access-upgrade rejection");
  std::cout << "Range field: " << count << " addresses, " << count * 4
            << " rejected shape mutations, " << accessMutations
            << " rejected executing access upgrades\n";
}

bool emitterAdmits(const lang::FrontendResult &frontend,
                   const lang::MirProgram &mir,
                   decltype(lang::MirBodyAddress{}.owner) function) {
  const auto lowered =
      gti_test::lowerProgram(frontend, mir, mir, lang::TargetInfo::host());
  lang::CppMirBodyEmissionMap rows(
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp20));
  lang::CppMirBodyEmitter emitter(mir, rows);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function};
  return emitter.analyze(address).ready() &&
         (emitter.supportsBodyText(address) ||
          emitter.supportsFailureBodyText(address));
}

// A view observer may read a call-produced SSA view directly; forged
// receiver category, type, identity or failure contract must fail closed.
void checkViewObserverMutations(const lang::FrontendResult &frontend) {
  std::size_t count = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if ((instruction.intrinsic != lang::IntrinsicKind::StringViewSize &&
             instruction.intrinsic != lang::IntrinsicKind::StringViewEmpty) ||
            !instruction.receiver ||
            instruction.receiver->kind != lang::MirOperandKind::Value)
          continue;
        const auto *producer =
            function.body.findValue(instruction.receiver->value);
        const auto *definition = producer == nullptr
                                     ? nullptr
                                     : [&]() -> const lang::MirInstruction * {
          for (const auto &candidate : function.body.blocks)
            for (const auto &operation : candidate.instructions)
              if (operation.result && *operation.result == producer->id)
                return &operation;
          return nullptr;
        }();
        if (definition == nullptr ||
            definition->kind == lang::MirInstructionKind::CallInput)
          continue;
        ++count;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = frontend.mir;
          auto &body = const_cast<lang::MirFunctionInstance *>(
                           malformed.findFunctionInstance(function.id))
                           ->body;
          auto &changed =
              *std::find_if(body.findBlock(block.id)->instructions.begin(),
                            body.findBlock(block.id)->instructions.end(),
                            [&](const auto &candidate) {
                              return candidate.id == instruction.id;
                            });
          if (mutation == 0) {
            changed.receiver->type = lang::SemanticType::Int32;
          } else if (mutation == 1) {
            changed.receiver->kind = lang::MirOperandKind::Copy;
          } else if (mutation == 2) {
            changed.receiver->value = *instruction.result;
          } else {
            changed.definedFailure.propagation =
                lang::FailurePropagationKind::Constructor;
          }
          (void)lang::rebuildMirValueUses(body);
          expect(!lang::verifyMirProgram(malformed).valid() ||
                     !emitterAdmits(frontend, malformed, function.id),
                 "forged view observer receiver or failure contract fails "
                 "closed");
        }
      }
  expect(count >= 10,
         "call-produced views exercise direct size/empty observation");
  std::cout << "Returned view observers: " << count << " observers, "
            << count * 4 << " rejected mutations\n";
}

void checkNegative(const std::string &prefix, const std::string &extra) {
  auto result = analyzeWithStandardLibrary(
      "operator-relay-negative.gti", prefix + extra + "int main(){return 0;}");
  expect(!result.canGenerateCode() &&
             std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.phase ==
                                      lang::DiagnosticPhase::Semantics &&
                                  diagnostic.severity ==
                                      lang::DiagnosticSeverity::Error;
                         }),
         "invalid owner relay is rejected by semantics");
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-B0001";
                      }),
         "invalid source never reaches an internal MIR/backend failure");
}
} // namespace

int main() {
  auto range = analyzeWithStandardLibrary("range_field_access.gti",
                                          readFixture("range_field_access"));
  auto text = analyzeWithStandardLibrary("indexed_view_relay.gti",
                                         readFixture("indexed_view_relay"));
  auto allocation =
      analyzeWithStandardLibrary("existing_value_allocation.gti",
                                 readFixture("existing_value_allocation"));
  auto views = analyzeWithStandardLibrary(
      "returned_view_observers.gti", readFixture("returned_view_observers"));
  for (const auto &[fixture, frontend] :
       std::vector<std::pair<std::string_view, const lang::FrontendResult *>>{
           {"range_field_access", &range},
           {"indexed_view_relay", &text},
           {"existing_value_allocation", &allocation},
           {"returned_view_observers", &views}}) {
    expect(frontend->canGenerateCode() &&
               lang::verifyMirProgram(frontend->mir).valid(),
           "positive fixture completes all compiler phases");
    if (!frontend->canGenerateCode())
      return 1;
    checkBackend(*frontend, fixture);
  }
  checkFieldMutations(range);
  checkAllocationMutations(allocation);
  checkViewObserverMutations(views);
  const auto source = readFixture("indexed_view_relay");
  const auto prefix = source.substr(0, source.find("int main()"));
  checkNegative(prefix, "std::string_view bad(){Records local{}; return "
                        "local.at(std::size_t(0));}");
  checkNegative(prefix,
                "std::string_view bad(){return Records().at(std::size_t(0));}");
  checkNegative(prefix, "[[borrowed_from(first)]] Record& bad(Records& first, "
                        "Records& second){return second[std::size_t(0)];}");
  checkNegative(prefix, "void bad(){mut Records records{}; std::string_view "
                        "held=records.at(std::size_t(0)); records.add(); "
                        "std::println(held);}");
  // Direct observation does not relax view lifetimes or exclusivity.
  checkNegative("import std.string;\n",
                "std::size_t bad(){std::string_view held = "
                "std::string(\"tmp\").view(); return held.size();}");
  checkNegative("import std.string;\n",
                "std::size_t bad(){mut std::string text = std::string(\"ab\"); "
                "std::string_view held = text.view(); text.append(\"c\"); "
                "return held.size();}");
  return failures == 0 ? 0 : 1;
}
