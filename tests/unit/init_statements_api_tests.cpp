// Init statements (if/switch/range-for headers): the AST keeps the written
// init and its extent, HIR wraps it in an ordinary block, MIR drops it on
// continue/break, LLVM emits it, and the formatter keeps it in the header.
// Source-observable scope, cleanup and borrow behaviour is in
// tests/ui/language/init_statements/.
#include "../llvm_backend_test_support.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "unit_test.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool llvmAccepts(const lang::FrontendResult &frontend) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(
      frontend, lang::OptimizationLevel::O0, lang::TargetInfo::host(), &error);
  if (!emitted)
    std::cerr << "LLVM emission failed: " << error << '\n';
  return emitted;
}

void testInitStatements() {
  const std::string source = R"(
class Marker {
public:
  Marker(int id) : id(id) {}
  ~Marker() {}
  int value() { return this.id; }

private:
  int id;
};

int run(int limit) {
  mut int total = 0;
  for (mut int index = 0; index < limit; index++) {
    if (Marker marker = Marker(index); marker.value() == 1) {
      continue;
    } else if (int doubled = marker.value() * 2; doubled > 4) {
      break;
    }
    total += index;
  }
  switch (Marker selected = Marker(total); selected.value()) {
  case 1:
    return 1;
  default:
    break;
  }
  return total;
}

int main() { return run(5) - 2; }
)";

  // The AST keeps the written header: each init statement belongs to its
  // own statement, not to a parser-synthesized block.
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "init-statements.gti"));
  const lang::Program program = parser.parse();
  const lang::FunctionDecl *parsedRun = nullptr;
  for (const lang::StmtPtr &declaration : program.declarations())
    if (const auto *function =
            lang::stmtAs<lang::FunctionDecl>(declaration.get());
        function != nullptr && function->name().lexeme == "run")
      parsedRun = function;
  const lang::IfStmt *parsedIf = nullptr;
  const lang::SwitchStmt *parsedSwitch = nullptr;
  if (parsedRun != nullptr) {
    const auto &statements = parsedRun->body()->statements();
    if (const auto *loop = lang::stmtAs<lang::ForStmt>(statements[1].get())) {
      const auto *body = lang::stmtAs<lang::BlockStmt>(loop->body().get());
      parsedIf =
          body == nullptr
              ? nullptr
              : lang::stmtAs<lang::IfStmt>(body->statements().front().get());
    }
    parsedSwitch = lang::stmtAs<lang::SwitchStmt>(statements[2].get());
  }
  const auto *parsedInit =
      parsedIf == nullptr
          ? nullptr
          : lang::stmtAs<lang::VariableDecl>(parsedIf->initializer().get());
  const auto *elseIf =
      parsedIf == nullptr
          ? nullptr
          : lang::stmtAs<lang::IfStmt>(parsedIf->elseBranch().get());
  const std::size_t markerInit = source.find("Marker marker");
  expect(parser.errors().empty() && parsedInit != nullptr &&
             parsedInit->name().lexeme == "marker" &&
             parsedIf->initializer()->extent() &&
             parsedIf->initializer()->extent()->start == markerInit &&
             parsedIf->initializer()->extent()->end ==
                 source.find("; marker.value()") + 1 &&
             elseIf != nullptr && elseIf->initializer() != nullptr &&
             parsedSwitch != nullptr &&
             lang::stmtAs<lang::VariableDecl>(
                 parsedSwitch->initializer().get()) != nullptr,
         "if, else-if and switch headers should retain their written init "
         "statements and exact extents");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("init-statements.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected init-statement diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid,
         "init statements should pass the complete frontend and MIR pipeline");

  // HIR publishes the init's scope as an ordinary block around the
  // statement, so cleanup and exits follow the block contract.
  const lang::HirFunctionInstance *run = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr && instance.source->name().lexeme == "run") {
      run = &instance;
      break;
    }
  }
  const auto findStatement =
      [&](lang::HirStatementId id) -> const lang::HirStatement * {
    if (run == nullptr)
      return nullptr;
    for (const lang::HirStatement &statement : run->body.statements)
      if (statement.id == id)
        return &statement;
    return nullptr;
  };
  std::size_t wrapped = 0;
  const lang::HirStatement *ifScope = nullptr;
  if (run != nullptr) {
    for (const lang::HirStatement &statement : run->body.statements) {
      if (statement.kind != lang::HirStatementKind::Block ||
          lang::headerInitializer(statement.source) == nullptr)
        continue;
      const lang::HirStatement *init =
          statement.statements.size() == 2
              ? findStatement(statement.statements[0])
              : nullptr;
      const lang::HirStatement *inner =
          statement.statements.size() == 2
              ? findStatement(statement.statements[1])
              : nullptr;
      if (init != nullptr && inner != nullptr &&
          init->kind == lang::HirStatementKind::Variable &&
          init->source == lang::headerInitializer(statement.source)->get() &&
          inner->source == statement.source &&
          (inner->kind == lang::HirStatementKind::If ||
           inner->kind == lang::HirStatementKind::Switch))
        ++wrapped;
      if (inner != nullptr && inner->kind == lang::HirStatementKind::If &&
          ifScope == nullptr)
        ifScope = &statement;
    }
  }
  expect(wrapped == 3,
         "each if, else-if and switch init should lower to a block whose "
         "first statement is the init");

  const lang::MirFunctionInstance *loweredRun =
      run == nullptr ? nullptr : frontend.mir.findFunctionInstance(run->id);
  std::size_t controlDrops = 0;
  if (loweredRun != nullptr) {
    for (const lang::HirStatement &statement : run->body.statements) {
      if (statement.kind != lang::HirStatementKind::Continue &&
          statement.kind != lang::HirStatementKind::Break)
        continue;
      for (const lang::MirBlock &block : loweredRun->body.blocks)
        if (block.terminator.hirStatement == statement.id &&
            std::any_of(block.instructions.begin(), block.instructions.end(),
                        [](const lang::MirInstruction &instruction) {
                          return instruction.kind ==
                                 lang::MirInstructionKind::Drop;
                        }))
          ++controlDrops;
    }
  }
  expect(ifScope != nullptr && controlDrops == 2,
         "continue and break out of an if with an owning init should drop "
         "the init before leaving the loop iteration");
  expect(llvmAccepts(frontend),
         "the native backend should emit init statements from verified MIR");

  const std::string formatted = lang::Formatter().format(
      "int f(mut std::vector<int>& v){if(int a=1;a>0){return a;}else "
      "if(auto&r=v;r.size()>0){return 2;}switch(int b=2;b){default:break;}"
      "for(auto c=v;int x:c){}return 0;}");
  expect(formatted == R"(int f(mut std::vector<int>& v) {
  if (int a = 1; a > 0) {
    return a;
  } else if (auto& r = v; r.size() > 0) {
    return 2;
  }
  switch (int b = 2; b) {
  default:
    break;
  }
  for (auto c = v; int x : c) {}
  return 0;
}
)" && lang::Formatter().format(formatted) == formatted,
         "the formatter should keep init statements in their headers and "
         "remain idempotent");
}

} // namespace

GTI_UNIT_TEST(init_statements_api) {
  testInitStatements();
  return failures == 0 ? 0 : 1;
}
