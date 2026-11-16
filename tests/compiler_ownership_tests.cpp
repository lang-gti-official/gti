#include "compiler_test_fixture.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "private_stdlib_fixture.h"

#include <cstdlib>
#include <string_view>

namespace {

// A source local's MIR facts: the function body that declares it, its HIR
// binding identity, and the MIR loan it retains (the most derived loan whose
// carriers name that binding).
struct MirRetained {
  const lang::MirFunctionInstance *function = nullptr;
  lang::HirBindingId binding = 0;
  const lang::MirLoan *loan = nullptr;
};

MirRetained mirRetained(const lang::FrontendResult &frontend,
                        const lang::VariableDecl *variable) {
  MirRetained result;
  if (variable == nullptr)
    return result;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    for (const lang::HirBinding &binding : instance.body.bindings) {
      if (binding.variable != variable)
        continue;
      result.binding = binding.id;
      result.function = frontend.mir.findFunctionInstance(instance.id);
      if (result.function == nullptr)
        return result;
      for (const lang::MirLoan &loan : result.function->body.loans)
        if (std::find(loan.carriers.begin(), loan.carriers.end(), binding.id) !=
            loan.carriers.end())
          result.loan = &loan;
      return result;
    }
  }
  return result;
}

const lang::MirPlace *mirLoanSource(const MirRetained &retained) {
  return retained.function == nullptr || retained.loan == nullptr
             ? nullptr
             : retained.function->body.findPlace(retained.loan->source);
}

// The prelude's result class, identified by its declaration rather than by a
// compiler-known type kind.
bool isExpectedClass(const lang::FrontendResult &frontend,
                     const lang::SemanticType &type) {
  if (type.kind != lang::SemanticType::Class)
    return false;
  const lang::ClassTypeInfo *info =
      frontend.semantics.findClassType(type.classId);
  return info != nullptr && info->qualifiedName == "expected";
}

// The fixture's own bodies; prelude instances also appear in HIR and MIR.
bool entryFunction(const lang::FrontendResult &frontend,
                   lang::HirFunctionInstanceId id) {
  const lang::HirFunctionInstance *function =
      frontend.hir.findFunctionInstance(id);
  return function != nullptr &&
         function->sourceUnit == frontend.sourceGraph.entryUnit();
}

// An ordinary call of the named member of the prelude result class.
bool isExpectedMemberCall(const lang::FrontendResult &frontend,
                          const lang::MirInstruction &instruction,
                          std::string_view member) {
  if (instruction.kind != lang::MirInstructionKind::Call ||
      !instruction.functionTarget || !instruction.receiver ||
      !isExpectedClass(frontend, instruction.receiver->type))
    return false;
  const lang::HirFunctionInstance *target =
      frontend.hir.findFunctionInstance(*instruction.functionTarget);
  return target != nullptr && target->source != nullptr &&
         target->source->name().lexeme == member;
}

// The instruction defining a MIR value, such as a staged call input.
const lang::MirInstruction *definingInstruction(const lang::MirBody &body,
                                                lang::MirValueId value) {
  const lang::MirValue *defined = body.findValue(value);
  if (defined == nullptr)
    return nullptr;
  for (const lang::MirBlock &block : body.blocks)
    for (const lang::MirInstruction &instruction : block.instructions)
      if (instruction.id == defined->definition)
        return &instruction;
  return nullptr;
}

bool checkedTemporaryObserver(const lang::FrontendResult &frontend,
                              std::string_view name, bool storedOwner) {
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            !call.functionTarget || call.operands.size() != 1 ||
            call.parameterTypes.size() != 1)
          continue;
        const auto *target =
            frontend.hir.findFunctionInstance(*call.functionTarget);
        const auto *input =
            definingInstruction(function.body, call.operands.front().value);
        const auto *loan = input && input->loan
                               ? function.body.findLoan(*input->loan)
                               : nullptr;
        const auto &parameter = call.parameterTypes.front();
        if (target && target->source && target->source->name().lexeme == name &&
            parameter.kind == lang::SemanticType::Reference &&
            parameter.referenceAccess == lang::AccessMode::ReadOnly && input &&
            input->callSite == call.callSite && input->info.type == parameter &&
            input->callInputRole == lang::MirCallInputRole::Argument &&
            input->callInputKind ==
                lang::HirCallInputKind::ReadTemporaryBorrow &&
            loan && loan->kind == lang::MirLoanKind::CallArgument &&
            loan->access == lang::AccessMode::ReadOnly && !loan->escapes &&
            bool(loan->storedDependency) == storedOwner &&
            call.borrowOrigin == lang::BorrowOriginKind::None)
          return true;
      }
  return false;
}

// The shipped native backend accepts the verified program: MIR verifies and
// the LLVM backend produces an object from the production lowering.
bool nativeBackendEmits(
    const lang::FrontendResult &frontend,
    lang::OptimizationLevel level = lang::OptimizationLevel::O0) {
  if (!frontend.canGenerateCode() ||
      !lang::verifyMirProgram(frontend.mir).valid())
    return false;
  std::string error;
  if (gti_test::llvmEmits(frontend, level, lang::TargetInfo::host(), &error))
    return true;
  std::cerr << "Native emission failed: " << error << '\n';
  return false;
}

// Every MIR function instance of the entry unit whose source is `name`.
std::vector<const lang::MirFunctionInstance *>
mirFunctionsNamed(const lang::FrontendResult &frontend, std::string_view name) {
  std::vector<const lang::MirFunctionInstance *> result;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    if (source != nullptr && source->source != nullptr &&
        entryFunction(frontend, function.id) &&
        source->source->name().lexeme == name)
      result.push_back(&function);
  }
  return result;
}

const lang::MirFunctionInstance *
mirFunctionNamed(const lang::FrontendResult &frontend, std::string_view name) {
  const auto functions = mirFunctionsNamed(frontend, name);
  return functions.empty() ? nullptr : functions.front();
}

// A body MIR lowered from source and that verifies independently: the
// function goes through the verified-MIR path rather than a declaration.
bool hasVerifiedSourceBody(const lang::MirFunctionInstance *function) {
  return function != nullptr &&
         function->definitionKind == lang::MirDefinitionKind::Source &&
         !function->body.blocks.empty() &&
         lang::verifyMirBody(function->body).valid();
}

// Some MIR call selects a member whose operator identity is `op`; when
// `mayRaise` is given the selected target's failure summary must match it.
bool mirCallsOperator(const lang::FrontendResult &frontend,
                      lang::OverloadedOperator op,
                      std::optional<bool> mayRaise = std::nullopt) {
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances())
    for (const lang::MirBlock &block : function.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::MirFunctionInstance *target =
            instruction.kind == lang::MirInstructionKind::Call &&
                    instruction.functionTarget
                ? frontend.mir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (target != nullptr && target->overloadedOperator == op &&
            (!mayRaise || target->mayRaiseDefinedFailure == *mayRaise))
          return true;
      }
  return false;
}

// Some MIR body carries a range iteration satisfying `matches`.
template <typename Predicate>
bool mirHasRangeIteration(const lang::FrontendResult &frontend,
                          Predicate matches) {
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances())
    for (const lang::MirRangeIteration &range : function.body.rangeIterations)
      if (matches(function.body, range))
        return true;
  return false;
}

// Some MIR call invokes a callable with exactly `capability`.
bool mirHasCallableInvocation(const lang::FrontendResult &frontend,
                              lang::CallableInvocationCapability capability) {
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances())
    for (const lang::MirBlock &block : function.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.callableInvocation == capability)
          return true;
  return false;
}

void testOwnershipSemanticFoundation() {
  const lang::SemanticType reference = lang::SemanticType::referenceTo(
      lang::SemanticType::Int32, lang::AccessMode::Mutable);
  const lang::SemanticType unique =
      lang::SemanticType::uniqueOwnerOf(lang::SemanticType::Int32);
  const lang::SemanticType shared =
      lang::SemanticType::sharedPointerTo(lang::SemanticType::Int32);
  const lang::SemanticTypeTraits referenceTraits =
      lang::semanticTraits(reference);
  const lang::SemanticTypeTraits uniqueTraits = lang::semanticTraits(unique);
  const lang::SemanticTypeTraits sharedTraits = lang::semanticTraits(shared);
  expect(reference.kind == lang::SemanticType::Reference &&
             reference.referenceAccess == lang::AccessMode::Mutable &&
             referenceTraits.ownership == lang::OwnershipKind::Borrowed &&
             referenceTraits.drop == lang::DropKind::Trivial,
         "references should carry borrowed access semantics");
  expect(uniqueTraits.ownership == lang::OwnershipKind::Unique &&
             uniqueTraits.drop == lang::DropKind::Lexical &&
             !uniqueTraits.copyable && uniqueTraits.movable,
         "unique pointers should be move-only lexical owners");
  expect(sharedTraits.ownership == lang::OwnershipKind::Shared &&
             sharedTraits.drop == lang::DropKind::Lexical &&
             sharedTraits.copyable && sharedTraits.movable,
         "shared pointers should be copyable lexical owners");

  lang::FrontendResult frontend =
      lang::Frontend().analyze("ownership-foundation.gti", R"(
struct Counter {
  mut int value = 0;
  void bump() mut { this.value += 1; }
public:
  Counter() = default;
};

int identity(int value) { return value; }

int main() {
  int fixed = 1;
  mut int changing = 2;
  mut Counter counter = Counter();
  int grouped = (fixed);
  changing++;
  counter.bump();
  return identity(changing);
}
)");
  expect(frontend.canGenerateCode(),
         "ownership metadata source should pass the frontend");
  // Six source bindings plus the struct's generated designated constructor
  // parameter and its reversed sibling's tag and field parameters.
  expect(frontend.semantics.bindingCount() == 9,
         "the semantic model should retain six source bindings and the "
         "struct's generated designated-construction parameters");

  const auto *identity = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(1).get());
  const auto *main = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(2).get());
  expect(identity != nullptr && main != nullptr,
         "ownership metadata fixture should retain its functions");
  if (identity == nullptr || main == nullptr) {
    return;
  }

  const lang::BindingInfo *parameter =
      frontend.semantics.findBinding(identity->parameters().front());
  expect(parameter != nullptr &&
             parameter->access == lang::AccessMode::ReadOnly &&
             parameter->traits.ownership == lang::OwnershipKind::Value,
         "parameters should expose binding access and ownership metadata");

  const lang::StmtList &statements = main->body()->statements();
  const auto *fixed =
      dynamic_cast<const lang::VariableDecl *>(statements.at(0).get());
  const auto *changing =
      dynamic_cast<const lang::VariableDecl *>(statements.at(1).get());
  const auto *counter =
      dynamic_cast<const lang::VariableDecl *>(statements.at(2).get());
  const auto *grouped =
      dynamic_cast<const lang::VariableDecl *>(statements.at(3).get());
  const auto *increment =
      dynamic_cast<const lang::ExpressionStmt *>(statements.at(4).get());
  const auto *methodCall =
      dynamic_cast<const lang::ExpressionStmt *>(statements.at(5).get());
  expect(fixed != nullptr && changing != nullptr && counter != nullptr &&
             grouped != nullptr && increment != nullptr &&
             methodCall != nullptr,
         "ownership metadata fixture should retain its statement shapes");
  if (fixed == nullptr || changing == nullptr || counter == nullptr ||
      grouped == nullptr || increment == nullptr || methodCall == nullptr) {
    return;
  }

  const lang::BindingInfo *fixedBinding =
      frontend.semantics.findBinding(*fixed);
  const lang::BindingInfo *changingBinding =
      frontend.semantics.findBinding(*changing);
  const lang::BindingInfo *counterBinding =
      frontend.semantics.findBinding(*counter);
  expect(fixedBinding != nullptr &&
             fixedBinding->access == lang::AccessMode::ReadOnly &&
             fixedBinding->traits.drop == lang::DropKind::Trivial &&
             changingBinding != nullptr &&
             changingBinding->access == lang::AccessMode::Mutable &&
             counterBinding != nullptr &&
             counterBinding->traits.drop == lang::DropKind::Lexical,
         "bindings should distinguish access and lexical drop requirements");

  const lang::ExpressionInfo counterConstruction =
      frontend.semantics.expressionInfo(*counter->initializer());
  const lang::ExpressionInfo groupedInfo =
      frontend.semantics.expressionInfo(*grouped->initializer());
  const auto *postfix =
      dynamic_cast<const lang::Postfix *>(increment->expression().get());
  const auto *call =
      dynamic_cast<const lang::Call *>(methodCall->expression().get());
  expect(counterConstruction.category == lang::ValueCategory::Value &&
             counterConstruction.traits.drop == lang::DropKind::Lexical &&
             groupedInfo.category == lang::ValueCategory::Place &&
             groupedInfo.access == lang::AccessMode::ReadOnly,
         "expressions should distinguish owned values from borrowed places");
  expect(
      postfix != nullptr && call != nullptr,
      "ownership metadata fixture should retain postfix and call expressions");
  if (postfix == nullptr || call == nullptr) {
    return;
  }

  const lang::ExpressionInfo incrementTarget =
      frontend.semantics.expressionInfo(*postfix->expression());
  const auto *member = dynamic_cast<const lang::Get *>(call->callee().get());
  expect(incrementTarget.category == lang::ValueCategory::Place &&
             incrementTarget.access == lang::AccessMode::Mutable &&
             member != nullptr,
         "mutable bindings should produce mutable place expressions");
  if (member == nullptr) {
    return;
  }
  const lang::ExpressionInfo receiver =
      frontend.semantics.expressionInfo(*member->object());
  const lang::ExpressionInfo memberFunction =
      frontend.semantics.expressionInfo(*member);
  expect(
      receiver.category == lang::ValueCategory::Place &&
          receiver.access == lang::AccessMode::Mutable &&
          memberFunction.category == lang::ValueCategory::Value &&
          frontend.semantics.findType(*member) != nullptr,
      "method analysis should preserve receiver access and callable metadata");
}

void testDisjointFieldReceiverBorrowsThroughReferenceParameters() {
  // Receiver borrows of disjoint fields reached through a mutable reference
  // parameter: semantics accepts them, and the lowered receiver-origin loan
  // places must spell the reference dereference explicitly so the backend
  // can key each field projection to its exact concrete class. Dropping the
  // dereference during borrow-source canonicalization previously composed a
  // field projection directly onto the reference-typed carrier, which the
  // C++ emitter declined as GTI-B0001.
  const lang::FrontendResult disjoint =
      lang::Frontend().analyze("disjoint-field-reference-receivers.gti", R"(
class Box {
  mut int value;

public:
  Box(int initial) : value(initial) {}
  mut int& write() mut { return this.value; }
};

class Boxes {
public:
  mut Box left = Box(1);
  mut Box right = Box(2);
public:
  Boxes() = default;
};

class Inner {
public:
  mut Box first = Box(3);
  mut Box second = Box(4);
public:
  Inner() = default;
};

class Outer {
public:
  mut Inner left = Inner();
  mut Inner right = Inner();
public:
  Outer() = default;
};

int sum(int& left, int& right) { return left + right; }

int disjoint_writes(mut Boxes& parent) {
  return sum(parent.left.write(), parent.right.write());
}

int nested_writes(mut Outer& parent) {
  return sum(parent.left.first.write(), parent.right.second.write());
}

int main() {
  mut Boxes pair = Boxes();
  mut Outer outer = Outer();
  return disjoint_writes(pair) + nested_writes(outer) - 10;
}
)");
  expect(disjoint.canGenerateCode() &&
             lang::verifyMirProgram(disjoint.mir).valid(),
         "mutable receiver borrows of disjoint fields through a reference "
         "parameter should stay accepted through MIR verification");

  const auto mirBodyFor =
      [&](const std::string &name) -> const lang::MirBody * {
    const lang::FunctionDecl *function =
        findTopLevelFunction(disjoint.program, name);
    for (const lang::HirFunctionInstance &candidate :
         disjoint.hir.functionInstances()) {
      if (candidate.source == function) {
        const lang::MirFunctionInstance *instance =
            disjoint.mir.findFunctionInstance(candidate.id);
        return instance == nullptr ? nullptr : &instance->body;
      }
    }
    return nullptr;
  };
  for (const std::string name : {"disjoint_writes", "nested_writes"}) {
    const lang::MirBody *body = mirBodyFor(name);
    std::vector<const lang::MirPlace *> sources;
    if (body != nullptr) {
      for (const lang::MirLoan &loan : body->loans) {
        if (loan.kind != lang::MirLoanKind::CallResult) {
          continue;
        }
        sources.push_back(body->findPlace(loan.source));
      }
    }
    const bool exactSpelling =
        sources.size() == 2 &&
        std::all_of(
            sources.begin(), sources.end(),
            [](const lang::MirPlace *place) {
              return place != nullptr && place->projections.size() >= 2 &&
                     place->projections.front().kind ==
                         lang::MirProjectionKind::Dereference &&
                     std::all_of(place->projections.begin() + 1,
                                 place->projections.end(),
                                 [](const lang::MirPlaceProjection &field) {
                                   return field.kind ==
                                              lang::MirProjectionKind::Field &&
                                          field.field != 0;
                                 });
            }) &&
        sources.front() != nullptr && sources.back() != nullptr &&
        sources.front()->projections.back().field !=
            sources.back()->projections.back().field;
    expect(body != nullptr && exactSpelling,
           "receiver-origin loan places in " + name +
               " should spell the reference dereference and their exact "
               "disjoint field projections");
  }

  if (disjoint.canGenerateCode()) {
    expect(nativeBackendEmits(disjoint),
           "the native backend should emit disjoint field receiver borrows "
           "reached through reference parameters");
  }
}

void testGlobalReferencePlaces() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("global-reference-places.gti", R"(
constexpr int direct_value = 7;
static constexpr int internal_value = 8;

namespace values {
constexpr int qualified_value = 9;
}

class Constants {
public:
  static constexpr int member_value = 10;
public:
  Constants() = default;
};

mut int slots[2] = {1, 2};

int main() {
  int& direct = direct_value;
  int& internal = internal_value;
  int& qualified = values::qualified_value;
  int& member = Constants::member_value;
  mut int& first = slots[0];
  mut int& second = slots[1];
  first += 1;
  second += 1;
  return direct + internal + qualified + member + first + second - 39;
}
)");
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected global-reference-place diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.mirValid && valid.mir.valid(),
         "references to unqualified, namespace-qualified, internal, static "
         "member, and projected global storage should lower through valid "
         "MIR places");

  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const std::array<std::string_view, 6> aliases{
      "direct", "internal", "qualified", "member", "first", "second"};
  std::array<const lang::MirPlace *, 6> loanSources{};
  bool mirLoansUseSymbolPlaces = true;
  for (std::size_t index = 0; index < aliases.size(); ++index) {
    const lang::VariableDecl *alias =
        findDirectLocal(main, std::string(aliases[index]));
    loanSources[index] = mirLoanSource(mirRetained(valid, alias));
    mirLoansUseSymbolPlaces =
        mirLoansUseSymbolPlaces && loanSources[index] != nullptr &&
        loanSources[index]->root == lang::MirPlaceRootKind::Symbol &&
        loanSources[index]->symbol != 0;
  }
  expect(mirLoansUseSymbolPlaces,
         "MIR loans should retain a nonzero concrete symbol-rooted place for "
         "every borrowed global or static binding");
  const auto constantIndex = [](const lang::MirPlace *place,
                                std::uint64_t index) {
    return place != nullptr && place->projections.size() == 1 &&
           place->projections.front().kind == lang::MirProjectionKind::Index &&
           place->projections.front().constantIndex == index;
  };
  expect(constantIndex(loanSources[4], 0) && constantIndex(loanSources[5], 1) &&
             loanSources[4]->symbol == loanSources[5]->symbol,
         "global fixed-array borrows should retain one shared symbol root "
         "with disjoint constant-index projections");

  const lang::FrontendResult conflicting =
      lang::Frontend().analyze("conflicting-global-reference-places.gti", R"(
namespace state {
mut int value = 1;
}

int main() {
  mut int& alias = state::value;
  state::value = 2;
  return alias;
}
)");
  expect(!conflicting.canGenerateCode() &&
             hasDiagnosticCode(conflicting.diagnostics, "GTI-S2017") &&
             hasDiagnostic(
                 conflicting.diagnostics,
                 "Cannot assign to storage while an overlapping borrow") &&
             !hasDiagnosticCode(conflicting.diagnostics, "GTI-B0001"),
         "a conflicting qualified-global write should be rejected by "
         "semantic loan analysis instead of reaching the MIR verifier");

  const lang::FrontendResult conflictingRead =
      lang::Frontend().analyze("read-global-reference-places.gti", R"(
mut int value = 1;

int main() {
  mut int& alias = value;
  int copy = value;
  return alias + copy;
}
)");
  expect(!conflictingRead.canGenerateCode() &&
             hasDiagnosticCode(conflictingRead.diagnostics, "GTI-S2017") &&
             hasDiagnostic(conflictingRead.diagnostics,
                           "Cannot read storage while an overlapping mutable "
                           "borrow") &&
             !hasDiagnosticCode(conflictingRead.diagnostics, "GTI-B0001"),
         "a conflicting unqualified-global read should be rejected by "
         "semantic loan analysis before MIR lowering");
}

void testGlobalBorrowReturns() {
  const std::string source = R"(
class LayerStack {
public:
  void Update() mut {}
public:
  LayerStack() = default;
};

class Application {
public:
  static mut Application& Get() {
    unsafe {
      return *Application::app;
    }
  }

  mut LayerStack& GetLayerStack() mut { return this.layers; }

private:
  mut LayerStack layers = LayerStack();
  static mut Application* app = nullptr;
public:
  Application() = default;
};

mut int global_value = 1;
mut int& global_value_ref() { return global_value; }

void update_application() {
  Application::Get().GetLayerStack().Update();
  Application::Get().GetLayerStack().Update();
}

void update_application_in_scopes() {
  {
    mut Application& first = Application::Get();
    first.GetLayerStack().Update();
  }
  mut Application& second = Application::Get();
  second.GetLayerStack().Update();
}

int main() {
  mut int& value = global_value_ref();
  value += 1;
  return value - 2;
}
)";
  const lang::FrontendResult valid =
      lang::Frontend().analyze("global-borrow-returns.gti", source);
  const lang::MirVerificationResult validMirVerification =
      lang::verifyMirProgram(valid.mir);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected global-borrow-return diagnostic: "
                << diagnostic.primary.start << ": " << diagnostic.message
                << '\n';
    }
  }
  if (!validMirVerification.valid()) {
    for (const lang::MirVerificationError &error :
         validMirVerification.errors) {
      std::cerr << "Unexpected global-borrow MIR error: " << error.owner << ':'
                << error.block << ':' << error.instruction << ' '
                << error.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.mirValid && valid.mir.valid() &&
             validMirVerification.valid() &&
             !hasDiagnosticCode(valid.diagnostics, "GTI-S2055"),
         "an unsafe global/static dereference should be contained by its "
         "accessor while safe callers receive a checked mutable borrow");

  const lang::ClassDecl *application =
      findTopLevelClass(valid.program, "Application");
  const lang::FunctionDecl *get = nullptr;
  if (application != nullptr) {
    for (const lang::StmtPtr &member : application->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == "Get") {
        get = function;
        break;
      }
    }
  }
  const lang::FunctionInfo *getInfo =
      get == nullptr ? nullptr : valid.semantics.findFunction(*get);
  const bool exactSemanticOrigin =
      getInfo != nullptr &&
      getInfo->returnBorrowOrigin == lang::BorrowOriginKind::Global &&
      getInfo->returnBorrowAccess == lang::AccessMode::Mutable &&
      getInfo->returnBorrowPlace && getInfo->returnBorrowPlace->root != 0 &&
      getInfo->returnBorrowPlace->projections.size() == 1 &&
      getInfo->returnBorrowPlace->projections.front().kind ==
          lang::PlaceProjectionKind::Dereference;
  expect(exactSemanticOrigin,
         "semantic function metadata should retain the exact dereferenced "
         "static-field origin");
  const std::optional<lang::BorrowOriginPlace> getPlace =
      getInfo == nullptr ? std::nullopt : getInfo->returnBorrowPlace;

  const lang::HirFunctionInstance *getHir = nullptr;
  if (getInfo != nullptr) {
    for (const lang::HirFunctionInstance &instance :
         valid.hir.functionInstances()) {
      if (instance.declaration == getInfo->id) {
        getHir = &instance;
        break;
      }
    }
  }
  const lang::MirFunctionInstance *getMir =
      getHir == nullptr ? nullptr : valid.mir.findFunctionInstance(getHir->id);
  if (!validMirVerification.valid() && getMir != nullptr) {
    std::cerr << lang::MirPrinter().print(getMir->body) << '\n';
  }
  expect(getHir != nullptr && getMir != nullptr &&
             getHir->returnBorrowOrigin == lang::BorrowOriginKind::Global &&
             getHir->returnBorrowPlace == getInfo->returnBorrowPlace &&
             getMir->returnBorrowOrigin == lang::BorrowOriginKind::Global &&
             getMir->returnBorrowPlace == getInfo->returnBorrowPlace,
         "HIR and MIR function contracts should preserve the exact global "
         "borrow origin");

  if (getHir != nullptr && getMir != nullptr && getMir->returnBorrowPlace) {
    lang::MirProgram forged = valid.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    lang::MirFunctionInstance &forgedGet = functions.at(getHir->id - 1);
    ++forgedGet.returnBorrowPlace->root;
    const lang::MirVerificationResult forgedResult =
        lang::verifyMirProgram(forged);
    expect(!forgedResult.valid() &&
               std::any_of(forgedResult.errors.begin(),
                           forgedResult.errors.end(),
                           [](const lang::MirVerificationError &error) {
                             return error.message.find(
                                        "summarized static storage place") !=
                                    std::string::npos;
                           }),
           "the MIR verifier should reject a forged global return origin");
  }

  std::size_t globalCalls = 0;
  std::size_t endedGlobalCalls = 0;
  for (const lang::MirFunctionInstance &instance :
       valid.mir.functionInstances()) {
    std::vector<lang::MirLoanId> callLoans;
    for (const lang::MirBlock &block : instance.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.borrowOrigin == lang::BorrowOriginKind::Global &&
            instruction.borrowPlace == getPlace && instruction.loan) {
          ++globalCalls;
          callLoans.push_back(*instruction.loan);
        }
      }
    }
    for (const lang::MirLoanId loan : callLoans) {
      const bool ended = std::any_of(
          instance.body.blocks.begin(), instance.body.blocks.end(),
          [&](const lang::MirBlock &block) {
            return std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [&](const lang::MirInstruction &instruction) {
                  return instruction.kind ==
                             lang::MirInstructionKind::EndBorrow &&
                         instruction.loan == loan;
                });
          });
      endedGlobalCalls += ended ? 1 : 0;
    }
  }
  expect(globalCalls >= 2 && endedGlobalCalls == globalCalls,
         "unretained global call-result loans should end at their enclosing "
         "full-expression boundary");

  const std::string mirDump = lang::MirPrinter().print(valid.mir);
  expect(mirDump.starts_with("mir-v121 ") &&
             mirDump.find("return-borrow-place=origin(root=") !=
                 std::string::npos &&
             mirDump.find("borrow-place=origin(root=") != std::string::npos,
         "deterministic MIR should serialize global function and call origin "
         "places");

  // Reading the static pointer cannot fail, so the accessor is a safe,
  // failure-free static member whose every return publishes its loan.
  const bool returnsLoan =
      getMir != nullptr &&
      std::all_of(getMir->body.blocks.begin(), getMir->body.blocks.end(),
                  [](const lang::MirBlock &block) {
                    return block.terminator.kind !=
                               lang::MirTerminatorKind::Return ||
                           block.terminator.returnLoan.has_value();
                  });
  const lang::MirFunctionInstance *updateMir =
      mirFunctionNamed(valid, "update_application");
  expect(getMir != nullptr && getMir->staticMember && !getMir->requiresUnsafe &&
             !getMir->mayRaiseDefinedFailure && returnsLoan &&
             hasVerifiedSourceBody(getMir) && updateMir != nullptr &&
             !updateMir->requiresUnsafe && nativeBackendEmits(valid),
         "the native backend should emit the static mutable-reference "
         "accessor without exposing unsafe syntax at its call sites");

  const lang::FrontendResult overlapping =
      lang::Frontend().analyze("overlapping-global-borrow-returns.gti", R"(
class Application {
public:
  static mut Application& Get() {
    unsafe { return *Application::app; }
  }
private:
  static mut Application* app = nullptr;
public:
  Application() = default;
};
int main() {
  mut Application& first = Application::Get();
  mut Application& second = Application::Get();
  return 0;
}
)");
  // Under MIR authority the semantic persistent-storage check, which also
  // names the protected borrow's origin, rejects the second call first.
  expect(!overlapping.canGenerateCode() &&
             ((countDiagnosticCode(overlapping.diagnostics, "GTI-S2017") == 1 &&
               hasDiagnostic(overlapping.diagnostics,
                             "Cannot create a mutable borrow while an "
                             "overlapping borrow") &&
               hasRelatedDiagnostic(overlapping.diagnostics,
                                    "Retained borrow originates here")) ||
              (hasDiagnostic(overlapping.diagnostics,
                             "while a conflicting borrow is live") &&
               hasRelatedDiagnostic(overlapping.diagnostics,
                                    "protected borrow originates here"))),
         "two retained mutable borrows from one global accessor should "
         "conflict even when the first carrier is otherwise unused");

  const lang::FrontendResult missingUnsafe =
      lang::Frontend().analyze("unsafe-global-borrow-return.gti", R"(
class Application {
public:
  static mut Application& Get() { return *Application::app; }
private:
  static mut Application* app = nullptr;
public:
  Application() = default;
};
int main() { return 0; }
)");
  expect(!missingUnsafe.canGenerateCode() &&
             countDiagnosticCode(missingUnsafe.diagnostics, "GTI-S2055") == 1 &&
             !hasDiagnostic(missingUnsafe.diagnostics,
                            "A free or static borrowed return requires"),
         "the accessor body should still require unsafe for its raw "
         "dereference while retaining a valid global borrow summary");

  const lang::FrontendResult mixedOrigins =
      lang::Frontend().analyze("mixed-global-borrow-returns.gti", R"(
class Values {
public:
  static mut int left = 1;
  static mut int right = 2;
  static mut int& pick(bool choose) {
    if (choose) { return Values::left; }
    return Values::right;
  }
public:
  Values() = default;
};
int main() { return 0; }
)");
  expect(!mixedOrigins.canGenerateCode() &&
             hasDiagnosticCode(mixedOrigins.diagnostics, "GTI-S2017") &&
             hasDiagnostic(mixedOrigins.diagnostics,
                           "requires either one exact global/static storage "
                           "origin"),
         "a global borrow accessor should reject differing return roots");

  const lang::FrontendResult noForwardLookup =
      lang::Frontend().analyze("forward-global-borrow-return.gti", R"(
mut int& value_ref() { return value; }
mut int value = 1;
int main() { return 0; }
)");
  expect(
      !noForwardLookup.canGenerateCode() &&
          hasDiagnosticCode(noForwardLookup.diagnostics, "GTI-S2001") &&
          !hasDiagnosticCode(noForwardLookup.diagnostics, "GTI-S2017") &&
          hasDiagnostic(noForwardLookup.diagnostics, "Undefined name 'value'"),
      "global borrow summary registration must not make namespace storage "
      "forward-declarable or cascade an ownership diagnostic in ordinary "
      "source lookup");

  lang::FrontendOptions concurrentOptions;
  concurrentOptions.target = lang::TargetInfo::host();
  concurrentOptions.target.executionProfile =
      lang::ExecutionProfile::Concurrent;
  const lang::FrontendResult concurrent =
      lang::Frontend(concurrentOptions)
          .analyze("concurrent-global-borrow-return.gti", R"(
class Application {
public:
  static mut Application& Get() {
    unsafe { return *Application::app; }
  }
private:
  static mut Application* app = nullptr;
public:
  Application() = default;
};
int main() { return 0; }
)");
  expect(!concurrent.canGenerateCode() &&
             hasDiagnosticCode(concurrent.diagnostics, "GTI-S2060") &&
             hasDiagnostic(concurrent.diagnostics,
                           "Concurrent execution profile requires static "
                           "field 'app'"),
         "the concurrent profile should reject unsynchronized mutable "
         "global/static accessor storage");
}

void testArgumentTiedReferenceReturns() {
  // A free function whose borrowed return derives from its single read-only
  // reference parameter returns that parameter's seeded entry loan. An entry
  // loan has no producing instruction, so reclassifying it in place as the
  // escaping Return loan left a body in which nothing ever named the address
  // the return published: the emitted callee returned a null pointer and
  // every caller that read through the result crashed. Lowering publishes a
  // fresh Return loan from the entry loan and retires the entry loan at the
  // same instruction boundary instead.
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("argument-tied-reference-returns.gti", R"(
class Box {
  mut int32_t value;

public:
  Box(int32_t initial) : value(initial) {}
  int32_t& observe() { return this.value; }
  int32_t read() { return this.value; }
};

int32_t& pick(int32_t& candidate) { return candidate; }

int32_t& forward(int32_t& candidate) { return pick(candidate); }

int32_t retain_argument_return() {
  mut int32_t local = 7;
  int32_t& chosen = pick(local);
  return chosen;
}

int32_t retain_forwarded_return() {
  mut int32_t local = 11;
  int32_t& chosen = forward(local);
  return chosen;
}

int32_t retain_call_result_argument() {
  mut Box box = Box(13);
  int32_t& chosen = pick(box.observe());
  return chosen;
}

int main() {
  return retain_argument_return() + retain_forwarded_return() +
         retain_call_result_argument() - 31;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected argument-tied reference diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "a borrowed return derived from a read-only reference parameter "
         "should lower to verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto mirBodyFor =
      [&](const std::string &name) -> const lang::MirBody * {
    const lang::FunctionDecl *function =
        findTopLevelFunction(frontend.program, name);
    for (const lang::HirFunctionInstance &candidate :
         frontend.hir.functionInstances()) {
      if (candidate.source != function) {
        continue;
      }
      const lang::MirFunctionInstance *instance =
          frontend.mir.findFunctionInstance(candidate.id);
      return instance == nullptr ? nullptr : &instance->body;
    }
    return nullptr;
  };

  // No body may record a loan that is simultaneously the body-entry
  // capability and the escaping return dependency: that is the exact shape
  // whose address nothing publishes.
  bool entryReturnLoan = false;
  for (const lang::MirFunctionInstance &instance :
       frontend.mir.functionInstances()) {
    for (const lang::MirLoan &loan : instance.body.loans) {
      if (loan.entry && loan.kind == lang::MirLoanKind::Return) {
        entryReturnLoan = true;
      }
    }
  }
  expect(!entryReturnLoan,
         "no MIR body should reclassify a seeded entry loan as its escaping "
         "Return loan");

  const lang::MirBody *picked = mirBodyFor("pick");
  expect(picked != nullptr,
         "the argument-tied fixture should retain the MIR body of pick");
  if (picked == nullptr) {
    return;
  }

  const lang::MirLoan *entryLoan = nullptr;
  const lang::MirLoan *returnLoan = nullptr;
  for (const lang::MirLoan &loan : picked->loans) {
    if (loan.entry && loan.kind == lang::MirLoanKind::Parameter) {
      entryLoan = &loan;
    } else if (loan.kind == lang::MirLoanKind::Return) {
      returnLoan = &loan;
    }
  }
  expect(entryLoan != nullptr && returnLoan != nullptr &&
             returnLoan->parent == 0 && returnLoan->escapes &&
             !returnLoan->entry && returnLoan->source == entryLoan->source &&
             returnLoan->access == entryLoan->access,
         "pick should publish a fresh parentless escaping Return loan from "
         "its reference parameter's entry loan");
  if (entryLoan == nullptr || returnLoan == nullptr) {
    return;
  }

  // The published Return loan must be produced by a Borrow that names the
  // entry loan and is immediately followed by that entry loan's retirement:
  // the transfer, not a second independent root borrow.
  bool publishedTransfer = false;
  bool returnsPublishedLoan = false;
  for (const lang::MirBlock &block : picked->blocks) {
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const lang::MirInstruction &instruction = block.instructions[index];
      if (instruction.kind != lang::MirInstructionKind::Borrow ||
          instruction.loan != returnLoan->id ||
          instruction.operands.size() != 1) {
        continue;
      }
      const lang::MirOperand &source = instruction.operands.front();
      if (source.kind != lang::MirOperandKind::Loan ||
          source.loan != entryLoan->id ||
          index + 1 >= block.instructions.size()) {
        continue;
      }
      const lang::MirInstruction &retire = block.instructions[index + 1];
      publishedTransfer = retire.kind == lang::MirInstructionKind::EndBorrow &&
                          retire.loan == entryLoan->id &&
                          retire.hirValue == instruction.hirValue;
    }
    if (block.terminator.kind == lang::MirTerminatorKind::Return &&
        block.terminator.returnLoan &&
        *block.terminator.returnLoan == returnLoan->id) {
      returnsPublishedLoan = true;
    }
  }
  expect(publishedTransfer && returnsPublishedLoan,
         "pick should forward its entry loan into the published Return loan "
         "and retire the entry loan at that same instruction boundary");

  // The regression the runtime test catches: without an instruction that
  // names it, the callee published a default-initialized address.
  expect(nativeBackendEmits(frontend),
         "the native backend should accept the published Return loan of the "
         "reference parameter's carrier");
}

void testNestedMemberReferenceReturnForwarding() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "nested-member-reference-return.gti", R"(
class Inner {
  mut int elements[2] = {2, 3};

public:
  mut int& operator[](uint64_t index) mut { return this.elements[index]; }
public:
  Inner() = default;
};

class Outer {
  mut Inner data = Inner();

public:
  mut int& operator[](uint64_t index) mut { return this.data[index]; }
public:
  Outer() = default;
};

int main() {
  mut Outer outer = Outer();
  outer[uint64_t(1)] = 11;
  return outer[uint64_t(1)] - 11;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected nested-return diagnostic: " << diagnostic.code
                << ' ' << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "a member returning another class's borrowed subscript result "
         "should lower and verify");

  // The outer subscript summarizes the transient call-result chain into a
  // fresh Return loan; the publishing Borrow must forward the runtime
  // pointer through a Loan operand naming the call-result loan and retire
  // that loan at the same boundary — never re-borrow the provenance place.
  const lang::MirFunctionInstance *forwardingInstance = nullptr;
  const lang::MirLoan *forwardedLoan = nullptr;
  const lang::MirLoan *publishedLoan = nullptr;
  const lang::MirInstruction *forwardingBorrow = nullptr;
  bool retiredAtBoundary = false;
  bool publishedAtReturn = false;
  for (const lang::MirFunctionInstance &function :
       valid.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind != lang::MirInstructionKind::Borrow ||
            !instruction.loan || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Loan) {
          continue;
        }
        const lang::MirLoan *produced =
            function.body.findLoan(*instruction.loan);
        const lang::MirLoan *retained =
            function.body.findLoan(instruction.operands.front().loan);
        if (produced == nullptr || retained == nullptr ||
            produced->kind != lang::MirLoanKind::Return ||
            retained->kind != lang::MirLoanKind::CallResult) {
          continue;
        }
        expect(forwardingInstance == nullptr,
               "exactly one body should publish a forwarded call-result "
               "return loan in the nested-subscript fixture");
        forwardingInstance = &function;
        forwardedLoan = retained;
        publishedLoan = produced;
        forwardingBorrow = &instruction;
        retiredAtBoundary = index + 1 < block.instructions.size() &&
                            block.instructions[index + 1].kind ==
                                lang::MirInstructionKind::EndBorrow &&
                            block.instructions[index + 1].loan == retained->id;
      }
    }
  }
  if (forwardingInstance != nullptr && publishedLoan != nullptr) {
    for (const lang::MirBlock &block : forwardingInstance->body.blocks) {
      publishedAtReturn =
          publishedAtReturn ||
          (block.terminator.kind == lang::MirTerminatorKind::Return &&
           block.terminator.returnLoan == publishedLoan->id);
    }
  }
  expect(forwardingInstance != nullptr && forwardedLoan != nullptr &&
             publishedLoan != nullptr && forwardingBorrow != nullptr &&
             retiredAtBoundary && publishedAtReturn && publishedLoan->escapes &&
             publishedLoan->parent == 0 &&
             publishedLoan->source == forwardedLoan->source &&
             publishedLoan->access == forwardedLoan->access,
         "the summarized reference return should forward the call-result "
         "loan's runtime pointer and retire it at the same boundary");

  const auto mutateForward = [&](const auto &mutation) {
    lang::MirProgram forged = valid.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    lang::MirFunctionInstance *candidate = nullptr;
    if (forwardingInstance != nullptr) {
      const auto found =
          std::find_if(functions.begin(), functions.end(),
                       [&](const lang::MirFunctionInstance &instance) {
                         return instance.id == forwardingInstance->id;
                       });
      if (found != functions.end()) {
        candidate = &*found;
      }
    }
    if (candidate != nullptr) {
      mutation(*candidate);
    }
    return std::pair{candidate != nullptr, lang::verifyMirProgram(forged)};
  };

  const lang::MirLoanId publishedId =
      publishedLoan == nullptr ? 0 : publishedLoan->id;
  const auto forgedOperandLoan =
      mutateForward([publishedId](lang::MirFunctionInstance &fn) {
        for (lang::MirBlock &block : fn.body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            if (instruction.kind == lang::MirInstructionKind::Borrow &&
                instruction.loan == publishedId &&
                !instruction.operands.empty()) {
              instruction.operands.front().loan = publishedId;
            }
          }
        }
      });
  expect(forgedOperandLoan.first && !forgedOperandLoan.second.valid() &&
             hasMirVerificationError(forgedOperandLoan.second,
                                     "does not match the loan's parent"),
         "MIR verification should reject a forwarded return transfer whose "
         "operand does not name the transient call-result loan");

  const auto forgedProvenance =
      mutateForward([publishedId](lang::MirFunctionInstance &fn) {
        for (lang::MirLoan &loan : fn.body.loans) {
          if (loan.id == publishedId) {
            for (const lang::MirPlace &place : fn.body.places) {
              if (place.id != loan.source &&
                  place.root == lang::MirPlaceRootKind::This &&
                  place.projections.empty()) {
                loan.source = place.id;
              }
            }
          }
        }
      });
  expect(forgedProvenance.first && !forgedProvenance.second.valid(),
         "MIR verification should reject a forwarded return transfer whose "
         "published provenance diverges from the transient chain");
}

void testDefaultedDefaultLifecycle() {
  const std::string source = R"(
class ImplicitStructural {
  int value = 7;
public:
  ImplicitStructural() = default;
};

class ExplicitStructural {
  int value = 7;

public:
  ExplicitStructural() = default;
  ~ExplicitStructural() = default;
  int read() { return this.value; }
};

class NestedStructural {
  ExplicitStructural value = ExplicitStructural();

public:
  NestedStructural() = default;
  ~NestedStructural() = default;
  int read() { return this.value.read(); }
};

int main() {
  ExplicitStructural direct = ExplicitStructural();
  NestedStructural nested = NestedStructural();
  return direct.read() + nested.read() - 14;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("defaulted-default-lifecycle.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected defaulted-lifecycle diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "explicitly defaulted default construction and destruction should "
         "complete the compiler pipeline");

  const lang::ClassDecl *explicitClass =
      findTopLevelClass(frontend.program, "ExplicitStructural");
  const lang::ConstructorDecl *constructor = nullptr;
  const lang::DestructorDecl *destructor = nullptr;
  if (explicitClass != nullptr) {
    for (const lang::StmtPtr &member : explicitClass->members()) {
      if (const auto *candidate =
              dynamic_cast<const lang::ConstructorDecl *>(member.get())) {
        constructor = candidate;
      } else if (const auto *candidate =
                     dynamic_cast<const lang::DestructorDecl *>(member.get())) {
        destructor = candidate;
      }
    }
  }
  const lang::ClassLifecycleInfo *lifecycle =
      explicitClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*explicitClass);
  expect(constructor != nullptr && constructor->specifier() &&
             constructor->specifier()->kind ==
                 lang::SpecialMemberSpecifierKind::Defaulted &&
             constructor->body() == nullptr && destructor != nullptr &&
             destructor->specifier() &&
             destructor->specifier()->kind ==
                 lang::SpecialMemberSpecifierKind::Defaulted &&
             destructor->body() == nullptr,
         "the AST should distinguish defaulted lifecycle policy from a source "
         "body");
  expect(lifecycle != nullptr &&
             lifecycle->defaultConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             lifecycle->destructor == lang::SpecialMemberStatus::Generated &&
             lifecycle->declaredDestructor &&
             lifecycle->declaredDestructor->declaration == destructor &&
             !lifecycle->requiresActiveDropState &&
             lifecycle->traits.copyable && lifecycle->traits.movable,
         "defaulted lifecycle declarations should retain structural traits "
         "without creating declared cleanup state");

  bool selectedDefaultedDeclaration = false;
  bool hirUsesGeneratedConstruction = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (lifecycle != nullptr && value.constructorTarget &&
          value.info.type.kind == lang::SemanticType::Class &&
          value.info.type.classId == lifecycle->id &&
          value.constructorKind == lang::ConstructorKind::Ordinary) {
        const auto *target =
            frontend.hir.findConstructorInstance(*value.constructorTarget);
        hirUsesGeneratedConstruction =
            hirUsesGeneratedConstruction ||
            (target && target->generatedDefault && target->source == nullptr &&
             target->declaration == 0 && target->parameterTypes.empty() &&
             target->initializers.size() == 1 &&
             target->body.statements.empty());
      }
      if (value.source != nullptr) {
        const lang::ResolvedConstructionInfo *construction =
            frontend.semantics.findConstruction(*value.source);
        selectedDefaultedDeclaration =
            selectedDefaultedDeclaration ||
            (construction != nullptr &&
             construction->declaration == constructor &&
             construction->generatedDefault);
      }
    }
  }
  expect(selectedDefaultedDeclaration && hirUsesGeneratedConstruction &&
             frontend.hir.destructorInstances().empty(),
         "semantic selection should retain the written declaration while HIR "
         "selects the generated field schedule without inventing a source "
         "constructor or destructor body");

  bool generatedSchedule = false;
  bool structuralClass = false;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.type.classId);
    if (info == nullptr || info->qualifiedName != "ExplicitStructural")
      continue;
    structuralClass =
        instance.defaultConstructor == lang::SpecialMemberStatus::Generated &&
        instance.destructorStatus == lang::SpecialMemberStatus::Generated &&
        !instance.destructor && !instance.requiresActiveDropState &&
        !instance.requiresActiveCleanup;
    for (const lang::MirConstructorInstance &constructor :
         frontend.mir.constructorInstances())
      generatedSchedule =
          generatedSchedule ||
          (constructor.owner == instance.id && constructor.generatedDefault &&
           constructor.parameterTypes.empty() &&
           constructor.initializers.size() == 1);
  }
  expect(structuralClass && generatedSchedule && nativeBackendEmits(frontend),
         "the backend should emit the selected generated constructor while "
         "keeping structural destruction and no duplicate default or "
         "active-drop adapter");

  const std::string invalidSource = R"(
class MissingInitializer {
  int value;
public:
  MissingInitializer() = default;
};

class ParameterizedDefault {
  int value = 0;
public:
  ParameterizedDefault(int initial) = default;
public:
  ParameterizedDefault() = default;
};

class DeletedDefault {
public:
  DeletedDefault() = delete;
};

class DeletedCleanup {
public:
  ~DeletedCleanup() = delete;
public:
  DeletedCleanup() = default;
};
)";
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-defaulted-default-lifecycle.gti", invalidSource);
  const auto diagnosticContaining =
      [&](std::string_view text) -> const lang::Diagnostic * {
    const auto found = std::find_if(
        invalid.diagnostics.begin(), invalid.diagnostics.end(),
        [&](const lang::Diagnostic &diagnostic) {
          return diagnostic.message.find(text) != std::string::npos;
        });
    return found == invalid.diagnostics.end() ? nullptr : &*found;
  };
  const lang::Diagnostic *unavailable =
      diagnosticContaining("defaulted default constructor");
  const lang::Diagnostic *shape =
      diagnosticContaining("must use the exact form");
  const lang::Diagnostic *deletedConstructor =
      diagnosticContaining("not supported for ordinary constructors");
  const lang::Diagnostic *deletedDestructor =
      diagnosticContaining("not supported for destructors");
  const std::size_t unavailableDefault = invalidSource.find("default;");
  const std::size_t missingField = invalidSource.find("value;");
  expect(!invalid.canGenerateCode() && unavailable != nullptr &&
             unavailable->code == "GTI-S2020" &&
             unavailable->primary.start == unavailableDefault &&
             unavailable->primary.end == unavailableDefault + 7 &&
             !unavailable->related.empty() &&
             unavailable->related.front().span.start == missingField &&
             !unavailable->hints.empty() && unavailable->fixes.empty() &&
             shape != nullptr && shape->code == "GTI-S2020" &&
             deletedConstructor != nullptr &&
             deletedConstructor->code == "GTI-S2020" &&
             deletedDestructor != nullptr &&
             deletedDestructor->code == "GTI-S2021",
         "defaulted lifecycle diagnostics should own exact shape, structural "
         "availability, and unsupported deletion with actionable spans");

  const std::string formatted = lang::Formatter().format(
      "class Value{int value=0;public:Value()=default;~Value()=default;};");
  expect(formatted.find("Value() = default;") != std::string::npos &&
             formatted.find("~Value() = default;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "defaulted constructor and destructor syntax should format "
         "idempotently");
}

void testRestrictedMemberOperators() {
  const std::string source = R"(
struct Payload {
  mut int value = 0;

  void increment() mut { this.value += 1; }
public:
  Payload() = default;
};

class Handle {
  mut Payload payload = Payload();
  mut int values[2] = {1, 2};

public:
  Payload& operator->() { return this.payload; }
  mut Payload& operator->() mut { return this.payload; }
  int& operator*() { return this.values[0]; }
  mut int& operator*() mut { return this.values[0]; }
  int& operator[](uint64_t index) { return this.values[index]; }
  mut int& operator[](uint64_t index) mut { return this.values[index]; }
  bool operator==(nullptr_t other) { return false; }
  bool operator!=(nullptr_t other) { return true; }
  operator bool() { return true; }
public:
  Handle() = default;
};

class Number {
  mut int value = 0;

public:
  Number(int initial) : value(initial) {}

  Number operator+() { return Number(this.value); }
  Number operator-() { return Number(0 - this.value); }
  Number operator+(int other) { return Number(this.value + other); }
  Number operator-(int other) { return Number(this.value - other); }
  Number operator*(int other) { return Number(this.value * other); }
  Number operator/(int other) { return Number(this.value / other); }
  Number operator%(int other) { return Number(this.value % other); }
  Number operator~() { return Number(~this.value); }
  Number operator&(int other) { return Number(this.value & other); }
  Number operator|(int other) { return Number(this.value | other); }
  Number operator^(int other) { return Number(this.value ^ other); }
  Number operator<<(int amount) { return Number(this.value << amount); }
  Number operator>>(int amount) { return Number(this.value >> amount); }

  void operator+=(int other) mut { this.value += other; }
  void operator-=(int other) mut { this.value -= other; }
  void operator*=(int other) mut { this.value *= other; }
  void operator/=(int other) mut { this.value /= other; }
  void operator%=(int other) mut { this.value %= other; }
  void operator&=(int other) mut { this.value &= other; }
  void operator|=(int other) mut { this.value |= other; }
  void operator^=(int other) mut { this.value ^= other; }
  void operator<<=(int amount) mut { this.value <<= amount; }
  void operator>>=(int amount) mut { this.value >>= amount; }
  void operator--() mut { this.value--; }

  int get() { return this.value; }
public:
  Number() = default;
};

struct NumberHolder {
  mut Number value = Number(1);
public:
  NumberHolder() = default;
};

class NumberIndex {
  mut Number value = Number(2);

public:
  mut Number& operator[](int index) mut { return this.value; }
public:
  NumberIndex() = default;
};

class NumberOwner {
  mut Number value = Number(3);

public:
  mut Number& operator*() mut { return this.value; }
public:
  NumberOwner() = default;
};

class NumberArrow {
  mut NumberHolder holder = NumberHolder();

public:
  mut NumberHolder& operator->() mut { return this.holder; }
public:
  NumberArrow() = default;
};

int main() {
  mut Handle handle = Handle();
  handle->increment();
  *handle = 7;
  handle[uint64_t(1)] += 3;
  mut Number number = Number(9);
  Number unaryPositive = +number;
  Number unaryNegative = -number;
  mut Number sum = number + 2;
  Number difference = number - 2;
  Number product = number * 2;
  Number quotient = number / 3;
  Number remainder = number % 4;
  Number conjunction = number & 7;
  Number disjunction = number | 2;
  Number exclusive = number ^ 3;
  Number shiftedLeft = number << 1;
  Number shiftedRight = number >> 1;
  sum += 3;
  --sum;
  Number inverted = ~sum;
  mut Number mutations = Number(60);
  mutations -= 1;
  mutations *= 2;
  mutations /= 3;
  mutations %= 17;
  mutations &= 15;
  mutations |= 16;
  mutations ^= 3;
  mutations <<= 1;
  mutations >>= 1;
  mut NumberHolder holder = NumberHolder();
  holder.value += 2;
  mut NumberIndex indexed = NumberIndex();
  indexed[0] += 2;
  mut NumberOwner owner = NumberOwner();
  *owner += 2;
  mut NumberArrow arrow = NumberArrow();
  arrow->value += 2;
  if (handle and handle != nullptr) {
    return *handle + handle[uint64_t(1)] - 12;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("member-operators.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected operator diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "the restricted pointer-like member operators should validate");

  const auto *handle = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(1).get());
  std::size_t operatorCount = 0;
  bool foundMutableReferenceReturn = false;
  if (handle != nullptr) {
    for (const lang::StmtPtr &member : handle->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function == nullptr || !function->operatorName()) {
        continue;
      }
      ++operatorCount;
      foundMutableReferenceReturn =
          foundMutableReferenceReturn ||
          (function->returnMutability() == lang::Mutability::Mutable &&
           function->returnType().reference.has_value());
    }
  }
  expect(operatorCount == 9 && foundMutableReferenceReturn,
         "the AST should retain operator identity and mutable reference "
         "returns");

  const lang::FunctionDecl *operatorMain =
      findTopLevelFunction(frontend.program, "main");
  const lang::IfStmt *conditional = nullptr;
  if (operatorMain != nullptr) {
    for (const lang::StmtPtr &statement : operatorMain->body()->statements()) {
      if (const auto *candidate =
              dynamic_cast<const lang::IfStmt *>(statement.get())) {
        conditional = candidate;
        break;
      }
    }
  }
  const auto *logical =
      conditional == nullptr
          ? nullptr
          : dynamic_cast<const lang::Logical *>(conditional->condition().get());
  const lang::ResolvedOperatorInfo *contextual =
      logical == nullptr
          ? nullptr
          : frontend.semantics.findContextualConversion(*logical->left());
  const lang::FunctionInfo *contextualTarget =
      contextual == nullptr
          ? nullptr
          : frontend.semantics.findFunction(contextual->function);
  expect(contextual != nullptr && contextualTarget != nullptr &&
             contextual->kind == lang::OverloadedOperator::ContextualBool &&
             contextual->borrowOrigin == contextualTarget->returnBorrowOrigin &&
             contextual->borrowArgument ==
                 contextualTarget->returnBorrowParameter &&
             contextual->borrowAccess == contextualTarget->returnBorrowAccess,
         "contextual conversion resolution should retain the selected "
         "declaration's owner-dependency summary");

  const auto *number = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(2).get());
  std::size_t arithmeticOperatorCount = 0;
  if (number != nullptr) {
    for (const lang::StmtPtr &member : number->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      arithmeticOperatorCount +=
          function != nullptr && function->operatorName() ? 1 : 0;
    }
  }
  expect(arithmeticOperatorCount == 24,
         "the AST should retain every restricted arithmetic, bitwise, "
         "compound-assignment, and prefix-decrement operator identity");

  // The field-returning arrow cannot fail and publishes its loan directly;
  // the checked-index dereference keeps its failure edge.
  expect(
      mirCallsOperator(frontend, lang::OverloadedOperator::Arrow, false) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::Dereference,
                           true) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::Subscript) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::NotEqual) &&
          mirCallsOperator(frontend,
                           lang::OverloadedOperator::ContextualBool) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::Plus) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::AddAssign) &&
          mirCallsOperator(frontend, lang::OverloadedOperator::PreDecrement) &&
          nativeBackendEmits(frontend),
      "MIR should retain the selected operator member calls");

  const std::string formatted = lang::Formatter().format(
      "class Handle{public:mut int& operator*()mut{return this.value;}"
      "bool operator<(Handle& other){return false;}"
      "Handle operator+(int other){return Handle();}"
      "void operator+=(int other)mut{}void operator--()mut{}"
      "operator bool(){return true;} public: Handle() = default; };");
  expect(formatted.find("mut int& operator*() mut {") != std::string::npos &&
             formatted.find("bool operator<(Handle& other) {") !=
                 std::string::npos &&
             formatted.find("Handle operator+(int other) {") !=
                 std::string::npos &&
             formatted.find("void operator+=(int other) mut {") !=
                 std::string::npos &&
             formatted.find("void operator--() mut {") != std::string::npos &&
             formatted.find("operator bool() {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "operator declarations should format with stable C++-style layout");

  const auto valueDereference =
      lang::Frontend().analyze("independent-value-dereference.gti", R"(
class ValueHandle {
  int value = 7;
public:
  int operator*() { return this.value; }
public:
  ValueHandle() = default;
};
int main() { ValueHandle handle{}; return *handle - 7; }
)");
  expect(valueDereference.canGenerateCode(),
         "read-only dereference may yield an independent copyable value");

  const std::pair<std::string_view, std::string_view> invalidContracts[] = {
      {"int operator*() mut { return this.value; }",
       "operator* must return a checked reference"},
      {"int operator->() { return this.value; }",
       "operator-> must return a checked reference"},
      {"int& operator[](uint64_t first, uint64_t second) { return this.value; "
       "}",
       "operator[] expects 1 parameter"},
      {"bool operator==(nullptr_t other) mut { return true; }",
       "must use a read-only receiver"},
      {"int operator<(InvalidOperators& other) { return 0; }",
       "operator< must return bool"},
      {"int operator+(int other) mut { return this.value + other; }",
       "operator+ must use a read-only or consuming receiver"},
      {"int& operator-(int other) { return this.value; }",
       "operator- must return an owned value"},
      {"int operator+=(int other) mut { return this.value + other; }",
       "operator+= must return void"},
      {"void operator+=(int other) {}",
       "operator+= must use a mutable receiver"},
      {"void operator--() {}", "operator-- must use a mutable receiver"},
      {"int operator~(int other) { return other; }",
       "operator~ expects 0 parameters"},
      {"operator bool() mut { return true; }", "must use a read-only receiver"},
  };
  for (const auto &[declaration, message] : invalidContracts) {
    const auto invalid = lang::Frontend().analyze(
        "invalid-operator-contract.gti",
        "class InvalidOperators { mut int value = 0; public: " +
            std::string(declaration) + " }; int main() { return 0; }");
    expect(!invalid.canGenerateCode() &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2022") &&
               hasDiagnostic(invalid.diagnostics, std::string(message)),
           "each invalid operator retains its own contract diagnostic: " +
               std::string(declaration));
  }

  const lang::FrontendResult invalidUses =
      lang::Frontend().analyze("invalid-operator-uses.gti", R"(
class ReadOnlyHandle {
  int value = 0;
public:
  int& operator*() { return this.value; }
  bool operator==(int other) { return true; }
public:
  ReadOnlyHandle() = default;
};
int main() {
  mut ReadOnlyHandle handle = ReadOnlyHandle();
  *handle = 1;
  if (handle == nullptr) { return 1; }
  if (handle) { return 2; }
  return 0;
}
)");
  expect(!invalidUses.canGenerateCode() &&
             hasDiagnostic(invalidUses.diagnostics,
                           "Dereference assignment requires mutable access") &&
             hasDiagnostic(invalidUses.diagnostics,
                           "No exact overload of operator==") &&
             hasDiagnostic(invalidUses.diagnostics,
                           "does not define operator bool"),
         "operator use should require mutable access, exact operands, and an "
         "explicit contextual conversion");

  const lang::FrontendResult exactArithmetic =
      lang::Frontend().analyze("exact-arithmetic-operator.gti", R"(
class ExactArithmetic {
public:
  ExactArithmetic operator+(int64_t other) { return ExactArithmetic(); }
public:
  ExactArithmetic() = default;
};
int main() {
  ExactArithmetic value = ExactArithmetic();
  ExactArithmetic result = value + int32_t(1);
  return 0;
}
)");
  expect(!exactArithmetic.canGenerateCode() &&
             hasDiagnostic(exactArithmetic.diagnostics,
                           "No exact overload of operator+ for argument "
                           "types (int32_t)") &&
             hasRelatedDiagnostic(exactArithmetic.diagnostics,
                                  "Candidate: operator+") &&
             hasDiagnosticHint(exactArithmetic.diagnostics,
                               "do not perform implicit conversions"),
         "arithmetic operator resolution should reject implicit operand "
         "conversions and list the exact candidates");

  const lang::FrontendResult noReversedOperands =
      lang::Frontend().analyze("no-reversed-arithmetic-operator.gti", R"(
class RightHandOnly {
public:
  RightHandOnly operator+(int other) { return RightHandOnly(); }
public:
  RightHandOnly() = default;
};
int main() {
  RightHandOnly value = RightHandOnly();
  RightHandOnly result = 1 + value;
  return 0;
}
)");
  expect(!noReversedOperands.canGenerateCode() &&
             hasDiagnostic(noReversedOperands.diagnostics,
                           "requires numeric operands"),
         "operator resolution should not reverse operands or search a "
         "right-hand class for a member operator");

  const lang::FrontendResult noOperatorSynthesis =
      lang::Frontend().analyze("no-operator-synthesis.gti", R"(
class PlusOnly {
public:
  PlusOnly operator+(int other) { return PlusOnly(); }
public:
  PlusOnly() = default;
};
int main() {
  mut PlusOnly value = PlusOnly();
  value += 1;
  return 0;
}
)");
  expect(!noOperatorSynthesis.canGenerateCode() &&
             hasDiagnostic(noOperatorSynthesis.diagnostics,
                           "does not define operator+=") &&
             hasDiagnosticHint(noOperatorSynthesis.diagnostics,
                               "does not synthesize compound operators"),
         "operator+= should require its own declaration instead of being "
         "synthesized from operator+");

  lang::Lexer lexer;
  lang::Parser unsupportedParser(lexer.scan(R"(
class Unsupported {
public:
  bool operator!() { return false; }
};
int main() { return 0; }
)"));
  const lang::Program recovered = unsupportedParser.parse();
  expect(unsupportedParser.hadError() &&
             hasDiagnostic(unsupportedParser.errors(),
                           "operator spelling is not supported for "
                           "overloading") &&
             !recovered.declarations().empty(),
         "unsupported operators should receive a focused parser diagnostic "
         "and recover to later declarations");

  lang::Parser freeOperatorParser(lexer.scan(R"(
int operator*(int value) { return value; }
int main() { return 0; }
)"));
  freeOperatorParser.parse();
  expect(freeOperatorParser.errors().size() == 1 &&
             hasDiagnostic(freeOperatorParser.errors(),
                           "only be declared as class or struct members"),
         "free operator overload declarations should be rejected by the "
         "source grammar");

  const lang::FrontendResult invalidAccess =
      lang::Frontend().analyze("invalid-operator-access.gti", R"(
class PrivateTruth {
  operator bool() { return true; }
public:
  PrivateTruth() = default;
};
class MutableOnly {
  mut int value = 0;
public:
  mut int& operator*() mut { return this.value; }
public:
  MutableOnly() = default;
};
int main() {
  PrivateTruth hidden = PrivateTruth();
  MutableOnly fixed = MutableOnly();
  if (hidden) { return *fixed; }
  return 0;
}
)");
  expect(!invalidAccess.canGenerateCode() &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "operator bool of 'PrivateTruth' is private") &&
             hasDiagnostic(invalidAccess.diagnostics,
                           "operator* requires a mutable receiver"),
         "operator access and receiver mutability should be enforced before "
         "lowering");
}

void testCallableMemberOperators() {
  const std::string source = R"(
class Accumulator {
  mut int total;

public:
  Accumulator(int initial) : total(initial) {}

  int operator()() { return this.total; }
  int operator()(int value) { return this.total + value; }
  int operator()(int value) mut {
    this.total += value;
    return this.total;
  }
  int operator()(int left, int right) { return this.total + left + right; }
};

class Identity<T> {
public:
  T operator()(T value) { return value; }
public:
  Identity() = default;
};

class Slot {
  mut int value = 1;

public:
  int& operator()() { return this.value; }
  mut int& operator()() mut { return this.value; }
public:
  Slot() = default;
};

int main() {
  Accumulator fixed = Accumulator(1);
  mut Accumulator changing = Accumulator(2);
  Identity<int> identity = Identity<int>();
  int first = fixed();
  int second = fixed(3);
  int third = changing(4);
  int fourth = fixed(5, 6);
  int generic = identity(7);
  mut Slot slot = Slot();
  mut int& slot_value = slot();
  slot_value = first + second + third + fourth + generic;
  return slot() - 25;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("call-operator.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected call-operator diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "operator() should support exact arbitrary-arity member overloads");

  const lang::MirVerificationResult callableVerification =
      lang::verifyMirProgram(frontend.mir);
  if (!callableVerification.valid()) {
    for (const lang::MirVerificationError &error :
         callableVerification.errors) {
      std::cerr << "Unexpected call-operator MIR error: " << error.owner << ':'
                << error.block << ':' << error.instruction << ' '
                << error.message << '\n';
    }
  }
  expect(callableVerification.valid(),
         "operator() source accepted for code generation should have a fully "
         "verified MIR program");

  expect(mirCallsOperator(frontend, lang::OverloadedOperator::Call) &&
             nativeBackendEmits(frontend),
         "call operators should lower to the semantically selected member");

  const std::string formatted = lang::Formatter().format(
      "class Fn{public:int operator()(int value){return value;} public: Fn() = "
      "default; };");
  expect(formatted.find("int operator()(int value) {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "operator() declarations should format idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-call-operator.gti", R"(
class PrivateCallable {
  int operator()(int value) { return value; }
public:
  PrivateCallable() = default;
};
class MutableCallable {
public:
  int operator()(int value) mut { return value; }
public:
  MutableCallable() = default;
};
class ExactCallable {
public:
  int operator()(int value) { return value; }
public:
  ExactCallable() = default;
};
int main() {
  PrivateCallable hidden = PrivateCallable();
  MutableCallable fixed = MutableCallable();
  ExactCallable exact = ExactCallable();
  int private_value = hidden(1);
  int mutable_value = fixed(1);
  int wrong_type = exact(true);
  int explicit_types = exact<int>(1);
  return private_value + mutable_value + wrong_type + explicit_types;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "operator() of 'PrivateCallable' is private") &&
             hasDiagnostic(invalid.diagnostics,
                           "operator() requires a mutable receiver") &&
             hasDiagnostic(invalid.diagnostics,
                           "No exact overload of operator()") &&
             hasDiagnostic(invalid.diagnostics,
                           "do not take explicit type arguments"),
         "call operators should enforce access, receiver mutability, and "
         "exact argument matching");

  const lang::FrontendResult temporaryBorrow =
      lang::Frontend().analyze("temporary-call-borrow.gti", R"(
class Slot {
  int value = 1;
public:
  int& operator()() { return this.value; }
public:
  Slot() = default;
};
int main() {
  int& dangling = Slot()();
  return dangling;
}
)");
  expect(!temporaryBorrow.canGenerateCode() &&
             hasDiagnostic(temporaryBorrow.diagnostics,
                           "derived from temporary storage"),
         "reference-returning call operators should preserve borrow lifetime "
         "checks");
}

void testPassiveCleanupRangeTemporary() {
  // The range class has no destructor and only scalar fields, so its lexical
  // cleanup is passive. The loop must still own and drop the temporary.
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "passive-cleanup-range-temporary.gti", R"(
class Cursor {
  mut int32_t at;
public:
  Cursor(int32_t at) : at(at) {}
  int32_t& operator*() { return this.at; }
  void operator++() mut { this.at += 1; }
  bool operator!=(Cursor& end) { return this.at < end.at; }
};

class Span {
  int32_t last;
public:
  Span(int32_t last) : last(last) {}
  Cursor begin() { return Cursor(0); }
  Cursor end() { return Cursor(this.last); }
};

int32_t first_over(int32_t limit) {
  for (int32_t value : Span(10)) {
    if (value > limit) { return value; }
  }
  return -1;
}

int main() {
  mut int32_t total = 0;
  for (int32_t value : Span(4)) {
    if (value == 1) { continue; }
    total += value;
  }
  return total - 5 + first_over(6) - 7;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << "Unexpected passive range diagnostic: " << diagnostic.message
              << '\n';
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             frontend.mir.valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "a range-for over a temporary with passive cleanup should lower to "
         "verified MIR");

  std::size_t passiveRanges = 0;
  const lang::MirBody *passiveBody = nullptr;
  for (const lang::HirFunctionInstance &hirFunction :
       frontend.hir.functionInstances()) {
    const lang::MirFunctionInstance *mirFunction =
        frontend.mir.findFunctionInstance(hirFunction.id);
    if (mirFunction == nullptr) {
      continue;
    }
    for (const lang::MirRangeIteration &range :
         mirFunction->body.rangeIterations) {
      if (!range.ownsRangeTemporary) {
        continue;
      }
      const lang::MirPlace *owner = mirFunction->body.findPlace(range.owner);
      const lang::MirDropObligation *drop =
          mirFunction->body.findDropObligation(range.ownerDrop);
      if (owner != nullptr && drop != nullptr && drop->place == range.owner &&
          drop->dropType.type == owner->type &&
          !drop->dropType.requiresActiveCleanup) {
        ++passiveRanges;
        passiveBody = &mirFunction->body;
      }
    }
  }
  expect(passiveRanges == 2 && passiveBody != nullptr,
         "each owned passive-cleanup range temporary should keep one exact "
         "owner drop obligation without requiring active cleanup");

  if (passiveBody != nullptr) {
    lang::MirBody missingDrop = *passiveBody;
    missingDrop.rangeIterations.front().ownerDrop = 0;
    expect(!lang::verifyMirBody(missingDrop).valid(),
           "MIR verification should reject an owned lexical range temporary "
           "with no cleanup obligation");

    lang::MirBody wrongType = *passiveBody;
    const lang::MirDropObligationId ownerDrop =
        wrongType.rangeIterations.front().ownerDrop;
    if (ownerDrop != 0 && ownerDrop <= wrongType.dropObligations.size()) {
      wrongType.dropObligations[ownerDrop - 1].dropType.type =
          lang::SemanticType::Int32;
    }
    expect(ownerDrop != 0 && !lang::verifyMirBody(wrongType).valid(),
           "MIR verification should reject a range owner obligation whose "
           "cleanup type differs from the owner");
  }

  if (!frontend.mirValid || !lang::verifyMirProgram(frontend.mir).valid()) {
    return;
  }
  const bool ownedSpanTemporary =
      mirHasRangeIteration(frontend, [&](const lang::MirBody &,
                                         const lang::MirRangeIteration &range) {
        const lang::ClassTypeInfo *info =
            range.rangeType.kind == lang::SemanticType::Class
                ? frontend.semantics.findClassType(range.rangeType.classId)
                : nullptr;
        return range.ownsRangeTemporary && range.ownerDrop != 0 &&
               info != nullptr && info->qualifiedName == "Span";
      });
  expect(ownedSpanTemporary && nativeBackendEmits(frontend),
         "the native backend should emit a passive-cleanup range temporary");
}

void testSharedPayloadSiblingReborrows() {
  // Read-only payload bindings of one matched enum are siblings. Reborrowing a
  // payload of one sibling must not conflict with the other read-only ones.
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "shared-payload-sibling-reborrows.gti", R"(
enum Event<T> { item(T value) };
class Inner {
public:
  Event<int32_t> data;
  Inner(Event<int32_t> data) : data(data) {}
};
enum Pair<T> { items(T first, T second), other(T first, T second) };

enum class Tag { small(int32_t value), big(int64_t value) };
class Cell {
public:
  Tag tag;
  int32_t weight;
  Cell(Tag tag, int32_t weight) : tag(tag), weight(weight) {}
};
enum class Trio { all(Cell a, Cell b, Cell c), none };

int32_t generic_pair() {
  Inner left = Inner(Event<int32_t>::item(1));
  Inner right = Inner(Event<int32_t>::item(2));
  Pair<Inner> pair = Pair<Inner>::items(left, right);
  switch (pair) {
    case Pair<Inner>::items(auto& first, auto& second):
      switch (first.data) {
        case Event<int32_t>::item(auto& value):
          if (value != 1) { return 1; }
          break;
      }
      switch (second.data) {
        case Event<int32_t>::item(value): return value - 2;
      }
    case Pair<Inner>::other: return 3;
  }
}

int32_t three_siblings() {
  Cell x = Cell(Tag::small(1), 2);
  Cell y = Cell(Tag::big(int64_t(3)), 4);
  Cell z = Cell(Tag::small(5), 6);
  Trio trio = Trio::all(x, y, z);
  switch (trio) {
    case Trio::all(auto& a, auto& b, auto& c):
      switch (a.tag) {
        case Tag::small(auto& v): if (v != 1) { return 1; } break;
        case Tag::big(auto& v): return 2;
      }
      switch (b.tag) {
        case Tag::small(auto& v): return 3;
        case Tag::big(auto& v): return int32_t(v) + a.weight + c.weight - 11;
      }
    case Trio::none: return 4;
  }
}

int main() { return generic_pair() + three_siblings(); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << "Unexpected shared payload diagnostic: " << diagnostic.message
              << '\n';
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             frontend.mir.valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "nested read-only payload reborrows beside live read-only payload "
         "siblings should lower to verified MIR");

  std::size_t corrupted = 0;
  std::size_t rejected = 0;
  for (const lang::HirFunctionInstance &hirFunction :
       frontend.hir.functionInstances()) {
    const lang::MirFunctionInstance *mirFunction =
        frontend.mir.findFunctionInstance(hirFunction.id);
    if (mirFunction == nullptr || hirFunction.source == nullptr ||
        hirFunction.source->name().lexeme == "main") {
      continue;
    }
    for (const lang::MirLoan &child : mirFunction->body.loans) {
      const lang::MirLoan *parent = mirFunction->body.findLoan(child.parent);
      if (child.kind != lang::MirLoanKind::Local ||
          child.access != lang::AccessMode::ReadOnly || parent == nullptr ||
          parent->kind != lang::MirLoanKind::Local ||
          parent->access != lang::AccessMode::ReadOnly) {
        continue;
      }
      // Widening the parent lets the child's place be written through it, so
      // the overlapping read-only sibling becomes a genuine conflict.
      lang::MirBody widened = mirFunction->body;
      widened.loans[parent->id - 1].access = lang::AccessMode::Mutable;
      ++corrupted;
      rejected += !lang::verifyMirBody(widened).valid();
      break;
    }
  }
  expect(corrupted == 2 && rejected == 2,
         "MIR verification should still reject a payload reborrow whose "
         "parent was widened to mutable access");
}

void testConfinedCallableParameters() {
  const std::string source = R"(
void apply_twice<T, Operation>(mut T& value, Operation operation) {
  operation(value);
  operation(value);
}

int apply_stateful<Operation>(mut Operation operation) {
  int first = operation(1);
  int second = operation(1);
  return first + second;
}

class Replace<T> {
  T replacement;

public:
  Replace(T replacement) : replacement(replacement) {}
  void operator()(mut T& value) { value = this.replacement; }
};

class Increasing {
  mut int next = 0;

public:
  int operator()(int value) mut {
    this.next++;
    return value + this.next;
  }
public:
  Increasing() = default;
};

int main() {
  mut int lambda_value = 1;
  auto increment = [](mut int& value) -> void { value++; };
  apply_twice(lambda_value, increment);
  mut int object_value = 2;
  Replace<int> object = Replace<int>(4);
  apply_twice(object_value, object);
  Increasing increasing = Increasing();
  int stateful = apply_stateful(increasing);
  auto doubled = [](int value) -> int { return value * 2; };
  int read_only = apply_stateful(doubled);
  return lambda_value + object_value + stateful + read_only - 16;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("callable-parameters.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected callable-parameter diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "confined generic operations should accept lambdas and exact "
         "function objects");

  const lang::FunctionDecl *apply =
      findTopLevelFunction(frontend.program, "apply_twice");
  const lang::FunctionInfo *applyInfo =
      apply == nullptr ? nullptr : frontend.semantics.findFunction(*apply);
  expect(applyInfo != nullptr && applyInfo->callableParameters.size() == 1 &&
             applyInfo->callableParameters.front().parameterIndex == 1 &&
             applyInfo->callableParameters.front().boundary ==
                 lang::CallableBoundary::Confined &&
             applyInfo->callableParameters.front().signatures.size() == 2,
         "semantic function metadata should retain each required callable "
         "signature and its confined parameter");

  std::size_t callableInstances = 0;
  bool foundLambdaContract = false;
  bool foundObjectContract = false;
  bool hirCallsConfined = true;
  bool mirContractsPreserved = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "apply_twice") {
      continue;
    }
    ++callableInstances;
    if (instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().signatures.size() != 2) {
      hirCallsConfined = false;
      continue;
    }
    const lang::HirCallableParameter &contract =
        instance.callableParameters.front();
    const bool lambda =
        contract.callableType.kind == lang::SemanticType::Lambda;
    const bool object = contract.callableType.kind == lang::SemanticType::Class;
    foundLambdaContract =
        foundLambdaContract ||
        (lambda &&
         std::all_of(contract.signatures.begin(), contract.signatures.end(),
                     [](const lang::HirCallableSignature &signature) {
                       return signature.lambdaTarget &&
                              !signature.functionTarget;
                     }));
    foundObjectContract =
        foundObjectContract ||
        (object &&
         std::all_of(contract.signatures.begin(), contract.signatures.end(),
                     [](const lang::HirCallableSignature &signature) {
                       return signature.functionTarget &&
                              !signature.lambdaTarget;
                     }));
    const std::size_t confinedCalls = static_cast<std::size_t>(std::count_if(
        instance.body.values.begin(), instance.body.values.end(),
        [](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Call &&
                 value.callableBoundary == lang::CallableBoundary::Confined;
        }));
    hirCallsConfined = hirCallsConfined && confinedCalls == 2;

    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    std::size_t mirConfinedCalls = 0;
    if (mir != nullptr) {
      for (const lang::MirBlock &block : mir->body.blocks) {
        mirConfinedCalls += static_cast<std::size_t>(std::count_if(
            block.instructions.begin(), block.instructions.end(),
            [](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.callableBoundary ==
                         lang::CallableBoundary::Confined;
            }));
      }
    }
    mirContractsPreserved = mirContractsPreserved && mir != nullptr &&
                            mir->callableParameters.size() == 1 &&
                            mirConfinedCalls == 2;
  }
  expect(callableInstances == 2 && foundLambdaContract && foundObjectContract &&
             hirCallsConfined && mirContractsPreserved,
         "concrete HIR and MIR instances should retain exact callable targets "
         "and confined invocation metadata");

  std::size_t mutableCallableInstances = 0;
  bool foundMutableSelection = false;
  bool foundReadSelectionThroughMutableAccess = false;
  bool mutableCallsPreserved = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "apply_stateful") {
      continue;
    }
    ++mutableCallableInstances;
    if (instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().access !=
            lang::AccessMode::Mutable ||
        instance.callableParameters.front().signatures.size() != 2) {
      mutableCallsPreserved = false;
      continue;
    }
    const lang::HirCallableParameter &contract =
        instance.callableParameters.front();
    for (const lang::HirCallableSignature &signature : contract.signatures) {
      mutableCallsPreserved = mutableCallsPreserved &&
                              signature.requiredCapability ==
                                  lang::CallableInvocationCapability::Mutable &&
                              signature.selectedCapability.has_value();
      foundMutableSelection = foundMutableSelection ||
                              signature.selectedCapability ==
                                  lang::CallableInvocationCapability::Mutable;
      foundReadSelectionThroughMutableAccess =
          foundReadSelectionThroughMutableAccess ||
          signature.selectedCapability ==
              lang::CallableInvocationCapability::Read;
    }
    const std::size_t mutableCalls = static_cast<std::size_t>(std::count_if(
        instance.body.values.begin(), instance.body.values.end(),
        [](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Call &&
                 value.callableBoundary == lang::CallableBoundary::Confined &&
                 value.callableInvocation ==
                     lang::CallableInvocationCapability::Mutable;
        }));
    const std::size_t readCalls = static_cast<std::size_t>(std::count_if(
        instance.body.values.begin(), instance.body.values.end(),
        [](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Call &&
                 value.callableBoundary == lang::CallableBoundary::Confined &&
                 value.callableInvocation ==
                     lang::CallableInvocationCapability::Read;
        }));
    mutableCallsPreserved =
        mutableCallsPreserved && (mutableCalls == 2 || readCalls == 2);
  }
  expect(mutableCallableInstances == 2 && foundMutableSelection &&
             foundReadSelectionThroughMutableAccess && mutableCallsPreserved,
         "mutable callable parameters should retain a mut-callable client "
         "requirement while accepting read-callable and mut-callable targets");
  const std::string mirDump = lang::MirPrinter().print(frontend.mir);
  expect(mirDump.find("callables=[callable(parameter=1") != std::string::npos &&
             mirDump.find("callable-boundary=confined") != std::string::npos &&
             mirDump.find("required=mut;selected=mut") != std::string::npos &&
             mirDump.find("required=mut;selected=read") != std::string::npos &&
             mirDump.find("callable-invocation=mut") != std::string::npos,
         "canonical MIR dumps should expose callable contracts and confined "
         "invocations");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainInstance = &instance;
      break;
    }
  }
  const std::size_t confinedArguments =
      mainInstance == nullptr
          ? 0
          : static_cast<std::size_t>(std::count_if(
                mainInstance->body.values.begin(),
                mainInstance->body.values.end(),
                [](const lang::HirValue &value) {
                  return value.callableArguments ==
                         std::vector{lang::CallableArgumentBoundary{
                             .parameterIndex = 1,
                             .boundary = lang::CallableBoundary::Confined}};
                }));
  expect(confinedArguments == 2,
         "algorithm call sites should identify confined callable arguments");

  // Confined callables invoke the exact selected GTI call operator members
  // through a read invocation capability rather than native overloading.
  expect(mirHasCallableInvocation(frontend,
                                  lang::CallableInvocationCapability::Read) &&
             mirCallsOperator(frontend, lang::OverloadedOperator::Call) &&
             nativeBackendEmits(frontend, lang::OptimizationLevel::O1),
         "lowering should bridge exact GTI call operators without exposing "
         "native overload resolution");

  const lang::FrontendResult invalidSignatures =
      lang::Frontend().analyze("invalid-callable-signatures.gti", R"(
void invoke<Operation>(Operation operation) { operation(1); }

class MutableOnly {
public:
  void operator()(int value) mut {}
public:
  MutableOnly() = default;
};

int main() {
  auto wrong_argument = [](bool value) -> void {};
  invoke(wrong_argument);
  auto wrong_return = [](int value) -> int { return value; };
  invoke(wrong_return);
  MutableOnly mutable_only = MutableOnly();
  invoke(mutable_only);
  return 0;
}
)");
  expect(
      !invalidSignatures.canGenerateCode() &&
          hasDiagnostic(invalidSignatures.diagnostics,
                        "parameter requires 'bool'") &&
          hasDiagnostic(invalidSignatures.diagnostics, "must return 'void'") &&
          hasDiagnostic(invalidSignatures.diagnostics,
                        "permits only read-callable invocation") &&
          hasDiagnosticHint(invalidSignatures.diagnostics,
                            "parameter with 'mut'") &&
          hasRelatedDiagnostic(invalidSignatures.diagnostics,
                               "Concrete generic instance requested here"),
      "concrete callable instances should reject inexact arguments and "
      "non-void operations before backend lowering");

  const lang::FrontendResult invalidBoundary =
      lang::Frontend().analyze("invalid-callable-boundary.gti", R"(
Operation escape<Operation>(Operation operation) { return operation; }

int main() {
  auto operation = [](int value) -> void {};
  auto escaped = escape(operation);
  return 0;
}
)");
  expect(
      !invalidBoundary.canGenerateCode() &&
          hasDiagnostic(invalidBoundary.diagnostics,
                        "requires an explicit std::move ownership transfer") &&
          hasDiagnosticHint(invalidBoundary.diagnostics,
                            "Move an exact movable callable"),
      "implicit owned callable transfers remain rejected");

  const lang::FrontendResult escapingObject =
      lang::Frontend().analyze("escaping-callable-object.gti", R"(
class Action {
public:
  void operator()() {}
public:
  Action() = default;
};

Operation invoke_and_return<Operation>(Operation operation) {
  operation();
  return operation;
}

int main() { return 0; }
)");
  const bool escapingObjectHasNoCallableIr =
      std::none_of(escapingObject.hir.functionInstances().begin(),
                   escapingObject.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &instance) {
                     return !instance.callableParameters.empty();
                   });
  expect(!escapingObject.canGenerateCode() &&
             countDiagnosticCode(escapingObject.diagnostics, "GTI-S2046") ==
                 1 &&
             hasDiagnostic(escapingObject.diagnostics,
                           "cannot escape through the function return value") &&
             hasRelatedDiagnostic(escapingObject.diagnostics,
                                  "Confined callable parameter declared") &&
             escapingObjectHasNoCallableIr,
         "invoked nominal callable objects must not escape while their "
         "compiler-owned boundary is confined");

  const lang::FrontendResult storedObject =
      lang::Frontend().analyze("stored-callable-object.gti", R"(
class Action {
public:
  void operator()() {}
public:
  Action() = default;
};

class Holder<T> {
public:
  mut T value;
  Holder(T value) : value(value) {}
};

void invoke_and_assign<Operation>(mut Operation& target,
                                  Operation operation) {
  operation();
  target = operation;
}

void invoke_and_store<Operation>(mut Holder<Operation>& target,
                                 Operation operation) {
  operation();
  target.value = operation;
}

void invoke_and_copy<Operation>(Operation operation) {
  operation();
  Operation copy = operation;
}

void invoke_and_capture<Operation>(Operation operation) {
  operation();
  auto nested = [operation]() -> void {};
}

int main() { return 0; }
)");
  expect(
      !storedObject.canGenerateCode() &&
          countDiagnosticCode(storedObject.diagnostics, "GTI-S2046") == 4 &&
          hasDiagnostic(storedObject.diagnostics,
                        "cannot be used as an ordinary value") &&
          hasRelatedDiagnostic(storedObject.diagnostics,
                               "Confined callable parameter declared") &&
          hasDiagnosticHint(storedObject.diagnostics,
                            "assignment, storage, and other value transport") &&
          !hasDiagnosticCode(storedObject.diagnostics, "GTI-B0001"),
      "an invoked nominal callable must not escape through assignment, field "
      "storage, local copying, or capture while its boundary is confined");

  const lang::FrontendResult shadowedName =
      lang::Frontend().analyze("shadowed-callable-name.gti", R"(
void invoke<Operation>(Operation operation) {
  operation();
  {
    mut int operation = 0;
    operation = 1;
  }
}

class Action {
public:
  void operator()() {}
public:
  Action() = default;
};

int main() {
  Action action = Action();
  invoke(action);
  return 0;
}
)");
  expect(shadowedName.canGenerateCode(),
         "confined callable use validation should follow resolved symbol "
         "identity rather than rejecting an unrelated shadowed binding");

  const lang::FrontendResult unprovenObjectForwarding =
      lang::Frontend().analyze("unproven-callable-object-forwarding.gti", R"(
void discard<Operation>(Operation operation) {}

void invoke_and_forward<Operation>(Operation operation) {
  operation();
  discard(operation);
}

class Action {
public:
  void operator()() {}
public:
  Action() = default;
};

int main() {
  Action action = Action();
  invoke_and_forward(action);
  return 0;
}
)");
  expect(
      !unprovenObjectForwarding.canGenerateCode() &&
          countDiagnosticCode(unprovenObjectForwarding.diagnostics,
                              "GTI-S2046") == 1 &&
          hasDiagnostic(unprovenObjectForwarding.diagnostics,
                        "selected target parameter is not proven confined") &&
          !hasDiagnosticCode(unprovenObjectForwarding.diagnostics, "GTI-B0001"),
      "a confined nominal callable must not pass through an unproven "
      "generic forwarding edge");

  const lang::FrontendResult ordinaryObjectTransport =
      lang::Frontend().analyze("ordinary-callable-object-transport.gti", R"(
Operation relay<Operation>(Operation operation) { return operation; }

class Action {
public:
  void operator()() {}
public:
  Action() = default;
};

int main() {
  Action action = Action();
  Action relayed = relay(action);
  relayed();
  return 0;
}
)");
  expect(ordinaryObjectTransport.canGenerateCode(),
         "a nominal callable object that is never invoked through a generic "
         "parameter should retain ordinary value transport semantics");

  const lang::FrontendResult unprovenLambdaTransport =
      lang::Frontend().analyze("unproven-lambda-transport.gti", R"(
void store_locally<Operation>(Operation operation) {
  Operation copy = operation;
}

int main() {
  int captured = 1;
  auto operation = [captured]() -> int { return captured; };
  store_locally(operation);
  return 0;
}
)");
  expect(
      !unprovenLambdaTransport.canGenerateCode() &&
          countDiagnosticCode(unprovenLambdaTransport.diagnostics,
                              "GTI-S2046") == 1 &&
          hasDiagnostic(unprovenLambdaTransport.diagnostics,
                        "requires an explicit std::move ownership transfer") &&
          hasRelatedDiagnostic(
              unprovenLambdaTransport.diagnostics,
              "Exact owned callable parameter declared here") &&
          !hasDiagnosticCode(unprovenLambdaTransport.diagnostics, "GTI-B0001"),
      "an owned local-copy consumer still requires an explicit caller "
      "transfer");

  const lang::FrontendResult genericCapturedCallables =
      lang::Frontend().analyze("generic-captured-callables.gti", R"(
struct CopyableCapture {
  int value = 7;
  CopyableCapture() {}
  CopyableCapture(CopyableCapture& other) = default;
};

int invoke<Operation>(Operation operation) { return operation(); }

int capture_and_invoke<T>(T value) {
  auto operation = [value]() -> int { return 1; };
  return invoke(operation);
}

int main() {
  int first = capture_and_invoke(1);
  int second = capture_and_invoke(CopyableCapture());
  return first + second - 2;
}
)",
                               {standardLibraryPrelude()}, {},
                               {standardLibraryRoot()});
  std::vector<const lang::HirLambda *> producedLambdas;
  for (const lang::HirLambda &lambda :
       genericCapturedCallables.hir.lambdaInstances()) {
    if (lambda.captures.size() == 1) {
      producedLambdas.push_back(&lambda);
    }
  }
  bool foundIntCapture = false;
  bool foundDistinctCapture = false;
  for (const lang::HirLambda *lambda : producedLambdas) {
    foundIntCapture = foundIntCapture || lambda->captures.front().type ==
                                             lang::SemanticType::Int32;
    foundDistinctCapture =
        foundDistinctCapture ||
        lambda->captures.front().type != lang::SemanticType::Int32;
  }
  std::size_t invokeInstances = 0;
  bool exactInvokeTargets = true;
  std::vector<lang::SemanticType> concreteLambdaTypes;
  for (const lang::HirFunctionInstance &instance :
       genericCapturedCallables.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "invoke") {
      continue;
    }
    ++invokeInstances;
    if (instance.typeArguments.size() != 1 ||
        instance.typeArguments.front().kind != lang::SemanticType::Lambda ||
        !instance.typeArguments.front().hasLambdaShape() ||
        instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().signatures.size() != 1 ||
        !instance.callableParameters.front().signatures.front().lambdaTarget) {
      exactInvokeTargets = false;
      continue;
    }
    const lang::SemanticType &lambdaType = instance.typeArguments.front();
    concreteLambdaTypes.push_back(lambdaType);
    const lang::HirLambda *target = genericCapturedCallables.hir.findLambda(
        *instance.callableParameters.front().signatures.front().lambdaTarget);
    exactInvokeTargets = exactInvokeTargets && target != nullptr &&
                         target->type == lambdaType &&
                         target->declaration == lambdaType.lambdaId &&
                         lambdaType.lambdaCaptureTypes().size() == 1 &&
                         target->captures.size() == 1 &&
                         target->captures.front().type ==
                             lambdaType.lambdaCaptureTypes().front();
  }
  expect(
      genericCapturedCallables.canGenerateCode() &&
          producedLambdas.size() == 2 &&
          producedLambdas.front()->declaration ==
              producedLambdas.back()->declaration &&
          foundIntCapture && foundDistinctCapture && invokeInstances == 2 &&
          concreteLambdaTypes.size() == 2 &&
          concreteLambdaTypes.front() != concreteLambdaTypes.back() &&
          concreteLambdaTypes.front().lambdaId ==
              concreteLambdaTypes.back().lambdaId &&
          exactInvokeTargets &&
          lang::verifyMirProgram(genericCapturedCallables.mir).valid(),
      "a lambda declared in a generic body should retain one lexical identity "
      "while concrete capture layouts produce distinct callable instances");

  const lang::FrontendResult sameShapeGenericCallables =
      lang::Frontend().analyze("same-shape-generic-callables.gti", R"(
uint64_t invoke_value<Operation>(Operation operation) {
  return operation();
}

class ValueThroughLambda<uint64_t N> {
public:
  uint64_t get() {
    auto operation = []() -> uint64_t { return N; };
    return invoke_value(operation);
  }
public:
  ValueThroughLambda() = default;
};

int main() {
  ValueThroughLambda<4> four{};
  ValueThroughLambda<8> eight{};
  uint64_t total = four.get() + eight.get();
  return int(total) - 12;
}
)");
  std::vector<const lang::HirLambda *> sameShapeLambdas;
  for (const lang::HirLambda &lambda :
       sameShapeGenericCallables.hir.lambdaInstances()) {
    if (lambda.captures.empty()) {
      sameShapeLambdas.push_back(&lambda);
    }
  }
  bool foundFourLambda = false;
  bool foundEightLambda = false;
  for (const lang::HirLambda *lambda : sameShapeLambdas) {
    const std::span<const lang::CompileTimeValue> values =
        lambda->type.lambdaClassValueArguments();
    if (values.size() != 1 ||
        values.front().kind != lang::CompileTimeValue::UInt64) {
      continue;
    }
    foundFourLambda = foundFourLambda || values.front().value == 4;
    foundEightLambda = foundEightLambda || values.front().value == 8;
  }
  std::size_t invokeValueInstances = 0;
  bool exactSameShapeTargets = true;
  for (const lang::HirFunctionInstance &instance :
       sameShapeGenericCallables.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "invoke_value") {
      continue;
    }
    ++invokeValueInstances;
    if (instance.typeArguments.size() != 1 ||
        instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().signatures.size() != 1 ||
        !instance.callableParameters.front().signatures.front().lambdaTarget) {
      exactSameShapeTargets = false;
      continue;
    }
    const lang::SemanticType &lambdaType = instance.typeArguments.front();
    const lang::HirLambda *target = sameShapeGenericCallables.hir.findLambda(
        *instance.callableParameters.front().signatures.front().lambdaTarget);
    exactSameShapeTargets = exactSameShapeTargets && target != nullptr &&
                            target->type == lambdaType &&
                            lambdaType.hasLambdaShape() &&
                            lambdaType.lambdaCaptureTypes().empty() &&
                            lambdaType.lambdaClassValueArguments().size() == 1;
  }
  expect(sameShapeGenericCallables.canGenerateCode() &&
             sameShapeLambdas.size() == 2 &&
             sameShapeLambdas.front()->declaration ==
                 sameShapeLambdas.back()->declaration &&
             sameShapeLambdas.front()->returnType ==
                 sameShapeLambdas.back()->returnType &&
             sameShapeLambdas.front()->parameterTypes ==
                 sameShapeLambdas.back()->parameterTypes &&
             sameShapeLambdas.front()->type != sameShapeLambdas.back()->type &&
             foundFourLambda && foundEightLambda && invokeValueInstances == 2 &&
             exactSameShapeTargets &&
             lang::verifyMirProgram(sameShapeGenericCallables.mir).valid(),
         "same-shaped lambdas from distinct enclosing value-generic instances "
         "should retain exact concrete identities and callable targets");

  lang::MirProgram swappedSameShapeTarget = sameShapeGenericCallables.mir;
  auto &swappedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      swappedSameShapeTarget.functionInstances());
  bool swappedTarget = false;
  if (swappedSameShapeTarget.lambdaInstances().size() >= 2) {
    for (lang::MirFunctionInstance &instance : swappedFunctions) {
      if (instance.callableParameters.size() != 1 ||
          instance.callableParameters.front().signatures.size() != 1) {
        continue;
      }
      lang::MirCallableSignature &signature =
          instance.callableParameters.front().signatures.front();
      if (!signature.lambdaTarget) {
        continue;
      }
      const lang::HirLambdaId alternate =
          *signature.lambdaTarget ==
                  swappedSameShapeTarget.lambdaInstances().front().id
              ? swappedSameShapeTarget.lambdaInstances().back().id
              : swappedSameShapeTarget.lambdaInstances().front().id;
      if (alternate != *signature.lambdaTarget) {
        signature.lambdaTarget = alternate;
        swappedTarget = true;
        break;
      }
    }
  }
  expect(swappedTarget &&
             !lang::verifyMirProgram(swappedSameShapeTarget).valid(),
         "MIR verification should reject a callable contract retargeted to a "
         "same-shaped lambda from another enclosing generic instance");

  const lang::FrontendResult forwarding =
      lang::Frontend().analyze("callable-forwarding.gti", R"(
void outer<Operation>(Operation operation) { middle(operation); }
void middle<Operation>(Operation operation) { inner(operation); }
void inner<Operation>(Operation operation) { operation(1); }

int main() {
  auto operation = [](int value) -> void {};
  outer(operation);
  return 0;
}
)");
  if (!forwarding.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : forwarding.diagnostics) {
      std::cerr << "Unexpected callable-forwarding diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(forwarding.canGenerateCode(),
         "nested generic forwarding should accept a lambda only through "
         "proven confined callable parameters");

  const lang::FunctionDecl *outer =
      findTopLevelFunction(forwarding.program, "outer");
  const lang::FunctionDecl *middle =
      findTopLevelFunction(forwarding.program, "middle");
  const lang::FunctionDecl *inner =
      findTopLevelFunction(forwarding.program, "inner");
  const lang::FunctionInfo *outerInfo =
      outer == nullptr ? nullptr : forwarding.semantics.findFunction(*outer);
  const lang::FunctionInfo *middleInfo =
      middle == nullptr ? nullptr : forwarding.semantics.findFunction(*middle);
  const lang::FunctionInfo *innerInfo =
      inner == nullptr ? nullptr : forwarding.semantics.findFunction(*inner);
  expect(outerInfo != nullptr && middleInfo != nullptr &&
             innerInfo != nullptr &&
             outerInfo->callableParameters.size() == 1 &&
             outerInfo->callableParameters.front().signatures.empty() &&
             outerInfo->callableParameters.front().forwardings.size() == 1 &&
             middleInfo->callableParameters.size() == 1 &&
             middleInfo->callableParameters.front().signatures.empty() &&
             middleInfo->callableParameters.front().forwardings.size() == 1 &&
             innerInfo->callableParameters.size() == 1 &&
             innerInfo->callableParameters.front().signatures.size() == 1 &&
             innerInfo->callableParameters.front().forwardings.empty(),
         "semantic callable contracts should distinguish forwarded edges "
         "from direct invocation requirements without declaration-order "
         "dependence");

  std::size_t hirForwardingCount = 0;
  std::size_t mirForwardingCount = 0;
  for (const lang::HirFunctionInstance &instance :
       forwarding.hir.functionInstances()) {
    if (instance.source == nullptr ||
        (instance.source->name().lexeme != "outer" &&
         instance.source->name().lexeme != "middle")) {
      continue;
    }
    hirForwardingCount +=
        instance.callableParameters.size() == 1 &&
                instance.callableParameters.front().forwardings.size() == 1 &&
                instance.callableParameters.front()
                    .forwardings.front()
                    .functionTarget.has_value()
            ? 1
            : 0;
    const lang::MirFunctionInstance *mir =
        forwarding.mir.findFunctionInstance(instance.id);
    mirForwardingCount +=
        mir != nullptr && mir->callableParameters.size() == 1 &&
                mir->callableParameters.front().forwardings.size() == 1 &&
                mir->callableParameters.front()
                    .forwardings.front()
                    .functionTarget.has_value()
            ? 1
            : 0;
  }
  const std::string forwardingMir = lang::MirPrinter().print(forwarding.mir);
  expect(hirForwardingCount == 2 && mirForwardingCount == 2 &&
             forwardingMir.find("forwardings=[forward(parameter=0;function=") !=
                 std::string::npos,
         "HIR and MIR should retain each proven callable forwarding target");

  const lang::FrontendResult unprovenForwarding =
      lang::Frontend().analyze("unproven-callable-forwarding.gti", R"(
void discard<Operation>(Operation operation) {}
void outer<Operation>(Operation operation) { discard(operation); }

int main() {
  auto operation = [](int value) -> void {};
  outer(operation);
  return 0;
}
)");
  expect(
      !unprovenForwarding.canGenerateCode() &&
          hasDiagnostic(unprovenForwarding.diagnostics,
                        "requires an explicit std::move ownership transfer") &&
          hasDiagnosticHint(unprovenForwarding.diagnostics,
                            "Move an exact movable callable"),
      "ordinary owned forwarding must not silently consume its source");
}

void testConfinedCallableResults() {
  const std::string source = R"(
bool accepts<Predicate>(int value, Predicate predicate) {
  return predicate(value);
}

bool accepts_split<Predicate>(int left, int right, Predicate predicate) {
  if (predicate(left) && !predicate(right)) {
    return true;
  }
  return false;
}

bool stores_result<Predicate>(int value, Predicate predicate) {
  bool initialized = predicate(value);
  mut bool assigned = false;
  assigned = predicate(value);
  return initialized && assigned;
}

void checks_loops<Predicate>(Predicate predicate) {
  mut int value = 0;
  while (predicate(value)) {
    value++;
  }
  for (mut int index = 0; predicate(index); index++) {}
}

class GreaterThan {
  int threshold;

public:
  GreaterThan(int threshold) : threshold(threshold) {}
  bool operator()(int value) { return value > this.threshold; }
};

int main() {
  auto positive = [](int value) -> bool { return value > 0; };
  bool lambda_result = accepts(4, positive);
  bool split_result = accepts_split(4, -1, positive);
  bool stored_result = stores_result(4, positive);
  auto never = [](int value) -> bool { return value < 0; };
  checks_loops(never);
  GreaterThan greater_than = GreaterThan(3);
  bool object_result = accepts(4, greater_than);
  if (lambda_result && split_result && stored_result && object_result) {
    return 0;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("callable-predicates.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected callable-predicate diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "confined predicates should support exact bool results in "
         "conditions, initializers, assignments, and returns");

  const lang::FunctionDecl *accepts =
      findTopLevelFunction(frontend.program, "accepts");
  const lang::FunctionInfo *acceptsInfo =
      accepts == nullptr ? nullptr : frontend.semantics.findFunction(*accepts);
  expect(acceptsInfo != nullptr &&
             acceptsInfo->callableParameters.size() == 1 &&
             acceptsInfo->callableParameters.front().signatures.size() == 1 &&
             acceptsInfo->callableParameters.front()
                     .signatures.front()
                     .returnType == lang::SemanticType::Bool,
         "semantic callable contracts should retain an exact bool predicate "
         "result requirement");

  std::size_t predicateInstances = 0;
  bool foundLambdaPredicate = false;
  bool foundObjectPredicate = false;
  bool hirPredicateValues = true;
  bool mirPredicateValues = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "accepts") {
      continue;
    }
    ++predicateInstances;
    if (instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().signatures.size() != 1) {
      hirPredicateValues = false;
      continue;
    }
    const lang::HirCallableParameter &contract =
        instance.callableParameters.front();
    const lang::HirCallableSignature &signature = contract.signatures.front();
    foundLambdaPredicate =
        foundLambdaPredicate ||
        (contract.callableType.kind == lang::SemanticType::Lambda &&
         signature.returnType == lang::SemanticType::Bool &&
         signature.lambdaTarget && !signature.functionTarget);
    foundObjectPredicate =
        foundObjectPredicate ||
        (contract.callableType.kind == lang::SemanticType::Class &&
         signature.returnType == lang::SemanticType::Bool &&
         signature.functionTarget && !signature.lambdaTarget);
    hirPredicateValues =
        hirPredicateValues &&
        std::any_of(instance.body.values.begin(), instance.body.values.end(),
                    [](const lang::HirValue &value) {
                      return value.kind == lang::HirValueKind::Call &&
                             value.callableBoundary ==
                                 lang::CallableBoundary::Confined &&
                             value.info.type == lang::SemanticType::Bool;
                    });

    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    bool foundMirPredicate = false;
    if (mir != nullptr && mir->callableParameters.size() == 1 &&
        mir->callableParameters.front().signatures.size() == 1 &&
        mir->callableParameters.front().signatures.front().returnType ==
            lang::SemanticType::Bool) {
      for (const lang::MirBlock &block : mir->body.blocks) {
        foundMirPredicate =
            foundMirPredicate ||
            std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [](const lang::MirInstruction &instruction) {
                  return instruction.kind == lang::MirInstructionKind::Call &&
                         instruction.callableBoundary ==
                             lang::CallableBoundary::Confined &&
                         instruction.info.type == lang::SemanticType::Bool;
                });
      }
    }
    mirPredicateValues = mirPredicateValues && foundMirPredicate;
  }
  expect(predicateInstances == 2 && foundLambdaPredicate &&
             foundObjectPredicate && hirPredicateValues && mirPredicateValues,
         "concrete HIR and MIR predicate instances should retain exact bool "
         "result types and selected callable identities");

  expect(mirHasCallableInvocation(frontend,
                                  lang::CallableInvocationCapability::Read) &&
             mirCallsOperator(frontend, lang::OverloadedOperator::Call) &&
             nativeBackendEmits(frontend, lang::OptimizationLevel::O1),
         "lowering should preserve predicate values through the exact "
         "callable bridge");

  const lang::FrontendResult wrongReturn =
      lang::Frontend().analyze("invalid-predicate-return.gti", R"(
bool accepts<Predicate>(int value, Predicate predicate) {
  return predicate(value);
}

int main() {
  auto wrong = [](int value) -> int { return value; };
  [[discard]] accepts(1, wrong);
  return 0;
}
)");
  expect(!wrongReturn.canGenerateCode() &&
             hasDiagnostic(wrongReturn.diagnostics,
                           "must return 'bool' for this invocation") &&
             hasRelatedDiagnostic(wrongReturn.diagnostics,
                                  "Concrete generic instance requested here"),
         "predicate instantiation should reject non-bool callable results "
         "before backend lowering");

  const lang::FrontendResult valueResults = lang::Frontend().analyze(
      "callable-value-results.gti", R"(
T map<T, Operation>(T value, Operation operation) {
  return operation(std::move(value));
}

T map_twice<T, Operation>(T value, Operation operation) {
  mut T result = operation(std::move(value));
  result = operation(std::move(result));
  return std::move(result);
}

class Increment {
public:
  int operator()(int value) { return value + 1; }
public:
  Increment() = default;
};

class Counter {
  int value;

public:
  Counter(int value) : value(value) {}
  Counter(Counter& other) = delete;
  Counter(Counter&& other) = default;

  int read() { return this.value; }
};

class Advance {
public:
  Counter operator()(Counter value) { return Counter(value.read() + 1); }
public:
  Advance() = default;
};

int main() {
  auto increment = [](int value) -> int { return value + 1; };
  int mapped = map(1, increment);
  int mapped_twice = map_twice(1, increment);
  Increment object = Increment();
  int object_result = map(4, object);
  Advance advance = Advance();
  Counter advanced = map_twice(Counter(6), advance);
  return mapped + mapped_twice + object_result + advanced.read() - 18;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valueResults.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valueResults.diagnostics) {
      std::cerr << "Unexpected callable-value diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valueResults.canGenerateCode(),
         "confined generic callables should return an exact value supplied by "
         "an explicit binding, assignment, or return context");

  bool foundValueRequirement = false;
  bool foundConfinedValueCall = false;
  bool foundMoveOnlyValueRequirement = false;
  bool foundMoveOnlyMirRequirement = false;
  const lang::ClassDecl *counterDeclaration =
      findTopLevelClass(valueResults.program, "Counter");
  const lang::ClassTypeInfo *counterInfo =
      counterDeclaration == nullptr
          ? nullptr
          : valueResults.semantics.findClassType(*counterDeclaration);
  for (const lang::HirFunctionInstance &instance :
       valueResults.hir.functionInstances()) {
    if (instance.source == nullptr ||
        (instance.source->name().lexeme != "map" &&
         instance.source->name().lexeme != "map_twice")) {
      continue;
    }
    foundValueRequirement =
        foundValueRequirement ||
        (instance.callableParameters.size() == 1 &&
         std::all_of(
             instance.callableParameters.front().signatures.begin(),
             instance.callableParameters.front().signatures.end(),
             [](const lang::HirCallableSignature &signature) {
               return signature.returnType == lang::SemanticType::Int32 &&
                      (signature.lambdaTarget || signature.functionTarget);
             }));
    foundConfinedValueCall =
        foundConfinedValueCall ||
        std::any_of(instance.body.values.begin(), instance.body.values.end(),
                    [](const lang::HirValue &value) {
                      return value.kind == lang::HirValueKind::Call &&
                             value.callableBoundary ==
                                 lang::CallableBoundary::Confined &&
                             value.info.type == lang::SemanticType::Int32;
                    });
    if (counterInfo != nullptr &&
        instance.source->name().lexeme == "map_twice" &&
        instance.callableParameters.size() == 1 &&
        std::any_of(instance.callableParameters.front().signatures.begin(),
                    instance.callableParameters.front().signatures.end(),
                    [&](const lang::HirCallableSignature &signature) {
                      return signature.returnType.kind ==
                                 lang::SemanticType::Class &&
                             signature.returnType.classId == counterInfo->id &&
                             signature.functionTarget.has_value();
                    })) {
      foundMoveOnlyValueRequirement = true;
      const lang::MirFunctionInstance *mir =
          valueResults.mir.findFunctionInstance(instance.id);
      foundMoveOnlyMirRequirement =
          mir != nullptr && mir->callableParameters.size() == 1 &&
          std::any_of(
              mir->callableParameters.front().signatures.begin(),
              mir->callableParameters.front().signatures.end(),
              [&](const lang::MirCallableSignature &signature) {
                return signature.returnType.kind == lang::SemanticType::Class &&
                       signature.returnType.classId == counterInfo->id &&
                       signature.functionTarget.has_value();
              });
    }
  }
  expect(foundValueRequirement && foundConfinedValueCall &&
             foundMoveOnlyValueRequirement && foundMoveOnlyMirRequirement,
         "HIR should retain exact value-result signatures and confined call "
         "boundaries for scalar and move-only class results");

  const lang::FrontendResult nestedResults =
      lang::Frontend().analyze("nested-callable-results.gti", R"(
int compose<Outer, Inner>(Outer outer, Inner inner, int value) {
  return outer(inner(value));
}
double widen(int value) { return double(value) + 0.5; }
class Narrow {
public:
  int operator()(double value) { return int(value); }
public:
  Narrow() = default;
};
int main() {
  auto inner = [](int value) -> double { return double(value) + 0.5; };
  auto outer = [](double value) -> int { return int(value); };
  Narrow narrow{};
  return compose(outer, inner, 4) + compose(narrow, widen, 5) - 9;
}
)");
  if (!nestedResults.canGenerateCode()) {
    for (const auto &diagnostic : nestedResults.diagnostics)
      std::cerr << "Unexpected nested-callable diagnostic: "
                << diagnostic.message << '\n';
  }
  expect(nestedResults.canGenerateCode() &&
             lang::verifyMirProgram(nestedResults.mir).valid(),
         "direct open-callable composition must infer a concrete intermediate "
         "without guessing it from the enclosing result");
  const auto *compose = findTopLevelFunction(nestedResults.program, "compose");
  const auto *composeInfo =
      compose ? nestedResults.semantics.findFunction(*compose) : nullptr;
  bool deferredIntermediate = false;
  if (composeInfo) {
    for (const auto &parameter : composeInfo->callableParameters)
      for (const auto &signature : parameter.signatures)
        deferredIntermediate |=
            signature.inferResultFromInstantiation &&
            signature.returnType == lang::SemanticType::Unknown;
  }
  bool concreteSignatures = true;
  std::size_t composedInstances = 0;
  for (const auto &instance : nestedResults.hir.functionInstances()) {
    if (instance.source != compose)
      continue;
    ++composedInstances;
    bool innerDouble = false;
    bool outerInt = false;
    for (const auto &parameter : instance.callableParameters) {
      for (const auto &signature : parameter.signatures) {
        concreteSignatures &=
            signature.returnType != lang::SemanticType::Unknown &&
            std::none_of(signature.parameterTypes.begin(),
                         signature.parameterTypes.end(), [](const auto &type) {
                           return type == lang::SemanticType::Unknown;
                         });
        innerDouble |=
            signature.returnType == lang::SemanticType::Double &&
            signature.parameterTypes ==
                std::vector<lang::SemanticType>{lang::SemanticType::Int32};
        outerInt |= signature.returnType == lang::SemanticType::Int32 &&
                    signature.parameterTypes == std::vector<lang::SemanticType>{
                                                    lang::SemanticType::Double};
      }
    }
    concreteSignatures &= innerDouble && outerInt;
  }
  expect(deferredIntermediate && composedInstances == 2 && concreteSignatures,
         "primary semantic inference obligations must become exact selected "
         "callable signatures before concrete HIR and verified MIR");

  const lang::FrontendResult nestedVoid =
      lang::Frontend().analyze("nested-void-callable.gti", R"(
int compose<Outer, Inner>(Outer outer, Inner inner, int value) {
  return outer(inner(value));
}
int main() {
  auto inner = [](int value) -> void {};
  auto outer = [](int value) -> int { return value; };
  return compose(outer, inner, 4);
}
)");
  expect(
      !nestedVoid.canGenerateCode() &&
          hasDiagnosticCode(nestedVoid.diagnostics, "GTI-S2046") &&
          hasDiagnostic(nestedVoid.diagnostics,
                        "inferred generic callable result must be a "
                        "non-void value") &&
          hasDiagnosticHint(nestedVoid.diagnostics,
                            "explicit result binding") &&
          !hasDiagnosticCode(nestedVoid.diagnostics, "GTI-B0001"),
      "nested inference must reject void producers with a semantic diagnostic");

  const lang::FrontendResult wrongNestedArgument =
      lang::Frontend().analyze("wrong-nested-argument.gti", R"(
int compose<Outer, Inner>(Outer outer, Inner inner, int value) {
  return outer(inner(value));
}
int main() {
  auto inner = [](int value) -> double { return 0.5; };
  auto outer = [](bool value) -> int { return 1; };
  return compose(outer, inner, 4);
}
)");
  expect(!wrongNestedArgument.canGenerateCode() &&
             !hasDiagnosticCode(wrongNestedArgument.diagnostics, "GTI-B0001"),
         "nested result inference must still check the outer exact parameter "
         "type");

  const lang::FrontendResult nestedReference =
      lang::Frontend().analyze("nested-reference-callable.gti", R"(
class ReferenceSource {
  int value = 7;
public:
  int& operator()(int unused) { return this.value; }
public:
  ReferenceSource() = default;
};
int compose<Outer, Inner>(Outer outer, Inner inner, int value) {
  return outer(inner(value));
}
int main() {
  ReferenceSource inner{};
  auto outer = [](int value) -> int { return value; };
  return compose(outer, inner, 4);
}
)");
  expect(
      !nestedReference.canGenerateCode() &&
          hasDiagnosticCode(nestedReference.diagnostics, "GTI-S2046") &&
          hasDiagnostic(nestedReference.diagnostics,
                        "without references, tracked borrowed state, or lambda "
                        "identity") &&
          !hasDiagnosticCode(nestedReference.diagnostics, "GTI-B0001"),
      "nested result inference must not erase receiver-tied reference origins");

  const lang::FrontendResult nestedBorrow =
      lang::Frontend().analyze("nested-borrowed-callable.gti", R"(
struct View {
  int& value;
  View(int& source) : value(source) {}
};
class ViewSource {
  int value = 7;
public:
  View operator()(int unused) { return View(this.value); }
public:
  ViewSource() = default;
};
int compose<Outer, Inner>(Outer outer, Inner inner, int value) {
  return outer(inner(value));
}
class ReadView {
public:
  int operator()(View value) { return value.value; }
public:
  ReadView() = default;
};
int main() {
  ViewSource inner{};
  ReadView outer{};
  return compose(outer, inner, 4);
}
)");
  if (!hasDiagnosticCode(nestedBorrow.diagnostics, "GTI-S2046")) {
    for (const auto &diagnostic : nestedBorrow.diagnostics)
      std::cerr << "Unexpected nested-borrow diagnostic: " << diagnostic.message
                << '\n';
  }
  expect(!nestedBorrow.canGenerateCode() &&
             hasDiagnosticCode(nestedBorrow.diagnostics, "GTI-S2046") &&
             hasDiagnostic(nestedBorrow.diagnostics,
                           "without references, tracked borrowed state, or "
                           "lambda identity") &&
             !hasDiagnosticCode(nestedBorrow.diagnostics, "GTI-B0001"),
         "nested result inference must not discard a borrowed aggregate's "
         "owner dependencies");

  const lang::FrontendResult wrongValueResult =
      lang::Frontend().analyze("wrong-callable-value-result.gti", R"(
int map<Operation>(int value, Operation operation) {
  return operation(value);
}

class Fraction {
public:
  double operator()(int value) { return 0.5; }
public:
  Fraction() = default;
};

int main() {
  Fraction fraction = Fraction();
  return map(1, fraction);
}
)");
  expect(!wrongValueResult.canGenerateCode() &&
             hasDiagnostic(wrongValueResult.diagnostics,
                           "must return 'int32_t' for this invocation") &&
             hasDiagnostic(wrongValueResult.diagnostics,
                           "selected callable returns 'double'") &&
             !hasDiagnosticCode(wrongValueResult.diagnostics, "GTI-B0001"),
         "a confined callable must return the exact contextual value type");

  const lang::FrontendResult inferredResult =
      lang::Frontend().analyze("inferred-callable-result.gti", R"(
void infer<Operation>(int value, Operation operation) {
  auto result = operation(value);
}

int main() { return 0; }
)");
  expect(!inferredResult.canGenerateCode() &&
             hasDiagnostic(inferredResult.diagnostics,
                           "cannot be inferred with 'auto'") &&
             hasDiagnosticHint(inferredResult.diagnostics,
                               "explicit result binding") &&
             inferredResult.diagnostics.size() == 1,
         "generic callable result inference should remain closed until the "
         "result type has an exact context");

  const lang::FrontendResult borrowedResult =
      lang::Frontend().analyze("borrowed-callable-result.gti", R"(
class BorrowView {
  int& value;

public:
  BorrowView(int& source) : value(source) {}
  int& get() { return this.value; }
};

BorrowView apply<Operation>(int& source, Operation operation) {
  return operation(source);
}

int main() { return 0; }
)");
  expect(
      !borrowedResult.canGenerateCode() &&
          countDiagnosticCode(borrowedResult.diagnostics, "GTI-S2046") == 1 &&
          countDiagnosticCode(borrowedResult.diagnostics, "GTI-S2045") == 0 &&
          hasDiagnostic(borrowedResult.diagnostics,
                        "must be exact non-reference values") &&
          !hasDiagnosticCode(borrowedResult.diagnostics, "GTI-B0001"),
      "a rejected borrowed callable result should produce one focused "
      "callable diagnostic without an owner-origin cascade");
}

void testConstructorRollbackSpansLocalCleanup() {
  // A constructor completes only after its body-local cleanup: a failing
  // local destructor must still roll back the completed field.
  const std::string source = R"(
mut int32_t zero = 0;

class Resource {
public:
  int32_t identity;
  Resource(int32_t value) : identity(value) {}
  ~Resource() {
    if (this.identity == 4) { int32_t failure = 19 / zero; }
  }
};

class Holder {
public:
  Resource completed;
  Holder() : completed(Resource(5)) { Resource local{4}; }
};

int main() {
  Holder holder{};
  return holder.completed.identity - 5;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "constructor-rollback-local-cleanup.gti", source);
  expect(frontend.canGenerateCode(),
         "the constructor rollback local-cleanup fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirBody *body = nullptr;
  lang::MirDropObligationId rollback = 0;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    for (const lang::MirDropObligation &obligation :
         constructor.body.dropObligations) {
      if (obligation.kind ==
          lang::MirDropObligationKind::ConstructionRollback) {
        body = &constructor.body;
        rollback = obligation.id;
      }
    }
  }
  expect(body != nullptr && rollback != 0,
         "Holder's constructor should arm rollback for its completed field");
  if (body == nullptr) {
    return;
  }
  const lang::MirBlock *dropBlock = nullptr;
  std::size_t dropIndex = 0;
  for (const lang::MirBlock &block : body->blocks) {
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const lang::MirInstruction &instruction = block.instructions[index];
      // The local's own drop, not the failure drain's fallible field drop.
      if (dropBlock == nullptr &&
          instruction.kind == lang::MirInstructionKind::Drop &&
          block.terminator.invokeInstruction == instruction.id &&
          block.terminator.elseTarget != 0 &&
          std::none_of(instruction.lifecycle.begin(),
                       instruction.lifecycle.end(),
                       [](const lang::MirLifecycleEvent &event) {
                         return event.failureCleanup;
                       })) {
        dropBlock = &block;
        dropIndex = index;
      }
    }
  }
  expect(dropBlock != nullptr,
         "the local's fallible destructor should end its block with a failure "
         "edge");
  if (dropBlock == nullptr) {
    return;
  }
  bool retiredBeforeLocalDrop = false;
  bool retired = false;
  bool failureDrainsField = false;
  for (const lang::MirBlock &block : body->blocks) {
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const lang::MirInstruction &instruction = block.instructions[index];
      for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.source != rollback) {
          continue;
        }
        if (event.kind == lang::MirLifecycleEventKind::TransferOut) {
          retired = true;
          retiredBeforeLocalDrop = retiredBeforeLocalDrop ||
                                   (&block == dropBlock && index < dropIndex);
        }
        if (block.id == dropBlock->terminator.elseTarget &&
            instruction.kind == lang::MirInstructionKind::Drop &&
            event.failureCleanup) {
          failureDrainsField = true;
        }
      }
    }
  }
  expect(retired && !retiredBeforeLocalDrop,
         "normal completion should retire construction rollback only after "
         "the body's scope-exit drops");
  expect(failureDrainsField,
         "the failure edge of a local destructor in a constructor should drain "
         "the armed field rollback");
}

void testFieldInitializerRollbackAndFailureEdges() {
  const std::string source = R"(
static mut int32_t seed = 4;

class Resource {
public:
  int32_t value;
  Resource(int32_t s) : value(s) {}
  ~Resource() {}
};

class Bundle {
public:
  Resource held = Resource(5);
  int32_t doubled = seed + seed;
  Bundle() {}
};

int main() {
  Bundle bundle = Bundle();
  return bundle.held.value + bundle.doubled - 13;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("field-initializer-rollback.gti", source);
  expect(frontend.canGenerateCode(),
         "the field-initializer rollback fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirBody *bundleInitializers = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.declaredFields.size() == 2) {
      bundleInitializers = &instance.fieldInitializers;
    }
  }
  expect(bundleInitializers != nullptr,
         "the fixture should lower Bundle's field-initializer body");
  if (bundleInitializers == nullptr) {
    return;
  }
  const lang::MirDropObligation *rollback = nullptr;
  for (const lang::MirDropObligation &obligation :
       bundleInitializers->dropObligations) {
    if (obligation.kind == lang::MirDropObligationKind::ConstructionRollback) {
      rollback = &obligation;
    }
  }
  expect(rollback != nullptr &&
             lang::mirBodyRoutesFailureEdges(*bundleInitializers) &&
             !bundleInitializers->failureRecords.empty(),
         "the owning declaration initializer should arm rollback and the "
         "checked initializer should keep its failure edge");
  if (rollback == nullptr) {
    return;
  }
  bool failureDrainsHeld = false;
  for (const lang::MirBlock &block : bundleInitializers->blocks) {
    if (block.failureParameter == 0) {
      continue;
    }
    for (const lang::MirInstruction &instruction : block.instructions) {
      failureDrainsHeld =
          failureDrainsHeld ||
          (instruction.kind == lang::MirInstructionKind::Drop &&
           instruction.lifecycle.size() == 1 &&
           instruction.lifecycle.front().failureCleanup &&
           instruction.lifecycle.front().source == rollback->id);
    }
  }
  expect(failureDrainsHeld,
         "the field-initializer failure edge must destroy the completed "
         "owning field");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "the rollback-bearing field-initializer program should verify");
}

void testDefaultFieldRollbackEntersConstructor() {
  const std::string source = R"(
class Resource {
public:
  int32_t value;
  Resource(int32_t seed) : value(seed) {}
  ~Resource() {}
};

class Bundle {
public:
  Resource held = Resource(5);
  int32_t checked = 0;
  Bundle(int32_t seed) : checked(seed + seed) {}
public:
  Bundle() = default;
};

int main() {
  Bundle bundle = Bundle(4);
  return bundle.held.value + bundle.checked - 13;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "default-field-constructor-rollback.gti", source);
  expect(frontend.canGenerateCode(),
         "a constructor with an owning default field should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *bundleConstructor = nullptr;
  const lang::MirDropObligation *fieldRollback = nullptr;
  std::size_t rollbackCount = 0;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    for (const lang::MirDropObligation &drop :
         constructor.body.dropObligations) {
      if (drop.kind == lang::MirDropObligationKind::ConstructionRollback) {
        bundleConstructor = &constructor;
        fieldRollback = &drop;
        ++rollbackCount;
      }
    }
  }
  expect(bundleConstructor != nullptr && fieldRollback != nullptr &&
             rollbackCount == 1 && !fieldRollback->initiallyActive,
         "the selected declaration initializer should have exactly one "
         "rollback obligation, inactive until construction completes");
  if (bundleConstructor == nullptr || fieldRollback == nullptr) {
    return;
  }

  const lang::MirClassInstance *owner =
      frontend.mir.findClassInstance(bundleConstructor->owner);
  const lang::MirPlace *fieldPlace =
      bundleConstructor->body.findPlace(fieldRollback->place);
  expect(owner != nullptr && fieldPlace != nullptr &&
             fieldPlace->root == lang::MirPlaceRootKind::This &&
             fieldPlace->projections.size() == 1 &&
             fieldPlace->projections.front().kind ==
                 lang::MirProjectionKind::Field,
         "the selected-field rollback should name one exact This-rooted "
         "field");
  if (owner == nullptr || fieldPlace == nullptr ||
      fieldPlace->projections.size() != 1) {
    return;
  }

  const lang::SymbolId fieldSymbol = fieldPlace->projections.front().field;
  std::size_t matchingInitializerRollbacks = 0;
  for (const lang::MirDropObligation &drop :
       owner->fieldInitializers.dropObligations) {
    const lang::MirPlace *place =
        owner->fieldInitializers.findPlace(drop.place);
    matchingInitializerRollbacks +=
        drop.kind == lang::MirDropObligationKind::ConstructionRollback &&
                place != nullptr &&
                place->root == lang::MirPlaceRootKind::Binding &&
                place->symbol == fieldSymbol &&
                drop.dropType == fieldRollback->dropType
            ? 1
            : 0;
  }
  expect(matchingInitializerRollbacks == 1,
         "selected constructor rollback must preserve the exact declaration "
         "field's cleanup recipe");

  std::size_t exactFieldActivations = 0;
  bool failureDropsField = false;
  bool normalTransfersField = false;
  for (const lang::MirBlock &block : bundleConstructor->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.kind == lang::MirLifecycleEventKind::Reparent &&
            event.target == fieldRollback->id &&
            instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.constructorInitializer != 0 &&
            instruction.constructorInitializer <=
                bundleConstructor->initializers.size()) {
          const auto &selected =
              bundleConstructor
                  ->initializers[instruction.constructorInitializer - 1];
          const auto *source =
              bundleConstructor->body.findDropObligation(event.source);
          exactFieldActivations +=
              selected.field == fieldSymbol && selected.arguments.size() == 1 &&
              source && source->kind == lang::MirDropObligationKind::Value &&
              source->value == selected.arguments.front() &&
              source->dropType == fieldRollback->dropType &&
              !event.conditional && !event.failureCleanup;
        }
        failureDropsField =
            failureDropsField ||
            (block.failureParameter != 0 &&
             event.kind == lang::MirLifecycleEventKind::Drop &&
             event.source == fieldRollback->id && event.failureCleanup);
        normalTransfersField =
            normalTransfersField ||
            (block.failureParameter == 0 &&
             event.kind == lang::MirLifecycleEventKind::TransferOut &&
             event.source == fieldRollback->id && !event.failureCleanup);
      }
    }
  }
  expect(exactFieldActivations == 1 && failureDropsField &&
             normalTransfersField,
         "one completed default value must arm its exact field rollback; "
         "failure drops that field and success transfers it to the object");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "the default-field constructor rollback program should verify");

  lang::MirProgram forged = frontend.mir;
  auto &constructors = const_cast<std::vector<lang::MirConstructorInstance> &>(
      forged.constructorInstances());
  bool forgedField = false;
  for (lang::MirConstructorInstance &constructor : constructors) {
    for (const lang::MirDropObligation &drop :
         constructor.body.dropObligations) {
      if (constructor.id != bundleConstructor->id ||
          drop.id != fieldRollback->id) {
        continue;
      }
      auto &places =
          const_cast<std::vector<lang::MirPlace> &>(constructor.body.places);
      places[drop.place - 1].projections.front().field = fieldSymbol + 1;
      forgedField = true;
    }
  }
  expect(forgedField && !lang::verifyMirProgram(forged).valid(),
         "the program verifier should reject a selected rollback that is not "
         "derived from its owner's exact field-initializer schedule");
  for (unsigned mutation = 0; mutation < 2; ++mutation) {
    auto malformed = frontend.mir;
    auto *constructor = const_cast<lang::MirConstructorInstance *>(
        malformed.findConstructorInstance(bundleConstructor->id));
    bool changed = false;
    if (mutation == 0) {
      auto *drop = const_cast<lang::MirDropObligation *>(
          constructor->body.findDropObligation(fieldRollback->id));
      drop->initiallyActive = true;
      changed = true;
    } else {
      for (auto &block : constructor->body.blocks)
        for (auto &instruction : block.instructions)
          changed |=
              std::erase_if(instruction.lifecycle, [&](const auto &event) {
                return event.kind == lang::MirLifecycleEventKind::Reparent &&
                       event.target == fieldRollback->id;
              }) != 0;
    }
    expect(changed && !lang::verifyMirProgram(malformed).valid(),
           "a selected field cannot start cleanup before construction or "
           "lose its exactly-once completion activation");
  }

  const lang::FrontendResult overridden =
      lang::Frontend().analyze("overridden-default-field-constructor.gti", R"(
class Resource {
public:
  int32_t value;
  Resource(int32_t seed) : value(seed) {}
  ~Resource() {}
};
class Bundle {
public:
  Resource held = Resource(5);
  Bundle(int32_t seed) : held(Resource(seed)) {}
public:
  Bundle() = default;
};
int main() { Bundle bundle = Bundle(7); return bundle.held.value - 7; }
)");
  expect(overridden.canGenerateCode(),
         "an explicit initializer should continue to override the in-class "
         "initializer");
  if (overridden.canGenerateCode()) {
    bool inheritedDefaultRollback = false;
    std::size_t selectedFieldConstructions = 0;
    for (const lang::MirConstructorInstance &constructor :
         overridden.mir.constructorInstances()) {
      inheritedDefaultRollback =
          inheritedDefaultRollback ||
          std::any_of(
              constructor.body.dropObligations.begin(),
              constructor.body.dropObligations.end(),
              [](const lang::MirDropObligation &drop) {
                return drop.kind ==
                           lang::MirDropObligationKind::ConstructionRollback &&
                       drop.initiallyActive;
              });
      if (constructor.initializers.size() != 1 ||
          constructor.initializers.front().targetType.kind !=
              lang::SemanticType::Class)
        continue;
      for (const auto &block : constructor.body.blocks)
        for (const auto &instruction : block.instructions)
          selectedFieldConstructions +=
              instruction.kind == lang::MirInstructionKind::Construct &&
              instruction.constructorTarget.has_value();
    }
    expect(!inheritedDefaultRollback && selectedFieldConstructions == 1 &&
               lang::verifyMirProgram(overridden.mir).valid(),
           "an explicitly initialized field must have one selected "
           "construction, without also running or inheriting its default");
  }
}

void testConstructorPartialRollbackRepresentation() {
  const std::string source = R"(
class Resource {
public:
  int32_t value;
  Resource(int32_t seed) : value(seed) {}
  ~Resource() {}
};

class Pair {
public:
  Resource first;
  Resource second;
  Pair(int32_t a, int32_t b) : first(Resource(a)), second(Resource(a + b)) {}
};

int main() {
  Pair pair = Pair(1, 2);
  return pair.first.value + pair.second.value - 4;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("constructor-rollback.gti", source);
  expect(frontend.canGenerateCode(),
         "the constructor-rollback fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *pairConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() == 2) {
      pairConstructor = &constructor;
    }
  }
  expect(pairConstructor != nullptr,
         "the fixture should lower the two-field constructor");
  if (pairConstructor == nullptr) {
    return;
  }
  const lang::MirBody &body = pairConstructor->body;

  std::vector<const lang::MirDropObligation *> rollback;
  for (const lang::MirDropObligation &obligation : body.dropObligations) {
    if (obligation.kind != lang::MirDropObligationKind::ConstructionRollback) {
      continue;
    }
    const lang::MirPlace *place = body.findPlace(obligation.place);
    expect(place != nullptr && place->root == lang::MirPlaceRootKind::This &&
               place->projections.size() == 1 &&
               place->projections.front().kind ==
                   lang::MirProjectionKind::Field,
           "every rollback obligation should own one exact This-rooted field "
           "place");
    rollback.push_back(&obligation);
  }
  expect(rollback.size() == 2,
         "each completed class-field stage should arm one rollback "
         "obligation");
  if (rollback.size() != 2) {
    return;
  }

  std::size_t armingStages = 0;
  bool retired = false;
  const lang::MirBlock *failureBlock = nullptr;
  for (const lang::MirBlock &block : body.blocks) {
    if (block.failureParameter != 0) {
      failureBlock = &block;
    }
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.constructorInitializer != 0) {
        for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
          if (event.kind == lang::MirLifecycleEventKind::Reparent &&
              (event.target == rollback[0]->id ||
               event.target == rollback[1]->id)) {
            ++armingStages;
          }
        }
      }
      if (instruction.kind == lang::MirInstructionKind::Lifecycle &&
          instruction.lifecycle.size() == 2 &&
          std::all_of(
              instruction.lifecycle.begin(), instruction.lifecycle.end(),
              [&](const lang::MirLifecycleEvent &event) {
                return event.kind == lang::MirLifecycleEventKind::TransferOut &&
                       (event.source == rollback[0]->id ||
                        event.source == rollback[1]->id);
              })) {
        retired = true;
      }
    }
  }
  expect(armingStages == 2,
         "each stage-completing Initialize should reparent its temporary "
         "into the armed rollback obligation");
  expect(retired, "normal completion should retire both rollback obligations "
                  "by transfer to the caller");
  expect(failureBlock != nullptr,
         "the checked second-stage initializer should produce a failure "
         "edge inside the constructor body");
  if (failureBlock == nullptr) {
    return;
  }

  bool droppedFirst = false;
  bool droppedSecond = false;
  for (const lang::MirInstruction &instruction : failureBlock->instructions) {
    if (instruction.kind != lang::MirInstructionKind::Drop ||
        instruction.lifecycle.size() != 1 ||
        !instruction.lifecycle.front().failureCleanup) {
      continue;
    }
    droppedFirst =
        droppedFirst || instruction.lifecycle.front().source == rollback[0]->id;
    droppedSecond = droppedSecond ||
                    instruction.lifecycle.front().source == rollback[1]->id;
  }
  expect(droppedFirst && !droppedSecond,
         "the failure edge must drop exactly the completed first subobject "
         "and never the unfinished second");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "the rollback-bearing program should verify");

  lang::MirProgram leaking = frontend.mir;
  auto &leakingConstructors =
      const_cast<std::vector<lang::MirConstructorInstance> &>(
          leaking.constructorInstances());
  for (lang::MirConstructorInstance &constructor : leakingConstructors) {
    if (constructor.initializers.size() != 2) {
      continue;
    }
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(constructor.body.blocks)) {
      if (block.failureParameter == 0) {
        continue;
      }
      auto &instructions =
          const_cast<std::vector<lang::MirInstruction> &>(block.instructions);
      std::erase_if(instructions, [&](const lang::MirInstruction &candidate) {
        return candidate.kind == lang::MirInstructionKind::Drop;
      });
    }
  }
  expect(!lang::verifyMirProgram(leaking).valid(),
         "a forged failure edge that leaks the completed subobject must be "
         "rejected");
}

void testMutableTemporaryReceivers() {
  const std::string source = R"(
class Probe {
public:
  Probe(int initial) : value(initial) {}

  int update(int amount) mut {
    this.value += amount;
    return this.value;
  }

  int inspect() { return 1; }
  int inspect() mut { return 2; }

private:
  mut int value;
};

class Observer {
public:
  Observer() {}
  int read() { return 3; }
};

class Invoker {
public:
  Invoker() {}
  int operator()() { return 4; }
  int operator()() mut { return 5; }
};

class GenericProbe<T> {
public:
  GenericProbe(T initial) : value(initial) {}

  T replace(T replacement) mut {
    this.value = replacement;
    return this.value;
  }

private:
  mut T value;
};

Probe make_probe() {
  return Probe(0);
}

int consume(int value) {
  return value;
}

int main() {
  int updated = Probe(4).update(3);
  int inspected = make_probe().inspect();
  int observed = Observer().read();
  int invoked = Invoker()();
  int generic = GenericProbe<int>(6).replace(7);
  int nested = consume((Probe(8)).update(1));
  return updated + inspected + observed + invoked + generic + nested;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("mutable-temporary-receivers.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected mutable-temporary diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "direct constructor and GTI-call temporaries should lower to "
         "verified receiver borrows");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto hirFunction =
      [&](std::string_view name) -> const lang::HirFunctionInstance * {
    const auto found =
        std::find_if(frontend.hir.functionInstances().begin(),
                     frontend.hir.functionInstances().end(),
                     [&](const lang::HirFunctionInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->name().lexeme == name;
                     });
    return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
  };
  const lang::HirFunctionInstance *main = hirFunction("main");
  expect(main != nullptr,
         "the mutable-temporary fixture should retain its main instance");
  if (main == nullptr) {
    return;
  }

  std::vector<const lang::HirValue *> temporaryCalls;
  for (const lang::HirValue &value : main->body.values) {
    if (value.kind == lang::HirValueKind::Call && value.callPlan &&
        value.callPlan->receiver &&
        (value.callPlan->receiver->kind ==
             lang::HirCallInputKind::ReadTemporaryBorrow ||
         value.callPlan->receiver->kind ==
             lang::HirCallInputKind::MutableTemporaryBorrow)) {
      temporaryCalls.push_back(&value);
    }
  }
  expect(temporaryCalls.size() == 6,
         "HIR should preserve direct, parenthesized, nested, and concrete "
         "generic temporary receiver calls");
  std::size_t mutableHirCalls = 0;
  std::size_t readHirCalls = 0;
  for (const lang::HirValue *call : temporaryCalls) {
    const lang::HirCallInputKind kind = call->callPlan->receiver->kind;
    mutableHirCalls +=
        kind == lang::HirCallInputKind::MutableTemporaryBorrow ? 1 : 0;
    readHirCalls += kind == lang::HirCallInputKind::ReadTemporaryBorrow ? 1 : 0;
    const auto *sourceCall = dynamic_cast<const lang::Call *>(call->source);
    const lang::ResolvedCallInfo *resolved =
        sourceCall == nullptr ? nullptr
                              : frontend.semantics.findCall(*sourceCall);
    const lang::ResolvedOperatorInfo *resolvedOperator =
        sourceCall == nullptr ? nullptr
                              : frontend.semantics.findOperator(*sourceCall);
    const lang::CallReceiverMode mode =
        resolvedOperator != nullptr ? resolvedOperator->receiverMode
        : resolved != nullptr       ? resolved->receiverMode
                                    : lang::CallReceiverMode::None;
    expect((kind == lang::HirCallInputKind::MutableTemporaryBorrow &&
            mode == lang::CallReceiverMode::FreshTemporaryMutable) ||
               (kind == lang::HirCallInputKind::ReadTemporaryBorrow &&
                mode == lang::CallReceiverMode::FreshTemporaryRead),
           "HIR temporary receiver access should copy the selected semantic "
           "mode exactly");
  }
  expect(mutableHirCalls == 5 && readHirCalls == 1,
         "fresh receivers should prefer mutable method/operator overloads "
         "while retaining read-only-only calls");

  const lang::MirFunctionInstance *mirMain =
      frontend.mir.findFunctionInstance(main->id);
  expect(mirMain != nullptr,
         "the mutable-temporary fixture should retain its MIR main body");
  if (mirMain == nullptr) {
    return;
  }
  const auto definitionFor =
      [&](const lang::MirBody &body,
          const lang::MirOperand &operand) -> const lang::MirInstruction * {
    if (operand.kind != lang::MirOperandKind::Value) {
      return nullptr;
    }
    const lang::MirValue *value = body.findValue(operand.value);
    const lang::MirBlock *block =
        value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
    if (value == nullptr || block == nullptr) {
      return nullptr;
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.id == value->definition;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };

  std::size_t temporaryStages = 0;
  const lang::MirInstruction *ordinaryMutableCall = nullptr;
  for (const lang::MirBlock &block : mirMain->body.blocks) {
    for (const lang::MirInstruction &call : block.instructions) {
      if (call.kind != lang::MirInstructionKind::Call || !call.receiver) {
        continue;
      }
      const lang::MirInstruction *stage =
          definitionFor(mirMain->body, *call.receiver);
      if (stage == nullptr ||
          (stage->callInputKind !=
               lang::HirCallInputKind::ReadTemporaryBorrow &&
           stage->callInputKind !=
               lang::HirCallInputKind::MutableTemporaryBorrow)) {
        continue;
      }
      ++temporaryStages;
      const bool mutableStage = stage->callInputKind ==
                                lang::HirCallInputKind::MutableTemporaryBorrow;
      const lang::MirOperandKind expectedOperand =
          mutableStage ? lang::MirOperandKind::BorrowWrite
                       : lang::MirOperandKind::BorrowRead;
      const lang::MirPlace *place =
          stage->operands.empty()
              ? nullptr
              : mirMain->body.findPlace(stage->operands.front().place);
      const std::size_t drops =
          place == nullptr
              ? 0
              : static_cast<std::size_t>(std::count_if(
                    mirMain->body.dropObligations.begin(),
                    mirMain->body.dropObligations.end(),
                    [&](const lang::MirDropObligation &drop) {
                      return drop.kind == lang::MirDropObligationKind::Value &&
                             drop.value == stage->hirValue &&
                             drop.place == place->id;
                    }));
      expect(stage->callInputRole == lang::MirCallInputRole::Receiver &&
                 stage->operands.size() == 1 &&
                 stage->operands.front().kind == expectedOperand &&
                 place != nullptr &&
                 place->root == lang::MirPlaceRootKind::Value &&
                 place->access == lang::AccessMode::Mutable &&
                 place->sourceValue == stage->hirValue && drops == 1,
             "MIR should borrow one exact mutable temporary place with one "
             "full-expression drop obligation");
      if (mutableStage && !call.callableInvocation) {
        ordinaryMutableCall = &call;
      }
    }
  }
  expect(temporaryStages == 6 && ordinaryMutableCall != nullptr,
         "MIR should retain every temporary stage and an ordinary mutable "
         "target for verifier mutation coverage");
  expect(nativeBackendEmits(frontend),
         "the native backend should consume verified temporary receiver "
         "stages");

  if (ordinaryMutableCall != nullptr) {
    lang::MirProgram wrongAccess = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongAccess.functionInstances());
    lang::MirFunctionInstance *forgedMain =
        main->id == 0 || main->id > functions.size() ? nullptr
                                                     : &functions[main->id - 1];
    lang::MirInstruction *forgedStage = nullptr;
    if (forgedMain != nullptr) {
      for (lang::MirBlock &block :
           const_cast<std::vector<lang::MirBlock> &>(forgedMain->body.blocks)) {
        for (lang::MirInstruction &instruction :
             const_cast<std::vector<lang::MirInstruction> &>(
                 block.instructions)) {
          if (instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputKind ==
                  lang::HirCallInputKind::MutableTemporaryBorrow) {
            forgedStage = &instruction;
            break;
          }
        }
        if (forgedStage != nullptr) {
          break;
        }
      }
    }
    if (forgedMain != nullptr && forgedStage != nullptr &&
        !forgedStage->operands.empty()) {
      auto &places =
          const_cast<std::vector<lang::MirPlace> &>(forgedMain->body.places);
      places[forgedStage->operands.front().place - 1].access =
          lang::AccessMode::ReadOnly;
    }
    expect(!lang::verifyMirProgram(wrongAccess).valid(),
           "the MIR verifier should reject a mutable temporary borrow from "
           "read-only storage");

    lang::MirProgram wrongQualifier = frontend.mir;
    auto &qualifierFunctions =
        const_cast<std::vector<lang::MirFunctionInstance> &>(
            wrongQualifier.functionInstances());
    lang::MirFunctionInstance *qualifierMain =
        main->id == 0 || main->id > qualifierFunctions.size()
            ? nullptr
            : &qualifierFunctions[main->id - 1];
    if (qualifierMain != nullptr) {
      for (lang::MirBlock &block : const_cast<std::vector<lang::MirBlock> &>(
               qualifierMain->body.blocks)) {
        for (lang::MirInstruction &instruction :
             const_cast<std::vector<lang::MirInstruction> &>(
                 block.instructions)) {
          if (instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputKind ==
                  lang::HirCallInputKind::MutableTemporaryBorrow) {
            instruction.callInputKind =
                lang::HirCallInputKind::ReadTemporaryBorrow;
            instruction.operands.front().kind =
                lang::MirOperandKind::BorrowRead;
            break;
          }
        }
      }
    }
    expect(!lang::verifyMirProgram(wrongQualifier).valid(),
           "the MIR verifier should reject a temporary input whose access "
           "disagrees with the exact target qualifier");

    lang::MirProgram missingDrop = frontend.mir;
    auto &dropFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        missingDrop.functionInstances());
    lang::MirFunctionInstance *dropMain =
        main->id == 0 || main->id > dropFunctions.size()
            ? nullptr
            : &dropFunctions[main->id - 1];
    bool forgedDrop = false;
    if (dropMain != nullptr) {
      auto &drops = const_cast<std::vector<lang::MirDropObligation> &>(
          dropMain->body.dropObligations);
      for (lang::MirDropObligation &drop : drops) {
        if (drop.kind == lang::MirDropObligationKind::Value &&
            drop.value != 0) {
          const auto stage = std::find_if(
              dropMain->body.blocks.begin(), dropMain->body.blocks.end(),
              [&](const lang::MirBlock &block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const lang::MirInstruction &instruction) {
                      return instruction.kind ==
                                 lang::MirInstructionKind::CallInput &&
                             instruction.hirValue == drop.value &&
                             (instruction.callInputKind ==
                                  lang::HirCallInputKind::ReadTemporaryBorrow ||
                              instruction.callInputKind ==
                                  lang::HirCallInputKind::
                                      MutableTemporaryBorrow);
                    });
              });
          if (stage != dropMain->body.blocks.end()) {
            drop.hirFullExpression = 0;
            forgedDrop = true;
            break;
          }
        }
      }
    }
    expect(forgedDrop && !lang::verifyMirProgram(missingDrop).valid(),
           "the MIR verifier should reject a temporary receiver without its "
           "exact full-expression drop obligation");
  }

  const std::string invalidSource = R"(
class InvalidReceiver {
public:
  InvalidReceiver() : value(0) {}
  void reset() mut {}
  mut int& expose() mut { return this.value; }

private:
  mut int value;
};

int main() {
  InvalidReceiver fixed = InvalidReceiver();
  fixed.reset();
  mut InvalidReceiver moved = InvalidReceiver();
  std::move(moved).reset();
  bool choose = true;
  (choose ? InvalidReceiver() : InvalidReceiver()).reset();
  [[discard]] InvalidReceiver().expose();
  return 0;
}
)";
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-mutable-temporary-receivers.gti", invalidSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalid.semanticValid &&
          hasDiagnostic(invalid.diagnostics,
                        "Mutable method requires a mutable receiver") &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2081") == 1 &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot produce a reference, pointer-like value") &&
          hasRelatedDiagnostic(invalid.diagnostics, "declared mutable here") &&
          hasDiagnosticHint(invalid.diagnostics, "Bind the value"),
      "unsupported temporary receivers should retain precise compiler "
      "diagnostics while immutable named objects remain rejected");
}

void testVirtualTemporaryReceivers() {
  // Dispatch selects which body runs over the borrowed temporary; it does not
  // change the receiver role. Virtual, override, interface-override, inherited
  // and operator() members all borrow the materialized temporary exactly as a
  // non-virtual member does, and keep their own dispatch.
  const std::string source = R"(
class Tracked {
  int id;
public:
  Tracked(int value) : id(value) {}
  ~Tracked() {}
  int read() { return this.id; }
};

class Maker {
public:
  virtual Tracked make() { return Tracked(1); }
  Tracked plain() { return Tracked(2); }
  virtual Tracked operator()(int value) { return Tracked(value); }
public:
  Maker() = default;
};

class Doubling : public Maker {
public:
  Tracked make() override { return Tracked(2); }
public:
  Doubling() = default;
};

class Inheriting : public Maker {
public:
  Inheriting() {}
};

interface Factory {
  Tracked build() mut;
};

class Counting : public Factory {
  mut int next;
public:
  Counting(int start) : next(start) {}
  Tracked build() mut override {
    this.next += 1;
    return Tracked(this.next);
  }
};

int main() {
  Tracked base = Maker().make();
  Tracked derived = Doubling().make();
  Tracked inherited = Inheriting().make();
  Tracked plain = Inheriting().plain();
  Tracked built = Counting(4).build();
  Tracked invoked = Maker()(6);
  return base.read() + derived.read() + inherited.read() + plain.read() +
         built.read() + invoked.read() - 18;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("virtual-temporary-receivers.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected virtual-temporary diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "virtual, override, interface and inherited members should borrow "
         "fresh temporaries through verified receiver stages");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirFunctionInstance *main = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr && instance.source->name().lexeme == "main")
      main = &instance;
  }
  const lang::MirFunctionInstance *mirMain =
      main == nullptr ? nullptr : frontend.mir.findFunctionInstance(main->id);
  expect(main != nullptr && mirMain != nullptr,
         "the virtual-temporary fixture should retain its main body");
  if (main == nullptr || mirMain == nullptr) {
    return;
  }

  std::size_t virtualHirCalls = 0;
  std::size_t staticHirCalls = 0;
  std::size_t mutableHirCalls = 0;
  for (const lang::HirValue &value : main->body.values) {
    if (value.kind != lang::HirValueKind::Call || !value.callPlan ||
        !value.callPlan->receiver ||
        (value.callPlan->receiver->kind !=
             lang::HirCallInputKind::ReadTemporaryBorrow &&
         value.callPlan->receiver->kind !=
             lang::HirCallInputKind::MutableTemporaryBorrow)) {
      continue;
    }
    const auto *sourceCall = dynamic_cast<const lang::Call *>(value.source);
    const lang::ResolvedCallInfo *resolved =
        sourceCall == nullptr ? nullptr
                              : frontend.semantics.findCall(*sourceCall);
    const lang::ResolvedOperatorInfo *resolvedOperator =
        sourceCall == nullptr ? nullptr
                              : frontend.semantics.findOperator(*sourceCall);
    const lang::CallReceiverMode mode =
        resolvedOperator != nullptr ? resolvedOperator->receiverMode
        : resolved != nullptr       ? resolved->receiverMode
                                    : lang::CallReceiverMode::None;
    const bool mutableInput = value.callPlan->receiver->kind ==
                              lang::HirCallInputKind::MutableTemporaryBorrow;
    expect(mode == (mutableInput ? lang::CallReceiverMode::FreshTemporaryMutable
                                 : lang::CallReceiverMode::FreshTemporaryRead),
           "HIR temporary receiver access should copy the selected semantic "
           "mode for virtual members too");
    virtualHirCalls += value.dispatch == lang::CallDispatch::Virtual ? 1 : 0;
    staticHirCalls += value.dispatch == lang::CallDispatch::Static ? 1 : 0;
    mutableHirCalls += mutableInput ? 1 : 0;
  }
  expect(virtualHirCalls == 5 && staticHirCalls == 1 && mutableHirCalls == 1,
         "HIR should keep each member's own dispatch over its temporary "
         "receiver borrow");

  const auto stageFor =
      [&](const lang::MirBody &body,
          const lang::MirInstruction &call) -> const lang::MirInstruction * {
    if (!call.receiver || call.receiver->kind != lang::MirOperandKind::Value)
      return nullptr;
    const lang::MirValue *value = body.findValue(call.receiver->value);
    const lang::MirBlock *block =
        value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
    if (block == nullptr)
      return nullptr;
    for (const lang::MirInstruction &instruction : block->instructions) {
      if (instruction.id == value->definition &&
          (instruction.callInputKind ==
               lang::HirCallInputKind::ReadTemporaryBorrow ||
           instruction.callInputKind ==
               lang::HirCallInputKind::MutableTemporaryBorrow))
        return &instruction;
    }
    return nullptr;
  };
  std::size_t virtualStages = 0;
  std::size_t staticStages = 0;
  for (const lang::MirBlock &block : mirMain->body.blocks) {
    for (const lang::MirInstruction &call : block.instructions) {
      if (call.kind != lang::MirInstructionKind::Call ||
          stageFor(mirMain->body, call) == nullptr) {
        continue;
      }
      const lang::MirFunctionInstance *target =
          call.functionTarget
              ? frontend.mir.findFunctionInstance(*call.functionTarget)
              : nullptr;
      const bool virtualCall = call.dispatch == lang::CallDispatch::Virtual;
      expect(target != nullptr && target->virtualMethod == virtualCall &&
                 (call.definedFailure.propagation ==
                  lang::FailurePropagationKind::VirtualCall) == virtualCall,
             "MIR should keep the target's dispatch and failure contract on "
             "a temporary receiver call");
      virtualStages += virtualCall ? 1 : 0;
      staticStages += virtualCall ? 0 : 1;
    }
  }
  expect(virtualStages == 5 && staticStages == 1,
         "MIR should stage every virtual and inherited temporary receiver");
  expect(nativeBackendEmits(frontend),
         "the native backend should consume verified virtual temporary "
         "receiver stages");

  const auto rejectsTemporaryReceiver = [](const lang::MirProgram &program) {
    const lang::MirVerificationResult result = lang::verifyMirProgram(program);
    return std::any_of(
        result.errors.begin(), result.errors.end(),
        [](const lang::MirVerificationError &error) {
          return error.message.find(
                     "temporary receiver input does not match") !=
                 std::string::npos;
        });
  };
  const auto hirClass = [&](std::optional<lang::HirClassInstanceId> id)
      -> const lang::HirClassInstance * {
    for (const lang::HirClassInstance &instance :
         frontend.hir.classInstances()) {
      if (id && instance.id == *id)
        return &instance;
    }
    return nullptr;
  };
  // Each mutation rewrites one temporary receiver call in main, selected by
  // its target's member name and owner class.
  const auto forgeCall = [&](std::string_view member, std::string_view owner,
                             const auto &rewrite) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    lang::MirFunctionInstance *forgedMain =
        main->id == 0 || main->id > functions.size() ? nullptr
                                                     : &functions[main->id - 1];
    bool rewritten = false;
    if (forgedMain == nullptr)
      return std::pair{false, forged};
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(forgedMain->body.blocks)) {
      for (lang::MirInstruction &call :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        const lang::HirFunctionInstance *target =
            call.functionTarget
                ? frontend.hir.findFunctionInstance(*call.functionTarget)
                : nullptr;
        const lang::HirClassInstance *targetOwner =
            target == nullptr ? nullptr : hirClass(target->owner);
        if (rewritten || call.kind != lang::MirInstructionKind::Call ||
            stageFor(forgedMain->body, call) == nullptr || target == nullptr ||
            target->source == nullptr || targetOwner == nullptr ||
            targetOwner->source == nullptr ||
            target->source->name().lexeme != member ||
            targetOwner->source->name().lexeme != owner) {
          continue;
        }
        rewritten = rewrite(call);
      }
    }
    return std::pair{rewritten, forged};
  };
  const auto staticVirtual =
      forgeCall("make", "Maker", [](lang::MirInstruction &call) {
        call.dispatch = lang::CallDispatch::Static;
        return true;
      });
  expect(staticVirtual.first && rejectsTemporaryReceiver(staticVirtual.second),
         "the MIR verifier should reject a virtual member statically "
         "dispatched over a temporary receiver");
  const auto virtualStatic =
      forgeCall("plain", "Maker", [](lang::MirInstruction &call) {
        call.dispatch = lang::CallDispatch::Virtual;
        return true;
      });
  expect(virtualStatic.first && rejectsTemporaryReceiver(virtualStatic.second),
         "the MIR verifier should reject table dispatch of a non-virtual "
         "member over a temporary receiver");
  // A sibling override has the same signature but is not a member of the
  // temporary's own class or any of its bases.
  std::optional<lang::HirFunctionInstanceId> siblingOverride;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    const lang::HirClassInstance *owner = hirClass(instance.owner);
    if (instance.source != nullptr && owner != nullptr &&
        owner->source != nullptr && instance.source->name().lexeme == "make" &&
        owner->source->name().lexeme == "Doubling")
      siblingOverride = instance.id;
  }
  const auto unrelatedOwner =
      forgeCall("plain", "Maker", [&](lang::MirInstruction &call) {
        // Dispatch matches the forged virtual target, so only the owner
        // relation differs from a verified shape.
        if (!siblingOverride)
          return false;
        call.functionTarget = *siblingOverride;
        call.dispatch = lang::CallDispatch::Virtual;
        return true;
      });
  expect(unrelatedOwner.first &&
             rejectsTemporaryReceiver(unrelatedOwner.second),
         "the MIR verifier should reject a temporary receiver whose member "
         "belongs to a class outside the temporary's own hierarchy");
}

void testGeneratedDefaultTemporaryArgumentBorrows() {
  const auto frontend = lang::Frontend().analyze(
      "generated-default-temporary-arguments.gti", R"gti(
struct Implicit { public: Implicit() = default; };
struct Defaulted { Defaulted() = default; };
struct Cleanup { ~Cleanup() {}  public: Cleanup() = default; };
bool inspect(Implicit& value) { return true; }
bool inspect(Defaulted& value) { return true; }
bool inspect(Cleanup& value) { return true; }
void observe(bool value) {}
Implicit make() { return Implicit(); }
int main() {
  Implicit named = make();
  observe(inspect(Implicit()));
  observe(inspect(Defaulted()));
  observe(inspect(Cleanup()));
  return 0;
}
)gti");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "implicit/defaulted passive and cleanup-owning temporary arguments "
         "use the ordinary generated-default producer");
  if (!frontend.canGenerateCode())
    return;

  lang::HirFunctionInstanceId mainId = 0;
  for (const auto &instance : frontend.hir.functionInstances())
    if (instance.source && instance.source->name().lexeme == "main")
      mainId = instance.id;
  const auto *main = frontend.mir.findFunctionInstance(mainId);
  expect(main != nullptr, "generated temporary fixture retains its main body");
  if (!main)
    return;

  std::size_t stages = 0;
  for (const auto &block : main->body.blocks)
    for (const auto &input : block.instructions) {
      if (input.kind != lang::MirInstructionKind::CallInput ||
          input.callInputRole != lang::MirCallInputRole::Argument ||
          input.callInputKind != lang::HirCallInputKind::ReadTemporaryBorrow)
        continue;
      const auto *place = main->body.findPlace(input.operands.front().place);
      const auto materialization =
          place ? lang::findMirTemporaryMaterialization(main->body, *place,
                                                        input.hirValue)
                : lang::MirTemporaryMaterialization{};
      expect(materialization && materialization.producer &&
                 materialization.producer->kind ==
                     lang::MirInstructionKind::Construct &&
                 !materialization.producer->functionTarget &&
                 !materialization.producer->constructorTarget &&
                 materialization.producer->operands.empty(),
             "each default temporary retains its explicit generated Construct");
      ++stages;
    }
  expect(stages == 3, "all three generated temporary argument loans survive");
  auto invalidHir = frontend.hir;
  auto *hirMain = const_cast<lang::HirFunctionInstance *>(
      invalidHir.findFunctionInstance(mainId));
  bool changedHir = false;
  if (hirMain)
    for (auto &value : hirMain->body.values)
      if (value.kind == lang::HirValueKind::DirectInitializer &&
          !value.constructorTarget) {
        value.kind = lang::HirValueKind::Call;
        changedHir = true;
        break;
      }
  expect(
      changedHir &&
          !lang::verifyHirProgramPlans(frontend.semantics, invalidHir).valid(),
      "HIR retains the semantic generated construction selection");

  // A standalone result must carry the same proof, even without a later
  // temporary receiver or argument loan to expose a forged producer.
  auto invalidStandalone = frontend.mir;
  bool changedStandalone = false;
  for (const auto &candidate : invalidStandalone.functionInstances()) {
    auto *function = const_cast<lang::MirFunctionInstance *>(&candidate);
    if (function->id == mainId)
      continue;
    for (auto &block : function->body.blocks)
      for (auto &producer : block.instructions)
        if (producer.kind == lang::MirInstructionKind::Construct &&
            !producer.constructorTarget) {
          producer.kind = lang::MirInstructionKind::Call;
          changedStandalone = true;
        }
  }
  expect(changedStandalone &&
             !lang::verifyMirProgram(invalidStandalone).valid(),
         "standalone generated results cannot become unexplained calls");

  auto invalidFactory = frontend.mir;
  auto *factoryMain = const_cast<lang::MirFunctionInstance *>(
      invalidFactory.findFunctionInstance(mainId));
  bool changedFactory = false;
  if (factoryMain)
    for (auto &block : factoryMain->body.blocks)
      for (auto &call : block.instructions)
        if (call.kind == lang::MirInstructionKind::Call &&
            call.functionTarget &&
            call.info.type.kind == lang::SemanticType::Class) {
          call.functionTarget.reset();
          changedFactory = true;
          break;
        }
  expect(changedFactory && !lang::verifyMirProgram(invalidFactory).valid(),
         "a factory call cannot become construction by losing its target");
  expect(nativeBackendEmits(frontend),
         "generated temporary arguments use the verified native lifetime path");

  const auto rejects = [&](auto mutation) {
    auto program = frontend.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        program.findFunctionInstance(mainId));
    if (!function)
      return false;
    for (auto &block : function->body.blocks)
      for (auto &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputRole != lang::MirCallInputRole::Argument ||
            input.callInputKind != lang::HirCallInputKind::ReadTemporaryBorrow)
          continue;
        const auto *place =
            function->body.findPlace(input.operands.front().place);
        const auto materialization =
            place ? lang::findMirTemporaryMaterialization(
                        function->body, *place, input.hirValue)
                  : lang::MirTemporaryMaterialization{};
        if (!materialization)
          return false;
        auto *producer =
            const_cast<lang::MirInstruction *>(materialization.producer);
        mutation(program, function->body, input, *producer);
        return !lang::verifyMirProgram(program).valid();
      }
    return false;
  };
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.kind = lang::MirInstructionKind::Call;
         }),
         "generated construction cannot revert to a targetless ordinary call");
  expect(rejects([&](auto &, auto &, auto &, auto &producer) {
           producer.bodyTarget =
               lang::MirBodyAddress{lang::MirBodyKind::Function, mainId};
         }),
         "bodyless construction cannot acquire an unrelated body target");
  expect(rejects([](auto &, auto &, auto &input, auto &producer) {
           producer.loan = input.loan;
         }),
         "bodyless construction cannot acquire a result loan");
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.definedFailure.propagation =
               lang::FailurePropagationKind::Constructor;
         }),
         "bodyless generated construction cannot invent a failure edge");
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.info.type = lang::SemanticType::UInt64;
         }),
         "a target-free producer cannot substitute another result type");
  expect(rejects([](auto &, auto &, auto &, auto &producer) {
           producer.parameterTypes.push_back(lang::SemanticType::UInt64);
           producer.operands.push_back(
               {.kind = lang::MirOperandKind::Constant,
                .literal = lang::Literal{std::uint64_t(1)},
                .type = lang::SemanticType::UInt64});
         }),
         "generated default construction cannot acquire runtime inputs");
  expect(rejects([](auto &program, auto &, auto &, auto &producer) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == producer.info.type)
               owner.defaultConstructor = lang::SpecialMemberStatus::Declared;
         }),
         "a target-free call cannot impersonate a nongenerated constructor");
  expect(rejects([](auto &program, auto &, auto &, auto &producer) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == producer.info.type)
               owner.generatedDefaultRequiresSchedule = true;
         }),
         "a target-free call cannot omit a required initializer schedule");
  expect(rejects([](auto &, auto &body, auto &input, auto &) {
           auto *loan = const_cast<lang::MirLoan *>(body.findLoan(*input.loan));
           loan->access = lang::AccessMode::Mutable;
         }),
         "generated default arguments do not relax read-only call-loan access");
  expect(rejects([](auto &, auto &body, auto &input, auto &) {
           for (auto &drop : body.dropObligations)
             if (drop.kind == lang::MirDropObligationKind::Value &&
                 drop.value == input.hirValue)
               drop.hirFullExpression = 0;
         }),
         "generated default arguments still require exact full-expression "
         "materialization and drop authority");

  const auto unionFrontend =
      lang::Frontend().analyze("generated-default-union-boundary.gti", R"gti(
union Overlay { mut uint64_t first; mut uint32_t second; };
Overlay zero() { return Overlay(); }
int main() {
  mut Overlay overlay{uint64_t(7)};
  unsafe { overlay.second = uint32_t(2); }
  return 0;
}
)gti");
  expect(unionFrontend.canGenerateCode() && unionFrontend.mirValid &&
             lang::verifyMirProgram(unionFrontend.mir).valid(),
         "exact first-field union construction remains distinct from an "
         "empty generated default");
  if (!unionFrontend.canGenerateCode())
    return;
  const auto rejectsUnion = [&](auto mutation) {
    auto program = unionFrontend.mir;
    for (const auto &original : unionFrontend.mir.functionInstances()) {
      auto *function = const_cast<lang::MirFunctionInstance *>(
          program.findFunctionInstance(original.id));
      for (auto &block : function->body.blocks)
        for (auto &construct : block.instructions) {
          if (construct.kind != lang::MirInstructionKind::Construct ||
              construct.constructorTarget || construct.operands.size() != 1)
            continue;
          mutation(program, construct);
          return !lang::verifyMirProgram(program).valid();
        }
    }
    return false;
  };
  expect(rejectsUnion([](auto &, auto &construct) {
           construct.operands.push_back(construct.operands.front());
           construct.parameterTypes.push_back(construct.parameterTypes.front());
         }),
         "a union cannot invent another initialization input");
  expect(rejectsUnion([](auto &, auto &construct) {
           construct.parameterTypes.front() = lang::SemanticType::UInt32;
           construct.operands.front() = {.kind = lang::MirOperandKind::Constant,
                                         .literal =
                                             lang::Literal{std::uint64_t(1)},
                                         .type = lang::SemanticType::UInt32};
         }),
         "a union initializer must use its first field's exact type");
  expect(rejectsUnion([](auto &program, auto &construct) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == construct.info.type)
               owner.kind = lang::ClassKind::Struct;
         }),
         "ordinary records cannot impersonate union-field construction");
  expect(rejectsUnion([](auto &program, auto &construct) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           std::erase_if(owners, [&](const auto &owner) {
             return owner.type == construct.info.type;
           });
         }),
         "union initialization requires its concrete owner");
  expect(rejectsUnion([](auto &program, auto &construct) {
           auto &owners = const_cast<std::vector<lang::MirClassInstance> &>(
               program.classInstances());
           for (auto &owner : owners)
             if (owner.type == construct.info.type)
               owner.unionLayout->fields.front().field =
                   owner.declaredFields.back().symbol;
         }),
         "union construction retains the exact declared field/layout identity");
  expect(rejectsUnion([](auto &, auto &construct) {
           construct.definedFailure.propagation =
               lang::FailurePropagationKind::Constructor;
         }),
         "passive union construction cannot invent a constructor failure");
  expect(
      rejectsUnion([](auto &, auto &construct) { construct.enumVariant = 0; }),
      "native union construction cannot acquire a payload-enum variant");
}

void testGeneratedDefaultConstructionContexts() {
  const auto frontend =
      lang::Frontend().analyze("generated-default-contexts.gti", R"gti(
namespace construction {
struct Token<T> { int number() { return 1; }  public: Token() = default; };
}
struct construction::Token<int> { int number() { return 2; }  public: Token() = default; };
struct construction::Token<char> { int number() { return 3; }  public: Token() = default; };
construction::Token<T> make_token<T>() { return construction::Token<T>(); }
struct Factory<T> {
  construction::Token<T> make() { return construction::Token<T>(); }
public:
  Factory() = default;
};
struct ConstructorContext<T> {
  construction::Token<T> token;
  ConstructorContext() : token(construction::Token<T>()) {}
  int number() { return this.token.number(); }
};
int nested_token<T>() {
  auto outer = []() -> int {
    auto inner = []() -> int {
      construction::Token<T> token = construction::Token<T>();
      return token.number();
    };
    return inner();
  };
  return outer();
}
int main() {
  construction::Token<int> integer = make_token<int>();
  construction::Token<char> character = make_token<char>();
  if (integer.number() != 2) { return 1; }
  if (character.number() != 3) { return 2; }
  Factory<int> factory = Factory<int>();
  construction::Token<int> member = factory.make();
  if (member.number() != 2) { return 3; }
  ConstructorContext<char> constructed{};
  if (constructed.number() != 3) { return 4; }
  if (nested_token<int>() != 2) { return 5; }
  if (nested_token<char>() != 3) { return 6; }
  return 0;
}
)gti");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "generic function, class, constructor and nested lambda construction "
         "select exact specialized owners");
  if (!frontend.canGenerateCode())
    return;
  const auto rejects = [&](auto mutation, std::string_view detail) {
    auto hir = frontend.hir;
    if (!mutation(hir))
      return false;
    const auto checked = lang::verifyHirProgramPlans(frontend.semantics, hir);
    return !checked.valid() &&
           std::any_of(checked.errors.begin(), checked.errors.end(),
                       [&](const auto &error) {
                         return error.find(detail) != std::string::npos;
                       });
  };
  expect(
      rejects(
          [](auto &hir) {
            for (const auto &candidate : hir.functionInstances()) {
              auto &function =
                  const_cast<lang::HirFunctionInstance &>(candidate);
              if (function.source &&
                  function.source->name().lexeme == "make_token" &&
                  function.typeArguments == std::vector<lang::SemanticType>{
                                                lang::SemanticType::Int32}) {
                function.typeArguments = {lang::SemanticType::Char};
                return true;
              }
            }
            return false;
          },
          "generated default value lost its exact construction selection"),
      "a function's mismatched generic arguments cannot retarget construction");
  expect(rejects(
             [](auto &hir) {
               std::optional<lang::HirValue> first;
               lang::HirValue *second = nullptr;
               for (const auto &candidate : hir.functionInstances()) {
                 auto &function =
                     const_cast<lang::HirFunctionInstance &>(candidate);
                 if (!function.source ||
                     function.source->name().lexeme != "nested_token")
                   continue;
                 for (auto &value : function.body.values)
                   if (value.kind == lang::HirValueKind::Lambda &&
                       value.lambdaTarget) {
                     if (function.typeArguments ==
                         std::vector<lang::SemanticType>{
                             lang::SemanticType::Int32})
                       first = value;
                     else if (function.typeArguments ==
                              std::vector<lang::SemanticType>{
                                  lang::SemanticType::Char})
                       second = &value;
                   }
               }
               if (!first || !second)
                 return false;
               second->lambdaTarget = first->lambdaTarget;
               second->info.type = first->info.type;
               return true;
             },
             "construction body has ambiguous instance arguments"),
         "sibling lambda creators cannot share incompatible construction "
         "contexts");
  expect(rejects(
             [](auto &hir) {
               for (const auto &function : hir.functionInstances())
                 for (const auto &value : function.body.values)
                   if (value.kind == lang::HirValueKind::Lambda &&
                       value.lambdaTarget) {
                     auto *lambda = const_cast<lang::HirLambda *>(
                         hir.findLambda(*value.lambdaTarget));
                     if (!lambda)
                       return false;
                     auto cycle = value;
                     cycle.id = 1000000;
                     lambda->body.values.push_back(std::move(cycle));
                     return true;
                   }
               return false;
             },
             "construction context contains a lambda creator cycle"),
         "a cyclic creator graph cannot infer construction authority");
}

void testTemporaryArgumentBorrows() {
  const std::string source = R"(
class Report {
  int value;

public:
  Report(int value) : value(value) {}
  int code() { return this.value; }
};

class Factory {
public:
  Factory() {}
  Report build(int value) { return Report(value); }
};

Report make_report(int value) { return Report(value); }
int inspect(Report& report) { return report.code(); }
int triple(int left, Report& report, int right) {
  return left + report.code() + right;
}
int combine(Report& left, Report& right) {
  return left.code() + right.code();
}
void observe(Report& report) {}
Report duplicate(Report& report) { return Report(report.code()); }
int generic<T>(T& value) { return 9; }
int choose(Report& report) { return 100 + report.code(); }
int choose(Report report) { return 200 + report.code(); }
int consume(int value) { return value; }
int side(int value) { return value; }

int main() {
  int direct = inspect(Report(1));
  int produced = inspect((make_report(2)));
  int nested = consume(inspect((Report(3))));
  int ordered = triple(side(4), Report(5), side(6));
  int multiple = combine(Report(7), make_report(8));
  observe(Report(11));
  int owned = duplicate(Report(12)).code();
  int concrete = generic(Report(9));
  int owning = choose(Report(10));
  Factory factory = Factory();
  int member_produced = inspect(factory.build(13));
  return direct + produced + nested + ordered + multiple + concrete + owning +
         member_produced;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("temporary-argument-borrows.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected temporary-argument diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "direct construction, GTI-call, grouping, nested, multiple, and "
         "concrete-generic temporary arguments should lower to verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto hirFunction =
      [&](std::string_view name) -> const lang::HirFunctionInstance * {
    const auto found =
        std::find_if(frontend.hir.functionInstances().begin(),
                     frontend.hir.functionInstances().end(),
                     [&](const lang::HirFunctionInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->name().lexeme == name;
                     });
    return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
  };
  const lang::HirFunctionInstance *main = hirFunction("main");
  expect(main != nullptr,
         "the temporary-argument fixture should retain its main instance");
  if (main == nullptr) {
    return;
  }

  std::size_t temporaryCalls = 0;
  std::size_t temporaryArguments = 0;
  bool owningOverloadSelected = false;
  for (const lang::HirValue &value : main->body.values) {
    if (value.kind != lang::HirValueKind::Call || !value.callPlan) {
      continue;
    }
    std::vector<std::size_t> hirIndices;
    for (const lang::HirCallArgument &argument : value.callPlan->arguments) {
      if (argument.kind != lang::HirCallInputKind::ReadTemporaryBorrow) {
        continue;
      }
      ++temporaryArguments;
      hirIndices.push_back(argument.parameterIndex);
      const lang::HirValue *producer = main->body.findValue(argument.value);
      expect(argument.parameterType.kind == lang::SemanticType::Reference &&
                 argument.parameterType.referenceAccess ==
                     lang::AccessMode::ReadOnly &&
                 argument.parameterType.arguments.size() == 1 &&
                 producer != nullptr &&
                 producer->info.type ==
                     argument.parameterType.arguments.front() &&
                 producer->info.category == lang::ValueCategory::Value &&
                 !producer->info.traits.containsBorrowedState,
             "HIR should retain the exact read-only reference parameter and "
             "fresh self-contained producer for each ephemeral argument");
    }
    const auto *call = dynamic_cast<const lang::Call *>(value.source);
    const lang::ResolvedCallInfo *resolved =
        call == nullptr ? nullptr : frontend.semantics.findCall(*call);
    if (!hirIndices.empty()) {
      ++temporaryCalls;
      expect(resolved != nullptr &&
                 resolved->ephemeralBorrowArguments == hirIndices,
             "HIR temporary argument indices should copy the semantic "
             "overload decision exactly");
    }
    if (resolved != nullptr && resolved->declaration != nullptr &&
        resolved->declaration->name().lexeme == "choose") {
      owningOverloadSelected =
          resolved->ephemeralBorrowArguments.empty() &&
          resolved->parameterTypes.size() == 1 &&
          resolved->parameterTypes.front().kind == lang::SemanticType::Class;
    }
  }
  expect(temporaryCalls == 9 && temporaryArguments == 10 &&
             owningOverloadSelected,
         "the fallback tier should preserve every written temporary argument "
         "while an existing exact by-value overload wins first");

  const lang::MirFunctionInstance *mirMain =
      frontend.mir.findFunctionInstance(main->id);
  expect(mirMain != nullptr,
         "the temporary-argument fixture should retain its MIR main body");
  if (mirMain == nullptr) {
    return;
  }
  std::vector<lang::MirLoanId> argumentLoans;
  for (const lang::MirBlock &block : mirMain->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput ||
          instruction.callInputKind !=
              lang::HirCallInputKind::ReadTemporaryBorrow ||
          instruction.callInputRole != lang::MirCallInputRole::Argument) {
        continue;
      }
      const lang::MirPlace *place =
          instruction.operands.empty()
              ? nullptr
              : mirMain->body.findPlace(instruction.operands.front().place);
      const lang::MirLoan *loan =
          instruction.loan ? mirMain->body.findLoan(*instruction.loan)
                           : nullptr;
      const std::size_t drops =
          place == nullptr
              ? 0
              : static_cast<std::size_t>(std::count_if(
                    mirMain->body.dropObligations.begin(),
                    mirMain->body.dropObligations.end(),
                    [&](const lang::MirDropObligation &drop) {
                      return drop.kind == lang::MirDropObligationKind::Value &&
                             drop.value == instruction.hirValue &&
                             drop.place == place->id &&
                             drop.hirFullExpression != 0 &&
                             drop.fullExpression != 0;
                    }));
      expect(instruction.operands.size() == 1 &&
                 instruction.operands.front().kind ==
                     lang::MirOperandKind::BorrowRead &&
                 place != nullptr &&
                 place->root == lang::MirPlaceRootKind::Value &&
                 place->access == lang::AccessMode::Mutable &&
                 loan != nullptr &&
                 loan->kind == lang::MirLoanKind::CallArgument &&
                 loan->source == place->id &&
                 loan->access == lang::AccessMode::ReadOnly &&
                 loan->producedBy == instruction.callSite && !loan->escapes &&
                 drops == 1,
             "MIR should materialize one owned place, call-scoped read loan, "
             "and full-expression drop for each temporary argument");
      if (loan != nullptr) {
        argumentLoans.push_back(loan->id);
      }
    }
  }
  expect(argumentLoans.size() == 10,
         "MIR should retain all ten ephemeral argument loans");
  for (const lang::MirLoanId loan : argumentLoans) {
    std::size_t endings = 0;
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      endings += static_cast<std::size_t>(std::count_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == loan;
          }));
    }
    expect(endings >= 1,
           "each call-scoped temporary argument loan should end on every "
           "reachable post-invocation route");
  }
  expect(nativeBackendEmits(frontend),
         "the native backend should consume verified temporary argument slots "
         "without native lifetime inference");

  const auto mutableMirMain =
      [&](lang::MirProgram &program) -> lang::MirFunctionInstance * {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    return main->id == 0 || main->id > functions.size()
               ? nullptr
               : &functions[main->id - 1];
  };
  const auto firstTemporaryStage =
      [](lang::MirFunctionInstance &function) -> lang::MirInstruction * {
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(function.body.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputKind ==
                lang::HirCallInputKind::ReadTemporaryBorrow &&
            instruction.callInputRole == lang::MirCallInputRole::Argument) {
          return &instruction;
        }
      }
    }
    return nullptr;
  };

  lang::MirProgram wrongIndex = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongIndex)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function)) {
      ++stage->callInputIndex;
    }
  }
  expect(!lang::verifyMirProgram(wrongIndex).valid(),
         "the verifier should reject a forged temporary argument index");

  lang::MirProgram wrongPlace = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongPlace)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function);
        stage != nullptr && !stage->operands.empty()) {
      auto &places =
          const_cast<std::vector<lang::MirPlace> &>(function->body.places);
      places[stage->operands.front().place - 1].access =
          lang::AccessMode::ReadOnly;
    }
  }
  expect(!lang::verifyMirProgram(wrongPlace).valid(),
         "the verifier should reject a temporary argument loan over a "
         "non-materializable place");

  lang::MirProgram wrongLoan = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongLoan)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function);
        stage != nullptr && stage->loan) {
      auto &loans =
          const_cast<std::vector<lang::MirLoan> &>(function->body.loans);
      loans[*stage->loan - 1].access = lang::AccessMode::Mutable;
    }
  }
  expect(!lang::verifyMirProgram(wrongLoan).valid(),
         "the verifier should reject a mutable or mismatched call-argument "
         "loan");

  lang::MirProgram wrongProducer = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(wrongProducer)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function)) {
      stage->hirValue = 0;
    }
  }
  expect(!lang::verifyMirProgram(wrongProducer).valid(),
         "the verifier should reject a temporary argument detached from its "
         "exact producer identity");

  lang::MirProgram missingFullExpression = frontend.mir;
  if (lang::MirFunctionInstance *function =
          mutableMirMain(missingFullExpression)) {
    if (lang::MirInstruction *stage = firstTemporaryStage(*function)) {
      auto &drops = const_cast<std::vector<lang::MirDropObligation> &>(
          function->body.dropObligations);
      const auto found =
          std::find_if(drops.begin(), drops.end(), [&](const auto &drop) {
            return drop.kind == lang::MirDropObligationKind::Value &&
                   drop.value == stage->hirValue;
          });
      if (found != drops.end()) {
        found->hirFullExpression = 0;
      }
    }
  }
  expect(!lang::verifyMirProgram(missingFullExpression).valid(),
         "the verifier should reject a temporary argument without its exact "
         "full-expression drop identity");

  lang::MirProgram missingEnd = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableMirMain(missingEnd)) {
    lang::MirInstruction *stage = firstTemporaryStage(*function);
    const lang::MirLoanId loan =
        stage != nullptr && stage->loan ? *stage->loan : 0;
    if (loan != 0) {
      for (lang::MirBlock &block :
           const_cast<std::vector<lang::MirBlock> &>(function->body.blocks)) {
        auto &instructions =
            const_cast<std::vector<lang::MirInstruction> &>(block.instructions);
        const auto ending = std::find_if(
            instructions.begin(), instructions.end(),
            [&](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == loan;
            });
        if (ending != instructions.end()) {
          instructions.erase(ending);
          break;
        }
      }
    }
  }
  expect(!lang::verifyMirProgram(missingEnd).valid(),
         "the verifier should reject a missing immediate temporary argument "
         "loan ending");

  const auto rejected =
      [](std::string_view name, std::string_view source,
         const std::vector<std::filesystem::path> &preludes = {}) {
        // The prelude includes <std/meta>, so it needs the library root.
        return lang::Frontend().analyze(
            std::string(name), std::string(source), preludes, {},
            preludes.empty() ? std::vector<std::filesystem::path>{}
                             : std::vector{standardLibraryRoot()});
      };
  const lang::FrontendResult mutableReference =
      rejected("mutable-temporary-argument.gti", R"(
class Report { public: Report() {} };
void update(mut Report& report) {}
int main() { update(Report()); return 0; }
)");
  expect(!mutableReference.canGenerateCode() &&
             hasDiagnosticCode(mutableReference.diagnostics, "GTI-S2017"),
         "a mutable reference should remain unable to bind a temporary");

  const lang::FrontendResult localReference =
      rejected("local-temporary-reference.gti", R"(
class Report { public: Report() {} };
int main() { Report& alias = Report(); return 0; }
)");
  expect(!localReference.canGenerateCode() &&
             hasDiagnosticCode(localReference.diagnostics, "GTI-S2017"),
         "a local reference binding should not gain lifetime extension");

  const std::vector<std::filesystem::path> prelude{standardLibraryPrelude()};
  // An explicitly moved whole local now binds a read-only parameter as a
  // full-expression temporary, as an explicitly moved receiver does.
  const lang::FrontendResult explicitMove =
      rejected("moved-temporary-argument.gti", R"(
class Report { public: Report() {} };
void inspect(Report& report) {}
int main() {
  mut Report report = Report();
  inspect(std::move(report));
  return 0;
}
)",
               prelude);
  expect(explicitMove.canGenerateCode() &&
             lang::verifyMirProgram(explicitMove.mir).valid(),
         "an explicitly moved local should bind a read-only parameter as a "
         "call-scoped temporary");
  const lang::FrontendResult explicitMutableMove =
      rejected("moved-mutable-temporary-argument.gti", R"(
class Report { public: Report() {} };
void update(mut Report& report) {}
int main() {
  mut Report report = Report();
  update(std::move(report));
  return 0;
}
)",
               prelude);
  expect(!explicitMutableMove.canGenerateCode() &&
             hasDiagnosticCode(explicitMutableMove.diagnostics, "GTI-S2017"),
         "an explicit move from named storage should not count as fresh");

  const lang::FrontendResult compound =
      rejected("compound-temporary-argument.gti", R"(
class Report { public: Report() {} };
void inspect(Report& report) {}
int main() {
  bool select = true;
  inspect(select ? Report() : Report());
  return 0;
}
)");
  expect(compound.canGenerateCode() &&
             lang::verifyMirProgram(compound.mir).valid(),
         "a conditional class producer should bind an exact call-scoped "
         "read-only argument");

  const lang::FrontendResult borrowedState =
      rejected("borrowed-state-temporary-argument.gti", R"(
class View {
  int& value;
public:
  View(int& value) : value(value) {}
};
void inspect(View& view) {}
int main() {
  mut int value = 1;
  inspect(View(value));
  return 0;
}
)");
  expect(borrowedState.canGenerateCode() &&
             lang::verifyMirProgram(borrowedState.mir).valid() &&
             checkedTemporaryObserver(borrowedState, "inspect", true),
         "a fresh carrier with a proved stable dependency now supplies a "
         "nonescaping readonly argument");

  const lang::FrontendResult escapingResult =
      rejected("escaping-temporary-argument.gti", R"(
class Report { public: Report() {} };
Report& retain(Report& report) { return report; }
int main() { [[discard]] retain(Report()); return 0; }
)");
  expect(!escapingResult.canGenerateCode() &&
             hasDiagnosticCode(escapingResult.diagnostics, "GTI-S2017"),
         "a call result tied to the temporary parameter should remain "
         "ineligible");

  const lang::FrontendResult ambiguousFallback =
      rejected("ambiguous-temporary-argument.gti", R"(
class Report { public: Report() {} };
int inspect(Report& report) { return 1; }
int inspect<T>(T& report) { return 2; }
int main() { return inspect(Report()); }
)");
  expect(!ambiguousFallback.canGenerateCode() &&
             hasDiagnosticCode(ambiguousFallback.diagnostics, "GTI-S2013"),
         "ordinary ambiguity should remain authoritative within the "
         "ephemeral-borrow fallback tier");
}

// A temporary argument evaluated only inside a conditional arm or a `&&`/`||`
// operand lives to its full-expression end, after the CFG merge. Lowering
// promotes it into the one hidden Temporary slot that carries its conditional
// drop, and the argument's BorrowRead and CallArgument loan name that slot.
// Verification admits exactly that materialization, no other Temporary place.
void testConditionalTemporaryArgumentBorrows() {
  const std::string source = R"(
class Report {
  int value;

public:
  Report(int value) : value(value) {}
  int code() { return this.value; }
};

class Tracked {
  int value;

public:
  Tracked(int value) : value(value) {}
  Tracked(Tracked& other) = delete;
  Tracked(Tracked&& other) = default;
  ~Tracked() {}
  int code() { return this.value; }
};

Report make_report(int value) { return Report(value); }
Tracked make_tracked(int value) { return Tracked(value); }
bool positive(Report& report) { return report.code() > 0; }
bool tracked(Tracked& value) { return value.code() > 0; }

int main() {
  bool direct = positive(Report(1));
  bool joined =
      positive(make_report(2)) && direct && !positive(make_report(-3));
  bool either = !direct || tracked(make_tracked(4));
  int chosen =
      direct ? (positive(Report(5)) ? 1 : 2) : (tracked(Tracked(6)) ? 3 : 4);
  return joined && either && chosen == 1 ? 0 : 1;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "conditional-temporary-argument-borrows.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected conditional temporary-argument diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "temporary arguments inside conditional arms and short-circuit "
         "operands should lower to verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto found =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &instance) {
                     return instance.source != nullptr &&
                            instance.source->name().lexeme == "main";
                   });
  const lang::MirFunctionInstance *mirMain =
      found == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(found->id);
  expect(mirMain != nullptr,
         "the conditional temporary-argument fixture should retain its MIR "
         "main body");
  if (mirMain == nullptr) {
    return;
  }
  const lang::MirBody &body = mirMain->body;

  std::size_t linear = 0;
  std::vector<const lang::MirInstruction *> hidden;
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &stage : block.instructions) {
      if (stage.kind != lang::MirInstructionKind::CallInput ||
          stage.callInputKind != lang::HirCallInputKind::ReadTemporaryBorrow ||
          stage.callInputRole != lang::MirCallInputRole::Argument ||
          stage.operands.size() != 1) {
        continue;
      }
      const lang::MirPlace *place =
          body.findPlace(stage.operands.front().place);
      if (place != nullptr && place->root == lang::MirPlaceRootKind::Value) {
        ++linear;
        continue;
      }
      hidden.push_back(&stage);
      const lang::MirTemporaryMaterialization materialization =
          place == nullptr ? lang::MirTemporaryMaterialization{}
                           : lang::findMirTemporaryMaterialization(
                                 body, *place, stage.hirValue);
      const lang::MirLoan *loan =
          stage.loan ? body.findLoan(*stage.loan) : nullptr;
      std::vector<const lang::MirDropObligation *> drops;
      for (const lang::MirDropObligation &drop : body.dropObligations) {
        if (drop.kind == lang::MirDropObligationKind::Value &&
            drop.value == stage.hirValue) {
          drops.push_back(&drop);
        }
      }
      bool conditionalDrop = false;
      bool loanEnds = false;
      for (const lang::MirBlock &candidate : body.blocks) {
        for (const lang::MirInstruction &instruction : candidate.instructions) {
          loanEnds = loanEnds ||
                     (instruction.kind == lang::MirInstructionKind::EndBorrow &&
                      loan != nullptr && instruction.loan == loan->id);
          conditionalDrop =
              conditionalDrop ||
              (instruction.kind == lang::MirInstructionKind::Drop &&
               place != nullptr && instruction.destination == place->id &&
               drops.size() == 1 &&
               std::any_of(
                   instruction.lifecycle.begin(), instruction.lifecycle.end(),
                   [&](const lang::MirLifecycleEvent &event) {
                     return event.kind == lang::MirLifecycleEventKind::Drop &&
                            event.source == drops.front()->id &&
                            event.conditional;
                   }));
        }
      }
      const lang::MirInstruction *producer = materialization.producer;
      expect(place != nullptr &&
                 place->root == lang::MirPlaceRootKind::Temporary &&
                 place->temporary != 0 && place->projections.empty() &&
                 place->access == lang::AccessMode::Mutable &&
                 place->sourceValue == stage.hirValue && producer != nullptr &&
                 (producer->kind == lang::MirInstructionKind::Call ||
                  producer->kind == lang::MirInstructionKind::Construct) &&
                 producer->hirValue == stage.hirValue && loan != nullptr &&
                 loan->kind == lang::MirLoanKind::CallArgument &&
                 loan->source == place->id &&
                 loan->access == lang::AccessMode::ReadOnly &&
                 loan->producedBy == stage.callSite && !loan->escapes &&
                 drops.size() == 1 && drops.front()->place == place->id &&
                 drops.front()->fullExpression != 0 && loanEnds &&
                 conditionalDrop,
             "a branch-only temporary argument should borrow its exact "
             "CFG-promoted slot, end its loan in the arm, and drop "
             "conditionally after the merge");
    }
  }
  const std::size_t cleanupSlots = static_cast<std::size_t>(std::count_if(
      hidden.begin(), hidden.end(), [&](const lang::MirInstruction *stage) {
        return std::any_of(
            body.dropObligations.begin(), body.dropObligations.end(),
            [&](const lang::MirDropObligation &drop) {
              return drop.kind == lang::MirDropObligationKind::Value &&
                     drop.value == stage->hirValue &&
                     drop.dropType.requiresActiveCleanup;
            });
      }));
  expect(linear == 2 && hidden.size() == 4 && cleanupSlots == 2,
         "the fixture should borrow two linear value-rooted temporaries and "
         "four branch-only hidden slots, two of them destructor-bearing");
  expect(nativeBackendEmits(frontend),
         "the native backend should emit borrows of CFG-promoted temporary "
         "argument slots");

  // Each forgery starts from a fresh copy of the verified program and edits
  // the hidden-slot stage at `index` in the order collected above.
  const auto forged = [&](const auto &edit) {
    lang::MirProgram program = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    lang::MirFunctionInstance &function = functions[mirMain->id - 1];
    edit(function.body);
    return lang::verifyMirProgram(program).valid();
  };
  const auto mutableStage = [&](lang::MirBody &forgedBody,
                                std::size_t index) -> lang::MirInstruction * {
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(forgedBody.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        if (instruction.id == hidden[index]->id) {
          return &instruction;
        }
      }
    }
    return nullptr;
  };
  if (hidden.size() != 4) {
    return;
  }
  // Two hidden Report slots: a borrow may not name its sibling's slot.
  std::vector<std::size_t> reports;
  for (std::size_t index = 0; index < hidden.size(); ++index) {
    const lang::MirPlace *place =
        body.findPlace(hidden[index]->operands.front().place);
    if (place != nullptr && place->type.kind == lang::SemanticType::Class &&
        !std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                     [&](const lang::MirDropObligation &drop) {
                       return drop.place == place->id &&
                              drop.dropType.requiresActiveCleanup;
                     })) {
      reports.push_back(index);
    }
  }
  expect(reports.size() == 2,
         "the fixture should keep two cleanup-free hidden argument slots");
  if (reports.size() == 2) {
    expect(!forged([&](lang::MirBody &forgedBody) {
      lang::MirInstruction *stage = mutableStage(forgedBody, reports.front());
      const lang::MirPlaceId sibling =
          hidden[reports.back()]->operands.front().place;
      if (stage != nullptr && stage->loan) {
        stage->operands.front().place = sibling;
        const_cast<std::vector<lang::MirLoan> &>(
            forgedBody.loans)[*stage->loan - 1]
            .source = sibling;
      }
    }),
           "the verifier should reject a temporary argument borrowing another "
           "temporary's hidden slot");
  }
  const auto forgedSlot = [&](const auto &edit) {
    return forged([&](lang::MirBody &forgedBody) {
      lang::MirInstruction *stage = mutableStage(forgedBody, 0);
      if (stage != nullptr) {
        edit(const_cast<std::vector<lang::MirPlace> &>(
            forgedBody.places)[stage->operands.front().place - 1]);
      }
    });
  };
  expect(!forgedSlot([](lang::MirPlace &slot) { slot.sourceValue = 0; }),
         "the verifier should reject a hidden argument slot detached from its "
         "producer identity");
  expect(!forgedSlot([](lang::MirPlace &slot) { slot.temporary = 0; }),
         "the verifier should reject an unnumbered hidden argument slot");
  expect(!forged([&](lang::MirBody &forgedBody) {
    const lang::MirPlaceId slot = hidden.front()->operands.front().place;
    for (lang::MirBlock &block :
         const_cast<std::vector<lang::MirBlock> &>(forgedBody.blocks)) {
      for (lang::MirInstruction &instruction :
           const_cast<std::vector<lang::MirInstruction> &>(
               block.instructions)) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.destination == slot) {
          for (lang::MirLifecycleEvent &event : instruction.lifecycle) {
            event.conditional = false;
          }
        }
      }
    }
  }),
         "the verifier should reject an unconditional drop of a branch-only "
         "argument slot after the merge");
}

void testRetainedBorrowedCallResults() {
  // A reference binding that retains a borrowed call result keeps the call
  // result's own loan identity. Recreating a Local loan from the receiver
  // origin instead produced a source place whose recorded type stated the
  // returned referent while its projection chain derived the receiver's
  // subobject, which the C++ emitter's place-coherence scan declined as
  // GTI-B0001.
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("retained-borrowed-call-results.gti", R"(
class Box {
  mut int32_t value;

public:
  Box(int32_t initial) : value(initial) {}
  mut int32_t& write() mut { return this.value; }
  int32_t& observe() { return this.value; }
  int32_t read() { return this.value; }
};

class Boxes {
public:
  mut Box left = Box(1);
  mut Box right = Box(2);
public:
  Boxes() = default;
};

class Outer {
public:
  mut Boxes inner = Boxes();
public:
  Outer() = default;
};

int32_t retain_through_alias() {
  mut Boxes parent = Boxes();
  mut Boxes& alias = parent;
  mut int32_t& child = alias.left.write();
  child = 5;
  return parent.left.read();
}

int32_t retain_across_statements() {
  mut Boxes parent = Boxes();
  mut Boxes& alias = parent;
  mut int32_t& child = alias.right.write();
  int32_t observed = alias.left.read();
  child = observed + 40;
  return parent.right.read();
}

int32_t retain_nested_field() {
  mut Outer outer = Outer();
  mut Outer& alias = outer;
  mut int32_t& child = alias.inner.right.write();
  child = 7;
  return outer.inner.right.read();
}

int32_t retain_read_only() {
  mut Boxes parent = Boxes();
  mut Boxes& alias = parent;
  int32_t& observed = alias.left.observe();
  return observed;
}

int32_t retain_from_local_receiver() {
  mut Boxes parent = Boxes();
  mut int32_t& child = parent.left.write();
  child = 9;
  return parent.left.read();
}

int32_t retain_through_reference_parameter(mut Box& box) {
  mut int32_t& child = box.write();
  child = 3;
  return box.read();
}

int main() {
  mut Box box = Box(0);
  return retain_through_alias() + retain_across_statements() +
         retain_nested_field() + retain_read_only() +
         retain_from_local_receiver() +
         retain_through_reference_parameter(box) - 66;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected retained-borrow diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "retained borrowed call results should lower to verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  // The emitter walks a projected place from its concrete root type; this
  // mirrors that walk so the structural assertion below fails on exactly the
  // disagreement the emitter declines, rather than on emission alone.
  const auto derivedType =
      [&](const lang::MirBody &body,
          const lang::MirPlace &place) -> std::optional<lang::SemanticType> {
    if (place.projections.empty()) {
      return place.type;
    }
    std::optional<lang::SemanticType> current;
    if (place.root == lang::MirPlaceRootKind::Binding) {
      const auto root = std::find_if(
          body.places.begin(), body.places.end(),
          [&](const lang::MirPlace &candidate) {
            return candidate.projections.empty() &&
                   candidate.root == lang::MirPlaceRootKind::Binding &&
                   candidate.binding == place.binding;
          });
      if (root != body.places.end()) {
        current = root->type;
      }
    }
    for (const lang::MirPlaceProjection &projection : place.projections) {
      if (!current) {
        return std::nullopt;
      }
      if (projection.kind == lang::MirProjectionKind::Dereference) {
        if (current->arguments.size() != 1) {
          return std::nullopt;
        }
        lang::SemanticType pointee = current->arguments.front();
        current = std::move(pointee);
        continue;
      }
      if (projection.kind != lang::MirProjectionKind::Field) {
        return std::nullopt;
      }
      const auto owner =
          std::find_if(frontend.mir.classInstances().begin(),
                       frontend.mir.classInstances().end(),
                       [&](const lang::MirClassInstance &candidate) {
                         return candidate.type == *current;
                       });
      if (owner == frontend.mir.classInstances().end()) {
        return std::nullopt;
      }
      const auto field = std::find_if(
          owner->declaredFields.begin(), owner->declaredFields.end(),
          [&](const lang::MirClassFieldInfo &candidate) {
            return candidate.symbol == projection.field;
          });
      if (field == owner->declaredFields.end()) {
        return std::nullopt;
      }
      current = field->type;
    }
    return current;
  };

  const auto mirBodyFor =
      [&](const std::string &name) -> const lang::MirBody * {
    const lang::FunctionDecl *function =
        findTopLevelFunction(frontend.program, name);
    for (const lang::HirFunctionInstance &candidate :
         frontend.hir.functionInstances()) {
      if (candidate.source != function) {
        continue;
      }
      const lang::MirFunctionInstance *instance =
          frontend.mir.findFunctionInstance(candidate.id);
      return instance == nullptr ? nullptr : &instance->body;
    }
    return nullptr;
  };

  for (const std::string name :
       {"retain_through_alias", "retain_across_statements",
        "retain_nested_field", "retain_read_only", "retain_from_local_receiver",
        "retain_through_reference_parameter"}) {
    const lang::MirBody *body = mirBodyFor(name);
    expect(body != nullptr,
           "the retained-borrow fixture should retain the MIR body of " + name);
    if (body == nullptr) {
      continue;
    }

    bool coherent = true;
    for (const lang::MirPlace &place : body->places) {
      const std::optional<lang::SemanticType> derived =
          derivedType(*body, place);
      if (derived && *derived != place.type) {
        coherent = false;
      }
    }
    expect(coherent,
           "every binding-rooted projected place in " + name +
               " should record the type its own projection chain derives");

    std::vector<lang::HirValueId> callValues;
    for (const lang::MirBlock &block : body->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call) {
          callValues.push_back(instruction.hirValue);
        }
      }
    }

    std::size_t retainedCallResults = 0;
    bool retainedSourcesAgree = true;
    for (const lang::MirLoan &loan : body->loans) {
      if (loan.carriers.empty() ||
          std::find(callValues.begin(), callValues.end(), loan.producedBy) ==
              callValues.end()) {
        continue;
      }
      // A carried loan produced by a call is the retained borrow: it must
      // stay that call result's own loan, and its origin place must record
      // the type its own projection chain derives.
      ++retainedCallResults;
      const lang::MirPlace *source = body->findPlace(loan.source);
      const std::optional<lang::SemanticType> derived =
          source == nullptr ? std::nullopt : derivedType(*body, *source);
      if (loan.kind != lang::MirLoanKind::CallResult || source == nullptr ||
          !derived || *derived != source->type) {
        retainedSourcesAgree = false;
      }
    }
    expect(retainedCallResults == 1 && retainedSourcesAgree,
           "the retained borrow in " + name +
               " should keep the call result's own loan over an origin place "
               "whose type agrees with its projection chain");
  }

  expect(nativeBackendEmits(frontend),
         "the native backend should emit retained borrowed call results "
         "without declining the retained loan's place");
}

void testNestedBorrowChainLoanOrdering() {
  // A reference binding may retain the result of a call whose own argument or
  // receiver is already a borrowed call result. Lowering keeps that outermost
  // call result's loan, so the links it reborrows must stay live: a parent
  // loan outlives its child reborrow on every edge, normal and failure alike.
  // Releasing the transient links by expression extent ended the parent while
  // the retained child was still active, which MIR verification correctly
  // rejected as "loan N is ended before its active child reborrow".
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("nested-borrow-chain.gti", R"(
class Leaf {
  mut int32_t value = 1;
  mut int32_t counter = 0;

public:
  Leaf() {}
  mut int32_t& write() mut { return this.value; }
  mut int32_t& overflow() mut {
    this.counter = 2147483647;
    this.counter += 1;
    return this.value;
  }
  int32_t& observe() { return this.value; }
  int32_t read() { return this.value; }
};

class Middle {
  mut Leaf leaf = Leaf();

public:
  Middle() {}
  mut Leaf& reach() mut { return this.leaf; }
  Leaf& look() { return this.leaf; }
  int32_t read() { return this.leaf.read(); }
};

class Pair {
public:
  mut Middle left = Middle();
  mut Middle right = Middle();
public:
  Pair() = default;
};

class Top {
public:
  mut Pair pair = Pair();
public:
  Top() = default;
};

int32_t& pick(Leaf& candidate) { return candidate.observe(); }

int32_t retained_argument() {
  mut Pair pair = Pair();
  int32_t& chosen = pick(pair.right.look());
  return chosen;
}

int32_t retained_receiver() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.right.reach().write();
  chosen = 8;
  return pair.right.read();
}

int32_t two_level_chain() {
  mut Top top = Top();
  mut int32_t& chosen = top.pair.left.reach().write();
  chosen = 6;
  return top.pair.left.read();
}

int32_t retained_across_statements() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.left.reach().write();
  int32_t observed = 30;
  chosen = observed + 7;
  return pair.left.read();
}

int32_t retained_chain_can_fail() {
  mut Pair pair = Pair();
  mut int32_t& chosen = pair.right.reach().overflow();
  chosen = 5;
  return pair.right.read();
}

int32_t single_level_contrast() {
  mut Leaf leaf = Leaf();
  mut int32_t& chosen = leaf.write();
  chosen = 9;
  return leaf.read();
}

int main() {
  return retained_argument() + retained_receiver() + two_level_chain() +
         retained_across_statements() + retained_chain_can_fail() +
         single_level_contrast() - 66;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected nested-borrow-chain diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "retained borrows built from another borrowed call result should "
         "lower to verified MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto mirBodyFor =
      [&](const std::string &name) -> const lang::MirBody * {
    const lang::FunctionDecl *function =
        findTopLevelFunction(frontend.program, name);
    for (const lang::HirFunctionInstance &candidate :
         frontend.hir.functionInstances()) {
      if (candidate.source != function) {
        continue;
      }
      const lang::MirFunctionInstance *instance =
          frontend.mir.findFunctionInstance(candidate.id);
      return instance == nullptr ? nullptr : &instance->body;
    }
    return nullptr;
  };

  const auto isAncestor = [&](const lang::MirBody &body, lang::MirLoanId older,
                              lang::MirLoanId candidate) {
    std::unordered_set<lang::MirLoanId> visited;
    const lang::MirLoan *loan = body.findLoan(candidate);
    while (loan != nullptr && loan->parent != 0 &&
           visited.insert(loan->parent).second) {
      if (loan->parent == older) {
        return true;
      }
      loan = body.findLoan(loan->parent);
    }
    return false;
  };

  for (const std::string name :
       {"retained_argument", "retained_receiver", "two_level_chain",
        "retained_across_statements", "retained_chain_can_fail",
        "single_level_contrast"}) {
    const lang::MirBody *body = mirBodyFor(name);
    expect(body != nullptr,
           "the nested-borrow-chain fixture should retain the MIR body of " +
               name);
    if (body == nullptr) {
      continue;
    }

    // The structural rule the schedule violated: within one block, no
    // EndBorrow may precede the EndBorrow of a loan that descends from it.
    // Every block is checked, so the normal route and each defined-failure
    // cleanup edge are held to the same order.
    bool parentsOutliveChildren = true;
    std::size_t checkedEndings = 0;
    for (const lang::MirBlock &block : body->blocks) {
      std::vector<lang::MirLoanId> ended;
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
            !instruction.loan) {
          continue;
        }
        for (const lang::MirLoanId earlier : ended) {
          if (isAncestor(*body, earlier, *instruction.loan)) {
            parentsOutliveChildren = false;
          }
        }
        ended.push_back(*instruction.loan);
        ++checkedEndings;
      }
    }
    expect(parentsOutliveChildren && checkedEndings > 0,
           "every block of " + name +
               " should end each parent loan after the child reborrows that "
               "hang from it");

    // Every reborrow's parent must still be live where the child is created,
    // which is what keeping the transient links alive buys.
    bool reborrowParentsAreLive = true;
    for (const lang::MirBlock &block : body->blocks) {
      std::vector<lang::MirLoanId> ended;
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (!instruction.loan) {
          continue;
        }
        if (instruction.kind == lang::MirInstructionKind::EndBorrow) {
          ended.push_back(*instruction.loan);
          continue;
        }
        const lang::MirLoan *loan = body->findLoan(*instruction.loan);
        if (loan == nullptr || loan->parent == 0) {
          continue;
        }
        if (std::find(ended.begin(), ended.end(), loan->parent) !=
            ended.end()) {
          reborrowParentsAreLive = false;
        }
      }
    }
    expect(reborrowParentsAreLive,
           "no reborrow in " + name +
               " should be created from a loan its own block already ended");
  }

  // The retained binding keeps the outermost call result's own loan rather
  // than a Local loan rebuilt from the receiver or argument origin, and that
  // loan reborrows the link the chain was built from.
  for (const std::string name :
       {"retained_argument", "retained_receiver", "two_level_chain",
        "retained_across_statements", "retained_chain_can_fail"}) {
    const lang::MirBody *body = mirBodyFor(name);
    if (body == nullptr) {
      continue;
    }
    std::vector<lang::HirValueId> callValues;
    for (const lang::MirBlock &block : body->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call) {
          callValues.push_back(instruction.hirValue);
        }
      }
    }
    std::size_t retainedChainResults = 0;
    for (const lang::MirLoan &loan : body->loans) {
      if (loan.carriers.empty() ||
          std::find(callValues.begin(), callValues.end(), loan.producedBy) ==
              callValues.end()) {
        continue;
      }
      const lang::MirLoan *parent = body->findLoan(loan.parent);
      if (loan.kind == lang::MirLoanKind::CallResult && parent != nullptr &&
          parent->kind == lang::MirLoanKind::CallResult) {
        ++retainedChainResults;
      }
    }
    expect(retainedChainResults == 1,
           "the retained borrow in " + name +
               " should keep the outermost call result's own loan as a "
               "reborrow of the link it was built from");
  }

  // The single-level contrast keeps the root call-result rule unchanged.
  const lang::MirBody *contrast = mirBodyFor("single_level_contrast");
  bool contrastIsRooted = contrast != nullptr;
  if (contrast != nullptr) {
    for (const lang::MirLoan &loan : contrast->loans) {
      if (!loan.carriers.empty() &&
          (loan.kind != lang::MirLoanKind::CallResult || loan.parent != 0)) {
        contrastIsRooted = false;
      }
    }
  }
  expect(contrastIsRooted,
         "a single-level retained borrow should still keep its root "
         "call-result loan");

  expect(nativeBackendEmits(frontend),
         "the native backend should emit retained borrows built from another "
         "borrowed call result");
}

void testCleanupOwningTemporaryArguments() {
  const std::string source = R"(
class cleanup_t {
public:
  cleanup_t(int32_t initial) : value(initial) {}
  cleanup_t(cleanup_t& other) = delete;
  cleanup_t(cleanup_t&& other) = default;
  ~cleanup_t() {}

  int32_t value;
};

int32_t read(cleanup_t value) { return value.value; }

expected<cleanup_t, int32_t> make_ready(int32_t value) {
  return cleanup_t(value);
}

int main() {
  int32_t direct = read(cleanup_t(3));
  expected<cleanup_t, int32_t> ready = make_ready(4);
  return direct + read(std::move(ready).value()) - 7;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cleanup-owning-temporary-arguments.gti", source,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected cleanup-temporary diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "cleanup-owning constructor and Expected results should lower to "
         "verified by-value argument transfers");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the cleanup-temporary fixture should retain its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  std::size_t directTransfers = 0;
  std::size_t expectedTransfers = 0;
  const auto instructionById = [&](lang::MirInstructionId id) {
    for (const lang::MirBlock &block : entry->body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == id;
                       });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &stage : block.instructions) {
      if (stage.kind != lang::MirInstructionKind::CallInput ||
          stage.callInputKind != lang::HirCallInputKind::MoveValue ||
          !stage.result || !stage.destination || !stage.preparedParameterDrop ||
          stage.operands.size() != 1 ||
          stage.operands.front().kind != lang::MirOperandKind::Value ||
          stage.lifecycle.size() != 1) {
        continue;
      }
      const lang::MirValue *sourceValue =
          entry->body.findValue(stage.operands.front().value);
      const lang::MirInstruction *producer =
          sourceValue == nullptr ? nullptr
                                 : instructionById(sourceValue->definition);
      const lang::MirDropObligation *sourceDrop =
          entry->body.findDropObligation(stage.lifecycle.front().source);
      const lang::MirDropObligation *preparedDrop =
          entry->body.findDropObligation(*stage.preparedParameterDrop);
      const lang::MirPlace *sourcePlace =
          sourceDrop == nullptr ? nullptr
                                : entry->body.findPlace(sourceDrop->place);
      const lang::MirPlace *preparedPlace =
          entry->body.findPlace(*stage.destination);
      const std::vector<lang::MirValueUse> stagedUses =
          entry->body.usesOf(*stage.result);
      const lang::MirInstruction *consumer =
          stagedUses.size() == 1
              ? instructionById(stagedUses.front().instruction)
              : nullptr;
      const bool transfer =
          producer != nullptr && sourceValue != nullptr &&
          sourceValue->info.type.kind == lang::SemanticType::Class &&
          sourceValue->info.traits.drop == lang::DropKind::Lexical &&
          sourceDrop != nullptr &&
          sourceDrop->kind == lang::MirDropObligationKind::Value &&
          sourceDrop->dropType.requiresActiveCleanup &&
          preparedDrop != nullptr &&
          preparedDrop->kind ==
              lang::MirDropObligationKind::PreparedParameter &&
          sourcePlace != nullptr &&
          sourcePlace->root == lang::MirPlaceRootKind::Value &&
          sourcePlace->value == sourceValue->id &&
          sourcePlace->projections.empty() && preparedPlace != nullptr &&
          preparedPlace->root == lang::MirPlaceRootKind::Temporary &&
          preparedDrop->place == preparedPlace->id &&
          stage.lifecycle.front().kind ==
              lang::MirLifecycleEventKind::Reparent &&
          stage.lifecycle.front().target == preparedDrop->id &&
          consumer != nullptr &&
          consumer->kind == lang::MirInstructionKind::Call &&
          std::count_if(consumer->lifecycle.begin(), consumer->lifecycle.end(),
                        [&](const lang::MirLifecycleEvent &event) {
                          return event.kind ==
                                     lang::MirLifecycleEventKind::TransferOut &&
                                 event.source == preparedDrop->id &&
                                 event.target == 0 && !event.conditional &&
                                 !event.failureCleanup;
                        }) == 1;
      expect(transfer,
             "each cleanup-owning CallInput should reparent one value drop "
             "into one prepared parameter consumed by its call");
      if (producer != nullptr &&
          producer->kind == lang::MirInstructionKind::Construct) {
        directTransfers += transfer ? 1 : 0;
      }
      // The owning extraction is the prelude class's consuming value().
      if (producer != nullptr &&
          isExpectedMemberCall(frontend, *producer, "value")) {
        expectedTransfers += transfer ? 1 : 0;
      }
    }
  }
  expect(directTransfers == 1 && expectedTransfers == 1,
         "the entry should retain one direct constructor transfer and one "
         "owning Expected extraction transfer");
  expect(nativeBackendEmits(frontend),
         "the native backend should emit both verified transfers without "
         "inventing cleanup storage");

  const lang::FrontendResult rejected = lang::Frontend().analyze(
      "cleanup-owning-named-copy.gti", R"(
class cleanup_t {
public:
  cleanup_t() {}
  cleanup_t(cleanup_t& other) = delete;
  cleanup_t(cleanup_t&& other) = default;
  ~cleanup_t() {}
};
void consume(cleanup_t value) {}
int main() {
  cleanup_t local{};
  consume(local);
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!rejected.canGenerateCode() &&
             hasDiagnosticCode(rejected.diagnostics, "GTI-S2018"),
         "a named move-only lvalue should remain rejected as a deleted "
         "by-value copy");
}

// An assignment resolves its target place before analysing the assigned
// value. The joins of `&&`, `||` and `?:` inside that value restore the scope
// state of their operands; they must do so without reallocating the bindings
// the place is rooted in (a receiver, a local or an indexed local), because
// the loan-conflict check and reinitialisation read the root after the value.
// An AddressSanitizer build exposed the stale root; this keeps every shape.
void testExpressionJoinsKeepAssignmentPlaces() {
  const std::string accepted = R"(
class Gate {
  mut bool open = false;
public:
  Gate() = default;
  void update(bool armed, bool ready) mut {
    this.open = armed && ready;
    this.open = armed || (ready && !armed);
    this.open = armed ? ready && armed : false;
  }
  bool isOpen() { return this.open; }
};
struct Counter {
  mut int value = 0;
  mut bool seen = false;
public:
  Counter() = default;
};
int main() {
  mut Gate gate{};
  gate.update(true, true);
  mut Counter counter{};
  mut bool[2] flags = {false, false};
  bool armed = gate.isOpen();
  bool ready = true;
  counter.seen = armed && ready;
  counter.seen = false && ready;
  counter.seen = true || ready;
  counter.value += (armed && ready) ? 1 : 0;
  flags[0] = armed && ready;
  flags[1] = armed ? ready : false;
  return counter.value == 1 && counter.seen && flags[0] && flags[1] ? 0 : 1;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "expression-join-assignment-places.gti", accepted,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << "Unexpected expression-join diagnostic: " << diagnostic.message
              << '\n';
  }
  expect(frontend.diagnostics.empty() && frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "field, receiver and indexed assignments of logical and conditional "
         "values should analyse and lower to verified MIR");

  const std::string conflicting = R"(
struct Counter {
  mut int value = 0;
  mut bool seen = false;
public:
  Counter() = default;
};
int main() {
  mut Counter counter{};
  bool armed = true;
  bool ready = true;
  Counter& view = counter;
  counter.seen = armed && ready;
  return view.value;
}
)";
  const lang::FrontendResult rejected = lang::Frontend().analyze(
      "expression-join-assignment-conflict.gti", conflicting,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::size_t assignment = conflicting.find("seen = armed && ready");
  // MIR reports a field store at its written place's root object.
  const std::size_t root = conflicting.find("counter.seen = armed");
  const auto conflict =
      std::find_if(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                   [&](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2017" &&
                            (diagnostic.primary.start == assignment ||
                             (diagnostic.primary.start == root));
                   });
  expect(!rejected.canGenerateCode() && rejected.diagnostics.size() == 1 &&
             conflict != rejected.diagnostics.end() &&
             conflict->message.find("overlapping borrow may still be live") !=
                 std::string::npos,
         "a field assignment whose value is a logical expression should still "
         "report the live borrow of its root after the join");
}

// On a mutable receiver, an accessor pair selects its read-only form for a
// use that only reads the result, and its `mut` form for one that may mutate.
void testUseBasedAccessorSelection() {
  const std::string pair = R"(
struct Slot {
  mut int32_t value = 2;
  int32_t& get() { return this.value; }
  mut int32_t& get() mut { return this.value; }
  int32_t& operator[](std::size_t index) { return this.value; }
  mut int32_t& operator[](std::size_t index) mut { return this.value; }
  bool empty() { return this.value == 0; }
public:
  Slot() = default;
};
int32_t read(int32_t& value) { return value; }
void write(mut int32_t& value) { value += 1; }
)";
  const std::string selections = pair + R"(
int main() {
  mut Slot slot{};
  int32_t& held = slot.get();
  int32_t copied = slot.get();
  int32_t passed = read(slot.get());
  int32_t& element = slot[std::size_t(0)];
  bool tested = slot.get() == 2 && !slot.empty();
  int32_t sum = held + copied + passed + element;
  mut int32_t& edited = slot.get();
  edited += sum;
  write(slot.get());
  return tested ? slot.get() : 0;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "use-based-accessor-selection.gti", selections,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << "Unexpected accessor selection diagnostic: "
                << diagnostic.message << '\n';
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "reads of a mutable owner through accessor pairs stay valid while "
         "one result is live");
  const lang::FunctionDecl *mainFunction =
      findTopLevelFunction(frontend.program, "main");
  const auto statement = [&](std::size_t index) -> const lang::Stmt * {
    return mainFunction == nullptr ||
                   index >= mainFunction->body()->statements().size()
               ? nullptr
               : mainFunction->body()->statements()[index].get();
  };
  const auto initializer = [&](std::size_t index) -> const lang::Expr * {
    const auto *declaration =
        dynamic_cast<const lang::VariableDecl *>(statement(index));
    return declaration == nullptr ? nullptr : declaration->initializer().get();
  };
  const auto firstArgument =
      [](const lang::Expr *expression) -> const lang::Expr * {
    const auto *call = dynamic_cast<const lang::Call *>(expression);
    return call == nullptr || call->arguments().empty()
               ? nullptr
               : call->arguments().front().get();
  };
  const auto selected =
      [&](const lang::Expr *expression) -> std::optional<lang::AccessMode> {
    const auto *call = dynamic_cast<const lang::Call *>(expression);
    const lang::ResolvedCallInfo *resolved =
        call == nullptr ? nullptr : frontend.semantics.findCall(*call);
    if (resolved == nullptr ||
        resolved->returnType.kind != lang::SemanticType::Reference)
      return std::nullopt;
    return resolved->returnType.referenceAccess;
  };
  const auto *logical = dynamic_cast<const lang::Logical *>(initializer(5));
  const auto *comparison =
      logical == nullptr
          ? nullptr
          : dynamic_cast<const lang::Binary *>(logical->left().get());
  const auto *written =
      dynamic_cast<const lang::ExpressionStmt *>(statement(9));
  const auto *returned = dynamic_cast<const lang::ReturnStmt *>(statement(10));
  const auto *conditional = returned == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::ConditionalExpr *>(
                                      returned->value().get());
  expect(selected(initializer(1)) == lang::AccessMode::ReadOnly &&
             selected(initializer(2)) == lang::AccessMode::ReadOnly &&
             selected(firstArgument(initializer(3))) ==
                 lang::AccessMode::ReadOnly &&
             comparison != nullptr &&
             selected(comparison->left().get()) == lang::AccessMode::ReadOnly &&
             conditional != nullptr &&
             selected(conditional->thenExpression().get()) ==
                 lang::AccessMode::ReadOnly,
         "a read-only reference, copy, read-only argument, operand and "
         "returned value select the read-only accessor");
  expect(selected(initializer(7)) == lang::AccessMode::Mutable &&
             written != nullptr &&
             selected(firstArgument(written->expression().get())) ==
                 lang::AccessMode::Mutable,
         "a mutable reference and a mutable argument select the mut accessor");
  const lang::ResolvedOperatorInfo *subscript =
      initializer(4) == nullptr
          ? nullptr
          : frontend.semantics.findOperator(*initializer(4));
  expect(subscript != nullptr && subscript->receiverMutability ==
                                     lang::ReceiverMutability::ReadOnly,
         "a read-only reference to an element selects the read-only "
         "operator[]");

  const std::string conflicting = pair + R"(
int main() {
  mut Slot slot{};
  int32_t& held = slot.get();
  slot.get() = 4;
  return held;
}
)";
  const lang::FrontendResult rejected = lang::Frontend().analyze(
      "use-based-accessor-conflict.gti", conflicting,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::size_t assignment = conflicting.find("slot.get() = 4");
  expect(!rejected.canGenerateCode() &&
             std::any_of(rejected.diagnostics.begin(),
                         rejected.diagnostics.end(),
                         [&](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-S2017" &&
                                  diagnostic.primary.start >= assignment &&
                                  diagnostic.primary.start <
                                      assignment +
                                          std::string_view("slot.get()").size();
                         }),
         "assigning through the accessor still selects the mut form and "
         "conflicts with a live read-only result");

  const std::string readOnly = pair + R"(
int main() {
  mut Slot slot{};
  int32_t& held = slot.get();
  return held + slot[std::size_t(0)] - 4;
}
)";
  const lang::FrontendResult advised = lang::Frontend().analyze(
      "use-based-accessor-advice.gti", readOnly, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(advised.canGenerateCode() &&
             std::any_of(advised.diagnostics.begin(), advised.diagnostics.end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-S2096" &&
                                  diagnostic.message.find("'slot'") !=
                                      std::string::npos;
                         }),
         "a mutable owner read only through accessor pairs is never used "
         "mutably");
}

constexpr NamedTest kTests[] = {
    {"testOwnershipSemanticFoundation", testOwnershipSemanticFoundation},
    {"testDisjointFieldReceiverBorrowsThroughReferenceParameters",
     testDisjointFieldReceiverBorrowsThroughReferenceParameters},
    {"testGlobalReferencePlaces", testGlobalReferencePlaces},
    {"testGlobalBorrowReturns", testGlobalBorrowReturns},
    {"testArgumentTiedReferenceReturns", testArgumentTiedReferenceReturns},
    {"testNestedMemberReferenceReturnForwarding",
     testNestedMemberReferenceReturnForwarding},
    {"testDefaultedDefaultLifecycle", testDefaultedDefaultLifecycle},
    {"testRestrictedMemberOperators", testRestrictedMemberOperators},
    {"testCallableMemberOperators", testCallableMemberOperators},
    {"testPassiveCleanupRangeTemporary", testPassiveCleanupRangeTemporary},
    {"testSharedPayloadSiblingReborrows", testSharedPayloadSiblingReborrows},
    {"testConfinedCallableParameters", testConfinedCallableParameters},
    {"testConfinedCallableResults", testConfinedCallableResults},
    {"testFieldInitializerRollbackAndFailureEdges",
     testFieldInitializerRollbackAndFailureEdges},
    {"testConstructorRollbackSpansLocalCleanup",
     testConstructorRollbackSpansLocalCleanup},
    {"testDefaultFieldRollbackEntersConstructor",
     testDefaultFieldRollbackEntersConstructor},
    {"testConstructorPartialRollbackRepresentation",
     testConstructorPartialRollbackRepresentation},
    {"testMutableTemporaryReceivers", testMutableTemporaryReceivers},
    {"testVirtualTemporaryReceivers", testVirtualTemporaryReceivers},
    {"testGeneratedDefaultTemporaryArgumentBorrows",
     testGeneratedDefaultTemporaryArgumentBorrows},
    {"testGeneratedDefaultConstructionContexts",
     testGeneratedDefaultConstructionContexts},
    {"testTemporaryArgumentBorrows", testTemporaryArgumentBorrows},
    {"testConditionalTemporaryArgumentBorrows",
     testConditionalTemporaryArgumentBorrows},
    {"testRetainedBorrowedCallResults", testRetainedBorrowedCallResults},
    {"testNestedBorrowChainLoanOrdering", testNestedBorrowChainLoanOrdering},
    {"testCleanupOwningTemporaryArguments",
     testCleanupOwningTemporaryArguments},
    {"testExpressionJoinsKeepAssignmentPlaces",
     testExpressionJoinsKeepAssignmentPlaces},
    {"testUseBasedAccessorSelection", testUseBasedAccessorSelection},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_ownership_tests");
  return runNamedTests(argc, argv, kTests, "ownership");
}
