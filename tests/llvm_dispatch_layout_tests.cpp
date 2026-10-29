#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/dispatch.h"
#include "../src/compiler/llvm/lifetimes.h"
#include "../src/compiler/llvm/payloads.h"
#include "../src/compiler/llvm/placement.h"
#include "../src/compiler/llvm/places.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/llvm_backend.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

namespace lang::native_llvm {
// This friend is intentionally defined only in the owning structural test.
// It cannot publish a production backend artifact or replace an admission plan.
struct DispatchStructuralTestAccess {
  static Dispatch inspect(Session &session) { return Dispatch(session); }
};
static_assert(
    !std::is_constructible_v<Dispatch, Session &, LlvmAdmissionPlan &&>,
    "dispatch cannot borrow a temporary admission plan");
} // namespace lang::native_llvm

namespace {
using namespace lang;
using namespace lang::native_llvm;

// These two mechanism probes deliberately inject a trusted test prelude;
// ordinary application source remains unable to name the legacy capability.
FrontendResult analyzeLegacyOwner(const std::filesystem::path &root,
                                  const std::string &name,
                                  const std::string &source) {
  const auto library = root / "stdlib";
  const auto entry = library / name;
  return Frontend().analyze(
      entry, source, {library / "prelude.gti", entry},
      {{std::filesystem::weakly_canonical(entry).string(), source}}, {library});
}

int failures = 0;
void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
template <typename Exception, typename Action>
void rejects(Action action, std::string_view message) {
  bool rejected = false;
  try {
    action();
  } catch (const Exception &) {
    rejected = true;
  }
  expect(rejected, message);
}

std::array<std::size_t, 4> modulePublication(Session &session) {
  std::array<std::size_t, 4> result{session.module().size(),
                                    session.module().global_size(), 0, 0};
  for (const auto &function : session.module()) {
    result[2] += function.size();
    for (const auto &block : function)
      result[3] += block.size();
  }
  return result;
}

void testAbiEquality() {
  llvm::LLVMContext context;
  FunctionAbi plan;
  plan.type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  plan.nativeMemory.push_back(
      {0, llvm::Type::getInt32Ty(context), 4, false, true});
  expect(plan == FunctionAbi(plan), "identical native ABI plans compare equal");
  const auto changed = [&](auto mutate) {
    auto other = plan;
    mutate(other.nativeMemory.front());
    expect(plan != other, "every native memory ABI attribute affects equality");
  };
  changed([](auto &parameter) { parameter.index = 1; });
  changed([&](auto &parameter) {
    parameter.storage = llvm::Type::getInt64Ty(context);
  });
  changed([](auto &parameter) { parameter.alignment = 8; });
  changed([](auto &parameter) { parameter.result = true; });
  changed([](auto &parameter) { parameter.byValue = false; });
  auto withoutMemory = plan;
  withoutMemory.nativeMemory.clear();
  expect(plan != withoutMemory,
         "native memory metadata cannot compare equal to an absent contract");
}

const MirClassInstance &named(const LoweredProgram &program,
                              std::string_view name) {
  for (const auto &owner : program.mir().classInstances()) {
    const auto *declaration = program.findClassDeclaration(owner.declaration);
    if (declaration && declaration->qualifiedName == name)
      return owner;
  }
  throw std::runtime_error("missing native dispatch layout fixture class");
}

std::uint64_t offsetFrom(Session &session, const llvm::Value *address,
                         const llvm::Value *base) {
  std::uint64_t result = 0;
  while (const auto *projection = llvm::dyn_cast<llvm::GEPOperator>(address)) {
    llvm::APInt offset(session.dataLayout().getPointerSizeInBits(), 0);
    if (!projection->accumulateConstantOffset(session.dataLayout(), offset))
      throw std::runtime_error(
          "test address is not a constant physical projection");
    result += offset.getZExtValue();
    address = projection->getPointerOperand();
  }
  expect(address == base,
         "physical projection retains its original storage root");
  return result;
}

void testPlaces(Session &session) {
  const auto &program = session.program();
  const auto &leaf = named(program, "Leaf");
  const auto &root = named(program, "Root");
  const auto &reader = named(program, "Reader");
  const auto &writer = named(program, "Writer");
  auto &layouts = session.layouts();
  const auto main = std::find_if(
      program.mir().functionInstances().begin(),
      program.mir().functionInstances().end(), [](const auto &function) {
        return function.entryKind != ProgramEntryKind::None;
      });
  if (main == program.mir().functionInstances().end())
    throw std::runtime_error("test lost its source entry body");
  auto body = main->body;
  const auto local = std::find_if(
      body.places.begin(), body.places.end(), [&](const auto &place) {
        return place.root == MirPlaceRootKind::Binding &&
               place.projections.empty() && place.type == leaf.type;
      });
  if (local == body.places.end())
    throw std::runtime_error("test lost its complete leaf storage");
  const auto localId = local->id;
  // These representation-only projections use exact sealed field identities.
  // They are not new source/MIR admission or machine-code execution evidence.
  auto inherited = *local;
  inherited.id = body.places.size() + 1;
  inherited.type = SemanticType::Int32;
  inherited.projections.push_back(
      {.kind = MirProjectionKind::Field,
       .field = root.declaredFields.front().symbol});
  body.places.push_back(inherited);
  auto declared = inherited;
  declared.id = body.places.size() + 1;
  declared.projections.front().field = leaf.declaredFields.back().symbol;
  body.places.push_back(declared);

  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::InternalLinkage, "dispatch.places.probe",
      session.module());
  auto *entry = llvm::BasicBlock::Create(session.context(), "entry", function);
  llvm::IRBuilder<> ir(entry);
  std::vector<llvm::Value *> values(body.values.size() + 1);
  Places memory(session, ir, body, values, [](auto *, auto) {
    throw std::runtime_error("unexpected checked operation in address probe");
  });
  memory.allocate({});
  auto *complete = memory.address(localId);
  for (const auto &target : {root.type, reader.type, writer.type}) {
    const auto path = layouts.baseSubobject(leaf.type, target);
    const auto operand = MirOperand{.kind = MirOperandKind::BorrowRead,
                                    .place = localId,
                                    .type = SemanticType::referenceTo(target)};
    expect(
        offsetFrom(session, memory.selected(operand), complete) == path.offset,
        "typed class reference selection uses the exact physical base offset");
  }
  expect(
      offsetFrom(session, memory.address(inherited.id), complete) ==
              layouts
                  .fieldSubobject(leaf.type, root.declaredFields.front().symbol)
                  .offset &&
          offsetFrom(session, memory.address(declared.id), complete) ==
              layouts
                  .fieldSubobject(leaf.type, leaf.declaredFields.back().symbol)
                  .offset,
      "inherited and direct fields follow their distinct sealed SymbolIds");
  rejects<BackendUnsupportedError>(
      [&] {
        static_cast<void>(memory.selected(
            {.kind = MirOperandKind::BorrowRead,
             .place = localId,
             .type = SemanticType::referenceTo(SemanticType::Int32)}));
      },
      "an unrelated referent cannot replace a class reference");
  ir.CreateRetVoid();
  expect(!llvm::verifyFunction(*function),
         "base/field projections form valid LLVM IR");
  bool readonly = false, writable = false;
  for (const auto &source : program.mir().functionInstances()) {
    if (source.parameterTypes.empty() || source.parameterBindings.empty() ||
        source.parameterTypes.front().kind != SemanticType::Reference ||
        source.parameterTypes.front().arguments.front().kind !=
            SemanticType::Class)
      continue;
    const auto &formal = source.parameterTypes.front();
    const auto place = std::find_if(
        source.body.places.begin(), source.body.places.end(),
        [&](const auto &candidate) {
          return candidate.root == MirPlaceRootKind::Binding &&
                 candidate.binding == source.parameterBindings.front() &&
                 candidate.projections.empty();
        });
    if (place == source.body.places.end())
      throw std::runtime_error("test reference formal has no physical root");
    auto *probe = llvm::Function::Create(
        llvm::FunctionType::get(ir.getVoidTy(), {layouts.pointer()}, false),
        llvm::GlobalValue::InternalLinkage, "dispatch.reference.probe",
        session.module());
    llvm::IRBuilder<> referenceIr(
        llvm::BasicBlock::Create(session.context(), "entry", probe));
    std::vector<llvm::Value *> referenceValues(source.body.values.size() + 1);
    Places references(
        session, referenceIr, source.body, referenceValues, [](auto *, auto) {
          throw std::runtime_error("unexpected reference address guard");
        });
    references.allocate({});
    references.parameter(
        {.binding = source.parameterBindings.front(), .type = formal},
        probe->getArg(0), false);
    const MirOperand operand{
        .kind = MirOperandKind::BorrowRead,
        .place = place->id,
        .type = SemanticType::referenceTo(formal.arguments.front())};
    const auto countLoads = [&]() {
      return std::count_if(probe->getEntryBlock().begin(),
                           probe->getEntryBlock().end(),
                           [](const auto &instruction) {
                             return llvm::isa<llvm::LoadInst>(instruction);
                           });
    };
    const auto before = countLoads();
    auto *selected = references.selected(operand);
    expect(llvm::isa<llvm::LoadInst>(selected) && countLoads() == before + 1,
           "an already-selected reference view loads its referent exactly once "
           "without another adjustment");
    auto mutableOperand = operand;
    mutableOperand.kind = MirOperandKind::BorrowWrite;
    mutableOperand.type = SemanticType::referenceTo(formal.arguments.front(),
                                                    AccessMode::Mutable);
    if (formal.referenceAccess == AccessMode::ReadOnly) {
      readonly = true;
      rejects<BackendUnsupportedError>(
          [&] { static_cast<void>(references.selected(mutableOperand)); },
          "reference selection cannot widen readonly access");
    } else {
      writable = true;
      expect(llvm::isa<llvm::LoadInst>(references.selected(mutableOperand)),
             "mutable reference selection retains existing writable access");
    }
    referenceIr.CreateRetVoid();
    expect(!llvm::verifyFunction(*probe),
           "reference-view selection forms valid LLVM IR");
  }
  expect(readonly && writable,
         "reference probes cover both supported access modes");
}

void testThunks(Session &session) {
  auto dispatch = DispatchStructuralTestAccess::inspect(session);
  bool failing = false, direct = false, indirect = false, boolean = false,
       noResult = false;
  for (const auto &active : session.program().mir().classInstances()) {
    if (!active.polymorphic || active.abstract ||
        active.kind == ClassKind::Interface)
      continue;
    const auto coldFunctions = session.module().size();
    const auto coldGlobals = session.module().global_size();
    dispatch.preflightState(active.id);
    dispatch.preflightState(active.id);
    expect(session.module().size() == coldFunctions &&
               session.module().global_size() == coldGlobals,
           "repeated cold state preflight plans every result contract without "
           "publishing functions or tables");
    std::map<std::pair<std::vector<unsigned>, unsigned>, llvm::GlobalVariable *>
        physical;
    for (const auto &view : dispatch.views(active.id)) {
      auto *table = dispatch.table(active.id, view.viewClass);
      const auto key =
          std::pair{view.location.owner.indices, view.location.index};
      const auto [shared, inserted] = physical.emplace(key, table);
      expect(inserted || shared->second == table,
             "one physical pointer location has one exact constant table");
      expect(dispatch.table(active.id, view.viewClass) == table,
             "table requests are stable within one immutable session");
      const auto *descriptor =
          llvm::dyn_cast<llvm::ConstantStruct>(table->getInitializer());
      if (!descriptor || descriptor->getNumOperands() != 3)
        throw std::runtime_error(
            "dispatch table lost its immutable active-object descriptor");
      const auto *offset =
          llvm::dyn_cast<llvm::ConstantInt>(descriptor->getOperand(0));
      expect(offset &&
                 offset->getSExtValue() ==
                     -static_cast<std::int64_t>(view.location.owner.offset),
             "active-object displacement is relative to the physical pointer "
             "owner, not its logical view or dispatch field");
      // Only a class allocated as a unique-owner pointee by this program
      // publishes its complete cleanup; no fixture here allocates one.
      expect(
          !dispatch.dynamicallyOwned(active.id) &&
              llvm::isa<llvm::ConstantPointerNull>(descriptor->getOperand(1)),
          "a never-allocated active class publishes no complete cleanup");
      const auto *initializer =
          llvm::dyn_cast<llvm::ConstantArray>(descriptor->getOperand(2));
      if (!initializer)
        throw std::runtime_error("dispatch descriptor lost its slot array");
      expect(initializer->getNumOperands() == view.slots.size(),
             "table slots retain their complete canonical profile");
      for (std::size_t index = 0; index < view.slots.size(); ++index) {
        const auto &slot = view.slots[index];
        auto *thunk =
            llvm::dyn_cast<llvm::Function>(initializer->getOperand(index));
        const auto root = dispatch.rootAbi(slot.root);
        const auto plans = session.abi().virtualAdjustment(root, slot.target);
        const auto *target =
            session.program().mir().findFunctionInstance(slot.target);
        expect(thunk && thunk->getFunctionType() == root.call.type &&
                   !llvm::verifyFunction(*thunk),
               "every adjustment thunk has the central root signature and "
               "valid typed IR");
        if (!thunk)
          continue;
        expect(
            dispatch.adjustment(active.id, view.profileOwner, slot.root) ==
                thunk,
            "thunk cache retains source and physical receiver presentations");
        llvm::CallInst *call = nullptr;
        std::vector<const llvm::StoreInst *> stores;
        for (auto &block : *thunk)
          for (auto &instruction : block) {
            if (auto *candidate =
                    llvm::dyn_cast<llvm::CallInst>(&instruction)) {
              expect(!call,
                     "one adjustment performs exactly one implementation call");
              call = candidate;
            }
            if (const auto *store =
                    llvm::dyn_cast<llvm::StoreInst>(&instruction))
              stores.push_back(store);
            expect(!llvm::isa<llvm::BitCastInst>(instruction),
                   "typed adapters never cast a function pointer");
          }
        expect(call &&
                   call->getCalledFunction() ==
                       session.abi().declareFunction(slot.target) &&
                   call->getFunctionType() == plans.implementation.type,
               "the exact real target is declared through shared ABI "
               "definition demand");
        if (!call)
          continue;
        for (unsigned argument = 0;
             argument < plans.implementation.parameters.size(); ++argument) {
          const auto &parameter = plans.implementation.parameters[argument];
          const auto entry = std::find(plans.entry.parameters.begin(),
                                       plans.entry.parameters.end(), parameter);
          expect(entry != plans.entry.parameters.end(),
                 "implementation channel has an exact root source");
          if (entry == plans.entry.parameters.end())
            continue;
          const auto ordinal =
              static_cast<unsigned>(entry - plans.entry.parameters.begin());
          const auto *actual = call->getArgOperand(argument);
          if (parameter.channel == AbiChannel::Receiver &&
              slot.receiverAdjustment) {
            const auto *projection = llvm::dyn_cast<llvm::GEPOperator>(actual);
            const auto *delta = projection ? llvm::dyn_cast<llvm::ConstantInt>(
                                                 projection->getOperand(1))
                                           : nullptr;
            expect(
                projection &&
                    projection->getPointerOperand() == thunk->getArg(ordinal) &&
                    delta && delta->getSExtValue() == slot.receiverAdjustment,
                "one signed receiver adjustment starts at the physical pointer "
                "owner");
          } else {
            expect(actual == thunk->getArg(ordinal),
                   "argument/result/failure channels preserve exact "
                   "sourceIndex and attributes");
          }
        }
        const auto *returned =
            llvm::dyn_cast<llvm::ReturnInst>(thunk->back().getTerminator());
        if (plans.implementation.statusResult) {
          failing = true;
          expect(stores.empty() && returned &&
                     returned->getReturnValue() == call,
                 "failure-capable thunks forward status without copying "
                 "failure or publishing a result");
        } else if (plans.implementation.resultDestination) {
          indirect = true;
          expect(stores.empty(), "infallible address-only bodies publish "
                                 "directly to the shared destination");
        } else if (!call->getType()->isVoidTy()) {
          direct = true;
          expect(stores.size() == 1 &&
                     stores.front()->getPointerOperand() ==
                         thunk->getArg(*plans.entry.resultDestination) &&
                     stores.front()->getAlign().value() ==
                         session.layouts().get(target->returnType).alignment,
                 "an infallible direct result publishes once with exact target "
                 "alignment");
          if (target->returnType == SemanticType::Bool) {
            boolean = true;
            expect(stores.front()->getValueOperand()->getType()->isIntegerTy(8),
                   "boolean results retain i1 values and i8 storage");
          }
        } else {
          noResult = true;
          expect(stores.empty(), "void success fabricates no result storage");
        }
      }
    }
    auto *installer = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                {session.layouts().pointer()}, false),
        llvm::GlobalValue::InternalLinkage, "dispatch.install.probe",
        session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", installer));
    dispatch.install(ir, installer->getArg(0), active.id);
    std::size_t stores = 0;
    for (const auto &instruction : installer->getEntryBlock())
      if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        ++stores;
        const auto offset = offsetFrom(session, store->getPointerOperand(),
                                       installer->getArg(0));
        expect(std::any_of(dispatch.views(active.id).begin(),
                           dispatch.views(active.id).end(),
                           [&](const auto &view) {
                             return offset ==
                                    view.location.owner.offset +
                                        session.layouts()
                                            .get(view.location.owner.type)
                                            .fieldOffsets.at(
                                                view.location.index);
                           }),
               "installation stores only to exact physical dispatch fields");
      }
    expect(stores == physical.size(),
           "installation deduplicates physical locations, not class names");
    ir.CreateRetVoid();
    expect(!llvm::verifyFunction(*installer),
           "isolated state installation forms valid LLVM IR");
    const auto warmFunctions = session.module().size();
    const auto warmGlobals = session.module().global_size();
    dispatch.preflightState(active.id);
    dispatch.preflightState(active.id);
    expect(session.module().size() == warmFunctions &&
               session.module().global_size() == warmGlobals,
           "repeated warm state preflight reuses representation caches without "
           "publishing further functions or tables");
  }
  std::cout << "thunk-contracts: failure=" << failing << " direct=" << direct
            << " indirect=" << indirect << " bool=" << boolean
            << " void=" << noResult << '\n';
  expect(failing && direct && indirect && boolean && noResult,
         "isolated thunks cover failing, scalar, address-only, boolean and "
         "void contracts");
  session.verify();
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(session.emitObject()); },
      "isolated helper tests cannot publish an artifact with demanded source "
      "bodies missing");
}

void testPureGates() {
  const auto frontend =
      Frontend().analyze("llvm-dispatch-pure-gate.gti", std::string(R"(
class Open { public: Open() {} virtual int read() = 0; };
class Closed : public Open {
public:
  Closed() : Open() {}
  int read() override { return 1; }
};
int main() { Closed value{}; return value.read() - 1; }
)"));
  expect(frontend.canGenerateCode(),
         "unused pure contracts do not reject an abstract base constructor");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("pure gate fixture failed its source seal");
  Session session(*input, {});
  auto dispatch = DispatchStructuralTestAccess::inspect(session);
  const auto &open = named(session.program(), "Open");
  const auto &closed = named(session.program(), "Closed");
  expect(dispatch.views(open.id).front().slots.front().pure,
         "the abstract active-state plan preserves its pure fact");
  const auto functions = session.module().size();
  const auto globals = session.module().global_size();
  for (int attempt = 0; attempt != 2; ++attempt) {
    rejects<BackendUnsupportedError>(
        [&] { dispatch.preflightState(open.id); },
        "pure active-state preflight rejects without inventing an outcome");
    dispatch.preflightState(closed.id);
  }
  expect(session.module().size() == functions &&
             session.module().global_size() == globals,
         "repeated pure rejection and nonpure preflight publish no functions "
         "or tables");
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(dispatch.table(open.id, open.id)); },
      "pure-bearing table publication stays explicitly gated without a failure "
      "invention");
  rejects<BackendUnsupportedError>(
      [&] {
        static_cast<void>(dispatch.adjustment(
            open.id, open.id, dispatch.slotProfile(open.id).front()));
      },
      "a pure target cannot acquire a null, trap or UB call adapter");
  expect(session.module().size() == functions &&
             session.module().global_size() == globals,
         "unsupported pure tables publish no partial functions or data");
  expect(dispatch.table(closed.id, closed.id) != nullptr,
         "the same pure root has a valid nonpure concrete dispatch table");
  const auto warmFunctions = session.module().size();
  const auto warmGlobals = session.module().global_size();
  for (int attempt = 0; attempt != 2; ++attempt) {
    dispatch.preflightState(closed.id);
    rejects<BackendUnsupportedError>(
        [&] { dispatch.preflightState(open.id); },
        "warm representation caches do not admit a pure active state");
  }
  expect(session.module().size() == warmFunctions &&
             session.module().global_size() == warmGlobals,
         "repeated warm positive and negative preflight publish no artifacts");
}

void testCallPlans(Session &session) {
  auto dispatch = DispatchStructuralTestAccess::inspect(session);
  std::size_t calls = 0;
  const MirInstruction *readerCall = nullptr;
  const auto &reader = named(session.program(), "Reader");
  const auto &writer = named(session.program(), "Writer");
  for (const auto address :
       enumerateMirBodyAddresses(session.program().mir())) {
    const auto *body = findMirBody(session.program().mir(), address);
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Call ||
            instruction.dispatch != CallDispatch::Virtual)
          continue;
        const auto plan = dispatch.planCall(instruction);
        const auto &source =
            *session.program().mir().findClassInstance(plan.sourceClass);
        const auto &root =
            *session.program().mir().findFunctionInstance(plan.root);
        expect(
            plan.sourceClass ==
                    session.layouts().instance(instruction.receiver->type).id &&
                root.owner == plan.rootOwner &&
                dispatch.slotProfile(plan.rootOwner).at(plan.slot) == plan.root,
            "call planning consumes the exact sealed source/root identities "
            "and stable slot");
        auto *probe = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                    {session.layouts().pointer()}, false),
            llvm::GlobalValue::InternalLinkage, "dispatch.call.probe",
            session.module());
        llvm::IRBuilder<> ir(
            llvm::BasicBlock::Create(session.context(), "entry", probe));
        const auto loaded =
            dispatch.loadCall(ir, instruction, plan, probe->getArg(0));
        const auto loads = std::count_if(
            probe->getEntryBlock().begin(), probe->getEntryBlock().end(),
            [](const auto &operation) {
              return llvm::isa<llvm::LoadInst>(operation);
            });
        expect(loads == 2,
               "active-object metadata adds no load to ordinary virtual calls");
        expect(llvm::isa<llvm::LoadInst>(loaded.callee) &&
                   offsetFrom(session, loaded.receiver, probe->getArg(0)) ==
                       plan.location.owner.offset,
               "one evaluated source receiver projects once to the physical "
               "pointer owner");
        for (int mutation : {0, 1, 2}) {
          auto changed = plan;
          if (mutation == 0)
            ++changed.slot;
          else if (mutation == 1)
            ++changed.location.owner.offset;
          else
            changed.abi.call.parameters.clear();
          rejects<std::invalid_argument>(
              [&] {
                static_cast<void>(dispatch.loadCall(ir, instruction, changed,
                                                    probe->getArg(0)));
              },
              "a forged slot/path/channel plan cannot change native call "
              "emission");
        }
        if (source.id == reader.id)
          readerCall = &instruction;
        ir.CreateRetVoid();
        expect(!llvm::verifyFunction(*probe),
               "virtual address selection forms valid typed IR");
        ++calls;
      }
  }
  expect(calls != 0 && readerCall,
         "call probes include an unknown interface receiver");
  if (!readerCall)
    return;
  auto missing = *readerCall;
  missing.receiver->type = writer.type;
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(dispatch.planCall(missing)); },
      "an unrelated view cannot supply a selected virtual root");
  auto mismatch = *readerCall;
  mismatch.info.type = SemanticType::Bool;
  rejects<std::invalid_argument>(
      [&] { static_cast<void>(dispatch.planCall(mismatch)); },
      "a mismatched call result cannot alter the root contract");
  mismatch = *readerCall;
  mismatch.definedFailure.propagation = FailurePropagationKind::None;
  rejects<std::invalid_argument>(
      [&] { static_cast<void>(dispatch.planCall(mismatch)); },
      "a virtual invocation cannot drop its original failure propagation");

  const auto &middle = named(session.program(), "Middle");
  const auto root = reader.virtualDispatch.front().root;
  const auto target =
      std::find_if(middle.virtualDispatch.begin(), middle.virtualDispatch.end(),
                   [&](const auto &row) { return row.root == root; });
  if (target == middle.virtualDispatch.end())
    throw std::runtime_error("test lost its concrete interface implementation");
  auto direct = *readerCall;
  direct.dispatch = CallDispatch::Static;
  direct.functionTarget = target->target;
  direct.dispatchOwner = middle.type;
  expect(
      isMirExactReceiverPresentation(session.program().mir(), direct,
                                     reader.type),
      "physical inverse test uses only the exact MIR presentation predicate");
  auto *probe = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {session.layouts().pointer()}, false),
      llvm::GlobalValue::InternalLinkage, "dispatch.exact.presentation.probe",
      session.module());
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", probe));
  // This checks only physical emission under that predicate, not a new
  // optimizer proof or acceptance of this edited instruction as source MIR.
  const auto *adjusted = llvm::dyn_cast<llvm::GEPOperator>(
      dispatch.directReceiver(ir, direct, probe->getArg(0)));
  const auto *delta =
      adjusted ? llvm::dyn_cast<llvm::ConstantInt>(adjusted->getOperand(1))
               : nullptr;
  expect(delta && delta->getSExtValue() ==
                      -static_cast<std::int64_t>(
                          session.layouts()
                              .baseSubobject(middle.type, reader.type)
                              .offset),
         "exact interface presentation reverses only its proven nonzero owner "
         "path");
  direct.dispatchOwner = SemanticType::Unknown;
  rejects<std::logic_error>(
      [&] {
        static_cast<void>(
            dispatch.directReceiver(ir, direct, probe->getArg(0)));
      },
      "without the exact presentation predicate no generic downcast exists");
  ir.CreateRetVoid();
  expect(!llvm::verifyFunction(*probe),
         "exact receiver presentation forms valid typed IR");
}

void testBodyCallHook() {
  const auto frontend =
      Frontend().analyze("llvm-dispatch-reference-body.gti", std::string(R"(
class Reader { public: virtual int read() { return 1; }  public: Reader() = default; };
int via(Reader& value) { return value.read(); }
)"));
  expect(frontend.canGenerateCode(),
         "reference-only call component is valid source");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error(
        "reference-only body component failed its source seal");
  Session session(*input, {});
  const auto admission = planLlvmAdmission(*input);
  expect(admission.admitted(),
         "borrowed scalar virtual calls receive exact per-use admission");
  // Inspect internal IR only for this reference-only module. No plan is
  // mutated, no production backend entry point or object publication is used.
  lowerScalarProgram(session, admission);
  bool found = false;
  for (const auto &function : session.program().mir().functionInstances()) {
    const auto *declaration =
        session.program().findFunctionDeclaration(function.declaration);
    if (!declaration || declaration->qualifiedName != "via")
      continue;
    auto *lowered = session.abi().declareFunction(function.id);
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.dispatch != CallDispatch::Virtual)
          continue;
        auto dispatch = DispatchStructuralTestAccess::inspect(session);
        const auto plan = dispatch.planCall(instruction);
        for (const auto &native : *lowered)
          for (const auto &operation : native)
            if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation);
                call && !call->getCalledFunction()) {
              found = true;
              expect(call->getFunctionType() == plan.abi.call.type,
                     "Body.call uses the exact root type rather than the named "
                     "body's signature");
              const auto *named = session.program().mir().findFunctionInstance(
                  *instruction.functionTarget);
              expect(!named->mayRaiseDefinedFailure &&
                         session.abi()
                                 .declareFunction(named->id)
                                 ->getFunctionType() != call->getFunctionType(),
                     "an infallible named root body does not erase the virtual "
                     "failure interface");
              auto *attributes = llvm::Function::Create(
                  plan.abi.call.type, llvm::GlobalValue::ExternalLinkage,
                  "dispatch.call.attributes", session.module());
              plan.abi.call.applyTo(*attributes);
              expect(call->getAttributes() == attributes->getAttributes(),
                     "indirect call attributes come from the same root ABI "
                     "planner");
              attributes->eraseFromParent();
              const auto *branch = native.getTerminator();
              expect(branch && branch->getNumSuccessors() == 2 &&
                         branch->getOperand(0) == call,
                     "virtual status branches through the existing call "
                     "success/failure machinery");
              if (!branch || branch->getNumSuccessors() != 2)
                continue;
              const auto *failure = branch->getSuccessor(1)->getTerminator();
              expect(failure && failure->getNumSuccessors() == 1 &&
                         failure->getSuccessor(0)->getName() ==
                             "bb" + std::to_string(block.terminator.elseTarget),
                     "the original MIR Invoke failure target is preserved");
              const auto *result =
                  call->getArgOperand(*plan.abi.call.resultDestination);
              for (const auto &use : result->uses())
                if (const auto *load =
                        llvm::dyn_cast<llvm::LoadInst>(use.getUser()))
                  expect(load->getParent() == branch->getSuccessor(0),
                         "the virtual result is loaded only in the success "
                         "continuation");
            }
      }
  }
  expect(found, "the isolated Body component contains its typed indirect call");
  session.verify();
}

void testStaticLifecycle() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto nested =
      Frontend().analyze(root / "tests/fixtures/llvm_dispatch_lifecycle.gti");
  expect(nested.canGenerateCode(), "nested lifecycle source verifies");
  if (!nested.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> nestedIssues;
  const auto nestedInput = verifiedLoweredProgram(
      gti_test::lowerProgram(nested, nested.mir, nested.mir), nestedIssues);
  if (!nestedInput)
    throw std::runtime_error("nested lifecycle source seal failed");
  const auto nestedAdmission = planLlvmAdmission(*nestedInput);
  // Its base destructor observes its own phase under the lifecycle-dispatch
  // transition MIR records for it (owner decision, 2026-10-27).
  expect(nestedAdmission.admitted() &&
             std::any_of(nestedAdmission.requirements().begin(),
                         nestedAdmission.requirements().end(),
                         [](const auto &use) {
                           return use.required &&
                                  use.entry->family == "Shape" &&
                                  use.entry->name ==
                                      "DestructorDispatchObservation" &&
                                  use.implementedShape &&
                                  use.implementedShape->name ==
                                      "NativeLifecycleDispatch";
                         }),
         "nested lifecycle fixture witnesses its destructor observation with "
         "the recorded lifecycle-dispatch transition: " +
             nestedAdmission.limitation());
  const auto &nestedContainer = named(nestedInput->program(), "Container");
  const auto &nestedTrivial = named(nestedInput->program(), "Trivial");
  bool nestedRecipe = false;
  for (const auto &function : nestedInput->program().mir().functionInstances())
    for (const auto &obligation : function.body.dropObligations)
      if (obligation.dropType.classInstance == nestedContainer.id) {
        nestedRecipe = true;
        expect(!nestedTrivial.requiresActiveCleanup &&
                   std::none_of(obligation.dropType.aggregate.begin(),
                                obligation.dropType.aggregate.end(),
                                [&](const auto &child) {
                                  return child.classInstance ==
                                         nestedTrivial.id;
                                }),
               "the original sealed recipe omits its live trivial polymorphic "
               "child rather than inventing liveness");
      }
  expect(nestedRecipe, "the original nested lifecycle cleanup remains covered");
  Session nestedSession(*nestedInput, {});
  lowerScalarProgram(nestedSession, nestedAdmission);
  nestedSession.verify();

  // Keep the constructor/base/failure-body inspection at whole local final
  // destinations. The original fixture still has no public admission for
  // destructor observations; its independent cleanup/placement contract is
  // exercised privately above without granting that authority.
  const auto frontend = Frontend().analyze("llvm-static-lifecycle-body.gti", R"(
class Token {
  int divisor = 1;
public:
  Token(int value) : divisor(value) {}
  ~Token() { int checked = 12 / this.divisor; }
public:
  Token() = default;
};
class Base {
public:
  Base(int divisor) {
    int checked = 12 / (divisor + 1);
    int observed = this.phase();
  }
  virtual int phase() { return 1; }
  ~Base() { int observed = this.phase(); }
public:
  Base() = default;
};
interface Side { int side(); };
interface OtherSide { int other_side(); };
class Derived : public Side, public OtherSide, public Base {
  Token token = Token(1);
  int quotient = 0;
public:
  Derived(int divisor) : Base(divisor), token(Token(divisor)), quotient(12 / divisor) {
    int observed = this.phase();
  }
  int phase() override { return 2; }
  int side() override { return 6; }
  int other_side() override { return 7; }
  ~Derived() { int observed = this.phase(); }
public:
  Derived() = default;
};
class Trivial {
public:
  Trivial() {}
  virtual int phase() { return 4; }
};
class Container {
  Token guard = Token(1);
  Token guards[2] = {Token(1), Token(2)};
public:
  Container() : guard(Token(1)) {}
  virtual int phase() { return 5; }
};
int evaluate(int divisor) {
  Derived value{divisor};
  Container holder{};
  Trivial trivial{};
  return value.phase();
}
int main() { return evaluate(2) - 2; }
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "static lifecycle source verifies");
    return;
  }
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("static lifecycle source seal failed");
  Session session(*input, {});
  const auto admission = planLlvmAdmission(*input);
  expect(admission.admitted(),
         "static lifecycle IR is admitted under its destructors' recorded "
         "lifecycle-dispatch transitions: " +
             admission.limitation());
  {
    // No Failures instance or emitting Lifetimes object exists in this scope.
    // Use actual plan-bound dispatch in otherwise cold sessions.
    Dispatch checkedDispatch(session, admission);
    const auto &derived = named(session.program(), "Derived");
    const MirDropType *recipe = nullptr;
    for (const auto &function : session.program().mir().functionInstances())
      for (const auto &obligation : function.body.dropObligations)
        if (obligation.dropType.classInstance == derived.id)
          recipe = &obligation.dropType;
    if (!recipe || recipe->aggregate.size() < 2 ||
        recipe->aggregate.back().type.kind != SemanticType::Class)
      throw std::runtime_error(
          "cleanup preflight test lost its ordered child recipes");
    const auto otherInput = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
    if (!otherInput)
      throw std::runtime_error("cleanup preflight test lost its second seal");
    const auto otherPlan = planLlvmAdmission(*otherInput);
    Session otherSession(*otherInput, {});
    Dispatch otherDispatch(otherSession, otherPlan);
    const LlvmBackendOptions otherOptions{
        .optimization = LlvmCodegenOptimization::Aggressive};
    const auto optionsPlan = planLlvmAdmission(*input, otherOptions);
    Session optionsSession(*input, otherOptions);
    Dispatch optionsDispatch(optionsSession, optionsPlan);
    auto *probe = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                false),
        llvm::GlobalValue::InternalLinkage, "cleanup.preflight.probe",
        session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", probe));
    auto *state = ir.CreateAlloca(ir.getInt8Ty());
    auto *marker = ir.CreateStore(ir.getInt8(73), state);
    const auto unchanged = [&](const auto &action) {
      const auto publication = modulePublication(session);
      const auto otherPublication = modulePublication(otherSession);
      const auto optionsPublication = modulePublication(optionsSession);
      const auto *block = ir.GetInsertBlock();
      const auto point = ir.GetInsertPoint();
      action();
      expect(modulePublication(session) == publication &&
                 modulePublication(otherSession) == otherPublication &&
                 modulePublication(optionsSession) == optionsPublication &&
                 ir.GetInsertBlock() == block && ir.GetInsertPoint() == point &&
                 marker->getPointerOperand() == state &&
                 marker->getValueOperand() == ir.getInt8(73),
             "cleanup preflight changes no functions, globals, blocks, IR "
             "insertion or stored state in any session");
    };
    unchanged([&] {
      Lifetimes::preflightCleanup(session, checkedDispatch, *recipe);
      Lifetimes::preflightCleanup(session, checkedDispatch, *recipe);
    });
    auto lateBadChild = *recipe;
    lateBadChild.aggregate.back().classInstance =
        std::numeric_limits<HirClassInstanceId>::max();
    unchanged([&] {
      rejects<std::logic_error>(
          [&] {
            Lifetimes::preflightCleanup(session, checkedDispatch, lateBadChild);
          },
          "a late child identity failure cannot publish an earlier cleanup "
          "helper or active-class table");
      rejects<BackendUnsupportedError>(
          [&] {
            Lifetimes::preflightCleanup(session, checkedDispatch, *recipe,
                                        true);
          },
          "non-emitting preflight retains polymorphic replacement rejection");
    });
    unchanged([&] {
      rejects<std::invalid_argument>(
          [&] { Lifetimes::preflightCleanup(session, otherDispatch, *recipe); },
          "equal numeric IDs from another sealed program cannot bind cleanup "
          "preflight");
      rejects<std::invalid_argument>(
          [&] {
            Lifetimes::preflightCleanup(session, optionsDispatch, *recipe);
          },
          "a dispatch planner from another options session cannot validate "
          "cleanup preflight");
    });
    ir.CreateRetVoid();
  }
  // Private component inspection only: no object/executable publication and no
  // native O10 coverage claim. The original rejected admission is unchanged.
  lowerScalarProgram(session, admission);
  const auto nativeBody = [&](MirBodyAddress address) {
    auto *function = session.module().getFunction(
        "__gti_native_v1_body_" +
        std::to_string(static_cast<unsigned>(address.kind)) + "_" +
        std::to_string(address.owner));
    if (!function)
      throw std::runtime_error("static lifecycle test lost its native body");
    return function;
  };
  const auto isTableStore = [](const llvm::Instruction &operation) {
    const auto *store = llvm::dyn_cast<llvm::StoreInst>(&operation);
    const auto *table = store
                            ? llvm::dyn_cast<llvm::GlobalVariable>(
                                  store->getValueOperand()->stripPointerCasts())
                            : nullptr;
    return table && table->getName().starts_with("__gti_dispatch_table_");
  };
  const auto &base = named(session.program(), "Base");
  const auto &derived = named(session.program(), "Derived");
  const auto &container = named(session.program(), "Container");
  expect(session.layouts().baseSubobject(derived.type, base.type).offset != 0,
         "lifecycle fixture retains two interface views before its physical "
         "state base");
  bool entry = false, afterBase = false, rollback = false;
  for (const auto &constructor :
       session.program().mir().constructorInstances()) {
    if (!constructor.dispatchState)
      continue;
    auto *function = nativeBody({MirBodyKind::Constructor, constructor.id});
    for (const auto &block : *function)
      for (const auto &operation : block) {
        if (!isTableStore(operation))
          continue;
        if (constructor.owner == base.id) {
          entry = true;
          expect(&block == &function->getEntryBlock(),
                 "root constructor installs state at its verified entry");
        }
        if (constructor.owner == derived.id) {
          afterBase = true;
          expect(block.getName().starts_with("call.success"),
                 "derived state is installed only on successful base "
                 "construction");
          const auto *predecessor = block.getSinglePredecessor();
          expect(
              predecessor &&
                  predecessor->getTerminator()->getNumSuccessors() == 2 &&
                  predecessor->getTerminator()->getSuccessor(0) == &block,
              "the failed base edge cannot enter derived state installation");
        }
      }
    if (constructor.owner == derived.id)
      for (const auto &obligation : constructor.body.dropObligations)
        if (obligation.kind == MirDropObligationKind::ConstructionRollback &&
            obligation.dropType.classInstance == base.id) {
          rollback = true;
          expect(
              !obligation.initiallyActive,
              "base rollback remains inactive before successful publication");
        }
  }
  expect(entry && afterBase && rollback,
         "static lifecycle inspection covers entry, base success and exact "
         "rollback");
  const auto recipeFor = [&](HirClassInstanceId owner) -> const MirDropType & {
    for (const auto &function : session.program().mir().functionInstances())
      for (const auto &obligation : function.body.dropObligations)
        if (obligation.dropType.classInstance == owner)
          return obligation.dropType;
    throw std::runtime_error(
        "static lifecycle test lost its complete drop recipe");
  };
  auto dispatch = DispatchStructuralTestAccess::inspect(session);
  Failures failureChannel(session);
  Lifetimes lifetimes(session, failureChannel, dispatch, nativeBody);
  const auto &recipe = recipeFor(derived.id);
  auto *cleanup = lifetimes.cleanup(recipe);
  llvm::Function *observation = nullptr;
  llvm::DominatorTree dominance(*cleanup);
  bool guardedStore = false;
  for (const auto &block : *cleanup)
    for (const auto &operation : block) {
      if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation);
          call && call->getCalledFunction() &&
          call->getCalledFunction()->getName().starts_with(
              "__gti_native_cleanup_observation_"))
        observation = call->getCalledFunction();
      if (isTableStore(operation)) {
        guardedStore = true;
        expect(
            block.getName() == "dispatch.install" &&
                dominance.dominates(&cleanup->getEntryBlock(), &block),
            "cleanup header stores are isolated behind the observation branch");
      }
    }
  expect(observation && guardedStore,
         "static cleanup has a representation-only observation guard");
  const auto *entryBranch = cleanup->getEntryBlock().getTerminator();
  expect(entryBranch && entryBranch->getNumSuccessors() == 2 &&
             entryBranch->getSuccessor(0)->getName() == "dispatch.install" &&
             entryBranch->getSuccessor(1)->getName() == "dispatch.ready" &&
             llvm::isa<llvm::CallInst>(entryBranch->getOperand(0)) &&
             llvm::cast<llvm::CallInst>(entryBranch->getOperand(0))
                     ->getCalledFunction() == observation,
         "only the true cleanup-observation edge can write dispatch headers");
  const auto publicationBeforeRejection = modulePublication(session);
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(lifetimes.cleanup(recipe, true)); },
      "polymorphic replacement cannot silently adopt destruction-phase "
      "dispatch");
  auto excluded = recipe;
  excluded.polymorphicCleanup = true;
  rejects<std::logic_error>(
      [&] { static_cast<void>(lifetimes.cleanup(excluded)); },
      "a class recipe cannot acquire dynamic owner cleanup authority");
  excluded = recipe;
  excluded.recursiveCleanup = true;
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(lifetimes.cleanup(excluded)); },
      "unknown recursive cleanup cannot become a false observation witness");
  expect(modulePublication(session) == publicationBeforeRejection,
         "excluded recipes reject before creating cleanup helpers, including "
         "cached replacements");

  const auto passiveArrayType =
      SemanticType::arrayOf(SemanticType::arrayOf(SemanticType::Int32, 3), 2);
  MirDropType passiveArray{
      .type = passiveArrayType,
      .fixedArray = MirFixedArrayDropSchedule{.extents = {2, 3},
                                              .leafType = SemanticType::Int32}};
  expect(!mirUniqueCleanupType(session.program().mir(), passiveArrayType),
         "passive fixed arrays retain the canonical-null fallback contract");
  const auto rejectPassiveMutations = [&](std::string_view phase) {
    for (int mutation = 0; mutation != 10; ++mutation) {
      auto malformed = passiveArray;
      if (mutation == 0)
        malformed.fixedArray->extents = {2, 4};
      else if (mutation == 1)
        malformed.fixedArray->leafType = SemanticType::UInt32;
      else if (mutation == 2)
        malformed.fixedArray->leafClassInstance = derived.id;
      else if (mutation == 3)
        malformed.fixedArray->leafClassInstance =
            std::numeric_limits<HirClassInstanceId>::max();
      else if (mutation == 4)
        malformed.fixedArray->leafClassInstance = HirClassInstanceId{0};
      else if (mutation == 5)
        malformed.destructor = derived.destructor;
      else if (mutation == 6)
        malformed.classInstance = derived.id;
      else if (mutation == 7)
        malformed.lambdaInstance = 1;
      else if (mutation == 8)
        malformed.requiresActiveCleanup = true;
      else
        malformed.uniqueOwnerField = 1;
      const auto publication = modulePublication(session);
      rejects<std::logic_error>(
          [&] { static_cast<void>(lifetimes.cleanup(malformed)); },
          "a malformed passive fixed-array fallback rejects " +
              std::string(phase));
      expect(modulePublication(session) == publication,
             "a rejected passive fixed-array fallback publishes no helper, "
             "global, block or instruction " +
                 std::string(phase));
    }
  };
  rejectPassiveMutations("before cache warming");
  auto *passiveCleanup = lifetimes.cleanup(passiveArray);
  const auto passivePublication = modulePublication(session);
  expect(passiveCleanup && lifetimes.cleanup(passiveArray) == passiveCleanup &&
             modulePublication(session) == passivePublication,
         "a real passive fixed-array fallback emits once and reuses its exact "
         "helper");
  rejectPassiveMutations("after cache warming");
  const auto &containerRecipe = recipeFor(container.id);
  auto *containerCleanup = lifetimes.cleanup(containerRecipe);
  llvm::Function *containerObservation = nullptr;
  for (const auto &operation : containerCleanup->getEntryBlock())
    if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation);
        call && call->getCalledFunction() &&
        call->getCalledFunction()->getName().starts_with(
            "__gti_native_cleanup_observation_"))
      containerObservation = call->getCalledFunction();
  expect(
      containerObservation && !containerRecipe.destructor,
      "a parent without its own destructor still observes exact live children");
  for (const auto &function : session.module())
    if (function.getName().starts_with("__gti_native_cleanup_observation_"))
      for (const auto &block : function)
        for (const auto &operation : block) {
          expect(!llvm::isa<llvm::StoreInst>(operation),
                 "observation helpers never change liveness or object state");
          if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&operation))
            expect(load->getType()->isIntegerTy(8),
                   "observation reads only source-destructor active bytes, "
                   "never dispatch pointers");
        }
  bool guardedDrop = false, failureDrain = false;
  for (const auto &function : session.module())
    for (const auto &block : function)
      for (const auto &operation : block)
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation);
            call && call->getCalledFunction() &&
            call->getCalledFunction()->getName().starts_with(
                "__gti_native_cleanup_") &&
            !call->getCalledFunction()->getName().starts_with(
                "__gti_native_cleanup_observation_")) {
          if (block.getName().starts_with("drop") &&
              !block.getName().starts_with("drop.done")) {
            const auto *predecessor = block.getSinglePredecessor();
            const auto *branch =
                predecessor ? predecessor->getTerminator() : nullptr;
            expect(branch && branch->getNumSuccessors() == 2 &&
                       branch->getSuccessor(0) == &block,
                   "outer obligations guard completed-object/subobject entry "
                   "before header access");
            guardedDrop = true;
          }
          if (block.getName().starts_with("next") ||
              block.getName() == "dispatch.ready")
            failureDrain = true;
        }
  expect(guardedDrop && failureDrain,
         "static cleanup retains obligation guards and child draining after "
         "source cleanup");

  // Constant-state IR probes check the exact generated witness truth table.
  // This is not native execution evidence and does not publish an artifact.
  std::vector<std::pair<std::string, bool>> probes;
  const auto probe = [&](const MirDropType &selected, llvm::Function *predicate,
                         std::string name, int armed, bool expected) {
    if (!predicate)
      return;
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt1Ty(session.context()),
                                false),
        llvm::GlobalValue::ExternalLinkage, name, session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    auto *address =
        ir.CreateAlloca(session.layouts().get(selected.type).storage);
    ir.CreateStore(llvm::Constant::getNullValue(
                       session.layouts().get(selected.type).storage),
                   address);
    std::vector<llvm::Value *> flags;
    const auto collect = [&](const auto &self, const MirDropType &current,
                             llvm::Value *where) -> void {
      if (current.destructor && !current.fixedArray)
        flags.push_back(ir.CreateStructGEP(
            session.layouts().get(current.type).storage, where,
            *session.layouts().activeIndex(current.type)));
      if (current.fixedArray) {
        expect(current.type.arrayLength <= 2,
               "constant-state fixture has bounded array probes");
        for (std::size_t i = 0; i < current.type.arrayLength; ++i)
          self(self, current.aggregate.front(),
               ir.CreateGEP(session.layouts().get(current.type).storage, where,
                            {ir.getInt64(0), ir.getInt64(i)}));
      } else {
        for (const auto &child : current.aggregate) {
          const auto index = child.uniqueOwnerField
                                 ? session.layouts().fieldIndex(
                                       current.type, child.uniqueOwnerField)
                                 : session.layouts().baseIndex(
                                       current.type, *child.uniqueOwnerBase);
          self(self, child,
               ir.CreateStructGEP(session.layouts().get(current.type).storage,
                                  where, index));
        }
      }
    };
    collect(collect, selected, address);
    if (armed >= 0)
      ir.CreateStore(ir.getInt8(1), flags.at(static_cast<std::size_t>(armed)));
    ir.CreateRet(ir.CreateCall(predicate, {address}));
    probes.emplace_back(std::move(name), expected);
  };
  probe(recipe, observation, "cleanup.probe.own.only", 0, true);
  probe(recipe, observation, "cleanup.probe.child.only", 1, true);
  probe(recipe, observation, "cleanup.probe.moved.shell", -1, false);
  probe(containerRecipe, containerObservation, "cleanup.probe.array.child", 0,
        true);
  probe(containerRecipe, containerObservation, "cleanup.probe.trivial.only", -1,
        false);
  session.verify();
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(cgscc);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  auto pipeline =
      builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  pipeline.run(session.module(), modules);
  session.verify();
  for (const auto &[name, expected] : probes) {
    const auto *function = session.module().getFunction(name);
    const auto *ret = function && function->size() == 1
                          ? llvm::dyn_cast<llvm::ReturnInst>(
                                function->getEntryBlock().getTerminator())
                          : nullptr;
    const auto *constant =
        ret ? llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue())
            : nullptr;
    expect(
        constant && constant->isOne() == expected,
        "generated cleanup observation folds to the exact state truth table: " +
            name);
  }
}

void testCleanupCompositionProofs() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto recursiveFrontend = Frontend().analyze(
      root / "tests/fixtures/recursive_cleanup_failure.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(recursiveFrontend.canGenerateCode(),
         "recursive cleanup composition source verifies");
  if (!recursiveFrontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> recursiveIssues;
  const auto recursiveInput = verifiedLoweredProgram(
      gti_test::lowerProgram(recursiveFrontend, recursiveFrontend.mir,
                             recursiveFrontend.mir),
      recursiveIssues);
  if (!recursiveInput)
    throw std::runtime_error(
        "recursive cleanup composition source seal failed");
  Session recursiveSession(*recursiveInput, {});
  auto recursiveDispatch =
      DispatchStructuralTestAccess::inspect(recursiveSession);
  Failures recursiveFailures(recursiveSession);
  std::map<HirDestructorInstanceId, llvm::Function *> destructorBodies;
  Lifetimes *activeLifetimes = nullptr;
  std::optional<MirDropType> reentryRecipe;
  bool reentryEnabled = false;
  bool reentryChecked = false;
  const auto findDestructorBody = [&](MirBodyAddress address) {
    if (reentryEnabled && !reentryChecked && activeLifetimes && reentryRecipe) {
      reentryChecked = true;
      reentryEnabled = false;
      const auto publication = modulePublication(recursiveSession);
      rejects<std::logic_error>(
          [&] {
            static_cast<void>(activeLifetimes->cleanup(*reentryRecipe, true));
          },
          "a public callback reentry proves its malformed cleanup "
          "independently");
      expect(modulePublication(recursiveSession) == publication,
             "a rejected public callback reentry publishes no nested artifact");
    }
    if (address.kind != MirBodyKind::Destructor)
      throw std::runtime_error(
          "cleanup composition requested a non-destructor body");
    if (const auto found = destructorBodies.find(address.owner);
        found != destructorBodies.end())
      return found->second;
    const auto *target =
        recursiveSession.program().mir().findDestructorInstance(address.owner);
    if (!target)
      throw std::runtime_error(
          "cleanup composition lost its destructor identity");
    const auto abi = recursiveSession.abi().gti(
        SemanticType::Void, {}, target->mayRaiseDefinedFailure, true);
    auto *function = llvm::Function::Create(
        abi.type, llvm::GlobalValue::InternalLinkage,
        "cleanup.composition.destructor." + std::to_string(address.owner),
        recursiveSession.module());
    abi.applyTo(*function);
    llvm::IRBuilder<> ir(llvm::BasicBlock::Create(recursiveSession.context(),
                                                  "entry", function));
    if (abi.statusResult)
      ir.CreateRet(ir.getTrue());
    else
      ir.CreateRetVoid();
    destructorBodies.emplace(address.owner, function);
    return function;
  };
  Lifetimes recursiveLifetimes(recursiveSession, recursiveFailures,
                               recursiveDispatch, findDestructorBody);
  activeLifetimes = &recursiveLifetimes;
  const auto &node = named(recursiveSession.program(), "Node");
  const auto &left = named(recursiveSession.program(), "Left");
  const auto nodeRecipe =
      mirUniqueCleanupType(recursiveSession.program().mir(), node.type);
  const auto leftRecipe =
      mirUniqueCleanupType(recursiveSession.program().mir(), left.type);
  expect(nodeRecipe && leftRecipe,
         "recursive cleanup composition retains exact finite root recipes");
  if (!nodeRecipe || !leftRecipe)
    return;

  std::vector<std::size_t> markerPath;
  const auto findMarker = [&](const auto &self, const MirDropType &selected,
                              std::vector<std::size_t> path) -> bool {
    if (selected.recursiveCleanup) {
      markerPath = std::move(path);
      return true;
    }
    for (std::size_t index = 0; index < selected.aggregate.size(); ++index) {
      auto child = path;
      child.push_back(index);
      if (self(self, selected.aggregate[index], std::move(child)))
        return true;
    }
    return false;
  };
  expect(findMarker(findMarker, *nodeRecipe, {}) && markerPath.size() >= 2,
         "recursive cleanup composition retains an owning-path back-edge");
  if (markerPath.size() < 2)
    return;
  const auto marker = [&](MirDropType &selected) -> MirDropType & {
    auto *current = &selected;
    for (const auto index : markerPath)
      current = &current->aggregate.at(index);
    return *current;
  };
  auto markerSource = *nodeRecipe;
  const MirDropType orphanMarker = marker(markerSource);
  reentryRecipe = orphanMarker;
  const auto rejectMalformed = [&](std::string_view phase) {
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = mutation == 0 ? orphanMarker : *nodeRecipe;
      if (mutation == 0) {
        // The exact marker is valid only while bound to its owning ancestor.
      } else if (mutation == 1)
        marker(malformed).classInstance = left.id;
      else if (mutation == 2)
        marker(malformed).uniqueOwnerField = 1;
      else if (mutation == 3) {
        const auto inlineMarker = marker(malformed);
        malformed.aggregate.front() = inlineMarker;
      } else
        marker(malformed).type = left.type;
      const auto publication = modulePublication(recursiveSession);
      rejects<std::logic_error>(
          [&] {
            static_cast<void>(recursiveLifetimes.cleanup(malformed, true));
          },
          "an orphan, foreign or inline recursive back-edge rejects " +
              std::string(phase));
      expect(modulePublication(recursiveSession) == publication,
             "a rejected recursive graph publishes no helper, global, block "
             "or instruction " +
                 std::string(phase));
    }
  };
  rejectMalformed("before cache warming");
  reentryEnabled = true;
  auto *nodeCleanup = recursiveLifetimes.cleanup(*nodeRecipe);
  auto *leftCleanup = recursiveLifetimes.cleanup(*leftRecipe);
  const auto warmPublication = modulePublication(recursiveSession);
  expect(reentryChecked && nodeCleanup && leftCleanup &&
             nodeCleanup != leftCleanup &&
             recursiveLifetimes.cleanup(*nodeRecipe, true) == nodeCleanup &&
             recursiveLifetimes.cleanup(*leftRecipe, true) == leftCleanup &&
             modulePublication(recursiveSession) == warmPublication,
         "ordinary recursive caches transition to exact validated replacement "
         "helpers after independently rejected callback reentry");
  rejectMalformed("after cache warming");
  bool failed = false, next = false, secondary = false;
  for (const auto &block : *nodeCleanup) {
    failed |= block.getName() == "failed";
    next |= block.getName() == "next";
    secondary |= block.getName() == "secondary";
  }
  expect(failed && next && secondary,
         "recursive owner cleanup retains first-failure draining and "
         "second-failure termination paths");
  recursiveSession.verify();

  const auto lambdaFrontend =
      Frontend().analyze("llvm-cleanup-lambda-identity.gti", R"(
import std.utility;
class Token {
public:
  Token() {}
  Token(Token& other) = delete;
  Token(Token&& other) = default;
  int value = 0;
  ~Token() { auto checked = this.value + 1; }
};
int invoke<F>(F callable) { return callable(); }
int owned() {
  Token first{};
  Token second{};
  auto callback =
      [first = std::move(first), second = std::move(second)]() -> int {
    return 17;
  };
  return callback();
}
class Field {
  int value = invoke([]() -> int { return 17; });
public:
  int read() { return this.value; }
public:
  Field() = default;
};
int main() { Field value{}; return value.read() + owned() - 34; }
)",
                         {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(lambdaFrontend.canGenerateCode(),
         "cleanup lambda identity source verifies");
  if (!lambdaFrontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> lambdaIssues;
  const auto lambdaInput = verifiedLoweredProgram(
      gti_test::lowerProgram(lambdaFrontend, lambdaFrontend.mir,
                             lambdaFrontend.mir),
      lambdaIssues);
  if (!lambdaInput)
    throw std::runtime_error("cleanup lambda identity source seal failed");
  Session lambdaSession(*lambdaInput, {});
  auto lambdaDispatch = DispatchStructuralTestAccess::inspect(lambdaSession);
  Failures lambdaFailures(lambdaSession);
  std::map<HirDestructorInstanceId, llvm::Function *> lambdaDestructorBodies;
  const auto findLambdaDestructorBody = [&](MirBodyAddress address) {
    if (address.kind != MirBodyKind::Destructor)
      throw std::runtime_error(
          "closure cleanup requested a non-destructor body");
    if (const auto found = lambdaDestructorBodies.find(address.owner);
        found != lambdaDestructorBodies.end())
      return found->second;
    const auto *target =
        lambdaSession.program().mir().findDestructorInstance(address.owner);
    if (!target)
      throw std::runtime_error("closure cleanup lost its destructor identity");
    const auto abi = lambdaSession.abi().gti(
        SemanticType::Void, {}, target->mayRaiseDefinedFailure, true);
    auto *function = llvm::Function::Create(
        abi.type, llvm::GlobalValue::InternalLinkage,
        "cleanup.capture.destructor." + std::to_string(address.owner),
        lambdaSession.module());
    abi.applyTo(*function);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(lambdaSession.context(), "entry", function));
    if (abi.statusResult)
      ir.CreateRet(ir.getTrue());
    else
      ir.CreateRetVoid();
    lambdaDestructorBodies.emplace(address.owner, function);
    return function;
  };
  Lifetimes lambdaLifetimes(lambdaSession, lambdaFailures, lambdaDispatch,
                            findLambdaDestructorBody);
#if !defined(GTI_LLVM_C04_NATIVE_UNWIND)
  const auto tokenRecipe =
      mirUniqueCleanupType(lambdaSession.program().mir(),
                           named(lambdaInput->program(), "Token").type);
  if (!tokenRecipe)
    throw std::runtime_error("terminal destructor fixture lost its recipe");
  auto *terminalCleanup = lambdaLifetimes.cleanup(*tokenRecipe);
  unsigned emergencyChecks = 0;
  for (const auto &block : *terminalCleanup)
    for (const auto &instruction : block)
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
        if (const auto *callee = call->getCalledFunction()) {
          const auto name = callee->getName();
          emergencyChecks += name == "gti_rt_failure_cleanup_check_v1";
          expect(name != "gti_rt_failure_cleanup_enter_v1" &&
                     name != "gti_rt_failure_cleanup_leave_v1",
                 "terminal destructor has no unobservable cleanup context");
        }
  expect(emergencyChecks == 1,
         "terminal destructor retains the incoming secondary-failure check");
#endif
  const MirLambdaInstance *first = nullptr;
  const MirLambdaInstance *second = nullptr;
  const MirLambdaInstance *owned = nullptr;
  for (const auto &candidate :
       lambdaSession.program().mir().lambdaInstances()) {
    if (!owned && candidate.captureTypes.size() == 2 &&
        candidate.captureModes ==
            std::vector<LambdaCaptureMode>{LambdaCaptureMode::Move,
                                           LambdaCaptureMode::Move} &&
        candidate.captureRequiresActiveCleanup == std::vector<bool>{true, true})
      owned = &candidate;
    if (!candidate.captureTypes.empty())
      continue;
    if (!first) {
      first = &candidate;
    } else if (candidate.type == first->type) {
      second = &candidate;
    }
  }
  expect(first && second && owned,
         "cleanup lambda source retains compatible duplicate bodies and an "
         "owned two-capture environment");
  if (!first || !second || !owned)
    return;
  const auto ownedLambda =
      mirUniqueCleanupType(lambdaSession.program().mir(), owned->type);
  expect(ownedLambda && ownedLambda->lambdaInstance == owned->id &&
             ownedLambda->aggregate.size() == 2 &&
             ownedLambda->aggregate[0].captureIndex == 1 &&
             ownedLambda->aggregate[1].captureIndex == 0,
         "owned closure cleanup retains its exact reverse capture schedule");
  if (!ownedLambda)
    return;
  const auto rejectOwnedLambda = [&](std::string_view phase) {
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = *ownedLambda;
      if (mutation == 0)
        malformed.aggregate.front().captureIndex.reset();
      else if (mutation == 1)
        malformed.aggregate.front().captureIndex = 0;
      else if (mutation == 2)
        std::swap(malformed.aggregate[0], malformed.aggregate[1]);
      else if (mutation == 3)
        malformed.aggregate.front().uniqueOwnerField = 1;
      else
        malformed.aggregate.pop_back();
      const auto publication = modulePublication(lambdaSession);
      rejects<std::logic_error>(
          [&] { static_cast<void>(lambdaLifetimes.cleanup(malformed)); },
          "an incomplete, reordered or foreign capture schedule rejects " +
              std::string(phase));
      expect(modulePublication(lambdaSession) == publication,
             "a rejected capture schedule publishes no helper, global, block "
             "or instruction " +
                 std::string(phase));
    }
  };
  rejectOwnedLambda("before cache warming");
  auto *ownedCleanup = lambdaLifetimes.cleanup(*ownedLambda);
  const auto ownedPublication = modulePublication(lambdaSession);
  expect(ownedCleanup &&
             lambdaLifetimes.cleanup(*ownedLambda) == ownedCleanup &&
             modulePublication(lambdaSession) == ownedPublication,
         "an exact populated capture cleanup reuses its validated helper");
  rejectOwnedLambda("after cache warming");
  const auto canonicalLambda =
      mirUniqueCleanupType(lambdaSession.program().mir(), first->type);
  if (!canonicalLambda)
    throw std::runtime_error(
        "cleanup lambda identity lost its canonical recipe");
  auto selectedLambda = *canonicalLambda;
  selectedLambda.lambdaInstance = second->id;
  auto *secondCleanup = lambdaLifetimes.cleanup(selectedLambda);
  auto *firstCleanup = lambdaLifetimes.cleanup(*canonicalLambda);
  const auto lambdaPublication = modulePublication(lambdaSession);
  expect(secondCleanup && firstCleanup && secondCleanup != firstCleanup &&
             lambdaLifetimes.cleanup(selectedLambda) == secondCleanup &&
             modulePublication(lambdaSession) == lambdaPublication,
         "a non-first compatible lambda retains its exact cleanup identity "
         "through emission and cache reuse");
  auto missingLambda = selectedLambda;
  missingLambda.lambdaInstance =
      lambdaSession.program().mir().lambdaInstances().size() + 1;
  const auto beforeMissing = modulePublication(lambdaSession);
  rejects<std::logic_error>(
      [&] { static_cast<void>(lambdaLifetimes.cleanup(missingLambda)); },
      "a missing exact lambda cleanup identity rejects after cache warming");
  expect(modulePublication(lambdaSession) == beforeMissing,
         "a rejected lambda cleanup identity publishes no artifact");
  lambdaSession.verify();

  // Retained private-token cleanup machinery; public source-owner execution
  // is covered independently by the ownership and dispatch native matrices.
  const auto hierarchyOwnerFrontend =
      analyzeLegacyOwner(root, "llvm-cleanup-hierarchy-owner.gti", R"(
import std.memory;
class Base {
public:
  Base() {}
  virtual int read() { return 1; }
};
class Derived : public Base {
public:
  Derived() : Base() {}
  int read() override { return 2; }
};
class HierarchyValue {
  Derived value;
public:
  HierarchyValue() : value(Derived()) {}
};
class UniqueNest {
  std::unique_ptr<HierarchyValue> value;
public:
  UniqueNest() : value(std::unique_ptr<HierarchyValue>()) {}
};
class SharedNest {
  gti_internal::shared_owner<HierarchyValue> value;
public:
  SharedNest() : value(gti_internal::shared_owner_empty<HierarchyValue>()) {}
};
class ActiveHierarchy : public Base {
public:
  ActiveHierarchy() : Base() {}
  ~ActiveHierarchy() {}
};
struct ActiveHierarchyValue { mut ActiveHierarchy value{};  public: ActiveHierarchyValue() = default; };
struct ActiveOwners {
  mut std::unique_ptr<ActiveHierarchyValue> unique = nullptr;
  mut gti_internal::shared_owner<ActiveHierarchyValue> shared = gti_internal::shared_owner_empty<ActiveHierarchyValue>();
public:
  ActiveOwners() = default;

};
int main() {
  UniqueNest unique{}; SharedNest shared{}; ActiveOwners active{}; return 0;
}
)");
  expect(hierarchyOwnerFrontend.canGenerateCode(),
         "hierarchy owner cleanup source verifies");
  if (!hierarchyOwnerFrontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> hierarchyOwnerIssues;
  const auto hierarchyOwnerInput = verifiedLoweredProgram(
      gti_test::lowerProgram(hierarchyOwnerFrontend, hierarchyOwnerFrontend.mir,
                             hierarchyOwnerFrontend.mir),
      hierarchyOwnerIssues);
  if (!hierarchyOwnerInput)
    throw std::runtime_error("hierarchy owner cleanup source seal failed");
  Session hierarchyOwnerSession(*hierarchyOwnerInput, {});
  auto hierarchyOwnerDispatch =
      DispatchStructuralTestAccess::inspect(hierarchyOwnerSession);
  Failures hierarchyOwnerFailures(hierarchyOwnerSession);
  Lifetimes hierarchyOwnerLifetimes(
      hierarchyOwnerSession, hierarchyOwnerFailures, hierarchyOwnerDispatch,
      [](MirBodyAddress) -> llvm::Function * {
        throw std::runtime_error(
            "passive hierarchy owner cleanup requested a source body");
      });
  const auto &hierarchyValue =
      named(hierarchyOwnerSession.program(), "HierarchyValue");
  const auto &uniqueNest = named(hierarchyOwnerSession.program(), "UniqueNest");
  const auto &sharedNest = named(hierarchyOwnerSession.program(), "SharedNest");
  expect(!hierarchyValue.polymorphic &&
             containsHierarchyStorage(hierarchyOwnerSession.program().mir(),
                                      hierarchyValue.type),
         "a nonpolymorphic wrapper independently retains inline hierarchy "
         "storage");
  const MirClassInstance *sharedOwner = nullptr;
  for (const auto &candidate :
       hierarchyOwnerSession.program().mir().classInstances())
    if (candidate.compilerCapability ==
            CompilerCapabilityTypeKind::SharedOwner &&
        candidate.type.arguments.size() == 1 &&
        candidate.type.arguments.front() == hierarchyValue.type) {
      sharedOwner = &candidate;
      break;
    }
  const auto directUnique =
      mirUniqueCleanupType(hierarchyOwnerSession.program().mir(),
                           SemanticType::uniqueOwnerOf(hierarchyValue.type));
  const auto directShared =
      sharedOwner ? mirUniqueCleanupType(hierarchyOwnerSession.program().mir(),
                                         sharedOwner->type)
                  : std::nullopt;
  const auto nestedUnique = mirUniqueCleanupType(
      hierarchyOwnerSession.program().mir(), uniqueNest.type);
  const auto nestedShared = mirUniqueCleanupType(
      hierarchyOwnerSession.program().mir(), sharedNest.type);
  expect(sharedOwner && directUnique && directShared && nestedUnique &&
             nestedShared,
         "direct and nested unique/shared hierarchy owners retain exact "
         "cleanup recipes");
  if (!sharedOwner || !directUnique || !directShared || !nestedUnique ||
      !nestedShared)
    return;
  const std::array hierarchyRecipes{
      std::pair<std::string_view, const MirDropType *>{"direct unique",
                                                       &*directUnique},
      std::pair<std::string_view, const MirDropType *>{"nested unique",
                                                       &*nestedUnique},
      std::pair<std::string_view, const MirDropType *>{"direct shared",
                                                       &*directShared},
      std::pair<std::string_view, const MirDropType *>{"nested shared",
                                                       &*nestedShared}};
  const auto containsDynamicCleanup = [&](const auto &self,
                                          const MirDropType &recipe) -> bool {
    return recipe.polymorphicCleanup ||
           std::any_of(recipe.aggregate.begin(), recipe.aggregate.end(),
                       [&](const auto &child) { return self(self, child); });
  };
  expect(std::none_of(hierarchyRecipes.begin(), hierarchyRecipes.end(),
                      [&](const auto &entry) {
                        return containsDynamicCleanup(containsDynamicCleanup,
                                                      *entry.second);
                      }),
         "static hierarchy-containing owners have no dynamic-cleanup marker");
  for (const auto &[name, recipe] : hierarchyRecipes) {
    auto *cleanup = hierarchyOwnerLifetimes.cleanup(*recipe);
    expect(cleanup && !llvm::verifyFunction(*cleanup),
           std::string(name) +
               " emits its exact static hierarchy-containing cleanup");
    const auto publication = modulePublication(hierarchyOwnerSession);
    expect(hierarchyOwnerLifetimes.cleanup(*recipe) == cleanup &&
               modulePublication(hierarchyOwnerSession) == publication,
           std::string(name) + " reuses only the exact validated recipe");
    // Freeing a passive pointee does not replace any live polymorphic object.
    expect(hierarchyOwnerLifetimes.cleanup(*recipe, true) == cleanup &&
               modulePublication(hierarchyOwnerSession) == publication,
           std::string(name) + " permits passive owner-handle replacement");
    auto forged = *recipe;
    forged.polymorphicCleanup = true;
    rejects<std::logic_error>(
        [&] { static_cast<void>(hierarchyOwnerLifetimes.cleanup(forged)); },
        "a warmed static cleanup cannot acquire dynamic cleanup authority");
    expect(modulePublication(hierarchyOwnerSession) == publication,
           "forged owner cleanup rejects before publication");
  }
  const auto &activeValue =
      named(hierarchyOwnerSession.program(), "ActiveHierarchyValue");
  const auto activeUnique =
      mirUniqueCleanupType(hierarchyOwnerSession.program().mir(),
                           SemanticType::uniqueOwnerOf(activeValue.type));
  expect(activeUnique && !activeUnique->aggregate.empty(),
         "an active hierarchy-containing pointee retains its child recipe");
  if (!activeUnique || activeUnique->aggregate.empty())
    return;
  const auto beforeActive = modulePublication(hierarchyOwnerSession);
  const auto activeInline = mirUniqueCleanupType(
      hierarchyOwnerSession.program().mir(), activeValue.type);
  expect(activeInline.has_value(),
         "inline active hierarchy retains an exact recipe");
  if (!activeInline)
    return;
  rejects<BackendUnsupportedError>(
      [&] {
        static_cast<void>(hierarchyOwnerLifetimes.cleanup(*activeInline, true));
      },
      "in-place hierarchy replacement retains its independent dispatch gate");
  expect(modulePublication(hierarchyOwnerSession) == beforeActive,
         "owned child preflight rejects before requesting a destructor body "
         "or publishing a helper");
  const auto dynamicOwner = mirUniqueCleanupType(
      hierarchyOwnerSession.program().mir(),
      SemanticType::uniqueOwnerOf(
          named(hierarchyOwnerSession.program(), "Derived").type));
  expect(dynamicOwner && dynamicOwner->polymorphicCleanup,
         "a genuinely polymorphic pointee retains dynamic cleanup authority");
  if (!dynamicOwner)
    return;
  // The owner's static pointee names only a view. Its helper recovers the
  // complete object and cleanup from one descriptor load, calls that cleanup
  // indirectly and releases the complete allocation, never a static recipe.
  auto *dynamicCleanup = hierarchyOwnerLifetimes.cleanup(*dynamicOwner);
  expect(dynamicCleanup && !llvm::verifyFunction(*dynamicCleanup),
         "dynamic owner cleanup emits a valid descriptor-selected helper");
  std::size_t indirect = 0, direct = 0;
  for (const auto &block : *dynamicCleanup)
    for (const auto &instruction : block)
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
        if (call->isIndirectCall())
          ++indirect;
        else if (const auto *callee = call->getCalledFunction();
                 callee && callee->getName().starts_with("__gti_native_"))
          ++direct;
      }
  expect(indirect == 1 && direct == 0,
         "dynamic owner cleanup selects exactly one descriptor cleanup and "
         "no statically named pointee recipe");
  const auto afterDynamic = modulePublication(hierarchyOwnerSession);
  expect(hierarchyOwnerLifetimes.cleanup(*dynamicOwner) == dynamicCleanup &&
             modulePublication(hierarchyOwnerSession) == afterDynamic,
         "dynamic owner cleanup reuses its exact validated helper");
  auto forgedDynamic = *dynamicOwner;
  forgedDynamic.aggregate.push_back(*activeInline);
  rejects<std::logic_error>(
      [&] {
        static_cast<void>(hierarchyOwnerLifetimes.cleanup(forgedDynamic));
      },
      "a dynamic owner cannot also carry a static pointee schedule");
  expect(!hierarchyOwnerDispatch.dynamicallyOwned(
             named(hierarchyOwnerSession.program(), "Derived").id),
         "a never-allocated polymorphic class publishes no complete cleanup");
  hierarchyOwnerSession.verify();

  TargetInfo privateTarget = TargetInfo::host();
  privateTarget.executionProfile = ExecutionProfile::Concurrent;
  privateTarget.runtimeCapabilities.threads = true;
  FrontendOptions privateOptions;
  privateOptions.target = privateTarget;
  const auto privateFrontend =
      Frontend(std::move(privateOptions))
          .analyze("llvm-private-cleanup-fallback.gti", R"(
import std.mutex;
int main() { std::mutex<int32_t> value{1}; return 0; }
)",
                   {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(privateFrontend.canGenerateCode(),
         "private cleanup fallback source verifies");
  if (!privateFrontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> privateIssues;
  const auto privateInput = verifiedLoweredProgram(
      gti_test::lowerProgram(privateFrontend, privateFrontend.mir,
                             privateFrontend.mir, privateTarget),
      privateIssues);
  if (!privateInput)
    throw std::runtime_error("private cleanup fallback source seal failed");
  Session privateSession(*privateInput, {});
  auto privateDispatch = DispatchStructuralTestAccess::inspect(privateSession);
  Failures privateFailures(privateSession);
  Lifetimes privateLifetimes(
      privateSession, privateFailures, privateDispatch,
      [](MirBodyAddress) -> llvm::Function * {
        throw std::runtime_error(
            "rejected private cleanup unexpectedly requested a source body");
      });
  const MirClassInstance *privateOwner = nullptr;
  for (const auto &candidate : privateSession.program().mir().classInstances())
    if (candidate.compilerCapability ==
        CompilerCapabilityTypeKind::MutexStorage) {
      privateOwner = &candidate;
      break;
    }
  // O-LLVM-13 implemented private mutex storage cleanup, so this family is no
  // longer an example of an unavailable one. The canonical recipe is now the
  // authority: the emitter must consume exactly that recipe, reuse one helper,
  // and still refuse a recipe that does not match the sealed graph.
  expect(privateOwner != nullptr, "private mutex storage instance exists");
  if (!privateOwner)
    return;
  const auto canonical =
      mirUniqueCleanupType(privateSession.program().mir(), privateOwner->type);
  expect(canonical.has_value(),
         "private mutex storage has an implemented canonical cleanup family");
  if (!canonical)
    return;
  auto *privateHelper = privateLifetimes.cleanup(*canonical);
  expect(privateHelper != nullptr,
         "the canonical private cleanup family emits its helper");
  const auto afterFirst = modulePublication(privateSession);
  expect(privateLifetimes.cleanup(*canonical) == privateHelper,
         "a repeated canonical private cleanup reuses one helper");
  expect(modulePublication(privateSession) == afterFirst,
         "a repeated canonical private cleanup publishes no second artifact");
  // A forged active-cleanup recipe for the same instance does not match the
  // sealed graph and must fail closed without publishing anything.
  const MirDropType forged{.type = privateOwner->type,
                           .classInstance = privateOwner->id,
                           .requiresActiveCleanup = true};
  if (forged != *canonical) {
    for (int attempt = 0; attempt != 2; ++attempt)
      rejects<std::exception>(
          [&] { static_cast<void>(privateLifetimes.cleanup(forged)); },
          "a forged private cleanup recipe fails closed");
    expect(modulePublication(privateSession) == afterFirst,
           "a forged private cleanup recipe publishes no artifact");
  }
}

void testDispatchAdmissionBinding() {
  const auto frontend = Frontend().analyze("llvm-dispatch-demand.gti", R"(
class Active { public: virtual int read() { return 7; }  public: Active() = default; };
class Dormant : public Active { public: virtual int extra() { return 8; }  public: Dormant() = default; };
void dormant(Dormant& value) {}
int main() { Active value{}; return 0; }
)");
  expect(frontend.canGenerateCode(), "no-call dispatch demand source verifies");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("no-call dispatch demand source seal failed");
  const auto admission = planLlvmAdmission(*input);
  expect(admission.admitted() &&
             admission.closure() ==
                 LlvmAdmissionClosure::SealedEntryDependencies,
         "checked no-call construction admits its exact state without forcing "
         "unreachable bodies: " +
             admission.limitation());
  Session session(*input, {});
  Dispatch dispatch(session, admission);
  const auto &active = named(session.program(), "Active");
  const auto &dormant = named(session.program(), "Dormant");
  const auto functions = session.module().size();
  const auto globals = session.module().global_size();
  for (int attempt = 0; attempt != 2; ++attempt) {
    dispatch.preflightState(active.id);
    rejects<BackendUnsupportedError>(
        [&] { dispatch.preflightState(dormant.id); },
        "state preflight requires every selected source body in the actual "
        "admission plan");
  }
  rejects<std::logic_error>(
      [&] {
        dispatch.preflightState(std::numeric_limits<HirClassInstanceId>::max());
      },
      "an unsealed active-state identity cannot enter preflight");
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(dispatch.table(dormant.id, dormant.id)); },
      "a real plan excluding a dormant implementation rejects the complete "
      "table demand, even when another selected body is already required");
  rejects<BackendUnsupportedError>(
      [&] {
        static_cast<void>(dispatch.adjustment(
            dormant.id, dormant.id, dormant.virtualDispatch.front().root));
      },
      "an unrequired active class cannot bypass admission through direct "
      "adjustment publication");
  const auto wrongOptions = planLlvmAdmission(
      *input, {.optimization = LlvmCodegenOptimization::Aggressive});
  rejects<std::invalid_argument>(
      [&] { Dispatch mismatched(session, wrongOptions); },
      "a different options identity cannot bind checked dispatch publication");
  const auto wrongDebugOptions = planLlvmAdmission(*input, {.debugInfo = true});
  rejects<std::invalid_argument>(
      [&] { Dispatch mismatched(session, wrongDebugOptions); },
      "debug options cannot be normalized when binding dispatch preflight");
  const auto otherInput = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!otherInput)
    throw std::runtime_error("no-call dispatch test lost its second snapshot");
  const auto otherPlan = planLlvmAdmission(*otherInput);
  rejects<std::invalid_argument>(
      [&] { Dispatch mismatched(session, otherPlan); },
      "equal source text is not the actual immutable admission snapshot");
  expect(session.module().size() == functions &&
             session.module().global_size() == globals,
         "all missing-target/identity controls reject before any function, "
         "thunk or table publication");
  // Private complete-body inspection with the actual unchanged plan. There
  // are no source virtual calls to trigger the historical all-body fallback.
  lowerScalarProgram(session, admission);
  auto *implementation =
      session.abi().declareFunction(active.virtualDispatch.front().target);
  expect(!implementation->empty(),
         "no-call construction defines its checked source table target through "
         "the normal body worklist, not emit-on-demand");
  session.verify();
}

void testReferencePublication() {
  const auto frontend = Frontend().analyze("llvm-reference-publication.gti", R"(
interface Front { int front(); };
interface Reader { int read(int extra); };
class Root {
  int value;
public:
  Root(int input) : value(input) {}
  virtual int phase() { return 1; }
  int base_value() { return this.value; }
};
class Middle : public Reader, public Root {
  int bias;
public:
  Middle(int input) : Root(input), bias(3) {}
  int read(int extra) override {
    return this.base_value() + this.bias + extra;
  }
};
class Leaf : public Front, public Middle {
  int tag;
public:
  Leaf(int input) : Middle(input), tag(9) {}
  int front() override { return this.tag; }
};
int via_reader(Reader& value, int extra) { return value.read(extra); }
int via_root(Root& value) { return value.base_value(); }
int main() {
  Leaf value{7};
  Reader& view = value;
  Reader& again = view;
  Root& state = value;
  return via_reader(view, 1) + via_reader(again, 2) + via_root(state) - 30;
}
)");
  expect(frontend.canGenerateCode(),
         "source interface/base aliases verify without ownership transport");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("reference publication source seal failed");
  const auto admission = planLlvmAdmission(*input);
  expect(admission.admitted(),
         "exact interface/base alias publication is admitted: " +
             admission.limitation());
  if (!admission.admitted())
    return;
  Session session(*input, {});
  const auto &leaf = named(session.program(), "Leaf");
  const auto &reader = named(session.program(), "Reader");
  const auto &root = named(session.program(), "Root");
  const auto readerPath =
      session.layouts().baseSubobject(leaf.type, reader.type);
  const auto rootPath = session.layouts().baseSubobject(leaf.type, root.type);
  expect(readerPath.offset != 0 && rootPath.offset != 0 &&
             readerPath.offset != rootPath.offset,
         "alias fixture distinguishes nonzero interface and state-base paths");
  const auto sourceFunction = [&](std::string_view name) {
    for (const auto &function : session.program().mir().functionInstances()) {
      const auto *declaration =
          session.program().findFunctionDeclaration(function.declaration);
      if (declaration && declaration->qualifiedName == name)
        return &function;
    }
    throw std::runtime_error("reference publication lost its source function");
  };
  const auto &main = *sourceFunction("main");
  std::size_t leafBorrows = 0, selectedBorrows = 0,
              interfaceInitializations = 0, baseInitializations = 0,
              selectedInitializations = 0;
  for (const auto &block : main.body.blocks)
    for (const auto &stage : block.instructions) {
      if (stage.tombstone)
        continue;
      if (stage.kind == MirInstructionKind::Borrow && stage.loan &&
          stage.info.type == leaf.type)
        ++leafBorrows;
      if (stage.kind == MirInstructionKind::Borrow && stage.loan &&
          stage.info.type == reader.type)
        ++selectedBorrows;
      if (stage.kind != MirInstructionKind::Initialize || !stage.destination ||
          stage.operands.size() != 1)
        continue;
      const auto &place = *main.body.findPlace(*stage.destination);
      if (place.type.kind != SemanticType::Reference)
        continue;
      const auto &operand = stage.operands.front();
      if (operand.kind != MirOperandKind::Loan || !operand.place ||
          !operand.loan)
        continue;
      interfaceInitializations += place.type.arguments.front() == reader.type &&
                                  operand.type == leaf.type;
      baseInitializations += place.type.arguments.front() == root.type &&
                             operand.type == leaf.type;
      selectedInitializations += place.type.arguments.front() == reader.type &&
                                 operand.type == reader.type;
    }
  expect(leafBorrows != 0 && interfaceInitializations == 1 &&
             baseInitializations == 1 && selectedInitializations == 1,
         "real MIR retains source-typed provenance separately from each alias "
         "destination presentation");
  lowerScalarProgram(session, admission);
  auto *lowered = session.abi().declareFunction(main.id);
  llvm::AllocaInst *complete = nullptr;
  std::vector<const llvm::StoreInst *> stores;
  for (auto &block : *lowered)
    for (auto &operation : block) {
      if (auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&operation);
          allocation && allocation->getAllocatedType() ==
                            session.layouts().get(leaf.type).storage) {
        expect(!complete, "alias fixture has one complete Leaf allocation");
        complete = allocation;
      }
      if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&operation))
        stores.push_back(store);
    }
  expect(complete != nullptr, "full Body lowering retains fresh Leaf storage");
  if (!complete)
    return;
  std::set<const llvm::Value *> readerSlots, rootSlots;
  std::size_t completePublications = 0;
  for (const auto *store : stores) {
    const auto *destination =
        llvm::dyn_cast<llvm::AllocaInst>(store->getPointerOperand());
    const auto *value = store->getValueOperand();
    if (!destination || !destination->getAllocatedType()->isPointerTy())
      continue;
    if (value == complete) {
      ++completePublications;
      continue;
    }
    const auto *base = value;
    while (const auto *projection = llvm::dyn_cast<llvm::GEPOperator>(base))
      base = projection->getPointerOperand();
    if (base != complete)
      continue;
    const auto offset = offsetFrom(session, value, complete);
    if (offset == readerPath.offset)
      readerSlots.insert(destination);
    if (offset == rootPath.offset)
      rootSlots.insert(destination);
  }
  expect(completePublications == leafBorrows,
         "original Leaf loans publish the complete address at offset zero");
  expect(readerSlots.size() == 1 && rootSlots.size() == 1,
         "reference initialization stores each computed physical target view, "
         "not its complete-object loan address");
  std::size_t selectedCopies = 0;
  for (const auto *store : stores)
    if (const auto *loaded =
            llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand());
        loaded && readerSlots.contains(loaded->getPointerOperand())) {
      const auto *destination =
          llvm::dyn_cast<llvm::AllocaInst>(store->getPointerOperand());
      if (destination && destination->getAllocatedType()->isPointerTy()) {
        readerSlots.insert(destination);
        ++selectedCopies;
      }
    }
  expect(selectedCopies == selectedBorrows + selectedInitializations &&
             readerSlots.size() == 1 + selectedCopies,
         "an already-selected Reader alias is copied from its slot without "
         "another base adjustment");
  std::size_t readerCalls = 0, rootCalls = 0;
  for (const std::string_view name : {"via_reader", "via_root"}) {
    const auto &target = *sourceFunction(name);
    const auto signature = session.abi().gti(
        target.returnType, target.parameterTypes, target.mayRaiseDefinedFailure,
        false, target.tracksCaller);
    const auto argument =
        std::find_if(signature.parameters.begin(), signature.parameters.end(),
                     [](const auto &parameter) {
                       return parameter.channel == AbiChannel::Argument &&
                              parameter.sourceIndex == 0;
                     });
    if (argument == signature.parameters.end())
      throw std::runtime_error("alias call lost its reference ABI argument");
    for (const auto &block : *lowered)
      for (const auto &operation : block)
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation);
            call && call->getCalledFunction() ==
                        session.abi().declareFunction(target.id)) {
          const auto *loaded = llvm::dyn_cast<llvm::LoadInst>(
              call->getArgOperand(argument - signature.parameters.begin()));
          const bool readerCall = name == "via_reader";
          expect(loaded && (readerCall ? readerSlots : rootSlots)
                               .contains(loaded->getPointerOperand()),
                 "downstream alias call loads its selected physical view "
                 "without a second upcast");
          readerCalls += readerCall;
          rootCalls += !readerCall;
        }
  }
  expect(
      readerCalls == 2 && rootCalls == 1,
      "actual full Body IR checks both interface aliases and the state base");
  session.verify();
}

void testOrdinaryTransferPlacement() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend = Frontend().analyze(
      root / "tests/fixtures/llvm_class_fresh_transfer_regression.gti",
      std::nullopt, {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "ordinary copy/move placement regression remains valid source");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("ordinary transfer source failed its exact seal");
  std::size_t copies = 0, moves = 0;
  const auto &program = input->program().mir();
  for (const auto &function : program.functionInstances()) {
    if (function.entryKind == ProgramEntryKind::None)
      continue;
    const Placement placement(function.body);
    const Placement hierarchyAware(program, function.body);
    const auto foreignPlain = function.body;
    rejects<std::invalid_argument>(
        [&] { static_cast<void>(Placement(program, foreignPlain)); },
        "program-aware placement rejects a foreign ordinary body before "
        "filtering");
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone || !stage.result || stage.operands.size() != 1 ||
            stage.info.type.kind != SemanticType::Class)
          continue;
        const bool copy = stage.kind == MirInstructionKind::Load &&
                          stage.operands.front().kind == MirOperandKind::Copy;
        const bool move = stage.kind == MirInstructionKind::Move &&
                          stage.operands.front().kind == MirOperandKind::Move;
        if (!copy && !move)
          continue;
        expect(!containsHierarchyStorage(program, stage.info.type) &&
                   !placement.fresh(*stage.result) &&
                   !placement.target(*stage.result) &&
                   !hierarchyAware.fresh(*stage.result) &&
                   !hierarchyAware.target(*stage.result),
               "ordinary Load/Move snapshot remains unbound until Initialize "
               "commits its value");
        copies += copy;
        moves += move;
      }
  }
  expect(copies == 1 && moves == 3,
         "ordinary regression retains one implicit Copy Load and three "
         "whole-local Move producers");
}

void testFreshConstructionPlanning() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend =
      Frontend({.requireEntryPoint = true,
                .lowerReachableOnly = true,
                .lowerLibraryReachableOnly = true,
                .trustedBodies = TrustedBodyAnalysis::Reachable})
          .analyze(root / "tests/fixtures/llvm_dispatch_inline_fields.gti",
                   std::nullopt, {root / "stdlib/prelude.gti"}, {},
                   {root / "stdlib"});
  expect(frontend.canGenerateCode(), "inline hierarchy fixture verifies");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("inline hierarchy fixture lost its seal");
  const auto admission = planLlvmAdmission(*input);
  expect(admission.admitted(),
         "inline hierarchy fixture has public admission: " +
             admission.limitation());
  const auto &program = admission.program().mir();
  std::size_t locals = 0, fields = 0, initializers = 0;
  // The planner also serves declaration initializer bodies. Exact dispatch
  // reachability need not emit those after constructor expansion.
  for (const auto &row : input->program().bodies()) {
    const auto address = row.identity.address;
    const auto &body = *findMirBody(program, address);
    const Placement placement(body);
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        const auto destination = freshConstructionDestination(
            program, address, body, placement, stage);
        if (!destination || !containsHierarchyStorage(program, stage.info.type))
          continue;
        const auto &place = *body.findPlace(*destination);
        locals += address.kind == MirBodyKind::Function;
        fields += place.root == MirPlaceRootKind::This;
        initializers += address.kind == MirBodyKind::FieldInitializers;
        const auto copiedStage = stage;
        expect(!freshConstructionDestination(program, address, body, placement,
                                             copiedStage),
               "copied instruction cannot acquire fresh construction identity");
        const auto copiedBody = body;
        expect(!freshConstructionDestination(program, address, copiedBody,
                                             Placement(copiedBody), stage),
               "foreign body cannot acquire a constructor destination");
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto changed = program;
          auto &changedBody = *findMirBody(changed, address);
          MirInstruction *producer = nullptr;
          MirInstruction *publication = nullptr;
          for (auto &candidateBlock : changedBody.blocks)
            for (auto &candidate : candidateBlock.instructions) {
              if (candidate.id == stage.id)
                producer = &candidate;
              if (candidate.kind == MirInstructionKind::Initialize &&
                  candidate.destination == destination &&
                  candidate.operands.size() == 1 &&
                  candidate.operands.front().kind == MirOperandKind::Value &&
                  candidate.operands.front().value == *stage.result)
                publication = &candidate;
            }
          if (!producer || !publication)
            throw std::runtime_error(
                "fresh construction mutation lost its pair");
          auto &changedPlace = changedBody.places.at(*destination - 1);
          if (mutation == 0)
            changedPlace.root = MirPlaceRootKind::Symbol;
          if (mutation == 1)
            changedPlace.foreignStorage = true;
          if (mutation == 2)
            changedPlace.type = SemanticType::Bool;
          if (mutation == 3)
            producer->constructorKind = ConstructorKind::Copy;
          if (mutation == 4)
            publication->tombstone = true;
          if (mutation == 5) {
            if (place.root == MirPlaceRootKind::This)
              publication->constructorInitializer = 0;
            else
              changedPlace.projections.push_back(
                  {.kind = MirProjectionKind::Field, .field = 1});
          }
          expect(!freshConstructionDestination(changed, address, changedBody,
                                               Placement(changedBody),
                                               *producer),
                 "nonlocal, foreign, mistyped, copy, missing or mismatched "
                 "publication rejects");
        }
      }
  }
  expect(locals && fields && initializers,
         "real MIR covers whole locals, constructor fields and default field "
         "initializers");
}

void testFreshTransferPlanning() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  // Match the artifact driver; raw-unit tests deliberately retain complete
  // trusted declaration inventories elsewhere.
  const auto frontend =
      Frontend({.requireEntryPoint = true,
                .lowerReachableOnly = true,
                .lowerLibraryReachableOnly = true,
                .trustedBodies = TrustedBodyAnalysis::Reachable})
          .analyze(root / "tests/fixtures/llvm_dispatch_fresh_transfer.gti",
                   std::nullopt, {root / "stdlib/prelude.gti"}, {},
                   {root / "stdlib"});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "fresh-transfer fixture verifies as source");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("fresh-transfer fixture failed its exact seal");
  const auto admission = planLlvmAdmission(*input);
  expect(std::none_of(admission.requirements().begin(),
                      admission.requirements().end(),
                      [](const auto &use) {
                        return use.required && !use.body &&
                               use.entry->key() == "SemanticType::Kind.Class" &&
                               !use.implementedShape;
                      }),
         "artifact-profile source does not require dormant raw class metadata");
  expect(admission.admitted(),
         "the bounded fresh hierarchy fixture has exact public admission: " +
             admission.limitation());
  Session session(*input, {});
  Dispatch dispatch(session, admission);
  const auto &program = session.program().mir();
  const auto main =
      std::find_if(program.functionInstances().begin(),
                   program.functionInstances().end(), [](const auto &function) {
                     return function.entryKind != ProgramEntryKind::None;
                   });
  if (main == program.functionInstances().end())
    throw std::runtime_error("fresh-transfer fixture lost its entry");
  const auto &body = main->body;
  const Placement placement(body);
  const Placement hierarchyAware(program, body);
  std::vector<std::pair<const MirInstruction *, FreshHierarchyTransfer>> plans;
  std::size_t copyConstructs = 0, copyLoads = 0, moves = 0;
  const auto cold = modulePublication(session);
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions) {
      const bool candidate = isFreshHierarchyTransferProducer(stage);
      if (!stage.tombstone && stage.result &&
          stage.info.type.kind == SemanticType::Class &&
          (stage.kind == MirInstructionKind::Load ||
           stage.kind == MirInstructionKind::Move ||
           (stage.kind == MirInstructionKind::Construct &&
            !stage.constructorTarget))) {
        std::cout << "fresh-transfer-stage: bb" << block.id << ":i" << stage.id
                  << " kind=" << static_cast<int>(stage.kind)
                  << " intrinsic=" << static_cast<int>(stage.intrinsic)
                  << " ctor=" << static_cast<int>(stage.constructorKind)
                  << " result=" << *stage.result << " candidate=" << candidate;
        if (!stage.operands.empty())
          std::cout << " source-kind="
                    << static_cast<int>(stage.operands.front().kind)
                    << " source-place=" << stage.operands.front().place
                    << " source-loan=" << stage.operands.front().loan;
        std::cout << '\n';
      }
      if (!candidate)
        continue;
      const auto planned = planFreshHierarchyTransfer(program, body, stage);
      const auto preflight =
          Lifetimes::preflightFreshTransfer(session, dispatch, body, stage);
      std::cout << "fresh-transfer-plan: i" << stage.id
                << " owner=" << planned.owner
                << " kind=" << static_cast<int>(planned.kind)
                << " destination=" << planned.destination << " publication=i"
                << planned.publication->id
                << " source-drop=" << planned.sourceDrop
                << " value-drop=" << planned.valueDrop << '\n';
      expect(planned.source == stage.operands.front() &&
                 preflight.source == planned.source &&
                 preflight.publication == planned.publication &&
                 preflight.owner == planned.owner && planned.publication &&
                 planned.publication->destination == planned.destination &&
                 planned.publication->operands.front().value == *stage.result,
             "pure transfer plan binds the exact producer/publication");
      expect(hierarchyAware.fresh(*stage.result) &&
                 hierarchyAware.target(*stage.result) &&
                 hierarchyAware.target(*stage.result)->place ==
                     planned.destination,
             "explicit program-aware placement binds only the proven hierarchy "
             "local");
      // The body-only API must retain generic snapshot placement. Hierarchy
      // final-local transport requires the explicit program-aware opt-in and
      // matching typed emission; candidate recognition alone is not permission.
      if (stage.kind == MirInstructionKind::Construct)
        expect(
            placement.fresh(*stage.result) && placement.target(*stage.result) &&
                placement.target(*stage.result)->place == planned.destination,
            "existing Construct placement remains unchanged");
      else
        expect(!placement.fresh(*stage.result) &&
                   !placement.target(*stage.result),
               "body-only placement does not opt into hierarchy Load/Move "
               "transport");
      if (planned.kind == ConstructorKind::Move) {
        ++moves;
        expect(planned.sourceDrop && planned.valueDrop && stage.ownership &&
                   stage.ownership->kind == OwnershipEventKind::Move &&
                   stage.lifecycle.front().source == planned.sourceDrop &&
                   stage.lifecycle.front().target == planned.valueDrop &&
                   body.findDropObligation(planned.sourceDrop)->place ==
                       planned.source.place &&
                   planned.publication->lifecycle.front().source ==
                       planned.valueDrop,
               "whole-local movement retains exact source/value drop IDs");
      } else {
        copyConstructs += stage.kind == MirInstructionKind::Construct;
        copyLoads += stage.kind == MirInstructionKind::Load;
        expect(!planned.sourceDrop && !planned.valueDrop &&
                   stage.lifecycle.empty(),
               "scalar hierarchy copies leave source cleanup unchanged");
      }
      plans.emplace_back(&stage, planned);
    }
  std::cout << "fresh-transfer-counts: copy-construct=" << copyConstructs
            << " copy-load=" << copyLoads << " move=" << moves << '\n';
  expect(copyConstructs == 2 && copyLoads == 1 && moves == 2,
         "real MIR covers generated Copy, implicit Copy Load and both "
         "whole-local moves");
  expect(modulePublication(session) == cold,
         "cold transfer preflight publishes no functions/globals/IR/state");
  auto *sentinel = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::InternalLinkage, "transfer.preflight.sentinel",
      session.module());
  llvm::IRBuilder<> sentinelIr(
      llvm::BasicBlock::Create(session.context(), "entry", sentinel));
  auto *state = sentinelIr.CreateAlloca(sentinelIr.getInt8Ty());
  auto *marker = sentinelIr.CreateStore(sentinelIr.getInt8(7), state);
  const auto insertion = sentinelIr.GetInsertPoint();
  const auto warm = modulePublication(session);
  for (int attempt = 0; attempt != 2; ++attempt)
    for (const auto &[stage, plan] : plans) {
      (void)plan;
      static_cast<void>(
          Lifetimes::preflightFreshTransfer(session, dispatch, body, *stage));
    }
  expect(modulePublication(session) == warm &&
             sentinelIr.GetInsertPoint() == insertion &&
             marker->getValueOperand() == sentinelIr.getInt8(7) &&
             marker->getPointerOperand() == state,
         "warm preflight changes neither insertion point nor runtime state IR");
  sentinelIr.CreateRetVoid();
  if (plans.empty())
    return;
  const auto &[first, firstPlan] = plans.front();
  auto foreignBody = body;
  rejects<std::invalid_argument>(
      [&] {
        static_cast<void>(planFreshHierarchyTransfer(
            program, foreignBody,
            foreignBody.blocks.front().instructions.front()));
      },
      "equal copied MIR body is not the program-owned body");
  rejects<std::invalid_argument>(
      [&] { static_cast<void>(Placement(program, foreignBody)); },
      "program-aware placement rejects a foreign hierarchy body upfront");
  const auto foreignStage = *first;
  rejects<std::invalid_argument>(
      [&] {
        static_cast<void>(
            planFreshHierarchyTransfer(program, body, foreignStage));
      },
      "equal copied transfer stage is not its exact program-owned stage");
  for (int mutation : {0, 1, 2}) {
    auto changed = program;
    auto &changedBody =
        *findMirBody(changed, {MirBodyKind::Function, main->id});
    const auto selected =
        std::find_if(plans.begin(), plans.end(), [&](const auto &row) {
          return mutation == 0 ? row.first->kind == MirInstructionKind::Load
                               : row.second.kind == ConstructorKind::Move;
        });
    if (selected == plans.end())
      throw std::runtime_error("negative transfer control lost its producer");
    MirInstruction *stage = nullptr;
    for (auto &block : changedBody.blocks)
      for (auto &candidate : block.instructions)
        if (candidate.id == selected->first->id)
          stage = &candidate;
    if (!stage)
      throw std::runtime_error("negative transfer control lost its stage");
    if (mutation == 0)
      changedBody.blocks.front().terminator.value =
          MirOperand{.kind = MirOperandKind::Value,
                     .value = *stage->result,
                     .type = stage->info.type};
    if (mutation == 1)
      changedBody.places.at(selected->second.destination - 1).root =
          MirPlaceRootKind::Symbol;
    if (mutation == 2)
      changedBody.places.at(stage->operands.front().place - 1)
          .projections.push_back(
              {.kind = MirProjectionKind::Base,
               .selection = named(session.program(), "MoveRoot").id});
    rejects<BackendUnsupportedError>(
        [&] {
          static_cast<void>(
              planFreshHierarchyTransfer(changed, changedBody, *stage));
        },
        "alternate/nonlocal/base-view move cannot acquire a fresh plan");
    const Placement rejected(changed, changedBody);
    expect(!rejected.fresh(*stage->result) && !rejected.target(*stage->result),
           "failed hierarchy Load/Move proof remains unbound under explicit "
           "opt-in");
  }
  const auto staged =
      Frontend().analyze("llvm-staged-move-exclusion.gti", std::string(R"(
mut int drops = 0;
class Value {
public:
  Value() {}
  virtual int read() { return 1; }
  ~Value() { drops = 1; }
};
int main() {
  Value first{};
  Value second{std::move(first)};
  return second.read() - 1;
}
)"),
                         {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!staged.canGenerateCode())
    for (const auto &diagnostic : staged.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(staged.canGenerateCode(),
         "explicit staged move remains valid source outside the fresh profile");
  if (staged.canGenerateCode()) {
    const auto stagedInput = verifiedLoweredProgram(
        gti_test::lowerProgram(staged, staged.mir, staged.mir), issues);
    if (!stagedInput)
      throw std::runtime_error(
          "staged move exclusion lost its exact source seal");
    bool found = false;
    const auto &stagedMir = stagedInput->program().mir();
    for (const auto address : enumerateMirBodyAddresses(stagedMir)) {
      const auto &stagedBody = *findMirBody(stagedMir, address);
      for (const auto &block : stagedBody.blocks)
        for (const auto &stage : block.instructions)
          if (!stage.tombstone && stage.kind == MirInstructionKind::Construct &&
              !stage.constructorTarget &&
              stage.constructorKind == ConstructorKind::Move &&
              stage.operands.size() == 1 &&
              stage.operands.front().kind == MirOperandKind::Value) {
            found = true;
            std::cout << "fresh-transfer-excluded-stage: i" << stage.id
                      << " result=" << stage.result.value_or(0)
                      << " source-value=" << stage.operands.front().value
                      << '\n';
            rejects<BackendUnsupportedError>(
                [&] {
                  static_cast<void>(
                      planFreshHierarchyTransfer(stagedMir, stagedBody, stage));
                },
                "real Move-then-Move-Construct intermediate is not a fresh "
                "producer");
            const Placement rejected(stagedMir, stagedBody);
            expect(!rejected.fresh(stage.operands.front().value) &&
                       !rejected.target(stage.operands.front().value),
                   "staged intermediate Move remains unbound under explicit "
                   "opt-in");
          }
    }
    expect(found,
           "valid negative source retains its generated staged Move Construct");
  }
  // Emission below is isolated private representation IR. It defines no source
  // body, does not lower the public source program and cannot publish an
  // object.
  Failures failures(session);
  Lifetimes lifetimes(
      session, failures, dispatch, [](MirBodyAddress) -> llvm::Function * {
        throw std::runtime_error(
            "private transfer unexpectedly requests a source body");
      });
  for (const auto &[stage, plan] : plans) {
    const auto &owner = *program.findClassInstance(plan.owner);
    auto *probe = llvm::Function::Create(
        llvm::FunctionType::get(
            llvm::Type::getVoidTy(session.context()),
            {session.layouts().pointer(), session.layouts().pointer()}, false),
        llvm::GlobalValue::InternalLinkage, "transfer.private.probe",
        session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", probe));
    auto *destination = probe->getArg(0);
    auto *source = probe->getArg(1);
    lifetimes.freshTransfer(ir, destination, source, body, *stage);
    ir.CreateRetVoid();
    if (plan.kind == ConstructorKind::Move) {
      // The generated relocation oracle below independently executes field,
      // header and suppressed-active-state transfer. This planner must select
      // that same typed helper, not duplicate a second move implementation.
      unsigned calls = 0;
      for (const auto &block : *probe)
        for (const auto &operation : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation)) {
            ++calls;
            expect(call->getCalledFunction() ==
                           lifetimes.generatedRelocation(owner.type) &&
                       call->arg_size() == 2 &&
                       call->getArgOperand(0) == destination &&
                       call->getArgOperand(1) == source &&
                       call->getCalledFunction()->hasFnAttribute(
                           llvm::Attribute::NoUnwind),
                   "fresh move uses the exact typed non-failing relocation");
          }
      expect(calls == 1, "fresh move performs exactly one relocation");
      continue;
    }
    std::set<std::uint64_t> fields, flags, headers, copiedFields, movedFlags,
        installed;
    const auto collect = [&](const auto &self, const MirClassInstance &object,
                             std::uint64_t offset) -> void {
      const auto &layout = session.layouts().get(object.type);
      for (const auto &field : object.declaredFields)
        fields.insert(
            offset +
            session.layouts().fieldSubobject(object.type, field.symbol).offset);
      if (const auto flag = session.layouts().activeIndex(object.type))
        flags.insert(offset + layout.fieldOffsets.at(*flag));
      for (const auto &base : object.bases)
        self(
            self, *program.findClassInstance(base.instance),
            offset +
                session.layouts().baseSubobject(object.type, base.type).offset);
    };
    collect(collect, owner, 0);
    for (const auto &view : dispatch.views(owner.id))
      headers.insert(view.location.owner.offset +
                     session.layouts()
                         .get(view.location.owner.type)
                         .fieldOffsets.at(view.location.index));
    bool suppressed = false;
    for (const auto &block : *probe)
      for (const auto &operation : block) {
        if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&operation)) {
          expect(!suppressed,
                 "all destination writes precede source suppression");
          const auto offset =
              offsetFrom(session, store->getPointerOperand(), destination);
          if (const auto *load =
                  llvm::dyn_cast<llvm::LoadInst>(store->getValueOperand())) {
            expect(offsetFrom(session, load->getPointerOperand(), source) ==
                       offset,
                   "typed field/flag transfer uses equal sealed subobject "
                   "offsets");
            if (fields.contains(offset))
              copiedFields.insert(offset);
            else if (flags.contains(offset))
              movedFlags.insert(offset);
            else
              expect(
                  false,
                  "transfer never copies source dispatch headers or padding");
          } else if (const auto *table = llvm::dyn_cast<llvm::GlobalVariable>(
                         store->getValueOperand())) {
            installed.insert(offset);
            expect(
                table->getName().str().find("__gti_dispatch_table_" +
                                            std::to_string(owner.id) + "_") ==
                    0,
                "destination headers select static destination class tables");
          } else {
            expect(flags.contains(offset) && plan.kind == ConstructorKind::Copy,
                   "only a new copy-owned active flag may use constant arming");
          }
        }
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation)) {
          const auto *callee = call->getCalledFunction();
          expect(
              callee &&
                  callee->getName().str().find("__gti_native_move_state_") ==
                      0 &&
                  call->getArgOperand(0) == source && !suppressed,
              "no memcpy/header-copy call replaces exact source suppression");
          suppressed = true;
        }
      }
    expect(
        copiedFields == fields && installed == headers &&
            (plan.kind == ConstructorKind::Move
                 ? movedFlags == flags && suppressed
                 : movedFlags.empty() && !suppressed),
        "private transfer copies scalar payload, rebuilds metadata and moves "
        "actual flags before suppressing the whole source");
  }
  session.verify();
}

void testNestedFreshTransferPlanning() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend =
      Frontend({.requireEntryPoint = true,
                .lowerReachableOnly = true,
                .lowerLibraryReachableOnly = true,
                .trustedBodies = TrustedBodyAnalysis::Reachable})
          .analyze(root / "examples/llvm/nested-hierarchy-transfer.gti",
                   std::nullopt, {root / "stdlib/prelude.gti"}, {},
                   {root / "stdlib"});
  expect(frontend.canGenerateCode(), "nested transfers are valid GTI source");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("nested transfer fixture lost its sealed program");
  const auto admission = planLlvmAdmission(*input);
  expect(admission.admitted(), "nested generated transfers have public proof");
  Session session(*input, {});
  Dispatch dispatch(session, admission);
  const auto &program = session.program().mir();
  unsigned copies = 0, moves = 0;
  const auto before = modulePublication(session);
  for (const auto &function : program.functionInstances()) {
    if (function.entryKind == ProgramEntryKind::None)
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        if (!isFreshHierarchyTransferProducer(stage) ||
            !containsHierarchyStorage(program, stage.info.type))
          continue;
        const auto transfer = Lifetimes::preflightFreshTransfer(
            session, dispatch, function.body, stage);
        if (transfer.kind == ConstructorKind::Copy) {
          ++copies;
          continue;
        }
        ++moves;
        for (unsigned mutation = 0; mutation != 4; ++mutation) {
          auto changed = program;
          auto &field =
              const_cast<MirClassInstance &>(*changed.findClassInstance(
                  named(session.program(), "OwnedValue").id));
          auto &body =
              *findMirBody(changed, {MirBodyKind::Function, function.id});
          const MirInstruction *producer = nullptr;
          for (const auto &candidate : body.blocks)
            for (const auto &instruction : candidate.instructions)
              if (instruction.id == stage.id)
                producer = &instruction;
          if (!producer)
            throw std::runtime_error("nested mutation lost its producer");
          if (mutation == 0)
            field.moveConstructor = SpecialMemberStatus::Deleted;
          else if (mutation == 1)
            field.moveConstructor = SpecialMemberStatus::Declared;
          else if (mutation == 2)
            // Borrowed state alone no longer blocks a generated value move
            // (owner decision 2026-10-28); an immovable field still does.
            field.traits.movable = false;
          else {
            auto &drop = const_cast<MirDropObligation &>(
                *body.findDropObligation(transfer.sourceDrop));
            auto &children = drop.dropType.aggregate.at(0).aggregate;
            if (children.size() != 2)
              throw std::runtime_error("nested mutation lost its field recipe");
            children.pop_back();
          }
          const auto check = [&] {
            static_cast<void>(
                planFreshHierarchyTransfer(changed, body, *producer));
          };
          if (mutation == 3)
            rejects<std::logic_error>(
                check, "nested move rejects an omitted field cleanup recipe");
          else
            rejects<BackendUnsupportedError>(
                check, "outer generated move cannot bypass a field contract");
        }
      }
  }
  expect(copies == 1 && moves == 2,
         "nested copy and both moves were inspected");
  expect(modulePublication(session) == before,
         "nested transfer proofs publish no runtime helpers or tables");
}

void testFreshCopyViewPlanning() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend =
      Frontend({.requireEntryPoint = true,
                .lowerReachableOnly = true,
                .lowerLibraryReachableOnly = true,
                .trustedBodies = TrustedBodyAnalysis::Reachable})
          .analyze(root / "tests/fixtures/llvm_dispatch_fresh_copy_view.gti",
                   std::nullopt, {root / "stdlib/prelude.gti"}, {},
                   {root / "stdlib"});
  expect(frontend.canGenerateCode(), "copy-through-view retains valid source");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("copy-through-view lost its exact MIR seal");
  const auto &program = input->program().mir();
  bool selectedRoot = false;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto &body = *findMirBody(program, address);
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (!isFreshHierarchyTransferProducer(stage) ||
            stage.kind != MirInstructionKind::Construct)
          continue;
        const auto transfer = planFreshHierarchyTransfer(program, body, stage);
        const auto *loan = body.findLoan(transfer.source.loan);
        const auto *place = body.findPlace(transfer.source.place);
        selectedRoot |= loan && place && loan->source != place->id &&
                        place->type == named(input->program(), "Root").type &&
                        transfer.kind == ConstructorKind::Copy &&
                        !transfer.sourceDrop && !transfer.valueDrop;
      }
  }
  expect(selectedRoot,
         "generated copy consumes the evaluated static Root view while "
         "preserving its distinct Derived loan origin and source lifetime");
}

void testGeneratedEmpty() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend = Frontend().analyze(
      root / "tests/fixtures/llvm_dispatch_generated_empty.gti");
  expect(frontend.canGenerateCode(), "generated-empty source verifies");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("generated-empty source seal failed");
  Session session(*input, {});
  const auto admission = planLlvmAdmission(*input);
  expect(admission.admitted(),
         "generated-empty IR is admitted under its destructors' recorded "
         "lifecycle-dispatch transitions: " +
             admission.limitation());
  {
    // These calls need neither Failures nor an emitting Lifetimes instance.
    Dispatch checkedDispatch(session, admission);
    const auto &solo = named(session.program(), "Solo");
    const auto &impl = named(session.program(), "Impl");
    const auto &leaf = named(session.program(), "Leaf");
    const MirBody *consumer = nullptr;
    const MirInstruction *fresh = nullptr, *interfaces = nullptr;
    for (const auto &function : session.program().mir().functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Construct &&
              instruction.constructorKind == ConstructorKind::Ordinary &&
              !instruction.constructorTarget) {
            if (instruction.info.type == solo.type) {
              consumer = &function.body;
              fresh = &instruction;
            }
            if (instruction.info.type == impl.type)
              interfaces = &instruction;
          }
    const MirConstructorInstance *constructor = nullptr;
    const MirInstruction *base = nullptr;
    for (const auto &candidate : session.program().mir().constructorInstances())
      if (candidate.owner == leaf.id)
        for (const auto &block : candidate.body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.kind == MirInstructionKind::InitializeBase &&
                instruction.info.type == solo.type &&
                !instruction.constructorTarget) {
              constructor = &candidate;
              base = &instruction;
            }
    if (!consumer || !fresh || !interfaces || !constructor || !base)
      throw std::runtime_error(
          "generated-empty preflight test lost its exact operations");
    const auto otherInput = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
    if (!otherInput)
      throw std::runtime_error("empty preflight test lost its second seal");
    const auto otherPlan = planLlvmAdmission(*otherInput);
    Session otherSession(*otherInput, {});
    Dispatch otherDispatch(otherSession, otherPlan);
    const LlvmBackendOptions otherOptions{
        .optimization = LlvmCodegenOptimization::Aggressive};
    const auto optionsPlan = planLlvmAdmission(*input, otherOptions);
    Session optionsSession(*input, otherOptions);
    Dispatch optionsDispatch(optionsSession, optionsPlan);
    auto *probe = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                false),
        llvm::GlobalValue::InternalLinkage, "empty.preflight.probe",
        session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", probe));
    auto *state = ir.CreateAlloca(ir.getInt8Ty());
    auto *marker = ir.CreateStore(ir.getInt8(91), state);
    const auto unchanged = [&](const auto &action) {
      const auto publication = modulePublication(session);
      const auto otherPublication = modulePublication(otherSession);
      const auto optionsPublication = modulePublication(optionsSession);
      const auto *block = ir.GetInsertBlock();
      const auto point = ir.GetInsertPoint();
      action();
      expect(modulePublication(session) == publication &&
                 modulePublication(otherSession) == otherPublication &&
                 modulePublication(optionsSession) == optionsPublication &&
                 ir.GetInsertBlock() == block && ir.GetInsertPoint() == point &&
                 marker->getPointerOperand() == state &&
                 marker->getValueOperand() == ir.getInt8(91),
             "empty preflight changes no functions, globals, blocks, IR "
             "insertion or stored state in any session");
    };
    unchanged([&] {
      for (int attempt = 0; attempt != 2; ++attempt) {
        expect(&Lifetimes::preflightGeneratedEmpty(
                   session, checkedDispatch, *consumer, *fresh) == &solo &&
                   &Lifetimes::preflightGeneratedEmpty(session, checkedDispatch,
                                                       *consumer,
                                                       *interfaces) == &impl &&
                   &Lifetimes::preflightGeneratedEmpty(session, checkedDispatch,
                                                       constructor->body, *base,
                                                       constructor) == &solo,
               "non-emitting empty preflight retains exact local, interface "
               "and selected-base identities");
      }
    });
    auto badBody = *consumer;
    const MirInstruction *badInstruction = nullptr;
    for (const auto &block : badBody.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == fresh->id)
          badInstruction = &instruction;
    if (!badInstruction)
      throw std::runtime_error(
          "empty preflight test lost its copied operation");
    badBody.values.at(*fresh->result - 1).definition =
        std::numeric_limits<MirInstructionId>::max();
    auto scheduledBody = *consumer;
    MirInstruction *scheduledInstruction = nullptr;
    for (auto &block : scheduledBody.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == fresh->id)
          scheduledInstruction = &instruction;
    if (!scheduledInstruction)
      throw std::runtime_error(
          "empty preflight test lost its scheduled control");
    scheduledInstruction->info.type =
        named(session.program(), "Scheduled").type;
    scheduledBody.values.at(*fresh->result - 1).info.type =
        scheduledInstruction->info.type;
    auto copiedConstructor = *constructor;
    unchanged([&] {
      rejects<std::invalid_argument>(
          [&] {
            static_cast<void>(Lifetimes::preflightGeneratedEmpty(
                session, checkedDispatch, badBody, *badInstruction));
          },
          "a copied body with a bad result identity cannot acquire sealed "
          "empty-operation authority");
      rejects<std::invalid_argument>(
          [&] {
            static_cast<void>(Lifetimes::preflightGeneratedEmpty(
                session, checkedDispatch, scheduledBody,
                *scheduledInstruction));
          },
          "a copied body cannot replace the source schedule with empty "
          "initialization metadata");
      rejects<std::invalid_argument>(
          [&] {
            static_cast<void>(Lifetimes::preflightGeneratedEmpty(
                session, checkedDispatch, constructor->body, *base,
                &copiedConstructor));
          },
          "a copied constructor record cannot publish selected-base state");
    });
    unchanged([&] {
      rejects<std::invalid_argument>(
          [&] {
            static_cast<void>(Lifetimes::preflightGeneratedEmpty(
                session, otherDispatch, *consumer, *fresh));
          },
          "another sealed program with equal numeric IDs cannot validate "
          "empty preflight");
      rejects<std::invalid_argument>(
          [&] {
            static_cast<void>(Lifetimes::preflightGeneratedEmpty(
                session, optionsDispatch, *consumer, *fresh));
          },
          "another options session cannot validate empty preflight");
    });
    ir.CreateRetVoid();
  }
  // Inspect the unchanged rejected plan privately, never publish native O10
  // artifacts or claim execution coverage from these representation probes.
  lowerScalarProgram(session, admission);
  auto dispatch = DispatchStructuralTestAccess::inspect(session);
  Failures failureChannel(session);
  const auto nativeBody = [&](MirBodyAddress address) {
    auto *function = session.module().getFunction(
        "__gti_native_v1_body_" +
        std::to_string(static_cast<unsigned>(address.kind)) + "_" +
        std::to_string(address.owner));
    if (!function)
      throw std::runtime_error("generated-empty test lost its native body");
    return function;
  };
  Lifetimes lifetimes(session, failureChannel, dispatch, nativeBody);
  const auto &solo = named(session.program(), "Solo");
  const auto &impl = named(session.program(), "Impl");
  const auto &leaf = named(session.program(), "Leaf");
  const auto &functions = session.program().mir().functionInstances();
  const auto evaluate = std::find_if(
      functions.begin(), functions.end(), [&](const auto &function) {
        const auto *declaration =
            session.program().findFunctionDeclaration(function.declaration);
        return declaration && declaration->qualifiedName == "evaluate";
      });
  if (evaluate == functions.end())
    throw std::runtime_error("generated-empty test lost its source consumer");
  auto *native = session.abi().declareFunction(evaluate->id);
  const auto pointerRoot = [](const llvm::Value *address) {
    while (const auto *projection = llvm::dyn_cast<llvm::GEPOperator>(address))
      address = projection->getPointerOperand();
    return address;
  };
  const auto storedTable = [](const llvm::StoreInst &store) {
    return llvm::dyn_cast<llvm::GlobalVariable>(
        store.getValueOperand()->stripPointerCasts());
  };
  const auto emittedTable = [&](HirClassInstanceId active,
                                HirClassInstanceId viewClass) {
    const auto &views = dispatch.views(active);
    const auto view =
        std::find_if(views.begin(), views.end(), [&](const auto &row) {
          return row.viewClass == viewClass;
        });
    if (view == views.end())
      throw std::runtime_error("generated-empty test lost a selected view");
    const auto *table = session.module().getNamedGlobal(
        "__gti_dispatch_table_" + std::to_string(active) + "_" +
        std::to_string(view->profileOwner));
    if (!table)
      throw std::runtime_error("generated-empty test lost an emitted table");
    return table;
  };
  for (const auto *owner : {&solo, &impl}) {
    std::set<const llvm::GlobalVariable *> expected;
    for (const auto &view : dispatch.views(owner->id))
      expected.insert(emittedTable(owner->id, view.viewClass));
    const llvm::StoreInst *zero = nullptr, *armed = nullptr;
    std::set<const llvm::GlobalVariable *> installed;
    for (const auto &block : *native)
      for (const auto &operation : block)
        if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&operation)) {
          const auto *constant =
              llvm::dyn_cast<llvm::Constant>(store->getValueOperand());
          if (constant && constant->isNullValue() &&
              constant->getType() == session.layouts().get(owner->type).storage)
            zero = store;
          if (!zero || pointerRoot(store->getPointerOperand()) !=
                           pointerRoot(zero->getPointerOperand()))
            continue;
          if (const auto *table = storedTable(*store);
              expected.contains(table)) {
            expect(store->getParent() == zero->getParent() &&
                       zero->comesBefore(store),
                   "fresh empty storage is initialized before its tables");
            installed.insert(table);
          }
          if (const auto index = session.layouts().activeIndex(owner->type);
              index && constant && constant->getType()->isIntegerTy(8) &&
              llvm::cast<llvm::ConstantInt>(constant)->isOne()) {
            armed = store;
            expect(installed == expected,
                   "all exact empty-object tables precede own cleanup arming");
          }
        }
    expect(zero && installed == expected &&
               (armed != nullptr) ==
                   session.layouts().activeIndex(owner->type).has_value(),
           "fresh root and multiple-interface empty construction publish exact "
           "metadata without a fabricated constructor body");
    if (owner == &impl)
      expect(expected.size() == 3,
             "this slice preserves all three existing physical interface "
             "headers, without unreviewed pointer reuse");
  }
  const auto &constructors = session.program().mir().constructorInstances();
  const auto selected =
      std::find_if(constructors.begin(), constructors.end(),
                   [&](const auto &row) { return row.owner == leaf.id; });
  if (selected == constructors.end())
    throw std::runtime_error(
        "generated-empty test lost its derived constructor");
  auto *constructor = nativeBody({MirBodyKind::Constructor, selected->id});
  const auto abi =
      session.abi().gti(SemanticType::Void, selected->parameterTypes,
                        selected->mayRaiseDefinedFailure, true);
  const auto receiver = std::find_if(
      abi.parameters.begin(), abi.parameters.end(), [](const auto &channel) {
        return channel.channel == AbiChannel::Receiver;
      });
  auto *thisAddress = constructor->getArg(receiver - abi.parameters.begin());
  const auto ownOffset =
      session.dataLayout()
          .getStructLayout(llvm::cast<llvm::StructType>(
              session.layouts().get(leaf.type).storage))
          ->getElementOffset(*session.layouts().activeIndex(leaf.type));
  const llvm::StoreInst *baseTable = nullptr, *derivedTable = nullptr;
  const llvm::StoreInst *baseZero = nullptr, *baseArm = nullptr;
  const llvm::StoreInst *rollbackPublication = nullptr;
  const auto baseOffset =
      session.layouts().baseSubobject(leaf.type, solo.type).offset;
  const auto baseOwnOffset =
      session.dataLayout()
          .getStructLayout(llvm::cast<llvm::StructType>(
              session.layouts().get(solo.type).storage))
          ->getElementOffset(*session.layouts().activeIndex(solo.type));
  const auto &soloLocation = dispatch.views(solo.id).front().location;
  const auto baseTableOffset =
      baseOffset + soloLocation.owner.offset +
      session.dataLayout()
          .getStructLayout(llvm::cast<llvm::StructType>(
              session.layouts().get(soloLocation.owner.type).storage))
          ->getElementOffset(soloLocation.index);
  bool normalArm = false;
  for (const auto &block : *constructor)
    for (const auto &operation : block)
      if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&operation)) {
        if (storedTable(*store) == emittedTable(solo.id, solo.id))
          baseTable = store;
        if (storedTable(*store) == emittedTable(leaf.id, leaf.id))
          derivedTable = store;
        const auto *constant =
            llvm::dyn_cast<llvm::Constant>(store->getValueOperand());
        if (constant && constant->isNullValue() &&
            constant->getType() == session.layouts().get(solo.type).storage)
          baseZero = store;
        const auto *value =
            llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
        if (value && value->isOne() && value->getType()->isIntegerTy(1) &&
            llvm::isa<llvm::AllocaInst>(store->getPointerOperand()))
          rollbackPublication = store;
        if (value && value->isOne() && value->getType()->isIntegerTy(8) &&
            pointerRoot(store->getPointerOperand()) == thisAddress &&
            offsetFrom(session, store->getPointerOperand(), thisAddress) ==
                baseOffset + baseOwnOffset)
          baseArm = store;
        if (value && value->getType()->isIntegerTy(8) && value->isOne() &&
            pointerRoot(store->getPointerOperand()) == thisAddress &&
            offsetFrom(session, store->getPointerOperand(), thisAddress) ==
                ownOffset) {
          const auto *returned =
              llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
          const auto *success = returned ? llvm::dyn_cast<llvm::ConstantInt>(
                                               returned->getReturnValue())
                                         : nullptr;
          expect(success && success->isOne(),
                 "the derived own destructor is armed only at normal return");
          normalArm = true;
        }
      }
  expect(baseTable && derivedTable &&
             baseTable->getParent() == derivedTable->getParent() &&
             baseTable->comesBefore(derivedTable) && normalArm,
         "exact empty base publication precedes the existing derived-state "
         "anchor and fallible field initialization");
  expect(baseOffset != 0 && baseZero && baseArm && baseTable && derivedTable &&
             rollbackPublication &&
             baseZero->getParent() == baseTable->getParent() &&
             baseArm->getParent() == baseTable->getParent() &&
             derivedTable->getParent() == baseTable->getParent() &&
             rollbackPublication->getParent() == baseTable->getParent() &&
             offsetFrom(session, baseZero->getPointerOperand(), thisAddress) ==
                 baseOffset &&
             offsetFrom(session, baseTable->getPointerOperand(), thisAddress) ==
                 baseTableOffset &&
             baseZero->comesBefore(baseTable) &&
             baseTable->comesBefore(baseArm) &&
             baseArm->comesBefore(derivedTable) &&
             derivedTable->comesBefore(rollbackPublication),
         "only the exact nonzero-offset empty base is zeroed, table-installed "
         "and armed before derived activation and rollback publication");
  bool rollback = false, failureEdge = false;
  for (const auto &obligation : selected->body.dropObligations)
    if (obligation.kind == MirDropObligationKind::ConstructionRollback) {
      expect(obligation.dropType.classInstance == solo.id &&
                 !obligation.initiallyActive,
             "partial construction has only the exact unpublished base "
             "rollback obligation, never a whole derived destructor");
      rollback = true;
    }
  for (const auto &block : selected->body.blocks)
    failureEdge |= block.terminator.kind == MirTerminatorKind::PropagateFailure;
  expect(rollback && failureEdge,
         "fallible field initialization retains base rollback and failure");
  llvm::DominatorTree dominance(*constructor);
  bool routedRollback = false;
  for (const auto &block : selected->body.blocks) {
    const auto &term = block.terminator;
    const auto operation = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const auto &row) { return row.id == term.invokeInstruction; });
    if (term.kind != MirTerminatorKind::Invoke ||
        operation == block.instructions.end() ||
        operation->kind != MirInstructionKind::Compute)
      continue;
    llvm::BasicBlock *failure = nullptr;
    for (auto &nativeBlock : *constructor)
      if (nativeBlock.getName() == "bb" + std::to_string(term.elseTarget))
        failure = &nativeBlock;
    if (!failure)
      throw std::runtime_error(
          "generated-empty test lost field-failure routing");
    const auto *branch = failure->getTerminator();
    const auto *guard =
        branch && branch->getNumSuccessors() == 2
            ? llvm::dyn_cast<llvm::LoadInst>(branch->getOperand(0))
            : nullptr;
    expect(guard && rollbackPublication &&
               guard->getPointerOperand() ==
                   rollbackPublication->getPointerOperand(),
           "the field-failure edge tests the exact published base obligation");
    for (const auto &nativeBlock : *constructor)
      for (const auto &nativeOperation : nativeBlock)
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&nativeOperation);
            call && call->getCalledFunction() &&
            call->getCalledFunction()->getName().starts_with(
                "__gti_native_cleanup_") &&
            !call->getCalledFunction()->getName().starts_with(
                "__gti_native_cleanup_observation_")) {
          expect(dominance.dominates(failure, &nativeBlock) &&
                     offsetFrom(session, call->getArgOperand(0), thisAddress) ==
                         baseOffset,
                 "partial field failure reaches cleanup of exactly the "
                 "published Solo base, not the complete Leaf");
          routedRollback = true;
        }
  }
  expect(routedRollback,
         "generated-empty base rollback is present on the field-failure route");
  for (const auto *owner : {&solo, &leaf}) {
    auto *destructor =
        nativeBody({MirBodyKind::Destructor, *owner->destructor});
    bool indirect = false;
    for (const auto &block : *destructor)
      for (const auto &operation : block)
        if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&operation))
          indirect |= !call->getCalledFunction();
    expect(indirect,
           "actual source destructor virtual observations remain indirect");
  }
  const MirInstruction *fresh = nullptr;
  for (const auto &block : evaluate->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == MirInstructionKind::Construct &&
          instruction.info.type == solo.type)
        fresh = &instruction;
  if (!fresh)
    throw std::runtime_error("generated-empty test lost its fresh operation");
  auto *probe = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::InternalLinkage, "generated.empty.rejection.probe",
      session.module());
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", probe));
  auto *storage = ir.CreateAlloca(session.layouts().get(solo.type).storage);
  for (int mutation = 0; mutation != 12; ++mutation) {
    auto body = evaluate->body;
    MirInstruction *operation = nullptr;
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == fresh->id)
          operation = &instruction;
    if (mutation == 0)
      operation->kind = MirInstructionKind::Assign;
    if (mutation == 1)
      operation->constructorKind = ConstructorKind::Copy;
    if (mutation == 2)
      operation->constructorKind = ConstructorKind::Move;
    if (mutation >= 3 && mutation <= 7) {
      operation->info.type =
          named(session.program(),
                std::array{"SourceDefault", "Scheduled", "Stateful", "Foreign",
                           "Leaf"}[mutation - 3])
              .type;
      body.values.at(*operation->result - 1).info.type = operation->info.type;
    }
    if (mutation == 8)
      operation->constructorInitializer = 1;
    if (mutation == 9)
      body.values.at(*operation->result - 1).info.type = impl.type;
    if (mutation == 10)
      operation->kind = MirInstructionKind::InitializeBase;
    if (mutation == 11) {
      operation->info.type = SemanticType::UniqueOwner;
      body.values.at(*operation->result - 1).info.type = operation->info.type;
    }
    const auto size = ir.GetInsertBlock()->size();
    rejects<std::exception>(
        [&] { lifetimes.generatedEmpty(ir, storage, body, *operation); },
        "wrong purpose, selected source/scheduled/stateful/foreign/fieldful "
        "type or result/stage identity cannot acquire empty initialization");
    expect(ir.GetInsertBlock()->size() == size,
           "rejected empty initialization never mutates the supplied storage");
  }
  rejects<std::invalid_argument>(
      [&] { lifetimes.generatedEmpty(ir, storage, MirBody{}, *fresh); },
      "a foreign body cannot borrow a valid empty operation identity");
  for (const auto &block : selected->body.blocks)
    for (const auto &operation : block.instructions)
      if (operation.kind == MirInstructionKind::InitializeBase) {
        const auto size = ir.GetInsertBlock()->size();
        rejects<std::invalid_argument>(
            [&] {
              lifetimes.generatedEmpty(ir, storage, selected->body, operation);
            },
            "an exact base instruction cannot initialize without its source "
            "constructor stage");
        auto foreignConstructor = *selected;
        rejects<std::invalid_argument>(
            [&] {
              lifetimes.generatedEmpty(ir, storage, selected->body, operation,
                                       &foreignConstructor);
            },
            "a copied constructor record cannot acquire exact base-stage "
            "authority");
        expect(ir.GetInsertBlock()->size() == size,
               "base-stage identity rejection precedes storage writes");
      }
  ir.CreateRetVoid();
  session.verify();
}

void testDeclarationInitializers() {
  const auto frontend =
      Frontend().analyze("llvm-declaration-placeholders.gti", R"(
class Value {
  int data;
public:
  Value(int input) : data(input) {}
  int read() { return this.data; }
};
class View {
  mut int& target;
public:
  View(mut int& source) : target(source) {}
  int read() { return this.target; }
};
int inspect(int input) { Value value{input}; return value.read(); }
)");
  expect(frontend.canGenerateCode(), "ordinary declaration-only fields verify");
  if (!frontend.canGenerateCode())
    return;
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  std::vector<LoweredProgramIssue> issues;
  const auto verified = verifiedLoweredProgram(lowered, issues);
  if (!verified)
    throw std::runtime_error("declaration-only source seal failed");
  const auto conservative = planLlvmAdmission(*verified);
  expect(conservative.closure() == LlvmAdmissionClosure::AllExecutableBodies &&
             conservative.admitted(),
         "ordinary conservative class inventory consumes its exact storage "
         "witness");
  expect(
      std::any_of(conservative.requirements().begin(),
                  conservative.requirements().end(),
                  [](const auto &use) {
                    return use.required && use.entry->family == "ClassKind" &&
                           use.entry->name == "Class" &&
                           use.entry->state == LlvmCoverageState::Pending &&
                           use.implementedShape &&
                           use.implementedShape->name == "NativeClassStorage";
                  }),
      "the class-use witness does not promote the global pending family row");
  const auto object = LlvmBackend().generate(lowered);
  expect(object.kind == BackendArtifactKind::Object &&
             object.contents.size() > 100,
         "the real public object API lowers retained declaration inventory");
  Session session(*verified, {});
  lowerScalarProgram(session, planLlvmAdmission(*verified));
  const auto &value = named(session.program(), "Value");
  const auto &view = named(session.program(), "View");
  for (const auto *owner : {&value, &view}) {
    bool placeholder = false;
    for (const auto &block : owner->fieldInitializers.blocks)
      for (const auto &instruction : block.instructions)
        placeholder =
            placeholder || isMirDeclarationOnlyFieldInitialization(
                               *owner, owner->fieldInitializers, instruction);
    expect(placeholder, "exact ordinary/reference field inventory retains its "
                        "declaration-only identity");
    auto *native = session.module().getFunction(
        "__gti_native_v1_body_" +
        std::to_string(static_cast<unsigned>(MirBodyKind::FieldInitializers)) +
        "_" + std::to_string(owner->id));
    expect(native && std::all_of(
                         native->begin(), native->end(),
                         [](const auto &block) {
                           return std::none_of(
                               block.begin(), block.end(),
                               [](const auto &operation) {
                                 return llvm::isa<llvm::StoreInst>(operation) ||
                                        llvm::isa<llvm::LoadInst>(operation);
                               });
                         }),
           "declaration-only field lowering performs no value/address store, "
           "load or cleanup arming");
  }
  for (int mutation : {0, 1, 2, 3, 4}) {
    auto changed = value;
    auto &body = changed.fieldInitializers;
    auto &instruction = body.blocks.front().instructions.front();
    auto &place = body.places.at(*instruction.destination - 1);
    if (mutation == 0)
      ++place.binding;
    if (mutation == 1) {
      place.type = SemanticType::Bool;
      instruction.info.type = place.type;
    }
    if (mutation == 2)
      body.kind = MirBodyKind::StaticFieldInitializers;
    if (mutation == 3)
      instruction.constructorInitializer = 1;
    if (mutation == 4)
      ++place.symbol;
    expect(!isMirDeclarationOnlyFieldInitialization(changed, body, instruction),
           "wrong binding/type/body/stage/symbol cannot acquire "
           "declaration-only lowering");
    expect(
        !isMirDeclarationOnlyFieldInitialization(view, body, instruction),
        "a foreign owner's inventory cannot acquire the placeholder exemption");
  }
  session.verify();

  const auto mixed =
      Frontend().analyze("llvm-mixed-declaration-placeholders.gti", R"(
class Mixed {
  int data;
  int defaulted = 7;
  int overridden = 12;
public:
  Mixed(int input) : data(input), overridden(3) {}
  virtual int read() { return this.data + this.defaulted + this.overridden; }
};
class InitializerOnly {
  int data;
public:
  InitializerOnly(int input) : data(input) {}
  virtual int read() { return this.data; }
};
int main() {
  Mixed value{3};
  InitializerOnly other{4};
  return value.read() - 13 + other.read() - 4;
}
)");
  expect(mixed.canGenerateCode(),
         "polymorphic mixed default/explicit constructor verifies");
  if (!mixed.canGenerateCode())
    return;
  const auto sealed = verifiedLoweredProgram(
      gti_test::lowerProgram(mixed, mixed.mir, mixed.mir), issues);
  if (!sealed)
    throw std::runtime_error("mixed field source seal failed");
  Session privateSession(*sealed, {});
  const auto gated = planLlvmAdmission(*sealed);
  expect(gated.admitted(),
         "mixed scalar field initialization receives exact per-use dispatch "
         "admission: " +
             gated.limitation());
  lowerScalarProgram(privateSession, gated);
  privateSession.verify();
  auto erased = mixed.mir;
  bool removed = false;
  auto &owner = const_cast<MirClassInstance &>(
      *erased.findClassInstance(named(privateSession.program(), "Mixed").id));
  for (auto &block : owner.fieldInitializers.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == MirInstructionKind::Initialize &&
          !instruction.operands.empty()) {
        instruction.operands.clear();
        removed = true;
        break;
      }
  expect(removed && rebuildMirValueUses(owner.fieldInitializers) &&
             verifyMirProgram(erased).valid(),
         "erasing a real scalar initializer can resemble an otherwise valid "
         "core placeholder");
  const auto forged =
      LoweredProgramBuilder().build(mixed.program, mixed.semantics, mixed.hir,
                                    erased, erased, {}, TargetInfo::host());
  expect(!forged.valid(), "source coherence rejects erasing a real declaration "
                          "initializer value/store");
  const auto missing = Frontend().analyze("llvm-missing-required-field.gti", R"(
class Missing { int data; public: Missing() {} };
int main() { Missing value{}; return 0; }
)");
  expect(!missing.canGenerateCode() &&
             std::any_of(missing.diagnostics.begin(), missing.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2000";
                         }),
         "actual missing required initialization remains a source error, never "
         "implicit zeroing");
}

void testProvedDirectReceiver() {
  const auto frontend =
      Frontend().analyze("llvm-dispatch-proved-direct.gti", std::string(R"(
interface Operation { int run(int value); };
class Increment : public Operation {
  int bias = 1;
public:
  Increment() {}
  int run(int value) override { return value + this.bias; }
};
class Decrement : public Operation {
public:
  Decrement() {}
  int run(int value) override { return value - 1; }
};
int invoke(Operation& selected, int value) { return selected.run(value); }
int apply(int value) {
  Increment concrete{};
  return invoke(concrete, value);
}
int main() { return apply(40) - 41; }
)"));
  expect(frontend.canGenerateCode(), "exact receiver source remains valid");
  if (!frontend.canGenerateCode())
    return;
  const auto optimized =
      OptimizationPipeline().run({.hir = frontend.hir,
                                  .mir = frontend.mir,
                                  .level = OptimizationLevel::O2,
                                  .target = TargetInfo::host()});
  expect(optimized.valid(), "real exact-receiver optimization verifies");
  if (!optimized.valid())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, optimized.sourceMir, optimized.mir,
                             TargetInfo::host(),
                             optimized.report.transformations),
      issues);
  if (!input)
    throw std::runtime_error(
        "optimized receiver presentation failed source coherence");
  Session session(*input, {});
  auto dispatch = DispatchStructuralTestAccess::inspect(session);
  bool found = false;
  for (const auto address : enumerateMirBodyAddresses(session.program().mir()))
    for (const auto &block :
         findMirBody(session.program().mir(), address)->blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.receiver || instruction.tombstone ||
            !isMirExactReceiverPresentation(session.program().mir(),
                                            instruction,
                                            instruction.receiver->type))
          continue;
        found = true;
        const auto *target = session.program().mir().findFunctionInstance(
            *instruction.functionTarget);
        const auto &owner =
            *session.program().mir().findClassInstance(*target->owner);
        const auto path = session.layouts().baseSubobject(
            owner.type, instruction.receiver->type);
        auto *probe = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                    {session.layouts().pointer()}, false),
            llvm::GlobalValue::InternalLinkage, "dispatch.proved.direct.probe",
            session.module());
        llvm::IRBuilder<> ir(
            llvm::BasicBlock::Create(session.context(), "entry", probe));
        auto *receiver =
            dispatch.directReceiver(ir, instruction, probe->getArg(0));
        expect(path.offset == 0 && receiver == probe->getArg(0),
               "verified direct interface presentation does not double-adjust "
               "the selected view");
        const auto abi = session.abi().gti(
            target->returnType, target->parameterTypes,
            target->mayRaiseDefinedFailure, true, target->tracksCaller);
        expect(session.abi().declareFunction(target->id)->getFunctionType() ==
                   abi.type,
               "devirtualized bodies retain their exact implementation ABI");
        ir.CreateRetVoid();
        expect(!llvm::verifyFunction(*probe),
               "source-proved direct presentation forms valid IR");
      }
  expect(found, "real optimized MIR supplies an exact interface presentation");
  const auto conflicting =
      Frontend().analyze("llvm-dispatch-conflicting-roots.gti", std::string(R"(
interface First { int read(); };
interface Second { bool read(); };
class Bad : public First, public Second {
public: int read() override { return 1; }
public:
  Bad() = default;
};
int main() { Bad value{}; return 0; }
)"));
  expect(!conflicting.canGenerateCode(),
         "conflicting required root contracts reject under source authority "
         "before LLVM");
  const auto repeated =
      Frontend().analyze("llvm-dispatch-repeated-generic.gti", std::string(R"(
interface Transform<T> { T apply(T value); };
class Both : public Transform<int>, public Transform<float> {
public:
  int apply(int value) override { return value; }
  float apply(float value) override { return value; }
public:
  Both() = default;
};
int main() { return 0; }
)"));
  expect(!repeated.canGenerateCode() &&
             std::any_of(repeated.diagnostics.begin(),
                         repeated.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2040";
                         }),
         "different arguments do not silently enable repeated nominal "
         "interface bases");
}

void test(Session &session, const VerifiedLoweredProgram &input) {
  const auto &program = session.program();
  const auto &root = named(program, "Root");
  const auto &middle = named(program, "Middle");
  const auto &leaf = named(program, "Leaf");
  const auto &reader = named(program, "Reader");
  const auto &writer = named(program, "Writer");
  auto &layouts = session.layouts();
  expect(layouts.baseSubobject(leaf.type, root.type).indices ==
                 std::vector<unsigned>({0, 0}) &&
             layouts.baseSubobject(reader.type, reader.type).indices.empty(),
         "physical identity queries preserve multilevel and already-selected "
         "views");
  const auto inherited =
      layouts.fieldSubobject(leaf.type, root.declaredFields.front().symbol);
  const auto declared =
      layouts.fieldSubobject(leaf.type, leaf.declaredFields.back().symbol);
  expect(inherited.owner.type == root.type && inherited.offset == 0 &&
             declared.owner.type == leaf.type &&
             declared.offset != inherited.offset,
         "field owner lookup uses exact identity across inherited and direct "
         "fields");
  rejects<std::logic_error>(
      [&] { static_cast<void>(layouts.baseSubobject(reader.type, leaf.type)); },
      "physical subobject lookup never manufactures a downcast");
  rejects<std::logic_error>(
      [&] { static_cast<void>(layouts.baseSubobject(root.type, writer.type)); },
      "a sibling interface is not a base of the selected source view");
  rejects<std::logic_error>(
      [&] {
        static_cast<void>(layouts.fieldSubobject(
            leaf.type, named(program, "Other").declaredFields.front().symbol));
      },
      "an unrelated same-named field cannot replace an inherited SymbolId");
  rejects<std::logic_error>(
      [&] {
        static_cast<void>(layouts.fieldSubobject(
            leaf.type, std::numeric_limits<SymbolId>::max()));
      },
      "a foreign field identity cannot enter physical layout");
  auto foreignType = leaf.type;
  foreignType.classId = std::numeric_limits<ClassId>::max();
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(layouts.baseSubobject(leaf.type, foreignType)); },
      "an unsealed concrete target cannot enter physical layout");
  auto dispatch = DispatchStructuralTestAccess::inspect(session);
  const auto &views = dispatch.views(leaf.id);
  expect(views.size() == 5,
         "leaf state plans its own and all four inherited polymorphic views");
  const auto &baseViews = dispatch.views(root.id);
  expect(baseViews.size() == 1 && baseViews.front().viewClass == root.id,
         "a base active state never installs future derived or sibling views");
  expect(dispatch.views(middle.id).size() == 3,
         "middle state includes exactly its constructed hierarchy");
  expect(layouts.fieldIndex(root.type, root.declaredFields.front().symbol) ==
                 0 &&
             layouts.activeIndex(root.type) == 1 &&
             layouts.dispatchIndex(root.type) == 2 &&
             layouts.baseIndex(leaf.type, middle.id) == 0,
         "dispatch storage preserves existing field/base/source-drop indices");

  bool readerAdjusted = false, writerAdjusted = false, adaptsBodyEffect = false;
  for (const auto &view : views) {
    const auto path = layouts.basePath(leaf.type, view.path);
    const auto *owner = program.mir().findClassInstance(view.viewClass);
    expect(path.type == owner->type && path.offset == view.offset &&
               layouts.get(view.location.owner.type)
                       .storage->getStructElementType(view.location.index) ==
                   layouts.pointer(),
           "every view uses the shared exact base path and pointer storage");
    for (const auto &slot : view.slots) {
      const auto selected =
          std::find_if(leaf.virtualDispatch.begin(), leaf.virtualDispatch.end(),
                       [&](const auto &row) { return row.root == slot.root; });
      expect(selected != leaf.virtualDispatch.end() &&
                 slot.target == selected->target &&
                 slot.targetOwner == selected->targetOwner && !slot.pure,
             "table slots consume the sealed final selection");
      if (selected == leaf.virtualDispatch.end())
        continue;
      const auto target = layouts.basePath(leaf.type, selected->targetPath);
      expect(static_cast<std::int64_t>(view.location.owner.offset) +
                     slot.receiverAdjustment ==
                 static_cast<std::int64_t>(target.offset),
             "receiver adjustment reaches the exact implementation subobject");
      if (view.viewClass == reader.id)
        readerAdjusted |= view.offset != 0 && slot.receiverAdjustment < 0 &&
                          slot.targetOwner == middle.id;
      if (view.viewClass == writer.id)
        writerAdjusted |= view.offset != 0 && slot.receiverAdjustment < 0 &&
                          slot.targetOwner == leaf.id;

      const auto plan = dispatch.rootAbi(slot.root);
      const auto adjusted = session.abi().virtualAdjustment(plan, slot.target);
      const auto *targetFunction =
          program.mir().findFunctionInstance(slot.target);
      adaptsBodyEffect |=
          plan.mayFail && !targetFunction->mayRaiseDefinedFailure;
      expect(adjusted.entry == plan.call &&
                 adjusted.implementation ==
                     session.abi().gti(targetFunction->returnType,
                                       targetFunction->parameterTypes,
                                       targetFunction->mayRaiseDefinedFailure,
                                       true, targetFunction->tracksCaller),
             "entry and implementation transport use the same ABI authority");
    }
  }
  expect(readerAdjusted && writerAdjusted,
         "multiple interfaces exercise distinct negative receiver adjustments");
  expect(
      adaptsBodyEffect,
      "virtual invocation contract remains distinct from an infallible body");

  const std::vector<HirClassInstanceId> missing{leaf.id};
  rejects<std::logic_error>(
      [&] { static_cast<void>(layouts.basePath(leaf.type, missing)); },
      "a base path cannot jump to its complete owner");
  const std::vector<HirClassInstanceId> repeated{middle.id, root.id, middle.id};
  rejects<std::logic_error>(
      [&] { static_cast<void>(layouts.basePath(leaf.type, repeated)); },
      "a base path cannot repeat or invent a direct edge");
  const auto readerRoot = reader.virtualDispatch.front().root;
  const auto plan = dispatch.rootAbi(readerRoot);
  std::vector<HirFunctionInstanceId> expectedTargets;
  for (const auto owner :
       {middle.id, named(program, "Other").id, named(program, "Front").id}) {
    const auto &rows = program.mir().findClassInstance(owner)->virtualDispatch;
    const auto selected =
        std::find_if(rows.begin(), rows.end(),
                     [&](const auto &row) { return row.root == readerRoot; });
    if (selected == rows.end())
      throw std::runtime_error("fixture lost an intended Reader target family");
    expectedTargets.push_back(selected->target);
  }
  std::sort(expectedTargets.begin(), expectedTargets.end());
  expect(
      plan.implementations == expectedTargets,
      "root ABI retains exactly the Middle, Other and Front implementations");
  rejects<std::invalid_argument>(
      [&] {
        auto incomplete = plan.implementations;
        incomplete.pop_back();
        static_cast<void>(session.abi().virtualRoot(readerRoot, incomplete));
      },
      "a callsite cannot narrow the root's target set");
  rejects<std::invalid_argument>(
      [&] {
        auto duplicate = plan.implementations;
        duplicate.push_back(duplicate.back());
        static_cast<void>(session.abi().virtualRoot(readerRoot, duplicate));
      },
      "duplicate implementations cannot alter a canonical root plan");
  rejects<std::invalid_argument>(
      [&] { static_cast<void>(session.abi().virtualAdjustment(plan, 0)); },
      "an adapter cannot select a body outside the sealed family");
  rejects<std::invalid_argument>(
      [&] {
        auto forged = plan;
        forged.call.parameters.clear();
        static_cast<void>(session.abi().virtualAdjustment(
            forged, forged.implementations.front()));
      },
      "an adapter cannot conceal different physical channels");
  Session independent(input, {});
  auto other = DispatchStructuralTestAccess::inspect(independent);
  const auto foreign = other.rootAbi(readerRoot);
  rejects<std::invalid_argument>(
      [&] {
        static_cast<void>(session.abi().virtualAdjustment(
            foreign, foreign.implementations.front()));
      },
      "LLVM plans from an independent context cannot be reused");
  std::set<std::pair<std::vector<unsigned>, unsigned>> physical;
  for (const auto &view : views)
    physical.emplace(view.location.owner.indices, view.location.index);
  expect(physical.size() == 3 && layouts.get(leaf.type).size < 64,
         "primary-chain reuse removes two pointers without merging interface "
         "views");
  for (const auto &view : views)
    if (view.location.owner.type == root.type)
      expect(view.slots == views.front().slots && view.profileOwner == leaf.id,
             "all aliases of one primary location share exactly one derived "
             "table");
  for (const auto ancestor : {root.id, middle.id}) {
    const auto &prefix = dispatch.slotProfile(ancestor);
    const auto &derived = dispatch.slotProfile(leaf.id);
    expect(derived.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), derived.begin()),
           "derived slot profiles never renumber the ancestor prefix");
  }
  const auto &front = named(program, "Front");
  const auto &frontLeaf = named(program, "FrontLeaf");
  const auto location = layouts.dispatchLocation(frontLeaf.type);
  expect(location && location->owner.type == root.type &&
             location->owner.indices == std::vector<unsigned>({1, 1}) &&
             location->owner.offset != 0,
         "interfaces preceding the state base retain a nonzero physical "
         "pointer-owner path");
  const auto &frontViews = dispatch.views(frontLeaf.id);
  const auto &frontPrefix = dispatch.slotProfile(front.id);
  const auto &frontProfile = dispatch.slotProfile(frontLeaf.id);
  expect(frontProfile.size() >= frontPrefix.size() &&
             std::equal(frontPrefix.begin(), frontPrefix.end(),
                        frontProfile.begin()),
         "a preceding interface cannot renumber the primary ancestor slots");
  bool adjustedFromPhysicalOwner = false;
  for (const auto &view : frontViews)
    if (view.location == frontViews.front().location) {
      expect(view.slots == frontViews.front().slots,
             "nonzero primary aliases also retain one exact table plan");
      for (const auto &slot : view.slots)
        if (slot.targetOwner == front.id)
          adjustedFromPhysicalOwner |=
              slot.receiverAdjustment < 0 &&
              static_cast<std::int64_t>(view.location.owner.offset) +
                      slot.receiverAdjustment ==
                  static_cast<std::int64_t>(
                      layouts.baseSubobject(frontLeaf.type, front.type).offset);
    }
  expect(adjustedFromPhysicalOwner,
         "thunk adjustment starts at the physical pointer owner, not the "
         "logical derived receiver");
  std::cout << "dispatch-layout-cost: leaf-size=" << layouts.get(leaf.type).size
            << " leaf-alignment=" << layouts.get(leaf.type).alignment
            << " views=" << views.size() << " dispatch-pointer-bytes="
            << physical.size() * session.dataLayout().getPointerSize() << '\n';
}
// A restoring exchange takes its moved replacement from the value's own
// storage and publishes the previous object directly in its destination. A
// local returned beside a sibling whose cleanup can fail reaches the caller
// through the body's return-result obligation. Each plan binds the exact
// lifecycle MIR proves; any other release of the moved value's obligation
// refuses rather than guessing which cleanup owns the object.
void testExchangedAndReturnedTransferPlanning() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto load = [&](const char *relative) {
    const auto frontend =
        Frontend({.requireEntryPoint = true,
                  .lowerReachableOnly = true,
                  .lowerLibraryReachableOnly = true,
                  .trustedBodies = TrustedBodyAnalysis::Reachable})
            .analyze(root / relative, std::nullopt,
                     {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    if (!frontend.canGenerateCode())
      throw std::runtime_error(std::string(relative) + " lost valid source");
    std::vector<LoweredProgramIssue> issues;
    auto input = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
    if (!input)
      throw std::runtime_error(std::string(relative) + " lost its MIR seal");
    return std::move(*input);
  };
  const auto find = [](MirBody &body, MirInstructionId id) -> MirInstruction & {
    for (auto &block : body.blocks)
      for (auto &stage : block.instructions)
        if (stage.id == id)
          return stage;
    throw std::runtime_error("transfer mutation lost its stage");
  };
  const auto obligation = [](MirBody &body,
                             MirDropObligationId id) -> MirDropObligation & {
    for (auto &drop : body.dropObligations)
      if (drop.id == id)
        return drop;
    throw std::runtime_error("transfer mutation lost its obligation");
  };

  const auto exchanged =
      load("tests/fixtures/llvm_dispatch_exchanged_transfers.gti");
  const auto &program = exchanged.program().mir();
  std::size_t exchanges = 0;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto &body = *findMirBody(program, address);
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (!isFreshHierarchyTransferProducer(stage) ||
            stage.kind != MirInstructionKind::Move)
          continue;
        FreshHierarchyTransfer planned;
        try {
          if (!containsHierarchyStorage(program, stage.info.type))
            continue;
          planned = planFreshHierarchyTransfer(program, body, stage);
        } catch (const BackendUnsupportedError &) {
          continue;
        }
        if (planned.publishes != FreshHierarchyPublication::Exchange)
          continue;
        ++exchanges;
        const auto &exchange = *planned.publication;
        const Placement placement(program, body);
        const MirInstruction *initialize = nullptr;
        for (const auto &candidate : block.instructions)
          if (candidate.kind == MirInstructionKind::Initialize &&
              candidate.operands.size() == 1 &&
              candidate.operands.front().value == *exchange.result)
            initialize = &candidate;
        expect(exchange.intrinsic == IntrinsicKind::RestoringExchange &&
                   exchange.operands.size() == 2 &&
                   exchange.operands[1].value == *stage.result &&
                   planned.valueDrop && !placement.target(*stage.result),
               "an exchanged replacement waits in its own value storage");
        expect(isFreshObjectProducer(program, exchange) && initialize &&
                   placement.target(*exchange.result) &&
                   placement.target(*exchange.result)->place ==
                       *initialize->destination,
               "the previous object publishes directly in its destination");
        // The exchange must take over the moved value's obligation exactly:
        // a conditional, missing or retyped release refuses.
        for (unsigned mutation = 0; mutation != 3; ++mutation) {
          auto changed = program;
          auto &changedBody = *findMirBody(changed, address);
          auto &events = find(changedBody, exchange.id).lifecycle;
          const auto release = std::find_if(
              events.begin(), events.end(), [&](const auto &event) {
                return event.kind == MirLifecycleEventKind::TransferOut &&
                       event.source == planned.valueDrop;
              });
          if (release == events.end())
            throw std::runtime_error("exchange lost its value release");
          if (mutation == 0)
            release->conditional = true;
          else if (mutation == 1)
            release->kind = MirLifecycleEventKind::Drop;
          else
            events.erase(release);
          rejects<BackendUnsupportedError>(
              [&] {
                static_cast<void>(planFreshHierarchyTransfer(
                    changed, changedBody, find(changedBody, stage.id)));
              },
              "an exchanged replacement requires the exchange's exact release");
        }
      }
  }
  expect(exchanges >= 3,
         "exchanged elements, fields and slots were planned and mutated");

  const auto siblings =
      load("tests/fixtures/llvm_dispatch_returned_siblings.gti");
  const auto &returned = siblings.program().mir();
  std::size_t reparented = 0;
  for (const auto address : enumerateMirBodyAddresses(returned)) {
    const auto &body = *findMirBody(returned, address);
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (!isFreshHierarchyTransferProducer(stage) ||
            stage.kind != MirInstructionKind::Move)
          continue;
        FreshHierarchyTransfer planned;
        try {
          if (!containsHierarchyStorage(returned, stage.info.type))
            continue;
          planned = planFreshHierarchyTransfer(returned, body, stage);
        } catch (const BackendUnsupportedError &) {
          continue;
        }
        if (planned.publishes != FreshHierarchyPublication::Returned ||
            !planned.valueDrop)
          continue;
        // The reparent into the return-result obligation that a failing
        // sibling cleanup drops instead of the moved value obligation.
        MirInstructionId handoff = 0;
        MirDropObligationId result = 0;
        for (const auto &candidate : body.blocks)
          for (const auto &instruction : candidate.instructions)
            for (const auto &event : instruction.lifecycle)
              if (event.kind == MirLifecycleEventKind::Reparent &&
                  event.source == planned.valueDrop) {
                handoff = instruction.id;
                result = event.target;
              }
        const auto *target = result ? body.findDropObligation(result) : nullptr;
        if (!target || target->kind != MirDropObligationKind::ReturnResult)
          continue;
        ++reparented;
        MirInstructionId release = 0;
        for (const auto &candidate : body.blocks)
          for (const auto &instruction : candidate.instructions)
            for (const auto &event : instruction.lifecycle)
              if (event.kind == MirLifecycleEventKind::TransferOut &&
                  event.source == result)
                release = instruction.id;
        expect(release != 0,
               "the return releases its return-result obligation");
        for (unsigned mutation = 0; mutation != 4; ++mutation) {
          auto changed = returned;
          auto &changedBody = *findMirBody(changed, address);
          if (mutation == 0)
            for (auto &event : find(changedBody, handoff).lifecycle)
              event.conditional = true;
          else if (mutation == 1)
            obligation(changedBody, result).initiallyActive = true;
          else if (mutation == 2)
            obligation(changedBody, result).kind = MirDropObligationKind::Value;
          else
            for (auto &event : find(changedBody, release).lifecycle)
              if (event.source == result)
                event.conditional = true;
          rejects<BackendUnsupportedError>(
              [&] {
                static_cast<void>(planFreshHierarchyTransfer(
                    changed, changedBody, find(changedBody, stage.id)));
              },
              "a returned local needs one exact return-result handoff");
        }
      }
  }
  expect(reparented >= 2, "both returned siblings reparent at the return");
}

// A carrier view's loan may depend on the carrier's own stored loans, which
// record the external owners the carrier borrows. A forged dependency -- a
// stored loan another binding carries, a non-stored loan, or a returned
// relay of a different dependency -- refuses.
void testCarrierLoanDependencies() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend =
      Frontend({.requireEntryPoint = true,
                .lowerReachableOnly = true,
                .lowerLibraryReachableOnly = true,
                .trustedBodies = TrustedBodyAnalysis::Reachable})
          .analyze(root / "tests/fixtures/carrier_upcast_references.gti",
                   std::nullopt, {root / "stdlib/prelude.gti"}, {},
                   {root / "stdlib"});
  if (!frontend.canGenerateCode())
    throw std::runtime_error("carrier upcast fixture lost valid source");
  const auto &program = frontend.mir;
  const auto supported = [](const MirLoan &) { return true; };
  const auto returned = [](const MirTerminator &) { return true; };
  std::size_t locals = 0, returns = 0;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto &body = *findMirBody(program, address);
    for (const auto &loan : body.loans) {
      if (!loan.storedDependency ||
          (loan.kind != MirLoanKind::Local &&
           loan.kind != MirLoanKind::Return) ||
          !carrierLoanDependency(body, loan, supported, returned))
        continue;
      (loan.kind == MirLoanKind::Local ? locals : returns) += 1;
      for (unsigned mutation = 0; mutation != 3; ++mutation) {
        auto changed = program;
        auto &changedBody = *findMirBody(changed, address);
        MirLoan *forged = nullptr;
        for (auto &candidate : changedBody.loans)
          if (candidate.id == loan.id)
            forged = &candidate;
        if (loan.kind == MirLoanKind::Local) {
          for (auto &candidate : changedBody.loans)
            if (candidate.id == loan.storedDependency) {
              if (mutation == 0)
                candidate.carriers.clear();
              else
                candidate.kind = MirLoanKind::Local;
            }
          if (mutation == 2)
            forged->escapes = true;
        } else {
          if (mutation == 0)
            forged->storedDependency = forged->id;
          else if (mutation == 1)
            forged->escapes = false;
          else
            for (auto &block : changedBody.blocks)
              for (auto &stage : block.instructions)
                if (stage.kind == MirInstructionKind::Borrow &&
                    stage.loan == loan.id)
                  stage.operands.front().loan = loan.id;
        }
        expect(
            !carrierLoanDependency(changedBody, *forged, supported, returned),
            "a forged carrier loan dependency refuses");
      }
    }
  }
  expect(locals >= 2 && returns >= 1,
         "local views and an upcast return depend on carrier stored loans");
}

void testInertRelocation() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend =
      analyzeLegacyOwner(root, "inert-relocation.gti", std::string(R"(
import std.memory;
interface First {};
interface Second {};
class Parent : public First { public: mut int number = 17;  public: Parent() = default; };
class Item : public Second, public Parent { public: mut int extra = 25;  public: Item() = default; };
struct Items { mut Item values[2] = {};  public: Items() = default; };
struct StaticOwners {
  mut std::unique_ptr<Items> unique = nullptr;
  mut gti_internal::shared_owner<Items> shared = gti_internal::shared_owner_empty<Items>();
public:
  StaticOwners() = default;

};
struct Active { mut int marker = 113; ~Active() {}  public: Active() = default; };
struct Owners {
  mut std::unique_ptr<int> unique = nullptr;
  mut gti_internal::shared_owner<int> shared = gti_internal::shared_owner_empty<int>();
  mut Active state{};
public:
  Owners() = default;
};
struct NonRelocatable {
  NonRelocatable() {}
  NonRelocatable(NonRelocatable&& other) = delete;
};
enum class Choice { both(Item item, int tail), number(int value), empty };
enum class ActiveChoice { held(Active value), empty };
struct Choices {
  mut Choice choice = Choice::empty;
  mut ActiveChoice active = ActiveChoice::empty;
public:
  Choices() = default;
};
int main() {
  Items items{}; Active active{}; Choices choices{}; Owners owners{};
  StaticOwners staticOwners{};
  NonRelocatable custom{};
  return 0;
}
)"));
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "inert relocation source passes frontend");
    return;
  }
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    throw std::runtime_error("inert relocation source lost its MIR seal");
  const auto &item = named(input->program(), "Item");
  const auto &items = named(input->program(), "Items");
  const MirClassInstance *staticShared = nullptr;
  for (const auto &candidate : input->program().mir().classInstances())
    if (candidate.compilerCapability ==
            CompilerCapabilityTypeKind::SharedOwner &&
        candidate.type.arguments == std::vector<SemanticType>{items.type})
      staticShared = &candidate;
  if (!staticShared)
    throw std::runtime_error("static owner fixture lost its shared capability");
  const auto &parent = named(input->program(), "Parent");
  const auto &active = named(input->program(), "Active");
  const auto &choices = named(input->program(), "Choices");
  const auto choiceType = choices.declaredFields.at(0).type;
  const auto activeChoiceType = choices.declaredFields.at(1).type;
  const auto &owners = named(input->program(), "Owners");
  const auto &customMove = named(input->program(), "NonRelocatable");
  expect(item.polymorphic && !item.requiresActiveCleanup,
         "fixture contains inert polymorphic objects, not plain records");
  const auto temporary =
      std::filesystem::temp_directory_path() /
      ("gti-inert-relocation-" +
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
#define _POSIX_C_SOURCE 200112L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
extern int gti_relocation_probe(void);
void *gti_cleanup_allocate(uint64_t bytes, uint64_t alignment) {
  void *result = NULL;
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  if (posix_memalign(&result, alignment, bytes) != 0) abort();
  memset(result, 0, bytes);
  return result;
}
int gti_relocation_observe(void *source, void *destination, uint64_t first,
                            uint64_t second, uint64_t header, void *expected,
                            int32_t number, int32_t extra) {
  int32_t actual_number, actual_extra;
  void *actual_table, *source_table;
  memcpy(&actual_number, (char*)destination + first, sizeof(actual_number));
  memcpy(&actual_extra, (char*)destination + second, sizeof(actual_extra));
  memcpy(&actual_table, (char*)destination + header, sizeof(actual_table));
  memcpy(&source_table, (char*)source + header, sizeof(source_table));
  return actual_number != number || actual_extra != extra ||
         actual_table != expected || source_table != 0;
}
int gti_relocation_tag_observe(void *source, void *destination,
                              uint64_t offset, uint32_t expected) {
  uint32_t original, relocated;
  memcpy(&original, (char*)source + offset, sizeof(original));
  memcpy(&relocated, (char*)destination + offset, sizeof(relocated));
  return original != UINT32_MAX || relocated != expected;
}
int gti_relocation_handles_observe(void *from_unique, void *to_unique,
                                  void *from_shared, void *to_shared,
                                  void *payload, void *control) {
  void *source, *destination, *old_shared[2], *new_shared[2];
  memcpy(&source, from_unique, sizeof(source));
  memcpy(&destination, to_unique, sizeof(destination));
  memcpy(old_shared, from_shared, sizeof(old_shared));
  memcpy(new_shared, to_shared, sizeof(new_shared));
  return source != 0 || destination != payload || old_shared[0] != 0 ||
         old_shared[1] != 0 || new_shared[0] != control || new_shared[1] != payload;
}
int gti_relocation_state_observe(uint8_t *source, uint8_t *destination,
                                 uint8_t expected) {
  return *source != 0 || *destination != expected;
}
int main(void) { return gti_relocation_probe(); }
)") == driver::ArtifactWriteStatus::Success,
         "write relocation native consumer");
  driver::NativeToolchain toolchain;
  const auto cobject = temporary / "consumer.o";
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), consumer, cobject, driver::CStandard::C17,
      OptimizationLevel::O2, {}, {}));
  expect(compiled.succeeded(), "compile relocation native consumer");
  if (!compiled.succeeded())
    return;
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive}) {
    Session session(*input, {.optimization = level});
    auto dispatch = DispatchStructuralTestAccess::inspect(session);
    Failures failures(session);
    Lifetimes lifetimes(
        session, failures, dispatch, [](MirBodyAddress) -> llvm::Function * {
          throw std::runtime_error("inert relocation requested a source body");
        });
    const auto before = modulePublication(session);
    Lifetimes::preflightInertRelocation(session, dispatch, items.type);
    Lifetimes::preflightInertRelocation(session, dispatch, choiceType);
    Lifetimes::preflightGeneratedRelocation(session, dispatch, owners.type);
    Lifetimes::preflightGeneratedRelocation(session, dispatch,
                                            activeChoiceType);
    Lifetimes::preflightGeneratedRelocation(
        session, dispatch,
        SemanticType::uniqueOwnerOf(named(input->program(), "First").type));
    expect(modulePublication(session) == before,
           "inert relocation preflight publishes no module state");
    rejects<BackendUnsupportedError>(
        [&] { static_cast<void>(lifetimes.inertRelocation(active.type)); },
        "active cleanup cannot use the inert relocation mechanism");
    rejects<BackendUnsupportedError>(
        [&] { static_cast<void>(lifetimes.inertRelocation(activeChoiceType)); },
        "an inactive tag cannot bypass cleanup-bearing variant preflight");
    rejects<BackendUnsupportedError>(
        [&] { static_cast<void>(lifetimes.inertRelocation(owners.type)); },
        "owner relocation does not broaden inline payload eligibility");
    rejects<BackendUnsupportedError>(
        [&] {
          static_cast<void>(lifetimes.generatedRelocation(customMove.type));
        },
        "generated relocation cannot bypass a deleted move constructor");
    rejects<BackendUnsupportedError>(
        [&] {
          static_cast<void>(lifetimes.generatedRelocation(
              SemanticType::uniqueOwnerOf(SemanticType::Unknown)));
        },
        "pointer-sized layout alone cannot validate an unknown owner pointee");
    rejects<BackendUnsupportedError>(
        [&] {
          static_cast<void>(lifetimes.inertRelocation(
              SemanticType::referenceTo(SemanticType::Int32)));
        },
        "borrowed representation cannot use inert payload relocation");
    expect(modulePublication(session) == before,
           "rejected relocations publish no helper or dispatch table");
    Session other(*input, {});
    rejects<std::invalid_argument>(
        [&] {
          Lifetimes::preflightInertRelocation(other, dispatch, items.type);
        },
        "inert relocation rejects a foreign dispatch session");
    auto *relocate = lifetimes.inertRelocation(items.type);
    expect(relocate == lifetimes.inertRelocation(items.type),
           "exact type reuses its generated relocation helper");
    expect(relocate == lifetimes.generatedRelocation(items.type),
           "inert and general relocation share one emitter and helper cache");
    std::set<std::uint64_t> headers;
    for (const auto &view : dispatch.views(item.id))
      headers.insert(view.location.owner.offset +
                     session.layouts()
                         .get(view.location.owner.type)
                         .fieldOffsets.at(view.location.index));
    expect(headers.size() >= 2,
           "native relocation exercises distinct physical interface headers");
    const auto plan = session.abi().nativeC(SemanticType::Int32, {});
    auto *probe =
        llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                               "gti_relocation_probe", session.module());
    plan.applyTo(*probe);
    const auto pointer = SemanticType::rawPointerTo(SemanticType::UInt8);
    const std::vector<SemanticType> observerParameters{pointer,
                                                       pointer,
                                                       SemanticType::UInt64,
                                                       SemanticType::UInt64,
                                                       SemanticType::UInt64,
                                                       pointer,
                                                       SemanticType::Int32,
                                                       SemanticType::Int32};
    const auto observerPlan =
        session.abi().nativeC(SemanticType::Int32, observerParameters);
    auto *observer = llvm::Function::Create(
        observerPlan.type, llvm::GlobalValue::ExternalLinkage,
        "gti_relocation_observe", session.module());
    observerPlan.applyTo(*observer);
    const std::vector<SemanticType> tagParameters{
        pointer, pointer, SemanticType::UInt64, SemanticType::UInt32};
    const auto tagPlan =
        session.abi().nativeC(SemanticType::Int32, tagParameters);
    auto *tagObserver =
        llvm::Function::Create(tagPlan.type, llvm::GlobalValue::ExternalLinkage,
                               "gti_relocation_tag_observe", session.module());
    tagPlan.applyTo(*tagObserver);
    const std::vector<SemanticType> handleParameters(6, pointer);
    const auto handlePlan =
        session.abi().nativeC(SemanticType::Int32, handleParameters);
    auto *handleObserver = llvm::Function::Create(
        handlePlan.type, llvm::GlobalValue::ExternalLinkage,
        "gti_relocation_handles_observe", session.module());
    handlePlan.applyTo(*handleObserver);
    const std::vector<SemanticType> stateParameters{pointer, pointer,
                                                    SemanticType::UInt8};
    const auto statePlan =
        session.abi().nativeC(SemanticType::Int32, stateParameters);
    auto *stateObserver = llvm::Function::Create(
        statePlan.type, llvm::GlobalValue::ExternalLinkage,
        "gti_relocation_state_observe", session.module());
    statePlan.applyTo(*stateObserver);
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", probe));
    const auto storage = session.layouts().get(items.type).storage;
    auto *source = ir.CreateAlloca(storage);
    auto *destination = ir.CreateAlloca(storage);
    ir.CreateStore(llvm::Constant::getNullValue(storage), source);
    ir.CreateStore(llvm::Constant::getNullValue(storage), destination);
    const auto beforeRecoveryRejection = probe->getEntryBlock().size();
    rejects<std::invalid_argument>(
        [&] {
          static_cast<void>(
              dispatch.activeObject(ir, source, SemanticType::Int32));
        },
        "active-object recovery cannot reinterpret a non-class view");
    rejects<BackendUnsupportedError>(
        [&] {
          static_cast<void>(dispatch.activeObject(ir, source, items.type));
        },
        "a containing record does not acquire its field's dispatch identity");
    expect(probe->getEntryBlock().size() == beforeRecoveryRejection,
           "unsupported active-object recovery emits no partial instructions");
    const auto arrayType = items.declaredFields.front().type;
    const auto arrayStorage = session.layouts().get(arrayType).storage;
    const auto field = session.layouts().fieldIndex(
        items.type, items.declaredFields.front().symbol);
    const auto element = [&](llvm::Value *address, unsigned index) {
      return ir.CreateGEP(arrayStorage,
                          ir.CreateStructGEP(storage, address, field),
                          {ir.getInt64(0), ir.getInt64(index)});
    };
    const auto numberOffset =
        session.layouts()
            .fieldSubobject(item.type, parent.declaredFields.front().symbol)
            .offset;
    const auto extraOffset =
        session.layouts()
            .fieldSubobject(item.type, item.declaredFields.front().symbol)
            .offset;
    const auto at = [&](llvm::Value *address, std::uint64_t offset) {
      return ir.CreateGEP(ir.getInt8Ty(), address, ir.getInt64(offset));
    };
    // Source headers intentionally remain null. Relocation must derive the
    // destination's exact tables rather than inherit those invalid bytes.
    for (unsigned index = 0; index != 2; ++index) {
      auto *address = element(source, index);
      ir.CreateStore(ir.getInt32(17 + index), at(address, numberOffset));
      ir.CreateStore(ir.getInt32(25 + index), at(address, extraOffset));
    }
    ir.CreateCall(relocate, {destination, source});
    llvm::Value *bad = ir.getFalse();
    for (unsigned index = 0; index != 2; ++index) {
      auto *address = element(destination, index);
      bad = ir.CreateOr(
          bad, ir.CreateICmpNE(
                   ir.CreateLoad(ir.getInt32Ty(), at(address, numberOffset)),
                   ir.getInt32(17 + index)));
      bad = ir.CreateOr(bad,
                        ir.CreateICmpNE(ir.CreateLoad(ir.getInt32Ty(),
                                                      at(address, extraOffset)),
                                        ir.getInt32(25 + index)));
      for (const auto &view : dispatch.views(item.id)) {
        const auto *viewClass =
            session.program().mir().findClassInstance(view.viewClass);
        auto *selected = at(address, view.offset);
        bad = ir.CreateOr(
            bad,
            ir.CreateICmpNE(
                dispatch.activeObject(ir, selected, viewClass->type), address));
        const auto offset = view.location.owner.offset +
                            session.layouts()
                                .get(view.location.owner.type)
                                .fieldOffsets.at(view.location.index);
        auto *table =
            ir.CreateLoad(session.layouts().pointer(), at(address, offset));
        bad = ir.CreateOr(
            bad,
            ir.CreateICmpNE(table, dispatch.table(item.id, view.viewClass)));
        auto *original = ir.CreateLoad(session.layouts().pointer(),
                                       at(element(source, index), offset));
        bad = ir.CreateOr(bad, ir.CreateIsNotNull(original));
        auto *observed = ir.CreateCall(
            observer,
            {element(source, index), address, ir.getInt64(numberOffset),
             ir.getInt64(extraOffset), ir.getInt64(offset),
             dispatch.table(item.id, view.viewClass), ir.getInt32(17 + index),
             ir.getInt32(25 + index)});
        observerPlan.applyTo(*observed);
        bad = ir.CreateOr(bad, ir.CreateICmpNE(observed, ir.getInt32(0)));
      }
      const auto parentPath =
          session.layouts().baseSubobject(item.type, parent.type);
      expect(parentPath.offset != 0,
             "active-state recovery exercises a nonzero state-base offset");
      auto *parentAddress = at(address, parentPath.offset);
      dispatch.install(ir, parentAddress, parent.id);
      bad = ir.CreateOr(
          bad,
          ir.CreateICmpNE(dispatch.activeObject(ir, parentAddress, parent.type),
                          parentAddress));
      // A base construction/destruction state identifies that base, not the
      // enclosing allocation. Restore complete state for the remaining probes.
      dispatch.install(ir, address, item.id);
    }
    const auto payload = session.layouts().payload(choiceType);
    auto *relocateChoice = lifetimes.inertRelocation(choiceType);
    const auto tagOffset = session.dataLayout()
                               .getStructLayout(payload.storage)
                               ->getElementOffset(payload.tagIndex);
    const auto payloadOffset = session.dataLayout()
                                   .getStructLayout(payload.storage)
                                   ->getElementOffset(payload.payloadIndex);
    for (const auto tag : {0u, 1u, 2u, PayloadLayout::inactive}) {
      auto *from = ir.CreateAlloca(payload.storage);
      auto *to = ir.CreateAlloca(payload.storage);
      ir.CreateStore(llvm::Constant::getNullValue(payload.storage), from);
      ir.CreateStore(llvm::Constant::getNullValue(payload.storage), to);
      ir.CreateStore(ir.getInt32(tag), payloadTagAddress(ir, payload, from));
      if (tag == 0) {
        auto *value = payloadFieldAddress(ir, payload, from, 0, 0);
        ir.CreateStore(ir.getInt32(43), at(value, numberOffset));
        ir.CreateStore(ir.getInt32(59), at(value, extraOffset));
        ir.CreateStore(ir.getInt32(71),
                       payloadFieldAddress(ir, payload, from, 0, 1));
      } else if (tag == 1) {
        ir.CreateStore(ir.getInt32(83),
                       payloadFieldAddress(ir, payload, from, 1, 0));
      } else {
        ir.CreateStore(ir.getInt8(0xA5), at(from, payloadOffset));
        ir.CreateStore(ir.getInt8(0x5A), at(to, payloadOffset));
      }
      ir.CreateCall(relocateChoice, {to, from});
      auto *observed = ir.CreateCall(
          tagObserver, {from, to, ir.getInt64(tagOffset), ir.getInt32(tag)});
      tagPlan.applyTo(*observed);
      bad = ir.CreateOr(bad, ir.CreateICmpNE(observed, ir.getInt32(0)));
      if (tag == 0) {
        auto *value = payloadFieldAddress(ir, payload, to, 0, 0);
        for (const auto &view : dispatch.views(item.id)) {
          const auto header = view.location.owner.offset +
                              session.layouts()
                                  .get(view.location.owner.type)
                                  .fieldOffsets.at(view.location.index);
          auto *check = ir.CreateCall(
              observer,
              {payloadFieldAddress(ir, payload, from, 0, 0), value,
               ir.getInt64(numberOffset), ir.getInt64(extraOffset),
               ir.getInt64(header), dispatch.table(item.id, view.viewClass),
               ir.getInt32(43), ir.getInt32(59)});
          observerPlan.applyTo(*check);
          bad = ir.CreateOr(bad, ir.CreateICmpNE(check, ir.getInt32(0)));
        }
        bad = ir.CreateOr(
            bad, ir.CreateICmpNE(
                     ir.CreateLoad(ir.getInt32Ty(),
                                   payloadFieldAddress(ir, payload, to, 0, 1)),
                     ir.getInt32(71)));
      } else if (tag == 1) {
        bad = ir.CreateOr(
            bad, ir.CreateICmpNE(
                     ir.CreateLoad(ir.getInt32Ty(),
                                   payloadFieldAddress(ir, payload, to, 1, 0)),
                     ir.getInt32(83)));
      } else {
        bad = ir.CreateOr(bad,
                          ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(),
                                                        at(to, payloadOffset)),
                                          ir.getInt8(0x5A)));
        bad = ir.CreateOr(
            bad, ir.CreateICmpNE(
                     ir.CreateLoad(ir.getInt8Ty(), at(from, payloadOffset)),
                     ir.getInt8(0xA5)));
      }
    }
    const auto ownerStorage = session.layouts().get(owners.type).storage;
    const auto &uniqueWrapper =
        session.layouts().instance(owners.declaredFields[0].type);
    const auto shared = session.layouts().sharedOwner();
    const auto ownerField = [&](llvm::Value *address, unsigned index) {
      return ir.CreateStructGEP(
          ownerStorage, address,
          session.layouts().fieldIndex(owners.type,
                                       owners.declaredFields[index].symbol));
    };
    const auto handleField = [&](llvm::Value *address,
                                 const MirClassInstance &wrapper) {
      return ir.CreateStructGEP(
          session.layouts().get(wrapper.type).storage, address,
          session.layouts().fieldIndex(wrapper.type,
                                       wrapper.declaredFields[0].symbol));
    };
    auto *payloadAddress = ir.CreateAlloca(ir.getInt32Ty());
    auto *control = ir.CreateAlloca(shared.block.storage);
    ir.CreateStore(llvm::Constant::getNullValue(shared.block.storage), control);
    ir.CreateStore(
        ir.getInt64(7),
        ir.CreateStructGEP(shared.block.storage, control, shared.blockStrong));
    auto *moveOwners = lifetimes.generatedRelocation(owners.type);
    rejects<BackendUnsupportedError>(
        [&] { static_cast<void>(lifetimes.inertRelocation(owners.type)); },
        "a cached owner helper cannot bypass stricter inline eligibility");
    const auto activePayload = session.layouts().payload(activeChoiceType);
    auto *moveActivePayload = lifetimes.generatedRelocation(activeChoiceType);
    for (const auto permission : {0u, 1u}) {
      auto *from = ir.CreateAlloca(ownerStorage);
      auto *to = ir.CreateAlloca(ownerStorage);
      ir.CreateStore(llvm::Constant::getNullValue(ownerStorage), from);
      ir.CreateStore(llvm::Constant::getNullValue(ownerStorage), to);
      auto *uniqueFrom = handleField(ownerField(from, 0), uniqueWrapper);
      auto *uniqueTo = handleField(ownerField(to, 0), uniqueWrapper);
      auto *sharedFrom = ownerField(from, 1);
      auto *sharedTo = ownerField(to, 1);
      ir.CreateStore(payloadAddress, uniqueFrom);
      ir.CreateStore(payloadAddress,
                     ir.CreateStructGEP(shared.handle.storage, sharedFrom,
                                        shared.handleView));
      ir.CreateStore(control,
                     ir.CreateStructGEP(shared.handle.storage, sharedFrom,
                                        shared.handleBlock));
      const auto activeStorage = session.layouts().get(active.type).storage;
      auto *sourceFlag =
          ir.CreateStructGEP(activeStorage, ownerField(from, 2),
                             *session.layouts().activeIndex(active.type));
      auto *destinationFlag =
          ir.CreateStructGEP(activeStorage, ownerField(to, 2),
                             *session.layouts().activeIndex(active.type));
      ir.CreateStore(ir.getInt8(permission), sourceFlag);
      ir.CreateCall(moveOwners, {to, from});
      auto *observed =
          ir.CreateCall(handleObserver, {uniqueFrom, uniqueTo, sharedFrom,
                                         sharedTo, payloadAddress, control});
      handlePlan.applyTo(*observed);
      bad = ir.CreateOr(bad, ir.CreateICmpNE(observed, ir.getInt32(0)));
      observed = ir.CreateCall(
          stateObserver, {sourceFlag, destinationFlag, ir.getInt8(permission)});
      statePlan.applyTo(*observed);
      bad = ir.CreateOr(bad, ir.CreateICmpNE(observed, ir.getInt32(0)));
      bad = ir.CreateOr(
          bad, ir.CreateICmpNE(ir.CreateLoad(ir.getInt64Ty(),
                                             ir.CreateStructGEP(
                                                 shared.block.storage, control,
                                                 shared.blockStrong)),
                               ir.getInt64(7)));
      auto *fromPayload = ir.CreateAlloca(activePayload.storage);
      auto *toPayload = ir.CreateAlloca(activePayload.storage);
      ir.CreateStore(llvm::Constant::getNullValue(activePayload.storage),
                     fromPayload);
      ir.CreateStore(llvm::Constant::getNullValue(activePayload.storage),
                     toPayload);
      auto *oldActive =
          payloadFieldAddress(ir, activePayload, fromPayload, 0, 0);
      auto *newActive = payloadFieldAddress(ir, activePayload, toPayload, 0, 0);
      auto *oldFlag =
          ir.CreateStructGEP(activeStorage, oldActive,
                             *session.layouts().activeIndex(active.type));
      auto *newFlag =
          ir.CreateStructGEP(activeStorage, newActive,
                             *session.layouts().activeIndex(active.type));
      ir.CreateStore(ir.getInt8(permission), oldFlag);
      ir.CreateCall(moveActivePayload, {toPayload, fromPayload});
      observed = ir.CreateCall(stateObserver,
                               {oldFlag, newFlag, ir.getInt8(permission)});
      statePlan.applyTo(*observed);
      bad = ir.CreateOr(bad, ir.CreateICmpNE(observed, ir.getInt32(0)));
      observed = ir.CreateCall(
          tagObserver,
          {fromPayload, toPayload,
           ir.getInt64(session.dataLayout()
                           .getStructLayout(activePayload.storage)
                           ->getElementOffset(activePayload.tagIndex)),
           ir.getInt32(0)});
      tagPlan.applyTo(*observed);
      bad = ir.CreateOr(bad, ir.CreateICmpNE(observed, ir.getInt32(0)));
    }
    const std::array<SemanticType, 2> allocationParameters{
        SemanticType::UInt64, SemanticType::UInt64};
    const auto allocationPlan =
        session.abi().nativeC(pointer, allocationParameters);
    auto *allocate = llvm::Function::Create(
        allocationPlan.type, llvm::GlobalValue::ExternalLinkage,
        "gti_cleanup_allocate", session.module());
    allocationPlan.applyTo(*allocate);
    const auto allocateBytes = [&](std::uint64_t size,
                                   std::uint64_t alignment) {
      auto *allocation =
          ir.CreateCall(allocate, {ir.getInt64(size), ir.getInt64(alignment)});
      allocationPlan.applyTo(*allocation);
      return allocation;
    };
    const auto itemLayout = session.layouts().get(items.type);
    const auto uniqueItems = SemanticType::uniqueOwnerOf(items.type);
    auto uniqueRecipe =
        mirUniqueCleanupType(session.program().mir(), uniqueItems);
    auto sharedRecipe =
        mirUniqueCleanupType(session.program().mir(), staticShared->type);
    if (!uniqueRecipe || !sharedRecipe)
      throw std::runtime_error("static owner native probe lost exact cleanup");
    auto *uniqueCleanup = lifetimes.cleanup(*uniqueRecipe);
    auto *sharedCleanup = lifetimes.cleanup(*sharedRecipe);
    auto *channel = ir.CreateAlloca(session.abi().failureChannel());
    const auto freshHeap = [&] {
      auto *allocation = allocateBytes(itemLayout.size, itemLayout.alignment);
      ir.CreateStore(llvm::Constant::getNullValue(storage), allocation);
      for (unsigned index = 0; index != 2; ++index) {
        auto *value = element(allocation, index);
        ir.CreateStore(ir.getInt32(17 + index), at(value, numberOffset));
        ir.CreateStore(ir.getInt32(25 + index), at(value, extraOffset));
        dispatch.install(ir, value, item.id);
      }
      return allocation;
    };
    auto *heap = freshHeap();
    auto *uniqueSlot = ir.CreateAlloca(session.layouts().pointer());
    ir.CreateStore(heap, uniqueSlot);
    for (int attempt = 0; attempt != 2; ++attempt) {
      bad = ir.CreateOr(
          bad, ir.CreateNot(ir.CreateCall(
                   uniqueCleanup, {uniqueSlot, channel, ir.getFalse()})));
      bad = ir.CreateOr(bad, ir.CreateIsNotNull(ir.CreateLoad(
                                 session.layouts().pointer(), uniqueSlot)));
    }
    heap = freshHeap();
    auto *block = allocateBytes(shared.block.size, shared.block.alignment);
    ir.CreateStore(llvm::Constant::getNullValue(shared.block.storage), block);
    auto *count =
        ir.CreateStructGEP(shared.block.storage, block, shared.blockStrong);
    ir.CreateStore(ir.getInt64(2), count);
    ir.CreateStore(heap, ir.CreateStructGEP(shared.block.storage, block,
                                            shared.blockAllocation));
    llvm::Value *handle = llvm::Constant::getNullValue(shared.handle.storage);
    handle = ir.CreateInsertValue(handle, block, shared.handleBlock);
    handle = ir.CreateInsertValue(handle, heap, shared.handleView);
    auto *firstOwner = ir.CreateAlloca(shared.handle.storage);
    auto *lastOwner = ir.CreateAlloca(shared.handle.storage);
    ir.CreateStore(handle, firstOwner);
    ir.CreateStore(handle, lastOwner);
    bad = ir.CreateOr(
        bad, ir.CreateNot(ir.CreateCall(sharedCleanup,
                                        {firstOwner, channel, ir.getFalse()})));
    bad =
        ir.CreateOr(bad, ir.CreateICmpNE(ir.CreateLoad(ir.getInt64Ty(), count),
                                         ir.getInt64(1)));
    bad = ir.CreateOr(
        bad, ir.CreateICmpNE(ir.CreateLoad(ir.getInt32Ty(),
                                           at(element(heap, 0), numberOffset)),
                             ir.getInt32(17)));
    for (auto *owner : {lastOwner, firstOwner, lastOwner}) {
      bad = ir.CreateOr(
          bad, ir.CreateNot(ir.CreateCall(sharedCleanup,
                                          {owner, channel, ir.getFalse()})));
      auto *empty = ir.CreateLoad(shared.handle.storage, owner);
      bad = ir.CreateOr(bad, ir.CreateIsNotNull(ir.CreateExtractValue(
                                 empty, shared.handleBlock)));
      bad = ir.CreateOr(bad, ir.CreateIsNotNull(ir.CreateExtractValue(
                                 empty, shared.handleView)));
    }
    ir.CreateRet(ir.CreateZExt(bad, ir.getInt32Ty()));
    expect(!llvm::verifyModule(session.module()),
           "inert relocation module verifies");
    session.requireDefinition(*probe);
    const auto object = temporary / "probe.o";
    expect(driver::writeArtifact(object, session.emitObject().contents) ==
               driver::ArtifactWriteStatus::Success,
           "write relocation native object");
    driver::NativeInputs inputs;
    inputs.finalLibraryFiles = {
        cobject, driver::discoverToolchainLayout(nullptr).runtimeLibrary};
    const auto executable = temporary / "probe";
    const auto linked = toolchain.invoke(driver::NativeLinkRequest(
        driver::discoverNativeCompiler(), object, executable, inputs));
    if (!linked.succeeded())
      std::cerr << linked.output << '\n';
    expect(linked.succeeded(), "link relocation native object");
    if (linked.succeeded())
      expect(driver::invokeProcess({executable.string()}).succeeded(),
             "inert hierarchy arrays relocate fields and rebuild headers at "
             "O0/O3");
  }
}

} // namespace

int main() {
  testAbiEquality();
  const auto shadow =
      Frontend().analyze("llvm-dispatch-shadow-rejection.gti", std::string(R"(
class Base { public: int value; };
class Derived : public Base { public: int value; };
int main() { return 0; }
)"));
  expect(!shadow.canGenerateCode() &&
             std::any_of(shadow.diagnostics.begin(), shadow.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2043";
                         }),
         "inherited data-member shadowing keeps its existing source rejection");
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto frontend = Frontend().analyze(
      root / "tests/fixtures/llvm_dispatch_static.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!input)
    return 1;
  Session session(*input, {});
  test(session, *input);
  testPlaces(session);
  testCallPlans(session);
  testThunks(session);
  testPureGates();
  testBodyCallHook();
  testStaticLifecycle();
  testCleanupCompositionProofs();
  testDispatchAdmissionBinding();
  testReferencePublication();
  testOrdinaryTransferPlacement();
  testFreshConstructionPlanning();
  testFreshTransferPlanning();
  testNestedFreshTransferPlanning();
  testFreshCopyViewPlanning();
  testExchangedAndReturnedTransferPlanning();
  testCarrierLoanDependencies();
  testInertRelocation();
  testGeneratedEmpty();
  testDeclarationInitializers();
  testProvedDirectReceiver();
  return failures ? 1 : 0;
}
