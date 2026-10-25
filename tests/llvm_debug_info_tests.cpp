#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/debug_info.h"
#include "../src/compiler/llvm/layout.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <stdexcept>

using namespace lang;
using namespace lang::native_llvm;

// Metadata component oracle only. These synthetic LLVM scopes are never
// published as generated GTI bodies or counted as native debugger execution.
std::string metadata(const std::string &path) {
  const auto frontend = Frontend().analyze(path, R"(
struct Node { mut Node* next = nullptr; mut int value = 7; };
enum class Mode : int64_t { Minus = -1, Plus = 7 };
extern "C" { int foreign(int input); }
int inspect(Node& owner, int input) {
  mut int local = input;
  Mode mode = Mode::Minus;
  auto text = "bytes";
  return local + owner.value;
}
int main() { Node node{}; return inspect(node, 4) == 11 ? 0 : 1; }
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid debug component source");
  std::vector<LoweredProgramIssue> issues;
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host(), true);
  if (!lowered.valid())
    throw std::runtime_error("invalid debug lowered source");
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified debug component source");
  Session session(*input, {});
  DebugInfo debug(session);
  const MirFunctionInstance *selected = nullptr;
  for (const auto &function : input->program().mir().functionInstances()) {
    const auto *declaration =
        input->program().findFunctionDeclaration(function.declaration);
    if (declaration && declaration->qualifiedName == "inspect")
      selected = &function;
  }
  if (!selected)
    throw std::runtime_error("missing debug component identity");
  const MirBodyAddress address{MirBodyKind::Function, selected->id};
  const auto *body = findMirBody(input->program().mir(), address);
  auto *function = session.abi().declareFunction(selected->id);
  auto *wrong = llvm::Function::Create(
      function->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
      "wrong_debug_identity", session.module());
  bool rejected = false;
  try {
    debug.begin(address, *wrong);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  if (!rejected || wrong->getSubprogram())
    throw std::runtime_error(
        "debug metadata accepted an unrelated LLVM function");
  bool declaration = false;
  for (const auto &candidate : input->program().mir().functionInstances())
    if (candidate.definitionKind == MirDefinitionKind::Declaration) {
      debug.begin({MirBodyKind::Function, candidate.id}, *wrong);
      declaration = true;
    }
  if (!declaration || wrong->getSubprogram())
    throw std::runtime_error("non-source declaration acquired a debug scope");
  const auto attributes = function->getAttributes();
  debug.begin(address, *function);
  if (attributes != function->getAttributes())
    throw std::runtime_error("debug info added execution attributes");
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  unsigned locals = 0, lines = 0;
  for (const auto &place : body->places) {
    if (place.root != MirPlaceRootKind::Binding || !place.projections.empty())
      continue;
    const auto *symbol = input->program().findSymbol(place.symbol);
    if (!symbol || (symbol->kind != SymbolKind::LocalVariable &&
                    symbol->kind != SymbolKind::Parameter))
      continue;
    auto *storage = ir.CreateAlloca(session.layouts().get(place.type).storage);
    ir.CreateStore(
        llvm::Constant::getNullValue(session.layouts().get(place.type).storage),
        storage);
    unsigned parameter = 0;
    for (unsigned index = 0; index < selected->parameterBindings.size();
         ++index)
      if (place.binding == selected->parameterBindings[index])
        parameter = index + 1;
    debug.local(ir, place, storage, parameter);
    debug.endLocal(ir, place);
    ++locals;
  }
  for (const auto &row : input->program().findBody(address)->sourceLocations) {
    debug.location(ir, row.block, row.instruction);
    if (!ir.getCurrentDebugLocation() ||
        ir.getCurrentDebugLocation().getLine() != row.source.line)
      throw std::runtime_error("sealed source location was lost");
    ++lines;
  }
  debug.location(ir, 999999, 999999);
  if (!ir.getCurrentDebugLocation() ||
      ir.getCurrentDebugLocation().getLine() != 0)
    throw std::runtime_error(
        "generated routing inherited a stale user source line");
  if (function->getReturnType()->isVoidTy())
    ir.CreateRetVoid();
  else
    ir.CreateRet(llvm::Constant::getNullValue(function->getReturnType()));
  debug.finalize();
  debug.finalize();
  const bool invalid = llvm::verifyModule(session.module(), &llvm::errs());
  if (locals != 5 || !lines || invalid) {
    std::cerr << "locals=" << locals << " lines=" << lines
              << " invalid=" << invalid << '\n';
    throw std::runtime_error("invalid local/recursive type debug metadata");
  }
  std::string text;
  llvm::raw_string_ostream output(text);
  // Raw IR provenance is not DWARF. Compare debug paths without the deliberate
  // source-ownership attachment used independently by the ABI boundary.
  function->setMetadata("gti.source", nullptr);
  session.module().print(output, nullptr);
  if (text.find(path) != std::string::npos ||
      text.find("directory: \".\"") == std::string::npos ||
      text.find("name: \"Node\"") == std::string::npos ||
      text.find("name: \"local\"") == std::string::npos ||
      text.find("name: \"Minus\", value: -1") == std::string::npos ||
      text.find("name: \"Plus\", value: 7") == std::string::npos ||
      text.find("name: \"string_view\"") == std::string::npos ||
      text.find("name: \"uint8_t\", size: 8") == std::string::npos)
    throw std::runtime_error(
        "debug metadata leaks a host path or loses a source type/name");
  return text;
}

int main() {
  if (metadata("/checkout-one/debug.gti") !=
      metadata("/other-checkout/debug.gti"))
    throw std::runtime_error(
        "debug metadata depends on an absolute checkout root");
  std::cout << "sealed debug identities, locations, locals, recursive layout "
               "and root relocation passed\n";
}
