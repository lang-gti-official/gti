#include "gti/ast_printer.h"
#include "gti/backend.h"
#include "gti/cpp_backend.h"
#include "gti/cpp_emitter.h"
#include "gti/executable_path.h"
#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/mir_printer.h"
#include "gti/optimizer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/standard_library.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasDiagnostic(const lang::SemanticVisitor &semantic,
                   const std::string &text) {
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                   const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                       const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

const lang::Diagnostic *
findDiagnosticByCode(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

bool hasRelatedDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                          const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      if (related.message.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

bool hasDiagnosticHint(const std::vector<lang::Diagnostic> &diagnostics,
                       const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const std::string &hint : diagnostic.hints) {
      if (hint.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

std::size_t countDiagnosticCode(const lang::SemanticVisitor &semantic,
                                const std::string &code) {
  std::size_t count = 0;
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

std::size_t
countDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti";
}

std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

bool hasLoanEndpoint(const lang::SemanticLoanInfo *loan,
                     lang::SemanticLoanEndKind kind,
                     const lang::Stmt *statement) {
  return loan != nullptr &&
         std::any_of(loan->endpoints.begin(), loan->endpoints.end(),
                     [&](const lang::SemanticLoanEndpoint &endpoint) {
                       return endpoint.kind == kind &&
                              endpoint.statement == statement;
                     });
}

const lang::FunctionDecl *findTopLevelFunction(const lang::Program &program,
                                               const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

const lang::ClassDecl *findTopLevelClass(const lang::Program &program,
                                         const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
  }
  return nullptr;
}

void testFrontendBackendAndOptimizationPipeline() {
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
  lang::FrontendResult frontend =
      lang::Frontend().analyze("frontend.gti", source);
  expect(frontend.canGenerateCode(),
         "the shared frontend should produce a checked program");
  expect(frontend.diagnostics.empty(),
         "valid frontend input should not produce diagnostics");
  expect(frontend.semantics.expressionCount() > 0,
         "the frontend should retain expression type information");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainInstance = &instance;
      break;
    }
  }
  expect(mainInstance != nullptr && mainInstance->body.roots.size() == 4 &&
             mainInstance->body.statements.size() >= 5,
         "typed HIR should retain an executable function body");
  const lang::HirStatement *conditional =
      mainInstance == nullptr
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[2]);
  expect(conditional != nullptr &&
             conditional->kind == lang::HirStatementKind::If &&
             conditional->condition.has_value() &&
             conditional->body.has_value(),
         "typed HIR should retain explicit branch and condition edges");

  const lang::HirValue *logicalValue = nullptr;
  if (mainInstance != nullptr) {
    for (const lang::HirValue &value : mainInstance->body.values) {
      if (value.kind == lang::HirValueKind::Logical) {
        logicalValue = &value;
        break;
      }
    }
  }
  expect(logicalValue != nullptr && logicalValue->operands.size() == 2,
         "typed HIR values should retain stable operand IDs");

  const lang::OptimizationResult unoptimized = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O0);
  expect(unoptimized.foldedExpressionCount() == 0,
         "-O0 should not rewrite expressions");

  const lang::OptimizationResult optimized = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O1);
  expect(optimized.foldedExpressionCount() > 0,
         "-O1 should record safe constant expressions");
  const lang::ConstantValue *foldedLogical =
      logicalValue == nullptr ? nullptr
                              : optimized.replacement(logicalValue->id);
  expect(
      foldedLogical != nullptr && std::get_if<bool>(foldedLogical) != nullptr &&
          *std::get_if<bool>(foldedLogical),
      "constant folding should consume typed HIR and key results by value ID");

  std::unique_ptr<lang::Backend> backend = std::make_unique<lang::CppBackend>();
  const lang::BackendArtifact artifact =
      backend->generate({.program = frontend.program,
                         .semantics = frontend.semantics,
                         .hir = frontend.hir,
                         .mir = frontend.mir,
                         .optimizations = optimized});
  expect(backend->name() == "cpp" &&
             artifact.kind == lang::BackendArtifactKind::Source &&
             artifact.extension == ".cpp",
         "the C++ emitter should be available through the backend contract");
  expect(artifact.contents.find("const bool folded = true") !=
             std::string::npos,
         "the C++ backend should consume optimization results");
  expect(artifact.contents.find(
             "const std::int32_t arithmetic = static_cast<std::int32_t>(3)") !=
             std::string::npos,
         "proven in-range integer arithmetic should fold to a typed constant");
  expect(artifact.contents.find("if (arithmetic == 3)") != std::string::npos &&
             artifact.contents.find("if ((arithmetic == 3))") ==
                 std::string::npos,
         "contextual binary conditions should not gain warning-producing "
         "parentheses in C++");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-frontend.gti", "int main() { return 0 }");
  expect(!invalid.canGenerateCode() && !invalid.syntaxValid,
         "the frontend should block code generation after syntax errors");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-P0001"),
         "the shared frontend should retain parser diagnostics");

  const std::filesystem::path overlayDirectory =
      std::filesystem::temp_directory_path();
  const std::filesystem::path overlayEntry =
      overlayDirectory / "gti-source-overlay-entry.gti";
  const std::filesystem::path overlayDependency =
      overlayDirectory / "gti-source-overlay-dependency.gti";
  const std::string dependencyKey =
      std::filesystem::weakly_canonical(overlayDependency).string();
  const lang::FrontendResult overlaid = lang::Frontend().analyze(
      overlayEntry,
      "#include \"gti-source-overlay-dependency.gti\"\n"
      "int main() { return dependency_value; }\n",
      {}, {{dependencyKey, "int dependency_value = 0;\n"}});
  expect(overlaid.canGenerateCode() && overlaid.diagnostics.empty(),
         "the frontend should analyze unsaved included-source overlays");
}

void testMirControlFlowAndOwnershipEffects() {
  const std::string source = R"(
class Resource {
  int32_t value;

public:
  Resource(int32_t initial) : value(initial) {}
  ~Resource() {}

  int32_t& read() {
    return this.value;
  }
};

class Bundle {
  Resource first;
  Resource second;

public:
  Bundle() : first(Resource(1)), second(Resource(2)) {}
};

class Gate {
  bool open;

public:
  Gate(bool value) : open(value) {}
  operator bool() { return this.open; }
};

int32_t inspect(int32_t& value) { return value; }

expected<int32_t, int32_t> checked(bool fail) {
  if (fail) {
    return unexpected(1);
  }
  return 2;
}

int32_t flow(bool stop) {
  Resource original = Resource(1);
  Resource moved = std::move(original);
  mut int32_t count = 0;
  while (count < 3) {
    count++;
    if (count == 1) {
      continue;
    }
    if (stop) {
      break;
    }
  }
  switch (count) {
  case 2:
    count += 1;
    break;
  default:
    break;
  }
  int32_t& count_ref = count;
  int32_t& value_ref = moved.read();
  Gate gate = Gate(stop);
  if (gate and inspect(count_ref) > 0) {
    count += 1;
  }
  expected<int32_t, int32_t> result = checked(stop);
  if (result) {
    count += 1;
  }
  return inspect(count_ref) + inspect(value_ref);
}

int main() { [[discard]] flow(true); }
)";

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "mir-foundation.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected MIR diagnostic: " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             frontend.mir.valid(),
         "valid typed HIR should lower to a validated MIR program");

  const lang::HirClassInstance *bundle = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Bundle") {
      bundle = &instance;
      break;
    }
  }
  const lang::MirClassInstance *loweredBundle =
      bundle == nullptr ? nullptr : frontend.mir.findClassInstance(bundle->id);
  expect(
      loweredBundle != nullptr && bundle->fields.size() == 2 &&
          loweredBundle->fieldDropOrder.size() == 2 &&
          loweredBundle->fieldDropOrder[0].field == bundle->fields[1].binding &&
          loweredBundle->fieldDropOrder[1].field == bundle->fields[0].binding &&
          loweredBundle->fieldDropOrder[0].symbol != 0 &&
          loweredBundle->fieldDropOrder[1].symbol != 0,
      "MIR class metadata should retain stable field symbols and reverse "
      "lexical drop order");

  const auto findBody = [&](std::string_view name) -> const lang::MirBody * {
    for (const lang::HirFunctionInstance &instance :
         frontend.hir.functionInstances()) {
      if (instance.source == nullptr ||
          instance.source->name().lexeme != name) {
        continue;
      }
      const lang::MirFunctionInstance *lowered =
          frontend.mir.findFunctionInstance(instance.id);
      return lowered == nullptr ? nullptr : &lowered->body;
    }
    return nullptr;
  };
  const lang::MirBody *flow = findBody("flow");
  const lang::MirBody *read = findBody("read");
  const lang::MirBody *mainBody = findBody("main");
  expect(flow != nullptr && read != nullptr && mainBody != nullptr,
         "MIR should preserve stable links to global and member functions");
  if (flow == nullptr || read == nullptr || mainBody == nullptr) {
    return;
  }

  const auto instructionCount = [](const lang::MirBody &body,
                                   lang::MirInstructionKind kind) {
    std::size_t count = 0;
    for (const lang::MirBlock &block : body.blocks) {
      count += static_cast<std::size_t>(
          std::count_if(block.instructions.begin(), block.instructions.end(),
                        [kind](const lang::MirInstruction &instruction) {
                          return instruction.kind == kind;
                        }));
    }
    return count;
  };
  const auto terminatorCount = [](const lang::MirBody &body,
                                  lang::MirTerminatorKind kind) {
    return static_cast<std::size_t>(
        std::count_if(body.blocks.begin(), body.blocks.end(),
                      [kind](const lang::MirBlock &block) {
                        return block.terminator.kind == kind;
                      }));
  };

  const lang::MirBlock *entryBlock = flow->findBlock(flow->entry);
  expect(entryBlock != nullptr &&
             std::all_of(flow->blocks.begin(), flow->blocks.end(),
                         [](const lang::MirBlock &block) {
                           return block.terminator.kind !=
                                  lang::MirTerminatorKind::None;
                         }) &&
             entryBlock->reachable,
         "MIR bodies should form validated CFGs with a reachable entry and "
         "explicit terminators");
  expect(terminatorCount(*flow, lang::MirTerminatorKind::Branch) >= 3 &&
             terminatorCount(*flow, lang::MirTerminatorKind::Switch) == 1 &&
             terminatorCount(*flow, lang::MirTerminatorKind::Goto) >= 3 &&
             terminatorCount(*flow, lang::MirTerminatorKind::Return) == 1,
         "if, loop, break, continue, and switch flow should lower to explicit "
         "MIR edges");
  bool hasImplicitZeroReturn = false;
  for (const lang::MirBlock &block : mainBody->blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Return ||
        !block.terminator.value ||
        block.terminator.value->kind != lang::MirOperandKind::Constant ||
        !block.terminator.value->literal) {
      continue;
    }
    const std::uint64_t *value =
        std::get_if<std::uint64_t>(&*block.terminator.value->literal);
    hasImplicitZeroReturn = value != nullptr && *value == 0;
  }
  expect(hasImplicitZeroReturn,
         "MIR should preserve main's defined implicit zero return");
  expect(instructionCount(*flow, lang::MirInstructionKind::Move) >= 1 &&
             instructionCount(*flow, lang::MirInstructionKind::Borrow) >= 1 &&
             instructionCount(*flow, lang::MirInstructionKind::Call) >= 3 &&
             instructionCount(*flow, lang::MirInstructionKind::EndBorrow) >=
                 2 &&
             instructionCount(*flow, lang::MirInstructionKind::Drop) >= 2,
         "MIR should make moves, borrows, calls, borrow ends, and lexical "
         "drops explicit");

  const auto operationCount = [&](lang::MirInstructionKind kind,
                                  lang::MirOperation operation) {
    std::size_t count = 0;
    for (const lang::MirBlock &block : flow->blocks) {
      count += static_cast<std::size_t>(
          std::count_if(block.instructions.begin(), block.instructions.end(),
                        [&](const lang::MirInstruction &instruction) {
                          return instruction.kind == kind &&
                                 instruction.operation == operation;
                        }));
    }
    return count;
  };
  expect(operationCount(lang::MirInstructionKind::Compute,
                        lang::MirOperation::Add) >= 1 &&
             operationCount(lang::MirInstructionKind::Compute,
                            lang::MirOperation::Less) >= 1 &&
             operationCount(lang::MirInstructionKind::Compute,
                            lang::MirOperation::ExpectedHasValue) >= 1 &&
             operationCount(lang::MirInstructionKind::Assign,
                            lang::MirOperation::AddAssign) >= 1 &&
             operationCount(lang::MirInstructionKind::Modify,
                            lang::MirOperation::PostIncrement) >= 1,
         "MIR should use backend-neutral scalar and mutation operations");

  const bool hasLogicalTemporary =
      std::any_of(flow->places.begin(), flow->places.end(),
                  [](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Temporary;
                  });
  const bool hasContextualBoolCall = std::any_of(
      flow->blocks.begin(), flow->blocks.end(),
      [](const lang::MirBlock &block) {
        return std::any_of(
            block.instructions.begin(), block.instructions.end(),
            [](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.receiver && instruction.result &&
                     instruction.info.type == lang::SemanticType::Bool;
            });
      });
  expect(hasLogicalTemporary && hasContextualBoolCall,
         "short-circuiting should use explicit CFG storage and the selected "
         "contextual bool call");

  bool validUseDef = flow->values.size() == flow->valueUses.size();
  bool hasInstructionUse = false;
  bool hasTerminatorUse = false;
  for (const lang::MirValue &value : flow->values) {
    const lang::MirBlock *definitionBlock =
        flow->findBlock(value.definitionBlock);
    validUseDef = validUseDef && definitionBlock != nullptr &&
                  value.definition != 0 &&
                  std::any_of(definitionBlock->instructions.begin(),
                              definitionBlock->instructions.end(),
                              [&](const lang::MirInstruction &instruction) {
                                return instruction.id == value.definition &&
                                       instruction.result == value.id;
                              });
    for (const lang::MirValueUse &use : flow->usesOf(value.id)) {
      validUseDef = validUseDef && use.value == value.id;
      hasInstructionUse = hasInstructionUse ||
                          use.kind == lang::MirValueUseKind::InstructionOperand;
      hasTerminatorUse =
          hasTerminatorUse || use.kind == lang::MirValueUseKind::Terminator;
    }
  }
  expect(validUseDef && hasInstructionUse && hasTerminatorUse,
         "every MIR value should have one definition and indexed instruction "
         "and terminator uses");

  bool cleanupBeforeReturn = false;
  for (const lang::MirBlock &block : flow->blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Return) {
      continue;
    }
    const bool endsBorrow = std::any_of(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::EndBorrow;
        });
    const bool dropsOwner =
        std::any_of(block.instructions.begin(), block.instructions.end(),
                    [](const lang::MirInstruction &instruction) {
                      return instruction.kind == lang::MirInstructionKind::Drop;
                    });
    cleanupBeforeReturn = endsBorrow && dropsOwner;
  }
  expect(cleanupBeforeReturn,
         "return blocks should end active borrows and drop lexical owners "
         "before transferring control");

  const auto escapingLoan = std::find_if(
      read->loans.begin(), read->loans.end(), [](const lang::MirLoan &loan) {
        return loan.kind == lang::MirLoanKind::Return && loan.escapes;
      });
  const lang::MirPlace *borrowedPlace =
      escapingLoan == read->loans.end() ? nullptr
                                        : read->findPlace(escapingLoan->source);
  expect(borrowedPlace != nullptr &&
             borrowedPlace->root == lang::MirPlaceRootKind::This &&
             std::any_of(borrowedPlace->projections.begin(),
                         borrowedPlace->projections.end(),
                         [](const lang::MirPlaceProjection &projection) {
                           return projection.kind ==
                                  lang::MirProjectionKind::Field;
                         }),
         "receiver-tied reference returns should retain an escaping loan from "
         "a projected 'this' place");
}

void testDefiniteReturnAnalysis() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("definite-return.gti", R"(
int choose(bool first) {
  if (first) {
    return 1;
  } else {
    return 2;
  }
}

int literal_branch() {
  if (true) {
    return 3;
  }
}

int spin() {
  while (true) {}
}

int iterate() {
  for (;;) {
    continue;
  }
}

int post_test_spin() {
  do {
    continue;
  } while (true);
}

int post_test_return() {
  do {
    return 4;
  } while (false);
}

int selected_target() {
#if target.os == "never"
  int inactive = 0;
#else
  return 3;
#endif
}

void observe() {}

int main() {}
)");
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "complete branches, proven infinite loops, void functions, and "
         "main fallthrough should pass return analysis");

  const lang::FrontendResult invalidMainReturn =
      lang::Frontend().analyze("main-return.gti", "void main() {}\n");
  const lang::FrontendResult invalidMainParameters = lang::Frontend().analyze(
      "main-parameters.gti", "int main(int value) { return value; }\n");
  const lang::FrontendResult missingMainBody =
      lang::Frontend().analyze("main-body.gti", "int main();\n");
  expect(
      !invalidMainReturn.canGenerateCode() &&
          !invalidMainParameters.canGenerateCode() &&
          !missingMainBody.canGenerateCode() &&
          hasDiagnosticCode(invalidMainReturn.diagnostics, "GTI-S2032") &&
          hasDiagnosticCode(invalidMainParameters.diagnostics, "GTI-S2032") &&
          hasDiagnosticCode(missingMainBody.diagnostics, "GTI-S2032"),
      "the frontend should own the currently supported int main() entry "
      "point contract");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("missing-return.gti", R"(
int branch(bool value) {
  if (value) {
    return 1;
  }
}

int conditional_loop(bool value) {
  while (value) {
    return 1;
  }
}

int breakable_loop(bool leave) {
  while (true) {
    if (leave) {
      break;
    }
  }
}

int post_test_once() {
  do {
    continue;
  } while (false);
}

expected<void, int> expected_result(bool ready) {
  if (ready) {
    return;
  }
}

int missing_target_return() {
#if target.os == "never"
  return 1;
#else
  int selected = 1;
#endif
}

class Broken {
public:
  int value() {}
};

int main() {
  auto incomplete = [](bool ready) -> int {
    if (ready) {
      return 1;
    }
  };
  return 0;
}
)");
  expect(!invalid.canGenerateCode() &&
             countDiagnosticCode(invalid.diagnostics, "GTI-S2031") == 8 &&
             hasDiagnostic(invalid.diagnostics,
                           "can reach the end without returning a value"),
         "every non-void function, method, expected result, and lambda path "
         "should return before backend entry");
}

void testSourceUnitDependencyGraph() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-source-graph";
  const std::filesystem::path entry = root / "main.gti";
  const std::filesystem::path branch = root / "branch.gti";
  const std::filesystem::path leaf = root / "leaf.gti";
  const auto canonical = [](const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(path).string();
  };
  const std::string entryKey = canonical(entry);
  const std::string branchKey = canonical(branch);
  const std::string leafKey = canonical(leaf);

  lang::Lexer includeLexer;
  const std::vector<lang::Token> includeTokens =
      includeLexer.scan("#include \"branch.gti\"\n");
  expect(!includeLexer.hadError() && includeTokens.size() >= 3 &&
             includeTokens.front().kind == lang::TokenKind::HASH_INCLUDE,
         "#include should have a dedicated directive token");
  const std::vector<lang::Token> identifierTokens =
      includeLexer.scan("int include = 1;\n");
  expect(!includeLexer.hadError() && identifierTokens.size() >= 3 &&
             identifierTokens[1].kind == lang::TokenKind::IDENTIFIER,
         "plain include should remain available as an identifier");
  const lang::FrontendResult includeIdentifier =
      lang::Frontend().analyze(entry, "int include = 1;\n"
                                      "int main() { return include - 1; }\n");
  expect(includeIdentifier.canGenerateCode(),
         "plain include should not remain reserved by source loading");

  const lang::FrontendResult legacyInclude = lang::Frontend().analyze(
      entry,
      "include \"branch.gti\"\n"
      "int main() { return 0; }\n",
      {}, {{branchKey, "int branch_value() { return 1; }\n"}});
  expect(!legacyInclude.sourceValid &&
             countDiagnosticCode(legacyInclude.diagnostics, "GTI-I0009") == 1 &&
             hasDiagnostic(legacyInclude.diagnostics, "use '#include'"),
         "the removed include spelling should receive one migration "
         "diagnostic");

  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "int main() { return branch_value(); }\n",
      {},
      {{branchKey, "#include \"leaf.gti\"\n"
                   "int branch_value() { return leaf_value(); }\n"},
       {leafKey, "int leaf_value() { return 1; }\n"}});
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "direct source dependencies should be visible within each unit");
  expect(valid.sourceGraph.sourceUnits().size() == 3 &&
             valid.sourceGraph.dependencyEdges().size() == 2,
         "the frontend should retain source units and explicit include edges");
  bool dependencySpansRetained = true;
  for (const lang::SourceDependency &dependency :
       valid.sourceGraph.dependencyEdges()) {
    dependencySpansRetained =
        dependencySpansRetained &&
        dependency.kind == lang::SourceDependencyKind::Include &&
        dependency.directive.has_value();
  }
  expect(dependencySpansRetained,
         "explicit dependency edges should retain their include locations");

  const lang::SourceUnitId entryId =
      valid.sourceGraph.sourceUnitForPath(entryKey);
  const lang::SourceUnitId branchId =
      valid.sourceGraph.sourceUnitForPath(branchKey);
  const lang::SourceUnitId leafId =
      valid.sourceGraph.sourceUnitForPath(leafKey);
  expect(entryId != 0 && branchId != 0 && leafId != 0 &&
             valid.sourceGraph.entryUnit() == entryId &&
             valid.sourceGraph.isVisible(entryId, branchId) &&
             !valid.sourceGraph.isVisible(entryId, leafId) &&
             valid.sourceGraph.isVisible(branchId, leafId),
         "source visibility should include only the current and direct units");
  const lang::SourceUnit *entryUnit = valid.sourceGraph.findUnit(entryId);
  const lang::SourceUnit *branchUnit = valid.sourceGraph.findUnit(branchId);
  const lang::SourceUnit *leafUnit = valid.sourceGraph.findUnit(leafId);
  expect(entryUnit != nullptr && branchUnit != nullptr && leafUnit != nullptr &&
             entryUnit->declarationCount == 1 &&
             branchUnit->declarationCount == 1 &&
             leafUnit->declarationCount == 1,
         "independently parsed units should retain their program ranges");

  const lang::FunctionDecl *leafFunction =
      findTopLevelFunction(valid.program, "leaf_value");
  const lang::FunctionInfo *leafInfo =
      leafFunction == nullptr ? nullptr
                              : valid.semantics.findFunction(*leafFunction);
  bool hirRetainsLeafUnit = false;
  if (leafInfo != nullptr) {
    for (const lang::HirFunctionInstance &instance :
         valid.hir.functionInstances()) {
      if (instance.declaration == leafInfo->id) {
        hirRetainsLeafUnit = instance.sourceUnit == leafId;
      }
    }
  }
  expect(leafInfo != nullptr && leafInfo->sourceUnit == leafId &&
             hirRetainsLeafUnit,
         "semantic and HIR function identities should retain source units");

  const lang::FrontendResult transitiveUse = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "int main() { return leaf_value(); }\n",
      {},
      {{branchKey, "#include \"leaf.gti\"\n"
                   "int branch_value() { return leaf_value(); }\n"},
       {leafKey, "int leaf_value() { return 1; }\n"}});
  expect(
      !transitiveUse.semanticValid &&
          hasDiagnosticCode(transitiveUse.diagnostics, "GTI-S2024") &&
          hasDiagnostic(transitiveUse.diagnostics,
                        "include its file directly") &&
          hasDiagnosticHint(transitiveUse.diagnostics, "#include \"leaf.gti\""),
      "an includer should not inherit a dependency's private includes");

  const lang::FrontendResult directAlias = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "BranchId id = BranchId(1);\n"
      "int main() { return 0; }\n",
      {}, {{branchKey, "using BranchId = uint64_t;\n"}});
  expect(directAlias.canGenerateCode() && directAlias.diagnostics.empty(),
         "type aliases from a direct dependency should be visible");

  const lang::FrontendResult hiddenAlias =
      lang::Frontend().analyze(entry,
                               "#include \"branch.gti\"\n"
                               "LeafId id = LeafId(1);\n"
                               "int main() { return 0; }\n",
                               {},
                               {{branchKey, "#include \"leaf.gti\"\n"},
                                {leafKey, "using LeafId = uint64_t;\n"}});
  expect(
      !hiddenAlias.semanticValid &&
          hasDiagnosticCode(hiddenAlias.diagnostics, "GTI-S2024") &&
          hasDiagnosticHint(hiddenAlias.diagnostics, "#include \"leaf.gti\""),
      "type aliases should follow direct source visibility rules");

  const lang::FrontendResult siblingLeak = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "#include \"leaf.gti\"\n"
      "int main() { return branch_value(); }\n",
      {},
      {{branchKey, "int branch_value() { return leaf_value(); }\n"},
       {leafKey, "int leaf_value() { return 1; }\n"}});
  expect(
      !siblingLeak.semanticValid &&
          hasDiagnosticCode(siblingLeak.diagnostics, "GTI-S2024") &&
          hasDiagnostic(siblingLeak.diagnostics, "include its file directly"),
      "a source unit should not see dependencies included by its parent");

  const std::filesystem::path prelude = root / "prelude.gti";
  const std::filesystem::path preludeDetail = root / "prelude_detail.gti";
  const std::string preludeKey = canonical(prelude);
  const std::string preludeDetailKey = canonical(preludeDetail);
  const lang::FrontendResult preludeGraph = lang::Frontend().analyze(
      entry, "int main() { return prelude_value(); }\n", {prelude},
      {{preludeKey, "#include \"prelude_detail.gti\"\n"
                    "int prelude_value() { return prelude_detail(); }\n"},
       {preludeDetailKey, "int prelude_detail() { return 1; }\n"}});
  const lang::SourceUnitId preludeEntryId =
      preludeGraph.sourceGraph.sourceUnitForPath(entryKey);
  const lang::SourceUnitId preludeId =
      preludeGraph.sourceGraph.sourceUnitForPath(preludeKey);
  const lang::SourceUnitId preludeDetailId =
      preludeGraph.sourceGraph.sourceUnitForPath(preludeDetailKey);
  expect(preludeGraph.canGenerateCode() &&
             preludeGraph.sourceGraph.compilationOrder().size() == 3 &&
             preludeGraph.sourceGraph.isVisible(preludeEntryId, preludeId) &&
             !preludeGraph.sourceGraph.isVisible(preludeEntryId,
                                                 preludeDetailId) &&
             preludeGraph.sourceGraph.isVisible(preludeId, preludeDetailId),
         "implicit preludes should remain acyclic without re-exporting their "
         "private dependencies");

  const lang::FrontendResult invalidDependency =
      lang::Frontend().analyze(entry,
                               "#include \"branch.gti\"\n"
                               "int main() { return 0; }\n",
                               {}, {{branchKey, "int broken = 1\n"}});
  expect(!invalidDependency.syntaxValid &&
             hasDiagnosticCode(invalidDependency.diagnostics, "GTI-P0001") &&
             hasRelatedDiagnostic(invalidDependency.diagnostics,
                                  "Included from here"),
         "dependency parser diagnostics should retain the incoming include "
         "location");

  const std::filesystem::path missingPath =
      root / "missing-include-never-created.gti";
  std::error_code removeError;
  std::filesystem::remove(missingPath, removeError);
  const std::string missingKey = canonical(missingPath);
  const lang::FrontendResult missingDependency = lang::Frontend().analyze(
      entry, "#include \"missing-include-never-created.gti\"\n"
             "int main() { return 0; }\n");
  bool missingDiagnosticUsesEntry = false;
  for (const lang::Diagnostic &diagnostic : missingDependency.diagnostics) {
    if (diagnostic.code == "GTI-I0008") {
      missingDiagnosticUsesEntry = diagnostic.primary.source == entryKey;
    }
  }
  expect(!missingDependency.sourceValid &&
             hasDiagnosticCode(missingDependency.diagnostics, "GTI-I0008") &&
             missingDiagnosticUsesEntry &&
             missingDependency.sourceGraph.sourceUnitForPath(missingKey) == 0 &&
             !std::filesystem::exists(missingPath),
         "missing relative includes should stay diagnostics on the includer "
         "without creating a source unit or file");

  const lang::FrontendResult dependencyMain =
      lang::Frontend().analyze(entry, "#include \"branch.gti\"\n", {},
                               {{branchKey, "int main() { return 0; }\n"}});
  expect(!dependencyMain.semanticValid &&
             hasDiagnosticCode(dependencyMain.diagnostics, "GTI-S2025"),
         "only the entry source unit should be allowed to declare main");
}

void testStandardLibraryImports() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-standard-imports";
  const std::filesystem::path entry = root / "main.gti";
  const std::filesystem::path arrayUnit =
      standardLibraryRoot() / "std/array.gti";
  const std::string arrayKey =
      std::filesystem::weakly_canonical(arrayUnit).string();

  const lang::FrontendResult imported = lang::Frontend().analyze(
      entry,
      "#include <std/array>\n"
      "#include <std/array>;\n"
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
  if (!imported.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : imported.diagnostics) {
      std::cerr << "Unexpected standard import diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(imported.canGenerateCode(),
         "standard-library imports should expose ordinary GTI declarations");
  const lang::SourceUnitId arrayId =
      imported.sourceGraph.sourceUnitForPath(arrayKey);
  const lang::SourceUnit *loadedArray = imported.sourceGraph.findUnit(arrayId);
  std::size_t standardEdges = 0;
  for (const lang::SourceDependency &dependency :
       imported.sourceGraph.dependencyEdges()) {
    if (dependency.kind == lang::SourceDependencyKind::StandardLibrary) {
      ++standardEdges;
    }
  }
  expect(arrayId != 0 && loadedArray != nullptr &&
             loadedArray->standardLibraryName == "std/array" &&
             standardEdges == 2 &&
             imported.sourceGraph.sourceUnits().size() == 3,
         "standard imports should retain their logical name and load each "
         "canonical unit once");

  lang::CppEmitter emitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                           nullptr, &imported.semantics);
  const std::string generated = emitter.emit(imported.program);
  expect(generated.find("namespace gti_std") != std::string::npos &&
             generated.find("class array") != std::string::npos &&
             generated.find("std::array<T, N> values") != std::string::npos &&
             generated.find("gti_std::array<std::int32_t, 3>") !=
                 std::string::npos,
         "std::array should remain source-defined over fixed-array lowering");

  const std::filesystem::path wrapper =
      standardLibraryRoot() / "std/import_test_wrapper.gti";
  const std::string wrapperKey =
      std::filesystem::weakly_canonical(wrapper).string();
  const lang::FrontendResult hidden = lang::Frontend().analyze(
      entry,
      "#include <std/import_test_wrapper>\n"
      "std::array<int, 1> hidden = std::array<int, 1>();\n"
      "int main() { return std::import_test_value(); }\n",
      {standardLibraryPrelude()},
      {{wrapperKey,
        "#include <std/array>\n"
        "namespace std { int import_test_value() { return 0; } }\n"}},
      {standardLibraryRoot()});
  expect(!hidden.semanticValid &&
             hasDiagnosticCode(hidden.diagnostics, "GTI-S2024") &&
             hasDiagnosticHint(hidden.diagnostics, "#include <std/array>"),
         "standard imports should remain private and provide logical import "
         "hints");

  const std::filesystem::path keywordUnit =
      standardLibraryRoot() / "std/char.gti";
  const std::string keywordKey =
      std::filesystem::weakly_canonical(keywordUnit).string();
  const lang::FrontendResult keywordPath = lang::Frontend().analyze(
      entry,
      "#include <std/char>\n"
      "int main() { return std::keyword_path_value(); }\n",
      {standardLibraryPrelude()},
      {{keywordKey,
        "namespace std { int keyword_path_value() { return 0; } }\n"}},
      {standardLibraryRoot()});
  expect(keywordPath.canGenerateCode(),
         "standard import components should permit GTI keyword spellings");

  const lang::FrontendResult missing = lang::Frontend().analyze(
      entry, "#include <std/not_present>\nint main() { return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!missing.sourceValid &&
             hasDiagnosticCode(missing.diagnostics, "GTI-I0007") &&
             hasDiagnostic(missing.diagnostics,
                           "<std/not_present>' was not found"),
         "missing standard units should fail during source loading");

  const lang::FrontendResult malformed = lang::Frontend().analyze(
      entry, "#include <array>\nint main() { return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!malformed.sourceValid &&
             hasDiagnosticCode(malformed.diagnostics, "GTI-I0007") &&
             hasDiagnostic(malformed.diagnostics, "#include <std/array>"),
         "malformed standard imports should explain the required spelling");
}

void testOwnershipSemanticFoundation() {
  const lang::SemanticType reference = lang::SemanticType::referenceTo(
      lang::SemanticType::Int32, lang::AccessMode::Mutable);
  const lang::SemanticType unique =
      lang::SemanticType::uniqueOwnerOf(lang::SemanticType::Int32);
  const lang::SemanticType shared =
      lang::SemanticType::sharedPointerTo(lang::SemanticType::Int32);
  const lang::SemanticType storage =
      lang::SemanticType::storageOf(lang::SemanticType::Int32);
  const lang::SemanticTypeTraits referenceTraits =
      lang::semanticTraits(reference);
  const lang::SemanticTypeTraits uniqueTraits = lang::semanticTraits(unique);
  const lang::SemanticTypeTraits sharedTraits = lang::semanticTraits(shared);
  const lang::SemanticTypeTraits storageTraits = lang::semanticTraits(storage);
  expect(reference.kind == lang::SemanticType::Reference &&
             reference.referenceAccess == lang::AccessMode::Mutable &&
             referenceTraits.ownership == lang::OwnershipKind::Borrowed &&
             referenceTraits.drop == lang::DropKind::Trivial,
         "references should carry borrowed access semantics");
  expect(uniqueTraits.ownership == lang::OwnershipKind::Unique &&
             uniqueTraits.drop == lang::DropKind::Lexical &&
             !uniqueTraits.copyable && uniqueTraits.movable,
         "unique pointers should be move-only lexical owners");
  expect(sharedTraits.ownership == lang::OwnershipKind::Shared &&
             sharedTraits.drop == lang::DropKind::Lexical &&
             sharedTraits.copyable && sharedTraits.movable,
         "shared pointers should be copyable lexical owners");
  expect(storageTraits.ownership == lang::OwnershipKind::Unique &&
             storageTraits.drop == lang::DropKind::Lexical &&
             !storageTraits.copyable && storageTraits.movable,
         "compiler-private storage should be a move-only lexical owner");

  lang::FrontendResult frontend =
      lang::Frontend().analyze("ownership-foundation.gti", R"(
struct Counter {
  mut int value = 0;
  void bump() mut { this.value += 1; }
};

int identity(int value) { return value; }

int main() {
  int fixed = 1;
  mut int changing = 2;
  mut Counter counter = Counter();
  int grouped = (fixed);
  changing++;
  counter.bump();
  return identity(changing);
}
)");
  expect(frontend.canGenerateCode(),
         "ownership metadata source should pass the frontend");
  expect(frontend.semantics.bindingCount() == 6,
         "the semantic model should retain fields, parameters, and locals");

  const auto *identity = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(1).get());
  const auto *main = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(2).get());
  expect(identity != nullptr && main != nullptr,
         "ownership metadata fixture should retain its functions");
  if (identity == nullptr || main == nullptr) {
    return;
  }

  const lang::BindingInfo *parameter =
      frontend.semantics.findBinding(identity->parameters().front());
  expect(parameter != nullptr &&
             parameter->access == lang::AccessMode::ReadOnly &&
             parameter->traits.ownership == lang::OwnershipKind::Value,
         "parameters should expose binding access and ownership metadata");

  const lang::StmtList &statements = main->body()->statements();
  const auto *fixed =
      dynamic_cast<const lang::VariableDecl *>(statements.at(0).get());
  const auto *changing =
      dynamic_cast<const lang::VariableDecl *>(statements.at(1).get());
  const auto *counter =
      dynamic_cast<const lang::VariableDecl *>(statements.at(2).get());
  const auto *grouped =
      dynamic_cast<const lang::VariableDecl *>(statements.at(3).get());
  const auto *increment =
      dynamic_cast<const lang::ExpressionStmt *>(statements.at(4).get());
  const auto *methodCall =
      dynamic_cast<const lang::ExpressionStmt *>(statements.at(5).get());
  expect(fixed != nullptr && changing != nullptr && counter != nullptr &&
             grouped != nullptr && increment != nullptr &&
             methodCall != nullptr,
         "ownership metadata fixture should retain its statement shapes");
  if (fixed == nullptr || changing == nullptr || counter == nullptr ||
      grouped == nullptr || increment == nullptr || methodCall == nullptr) {
    return;
  }

  const lang::BindingInfo *fixedBinding =
      frontend.semantics.findBinding(*fixed);
  const lang::BindingInfo *changingBinding =
      frontend.semantics.findBinding(*changing);
  const lang::BindingInfo *counterBinding =
      frontend.semantics.findBinding(*counter);
  expect(fixedBinding != nullptr &&
             fixedBinding->access == lang::AccessMode::ReadOnly &&
             fixedBinding->traits.drop == lang::DropKind::Trivial &&
             changingBinding != nullptr &&
             changingBinding->access == lang::AccessMode::Mutable &&
             counterBinding != nullptr &&
             counterBinding->traits.drop == lang::DropKind::Lexical,
         "bindings should distinguish access and lexical drop requirements");

  const lang::ExpressionInfo counterConstruction =
      frontend.semantics.expressionInfo(*counter->initializer());
  const lang::ExpressionInfo groupedInfo =
      frontend.semantics.expressionInfo(*grouped->initializer());
  const auto *postfix =
      dynamic_cast<const lang::Postfix *>(increment->expression().get());
  const auto *call =
      dynamic_cast<const lang::Call *>(methodCall->expression().get());
  expect(counterConstruction.category == lang::ValueCategory::Value &&
             counterConstruction.traits.drop == lang::DropKind::Lexical &&
             groupedInfo.category == lang::ValueCategory::Place &&
             groupedInfo.access == lang::AccessMode::ReadOnly,
         "expressions should distinguish owned values from borrowed places");
  expect(
      postfix != nullptr && call != nullptr,
      "ownership metadata fixture should retain postfix and call expressions");
  if (postfix == nullptr || call == nullptr) {
    return;
  }

  const lang::ExpressionInfo incrementTarget =
      frontend.semantics.expressionInfo(*postfix->expression());
  const auto *member = dynamic_cast<const lang::Get *>(call->callee().get());
  expect(incrementTarget.category == lang::ValueCategory::Place &&
             incrementTarget.access == lang::AccessMode::Mutable &&
             member != nullptr,
         "mutable bindings should produce mutable place expressions");
  if (member == nullptr) {
    return;
  }
  const lang::ExpressionInfo receiver =
      frontend.semantics.expressionInfo(*member->object());
  const lang::ExpressionInfo memberFunction =
      frontend.semantics.expressionInfo(*member);
  expect(
      receiver.category == lang::ValueCategory::Place &&
          receiver.access == lang::AccessMode::Mutable &&
          memberFunction.category == lang::ValueCategory::Value &&
          frontend.semantics.findType(*member) != nullptr,
      "method analysis should preserve receiver access and callable metadata");
}

void testExplicitValueMoves() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("explicit-value-moves.gti", R"(
struct Record {
  int value = 4;
};

int forward_int(int value) { return std::move(value); }
T transfer<T>(T value) { return std::move(value); }

int main() {
  int source = 1;
  int moved = std::move(source);
  int forwarded = forward_int(std::move(moved));

  mut int reusable = 2;
  int first = std::move(reusable);
  reusable = 3;
  int generic = transfer(std::move(first));

  Record record = Record();
  Record moved_record = std::move(record);
  return reusable + forwarded + generic + moved_record.value;
}
)",
                               {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected explicit-move diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "std::move should explicitly consume any named movable local value "
         "or by-value parameter");

  const lang::FunctionDecl *forward =
      findTopLevelFunction(valid.program, "forward_int");
  const lang::FunctionDecl *transfer =
      findTopLevelFunction(valid.program, "transfer");
  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const lang::BindingInfo *forwardedParameter =
      forward == nullptr
          ? nullptr
          : valid.semantics.findBinding(forward->parameters().front());
  const lang::BindingInfo *genericParameter =
      transfer == nullptr
          ? nullptr
          : valid.semantics.findBinding(transfer->parameters().front());
  const lang::VariableDecl *source = nullptr;
  const lang::VariableDecl *record = nullptr;
  if (main != nullptr) {
    for (const lang::StmtPtr &statement : main->body()->statements()) {
      const auto *variable =
          dynamic_cast<const lang::VariableDecl *>(statement.get());
      if (variable == nullptr) {
        continue;
      }
      if (variable->name().lexeme == "source") {
        source = variable;
      } else if (variable->name().lexeme == "record") {
        record = variable;
      }
    }
  }
  const lang::BindingInfo *sourceBinding =
      source == nullptr ? nullptr : valid.semantics.findBinding(*source);
  const lang::BindingInfo *recordBinding =
      record == nullptr ? nullptr : valid.semantics.findBinding(*record);
  expect(forwardedParameter != nullptr && forwardedParameter->explicitlyMoved &&
             genericParameter != nullptr && genericParameter->explicitlyMoved &&
             sourceBinding != nullptr && sourceBinding->explicitlyMoved &&
             recordBinding != nullptr && recordBinding->explicitlyMoved,
         "binding metadata should retain explicit consumption for parameters "
         "and local values");

  std::size_t moves = 0;
  bool allMovesHaveOneOperand = true;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    for (const lang::HirValue &value : instance.body.values) {
      if (value.kind != lang::HirValueKind::Move) {
        continue;
      }
      ++moves;
      allMovesHaveOneOperand = allMovesHaveOneOperand &&
                               value.intrinsic == lang::IntrinsicKind::Move &&
                               value.operands.size() == 1;
    }
  }
  expect(moves >= 7 && allMovesHaveOneOperand,
         "typed HIR should represent explicit moves as unary ownership "
         "operations rather than ordinary calls");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = valid.program,
                                   .semantics = valid.semantics,
                                   .hir = valid.hir,
                                   .mir = valid.mir,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("const std::int32_t value") == std::string::npos &&
          artifact.contents.find("const std::int32_t source") ==
              std::string::npos &&
          artifact.contents.find("const Record record") == std::string::npos &&
          artifact.contents.find("std::move(source)") != std::string::npos,
      "the C++ backend should keep explicitly consumed immutable bindings "
      "physically movable");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-explicit-value-moves.gti", R"(
int global_value = 1;

struct Box {
  int field = 1;
  int take() mut { return std::move(this.field); }
};

int main() {
  int used = 1;
  int once = std::move(used);
  int twice = used;

  mut int conditional = 2;
  if (once == 1) {
    int branch = std::move(conditional);
  }
  int maybe_moved = conditional;

  [[discard]] std::move(1);
  [[discard]] std::move(global_value);
  int values[1] = {1};
  [[discard]] std::move(values[0]);
  mut int referenced = 3;
  int& alias = referenced;
  [[discard]] std::move(alias);

  mut int self_move = 5;
  self_move = std::move(self_move);

  int captured = 4;
  auto closure = [captured]() -> int { return std::move(captured); };
  return twice + maybe_moved + closure();
}
)",
                               {standardLibraryPrelude()});
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics, "has already been moved") &&
          hasDiagnostic(invalid.diagnostics,
                        "may have been moved on another control-flow path") &&
          hasDiagnostic(invalid.diagnostics,
                        "temporaries are already values") &&
          hasDiagnostic(invalid.diagnostics, "cannot consume global binding") &&
          hasDiagnostic(invalid.diagnostics, "cannot partially move a field") &&
          hasDiagnostic(invalid.diagnostics, "cannot consume a reference") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot move a binding into itself") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot consume immutable lambda capture"),
      "move diagnostics should reject unavailable values and storage whose "
      "state cannot yet be tracked soundly");
}

void testTrustedIntrinsicDeclarations() {
  const lang::FrontendResult trusted =
      lang::Frontend().analyze("trusted-intrinsic-declaration.gti", R"(
namespace utility = std;

int main() {
  int source = 1;
  int moved = utility::move(source);
  return moved;
}
)",
                               {standardLibraryPrelude()});
  expect(trusted.canGenerateCode(),
         "a namespace alias should retain the trusted move declaration's "
         "intrinsic identity");

  const lang::FunctionDecl *trustedMain =
      findTopLevelFunction(trusted.program, "main");
  const auto *trustedMoved =
      trustedMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                trustedMain->body()->statements().at(1).get());
  const auto *trustedCall =
      trustedMoved == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(trustedMoved->initializer().get());
  const lang::ResolvedCallInfo *trustedResolution =
      trustedCall == nullptr ? nullptr
                             : trusted.semantics.findCall(*trustedCall);
  const lang::FunctionInfo *trustedDeclaration =
      trustedResolution == nullptr || trustedResolution->declaration == nullptr
          ? nullptr
          : trusted.semantics.findFunction(*trustedResolution->declaration);
  const lang::SourceUnit *trustedUnit =
      trustedDeclaration == nullptr
          ? nullptr
          : trusted.sourceGraph.findUnit(trustedDeclaration->sourceUnit);
  expect(trustedResolution != nullptr && trustedResolution->function != 0 &&
             trustedResolution->intrinsic == lang::IntrinsicKind::Move &&
             trustedDeclaration != nullptr &&
             trustedDeclaration->intrinsic == lang::IntrinsicKind::Move &&
             trustedUnit != nullptr && trustedUnit->prelude,
         "resolved intrinsic calls should point to a trusted prelude "
         "declaration instead of deriving behavior from call-site spelling");
  expect(trustedResolution != nullptr &&
             std::none_of(trusted.hir.functionInstances().begin(),
                          trusted.hir.functionInstances().end(),
                          [&](const lang::HirFunctionInstance &instance) {
                            return instance.declaration ==
                                   trustedResolution->function;
                          }),
         "intrinsic declarations should remain semantic operations rather than "
         "bodyless HIR call targets");

  const lang::FrontendResult ordinary =
      lang::Frontend().analyze("ordinary-function-named-move.gti", R"(
namespace std {
T move<T>(T& value) { return value; }
}

int main() {
  int source = 1;
  int copied = std::move(source);
  return source + copied - 2;
}
)");
  expect(ordinary.canGenerateCode(),
         "an ordinary function named std::move should follow ordinary call "
         "semantics outside the trusted prelude");
  const lang::FunctionDecl *ordinaryMain =
      findTopLevelFunction(ordinary.program, "main");
  const auto *ordinarySource =
      ordinaryMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                ordinaryMain->body()->statements().front().get());
  const auto *ordinaryCopied =
      ordinaryMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                ordinaryMain->body()->statements().at(1).get());
  const auto *ordinaryCall = ordinaryCopied == nullptr
                                 ? nullptr
                                 : dynamic_cast<const lang::Call *>(
                                       ordinaryCopied->initializer().get());
  const lang::BindingInfo *ordinarySourceBinding =
      ordinarySource == nullptr
          ? nullptr
          : ordinary.semantics.findBinding(*ordinarySource);
  const lang::ResolvedCallInfo *ordinaryResolution =
      ordinaryCall == nullptr ? nullptr
                              : ordinary.semantics.findCall(*ordinaryCall);
  expect(ordinarySourceBinding != nullptr &&
             !ordinarySourceBinding->explicitlyMoved &&
             ordinaryResolution != nullptr &&
             ordinaryResolution->intrinsic == lang::IntrinsicKind::None,
         "untrusted declarations must not acquire ownership effects by using "
         "a standard-library spelling");

  const lang::FrontendResult untrustedMutableReturn =
      lang::Frontend().analyze("untrusted-mutable-return.gti", R"(
mut int& expose(mut int& value) { return value; }
int main() { return 0; }
)");
  expect(!untrustedMutableReturn.semanticValid &&
             hasDiagnostic(untrustedMutableReturn.diagnostics,
                           "Mutable reference returns are currently limited"),
         "parsing trusted intrinsic signatures must not permit mutable "
         "reference returns from ordinary free functions");
}

void testNonNullReferences() {
  const std::string source = R"(
struct Counter {
public:
  mut int value = 0;
  void bump() mut { this.value += 1; }
};

int read(Counter& counter) { return counter.value; }
void increment(mut Counter& counter) { counter.bump(); }

int main() {
  mut Counter counter = Counter();
  Counter& read_only = counter;
  mut Counter& writable = counter;
  increment(writable);

  mut int value = 1;
  mut int& alias = value;
  alias += 2;
  if (read(read_only) == 1 and value == 3) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("references.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected reference diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "non-null read-only and mutable references should validate");

  const auto *read = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(1).get());
  const auto *increment = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(2).get());
  const lang::BindingInfo *readParameter =
      read == nullptr
          ? nullptr
          : frontend.semantics.findBinding(read->parameters().front());
  const lang::BindingInfo *writeParameter =
      increment == nullptr
          ? nullptr
          : frontend.semantics.findBinding(increment->parameters().front());
  expect(readParameter != nullptr &&
             readParameter->type.kind == lang::SemanticType::Reference &&
             readParameter->type.referenceAccess ==
                 lang::AccessMode::ReadOnly &&
             writeParameter != nullptr &&
             writeParameter->type.kind == lang::SemanticType::Reference &&
             writeParameter->type.referenceAccess == lang::AccessMode::Mutable,
         "reference bindings should retain borrow access in semantic metadata");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("const Counter &counter") != std::string::npos &&
          artifact.contents.find("Counter &counter") != std::string::npos &&
          artifact.contents.find("std::int32_t &alias = value") !=
              std::string::npos,
      "references should lower to C++ references with matching const access");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-references.gti", R"(
int& escape(mut int& value) { return value; }
int global_value = 1;
int& global_reference = global_value;

struct InvalidStorage {
  int& field;
};

void inspect(int& value) {}
void modify(mut int& value) {}

int main() {
  int immutable = 1;
  mut int mutable_value = 2;
  inspect(3);
  modify(immutable);
  mut int& invalid_mutable = immutable;
  int& missing;
  int values[2] = {1, 2};
  int[2]& array_reference = values;
  expected<int&, int> nested = 1;
  Counter& dangling_owner = *std::make_unique<Counter>();
  int& dangling_member = Counter().value;
  inspect(mutable_value);
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode(),
         "escaping, temporary, and invalid mutable references should fail");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "References cannot be used as a function return type") &&
          hasDiagnostic(invalid.diagnostics,
                        "References cannot be used as a storage") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot bind a reference to a temporary") &&
          hasDiagnostic(invalid.diagnostics, "requires a mutable value") &&
          hasDiagnostic(invalid.diagnostics,
                        "mutable reference requires a mutable initializer") &&
          hasDiagnostic(invalid.diagnostics,
                        "Reference bindings require an initializer") &&
          hasDiagnostic(invalid.diagnostics, "References to fixed arrays") &&
          hasDiagnostic(invalid.diagnostics, "References cannot be nested") &&
          hasDiagnostic(invalid.diagnostics, "derived from temporary storage"),
      "reference diagnostics should identify each rejected lifetime rule");

  const std::string formatted = lang::Formatter().format(
      "void inspect(int& value){}void modify(mut int& value){}"
      "int main(){mut int value=1;mut int& alias=value;modify(alias);return "
      "0;}");
  expect(lang::Formatter().format(formatted) == formatted,
         "reference syntax formatting should be idempotent");
}

void testReceiverTiedReferenceReturns() {
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}

  T& get() {
    return this.value;
  }
};

class Buffer<T> {
  mut gti_internal::storage<T> data;

public:
  Buffer(uint64_t capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  void push(T value) mut {
    gti_internal::storage_construct(this.data, uint64_t(0), value);
  }

  T& at(uint64_t index) {
    return gti_internal::storage_read(this.data, index);
  }
};

int main() {
  Box<int> box = Box<int>(7);
  int& boxed = box.get();
  mut Buffer<int> buffer = Buffer<int>(uint64_t(1));
  buffer.push(9);
  int& stored = buffer.at(uint64_t(0));
  return boxed + stored - 16;
}
)";

  lang::FrontendResult frontend = lang::Frontend().analyze(
      "receiver-tied-references.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected receiver-tied reference diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "methods should return read-only references borrowed from 'this'");

  const lang::FunctionDecl *mainFunction =
      findTopLevelFunction(frontend.program, "main");
  const auto *borrowed =
      mainFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                mainFunction->body()->statements().at(1).get());
  const auto *borrowCall =
      borrowed == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(borrowed->initializer().get());
  const lang::ResolvedCallInfo *resolved =
      borrowCall == nullptr ? nullptr
                            : frontend.semantics.findCall(*borrowCall);
  const lang::ExpressionInfo *expression =
      borrowCall == nullptr ? nullptr
                            : frontend.semantics.findExpression(*borrowCall);
  expect(resolved != nullptr &&
             resolved->returnType.kind == lang::SemanticType::Reference &&
             resolved->borrowOrigin == lang::BorrowOriginKind::Receiver &&
             expression != nullptr &&
             expression->category == lang::ValueCategory::Place &&
             expression->access == lang::AccessMode::ReadOnly,
         "reference calls should retain receiver-tied borrow metadata");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("const T &") != std::string::npos &&
             artifact.contents.find("inline const T &storage_read") !=
                 std::string::npos &&
             artifact.contents.find("const std::int32_t &boxed") !=
                 std::string::npos,
         "receiver-tied borrows should lower to const C++ references");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-reference-returns.gti", R"(
int& escape(int& value) { return value; }

class InvalidReferences {
  int value = 1;

public:
  int& local() {
    int temporary = 2;
    return temporary;
  }

  int& parameter(int& value) {
    return value;
  }

  int& literal() {
    return 3;
  }
};

int main() {
  int& dangling = InvalidReferences().local();
  return dangling;
}
)");
  expect(!invalid.canGenerateCode(),
         "reference returns must not escape locals, parameters, or temporary "
         "receivers");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "References cannot be used as a function return type") &&
          hasDiagnostic(invalid.diagnostics,
                        "Method reference returns must borrow from 'this'") &&
          hasDiagnostic(invalid.diagnostics,
                        "Reference return requires an addressable value") &&
          hasDiagnostic(invalid.diagnostics, "derived from temporary storage"),
      "reference-return diagnostics should explain each rejected lifetime");

  const lang::FrontendResult invalidated =
      lang::Frontend().analyze("invalidated-reference.gti", R"(
class Buffer<T> {
  mut gti_internal::storage<T> data;

public:
  Buffer(uint64_t capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  T& at(uint64_t index) {
    return gti_internal::storage_read(this.data, index);
  }

  void clear(uint64_t index) mut {
    gti_internal::storage_destroy(this.data, index);
  }

  int invalidate_receiver(uint64_t index) mut {
    int& value = gti_internal::storage_read(this.data, index);
    gti_internal::storage_destroy(this.data, index);
    return value;
  }
};

int main() {
  mut Buffer<int> buffer = Buffer<int>(uint64_t(1));
  int& value = buffer.at(uint64_t(0));
  buffer.clear(uint64_t(0));
  Buffer<int> moved = std::move(buffer);
  return value;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalidated.canGenerateCode(),
         "move-only receivers must remain stable while borrowed");
  expect(hasDiagnostic(invalidated.diagnostics,
                       "Mutable method cannot use move-only storage") &&
             hasDiagnostic(invalidated.diagnostics,
                           "Cannot move storage while a reference borrowed") &&
             hasDiagnostic(invalidated.diagnostics,
                           "cannot mutate receiver storage while a reference"),
         "borrow diagnostics should prevent receiver invalidation");
}

void testStoredReferenceGroundwork() {
  const std::string source = R"(
struct SingleSentinel {};

class BorrowingIterator<T> {
  T& value;
  mut uint64_t position = 0;

public:
  BorrowingIterator(T& source) : value(source) {}

  T& operator*() { return this.value; }
  void operator++() mut { this.position++; }
  bool operator!=(SingleSentinel& sentinel) { return this.position == 0; }
};

class SingleRange<T> {
  mut T value;

public:
  SingleRange(T initial) : value(initial) {}

  BorrowingIterator<T> begin() {
    return BorrowingIterator<T>(this.value);
  }

  SingleSentinel end() { return SingleSentinel(); }

  void replace(T next) mut { this.value = next; }
};

int main() {
  mut SingleRange<int> range = SingleRange<int>(7);
  mut BorrowingIterator<int> iterator = range.begin();
  int& value = *iterator;
  ++iterator;
  mut BorrowingIterator<int> moved = std::move(iterator);
  ++moved;
  mut int total = 0;
  for (int& item : range) { total += item; }
  return value + total - 14;
}
)";

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "stored-references.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected stored-reference diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "a read-only stored reference should support an owner-tied iterator");

  const lang::FunctionDecl *storedReferenceMain =
      findTopLevelFunction(frontend.program, "main");
  const lang::VariableDecl *originalIterator =
      storedReferenceMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                storedReferenceMain->body()->statements().at(1).get());
  const lang::VariableDecl *movedIterator =
      storedReferenceMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                storedReferenceMain->body()->statements().at(4).get());
  const lang::BindingInfo *originalIteratorBinding =
      originalIterator == nullptr
          ? nullptr
          : frontend.semantics.findBinding(*originalIterator);
  const lang::BindingInfo *movedIteratorBinding =
      movedIterator == nullptr ? nullptr
                               : frontend.semantics.findBinding(*movedIterator);
  expect(originalIteratorBinding != nullptr &&
             movedIteratorBinding != nullptr &&
             originalIteratorBinding->retainedLoan != 0 &&
             originalIteratorBinding->retainedLoan ==
                 movedIteratorBinding->retainedLoan,
         "moving borrowed state should transfer one semantic loan identity "
         "instead of creating a second owner dependency");

  const lang::FrontendResult standardStringRange = lang::Frontend().analyze(
      "standard-string-range.gti",
      "#include <std/string>\n"
      "int main() { std::string value = std::string(\"gti\"); "
      "mut uint64_t count = 0; for (char character : value) { count++; } "
      "return int(count) - 3; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!standardStringRange.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic :
         standardStringRange.diagnostics) {
      std::cerr << "Unexpected std::string range diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(standardStringRange.canGenerateCode(),
         "the source-defined standard string should expose read-only "
         "owner-tied iteration");

  const lang::FrontendResult releasedStandardStringBorrow =
      lang::Frontend().analyze(
          "released-standard-string-borrow.gti",
          "#include <std/string>\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); char first = *iterator; "
          "value.push_back('!'); return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!releasedStandardStringBorrow.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic :
         releasedStandardStringBorrow.diagnostics) {
      std::cerr << "Unexpected released-borrow diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(releasedStandardStringBorrow.canGenerateCode(),
         "a retained string iterator should release its owner after its last "
         "straight-line use");
  const lang::FunctionDecl *releasedMain =
      findTopLevelFunction(releasedStandardStringBorrow.program, "main");
  const lang::VariableDecl *releasedIterator =
      releasedMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                releasedMain->body()->statements().at(1).get());
  const lang::Stmt *releasedLastUse =
      releasedMain == nullptr ? nullptr
                              : releasedMain->body()->statements().at(2).get();
  const lang::BindingInfo *releasedBinding =
      releasedIterator == nullptr
          ? nullptr
          : releasedStandardStringBorrow.semantics.findBinding(
                *releasedIterator);
  const lang::SemanticLoanInfo *releasedLoan =
      releasedBinding == nullptr
          ? nullptr
          : releasedStandardStringBorrow.semantics.findLoan(
                releasedBinding->retainedLoan);
  expect(releasedLoan != nullptr && releasedLoan->protectsStorage &&
             hasLoanEndpoint(releasedLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             releasedLastUse),
         "semantic loan metadata should identify the owner dependency and its "
         "proven last-use statement");

  const auto releasedHirFunction =
      releasedMain == nullptr
          ? releasedStandardStringBorrow.hir.functionInstances().end()
          : std::find_if(
                releasedStandardStringBorrow.hir.functionInstances().begin(),
                releasedStandardStringBorrow.hir.functionInstances().end(),
                [&](const lang::HirFunctionInstance &candidate) {
                  return candidate.source == releasedMain;
                });
  const lang::HirStatement *releasedHirLastUse = nullptr;
  if (releasedHirFunction !=
      releasedStandardStringBorrow.hir.functionInstances().end()) {
    const auto statement =
        std::find_if(releasedHirFunction->body.statements.begin(),
                     releasedHirFunction->body.statements.end(),
                     [&](const lang::HirStatement &candidate) {
                       return candidate.source == releasedLastUse;
                     });
    if (statement != releasedHirFunction->body.statements.end()) {
      releasedHirLastUse = &*statement;
    }
  }
  expect(releasedHirLastUse != nullptr && releasedBinding != nullptr &&
             std::find(releasedHirLastUse->endedLoans.begin(),
                       releasedHirLastUse->endedLoans.end(),
                       releasedBinding->retainedLoan) !=
                 releasedHirLastUse->endedLoans.end(),
         "HIR should carry the semantic loan endpoint without recomputing it");

  const lang::MirFunctionInstance *releasedMirFunction =
      releasedHirFunction ==
              releasedStandardStringBorrow.hir.functionInstances().end()
          ? nullptr
          : releasedStandardStringBorrow.mir.findFunctionInstance(
                releasedHirFunction->id);
  const lang::MirLoan *releasedMirLoan = nullptr;
  if (releasedMirFunction != nullptr && releasedBinding != nullptr) {
    const auto loan = std::find_if(releasedMirFunction->body.loans.begin(),
                                   releasedMirFunction->body.loans.end(),
                                   [&](const lang::MirLoan &candidate) {
                                     return candidate.semanticLoan ==
                                            releasedBinding->retainedLoan;
                                   });
    if (loan != releasedMirFunction->body.loans.end()) {
      releasedMirLoan = &*loan;
    }
  }
  bool foundLastUseEnd = false;
  if (releasedMirFunction != nullptr && releasedMirLoan != nullptr &&
      releasedHirLastUse != nullptr) {
    for (const lang::MirBlock &block : releasedMirFunction->body.blocks) {
      foundLastUseEnd =
          foundLastUseEnd ||
          std::any_of(block.instructions.begin(), block.instructions.end(),
                      [&](const lang::MirInstruction &instruction) {
                        return instruction.kind ==
                                   lang::MirInstructionKind::EndBorrow &&
                               instruction.loan == releasedMirLoan->id &&
                               instruction.hirStatement ==
                                   releasedHirLastUse->id;
                      });
    }
  }
  expect(foundLastUseEnd,
         "MIR should end the retained loan at the frontend-proven statement");

  const lang::FrontendResult invalidStandardStringBorrow =
      lang::Frontend().analyze(
          "invalid-standard-string-borrow.gti",
          "#include <std/string>\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); value.push_back('!'); "
          "char first = *iterator; return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidStandardStringBorrow.canGenerateCode() &&
             hasDiagnostic(invalidStandardStringBorrow.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a retained string iterator should prevent storage-invalidating "
         "mutation");

  const lang::FrontendResult controlFlowBorrow = lang::Frontend().analyze(
      "control-flow-string-borrow.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { char first = "
      "*iterator; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!controlFlowBorrow.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : controlFlowBorrow.diagnostics) {
      std::cerr << "Unexpected conditional-borrow diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(controlFlowBorrow.canGenerateCode(),
         "an unshared retained borrow should end after an if join when its "
         "final use is inside a branch");
  const lang::FunctionDecl *controlFlowMain =
      findTopLevelFunction(controlFlowBorrow.program, "main");
  const auto *controlFlowIterator =
      controlFlowMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                controlFlowMain->body()->statements().at(1).get());
  const lang::Stmt *controlFlowIf =
      controlFlowMain == nullptr
          ? nullptr
          : controlFlowMain->body()->statements().at(2).get();
  const lang::BindingInfo *controlFlowBinding =
      controlFlowIterator == nullptr
          ? nullptr
          : controlFlowBorrow.semantics.findBinding(*controlFlowIterator);
  const lang::SemanticLoanInfo *controlFlowLoan =
      controlFlowBinding == nullptr ? nullptr
                                    : controlFlowBorrow.semantics.findLoan(
                                          controlFlowBinding->retainedLoan);
  expect(hasLoanEndpoint(controlFlowLoan,
                         lang::SemanticLoanEndKind::AfterStatement,
                         controlFlowIf),
         "semantic loan metadata should project a branch-local final use to "
         "the enclosing if join");
  const auto controlFlowHirFunction =
      controlFlowMain == nullptr
          ? controlFlowBorrow.hir.functionInstances().end()
          : std::find_if(controlFlowBorrow.hir.functionInstances().begin(),
                         controlFlowBorrow.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == controlFlowMain;
                         });
  const lang::HirStatement *controlFlowHirIf = nullptr;
  if (controlFlowHirFunction !=
      controlFlowBorrow.hir.functionInstances().end()) {
    const auto statement =
        std::find_if(controlFlowHirFunction->body.statements.begin(),
                     controlFlowHirFunction->body.statements.end(),
                     [&](const lang::HirStatement &candidate) {
                       return candidate.source == controlFlowIf;
                     });
    if (statement != controlFlowHirFunction->body.statements.end()) {
      controlFlowHirIf = &*statement;
    }
  }
  expect(controlFlowHirIf != nullptr && controlFlowBinding != nullptr &&
             std::find(controlFlowHirIf->endedLoans.begin(),
                       controlFlowHirIf->endedLoans.end(),
                       controlFlowBinding->retainedLoan) !=
                 controlFlowHirIf->endedLoans.end(),
         "HIR should retain the semantic loan endpoint on the if statement");
  const lang::MirFunctionInstance *controlFlowMirFunction =
      controlFlowHirFunction == controlFlowBorrow.hir.functionInstances().end()
          ? nullptr
          : controlFlowBorrow.mir.findFunctionInstance(
                controlFlowHirFunction->id);
  bool foundConditionalEnd = false;
  if (controlFlowMirFunction != nullptr && controlFlowHirIf != nullptr &&
      controlFlowBinding != nullptr) {
    const auto loan = std::find_if(controlFlowMirFunction->body.loans.begin(),
                                   controlFlowMirFunction->body.loans.end(),
                                   [&](const lang::MirLoan &candidate) {
                                     return candidate.semanticLoan ==
                                            controlFlowBinding->retainedLoan;
                                   });
    if (loan != controlFlowMirFunction->body.loans.end()) {
      for (const lang::MirBlock &block : controlFlowMirFunction->body.blocks) {
        foundConditionalEnd =
            foundConditionalEnd ||
            std::any_of(block.instructions.begin(), block.instructions.end(),
                        [&](const lang::MirInstruction &instruction) {
                          return instruction.kind ==
                                     lang::MirInstructionKind::EndBorrow &&
                                 instruction.loan == loan->id &&
                                 instruction.hirStatement ==
                                     controlFlowHirIf->id;
                        });
      }
    }
  }
  expect(foundConditionalEnd,
         "MIR should end the retained loan after lowering the if merge");

  const lang::FrontendResult bothBranchBorrow = lang::Frontend().analyze(
      "both-branch-string-borrow.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { char left = "
      "*iterator; } else { char right = *iterator; } value.push_back('!'); "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(bothBranchBorrow.canGenerateCode(),
         "a retained borrow used on both conditional paths should end once "
         "after their merge");

  const lang::FrontendResult nestedConditionalBorrow = lang::Frontend().analyze(
      "nested-conditional-string-borrow.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { if (true) { "
      "char first = *iterator; } } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(nestedConditionalBorrow.canGenerateCode(),
         "a nested conditional use should project to the outer join that "
         "dominates the following mutation");
  const lang::FunctionDecl *nestedConditionalMain =
      findTopLevelFunction(nestedConditionalBorrow.program, "main");
  const auto *nestedConditionalIterator =
      nestedConditionalMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                nestedConditionalMain->body()->statements().at(1).get());
  const lang::BindingInfo *nestedConditionalBinding =
      nestedConditionalIterator == nullptr
          ? nullptr
          : nestedConditionalBorrow.semantics.findBinding(
                *nestedConditionalIterator);
  const lang::SemanticLoanInfo *nestedConditionalLoan =
      nestedConditionalBinding == nullptr
          ? nullptr
          : nestedConditionalBorrow.semantics.findLoan(
                nestedConditionalBinding->retainedLoan);
  expect(hasLoanEndpoint(
             nestedConditionalLoan, lang::SemanticLoanEndKind::AfterStatement,
             nestedConditionalMain->body()->statements().at(2).get()),
         "nested conditional uses should end at the outermost required join");

  const lang::FrontendResult conditionalCarrierMove = lang::Frontend().analyze(
      "conditional-carrier-move.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { mut auto moved = "
      "std::move(iterator); char first = *moved; } value.push_back('!'); "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!conditionalCarrierMove.canGenerateCode() &&
             hasDiagnostic(conditionalCarrierMove.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a carrier moved on only one branch must keep its owner loan "
         "conservative after the join");

  const lang::FrontendResult terminatingConditionalBorrow =
      lang::Frontend().analyze(
          "terminating-conditional-string-borrow.gti",
          "#include <std/string>\n"
          "int consume(bool choose) { mut std::string value = "
          "std::string(\"gti\"); mut auto iterator = value.begin(); if "
          "(choose) { char first = *iterator; return 1; } else { return 0; "
          "} }\n"
          "int main() { return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(terminatingConditionalBorrow.canGenerateCode(),
         "a terminating conditional should rely on path cleanup instead of "
         "creating an unreachable join endpoint");

  const lang::FrontendResult laterConditionalUse = lang::Frontend().analyze(
      "later-conditional-string-borrow.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { char first = "
      "*iterator; } value.push_back('!'); char second = *iterator; "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!laterConditionalUse.canGenerateCode() &&
             hasDiagnostic(laterConditionalUse.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a use after an if must keep the retained loan live across an "
         "intervening invalidation");

  const lang::FrontendResult branchLocalInvalidation = lang::Frontend().analyze(
      "branch-local-string-invalidation.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (value.empty()) { char first = "
      "*iterator; value.push_back('!'); } else { value.push_back('?'); } "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(branchLocalInvalidation.canGenerateCode(),
         "a retained borrow should end independently on both conditional "
         "paths before a branch-local invalidation");
  const lang::FunctionDecl *branchLocalMain =
      findTopLevelFunction(branchLocalInvalidation.program, "main");
  const auto *branchLocalIterator =
      branchLocalMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                branchLocalMain->body()->statements().at(1).get());
  const auto *branchLocalIf =
      branchLocalMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                branchLocalMain->body()->statements().at(2).get());
  const auto *branchLocalThen = branchLocalIf == nullptr
                                    ? nullptr
                                    : dynamic_cast<const lang::BlockStmt *>(
                                          branchLocalIf->thenBranch().get());
  const lang::Stmt *branchLocalLastUse =
      branchLocalThen == nullptr ? nullptr
                                 : branchLocalThen->statements().front().get();
  const lang::BindingInfo *branchLocalBinding =
      branchLocalIterator == nullptr
          ? nullptr
          : branchLocalInvalidation.semantics.findBinding(*branchLocalIterator);
  const lang::SemanticLoanInfo *branchLocalLoan =
      branchLocalBinding == nullptr
          ? nullptr
          : branchLocalInvalidation.semantics.findLoan(
                branchLocalBinding->retainedLoan);
  expect(branchLocalLoan != nullptr && branchLocalLoan->endpoints.size() == 2 &&
             hasLoanEndpoint(branchLocalLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             branchLocalLastUse) &&
             hasLoanEndpoint(branchLocalLoan,
                             lang::SemanticLoanEndKind::ElseBranchEntry,
                             branchLocalIf),
         "semantic loan metadata should describe both path-specific endings");

  const auto branchLocalHirFunction =
      branchLocalMain == nullptr
          ? branchLocalInvalidation.hir.functionInstances().end()
          : std::find_if(
                branchLocalInvalidation.hir.functionInstances().begin(),
                branchLocalInvalidation.hir.functionInstances().end(),
                [&](const lang::HirFunctionInstance &candidate) {
                  return candidate.source == branchLocalMain;
                });
  const lang::HirStatement *branchLocalHirIf = nullptr;
  const lang::HirStatement *branchLocalHirLastUse = nullptr;
  if (branchLocalHirFunction !=
      branchLocalInvalidation.hir.functionInstances().end()) {
    for (const lang::HirStatement &statement :
         branchLocalHirFunction->body.statements) {
      if (statement.source == branchLocalIf) {
        branchLocalHirIf = &statement;
      }
      if (statement.source == branchLocalLastUse) {
        branchLocalHirLastUse = &statement;
      }
    }
  }
  const lang::SemanticLoanId branchLocalLoanId =
      branchLocalBinding == nullptr ? 0 : branchLocalBinding->retainedLoan;
  expect(branchLocalHirIf != nullptr && branchLocalHirLastUse != nullptr &&
             std::find(branchLocalHirLastUse->endedLoans.begin(),
                       branchLocalHirLastUse->endedLoans.end(),
                       branchLocalLoanId) !=
                 branchLocalHirLastUse->endedLoans.end() &&
             std::find(branchLocalHirIf->elseEntryEndedLoans.begin(),
                       branchLocalHirIf->elseEntryEndedLoans.end(),
                       branchLocalLoanId) !=
                 branchLocalHirIf->elseEntryEndedLoans.end(),
         "HIR should preserve statement and branch-entry loan endpoints");

  const lang::MirFunctionInstance *branchLocalMirFunction =
      branchLocalHirFunction ==
              branchLocalInvalidation.hir.functionInstances().end()
          ? nullptr
          : branchLocalInvalidation.mir.findFunctionInstance(
                branchLocalHirFunction->id);
  std::size_t branchLocalEndCount = 0;
  bool branchLocalLastUseEnd = false;
  bool branchLocalElseEntryEnd = false;
  if (branchLocalMirFunction != nullptr) {
    const auto loan =
        std::find_if(branchLocalMirFunction->body.loans.begin(),
                     branchLocalMirFunction->body.loans.end(),
                     [&](const lang::MirLoan &candidate) {
                       return candidate.semanticLoan == branchLocalLoanId;
                     });
    if (loan != branchLocalMirFunction->body.loans.end()) {
      for (const lang::MirBlock &block : branchLocalMirFunction->body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
              instruction.loan != loan->id) {
            continue;
          }
          ++branchLocalEndCount;
          branchLocalLastUseEnd =
              branchLocalLastUseEnd ||
              (branchLocalHirLastUse != nullptr &&
               instruction.hirStatement == branchLocalHirLastUse->id);
          branchLocalElseEntryEnd =
              branchLocalElseEntryEnd ||
              (branchLocalHirIf != nullptr &&
               instruction.hirStatement == branchLocalHirIf->id);
        }
      }
    }
  }
  expect(branchLocalMirFunction != nullptr && branchLocalEndCount == 2 &&
             branchLocalLastUseEnd && branchLocalElseEntryEnd &&
             lang::verifyMirBody(branchLocalMirFunction->body).valid(),
         "MIR should end the same loan once on each conditional path");

  const lang::FrontendResult nestedBranchInvalidation =
      lang::Frontend().analyze(
          "nested-branch-string-invalidation.gti",
          "#include <std/string>\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); if (true) { if (true) { "
          "char first = *iterator; } value.push_back('!'); } return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!nestedBranchInvalidation.canGenerateCode() &&
             hasDiagnostic(nestedBranchInvalidation.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "nested conditional invalidation should remain conservative until "
         "nested edge proofs are implemented");

  const lang::FrontendResult loopBorrow = lang::Frontend().analyze(
      "loop-string-borrow.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); while (false) { char first = "
      "*iterator; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!loopBorrow.canGenerateCode() &&
             hasDiagnostic(loopBorrow.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "retained borrow endings across loop CFG remain conservative");

  const lang::FrontendResult scopedBorrow = lang::Frontend().analyze(
      "scoped-string-borrow.gti",
      "#include <std/string>\n"
      "int main() { mut std::string value = std::string(\"gti\"); { "
      "mut auto iterator = value.begin(); char first = *iterator; } "
      "value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(scopedBorrow.canGenerateCode(),
         "ending a nested lexical scope should deactivate its semantic owner "
         "loan before following statements");

  const lang::FrontendResult applicationStorageBorrow =
      lang::Frontend().analyze("application-storage-borrow.gti", R"(
class StorageBorrow<T> {
  gti_internal::storage<T>& data;
public:
  StorageBorrow(gti_internal::storage<T>& source) : data(source) {}
};
int main() { return 0; }
)");
  expect(!applicationStorageBorrow.canGenerateCode() &&
             hasDiagnostic(applicationStorageBorrow.diagnostics,
                           "References to compiler-private storage are not "
                           "supported"),
         "ordinary source must not acquire the standard library's private "
         "storage-borrow capability");

  const lang::ClassDecl *iteratorClass =
      findTopLevelClass(frontend.program, "BorrowingIterator");
  const lang::ClassLifecycleInfo *lifecycle =
      iteratorClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*iteratorClass);
  expect(lifecycle != nullptr && lifecycle->traits.containsBorrowedState &&
             !lifecycle->traits.copyable && lifecycle->traits.movable &&
             !lifecycle->traits.copyAssignable &&
             !lifecycle->traits.moveAssignable &&
             lifecycle->constructors.size() == 1 &&
             lifecycle->constructors.front().borrowParameter == 0,
         "stored-reference classes should be move-constructible borrow "
         "carriers with explicit constructor provenance");

  const lang::ClassDecl *rangeClass =
      findTopLevelClass(frontend.program, "SingleRange");
  const auto *begin = rangeClass == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::FunctionDecl *>(
                                rangeClass->members().at(3).get());
  const auto *returnStatement =
      begin == nullptr ? nullptr
                       : dynamic_cast<const lang::ReturnStmt *>(
                             begin->body()->statements().front().get());
  const auto *construction =
      returnStatement == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(returnStatement->value().get());
  const lang::ResolvedConstructionInfo *resolvedConstruction =
      construction == nullptr
          ? nullptr
          : frontend.semantics.findConstruction(*construction);
  expect(resolvedConstruction != nullptr &&
             resolvedConstruction->borrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             resolvedConstruction->borrowArgument == 0,
         "stored-reference construction should identify its exact owner "
         "argument before HIR lowering");

  bool foundStoredLoan = false;
  bool foundEscapingFieldLoan = false;
  bool foundReceiverReturnLoan = false;
  for (const lang::MirConstructorInstance &constructorInstance :
       frontend.mir.constructorInstances()) {
    for (const lang::MirLoan &loan : constructorInstance.body.loans) {
      foundEscapingFieldLoan =
          foundEscapingFieldLoan ||
          (loan.kind == lang::MirLoanKind::Stored && loan.escapes &&
           loan.storedField != 0 && loan.source != 0);
    }
  }
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirLoan &loan : function.body.loans) {
      foundStoredLoan =
          foundStoredLoan || (loan.kind == lang::MirLoanKind::Stored &&
                              !loan.escapes && loan.source != 0);
      foundReceiverReturnLoan =
          foundReceiverReturnLoan || (loan.kind == lang::MirLoanKind::Return &&
                                      loan.escapes && loan.source != 0);
    }
  }
  expect(frontend.mir.valid() && foundStoredLoan && foundEscapingFieldLoan &&
             foundReceiverReturnLoan,
         "MIR should retain local, field-stored, and receiver-returned borrow "
         "provenance");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("const T &value") != std::string::npos &&
             artifact.contents.find("BorrowingIterator(const "
                                    "BorrowingIterator &) = delete") !=
                 std::string::npos &&
             artifact.contents.find("BorrowingIterator(BorrowingIterator "
                                    "&&) = default") != std::string::npos,
         "the C++ backend should preserve read-only storage and frontend "
         "special-member decisions");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-stored-references.gti", R"(
class BorrowingIterator<T> {
  T& value;
public:
  BorrowingIterator(T& source) : value(source) {}
};

class SingleRange<T> {
  mut T value;
public:
  SingleRange(T initial) : value(initial) {}
  BorrowingIterator<T> begin() {
    return BorrowingIterator<T>(this.value);
  }
  void replace(T next) mut { this.value = next; }
};

class MutableStored {
  mut int& value;
public:
  MutableStored(mut int& source) : value(source) {}
};

class TwoReferences {
  int& left;
  int& right;
public:
  TwoReferences(int& first, int& second) : left(first), right(second) {}
};

class MissingBinding {
  int& value;
public:
  MissingBinding(int& source) {}
};

class NestedBorrow {
  BorrowingIterator<int> iterator;
public:
  NestedBorrow(BorrowingIterator<int> source)
      : iterator(std::move(source)) {}
};

class CleanupBorrow {
  int& value;
public:
  CleanupBorrow(int& source) : value(source) {}
  ~CleanupBorrow() {}
};

class GenericNest<T> {
  T nested;
public:
  GenericNest(T value) : nested(std::move(value)) {}
};

BorrowingIterator<int> escape(int& value) {
  return BorrowingIterator<int>(value);
}

SingleRange<int> global_range = SingleRange<int>(1);
BorrowingIterator<int> global_iterator = global_range.begin();

int main() {
  mut SingleRange<int> range = SingleRange<int>(1);
  mut BorrowingIterator<int> iterator = range.begin();
  BorrowingIterator<int> copy = iterator;
  BorrowingIterator<int> other = range.begin();
  iterator = std::move(other);
  BorrowingIterator<int> nested_source = range.begin();
  GenericNest<BorrowingIterator<int>> nested =
      GenericNest<BorrowingIterator<int>>(std::move(nested_source));
  BorrowingIterator<int> dangling = SingleRange<int>(2).begin();
  range.replace(2);
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics,
                        "Stored reference fields must be read-only") &&
          hasDiagnostic(invalid.diagnostics, "may store only one reference") &&
          hasDiagnostic(invalid.diagnostics,
                        "Every constructor of a stored-reference class") &&
          hasDiagnostic(invalid.diagnostics,
                        "Borrowed state cannot be nested") &&
          hasDiagnostic(invalid.diagnostics, "cannot declare a destructor") &&
          hasDiagnostic(invalid.diagnostics,
                        "may only be returned from an instance method") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot have global or static storage") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'copy'") &&
          hasDiagnostic(invalid.diagnostics, "Cannot assign a value of type") &&
          hasDiagnostic(invalid.diagnostics,
                        "Stored-reference value is derived from temporary "
                        "storage") &&
          hasDiagnostic(invalid.diagnostics,
                        "Mutable method cannot use storage while a "
                        "retained borrow"),
      "stored-reference diagnostics should reject unsupported reference "
      "graphs, escape, copies, assignment, and owner invalidation");
}

void testUniqueOwnershipAndAllocation() {
  const std::string source = R"(
struct Widget {
public:
  mut int value = 0;

  Widget(int initial) : value(initial) {}
  int read() { return this.value; }
  void increment() mut { this.value += 1; }
};

struct Holder {
  std::unique_ptr<Widget> widget = std::unique_ptr<Widget>();
};

int inspect(Widget& widget) { return widget.read(); }

std::unique_ptr<Widget> create(int value) {
  std::unique_ptr<Widget> widget = std::make_unique<Widget>(value);
  return std::move(widget);
}

int main() {
  mut std::unique_ptr<Widget> widget = create(4);
  Holder holder = Holder();
  std::unique_ptr<Widget> owners[1] = {std::make_unique<Widget>(9)};
  widget->increment();
  if (widget and widget != nullptr and inspect(*widget) == 5) {
    return 0;
  }
  std::unique_ptr<Widget>& owner_reference = widget;
  return 1;
}
)";

  lang::FrontendResult frontend = lang::Frontend().analyze(
      "unique-ownership.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected unique-owner diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(), "unique allocation, checked access, and "
                                     "explicit transfer should validate");

  const lang::FunctionDecl *create =
      findTopLevelFunction(frontend.program, "create");
  const auto *local = create == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                create->body()->statements().front().get());
  const lang::BindingInfo *binding =
      local == nullptr ? nullptr : frontend.semantics.findBinding(*local);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Class &&
             binding->traits.ownership == lang::OwnershipKind::Unique &&
             !binding->traits.copyable && binding->traits.movable &&
             binding->traits.drop == lang::DropKind::Lexical,
         "allocated owners should retain move-only lexical ownership metadata");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("class unique_ptr") != std::string::npos &&
             artifact.contents.find("std::unique_ptr<T> owner = nullptr") !=
                 std::string::npos &&
             artifact.contents.find("gti_std::unique_ptr<Widget>") !=
                 std::string::npos,
         "the C++ backend should emit the nominal unique_ptr wrapper");
  expect(
      artifact.contents.find(
          "gti_internal::backend::make_unique<T>(gti_internal::backend::"
          "forward_pack_argument(args)...)") != std::string::npos &&
          artifact.contents.find("gti_std::__gti_fn_") != std::string::npos &&
          artifact.contents.find("_make_unique<Widget>(value)") !=
              std::string::npos &&
          artifact.contents.find(
              "gti_internal::backend::owner_access(((*this)).owner)") !=
              std::string::npos &&
          artifact.contents.find("unique_owner_is_null") != std::string::npos &&
          artifact.contents.find("unique_owner_has_value") == std::string::npos,
      "the public factory should remain ordinary GTI while its wrapper "
      "lowers only narrow owner capabilities");
  expect(
      artifact.contents.find("return std::move(widget)") != std::string::npos &&
          artifact.contents.find("unique_ptr(const unique_ptr &) = delete") !=
              std::string::npos &&
          artifact.contents.find("unique_ptr(unique_ptr &&) = default") !=
              std::string::npos &&
          artifact.contents.find(
              "const gti_std::unique_ptr<Widget> widget =") ==
              std::string::npos,
      "nominal unique owners should remain move-only and physically movable");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-unique-ownership.gti", R"(
struct Widget {
public:
  int value = 0;
  Widget() {}
  int read() { return this.value; }
};

void consume(std::unique_ptr<Widget> widget) {}

std::unique_ptr<Widget> return_copy(std::unique_ptr<Widget> widget) {
  return widget;
}

std::unique_ptr<Widget> global_owner = std::unique_ptr<Widget>();
std::unique_ptr<Widget> global_owners[1] = {
    std::unique_ptr<Widget>()};

T identity<T>(T value) { return value; }

int main() {
  mut std::unique_ptr<Widget> owner = std::make_unique<Widget>();
  std::unique_ptr<Widget> copied = owner;
  consume(owner);
  std::unique_ptr<Widget> moved = std::move(owner);
  int after_move = owner->read();

  mut std::unique_ptr<Widget> conditional = std::make_unique<Widget>();
  if (true) {
    consume(std::move(conditional));
  }
  int maybe_moved = conditional->read();

  [[discard]] std::move(std::make_unique<Widget>());
  int wrong_member = moved.value;
  mut std::unique_ptr<Widget> missing;
  std::unique_ptr<Widget> generic = identity(std::move(moved));
  return after_move + maybe_moved + wrong_member;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode(),
         "copies, invalid global storage, and unsafe unique-owner use should "
         "fail");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "Cannot return a value of type 'std::unique_ptr") &&
          hasDiagnostic(invalid.diagnostics,
                        "Unique owners can only be local bindings") &&
          hasDiagnostic(invalid.diagnostics, "would copy a unique owner") &&
          hasDiagnostic(invalid.diagnostics, "already been moved") &&
          hasDiagnostic(invalid.diagnostics,
                        "may have been moved on another control-flow path") &&
          hasDiagnostic(invalid.diagnostics,
                        "temporaries are already values") &&
          hasDiagnostic(invalid.diagnostics,
                        "Unknown member 'value' on 'unique_ptr'") &&
          hasDiagnostic(invalid.diagnostics, "require explicit construction"),
      "unique-owner diagnostics should cover transfer, flow, and surface "
      "limits");

  const lang::FrontendResult invalidPointee =
      lang::Frontend().analyze("invalid-unique-pointee.gti", R"(
int main() {
  std::unique_ptr<int> primitive = std::make_unique<int>(1);
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(
      !invalidPointee.canGenerateCode() &&
          hasDiagnostic(invalidPointee.diagnostics,
                        "requires a class, struct, or generic object type") &&
          hasRelatedDiagnostic(invalidPointee.diagnostics,
                               "Concrete generic instance requested here"),
      "ordinary stdlib generic factories should diagnose invalid nested "
      "capability use at their concrete instantiation site");

  const lang::FrontendResult invalidConstructor =
      lang::Frontend().analyze("invalid-unique-constructor.gti", R"(
struct Widget {
public:
  Widget(int value) {}
};

int main() {
  auto owner = std::make_unique<Widget>(true);
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(
      !invalidConstructor.canGenerateCode() &&
          hasDiagnostic(invalidConstructor.diagnostics,
                        "No constructor of 'Widget' exactly matches argument "
                        "types (bool)") &&
          hasRelatedDiagnostic(invalidConstructor.diagnostics,
                               "Concrete generic instance requested here"),
      "concrete forwarding through an ordinary stdlib factory should retain "
      "frontend constructor checking");

  const lang::FrontendResult invalidCapabilities =
      lang::Frontend().analyze("invalid-owner-capabilities.gti", R"(
struct Widget {
public:
  int read() { return 0; }
};

int misuse_internal_owner() {
  gti_internal::unique_owner<Widget> owner =
      gti_internal::allocate_unique_owner<Widget>();
  mut Widget& writable =
      gti_internal::unique_owner_borrow_mut(owner);
  Widget& dangling = gti_internal::unique_owner_borrow(
      gti_internal::allocate_unique_owner<Widget>());
  return (*owner).read();
}
)",
                               {standardLibraryPrelude()});
  expect(!invalidCapabilities.canGenerateCode() &&
             hasDiagnostic(invalidCapabilities.diagnostics,
                           "requires mutable unique-owner storage") &&
             hasDiagnostic(invalidCapabilities.diagnostics,
                           "requires stable unique-owner storage") &&
             hasDiagnostic(invalidCapabilities.diagnostics,
                           "Dereference requires a class defining operator*"),
         "compiler-private owner capabilities should preserve their checked "
         "borrow boundary");

  const lang::FrontendResult hiddenOwnerPolicy =
      lang::Frontend().analyze("hidden-owner-policy.gti", R"(
struct Widget {};

int main() {
  gti_internal::unique_owner<Widget> owner =
      gti_internal::allocate_unique_owner<Widget>();
  bool engaged = gti_internal::unique_owner_has_value(owner);
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!hiddenOwnerPolicy.canGenerateCode() &&
             hasDiagnostic(hiddenOwnerPolicy.diagnostics,
                           "Undefined qualified name "
                           "'gti_internal::unique_owner_has_value'"),
         "the intrinsic owner should expose null representation rather than a "
         "stdlib-shaped has_value policy query");

  const std::string formatted = lang::Formatter().format(
      "std::unique_ptr<Widget> make(){return std::make_unique<Widget>();}"
      "int read(mut std::unique_ptr<Widget> value){return value->read();}");
  expect(formatted.find("value->read()") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "unique-owner spelling should format idempotently");
}

void testTypedHirGenericInstances() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("hir-generics.gti",
                               R"(
struct Widget {
public:
  int value = 7;
};

T transfer<T>(T value) {
  return std::move(value);
}

class Holder<T> {
  T value;
public:
  Holder(T value) : value(std::move(value)) {}
};

int main() {
  mut std::unique_ptr<Widget> owner = std::make_unique<Widget>();
  mut std::unique_ptr<Widget> transferred = transfer(std::move(owner));
  Holder<std::unique_ptr<Widget>> holder =
      Holder<std::unique_ptr<Widget>>(std::move(transferred));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected HIR diagnostic: " << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "typed HIR should recheck valid concrete ownership transfers");
  expect(valid.hir.valid() && !valid.hir.classInstances().empty() &&
             !valid.hir.functionInstances().empty() &&
             !valid.hir.constructorInstances().empty() &&
             valid.hir.valueCount() > 0,
         "typed HIR should retain concrete class, callable, binding, and "
         "value instances");

  bool foundMoveOnlyTransfer = false;
  bool foundResolvedTransferCall = false;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "transfer" &&
        !instance.parameterTypes.empty() && !instance.body.bindings.empty() &&
        !instance.body.bindings.front().info.traits.copyable) {
      foundMoveOnlyTransfer = true;
    }
    for (const lang::HirValue &value : instance.body.values) {
      if (value.functionTarget) {
        const lang::HirFunctionInstance *target =
            valid.hir.findFunctionInstance(*value.functionTarget);
        if (target != nullptr && target->source != nullptr &&
            target->source->name().lexeme == "transfer") {
          foundResolvedTransferCall = true;
        }
      }
    }
  }
  expect(foundMoveOnlyTransfer && foundResolvedTransferCall,
         "HIR should monomorphize move-only bindings and retain resolved call "
         "edges");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-hir-generics.gti",
                               R"(
struct Widget {};

T accidental_copy<T>(T value) {
  return value;
}

class BadHolder<T> {
  T value;
public:
  BadHolder(T value) : value(value) {}
};

int main() {
  mut std::unique_ptr<Widget> first = std::make_unique<Widget>();
  std::unique_ptr<Widget> copied = accidental_copy(std::move(first));
  mut std::unique_ptr<Widget> second = std::make_unique<Widget>();
  BadHolder<std::unique_ptr<Widget>> holder =
      BadHolder<std::unique_ptr<Widget>>(std::move(second));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode() && invalid.semanticValid &&
             !invalid.hirValid,
         "invalid ownership in a concrete generic instance should fail in "
         "HIR lowering");
  expect(hasDiagnostic(invalid.diagnostics, "Cannot return a value") &&
             hasDiagnostic(invalid.diagnostics, "Cannot initialize field") &&
             hasRelatedDiagnostic(invalid.diagnostics,
                                  "Concrete generic instance requested here"),
         "HIR diagnostics should report the generic body and instantiation "
         "site");
}

void testCompilerPrivateStorage() {
  const std::string source = R"(
class Buffer<T> {
  mut gti_internal::storage<T> data;
  mut uint64_t count = 0;
  mut uint64_t reserved = 0;

public:
  Buffer(uint64_t capacity)
      : data(gti_internal::allocate_storage<T>(capacity)), reserved(capacity) {}

  uint64_t capacity() {
    return this.reserved;
  }

  void push(T value) mut {
    gti_internal::storage_construct(this.data, this.count, value);
    this.count++;
  }

  T& at(uint64_t index) {
    return gti_internal::storage_read(this.data, index);
  }

  void grow(uint64_t capacity) mut {
    mut gti_internal::storage<T> replacement =
        gti_internal::allocate_storage<T>(capacity);
    gti_internal::storage_relocate(this.data, replacement, this.count);
    this.data = std::move(replacement);
    this.reserved = capacity;
  }

  void pop() mut {
    this.count--;
    gti_internal::storage_destroy(this.data, this.count);
  }
};

int main() {
  mut Buffer<int> values = Buffer<int>(uint64_t(2));
  mut Buffer<char> characters = Buffer<char>(uint64_t(1));
  values.push(7);
  values.push(9);
  characters.push('G');
  values.grow(uint64_t(4));
  if (values.capacity() == 4 and values.at(uint64_t(0)) == 7 and
      values.at(uint64_t(1)) == 9 and characters.at(uint64_t(0)) == 'G') {
    values.pop();
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend = lang::Frontend().analyze(
      "internal-storage.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected internal-storage diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "compiler-private storage should support vector-style growth and "
         "element lifetime operations");

  const lang::ClassDecl *buffer = findTopLevelClass(frontend.program, "Buffer");
  const auto *field = buffer == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                buffer->members().front().get());
  const lang::BindingInfo *binding =
      field == nullptr ? nullptr : frontend.semantics.findBinding(*field);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Storage &&
             binding->traits.ownership == lang::OwnershipKind::Unique &&
             !binding->traits.copyable && binding->traits.movable,
         "storage fields should retain move-only ownership metadata");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("gti_internal::backend::storage<T> data") !=
              std::string::npos &&
          artifact.contents.find(
              "gti_internal::backend::allocate_storage<T>(capacity)") !=
              std::string::npos &&
          artifact.contents.find("gti_internal::backend::storage_relocate") !=
              std::string::npos &&
          artifact.contents.find("storage_capacity") == std::string::npos &&
          artifact.contents.find("std::construct_at") != std::string::npos &&
          artifact.contents.find("std::destroy_at") != std::string::npos,
      "the C++ backend should lower storage through its private RAII helper");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-internal-storage.gti", R"(
gti_internal::storage<int> global =
    gti_internal::allocate_storage<int>(uint64_t(1));

int main() {
  mut gti_internal::storage<int> values =
      gti_internal::allocate_storage<int>(uint64_t(2));
  gti_internal::storage_construct(values, 0, 1);
  gti_internal::storage_construct(values, uint64_t(0), true);
  gti_internal::storage<int> copied = values;
  gti_internal::storage<int> moved = std::move(values);
  return gti_internal::storage_read(values, uint64_t(0));
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode(),
         "storage misuse should be rejected before backend generation");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-S2019") &&
             hasDiagnostic(invalid.diagnostics,
                           "only be used as a local binding or class field") &&
             hasDiagnostic(invalid.diagnostics, "requires a uint64_t") &&
             hasDiagnostic(invalid.diagnostics,
                           "this storage contains 'int32_t'") &&
             hasDiagnostic(invalid.diagnostics, "Cannot initialize 'copied'") &&
             hasDiagnostic(invalid.diagnostics, "has already been moved"),
         "storage diagnostics should cover placement, exact types, copying, "
         "and use after move");

  const lang::FrontendResult hiddenStoragePolicy =
      lang::Frontend().analyze("hidden-storage-policy.gti", R"(
int main() {
  mut gti_internal::storage<int> values =
      gti_internal::allocate_storage<int>(uint64_t(2));
  uint64_t capacity = gti_internal::storage_capacity(values);
  return int(capacity);
}
)",
                               {standardLibraryPrelude()});
  expect(!hiddenStoragePolicy.canGenerateCode() &&
             hasDiagnostic(hiddenStoragePolicy.diagnostics,
                           "Undefined qualified name "
                           "'gti_internal::storage_capacity'"),
         "storage allocation extent should remain private safety bookkeeping, "
         "not an intrinsic container-policy query");
}

void testAggregateOwnershipTraits() {
  const std::string source = R"(
class Buffer<T> {
  mut gti_internal::storage<T> data;
  uint64_t reserved;

public:
  Buffer(uint64_t capacity)
      : data(gti_internal::allocate_storage<T>(capacity)), reserved(capacity) {}

  uint64_t capacity() {
    return this.reserved;
  }
};

class NestedBuffer {
  Buffer<int> buffer;

public:
  NestedBuffer(uint64_t capacity) : buffer(Buffer<int>(capacity)) {}

  uint64_t capacity() {
    return this.buffer.capacity();
  }
};

struct CopyableValue {
  int value = 1;
};

Buffer<int> transfer(Buffer<int> value) {
  return std::move(value);
}

NestedBuffer transfer_nested(NestedBuffer value) {
  return std::move(value);
}

uint64_t inspect(Buffer<int>& value) {
  return value.capacity();
}

CopyableValue copy_value(CopyableValue value) {
  return value;
}

int main() {
  Buffer<int> buffer = Buffer<int>(uint64_t(2));
  Buffer<int> moved = transfer(std::move(buffer));
  NestedBuffer nested = NestedBuffer(uint64_t(3));
  NestedBuffer moved_nested = transfer_nested(std::move(nested));
  CopyableValue value = CopyableValue();
  CopyableValue copied = copy_value(value);
  return int(inspect(moved) + moved_nested.capacity()) - 5 + copied.value - 1;
}
)";

  lang::FrontendResult frontend = lang::Frontend().analyze(
      "aggregate-ownership.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected aggregate-owner diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(
      frontend.canGenerateCode(),
      "classes should inherit move-only traits from direct and nested fields");

  const lang::ClassDecl *bufferClass =
      findTopLevelClass(frontend.program, "Buffer");
  const lang::ClassDecl *copyableClass =
      findTopLevelClass(frontend.program, "CopyableValue");
  const lang::FunctionDecl *transfer =
      findTopLevelFunction(frontend.program, "transfer");
  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *buffer = main == nullptr
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().at(0).get());
  const auto *movedNested = main == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::VariableDecl *>(
                                      main->body()->statements().at(3).get());
  const auto *copied = main == nullptr
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().at(5).get());
  const lang::BindingInfo *parameter =
      transfer == nullptr
          ? nullptr
          : frontend.semantics.findBinding(transfer->parameters().front());
  const lang::BindingInfo *bufferBinding =
      buffer == nullptr ? nullptr : frontend.semantics.findBinding(*buffer);
  const lang::BindingInfo *nestedBinding =
      movedNested == nullptr ? nullptr
                             : frontend.semantics.findBinding(*movedNested);
  const lang::BindingInfo *copyableBinding =
      copied == nullptr ? nullptr : frontend.semantics.findBinding(*copied);
  const lang::ClassLifecycleInfo *bufferLifecycle =
      bufferClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*bufferClass);
  const lang::ClassLifecycleInfo *copyableLifecycle =
      copyableClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*copyableClass);
  expect(parameter != nullptr && bufferBinding != nullptr &&
             nestedBinding != nullptr && copyableBinding != nullptr &&
             parameter->traits.ownership == lang::OwnershipKind::Unique &&
             !parameter->traits.copyable && parameter->traits.movable &&
             parameter->traits.drop == lang::DropKind::Lexical &&
             !bufferBinding->traits.copyable && bufferBinding->traits.movable &&
             nestedBinding->traits.ownership == lang::OwnershipKind::Unique &&
             !nestedBinding->traits.copyable && nestedBinding->traits.movable &&
             copyableBinding->traits.copyable &&
             copyableBinding->traits.movable,
         "binding metadata should recursively propagate aggregate traits");
  expect(bufferLifecycle != nullptr && copyableLifecycle != nullptr &&
             bufferLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Deleted &&
             bufferLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             bufferLifecycle->copyAssignment ==
                 lang::SpecialMemberStatus::Deleted &&
             bufferLifecycle->moveAssignment ==
                 lang::SpecialMemberStatus::Generated &&
             copyableLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             copyableLifecycle->moveAssignment ==
                 lang::SpecialMemberStatus::Generated,
         "class lifecycle metadata should derive special-member availability "
         "from field ownership");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("const Buffer<std::int32_t> value") ==
              std::string::npos &&
          artifact.contents.find("const Buffer<std::int32_t> buffer") ==
              std::string::npos &&
          artifact.contents.find("return std::move(value)") !=
              std::string::npos &&
          artifact.contents.find("Buffer(const Buffer &) = delete;") !=
              std::string::npos &&
          artifact.contents.find("Buffer(Buffer &&) = default;") !=
              std::string::npos &&
          artifact.contents.find(
              "Buffer &operator=(const Buffer &) = delete;") !=
              std::string::npos &&
          artifact.contents.find("Buffer &operator=(Buffer &&) = default;") !=
              std::string::npos,
      "the backend should keep immutable move-only aggregates transferable");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-aggregate-ownership.gti", R"(
class Buffer {
  mut gti_internal::storage<int> data;

public:
  Buffer() : data(gti_internal::allocate_storage<int>(uint64_t(1))) {}
};

class NestedBuffer {
  Buffer buffer;

public:
  NestedBuffer() : buffer(Buffer()) {}
};

void consume(Buffer value) {}

Buffer return_copy(Buffer value) {
  return value;
}

int main() {
  Buffer owner = Buffer();
  Buffer copied = owner;
  consume(owner);
  Buffer moved = std::move(owner);
  Buffer moved_again = std::move(owner);

  NestedBuffer nested = NestedBuffer();
  NestedBuffer nested_copy = nested;
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode(),
         "copying or reusing move-only aggregates should fail semantically");
  expect(
      hasDiagnosticCode(invalid.diagnostics, "GTI-S2018") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot return a value of type 'Buffer'") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'copied'") &&
          hasDiagnostic(invalid.diagnostics, "would copy a unique owner") &&
          hasDiagnostic(invalid.diagnostics, "has already been moved") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'nested_copy'"),
      "aggregate diagnostics should cover return, call, copy, nesting, and "
      "use after move");
}

void testCompletePipeline() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
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
)");
  expect(!lexer.hadError(), "valid source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "valid source should parse");
  expect(program.declarations().size() == 2,
         "program should contain both functions");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program), "valid program should pass semantic checks");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find(
             "std::int32_t twice(const std::int32_t value)") !=
             std::string::npos,
         "emitter should lower function signatures");
  expect(generated.find("const std::int32_t result = twice(4)") !=
             std::string::npos,
         "emitter should make variables const by default");
  expect(generated.find("#include <iostream>") == std::string::npos,
         "emitter should not include print runtime support");
}

void testLoopControlStatements() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
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
)",
                           "loop-control.gti");
  expect(!lexer.hadError(), "break and continue should lex as keywords");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "loop control statements should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "break and continue should be valid in nested loop bodies");

  lang::HirLoweringResult hir = lang::HirLowerer().lower(program, semantic);
  expect(hir.valid(), "loop control should lower to typed HIR");
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(hir.program,
                                       lang::OptimizationLevel::O1);
  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       &optimizations, nullptr, &hir.program)
          .emit(program);
  expect(generated.find("continue;") != std::string::npos &&
             generated.find("break;") != std::string::npos,
         "the C++ backend should preserve loop control statements");

  auto invalidTokens = lexer.scan(R"(
void stop() {
  break;
}

void skip() {
  if (true) {
    continue;
  }
}
)",
                                  "invalid-loop-control.gti");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "loop control outside a loop should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "loop control outside loops should be rejected semantically");
  expect(
      countDiagnosticCode(invalidSemantic, "GTI-S2010") == 2 &&
          hasDiagnostic(invalidSemantic,
                        "'break' can only be used inside a loop or switch") &&
          hasDiagnostic(invalidSemantic,
                        "'continue' can only be used inside a loop"),
      "invalid break and continue should receive focused diagnostics");

  auto recoveredTokens = lexer.scan(R"(
void recover() {
  while (true) {
    break
    continue;
  }
}
)",
                                    "recover-loop-control.gti");
  lang::Parser recoveredParser(std::move(recoveredTokens));
  lang::Program recoveredProgram = recoveredParser.parse();
  expect(recoveredParser.errors().size() == 1 &&
             !recoveredParser.errors().front().fixes.empty() &&
             recoveredParser.errors().front().fixes.front().replacement == ";",
         "a missing loop-control semicolon should offer an insertion fix");
  lang::SemanticVisitor recoveredSemantic;
  expect(recoveredSemantic.check(recoveredProgram),
         "parser recovery should retain the following loop-control statement");
}

void testDoWhileStatements() {
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
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected do-while diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid,
         "a do-while loop should pass the complete frontend and MIR pipeline");

  const lang::HirFunctionInstance *run = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr && instance.source->name().lexeme == "run") {
      run = &instance;
      break;
    }
  }
  const lang::HirStatement *loop = nullptr;
  const lang::HirStatement *continueStatement = nullptr;
  const lang::HirStatement *breakStatement = nullptr;
  if (run != nullptr) {
    for (const lang::HirStatement &statement : run->body.statements) {
      if (statement.kind == lang::HirStatementKind::DoWhile) {
        loop = &statement;
      } else if (statement.kind == lang::HirStatementKind::Continue) {
        continueStatement = &statement;
      } else if (statement.kind == lang::HirStatementKind::Break) {
        breakStatement = &statement;
      }
    }
  }
  expect(loop != nullptr && loop->body && loop->condition,
         "typed HIR should preserve a distinct body-first do-while statement");

  const lang::MirFunctionInstance *loweredRun =
      run == nullptr ? nullptr : frontend.mir.findFunctionInstance(run->id);
  const lang::MirBlock *conditionBlock = nullptr;
  if (loweredRun != nullptr && loop != nullptr) {
    for (const lang::MirBlock &block : loweredRun->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Branch &&
          block.terminator.hirStatement == loop->id) {
        conditionBlock = &block;
        break;
      }
    }
  }

  const auto controlBlock =
      [&](const lang::HirStatement *statement) -> const lang::MirBlock * {
    if (loweredRun == nullptr || statement == nullptr) {
      return nullptr;
    }
    for (const lang::MirBlock &block : loweredRun->body.blocks) {
      if (block.terminator.hirStatement == statement->id) {
        return &block;
      }
    }
    return nullptr;
  };
  const lang::MirBlock *continueBlock = controlBlock(continueStatement);
  const lang::MirBlock *breakBlock = controlBlock(breakStatement);
  const auto dropsMarker = [](const lang::MirBlock *block) {
    return block != nullptr &&
           std::any_of(block->instructions.begin(), block->instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                lang::MirInstructionKind::Drop;
                       });
  };
  expect(conditionBlock != nullptr && continueBlock != nullptr &&
             continueBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
             continueBlock->terminator.target == conditionBlock->id &&
             dropsMarker(continueBlock),
         "do-while continue should clean the body scope and target the "
         "condition block");
  expect(conditionBlock != nullptr && breakBlock != nullptr &&
             breakBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
             breakBlock->terminator.target ==
                 conditionBlock->terminator.elseTarget &&
             dropsMarker(breakBlock),
         "do-while break should clean the body scope and target the loop exit");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact generated =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(generated.contents.find("do {") != std::string::npos &&
             generated.contents.find("while (value < 5);") != std::string::npos,
         "the C++ backend should preserve the body-first loop representation");

  const lang::FrontendResult invalidCondition = lang::Frontend().analyze(
      "invalid-do-while.gti", "void run() { do {} while (1); }\n");
  expect(!invalidCondition.canGenerateCode() &&
             hasDiagnostic(invalidCondition.diagnostics,
                           "Do-while condition must be bool"),
         "do-while should use GTI's exact contextual-bool rule");

  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("void recover() { do {} while (false) return; }\n",
                 "recover-do-while.gti"));
  lang::Program recovered = parser.parse();
  expect(parser.errors().size() == 1 &&
             !parser.errors().front().fixes.empty() &&
             parser.errors().front().fixes.front().replacement == ";" &&
             !recovered.declarations().empty(),
         "a missing do-while semicolon should offer a focused insertion fix");
}

void testConditionalExpressions() {
  const std::string source = R"(
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

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "conditional-expression.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected conditional-expression diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid,
         "conditional expressions should pass the complete frontend and MIR "
         "pipeline");

  const lang::HirFunctionInstance *choose = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "choose") {
      choose = &instance;
      break;
    }
  }
  const lang::HirValue *conditional = nullptr;
  if (choose != nullptr) {
    for (const lang::HirValue &value : choose->body.values) {
      if (value.kind == lang::HirValueKind::Conditional) {
        conditional = &value;
        break;
      }
    }
  }
  expect(conditional != nullptr && conditional->operands.size() == 3 &&
             conditional->info.type == lang::SemanticType::Int32 &&
             conditional->info.category == lang::ValueCategory::Value,
         "typed HIR should preserve condition, true arm, false arm, and the "
         "owned result type");

  const lang::MirFunctionInstance *loweredChoose =
      choose == nullptr ? nullptr
                        : frontend.mir.findFunctionInstance(choose->id);
  const lang::MirBlock *branch = nullptr;
  if (loweredChoose != nullptr && conditional != nullptr) {
    for (const lang::MirBlock &block : loweredChoose->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Branch &&
          block.terminator.hirValue == conditional->id) {
        branch = &block;
        break;
      }
    }
  }
  const lang::MirBlock *thenBlock =
      branch == nullptr
          ? nullptr
          : loweredChoose->body.findBlock(branch->terminator.target);
  const lang::MirBlock *elseBlock =
      branch == nullptr
          ? nullptr
          : loweredChoose->body.findBlock(branch->terminator.elseTarget);
  const bool armsMerge =
      thenBlock != nullptr && elseBlock != nullptr &&
      thenBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
      elseBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
      thenBlock->terminator.target == elseBlock->terminator.target;
  const lang::MirBlock *mergeBlock =
      !armsMerge ? nullptr
                 : loweredChoose->body.findBlock(thenBlock->terminator.target);
  const auto initializesConditional = [conditional](
                                          const lang::MirBlock *block) {
    return conditional != nullptr && block != nullptr &&
           std::any_of(block->instructions.begin(), block->instructions.end(),
                       [conditional](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                    lang::MirInstructionKind::Initialize &&
                                instruction.hirValue == conditional->id;
                       });
  };
  expect(branch != nullptr && armsMerge && initializesConditional(thenBlock) &&
             initializesConditional(elseBlock) && mergeBlock != nullptr &&
             std::any_of(
                 mergeBlock->instructions.begin(),
                 mergeBlock->instructions.end(),
                 [conditional](const lang::MirInstruction &instruction) {
                   return instruction.kind == lang::MirInstructionKind::Load &&
                          instruction.hirValue == conditional->id;
                 }),
         "MIR should evaluate one arm in separate blocks and load one merged "
         "result");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact generated =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(generated.contents.find(
             "static_cast<std::remove_cvref_t<decltype(((condition) ? "
             "(left) : (right)))>>(((condition) ? (left) : (right)))") !=
                 std::string::npos &&
             generated.contents.find("((condition) ? (std::move(left)) : "
                                     "(std::move(right)))") !=
                 std::string::npos,
         "the C++ backend should force an owned lazy result while preserving "
         "explicit ownership transfer");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-conditional-expression.gti", R"(
int invalid_condition() { return 1 ? 2 : 3; }
bool mismatched_arms(bool condition) { return condition ? 1 : false; }
struct Item {};
std::unique_ptr<Item> implicit_copy(
    bool condition,
    std::unique_ptr<Item> left,
    std::unique_ptr<Item> right) {
  return condition ? left : right;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "Conditional expression condition must be bool") &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2050") &&
             countDiagnosticCode(invalid.diagnostics, "GTI-S2052") == 2,
         "conditional expressions should diagnose non-bool conditions, "
         "inexact arm types, and implicit move-only copies");

  const lang::FrontendResult maybeMoved =
      lang::Frontend().analyze("conditional-move-state.gti", R"(
struct Item {
  int value;
  Item(int initial) : value(initial) {}
};

int inspect(bool condition) {
  mut auto owner = std::make_unique<Item>(1);
  auto selected = condition ? std::move(owner) : std::make_unique<Item>(2);
  return owner->value;
}
)",
                               {standardLibraryPrelude()});
  expect(!maybeMoved.canGenerateCode() &&
             hasDiagnostic(maybeMoved.diagnostics,
                           "may have been moved on another control-flow path"),
         "conditional arm states should merge explicit moves across runtime "
         "paths");

  const lang::FrontendResult borrowedResult =
      lang::Frontend().analyze("conditional-borrowed-result.gti", R"(
class BorrowedValue {
  int& value;

public:
  BorrowedValue(int& source) : value(source) {}
};

BorrowedValue choose_borrowed(
    bool condition, BorrowedValue left, BorrowedValue right) {
  return condition ? std::move(left) : std::move(right);
}
)",
                               {standardLibraryPrelude()});
  expect(!borrowedResult.canGenerateCode() &&
             hasDiagnosticCode(borrowedResult.diagnostics, "GTI-S2051"),
         "conditional values should reject branch-selected borrowed state "
         "until MIR can preserve its loan origin");

  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("int recover(bool value) { return value ? 1 2; }\n",
                 "recover-conditional-expression.gti"));
  lang::Program recovered = parser.parse();
  expect(parser.errors().size() == 1 &&
             !parser.errors().front().fixes.empty() &&
             parser.errors().front().fixes.front().replacement == ":" &&
             !recovered.declarations().empty(),
         "a missing conditional colon should offer a focused insertion fix");

  const std::string formatted = lang::Formatter().format(
      "int choose(bool c,int a,int b){return c?a:b?b:a;}");
  expect(formatted == R"(int choose(bool c, int a, int b) {
  return c ? a : b ? b : a;
}
)" && lang::Formatter().format(formatted) == formatted,
         "the formatter should apply stable C++-style conditional spacing");
}

void testSwitchStatements() {
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
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected switch diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "explicit integer, char, and scoped-enum switches should compile");

  const lang::FunctionDecl *classify =
      findTopLevelFunction(frontend.program, "classify");
  const auto *switchStatement =
      classify == nullptr || classify->body()->statements().empty()
          ? nullptr
          : dynamic_cast<const lang::SwitchStmt *>(
                classify->body()->statements().front().get());
  expect(switchStatement != nullptr && switchStatement->arms().size() == 3 &&
             switchStatement->arms()[1].labels.size() == 2,
         "the AST should group adjacent labels into one executable arm");
  expect(switchStatement != nullptr &&
             frontend.semantics.findSwitchCase(
                 *switchStatement->arms()[0].labels[0].value) != nullptr,
         "semantic analysis should retain normalized case constants");

  const lang::HirFunctionInstance *classifyInstance = nullptr;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "classify") {
      classifyInstance = &function;
      break;
    }
  }
  const lang::HirStatement *loweredSwitch =
      classifyInstance == nullptr || classifyInstance->body.roots.empty()
          ? nullptr
          : classifyInstance->body.findStatement(
                classifyInstance->body.roots.front());
  expect(loweredSwitch != nullptr &&
             loweredSwitch->kind == lang::HirStatementKind::Switch &&
             loweredSwitch->switchArms.size() == 3 &&
             loweredSwitch->switchArms[1].labels.size() == 2 &&
             loweredSwitch->switchArms[1].labels[0].constant.has_value(),
         "HIR should preserve switch arms and normalized labels for backends");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &frontend.semantics, &frontend.hir)
          .emit(frontend.program);
  expect(generated.find("switch (stage)") != std::string::npos &&
             generated.find("case Stage::Boot:") != std::string::npos &&
             generated.find("case static_cast<std::uint64_t>(7):") !=
                 std::string::npos &&
             generated.find("case Stage::Ready:\n    case Stage::Running:\n"
                            "    {") != std::string::npos,
         "the C++ backend should emit explicit labels with arm-local scopes");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-switch.gti", R"(
enum class State { Idle, Running };
enum class Alias { First = 1, Second = 1 };
int dynamic_label() { return 1; }

int invalid(uint64_t wide, int value, Alias alias) {
  switch (true) {
  case true:
    break;
  default:
    break;
  }
  switch (wide) {
  case 1:
    break;
  case uint64_t(2):
    break;
  case uint64_t(2):
    break;
  default:
    break;
  default:
    break;
  }
  switch (alias) {
  case Alias::First:
    break;
  case Alias::Second:
    break;
  default:
    break;
  }
  switch (value) {
  case dynamic_label():
    break;
  case 3:
    int missing_terminator = 3;
  default:
    continue;
  }
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid switch subjects, labels, and arms should block lowering");
  expect(
      countDiagnosticCode(invalid.diagnostics, "GTI-S2037") == 7 &&
          hasDiagnostic(invalid.diagnostics,
                        "Switch expression must have an integer") &&
          hasDiagnostic(invalid.diagnostics,
                        "does not exactly match subject type") &&
          hasDiagnostic(invalid.diagnostics, "Duplicate switch case value") &&
          hasDiagnostic(invalid.diagnostics, "only one 'default' label") &&
          hasDiagnostic(invalid.diagnostics,
                        "must be a compile-time integer") &&
          hasDiagnostic(invalid.diagnostics,
                        "without an explicit terminator") &&
          hasDiagnostic(invalid.diagnostics,
                        "'continue' can only be used inside a loop") &&
          hasRelatedDiagnostic(invalid.diagnostics,
                               "First matching case label") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "do not perform implicit conversions"),
      "switch diagnostics should explain exact matching, duplicates, and "
      "explicit termination");

  lang::Lexer lexer;
  lang::Parser recoveryParser(lexer.scan(R"(
void recover(int value) {
  switch (value) {
  case 0
    int skipped_arm = 0;
  case 1:
    break;
  default:
    break;
  }
}
)",
                                         "recover-switch.gti"));
  const lang::Program recovered = recoveryParser.parse();
  expect(recoveryParser.errors().size() == 1 &&
             !recoveryParser.errors().front().fixes.empty() &&
             recoveryParser.errors().front().fixes.front().replacement == ":",
         "a missing case colon should offer an insertion fix and recover");
  lang::SemanticVisitor recoveredSemantic;
  expect(recoveredSemantic.check(recovered),
         "switch recovery should retain later case and default arms");

  const std::string formatted = lang::Formatter().format(
      "void choose(mut int value){switch(value){case 0:case 1:value+=1;"
      "break;default:break;}}");
  expect(formatted == R"(void choose(mut int value) {
  switch (value) {
  case 0:
  case 1:
    value += 1;
    break;
  default:
    break;
  }
}
)" && lang::Formatter().format(formatted) == formatted,
         "switch formatting should match C++ label indentation and remain "
         "idempotent");
}

void testFixedWidthIntegers() {
  lang::Lexer aliasLexer;
  const std::vector<lang::Token> aliases = aliasLexer.scan(
      "int8 int8_t int16 int16_t int32 int32_t int64 int64_t "
      "uint8 uint8_t uint16 uint16_t uint32 uint32_t uint64 uint64_t");
  expect(!aliasLexer.hadError() && aliases.size() == 17 &&
             aliases[0].kind == lang::TokenKind::INT8 &&
             aliases[0].kind == aliases[1].kind &&
             aliases[2].kind == lang::TokenKind::INT16 &&
             aliases[2].kind == aliases[3].kind &&
             aliases[4].kind == lang::TokenKind::INT32 &&
             aliases[4].kind == aliases[5].kind &&
             aliases[6].kind == lang::TokenKind::INT64 &&
             aliases[6].kind == aliases[7].kind &&
             aliases[8].kind == lang::TokenKind::UINT8 &&
             aliases[8].kind == aliases[9].kind &&
             aliases[10].kind == lang::TokenKind::UINT16 &&
             aliases[10].kind == aliases[11].kind &&
             aliases[12].kind == lang::TokenKind::UINT32 &&
             aliases[12].kind == aliases[13].kind &&
             aliases[14].kind == lang::TokenKind::UINT64 &&
             aliases[14].kind == aliases[15].kind,
         "canonical and suffix-less fixed-width spellings should lex to the "
         "same primitive token kinds");

  const lang::FrontendResult legacy =
      lang::Frontend().analyze("legacy-integer-aliases.gti", R"(
int main() {
  int8 signed8 = 1;
  int16 signed16 = 2;
  int32 signed32 = 3;
  int64 signed64 = 4;
  uint8 unsigned8 = 5;
  uint16 unsigned16 = 6;
  uint32 unsigned32 = 7;
  uint64 unsigned64 = 8;
  return 0;
}
)");
  expect(legacy.canGenerateCode() && legacy.diagnostics.empty(),
         "suffix-less fixed-width aliases should remain valid complete GTI "
         "programs without a prelude include");

  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
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
  uint32_t counter = 1;
  uint32_t next = counter + 1;
  bool has_next = next > 0;
  return promoted;
}
)");
  expect(!lexer.hadError(), "fixed-width integer source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "fixed-width integer declarations should parse");

  lang::SemanticVisitor semantic;
  const bool valid = semantic.check(program);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
      std::cerr << "Unexpected fixed-width diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid,
         "in-range literals and widening conversions should be valid");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("#include <cstdint>") != std::string::npos &&
             generated.find("const std::int8_t minimum8 = "
                            "gti_internal::backend::negate(128)") !=
                 std::string::npos &&
             generated.find("const std::int16_t widened16 = minimum8") !=
                 std::string::npos &&
             generated.find("const std::int32_t default32 = maximum32") !=
                 std::string::npos &&
             generated.find("const std::int64_t maximum64 = "
                            "9223372036854775807") != std::string::npos &&
             generated.find("const std::int64_t minimum64 = "
                            "(-9223372036854775807LL - 1)") !=
                 std::string::npos &&
             generated.find("const std::uint8_t maximum_u8 = 255") !=
                 std::string::npos &&
             generated.find("const std::uint16_t widened_u16 = maximum_u8") !=
                 std::string::npos &&
             generated.find("const std::uint32_t default_u32 = maximum_u32") !=
                 std::string::npos &&
             generated.find("const std::uint64_t maximum_u64 = "
                            "18446744073709551615ULL") != std::string::npos &&
             generated.find("const std::uint32_t hexadecimal_pattern = "
                            "42405") != std::string::npos &&
             generated.find("const std::uint8_t binary_pattern = 165") !=
                 std::string::npos &&
             generated.find("int main()") != std::string::npos,
         "integer widths should lower to cstdint types while main stays valid");

  auto invalidTokens = lexer.scan(R"(
int8_t too_high = 128;
int8_t too_low = -129;
int16_t wide = 1;
int8_t narrowing = wide;
int alias_overflow = 2147483648;
int64_t signed_overflow = 9223372036854775808;
uint8_t unsigned_negative = -1;
uint8_t unsigned_overflow = 256;
uint16_t unsigned_wide = 1;
uint8_t unsigned_narrowing = unsigned_wide;
int32_t signed_value = 1;
uint32_t unsigned_value = 1;
uint32_t signed_to_unsigned = signed_value;
int32_t unsafe_sum = signed_value + unsigned_value;
bool unsafe_comparison = signed_value < unsigned_value;
uint32_t unsafe_negation = -unsigned_value;
uint alias_unsigned_overflow = 4294967296;
)");
  expect(!lexer.hadError(),
         "signed range errors should be diagnosed semantically");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "out-of-range source should still parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "out-of-range literals and narrowing should be rejected");
  expect(invalidSemantic.errors().size() == 13,
         "each invalid fixed-width integer conversion should be diagnosed");

  auto lexicalOverflow =
      lexer.scan("uint64_t too_large = 18446744073709551616;");
  (void)lexicalOverflow;
  expect(lexer.hadError() && lexer.errors().size() == 1,
         "integer literals larger than uint64_t should fail during lexing");

  auto invalidPrefixed =
      lexer.scan("uint64_t bad_hex = 0xGG; uint64_t bad_binary = 0b102; "
                 "uint64_t wide_hex = 0x10000000000000000;");
  (void)invalidPrefixed;
  expect(lexer.hadError() && lexer.errors().size() == 3 &&
             std::all_of(lexer.errors().begin(), lexer.errors().end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-L0007";
                         }),
         "malformed and overflowing hexadecimal or binary literals should "
         "fail during lexing");

  const std::string formatted =
      lang::Formatter().format("#include <std/uint64>\n"
                               "int8 small=1;int64 large=small;uint8 byte=0xFF;"
                               "uint64 wide=0b1010;");
  expect(formatted == "#include <std/uint64>\n"
                      "int8_t small = 1;\nint64_t large = small;\n"
                      "uint8_t byte = 0xFF;\nuint64_t wide = 0b1010;\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should canonicalize legacy fixed-width aliases, preserve "
         "include paths and prefixed integer literals, and remain idempotent");
}

void testCharactersAndStringViews() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("text-values.gti", R"(
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

int main() {
  if (chars_match and text_matches and text_size == 8 and text_not_empty and
      first == 'G' and embedded_zero == '\0') { return 0; }
  return 1;
}
)",
                               {standardLibraryPrelude()});
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "character values and literal-backed string views should validate");

  const lang::VariableDecl *textDeclaration = nullptr;
  for (const lang::StmtPtr &declaration : valid.program.declarations()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(declaration.get());
    if (variable != nullptr && variable->name().lexeme == "text") {
      textDeclaration = variable;
      break;
    }
  }
  const lang::BindingInfo *textBinding =
      textDeclaration == nullptr
          ? nullptr
          : valid.semantics.findBinding(*textDeclaration);
  expect(textBinding != nullptr &&
             textBinding->type == lang::SemanticType::StringView &&
             textBinding->traits.drop == lang::DropKind::Trivial &&
             textBinding->traits.copyable,
         "string views should remain trivial copyable values in semantics");

  const lang::OptimizationResult optimized =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O1);
  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       &optimized, &valid.semantics, &valid.hir)
          .emit(valid.program);
  expect(generated.find("using string_view = std::string_view;") !=
                 std::string::npos &&
             generated.find("std::uint8_t{71}") != std::string::npos &&
             generated.find("std::string_view{\"GTI\\000text\", 8}") !=
                 std::string::npos &&
             generated.find("gti_internal::backend::string_view_at") !=
                 std::string::npos &&
             generated.find("const bool literal_chars_match = true") !=
                 std::string::npos,
         "the C++ backend should preserve counted text and exact code units");

  const lang::FrontendResult invalidView = lang::Frontend().analyze(
      "invalid-string-view.gti",
      "int main() { mut std::string_view view = \"abc\"; "
      "view[0] = 'x'; char outside = \"abc\"[3]; "
      "char wrong = view[false]; view.missing(); return 0; }",
      {standardLibraryPrelude()});
  expect(!invalidView.canGenerateCode() &&
             countDiagnosticCode(invalidView.diagnostics, "GTI-S2035") == 4 &&
             hasDiagnostic(invalidView.diagnostics,
                           "character access is read-only") &&
             hasDiagnostic(invalidView.diagnostics,
                           "outside the valid range [0, 3)") &&
             hasDiagnostic(invalidView.diagnostics,
                           "index must have an integer type") &&
             hasDiagnostic(invalidView.diagnostics,
                           "Unknown std::string_view member"),
         "string-view traversal should reject mutation, invalid literal "
         "indexes, non-integer indexes, and unknown members in semantics");

  lang::Lexer lexer;
  lang::Parser expressionParser(lexer.scan("'a'"));
  lang::ExprPtr expression = expressionParser.parseExpression();
  expect(expression != nullptr && !expressionParser.hadError() &&
             lang::AstPrinter().print(*expression) == "'a'",
         "character literals should retain their source-level AST spelling");

  (void)lexer.scan("char empty = ''; char many = 'ab'; "
                   "char escape = '\\q'; char open = 'x\n");
  expect(countDiagnosticCode(lexer.errors(), "GTI-L0010") == 2 &&
             countDiagnosticCode(lexer.errors(), "GTI-L0005") == 1 &&
             countDiagnosticCode(lexer.errors(), "GTI-L0009") == 1,
         "invalid character widths, escapes, and termination should have "
         "focused lexical diagnostics");

  const lang::FrontendResult numeric = lang::Frontend().analyze(
      "character-numeric.gti",
      "int main() { char value = 'A'; int number = value; "
      "return value + 1; }",
      {standardLibraryPrelude()});
  expect(!numeric.canGenerateCode() &&
             hasDiagnostic(numeric.diagnostics, "value of type 'char'") &&
             hasDiagnostic(numeric.diagnostics, "requires numeric operands"),
         "char should not inherit integer arithmetic or conversions");

  const lang::FrontendResult obsolete = lang::Frontend().analyze(
      "obsolete-string.gti", "int main() { string text = \"old\"; return 0; }",
      {standardLibraryPrelude()});
  const lang::Diagnostic *migration =
      findDiagnosticByCode(obsolete.diagnostics, "GTI-S2033");
  expect(!obsolete.canGenerateCode() && migration != nullptr &&
             migration->fixes.size() == 1 &&
             migration->fixes.front().replacement == "std::string_view" &&
             !migration->hints.empty(),
         "obsolete string declarations should receive a mechanical migration "
         "diagnostic");

  const std::string formatted = lang::Formatter().format(
      "int main(){char quote='\\\'';char newline='\\n';return 0;}");
  expect(formatted.find("char quote = '\\\'';") != std::string::npos &&
             formatted.find("char newline = '\\n';") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve character literal escapes");
}

void testStandardString() {
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() / "gti-standard-string/main.gti";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry, R"(
#include <std/string>

int main() {
  std::string_view literal = "engine";
  mut std::string value = std::string(literal);
  value.push_back(' ');
  value.append("runtime");
  mut std::string copy = value.clone();
  bool cloned = value == copy;
  copy[0] = 'E';
  char first = copy.at(0);
  copy.clear();
  if (literal.size() == 6 and !literal.empty() and literal[0] == 'e' and
      cloned and value == "engine runtime" and first == 'E' and copy.empty()) {
    return 0;
  }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected standard-string diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "std::string should be an ordinary source-defined owner over char "
         "storage");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &valid.semantics, &valid.hir)
          .emit(valid.program);
  expect(
      generated.find("class string") != std::string::npos &&
          generated.find("gti_internal::backend::storage<std::uint8_t> data") !=
              std::string::npos &&
          generated.find("string(const string &) = delete;") !=
              std::string::npos &&
          generated.find("string(string &&) = default;") != std::string::npos &&
          generated.find("gti_internal::backend::storage_read_mut") !=
              std::string::npos,
      "std::string lowering should retain nominal move-only lifecycle and "
      "checked mutable storage access");

  const lang::FrontendResult invalidCopy = lang::Frontend().analyze(
      entry, R"(
#include <std/string>
int main() {
  std::string original = std::string("text");
  std::string copied = original;
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidCopy.canGenerateCode() &&
             hasDiagnosticCode(invalidCopy.diagnostics, "GTI-S2003") &&
             hasDiagnostic(invalidCopy.diagnostics,
                           "Cannot initialize 'copied'") &&
             hasDiagnosticHint(invalidCopy.diagnostics,
                               "Move-only owners cannot be copied"),
         "std::string copies should require explicit allocating clone() rather "
         "than hidden allocation");
}

void testLogicalOperatorSpellings() {
  lang::Lexer lexer;

  const std::vector<lang::Token> aliasTokens = lexer.scan("and && & or || |");
  expect(!lexer.hadError() && aliasTokens.size() == 7 &&
             aliasTokens[0].kind == lang::TokenKind::AND &&
             aliasTokens[1].kind == lang::TokenKind::AND &&
             aliasTokens[1].lexeme == "&&" &&
             aliasTokens[2].kind == lang::TokenKind::AMPERSAND &&
             aliasTokens[3].kind == lang::TokenKind::OR &&
             aliasTokens[4].kind == lang::TokenKind::OR &&
             aliasTokens[4].lexeme == "||" &&
             aliasTokens[5].kind == lang::TokenKind::PIPE,
         "logical words and symbols should normalize without consuming "
         "single-character bitwise operators");

  lang::Parser symbolicPrecedence(lexer.scan("true || false && false"));
  lang::ExprPtr symbolicExpression = symbolicPrecedence.parseExpression();
  expect(symbolicExpression != nullptr && !symbolicPrecedence.hadError() &&
             lang::AstPrinter().print(*symbolicExpression) ==
                 "(|| true (&& false false))",
         "symbolic logical operators should use C++ precedence");

  lang::Parser wordPrecedence(lexer.scan("true or false and false"));
  lang::ExprPtr wordExpression = wordPrecedence.parseExpression();
  expect(wordExpression != nullptr && !wordPrecedence.hadError() &&
             lang::AstPrinter().print(*wordExpression) ==
                 "(or true (and false false))",
         "word logical operators should retain their existing precedence");

  auto validTokens = lexer.scan(R"(
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
)");
  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "mixed logical operator spellings should parse together");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "mixed logical operator spellings should share boolean semantics");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("&&") != std::string::npos &&
             generated.find("||") != std::string::npos,
         "both logical spellings should lower to short-circuit C++ operators");

  auto invalidTokens =
      lexer.scan("int main() { if (1 && true) { return 0; } return 1; }");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid symbolic logical operands should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram) &&
             hasDiagnostic(invalidSemantic, "Logical operands must be bool."),
         "symbolic logical operators should use existing operand diagnostics");

  const std::string formatted =
      lang::Formatter().format("int main(){bool value=true&&false||true;"
                               "if(value||false&&true){return 0;}return 1;}");
  expect(formatted.find("bool value = true && false || true;") !=
                 std::string::npos &&
             formatted.find("if (value || false && true)") != std::string::npos,
         "formatter should treat symbolic logical aliases as binary operators");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted symbolic logical aliases should be idempotent");
}

void testIntegerBitwiseAndModuloOperators() {
  lang::Lexer lexer;

  lang::Parser bitwisePrecedence(lexer.scan("1 | 2 ^ 3 & 4"));
  lang::ExprPtr bitwiseExpression = bitwisePrecedence.parseExpression();
  expect(bitwiseExpression != nullptr &&
             lang::AstPrinter().print(*bitwiseExpression) ==
                 "(| 1 (^ 2 (& 3 4)))",
         "bitwise operators should follow C++ precedence");

  lang::Parser shiftPrecedence(lexer.scan("1 < 2 << 3 + 4 % 2"));
  lang::ExprPtr shiftExpression = shiftPrecedence.parseExpression();
  expect(shiftExpression != nullptr &&
             lang::AstPrinter().print(*shiftExpression) ==
                 "(< 1 (<< 2 (+ 3 (% 4 2))))",
         "modulo and shifts should integrate with arithmetic precedence");

  lang::Parser unaryPrecedence(lexer.scan("~1 * 2"));
  lang::ExprPtr unaryExpression = unaryPrecedence.parseExpression();
  expect(unaryExpression != nullptr &&
             lang::AstPrinter().print(*unaryExpression) == "(* (~ 1) 2)",
         "bitwise complement should bind as a unary operator");

  lang::Parser separatedShift(lexer.scan("1 > > 2"));
  expect(separatedShift.parseExpression() == nullptr &&
             separatedShift.hadError(),
         "spaced angle tokens should not become a shift operator");

  auto validTokens = lexer.scan(R"(
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
)");
  expect(!lexer.hadError(), "integer bitwise source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "integer bitwise and modulo operators should parse");

  lang::SemanticVisitor validSemantic;
  const bool valid = validSemantic.check(validProgram);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : validSemantic.errors()) {
      std::cerr << "Unexpected integer operator diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid,
         "valid integer bitwise and modulo operations should type-check");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("gti_internal::backend::modulo(") !=
                 std::string::npos &&
             generated.find("gti_internal::backend::shift_left(") !=
                 std::string::npos &&
             generated.find("gti_internal::backend::shift_right(") !=
                 std::string::npos,
         "modulo and shifts should lower through checked integer helpers");
  expect(generated.find("(left & right)") != std::string::npos &&
             generated.find("(left ^ right)") != std::string::npos,
         "ordinary bitwise operators should lower directly");
  expect(generated.find("modulo by zero") != std::string::npos &&
             generated.find("std::bit_cast") != std::string::npos,
         "generated helpers should define invalid modulo and shift behavior");

  auto invalidTokens = lexer.scan(R"(
float decimal = 1.0;
bool condition = true;
int invalid_modulo = decimal % 2;
int invalid_and = condition & true;
int invalid_shift = 1 << decimal;
int invalid_complement = ~decimal;
int zero_modulo = 7 % 0;
int negative_shift = 1 << -1;
int wide_shift = 1 >> 32;
int32_t signed_value = 1;
uint32_t unsigned_value = 1;
int unsafe_bits = signed_value | unsigned_value;
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid integer operator types should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid integer operators should be rejected semantically");
  expect(hasDiagnostic(invalidSemantic, "requires integer operands") &&
             hasDiagnostic(invalidSemantic,
                           "Bitwise complement requires an integer"),
         "floats and bools should not gain bitwise behavior");
  expect(hasDiagnostic(invalidSemantic, "Modulo divisor cannot be zero"),
         "literal modulo by zero should be rejected before lowering");
  expect(hasDiagnostic(invalidSemantic, "Shift count cannot be negative") &&
             hasDiagnostic(invalidSemantic, "Shift count must be less than 32"),
         "invalid literal shift counts should be diagnosed");
  expect(hasDiagnostic(invalidSemantic, "no safe common type"),
         "bitwise operations should preserve safe signed/unsigned rules");

  const std::string formatted = lang::Formatter().format(
      "int value=(mask&3)|((mask^1)<<2);int mod=value%7;"
      "int inv=~value;int shifted=value>>1;");
  expect(formatted == "int value = (mask & 3) | ((mask ^ 1) << 2);\n"
                      "int mod = value % 7;\n"
                      "int inv = ~value;\n"
                      "int shifted = value >> 1;\n",
         "formatter should use C++ spacing for integer operators");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted integer operators should be idempotent");
}

void testCheckedArithmeticAndCompoundAssignments() {
  lang::Lexer lexer;
  const std::vector<lang::Token> operators =
      lexer.scan("+= -= *= /= %= &= |= ^= <<= >>=");
  expect(!lexer.hadError() && operators.size() == 11 &&
             operators[0].kind == lang::TokenKind::PLUS_EQUAL &&
             operators[1].kind == lang::TokenKind::MINUS_EQUAL &&
             operators[2].kind == lang::TokenKind::STAR_EQUAL &&
             operators[3].kind == lang::TokenKind::SLASH_EQUAL &&
             operators[4].kind == lang::TokenKind::PERCENT_EQUAL &&
             operators[5].kind == lang::TokenKind::AMPERSAND_EQUAL &&
             operators[6].kind == lang::TokenKind::PIPE_EQUAL &&
             operators[7].kind == lang::TokenKind::CARET_EQUAL &&
             operators[8].kind == lang::TokenKind::SHIFT_LEFT_EQUAL &&
             operators[9].kind == lang::TokenKind::SHIFT_RIGHT_EQUAL,
         "the lexer should retain every compound assignment as one token");

  lang::Parser expressionParser(lexer.scan("value *= 2"));
  lang::ExprPtr expression = expressionParser.parseExpression();
  expect(expression != nullptr && !expressionParser.hadError() &&
             lang::AstPrinter().print(*expression) == "(*= value 2)",
         "compound assignment should parse as a right-associative assignment");

  const lang::FrontendResult valid =
      lang::Frontend().analyze("checked-arithmetic.gti", R"(
struct Meter {
  mut int value = 1;
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
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected checked-arithmetic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "checked arithmetic and every compound assignment should validate");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &valid.semantics, &valid.hir)
          .emit(valid.program);
  const std::vector<std::string_view> checkedHelpers = {
      "backend::add(",
      "backend::subtract(",
      "backend::multiply(",
      "backend::divide(",
      "backend::negate(",
      "backend::add_assign(",
      "backend::subtract_assign(",
      "backend::multiply_assign(",
      "backend::divide_assign(",
      "backend::remainder_assign(",
      "backend::bitwise_and_assign(",
      "backend::bitwise_or_assign(",
      "backend::bitwise_xor_assign(",
      "backend::shift_left_assign(",
      "backend::shift_right_assign(",
      "backend::post_increment(",
  };
  expect(std::all_of(checkedHelpers.begin(), checkedHelpers.end(),
                     [&](std::string_view helper) {
                       return generated.find(helper) != std::string::npos;
                     }),
         "arithmetic and mutation should lower through checked GTI helpers");
  expect(generated.find("integer addition overflow") != std::string::npos &&
             generated.find("integer subtraction overflow") !=
                 std::string::npos &&
             generated.find("integer multiplication overflow") !=
                 std::string::npos &&
             generated.find("integer division overflow") != std::string::npos &&
             generated.find("division by zero") != std::string::npos,
         "generated arithmetic helpers should carry stable failure reasons");

  const lang::OptimizationResult optimized =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O1);
  const lang::HirFunctionInstance *negationInstance = nullptr;
  const lang::HirFunctionInstance *additionInstance = nullptr;
  const lang::HirFunctionInstance *unsignedSubtractionInstance = nullptr;
  const lang::HirFunctionInstance *foldedArithmeticInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "checked_negation") {
      negationInstance = &instance;
    } else if (instance.source->name().lexeme == "checked_addition") {
      additionInstance = &instance;
    } else if (instance.source->name().lexeme ==
               "checked_unsigned_subtraction") {
      unsignedSubtractionInstance = &instance;
    } else if (instance.source->name().lexeme == "folded_arithmetic") {
      foldedArithmeticInstance = &instance;
    }
  }
  const bool retainsCheckedNegation =
      negationInstance != nullptr &&
      std::any_of(negationInstance->body.values.begin(),
                  negationInstance->body.values.end(),
                  [&](const lang::HirValue &value) {
                    return value.kind == lang::HirValueKind::Unary &&
                           value.operation == lang::TokenKind::MINUS &&
                           optimized.replacement(value.id) == nullptr;
                  });
  expect(retainsCheckedNegation,
         "constant folding must not erase a signed negation overflow trap");
  const auto retainsCheckedBinary =
      [&](const lang::HirFunctionInstance *instance,
          lang::TokenKind operation) {
        return instance != nullptr &&
               std::any_of(instance->body.values.begin(),
                           instance->body.values.end(),
                           [&](const lang::HirValue &value) {
                             return value.kind == lang::HirValueKind::Binary &&
                                    value.operation == operation &&
                                    optimized.replacement(value.id) == nullptr;
                           });
      };
  expect(retainsCheckedBinary(additionInstance, lang::TokenKind::PLUS) &&
             retainsCheckedBinary(unsignedSubtractionInstance,
                                  lang::TokenKind::MINUS),
         "constant folding must preserve signed overflow and unsigned "
         "underflow operations");

  bool foldsInRangeArithmetic = false;
  if (foldedArithmeticInstance != nullptr) {
    for (const lang::HirValue &value : foldedArithmeticInstance->body.values) {
      if (value.kind != lang::HirValueKind::Binary ||
          value.operation != lang::TokenKind::SLASH) {
        continue;
      }
      const lang::ConstantValue *replacement = optimized.replacement(value.id);
      const auto *integer =
          replacement == nullptr
              ? nullptr
              : std::get_if<lang::IntegerConstant>(replacement);
      foldsInRangeArithmetic =
          integer != nullptr && !integer->negative && integer->magnitude == 42;
    }
  }
  expect(foldsInRangeArithmetic,
         "bounded constant evaluation should fold proven in-range arithmetic");

  const lang::MirBody *mainBody = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "main") {
      continue;
    }
    const lang::MirFunctionInstance *lowered =
        valid.mir.findFunctionInstance(instance.id);
    mainBody = lowered == nullptr ? nullptr : &lowered->body;
    break;
  }
  const auto hasOperation = [&](lang::MirOperation operation) {
    return mainBody != nullptr &&
           std::any_of(mainBody->blocks.begin(), mainBody->blocks.end(),
                       [&](const lang::MirBlock &block) {
                         return std::any_of(
                             block.instructions.begin(),
                             block.instructions.end(),
                             [&](const lang::MirInstruction &instruction) {
                               return instruction.operation == operation;
                             });
                       });
  };
  expect(hasOperation(lang::MirOperation::MultiplyAssign) &&
             hasOperation(lang::MirOperation::DivideAssign) &&
             hasOperation(lang::MirOperation::RemainderAssign) &&
             hasOperation(lang::MirOperation::BitwiseAndAssign) &&
             hasOperation(lang::MirOperation::BitwiseOrAssign) &&
             hasOperation(lang::MirOperation::BitwiseXorAssign) &&
             hasOperation(lang::MirOperation::ShiftLeftAssign) &&
             hasOperation(lang::MirOperation::ShiftRightAssign),
         "MIR should preserve each compound mutation as a closed operation");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-checked-arithmetic.gti", R"(
int main() {
  mut int integer = 1;
  mut float decimal = 1.0;
  mut bool condition = true;
  mut int32_t signed_value = 1;
  mut uint32_t unsigned_value = 1;
  integer += 1.5;
  integer /= 0;
  integer %= 0;
  integer <<= -1;
  integer >>= 32;
  decimal %= 2;
  condition &= true;
  signed_value *= unsigned_value;
  int zero = 7 / 0;
  return 0;
}
)");
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot use a floating-point right operand") &&
          hasDiagnostic(invalid.diagnostics,
                        "Integer division by zero is not allowed") &&
          hasDiagnostic(invalid.diagnostics, "Modulo divisor cannot be zero") &&
          hasDiagnostic(invalid.diagnostics,
                        "Shift count cannot be negative") &&
          hasDiagnostic(invalid.diagnostics,
                        "Shift count must be less than 32") &&
          hasDiagnostic(invalid.diagnostics, "requires integer operands") &&
          hasDiagnostic(invalid.diagnostics, "no safe common type") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "Convert the right operand explicitly"),
      "invalid compound domains and statically known arithmetic failures "
      "should be rejected by semantics");

  const std::string formatted = lang::Formatter().format(
      "void update(mut int value){value*=2;value/=2;value%=3;value&=1;"
      "value|=2;value^=3;value<<=1;value>>=1;}");
  expect(formatted.find("value *= 2;") != std::string::npos &&
             formatted.find("value /= 2;") != std::string::npos &&
             formatted.find("value %= 3;") != std::string::npos &&
             formatted.find("value &= 1;") != std::string::npos &&
             formatted.find("value |= 2;") != std::string::npos &&
             formatted.find("value ^= 3;") != std::string::npos &&
             formatted.find("value <<= 1;") != std::string::npos &&
             formatted.find("value >>= 1;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should space every compound operator idempotently");
}

void testParserRecovery() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int first = ;
int second = ;
class Broken {
  return 1;
  int value = 2;
};
int main() { return 0; }
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();

  expect(parser.errors().size() == 3,
         "parser should report independent declaration errors");
  expect(program.declarations().size() == 2,
         "parser should recover and keep later declarations");
}

void testSemanticDiagnostics() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int main() {
  if (1) { missing = 3; }
  return 0;
}
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "semantic test source should parse");

  lang::SemanticVisitor semantic;
  expect(!semantic.check(program), "invalid semantics should be rejected");
  expect(semantic.errors().size() == 2,
         "condition type and undefined variable should both be reported");
  expect(!semantic.errors().empty() &&
             semantic.errors().front().primary.line == 3,
         "semantic diagnostics should preserve literal source lines");
}

void testDiagnosticFoundation() {
  lang::SourceManager sources;
  const std::string unicodePrefix = "\xF0\x9F\x99\x82value";
  sources.set("unicode.gti", unicodePrefix);
  const lang::SourceLocation unicodeLocation =
      sources.locate(lang::SourceSpan{"unicode.gti", 4, 9, 1});
  expect(unicodeLocation.line == 1 && unicodeLocation.column == 2,
         "source locations should count a UTF-8 scalar as one CLI column");

  lang::Lexer lexer;
  const std::string invalidEscape =
      "std::string_view value = \"first\nbad\\q\";";
  lexer.scan(invalidEscape, "escape.gti");
  expect(
      lexer.errors().size() == 1 &&
          lexer.errors().front().code == "GTI-L0005" &&
          lexer.errors().front().primary.start == invalidEscape.find("\\q") &&
          lexer.errors().front().primary.end == invalidEscape.find("\\q") + 2 &&
          lexer.errors().front().primary.line == 2,
      "lexical diagnostics should identify the exact invalid escape span");

  const std::string missingSemicolon = "int first = 1\nint second = 2;\n";
  lang::Parser parser(lexer.scan(missingSemicolon, "parse.gti"));
  parser.parse();
  expect(parser.errors().size() == 1 &&
             parser.errors().front().code == "GTI-P0001" &&
             parser.errors().front().fixes.size() == 1 &&
             parser.errors().front().fixes.front().replacement == ";" &&
             parser.errors().front().fixes.front().span.start ==
                 missingSemicolon.find("int second"),
         "missing punctuation should carry an insertion fix-it");

  auto semanticTokens = lexer.scan(R"(
int duplicate = 1;
int duplicate = 2;
int main() {
  int fixed = 1;
  fixed = 2;
  int value = "text";
  return 0;
}
)",
                                   "semantic.gti");
  lang::Parser semanticParser(std::move(semanticTokens));
  lang::Program program = semanticParser.parse();
  lang::SemanticVisitor semantic;
  expect(!semantic.check(program),
         "rich semantic diagnostic source should fail");

  const lang::Diagnostic *duplicate = nullptr;
  const lang::Diagnostic *immutable = nullptr;
  const lang::Diagnostic *mismatch = nullptr;
  for (const lang::Diagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == "GTI-S2006") {
      duplicate = &diagnostic;
    } else if (diagnostic.code == "GTI-S2002") {
      immutable = &diagnostic;
    } else if (diagnostic.code == "GTI-S2003") {
      mismatch = &diagnostic;
    }
  }
  expect(duplicate != nullptr && duplicate->related.size() == 1,
         "duplicate declarations should reference the original declaration");
  expect(immutable != nullptr && immutable->related.size() == 1 &&
             !immutable->hints.empty(),
         "immutability diagnostics should explain the declaration and remedy");
  expect(mismatch != nullptr &&
             mismatch->message.find("int32_t") != std::string::npos &&
             mismatch->message.find("std::string_view") != std::string::npos,
         "type mismatches should name expected and actual GTI types");
}

void testExecutablePathDiscovery() {
  const std::filesystem::path executable =
      lang::executablePath("not-the-running-test-binary");
  std::error_code error;
  expect(executable.is_absolute() &&
             std::filesystem::is_regular_file(executable, error),
         "native executable discovery should not depend on argv[0]");

  const lang::StandardLibraryLayout rootLayout =
      lang::standardLibraryLayout("/toolchain/share/gti/stdlib");
  const lang::StandardLibraryLayout legacyLayout =
      lang::standardLibraryLayout("/custom/prelude.gti");
  expect(rootLayout.root == "/toolchain/share/gti/stdlib" &&
             rootLayout.prelude == "/toolchain/share/gti/stdlib/prelude.gti" &&
             legacyLayout.root == "/custom" &&
             legacyLayout.prelude == "/custom/prelude.gti",
         "standard-library discovery should accept roots and legacy prelude "
         "paths");
}

void testDefaultImmutability() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
int identity(int value) { return value; }
int main() {
  int fixed = 1;
  mut int moving = 1;
  moving++;
  return identity(fixed);
}
)");
  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "mutability syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "mutable bindings should permit mutation");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find(
             "std::int32_t identity(const std::int32_t value)") !=
             std::string::npos,
         "parameters should be const by default");
  expect(generated.find("const std::int32_t fixed = 1") !=
             std::string::npos,
         "immutable variables should lower to const");
  expect(generated.find("std::int32_t moving = 1") != std::string::npos,
         "mut variables should lower without const");

  auto invalidTokens = lexer.scan(R"(
class Box {
  int value = 1;
  int change() { this.value = 2; return this.value; }
};
int changeParameter(int value) { value = 2; return value; }
int main() {
  int fixed = 1;
  fixed = 2;
  int missingInitializer;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "immutability error source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "immutable bindings should reject mutation");
  expect(invalidSemantic.errors().size() == 4,
         "members, parameters, locals, and missing initializers should fail");
}

void testStaticStorageAndMembers() {
  lang::Lexer lexer;
  const std::vector<lang::Token> keyword = lexer.scan("static int value = 1;");
  expect(keyword.size() >= 2 && keyword.front().kind == lang::TokenKind::STATIC,
         "static should be a dedicated declaration keyword");

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
};

class Empty {
public:
  static Empty value = Empty();
};

class Managed {
public:
  static mut int live = 0;
  ~Managed() {}
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
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "namespace and class static declarations should compile");
  const std::size_t assignmentOwner = source.find("Counter::count =");
  const std::vector<lang::SemanticOccurrence> &entryOccurrences =
      frontend.semantics.database().occurrences(
          frontend.sourceGraph.entryUnit());
  const auto ownerOccurrence = std::find_if(
      entryOccurrences.begin(), entryOccurrences.end(),
      [assignmentOwner](const lang::SemanticOccurrence &occurrence) {
        return occurrence.span.start == assignmentOwner &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::TypeUse);
      });
  expect(ownerOccurrence != entryOccurrences.end(),
         "qualified static assignments should retain their class occurrence");

  const lang::ClassTypeInfo *counter = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Counter") {
      counter = frontend.semantics.findClassType(instance.declaration);
      expect(instance.fields.size() == 1 && instance.staticFields.size() == 2,
             "HIR should keep static fields separate from object fields");
      break;
    }
  }
  expect(counter != nullptr && counter->fields.size() == 1 &&
             counter->staticFields.size() == 2,
         "semantic class layout should exclude static data members");

  bool foundStaticMethod = false;
  bool foundStaticFunction = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source == nullptr) {
      continue;
    }
    if (function.source->name().lexeme == "current") {
      foundStaticMethod = function.staticMember && !function.internalLinkage;
    } else if (function.source->name().lexeme == "add_file_value") {
      foundStaticFunction = !function.staticMember && function.internalLinkage;
    }
  }
  expect(foundStaticMethod && foundStaticFunction,
         "HIR functions should distinguish static methods from internal "
         "namespace functions");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &frontend.semantics, &frontend.hir)
          .emit(frontend.program);
  expect(
      generated.find("static const std::int32_t answer;") !=
              std::string::npos &&
          generated.find("inline const std::int32_t Counter::answer = 42") !=
              std::string::npos &&
          generated.find("static std::int32_t count;") != std::string::npos &&
          generated.find("inline std::int32_t Counter::count = 0") !=
              std::string::npos &&
          generated.find("inline const Empty Empty::value = Empty()") !=
              std::string::npos &&
          generated.find("inline std::int32_t Managed::live = 0") !=
              std::string::npos &&
          generated.find("live(std::move(other.live))") == std::string::npos &&
          generated.find("static std::int32_t __gti_fn_") !=
              std::string::npos &&
          generated.find("detail::__gti_static_") != std::string::npos &&
          generated.find("__gti_static_") != std::string::npos,
      "the C++ backend should emit inline class statics and uniquely named "
      "qualified internal-linkage declarations");

  const lang::FrontendResult localStatic = lang::Frontend().analyze(
      "local-static.gti", "int main() { static int value = 1; return value; }");
  expect(!localStatic.syntaxValid &&
             hasDiagnostic(localStatic.diagnostics,
                           "Block-scope static declarations are not supported"),
         "block-scope static should fail with a focused parser diagnostic");

  const lang::FrontendResult invalidMembers =
      lang::Frontend().analyze("invalid-static-members.gti", R"(
class Bad {
public:
  static int missing;
  int value = 1;
  static int read() { return this.value; }
};
)");
  expect(!invalidMembers.semanticValid &&
             hasDiagnostic(invalidMembers.diagnostics,
                           "require an in-class initializer") &&
             hasDiagnostic(invalidMembers.diagnostics,
                           "Static methods do not have a 'this' object"),
         "static fields should require definitions and static methods should "
         "have no receiver");

  const lang::FrontendResult invalidAccess =
      lang::Frontend().analyze("invalid-static-access.gti", R"(
class Registry {
public:
  static int value = 1;
};

int main() {
  Registry registry = Registry();
  return registry.value;
}
)");
  expect(!invalidAccess.semanticValid &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "must be accessed through its class or struct name"),
         "static members should reject object-qualified access");

  const lang::FrontendResult genericStatic =
      lang::Frontend().analyze("generic-static.gti", R"(
class Registry<T> {
public:
  static int value = 1;
};
)");
  expect(!genericStatic.semanticValid &&
             hasDiagnostic(genericStatic.diagnostics,
                           "qualified generic member paths"),
         "generic class statics should remain rejected until qualified "
         "generic paths have a complete representation");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-static-storage";
  const std::filesystem::path entry = root / "main.gti";
  const std::filesystem::path left = root / "left.gti";
  const std::filesystem::path right = root / "right.gti";
  const auto canonical = [](const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(path).string();
  };
  const lang::FrontendResult separateUnits = lang::Frontend().analyze(
      entry,
      "#include \"left.gti\"\n#include \"right.gti\"\n"
      "int main() { return left_value() + right_value() - 3; }\n",
      {},
      {{canonical(left),
        "using SharedName = int; static int value = 1; "
        "static int helper() { return value; } "
        "int left_value() { SharedName local = helper(); return local; }\n"},
       {canonical(right), "static int SharedName = 2; static int value = 2; "
                          "static int helper() { return value; } "
                          "int right_value() { return helper(); }\n"}});
  expect(separateUnits.canGenerateCode() && separateUnits.diagnostics.empty(),
         "different source units should isolate static names from sibling "
         "symbols and aliases");

  const lang::FrontendResult hiddenStatic = lang::Frontend().analyze(
      entry, "#include \"left.gti\"\nint main() { return value; }\n", {},
      {{canonical(left), "static int value = 1;\n"}});
  expect(!hiddenStatic.semanticValid &&
             hasDiagnostic(hiddenStatic.diagnostics, "Undefined name 'value'"),
         "namespace static declarations should not leak into includers");
}

void testThisReceiverKeyword() {
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan("this self");
  expect(tokens.size() >= 3 && tokens[0].kind == lang::TokenKind::THIS &&
             tokens[1].kind == lang::TokenKind::IDENTIFIER,
         "this should be the receiver keyword and self should remain an "
         "ordinary identifier");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("this-receiver.gti", R"(
class Counter {
  int value = 4;
public:
  int read() {
    int self = 1;
    return this.value + self - 1;
  }
};

int main() {
  Counter counter = Counter();
  return counter.read() - 4;
}
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "this member access and self identifiers should compile together");

  bool foundThisValue = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.kind == lang::HirValueKind::This) {
        foundThisValue = true;
      }
    }
  }
  expect(foundThisValue,
         "typed HIR should preserve the current-object expression");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &frontend.semantics, &frontend.hir)
          .emit(frontend.program);
  expect(generated.find("((*this)).value") != std::string::npos,
         "the C++ backend should lower GTI this as a checked object receiver");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-this.gti", "int main() { return this.value; }");
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot use 'this' outside a class or struct method"),
      "this should be rejected outside an instance method");
}

void testClassesStructsAndAccess() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
class Vault {
  int secret = 7;

public:
  int reveal() { return this.secret; }
  int reveal_other(Vault other) { return other.secret; }
};

struct Reading {
  int value = 1;

private:
  int hidden = 2;

public:
  int total() { return this.value + this.hidden; }
};

int open(mut Vault vault) { return vault.reveal(); }
int read(Reading reading) { return reading.value; }
)");
  expect(!lexer.hadError(), "class and struct access syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "class and struct access syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "public members and same-class private access should resolve");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("class Vault;") != std::string::npos &&
             generated.find("struct Reading;") != std::string::npos &&
             generated.find("class Vault {") != std::string::npos &&
             generated.find("struct Reading {") != std::string::npos &&
             generated.find("public:\n  std::int32_t reveal()") !=
                 std::string::npos &&
             generated.find("private:\n  std::int32_t hidden = 2") !=
                 std::string::npos,
         "emitter should preserve declaration kinds, access labels, and "
         "frontend-owned field immutability");

  auto invalidTokens = lexer.scan(R"(
class A {
  int private_value = 1;
public:
  int visible = 2;
  int inspect(A other) { return other.private_value; }
};

struct B {
  int public_value = 1;
private:
  int hidden = 2;
};

A wrong_type(B value) { return value; }
int read_class_private(A value) { return value.private_value; }
int read_struct_private(B value) { return value.hidden; }
int read_missing(B value) { return value.missing; }

class Duplicate {
  int value = 1;
public:
  int value = 2;
};

class InvalidFields {
  mut int missing_initializer;
  int invalid_reference = missing_initializer;
  int invalid_receiver = this.private_value;
};

int invalid_global_receiver = this.private_value;
MissingType unresolved();
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid class semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(
      !invalidSemantic.check(invalidProgram),
      "nominal, access, member, field, and receiver errors should be rejected");
  expect(hasDiagnostic(invalidSemantic, "Cannot return a value of type"),
         "different nominal class types should not be assignable");
  expect(hasDiagnostic(invalidSemantic, "of 'A' is private") &&
             hasDiagnostic(invalidSemantic, "of 'B' is private"),
         "class defaults and explicit private labels should be enforced");
  expect(hasDiagnostic(invalidSemantic, "Unknown member 'missing'"),
         "unknown members should be diagnosed on their nominal type");
  expect(hasDiagnostic(invalidSemantic, "Duplicate member declaration"),
         "duplicate fields and methods should be rejected");
  expect(hasDiagnostic(invalidSemantic, "fields must have an initializer"),
         "mutable fields should require initialization until constructors exist");
  expect(hasDiagnostic(invalidSemantic, "referenced from field initializers"),
         "field initializers should not depend on member initialization order");
  expect(hasDiagnostic(invalidSemantic, "outside a class or struct method"),
         "this should be rejected in fields and outside methods");
  expect(hasDiagnostic(invalidSemantic, "Unknown type 'MissingType'"),
         "unknown nominal types should be diagnosed");

  auto conditionalTokens = lexer.scan(R"(
class PlatformValue {
#if target.os == "public-os"
public:
#else
private:
#endif
  int value = 1;
};
int read_platform(PlatformValue value) { return value.value; }
)");
  lang::Parser conditionalParser(std::move(conditionalTokens));
  lang::Program conditionalProgram = conditionalParser.parse();
  expect(!conditionalParser.hadError(),
         "conditional access labels should parse in class bodies");

  lang::SemanticVisitor publicTarget(
      lang::TargetInfo{.os = "public-os", .vendor = "test", .arch = "test"});
  lang::SemanticVisitor privateTarget(
      lang::TargetInfo{.os = "private-os", .vendor = "test", .arch = "test"});
  expect(publicTarget.check(conditionalProgram),
         "the active public access branch should expose following members");
  expect(!privateTarget.check(conditionalProgram) &&
             hasDiagnostic(privateTarget, "is private"),
         "the active private access branch should hide following members");

  auto recoveryTokens = lexer.scan(R"(
struct Recovered {
  public int value = 1;
private:
  int hidden = 2;
};
int main() { return 0; }
)");
  lang::Parser recoveryParser(std::move(recoveryTokens));
  lang::Program recoveryProgram = recoveryParser.parse();
  expect(recoveryParser.errors().size() == 1 &&
             recoveryProgram.declarations().size() == 2,
         "parser recovery should resume at access labels and later declarations");

  const std::string formatted = lang::Formatter().format(
      "class Box{public:int value=1;private:int hidden=2;};"
      "struct Point{int x=0;};");
  expect(formatted == "class Box {\npublic:\n  int value = 1;\nprivate:\n"
                      "  int hidden = 2;\n};\nstruct Point {\n"
                      "  int x = 0;\n};\n",
         "formatter should outdent C++-style access labels");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted class access labels should be idempotent");
}

void testConstructorsAndReceiverMutability() {
  lang::Lexer lexer;
  const std::string validSource = R"(
class Counter {
  mut int value;
  int step = 1;

public:
  Counter() : value(0) {}
  Counter(int initial) : value(initial) {}
  Counter(bool reset) : value(0) {}
  int read() { return this.value; }
  int advance(int amount) mut {
    this.value += amount;
    return this.value;
  }
};

struct Origin {
  int x = 0;
  Origin(int initial) : x(initial) {}
};

class LifecycleValue {
  int value = 0;

public:
  LifecycleValue(LifecycleValue& other) = default;
  LifecycleValue(LifecycleValue&& other) = default;
};

class MoveOnlyPolicy {
  int value = 0;

public:
  MoveOnlyPolicy(MoveOnlyPolicy& other) = delete;
  MoveOnlyPolicy(MoveOnlyPolicy&& other) = default;
};

int inspect(Counter counter) { return counter.read(); }
int main() {
  Counter zero = Counter();
  Counter fixed = Counter(1);
  mut int observed = fixed.read();
  mut Counter moving = Counter(observed);
  observed = moving.advance(2);
  Origin origin = Origin();
  Origin shifted = Origin(4);
  Counter reset = Counter(true);
  LifecycleValue source{};
  LifecycleValue copied = LifecycleValue(source);
  mut LifecycleValue movable{};
  LifecycleValue moved = LifecycleValue(std::move(movable));
  mut MoveOnlyPolicy owner{};
  MoveOnlyPolicy transferred = MoveOnlyPolicy(std::move(owner));
  return observed + origin.x;
}
)";
  auto validTokens = lexer.scan(validSource);
  expect(!lexer.hadError(), "constructor and receiver syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "constructor and receiver syntax should parse");

  const lang::FrontendResult validFrontend =
      lang::Frontend().analyze("constructors-and-receivers.gti", validSource,
                               {standardLibraryPrelude()});
  expect(validFrontend.semanticValid,
         "explicit construction and mutable receiver calls should validate");

  const lang::ClassDecl *counter =
      findTopLevelClass(validFrontend.program, "Counter");
  const lang::ClassDecl *origin =
      findTopLevelClass(validFrontend.program, "Origin");
  const lang::ClassDecl *lifecycleValue =
      findTopLevelClass(validFrontend.program, "LifecycleValue");
  const lang::ClassDecl *moveOnlyPolicy =
      findTopLevelClass(validFrontend.program, "MoveOnlyPolicy");
  const lang::FunctionDecl *main =
      findTopLevelFunction(validFrontend.program, "main");
  const auto *zero = main == nullptr
                         ? nullptr
                         : dynamic_cast<const lang::VariableDecl *>(
                               main->body()->statements().at(0).get());
  const auto *fixed = main == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                main->body()->statements().at(1).get());
  const auto *defaultOrigin = main == nullptr
                                  ? nullptr
                                  : dynamic_cast<const lang::VariableDecl *>(
                                        main->body()->statements().at(5).get());
  const auto *zeroCall =
      zero == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(zero->initializer().get());
  const auto *fixedCall =
      fixed == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(fixed->initializer().get());
  const auto *originCall = defaultOrigin == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::Call *>(
                                     defaultOrigin->initializer().get());
  const auto constructorCallAt = [&](std::size_t index) {
    const auto *declaration =
        main == nullptr ? nullptr
                        : dynamic_cast<const lang::VariableDecl *>(
                              main->body()->statements().at(index).get());
    return declaration == nullptr ? static_cast<const lang::Call *>(nullptr)
                                  : dynamic_cast<const lang::Call *>(
                                        declaration->initializer().get());
  };
  const lang::Call *copyCall = constructorCallAt(9);
  const lang::Call *moveCall = constructorCallAt(11);
  const lang::Call *moveOnlyCall = constructorCallAt(13);
  const lang::ClassLifecycleInfo *counterLifecycle =
      counter == nullptr ? nullptr
                         : validFrontend.semantics.findClassLifecycle(*counter);
  const lang::ClassLifecycleInfo *originLifecycle =
      origin == nullptr ? nullptr
                        : validFrontend.semantics.findClassLifecycle(*origin);
  const lang::ClassLifecycleInfo *valueLifecycle =
      lifecycleValue == nullptr
          ? nullptr
          : validFrontend.semantics.findClassLifecycle(*lifecycleValue);
  const lang::ClassLifecycleInfo *moveOnlyLifecycle =
      moveOnlyPolicy == nullptr
          ? nullptr
          : validFrontend.semantics.findClassLifecycle(*moveOnlyPolicy);
  const lang::ResolvedConstructionInfo *zeroConstruction =
      zeroCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*zeroCall);
  const lang::ResolvedConstructionInfo *fixedConstruction =
      fixedCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*fixedCall);
  const lang::ResolvedConstructionInfo *originConstruction =
      originCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*originCall);
  const lang::ResolvedConstructionInfo *copyConstruction =
      copyCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*copyCall);
  const lang::ResolvedConstructionInfo *moveConstruction =
      moveCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*moveCall);
  const lang::ResolvedConstructionInfo *moveOnlyConstruction =
      moveOnlyCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*moveOnlyCall);
  expect(counterLifecycle != nullptr && originLifecycle != nullptr &&
             counterLifecycle->constructors.size() == 3 &&
             counterLifecycle->defaultConstructor ==
                 lang::SpecialMemberStatus::Declared &&
             counterLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             counterLifecycle->moveAssignment ==
                 lang::SpecialMemberStatus::Generated &&
             originLifecycle->defaultConstructor ==
                 lang::SpecialMemberStatus::Generated,
         "class lifecycle metadata should distinguish declared, generated, "
         "and available special members");
  expect(valueLifecycle != nullptr && moveOnlyLifecycle != nullptr &&
             valueLifecycle->declaredCopyConstructor &&
             valueLifecycle->declaredMoveConstructor &&
             valueLifecycle->declaredCopyConstructor->kind ==
                 lang::ConstructorKind::Copy &&
             valueLifecycle->declaredMoveConstructor->kind ==
                 lang::ConstructorKind::Move &&
             valueLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             valueLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             moveOnlyLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Deleted &&
             moveOnlyLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated,
         "declared copy and move policies should override lifecycle "
         "availability without entering ordinary overload sets");
  expect(zeroConstruction != nullptr && fixedConstruction != nullptr &&
             originConstruction != nullptr &&
             zeroConstruction->declaration != fixedConstruction->declaration &&
             !zeroConstruction->generatedDefault &&
             originConstruction->generatedDefault,
         "construction metadata should retain exact overload selection and "
         "generated default construction");
  expect(copyConstruction != nullptr && moveConstruction != nullptr &&
             moveOnlyConstruction != nullptr &&
             copyConstruction->kind == lang::ConstructorKind::Copy &&
             moveConstruction->kind == lang::ConstructorKind::Move &&
             moveOnlyConstruction->kind == lang::ConstructorKind::Move &&
             copyConstruction->declaration != nullptr &&
             moveConstruction->declaration != nullptr,
         "same-type construction should resolve explicitly to copy or move "
         "lifecycle operations");

  const lang::FrontendResult lifecycleFrontend =
      lang::Frontend().analyze("copy-move-policy.gti", R"(
class Value {
  int value = 0;
public:
  Value(Value& other) = default;
  Value(Value&& other) = default;
};
class GenericValue<T> {
  T value;
public:
  GenericValue(T initial) : value(initial) {}
  GenericValue(GenericValue<T>& other) = default;
  GenericValue(GenericValue<T>&& other) = default;
};
int main() {
  Value source{};
  Value copied = Value(source);
  mut Value movable{};
  Value moved = Value(std::move(movable));
  GenericValue<int> generic_source = GenericValue<int>(1);
  GenericValue<int> generic_copy = GenericValue<int>(generic_source);
  mut GenericValue<int> generic_movable = GenericValue<int>(2);
  GenericValue<int> generic_move =
      GenericValue<int>(std::move(generic_movable));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(lifecycleFrontend.canGenerateCode(),
         "copy and move policy calls should pass the complete frontend");
  std::size_t hirCopies = 0;
  std::size_t hirMoves = 0;
  for (const lang::HirFunctionInstance &function :
       lifecycleFrontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      hirCopies += value.constructorKind == lang::ConstructorKind::Copy;
      hirMoves += value.constructorKind == lang::ConstructorKind::Move;
    }
  }
  expect(hirCopies == 2 && hirMoves == 2,
         "typed HIR should preserve special construction kinds without "
         "creating source constructor bodies");

  expect(lifecycleFrontend.mir.valid(),
         "copy and move construction should lower to valid MIR");
  std::size_t mirCopies = 0;
  std::size_t mirMoves = 0;
  for (const lang::MirFunctionInstance &function :
       lifecycleFrontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct) {
          continue;
        }
        mirCopies += instruction.constructorKind == lang::ConstructorKind::Copy;
        mirMoves += instruction.constructorKind == lang::ConstructorKind::Move;
      }
    }
  }
  expect(mirCopies == 2 && mirMoves == 2,
         "MIR construct instructions should retain copy and move identity");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  const std::string lifecycleGenerated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &validFrontend.semantics)
          .emit(validFrontend.program);
  expect(generated.find(
             "explicit Counter(const std::int32_t initial) : value(initial)") !=
             std::string::npos,
         "constructor overloads should lower explicitly with field "
         "initialization");
  expect(generated.find("std::int32_t read() const") != std::string::npos,
         "methods should lower as read-only by default");
  expect(
      generated.find("std::int32_t advance(const std::int32_t amount) const") ==
              std::string::npos &&
          generated.find("std::int32_t advance(const std::int32_t amount)") !=
              std::string::npos,
      "mutable receiver methods should lower without C++ const");
  expect(generated.find("Origin origin = Origin()") != std::string::npos,
         "generated default construction should remain explicit at the call "
         "site");
  expect(lifecycleGenerated.find("Origin() = default;") != std::string::npos &&
             lifecycleGenerated.find("Counter(const Counter &) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find("Counter(Counter &&) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find(
                 "Counter &operator=(const Counter &) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find(
                 "Counter &operator=(Counter &&) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find("~Counter() noexcept = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find(
                 "LifecycleValue(const LifecycleValue &) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find(
                 "LifecycleValue(LifecycleValue &&) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find(
                 "MoveOnlyPolicy(const MoveOnlyPolicy &) = delete;") !=
                 std::string::npos,
         "the backend should explicitly emit compiler-generated special "
         "members");

  const std::string invalidSource = R"(
class MissingInitialization {
  int value;
};

class InvalidConstructor {
  int first;
  int second;

public:
  InvalidConstructor(int value)
      : second(value), first(this.second), second(value) { return; }
  InvalidConstructor(mut int value) : first(value), second(value) {}
};

class ReservedCopy {
  int value = 0;

public:
  ReservedCopy(ReservedCopy& other) : value(other.value) {}
};

class CleanupCopy {
  int value = 0;

public:
  CleanupCopy(CleanupCopy& other) = default;
  ~CleanupCopy() {}
};

class DeletedCopy {
  int value = 0;

public:
  DeletedCopy(DeletedCopy& other) = delete;
  DeletedCopy(DeletedCopy&& other) = default;
};

class DeletedMove {
  int value = 0;

public:
  DeletedMove(DeletedMove& other) = default;
  DeletedMove(DeletedMove&& other) = delete;
};

class InvalidDefaultTarget {
  int value = 0;

public:
  InvalidDefaultTarget(int value) = default;
};

class PrivatePolicy {
  PrivatePolicy(PrivatePolicy& other) = delete;
};

void invalid_rvalue_reference(DeletedCopy&& value) {}
void consume_deleted(DeletedCopy value) {}

class PrivateValue {
  int value;
  PrivateValue(int initial) : value(initial) {}
};

class MutableValue {
  mut int value = 0;

public:
  void mutate() { this.value = 1; }
  void mutate_other(MutableValue other) mut { other.value = 1; }
  void bump() mut { this.value += 1; }
};

class ImmutableField {
  int value = 0;

public:
  void replace() mut { this.value = 1; }
};

int main() {
  PrivateValue hidden = PrivateValue(1);
  MutableValue fixed = MutableValue();
  fixed.bump();
  mut MutableValue moving = MutableValue();
  moving.bump();
  mut MutableValue uninitialized;
  InvalidConstructor mismatch = InvalidConstructor(true);
  MutableValue implicit_value = 1;
  DeletedCopy original{};
  DeletedCopy copied = DeletedCopy(original);
  DeletedCopy copied_directly = original;
  consume_deleted(original);
  mut DeletedMove stationary{};
  DeletedMove moved = DeletedMove(std::move(stationary));
  return 0;
}
)";
  auto invalidTokens = lexer.scan(invalidSource);
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid constructor semantics should remain valid syntax");

  const lang::FrontendResult invalidFrontend =
      lang::Frontend().analyze("invalid-constructors-and-receivers.gti",
                               invalidSource, {standardLibraryPrelude()});
  const std::vector<lang::Diagnostic> &invalidSemantic =
      invalidFrontend.diagnostics;
  expect(!invalidFrontend.semanticValid,
         "invalid construction and receiver use should be rejected");
  expect(hasDiagnostic(invalidSemantic, "fields must have an initializer"),
         "a class without a constructor should still initialize every field");
  expect(hasDiagnostic(invalidSemantic,
                       "Duplicate constructor overload signature"),
         "constructor overload signatures should be unique by exact parameter "
         "type");
  expect(hasDiagnostic(invalidSemantic, "field declaration order") &&
             hasDiagnostic(invalidSemantic, "initialized more than once"),
         "constructor initializer order and uniqueness should be enforced");
  expect(hasDiagnostic(invalidSemantic,
                       "Cannot use 'this' in a constructor initializer"),
         "constructor initializers should not observe a partial object");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructors cannot contain return statements"),
         "constructor bodies should reject return statements");
  expect(hasDiagnostic(invalidSemantic,
                       "Custom copy and move constructor bodies require"),
         "custom lifecycle bodies should remain gated on place-aware moves");
  expect(hasDiagnostic(invalidSemantic,
                       "defaulted copy constructor of 'CleanupCopy' is "
                       "unavailable"),
         "a defaulted copy policy should reject structurally noncopyable "
         "state");
  expect(hasDiagnostic(invalidSemantic,
                       "Copy construction of 'DeletedCopy' is deleted"),
         "an explicitly deleted copy constructor should reject construction");
  expect(hasDiagnostic(invalidSemantic, "Cannot initialize 'copied_directly'"),
         "deleted copy policy should govern direct initialization");
  expect(hasDiagnostic(invalidSemantic, "deleted copy constructor"),
         "deleted copy policy should govern by-value calls");
  expect(hasDiagnostic(invalidSemantic, "type 'DeletedMove' is not movable"),
         "an explicitly deleted move constructor should reject std::move");
  expect(hasDiagnostic(invalidSemantic,
                       "currently available only for exact copy and move"),
         "ordinary constructors should reject special-member specifiers");
  expect(
      hasDiagnostic(invalidSemantic, "constructor policies must be public") &&
          hasDiagnostic(invalidSemantic,
                        "'&&' is currently confined to a class or struct's "
                        "exact move constructor policy"),
      "the confined policy layer should reject private policies and "
      "general rvalue references");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructor of 'PrivateValue' is private"),
         "constructor access should follow class access labels");
  expect(hasDiagnostic(invalidSemantic,
                       "Mutable method requires a mutable receiver") &&
             hasDiagnostic(invalidSemantic,
                           "Cannot mutate through a read-only receiver"),
         "mutable methods and field writes should require mutable receivers");
  expect(hasDiagnostic(invalidSemantic, "Member is immutable"),
         "frontend field immutability should remain enforced independently of "
         "the C++ representation");
  expect(
      hasDiagnostic(invalidSemantic,
                    "No constructor of 'InvalidConstructor'") &&
          hasDiagnostic(invalidSemantic, "Cannot initialize 'implicit_value'"),
      "constructor calls should reject mismatched and implicit conversions");
  expect(hasDiagnostic(invalidSemantic, "require explicit construction"),
         "class variables should never invoke construction implicitly");

  const std::string formatted = lang::Formatter().format(
      "class Counter{mut int value;public:Counter(int initial):value(initial){}"
      "Counter(Counter& other)=default;Counter(Counter&& other)=delete;"
      "int read(){return this.value;}void reset()mut{this.value=0;}};");
  expect(formatted.find("Counter(int initial) : value(initial) {}") !=
                 std::string::npos &&
             formatted.find("Counter(Counter & other) = default;") !=
                 std::string::npos &&
             formatted.find("Counter(Counter && other) = delete;") !=
                 std::string::npos &&
             formatted.find("void reset() mut {") != std::string::npos,
         "formatter should distinguish constructor policies from logical "
         "operators and switch labels");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted constructors and receiver qualifiers should be idempotent");

  lang::Parser globalQualifierParser(lexer.scan("void invalid() mut {}"));
  globalQualifierParser.parse();
  expect(globalQualifierParser.hadError() &&
             globalQualifierParser.errors().front().message.find(
                 "Only class and struct methods") != std::string::npos,
         "free functions should reject receiver mutability qualifiers");
}

void testInheritanceAndInterfaces() {
  const std::string source = R"(
interface Renderable {
  int render(int frame) = 0;
};

interface Named {
  int name_id() = 0;
};

class Entity {
public:
  Entity(int id) : id(id) {}
  virtual int tick(int frame) { return frame + this.id; }
  virtual int tick(float frame) { return 10; }

private:
  int id;
};

class Sprite : public Entity, public Renderable, public Named {
public:
  Sprite(int id) : Entity(id) {}
  int tick(int frame) override { return frame + 2; }
  int inherited_tick() { return tick(1.5); }
  int render(int frame) override { return this.tick(frame); }
  int name_id() override { return 7; }
};

interface Reader<T> {
  T read() = 0;
};

class Box<T> : public Reader<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() override { return this.value; }
};

int invoke(Renderable& renderable) {
  return renderable.render(3);
}

int main() {
  Sprite sprite{4};
  Renderable& renderable = sprite;
  Box<int> box{3};
  Reader<int>& reader = box;
  int inherited_overload = sprite.tick(1.5);
  return invoke(renderable) + inherited_overload + sprite.inherited_tick() +
         reader.read() - 28;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("inheritance.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected inheritance diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "public state, interface, and generic inheritance should pass the "
         "frontend");

  const auto *renderable = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(0).get());
  const auto *entity = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(2).get());
  const auto *sprite = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(3).get());
  const lang::ClassTypeInfo *renderableType =
      renderable == nullptr ? nullptr
                            : frontend.semantics.findClassType(*renderable);
  const lang::ClassTypeInfo *spriteType =
      sprite == nullptr ? nullptr : frontend.semantics.findClassType(*sprite);
  const lang::ClassLifecycleInfo *entityLifecycle =
      entity == nullptr ? nullptr
                        : frontend.semantics.findClassLifecycle(*entity);
  const lang::ClassLifecycleInfo *spriteLifecycle =
      sprite == nullptr ? nullptr
                        : frontend.semantics.findClassLifecycle(*sprite);
  expect(renderableType != nullptr && renderableType->abstract &&
             renderableType->polymorphic &&
             renderableType->kind == lang::ClassKind::Interface,
         "interfaces should be abstract polymorphic semantic types");
  expect(spriteType != nullptr && spriteType->bases.size() == 3 &&
             !spriteType->abstract && spriteType->polymorphic &&
             entityLifecycle != nullptr && entityLifecycle->polymorphic &&
             spriteLifecycle != nullptr && spriteLifecycle->polymorphic,
         "concrete derived classes should retain base and lifecycle metadata");

  const auto findMethod = [](const lang::ClassDecl *owner,
                             std::string_view name) {
    if (owner == nullptr) {
      return static_cast<const lang::FunctionDecl *>(nullptr);
    }
    for (const lang::StmtPtr &member : owner->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == name) {
        return function;
      }
    }
    return static_cast<const lang::FunctionDecl *>(nullptr);
  };
  const lang::FunctionDecl *contract = findMethod(renderable, "render");
  const lang::FunctionDecl *implementation = findMethod(sprite, "render");
  const lang::FunctionInfo *contractInfo =
      contract == nullptr ? nullptr
                          : frontend.semantics.findFunction(*contract);
  const lang::FunctionInfo *implementationInfo =
      implementation == nullptr
          ? nullptr
          : frontend.semantics.findFunction(*implementation);
  expect(contractInfo != nullptr && contractInfo->virtualMethod &&
             contractInfo->pureVirtual && implementationInfo != nullptr &&
             implementationInfo->virtualMethod &&
             implementationInfo->overrideMethod &&
             implementationInfo->virtualRoots.size() == 1 &&
             implementationInfo->virtualRoots.front() == contractInfo->id,
         "exact overrides should retain their virtual contract identity");

  const lang::FunctionDecl *invoke =
      findTopLevelFunction(frontend.program, "invoke");
  const auto *invokeReturn =
      invoke == nullptr || invoke->body()->statements().empty()
          ? nullptr
          : dynamic_cast<const lang::ReturnStmt *>(
                invoke->body()->statements().front().get());
  const auto *virtualCall =
      invokeReturn == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(invokeReturn->value().get());
  const lang::ResolvedCallInfo *callInfo =
      virtualCall == nullptr ? nullptr
                             : frontend.semantics.findCall(*virtualCall);
  expect(callInfo != nullptr &&
             callInfo->dispatch == lang::CallDispatch::Virtual,
         "calls through interface references should carry virtual dispatch");

  const lang::HirClassInstance *spriteHir = nullptr;
  const lang::HirFunctionInstance *invokeHir = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == sprite) {
      spriteHir = &instance;
    }
  }
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == invoke) {
      invokeHir = &instance;
    }
  }
  const bool hirHasVirtualCall =
      invokeHir != nullptr &&
      std::any_of(invokeHir->body.values.begin(), invokeHir->body.values.end(),
                  [](const lang::HirValue &value) {
                    return value.kind == lang::HirValueKind::Call &&
                           value.dispatch == lang::CallDispatch::Virtual &&
                           value.dispatchOwner.kind ==
                               lang::SemanticType::Class;
                  });
  expect(spriteHir != nullptr && spriteHir->bases.size() == 3 &&
             spriteHir->polymorphic && hirHasVirtualCall,
         "HIR should retain base instances and virtual call dispatch");
  const lang::MirClassInstance *spriteMir =
      spriteHir == nullptr ? nullptr
                           : frontend.mir.findClassInstance(spriteHir->id);
  expect(spriteMir != nullptr && spriteMir->bases.size() == 3 &&
             spriteMir->polymorphic,
         "MIR should preserve inheritance metadata for future backends");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &frontend.semantics)
          .emit(frontend.program);
  expect(
      generated.find(
          "class Sprite : public Entity, public Renderable, public Named") !=
              std::string::npos &&
          generated.find("virtual std::int32_t render(") != std::string::npos &&
          generated.find("render(const std::int32_t frame) const override") !=
              std::string::npos &&
          generated.find(" = 0;") != std::string::npos &&
          generated.find("virtual ~Renderable() noexcept = default;") !=
              std::string::npos &&
          generated.find("explicit Sprite(const std::int32_t id) : "
                         "Entity(id)") != std::string::npos &&
          generated.find("static_cast<const ::Entity &>") != std::string::npos,
      "the C++ backend should emit inheritance, pure contracts, overrides, "
      "base construction, polymorphic destruction, and GTI-selected virtual "
      "overloads");

  const std::string formatted = lang::Formatter().format(
      "interface Renderable{int render(int frame)=0;};"
      "class Sprite:public Entity,public Renderable{public:"
      "Sprite(int id):Entity(id){}int render(int frame)override{return frame;}"
      "};");
  if (formatted.find("interface Renderable {") == std::string::npos ||
      formatted.find("class Sprite : public Entity, public Renderable {") ==
          std::string::npos ||
      formatted.find("int render(int frame) override {") == std::string::npos ||
      lang::Formatter().format(formatted) != formatted) {
    std::cerr << "Inheritance formatted output was:\n" << formatted;
  }
  expect(
      formatted.find("interface Renderable {") != std::string::npos &&
          formatted.find("class Sprite : public Entity, public Renderable {") !=
              std::string::npos &&
          formatted.find("int render(int frame) override {") !=
              std::string::npos &&
          lang::Formatter().format(formatted) == formatted,
      "inheritance and override syntax should format idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-inheritance.gti", R"(
interface Broken {
public:
  Broken() {}
  int state = 0;
  int body() { return 1; }
};

class Left {};
class Right {};
class TooMany : public Left, public Right {};
class HiddenBase : Left {};

interface RootContract { int root() = 0; };
interface LeftContract : public RootContract {};
interface RightContract : public RootContract {};
interface DiamondContract : public LeftContract, public RightContract {};
interface InvalidInterfaceBase : public Left { int invalid() = 0; };

class CycleLeft : public CycleRight {};
class CycleRight : public CycleLeft {};

class Abstract {
public:
  virtual int work(int value) = 0;
};

class Missing : public Abstract {};
class Accidental : public Abstract {
public:
  int work(int value) { return value; }
};
class ReturnMismatch : public Abstract {
public:
  bool work(int value) override { return true; }
};

class RequiresArgument {
  int stored;

public:
  RequiresArgument(int value) : stored(value) {}
};
class MissingBaseInitializer : public RequiresArgument {
public:
  MissingBaseInitializer() {}
};

class Plain {
public:
  int read() { return 1; }
};
class HidesPlain : public Plain {
public:
  int read() override { return 2; }
};

class PrivateState {
  int secret = 1;
};
class LeaksPrivateState : public PrivateState {
public:
  int leak() { return secret; }
};

class Concrete : public Abstract {
public:
  int work(int value) override { return value; }
};
int consume(Abstract& value) { return value.work(1); }
int main() {
  Missing missing{};
  Concrete concrete{};
  Abstract sliced = concrete;
  return consume(concrete);
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid inheritance and interface programs should be rejected");
  expect(hasDiagnostic(invalid.diagnostics, "Interfaces are behavior-only") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interfaces cannot declare constructors") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interface methods are pure contracts"),
         "interfaces should permit only public pure behavior contracts");
  expect(hasDiagnostic(invalid.diagnostics, "only one state-bearing base") &&
             hasDiagnostic(invalid.diagnostics, "Inheritance must be public"),
         "inheritance should be public with at most one state-bearing base");
  expect(hasDiagnostic(invalid.diagnostics,
                       "An interface can inherit only from other interfaces") &&
             hasDiagnostic(invalid.diagnostics,
                           "inheritance diamonds are not supported") &&
             hasDiagnostic(invalid.diagnostics, "Inheritance cycle"),
         "interface bases, inheritance diamonds, and cycles should be "
         "diagnosed before lowering");
  expect(hasDiagnostic(invalid.diagnostics, "Overriding methods must use") &&
             hasDiagnostic(invalid.diagnostics,
                           "Override return type must exactly match") &&
             hasDiagnostic(invalid.diagnostics,
                           "same signature as a non-virtual base method"),
         "override checking should be exact, explicit, and virtual-only");
  expect(
      hasDiagnostic(invalid.diagnostics, "must explicitly initialize base") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot construct abstract class 'Missing'"),
      "derived construction should initialize bases and reject abstract "
      "objects");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "Member 'secret' of 'PrivateState' is private") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'sliced'") &&
          hasDiagnostic(invalid.diagnostics, "parameter requires 'Abstract&'"),
      "private state, object slicing, and implicit call upcasts should stay "
      "rejected");
}

void testDirectBraceConstruction() {
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() { return this.value; }
};

using IntBox = Box<int>;

class Holder {
  Box<int> box{3};

public:
  int read() { return this.box.read(); }
};

class Pair {
  int left;
  int right;

public:
  Pair(int left, int right) : left(left), right(right) {}
  int sum() { return this.left + this.right; }
};

struct Point {
  int x = 4;
};

int main() {
  Box<int> boxed{1};
  mut Box<int> changing{2};
  IntBox aliased{3,};
  Holder holder{};
  Point point{};
  Pair pair{5, 6,};
  return boxed.read() + changing.read() + aliased.read() + holder.read() +
         point.x + pair.sum();
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("direct-initialization.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected direct-initialization diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "class direct initialization should pass the shared frontend");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *boxed = main == nullptr || main->body()->statements().empty()
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                main->body()->statements().front().get());
  const auto *initializer = boxed == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::DirectInitializer *>(
                                      boxed->initializer().get());
  const lang::ResolvedConstructionInfo *construction =
      initializer == nullptr
          ? nullptr
          : frontend.semantics.findConstruction(*initializer);
  expect(initializer != nullptr && initializer->arguments().size() == 1 &&
             lang::AstPrinter().print(*initializer) == "(direct-init 1)",
         "the AST should preserve direct constructor arguments");
  expect(construction != nullptr && construction->constructor != 0 &&
             construction->constructedType.kind == lang::SemanticType::Class,
         "semantics should retain the exact selected constructor identity");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainInstance = &instance;
      break;
    }
  }
  const lang::HirStatement *boxedStatement =
      mainInstance == nullptr || mainInstance->body.roots.empty()
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots.front());
  const lang::HirValue *boxedValue =
      boxedStatement == nullptr || !boxedStatement->value
          ? nullptr
          : mainInstance->body.findValue(*boxedStatement->value);
  expect(boxedValue != nullptr &&
             boxedValue->kind == lang::HirValueKind::DirectInitializer &&
             boxedValue->operands.size() == 1 &&
             boxedValue->constructorTarget.has_value(),
         "HIR should retain direct initialization and its constructor edge");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &frontend.semantics, &frontend.hir)
          .emit(frontend.program);
  expect(generated.find("const Box<std::int32_t> boxed = "
                        "Box<std::int32_t>(1)") != std::string::npos &&
             generated.find("Box<std::int32_t> changing = "
                            "Box<std::int32_t>(2)") != std::string::npos &&
             generated.find("const Holder holder = Holder()") !=
                 std::string::npos &&
             generated.find("const Pair pair = Pair(5, 6)") !=
                 std::string::npos &&
             generated.find("Box<std::int32_t> box = "
                            "Box<std::int32_t>(3)") != std::string::npos,
         "the backend should lower braces through explicit constructor calls");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-direct-initialization.gti", R"(
class Sample {
public:
  Sample(int value) {}
};

int main() {
  int primitive{1};
  int values[1]{1};
  auto inferred{1};
  Sample copy_list = {1};
  Sample& reference{1};
  Sample mismatch{true};
  Sample missing;
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "unsupported brace forms and constructor mismatches should fail");
  expect(
      countDiagnosticCode(invalid.diagnostics, "GTI-S2038") == 4 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2028") == 1 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2012") == 1 &&
          hasDiagnostic(invalid.diagnostics,
                        "limited to classes and structs") &&
          hasDiagnostic(invalid.diagnostics, "initialize a fixed array") &&
          hasDiagnostic(invalid.diagnostics, "does not use '= {...}'") &&
          hasDiagnostic(invalid.diagnostics, "cannot initialize a reference") &&
          hasDiagnostic(invalid.diagnostics, "require explicit construction"),
      "direct initialization should diagnose each unsupported form at the "
      "GTI layer");

  lang::Lexer lexer;
  lang::Parser recoveryParser(lexer.scan(R"(
class Sample {
public:
  Sample(int value) {}
};
int main() {
  Sample broken{1;
  Sample retained{2};
  return retained.value;
}
)",
                                         "direct-recovery.gti"));
  const lang::Program recovered = recoveryParser.parse();
  const lang::FunctionDecl *recoveredMain =
      findTopLevelFunction(recovered, "main");
  expect(recoveryParser.hadError() && recoveredMain != nullptr &&
             recoveredMain->body()->statements().size() == 2,
         "a malformed direct initializer should preserve later declarations");

  lang::Parser parenthesizedParser(lexer.scan(R"(
class Sample {
public:
  Sample(int value) {}
};
int main() {
  mut Sample value(1);
  return 0;
}
)"));
  parenthesizedParser.parse();
  expect(parenthesizedParser.hadError() &&
             hasDiagnostic(parenthesizedParser.errors(),
                           "use 'Type name{arguments};'"),
         "block-scope parenthesized declarations should direct users to the "
         "unambiguous brace form");

  const std::string formatted = lang::Formatter().format(
      "class Box<T>{public:Box(T value){}};int main(){mut Box<int> "
      "value { 1, };Box<int> empty { };int values[1]={1};return 0;}");
  if (formatted.find("mut Box<int> value{1,};") == std::string::npos ||
      formatted.find("Box<int> empty{};") == std::string::npos ||
      formatted.find("int values[1] = {1};") == std::string::npos) {
    std::cerr << "Direct-initialization formatted output was:\n" << formatted;
  }
  expect(formatted.find("mut Box<int> value{1,};") != std::string::npos &&
             formatted.find("Box<int> empty{};") != std::string::npos &&
             formatted.find("int values[1] = {1};") != std::string::npos,
         "formatter should keep direct braces compact and distinct from "
         "array initialization");
  expect(lang::Formatter().format(formatted) == formatted,
         "direct brace formatting should be idempotent");
}

void testDestructorsAndActiveDropState() {
  const std::string source = R"(
class CleanupBuffer<T> {
  mut gti_internal::storage<T> data;
  mut uint64_t count = 0;

public:
  CleanupBuffer(uint64_t capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  ~CleanupBuffer() {
    while (this.count > 0) {
      this.count--;
      gti_internal::storage_destroy(this.data, this.count);
    }
  }

  void push(T value) mut {
    gti_internal::storage_construct(this.data, this.count, value);
    this.count++;
  }
};

CleanupBuffer<int> transfer(CleanupBuffer<int> value) {
  return std::move(value);
}

int main() {
  mut CleanupBuffer<int> values = CleanupBuffer<int>(uint64_t(2));
  values.push(7);
  CleanupBuffer<int> moved = transfer(std::move(values));
  return 0;
}
)";

  lang::FrontendResult frontend = lang::Frontend().analyze(
      "destructor.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected destructor diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "public destructors should support mutable vector-style cleanup");

  const lang::ClassDecl *cleanupClass =
      findTopLevelClass(frontend.program, "CleanupBuffer");
  const auto *destructor = cleanupClass == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::DestructorDecl *>(
                                     cleanupClass->members().at(4).get());
  const lang::ClassLifecycleInfo *lifecycle =
      cleanupClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*cleanupClass);
  expect(destructor != nullptr && destructor->name().lexeme == "CleanupBuffer",
         "the parser should retain destructor identity and body structure");
  expect(
      lifecycle != nullptr && lifecycle->declaredDestructor &&
          lifecycle->declaredDestructor->declaration == destructor &&
          lifecycle->destructor == lang::SpecialMemberStatus::Declared &&
          lifecycle->requiresActiveDropState &&
          lifecycle->copyConstructor == lang::SpecialMemberStatus::Deleted &&
          lifecycle->copyAssignment == lang::SpecialMemberStatus::Deleted &&
          lifecycle->moveConstructor == lang::SpecialMemberStatus::Generated &&
          lifecycle->moveAssignment == lang::SpecialMemberStatus::Generated &&
          !lifecycle->traits.copyable && lifecycle->traits.movable,
      "declared cleanup should be explicit, noncopyable, and safely movable");

  const lang::HirDestructorInstance *hirDestructor = nullptr;
  for (const lang::HirDestructorInstance &instance :
       frontend.hir.destructorInstances()) {
    if (instance.source == destructor) {
      hirDestructor = &instance;
      break;
    }
  }
  const lang::HirStatement *cleanupLoop =
      hirDestructor == nullptr || hirDestructor->body.roots.empty()
          ? nullptr
          : hirDestructor->body.findStatement(
                hirDestructor->body.roots.front());
  expect(hirDestructor != nullptr && cleanupLoop != nullptr &&
             cleanupLoop->kind == lang::HirStatementKind::While &&
             cleanupLoop->condition.has_value() &&
             cleanupLoop->body.has_value(),
         "typed HIR should retain concrete destructor control flow");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  const lang::ClassTypeInfo *cleanupType =
      cleanupClass == nullptr ? nullptr
                              : frontend.semantics.findClassType(*cleanupClass);
  const std::string lifecycleSuffix =
      cleanupType == nullptr ? "" : std::to_string(cleanupType->id);
  const std::string cleanupName = "__gti_lifecycle_cleanup_" + lifecycleSuffix;
  const std::string activeName = "__gti_lifecycle_active_" + lifecycleSuffix;
  expect(
      cleanupType != nullptr &&
          artifact.contents.find("~CleanupBuffer() noexcept { " + cleanupName +
                                 "(); }") != std::string::npos &&
          artifact.contents.find("bool " + activeName + " = true;") !=
              std::string::npos &&
          artifact.contents.find("if (!" + activeName + ") { return; }") !=
              std::string::npos &&
          artifact.contents.find("CleanupBuffer(const CleanupBuffer &) = "
                                 "delete;") != std::string::npos &&
          artifact.contents.find(
              "CleanupBuffer(CleanupBuffer &&other) noexcept") !=
              std::string::npos &&
          artifact.contents.find("other." + activeName + " = false;") !=
              std::string::npos &&
          artifact.contents.find(cleanupName + "();") != std::string::npos &&
          artifact.contents.find("gti_internal::backend::storage_destroy") !=
              std::string::npos,
      "the backend should lower declared cleanup through an active drop state "
      "and generated safe moves");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-destructor.gti", R"(
class WrongName {
public:
  ~Other() {}
};

class DuplicateCleanup {
public:
  ~DuplicateCleanup() {}
  ~DuplicateCleanup() {}
};

class PrivateCleanup {
  ~PrivateCleanup() {}
};

class ReturningCleanup {
public:
  ~ReturningCleanup() { return; }
};

int main() { return 0; }
)");
  expect(!invalid.canGenerateCode(),
         "invalid destructor declarations should fail in the frontend");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-S2021") &&
             hasDiagnostic(invalid.diagnostics, "Destructor name must match") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot declare more than one destructor") &&
             hasDiagnostic(invalid.diagnostics, "Destructors must be public") &&
             hasDiagnostic(invalid.diagnostics,
                           "Destructors cannot contain return statements"),
         "destructor diagnostics should cover identity, uniqueness, access, "
         "and control flow");

  lang::Lexer lexer;
  lang::Parser parameterParser(lexer.scan(R"(
class ParameterizedCleanup {
public:
  ~ParameterizedCleanup(int value) {}
};
int main() { return 0; }
)"));
  const lang::Program recovered = parameterParser.parse();
  const auto *recoveredMain = recovered.declarations().empty()
                                  ? nullptr
                                  : dynamic_cast<const lang::FunctionDecl *>(
                                        recovered.declarations().back().get());
  expect(parameterParser.hadError() &&
             hasDiagnostic(parameterParser.errors(),
                           "Destructors do not take parameters") &&
             recoveredMain != nullptr && recoveredMain->name().lexeme == "main",
         "destructor parameter errors should recover to later declarations");

  const std::string formatted = lang::Formatter().format(
      "class Trace{mut int state=1;public:~Trace(){while(this.state>0){"
      "this.state--;}}};");
  expect(formatted.find("~Trace() {") != std::string::npos &&
             formatted.find("while (this.state > 0) {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "destructor syntax should format with stable C++-style layout");

  const lang::FrontendResult reservedName = lang::Frontend().analyze(
      "reserved-lifecycle-name.gti",
      "class Collision { mut bool __gti_lifecycle_active_1 = true; };");
  expect(!reservedName.canGenerateCode() &&
             hasDiagnosticCode(reservedName.diagnostics, "GTI-L0008"),
         "source identifiers must not collide with generated lifecycle names");
}

void testRestrictedMemberOperators() {
  const std::string source = R"(
struct Payload {
  mut int value = 0;

  void increment() mut { this.value += 1; }
};

class Handle {
  mut Payload payload = Payload();
  mut int values[2] = {1, 2};

public:
  Payload& operator->() { return this.payload; }
  mut Payload& operator->() mut { return this.payload; }
  int& operator*() { return this.values[0]; }
  mut int& operator*() mut { return this.values[0]; }
  int& operator[](uint64_t index) { return this.values[index]; }
  mut int& operator[](uint64_t index) mut { return this.values[index]; }
  bool operator==(nullptr_t other) { return false; }
  bool operator!=(nullptr_t other) { return true; }
  operator bool() { return true; }
};

int main() {
  mut Handle handle = Handle();
  handle->increment();
  *handle = 7;
  handle[uint64_t(1)] += 3;
  if (handle and handle != nullptr) {
    return *handle + handle[uint64_t(1)] - 12;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("member-operators.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected operator diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the restricted pointer-like member operators should validate");

  const auto *handle = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(1).get());
  std::size_t operatorCount = 0;
  bool foundMutableReferenceReturn = false;
  if (handle != nullptr) {
    for (const lang::StmtPtr &member : handle->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function == nullptr || !function->operatorName()) {
        continue;
      }
      ++operatorCount;
      foundMutableReferenceReturn =
          foundMutableReferenceReturn ||
          (function->returnMutability() == lang::Mutability::Mutable &&
           function->returnType().reference.has_value());
    }
  }
  expect(operatorCount == 9 && foundMutableReferenceReturn,
         "the AST should retain operator identity and mutable reference "
         "returns");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("___gti_operator_arrow") != std::string::npos &&
             artifact.contents.find("___gti_operator_dereference") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_subscript") !=
                 std::string::npos &&
             artifact.contents.find(".operator!=(nullptr)") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_bool()") !=
                 std::string::npos &&
             artifact.contents.find(" operator*(") == std::string::npos,
         "the backend should retain selected method calls while exposing only "
         "validated comparisons as C++ operators for generic representation");

  const std::string formatted = lang::Formatter().format(
      "class Handle{public:mut int& operator*()mut{return this.value;}"
      "bool operator<(Handle& other){return false;}"
      "operator bool(){return true;}};");
  expect(formatted.find("mut int & operator*() mut {") != std::string::npos &&
             formatted.find("bool operator<(Handle & other) {") !=
                 std::string::npos &&
             formatted.find("operator bool() {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "operator declarations should format with stable C++-style layout");

  const lang::FrontendResult invalidContracts =
      lang::Frontend().analyze("invalid-operator-contracts.gti", R"(
class InvalidOperators {
  mut int value = 0;
public:
  int operator*() { return this.value; }
  int operator->() { return this.value; }
  int& operator[](uint64_t first, uint64_t second) { return this.value; }
  bool operator==(nullptr_t other) mut { return true; }
  int operator<(InvalidOperators& other) { return 0; }
  operator bool() mut { return true; }
};
int main() { return 0; }
)");
  expect(!invalidContracts.canGenerateCode() &&
             hasDiagnosticCode(invalidContracts.diagnostics, "GTI-S2022") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "operator* must return a checked reference") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "operator-> must return a checked reference") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "operator[] expects 1 parameter") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "operator< must return bool") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "must use a read-only receiver"),
         "operator declarations should enforce the restricted contracts");

  const lang::FrontendResult invalidUses =
      lang::Frontend().analyze("invalid-operator-uses.gti", R"(
class ReadOnlyHandle {
  int value = 0;
public:
  int& operator*() { return this.value; }
  bool operator==(int other) { return true; }
};
int main() {
  mut ReadOnlyHandle handle = ReadOnlyHandle();
  *handle = 1;
  if (handle == nullptr) { return 1; }
  if (handle) { return 2; }
  return 0;
}
)");
  expect(!invalidUses.canGenerateCode() &&
             hasDiagnostic(invalidUses.diagnostics,
                           "Dereference assignment requires mutable access") &&
             hasDiagnostic(invalidUses.diagnostics,
                           "No exact overload of operator==") &&
             hasDiagnostic(invalidUses.diagnostics,
                           "does not define operator bool"),
         "operator use should require mutable access, exact operands, and an "
         "explicit contextual conversion");

  lang::Lexer lexer;
  lang::Parser unsupportedParser(lexer.scan(R"(
class Unsupported {
public:
  int operator+(int other) { return other; }
};
int main() { return 0; }
)"));
  const lang::Program recovered = unsupportedParser.parse();
  expect(unsupportedParser.hadError() &&
             hasDiagnostic(unsupportedParser.errors(),
                           "Supported overloads are operator*") &&
             !recovered.declarations().empty(),
         "unsupported operators should receive a focused parser diagnostic "
         "and recover to later declarations");

  lang::Parser freeOperatorParser(lexer.scan(R"(
int operator*(int value) { return value; }
int main() { return 0; }
)"));
  freeOperatorParser.parse();
  expect(freeOperatorParser.errors().size() == 1 &&
             hasDiagnostic(freeOperatorParser.errors(),
                           "only be declared as class or struct members"),
         "free operator overload declarations should be rejected by the "
         "source grammar");

  const lang::FrontendResult invalidAccess =
      lang::Frontend().analyze("invalid-operator-access.gti", R"(
class PrivateTruth {
  operator bool() { return true; }
};
class MutableOnly {
  mut int value = 0;
public:
  mut int& operator*() mut { return this.value; }
};
int main() {
  PrivateTruth hidden = PrivateTruth();
  MutableOnly fixed = MutableOnly();
  if (hidden) { return *fixed; }
  return 0;
}
)");
  expect(!invalidAccess.canGenerateCode() &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "operator bool of 'PrivateTruth' is private") &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "operator* requires a mutable receiver"),
         "operator access and receiver mutability should be enforced before "
         "lowering");
}

void testCallableMemberOperators() {
  const std::string source = R"(
class Accumulator {
  mut int total;

public:
  Accumulator(int initial) : total(initial) {}

  int operator()() { return this.total; }
  int operator()(int value) { return this.total + value; }
  int operator()(int value) mut {
    this.total += value;
    return this.total;
  }
  int operator()(int left, int right) { return this.total + left + right; }
};

class Identity<T> {
public:
  T operator()(T value) { return value; }
};

class Slot {
  mut int value = 1;

public:
  int& operator()() { return this.value; }
  mut int& operator()() mut { return this.value; }
};

int main() {
  Accumulator fixed = Accumulator(1);
  mut Accumulator changing = Accumulator(2);
  Identity<int> identity = Identity<int>();
  int first = fixed();
  int second = fixed(3);
  int third = changing(4);
  int fourth = fixed(5, 6);
  int generic = identity(7);
  mut Slot slot = Slot();
  mut int& slot_value = slot();
  slot_value = first + second + third + fourth + generic;
  return slot() - 25;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("call-operator.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected call-operator diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "operator() should support exact arbitrary-arity member overloads");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("___gti_operator_call") != std::string::npos &&
             artifact.contents.find(" operator()(") == std::string::npos,
         "call operators should lower to the semantically selected member");

  const std::string formatted = lang::Formatter().format(
      "class Fn{public:int operator()(int value){return value;}};");
  expect(formatted.find("int operator()(int value) {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "operator() declarations should format idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-call-operator.gti", R"(
class PrivateCallable {
  int operator()(int value) { return value; }
};
class MutableCallable {
public:
  int operator()(int value) mut { return value; }
};
class ExactCallable {
public:
  int operator()(int value) { return value; }
};
int main() {
  PrivateCallable hidden = PrivateCallable();
  MutableCallable fixed = MutableCallable();
  ExactCallable exact = ExactCallable();
  int private_value = hidden(1);
  int mutable_value = fixed(1);
  int wrong_type = exact(true);
  int explicit_types = exact<int>(1);
  return private_value + mutable_value + wrong_type + explicit_types;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "operator() of 'PrivateCallable' is private") &&
             hasDiagnostic(invalid.diagnostics,
                           "operator() requires a mutable receiver") &&
             hasDiagnostic(invalid.diagnostics,
                           "No exact overload of operator()") &&
             hasDiagnostic(invalid.diagnostics,
                           "do not take explicit type arguments"),
         "call operators should enforce access, receiver mutability, and "
         "exact argument matching");

  const lang::FrontendResult temporaryBorrow =
      lang::Frontend().analyze("temporary-call-borrow.gti", R"(
class Slot {
  int value = 1;
public:
  int& operator()() { return this.value; }
};
int main() {
  int& dangling = Slot()();
  return dangling;
}
)");
  expect(!temporaryBorrow.canGenerateCode() &&
             hasDiagnostic(temporaryBorrow.diagnostics,
                           "derived from temporary storage"),
         "reference-returning call operators should preserve borrow lifetime "
         "checks");
}

void testRangeBasedForAndIteratorProtocol() {
  const std::string source = R"(
interface IteratorContract<T> {
  T& operator*() = 0;
  void operator++() mut = 0;
};

struct CounterSentinel {
  int limit;
  CounterSentinel(int value) : limit(value) {}
};

class CounterIterator : public IteratorContract<int> {
  mut int current;

public:
  CounterIterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(CounterSentinel& sentinel) {
    return this.current != sentinel.limit;
  }
};

class CounterRange {
  int first;
  int last;

public:
  CounterRange(int first, int last) : first(first), last(last) {}
  CounterIterator begin() { return CounterIterator(this.first); }
  CounterSentinel end() { return CounterSentinel(this.last); }
};

int main() {
  CounterRange values{1, 5};
  mut int total = 0;
  for (auto& value : values) {
    if (value == 2) { continue; }
    total += value;
  }
  return total - 8;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("range-for.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected range-for diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "range-based for should use the ordinary structural iterator "
         "protocol through generic virtual operator contracts");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == main) {
      mainInstance = &instance;
      break;
    }
  }
  bool hasRangeFor = false;
  bool hasProtocolCalls = false;
  if (mainInstance != nullptr) {
    for (const lang::HirStatementId root : mainInstance->body.roots) {
      const lang::HirStatement *statement =
          mainInstance->body.findStatement(root);
      hasRangeFor =
          hasRangeFor || (statement != nullptr &&
                          statement->kind == lang::HirStatementKind::RangeFor);
    }
    std::size_t selectedCalls = 0;
    for (const lang::HirValue &value : mainInstance->body.values) {
      if (value.functionTarget) {
        ++selectedCalls;
      }
    }
    hasProtocolCalls = selectedCalls >= 5;
  }
  expect(hasRangeFor && hasProtocolCalls,
         "HIR should retain range syntax provenance and concrete begin, end, "
         "comparison, dereference, and increment targets");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const auto &occurrences = frontend.semantics.database().occurrences(unit);
  expect(std::none_of(occurrences.begin(), occurrences.end(),
                      [](const lang::SemanticOccurrence &occurrence) {
                        return occurrence.name.rfind("__gti_", 0) == 0;
                      }),
         "compiler-generated range bindings should not leak into semantic "
         "tooling occurrences");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  const bool emittedRangeCore =
      artifact.contents.find("__gti_range_") != std::string::npos &&
      artifact.contents.find("__gti_iterator_") != std::string::npos &&
      artifact.contents.find("__gti_operator_pre_increment") !=
          std::string::npos &&
      artifact.contents.find("for (; ") != std::string::npos;
  expect(emittedRangeCore,
         "the C++ backend should emit one stable range borrow and the selected "
         "GTI iterator methods without native C++ range lookup");

  const std::string formatted = lang::Formatter().format(
      "class Iterator{public:void operator++()mut{}};"
      "int sum(Range& values){mut int total=0;"
      "for(auto& value:values){total+=value;}return total;}");
  expect(formatted.find("void operator++() mut {") != std::string::npos &&
             formatted.find("for (auto & value : values) {") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "prefix increment and range-based for syntax should format "
         "idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-range-for.gti", R"(
class BadIncrement {
  mut int value = 0;
public:
  int& operator*() { return this.value; }
  int operator++() mut { return this.value; }
};

class ReadOnlyIncrement {
public:
  void operator++() {}
};

class ValueSentinelIterator {
  mut int value = 0;
public:
  int& operator*() { return this.value; }
  void operator++() mut { this.value++; }
  bool operator!=(ValueSentinelIterator other) {
    return this.value != other.value;
  }
};

class ValueSentinelRange {
public:
  ValueSentinelIterator begin() { return ValueSentinelIterator(); }
  ValueSentinelIterator end() { return ValueSentinelIterator(); }
};

class MissingBegin {};

int main() {
  mut int value = 1;
  MissingBegin range{};
  for (int item : range) { return item; }
  for (int item : MissingBegin()) { return item; }
  ValueSentinelRange copied_sentinel{};
  for (int item : copied_sentinel) { return item; }
  BadIncrement iterator{};
  ++iterator;
  return value;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "Prefix operator++ must return void") &&
             hasDiagnostic(invalid.diagnostics,
                           "Prefix operator++ must use a mutable receiver") &&
             hasDiagnostic(invalid.diagnostics,
                           "Reference initializer must be an addressable") &&
             hasDiagnostic(invalid.diagnostics,
                           "must accept its sentinel by read-only reference") &&
             hasDiagnostic(invalid.diagnostics, "Unknown member 'begin'") &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2022"),
         "range and iterator diagnostics should reject malformed operator "
         "contracts, temporary auto references, and missing begin members");
  const auto generatedDiagnostic =
      std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.message.find("Unknown member 'begin'") !=
                            std::string::npos;
                   });
  expect(generatedDiagnostic != invalid.diagnostics.end() &&
             generatedDiagnostic->primary.end ==
                 generatedDiagnostic->primary.start + 1,
         "diagnostics from lowered range operations should point at the "
         "source colon rather than a synthetic identifier width");
}

void testNamedGenerics() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
namespace std { using string_view = gti_internal::text_view; }

class Box<T> {
  mut T value;

public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
  U echo<U>(U replacement) { return replacement; }
  void set(T replacement) mut { this.value = replacement; }
};

T identity<T>(T value) { return value; }
T unbox<T>(Box<T> box) { return box.get(); }
U relay<T, U>(Box<T> box, U value) { return box.echo<U>(value); }

int main() {
  mut Box<int> box = Box<int>(identity(7));
  box.set(identity<int>(9));
  int value = unbox(box);
  int relayed = relay(box, box.echo<int>(value));
  std::string_view text = identity<std::string_view>("generic");
  return relayed;
}

)");
  expect(!lexer.hadError(), "named generic source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "generic classes, functions, and applications should parse");

  lang::SemanticVisitor validSemantic;
  const bool valid = validSemantic.check(validProgram);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : validSemantic.errors()) {
      std::cerr << "Unexpected generic diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid, "generic substitution and exact inference should validate");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("template <typename T>\nclass Box;") !=
                 std::string::npos &&
             generated.find("template <typename T>\nclass Box {") !=
                 std::string::npos,
         "generic classes should lower with matching C++ forward declarations");
  expect(generated.find("template <typename T>\nT identity(const T value)") !=
                 std::string::npos &&
             generated.find("template <typename T>\nT unbox(") !=
                 std::string::npos,
         "generic functions should lower as C++ function templates");
  expect(generated.find("Box<std::int32_t> box = "
                        "Box<std::int32_t>(identity(7))") !=
                 std::string::npos &&
             generated.find("identity<std::int32_t>(9)") != std::string::npos,
         "applied types and explicit generic calls should lower recursively");
  expect(generated.find(".template echo<std::int32_t>(") != std::string::npos,
         "the backend should hide C++ dependent-template disambiguation");

  auto invalidTokens = lexer.scan(R"(
class Duplicate<T, T> {};
class SameName<SameName> {};
class Shadow<T> {
public:
  T replace<T>(T value) { return value; }
};
class Box<T> {
  T value;
public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
};

T identity<T>(T value) { return value; }
T choose<T>(T left, T right) { return left; }
T unsupported_add<T>(T left, T right) { return left + right; }
bool unsupported_equal<T>(T left, T right) { return left == right; }
T make<T>();
int ordinary(int value) { return value; }
int main<T>() { return 0; }

int use() {
  Box missing = Box(1);
  Box<int, bool> excessive = Box<int, bool>(1);
  Box<void> impossible = Box<void>(1);
  int mismatch = identity<int>(true);
  int conflict = choose(1, true);
  int unknown = make();
  int excessive_types = identity<int, bool>(1);
  int not_generic = ordinary<int>(1);
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid generic semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid generic applications should be rejected semantically");
  expect(hasDiagnostic(invalidSemantic, "Duplicate generic type parameter"),
         "generic parameter names should be unique");
  expect(hasDiagnostic(invalidSemantic, "same name as its declaration") &&
             hasDiagnostic(invalidSemantic, "cannot shadow"),
         "generic parameters should not collide with enclosing declarations");
  expect(hasDiagnostic(invalidSemantic, "requires 1 generic type argument"),
         "generic class applications should enforce arity");
  expect(hasDiagnostic(invalidSemantic, "cannot be void"),
         "void should not be accepted as a user generic argument");
  expect(hasDiagnostic(invalidSemantic, "Conflicting types inferred"),
         "repeated generic parameters should infer one exact type");
  expect(
      hasDiagnostic(invalidSemantic, "numeric operands"),
      "unconstrained type parameters should not gain operators by duck typing");
  expect(hasDiagnostic(invalidSemantic, "Equality operands"),
         "generic equality should wait for an explicit contract model");
  expect(hasDiagnostic(invalidSemantic, "Cannot infer generic type parameter"),
         "return-only generic parameters should require explicit arguments");
  expect(
      hasDiagnostic(invalidSemantic, "wrong number of type arguments") &&
          hasDiagnostic(invalidSemantic, "Non-generic functions do not take"),
      "explicit function type arguments should enforce generic arity");
  expect(hasDiagnostic(invalidSemantic, "main entry point cannot be generic"),
         "the native entry point should remain non-generic");

  const std::string formatted = lang::Formatter().format(
      "class Box<T>{T value;public:Box(T value):value(value){}T get(){return "
      "this.value;}};T identity<T>(T value){return value;}int main(){Box<"
      "Box<int>> nested=Box<Box<int>>(Box<int>(1));int value=identity<int>(1);"
      "return value;}");
  expect(formatted.find("class Box<T> {") != std::string::npos &&
             formatted.find("T identity<T>(T value) {") != std::string::npos &&
             formatted.find("Box<Box<int>> nested = Box<Box<int>>(") !=
                 std::string::npos &&
             formatted.find("identity<int>(1)") != std::string::npos,
         "formatter should preserve compact generic angle brackets");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted generic syntax should be idempotent");

  const std::string comparison =
      lang::Formatter().format("bool result=a < b > c;");
  expect(comparison == "bool result = a < b > c;\n",
         "formatter should not treat relational expressions as generic types");

  lang::Parser malformedParser(lexer.scan("class Broken<> {}; int okay = 1;"));
  const lang::Program recovered = malformedParser.parse();
  expect(malformedParser.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after malformed generic parameters");
}

void testConstrainedGenerics() {
  const std::string source = R"(
T minimum<std::ordered T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}

T multiply<std::numeric T>(T left, T right) {
  return T(left * right);
}

T remainder<std::integral T>(T left, T right) {
  return T(left % right);
}

T negate<std::signed_numeric T>(T value) { return T(-value); }
T signed_integer<std::signed_integral T>(T value) { return value; }
T unsigned_integer<std::unsigned_integral T>(T value) { return value; }
T floating_value<std::floating_point T>(T value) { return value; }

T square_integral<std::integral T>(T value) {
  return multiply(value, value);
}

void consume_integrals<std::integral Values...>(Values... values) {}

class NumericBox<std::numeric T> {
  T value;

public:
  NumericBox(T value) : value(value) {}
  T doubled() { return T(this.value + this.value); }
};

int main() {
  int low = minimum(2, 5);
  int product = multiply(3, 4);
  int rest = remainder(7, 4);
  int negative = negate(2);
  int signed_value = signed_integer(2);
  uint64_t unsigned_value = unsigned_integer(uint64_t(2));
  float decimal = floating_value(2.5);
  int square = square_integral(5);
  consume_integrals(1, uint64_t(2));
  NumericBox<int> box = NumericBox<int>(6);
  if (low == 2 and product == 12 and rest == 3 and negative == -2 and
      signed_value == 2 and unsigned_value == uint64_t(2) and decimal == 2.5 and
      square == 25 and box.doubled() == 12) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult valid = lang::Frontend().analyze(
      "constrained-generics.gti", source, {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected constrained generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "standard numeric constraints should validate generic operations, "
         "propagation, packs, and classes");

  const lang::FunctionDecl *minimum =
      findTopLevelFunction(valid.program, "minimum");
  expect(minimum != nullptr && minimum->genericParameters().size() == 1 &&
             minimum->genericParameters().front().constraint &&
             minimum->genericParameters().front().constraint->segments.size() ==
                 2 &&
             minimum->genericParameters()
                     .front()
                     .constraint->segments.back()
                     .lexeme == "ordered",
         "the AST should retain the qualified generic constraint path");

  lang::CppEmitter emitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                           nullptr, &valid.semantics);
  const std::string generated = emitter.emit(valid.program);
  expect(generated.find("template <typename T>\nT __gti_fn_") !=
                 std::string::npos &&
             generated.find("_minimum(T left, T right)") != std::string::npos &&
             generated.find("numeric_cast<T>(gti_internal::backend::multiply("
                            "left, right))") != std::string::npos,
         "constraints should remain frontend metadata while generic numeric "
         "conversions lower through checked backend casts");

  lang::FrontendResult capabilities =
      lang::Frontend().analyze("exact-generic-capabilities.gti", R"(
class Rank {
  int value;

public:
  Rank() : value(0) {}
  Rank(int value) : value(value) {}
  int get() { return this.value; }

  bool operator==(Rank& other) { return this.value == other.value; }
  bool operator!=(Rank& other) { return this.value != other.value; }
  bool operator<(Rank& other) { return this.value < other.value; }
  bool operator<=(Rank& other) { return this.value <= other.value; }
  bool operator>(Rank& other) { return this.value > other.value; }
  bool operator>=(Rank& other) { return this.value >= other.value; }
};

class MoveOnly {
  int value;

public:
  MoveOnly() : value(0) {}
  MoveOnly(int value) : value(value) {}
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};

bool equivalent<std::equality_comparable T>(T left, T right) {
  return left == right;
}

T earlier<std::totally_ordered T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}

T legacy_earlier<std::ordered T>(T left, T right) {
  return earlier(left, right);
}

T copy_value<std::copyable T>(T value) { return value; }
T move_value<std::movable T>(T value) { return std::move(value); }
T default_value<std::default_initializable T>() { return T(); }
bool ordered_equal<std::totally_ordered T>(T left, T right) {
  return equivalent(left, right);
}
T numeric_copy<std::numeric T>(T value) { return copy_value(value); }
T copy_move<std::copyable T>(T value) {
  return move_value(std::move(value));
}

int main() {
  Rank first = Rank(1);
  Rank second = Rank(2);
  bool same = equivalent(first, first);
  Rank lower = earlier(first, second);
  Rank legacy = legacy_earlier(first, second);
  Rank copied = copy_value(first);
  Rank moved_copy = copy_move(first);
  Rank zero = default_value<Rank>();
  int copied_number = numeric_copy(4);
  MoveOnly source = MoveOnly(3);
  MoveOnly moved = move_value(std::move(source));
  if (same and ordered_equal(first, first) and lower.get() == 1 and
      legacy.get() == 1 and copied.get() == 1 and moved_copy.get() == 1 and
      zero.get() == 0 and copied_number == 4) {
    return 0;
  }
  return 1;
}
)",
                               {standardLibraryPrelude()});
  if (!capabilities.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : capabilities.diagnostics) {
      std::cerr << "Unexpected exact capability diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(capabilities.canGenerateCode(),
         "exact lifecycle and comparison capabilities should accept "
         "matching primitives and nominal classes");

  bool sawDefaultConstructionHir = false;
  for (const lang::HirFunctionInstance &instance :
       capabilities.hir.functionInstances()) {
    sawDefaultConstructionHir =
        sawDefaultConstructionHir ||
        std::any_of(
            instance.body.values.begin(), instance.body.values.end(),
            [](const lang::HirValue &value) {
              return value.intrinsic ==
                     lang::IntrinsicKind::DefaultTypeParameterConstruction;
            });
  }
  bool sawDefaultConstructionMir = false;
  for (const lang::MirFunctionInstance &instance :
       capabilities.mir.functionInstances()) {
    for (const lang::MirBlock &block : instance.body.blocks) {
      sawDefaultConstructionMir =
          sawDefaultConstructionMir ||
          std::any_of(
              block.instructions.begin(), block.instructions.end(),
              [](const lang::MirInstruction &instruction) {
                return instruction.intrinsic ==
                       lang::IntrinsicKind::DefaultTypeParameterConstruction;
              });
    }
  }
  expect(sawDefaultConstructionHir && sawDefaultConstructionMir,
         "constrained generic default construction should retain an explicit "
         "intrinsic through HIR and MIR");

  lang::CppEmitter capabilityEmitter(lang::CppStandard::Cpp23,
                                     lang::TargetInfo::host(), nullptr,
                                     &capabilities.semantics);
  const std::string capabilityCpp =
      capabilityEmitter.emit(capabilities.program);
  expect(capabilityCpp.find("operator<(") != std::string::npos &&
             capabilityCpp.find("operator>=(") != std::string::npos &&
             capabilityCpp.find("return T();") != std::string::npos,
         "the C++ backend should emit selected relational methods and generic "
         "default construction without C++ concepts");

  lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-constrained-generics.gti", R"(
T unsupported_add<T>(T left, T right) { return left + right; }
T ordered_value<std::ordered T>(T value) { return value; }
T needs_numeric<std::numeric T>(T value) { return value; }
T integral_value<std::integral T>(T value) { return value; }
T signed_number<std::signed_numeric T>(T value) { return value; }
T forwards_ordered<std::ordered T>(T value) { return needs_numeric(value); }
T bad_negate<std::numeric T>(T value) { return -value; }
T bad_modulo<std::numeric T>(T left, T right) { return left % right; }
T bad_conversion<T>(T value) { return T(1); }
T unknown<std::mystery T>(T value) { return value; }
T signed_integer<std::signed_integral T>(T value) { return value; }
T unsigned_integer<std::unsigned_integral T>(T value) { return value; }
T floating_value<std::floating_point T>(T value) { return value; }

class EqualOnly {
public:
  bool operator==(EqualOnly& other) { return true; }
};

class AlmostOrdered {
public:
  bool operator==(AlmostOrdered& other) { return true; }
  bool operator!=(AlmostOrdered& other) { return false; }
  bool operator<(AlmostOrdered& other) { return false; }
  bool operator<=(AlmostOrdered& other) { return true; }
  bool operator>(AlmostOrdered& other) { return false; }
};

class DeletedCopy {
public:
  DeletedCopy() {}
  DeletedCopy(DeletedCopy& other) = delete;
  DeletedCopy(DeletedCopy&& other) = default;
};

class DeletedMove {
public:
  DeletedMove() {}
  DeletedMove(DeletedMove&& other) = delete;
};

class NoDefault {
  int stored;

public:
  NoDefault(int value) : stored(value) {}
};

bool needs_equality<std::equality_comparable T>(T left, T right) {
  return left == right;
}
T needs_total_order<std::totally_ordered T>(T value) { return value; }
T needs_copy<std::copyable T>(T value) { return value; }
T needs_move<std::movable T>(T value) { return std::move(value); }
T needs_default<std::default_initializable T>() { return T(); }
T unconstrained_default<T>() { return T(); }

class NumericBox<std::numeric T> {
  T value;
public:
  NumericBox(T value) : value(value) {}
};

void use_box(NumericBox<bool> value) {}

int use() {
  bool invalid_ordered = ordered_value(true);
  bool invalid_value = needs_numeric(true);
  float invalid_integral = integral_value(2.0);
  uint64_t invalid_signed_numeric = signed_number(uint64_t(1));
  uint64_t invalid_signed_integral = signed_integer(uint64_t(1));
  int invalid_unsigned_integral = unsigned_integer(1);
  int invalid_floating_point = floating_value(1);
  bool invalid_equality = needs_equality(EqualOnly(), EqualOnly());
  AlmostOrdered invalid_total = needs_total_order(AlmostOrdered());
  DeletedCopy invalid_copy = needs_copy(DeletedCopy());
  DeletedMove invalid_move = needs_move(DeletedMove());
  NoDefault invalid_default = needs_default<NoDefault>();
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode(),
         "unsupported operations and concrete constraint violations should "
         "fail before backend entry");
  expect(
      hasDiagnosticCode(invalid.diagnostics, "GTI-S2029") &&
          hasDiagnostic(invalid.diagnostics,
                        "does not satisfy generic constraint") &&
          hasDiagnostic(invalid.diagnostics,
                        "Unknown generic concept 'std::mystery'") &&
          hasDiagnostic(invalid.diagnostics,
                        "requires the numeric capability") &&
          hasDiagnostic(invalid.diagnostics, "'std::ordered'") &&
          hasDiagnostic(invalid.diagnostics, "'std::integral'") &&
          hasDiagnostic(invalid.diagnostics, "'std::signed_numeric'") &&
          hasDiagnostic(invalid.diagnostics, "'std::signed_integral'") &&
          hasDiagnostic(invalid.diagnostics, "'std::unsigned_integral'") &&
          hasDiagnostic(invalid.diagnostics, "'std::floating_point'") &&
          hasDiagnostic(invalid.diagnostics, "'std::equality_comparable'") &&
          hasDiagnostic(invalid.diagnostics, "'std::totally_ordered'") &&
          hasDiagnostic(invalid.diagnostics, "'std::copyable'") &&
          hasDiagnostic(invalid.diagnostics, "'std::movable'") &&
          hasDiagnostic(invalid.diagnostics, "'std::default_initializable'") &&
          hasDiagnostic(invalid.diagnostics,
                        "requires the default-initialization capability") &&
          hasDiagnostic(invalid.diagnostics, "signed numeric value") &&
          hasDiagnostic(invalid.diagnostics, "requires integer operands") &&
          hasDiagnostic(invalid.diagnostics, "numeric operands"),
      "constraint diagnostics should cover declarations, propagation, "
      "calls, conversions, and required operator capabilities");
  expect(hasDiagnosticHint(invalid.diagnostics,
                           "public, read-only bool operator==") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "exact public, read-only bool overloads") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "available copy construction and assignment") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "available move construction and assignment") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "public zero-argument constructor"),
         "exact capability failures should explain the required public "
         "contract");

  lang::FrontendResult duplicate =
      lang::Frontend().analyze("duplicate-constrained-overload.gti", R"(
T select<std::numeric T>(T value) { return value; }
T select<std::integral T>(T value) { return value; }
)",
                               {standardLibraryPrelude()});
  expect(!duplicate.canGenerateCode() &&
             hasDiagnostic(duplicate.diagnostics,
                           "Duplicate overload signature for 'select'"),
         "constraints should not distinguish or rank overload signatures");

  const std::string formatted = lang::Formatter().format(
      "T minimum<std::totally_ordered T>(T left,T right){if(left<right){return "
      "left;}return right;}");
  expect(
      formatted.find("T minimum<std::totally_ordered T>(T left, T right) {") !=
              std::string::npos &&
          lang::Formatter().format(formatted) == formatted,
      "constrained generic declarations should format idempotently");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan(
      "T broken<std::numeric>(T value) { return value; } int okay = 1;"));
  const lang::Program recovered = malformed.parse();
  expect(malformed.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after a missing constrained "
         "parameter name");
}

void testSourceDefinedConcepts() {
  const std::string source = R"(
concept arithmetic_value<T> = std::numeric<T> && std::copyable<T>;
concept signed_arithmetic<T> =
    arithmetic_value<T> and std::signed_numeric<T>;

T numeric_identity<std::numeric T>(T value) { return value; }

T twice<signed_arithmetic T>(T value) {
  return T(value + value);
}

T forward<signed_arithmetic T>(T value) {
  return numeric_identity(value);
}

int main() {
  int result = forward(twice(3));
  if (result == 6) { return 0; }
  return 1;
}
)";
  lang::FrontendResult valid = lang::Frontend().analyze(
      "source-concepts.gti", source, {standardLibraryPrelude()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected source concept diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "source-defined concept aliases and conjunctions should propagate "
         "their exact capabilities");

  const lang::FunctionDecl *twice =
      findTopLevelFunction(valid.program, "twice");
  const lang::FunctionInfo *twiceInfo =
      twice == nullptr ? nullptr : valid.semantics.findFunction(*twice);
  expect(
      twiceInfo != nullptr && twiceInfo->genericParameters.size() == 1 &&
          lang::hasConstraint(twiceInfo->genericParameters.front().constraints,
                              lang::GenericConstraintKind::Numeric) &&
          lang::hasConstraint(twiceInfo->genericParameters.front().constraints,
                              lang::GenericConstraintKind::SignedNumeric) &&
          lang::hasConstraint(twiceInfo->genericParameters.front().constraints,
                              lang::GenericConstraintKind::Copyable),
      "semantic generic metadata should retain the resolved atomic "
      "capability set rather than a public concept spelling");

  const bool recordedConcept =
      std::any_of(valid.semantics.database().symbols().begin(),
                  valid.semantics.database().symbols().end(),
                  [](const lang::SymbolRecord &symbol) {
                    return symbol.kind == lang::SymbolKind::Concept &&
                           symbol.qualifiedName == "signed_arithmetic";
                  });
  expect(recordedConcept,
         "source concepts should be compiler-owned symbols for tooling");

  const lang::SourceUnitId sourceUnit = valid.sourceGraph.entryUnit();
  const std::size_t constrainedUse =
      source.find("signed_arithmetic T", source.find("twice"));
  const std::size_t conceptDeclaration = source.find("signed_arithmetic<T>");
  const lang::LanguageQueries queries;
  const std::optional<lang::HoverInfo> conceptHover =
      queries.hover(valid, sourceUnit, constrainedUse + 1);
  const std::optional<lang::DefinitionInfo> conceptDefinition =
      queries.definition(valid, sourceUnit, constrainedUse + 1);
  expect(conceptHover &&
             conceptHover->signature == "concept signed_arithmetic" &&
             conceptDefinition &&
             conceptDefinition->target.start == conceptDeclaration,
         "concept uses should expose semantic hover and definition identity");

  lang::CppEmitter emitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                           nullptr, &valid.semantics);
  const std::string generated = emitter.emit(valid.program);
  expect(generated.find("concept arithmetic_value") == std::string::npos &&
             generated.find("concept signed_arithmetic") == std::string::npos &&
             generated.find("template <typename T>") != std::string::npos,
         "concepts should remain frontend validity metadata and not delegate "
         "constraint checking to the C++ backend");

  lang::FrontendResult concreteFailure =
      lang::Frontend().analyze("source-concept-failure.gti", R"(
concept signed_value<T> = std::signed_numeric<T> && std::copyable<T>;
T require_signed<signed_value T>(T value) { return value; }
int main() {
  uint64_t value = require_signed(uint64_t(1));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!concreteFailure.canGenerateCode() &&
             hasDiagnostic(concreteFailure.diagnostics,
                           "does not satisfy generic constraint "
                           "'signed_value'"),
         "concrete arguments should be checked against composed concepts with "
         "the source concept name in diagnostics");

  lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-source-concepts.gti", R"(
concept broken<T> = missing<T>;
concept first<T> = second<T>;
concept second<T> = first<T>;
concept wrong<T> = std::copyable<U>;
concept duplicate<T> = std::copyable<T>;
concept duplicate<T> = std::movable<T>;
concept leaked<T> = gti_internal::numeric_capability<T>;
@compiler_constraint("numeric") concept forged<T>;
T direct<gti_internal::numeric_capability T>(T value) { return value; }
int main() { return 0; }
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2049") &&
             hasDiagnostic(invalid.diagnostics, "Unknown concept 'missing'") &&
             hasDiagnostic(invalid.diagnostics,
                           "Concept definition cycle involving") &&
             hasDiagnostic(invalid.diagnostics,
                           "must apply to the declaration's type parameter") &&
             hasDiagnostic(invalid.diagnostics,
                           "Duplicate declaration of 'duplicate'") &&
             hasDiagnostic(invalid.diagnostics,
                           "is private to the standard prelude") &&
             hasDiagnostic(invalid.diagnostics,
                           "@compiler_constraint is reserved"),
         "concept declaration diagnostics should cover unknown dependencies, "
         "cycles, parameter misuse, duplicates, and forged compiler bindings");

  lang::FrontendResult noPrelude = lang::Frontend().analyze(
      "concept-without-prelude.gti",
      "T value<std::numeric T>(T input) { return input; }");
  expect(!noPrelude.canGenerateCode() &&
             hasDiagnostic(noPrelude.diagnostics,
                           "Unknown generic concept 'std::numeric'"),
         "public standard concepts should come from the prelude rather than "
         "hard-coded std spellings");

  const std::filesystem::path visibilityRoot =
      std::filesystem::temp_directory_path() / "gti-concept-visibility";
  const std::filesystem::path visibilityEntry = visibilityRoot / "main.gti";
  const std::filesystem::path conceptsUnit = visibilityRoot / "concepts.gti";
  const std::filesystem::path wrapperUnit = visibilityRoot / "wrapper.gti";
  const auto canonical = [](const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(path).string();
  };
  const std::string conceptsSource = R"(
namespace capabilities {
concept signed_value<T> = std::signed_numeric<T>;
}
)";
  lang::FrontendResult visible = lang::Frontend().analyze(
      visibilityEntry,
      "#include \"concepts.gti\"\n"
      "namespace cap = capabilities;\n"
      "T use<cap::signed_value T>(T value) { return value; }\n"
      "int main() { return use(1) - 1; }\n",
      {standardLibraryPrelude()}, {{canonical(conceptsUnit), conceptsSource}});
  expect(visible.canGenerateCode(),
         "directly included concepts should resolve through namespace aliases");

  lang::FrontendResult hidden = lang::Frontend().analyze(
      visibilityEntry,
      "#include \"wrapper.gti\"\n"
      "T use<capabilities::signed_value T>(T value) { return value; }\n"
      "int main() { return use(1) - 1; }\n",
      {standardLibraryPrelude()},
      {{canonical(wrapperUnit), "#include \"concepts.gti\"\n"},
       {canonical(conceptsUnit), conceptsSource}});
  expect(!hidden.canGenerateCode() &&
             hasDiagnosticCode(hidden.diagnostics, "GTI-S2024") &&
             hasDiagnosticHint(hidden.diagnostics, "#include \"concepts.gti\""),
         "concept declarations should obey direct-include visibility without "
         "transitive re-export");

  lang::Lexer lexer;
  lang::Parser disjunctionParser(lexer.scan(
      "concept either<T> = std::integral<T> || std::floating_point<T>; "
      "int okay = 1;"));
  const lang::Program recovered = disjunctionParser.parse();
  expect(disjunctionParser.hadError() &&
             hasDiagnostic(disjunctionParser.errors(),
                           "support conjunction only") &&
             recovered.declarations().size() == 1,
         "concept disjunction should be rejected explicitly without losing "
         "the following declaration");

  const std::string formatted = lang::Formatter().format(
      "concept sortable<T>=std::totally_ordered<T>&&std::movable<T>;");
  expect(formatted == "concept sortable<T> = std::totally_ordered<T> && "
                      "std::movable<T>;\n" &&
             lang::Formatter().format(formatted) == formatted,
         "concept declarations should format with C++-familiar generic and "
         "conjunction spacing");
}

void testValueGenerics() {
  const std::string source = R"(
class StaticArray<T, uint64_t N> {
  T values[N] = {};

public:
  uint64_t size() { return N; }
  T first() { return this.values[0]; }
};

class WrappedArray<T, uint64_t N> {
  StaticArray<T, N> value = StaticArray<T, N>();

public:
  uint64_t size() { return this.value.size(); }
};

int main() {
  StaticArray<int, 4> four = StaticArray<int, 4>();
  StaticArray<int, 8> eight = StaticArray<int, 8>();
  WrappedArray<int, 4> wrapped = WrappedArray<int, 4>();
  if (four.size() == 4 and eight.size() == 8 and wrapped.size() == 4 and
      four.first() == 0) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult valid =
      lang::Frontend().analyze("value-generics.gti", source);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected value generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "uint64_t value parameters should support class identity and array "
         "extents");

  bool foundFour = false;
  bool foundEight = false;
  for (const lang::HirClassInstance &instance : valid.hir.classInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "StaticArray" ||
        instance.valueArguments.size() != 1 || instance.fields.empty()) {
      continue;
    }
    const lang::CompileTimeValue length = instance.valueArguments.front();
    const lang::SemanticType &storage = instance.fields.front().info.type;
    if (length.kind == lang::CompileTimeValue::UInt64 &&
        storage.kind == lang::SemanticType::Array &&
        storage.arrayLengthParameterId == 0 &&
        storage.arrayLength == length.value) {
      foundFour = foundFour || length.value == 4;
      foundEight = foundEight || length.value == 8;
    }
  }
  expect(foundFour && foundEight,
         "HIR should keep value arguments in class identity and substitute "
         "symbolic array extents");

  lang::CppEmitter emitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                           nullptr, &valid.semantics);
  const std::string generated = emitter.emit(valid.program);
  expect(generated.find(
             "template <typename T, std::uint64_t N>\nclass StaticArray") !=
                 std::string::npos &&
             generated.find("std::array<T, N> values = {}") !=
                 std::string::npos &&
             generated.find("StaticArray<T, N> value") != std::string::npos &&
             generated.find("StaticArray<std::int32_t, 4>") !=
                 std::string::npos,
         "the C++ backend should preserve mixed type and value arguments");

  lang::Lexer lexer;
  lang::Parser invalidParser(lexer.scan(R"(
class WrongType<T, int N> {};
class WrongOrder<uint64_t N, T> {};
class NeedsValue<T, uint64_t N> {};
class ImmutableValue<uint64_t N> {
public:
  void overwrite() { N = 2; }
};
uint64_t invalid_function<uint64_t N>() { return N; }
NeedsValue<int, int> wrong_value;
NeedsValue<4, 4> wrong_type;
NeedsValue<int, Missing> missing_value;
int invalid_extent[Missing] = {};
)"));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid value generic programs should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid value generic declarations and arguments should fail");
  expect(hasDiagnostic(invalidSemantic, "currently require type uint64_t") &&
             hasDiagnostic(invalidSemantic,
                           "type parameters must appear before value") &&
             hasDiagnostic(invalidSemantic,
                           "currently limited to classes and structs") &&
             hasDiagnostic(invalidSemantic,
                           "requires a uint64_t compile-time value") &&
             hasDiagnostic(invalidSemantic, "requires a type argument") &&
             hasDiagnostic(invalidSemantic,
                           "Cannot assign to compile-time value parameter") &&
             hasDiagnostic(invalidSemantic,
                           "is not an in-scope uint64_t value generic"),
         "value generic diagnostics should identify unsupported parameter "
         "types, ordering, scope, and argument kind");

  const std::string formatted = lang::Formatter().format(
      "class StaticArray<T,uint64_t N>{T values[N]={};};StaticArray<int,4> "
      "values=StaticArray<int,4>();");
  expect(formatted.find("class StaticArray<T, uint64_t N> {") !=
                 std::string::npos &&
             formatted.find("StaticArray<int, 4> values =") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "value generic syntax should format idempotently");
}

void testVariadicGenerics() {
  const std::string source = R"(
namespace std { using string_view = gti_internal::text_view; }

void consume<Args...>(Args... values) {}

void route<Rest...>(int first, Rest... rest) { consume(rest...); }
void route<Rest...>(bool first, Rest... rest) { consume(rest...); }

void relay<Args...>(Args... values) {
  consume(values...);
  consume(0, values...);
  route(1, values...);
}

T first<T, Rest...>(T value, Rest... rest) {
  relay(rest...);
  return value;
}

class Forwarder {
public:
  void send<Args...>(Args... values) {
    relay(values...);
  }
};

int main() {
  consume();
  relay(1, true, "gti");
  int inferred = first(7, false, "tail");
  int explicit_types = first<int, std::string_view>(9, "tail");
  Forwarder forwarder = Forwarder();
  forwarder.send(uint64_t(3), "method");
  if (inferred == 7 and explicit_types == 9) { return 0; }
  return 1;
}
)";

  lang::FrontendResult valid =
      lang::Frontend().analyze("variadic-generics.gti", source);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected variadic diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "final function type packs should infer and forward exactly");

  lang::CppEmitter emitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                           nullptr, &valid.semantics);
  const std::string generated = emitter.emit(valid.program);
  expect(generated.find("template <typename... Args>") != std::string::npos &&
             generated.find("Args... values") != std::string::npos &&
             generated.find("forward_pack_argument(values)...") !=
                 std::string::npos,
         "variadic declarations and forwarding should lower explicitly");
  expect(generated.find("0, gti_internal::backend::forward_pack_argument("
                        "values)...") != std::string::npos,
         "a final symbolic pack should compose with concrete pack elements");
  expect(generated.find("template <typename T, typename... Rest>") !=
                 std::string::npos &&
             generated.find("first<std::int32_t, gti_std::string_view>") !=
                 std::string::npos,
         "fixed generic arguments and explicit pack elements should lower in "
         "source order");

  const lang::FrontendResult movePack =
      lang::Frontend().analyze("move-pack.gti", R"(
class PackedValue {};

void consume_owner<Args...>(Args... values) {}

void forward_owner<Args...>(Args... values) {
  consume_owner(values...);
}

int main() {
  auto owner = std::make_unique<PackedValue>();
  forward_owner(std::move(owner));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  bool foundConcreteOwnerPack = false;
  for (const lang::HirFunctionInstance &instance :
       movePack.hir.functionInstances()) {
    foundConcreteOwnerPack =
        foundConcreteOwnerPack ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "forward_owner" &&
         instance.typeArguments.size() == 1 &&
         instance.typeArguments.front().kind == lang::SemanticType::Class);
  }
  expect(movePack.canGenerateCode() && foundConcreteOwnerPack,
         "a concrete move-only pack should be forwarded once and retained in "
         "typed HIR");

  const lang::FrontendResult reusedMovePack =
      lang::Frontend().analyze("reused-move-pack.gti", R"(
class PackedValue {};

void consume_owner<Args...>(Args... values) {}

void forward_owner_twice<Args...>(Args... values) {
  consume_owner(values...);
  consume_owner(values...);
}

int main() {
  auto owner = std::make_unique<PackedValue>();
  forward_owner_twice(std::move(owner));
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!reusedMovePack.canGenerateCode() &&
             hasDiagnostic(reusedMovePack.diagnostics,
                           "Value 'values' has already been moved") &&
             hasRelatedDiagnostic(reusedMovePack.diagnostics,
                                  "Concrete generic instance requested here"),
         "a move-only parameter pack should be consumed as a whole and cannot "
         "be forwarded twice");

  lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-variadic-generics.gti", R"(
class Packed<T...> {};

void fixed(int value) {}
void nonfinal_generic<Args..., T>(Args... values, T tail) {}
void missing_parameter<Args...>() {}
void wrong_type<Args...>(int... values) {}
void mutable_pack<Args...>(mut Args... values) {}
void reference_pack<Args...>(Args&... values) {}
void direct_use<Args...>(Args... values) { values; }
void local_pack<Args...>(Args... values) { Args local; }
void bad_forward<Args...>(Args... values) { fixed(values...); }
void not_a_pack(int value) { consume(value...); }
)");
  expect(!invalid.canGenerateCode(),
         "invalid variadic declarations and expansions should be rejected");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "type packs are currently limited to functions") &&
          hasDiagnostic(invalid.diagnostics,
                        "generic type pack must be the final") &&
          hasDiagnostic(invalid.diagnostics,
                        "must be bound by a final parameter pack") &&
          hasDiagnostic(invalid.diagnostics, "parameter pack type must name") &&
          hasDiagnostic(invalid.diagnostics, "Parameter packs are immutable") &&
          hasDiagnostic(invalid.diagnostics, "only by-value parameter packs") &&
          hasDiagnostic(invalid.diagnostics,
                        "must be expanded as the final call argument") &&
          hasDiagnostic(
              invalid.diagnostics,
              "can only appear in its matching final parameter-pack") &&
          hasDiagnostic(invalid.diagnostics,
                        "only be forwarded to another variadic") &&
          hasDiagnostic(invalid.diagnostics, "is not a parameter pack"),
      "variadic diagnostics should explain every confined first-wave rule");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan(R"(
void consume<Args...>(Args... values) {}
void bad<Args...>(Args... values) { consume(values..., 1); }
int okay = 1;
)",
                                    "malformed-variadic.gti"));
  const lang::Program recovered = malformed.parse();
  const bool recoveredOkay = std::any_of(
      recovered.declarations().begin(), recovered.declarations().end(),
      [](const lang::StmtPtr &declaration) {
        const auto *variable =
            dynamic_cast<const lang::VariableDecl *>(declaration.get());
        return variable != nullptr && variable->name().lexeme == "okay";
      });
  expect(malformed.hadError() && recoveredOkay,
         "parser recovery should continue after a non-final pack expansion");

  const std::string formatted = lang::Formatter().format(
      "void relay<Args...>(Args... values){consume(values...);}");
  expect(formatted == "void relay<Args...>(Args... values) {\n"
                      "  consume(values...);\n"
                      "}\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should preserve compact, idempotent pack syntax");
}

void testExactFunctionOverloadsAndConversions() {
  const std::string source = R"(
namespace math {
uint64_t pow(uint64_t base, uint64_t exponent) { return base * exponent; }
float pow(float base, float exponent) { return base * exponent; }
}

struct Selector<T, U> {
  T apply(T value) { return value; }
  U apply(U value) { return value; }
};

struct Receiver {
  mut int value = 0;
  int inspect() { return this.value; }
  int inspect() mut {
    this.value += 1;
    return this.value;
  }
};

int main() {
  uint64_t base = uint64_t(2);
  uint64_t exponent = uint64_t(8);
  uint64_t whole = math::pow(base, exponent);
  float decimal = math::pow(2.0, 8.0);
  Selector<int, float> selector = Selector<int, float>();
  int selected = selector.apply(2);
  float selected_decimal = selector.apply(2.0);
  Receiver read_only = Receiver();
  mut Receiver writable = Receiver();
  int read_result = read_only.inspect();
  int write_result = writable.inspect();
  return int(whole) + selected + read_result + write_result;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("overloads.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected overload diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "exact namespace and method overloads should validate");
  expect(
      frontend.semantics.functionCount() == 7 &&
          frontend.semantics.resolvedCallCount() == 6,
      "semantic analysis should retain function identities and selected calls");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto selectedReceiver = [&](std::size_t statementIndex) {
    if (main == nullptr ||
        statementIndex >= main->body()->statements().size()) {
      return static_cast<const lang::ResolvedCallInfo *>(nullptr);
    }
    const auto *variable = dynamic_cast<const lang::VariableDecl *>(
        main->body()->statements()[statementIndex].get());
    const auto *call =
        variable == nullptr
            ? nullptr
            : dynamic_cast<const lang::Call *>(variable->initializer().get());
    return call == nullptr ? nullptr : frontend.semantics.findCall(*call);
  };
  const lang::ResolvedCallInfo *readCall = selectedReceiver(9);
  const lang::ResolvedCallInfo *writeCall = selectedReceiver(10);
  expect(readCall != nullptr && readCall->declaration != nullptr &&
             readCall->declaration->receiverMutability() ==
                 lang::ReceiverMutability::ReadOnly &&
             writeCall != nullptr && writeCall->declaration != nullptr &&
             writeCall->declaration->receiverMutability() ==
                 lang::ReceiverMutability::Mutable,
         "method overload selection should choose the receiver-qualified "
         "declaration exactly");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("__gti_fn_1_pow") != std::string::npos &&
             artifact.contents.find("__gti_fn_2_pow") != std::string::npos &&
             artifact.contents.find("math::__gti_fn_1_pow(base, exponent)") !=
                 std::string::npos &&
             artifact.contents.find(".__gti_fn_3_apply(2)") !=
                 std::string::npos,
         "the C++ backend should emit the function identity selected by GTI");
  expect(artifact.contents.find("numeric_cast<std::uint64_t>(2)") !=
                 std::string::npos &&
             artifact.contents.find("numeric_cast<std::int32_t>(whole)") !=
                 std::string::npos &&
             artifact.contents.find("2.00000000F") != std::string::npos,
         "explicit conversions and float literals should lower to matching C++ "
         "types");

  lang::Lexer lexer;
  auto invalidTokens = lexer.scan(R"(
int same(int value) { return value; }
float same(int value) { return float(value); }

int mutate(int value) { return value; }
int mutate(mut int value) { return value; }

T echo<T>(T value) { return value; }
U echo<U>(U value) { return value; }

struct MutableOnly {
  int inspect() mut { return 1; }
};

int choose(int value) { return value; }
T choose<T>(T value) { return value; }

uint64_t width(uint64_t value) { return value; }
float width(float value) { return value; }
float only_float(float value) { return value; }
void function_value() {}

int main(int value) { return value; }
int main() {
  MutableOnly read_only = MutableOnly();
  int invalid_receiver = read_only.inspect();
  function_value;
  float mismatch = width(1);
  int ambiguous = choose(1);
  float inexact = only_float(1);
  uint8_t too_small = uint8_t(300);
  int not_numeric = int("text");
  return 0;
}
)");
  expect(!lexer.hadError(), "invalid overload cases should lex");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid overload cases should remain syntactically valid");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid overloads and conversions should fail semantics");
  expect(countDiagnosticCode(invalidSemantic, "GTI-S2011") == 4,
         "return type, by-value mutability, and generic spelling should not "
         "create distinct overload signatures");
  expect(
      hasDiagnostic(invalidSemantic, "main entry point cannot be overloaded"),
      "the native entry point should remain a unique function");
  expect(hasDiagnostic(invalidSemantic, "No overload of 'width'") &&
             hasDiagnostic(invalidSemantic, "argument types (int32_t)"),
         "overload lookup should reject calls without an exact candidate");
  expect(hasDiagnostic(invalidSemantic, "ambiguous") &&
             hasDiagnostic(invalidSemantic, "exactly match"),
         "generic and concrete exact matches should be diagnosed as ambiguous");
  expect(hasDiagnostic(invalidSemantic, "parameter requires 'float'"),
         "single functions should also require exact argument types");
  expect(hasDiagnostic(invalidSemantic,
                       "Mutable method requires a mutable receiver"),
         "receiver-qualified overloads should reject mutable-only methods on "
         "read-only objects");
  expect(
      hasDiagnostic(invalidSemantic, "function values are not supported"),
      "function overload sets should not escape unresolved into the backend");
  expect(hasDiagnostic(invalidSemantic, "outside the range of 'uint8_t'") &&
             hasDiagnostic(invalidSemantic,
                           "numeric conversions require a numeric value"),
         "explicit conversions should reject invalid literals and domains");

  const std::string formatted = lang::Formatter().format(
      "uint64_t pow(uint64_t base,uint64_t exponent){return base*exponent;}int "
      "main(){uint64_t value=uint64_t(2);return int(value);}");
  expect(formatted.find("uint64_t value = uint64_t(2);") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "explicit conversion syntax should format like a C++ functional cast");
}

void testFixedArrays() {
  const std::string source = R"(
uint32_t video[64 * 32] = {};

int extent(int values[2]) { return 2; }
int extent(int values[3]) { return 3; }
int first(int values[3]) { return values[0]; }

struct Buffers {
public:
  mut int samples[3] = {1, 2, 3};

  void bump() mut { this.samples[1] += 4; }
  uint64_t count() { return this.samples.size(); }
};

int main() {
  mut int buffer[2 + 3] = {1, 2, 3, 4, 5};
  buffer[2] = 10;
  buffer[1]++;
  int copy[5] = buffer;
  int matrix[0x2][0b11] = {{1, 2, 3}, {4, 5, 6}};
  mut Buffers buffers = Buffers();
  buffers.bump();
  uint64_t count = buffer.size();
  if (count == 5 and first(matrix[1]) == 4 and extent(matrix[0]) == 3 and
      buffers.count() == 3 and copy[4] == 5) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("arrays.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected fixed array diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays should support value semantics and checked indexing");

  const auto *main = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().back().get());
  const auto *buffer = main == nullptr
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().front().get());
  const lang::BindingInfo *binding =
      buffer == nullptr ? nullptr : frontend.semantics.findBinding(*buffer);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Array &&
             binding->type.arrayLength == 5 &&
             binding->type.arguments.size() == 1 &&
             binding->type.arguments[0] == lang::SemanticType::Int32 &&
             binding->access == lang::AccessMode::Mutable,
         "fixed array bindings should retain length, element type, and access");

  const lang::SemanticType moveOnlyArray = lang::SemanticType::arrayOf(
      lang::SemanticType::uniqueOwnerOf(lang::SemanticType::Int32), 2);
  expect(!lang::semanticTraits(moveOnlyArray).copyable &&
             lang::semanticTraits(moveOnlyArray).movable,
         "fixed arrays should inherit element copy and move traits");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("std::array<std::uint32_t, 2048> video = {}") !=
                 std::string::npos &&
             artifact.contents.find(
                 "std::array<std::int32_t, 5> buffer = {1, 2, 3, 4, 5}") !=
                 std::string::npos &&
             artifact.contents.find(
                 "std::array<std::array<std::int32_t, 3>, 2> matrix") !=
                 std::string::npos &&
             artifact.contents.find("backend::array_at") != std::string::npos &&
             artifact.contents.find("static_cast<std::uint64_t>") !=
                 std::string::npos,
         "the C++ backend should preserve array values and checked operations");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-arrays.gti", R"(
struct NoDefault {
  int stored;
  NoDefault(int value) : stored(value) {}
};

class ReadOnlyArrayReceiver {
  mut int slots[1] = {0};

public:
  void write() { slots[0] = 1; }
};

int overflow_extent[18446744073709551615 + 1] = {};
int negative_extent[1 - 2] = {};
int zero_divisor_extent[4 / 0] = {};

int choose(int values[2]) { return 2; }
int choose(int values[3]) { return 3; }

int main() {
  int wrong_count[3] = {1, 2};
  int immutable[1] = {1};
  immutable[0] = 2;
  int values[2] = {1, 2};
  int bad_index = values[1.5];
  int negative = values[-1];
  int past_end = values[2];
  mut int missing[2];
  NoDefault objects[1] = {};
  int other[4] = {1, 2, 3, 4};
  int mismatch = choose(other);
  uint64_t hidden = other.length();
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid fixed array operations should fail semantic analysis");
  expect(hasDiagnostic(invalid.diagnostics, "requires exactly 3") &&
             hasDiagnostic(invalid.diagnostics, "immutable fixed array") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot modify field 'slots' through a read-only "
                           "receiver") &&
             hasDiagnosticHint(invalid.diagnostics, "trailing 'mut'") &&
             hasRelatedDiagnostic(invalid.diagnostics,
                                  "is declared mutable here") &&
             hasDiagnostic(invalid.diagnostics, "index must have an integer") &&
             hasDiagnostic(invalid.diagnostics, "valid range [0, 2)") &&
             hasDiagnostic(invalid.diagnostics, "require an initializer") &&
             hasDiagnostic(invalid.diagnostics, "default-initializable") &&
             hasDiagnostic(invalid.diagnostics, "No overload of 'choose'") &&
             hasDiagnostic(invalid.diagnostics, "Unknown fixed array member") &&
             hasDiagnostic(invalid.diagnostics,
                           "extent arithmetic overflows uint64_t") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot produce a negative value") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot divide or take modulo by zero"),
         "fixed array diagnostics should explain extent, access, and bounds");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan("int broken[] = {}; int recovered = 1;"));
  const lang::Program recovered = malformed.parse();
  expect(malformed.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after a missing array extent");

  const std::string formatted = lang::Formatter().format(
      "mut int buffer[1+2]={1,2,3};int matrix[0x2][0b10]={{1,2},{3,4}};");
  expect(formatted == "mut int buffer[1 + 2] = {1, 2, 3};\n"
                      "int matrix[0x2][0b10] = {{1, 2}, {3, 4}};\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should preserve compact C++-style array declarations");
  const std::string constructorFormatted = lang::Formatter().format(
      "class Pair<T>{T values[2];public:Pair(T left,T right):"
      "values({left,right}){}};");
  expect(constructorFormatted.find("values({left, right}) {}") !=
             std::string::npos,
         "formatter should keep array constructor initializers compact");
}

void testLocalTypeInference() {
  const std::string source = R"(
class Box {
  int value = 0;

public:
  Box(int value) : value(value) {}
  int read() { return this.value; }
};

T preserve<T>(T value) {
  auto inferred = value;
  return inferred;
}

int main() {
  auto integer = 1;
  mut auto running = preserve(integer);
  running += 2;
  auto truth = running == 3;
  auto text = "gti";
  auto box = Box(running);
  int values[2] = {running, 4};
  auto copied_values = values;
  auto empty = nullptr;
  for (mut auto index = 0; index < 2; index++) {
    running += copied_values[index];
  }
  if (truth and text == "gti" and empty == nullptr and box.read() == 3 and
      running == 10) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("auto-inference.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected auto diagnostic: " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "initialized local auto bindings should pass the frontend");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *integer = main == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::VariableDecl *>(
                                  main->body()->statements().front().get());
  const auto *running = main == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::VariableDecl *>(
                                  main->body()->statements().at(1).get());
  const lang::BindingInfo *integerBinding =
      integer == nullptr ? nullptr : frontend.semantics.findBinding(*integer);
  const lang::BindingInfo *runningBinding =
      running == nullptr ? nullptr : frontend.semantics.findBinding(*running);
  expect(integerBinding != nullptr &&
             integerBinding->type == lang::SemanticType::Int32 &&
             integerBinding->access == lang::AccessMode::ReadOnly &&
             runningBinding != nullptr &&
             runningBinding->type == lang::SemanticType::Int32 &&
             runningBinding->access == lang::AccessMode::Mutable,
         "semantic bindings should retain concrete inferred types and access");

  const lang::HirFunctionInstance *preserveInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "preserve" &&
        instance.typeArguments ==
            std::vector<lang::SemanticType>{lang::SemanticType::Int32}) {
      preserveInstance = &instance;
      break;
    }
  }
  expect(preserveInstance != nullptr &&
             preserveInstance->body.bindings.size() == 2 &&
             preserveInstance->body.bindings.back().info.type ==
                 lang::SemanticType::Int32,
         "instantiated HIR should retain the concrete type of an inferred "
         "generic local");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("const auto integer = 1") != std::string::npos &&
          artifact.contents.find("auto running = __gti_fn_") !=
              std::string::npos &&
          artifact.contents.find("for (auto index = 0;") != std::string::npos,
      "the C++ backend should preserve inferred immutable, mutable, and "
      "loop bindings");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-auto.gti", R"(
auto global = 1;

class InvalidField {
  auto value = 1;
};

auto inferred_return() { return 1; }
int inferred_parameter(auto value) { return 0; }
void perform() {}

int main() {
  mut int value = 1;
  auto fixed = value;
  fixed = 2;
  auto missing;
  auto& reference = value;
  auto array[2] = {1, 2};
  auto values = {1, 2};
  auto no_value = perform();
  return 0;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2028") &&
             hasDiagnostic(invalid.diagnostics, "limited to local bindings") &&
             hasDiagnostic(invalid.diagnostics, "initialized local binding") &&
             hasDiagnostic(invalid.diagnostics, "requires an initializer") &&
             hasDiagnostic(invalid.diagnostics,
                           "limited to range-for element bindings") &&
             hasDiagnostic(invalid.diagnostics, "array extents") &&
             hasDiagnostic(invalid.diagnostics,
                           "requires a fixed array type from its context") &&
             hasDiagnostic(invalid.diagnostics, "complete value type") &&
             hasDiagnostic(invalid.diagnostics, "immutable binding 'fixed'"),
         "auto diagnostics should explain every unsupported declaration and "
         "inference context");

  const lang::FrontendResult moveOnly =
      lang::Frontend().analyze("move-only-auto.gti", R"(
class Value {
public:
  int number = 1;
};

int main() {
  auto owner = std::make_unique<Value>();
  auto copied = owner;
  return copied->number;
}
)",
                               {standardLibraryPrelude()});
  expect(!moveOnly.canGenerateCode() &&
             hasDiagnosticCode(moveOnly.diagnostics, "GTI-S2003") &&
             hasDiagnostic(moveOnly.diagnostics,
                           "Cannot initialize inferred binding 'copied'") &&
             hasDiagnosticHint(moveOnly.diagnostics, "std::move(owner)"),
         "auto initialization should reject move-only copies in GTI semantics");

  const lang::FrontendResult movedOwner =
      lang::Frontend().analyze("moved-auto.gti", R"(
class Value {
public:
  int number = 1;
};

int main() {
  auto owner = std::make_unique<Value>();
  auto moved = std::move(owner);
  return moved->number - 1;
}
)",
                               {standardLibraryPrelude()});
  const lang::FunctionDecl *movedMain =
      findTopLevelFunction(movedOwner.program, "main");
  const auto *moved = movedMain == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                movedMain->body()->statements().at(1).get());
  const lang::BindingInfo *movedBinding =
      moved == nullptr ? nullptr : movedOwner.semantics.findBinding(*moved);
  expect(movedOwner.canGenerateCode() && movedBinding != nullptr &&
             movedBinding->traits.ownership == lang::OwnershipKind::Unique &&
             !movedBinding->traits.copyable && movedBinding->traits.movable,
         "auto should retain ownership traits after an explicit move");

  const std::string formatted = lang::Formatter().format(
      "int main(){auto fixed=1;mut auto changing=fixed;for(mut auto i=0;i<2;"
      "i++){changing+=i;}return changing;}");
  expect(formatted.find("auto fixed = 1;") != std::string::npos &&
             formatted.find("mut auto changing = fixed;") !=
                 std::string::npos &&
             formatted.find("for (mut auto i = 0; i < 2; i++) {") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "local auto declarations should receive stable C++-style formatting");
}

void testStructuredBindings() {
  const std::string source = R"(
struct Pair<T, U> {
  T first;
  U second;

  Pair(T first, U second) : first(first), second(second) {}
  ~Pair() {}
};

int main() {
  int values[2] = {2, 3};
  auto [left, right] = values;
  auto [number, enabled] = Pair<int, bool>(4, true);
  mut int result = 1;
  if (left == 2 && right == 3 && number == 4 && enabled) {
    result = 0;
  }
  return result;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("structured-bindings.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected structured-binding diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays and public direct aggregate fields should support "
         "structured bindings");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *arrayBinding =
      main == nullptr ? nullptr
                      : dynamic_cast<const lang::StructuredBindingDecl *>(
                            main->body()->statements().at(1).get());
  const auto *pairBinding =
      main == nullptr ? nullptr
                      : dynamic_cast<const lang::StructuredBindingDecl *>(
                            main->body()->statements().at(2).get());
  expect(arrayBinding != nullptr && pairBinding != nullptr &&
             arrayBinding->bindings().size() == 2 &&
             pairBinding->bindings().size() == 2,
         "the AST should retain ordered structured-binding declarations");

  const lang::StructuredBindingInfo *arrayInfo =
      arrayBinding == nullptr
          ? nullptr
          : frontend.semantics.findStructuredBinding(*arrayBinding);
  const lang::StructuredBindingInfo *pairInfo =
      pairBinding == nullptr
          ? nullptr
          : frontend.semantics.findStructuredBinding(*pairBinding);
  expect(arrayInfo != nullptr && arrayInfo->elements.size() == 2 &&
             arrayInfo->elements[0].projection ==
                 lang::StructuredBindingProjectionKind::ArrayElement &&
             arrayInfo->elements[0].index == 0 &&
             arrayInfo->elements[1].index == 1 &&
             arrayInfo->elements[0].binding.type == lang::SemanticType::Int32 &&
             arrayInfo->elements[0].binding.access ==
                 lang::AccessMode::ReadOnly,
         "array decomposition should record exact immutable element "
         "projections");
  expect(pairInfo != nullptr &&
             pairInfo->source.type.kind == lang::SemanticType::Class &&
             pairInfo->elements.size() == 2 &&
             pairInfo->elements[0].projection ==
                 lang::StructuredBindingProjectionKind::Field &&
             pairInfo->elements[0].field != 0 &&
             pairInfo->elements[1].field != 0 &&
             pairInfo->elements[0].binding.type == lang::SemanticType::Int32 &&
             pairInfo->elements[1].binding.type == lang::SemanticType::Bool &&
             pairInfo->elements[0].binding.symbol != 0 &&
             pairInfo->elements[1].binding.symbol != 0,
         "aggregate decomposition should substitute field types and retain "
         "source-visible symbols");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const std::size_t leftDeclaration = source.find("left");
  const std::size_t leftUse = source.rfind("left");
  const std::optional<lang::HoverInfo> leftHover =
      queries.hover(frontend, unit, leftDeclaration + 1);
  const std::optional<lang::DefinitionInfo> leftDefinition =
      queries.definition(frontend, unit, leftUse + 1);
  expect(leftHover && leftHover->signature == "int32_t left" &&
             leftDefinition && leftDefinition->target.start == leftDeclaration,
         "shared semantic queries should expose hover and definition for "
         "each decomposed name");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == main) {
      mainInstance = &instance;
      break;
    }
  }
  const lang::HirStatement *arrayStatement =
      mainInstance == nullptr || mainInstance->body.roots.size() < 3
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[1]);
  const lang::HirStatement *pairStatement =
      mainInstance == nullptr || mainInstance->body.roots.size() < 3
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[2]);
  expect(arrayStatement != nullptr && pairStatement != nullptr &&
             arrayStatement->kind ==
                 lang::HirStatementKind::StructuredBinding &&
             pairStatement->kind == lang::HirStatementKind::StructuredBinding &&
             arrayStatement->binding && pairStatement->binding &&
             arrayStatement->structuredBindings.size() == 2 &&
             pairStatement->structuredBindings.size() == 2 &&
             arrayStatement->structuredBindings[0].index &&
             pairStatement->structuredBindings[0].field != 0,
         "HIR should retain one hidden owner and typed array/field "
         "projections");

  const lang::MirFunctionInstance *mirMain =
      mainInstance == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(mainInstance->id);
  const auto findSourcePlace =
      [&](const lang::HirStatement *statement) -> const lang::MirPlace * {
    if (mirMain == nullptr || statement == nullptr || !statement->binding) {
      return nullptr;
    }
    const auto found =
        std::find_if(mirMain->body.places.begin(), mirMain->body.places.end(),
                     [&](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::Binding &&
                              place.binding == *statement->binding &&
                              place.projections.empty();
                     });
    return found == mirMain->body.places.end() ? nullptr : &*found;
  };
  const lang::MirPlace *arraySource = findSourcePlace(arrayStatement);
  const lang::MirPlace *pairSource = findSourcePlace(pairStatement);
  std::size_t arrayInitializations = 0;
  std::size_t pairInitializations = 0;
  std::size_t pairDrops = 0;
  if (mirMain != nullptr) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        arrayInitializations +=
            arrayStatement != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.hirStatement == arrayStatement->id
                ? 1
                : 0;
        pairInitializations +=
            pairStatement != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.hirStatement == pairStatement->id
                ? 1
                : 0;
        pairDrops +=
            pairSource != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Drop &&
                    instruction.destination == pairSource->id
                ? 1
                : 0;
      }
    }
  }
  const bool hasArrayProjection =
      mirMain != nullptr && arrayStatement != nullptr &&
      std::any_of(mirMain->body.places.begin(), mirMain->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == *arrayStatement->binding &&
                           std::any_of(
                               place.projections.begin(),
                               place.projections.end(),
                               [](const lang::MirPlaceProjection &projection) {
                                 return projection.kind ==
                                        lang::MirProjectionKind::Index;
                               });
                  });
  const bool hasFieldProjection =
      mirMain != nullptr && pairStatement != nullptr &&
      std::any_of(mirMain->body.places.begin(), mirMain->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == *pairStatement->binding &&
                           std::any_of(
                               place.projections.begin(),
                               place.projections.end(),
                               [](const lang::MirPlaceProjection &projection) {
                                 return projection.kind ==
                                        lang::MirProjectionKind::Field;
                               });
                  });
  expect(arraySource != nullptr && pairSource != nullptr &&
             arrayInitializations == 1 && pairInitializations == 1 &&
             pairDrops == 1 && hasArrayProjection && hasFieldProjection,
         "MIR should initialize and destroy each hidden owner once while "
         "visible names remain projected places");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("const auto [left, right] = values;") !=
                 std::string::npos &&
             artifact.contents.find(
                 "const auto [number, enabled] = Pair<std::int32_t, bool>(4, "
                 "true);") != std::string::npos,
         "the C++ backend should represent validated structured bindings "
         "without knowing aggregate names");

  const std::string formatted = lang::Formatter().format(
      "int main(){int values[2]={1,2};auto[left,right]=values;return left;}");
  expect(formatted.find("auto [left, right] = values;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "structured bindings should receive stable C++-style formatting");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-structured-bindings.gti", R"(
class Hidden {
  int secret = 1;
};

struct Base {
  int base = 1;
};

struct Derived : public Base {
  int own = 2;
};

struct MoveOnly {
  int value = 1;

  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};

int main() {
  int values[2] = {1, 2};
  auto [too, many, names] = values;
  auto [scalar] = 1;
  Hidden hidden{};
  auto [secret] = hidden;
  Derived derived{};
  auto [base, own] = derived;
  auto [same, same] = values;
  auto [first, second] = values;
  first = 3;
  auto moved = std::move(second);
  MoveOnly owner{};
  auto [copied] = owner;
  auto [untyped_left, untyped_right] = {1, 2};
  return 0;
}
)",
                               {standardLibraryPrelude()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2048") &&
             hasDiagnostic(invalid.diagnostics,
                           "declares 3 names but the source provides 2") &&
             hasDiagnostic(invalid.diagnostics,
                           "require a fixed array or a class/struct") &&
             hasDiagnostic(invalid.diagnostics,
                           "every direct instance field to be public") &&
             hasDiagnostic(invalid.diagnostics,
                           "do not decompose inherited storage") &&
             hasDiagnostic(invalid.diagnostics, "Duplicate declaration") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot assign to immutable binding 'first'") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot partially move structured binding") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot initialize structured binding by copying") &&
             hasDiagnostic(invalid.diagnostics,
                           "requires a fixed array type from its context"),
         "structured-binding diagnostics should reject ambiguous ownership, "
         "shape, access, mutation, and untyped initialization");

  lang::Lexer lexer;
  const auto expectSyntaxError = [&](std::string input,
                                     std::string_view message) {
    lang::Parser parser(lexer.scan(input));
    (void)parser.parse();
    expect(parser.hadError() &&
               hasDiagnostic(parser.errors(), std::string(message)),
           "structured-binding syntax should diagnose " + std::string(message));
  };
  expectSyntaxError("auto [left, right] = value;", "local declarations");
  expectSyntaxError("class Bad { auto [left, right] = value; };",
                    "cannot declare class or struct fields");
  expectSyntaxError("int main() { mut auto [left, right] = values; return 0; }",
                    "Structured bindings are immutable");
  expectSyntaxError("int main() { auto& [left, right] = values; return 0; }",
                    "Reference structured bindings are not supported");
  expectSyntaxError(
      "int main() { for (auto [left, right] = values; true; left++) {} "
      "return 0; }",
      "not supported in a for-loop initializer");
  lang::Parser autoArrayParser(
      lexer.scan("int main() { auto[2] values = {1, 2}; return 0; }"));
  (void)autoArrayParser.parse();
  expect(!autoArrayParser.hadError(),
         "structured-binding lookahead should preserve typed array syntax for "
         "semantic auto diagnostics");
  lang::Parser recoveryParser(lexer.scan(
      "int main() { int broken = 1 auto [left, right] = values; return left; "
      "}"));
  const lang::Program recovered = recoveryParser.parse();
  const lang::FunctionDecl *recoveredMain =
      findTopLevelFunction(recovered, "main");
  expect(recoveryParser.hadError() && recoveredMain != nullptr &&
             !recoveredMain->body()->statements().empty() &&
             dynamic_cast<const lang::StructuredBindingDecl *>(
                 recoveredMain->body()->statements().front().get()) != nullptr,
         "parser recovery should resume at a structured binding after a "
         "missing semicolon");
}

void testLambdas() {
  const std::string source = R"(
int main() {
  int offset = 3;
  auto add = [offset](int value) -> int { return offset + value; };
  auto copied = add;
  auto compose = [copied](int value) -> int {
    return copied(value) + 1;
  };
  int direct = [](int value) -> int { return value * 2; }(4);
  auto inferred = direct;
  return compose(inferred) - 12;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("lambdas.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected lambda diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "explicit value-capture lambdas should pass the frontend");
  expect(frontend.semantics.lambdaCount() == 3 &&
             frontend.hir.lambdaInstances().size() == 3,
         "semantic analysis and HIR should retain each typed closure");

  bool foundCapturedCall = false;
  for (const lang::HirLambda &lambda : frontend.hir.lambdaInstances()) {
    expect(lambda.source != nullptr &&
               lambda.returnType == lang::SemanticType::Int32 &&
               lambda.parameterTypes.size() == 1 &&
               lambda.parameterTypes.front() == lang::SemanticType::Int32,
           "HIR lambdas should retain concrete signatures and source identity");
    for (const lang::HirValue &value : lambda.body.values) {
      foundCapturedCall =
          foundCapturedCall || (value.kind == lang::HirValueKind::Call &&
                                value.lambdaTarget.has_value());
    }
  }
  expect(foundCapturedCall,
         "calls through captured lambda values should resolve in typed HIR");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find(
             "const auto add = [offset](const std::int32_t value) -> "
             "std::int32_t {") != std::string::npos &&
             artifact.contents.find("const auto copied = add") !=
                 std::string::npos &&
             artifact.contents.find("[](const std::int32_t value) -> "
                                    "std::int32_t {") != std::string::npos,
         "the C++ backend should emit immutable closure objects and value "
         "captures");

  const lang::FrontendResult generic =
      lang::Frontend().analyze("generic-lambdas.gti", R"(
T add_with<T>(T offset, T value) {
  auto add = [offset](T input) -> T { return input; };
  return add(value);
}

int main() {
  int signed_result = add_with(1, 2);
  uint64_t unsigned_result = add_with(uint64_t(1), uint64_t(2));
  return signed_result - int(unsigned_result);
}
)");
  expect(generic.canGenerateCode() && generic.hir.lambdaInstances().size() == 2,
         "generic function instances should receive distinct concrete "
         "closure bodies");
  bool foundSigned = false;
  bool foundUnsigned = false;
  for (const lang::HirLambda &lambda : generic.hir.lambdaInstances()) {
    foundSigned = foundSigned ||
                  (lambda.returnType == lang::SemanticType::Int32 &&
                   lambda.parameterTypes.front() == lang::SemanticType::Int32);
    foundUnsigned =
        foundUnsigned ||
        (lambda.returnType == lang::SemanticType::UInt64 &&
         lambda.parameterTypes.front() == lang::SemanticType::UInt64);
  }
  expect(foundSigned && foundUnsigned,
         "HIR should substitute lambda signatures per generic instance");

  const lang::FrontendResult expectedLambda =
      lang::Frontend().analyze("expected-lambda.gti", R"(
namespace std { using string_view = gti_internal::text_view; }

int main() {
  auto calculate = [](bool fail) -> expected<int, std::string_view> {
    if (fail) { return unexpected("failed"); }
    return 1;
  };
  expected<int, std::string_view> result = calculate(false);
  return result.value_or(0) - 1;
}
)");
  const std::string cpp20 =
      expectedLambda.canGenerateCode()
          ? lang::CppEmitter(lang::CppStandard::Cpp20, lang::TargetInfo::host(),
                             nullptr, &expectedLambda.semantics,
                             &expectedLambda.hir)
                .emit(expectedLambda.program)
          : std::string{};
  expect(expectedLambda.canGenerateCode() &&
             cpp20.find("#include <nonstd/expected.hpp>") != std::string::npos,
         "backend support includes should account for types nested in lambda "
         "signatures");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-lambdas.gti", R"(
T invoke<T>(T value) { return value; }
auto global_value = 1;

class ThisCapture {
  int value = 1;

public:
  int read() {
    auto invalid = []() -> int { return this.value; };
    return invalid();
  }
};

int main() {
  mut int local = 1;
  int& alias = local;
  auto missing_initializer;
  auto inferred_reference = alias;
  auto missing = [](int value) -> int { return local + value; };
  auto duplicate = [local, local]() -> int { return local; };
  auto referenced = [alias]() -> int { return alias; };
  mut auto mutable_lambda = [local](int value) -> int {
    local += value;
    return local;
  };
  auto exact = [](int value) -> int { return value; };
  int wrong_type = exact(true);
  int escaped = invoke(missing);
  return escaped;
}
)");
  expect(!invalid.canGenerateCode(),
         "implicit, duplicate, reference, mutable, and escaping lambdas "
         "should fail");
  expect(hasDiagnostic(invalid.diagnostics, "not captured") &&
             hasDiagnostic(invalid.diagnostics, "listed more than once") &&
             hasDiagnostic(invalid.diagnostics,
                           "reference captures are not supported") &&
             hasDiagnostic(invalid.diagnostics,
                           "captured snapshots cannot be made mutable") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot assign to immutable lambda capture") &&
             hasDiagnostic(invalid.diagnostics,
                           "would escape through the function return value") &&
             hasDiagnostic(invalid.diagnostics, "cannot capture 'this'") &&
             hasDiagnostic(invalid.diagnostics,
                           "Lambda argument 1 has type 'bool'") &&
             hasDiagnostic(invalid.diagnostics, "limited to local bindings") &&
             hasDiagnostic(invalid.diagnostics, "requires an initializer"),
         "lambda diagnostics should explain each restricted lifetime or "
         "capture operation");
  expect(hasRelatedDiagnostic(invalid.diagnostics, "Local binding declared") &&
             hasDiagnosticHint(invalid.diagnostics, "capture list"),
         "missing capture diagnostics should point back to the declaration");

  const lang::FrontendResult moveCapture =
      lang::Frontend().analyze("move-capture.gti", R"(
int main() {
  std::unique_ptr<int> owner = std::make_unique<int>(1);
  auto invalid = [owner]() -> int { return *owner; };
  return invalid();
}
)",
                               {standardLibraryPrelude()});
  expect(!moveCapture.canGenerateCode() &&
             hasDiagnostic(moveCapture.diagnostics, "is not copyable"),
         "move-only values should not enter closures without explicit "
         "ownership-transfer capture syntax");

  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(R"(
int main() {
  auto bad_default = [=]() -> int { return 0; };
  auto bad_reference = [&value]() -> int { return value; };
  auto bad_init = [value = 1]() -> int { return value; };
  int recovered = 1;
  return recovered;
}
)"));
  const lang::Program recovered = parser.parse();
  expect(parser.hadError() && !recovered.declarations().empty() &&
             hasDiagnostic(parser.errors(), "capture defaults") &&
             hasDiagnostic(parser.errors(), "reference captures") &&
             hasDiagnostic(parser.errors(), "init captures"),
         "the parser should reject C++ capture defaults and references while "
         "recovering");

  const std::string formatted = lang::Formatter().format(
      "int main(){int offset=1;auto add=[offset](int value)->int{return "
      "offset+value;};return add(1);}");
  expect(formatted.find("auto add = [offset](int value) -> int {\n") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "lambda syntax should receive stable C++-style formatting");
}

void testNonEscapingCallableParameters() {
  const std::string source = R"(
void apply_twice<T, Operation>(mut T& value, Operation operation) {
  operation(value);
  operation(value);
}

class Replace<T> {
  T replacement;

public:
  Replace(T replacement) : replacement(replacement) {}
  void operator()(mut T& value) { value = this.replacement; }
};

int main() {
  mut int lambda_value = 1;
  auto increment = [](mut int& value) -> void { value++; };
  apply_twice(lambda_value, increment);
  mut int object_value = 2;
  Replace<int> object = Replace<int>(4);
  apply_twice(object_value, object);
  return lambda_value + object_value - 7;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("callable-parameters.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected callable-parameter diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "non-escaping generic operations should accept lambdas and exact "
         "function objects");

  const lang::FunctionDecl *apply =
      findTopLevelFunction(frontend.program, "apply_twice");
  const lang::FunctionInfo *applyInfo =
      apply == nullptr ? nullptr : frontend.semantics.findFunction(*apply);
  expect(applyInfo != nullptr && applyInfo->callableParameters.size() == 1 &&
             applyInfo->callableParameters.front().parameterIndex == 1 &&
             applyInfo->callableParameters.front().nonEscaping &&
             applyInfo->callableParameters.front().signatures.size() == 2,
         "semantic function metadata should retain each required callable "
         "signature and its confined parameter");

  std::size_t callableInstances = 0;
  bool foundLambdaContract = false;
  bool foundObjectContract = false;
  bool hirCallsConfined = true;
  bool mirContractsPreserved = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "apply_twice") {
      continue;
    }
    ++callableInstances;
    if (instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().signatures.size() != 2) {
      hirCallsConfined = false;
      continue;
    }
    const lang::HirCallableParameter &contract =
        instance.callableParameters.front();
    const bool lambda =
        contract.callableType.kind == lang::SemanticType::Lambda;
    const bool object = contract.callableType.kind == lang::SemanticType::Class;
    foundLambdaContract =
        foundLambdaContract ||
        (lambda &&
         std::all_of(contract.signatures.begin(), contract.signatures.end(),
                     [](const lang::HirCallableSignature &signature) {
                       return signature.lambdaTarget &&
                              !signature.functionTarget;
                     }));
    foundObjectContract =
        foundObjectContract ||
        (object &&
         std::all_of(contract.signatures.begin(), contract.signatures.end(),
                     [](const lang::HirCallableSignature &signature) {
                       return signature.functionTarget &&
                              !signature.lambdaTarget;
                     }));
    const std::size_t confinedCalls = static_cast<std::size_t>(
        std::count_if(instance.body.values.begin(), instance.body.values.end(),
                      [](const lang::HirValue &value) {
                        return value.kind == lang::HirValueKind::Call &&
                               value.nonEscapingCallable;
                      }));
    hirCallsConfined = hirCallsConfined && confinedCalls == 2;

    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    std::size_t mirConfinedCalls = 0;
    if (mir != nullptr) {
      for (const lang::MirBlock &block : mir->body.blocks) {
        mirConfinedCalls += static_cast<std::size_t>(std::count_if(
            block.instructions.begin(), block.instructions.end(),
            [](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.nonEscapingCallable;
            }));
      }
    }
    mirContractsPreserved = mirContractsPreserved && mir != nullptr &&
                            mir->callableParameters.size() == 1 &&
                            mirConfinedCalls == 2;
  }
  expect(callableInstances == 2 && foundLambdaContract && foundObjectContract &&
             hirCallsConfined && mirContractsPreserved,
         "concrete HIR and MIR instances should retain exact callable targets "
         "and non-escaping invocation metadata");
  const std::string mirDump = lang::MirPrinter().print(frontend.mir);
  expect(mirDump.find("callables=[callable(parameter=1") != std::string::npos &&
             mirDump.find("non-escaping-callable=1") != std::string::npos,
         "canonical MIR dumps should expose callable contracts and confined "
         "invocations");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainInstance = &instance;
      break;
    }
  }
  const std::size_t confinedArguments =
      mainInstance == nullptr ? 0
                              : static_cast<std::size_t>(std::count_if(
                                    mainInstance->body.values.begin(),
                                    mainInstance->body.values.end(),
                                    [](const lang::HirValue &value) {
                                      return value.nonEscapingArguments ==
                                             std::vector<std::size_t>{1};
                                    }));
  expect(confinedArguments == 2,
         "algorithm call sites should identify confined callable arguments");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find(
             "gti_internal::backend::invoke(operation, value)") !=
                 std::string::npos &&
             artifact.contents.find("friend void __gti_invoke(") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_call") !=
                 std::string::npos &&
             artifact.contents.find(" operator()(") == std::string::npos,
         "C++ lowering should bridge exact GTI call operators without "
         "exposing native overload resolution");

  const lang::FrontendResult invalidSignatures =
      lang::Frontend().analyze("invalid-callable-signatures.gti", R"(
void invoke<Operation>(Operation operation) { operation(1); }

class MutableOnly {
public:
  void operator()(int value) mut {}
};

int main() {
  auto wrong_argument = [](bool value) -> void {};
  invoke(wrong_argument);
  auto wrong_return = [](int value) -> int { return value; };
  invoke(wrong_return);
  MutableOnly mutable_only = MutableOnly();
  invoke(mutable_only);
  return 0;
}
)");
  expect(
      !invalidSignatures.canGenerateCode() &&
          hasDiagnostic(invalidSignatures.diagnostics,
                        "parameter requires 'bool'") &&
          hasDiagnostic(invalidSignatures.diagnostics, "must return 'void'") &&
          hasDiagnostic(invalidSignatures.diagnostics,
                        "operator() requires a mutable receiver") &&
          hasRelatedDiagnostic(invalidSignatures.diagnostics,
                               "Concrete generic instance requested here"),
      "concrete callable instances should reject inexact arguments and "
      "non-void operations before backend lowering");

  const lang::FrontendResult invalidBoundary =
      lang::Frontend().analyze("invalid-callable-boundary.gti", R"(
void invoke_ref<Operation>(Operation& operation) { operation(1); }
Operation escape<Operation>(Operation operation) { return operation; }

int main() {
  auto operation = [](int value) -> void {};
  auto escaped = escape(operation);
  invoke_ref(operation);
  return 0;
}
)");
  expect(
      !invalidBoundary.canGenerateCode() &&
          hasDiagnostic(invalidBoundary.diagnostics,
                        "Only a direct by-value generic function parameter") &&
          hasDiagnostic(invalidBoundary.diagnostics,
                        "would escape through the function return value") &&
          hasDiagnosticHint(invalidBoundary.diagnostics,
                            "Non-escaping callable parameters"),
      "the first callable layer should reject references and escaping "
      "closure values explicitly");

  const lang::FrontendResult forwarding =
      lang::Frontend().analyze("callable-forwarding.gti", R"(
void outer<Operation>(Operation operation) { middle(operation); }
void middle<Operation>(Operation operation) { inner(operation); }
void inner<Operation>(Operation operation) { operation(1); }

int main() {
  auto operation = [](int value) -> void {};
  outer(operation);
  return 0;
}
)");
  if (!forwarding.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : forwarding.diagnostics) {
      std::cerr << "Unexpected callable-forwarding diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(forwarding.canGenerateCode(),
         "nested generic forwarding should accept a lambda only through "
         "proven non-escaping callable parameters");

  const lang::FunctionDecl *outer =
      findTopLevelFunction(forwarding.program, "outer");
  const lang::FunctionDecl *middle =
      findTopLevelFunction(forwarding.program, "middle");
  const lang::FunctionDecl *inner =
      findTopLevelFunction(forwarding.program, "inner");
  const lang::FunctionInfo *outerInfo =
      outer == nullptr ? nullptr : forwarding.semantics.findFunction(*outer);
  const lang::FunctionInfo *middleInfo =
      middle == nullptr ? nullptr : forwarding.semantics.findFunction(*middle);
  const lang::FunctionInfo *innerInfo =
      inner == nullptr ? nullptr : forwarding.semantics.findFunction(*inner);
  expect(outerInfo != nullptr && middleInfo != nullptr &&
             innerInfo != nullptr &&
             outerInfo->callableParameters.size() == 1 &&
             outerInfo->callableParameters.front().signatures.empty() &&
             outerInfo->callableParameters.front().forwardings.size() == 1 &&
             middleInfo->callableParameters.size() == 1 &&
             middleInfo->callableParameters.front().signatures.empty() &&
             middleInfo->callableParameters.front().forwardings.size() == 1 &&
             innerInfo->callableParameters.size() == 1 &&
             innerInfo->callableParameters.front().signatures.size() == 1 &&
             innerInfo->callableParameters.front().forwardings.empty(),
         "semantic callable contracts should distinguish forwarded edges "
         "from direct invocation requirements without declaration-order "
         "dependence");

  std::size_t hirForwardingCount = 0;
  std::size_t mirForwardingCount = 0;
  for (const lang::HirFunctionInstance &instance :
       forwarding.hir.functionInstances()) {
    if (instance.source == nullptr ||
        (instance.source->name().lexeme != "outer" &&
         instance.source->name().lexeme != "middle")) {
      continue;
    }
    hirForwardingCount +=
        instance.callableParameters.size() == 1 &&
                instance.callableParameters.front().forwardings.size() == 1 &&
                instance.callableParameters.front()
                    .forwardings.front()
                    .functionTarget.has_value()
            ? 1
            : 0;
    const lang::MirFunctionInstance *mir =
        forwarding.mir.findFunctionInstance(instance.id);
    mirForwardingCount +=
        mir != nullptr && mir->callableParameters.size() == 1 &&
                mir->callableParameters.front().forwardings.size() == 1 &&
                mir->callableParameters.front()
                    .forwardings.front()
                    .functionTarget.has_value()
            ? 1
            : 0;
  }
  const std::string forwardingMir = lang::MirPrinter().print(forwarding.mir);
  expect(hirForwardingCount == 2 && mirForwardingCount == 2 &&
             forwardingMir.find("forwardings=[forward(parameter=0;function=") !=
                 std::string::npos,
         "HIR and MIR should retain each proven callable forwarding target");

  const lang::FrontendResult unprovenForwarding =
      lang::Frontend().analyze("unproven-callable-forwarding.gti", R"(
void discard<Operation>(Operation operation) {}
void outer<Operation>(Operation operation) { discard(operation); }

int main() {
  auto operation = [](int value) -> void {};
  outer(operation);
  return 0;
}
)");
  expect(!unprovenForwarding.canGenerateCode() &&
             hasDiagnostic(unprovenForwarding.diagnostics,
                           "not proven non-escaping") &&
             hasDiagnosticHint(unprovenForwarding.diagnostics,
                               "direct by-value generic parameter"),
         "ordinary generic parameters should not gain callable forwarding "
         "privileges without a proven target contract");
}

void testNonEscapingCallablePredicates() {
  const std::string source = R"(
bool accepts<Predicate>(int value, Predicate predicate) {
  return predicate(value);
}

bool accepts_split<Predicate>(int left, int right, Predicate predicate) {
  if (predicate(left) && !predicate(right)) {
    return true;
  }
  return false;
}

bool stores_result<Predicate>(int value, Predicate predicate) {
  bool initialized = predicate(value);
  mut bool assigned = false;
  assigned = predicate(value);
  return initialized && assigned;
}

void checks_loops<Predicate>(Predicate predicate) {
  mut int value = 0;
  while (predicate(value)) {
    value++;
  }
  for (mut int index = 0; predicate(index); index++) {}
}

class GreaterThan {
  int threshold;

public:
  GreaterThan(int threshold) : threshold(threshold) {}
  bool operator()(int value) { return value > this.threshold; }
};

int main() {
  auto positive = [](int value) -> bool { return value > 0; };
  bool lambda_result = accepts(4, positive);
  bool split_result = accepts_split(4, -1, positive);
  bool stored_result = stores_result(4, positive);
  auto never = [](int value) -> bool { return value < 0; };
  checks_loops(never);
  GreaterThan greater_than = GreaterThan(3);
  bool object_result = accepts(4, greater_than);
  if (lambda_result && split_result && stored_result && object_result) {
    return 0;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("callable-predicates.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected callable-predicate diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "non-escaping predicates should support exact bool results in "
         "conditions, initializers, assignments, and returns");

  const lang::FunctionDecl *accepts =
      findTopLevelFunction(frontend.program, "accepts");
  const lang::FunctionInfo *acceptsInfo =
      accepts == nullptr ? nullptr : frontend.semantics.findFunction(*accepts);
  expect(acceptsInfo != nullptr &&
             acceptsInfo->callableParameters.size() == 1 &&
             acceptsInfo->callableParameters.front().signatures.size() == 1 &&
             acceptsInfo->callableParameters.front()
                     .signatures.front()
                     .returnType == lang::SemanticType::Bool,
         "semantic callable contracts should retain an exact bool predicate "
         "result requirement");

  std::size_t predicateInstances = 0;
  bool foundLambdaPredicate = false;
  bool foundObjectPredicate = false;
  bool hirPredicateValues = true;
  bool mirPredicateValues = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "accepts") {
      continue;
    }
    ++predicateInstances;
    if (instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().signatures.size() != 1) {
      hirPredicateValues = false;
      continue;
    }
    const lang::HirCallableParameter &contract =
        instance.callableParameters.front();
    const lang::HirCallableSignature &signature = contract.signatures.front();
    foundLambdaPredicate =
        foundLambdaPredicate ||
        (contract.callableType.kind == lang::SemanticType::Lambda &&
         signature.returnType == lang::SemanticType::Bool &&
         signature.lambdaTarget && !signature.functionTarget);
    foundObjectPredicate =
        foundObjectPredicate ||
        (contract.callableType.kind == lang::SemanticType::Class &&
         signature.returnType == lang::SemanticType::Bool &&
         signature.functionTarget && !signature.lambdaTarget);
    hirPredicateValues =
        hirPredicateValues &&
        std::any_of(instance.body.values.begin(), instance.body.values.end(),
                    [](const lang::HirValue &value) {
                      return value.kind == lang::HirValueKind::Call &&
                             value.nonEscapingCallable &&
                             value.info.type == lang::SemanticType::Bool;
                    });

    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    bool foundMirPredicate = false;
    if (mir != nullptr && mir->callableParameters.size() == 1 &&
        mir->callableParameters.front().signatures.size() == 1 &&
        mir->callableParameters.front().signatures.front().returnType ==
            lang::SemanticType::Bool) {
      for (const lang::MirBlock &block : mir->body.blocks) {
        foundMirPredicate =
            foundMirPredicate ||
            std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [](const lang::MirInstruction &instruction) {
                  return instruction.kind == lang::MirInstructionKind::Call &&
                         instruction.nonEscapingCallable &&
                         instruction.info.type == lang::SemanticType::Bool;
                });
      }
    }
    mirPredicateValues = mirPredicateValues && foundMirPredicate;
  }
  expect(predicateInstances == 2 && foundLambdaPredicate &&
             foundObjectPredicate && hirPredicateValues && mirPredicateValues,
         "concrete HIR and MIR predicate instances should retain exact bool "
         "result types and selected callable identities");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find(
             "return gti_internal::backend::invoke(predicate, value)") !=
                 std::string::npos &&
             artifact.contents.find("friend bool __gti_invoke(") !=
                 std::string::npos,
         "C++ lowering should preserve predicate values through the exact "
         "callable bridge");

  const lang::FrontendResult wrongReturn =
      lang::Frontend().analyze("invalid-predicate-return.gti", R"(
bool accepts<Predicate>(int value, Predicate predicate) {
  return predicate(value);
}

int main() {
  auto wrong = [](int value) -> int { return value; };
  [[discard]] accepts(1, wrong);
  return 0;
}
)");
  expect(!wrongReturn.canGenerateCode() &&
             hasDiagnostic(wrongReturn.diagnostics,
                           "must return 'bool' for this invocation") &&
             hasRelatedDiagnostic(wrongReturn.diagnostics,
                                  "Concrete generic instance requested here"),
         "predicate instantiation should reject non-bool callable results "
         "before backend lowering");

  const lang::FrontendResult unsupportedResults =
      lang::Frontend().analyze("unsupported-callable-results.gti", R"(
int map<Operation>(int value, Operation operation) {
  return operation(value);
}

void infer<Predicate>(int value, Predicate predicate) {
  auto result = predicate(value);
}

int main() { return 0; }
)");
  expect(!unsupportedResults.canGenerateCode() &&
             hasDiagnostic(unsupportedResults.diagnostics,
                           "support only exact bool predicates") &&
             hasDiagnostic(unsupportedResults.diagnostics,
                           "cannot be inferred with 'auto'") &&
             unsupportedResults.diagnostics.size() == 2,
         "arbitrary and inferred generic callable results should remain "
         "closed with focused diagnostics");
}

void testDefaultNodiscard() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
int calculate() { return 7; }
void perform() {}
int main() {
  [[discard]] calculate();
  perform();
  mut int count = 0;
  count++;
  return 0;
}
)");
  expect(!lexer.hadError(), "discard attribute syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "discard attribute syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "explicit discard and void calls should pass semantic checks");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("calculate();") != std::string::npos &&
             generated.find("[[discard]]") == std::string::npos,
         "discard should be a GTI-only call-site attribute");

  auto invalidTokens = lexer.scan(R"(
int calculate() { return 7; }
void perform() {}
int main() {
  calculate();
  [[discard]] perform();
  [[discard]] 1 + 2;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid discard uses should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "ignored function results and invalid discard attributes should fail");
  expect(invalidSemantic.errors().size() == 3,
         "nodiscard should produce focused diagnostics for all invalid uses");
}

void testExpectedValues() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
namespace std { using string_view = gti_internal::text_view; }

class MutableValue {
  mut int stored = 0;
public:
  void set(int value) mut { this.stored = value; }
  int get() { return this.stored; }
};

expected<int, std::string_view> calculate(bool fail) {
  if (fail) { return unexpected("calculation failed"); }
  return 42;
}
expected<void, std::string_view> render(bool fail) {
  if (fail) { return unexpected("render failed"); }
  return;
}
int main() {
  expected<int, std::string_view> result = calculate(false);
  if (!result.has_value()) { return 1; }
  int value = result.value_or(0);
  expected<void, std::string_view> rendered = render(false);
  if (!rendered) { return 2; }
  rendered.value();
  mut expected<MutableValue, int> mutable_result = MutableValue();
  mutable_result.value().set(42);
  if (mutable_result.value().get() != 42) { return 3; }
  [[discard]] calculate(false);
  return value - 42;
}
)");
  expect(!lexer.hadError(), "expected source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "expected types and values should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "expected construction and observers should pass semantic checks");

  const std::string cpp23 = lang::CppEmitter().emit(program);
  expect(cpp23.find("#include <expected>") != std::string::npos &&
             cpp23.find("std::expected<std::int32_t, gti_std::string_view>") !=
                 std::string::npos &&
             cpp23.find("std::unexpected(") != std::string::npos &&
             cpp23.find("return {};") != std::string::npos,
         "C++23 should lower expected values to the standard library");

  const std::string cpp20 =
      lang::CppEmitter(lang::CppStandard::Cpp20).emit(program);
  expect(
      cpp20.find("#include <nonstd/expected.hpp>") != std::string::npos &&
          cpp20.find("nonstd::expected<std::int32_t, gti_std::string_view>") !=
              std::string::npos &&
          cpp20.find("nonstd::make_unexpected(") != std::string::npos,
      "C++20 should lower expected values to the vendored implementation");

  auto invalidTokens = lexer.scan(R"(
expected<int, void> invalid_error() { return 1; }
expected<int, int> bad_success() { return "wrong"; }
expected<int, int> bad_error() { return unexpected("wrong"); }
expected<int, int> valid_result() { return 1; }
expected<void, int> complete() { return; }
int main() {
  valid_result();
  expected<int, int> result = bad_success();
  result.has_value();
  expected<void, int> completion = complete();
  completion.value_or(0);
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid expected uses should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid expected types, states, and observers should fail");
  expect(invalidSemantic.errors().size() == 6,
         "expected validation should produce focused diagnostics");
}

void testPrintIsAnIdentifier() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int print(int value) { return value; }
int main() { return print(0); }
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "print should parse as an ordinary identifier");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "user code should be able to declare a function named print");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find(
             "std::int32_t print(const std::int32_t value)") !=
             std::string::npos,
         "print should lower as a normal function");
}

void testTypeAliases() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
using LaterSize = Size;
using Size = uint64_t;
using Triple = int[3];

class Box<T> {
  T value;
public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
};

using IntBox = Box<int>;
using Completion = expected<void, int>;
using Result = int;

Completion complete() { return; }

Result main() {
  LaterSize count = Size(3);
  Triple values = {1, 2, 3};
  IntBox box = IntBox(values[1]);
  Completion completion = complete();
  if (count == 3 and box.get() == 2 and completion) { return 0; }
  return 1;
}
)");
  expect(!lexer.hadError(), "type aliases should lex as declarations");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "namespace-scoped type aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "type aliases should canonicalize before semantic checks");

  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &semantic.model())
          .emit(program);
  expect(generated.find("using LaterSize = std::uint64_t;") !=
                 std::string::npos &&
             generated.find("using Triple = std::array<std::int32_t, 3>;") !=
                 std::string::npos &&
             generated.find("using IntBox = ::Box<std::int32_t>;") !=
                 std::string::npos &&
             generated.find("numeric_cast<Size>") != std::string::npos &&
             generated.find("#include <expected>") != std::string::npos &&
             generated.find("int main()") != std::string::npos,
         "the C++ backend should emit canonical, declaration-order-independent "
         "aliases");

  auto invalidTokens = lexer.scan(R"(
using First = Second;
using Second = First;
using Borrow = int&;
using Missing = MissingType;
using Duplicate = int;
using Duplicate = uint64_t;
int main() { return 0; }
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid alias relationships should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "cycles, reference aliases, missing targets, and duplicates should "
         "be rejected");
  expect(hasDiagnostic(invalidSemantic, "Type alias cycle") &&
             hasDiagnostic(invalidSemantic, "Reference aliases") &&
             hasDiagnostic(invalidSemantic, "Unknown type 'MissingType'") &&
             hasDiagnostic(invalidSemantic, "Duplicate declaration"),
         "invalid aliases should produce focused diagnostics");

  lang::Parser localAliasParser(
      lexer.scan("int main() { using Local = int; return 0; }"));
  (void)localAliasParser.parse();
  expect(localAliasParser.hadError() &&
             hasDiagnostic(localAliasParser.errors(), "namespace scope"),
         "local type aliases should be rejected by the parser");
}

void testNamespacesAndAliases() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
namespace engine {
namespace graphics {
class Renderer {};
void render() {}
void renderTwice() {
  render();
  render();
}
}
}

namespace gfx = engine::graphics;
gfx::Renderer createRenderer();

int main() {
  engine::graphics::render();
  gfx::renderTwice();
  return 0;
}
)");
  expect(!lexer.hadError(), "namespace source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "nested namespaces and aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "qualified calls and namespace aliases should resolve");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("namespace engine {") != std::string::npos &&
             generated.find("namespace graphics {") != std::string::npos,
         "emitter should preserve nested namespaces");
  expect(generated.find("namespace gfx = engine::graphics;") !=
             std::string::npos,
         "emitter should preserve namespace aliases");
  expect(generated.find("gfx::Renderer createRenderer();") !=
             std::string::npos,
         "qualified types should parse and emit through namespace aliases");
  expect(generated.find("engine::graphics::render();") != std::string::npos &&
             generated.find("gfx::renderTwice();") != std::string::npos,
         "emitter should preserve qualified calls");

  auto invalidTokens = lexer.scan(R"(
namespace engine {}
namespace gfx = engine::missing;
int main() {
  engine::missing::render();
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "invalid namespace source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "unknown namespace paths should be rejected");
  expect(invalidSemantic.errors().size() == 2,
         "alias targets and qualified calls should both be diagnosed");
}

void testCompileTimeConditionals() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
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
};

int main() {
#if target.os != "windows"
  int value = platform_value();
#else
  int value = platform_value();
#endif
  return value;
}
)");
  expect(!lexer.hadError(), "compile-time directives should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "compile-time branches should parse");

  const lang::TargetInfo apple{"macos", "apple", "arm64"};
  lang::SemanticVisitor appleSemantic(apple);
  expect(appleSemantic.check(program),
         "inactive branches should not participate in Apple semantics");
  const std::string appleCpp =
      lang::CppEmitter(lang::CppStandard::Cpp23, apple).emit(program);
  expect(appleCpp.find("return 101;") != std::string::npos &&
             appleCpp.find("return 64;") != std::string::npos &&
             appleCpp.find("std::int32_t bits = 64") != std::string::npos &&
             appleCpp.find("missing_name") == std::string::npos &&
             appleCpp.find("#include <expected>") == std::string::npos &&
             appleCpp.find("#include <gti/runtime.hpp>") == std::string::npos &&
             appleCpp.find("#if") == std::string::npos,
         "Apple lowering should emit only active branches without C++ macros");

  const lang::TargetInfo windows{"windows", "pc", "x86_64"};
  lang::SemanticVisitor windowsSemantic(windows);
  expect(windowsSemantic.check(program),
         "Windows should select the elif and else branches");
  const std::string windowsCpp =
      lang::CppEmitter(lang::CppStandard::Cpp23, windows).emit(program);
  expect(windowsCpp.find("return 202;") != std::string::npos &&
             windowsCpp.find("std::int32_t bits = 32") != std::string::npos &&
             windowsCpp.find("nested_value") == std::string::npos &&
             windowsCpp.find("return 101;") == std::string::npos,
         "target selection should distinguish vendor, OS, and architecture");

  auto malformedTokens = lexer.scan(R"(
#if target.os == "never"
int broken = ;
#else
int valid = 1;
#endif
)");
  lang::Parser malformedParser(std::move(malformedTokens));
  malformedParser.parse();
  expect(malformedParser.errors().size() == 1,
         "inactive branches must still be syntactically valid");

  auto invalidConditionTokens = lexer.scan(R"(
#if target.platform == "macos"
int value = 1;
#endif
)");
  lang::Parser invalidConditionParser(std::move(invalidConditionTokens));
  invalidConditionParser.parse();
  expect(invalidConditionParser.hadError(),
         "unknown target properties should be diagnosed");

  const std::vector<lang::Token> errorKeyword =
      lexer.scan("#error \"unsupported configuration\"");
  expect(errorKeyword.size() >= 3 &&
             errorKeyword.front().kind == lang::TokenKind::HASH_ERROR,
         "#error should have a dedicated directive token");

  auto errorTokens = lexer.scan(R"(
#if target.os == "blocked"
#error "This target is not supported."
#endif

class PlatformState {
#if target.os == "blocked"
#error "PlatformState is unavailable on this target."
#endif
  int value = 1;
};

int main() {
#if target.os == "blocked"
#error "No entry point is available for this target."
#endif
  return 0;
}
)");
  lang::Parser errorParser(std::move(errorTokens));
  lang::Program errorProgram = errorParser.parse();
  expect(!errorParser.hadError(),
         "#error should parse in declaration, member, and block contexts");

  const lang::TargetInfo allowed{"allowed", "test", "test"};
  lang::SemanticVisitor allowedSemantic(allowed);
  expect(allowedSemantic.check(errorProgram),
         "inactive #error directives should not affect semantics");
  const std::string allowedCpp =
      lang::CppEmitter(lang::CppStandard::Cpp23, allowed).emit(errorProgram);
  expect(allowedCpp.find("#error") == std::string::npos,
         "#error directives should never leak into backend output");

  const lang::TargetInfo blocked{"blocked", "test", "test"};
  lang::SemanticVisitor blockedSemantic(blocked);
  expect(!blockedSemantic.check(errorProgram) &&
             blockedSemantic.errors().size() == 3 &&
             std::all_of(blockedSemantic.errors().begin(),
                         blockedSemantic.errors().end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-S2047";
                         }) &&
             hasDiagnostic(blockedSemantic, "This target is not supported."),
         "active #error directives should report their source messages once");

  auto missingMessageTokens = lexer.scan("#error\nint value = 1;");
  lang::Parser missingMessageParser(std::move(missingMessageTokens));
  missingMessageParser.parse();
  expect(missingMessageParser.hadError() &&
             hasDiagnostic(missingMessageParser.errors(),
                           "Expect a string message after '#error'"),
         "#error should require an explicit string message");

  const std::string formattedError = lang::Formatter().format(
      "#if target.os==\"never\"\n#error   \"unsupported\"\n#endif");
  expect(formattedError ==
             "#if target.os == \"never\"\n#error \"unsupported\"\n#endif\n",
         "the formatter should preserve and normalize compile-time errors");
}

void testRuntimeBackedStdlibSurface() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
namespace gti_internal {
namespace runtime {
@runtime("stdout.write")
void write_stdout(gti_internal::text_view value);
@runtime("stdin.read_byte")
int32_t read_stdin_byte();
@runtime("file.open_read")
int64_t open_file_read(gti_internal::text_view path);
@runtime("file.read_byte")
int32_t read_file_byte(int64_t descriptor);
@runtime("file.close")
int32_t close_file(int64_t descriptor);
}
}

namespace std {
using string_view = gti_internal::text_view;

void print(string_view value) {
  gti_internal::runtime::write_stdout(value);
}
}

int main() {
  std::print("hello");
  [[discard]] gti_internal::runtime::read_stdin_byte();
  int64_t descriptor = gti_internal::runtime::open_file_read("input");
  [[discard]] gti_internal::runtime::read_file_byte(descriptor);
  [[discard]] gti_internal::runtime::close_file(descriptor);
  return 0;
}
)");
  expect(!lexer.hadError(), "runtime-backed stdlib source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "runtime-backed stdlib source should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "runtime binding and text-view call signatures should validate");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("#include <gti/runtime.hpp>") != std::string::npos,
         "runtime-backed programs should include the native adapter");
  expect(generated.find("gti_internal::runtime::open_file_read") !=
                 std::string::npos &&
             generated.find("gti_internal::runtime::read_file_byte") !=
                 std::string::npos &&
             generated.find("gti_internal::runtime::close_file") !=
                 std::string::npos,
         "validated file bindings should lower through the native adapter");
  expect(generated.find("namespace gti_std") != std::string::npos &&
             generated.find("gti_std::print(std::string_view{\"hello\", 5})") !=
                 std::string::npos,
         "GTI std should lower outside the reserved C++ std namespace");
  expect(generated.find("const string_view value") != std::string::npos &&
             generated.find("const string_view &value") == std::string::npos,
         "small immutable text views should lower by value");

  auto invalidTokens = lexer.scan(R"(
@runtime("stdout.write")
void fake_write(gti_internal::text_view value);
int main() { fake_write("hello"); return 0; }
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "invalid runtime declaration should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "runtime bindings outside the compiler-owned symbol should fail");
  expect(invalidSemantic.errors().size() == 1,
         "invalid runtime binding should produce one focused diagnostic");
}

void testScopedEnums() {
  const std::string source = R"(
namespace engine {
enum class State : uint8_t {
  Idle,
  Running = 4,
  Stopped,
};
enum class Mode { editor, game };
}

using StateAlias = engine::State;

StateAlias next(StateAlias state) {
  if (state == StateAlias::Idle) {
    return engine::State::Running;
  }
  return StateAlias::Stopped;
}

int main() {
  auto state = next(StateAlias::Idle);
  if (state == engine::State::Running) {
    return 0;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("scoped-enums.gti", source);
  expect(frontend.canGenerateCode(),
         "scoped enum declarations and qualified enumerators should compile");
  expect(frontend.diagnostics.empty(),
         "valid scoped enums should not produce diagnostics");
  expect(frontend.hir.enumDeclarations().size() == 2,
         "HIR should retain scoped enum declarations");
  const lang::HirEnum *stateEnum =
      frontend.hir.enumDeclarations().empty()
          ? nullptr
          : &frontend.hir.enumDeclarations().front();
  expect(stateEnum != nullptr &&
             stateEnum->underlyingType == lang::SemanticType::UInt8 &&
             stateEnum->enumerators.size() == 3 &&
             stateEnum->enumerators[2].value ==
                 lang::EnumConstant{.magnitude = 5},
         "HIR should retain the backing type and evaluated enumerator values");

  bool foundResolvedEnumerator = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.kind == lang::HirValueKind::QualifiedName && value.enumOwner &&
          value.enumValue) {
        foundResolvedEnumerator = true;
      }
    }
  }
  expect(foundResolvedEnumerator,
         "HIR enum references should carry resolved nominal value identity");

  const std::string generated = lang::CppEmitter(
                                    lang::CppStandard::Cpp23,
                                    lang::TargetInfo::host(), nullptr,
                                    &frontend.semantics, &frontend.hir)
                                    .emit(frontend.program);
  expect(generated.find("enum class State : std::uint8_t;") !=
             std::string::npos &&
             generated.find("enum class State : std::uint8_t {") !=
                 std::string::npos &&
             generated.find("enum class Mode : std::int32_t {") !=
                 std::string::npos &&
             generated.find("Running = 4") != std::string::npos,
         "the C++ backend should emit fixed-backing scoped enums");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-scoped-enums.gti", R"(
enum class Tiny : uint8_t {
  First,
  TooLarge = 256,
  First = 2,
};
enum class Other { First };

int main() {
  mut Tiny uninitialized;
  Tiny value = 1;
  int raw = Tiny::First;
  if (Tiny::First) {
    return 1;
  }
  bool mixed = Tiny::First == Other::First;
  auto missing = Tiny::Missing;
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid scoped enum uses should block code generation");
  expect(hasDiagnostic(invalid.diagnostics,
                       "Scoped enum value does not fit its backing type") &&
             hasDiagnostic(invalid.diagnostics, "Duplicate enumerator") &&
             hasDiagnostic(invalid.diagnostics,
                           "Equality operands have incompatible types") &&
             hasDiagnostic(invalid.diagnostics,
                           "explicit enumerator initializer") &&
             hasDiagnostic(invalid.diagnostics, "Unknown enumerator"),
         "scoped enums should diagnose range, scope, and nominal type errors");

  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("enum Legacy { Value }; int main() { return 0; }"));
  const lang::Program recovered = parser.parse();
  expect(parser.hadError() && recovered.declarations().size() == 1,
         "unscoped enums should be rejected without preventing parser "
         "recovery");

  const std::string formatted = lang::Formatter().format(
      "enum class State:uint8_t{Idle,Running=4,Stopped,};");
  expect(formatted == R"(enum class State : uint8_t {
  Idle,
  Running = 4,
  Stopped,
};
)",
         "formatter should produce stable C++-style scoped enum layout");
  expect(lang::Formatter().format(formatted) == formatted,
         "scoped enum formatting should be idempotent");
}

void testFormatting() {
  const std::string source = R"(#include   "math.gti"
#include   < std / array >

namespace engine{class Counter{mut int value=0;
#if target.arch=="arm64"
int word_bits=64;
#endif
int tick(int amount)mut{if(amount>0){this.value+=amount;}else{this.value-=1;}return this.value;}};}
#if target.vendor=="apple"
int main(){for(mut int i=0;i<3;i++){if(i==1){continue ;}std::println("frame"); // keep this comment
if(i>1){break ;}
}mut int attempts=0;do{attempts++;}while(attempts<1);return -1;}
#else
int main(){[[discard]] engine::run();return 0;}
#endif
)";

  const std::string expected = R"(#include "math.gti"
#include <std/array>

namespace engine {
  class Counter {
    mut int value = 0;
#if target.arch == "arm64"
    int word_bits = 64;
#endif
    int tick(int amount) mut {
      if (amount > 0) {
        this.value += amount;
      } else {
        this.value -= 1;
      }
      return this.value;
    }
  };
}
#if target.vendor == "apple"
int main() {
  for (mut int i = 0; i < 3; i++) {
    if (i == 1) {
      continue;
    }
    std::println("frame"); // keep this comment
    if (i > 1) {
      break;
    }
  }
  mut int attempts = 0;
  do {
    attempts++;
  } while (attempts < 1);
  return -1;
}
#else
int main() {
  [[discard]] engine::run();
  return 0;
}
#endif
)";

  const std::string formatted = lang::Formatter().format(source);
  if (formatted != expected) {
    std::cerr << "Formatted output was:\n" << formatted;
  }
  expect(formatted == expected,
         "formatter should produce stable C++-style GTI layout");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatting should be idempotent");

  const std::string staticFormatted = lang::Formatter().format(
      "static mut int count=0;class Registry{public:static int value=1;"
      "static int current(){return value;}};");
  expect(staticFormatted == R"(static mut int count = 0;
class Registry {
public:
  static int value = 1;
  static int current() {
    return value;
  }
};
)" && lang::Formatter().format(staticFormatted) == staticFormatted,
         "formatter should preserve static declaration ownership and remain "
         "idempotent");

  const std::string tabIndented =
      lang::Formatter({.indentWidth = 4, .insertSpaces = false})
          .format("int main(){if(true){return 0;}}");
  expect(tabIndented.find("\n\tif (true) {\n\t\treturn 0;") !=
             std::string::npos,
         "formatter should honor tab indentation requested by an editor");

  const std::string referenceSource =
      "class Box<T>{};"
      "void use(int& value,Box<int>& box){int A=value;int bits=A&value;}";
  const std::string leftReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Left})
          .format(referenceSource);
  const std::string rightReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Right})
          .format(referenceSource);
  const std::string middleReferences = lang::Formatter().format(
      "class Box{};void use(int& value,Box& box){int bits=value&value;}"
      "int min(int left,int right){if(left> right){return right;}return "
      "left;}");
  expect(leftReferences.find("int& value") != std::string::npos &&
             leftReferences.find("Box<int>& box") != std::string::npos &&
             rightReferences.find("int &value") != std::string::npos &&
             rightReferences.find("Box<int> &box") != std::string::npos &&
             leftReferences.find("A & value") != std::string::npos &&
             rightReferences.find("A & value") != std::string::npos &&
             middleReferences.find("int & value") != std::string::npos,
         "reference alignment should recognize declared generic types "
         "without changing bitwise-and spacing for capitalized values");
  expect(middleReferences.find("if (left > right)") != std::string::npos,
         "comparison operators should retain binary spacing");

  const lang::FormatConfigResult parsedStyle =
      lang::parseFormatConfig("IndentWidth: 4\n"
                              "BasedOnStyle: LLVM\n"
                              "UseTab: Never\n"
                              "BreakBeforeBraces: Allman\n"
                              "SpaceBeforeParens: Never\n"
                              "IndentCaseLabels: true\n"
                              "AccessModifierOffset: -4\n"
                              "MaxEmptyLinesToKeep: 0\n"
                              "SpacesBeforeTrailingComments: 3\n"
                              "SpaceBeforeAssignmentOperators: false\n"
                              "ReferenceAlignment: Left\n",
                              {.indentWidth = 7});
  expect(parsedStyle.issues.empty() && parsedStyle.options.indentWidth == 4 &&
             parsedStyle.options.insertSpaces &&
             parsedStyle.options.breakBeforeBraces ==
                 lang::BraceBreakingStyle::Allman &&
             parsedStyle.options.spaceBeforeParens ==
                 lang::SpaceBeforeParensStyle::Never &&
             parsedStyle.options.indentCaseLabels &&
             parsedStyle.options.accessModifierOffset == -4 &&
             parsedStyle.options.maxEmptyLinesToKeep == 0 &&
             parsedStyle.options.spacesBeforeTrailingComments == 3 &&
             !parsedStyle.options.spaceBeforeAssignmentOperators &&
             parsedStyle.options.referenceAlignment ==
                 lang::ReferenceAlignment::Left,
         "format configuration should apply a base style before explicit "
         "clang-format-compatible overrides");

  const std::string configuredSource =
      "class Example{public:int run(int value){if(value>0){value+=1;"
      "// retain\n}else{return 0;}switch(value){case 1:value+=1;break;"
      "default:return value;}}};";
  const std::string configuredExpected = R"(class Example
{
public:
    int run(int value)
    {
        if(value > 0)
        {
            value+= 1;   // retain
        }
        else
        {
            return 0;
        }
        switch(value)
        {
            case 1:
                value+= 1;
                break;
            default:
                return value;
        }
    }
};
)";
  const std::string configured =
      lang::Formatter(parsedStyle.options).format(configuredSource);
  if (configured != configuredExpected) {
    std::cerr << "Configured formatted output was:\n" << configured;
  }
  expect(configured == configuredExpected &&
             lang::Formatter(parsedStyle.options).format(configured) ==
                 configured,
         "configured Allman, spacing, access, case-label, and comment styles "
         "should be stable and idempotent");

  const lang::FormatConfigResult tabStyle =
      lang::parseFormatConfig("IndentWidth: 4\nUseTab: ForIndentation\n");
  expect(!tabStyle.options.insertSpaces &&
             lang::Formatter(tabStyle.options)
                     .format("int main(){if(true){return 0;}}")
                     .find("\n\tif (true) {") != std::string::npos,
         "UseTab: ForIndentation should use tabs for structural indentation");

  const std::string spacedParentheses =
      lang::Formatter(
          {.spaceBeforeParens = lang::SpaceBeforeParensStyle::Always})
          .format("int invoke<T>(T value){if(true){return invoke<T>(value);}}");
  expect(spacedParentheses.find("int invoke<T> (T value)") !=
                 std::string::npos &&
             spacedParentheses.find("if (true)") != std::string::npos &&
             spacedParentheses.find("invoke<T> (value)") != std::string::npos,
         "SpaceBeforeParens: Always should cover declarations, controls, and "
         "generic calls");

  const lang::FormatConfigResult invalidStyle =
      lang::parseFormatConfig("IndentWidth: 0\n"
                              "UseTab: Sometimes\n"
                              "UnknownOption: value\n"
                              "not-an-entry\n");
  expect(invalidStyle.issues.size() == 4,
         "invalid and unsupported format entries should remain observable");

  const std::string emptyLines = lang::Formatter({.maxEmptyLinesToKeep = 2})
                                     .format("int first=1;\n\n\nint second=2;");
  expect(emptyLines == "int first = 1;\n\n\nint second = 2;\n" &&
             lang::Formatter({.maxEmptyLinesToKeep = 2}).format(emptyLines) ==
                 emptyLines,
         "MaxEmptyLinesToKeep should cap and preserve the requested number "
         "of empty lines");

  const std::string disabledSource = "int main(){return 0;}";
  expect(lang::Formatter({.disableFormat = true}).format(disabledSource) ==
             disabledSource,
         "DisableFormat should leave source byte-for-byte unchanged");
}

void testLanguageQueries() {
  const std::string source = R"(
uint64_t choose(uint64_t value) { return value; }
float choose(float value) { return value; }

namespace engine {
namespace graphics {
int render() { return 1; }
}
}
namespace gfx = engine::graphics;

class Fixed<T, uint64_t N> {
  T values[N] = {};
};

class Box {
  int value = 0;
public:
  Box(int value) : value(value) {}
};

class Lifecycle {
  int value = 0;
public:
  Lifecycle(Lifecycle& other) = default;
  Lifecycle(Lifecycle&& other) = default;
};

class Accessor {
  mut int value = 0;
public:
  int inspect() { return this.value; }
  int inspect() mut {
    this.value += 1;
    return this.value;
  }
};

int main() {
  auto inferred = choose(uint64_t(1));
  int rendered = gfx::render();
  mut int counter = 0;
  counter++;
  ++counter;
  Box box = Box(1);
  Lifecycle lifecycle_source{};
  Lifecycle lifecycle_copy = Lifecycle(lifecycle_source);
  mut Lifecycle lifecycle_movable{};
  Lifecycle lifecycle_move = Lifecycle(std::move(lifecycle_movable));
  Accessor fixed = Accessor();
  mut Accessor changing = Accessor();
  int fixed_value = fixed.inspect();
  int changing_value = changing.inspect();
  return 0;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "language-queries.gti", source, {standardLibraryPrelude()});
  expect(frontend.semanticValid,
         "language-query fixture should pass semantic analysis");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const auto hoverAt = [&](std::size_t offset) {
    return queries.hover(frontend, unit, offset);
  };

  const std::size_t call = source.rfind("choose(uint64_t");
  const std::optional<lang::HoverInfo> selected = hoverAt(call + 1);
  expect(selected && selected->signature == "uint64_t choose(uint64_t value)",
         "hover should render the exact selected overload in GTI syntax");
  expect(selected && selected->signature.find("float") == std::string::npos,
         "selected-call hover should not include unrelated overloads");
  expect(selected && selected->range.start == call &&
             selected->range.end == call + std::string("choose").size(),
         "hover should retain the exact source identifier range");

  const std::size_t readonlyReceiverCall =
      source.find("fixed.inspect") + std::string("fixed.").size();
  const std::size_t mutableReceiverCall =
      source.find("changing.inspect") + std::string("changing.").size();
  const std::optional<lang::HoverInfo> readonlyReceiverHover =
      hoverAt(readonlyReceiverCall + 1);
  const std::optional<lang::HoverInfo> mutableReceiverHover =
      hoverAt(mutableReceiverCall + 1);
  expect(
      readonlyReceiverHover &&
          readonlyReceiverHover->signature == "int32_t Accessor::inspect()" &&
          mutableReceiverHover &&
          mutableReceiverHover->signature == "int32_t Accessor::inspect() mut",
      "hover should expose the exact receiver-qualified method selected by "
      "semantics");

  const std::size_t autoKeyword = source.find("auto inferred");
  const std::optional<lang::HoverInfo> inferredType = hoverAt(autoKeyword + 1);
  expect(inferredType && inferredType->signature == "uint64_t",
         "hover over auto should show the complete inferred type");

  const std::size_t inferredName = source.find("inferred", autoKeyword);
  const std::optional<lang::HoverInfo> binding = hoverAt(inferredName + 1);
  expect(binding && binding->signature == "uint64_t inferred",
         "hover over a binding declaration should show mutability and type");

  const std::size_t construction = source.rfind("Box(1)");
  const std::optional<lang::HoverInfo> constructor = hoverAt(construction + 1);
  expect(constructor && constructor->signature == "Box(int32_t value)",
         "constructor hover should show the selected constructor signature");

  const std::size_t copyConstruction =
      source.rfind("Lifecycle(lifecycle_source)");
  const std::size_t moveConstruction =
      source.rfind("Lifecycle(std::move(lifecycle_movable))");
  const std::optional<lang::HoverInfo> copyConstructor =
      hoverAt(copyConstruction + 1);
  const std::optional<lang::HoverInfo> moveConstructor =
      hoverAt(moveConstruction + 1);
  expect(copyConstructor &&
             copyConstructor->signature == "Lifecycle(Lifecycle&)" &&
             moveConstructor &&
             moveConstructor->signature == "Lifecycle(Lifecycle&&)",
         "constructor hover should distinguish copy and move policies");

  expect(!hoverAt(source.find("return 0") + std::string("return").size()),
         "hover should return no result for whitespace");
  expect(!frontend.semantics.database().occurrences(unit).empty(),
         "semantic analysis should retain tooling occurrences in its snapshot");

  const auto parameterReference = std::find_if(
      frontend.semantics.database().occurrences(unit).begin(),
      frontend.semantics.database().occurrences(unit).end(),
      [](const lang::SemanticOccurrence &occurrence) {
        return occurrence.name == "value" &&
               !lang::hasRole(occurrence.roles,
                              lang::OccurrenceRole::Declaration) &&
               occurrence.bindingKind == lang::SemanticBindingKind::Parameter;
      });
  expect(parameterReference !=
             frontend.semantics.database().occurrences(unit).end(),
         "semantic occurrences should retain resolved parameter references");
  if (parameterReference !=
      frontend.semantics.database().occurrences(unit).end()) {
    const lang::SymbolRecord *parameterSymbol =
        frontend.semantics.database().findSymbol(parameterReference->symbol);
    expect(parameterSymbol != nullptr &&
               parameterSymbol->kind == lang::SymbolKind::Parameter &&
               parameterSymbol->name == "value",
           "resolved parameter uses should point to a compiler-owned symbol");
    expect(frontend.semantics.database()
                   .occurrencesForSymbol(parameterReference->symbol)
                   .size() >= 2,
           "symbol queries should connect a parameter declaration and use");
  }

  const auto counterMutation = std::find_if(
      frontend.semantics.database().occurrences(unit).begin(),
      frontend.semantics.database().occurrences(unit).end(),
      [](const lang::SemanticOccurrence &occurrence) {
        return occurrence.name == "counter" &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::Read) &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::Write);
      });
  expect(counterMutation !=
             frontend.semantics.database().occurrences(unit).end(),
         "increment operands should retain combined read/write roles");

  const std::optional<lang::DefinitionInfo> selectedDefinition =
      queries.definition(frontend, unit, call + 1);
  const std::size_t firstChoose = source.find("choose(uint64_t");
  expect(selectedDefinition &&
             selectedDefinition->target.start == firstChoose &&
             selectedDefinition->target.end ==
                 firstChoose + std::string("choose").size(),
         "definition should follow the exact selected overload");

  const std::optional<lang::DefinitionInfo> readonlyReceiverDefinition =
      queries.definition(frontend, unit, readonlyReceiverCall + 1);
  const std::optional<lang::DefinitionInfo> mutableReceiverDefinition =
      queries.definition(frontend, unit, mutableReceiverCall + 1);
  const std::size_t readonlyReceiverDeclaration = source.find("inspect()");
  const std::size_t mutableReceiverDeclaration =
      source.find("inspect() mut", readonlyReceiverDeclaration + 1);
  expect(readonlyReceiverDefinition && mutableReceiverDefinition &&
             readonlyReceiverDefinition->target.start ==
                 readonlyReceiverDeclaration &&
             mutableReceiverDefinition->target.start ==
                 mutableReceiverDeclaration,
         "definition should follow receiver-qualified method identities");

  const std::size_t aliasUse = source.find("gfx::render");
  const std::optional<lang::DefinitionInfo> aliasDefinition =
      queries.definition(frontend, unit, aliasUse + 1);
  const std::size_t aliasDeclaration =
      source.find("gfx", source.find("namespace gfx"));
  expect(aliasDefinition && aliasDefinition->target.start == aliasDeclaration,
         "definition should preserve namespace alias identity");

  const std::size_t aliasTarget =
      source.find("engine::graphics", aliasDeclaration);
  const std::optional<lang::DefinitionInfo> namespaceDefinition =
      queries.definition(frontend, unit, aliasTarget + 1);
  const std::size_t namespaceDeclaration =
      source.find("engine", source.find("namespace engine"));
  expect(namespaceDefinition &&
             namespaceDefinition->target.start == namespaceDeclaration,
         "definition on an alias target should resolve its namespace");

  const std::size_t valueParameterUse = source.find("[N]") + 1;
  const std::optional<lang::DefinitionInfo> valueParameterDefinition =
      queries.definition(frontend, unit, valueParameterUse);
  const std::size_t valueParameterDeclaration =
      source.find("N>", source.find("class Fixed"));
  expect(valueParameterDefinition && valueParameterDefinition->target.start ==
                                         valueParameterDeclaration,
         "definition should connect value-generic uses to their parameter");

  const std::size_t boxTypeUse = source.find("Box box");
  const std::optional<lang::DefinitionInfo> classDefinition =
      queries.definition(frontend, unit, boxTypeUse + 1);
  const std::size_t boxClass = source.find("Box", source.find("class Box"));
  expect(classDefinition && classDefinition->target.start == boxClass,
         "definition on a declared type should target the class declaration");

  const std::optional<lang::DefinitionInfo> constructorDefinition =
      queries.definition(frontend, unit, construction + 1);
  const std::size_t boxConstructor = source.find("Box(int value)");
  expect(constructorDefinition &&
             constructorDefinition->target.start == boxConstructor,
         "definition on a construction call should target its constructor");
  expect(!queries.definition(frontend, unit,
                             source.find("return 0") +
                                 std::string("return").size()),
         "definition should fail closed when no resolved symbol is present");

  const auto complete = [&](const std::string &completionSource,
                            std::string_view prefix) {
    const std::size_t offset = completionSource.rfind(prefix);
    expect(offset != std::string::npos,
           "completion test fixture should contain its cursor prefix");
    return queries.complete({.entryPath = "completion.gti",
                             .source = completionSource,
                             .byteOffset = offset + prefix.size()});
  };
  const auto findCandidate = [](const lang::CompletionResult &completion,
                                std::string_view label) {
    return std::find_if(completion.candidates.begin(),
                        completion.candidates.end(),
                        [label](const lang::CompletionCandidate &candidate) {
                          return candidate.label == label;
                        });
  };

  const std::string localSource =
      "int calculate(int parameter) { int local = 1; int sink = loc; "
      "return parameter; }";
  const lang::CompletionResult localCompletion = complete(localSource, "loc");
  const auto local = findCandidate(localCompletion, "local");
  expect(local != localCompletion.candidates.end() &&
             local->kind == lang::CompletionCandidateKind::Variable &&
             local->detail == "int32_t local",
         "unqualified completion should use live semantic locals and types");
  expect(local != localCompletion.candidates.end() &&
             local->replacementRange.start == localSource.rfind("loc") &&
             local->replacementRange.end ==
                 localSource.rfind("loc") + std::string("loc").size(),
         "completion should replace exactly the identifier prefix");

  const std::string namespaceSource =
      "namespace math { uint64_t power(uint64_t base, uint64_t exponent) { "
      "return "
      "base; } float power(float base, float exponent) { return base; } } "
      "int main() { uint64_t result = math::po; return 0; }";
  const lang::CompletionResult namespaceCompletion =
      complete(namespaceSource, "po");
  const std::size_t powerOverloads = static_cast<std::size_t>(
      std::count_if(namespaceCompletion.candidates.begin(),
                    namespaceCompletion.candidates.end(),
                    [](const lang::CompletionCandidate &candidate) {
                      return candidate.label == "power";
                    }));
  const auto power = findCandidate(namespaceCompletion, "power");
  expect(powerOverloads == 2 && power != namespaceCompletion.candidates.end() &&
             power->detail.find("math::power") != std::string::npos &&
             power->snippet &&
             power->snippet->find("${1:base}") != std::string::npos,
         "namespace completion should preserve overloads, GTI signatures, and "
         "parameter snippets");

  const std::string memberSource =
      "class Box { int hidden = 0; public: int read() { return this.hidden; } "
      "}; int main() { Box box{}; int result = box.rea; return 0; }";
  const lang::CompletionResult memberCompletion = complete(memberSource, "rea");
  expect(findCandidate(memberCompletion, "read") !=
                 memberCompletion.candidates.end() &&
             findCandidate(memberCompletion, "hidden") ==
                 memberCompletion.candidates.end(),
         "member completion should include accessible methods and hide private "
         "fields");

  const std::string enumSource =
      "enum class State { Ready, Running }; int main() { State state = "
      "State::Ru; return 0; }";
  const lang::CompletionResult enumCompletion = complete(enumSource, "Ru");
  const auto running = findCandidate(enumCompletion, "Running");
  expect(running != enumCompletion.candidates.end() &&
             running->kind == lang::CompletionCandidateKind::Enumerator &&
             std::all_of(enumCompletion.candidates.begin(),
                         enumCompletion.candidates.end(),
                         [](const lang::CompletionCandidate &candidate) {
                           return candidate.kind ==
                                  lang::CompletionCandidateKind::Enumerator;
                         }),
         "scoped-enum completion should filter and expose only enum members");
}

} // namespace

int main() {
  testFrontendBackendAndOptimizationPipeline();
  testMirControlFlowAndOwnershipEffects();
  testDefiniteReturnAnalysis();
  testSourceUnitDependencyGraph();
  testStandardLibraryImports();
  testOwnershipSemanticFoundation();
  testExplicitValueMoves();
  testTrustedIntrinsicDeclarations();
  testNonNullReferences();
  testReceiverTiedReferenceReturns();
  testStoredReferenceGroundwork();
  testUniqueOwnershipAndAllocation();
  testTypedHirGenericInstances();
  testCompilerPrivateStorage();
  testAggregateOwnershipTraits();
  testCompletePipeline();
  testLoopControlStatements();
  testDoWhileStatements();
  testConditionalExpressions();
  testSwitchStatements();
  testFixedWidthIntegers();
  testCharactersAndStringViews();
  testStandardString();
  testLogicalOperatorSpellings();
  testIntegerBitwiseAndModuloOperators();
  testCheckedArithmeticAndCompoundAssignments();
  testParserRecovery();
  testSemanticDiagnostics();
  testDiagnosticFoundation();
  testExecutablePathDiscovery();
  testDefaultImmutability();
  testStaticStorageAndMembers();
  testThisReceiverKeyword();
  testClassesStructsAndAccess();
  testConstructorsAndReceiverMutability();
  testInheritanceAndInterfaces();
  testDirectBraceConstruction();
  testDestructorsAndActiveDropState();
  testRestrictedMemberOperators();
  testCallableMemberOperators();
  testRangeBasedForAndIteratorProtocol();
  testNamedGenerics();
  testConstrainedGenerics();
  testSourceDefinedConcepts();
  testValueGenerics();
  testVariadicGenerics();
  testExactFunctionOverloadsAndConversions();
  testFixedArrays();
  testLocalTypeInference();
  testStructuredBindings();
  testLambdas();
  testNonEscapingCallableParameters();
  testNonEscapingCallablePredicates();
  testDefaultNodiscard();
  testExpectedValues();
  testPrintIsAnIdentifier();
  testTypeAliases();
  testNamespacesAndAliases();
  testCompileTimeConditionals();
  testRuntimeBackedStdlibSurface();
  testScopedEnums();
  testFormatting();
  testLanguageQueries();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }

  std::cout << "All compiler tests passed\n";
  return 0;
}
