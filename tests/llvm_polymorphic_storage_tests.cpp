#include "../src/compiler/llvm/polymorphic_storage.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/llvm_backend.h"

#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <llvm/IR/Verifier.h>

namespace lang::native_llvm {
struct DispatchStructuralTestAccess {
  static Dispatch inspect(Session &session) { return Dispatch(session); }
};
} // namespace lang::native_llvm

namespace {
using namespace lang;
using namespace lang::native_llvm;
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
struct Operation {
  const MirBody *body;
  const MirInstruction *instruction;
};
Operation operation(const MirProgram &program, IntrinsicKind kind,
                    std::uint64_t capacity = 64, std::uint64_t alignment = 16,
                    const SemanticType *model = nullptr) {
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto &body = *findMirBody(program, address);
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.tombstone || instruction.intrinsic != kind ||
            instruction.operands.empty())
          continue;
        if (model && (instruction.operands.size() < 2 ||
                      instruction.operands[1].type != *model))
          continue;
        const auto &arguments =
            instruction.operands.front().type.valueArguments;
        if (arguments.size() == 2 && arguments[0].value == capacity &&
            arguments[1].value == alignment)
          return {&body, &instruction};
      }
  }
  throw std::runtime_error("missing real sealed slot operation");
}

void test(const std::filesystem::path &root) {
  const auto library =
      std::filesystem::canonical(root / "stdlib/std/polymorphic.gti");
  std::ifstream file(library);
  std::string source(std::istreambuf_iterator<char>{file}, {});
  source += R"(
namespace slot_operation_probe {
interface First {};
interface Target {};
class Model : public First, public Target { public: mut int number = 41; };
class Other : public Target, public First { public: mut int number = 41; };
class Plain { public: Plain() {} };
struct Holder {
  mut gti_internal::polymorphic_storage<Target, 64, 16> first{};
  mut gti_internal::polymorphic_storage<Target, 64, 16> second{};
};
int run(int mode) {
  Plain plain{};
  Holder holder{};
  if (mode == 2) {
    mut gti_internal::polymorphic_storage<Target, 1, 16> tiny{};
    Model value{};
    gti_internal::polymorphic_storage_construct(tiny, std::move(value));
  }
  if (mode == 5) {
    mut gti_internal::polymorphic_storage<Target, 64, 1> unaligned{};
    Model value{};
    gti_internal::polymorphic_storage_construct(unaligned, std::move(value));
  }
  mut gti_internal::polymorphic_storage<Target, 64, 16> storage{};
  if (mode == 6) {
    Other other{};
    gti_internal::polymorphic_storage_construct(storage, std::move(other));
    gti_internal::polymorphic_storage_reset(storage);
  }
  Model value{};
  gti_internal::polymorphic_storage_construct(storage, std::move(value));
  { Target& view = gti_internal::polymorphic_storage_read(storage); }
  { mut Target& view = gti_internal::polymorphic_storage_read_mut(storage); }
  bool present = gti_internal::polymorphic_storage_has_value(storage);
  gti_internal::polymorphic_storage_reset(storage);
  return present ? 0 : 1;
}
}
)";
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      root / "tests/slot-operation-entry.gti",
      "#include <std/polymorphic>\nint main() { return "
      "slot_operation_probe::run(0); }\n",
      {root / "stdlib/prelude.gti"}, {{library.string(), source}},
      {root / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("slot operation source failed frontend");
  }
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("slot operation source failed MIR seal");
  // Public slot operations now have their own exact admission shape; the
  // component checks below still exercise the private mechanism directly.
  const auto admission = planLlvmAdmission(*input);
  if (!admission.admitted())
    std::cerr << "slot admission limitation: " << admission.limitation()
              << '\n';
  expect(admission.admitted(),
         "exact slot operations are admitted by NativeInlinePolymorphicSlot");
  bool slotWitness = false;
  for (const auto &use : admission.requirements())
    slotWitness |= use.required &&
                   use.entry->key() ==
                       "IntrinsicKind.PolymorphicStorageConstruct" &&
                   use.implementedShape &&
                   use.implementedShape->name == "NativeInlinePolymorphicSlot";
  expect(slotWitness,
         "slot construction carries the exact inline-slot operation witness");
  const auto &program = input->program().mir();
  const auto namedClass =
      [&](std::string_view name) -> const MirClassInstance & {
    for (const auto &instance : program.classInstances()) {
      const auto *declaration =
          input->program().findClassDeclaration(instance.declaration);
      if (declaration && declaration->qualifiedName == name)
        return instance;
    }
    throw std::runtime_error("missing real slot test class");
  };
  const auto model = namedClass("slot_operation_probe::Model").type;
  const auto otherModel = namedClass("slot_operation_probe::Other").type;
  const auto construct = operation(
      program, IntrinsicKind::PolymorphicStorageConstruct, 64, 16, &model);
  const auto constructOther = operation(
      program, IntrinsicKind::PolymorphicStorageConstruct, 64, 16, &otherModel);
  const auto read = operation(program, IntrinsicKind::PolymorphicStorageRead);
  const auto readMut =
      operation(program, IntrinsicKind::PolymorphicStorageReadMut);
  const auto has =
      operation(program, IntrinsicKind::PolymorphicStorageHasValue);
  const auto reset = operation(program, IntrinsicKind::PolymorphicStorageReset);
  const auto tiny =
      operation(program, IntrinsicKind::PolymorphicStorageConstruct, 1);
  const auto unaligned =
      operation(program, IntrinsicKind::PolymorphicStorageConstruct, 64, 1);
  const auto type = construct.instruction->operands[0].type;
  const MirConstructorInstance *slotConstructor = nullptr;
  const MirConstructorInstance *plainConstructor = nullptr;
  for (const auto &candidate : program.constructorInstances()) {
    const auto *owner = program.findClassInstance(candidate.owner);
    if (owner && owner->type == type && candidate.parameterTypes.empty()) {
      if (slotConstructor)
        throw std::runtime_error("ambiguous real slot default constructor");
      slotConstructor = &candidate;
    }
    if (owner &&
        owner->type == namedClass("slot_operation_probe::Plain").type &&
        candidate.parameterTypes.empty())
      plainConstructor = &candidate;
  }
  if (!slotConstructor || !plainConstructor)
    throw std::runtime_error("missing real slot default constructor");
  const auto *holder = &namedClass("slot_operation_probe::Holder");
  const auto temporary =
      std::filesystem::temp_directory_path() /
      ("gti-slot-operation-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(temporary);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{temporary};
  const auto consumer = temporary / "consumer.c";
  expect(driver::writeArtifact(consumer, R"(
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
extern int gti_slot_probe(int);
int gti_slot_observe(void *slot, void *view, uint64_t payload, uint64_t base, uint64_t field) {
  int32_t number;
  memcpy(&number, (char*)slot + payload + field, sizeof(number));
  int32_t through_view;
  memcpy(&through_view, (char*)view - base + field, sizeof(through_view));
  return (char*)view != (char*)slot + payload + base || number != 41 || through_view != 41;
}
int main(int argc, char **argv) {
  struct rlimit limit = {0, 0};
  setrlimit(RLIMIT_CORE, &limit);
  return gti_slot_probe(argc > 1 ? atoi(argv[1]) : 0);
}
)") == driver::ArtifactWriteStatus::Success,
         "write slot operation C observer");
  driver::NativeToolchain toolchain;
  const auto cobject = temporary / "consumer.o";
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), consumer, cobject, driver::CStandard::C17,
      OptimizationLevel::O2, {}, {}));
  expect(compiled.succeeded(), "compile slot operation C observer");
  if (!compiled.succeeded())
    return;
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive}) {
    Session session(*input, {.optimization = level});
    auto dispatch = DispatchStructuralTestAccess::inspect(session);
    Failures failure(session);
    Lifetimes lifetimes(
        session, failure, dispatch, [](MirBodyAddress) -> llvm::Function * {
          throw std::runtime_error("slot mechanism requested a source body");
        });
    PolymorphicStorage slots(session, lifetimes, dispatch);
    const auto functions = session.module().size();
    const auto globals = session.module().global_size();
    for (const auto selected : {construct, constructOther, read, readMut, has,
                                reset, tiny, unaligned})
      slots.preflight(*selected.body, *selected.instruction);
    slots.preflightDefaultConstructor(*slotConstructor);
    auto detachedConstructor = *slotConstructor;
    bool rejectedConstructor = false;
    try {
      slots.preflightDefaultConstructor(detachedConstructor);
    } catch (const std::invalid_argument &) {
      rejectedConstructor = true;
    }
    expect(rejectedConstructor,
           "a detached constructor cannot impersonate sealed authority");
    bool rejectedPlain = false;
    try {
      slots.preflightDefaultConstructor(*plainConstructor);
    } catch (const BackendUnsupportedError &) {
      rejectedPlain = true;
    }
    expect(
        rejectedPlain,
        "an ordinary source constructor cannot acquire private slot behavior");
    auto detached = *construct.instruction;
    bool rejected = false;
    try {
      slots.preflight(*construct.body, detached);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    expect(rejected,
           "a detached instruction cannot impersonate sealed authority");
    expect(session.module().size() == functions &&
               session.module().global_size() == globals,
           "slot preflight and rejection publish no helper or descriptor");
    const std::vector<SemanticType> parameters{SemanticType::Int32};
    const auto plan = session.abi().nativeC(SemanticType::Int32, parameters);
    auto *probe =
        llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                               "gti_slot_probe", session.module());
    plan.applyTo(*probe);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", probe));
    const auto layout = session.layouts().polymorphicStorage(type);
    auto *first = ir.CreateAlloca(layout.type.storage);
    auto *second = ir.CreateAlloca(layout.type.storage);
    auto *third = ir.CreateAlloca(layout.type.storage);
    const auto modelLayout = session.layouts().get(model);
    auto *payload = ir.CreateAlloca(modelLayout.storage);
    ir.CreateStore(llvm::Constant::getNullValue(modelLayout.storage), payload);
    const auto number =
        session.layouts().instance(model).declaredFields.front().symbol;
    const auto field = session.layouts().fieldSubobject(model, number).offset;
    ir.CreateStore(ir.getInt32(41),
                   ir.CreateGEP(ir.getInt8Ty(), payload, ir.getInt64(field)));
    // Fresh storage may contain arbitrary bytes; constructor entry must not
    // assume allocator zeroing or copy an existing slot representation.
    ir.CreateMemSet(first, ir.getInt8(0xa5), layout.type.size,
                    llvm::Align(layout.type.alignment));
    slots.initializeDefaultConstructor(ir, *slotConstructor, first);
    const auto beforeAlias = ir.GetInsertBlock()->size();
    bool rejectedAlias = false;
    try {
      slots.relocate(ir, type, first, first);
    } catch (const std::invalid_argument &) {
      rejectedAlias = true;
    }
    expect(
        rejectedAlias && ir.GetInsertBlock()->size() == beforeAlias,
        "identical storage cannot impersonate a fresh relocation destination");
    const auto emit = [&](Operation selected,
                          std::initializer_list<llvm::Value *> values) {
      return slots.emit(
          ir, *selected.body, *selected.instruction,
          std::span<llvm::Value *const>(values.begin(), values.size()));
    };
    const auto branch = [&](int mode, auto action) {
      auto *selected =
          llvm::BasicBlock::Create(session.context(), "selected", probe);
      auto *next = llvm::BasicBlock::Create(session.context(), "next", probe);
      ir.CreateCondBr(ir.CreateICmpEQ(probe->getArg(0), ir.getInt32(mode)),
                      selected, next);
      ir.SetInsertPoint(selected);
      action();
      ir.CreateBr(next);
      ir.SetInsertPoint(next);
    };
    branch(1, [&] { static_cast<void>(emit(read, {first})); });
    for (const auto selected : {tiny, unaligned}) {
      const auto selectedType = selected.instruction->operands.front().type;
      branch(selectedType.valueArguments.front().value == 1 ? 2 : 5, [&] {
        auto *address =
            ir.CreateAlloca(session.layouts().get(selectedType).storage);
        slots.initialize(ir, selectedType, address);
        static_cast<void>(emit(selected, {address, payload}));
      });
    }
    auto *useOther = ir.CreateICmpEQ(probe->getArg(0), ir.getInt32(6));
    auto *otherBlock =
        llvm::BasicBlock::Create(session.context(), "other", probe);
    auto *modelBlock =
        llvm::BasicBlock::Create(session.context(), "model", probe);
    auto *joined =
        llvm::BasicBlock::Create(session.context(), "constructed", probe);
    ir.CreateCondBr(useOther, otherBlock, modelBlock);
    ir.SetInsertPoint(otherBlock);
    const auto otherLayout = session.layouts().get(otherModel);
    auto *otherPayload = ir.CreateAlloca(otherLayout.storage);
    ir.CreateStore(llvm::Constant::getNullValue(otherLayout.storage),
                   otherPayload);
    const auto otherNumber =
        session.layouts().instance(otherModel).declaredFields.front().symbol;
    const auto otherField =
        session.layouts().fieldSubobject(otherModel, otherNumber).offset;
    ir.CreateStore(ir.getInt32(41), ir.CreateGEP(ir.getInt8Ty(), otherPayload,
                                                 ir.getInt64(otherField)));
    static_cast<void>(emit(constructOther, {first, otherPayload}));
    ir.CreateBr(joined);
    ir.SetInsertPoint(modelBlock);
    static_cast<void>(emit(construct, {first, payload}));
    ir.CreateBr(joined);
    ir.SetInsertPoint(joined);
    branch(3, [&] { static_cast<void>(emit(construct, {first, payload})); });
    llvm::Value *bad = ir.CreateNot(emit(has, {first}));
    slots.relocate(ir, type, second, first);
    bad = ir.CreateOr(bad, emit(has, {first}));
    const auto holderStorage = session.layouts().get(holder->type).storage;
    auto *holderFrom = ir.CreateAlloca(holderStorage);
    auto *holderTo = ir.CreateAlloca(holderStorage);
    ir.CreateStore(llvm::Constant::getNullValue(holderStorage), holderFrom);
    const auto entry = [&](llvm::Value *address, unsigned index) {
      return ir.CreateStructGEP(
          holderStorage, address,
          session.layouts().fieldIndex(
              holder->type, holder->declaredFields.at(index).symbol));
    };
    slots.relocate(ir, type, entry(holderFrom, 0), second);
    ir.CreateCall(lifetimes.inertRelocation(holder->type),
                  {holderTo, holderFrom});
    bad = ir.CreateOr(bad, emit(has, {entry(holderFrom, 0)}));
    bad = ir.CreateOr(bad, emit(has, {entry(holderTo, 1)}));
    const auto base =
        session.layouts().baseSubobject(model, type.arguments[0]).offset;
    const auto otherBase =
        session.layouts().baseSubobject(otherModel, type.arguments[0]).offset;
    expect(base != otherBase,
           "runtime models require distinct interface offsets");
    auto *expectedBase =
        ir.CreateSelect(useOther, ir.getInt64(otherBase), ir.getInt64(base));
    auto *expectedField =
        ir.CreateSelect(useOther, ir.getInt64(otherField), ir.getInt64(field));
    auto *nestedView = emit(read, {entry(holderTo, 0)});
    auto *nestedPayload = ir.CreateStructGEP(
        layout.type.storage, entry(holderTo, 0), layout.payloadIndex);
    bad = ir.CreateOr(
        bad,
        ir.CreateICmpNE(nestedView, ir.CreateGEP(ir.getInt8Ty(), nestedPayload,
                                                 expectedBase)));
    slots.relocate(ir, type, third, entry(holderTo, 0));
    bad = ir.CreateOr(bad, emit(has, {second}));
    for (auto *moved : {first, second})
      ir.CreateMemSet(
          ir.CreateStructGEP(layout.type.storage, moved, layout.payloadIndex),
          ir.getInt8(0xa5), layout.capacity,
          llvm::Align(layout.payloadAlignment));
    auto *view = emit(read, {third});
    auto *mutableView = emit(readMut, {third});
    bad = ir.CreateOr(bad, ir.CreateICmpNE(view, mutableView));
    expect(base != 0,
           "slot cache test requires a nonzero interface adjustment");
    const auto pointer = SemanticType::rawPointerTo(SemanticType::UInt8);
    const std::vector<SemanticType> observerParameters{
        pointer, pointer, SemanticType::UInt64, SemanticType::UInt64,
        SemanticType::UInt64};
    const auto observerPlan =
        session.abi().nativeC(SemanticType::Int32, observerParameters);
    auto *observer = llvm::Function::Create(
        observerPlan.type, llvm::GlobalValue::ExternalLinkage,
        "gti_slot_observe", session.module());
    observerPlan.applyTo(*observer);
    auto *observed = ir.CreateCall(
        observer, {third, view,
                   ir.getInt64(layout.type.fieldOffsets[layout.payloadIndex]),
                   expectedBase, expectedField});
    observerPlan.applyTo(*observed);
    bad = ir.CreateOr(bad, ir.CreateICmpNE(observed, ir.getInt32(0)));
    static_cast<void>(emit(reset, {third}));
    static_cast<void>(emit(reset, {third}));
    bad = ir.CreateOr(bad, emit(has, {third}));
    branch(4, [&] { static_cast<void>(emit(readMut, {third})); });
    slots.relocate(ir, type, first, third);
    bad = ir.CreateOr(bad, emit(has, {first}));
    ir.CreateRet(ir.CreateZExt(bad, ir.getInt32Ty()));
    expect(!llvm::verifyModule(session.module()),
           "slot operation module verifies");
    session.requireDefinition(*probe);
    const auto object = temporary / "probe.o";
    expect(driver::writeArtifact(object, session.emitObject().contents) ==
               driver::ArtifactWriteStatus::Success,
           "write native slot operation object");
    driver::NativeInputs native;
    native.libraryFiles.push_back(cobject);
    const auto executable = temporary / "probe";
    const auto linked = toolchain.invoke(driver::NativeLinkRequest(
        driver::discoverNativeCompiler(), object, executable, native));
    expect(linked.succeeded(), "link native slot operation object");
    if (!linked.succeeded())
      continue;
    expect(driver::invokeProcess({executable.string()}).succeeded(),
           "construct, borrow, repeated move, reset and empty move execute at "
           "O0/O3");
    expect(
        driver::invokeProcess({executable.string(), "6"}).succeeded(),
        "runtime-selected model uses its own relocation and interface offset");
    for (int mode = 1; mode <= 5; ++mode) {
      const auto result =
          driver::invokeProcess({executable.string(), std::to_string(mode)},
                                {.timeout = std::chrono::seconds(10)});
      expect(!result.driverDiagnostic && !result.timedOut && result.signal &&
                 (*result.signal == SIGILL || *result.signal == SIGTRAP),
             "private invalid state/footprint traps without a GTI failure "
             "channel");
    }
  }
}
} // namespace

int main() {
  try {
    test(std::filesystem::path(__FILE__).parent_path().parent_path());
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return failures ? 1 : 0;
}
