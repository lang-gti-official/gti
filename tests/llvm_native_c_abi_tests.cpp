#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/coverage.h"
#include "../src/compiler/llvm/failure.h"
#include "../src/compiler/llvm/native_c_abi.h"
#include "../src/compiler/llvm/native_c_contract.h"
#include "cpp_backend_test_support.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
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
  for (const auto &function : program.functionInstances()) {
    if (function.linkage == LanguageLinkage::C) {
      require(nativeCDeclarationContract(program, target, function),
              "ordinary unrelated C declarations need structural contracts");
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
        if (instruction.nativeCalleeType) {
          require(nativeCIndirectCallContract(program, target, function.body,
                                              instruction),
                  "verified native pointer call requires its exact native "
                  "signature");
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

int main(int argc, char **argv) {
  try {
    const bool contracts = argc == 6 && std::string(argv[5]) == "--contracts";
    const bool preflight =
        argc == 6 && std::string(argv[5]) == "--source-preflight";
    const bool storage = argc == 6 && std::string(argv[5]) == "--storage";
    const bool concurrent =
        argc == 6 && std::string(argv[5]) == "--thread-callbacks";
    require(argc == 5 || (argc == 6 &&
                          (std::string(argv[5]) == "--callbacks" ||
                           concurrent || contracts || preflight || storage)),
            "usage: test fixture.gti output.o output.ll O[0-3] "
            "[--callbacks|--thread-callbacks]");
    const auto input = fixture(argv[1], concurrent, argc == 6);
    const auto level = std::stoi(argv[4]);
    require(level >= 0 && level <= 3, "invalid optimization level");
    const LlvmCodegenOptimization levels[] = {
        LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default, LlvmCodegenOptimization::Aggressive};
    Session session(input, {.optimization = levels[level]});
    if (storage) {
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
        << (storage     ? ": exact foreign storage contracts; "
            : preflight ? ": real GTI body native preflight; "
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
