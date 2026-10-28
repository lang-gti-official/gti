#include "../src/compiler/llvm/atomic_types.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/lifetimes.h"
#include "../src/compiler/llvm/mutexes.h"
#include "../src/compiler/llvm/placement.h"
#include "cpp_backend_test_support.h"
#include "gti/mir_effects.h"

#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <set>
#include <tuple>

namespace {
using namespace lang;
using namespace lang::native_llvm;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void checkHierarchyContracts(const std::filesystem::path &root) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_mutex_hierarchy_contract.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(frontend.canGenerateCode(),
          "hierarchy capability source reaches MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(),
          "hierarchy capability source has a sealed snapshot");
  const auto &program = input->program().mir();
  unsigned checked = 0;
  for (const auto &owner : program.classInstances()) {
    const bool atomic =
        owner.compilerCapability == CompilerCapabilityTypeKind::AtomicStorage;
    const bool storage =
        owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage;
    const bool token =
        owner.compilerCapability == CompilerCapabilityTypeKind::MutexLockToken;
    if (!atomic && !storage && !token)
      continue;
    ++checked;
    if (atomic) {
      require(nativeAtomicElement(program, owner.type) == SemanticType::Int32,
              "atomic hierarchy proof consumes exact scalar storage");
    } else {
      const auto payload =
          nativeMutexElement(program, owner.type, owner.compilerCapability);
      require(payload && containsHierarchyStorage(program, *payload),
              "mutex payload recursively contains an ordinary hierarchy");
    }
    require(containsHierarchyStorage(program, owner.type) == storage,
            "only mutex storage inlines its payload; a token never does");
    const auto rejects = [&](auto mutate) {
      auto malformed = program;
      auto &changed = const_cast<MirClassInstance &>(
          *malformed.findClassInstance(owner.id));
      mutate(changed);
      bool rejected = false;
      try {
        static_cast<void>(containsHierarchyStorage(malformed, changed.type));
      } catch (const BackendUnsupportedError &) {
        rejected = true;
      }
      require(rejected,
              "malformed private layout rejects in the non-emitting query");
    };
    rejects([](auto &row) { row.polymorphic = true; });
    rejects([](auto &row) { row.requiresActiveDropState = true; });
    rejects([](auto &row) { row.cAbiLayout = MirCAbiRecordLayout{}; });
    rejects([](auto &row) { row.destructor = 1; });
    rejects([](auto &row) { row.fields.push_back({}); });
    rejects([](auto &row) { row.fieldDropOrder.push_back({}); });
    rejects([](auto &row) { row.type.arguments.clear(); });
    rejects([](auto &row) {
      row.compilerCapability = CompilerCapabilityTypeKind::MutexLockToken;
      row.requiresActiveCleanup = true;
    });
    auto duplicate = program;
    const_cast<std::vector<MirClassInstance> &>(duplicate.classInstances())
        .push_back(owner);
    bool rejected = false;
    try {
      static_cast<void>(containsHierarchyStorage(duplicate, owner.type));
    } catch (const BackendUnsupportedError &) {
      rejected = true;
    }
    require(rejected, "duplicate capability identity rejects before emission");
  }
  require(checked == 3, "atomic, mutex storage and token proofs all exercised");
  // The protected hierarchy payload is constructed, guarded and retired
  // natively under its own witness rather than refused as a transfer.
  const auto admission = planLlvmAdmission(*input);
  if (!admission.admitted())
    throw std::runtime_error(admission.limitation());
  require(std::any_of(admission.requirements().begin(),
                      admission.requirements().end(),
                      [](const auto &use) {
                        return use.implementedShape &&
                               use.implementedShape->name ==
                                   "NativeHierarchyMutexStorage";
                      }),
          "a mutex protecting a hierarchy payload has its own witness");
  std::cout
      << "private capability inline hierarchy and malformed controls PASS\n";
}

void checkPredicates(const std::filesystem::path &root) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/mutex_basic.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  require(frontend.canGenerateCode(), "mutex predicate source reaches MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(),
          "mutex predicates consume a sealed source snapshot");
  const auto &program = input->program().mir();
  std::vector<const MirBody *> bodies;
  for (const auto &owner : program.functionInstances())
    bodies.push_back(&owner.body);
  for (const auto &owner : program.destructorInstances())
    bodies.push_back(&owner.body);
  std::set<IntrinsicKind> seen;
  for (const auto *body : bodies)
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        const bool terminal =
            instruction.intrinsic == IntrinsicKind::MutexLockFailure;
        if (!terminal && !Mutexes::handles(instruction.intrinsic))
          continue;
        const auto recognized = [&](const MirInstruction &call) {
          return terminal ? nativeMutexLockFailure(program, call)
                          : nativeMutexCall(program, *body, call);
        };
        require(recognized(instruction), "exact mutex operation recognized");
        seen.insert(instruction.intrinsic);
        const auto reject = [&](auto mutate) {
          auto bad = instruction;
          mutate(bad);
          require(!recognized(bad),
                  "mutex operation rejects unrelated metadata");
        };
        reject([](auto &call) { call.nativeCalleeType = SemanticType::Int32; });
        reject([](auto &call) { call.nativeCallbackAdapter = 1; });
        reject([](auto &call) { call.threadTaskAdapter = 1; });
        reject([](auto &call) { call.functionPointerFunctionTarget = 1; });
        reject([](auto &call) { call.rawMemoryAccess = true; });
        reject([](auto &call) { call.dispatch = CallDispatch::Virtual; });
        reject([](auto &call) { call.objectLayoutType = SemanticType::Int32; });
        reject([](auto &call) {
          call.callableBoundary = CallableBoundary::Owned;
        });
        reject([](auto &call) { call.borrowArguments.push_back(0); });
        reject([](auto &call) { call.preparedParameterDrop = 1; });
        reject([](auto &call) { call.successResultDestination = 1; });
        if (terminal) {
          reject([](auto &call) { call.localFailureSites.clear(); });
          reject([](auto &call) { call.definedFailure.localOrigins.clear(); });
          reject([](auto &call) {
            ++call.definedFailure.localOrigins.front().start;
          });
          reject([](auto &call) {
            call.definedFailure.localOrigins.front().outcomes.front().detail =
                DefinedFailureDetail::FixedArray;
          });
          reject([](auto &call) {
            call.synchronization.kind = SynchronizationOperationKind::MutexLock;
          });
          reject([](auto &call) { call.info.type = SemanticType::Int32; });
        } else {
          reject([](auto &call) { call.parameterTypes.clear(); });
          reject(
              [](auto &call) { call.operands[0].kind = MirOperandKind::Move; });
          reject([](auto &call) {
            call.synchronization.order = AtomicMemoryOrder::Acquire;
          });
          if (instruction.loan) {
            reject([](auto &call) { call.loan.reset(); });
            auto missingLoan = *body;
            missingLoan.loans.clear();
            require(!nativeMutexCall(program, missingLoan, instruction),
                    "guard access requires its exact MIR result loan");
          }
          if (instruction.result) {
            auto missingValue = *body;
            missingValue.values.clear();
            require(!nativeMutexCall(program, missingValue, instruction),
                    "mutex result requires its exact MIR value identity");
          }
        }
      }
  require(seen.size() == 6,
          "all five mutex operations and lock failure checked");
  std::size_t constructors = 0;
  for (const auto &constructor : program.constructorInstances()) {
    const auto *owner = program.findClassInstance(constructor.owner);
    if (!owner ||
        owner->compilerCapability != CompilerCapabilityTypeKind::MutexStorage)
      continue;
    require(nativeMutexConstructor(program, constructor).has_value(),
            "mutex constructor binds exact private payload initialization");
    ++constructors;
    auto malformed = constructor;
    malformed.parameterTypes.clear();
    require(!nativeMutexConstructor(program, malformed),
            "mutex constructor rejects a mismatched payload parameter");
    malformed = constructor;
    malformed.body.places.clear();
    require(!nativeMutexConstructor(program, malformed),
            "mutex constructor rejects missing payload storage identity");
  }
  require(constructors != 0,
          "source predicate fixture materializes a native constructor");
  std::cout << "mutex exact predicates: six operation families and malformed "
               "controls PASS\n";
}

void requirePublicAdmission(const VerifiedLoweredProgram &input) {
  const auto plan = planLlvmAdmission(input);
  if (const auto *unsupported = plan.firstUnsupported())
    throw std::runtime_error("mutex public admission pending: " +
                             unsupported->entry->key());
  require(plan.admitted(), "mutex exact public admission");
  static_cast<void>(LlvmBackend().generate(input.program()));
}

// Exercise publication/retirement with source-selected roots and recipes.
// Shell initialization here is a kernel driver, not source-constructor
// evidence.
void checkProcessStorage(const std::filesystem::path &root,
                         const std::filesystem::path &output,
                         std::string_view sanitizer) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.lowerReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_native_process_storage.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  require(frontend.canGenerateCode(),
          "process shell source reaches verified MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(), "process shell source has a sealed snapshot");
  requirePublicAdmission(*input);
  Session session(*input, {});
  const auto &program = input->program().mir();
  std::map<SymbolId, llvm::GlobalVariable *> globals;
  require(std::count_if(program.programInitializationPlan().steps.begin(),
                        program.programInitializationPlan().steps.end(),
                        [](const auto &step) {
                          return step.nativeRetirement.has_value();
                        }) == 2,
          "global and static wrapper are exactly two process roots");
  for (const auto &step : program.programInitializationPlan().steps) {
    if (!step.nativeRetirement)
      continue;
    require(step.nativeRetirement &&
                mirDropIsNativeRetirement(program, *step.nativeRetirement),
            "process root has its exact native-only cleanup recipe");
    auto *type = session.layouts().get(step.nativeRetirement->type).storage;
    globals.emplace(step.symbol,
                    new llvm::GlobalVariable(session.module(), type, false,
                                             llvm::GlobalValue::InternalLinkage,
                                             llvm::Constant::getNullValue(type),
                                             "process-root"));
  }
  Failures failures(session);
  const auto admission = planLlvmAdmission(*input);
  Dispatch dispatch(session, admission);
  Lifetimes lifetimes(session, failures, dispatch,
                      [](MirBodyAddress) -> llvm::Function * {
                        throw std::logic_error(
                            "native process retirement invokes no source body");
                      });
  lifetimes.prepareProgramStorage(globals);
  auto *initialize = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {llvm::Type::getInt32Ty(session.context())},
                              false),
      llvm::GlobalValue::ExternalLinkage, "gti_test_program_init_prefix",
      session.module());
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", initialize));
  const auto initializeShells = [&](const auto &self, llvm::Value *address,
                                    const SemanticType &type) -> void {
    if (nativeMutexElement(program, type,
                           CompilerCapabilityTypeKind::MutexStorage)) {
      const auto layout = session.layouts().mutexStorage(type);
      Mutexes(session, ir).initializeShell(address, type);
      ir.CreateStore(ir.getInt32(7),
                     ir.CreateStructGEP(layout.object.storage, address,
                                        layout.payloadIndex));
      ir.CreateStore(ir.getInt8(MutexStorageLayout::shellLive |
                                MutexStorageLayout::payloadLive),
                     ir.CreateStructGEP(layout.object.storage, address,
                                        layout.payloadActiveIndex));
      return;
    }
    const auto &owner = session.layouts().instance(type);
    require(owner.bases.empty(),
            "kernel wrapper has no invented base schedule");
    for (std::size_t field = 0; field < owner.declaredFields.size(); ++field)
      self(self,
           ir.CreateStructGEP(session.layouts().get(type).storage, address,
                              field),
           owner.declaredFields[field].type);
  };
  unsigned prefix = 0;
  for (const auto &step : program.programInitializationPlan().steps) {
    if (!step.nativeRetirement)
      continue;
    auto *run =
        llvm::BasicBlock::Create(session.context(), "initialize", initialize);
    auto *next =
        llvm::BasicBlock::Create(session.context(), "next", initialize);
    ir.CreateCondBr(
        ir.CreateICmpUGE(initialize->getArg(0), ir.getInt32(++prefix)), run,
        next);
    ir.SetInsertPoint(run);
    initializeShells(initializeShells, globals.at(step.symbol),
                     step.nativeRetirement->type);
    lifetimes.publishProgramStorage(ir, step.storageInitialization);
    ir.CreateBr(next);
    ir.SetInsertPoint(next);
  }
  ir.CreateRetVoid();
  auto *retire =
      llvm::Function::Create(llvm::FunctionType::get(ir.getVoidTy(), false),
                             llvm::GlobalValue::ExternalLinkage,
                             "gti_test_program_retire", session.module());
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", retire));
  lifetimes.retireProgramStorage(ir);
  ir.CreateRetVoid();
  auto *unload = session.module().getFunction("__gti_program_unload_native");
  require(
      unload && session.module().getGlobalVariable("llvm.global_dtors"),
      "module endpoint guards pinned storage instead of silently unloading it");
  auto *unloadTest = llvm::Function::Create(
      unload->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
      "gti_test_program_unload", session.module());
  ir.SetInsertPoint(
      llvm::BasicBlock::Create(session.context(), "entry", unloadTest));
  ir.CreateCall(unload);
  ir.CreateRetVoid();
  if (!sanitizer.empty())
    for (auto &function : session.module())
      if (!function.isDeclaration())
        function.addFnAttr(sanitizer == "thread"
                               ? llvm::Attribute::SanitizeThread
                               : llvm::Attribute::SanitizeAddress);
  if (!output.empty()) {
    std::error_code error;
    llvm::raw_fd_ostream stream(output.string(), error);
    require(!error, "process kernel IR output opens");
    session.module().print(stream, nullptr);
  }
  static_cast<void>(session.emitObject());
}

void checkSourceComposition(const std::filesystem::path &root,
                            const std::filesystem::path &output,
                            std::string_view sanitizer, bool activePayload,
                            bool guard = false, bool array = false) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / (array   ? "tests/fixtures/llvm_mutex_array_lifecycle.gti"
              : guard ? "tests/fixtures/llvm_mutex_guard_lifecycle.gti"
              : activePayload
                  ? "tests/fixtures/llvm_mutex_source_lifecycle.gti"
                  : "tests/fixtures/llvm_native_process_storage.gti"),
      std::nullopt, {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  require(frontend.canGenerateCode(),
          "source composition reaches verified MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(), "source composition has a sealed snapshot");
  requirePublicAdmission(*input);
  Session session(*input, {});
  // The public backend was exercised above. This second emission instruments
  // the same admitted source MIR for the native observer/sanitizer oracles.
  const auto admission = planLlvmAdmission(*input);
  std::set<std::string> pending;
  for (const auto &use : admission.requirements())
    if (use.required && use.entry &&
        use.entry->state == LlvmCoverageState::Pending &&
        (!use.implementedShape ||
         use.implementedShape->state != LlvmCoverageState::Implemented))
      pending.insert(use.entry->key());
  for (const auto &key : pending)
    std::cout << "source-composition pending public gate: " << key << '\n';
  lowerScalarProgram(session, admission);
  if (guard) {
    bool checked = false;
    for (const auto &constructor :
         input->program().mir().constructorInstances()) {
      const bool nativeReference = std::any_of(
          constructor.initializers.begin(), constructor.initializers.end(),
          [&](const auto &initializer) {
            return initializer.storesReference &&
                   initializer.targetType.kind == SemanticType::Reference &&
                   initializer.targetType.arguments.size() == 1 &&
                   nativeMutexElement(input->program().mir(),
                                      initializer.targetType.arguments.front(),
                                      CompilerCapabilityTypeKind::MutexStorage)
                       .has_value();
          });
      if (!nativeReference)
        continue;
      const auto *function = session.module().getFunction(
          "__gti_native_v1_body_" +
          std::to_string(static_cast<unsigned>(MirBodyKind::Constructor)) +
          "_" + std::to_string(constructor.id));
      require(function,
              "stored native reference has its exact source constructor");
      for (const auto &block : *function)
        for (const auto &instruction : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
            if (const auto *callee = call->getCalledFunction())
              require(!callee->getName().starts_with(
                          "__gti_native_mutex_transfer_"),
                      "publishing a stored reference must never transfer its "
                      "shell");
      checked = true;
    }
    require(checked,
            "real source exercises stored native reference publication");
  }
  if (!sanitizer.empty())
    for (auto &function : session.module())
      if (!function.isDeclaration())
        function.addFnAttr(sanitizer == "thread"
                               ? llvm::Attribute::SanitizeThread
                               : llvm::Attribute::SanitizeAddress);
  std::error_code error;
  llvm::raw_fd_ostream stream(output.string(), error);
  require(!error, "source composition IR output opens");
  session.module().print(stream, nullptr);
  static_cast<void>(session.emitObject());
}

// Test the native lifecycle kernel, not BodyDrop or public admission. The C
// observer stands in for the selected source destructor, with its exact planned
// signature; no recipe or source cleanup endpoint is synthesized by this test.
void checkActive(const std::filesystem::path &root,
                 const std::filesystem::path &output,
                 std::string_view sanitizer) {
  FrontendOptions options;
  options.target.executionProfile = ExecutionProfile::Concurrent;
  options.target.runtimeCapabilities.threads = true;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_mutex_shell_lifecycle.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  require(frontend.canGenerateCode(),
          "active shell source reaches verified MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                             options.target),
      issues);
  require(input.has_value(), "active shell recipes have a sealed snapshot");
  requirePublicAdmission(*input);
  Session session(*input, {});
  const auto &program = input->program().mir();
  SemanticType storage, cell;
  for (const auto &owner : program.classInstances()) {
    if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage)
      storage = owner.type;
    if (owner.declaredFields.size() == 2 &&
        owner.declaredFields[0].type == owner.declaredFields[1].type)
      cell = owner.type;
  }
  require(storage.kind == SemanticType::Class &&
              cell.kind == SemanticType::Class,
          "source materializes a mutex and unrelated two-field wrapper");
  const auto recipe = mirUniqueCleanupType(program, storage);
  const auto cellRecipe = mirUniqueCleanupType(program, cell);
  require(recipe && recipe->aggregate.size() == 1 && cellRecipe &&
              cellRecipe->aggregate.size() == 2 &&
              recipe->aggregate[0].destructor.has_value(),
          "active shell cleanup has exactly its source-selected payload child");
  const auto &payload = recipe->aggregate[0];
  const auto *destructor = program.findDestructorInstance(*payload.destructor);
  require(destructor && destructor->mayRaiseDefinedFailure,
          "payload observer exercises the existing fallible destructor ABI");
  bool activeConstructor = false;
  for (const auto &constructor : program.constructorInstances()) {
    const auto *owner = program.findClassInstance(constructor.owner);
    if (!owner || owner->type != storage)
      continue;
    activeConstructor = true;
    const auto initialization =
        mirOwnedStoragePayloadInitialization(program, constructor);
    require(
        initialization && initialization->payloadType == payload.type &&
            initialization->parameterPlace ==
                constructor.body.places.front().id &&
            initialization->parameterDrop ==
                constructor.body.dropObligations.front().id,
        "MIR proves the exact owned parameter and moved-from cleanup endpoint");
    require(
        nativeMutexConstructor(program, constructor) == payload.type,
        "active payload constructor has an exact cleanup-only parameter body");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto changed = constructor;
      if (mutation == 0)
        changed.body.dropObligations.front().dropType.destructor.reset();
      if (mutation == 1)
        changed.body.dropObligations.front().initiallyActive = false;
      if (mutation == 2)
        changed.body.blocks.front().instructions.push_back(
            {.id = changed.body.instructionCount() + 1,
             .kind = MirInstructionKind::Call});
      if (mutation == 3)
        changed.body.places.front().traits.containsBorrowedState = true;
      require(!nativeMutexConstructor(program, changed),
              "active payload proof rejects wrong recipe/state/code/borrowed "
              "authority");
      require(!mirOwnedStoragePayloadInitialization(program, changed),
              "the backend-neutral owned-payload query independently rejects "
              "mutations");
    }
  }
  require(activeConstructor,
          "active source materializes its native constructor");
  const auto layout = session.layouts().mutexStorage(storage);
  require(layout.object.size == 80 && layout.payloadIndex == 1 &&
              layout.object.fieldOffsets[1] == 64 &&
              layout.object.fieldOffsets[2] == 72 &&
              session.layouts().get(payload.type).size == 8 &&
              session.layouts().activeIndex(payload.type) == 1 &&
              session.layouts().get(cell).size == 160,
          "C observer checks the exact payload, shell and wrapper layout");
  Failures failures(session);
  const auto admission = planLlvmAdmission(*input);
  Dispatch dispatch(session, admission);
  Lifetimes lifetimes(session, failures, dispatch, [&](MirBodyAddress address) {
    require(address.kind == MirBodyKind::Destructor &&
                address.owner == destructor->id,
            "cleanup selects only the sealed payload destructor");
    if (auto *existing =
            session.module().getFunction("gti_test_mutex_payload_drop"))
      return existing;
    const auto abi = session.abi().gti(
        SemanticType::Void, {}, destructor->mayRaiseDefinedFailure, true);
    auto *observer =
        llvm::Function::Create(abi.type, llvm::GlobalValue::ExternalLinkage,
                               "gti_test_mutex_payload_drop", session.module());
    abi.applyTo(*observer);
    return observer;
  });
  const auto function = [&](const char *name, llvm::Type *result,
                            std::vector<llvm::Type *> parameters) {
    return llvm::Function::Create(
        llvm::FunctionType::get(result, parameters, false),
        llvm::GlobalValue::ExternalLinkage, name, session.module());
  };
  auto *initialize = function("gti_test_mutex_active_initialize",
                              llvm::Type::getVoidTy(session.context()),
                              {session.layouts().pointer(),
                               llvm::Type::getInt32Ty(session.context()),
                               llvm::Type::getInt32Ty(session.context())});
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", initialize));
  Mutexes(session, ir).initializeShell(initialize->getArg(0), storage);
  auto *value = ir.CreateStructGEP(layout.object.storage, initialize->getArg(0),
                                   layout.payloadIndex);
  ir.CreateStore(initialize->getArg(1),
                 ir.CreateStructGEP(session.layouts().get(payload.type).storage,
                                    value, 0));
  ir.CreateStore(
      ir.CreateZExt(ir.CreateICmpNE(initialize->getArg(2), ir.getInt32(0)),
                    ir.getInt8Ty()),
      ir.CreateStructGEP(session.layouts().get(payload.type).storage, value,
                         *session.layouts().activeIndex(payload.type)));
  ir.CreateStore(
      ir.CreateSelect(ir.CreateICmpNE(initialize->getArg(2), ir.getInt32(0)),
                      ir.getInt8(MutexStorageLayout::shellLive |
                                 MutexStorageLayout::payloadLive),
                      ir.getInt8(MutexStorageLayout::shellLive)),
      ir.CreateStructGEP(layout.object.storage, initialize->getArg(0),
                         layout.payloadActiveIndex));
  ir.CreateRetVoid();
  for (const auto &[type, drop, suffix] :
       {std::tuple{storage, *recipe, "active"},
        std::tuple{cell, *cellRecipe, "cell"}}) {
    auto *transfer = function(
        (std::string("gti_test_mutex_") + suffix + "_transfer").c_str(),
        ir.getVoidTy(),
        {session.layouts().pointer(), session.layouts().pointer()});
    ir.SetInsertPoint(
        llvm::BasicBlock::Create(session.context(), "entry", transfer));
    require(lifetimes.transferMutexes(ir, transfer->getArg(0),
                                      transfer->getArg(1), type),
            "structural transfer preserves fresh shell identity");
    ir.CreateRetVoid();
    auto *cleanup =
        function((std::string("gti_test_mutex_") + suffix + "_cleanup").c_str(),
                 ir.getInt32Ty(),
                 {session.layouts().pointer(), session.layouts().pointer()});
    ir.SetInsertPoint(
        llvm::BasicBlock::Create(session.context(), "entry", cleanup));
    ir.CreateRet(ir.CreateZExt(
        ir.CreateCall(lifetimes.cleanup(drop),
                      {cleanup->getArg(0), cleanup->getArg(1), ir.getFalse()}),
        ir.getInt32Ty()));
  }
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
    require(!stream.has_error(), "write source-derived active shell kernel IR");
  }
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
  requirePublicAdmission(*input);
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
  const auto admission = planLlvmAdmission(*input);
  Dispatch dispatch(session, admission);
  Lifetimes lifetimes(
      session, failures, dispatch, [](MirBodyAddress) -> llvm::Function * {
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
  if (argc == 3 && std::string_view(argv[2]) == "--hierarchy-contracts") {
    try {
      checkHierarchyContracts(argv[1]);
      return 0;
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc == 3 && std::string_view(argv[2]) == "--predicates") {
    try {
      checkPredicates(argv[1]);
      return 0;
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if ((argc == 4 || argc == 5) &&
      (std::string_view(argv[2]) == "--source-composition" ||
       std::string_view(argv[2]) == "--guard-source-composition" ||
       std::string_view(argv[2]) == "--array-source-composition" ||
       std::string_view(argv[2]) == "--active-source-composition")) {
    try {
      const std::string_view sanitizer = argc == 5 ? argv[4] : "";
      if (!sanitizer.empty() && sanitizer != "thread" &&
          sanitizer != "address,undefined")
        return 2;
      checkSourceComposition(
          argv[1], argv[3], sanitizer,
          std::string_view(argv[2]) == "--active-source-composition",
          std::string_view(argv[2]) == "--guard-source-composition",
          std::string_view(argv[2]) == "--array-source-composition");
      return 0;
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc != 2 && argc != 3 && argc != 4)
    return 2;
  const std::string_view sanitizer = argc == 4 ? argv[3] : "";
  if (!sanitizer.empty() && sanitizer != "thread" &&
      sanitizer != "address,undefined")
    return 2;
  try {
    checkHierarchyContracts(argv[1]);
    checkPredicates(argv[1]);
    check(argv[1], argc >= 3 ? argv[2] : "", sanitizer);
    checkActive(argv[1],
                argc >= 3 ? std::filesystem::path(argv[2]).parent_path() /
                                "mutex-active.ll"
                          : std::filesystem::path{},
                sanitizer);
    checkProcessStorage(argv[1],
                        argc >= 3
                            ? std::filesystem::path(argv[2]).parent_path() /
                                  "mutex-process.ll"
                            : std::filesystem::path{},
                        sanitizer);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
