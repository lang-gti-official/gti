#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
using namespace gti_test::cpp_mir_body_emitter_fixture;

void checkMutations(const lang::FrontendResult &frontend) {
  std::size_t reservations = 0;
  bool nested = false;
  bool primitiveCompound = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::Add &&
            instruction.hirValue != 0) {
          const auto *hir = frontend.hir.findFunctionInstance(function.id);
          const auto *value = hir == nullptr
                                  ? nullptr
                                  : hir->body.findValue(instruction.hirValue);
          primitiveCompound =
              primitiveCompound ||
              (value != nullptr &&
               value->operation == lang::TokenKind::PLUS_EQUAL &&
               value->functionTarget.has_value());
        }
      }
    for (const auto &reservation : function.body.loans) {
      if (!reservation.twoPhase)
        continue;
      ++reservations;
      nested = nested || reservation.parent != 0;
      for (int mutation = 0; mutation < 4; ++mutation) {
        auto changed = frontend.mir;
        auto *body = &const_cast<lang::MirFunctionInstance *>(
                          changed.findFunctionInstance(function.id))
                          ->body;
        if (mutation == 0) {
          for (auto &block : body->blocks)
            std::erase_if(block.instructions, [&](const auto &instruction) {
              return instruction.kind ==
                         lang::MirInstructionKind::ActivateBorrow &&
                     instruction.loan == reservation.id;
            });
        } else if (mutation == 1) {
          body->loans[reservation.id - 1].access = lang::AccessMode::ReadOnly;
        } else if (mutation == 2) {
          body->loans[reservation.id - 1].escapes = true;
        } else {
          bool changedEnd = false;
          for (auto &block : body->blocks)
            for (auto &instruction : block.instructions) {
              if (!changedEnd &&
                  instruction.kind == lang::MirInstructionKind::EndBorrow &&
                  instruction.loan && *instruction.loan != reservation.id &&
                  body->loans[*instruction.loan - 1].access ==
                      lang::AccessMode::ReadOnly &&
                  body->loans[*instruction.loan - 1].kind ==
                      lang::MirLoanKind::CallResult) {
                instruction.loan = reservation.id;
                changedEnd = true;
              }
            }
          if (!changedEnd)
            continue;
        }
        (void)lang::rebuildMirValueUses(*body);
        expect(!lang::verifyMirProgram(changed).valid(),
               "missing activation, access upgrade, escape or premature loan "
               "end is rejected");
      }
    }
  }
  expect(reservations >= 3 && nested && primitiveCompound,
         "fixture exercises nested reservations and primitive updates of exact "
         "accessor results");
}

void checkCheckedReferenceFailures(const lang::FrontendResult &frontend) {
  bool scalar = false;
  bool nominal = false;
  std::size_t count = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Borrow ||
            instruction.localFailureSites.empty())
          continue;
        ++count;
        scalar = scalar || instruction.info.type == lang::SemanticType::Int32;
        nominal =
            nominal || instruction.info.type.kind == lang::SemanticType::Class;
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "checked reference formation owns its exact Invoke before "
               "publication");
        for (int mutation = 0; mutation < 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = &const_cast<lang::MirFunctionInstance *>(
                            malformed.findFunctionInstance(function.id))
                            ->body;
          auto *producer = body->findBlock(block.id);
          if (mutation == 0) {
            const auto success = producer->terminator.target;
            producer->terminator = {.kind = lang::MirTerminatorKind::Goto,
                                    .target = success};
          } else if (mutation == 1) {
            producer->instructions.back().definedFailure.localOrigins.clear();
          } else {
            auto *failure = body->findBlock(producer->terminator.elseTarget);
            std::erase_if(failure->instructions, [&](const auto &cleanup) {
              return cleanup.kind == lang::MirInstructionKind::EndBorrow &&
                     cleanup.loan == instruction.loan;
            });
          }
          lang::rebuildMirReachability(*body);
          (void)lang::rebuildMirValueUses(*body);
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "checked reference rejects lost routing, origin or "
                 "failed-publication retirement");
        }
      }
    }
  }
  expect(count >= 3 && scalar && nominal,
         "ordinary scalar and class reference results share checked-place "
         "failure authority");
}

void checkNamedPlaceStores(const lang::FrontendResult &frontend) {
  std::size_t stores = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (hir == nullptr)
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *value = hir->body.findValue(instruction.hirValue);
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            value == nullptr || value->kind != lang::HirValueKind::PlaceSet)
          continue;
        ++stores;
        const auto *destination =
            function.body.findPlace(instruction.destination);
        expect(destination != nullptr &&
                   destination->root == lang::MirPlaceRootKind::Loan &&
                   destination->access == lang::AccessMode::Mutable,
               "a named accessor store uses its exact mutable returned loan");
        if (destination == nullptr || destination->loan == 0)
          continue;
        auto malformed = frontend.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         malformed.findFunctionInstance(function.id))
                         ->body;
        body.loans[destination->loan - 1].access = lang::AccessMode::ReadOnly;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "MIR independently rejects upgrading a read-only accessor loan "
               "to a writable destination");
      }
    }
  }
  expect(stores >= 2, "named plain and primitive compound stores reach MIR");
}

void checkNegative(std::string declarations, std::string body,
                   std::string_view expectedCode = {},
                   std::string_view expectedMessage = {}) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto result = lang::Frontend().analyze(
      "operator-place-negative.gti",
      std::move(declarations) + "int main(){" + body + "}",
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  const auto error = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const auto &diagnostic) {
        return diagnostic.severity == lang::DiagnosticSeverity::Error &&
               diagnostic.phase == lang::DiagnosticPhase::Semantics;
      });
  expect(!result.canGenerateCode() && error != result.diagnostics.end(),
         "invalid access and retained overlap are diagnosed by semantics");
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-B0001";
                      }),
         "invalid source must not reach an internal MIR/backend error");
  if (!expectedCode.empty()) {
    const auto located = std::find_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &diagnostic) {
          return diagnostic.code == expectedCode &&
                 diagnostic.message.find(expectedMessage) != std::string::npos;
        });
    expect(located != result.diagnostics.end() &&
               located->primary.source == "operator-place-negative.gti" &&
               located->primary.end > located->primary.start,
           "the invalid place has its owning semantic diagnostic and exact "
           "source identity");
  }
  if (error == result.diagnostics.end())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
} // namespace

int main() {
  using namespace gti_test::cpp_mir_body_emitter_fixture;
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/operator_place_composition.gti";
  std::ifstream stream(fixture);
  const std::string source((std::istreambuf_iterator<char>(stream)), {});
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "operator-place composition satisfies frontend and complete MIR "
         "verification");
  if (!frontend.canGenerateCode())
    return 1;
  checkMutations(frontend);
  checkNamedPlaceStores(frontend);
  checkCheckedReferenceFailures(frontend);
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
                     lang::OptimizationLevel::O3}) {
    const auto optimized =
        lang::OptimizationPipeline().run({.hir = frontend.hir,
                                          .mir = frontend.mir,
                                          .level = level,
                                          .target = lang::TargetInfo::host()});
    expect(
        optimized.valid() && lang::verifyMirProgram(optimized.mir).valid(),
        "optimization preserves exact update places and reservation cleanup");
    const auto lowered = gti_test::lowerProgram(
        frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
        optimized.report.transformations);
    expect(lang::verifyLoweredProgram(lowered).empty(),
           "production lowered snapshot is coherent");
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      lang::CppMirBodyEmissionMap rows(
          lang::buildCppMirBodyEmissionMapRows(lowered, standard));
      lang::CppMirBodyEmitter emitter(optimized.mir, rows);
      for (const auto &function : optimized.mir.functionInstances()) {
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        expect(emitter.analyze(address).ready() &&
                   (emitter.supportsBodyText(address) ||
                    emitter.supportsFailureBodyText(address)),
               "production backend supports verified operator-place schedules");
      }
    }
  }
  const auto declarations = source.substr(0, source.find("int main()"));
  const auto global = analyzeWithStandardLibrary(
      "global-operator-place.gti",
      declarations + "mut Grid global{}; int main(){"
                     "global[std::size_t(0)][std::size_t(0)] = 2; return 0; }");
  expect(global.canGenerateCode(),
         "nested global target retains its ordinary exclusive schedule");
  std::size_t globalConsumers = 0;
  for (const auto &function : global.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    ++globalConsumers;
    expect(std::none_of(function.body.loans.begin(), function.body.loans.end(),
                        [](const auto &loan) { return loan.twoPhase; }),
           "a loan-rooted intermediate must not disguise a global as a local "
           "reservation");
  }
  expect(globalConsumers == 1, "global control checks exactly its entry body");
  checkNegative(declarations, "Cell cell{}; *cell = 2; return 0;");
  checkNegative(declarations,
                "mut Cell cell{}; cell.read_slot() = 2; return 0;", "GTI-S2002",
                "requires a mutable place");
  checkNegative(declarations, "mut Cell cell{}; cell.read() = 2; return 0;",
                "GTI-S2002", "requires a mutable place");
  checkNegative(declarations, "Cell cell{}; cell.slot() = 2; return 0;");
  checkNegative(declarations, "Cell().slot() = 2; return 0;");
  checkNegative(declarations, "mut Cell cell{}; cell.slot() = true; return 0;");
  checkNegative(declarations,
                "mut Cell cell{}; cell.slot() = cell.read(); return 0;",
                "GTI-S2017", "still mutably borrowed");
  checkNegative(declarations,
                "mut Cell cell{}; cell.slot() += cell.read(); return 0;");
  checkNegative(declarations,
                "mut Cell cell{}; int32_t& held = cell.read_slot(); "
                "cell.slot() = 4; return held;");
  checkNegative(declarations, "mut Cell cell{}; (cell.read()) = 2; return 0;");
  checkNegative(declarations, "1 = 2; return 0;");
  checkNegative(
      declarations,
      "mut Cell cell{}; mut int32_t& held = *cell; *cell = 2; return held;");
  checkNegative(
      declarations,
      "mut Grid grid{}; mut Row& held = grid[std::size_t(0)]; "
      "grid[std::size_t(1)][std::size_t(0)] = 2; return held[std::size_t(0)];");
  const std::string sentinelDeclarations = R"(
#include <std/iterator>
class End {};
class Cursor {
  mut int32_t current = 0;
public:
  int32_t& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(End& end) { return true; }
};
)";
  checkNegative(sentinelDeclarations,
                "mut Cursor cursor{}; bool compared = cursor != End(); "
                "return compared ? 1 : 0;",
                "GTI-S2022", "No exact overload");
  checkNegative("float identity(float value){return value;}",
                "float result = identity(16777217); return int32_t(result);",
                "GTI-S2012", "exactly matches");
  checkNegative("class Scalar{ public: Scalar(float value){} };",
                "Scalar result = Scalar(16777217); return 0;", "GTI-S2012",
                "exactly matches");
  checkNegative("",
                "double value = double(1.0); float narrowed = value; return 0;",
                "GTI-S2003", "initialize");
  return failures == 0 ? 0 : 1;
}
