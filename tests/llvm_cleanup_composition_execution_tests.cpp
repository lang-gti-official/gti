#include "../src/compiler/llvm/dispatch.h"
#include "../src/compiler/llvm/failure.h"
#include "../src/compiler/llvm/lifetimes.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/process.h"
#include "gti/llvm_backend.h"
#include "gti/mir_effects.h"
#include "gti/runtime_failure.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {
// A separate test executable owns this private shim. It can emit only the
// component module assembled below; it cannot create a public admission plan.
struct DispatchStructuralTestAccess {
  static Dispatch inspect(Session &session) { return Dispatch(session); }
};
} // namespace lang::native_llvm

namespace {
using namespace lang;
using namespace lang::native_llvm;

const MirClassInstance &named(const LoweredProgram &program,
                              std::string_view name) {
  for (const auto &owner : program.mir().classInstances()) {
    const auto *declaration = program.findClassDeclaration(owner.declaration);
    if (declaration && declaration->qualifiedName == name)
      return owner;
  }
  throw std::runtime_error("missing recursive cleanup fixture class");
}

const MirInstruction &failureInstruction(const MirDestructorInstance &target) {
  for (const auto &block : target.body.blocks)
    for (const auto &instruction : block.instructions)
      if (!instruction.definedFailure.localOrigins.empty() &&
          std::any_of(
              instruction.definedFailure.localOrigins.begin(),
              instruction.definedFailure.localOrigins.end(),
              [](const auto &origin) { return !origin.outcomes.empty(); }))
        return instruction;
  throw std::runtime_error("failing destructor has no local failure origin");
}

DefinedFailureOutcome failureOutcome(const MirInstruction &instruction) {
  for (const auto &origin : instruction.definedFailure.localOrigins)
    if (!origin.outcomes.empty())
      return origin.outcomes.front();
  throw std::runtime_error("failure instruction has no authorized outcome");
}

llvm::Argument *channelArgument(llvm::Function &function,
                                const FunctionAbi &abi) {
  if (!abi.failureDestination || *abi.failureDestination >= function.arg_size())
    throw std::runtime_error("failing destructor lost its failure channel");
  return function.getArg(*abi.failureDestination);
}

struct ScratchDirectory {
  std::filesystem::path path;
  ~ScratchDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

class NativeCleanupCase final {
public:
  NativeCleanupCase(const VerifiedLoweredProgram &program,
                    LlvmCodegenOptimization optimization,
                    std::filesystem::path runtime,
                    std::filesystem::path scratch)
      : session(program, {.optimization = optimization}),
        dispatch(DispatchStructuralTestAccess::inspect(session)),
        failures(session),
        lifetimes(session, failures, dispatch,
                  [&](MirBodyAddress address) { return destructor(address); }),
        runtime(std::move(runtime)), scratch(std::move(scratch)) {}

  void execute(std::string_view graph) {
    const auto &node = named(session.program(), "Node");
    const auto &left = named(session.program(), "Left");
    const auto &right = named(session.program(), "Right");
    failingOwners = {node.id, left.id};

    const MirClassInstance *root = nullptr;
    const MirDropType *recipe = nullptr;
    std::optional<MirDropType> selected;
    if (graph == "recursive") {
      root = &node;
      selected = mirUniqueCleanupType(session.program().mir(), node.type);
    } else if (graph == "mutual") {
      root = &left;
      selected = mirUniqueCleanupType(session.program().mir(), left.type);
    } else {
      throw std::invalid_argument("unknown cleanup graph");
    }
    if (!selected)
      throw std::runtime_error("recursive cleanup fixture lost its recipe");
    recipe = &*selected;

    auto *cleanup = lifetimes.cleanup(*recipe, true);
    auto *main = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(session.context()),
                                false),
        llvm::GlobalValue::ExternalLinkage, "main", session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", main));

    auto *rootAddress = allocate(ir, *root);
    llvm::Value *middle = nullptr;
    llvm::Value *tail = nullptr;
    llvm::Value *allocationFailed = ir.CreateIsNull(rootAddress);
    if (graph == "recursive") {
      tail = allocate(ir, node);
      allocationFailed = ir.CreateOr(allocationFailed, ir.CreateIsNull(tail));
    } else {
      middle = allocate(ir, right);
      tail = allocate(ir, left);
      allocationFailed =
          ir.CreateOr(allocationFailed, ir.CreateOr(ir.CreateIsNull(middle),
                                                    ir.CreateIsNull(tail)));
    }

    auto *ready = llvm::BasicBlock::Create(session.context(), "ready", main);
    auto *outOfMemory =
        llvm::BasicBlock::Create(session.context(), "out.of.memory", main);
    ir.CreateCondBr(allocationFailed, outOfMemory, ready);
    ir.SetInsertPoint(outOfMemory);
    ir.CreateRet(ir.getInt32(92));
    ir.SetInsertPoint(ready);
    if (graph == "recursive") {
      initialize(ir, node, rootAddress);
      initialize(ir, node, tail);
      connect(ir, node, rootAddress, node, tail);
      connect(ir, node, tail, node, nullptr);
    } else {
      initialize(ir, left, rootAddress);
      initialize(ir, right, middle);
      initialize(ir, left, tail);
      connect(ir, left, rootAddress, right, middle);
      connect(ir, right, middle, left, tail);
      connect(ir, left, tail, right, nullptr);
    }
    auto *channel = ir.CreateAlloca(session.abi().failureChannel());
    ir.CreateStore(llvm::Constant::getNullValue(session.abi().failureChannel()),
                   channel);
    static_cast<void>(
        ir.CreateCall(cleanup, {rootAddress, channel, ir.getFalse()}));
    ir.CreateRet(ir.getInt32(91));

    session.verify();
    const auto artifact = session.emitObject();
    if (artifact.kind != BackendArtifactKind::Object ||
        artifact.contents.empty())
      throw std::runtime_error("cleanup component emitted no object");
    const auto object = scratch / (std::string(graph) + ".o");
    if (driver::writeArtifact(object, artifact.contents) !=
        driver::ArtifactWriteStatus::Success)
      throw std::runtime_error("failed to write cleanup component object");
    driver::NativeInputs inputs;
    inputs.finalLibraryFiles.push_back(runtime);
    const auto executable = scratch / std::string(graph);
    const auto linked = driver::NativeToolchain().invoke(
        driver::NativeLinkRequest(driver::discoverNativeCompiler(), object,
                                  executable, inputs),
        {.captureSuccessfulOutput = true});
    if (!linked.succeeded())
      throw std::runtime_error("cleanup component link failed: " +
                               linked.output);
    const auto result = driver::invokeProcess(
        {executable.string()}, {.captureSuccessfulOutput = true,
                                .timeout = std::chrono::seconds(30),
                                .isolateProcessGroup = true});
    if (result.driverDiagnostic || result.timedOut ||
        result.exitCode != GTI_FAILURE_EXIT_STATUS ||
        result.output.find("GTI-R0014") == std::string::npos ||
        result.output.find("failure_during_cleanup") == std::string::npos ||
        result.output.find("primary [GTI-R0001]") == std::string::npos ||
        result.output.find("secondary [GTI-R0001]") == std::string::npos)
      throw std::runtime_error(
          "cleanup component did not reach the secondary-failure boundary " +
          std::string(graph) + " (exit " + std::to_string(result.exitCode) +
          "): " + result.output);
  }

private:
  llvm::Function *destructor(MirBodyAddress address) {
    if (address.kind != MirBodyKind::Destructor)
      throw std::runtime_error("cleanup requested a non-destructor body");
    if (const auto found = destructorBodies.find(address.owner);
        found != destructorBodies.end())
      return found->second;
    const auto *target =
        session.program().mir().findDestructorInstance(address.owner);
    if (!target)
      throw std::runtime_error("cleanup lost its destructor identity");
    const auto abi = session.abi().gti(SemanticType::Void, {},
                                       target->mayRaiseDefinedFailure, true);
    auto *function = llvm::Function::Create(
        abi.type, llvm::GlobalValue::InternalLinkage,
        "cleanup.native.destructor." + std::to_string(address.owner),
        session.module());
    abi.applyTo(*function);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    if (failingOwners.contains(target->owner)) {
      if (!abi.statusResult)
        throw std::runtime_error("failing destructor has no status result");
      const auto &instruction = failureInstruction(*target);
      failures.originate(ir, channelArgument(*function, abi), instruction,
                         failureOutcome(instruction));
      ir.CreateRet(ir.getFalse());
    } else if (abi.statusResult) {
      ir.CreateRet(ir.getTrue());
    } else {
      ir.CreateRetVoid();
    }
    destructorBodies.emplace(address.owner, function);
    return function;
  }

  llvm::Value *allocate(llvm::IRBuilder<> &ir, const MirClassInstance &owner) {
    const auto layout = session.layouts().get(owner.type);
    const auto scalar =
        session.program().mir().target().nativeCScalar(NativeCScalarKind::Size);
    if (!scalar || !scalar->layout.sizeBytes)
      throw std::runtime_error("target has no represented size_t");
    auto *size = llvm::IntegerType::get(
        session.context(), static_cast<unsigned>(scalar->layout.sizeBytes * 8));
    const bool windows = session.program().mir().target().os == "windows";
    auto *type =
        llvm::FunctionType::get(session.layouts().pointer(),
                                windows ? std::vector<llvm::Type *>{size, size}
                                        : std::vector<llvm::Type *>{size},
                                false);
    const auto callee = session.module().getOrInsertFunction(
        windows ? "_aligned_malloc" : "malloc", type);
    std::vector<llvm::Value *> arguments{
        llvm::ConstantInt::get(size, layout.size)};
    if (windows)
      arguments.push_back(llvm::ConstantInt::get(size, layout.alignment));
    return ir.CreateCall(callee, arguments);
  }

  void initialize(llvm::IRBuilder<> &ir, const MirClassInstance &owner,
                  llvm::Value *address) {
    const auto layout = session.layouts().get(owner.type);
    ir.CreateMemSet(address, ir.getInt8(0), layout.size,
                    llvm::MaybeAlign(layout.alignment));
    lifetimes.active(ir, address, owner.type, true);
  }

  void connect(llvm::IRBuilder<> &ir, const MirClassInstance &owner,
               llvm::Value *address, const MirClassInstance &pointee,
               llvm::Value *child) {
    const auto recipe =
        mirUniqueCleanupType(session.program().mir(), owner.type);
    if (!recipe)
      throw std::runtime_error("recursive cleanup owner has no recipe");
    const auto edge = std::find_if(
        recipe->aggregate.begin(), recipe->aggregate.end(),
        [&](const auto &candidate) { return candidate.uniqueOwnerField != 0; });
    if (edge == recipe->aggregate.end() ||
        std::find_if(std::next(edge), recipe->aggregate.end(),
                     [](const auto &candidate) {
                       return candidate.uniqueOwnerField != 0;
                     }) != recipe->aggregate.end())
      throw std::runtime_error("recursive cleanup fixture lost its owner edge");
    const auto reachesPointee = [&](const auto &self,
                                    const MirDropType &candidate) -> bool {
      if (candidate.type == pointee.type)
        return true;
      return std::any_of(
          candidate.aggregate.begin(), candidate.aggregate.end(),
          [&](const auto &childRecipe) { return self(self, childRecipe); });
    };
    if (!reachesPointee(reachesPointee, *edge))
      throw std::runtime_error(
          "recursive cleanup owner edge selected a foreign pointee");
    auto *slot = ir.CreateStructGEP(
        session.layouts().get(owner.type).storage, address,
        session.layouts().fieldIndex(owner.type, edge->uniqueOwnerField));
    if (!child)
      child = llvm::ConstantPointerNull::get(session.layouts().pointer());
    ir.CreateStore(child, slot);
  }

  Session session;
  Dispatch dispatch;
  Failures failures;
  Lifetimes lifetimes;
  std::filesystem::path runtime;
  std::filesystem::path scratch;
  std::set<HirClassInstanceId> failingOwners;
  std::map<HirDestructorInstanceId, llvm::Function *> destructorBodies;
};

VerifiedLoweredProgram loadProgram(const std::filesystem::path &repository) {
  const auto frontend = Frontend().analyze(
      repository / "tests/fixtures/recursive_cleanup_failure.gti", std::nullopt,
      {repository / "stdlib/prelude.gti"}, {}, {repository / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("recursive cleanup fixture did not analyze");
  }
  std::vector<LoweredProgramIssue> issues;
  auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!program) {
    for (const auto &issue : issues)
      std::cerr << issue.detail << '\n';
    throw std::runtime_error("recursive cleanup fixture did not seal");
  }
  return *program;
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::invalid_argument(
          "usage: cleanup-composition-execution <repository> <runtime>");
    const auto repository = std::filesystem::canonical(argv[1]);
    const auto runtime = std::filesystem::canonical(argv[2]);
    const auto program = loadProgram(repository);
    const auto base =
        std::filesystem::temp_directory_path() /
        ("gti-llvm-cleanup-composition-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(base);
    ScratchDirectory cleanup{base};
    for (const auto optimization :
         {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive}) {
      const auto level = optimization == LlvmCodegenOptimization::None
                             ? std::string("O0")
                             : std::string("O3");
      for (const std::string graph : {"recursive", "mutual"}) {
        const auto scratch = base / (graph + "-" + level);
        std::filesystem::create_directory(scratch);
        NativeCleanupCase(program, optimization, runtime, scratch)
            .execute(graph);
        std::cout << graph << " cleanup: " << level
                  << " reached GTI-R0014 with primary/secondary GTI-R0001\n";
      }
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
