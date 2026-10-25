#include "../src/compiler/llvm/dispatch.h"
#include "../src/compiler/llvm/places.h"
#include "cpp_backend_test_support.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
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
               layouts.get(owner->type)
                       .storage->getStructElementType(view.dispatchIndex) ==
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
      expect(static_cast<std::int64_t>(view.offset) + slot.receiverAdjustment ==
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
  expect(plan.implementations.size() == 2,
         "root ABI retains both implementations in the sealed family");
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
  std::cout << "dispatch-layout-cost: leaf-size=" << layouts.get(leaf.type).size
            << " leaf-alignment=" << layouts.get(leaf.type).alignment
            << " views=" << views.size() << " dispatch-pointer-bytes="
            << views.size() * session.dataLayout().getPointerSize() << '\n';
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
  return failures ? 1 : 0;
}
