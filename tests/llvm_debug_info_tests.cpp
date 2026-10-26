#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/debug_info.h"
#include "../src/compiler/llvm/layout.h"
#include "../src/compiler/llvm/parameter_packs.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

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
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze(path, R"(
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
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid debug lowered source");
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified debug component source");
  Session session(*input,
                  {.optimization = LlvmCodegenOptimization::Aggressive});
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
  if (!session.hasSourceDefinition(selected->id, *function))
    throw std::runtime_error("new source declaration lost its exact identity");
  {
    Session reused(*input, {});
    DebugInfo reusedDebug(reused);
    const auto plan = reused.abi().gti(
        selected->returnType, selected->parameterTypes,
        selected->mayRaiseDefinedFailure, false, selected->tracksCaller);
    auto *existing =
        llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                               function->getName(), reused.module());
    plan.applyTo(*existing);
    bool forgeryRejected = false;
    try {
      reusedDebug.begin(address, *existing);
    } catch (const std::logic_error &) {
      forgeryRejected = true;
    }
    if (!forgeryRejected || existing->getSubprogram())
      throw std::runtime_error(
          "a matching source symbol forged debug identity");
    if (reused.abi().declareFunction(selected->id) != existing ||
        !reused.hasSourceDefinition(selected->id, *existing))
      throw std::runtime_error("reused source declaration lost exact identity");
    reusedDebug.begin(address, *existing);
    llvm::IRBuilder<> body(
        llvm::BasicBlock::Create(reused.context(), "entry", existing));
    if (existing->getReturnType()->isVoidTy())
      body.CreateRetVoid();
    else
      body.CreateRet(llvm::Constant::getNullValue(existing->getReturnType()));
    reusedDebug.finalize();
    if (!existing->getSubprogram() ||
        llvm::verifyModule(reused.module(), &llvm::errs()))
      throw std::runtime_error("reused source metadata did not verify");
  }
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
  const auto executionName = function->getName().str();
  debug.begin(address, *function);
  if (attributes != function->getAttributes())
    throw std::runtime_error("debug info added execution attributes");
  if (function->getName() != executionName ||
      function->getSubprogram()->getName() != "inspect" ||
      !function->getSubprogram()->getLinkageName().empty())
    throw std::runtime_error(
        "source debugger names changed or exposed private execution symbols");
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
  // This metadata-only unit supplies typed stand-ins; real values are checked
  // separately through production lowering and native debugger execution.
  std::vector<llvm::Value *> debugValues(body->values.size() + 1);
  for (const auto &range :
       input->program().findBody(address)->sourceDebug.tables->valueRanges)
    debugValues.at(range.value) = llvm::Constant::getNullValue(
        session.layouts().get(body->findValue(range.value)->info.type).value);
  const auto &valueRanges =
      input->program().findBody(address)->sourceDebug.tables->valueRanges;
  if (!valueRanges.empty()) {
    const auto &range = valueRanges.front();
    const auto rejects = [&](std::span<llvm::Value *const> values) {
      try {
        debug.point(ir, range.begin, values);
      } catch (const std::logic_error &) {
        return;
      }
      throw std::runtime_error("forged LLVM debug value was accepted");
    };
    rejects({});
    auto forged = debugValues;
    forged.at(range.value) = llvm::ConstantFP::get(ir.getDoubleTy(), 0);
    rejects(forged);
    bool foreign = false;
    for (auto &argument : wrong->args())
      if (argument.getType() == debugValues.at(range.value)->getType()) {
        forged.at(range.value) = &argument;
        rejects(forged);
        foreign = true;
        break;
      }
    if (!foreign)
      throw std::runtime_error("foreign-function SSA control did not execute");
  }
  for (const auto &block : body->blocks) {
    debug.point(ir, {block.id, 0, false, true}, debugValues);
    for (const auto &instruction : block.instructions) {
      debug.point(ir, {block.id, instruction.id, false}, debugValues);
      debug.point(ir, {block.id, instruction.id, true}, debugValues);
    }
    debug.point(ir, {block.id, 0, false}, debugValues);
  }
  for (const auto &row :
       input->program().findBody(address)->sourceLocations()) {
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
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze("inline-debug.gti", R"(
int identity(int input) { return input; }
int caller(int input) { return identity(input); }
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid inline debug source");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  std::vector<LoweredProgramIssue> issues;
  if (!lowered.valid())
    throw std::runtime_error("invalid inline debug lowered source");
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified inline debug source");
  Session session(*input,
                  {.optimization = LlvmCodegenOptimization::Aggressive});
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
      input->program().findBody(calleeAddress)->sourceLocations();
  const auto &callerLocations =
      input->program().findBody(callerAddress)->sourceLocations();
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
  const auto &calleeRanges =
      input->program().findBody(calleeAddress)->sourceDebug.tables->valueRanges;
  if (calleeRanges.empty())
    throw std::runtime_error(
        "inline parameter has no verified readable interval");
  std::vector<llvm::Value *> calleeValues(calleeSource->body.values.size() + 1);
  calleeValues.at(calleeRanges.front().value) = callee->getArg(0);
  debug.point(ir, calleeRanges.front().begin, calleeValues);
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

void lifecycleIdentities() {
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze("/scope/lifecycle.gti", R"(
class Sample { public: Sample() {} ~Sample() {} };
struct Generated { int value = 7; };
int main() { Sample value{}; Generated generated{}; return generated.value == 7 ? 0 : 1; }
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid lifecycle debug source");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid lifecycle debug lowering");
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("invalid lifecycle debug snapshot");
  Session session(*input, {});
  DebugInfo debug(session);
  unsigned constructors = 0, destructors = 0, excluded = 0, controls = 0;
  llvm::Function *constructorFunction = nullptr;
  const auto reject = [&](auto operation) {
    bool rejected = false;
    try {
      operation();
    } catch (const std::logic_error &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error(
          "malformed lifecycle debug identity was accepted");
    ++controls;
  };
  const auto check = [&](MirBodyAddress address, const FunctionAbi &plan,
                         bool source) {
    auto *function =
        llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                               "metadata_lifecycle", session.module());
    plan.applyTo(*function);
    if (!source) {
      debug.registerLifecycleBody(address, *function);
      debug.begin(address, *function);
      if (function->getSubprogram())
        throw std::runtime_error(
            "generated lifecycle acquired a user source scope");
      ++excluded;
      return function;
    }
    auto *wrong =
        llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                               "unrelated_lifecycle", session.module());
    reject([&] { debug.begin(address, *wrong); });
    debug.registerLifecycleBody(address, *function);
    debug.registerLifecycleBody(
        address, *function); // Reusing the same factory result is lawful.
    reject([&] { debug.registerLifecycleBody(address, *wrong); });
    reject([&] { debug.begin(address, *wrong); });
    if (wrong->getSubprogram())
      throw std::runtime_error("wrong lifecycle retained source metadata");
    debug.begin(address, *function);
    if (!function->getSubprogram())
      throw std::runtime_error("exact lifecycle lost its source metadata");
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    if (function->getReturnType()->isVoidTy())
      ir.CreateRetVoid();
    else
      ir.CreateRet(llvm::ConstantInt::getTrue(session.context()));
    return function;
  };
  for (const auto &row : input->program().mir().constructorInstances()) {
    const bool source = row.definitionKind == MirDefinitionKind::Source &&
                        !row.generatedDefault;
    auto *function =
        check({MirBodyKind::Constructor, row.id},
              session.abi().gti(SemanticType::Void, row.parameterTypes,
                                row.mayRaiseDefinedFailure, true),
              source);
    if (source) {
      ++constructors;
      constructorFunction = function;
    }
  }
  for (const auto &row : input->program().mir().destructorInstances()) {
    const bool source = row.definitionKind == MirDefinitionKind::Source;
    if (source && constructorFunction)
      reject([&] {
        debug.registerLifecycleBody({MirBodyKind::Destructor, row.id},
                                    *constructorFunction);
      });
    check({MirBodyKind::Destructor, row.id},
          session.abi().gti(SemanticType::Void, {}, row.mayRaiseDefinedFailure,
                            true),
          source);
    if (source)
      ++destructors;
  }
  llvm::Module foreign("foreign_debug_session", session.context());
  auto *outside = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::ExternalLinkage, "outside", foreign);
  const auto &first = input->program().mir().constructorInstances().front();
  reject([&] {
    debug.registerLifecycleBody({MirBodyKind::Constructor, first.id}, *outside);
  });
  if (!constructors || !destructors || !excluded)
    throw std::runtime_error(
        "lifecycle identity or generated-exclusion controls did not execute");
  debug.finalize();
  if (llvm::verifyModule(session.module(), &llvm::errs()))
    throw std::runtime_error("lifecycle debug metadata did not verify");
  std::cout << constructors << " source constructors, " << destructors
            << " source destructors, " << excluded
            << " generated exclusions and " << controls
            << " identity controls passed\n";
}

void generatedMetadataExclusion() {
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze("generated-debug.gti", R"(
int main() {
  auto make = []() -> nullptr_t { return nullptr; };
  return make() == nullptr ? 0 : 1;
}
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid generated debug source");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  std::vector<LoweredProgramIssue> issues;
  if (!lowered.valid())
    throw std::runtime_error("invalid generated debug lowered source");
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified generated debug source");
  Session session(*input, {});
  DebugInfo debug(session);
  unsigned excluded = 0, unrepresented = 0;
  for (const auto &source : input->program().mir().lambdaInstances()) {
    const MirBodyAddress address{MirBodyKind::Lambda, source.id};
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                false),
        llvm::GlobalValue::InternalLinkage,
        "generated_debug_control_" + std::to_string(source.id),
        session.module());
    debug.begin(address, *function);
    if (function->getSubprogram())
      throw std::runtime_error(
          "generated body acquired a fabricated source scope");
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    ir.CreateRetVoid();
    ++excluded;
    if (source.returnType.kind == SemanticType::NullPtr)
      ++unrepresented;
  }
  debug.finalize();
  if (excluded != 1 || unrepresented != 1 ||
      llvm::verifyModule(session.module(), &llvm::errs()))
    throw std::runtime_error("generated metadata exclusion did not execute");
  std::cout << excluded
            << " generated bodies excluded before planning source types; "
            << unrepresented << " unavailable return-type control passed\n";
}

void concreteParameterMetadata() {
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze("pack-debug.gti", R"(
int first<Args...>(int input, Args... rest) { return input; }
int main() { return first(7) + first(7, 9, 11) == 14 ? 0 : 1; }
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid parameter debug source");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  std::vector<LoweredProgramIssue> issues;
  if (!lowered.valid())
    throw std::runtime_error("invalid parameter debug lowered source");
  auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified parameter debug source");
  Session session(*input,
                  {.optimization = LlvmCodegenOptimization::Aggressive});
  DebugInfo debug(session);
  unsigned empty = 0, expanded = 0, unavailable = 0;
  for (const auto &source : input->program().mir().functionInstances()) {
    const auto *declaration =
        input->program().findFunctionDeclaration(source.declaration);
    if (!declaration || declaration->qualifiedName != "first")
      continue;
    const MirBodyAddress address{MirBodyKind::Function, source.id};
    const auto plan = planConcreteParameters(
        source.parameterTypes, source.parameterBindings, source.body);
    if (!plan || plan->packs.size() != 1 ||
        (plan->lanes.size() != 1 && plan->lanes.size() != 3))
      throw std::runtime_error("debug fixture lost its exact trailing pack");
    auto *function = session.abi().declareFunction(source.id);
    debug.begin(address, *function);
    const auto types = function->getSubprogram()->getType()->getTypeArray();
    if (types.size() != plan->lanes.size() + 1)
      throw std::runtime_error("debug signature lost concrete parameter types");
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    const MirPlace *prefix = nullptr;
    for (const auto &place : source.body.places)
      if (place.root == MirPlaceRootKind::Binding &&
          place.binding == plan->lanes.front().binding &&
          place.projections.empty()) {
        const auto *symbol = input->program().findSymbol(place.symbol);
        if (symbol && symbol->kind == SymbolKind::Parameter &&
            symbol->name == "input")
          prefix = &place;
      }
    if (!prefix || plan->lanes.front().packElement)
      throw std::runtime_error("debug fixture lost its ordinary prefix");
    auto *storage =
        ir.CreateAlloca(session.layouts().get(prefix->type).storage);
    ir.CreateStore(function->getArg(0), storage);
    debug.local(ir, *prefix, storage, 1);
    const auto &tables =
        *input->program().findBody(address)->sourceDebug.tables;
    const auto &ranges = tables.valueRanges;
    const auto range =
        std::find_if(ranges.begin(), ranges.end(), [&](const auto &row) {
          return tables.locals.at(row.local - 1).place == prefix->id;
        });
    if (range == ranges.end())
      throw std::runtime_error(
          "prefix parameter has no fresh readable interval");
    std::vector<llvm::Value *> prefixValues(source.body.values.size() + 1);
    prefixValues.at(range->value) = function->getArg(0);
    debug.point(ir, range->begin, prefixValues);
    debug.endLocal(ir, *prefix);
    ir.CreateRet(ir.getInt32(7));
    unsigned declarations = 0, ends = 0;
    for (const auto &instruction : function->getEntryBlock())
      for (const auto &record : instruction.getDbgRecordRange())
        if (const auto *variable =
                llvm::dyn_cast<llvm::DbgVariableRecord>(&record)) {
          if (variable->getVariable()->getName() != "input" ||
              variable->getVariable()->getArg() != 1)
            throw std::runtime_error(
                "pack or hidden lane acquired a source name");
          if (variable->isDbgDeclare() ||
              (variable->isDbgValue() && !variable->isKillLocation()))
            ++declarations;
          else if (llvm::isa<llvm::PoisonValue>(variable->getValue()))
            ++ends;
        }
    if (declarations != 1 || ends != 1)
      throw std::runtime_error(
          "parameter value-unavailable control did not execute");
    unavailable += ends;
    (plan->lanes.size() == 1 ? empty : expanded)++;
  }
  debug.finalize();
  if (empty != 1 || expanded != 1 || unavailable != 2 ||
      llvm::verifyModule(session.module(), &llvm::errs()))
    throw std::runtime_error("parameter metadata controls did not verify");
  std::cout << empty << " empty and " << expanded
            << " expanded trailing packs preserve prefix-only named metadata; "
            << unavailable << " parameter unavailable controls passed\n";
}

void optimizedOutDeclarations() {
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze("optimized-debug.gti",
                   "int probe(int input) { mut int local = input & 31; return "
                   "local; }");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid optimized debug fixture");
  const auto optimized =
      OptimizationPipeline().run({.hir = frontend.hir,
                                  .mir = frontend.mir,
                                  .level = OptimizationLevel::O3,
                                  .target = TargetInfo::host()});
  if (!optimized.valid())
    throw std::runtime_error("invalid optimized debug MIR");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid optimized debug lowering");
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(std::move(*lowered.program), issues);
  if (!input)
    throw std::runtime_error("invalid optimized debug snapshot");
  Session session(*input,
                  {.optimization = LlvmCodegenOptimization::Aggressive});
  DebugInfo debug(session);
  llvm::Function *selected = nullptr;
  bool removed = false;
  for (const auto &source : input->program().mir().functionInstances()) {
    if (input->program()
            .findFunctionDeclaration(source.declaration)
            ->qualifiedName != "probe")
      continue;
    const MirBodyAddress address{MirBodyKind::Function, source.id};
    const auto &tables =
        *input->program().findBody(address)->sourceDebug.tables;
    for (const auto &local : tables.locals)
      if (local.declaration.name == "local")
        removed =
            local.policy == MirSourceLocalPolicy::OptimizedOut && !local.place;
    selected = session.abi().declareFunction(source.id);
    debug.begin(address, *selected);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", selected));
    if (selected->getReturnType()->isVoidTy())
      ir.CreateRetVoid();
    else
      ir.CreateRet(llvm::Constant::getNullValue(selected->getReturnType()));
  }
  debug.finalize();
  bool retained = false;
  if (selected)
    for (const auto *node : selected->getSubprogram()->getRetainedNodes())
      if (const auto *local = llvm::dyn_cast<llvm::DILocalVariable>(node))
        retained |= local->getName() == "local";
  if (!removed || !retained ||
      llvm::verifyModule(session.module(), &llvm::errs()))
    throw std::runtime_error(
        "optimized-out local lost its declaration or gained invalid metadata");
  for (const auto &block : *selected)
    for (const auto &instruction : block)
      if (!instruction.getDbgRecordRange().empty() ||
          llvm::isa<llvm::AllocaInst>(instruction))
        throw std::runtime_error("optimized-out declaration fabricated value "
                                 "storage or availability");
}

void hostedArtificialCarrier() {
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze("startup-debug.gti", "int main() { return 0; }");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  std::vector<LoweredProgramIssue> issues;
  if (!lowered.valid())
    throw std::runtime_error("invalid startup debug fixture");
  auto input = verifiedLoweredProgram(std::move(*lowered.program), issues);
  if (!input)
    throw std::runtime_error("invalid startup debug snapshot");
  Session session(*input, {.debugInfo = true});
  auto &debug = *session.debugInfo();
  const MirBodyAddress address{
      MirBodyKind::HostedStartup,
      input->program().mir().hostedStartupPlan()->entry};
  auto *alien = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(session.context()), false),
      llvm::GlobalValue::ExternalLinkage, "forged_startup", session.module());
  const auto rejects = [](auto action) {
    try {
      action();
    } catch (const std::logic_error &) {
      return;
    }
    throw std::runtime_error("forged startup identity was accepted");
  };
  rejects([&] { debug.begin(address, *alien); });
  lowerScalarProgram(session, planLlvmAdmission(*input));
  auto *entry = session.module().getFunction("main");
  if (!entry || !entry->getSubprogram())
    throw std::runtime_error("registered startup lost its artificial carrier");
  const auto *scope = entry->getSubprogram();
  if (!(scope->getFlags() & llvm::DINode::FlagArtificial) || scope->getLine() ||
      scope->getScopeLine() ||
      scope->getFile()->getFilename() != "<gti-generated>")
    throw std::runtime_error("startup acquired a fabricated source scope");
  rejects([&] { debug.begin(address, *alien); });
  rejects([&] { debug.registerHostedStartupBody(address, *alien); });
  rejects([&] {
    debug.begin({MirBodyKind::HostedStartup, address.owner + 1}, *entry);
  });
  rejects([&] {
    debug.registerHostedStartupBody({MirBodyKind::Function, address.owner},
                                    *entry);
  });
  llvm::IRBuilder<> ir(&entry->getEntryBlock());
  rejects([&] {
    debug.location(ir, {MirBodyKind::Function, address.owner}, 1, 0);
  });
  unsigned calls = 0;
  for (const auto &block : *entry)
    for (const auto &instruction : block) {
      if (const auto &location = instruction.getDebugLoc())
        if (location.getLine() || location->getScope() != scope ||
            location->getInlinedAt())
          throw std::runtime_error(
              "generated startup acquired a user source location");
      if (llvm::isa<llvm::CallBase>(instruction)) {
        ++calls;
        if (!instruction.getDebugLoc())
          throw std::runtime_error(
              "startup call lost its line-zero inline carrier");
      }
    }
  debug.finalize();
  if (!calls || scope->getRetainedNodes().size() || alien->getSubprogram() ||
      llvm::verifyModule(session.module(), &llvm::errs()))
    throw std::runtime_error("artificial startup metadata failed verification");
}

void productionValueEmission(const std::string &dumpPath,
                             bool receiver = false) {
  const auto frontend =
      Frontend({.sourceDebugProfile = SourceDebugProfile::LexicalV1})
          .analyze("value-emission.gti", receiver ? R"gti(
extern "C" { int32_t debug_input(); void debug_checkpoint(int32_t value); }
class Receiver {
public:
  int32_t value = 4;
  int32_t inspect(int32_t input) {
    mut int32_t local = this.value & input;
    debug_checkpoint(local);
    return local;
  }
};
int main() { Receiver owner{}; return owner.inspect(debug_input()) == 4 ? 0 : 1; }
)gti"
                                                  : R"gti(
extern "C" { int32_t debug_input(); void debug_checkpoint(int32_t value); }
int32_t inspect(int32_t input) {
  mut int32_t local = input & 31;
  debug_checkpoint(local);
  return local;
}
int main() { return inspect(debug_input()) == 4 ? 0 : 1; }
)gti");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid production value fixture");
  const auto optimized =
      OptimizationPipeline().run({.hir = frontend.hir,
                                  .mir = frontend.mir,
                                  .level = OptimizationLevel::O3,
                                  .target = TargetInfo::host()});
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid production value lowering");
  std::vector<LoweredProgramIssue> issues;
  auto input = verifiedLoweredProgram(std::move(*lowered.program), issues);
  if (!input)
    throw std::runtime_error("invalid production value snapshot");
  Session session(*input, {.optimization = LlvmCodegenOptimization::Aggressive,
                           .debugInfo = true});
  Session plain(*input, {.optimization = LlvmCodegenOptimization::Aggressive});
  lowerScalarProgram(session, planLlvmAdmission(*input));
  lowerScalarProgram(plain, planLlvmAdmission(*input));
  unsigned values = 0, receivers = 0;
  for (const auto &function : session.module()) {
    const auto *other = plain.module().getFunction(function.getName());
    if (!other || function.size() != other->size() ||
        function.arg_size() != other->arg_size())
      throw std::runtime_error(
          "debug value events changed execution functions");
    for (unsigned i = 0; i <= function.arg_size(); ++i)
      if (function.getAttributes().getAsString(i) !=
          other->getAttributes().getAsString(i))
        throw std::runtime_error(
            "debug value events changed execution parameter attributes");
    if (function.getAttributes().getAsString(
            llvm::AttributeList::FunctionIndex) !=
        other->getAttributes().getAsString(llvm::AttributeList::FunctionIndex))
      throw std::runtime_error(
          "debug value events changed execution function attributes");
    auto otherBlock = other->begin();
    for (const auto &block : function) {
      if (block.size() != otherBlock->size())
        throw std::runtime_error(
            "debug value events added execution instructions");
      auto otherInstruction = otherBlock->begin();
      for (const auto &instruction : block) {
        if (instruction.getOpcode() != otherInstruction->getOpcode())
          throw std::runtime_error(
              "debug value events changed execution opcodes");
        for (const auto &record : instruction.getDbgRecordRange()) {
          if (const auto *variable =
                  llvm::dyn_cast<llvm::DbgVariableRecord>(&record);
              variable && variable->isDbgValue() &&
              !variable->isKillLocation() &&
              variable->getVariable()->getName() == "local")
            ++values;
          if (const auto *variable =
                  llvm::dyn_cast<llvm::DbgVariableRecord>(&record);
              variable && !variable->isKillLocation() &&
              variable->getVariable()->getName() == "this") {
            if (variable->getVariable()->getLine() != 0 ||
                !variable->getVariable()->isArtificial())
              throw std::runtime_error(
                  "implicit receiver forged a source declaration");
            ++receivers;
          }
        }
        ++otherInstruction;
      }
      ++otherBlock;
    }
  }
  if (!values)
    throw std::runtime_error(
        "production local has no existing SSA value metadata");
  if (receiver && !receivers)
    throw std::runtime_error(
        "production receiver has no existing address metadata");
  session.debugInfo()->finalize();
  const auto dump = [&](const std::string &path) {
    if (path.empty())
      return;
    std::error_code error;
    llvm::raw_fd_ostream output(path, error);
    if (error)
      throw std::runtime_error("cannot retain debug IR");
    session.module().print(output, nullptr);
  };
  dump(dumpPath);
  (void)session.emitObject();
  dump(dumpPath.empty() ? "" : dumpPath + ".optimized.ll");
}

int main(int argc, char **argv) {
  if (metadata("/checkout-one/debug.gti") !=
      metadata("/other-checkout/debug.gti"))
    throw std::runtime_error(
        "debug metadata depends on an absolute checkout root");
  inlinedLocations();
  lifecycleIdentities();
  concreteParameterMetadata();
  generatedMetadataExclusion();
  optimizedOutDeclarations();
  hostedArtificialCarrier();
  productionValueEmission(argc > 1 ? argv[1] : "");
  productionValueEmission(argc > 1 ? std::string(argv[1]) + ".receiver.ll" : "", true);
  std::cout << "sealed debug identities, locations, locals, recursive layout, "
               "root relocation and inlined source/parameter chains passed\n";
}
