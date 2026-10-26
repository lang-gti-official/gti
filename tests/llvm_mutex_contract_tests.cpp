#include "../src/compiler/llvm/lifetimes.h"
#include "../src/compiler/llvm/mutexes.h"
#include "cpp_backend_test_support.h"
#include "gti/mir_effects.h"

#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <map>

namespace {
using namespace lang;
using namespace lang::native_llvm;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void check(const std::filesystem::path &root,
           const std::filesystem::path &output, std::string_view sanitizer) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/mutex_basic.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  require(frontend.canGenerateCode(), "mutex producer reaches MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(), "mutex source has a verified snapshot");
  require(!planLlvmAdmission(*input).admitted(),
          "native shell lifecycle remains closed to public admission");
  Session session(*input, {});
  const auto &program = input->program().mir();
  SemanticType storage, token;
  for (const auto &owner : program.classInstances()) {
    if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage)
      storage = owner.type;
    if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexLockToken)
      token = owner.type;
  }
  require(storage != SemanticType::Void && token != SemanticType::Void,
          "exact source has both mutex capabilities");
  bool constructorSeen = false;
  for (const auto &constructor : program.constructorInstances()) {
    const auto *owner = program.findClassInstance(constructor.owner);
    if (!owner || owner->type != storage)
      continue;
    constructorSeen = true;
    require(nativeMutexConstructor(program, constructor) == SemanticType::Int32,
            "sealed private constructor selects its exact payload transport");
    auto changed = constructor;
    changed.parameterTypes.clear();
    require(!nativeMutexConstructor(program, changed),
            "constructor parameter mismatch rejected");
    changed = constructor;
    changed.body.places.clear();
    require(!nativeMutexConstructor(program, changed),
            "constructor binding mismatch rejected");
    changed = constructor;
    changed.body.blocks.front().instructions.push_back(
        {.kind = MirInstructionKind::Call});
    require(!nativeMutexConstructor(program, changed),
            "constructor cannot hide a source call");
  }
  require(constructorSeen,
          "source materializes the native payload constructor");
  const auto layout = session.layouts().mutexStorage(storage);
  require(layout.element == SemanticType::Int32 &&
              layout.object.alignment == 8 &&
              layout.object.fieldOffsets[0] == 0 &&
              layout.object.fieldOffsets[1] == 64 &&
              layout.object.fieldOffsets[2] == 68 && layout.object.size == 72,
          "native slot is 64-byte aligned-eight, distinct from payload and "
          "active byte");
  require(session.layouts().get(token).size == 8,
          "guard token holds exactly one native identity pointer");
  const std::map<IntrinsicKind, std::string> names{
      {IntrinsicKind::MutexLock, "lock"},
      {IntrinsicKind::MutexUnlock, "unlock"},
      {IntrinsicKind::MutexGuardActive, "active"},
      {IntrinsicKind::MutexGuardRead, "read"},
      {IntrinsicKind::MutexGuardReadMut, "read_mut"}};
  std::size_t seen = 0;
  std::vector<const MirBody *> bodies;
  for (const auto &owner : program.functionInstances())
    bodies.push_back(&owner.body);
  for (const auto &owner : program.destructorInstances())
    bodies.push_back(&owner.body);
  for (const auto *body : bodies)
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        if (!Mutexes::handles(instruction.intrinsic))
          continue;
        require(nativeMutexCall(program, *body, instruction),
                "exact mutex plan recognized");
        const auto reject = [&](auto mutate) {
          auto bad = instruction;
          mutate(bad);
          require(!nativeMutexCall(program, *body, bad),
                  "malformed mutex plan rejected");
        };
        reject([](auto &call) { call.parameterTypes.clear(); });
        reject(
            [](auto &call) { call.operands[0].kind = MirOperandKind::Move; });
        reject([](auto &call) { call.destination = 1; });
        reject([](auto &call) {
          call.synchronization.order = AtomicMemoryOrder::Acquire;
        });
        reject([](auto &call) { call.functionTarget = 1; });
        if (instruction.intrinsic == IntrinsicKind::MutexGuardRead ||
            instruction.intrinsic == IntrinsicKind::MutexGuardReadMut)
          reject(
              [](auto &call) { call.borrowOrigin = BorrowOriginKind::None; });
        const auto name = "gti_test_mutex_" + names.at(instruction.intrinsic);
        if (session.module().getFunction(name))
          continue;
        ++seen;
        llvm::Type *result =
            instruction.intrinsic == IntrinsicKind::MutexUnlock
                ? llvm::Type::getVoidTy(session.context())
            : instruction.intrinsic == IntrinsicKind::MutexLock
                ? llvm::Type::getInt32Ty(session.context())
            : instruction.intrinsic == IntrinsicKind::MutexGuardActive
                ? llvm::Type::getInt1Ty(session.context())
                : static_cast<llvm::Type *>(session.layouts().pointer());
        std::vector<llvm::Type *> parameters(instruction.operands.size(),
                                             session.layouts().pointer());
        auto *function = llvm::Function::Create(
            llvm::FunctionType::get(result, parameters, false),
            llvm::GlobalValue::ExternalLinkage, name, session.module());
        if (result->isIntegerTy(1))
          function->addRetAttr(llvm::Attribute::ZExt);
        llvm::IRBuilder<> ir(
            llvm::BasicBlock::Create(session.context(), "entry", function));
        std::vector<llvm::Value *> arguments;
        for (auto &argument : function->args())
          arguments.push_back(&argument);
        const auto before = ir.GetInsertBlock()->size();
        auto malformed = arguments;
        malformed[0] = ir.getInt32(0);
        bool refused = false;
        try {
          static_cast<void>(
              Mutexes(session, ir).emit(*body, instruction, malformed));
        } catch (const BackendUnsupportedError &) {
          refused = true;
        }
        require(refused && ir.GetInsertBlock()->size() == before,
                "bad materialized lane emits nothing");
        auto *value = Mutexes(session, ir).emit(*body, instruction, arguments);
        if (value)
          ir.CreateRet(value);
        else
          ir.CreateRetVoid();
      }
  require(seen == 5,
          "all five non-failure mutex operations have source witnesses");
  const auto function = [&](const char *name,
                            std::vector<llvm::Type *> parameters) {
    return llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                parameters, false),
        llvm::GlobalValue::ExternalLinkage, name, session.module());
  };
  auto *initialize = function(
      "gti_test_mutex_initialize",
      {session.layouts().pointer(), llvm::Type::getInt32Ty(session.context())});
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", initialize));
  Mutexes mutexes(session, ir);
  mutexes.initializeShell(initialize->getArg(0), storage);
  ir.CreateStore(initialize->getArg(1),
                 ir.CreateStructGEP(layout.object.storage,
                                    initialize->getArg(0),
                                    layout.payloadIndex));
  ir.CreateStore(ir.getInt8(MutexStorageLayout::shellLive |
                            MutexStorageLayout::payloadLive),
                 ir.CreateStructGEP(layout.object.storage,
                                    initialize->getArg(0),
                                    layout.payloadActiveIndex));
  ir.CreateRetVoid();
  auto *destroy =
      function("gti_test_mutex_destroy", {session.layouts().pointer()});
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", destroy));
  mutexes.destroyShell(destroy->getArg(0), storage);
  ir.CreateRetVoid();
  auto *initializeToken =
      function("gti_test_mutex_token", {session.layouts().pointer()});
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", initializeToken));
  mutexes.initializeToken(initializeToken->getArg(0), token);
  ir.CreateRetVoid();
  auto *move =
      function("gti_test_mutex_move_token",
               {session.layouts().pointer(), session.layouts().pointer()});
  ir.SetInsertPoint(llvm::BasicBlock::Create(session.context(), "entry", move));
  mutexes.moveToken(move->getArg(0), move->getArg(1), token);
  ir.CreateRetVoid();
  Failures failures(session);
  Lifetimes lifetimes(
      session, failures, [](MirBodyAddress) -> llvm::Function * {
        throw std::logic_error("passive shell has no source cleanup body");
      });
  auto *transfer =
      function("gti_test_mutex_transfer",
               {session.layouts().pointer(), session.layouts().pointer()});
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", transfer));
  require(lifetimes.transferMutexes(ir, transfer->getArg(0),
                                    transfer->getArg(1), storage),
          "sealed mutex storage uses native identity transfer");
  ir.CreateRetVoid();
  const auto recipe = mirUniqueCleanupType(program, storage);
  require(recipe && recipe->requiresActiveCleanup && recipe->aggregate.empty(),
          "source-derived passive shell cleanup needs no fabricated payload "
          "recipe");
  auto *cleanup =
      function("gti_test_mutex_cleanup", {session.layouts().pointer()});
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", cleanup));
  ir.CreateCall(lifetimes.cleanup(*recipe),
                {cleanup->getArg(0),
                 ir.CreateAlloca(session.abi().failureChannel()),
                 ir.getFalse()});
  ir.CreateRetVoid();
  if (!sanitizer.empty())
    for (auto &function : session.module())
      if (!function.isDeclaration())
        function.addFnAttr(sanitizer == "thread"
                               ? llvm::Attribute::SanitizeThread
                               : llvm::Attribute::SanitizeAddress);
  session.verify();
  if (!output.empty()) {
    std::error_code error;
    llvm::raw_fd_ostream stream(output.string(), error);
    if (error)
      throw std::runtime_error(error.message());
    session.module().print(stream, nullptr);
    stream.close();
    require(!stream.has_error(), "write source-verified mutex adapter IR");
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3 && argc != 4)
    return 2;
  const std::string_view sanitizer = argc == 4 ? argv[3] : "";
  if (!sanitizer.empty() && sanitizer != "thread" &&
      sanitizer != "address,undefined")
    return 2;
  try {
    check(argv[1], argc >= 3 ? argv[2] : "", sanitizer);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
