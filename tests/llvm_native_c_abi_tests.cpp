#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/failure.h"
#include "../src/compiler/llvm/native_c_abi.h"
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
      standardLibraryEnabled ? Frontend(options).analyze(source.string(), text,
                                             {standardLibrary / "prelude.gti"},
                                             {}, {standardLibrary})
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

} // namespace

int main(int argc, char **argv) {
  try {
    const bool concurrent =
        argc == 6 && std::string(argv[5]) == "--thread-callbacks";
    require(argc == 5 || (argc == 6 && (std::string(argv[5]) == "--callbacks" ||
                                        concurrent)),
            "usage: test fixture.gti output.o output.ll O[0-3] "
            "[--callbacks|--thread-callbacks]");
    const auto input = fixture(argv[1], concurrent, argc == 6);
    const auto level = std::stoi(argv[4]);
    require(level >= 0 && level <= 3, "invalid optimization level");
    const LlvmCodegenOptimization levels[] = {
        LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default, LlvmCodegenOptimization::Aggressive};
    Session session(input, {.optimization = levels[level]});
    if (argc == 6) {
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
        << (argc == 6
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
