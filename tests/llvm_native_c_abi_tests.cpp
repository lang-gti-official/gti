#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/coverage.h"
#include "../src/compiler/llvm/failure.h"
#include "../src/compiler/llvm/native_c_abi.h"
#include "../src/compiler/llvm/native_c_contract.h"
#include "../src/compiler/llvm/reachability.h"
#include "cpp_backend_test_support.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace {
using namespace lang;
using namespace lang::native_llvm;

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

template <typename Exception = BackendUnsupportedError, typename Action>
void rejects(Action action) {
  try {
    action();
  } catch (const Exception &) {
    return;
  }
  throw std::runtime_error("expected explicit classifier rejection");
}

VerifiedLoweredProgram fixture(const std::filesystem::path &source,
                               bool concurrent = false,
                               bool standardLibraryEnabled = false) {
  std::ifstream stream(source);
  require(stream.good(), "missing GTI fixture");
  const std::string text{std::istreambuf_iterator<char>(stream), {}};
  FrontendOptions options;
  if (concurrent) {
    options.target.executionProfile = ExecutionProfile::Concurrent;
    options.target.runtimeCapabilities.threads = true;
  }
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend =
      standardLibraryEnabled
          ? Frontend(options).analyze(source.string(), text,
                                      {standardLibrary / "prelude.gti"}, {},
                                      {standardLibrary})
          : Frontend(options).analyze(source.string(), text);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("invalid GTI record fixture");
  }
  std::vector<LoweredProgramIssue> issues;
  auto verified = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(verified.has_value(), "unverified GTI record fixture");
  return *verified;
}

const MirClassInstance &record(Session &session, const std::string &name) {
  for (const auto &instance : session.program().mir().classInstances()) {
    const auto *decl =
        session.program().findClassDeclaration(instance.declaration);
    if (decl && decl->qualifiedName == name)
      return instance;
  }
  throw std::runtime_error("missing concrete record: " + name);
}

llvm::Value *offset(llvm::IRBuilder<> &builder, llvm::Value *pointer,
                    std::uint64_t bytes) {
  return builder.CreateGEP(builder.getInt8Ty(), pointer,
                           builder.getInt64(bytes));
}

struct Probe {
  std::string recordName;
  std::string symbol;
  unsigned integerPrefix = 0;
  unsigned vectorPrefix = 0;
  bool integerTail = false;
  bool vectorTail = false;
};

void checkImportContract(const FunctionAbi &plan) {
  const std::unique_ptr<llvm::Function> function(llvm::Function::Create(
      plan.type, llvm::Function::ExternalLinkage, "native_import_contract"));
  plan.applyTo(*function);
  verifyNativeCImport(*function, plan);
  const auto attributes = function->getAttributes();
  function->setCallingConv(llvm::CallingConv::Fast);
  rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
  function->setCallingConv(llvm::CallingConv::C);
  function->setLinkage(llvm::Function::InternalLinkage);
  rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
  function->setLinkage(llvm::Function::ExternalLinkage);
  auto *entry = llvm::BasicBlock::Create(plan.type->getContext(), "entry",
                                         function.get());
  rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
  entry->eraseFromParent();
  function->addFnAttr(llvm::Attribute::NoUnwind);
  rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
  function->setAttributes(attributes);
  if (plan.resultExtension != AbiExtension::None) {
    function->removeRetAttr(plan.resultExtension == AbiExtension::Signed
                                ? llvm::Attribute::SExt
                                : llvm::Attribute::ZExt);
    rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
    function->setAttributes(attributes);
  }
  for (unsigned index = 0; index < plan.parameters.size(); ++index)
    if (plan.parameters[index].extension != AbiExtension::None) {
      function->removeParamAttr(index, plan.parameters[index].extension ==
                                               AbiExtension::Signed
                                           ? llvm::Attribute::SExt
                                           : llvm::Attribute::ZExt);
      rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
      function->setAttributes(attributes);
    }
  for (const auto &memory : plan.nativeMemory) {
    function->removeParamAttr(memory.index, llvm::Attribute::Alignment);
    rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
    function->setAttributes(attributes);
    if (memory.result || memory.byValue) {
      function->removeParamAttr(memory.index, memory.result
                                                  ? llvm::Attribute::StructRet
                                                  : llvm::Attribute::ByVal);
      rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
      function->setAttributes(attributes);
      const auto wrongStorage =
          memory.result ? llvm::Attribute::getWithStructRetType(
                              plan.type->getContext(),
                              llvm::Type::getInt8Ty(plan.type->getContext()))
                        : llvm::Attribute::getWithByValType(
                              plan.type->getContext(),
                              llvm::Type::getInt8Ty(plan.type->getContext()));
      function->addParamAttr(memory.index, wrongStorage);
      rejects<std::logic_error>([&] { verifyNativeCImport(*function, plan); });
      function->setAttributes(attributes);
    }
  }
  verifyNativeCImport(*function, plan);
}

void emitProbe(Session &session, const Probe &probe) {
  const auto &instance = record(session, probe.recordName);
  const auto prefix = probe.integerPrefix + probe.vectorPrefix;
  const auto plan =
      classifyNativeCRecord(session, instance.type,
                            {6 - probe.integerPrefix, 8 - probe.vectorPrefix});
  const bool sysv =
      llvm::Triple(session.module().getTargetTriple()).isOSLinux();
  const bool pressure = prefix != 0;
  require(plan.argument.byValue == (sysv && pressure),
          "System V pressure must roll back whole aggregate to byval stack");
  if (sysv && pressure)
    require(plan.remaining.integers == 6 - probe.integerPrefix &&
                plan.remaining.vectors == 8 - probe.vectorPrefix,
            "stack rollback must preserve remaining registers for later args");
  const std::vector<SemanticType> sourceArguments{instance.type};
  require(session.abi().nativeC(instance.type, sourceArguments).type != nullptr,
          "one native planner supplies concrete import/export signatures");

  std::vector<SemanticType> sourceTypes(probe.integerPrefix,
                                        SemanticType::UInt64);
  sourceTypes.insert(sourceTypes.end(), probe.vectorPrefix,
                     SemanticType::Double);
  sourceTypes.push_back(instance.type);
  if (probe.integerTail)
    sourceTypes.push_back(SemanticType::UInt64);
  if (probe.vectorTail)
    sourceTypes.push_back(SemanticType::Double);
  const auto nativePlan =
      planNativeCFunction(session, instance.type, sourceTypes);
  checkImportContract(nativePlan.signature);
  auto *signature = nativePlan.signature.type;
  auto *callee =
      llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                             "c_" + probe.symbol, session.module());
  auto *function =
      llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                             "llvm_" + probe.symbol, session.module());
  nativePlan.signature.applyTo(*function);
  nativePlan.signature.applyTo(*callee);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  const auto incoming =
      decodeNativeCArguments(session, builder, nativePlan, *function);
  auto *local = incoming.at(prefix);
  const auto &fields = instance.cAbiLayout->fields;
  for (unsigned index = 0; index < fields.size(); ++index) {
    const auto &field = fields[index];
    auto *fieldType = session.layouts().get(field.type).storage;
    auto *address = offset(builder, local, field.offsetBytes);
    auto *value = builder.CreateLoad(fieldType, address);
    const unsigned delta = pressure ? 1 : index + 1;
    llvm::Value *increment =
        fieldType->isFloatingPointTy()
            ? llvm::ConstantFP::get(fieldType, delta)
            : static_cast<llvm::Value *>(builder.getInt64(delta));
    if (pressure) {
      if (index == 0) {
        increment = probe.vectorPrefix
                        ? static_cast<llvm::Value *>(
                              llvm::ConstantFP::get(builder.getDoubleTy(), 0.0))
                        : static_cast<llvm::Value *>(builder.getInt64(0));
        for (unsigned i = 0; i < prefix; ++i)
          increment = probe.vectorPrefix
                          ? builder.CreateFAdd(increment, incoming.at(i))
                          : builder.CreateAdd(increment, incoming.at(i));
        if (probe.symbol == "gpr")
          increment = builder.CreateAdd(increment, incoming.back());
      } else if (probe.symbol != "gpr") {
        increment = incoming.back();
        if (probe.integerTail)
          increment = builder.CreateUIToFP(increment, fieldType);
      }
    }
    llvm::Value *changed = nullptr;
    if (fieldType->isPointerTy())
      changed = builder.CreateGEP(builder.getInt8Ty(), value, increment);
    else if (fieldType->isFloatingPointTy())
      changed = builder.CreateFAdd(value, increment);
    else
      changed = builder.CreateAdd(
          value, builder.CreateIntCast(increment, fieldType, false));
    builder.CreateStore(changed, address);
  }
  emitNativeCReturn(session, builder, nativePlan, *function, local);

  auto *pointer = session.layouts().pointer();
  auto *bridgeType =
      llvm::FunctionType::get(builder.getVoidTy(), {pointer, pointer}, false);
  auto *bridge =
      llvm::Function::Create(bridgeType, llvm::Function::ExternalLinkage,
                             "import_" + probe.symbol, session.module());
  builder.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", bridge));
  std::vector<llvm::Value *> arguments;
  for (unsigned i = 0; i < probe.integerPrefix; ++i)
    arguments.push_back(builder.getInt64(i + 1));
  for (unsigned i = 0; i < probe.vectorPrefix; ++i)
    arguments.push_back(llvm::ConstantFP::get(builder.getDoubleTy(), i + 1));
  arguments.push_back(bridge->getArg(0));
  if (probe.integerTail) {
    arguments.push_back(builder.getInt64(17));
  }
  if (probe.vectorTail) {
    arguments.push_back(llvm::ConstantFP::get(builder.getDoubleTy(), 17.25));
  }
  require(nativePlan.signature.type == callee->getFunctionType(),
          "function-level budgets agree with the independent record oracle");
  static_cast<void>(emitNativeCCall(session, builder, nativePlan, callee,
                                    arguments, bridge->getArg(1)));
  builder.CreateRetVoid();

  if (pressure)
    return;
  auto *layoutType = llvm::FunctionType::get(builder.getInt64Ty(),
                                             {builder.getInt32Ty()}, false);
  auto *layout =
      llvm::Function::Create(layoutType, llvm::Function::ExternalLinkage,
                             "layout_" + probe.symbol, session.module());
  builder.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", layout));
  std::vector<std::uint64_t> facts{plan.storage.size, plan.storage.alignment};
  facts.insert(facts.end(), plan.storage.fieldOffsets.begin(),
               plan.storage.fieldOffsets.end());
  llvm::Value *answer = builder.getInt64(UINT64_MAX);
  for (unsigned i = 0; i < facts.size(); ++i)
    answer = builder.CreateSelect(
        builder.CreateICmpEQ(layout->getArg(0), builder.getInt32(i)),
        builder.getInt64(facts[i]), answer);
  builder.CreateRet(answer);
}

void emitByteProbe(Session &session, const std::string &name) {
  const auto &instance = record(session, name);
  const std::vector<SemanticType> types{instance.type};
  const auto plan = planNativeCFunction(session, instance.type, types);
  auto *function = llvm::Function::Create(
      plan.signature.type, llvm::Function::ExternalLinkage,
      "llvm_bytes_" + name, session.module());
  auto *callee = llvm::Function::Create(plan.signature.type,
                                        llvm::Function::ExternalLinkage,
                                        "c_bytes_" + name, session.module());
  plan.signature.applyTo(*function);
  plan.signature.applyTo(*callee);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *value = decodeNativeCArguments(session, ir, plan, *function).front();
  // The independent C oracle changes object bytes too. These selected records
  // have no padding, pointers or invalid scalar bit patterns after the XOR.
  for (std::uint64_t index = 0; index < plan.result.storage.size; ++index) {
    auto *byte = offset(ir, value, index);
    ir.CreateStore(
        ir.CreateXor(ir.CreateLoad(ir.getInt8Ty(), byte), ir.getInt8(1)), byte);
  }
  emitNativeCReturn(session, ir, plan, *function, value);
  auto *pointer = session.layouts().pointer();
  auto *bridge = llvm::Function::Create(
      llvm::FunctionType::get(ir.getVoidTy(), {pointer, pointer}, false),
      llvm::Function::ExternalLinkage, "import_bytes_" + name,
      session.module());
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", bridge));
  const std::vector<llvm::Value *> arguments{bridge->getArg(0)};
  static_cast<void>(
      emitNativeCCall(session, ir, plan, callee, arguments, bridge->getArg(1)));
  ir.CreateRetVoid();
}

void negativeChecks(Session &session) {
  for (const auto *name : {"Ordinary"})
    rejects([&] {
      static_cast<void>(
          classifyNativeCRecord(session, record(session, name).type, {6, 8}));
    });
  for (const auto *name :
       {"Nested", "ArrayField", "Large", "Boolean", "UnionRecord"})
    require(classifyNativeCRecord(session, record(session, name).type, {6, 8})
                    .storage.size != 0,
            "general passive classification subsumes the original flat bound");
  rejects([&] {
    static_cast<void>(
        classifyNativeCRecord(session, SemanticType::Int64, {6, 8}));
  });
  const auto pair = record(session, "Pair").type;
  rejects(
      [&] { static_cast<void>(classifyNativeCRecord(session, pair, {7, 8})); });
  const auto triple = session.module().getTargetTriple();
#if LLVM_VERSION_MAJOR >= 21
  session.module().setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
#else
  session.module().setTargetTriple("x86_64-pc-windows-msvc");
#endif
  rejects(
      [&] { static_cast<void>(classifyNativeCRecord(session, pair, {6, 8})); });
  session.module().setTargetTriple(triple);
  const auto layout = session.module().getDataLayout();
  session.module().setDataLayout("e-p:64:64-i64:32:32");
  rejects(
      [&] { static_cast<void>(classifyNativeCRecord(session, pair, {6, 8})); });
  session.module().setDataLayout(layout);
  auto *inProgress = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::Function::InternalLinkage, "in_progress", session.module());
  auto *block =
      llvm::BasicBlock::Create(session.context(), "entry", inProgress);
  static_cast<void>(classifyNativeCRecord(session, pair, {6, 8}));
  llvm::ReturnInst::Create(session.context(), block);
}

// This is an isolated boundary/transport oracle, not GTI body coverage. Exact
// sealed callback targets get deliberately tiny private-ABI test bodies here.
void emitCallbackProbes(Session &session, bool concurrent) {
  const auto &program = session.program().mir();
  const auto &targetInfo = session.program().target();
  unsigned conversions = 0;
  for (const auto &adapter : program.nativeCallbackAdapters()) {
    require(nativeCCallbackContract(program, targetInfo, adapter),
            "callback requires its exact sealed target, ABI and attachment");
    const auto malformed = [&](auto change) {
      auto forged = adapter;
      change(forged);
      require(!nativeCCallbackContract(program, targetInfo, forged),
              "a forged callback contract must reject before emission");
    };
    malformed([](auto &value) { value.id = 0; });
    malformed([](auto &value) { value.target = 0; });
    malformed([](auto &value) { value.type = SemanticType::Int32; });
    malformed([](auto &value) {
      value.targetMayRaiseDefinedFailure = !value.targetMayRaiseDefinedFailure;
    });
    malformed([](auto &value) { value.catchesNativeExceptions = false; });
    malformed([](auto &value) {
      value.cThreadCallback = value.cThreadCallback == CThreadCallbackKind::None
                                  ? CThreadCallbackKind::ThreadConfined
                                  : CThreadCallbackKind::None;
    });
  }
  for (const auto &function : program.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.nativeCallbackAdapter)
          continue;
        require(nativeCCallbackStageContract(program, targetInfo, function.body,
                                             instruction),
                "actual callback conversion must bind the exact source stage");
        ++conversions;
        const auto malformed = [&](auto change) {
          auto body = function.body;
          auto &changedBlock = *std::find_if(
              body.blocks.begin(), body.blocks.end(),
              [&](const auto &candidate) { return candidate.id == block.id; });
          auto &changed = *std::find_if(changedBlock.instructions.begin(),
                                        changedBlock.instructions.end(),
                                        [&](const auto &candidate) {
                                          return candidate.id == instruction.id;
                                        });
          change(changed);
          require(
              !nativeCCallbackStageContract(program, targetInfo, body, changed),
              "a matching forged body row cannot grant callback conversion");
        };
        malformed([](auto &value) { value.kind = MirInstructionKind::Load; });
        malformed([](auto &value) { value.operation = MirOperation::None; });
        malformed([](auto &value) { value.result.reset(); });
        malformed([](auto &value) { value.info.type = SemanticType::Int32; });
        malformed([](auto &value) { value.rawMemoryAccess = true; });
        malformed([](auto &value) { value.functionTarget = 1; });
        malformed([](auto &value) { value.nativeCallbackAdapter = 0; });
        malformed([](auto &value) { value.operands.push_back({}); });
      }
  require(conversions >= program.nativeCallbackAdapters().size(),
          "the callback oracle must include real source conversion stages");
  Failures failures(session);
  emitNativeCCallbacks(session, failures);
  require(session.program().mir().nativeCallbackAdapters().size() ==
              (concurrent ? 3 : 7),
          "callback fixture must seal all exact adapters");
  for (const auto &adapter : session.program().mir().nativeCallbackAdapters()) {
    const auto &target =
        *session.program().mir().findFunctionInstance(adapter.target);
    const auto *declaration =
        session.program().findFunctionDeclaration(target.declaration);
    require(declaration != nullptr, "missing callback declaration");
    auto *function = session.abi().declareFunction(target.id);
    const auto abi = session.abi().gti(target.returnType, target.parameterTypes,
                                       target.mayRaiseDefinedFailure);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    llvm::Value *result = nullptr, *failure = nullptr, *input = nullptr;
    for (unsigned index = 0; index < abi.parameters.size(); ++index) {
      const auto &parameter = abi.parameters[index];
      if (parameter.channel == AbiChannel::ResultDestination)
        result = function->getArg(index);
      else if (parameter.channel == AbiChannel::FailureDestination)
        failure = function->getArg(index);
      else if (parameter.channel == AbiChannel::Argument)
        input = function->getArg(index);
    }
    if (declaration->qualifiedName == "callback_exception") {
      const auto service = session.module().getOrInsertFunction(
          "native_throw",
          llvm::FunctionType::get(ir.getInt32Ty(), {ir.getInt32Ty()}, false));
      input = ir.CreateCall(service, {input});
    }
    if (declaration->qualifiedName == "callback_worker")
      input = ir.getInt32(31);
    if (declaration->qualifiedName == "callback_view")
      input =
          ir.CreateLoad(ir.getInt64Ty(),
                        ir.CreateStructGEP(session.abi().textView(), input, 1));
    const bool fails = declaration->qualifiedName == "callback_failure";
    if (fails) {
      require(failure != nullptr,
              "failure callback needs unchanged GTI channel");
      const auto *body = findMirBody(session.program().mir(),
                                     {MirBodyKind::Function, target.id});
      const MirInstruction *origin = nullptr;
      DefinedFailureOutcome outcome;
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (const auto &site : instruction.definedFailure.localOrigins)
            for (const auto &candidate : site.outcomes)
              if (candidate.code == DefinedFailureCode::DivisionByZero) {
                origin = &instruction;
                outcome = candidate;
              }
      require(origin != nullptr, "missing verified division failure site");
      failures.originate(ir, failure, *origin, outcome);
    } else if (result) {
      const auto layout = session.layouts().get(target.returnType);
      if (layout.addressOnly)
        ir.CreateMemCpy(result, llvm::MaybeAlign(layout.alignment), input,
                        llvm::MaybeAlign(layout.alignment), layout.size);
      else
        ir.CreateStore(input, result);
    }
    if (abi.statusResult)
      ir.CreateRet(ir.getInt1(!fails));
    else if (abi.resultDestination || target.returnType == SemanticType::Void)
      ir.CreateRetVoid();
    else
      ir.CreateRet(input);

    auto *getter = llvm::Function::Create(
        llvm::FunctionType::get(session.layouts().pointer(), false),
        llvm::Function::ExternalLinkage, "get_" + declaration->qualifiedName,
        session.module());
    ir.SetInsertPoint(
        llvm::BasicBlock::Create(session.context(), "entry", getter));
    ir.CreateRet(nativeCCallbackAddress(session, ir, adapter.id));
  }
}

void emitContractProbes(Session &session) {
  const auto &program = session.program().mir();
  const auto &target = session.program().target();
  const std::vector<SemanticType> narrow{
      SemanticType::Int8, SemanticType::UInt8, SemanticType::Int16,
      SemanticType::UInt16};
  checkImportContract(session.abi().nativeC(SemanticType::Int8, narrow));
  checkImportContract(session.abi().nativeC(SemanticType::UInt8, narrow));
  const auto nativeChar =
      SemanticType::nativeCScalarType(NativeCScalarKind::Char, target);
  require(nativeChar && nativeCScalarValueContract(target, *nativeChar),
          "native scalar identity consumes the canonical target query");
  auto forgedChar = *nativeChar;
  forgedChar.kind = SemanticType::UInt64;
  require(!nativeCScalarValueContract(target, forgedChar) &&
              !nativeCValueContract(program, target, forgedChar,
                                    NativeCValueRole::Argument),
          "a native C scalar spelling cannot override its canonical domain");
  unsigned direct = 0, indirect = 0;
  const std::array<SemanticType, 2> scalarParameters{SemanticType::Int8,
                                                     SemanticType::UInt16};
  const auto scalarFunction =
      SemanticType::nativeFunctionType(SemanticType::Int16, scalarParameters);
  require(nativeCScalarFunctionValueContract(program, target, scalarFunction),
          "scalar native signatures use the exact general value contract");
  const std::array<SemanticType, 1> countedParameters{SemanticType::StringView};
  const auto countedFunction =
      SemanticType::nativeFunctionType(SemanticType::Int32, countedParameters);
  require(
      nativeCValueContract(program, target, countedFunction,
                           NativeCValueRole::Argument) &&
          !nativeCScalarFunctionValueContract(program, target, countedFunction),
      "a representable counted signature does not open indirect admission");
  const auto nestedFunction =
      SemanticType::nativeFunctionType(scalarFunction, {});
  require(
      !nativeCScalarFunctionValueContract(program, target, nestedFunction) &&
          !nativeCScalarFunctionValueContract(program, target,
                                              SemanticType::NativeFunction),
      "nested and incomplete native signatures remain closed");
  for (const auto &function : program.functionInstances()) {
    if (function.linkage == LanguageLinkage::C) {
      require(nativeCDeclarationContract(program, target, function),
              "ordinary unrelated C declarations need structural contracts");
      require(nativeCScalarFunctionImportContract(program, target, function) ==
                  (function.returnType.kind == SemanticType::NativeFunction),
              "only the exact scalar-function getter receives its new witness");
      auto *imported = session.abi().declareFunction(function.id);
      const auto plan =
          session.abi().nativeC(function.returnType, function.parameterTypes);
      checkImportContract(plan);
      require(session.abi().declareFunction(function.id) == imported,
              "exact native import must reuse its canonical declaration");
      imported->setCallingConv(llvm::CallingConv::Fast);
      rejects<std::logic_error>([&] {
        static_cast<void>(session.abi().declareFunction(function.id));
      });
      imported->setCallingConv(llvm::CallingConv::C);
      for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto invalid = function;
        if (mutation == 0)
          invalid.externalSymbol += "_forged";
        if (mutation == 1)
          invalid.definitionKind = MirDefinitionKind::RuntimeBinding;
        if (mutation == 2)
          invalid.returnType = SemanticType::Unknown;
        if (mutation == 3)
          invalid.tracksCaller = true;
        if (mutation == 4)
          invalid.linkage = LanguageLinkage::Gti;
        require(!nativeCDeclarationContract(program, target, invalid),
                "mutated C declaration cannot establish a native witness");
        require(!nativeCScalarFunctionImportContract(program, target, invalid),
                "a forged getter cannot establish scalar native admission");
      }
    }
    const auto *declaration =
        session.program().findFunctionDeclaration(function.declaration);
    if (!declaration || declaration->qualifiedName != "main")
      continue;
    const MirInstruction *indirectCall = nullptr;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.nativeCalleeType)
          indirectCall = &instruction;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.callInputRole == MirCallInputRole::Receiver &&
            instruction.info.type.kind == SemanticType::NativeFunction) {
          require(nativeCScalarReceiverInputContract(
                      program, target, function.body, instruction),
                  "native receiver input requires its exact consuming call");
          auto invalid = instruction;
          invalid.callSite += 1000;
          require(!nativeCScalarReceiverInputContract(program, target,
                                                      function.body, invalid),
                  "a mismatched staged receiver cannot gain a native witness");
          invalid = instruction;
          invalid.operands.clear();
          require(
              !nativeCScalarReceiverInputContract(program, target,
                                                  function.body, invalid),
              "a noncanonical staged receiver cannot gain a native witness");
        }
        if (instruction.nativeCalleeType) {
          require(nativeCIndirectCallContract(program, target, function.body,
                                              instruction),
                  "verified native pointer call requires its exact native "
                  "signature");
          require(nativeCScalarIndirectCallContract(program, target,
                                                    function.body, instruction),
                  "scalar indirect witness consumes the exact staged contract");
          for (unsigned mutation = 0; mutation < 5; ++mutation) {
            auto invalid = instruction;
            if (mutation == 0)
              invalid.callSite += 1000;
            if (mutation == 1)
              invalid.functionTarget = 1;
            if (mutation == 2)
              invalid.receiver.reset();
            if (mutation == 3)
              invalid.parameterTypes.push_back(SemanticType::Int32);
            if (mutation == 4)
              invalid.tombstone = true;
            require(!nativeCIndirectCallContract(program, target, function.body,
                                                 invalid),
                    "mutated native indirect call must reject");
            require(!nativeCScalarIndirectCallContract(program, target,
                                                       function.body, invalid),
                    "a malformed indirect call cannot gain scalar admission");
          }
          ++indirect;
        }
        const auto *callee =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (!callee || callee->linkage != LanguageLinkage::C)
          continue;
        if (!nativeCDirectCallContract(program, target, function.body,
                                       instruction))
          std::cerr << "native contract mismatch: " << callee->externalSymbol
                    << " kind=" << unsigned(instruction.kind)
                    << " unsafe=" << unsigned(instruction.unsafeOperation)
                    << " site=" << instruction.callSite
                    << " raw=" << instruction.rawMemoryAccess
                    << " receiver=" << instruction.receiver.has_value()
                    << " callable="
                    << instruction.callableInvocation.has_value()
                    << " failure=" << !instruction.definedFailure.empty()
                    << " sites=" << instruction.localFailureSites.size()
                    << " parameters=" << instruction.parameterTypes.size()
                    << " expected=" << callee->parameterTypes.size()
                    << " operands=" << instruction.operands.size() << '\n';
        require(
            nativeCDirectCallContract(program, target, function.body,
                                      instruction),
            "real staged direct C call did not establish its exact witness");
        ++direct;
        for (unsigned mutation = 0; mutation < 12; ++mutation) {
          auto invalid = instruction;
          if (mutation == 0)
            invalid.callSite = 0;
          if (mutation == 1)
            invalid.tracksCaller = true;
          if (mutation == 2)
            invalid.tombstone = true;
          if (mutation == 3)
            invalid.rawMemoryAccess = true;
          if (mutation == 4)
            invalid.localFailureSites.push_back(1);
          if (mutation == 5)
            invalid.info.type = SemanticType::Unknown;
          if (mutation == 6)
            invalid.unsafeOperation = UnsafeOperationKind::DeclaredUnsafeCall;
          if (mutation == 7)
            invalid.callableInvocation = CallableInvocationCapability::Read;
          if (mutation == 8)
            invalid.parameterTypes.push_back(SemanticType::Int32);
          if (mutation == 9)
            invalid.functionTarget = 0;
          if (mutation == 10)
            invalid.callSite += 1000;
          if (mutation == 11)
            invalid.id = 0;
          require(!nativeCDirectCallContract(program, target, function.body,
                                             invalid),
                  "malformed staged C call cannot acquire native admission");
        }
        const auto native = planNativeCFunction(session, callee->returnType,
                                                callee->parameterTypes);
        require(!native.signature.failureDestination &&
                    !native.signature.statusResult,
                "conservative C declaration effects cannot add private GTI "
                "channels");
        std::string name;
        if (callee->externalSymbol == "unrelated_getter")
          name = "bridge_getter";
        if (callee->externalSymbol == "unrelated_math")
          name = "bridge_math";
        if (callee->externalSymbol == "unrelated_callback_getter")
          name = "bridge_callback";
        require(!name.empty(), "unexpected contract fixture target");
        const bool callback = name == "bridge_callback";
        require(
            nativeCScalarImportContract(program, target, *callee) == !callback,
            "scalar import admission cannot imply returned-function support");
        auto *type =
            callback ? llvm::FunctionType::get(
                           llvm::Type::getInt32Ty(session.context()),
                           {llvm::Type::getInt32Ty(session.context())}, false)
                     : native.signature.type;
        auto *bridge = llvm::Function::Create(
            type, llvm::Function::ExternalLinkage, name, session.module());
        llvm::IRBuilder<> ir(
            llvm::BasicBlock::Create(session.context(), "entry", bridge));
        std::vector<llvm::Value *> arguments;
        if (!callback)
          for (auto &argument : bridge->args())
            arguments.push_back(&argument);
        auto *result = emitNativeCDirectCall(session, ir, function.body,
                                             instruction, arguments);
        if (callback) {
          require(indirectCall != nullptr,
                  "missing exact staged native pointer call");
          const std::vector<llvm::Value *> inputs{bridge->getArg(0)};
          result = emitNativeCIndirectCall(session, ir, function.body,
                                           *indirectCall, result, inputs);
        }
        ir.CreateRet(result);
        unsigned calls = 0;
        for (const auto &block : *bridge)
          for (const auto &operation : block) {
            require(!llvm::isa<llvm::AllocaInst>(operation),
                    "scalar native transport cannot introduce stack storage");
            calls += llvm::isa<llvm::CallBase>(operation);
          }
        require(calls == (callback ? 2u : 1u),
                "native scalar transport adds no runtime adapter call");
      }
  }
  require(direct == 3 && indirect == 1,
          "contract fixture must retain all staged C boundaries");
}

void emitCountedTextProbes(Session &session) {
  const auto &program = session.program().mir();
  const auto &target = session.program().target();
  require(!nativeCValueContract(program, target, SemanticType::StringView,
                                NativeCValueRole::Result),
          "counted C input cannot grant text return or ownership transfer");
  std::set<std::string> emitted;
  for (const auto &owner : program.functionInstances())
    for (const auto &block : owner.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *callee =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (!callee ||
            (callee->externalSymbol != "consume_text" &&
             callee->externalSymbol != "consume_text_pressure" &&
             callee->externalSymbol != "gti_rt_write_stdout") ||
            !emitted.insert(callee->externalSymbol).second)
          continue;
        require(nativeCCountedInputContract(program, target, *callee) &&
                    !nativeCScalarImportContract(program, target, *callee) &&
                    nativeCDirectCallContract(program, target, owner.body,
                                              instruction),
                "counted C input requires an exact staged record contract");
        const auto native = planNativeCFunction(session, callee->returnType,
                                                callee->parameterTypes);
        require(callee->returnType == SemanticType::Int32 &&
                    !native.signature.statusResult &&
                    !native.signature.failureDestination,
                "counted C input preserves the declared int32 status result");
        auto *bridge = llvm::Function::Create(
            native.signature.type, llvm::Function::ExternalLinkage,
            "bridge_" + callee->externalSymbol, session.module());
        native.signature.applyTo(*bridge);
        llvm::IRBuilder<> ir(
            llvm::BasicBlock::Create(session.context(), "entry", bridge));
        const auto arguments =
            decodeNativeCArguments(session, ir, native, *bridge);
        auto *result = emitNativeCDirectCall(session, ir, owner.body,
                                             instruction, arguments);
        emitNativeCReturn(session, ir, native, *bridge, result);
      }
  require(emitted.size() == 3,
          "counted C oracle must include unrelated and actual runtime clients");
}

void emitSourcePreflight(Session &session,
                         const VerifiedLoweredProgram &input) {
  const auto &program = session.program().mir();
  std::set<std::string> scalarRequirements;
  const auto inspectScalar = [&](const auto &self,
                                 const SemanticType &type) -> void {
    if (type.nativeCScalar != NativeCScalarKind::None) {
      require(nativeCScalarValueContract(session.program().target(), type),
              "source preflight requires each canonical native scalar domain");
      scalarRequirements.insert(
          llvm_detail::coverage(type.nativeCScalar).key());
    }
    for (const auto &argument : type.arguments)
      self(self, argument);
  };
  for (const auto &function : program.functionInstances()) {
    inspectScalar(inspectScalar, function.returnType);
    for (const auto &parameter : function.parameterTypes)
      inspectScalar(inspectScalar, parameter);
    for (const auto &value : function.body.values)
      inspectScalar(inspectScalar, value.info.type);
    for (const auto &place : function.body.places)
      inspectScalar(inspectScalar, place.type);
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *target =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (target && target->linkage == LanguageLinkage::C)
          require(nativeCDirectCallContract(program, session.program().target(),
                                            function.body, instruction),
                  "source preflight needs every real C call's exact witness");
      }
  }
  const auto admission = planLlvmAdmission(input, session.options());
  for (const auto &use : admission.requirements()) {
    if (!use.required || use.entry->state != LlvmCoverageState::Pending ||
        (use.implementedShape &&
         use.implementedShape->state == LlvmCoverageState::Implemented))
      continue;
    const auto &entry = *use.entry;
    const bool declarationRequirement =
        (entry.family == "Shape" &&
         (entry.name == "NativeCall" || entry.name == "LibraryDeclaration")) ||
        (entry.family == "LanguageLinkage" && entry.name == "C") ||
        (entry.family == "MirDefinitionKind" && entry.name == "Declaration");
    bool witnessed = false;
    if (entry.family == "NativeCScalarKind")
      witnessed = scalarRequirements.contains(entry.key());
    if (entry.family == "Shape" && entry.name == "NativeScalar")
      witnessed = !scalarRequirements.empty();
    if (declarationRequirement && use.body &&
        use.body->kind == MirBodyKind::Function)
      if (const auto *function = program.findFunctionInstance(use.body->owner))
        witnessed = nativeCScalarImportContract(
                        program, session.program().target(), *function) ||
                    nativeCCountedInputContract(
                        program, session.program().target(), *function);
    if (entry.family == "UnsafeOperationKind" &&
        entry.name == "ForeignPointerCall" && use.body)
      if (const auto *body = findMirBody(program, *use.body))
        if (const auto *block = body->findBlock(use.block))
          for (const auto &instruction : block->instructions)
            if (instruction.id == use.instruction)
              witnessed = nativeCDirectCallContract(
                  program, session.program().target(), *body, instruction);
    if (!witnessed)
      throw std::runtime_error("source preflight still requires " +
                               entry.key());
  }
  // Only this explicit test consumer permits the independently validated
  // native inventory above. The public backend admission remains unchanged.
  lowerScalarProgram(session, admission);
}

void emitStorageProbes(Session &session) {
  const auto &program = session.program().mir();
  std::map<SymbolId, llvm::GlobalVariable *> globals;
  declareNativeCStorage(session, globals);
  const auto storage = [&](const std::string &name) -> llvm::GlobalVariable * {
    for (const auto &row : program.foreignStorageDeclarations()) {
      require(nativeCStorageContract(program, session.program().target(), row),
              "foreign storage requires its exact sealed declaration");
      auto forged = row;
      forged.externalSymbol += "_forged";
      require(
          !nativeCStorageContract(program, session.program().target(), forged),
          "foreign symbol mismatch cannot establish a storage witness");
      if (row.externalSymbol == name)
        return globals.at(row.symbol);
    }
    throw std::runtime_error("missing native storage fixture: " + name);
  };
  require(!storage("native_limit")->isConstant(),
          "source read-only access does not prove native value stability");
  auto *i32 = llvm::Type::getInt32Ty(session.context());
  const auto function = [&](const char *name, llvm::Type *result,
                            llvm::ArrayRef<llvm::Type *> parameters) {
    return llvm::Function::Create(
        llvm::FunctionType::get(result, parameters, false),
        llvm::Function::ExternalLinkage, name, session.module());
  };
  auto *roundtrip = function("llvm_storage_roundtrip", i32, {i32});
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", roundtrip));
  ir.CreateStore(roundtrip->getArg(0), storage("native_counter"));
  const MirInstruction *bump = nullptr;
  const MirBody *bumpBody = nullptr;
  for (const auto &owner : program.functionInstances())
    for (const auto &block : owner.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.functionTarget)
          if (const auto *callee =
                  program.findFunctionInstance(*instruction.functionTarget);
              callee && callee->externalSymbol == "native_bump") {
            bump = &instruction;
            bumpBody = &owner.body;
          }
  require(bump && bumpBody, "missing real staged foreign-effect call");
  static_cast<void>(emitNativeCDirectCall(session, ir, *bumpBody, *bump, {}));
  ir.CreateRet(ir.CreateLoad(i32, storage("native_counter")));

  auto *apply = function("llvm_storage_apply", i32, {i32});
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", apply));
  const std::vector<SemanticType> parameters{SemanticType::Int32};
  const auto plan =
      planNativeCFunction(session, SemanticType::Int32, parameters);
  const std::vector<llvm::Value *> arguments{apply->getArg(0)};
  ir.CreateRet(emitNativeCCall(
      session, ir, plan,
      ir.CreateLoad(session.layouts().pointer(), storage("native_operation")),
      arguments));

  auto *pointer = function("llvm_storage_pointer", i32, {});
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", pointer));
  ir.CreateRet(ir.CreateLoad(i32, ir.CreateLoad(session.layouts().pointer(),
                                                storage("native_pointer"))));
  auto *limit = function("llvm_storage_limit", i32, {});
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", limit));
  ir.CreateRet(ir.CreateLoad(i32, storage("native_limit")));
}

} // namespace

void checkAggregateImports(Session &session) {
  const auto &program = session.program().mir();
  const auto &target = session.program().target();
  unsigned imports = 0, calls = 0;
  bool callbackField = false;
  for (const auto &function : program.functionInstances()) {
    if (function.externalSymbol == "native_unadmitted_hook") {
      require(nativeCDeclarationContract(program, target, function) &&
                  !nativeCAggregateImportContract(program, target, function),
              "physical native layout cannot admit a function-pointer field");
      callbackField = true;
    }
    if (nativeCAggregateImportContract(program, target, function)) {
      ++imports;
      require(
          !nativeCScalarImportContract(program, target, function) &&
              !nativeCCountedInputContract(program, target, function) &&
              !nativeCScalarFunctionImportContract(program, target, function),
          "aggregate import witness cannot relabel scalar/count/getter "
          "families");
      const auto plan = planNativeCFunction(session, function.returnType,
                                            function.parameterTypes);
      verifyNativeCImport(*session.abi().declareFunction(function.id),
                          plan.signature);
      for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto invalid = function;
        if (mutation == 0)
          invalid.externalSymbol += "_forged";
        if (mutation == 1)
          invalid.definitionKind = MirDefinitionKind::RuntimeBinding;
        if (mutation == 2)
          invalid.returnType = SemanticType::Unknown;
        if (mutation == 3)
          invalid.parameterTypes.push_back(SemanticType::Int32);
        if (mutation == 4)
          invalid.linkage = LanguageLinkage::Gti;
        require(!nativeCAggregateImportContract(program, target, invalid),
                "forged aggregate declaration cannot acquire a native witness");
      }
    }
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *callee =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (!callee ||
            !nativeCAggregateImportContract(program, target, *callee))
          continue;
        require(
            nativeCDirectCallContract(program, target, function.body,
                                      instruction),
            "aggregate calls consume the exact canonical staged C contract");
        auto invalid = instruction;
        ++invalid.callSite;
        require(
            !nativeCDirectCallContract(program, target, function.body, invalid),
            "aggregate transport cannot accept mismatched staged inputs");
        ++calls;
      }
  }
  require(
      imports >= 8 && calls >= 8 && callbackField,
      "aggregate fixture must exercise multiple result and argument families");
}

void checkNativeBoundaryReachability(Session &session) {
  const auto roots = reachableProgram(session.program());
  require(!roots.conservative,
          "verified native signatures must retain exact roots");
  unsigned checked = 0;
  auto malformed = session.program();
  auto &functions = const_cast<std::vector<MirFunctionInstance> &>(
      malformed.mir().functionInstances());
  for (auto &function : functions) {
    if (!roots.contains(MirBodyAddress{MirBodyKind::Function, function.id}))
      continue;
    for (auto &block : function.body.blocks)
      for (auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        if (stage.nativeCallbackAdapter) {
          const auto id = *stage.nativeCallbackAdapter;
          const auto *adapter = malformed.mir().findNativeCallbackAdapter(id);
          require(adapter &&
                      roots.contains(MirBodyAddress{MirBodyKind::Function,
                                                    adapter->target}) &&
                      roots.contains(LoweredGeneratedItemIdentity{
                          .kind = LoweredGeneratedItemKind::NativeInteropAdapter,
                          .owner = id}),
                  "a callback must retain its exact adapter and target body");
          stage.nativeCallbackAdapter = 0;
          require(reachableProgram(malformed).conservative,
                  "a forged callback conversion must restore conservative roots");
          stage.nativeCallbackAdapter = id;
          ++checked;
        }
        if (stage.nativeCalleeType) {
          ++stage.callSite;
          require(reachableProgram(malformed).conservative,
                  "a forged native signature stage must restore conservative roots");
          --stage.callSite;
          ++checked;
        }
      }
  }
  require(checked != 0,
          "native closure fixture must exercise a call or callback");
}

void checkNativeReachability(Session &session, bool conservative) {
  const auto roots = reachableProgram(session.program());
  require(roots.conservative == conservative,
          "only exact supported foreign calls may avoid unknown GTI dispatch");
  if (conservative)
    return;
  std::set<std::string> found;
  bool destructor = false;
  for (const auto &function : session.program().mir().functionInstances()) {
    const auto *declaration =
        session.program().findFunctionDeclaration(function.declaration);
    if (!declaration)
      continue;
    const auto &name = declaration->qualifiedName;
    if (name == "main" || name == "required_dependency" ||
        name == "native_unary" || name == "native_mark" ||
        name == "export_root_candidate" || name == "dormant_unsupported") {
      const bool required =
          roots.contains(MirBodyAddress{MirBodyKind::Function, function.id});
      require(required == (name != "dormant_unsupported" &&
                           name != "export_root_candidate"),
              "foreign calls must retain exact GTI, import and cleanup roots");
      found.insert(name);
    }
  }
  for (auto address : roots.bodies)
    destructor |= address.kind == MirBodyKind::Destructor;
  require(
      found.size() == 6 && destructor,
      "sealed closure must exercise dormant, direct, export and cleanup roots");
  // Public extern-C syntax is declaration-only. Exercise the pre-existing
  // source-C root rule structurally without granting source exports or codegen
  // authority to this altered test copy.
  auto exported = session.program();
  auto &functions = const_cast<std::vector<MirFunctionInstance> &>(
      exported.mir().functionInstances());
  bool checkedExport = false, checkedMalformed = false;
  for (auto &function : functions) {
    const auto *declaration =
        exported.findFunctionDeclaration(function.declaration);
    if (declaration && declaration->qualifiedName == "export_root_candidate") {
      function.linkage = LanguageLinkage::C;
      require(reachableProgram(exported).contains(
                  MirBodyAddress{MirBodyKind::Function, function.id}),
              "foreign calls must preserve every existing C source root");
      function.linkage = LanguageLinkage::Gti;
      checkedExport = true;
    }
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.nativeCalleeType) {
          const bool required = roots.contains(
              MirBodyAddress{MirBodyKind::Function, function.id});
          ++instruction.callSite;
          require(reachableProgram(exported).conservative == required,
                  "only malformed required native calls broaden the closure");
          --instruction.callSite;
          checkedMalformed |= required;
        }
  }
  require(checkedExport && checkedMalformed,
          "root controls must exercise source-C and malformed call fallbacks");
}

void checkStorageImports(Session &session,
                         const VerifiedLoweredProgram &input) {
  const auto &program = session.program().mir();
  const auto &target = session.program().target();
  std::map<SymbolId, llvm::GlobalVariable *> globals;
  declareNativeCStorage(session, globals);
  for (const auto &identity : program.classInstances()) {
    const auto pointer = SemanticType::rawPointerTo(identity.type);
    require(
        nativeCOpaquePointerReferentContract(program, target, pointer) ==
            identity.cOpaqueHandle,
        "opaque witness must select the exact sealed pointer-only identity");
  }
  for (const auto &storage : program.foreignStorageDeclarations()) {
    require(nativeCStorageImportContract(program, target, storage),
            "public storage requires its exact bounded import witness");
    if (nativeCOpaquePointerReferentContract(program, target, storage.type)) {
      const auto &referent = storage.type.arguments.front();
      require(
          !nativeCValueContract(program, target, referent,
                                NativeCValueRole::Storage) &&
              !nativeCOpaquePointerReferentContract(program, target, referent),
          "pointer-only identity must not admit an opaque object value");
      auto pointer = storage.type;
      pointer.arguments.front() = SemanticType::Int32;
      require(!nativeCOpaquePointerReferentContract(program, target, pointer),
              "an ordinary pointer is not an opaque identity witness");
      pointer = storage.type;
      pointer.arguments.push_back(referent);
      require(!nativeCOpaquePointerReferentContract(program, target, pointer),
              "a malformed pointer cannot authorize opaque type metadata");
      auto unsupportedTarget = target;
      unsupportedTarget.arch = "unsupported";
      require(!nativeCOpaquePointerReferentContract(program, unsupportedTarget,
                                                    storage.type),
              "opaque pointer representation remains target bound");
    }
    auto forged = storage;
    forged.externalSymbol += "_forged";
    require(!nativeCStorageImportContract(program, target, forged),
            "forged foreign storage name must reject");
    forged = storage;
    forged.type = SemanticType::StringView;
    require(!nativeCStorageImportContract(program, target, forged),
            "forged foreign storage type must reject");
    auto *global = globals.at(storage.symbol);
    require(!global->isConstant() && !global->hasInitializer() &&
                global->hasExternalLinkage() &&
                global->getAlign() ==
                    session.layouts().get(storage.type).alignment,
            "foreign storage is external nonconstant aligned data, never GTI "
            "init/drop");
  }
  rejects<std::logic_error>([&] { declareNativeCStorage(session, globals); });
  require(!program.foreignStorageDeclarations().empty(),
          "missing foreign storage control");
  Session collision(input, session.options());
  static_cast<void>(llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(collision.context()),
                              false),
      llvm::GlobalValue::ExternalLinkage,
      program.foreignStorageDeclarations().front().externalSymbol,
      collision.module()));
  std::map<SymbolId, llvm::GlobalVariable *> conflicting;
  rejects<std::logic_error>(
      [&] { declareNativeCStorage(collision, conflicting); });
  unsigned loads = 0, assigns = 0, modifies = 0;
  for (const auto &function : program.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        if ((stage.unsafeOperation != UnsafeOperationKind::ForeignStorage &&
             stage.unsafeOperation != UnsafeOperationKind::UnionMember) ||
            stage.kind == MirInstructionKind::Lifecycle)
          continue;
        const bool unionAccess =
            stage.unsafeOperation == UnsafeOperationKind::UnionMember;
        const auto foreignPlace = [&](MirPlaceId id) {
          const auto *place = function.body.findPlace(id);
          return place && place->foreignStorage;
        };
        if (unionAccess &&
            !(stage.destination && foreignPlace(*stage.destination)) &&
            !std::any_of(stage.operands.begin(), stage.operands.end(),
                         [&](const auto &operand) {
                           return foreignPlace(operand.place);
                         }))
          continue;
        const auto accessContract = unionAccess
                                        ? nativeCStorageUnionAccessContract
                                        : nativeCStorageAccessContract;
        std::cout << "storage stage " << function.id << '/' << block.id << '/'
                  << stage.id << " kind=" << static_cast<unsigned>(stage.kind)
                  << " witness="
                  << accessContract(program, target, function.body, stage)
                  << '\n';
        require(
            accessContract(program, target, function.body, stage),
            "actual foreign storage stage must retain exact structural proof");
        if (loads == 0 && assigns == 0) {
          const auto malformed = [&](auto change) {
            auto body = function.body;
            MirInstruction *selected = nullptr;
            for (auto &block : body.blocks)
              for (auto &candidate : block.instructions)
                if (candidate.id == stage.id)
                  selected = &candidate;
            require(selected != nullptr, "missing cloned storage stage");
            change(body, *selected);
            require(
                !accessContract(program, target, body, *selected),
                "forged or mixed storage stage cannot acquire raw permission");
          };
          malformed([](auto &, auto &value) {
            value.unsafeOperation = UnsafeOperationKind::RawDereference;
          });
          malformed([&](auto &, auto &value) {
            value.unsafeOperation = unionAccess
                                        ? UnsafeOperationKind::ForeignStorage
                                        : UnsafeOperationKind::UnionMember;
          });
          malformed([](auto &, auto &value) {
            value.kind = MirInstructionKind::Call;
          });
          malformed([](auto &, auto &value) {
            value.intrinsic = IntrinsicKind::ObjectDestroy;
          });
          malformed([](auto &, auto &value) { value.functionTarget = 1; });
          malformed([](auto &, auto &value) { value.rawMemoryAccess = false; });
          malformed([](auto &, auto &value) {
            value.receiver = value.operands.front();
          });
          malformed([](auto &body, auto &value) {
            auto extra = body.places.front();
            extra.id = body.places.size() + 1;
            extra.root = MirPlaceRootKind::Value;
            extra.foreignStorage = false;
            extra.capture = 0;
            extra.type = SemanticType::Int32;
            extra.projections = {{.kind = MirProjectionKind::RawDereference}};
            body.places.push_back(extra);
            value.operands.push_back({.kind = MirOperandKind::Copy,
                                      .place = extra.id,
                                      .type = extra.type});
          });
          malformed([](auto &body, auto &value) {
            auto extra = body.places.front();
            extra.id = body.places.size() + 1;
            extra.root = MirPlaceRootKind::Symbol;
            extra.foreignStorage = true;
            extra.symbol = 0;
            extra.capture = 0;
            extra.type = SemanticType::Int32;
            extra.projections.clear();
            body.places.push_back(extra);
            value.operands.push_back({.kind = MirOperandKind::Copy,
                                      .place = extra.id,
                                      .type = extra.type});
          });
          malformed([](auto &, auto &value) {
            value.operands.push_back({.kind = MirOperandKind::Value,
                                      .type = SemanticType::StringView});
          });
          for (const auto &place : function.body.places)
            if (place.foreignStorage) {
              require(
                  nativeCStoragePlaceContract(program, target, function.body,
                                              place),
                  "actual foreign place must bind canonical root and storage");
              auto forged = place;
              forged.symbol = 0;
              require(!nativeCStoragePlaceContract(program, target,
                                                   function.body, forged),
                      "forged storage symbol cannot acquire a place witness");
              forged = place;
              forged.capture = 1;
              require(!nativeCStoragePlaceContract(program, target,
                                                   function.body, forged),
                      "foreign storage cannot grant capture policy");
              forged = place;
              forged.type = SemanticType::StringView;
              require(!nativeCStoragePlaceContract(program, target,
                                                   function.body, forged),
                      "forged storage type cannot acquire a place witness");
              forged = place;
              forged.projections.push_back(
                  {.kind = MirProjectionKind::Field, .field = 0});
              require(!nativeCStoragePlaceContract(program, target,
                                                   function.body, forged),
                      "forged projection cannot acquire a place witness");
              const auto malformedPlace = [&](auto change) {
                auto body = function.body;
                auto &changed =
                    *std::find_if(body.places.begin(), body.places.end(),
                                  [&](const auto &candidate) {
                                    return candidate.id == place.id;
                                  });
                change(changed);
                require(!nativeCStoragePlaceContract(program, target, body,
                                                     changed),
                        "a matching body row must still prove its exact "
                        "projection");
              };
              malformedPlace(
                  [](auto &value) { value.initiallyAvailable = false; });
              malformedPlace([](auto &value) { value.binding = 1; });
              malformedPlace(
                  [](auto &value) { value.type = SemanticType::StringView; });
              if (!place.projections.empty()) {
                malformedPlace([](auto &value) {
                  value.projections.front().kind =
                      MirProjectionKind::RawDereference;
                });
                if (place.projections.front().kind == MirProjectionKind::Field)
                  malformedPlace(
                      [](auto &value) { value.projections.front().field = 0; });
              }
            }
        }
        loads += stage.kind == MirInstructionKind::Load;
        assigns += stage.kind == MirInstructionKind::Assign;
        modifies += stage.kind == MirInstructionKind::Modify;
      }
  const auto admission = planLlvmAdmission(input, session.options());
  for (const auto &use : admission.requirements())
    if (use.required && use.entry->state == LlvmCoverageState::Pending &&
        (!use.implementedShape ||
         use.implementedShape->state != LlvmCoverageState::Implemented))
      std::cout << "remaining " << use.entry->key() << " block=" << use.block
                << " instruction=" << use.instruction << '\n';
  require(loads && assigns,
          "storage fixture needs actual verified loads and assignments");
  std::cout << "storage counts " << loads << '/' << assigns << '/' << modifies
            << '\n';
}

int main(int argc, char **argv) {
  try {
    const bool contracts = argc == 6 && std::string(argv[5]) == "--contracts";
    const bool preflight =
        argc == 6 && std::string(argv[5]) == "--source-preflight";
    const bool storageContracts =
        argc == 6 && std::string(argv[5]) == "--storage-contracts";
    const bool storage = argc == 6 && std::string(argv[5]) == "--storage";
    const bool text = argc == 6 && std::string(argv[5]) == "--text-contracts";
    const bool aggregates =
        argc == 6 && std::string(argv[5]) == "--aggregate-contracts";
    const bool reachability =
        argc == 6 && std::string(argv[5]) == "--reachability";
    const bool nativeReachability =
        argc == 6 && std::string(argv[5]) == "--native-boundary-reachability";
    const bool conservative =
        argc == 6 && std::string(argv[5]) == "--conservative-reachability";
    const bool concurrent =
        argc == 6 && std::string(argv[5]) == "--thread-callbacks";
    require(argc == 5 ||
                (argc == 6 &&
                 (std::string(argv[5]) == "--callbacks" || concurrent ||
                  contracts || preflight || storage || text || aggregates ||
                  reachability || nativeReachability || conservative ||
                  storageContracts)),
            "usage: test fixture.gti output.o output.ll O[0-3] "
            "[--callbacks|--thread-callbacks]");
    const auto input = fixture(argv[1], concurrent, argc == 6);
    const auto level = std::stoi(argv[4]);
    require(level >= 0 && level <= 3, "invalid optimization level");
    const LlvmCodegenOptimization levels[] = {
        LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default, LlvmCodegenOptimization::Aggressive};
    Session session(input, {.optimization = levels[level]});
    if (nativeReachability) {
      checkNativeBoundaryReachability(session);
      std::cout << "PASS: exact callback and native signature roots\n";
      return 0;
    }
    if (reachability || conservative) {
      checkNativeReachability(session, conservative);
      std::cout << "PASS: exact native call dependency roots\n";
      return 0;
    }
    if (storageContracts) {
      checkStorageImports(session, input);
    } else if (aggregates) {
      checkAggregateImports(session);
      emitSourcePreflight(session, input);
    } else if (text) {
      emitCountedTextProbes(session);
    } else if (storage) {
      emitStorageProbes(session);
    } else if (preflight) {
      emitSourcePreflight(session, input);
    } else if (contracts) {
      emitContractProbes(session);
    } else if (argc == 6) {
      emitCallbackProbes(session, concurrent);
    } else {
      negativeChecks(session);
      for (const auto *name :
           {"Byte", "Tiny", "Shorts", "Words", "TripleWords", "Tail", "Pair",
            "Pointer", "Float1", "Float2", "Float3", "Float4", "Double1",
            "Double2", "IntDouble", "DoubleInt", "FloatInt", "FloatDouble"})
        emitProbe(session, {name, name});
      emitProbe(session, {"Pair", "gpr", 5, 0, true, false});
      emitProbe(session, {"IntDouble", "mixed", 6, 0, false, true});
      emitProbe(session, {"Double2", "sse", 0, 8, true, false});
      for (const auto *name : {"Nested", "ArrayField", "Large", "Boolean",
                               "UnionRecord", "DoubleArray", "HfaUnion"})
        emitByteProbe(session, name);
    }
    session.verify();
    std::error_code error;
    llvm::raw_fd_ostream ir(argv[3], error);
    require(!error, "cannot open IR output");
    session.module().print(ir, nullptr);
    ir.close();
    const auto object = session.emitObject();
    std::ofstream output(argv[2], std::ios::binary);
    output.write(object.contents.data(), object.contents.size());
    require(output.good(), "cannot write object output");
    std::cout
        << "classifier/oracle object O" << level
        << (text         ? ": public counted C input record contracts; "
            : storage    ? ": exact foreign storage contracts; "
            : preflight  ? ": real GTI body native preflight; "
            : aggregates ? ": exact passive aggregate import contracts; "
            : storageContracts
                ? ": exact foreign storage projection contracts; "
            : contracts ? ": exact declaration/call contracts; "
            : argc == 6
                ? ": isolated callback boundary test bodies; "
                : ": 25 records, 3 pressure signatures, explicit rejections; ")
        << object.contents.size() << " bytes; LLVM " << LLVM_VERSION_STRING
        << "; " << llvm::Triple(session.module().getTargetTriple()).str()
        << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
