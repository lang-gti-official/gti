#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/dispatch.h"
#include "../src/compiler/llvm/lifetimes.h"
#include "../src/compiler/llvm/places.h"
#include "cpp_backend_test_support.h"
#include "gti/llvm_backend.h"
#include "gti/mir_editor.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
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
    references.parameter(source.parameterBindings.front(), probe->getArg(0),
                         false);
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
      const auto *initializer =
          llvm::dyn_cast<llvm::ConstantArray>(table->getInitializer());
      if (!initializer)
        throw std::runtime_error(
            "dispatch table lost its immutable slot array");
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
class Reader { public: virtual int read() { return 1; } };
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
  expect(!nestedAdmission.admitted() &&
             std::any_of(nestedAdmission.requirements().begin(),
                         nestedAdmission.requirements().end(),
                         [](const auto &use) {
                           return use.required &&
                                  use.entry->family == "Shape" &&
                                  use.entry->name == "PolymorphicTransfer" &&
                                  !use.implementedShape;
                         }),
         "nested polymorphic field construction retains its explicit pending "
         "transfer requirement");
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
  Session rejectedSession(*nestedInput, {});
  rejects<BackendUnsupportedError>(
      [&] { lowerScalarProgram(rejectedSession, nestedAdmission); },
      "private whole-program lowering also rejects nested polymorphic fields");

  // Keep the constructor/base/failure-body inspection at whole local final
  // destinations. The original nested-field fixture above remains rejected;
  // its independent trivial-child cleanup contract is still checked there.
  const auto frontend = Frontend().analyze("llvm-static-lifecycle-body.gti", R"(
class Token {
  int divisor = 1;
public:
  Token(int value) : divisor(value) {}
  ~Token() { int checked = 12 / this.divisor; }
};
class Base {
public:
  Base(int divisor) {
    int checked = 12 / (divisor + 1);
    int observed = this.phase();
  }
  virtual int phase() { return 1; }
  ~Base() { int observed = this.phase(); }
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
  expect(!admission.admitted(),
         "static lifecycle IR does not widen public admission");
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
  const auto functionsBeforeRejection = session.module().size();
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(lifetimes.cleanup(recipe, true)); },
      "polymorphic replacement cannot silently adopt destruction-phase "
      "dispatch");
  auto excluded = recipe;
  excluded.polymorphicCleanup = true;
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(lifetimes.cleanup(excluded)); },
      "dynamic owner cleanup remains independently gated");
  excluded = recipe;
  excluded.recursiveCleanup = true;
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(lifetimes.cleanup(excluded)); },
      "unknown recursive cleanup cannot become a false observation witness");
  expect(session.module().size() == functionsBeforeRejection,
         "excluded recipes reject before creating cleanup helpers, including "
         "cached replacements");
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

void testDispatchAdmissionBinding() {
  const auto frontend = Frontend().analyze("llvm-dispatch-demand.gti", R"(
class Active { public: virtual int read() { return 7; } };
class Dormant : public Active { public: virtual int extra() { return 8; } };
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
  expect(!admission.admitted(),
         "generated-empty private IR does not widen polymorphic admission");
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
} // namespace

int main() {
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
  testDispatchAdmissionBinding();
  testReferencePublication();
  testGeneratedEmpty();
  testDeclarationInitializers();
  testProvedDirectReceiver();
  return failures ? 1 : 0;
}
