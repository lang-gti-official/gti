#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/debug_info.h"
#include "../src/compiler/llvm/layout.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <iostream>
#include <stdexcept>
#include <utility>

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
        !std::cmp_equal(ir.getCurrentDebugLocation().getLine(),
                        row.source.line))
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

void inlinedLocations() {
  const auto frontend = Frontend().analyze("inline-debug.gti", R"(
int identity(int input) { return input; }
int caller(int input) { return identity(input); }
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid inline debug source");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host(), true);
  std::vector<LoweredProgramIssue> issues;
  if (!lowered.valid())
    throw std::runtime_error("invalid inline debug lowered source");
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified inline debug source");
  Session session(*input, {});
  DebugInfo debug(session);
  const MirFunctionInstance *calleeSource = nullptr, *callerSource = nullptr;
  for (const auto &source : input->program().mir().functionInstances()) {
    const auto *declaration =
        input->program().findFunctionDeclaration(source.declaration);
    if (declaration && declaration->qualifiedName == "identity")
      calleeSource = &source;
    else if (declaration && declaration->qualifiedName == "caller")
      callerSource = &source;
  }
  if (!calleeSource || !callerSource)
    throw std::runtime_error("lost inline source identities");
  auto *callee = session.abi().declareFunction(calleeSource->id);
  auto *caller = session.abi().declareFunction(callerSource->id);
  auto *integer = llvm::Type::getInt32Ty(session.context());
  if (callee->getReturnType() != integer ||
      caller->getReturnType() != integer || callee->arg_size() != 1 ||
      caller->arg_size() != 1 || callee->getArg(0)->getType() != integer ||
      caller->getArg(0)->getType() != integer)
    throw std::runtime_error(
        "inline metadata control changed its central scalar ABI");
  const MirBodyAddress calleeAddress{MirBodyKind::Function, calleeSource->id};
  const MirBodyAddress callerAddress{MirBodyKind::Function, callerSource->id};
  debug.begin(calleeAddress, *callee);
  debug.begin(callerAddress, *caller);
  auto *calleeScope = callee->getSubprogram();
  auto *callerScope = caller->getSubprogram();
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", callee));
  const auto &calleeLocations =
      input->program().findBody(calleeAddress)->sourceLocations;
  const auto &callerLocations =
      input->program().findBody(callerAddress)->sourceLocations;
  if (calleeLocations.empty() || callerLocations.empty())
    throw std::runtime_error(
        "inline source has no sealed instruction locations");
  debug.location(ir, calleeLocations.front().block,
                 calleeLocations.front().instruction);
  auto *storage = ir.CreateAlloca(integer);
  ir.CreateStore(callee->getArg(0), storage);
  bool parameter = false;
  for (const auto &place :
       findMirBody(input->program().mir(), calleeAddress)->places)
    if (place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
        place.binding == calleeSource->parameterBindings.front()) {
      debug.local(ir, place, storage, 1);
      parameter = true;
      break;
    }
  if (!parameter)
    throw std::runtime_error("inline control lost its sealed parameter place");
  ir.CreateRet(ir.CreateLoad(integer, storage));
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", caller));
  debug.location(ir, callerLocations.front().block,
                 callerLocations.front().instruction);
  auto *call = ir.CreateCall(callee, {caller->getArg(0)});
  const auto callLocation = call->getDebugLoc();
  ir.CreateRet(call);
  debug.finalize();
  // Controlled LLVM metadata transformation only, not a production inlining
  // policy or execution proof. No alwaysinline/semantic attributes are added.
  llvm::InlineFunctionInfo information;
  if (!llvm::InlineFunction(*call, information).isSuccess())
    throw std::runtime_error("LLVM rejected the inline debug control");
  bool found = false, parameterFound = false;
  for (const auto &block : *caller)
    for (const auto &instruction : block) {
      const auto location = instruction.getDebugLoc();
      if (location && location->getScope()->getSubprogram() == calleeScope) {
        const auto *chain = location->getInlinedAt();
        if (!chain || chain->getScope()->getSubprogram() != callerScope ||
            chain->getLine() != callLocation.getLine())
          throw std::runtime_error("inlining lost the caller source chain");
        found = true;
      }
      for (const auto &record : instruction.getDbgRecordRange()) {
        const auto *variable = llvm::dyn_cast<llvm::DbgVariableRecord>(&record);
        if (!variable || variable->getVariable()->getScope() != calleeScope)
          continue;
        const auto location = variable->getDebugLoc();
        if (variable->getVariable()->getArg() != 1 ||
            variable->getVariable()->getName() != "input" || !location ||
            !location->getInlinedAt() ||
            location->getInlinedAt()->getScope()->getSubprogram() !=
                callerScope)
          throw std::runtime_error("inlining lost the parameter source chain");
        parameterFound = true;
      }
    }
  if (!found || !parameterFound ||
      llvm::verifyModule(session.module(), &llvm::errs()))
    throw std::runtime_error("inlining lost callee source metadata");
}

int main() {
  if (metadata("/checkout-one/debug.gti") !=
      metadata("/other-checkout/debug.gti"))
    throw std::runtime_error(
        "debug metadata depends on an absolute checkout root");
  inlinedLocations();
  std::cout << "sealed debug identities, locations, locals, recursive layout, "
               "root relocation and inlined source/parameter chains passed\n";
}
