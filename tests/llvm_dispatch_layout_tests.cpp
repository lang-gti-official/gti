#include "../src/compiler/llvm/dispatch.h"
#include "../src/compiler/llvm/places.h"
#include "cpp_backend_test_support.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>

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
  Dispatch dispatch(session);
  bool failing = false, direct = false, indirect = false, boolean = false,
       noResult = false;
  for (const auto &active : session.program().mir().classInstances()) {
    if (!active.polymorphic || active.abstract ||
        active.kind == ClassKind::Interface)
      continue;
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
  Dispatch dispatch(session);
  const auto &open = named(session.program(), "Open");
  const auto &closed = named(session.program(), "Closed");
  expect(dispatch.views(open.id).front().slots.front().pure,
         "the abstract active-state plan preserves its pure fact");
  const auto functions = session.module().size();
  const auto globals = session.module().global_size();
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
  Dispatch dispatch(session);
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
  Dispatch other(independent);
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
  testThunks(session);
  testPureGates();
  return failures ? 1 : 0;
}
