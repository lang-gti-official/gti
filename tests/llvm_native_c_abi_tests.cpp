#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/native_c_abi.h"
#include "cpp_backend_test_support.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <filesystem>
#include <algorithm>
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

VerifiedLoweredProgram fixture(const std::filesystem::path &source) {
  std::ifstream stream(source);
  require(stream.good(), "missing GTI fixture");
  const std::string text{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = Frontend().analyze(source.string(), text);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("invalid GTI record fixture");
  }
  std::vector<LoweredProgramIssue> issues;
  auto verified = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
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
  return builder.CreateGEP(builder.getInt8Ty(), pointer, builder.getInt64(bytes));
}

// Coercions may be wider than the object (e.g. Darwin's i64 for a 3-byte
// record). Use zeroed, correctly sized scratch and copy only actual bytes.
llvm::Value *pack(Session &session, llvm::IRBuilder<> &builder,
                  llvm::Value *storage, llvm::Type *carrier,
                  std::uint64_t at, std::uint64_t size) {
  const auto carrierSize =
      session.dataLayout().getTypeAllocSize(carrier).getFixedValue();
  auto *scratch = builder.CreateAlloca(carrier);
  builder.CreateStore(llvm::Constant::getNullValue(carrier), scratch);
  builder.CreateMemCpy(scratch, llvm::MaybeAlign(1), offset(builder, storage, at),
                      llvm::MaybeAlign(1), std::min(carrierSize, size - at));
  return builder.CreateLoad(carrier, scratch);
}

void unpack(Session &session, llvm::IRBuilder<> &builder,
            llvm::Value *storage, llvm::Value *value, std::uint64_t at,
            std::uint64_t size) {
  const auto carrierSize =
      session.dataLayout().getTypeAllocSize(value->getType()).getFixedValue();
  auto *scratch = builder.CreateAlloca(value->getType());
  builder.CreateStore(value, scratch);
  builder.CreateMemCpy(offset(builder, storage, at), llvm::MaybeAlign(1), scratch,
                      llvm::MaybeAlign(1), std::min(carrierSize, size - at));
}

void applyByval(Session &session, const NativeCRecordAbi &plan,
                llvm::Function &function, unsigned index) {
  if (!plan.argument.byValue)
    return;
  function.addParamAttr(index, llvm::Attribute::getWithByValType(
                                   session.context(), plan.storage.storage));
  function.addParamAttr(index, llvm::Attribute::getWithAlignment(
                                   session.context(),
                                   llvm::Align(plan.argument.stackAlignment)));
}

void applyByval(Session &session, const NativeCRecordAbi &plan,
                llvm::CallBase &call, unsigned index) {
  if (!plan.argument.byValue)
    return;
  call.addParamAttr(index, llvm::Attribute::getWithByValType(
                               session.context(), plan.storage.storage));
  call.addParamAttr(index, llvm::Attribute::getWithAlignment(
                               session.context(),
                               llvm::Align(plan.argument.stackAlignment)));
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
  const auto plan = classifyNativeCRecord(
      session, instance.type,
      {6 - probe.integerPrefix, 8 - probe.vectorPrefix});
  const bool sysv = llvm::Triple(session.module().getTargetTriple()).isOSLinux();
  const bool pressure = prefix != 0;
  require(plan.argument.byValue == (sysv && pressure),
          "System V pressure must roll back whole aggregate to byval stack");
  if (sysv && pressure)
    require(plan.remaining.integers == 6 - probe.integerPrefix &&
                plan.remaining.vectors == 8 - probe.vectorPrefix,
            "stack rollback must preserve remaining registers for later args");
  rejects([&] {
    const std::vector<SemanticType> args{instance.type};
    static_cast<void>(session.abi().nativeC(instance.type, args));
  });

  std::vector<llvm::Type *> parameters;
  for (unsigned i = 0; i < probe.integerPrefix; ++i)
    parameters.push_back(llvm::Type::getInt64Ty(session.context()));
  for (unsigned i = 0; i < probe.vectorPrefix; ++i)
    parameters.push_back(llvm::Type::getDoubleTy(session.context()));
  if (plan.argument.expand) {
    for (const auto &piece : plan.argument.pieces)
      parameters.push_back(piece.type);
  } else {
    parameters.push_back(plan.argument.coercion);
  }
  if (probe.integerTail)
    parameters.push_back(llvm::Type::getInt64Ty(session.context()));
  if (probe.vectorTail)
    parameters.push_back(llvm::Type::getDoubleTy(session.context()));
  auto *signature =
      llvm::FunctionType::get(plan.result.coercion, parameters, false);
  auto *callee = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                       "c_" + probe.symbol, session.module());
  auto *function = llvm::Function::Create(
      signature, llvm::Function::ExternalLinkage, "llvm_" + probe.symbol,
      session.module());
  applyByval(session, plan, *function, prefix);
  applyByval(session, plan, *callee, prefix);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *local = builder.CreateAlloca(plan.storage.storage);
  builder.CreateStore(llvm::Constant::getNullValue(plan.storage.storage), local);
  if (plan.argument.byValue) {
    builder.CreateMemCpy(local, llvm::MaybeAlign(plan.storage.alignment),
                        function->getArg(prefix),
                        llvm::MaybeAlign(plan.argument.stackAlignment),
                        plan.storage.size);
  } else if (plan.argument.expand) {
    for (unsigned i = 0; i < plan.argument.pieces.size(); ++i)
      unpack(session, builder, local, function->getArg(prefix + i),
             plan.argument.pieces[i].offset, plan.storage.size);
  } else {
    unpack(session, builder, local, function->getArg(prefix), 0,
           plan.storage.size);
  }
  const auto &fields = instance.cAbiLayout->fields;
  for (unsigned index = 0; index < fields.size(); ++index) {
    const auto &field = fields[index];
    auto *fieldType = session.layouts().get(field.type).storage;
    auto *address = offset(builder, local, field.offsetBytes);
    auto *value = builder.CreateLoad(fieldType, address);
    llvm::Value *increment = fieldType->isFloatingPointTy()
                                 ? llvm::ConstantFP::get(fieldType, 1.0)
                                 : static_cast<llvm::Value *>(builder.getInt64(1));
    if (pressure) {
      if (index == 0) {
        increment = probe.vectorPrefix
                        ? static_cast<llvm::Value *>(llvm::ConstantFP::get(
                              builder.getDoubleTy(), 0.0))
                        : static_cast<llvm::Value *>(builder.getInt64(0));
        for (unsigned i = 0; i < prefix; ++i)
          increment = probe.vectorPrefix
                          ? builder.CreateFAdd(increment, function->getArg(i))
                          : builder.CreateAdd(increment, function->getArg(i));
        if (probe.symbol == "gpr")
          increment = builder.CreateAdd(increment, function->getArg(
                                                       parameters.size() - 1));
      } else if (probe.symbol != "gpr") {
        increment = function->getArg(parameters.size() - 1);
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
  const auto resultValue = [&](llvm::Value *storage) {
    if (!sysv || plan.result.pieces.size() == 1)
      return pack(session, builder, storage, plan.result.coercion, 0,
                  plan.storage.size);
    llvm::Value *value = llvm::PoisonValue::get(plan.result.coercion);
    for (unsigned i = 0; i < plan.result.pieces.size(); ++i) {
      const auto &piece = plan.result.pieces[i];
      value = builder.CreateInsertValue(
          value, pack(session, builder, storage, piece.type, piece.offset,
                      plan.storage.size), i);
    }
    return value;
  };
  builder.CreateRet(resultValue(local));

  auto *pointer = session.layouts().pointer();
  auto *bridgeType = llvm::FunctionType::get(builder.getVoidTy(),
                                             {pointer, pointer}, false);
  auto *bridge = llvm::Function::Create(bridgeType, llvm::Function::ExternalLinkage,
                                       "import_" + probe.symbol,
                                       session.module());
  builder.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", bridge));
  std::vector<llvm::Value *> arguments;
  for (unsigned i = 0; i < probe.integerPrefix; ++i)
    arguments.push_back(builder.getInt64(i + 1));
  for (unsigned i = 0; i < probe.vectorPrefix; ++i)
    arguments.push_back(llvm::ConstantFP::get(builder.getDoubleTy(), i + 1));
  if (plan.argument.byValue) {
    // byval pointer alignment must be real, not asserted on an arbitrary input.
    auto *copy = builder.CreateAlloca(plan.storage.storage);
    copy->setAlignment(llvm::Align(plan.argument.stackAlignment));
    builder.CreateMemCpy(copy, llvm::MaybeAlign(plan.argument.stackAlignment),
                        bridge->getArg(0), llvm::MaybeAlign(1), plan.storage.size);
    arguments.push_back(copy);
  } else if (plan.argument.expand) {
    for (const auto &piece : plan.argument.pieces)
      arguments.push_back(pack(session, builder, bridge->getArg(0), piece.type,
                                piece.offset, plan.storage.size));
  } else {
    arguments.push_back(pack(session, builder, bridge->getArg(0),
                              plan.argument.coercion, 0, plan.storage.size));
  }
  if (probe.integerTail)
    arguments.push_back(builder.getInt64(17));
  if (probe.vectorTail)
    arguments.push_back(llvm::ConstantFP::get(builder.getDoubleTy(), 17.25));
  auto *call = builder.CreateCall(callee, arguments);
  applyByval(session, plan, *call, prefix);
  if (!sysv || plan.result.pieces.size() == 1) {
    unpack(session, builder, bridge->getArg(1), call, 0, plan.storage.size);
  } else {
    for (unsigned i = 0; i < plan.result.pieces.size(); ++i)
      unpack(session, builder, bridge->getArg(1),
             builder.CreateExtractValue(call, i), plan.result.pieces[i].offset,
             plan.storage.size);
  }
  builder.CreateRetVoid();

  if (pressure)
    return;
  auto *layoutType = llvm::FunctionType::get(builder.getInt64Ty(),
                                             {builder.getInt32Ty()}, false);
  auto *layout = llvm::Function::Create(layoutType, llvm::Function::ExternalLinkage,
                                       "layout_" + probe.symbol,
                                       session.module());
  builder.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", layout));
  std::vector<std::uint64_t> facts{plan.storage.size, plan.storage.alignment};
  facts.insert(facts.end(), plan.storage.fieldOffsets.begin(),
                plan.storage.fieldOffsets.end());
  llvm::Value *answer = builder.getInt64(UINT64_MAX);
  for (unsigned i = 0; i < facts.size(); ++i)
    answer = builder.CreateSelect(builder.CreateICmpEQ(layout->getArg(0),
                                                       builder.getInt32(i)),
                                    builder.getInt64(facts[i]), answer);
  builder.CreateRet(answer);
}

void negativeChecks(Session &session) {
  for (const auto *name : {"Nested", "ArrayField", "Large", "Boolean",
                            "UnionRecord", "Ordinary"})
    rejects([&] {
      static_cast<void>(classifyNativeCRecord(session, record(session, name).type,
                                               {6, 8}));
    });
  rejects([&] {
    static_cast<void>(classifyNativeCRecord(session, SemanticType::Int64, {6, 8}));
  });
  const auto pair = record(session, "Pair").type;
  rejects([&] {
    static_cast<void>(classifyNativeCRecord(session, pair, {7, 8}));
  });
  const auto triple = session.module().getTargetTriple();
#if LLVM_VERSION_MAJOR >= 21
  session.module().setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
#else
  session.module().setTargetTriple("x86_64-pc-windows-msvc");
#endif
  rejects([&] {
    static_cast<void>(classifyNativeCRecord(session, pair, {6, 8}));
  });
  session.module().setTargetTriple(triple);
  const auto layout = session.module().getDataLayout();
  session.module().setDataLayout("e-p:64:64-i64:32:32");
  rejects<std::logic_error>([&] {
    static_cast<void>(classifyNativeCRecord(session, pair, {6, 8}));
  });
  session.module().setDataLayout(layout);
}

} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 5, "usage: test records.gti output.o output.ll O[0-3]");
    const auto input = fixture(argv[1]);
    const auto level = std::stoi(argv[4]);
    require(level >= 0 && level <= 3, "invalid optimization level");
    const LlvmCodegenOptimization levels[] = {
        LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default, LlvmCodegenOptimization::Aggressive};
    Session session(input, {.optimization = levels[level]});
    negativeChecks(session);
    for (const auto *name : {"Byte", "Tiny", "Shorts", "Words", "Tail", "Pair",
                              "Pointer", "Float1", "Float2", "Float3", "Float4",
                              "Double1", "Double2", "IntDouble", "DoubleInt",
                              "FloatInt", "FloatDouble"})
      emitProbe(session, {name, name});
    emitProbe(session, {"Pair", "gpr", 5, 0, true, false});
    emitProbe(session, {"IntDouble", "mixed", 6, 0, false, true});
    emitProbe(session, {"Double2", "sse", 0, 8, true, false});
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
    std::cout << "classifier/oracle object O" << level
              << ": 17 records, 3 pressure signatures, explicit rejections; "
              << object.contents.size() << " bytes\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
