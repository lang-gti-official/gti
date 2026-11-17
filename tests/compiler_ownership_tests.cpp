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

const lang::MirLoan *mirLoanParent(const MirRetained &retained) {
  return retained.function == nullptr || retained.loan == nullptr
             ? nullptr
             : retained.function->body.findLoan(retained.loan->parent);
}

bool hasMirIntrinsicKind(const lang::FrontendResult &frontend,
                         lang::IntrinsicKind kind) {
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == kind)
          return true;
  return false;
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

// Concrete MIR class instances of the source class `qualifiedName`.
std::size_t mirClassInstanceCount(const lang::FrontendResult &frontend,
                                  std::string_view qualifiedName) {
  return static_cast<std::size_t>(std::count_if(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(),
      [&](const lang::MirClassInstance &instance) {
        const lang::ClassTypeInfo *info =
            frontend.semantics.findClassType(instance.type.classId);
        return instance.type.kind == lang::SemanticType::Class &&
               info != nullptr && info->qualifiedName == qualifiedName;
      }));
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

// The source name of a binding-rooted MIR place, or empty.
std::string_view mirBindingName(const lang::FrontendResult &frontend,
                                const lang::MirFunctionInstance &function,
                                lang::HirBindingId binding) {
  const lang::HirFunctionInstance *source =
      frontend.hir.findFunctionInstance(function.id);
  if (source == nullptr)
    return {};
  for (const lang::HirBinding &candidate : source->body.bindings) {
    if (candidate.id != binding)
      continue;
    if (candidate.variable != nullptr)
      return candidate.variable->name().lexeme;
    if (candidate.parameter != nullptr)
      return candidate.parameter->name.lexeme;
  }
  return {};
}

// Some instruction of `function` consumes the unprojected binding `name`
// through a Move operand: the value is transferred, never copied.
bool mirMovesBinding(const lang::FrontendResult &frontend,
                     const lang::MirFunctionInstance &function,
                     std::string_view name) {
  for (const lang::MirBlock &block : function.body.blocks)
    for (const lang::MirInstruction &instruction : block.instructions)
      for (const lang::MirOperand &operand : instruction.operands) {
        if (operand.kind != lang::MirOperandKind::Move)
          continue;
        const lang::MirPlace *place = function.body.findPlace(operand.place);
        if (place != nullptr && place->projections.empty() &&
            place->root == lang::MirPlaceRootKind::Binding &&
            mirBindingName(frontend, function, place->binding) == name)
          return true;
      }
  return false;
}

// Every normal return of `function` publishes an escaping Return loan with
// `access`: the result is the loaned referent, never a copied value.
bool returnsOnlyEscapingLoan(const lang::MirFunctionInstance &function,
                             lang::AccessMode access) {
  bool returned = false;
  for (const lang::MirBlock &block : function.body.blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Return)
      continue;
    const lang::MirLoan *loan =
        block.terminator.returnLoan
            ? function.body.findLoan(*block.terminator.returnLoan)
            : nullptr;
    if (loan == nullptr || loan->kind != lang::MirLoanKind::Return ||
        !loan->escapes || loan->access != access)
      return false;
    returned = true;
  }
  return returned;
}

// The escaping Return loan is produced by a Borrow that transfers an exact
// call-result loan, rather than re-borrowing a place.
bool returnTransfersCallResultLoan(const lang::MirFunctionInstance &function) {
  for (const lang::MirBlock &block : function.body.blocks)
    for (const lang::MirInstruction &instruction : block.instructions) {
      const lang::MirLoan *result =
          instruction.kind == lang::MirInstructionKind::Borrow &&
                  instruction.loan
              ? function.body.findLoan(*instruction.loan)
              : nullptr;
      if (result == nullptr || result->kind != lang::MirLoanKind::Return ||
          instruction.operands.size() != 1 ||
          instruction.operands.front().kind != lang::MirOperandKind::Loan)
        continue;
      const lang::MirLoan *source =
          function.body.findLoan(instruction.operands.front().loan);
      if (source != nullptr && source->kind == lang::MirLoanKind::CallResult)
        return true;
    }
  return false;
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

// The source name of a MIR call's selected function target, or empty.
std::string_view mirCallTargetName(const lang::FrontendResult &frontend,
                                   const lang::MirInstruction &instruction) {
  if (instruction.kind != lang::MirInstructionKind::Call ||
      !instruction.functionTarget)
    return {};
  const lang::HirFunctionInstance *target =
      frontend.hir.findFunctionInstance(*instruction.functionTarget);
  return target == nullptr || target->source == nullptr
             ? std::string_view{}
             : std::string_view(target->source->name().lexeme);
}

// Some MIR body in the program calls a function whose source name is `name`.
bool mirCallsFunctionNamed(const lang::FrontendResult &frontend,
                           std::string_view name) {
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances())
    for (const lang::MirBlock &block : function.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        if (mirCallTargetName(frontend, instruction) == name)
          return true;
  return false;
}

lang::FrontendResult analyzeWithMemory(std::string_view name,
                                       std::string source) {
  source.insert(0, "import std.memory;\n");
  return lang::Frontend().analyze(std::filesystem::temp_directory_path() /
                                      std::string(name),
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
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

void testExplicitValueMoves() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "explicit-value-moves.gti", R"(
struct Record {
  int value = 4;
public:
  Record() = default;
};

int forward_int(int value) { return std::move(value); }
T transfer<T>(T value) { return std::move(value); }

int main() {
  int source = 1;
  int moved = std::move(source);
  int forwarded = forward_int(std::move(moved));

  mut int reusable = 2;
  int first = std::move(reusable);
  reusable = 3;
  int generic = transfer(std::move(first));

  Record record = Record();
  Record moved_record = std::move(record);
  return reusable + forwarded + generic + moved_record.value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected explicit-move diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "std::move should explicitly consume any named movable local value "
         "or by-value parameter");

  const lang::FunctionDecl *forward =
      findTopLevelFunction(valid.program, "forward_int");
  const lang::FunctionDecl *transfer =
      findTopLevelFunction(valid.program, "transfer");
  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const lang::BindingInfo *forwardedParameter =
      forward == nullptr
          ? nullptr
          : valid.semantics.findBinding(forward->parameters().front());
  const lang::BindingInfo *genericParameter =
      transfer == nullptr
          ? nullptr
          : valid.semantics.findBinding(transfer->parameters().front());
  const lang::VariableDecl *source = nullptr;
  const lang::VariableDecl *record = nullptr;
  if (main != nullptr) {
    for (const lang::StmtPtr &statement : main->body()->statements()) {
      const auto *variable =
          dynamic_cast<const lang::VariableDecl *>(statement.get());
      if (variable == nullptr) {
        continue;
      }
      if (variable->name().lexeme == "source") {
        source = variable;
      } else if (variable->name().lexeme == "record") {
        record = variable;
      }
    }
  }
  const lang::BindingInfo *sourceBinding =
      source == nullptr ? nullptr : valid.semantics.findBinding(*source);
  const lang::BindingInfo *recordBinding =
      record == nullptr ? nullptr : valid.semantics.findBinding(*record);
  expect(forwardedParameter != nullptr && forwardedParameter->explicitlyMoved &&
             genericParameter != nullptr && genericParameter->explicitlyMoved &&
             sourceBinding != nullptr && sourceBinding->explicitlyMoved &&
             recordBinding != nullptr && recordBinding->explicitlyMoved,
         "binding metadata should retain explicit consumption for parameters "
         "and local values");

  std::size_t moves = 0;
  bool allMovesHaveOneOperand = true;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    for (const lang::HirValue &value : instance.body.values) {
      if (value.kind != lang::HirValueKind::Move) {
        continue;
      }
      ++moves;
      allMovesHaveOneOperand = allMovesHaveOneOperand &&
                               value.intrinsic == lang::IntrinsicKind::Move &&
                               value.operands.size() == 1;
    }
  }
  expect(moves >= 7 && allMovesHaveOneOperand,
         "typed HIR should represent explicit moves as unary ownership "
         "operations rather than ordinary calls");

  // Explicitly consumed immutable bindings are moved, not copied, by MIR:
  // each consumption is a Move operand of the exact unprojected binding.
  const lang::MirFunctionInstance *forwardMir =
      mirFunctionNamed(valid, "forward_int");
  const lang::MirFunctionInstance *mainMir = mirFunctionNamed(valid, "main");
  expect(hasVerifiedSourceBody(forwardMir) && hasVerifiedSourceBody(mainMir) &&
             mirMovesBinding(valid, *forwardMir, "value") &&
             mirMovesBinding(valid, *mainMir, "source") &&
             mirMovesBinding(valid, *mainMir, "record") &&
             nativeBackendEmits(valid),
         "verified MIR should move explicitly consumed immutable bindings "
         "and the native backend should accept that program");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-explicit-value-moves.gti", R"(
int global_value = 1;

struct Box {
  int field = 1;
  int take() mut { return std::move(this.field); }
public:
  Box() = default;
};

int main() {
  int used = 1;
  int once = std::move(used);
  int twice = used;

  mut int conditional = 2;
  if (once == 1) {
    int branch = std::move(conditional);
  }
  int maybe_moved = conditional;

  [[discard]] std::move(1);
  [[discard]] std::move(global_value);
  int values[1] = {1};
  [[discard]] std::move(values[0]);
  mut int referenced = 3;
  int& alias = referenced;
  [[discard]] std::move(alias);

  mut int self_move = 5;
  self_move = std::move(self_move);

  int captured = 4;
  auto closure = [captured]() -> int { return std::move(captured); };
  return twice + maybe_moved + closure();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics, "has already been moved") &&
          hasDiagnostic(invalid.diagnostics,
                        "may have been moved on another control-flow path") &&
          hasDiagnostic(invalid.diagnostics,
                        "temporaries are already values") &&
          hasDiagnostic(invalid.diagnostics, "cannot consume global binding") &&
          hasDiagnostic(invalid.diagnostics,
                        "requires a writable field or constant-index") &&
          hasDiagnostic(invalid.diagnostics, "cannot consume a reference") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot move a binding into itself") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot consume immutable lambda capture"),
      "move diagnostics should reject unavailable values and unsupported or "
      "immutable storage");
}

void testImplicitReturnMoves() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "implicit-return-moves.gti", R"(
class Owner {
public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  ~Owner() {}
};

struct Copyable {
  int value = 7;
public:
  Copyable() = default;
};

Owner return_parameter(Owner value) {
  return value;
}

Owner return_local() {
  Owner value{};
  return (value);
}

Copyable return_copy(Copyable value) {
  return value;
}

T return_generic<T>(T value) {
  return value;
}

int main() {
  Owner owner = return_generic(Owner());
  Copyable copy = return_generic(Copyable());
  return copy.value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected implicit-return-move diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "whole move-only locals and by-value parameters should move "
         "implicitly at an exact return boundary");

  const lang::FunctionDecl *parameterFunction =
      findTopLevelFunction(frontend.program, "return_parameter");
  const lang::FunctionDecl *localFunction =
      findTopLevelFunction(frontend.program, "return_local");
  const lang::FunctionDecl *copyFunction =
      findTopLevelFunction(frontend.program, "return_copy");
  const lang::FunctionDecl *genericFunction =
      findTopLevelFunction(frontend.program, "return_generic");
  const auto returnStatement =
      [](const lang::FunctionDecl *function) -> const lang::ReturnStmt * {
    if (function == nullptr || function->body() == nullptr) {
      return nullptr;
    }
    for (const lang::StmtPtr &statement : function->body()->statements()) {
      if (const auto *returned =
              dynamic_cast<const lang::ReturnStmt *>(statement.get())) {
        return returned;
      }
    }
    return nullptr;
  };
  const lang::ReturnStmt *parameterReturn = returnStatement(parameterFunction);
  const lang::ReturnStmt *localReturn = returnStatement(localFunction);
  const lang::ReturnStmt *copyReturn = returnStatement(copyFunction);
  expect(parameterReturn != nullptr && localReturn != nullptr &&
             copyReturn != nullptr && parameterReturn->value() != nullptr &&
             localReturn->value() != nullptr && copyReturn->value() != nullptr,
         "the implicit-return fixture should retain all three return values");
  if (parameterReturn == nullptr || localReturn == nullptr ||
      copyReturn == nullptr || !parameterReturn->value() ||
      !localReturn->value() || !copyReturn->value()) {
    return;
  }

  const lang::ImplicitReturnMoveInfo *parameterMove =
      frontend.semantics.findImplicitReturnMove(*parameterReturn->value());
  const lang::ImplicitReturnMoveInfo *localMove =
      frontend.semantics.findImplicitReturnMove(*localReturn->value());
  expect(parameterMove != nullptr && localMove != nullptr &&
             frontend.semantics.findImplicitReturnMove(*copyReturn->value()) ==
                 nullptr,
         "semantics should record implicit movement only for exact "
         "noncopyable returns");

  const lang::VariableDecl *local = nullptr;
  if (localFunction != nullptr && localFunction->body() != nullptr &&
      !localFunction->body()->statements().empty()) {
    local = dynamic_cast<const lang::VariableDecl *>(
        localFunction->body()->statements().front().get());
  }
  const lang::BindingInfo *parameterBinding =
      parameterFunction == nullptr
          ? nullptr
          : frontend.semantics.findBinding(
                parameterFunction->parameters().front());
  const lang::BindingInfo *localBinding =
      local == nullptr ? nullptr : frontend.semantics.findBinding(*local);
  expect(parameterBinding != nullptr && parameterBinding->explicitlyMoved &&
             localBinding != nullptr && localBinding->explicitlyMoved,
         "implicitly returned owners should request physically movable "
         "native storage for their source bindings");

  const auto hasExactHirMove = [&](const lang::FunctionDecl *function,
                                   const lang::Expr *source) {
    const lang::FunctionInfo *info =
        function == nullptr ? nullptr
                            : frontend.semantics.findFunction(*function);
    if (info == nullptr || source == nullptr) {
      return false;
    }
    for (const lang::HirFunctionInstance &instance :
         frontend.hir.functionInstances()) {
      if (instance.declaration != info->id) {
        continue;
      }
      for (const lang::HirValue &value : instance.body.values) {
        if (value.source == source && value.kind == lang::HirValueKind::Move &&
            value.intrinsic == lang::IntrinsicKind::Move &&
            value.operands.size() == 1 && value.ownership &&
            value.ownership->kind == lang::OwnershipEventKind::Move) {
          return true;
        }
      }
    }
    return false;
  };
  expect(hasExactHirMove(parameterFunction, parameterReturn->value().get()) &&
             hasExactHirMove(localFunction, localReturn->value().get()) &&
             !hasExactHirMove(copyFunction, copyReturn->value().get()),
         "HIR should materialize each implicit return as the same unary Move "
         "used by explicit std::move while preserving copyable returns");

  const lang::FunctionInfo *genericInfo =
      genericFunction == nullptr
          ? nullptr
          : frontend.semantics.findFunction(*genericFunction);
  std::size_t genericInstances = 0;
  std::size_t genericMoveInstances = 0;
  if (genericInfo != nullptr) {
    for (const lang::HirFunctionInstance &instance :
         frontend.hir.functionInstances()) {
      if (instance.declaration != genericInfo->id) {
        continue;
      }
      ++genericInstances;
      genericMoveInstances +=
          std::any_of(instance.body.values.begin(), instance.body.values.end(),
                      [](const lang::HirValue &value) {
                        return value.kind == lang::HirValueKind::Move;
                      })
              ? 1
              : 0;
    }
  }
  expect(genericInstances == 2 && genericMoveInstances == 1,
         "concrete generic reanalysis should move the noncopyable instance "
         "and leave the copyable instance as a copy");

  const lang::FrontendResult excluded = lang::Frontend().analyze(
      "excluded-implicit-return-moves.gti", R"(
class Owner {
public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  ~Owner() {}
};

class Holder {
  Owner value;
public:
  Holder() : value(Owner()) {}
  Owner take() mut { return this.value; }
};

struct Pair {
  Owner value;
  int marker;

  Pair(Owner input) : value(std::move(input)), marker(0) {}
};

Owner return_binding() {
  auto [value, marker] = Pair(Owner());
  return value;
}

class Pinned {
public:
  Pinned() {}
  Pinned(Pinned& other) = delete;
  Pinned(Pinned&& other) = delete;
};

Pinned return_immovable(Pinned value) { return value; }
int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!excluded.canGenerateCode() &&
             countDiagnosticCode(excluded.diagnostics, "GTI-S2003") == 2 &&
             hasDiagnosticCode(excluded.diagnostics, "GTI-S2018") &&
             hasDiagnostic(excluded.diagnostics,
                           "Returning this value would copy move-only type") &&
             hasDiagnostic(excluded.diagnostics,
                           "cannot partially move structured binding") &&
             hasDiagnostic(excluded.diagnostics,
                           "Cannot return a value of type 'Pinned'"),
         "implicit return movement should exclude fields, structured-binding "
         "projections, and immovable values rather than weakening their "
         "diagnostics");

  // A return that initializes its declared type through an implicit
  // constructor copies a named move-only value like any other return. The
  // return reports that copy, not an argument of a constructor call the
  // source never wrote.
  const std::string contextualSource = R"(
class Owner {
public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  ~Owner() {}
};
class Wrapper {
  Owner owner;
public:
  implicit Wrapper(Owner owner) : owner(std::move(owner)) {}
};
class Holder {
  Owner value;
public:
  Holder() : value(Owner()) {}
  Wrapper wrap() { return this.value; }
};
Wrapper wrap_local() {
  Owner local = Owner();
  return local;
}
Wrapper wrap_parameter(Owner input) { return input; }
Wrapper wrap_moved() {
  Owner moved = Owner();
  return std::move(moved);
}
int main() { return 0; }
)";
  const lang::FrontendResult contextual = lang::Frontend().analyze(
      "contextual-return-copy.gti", contextualSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  // The diagnostic names `token` inside `statement`, with exactly one hint.
  const auto returnCopy = [&](std::string_view statement,
                              std::string_view token, std::string_view message,
                              std::string_view hint) {
    const std::size_t start =
        contextualSource.find(statement) + statement.find(token);
    return std::count_if(
               contextual.diagnostics.begin(), contextual.diagnostics.end(),
               [&](const lang::Diagnostic &diagnostic) {
                 return diagnostic.code == "GTI-S2003" &&
                        diagnostic.message == message &&
                        diagnostic.primary.start == start &&
                        diagnostic.primary.end == start + token.size() &&
                        diagnostic.hints.size() == 1 &&
                        diagnostic.hints.front() == hint &&
                        diagnostic.related.empty() && diagnostic.fixes.empty();
               }) == 1;
  };
  expect(!contextual.canGenerateCode() && contextual.diagnostics.size() == 3 &&
             returnCopy("return local;", "local",
                        "Cannot return 'local' by copy because type 'Owner' "
                        "is move-only.",
                        "Use 'return std::move(local);' to transfer "
                        "ownership.") &&
             returnCopy("return input;", "input",
                        "Cannot return 'input' by copy because type 'Owner' "
                        "is move-only.",
                        "Use 'return std::move(input);' to transfer "
                        "ownership.") &&
             returnCopy("return this.value;", "value",
                        "Returning this value would copy move-only type "
                        "'Owner'.",
                        "Return a fresh value or transfer an eligible owner "
                        "explicitly with std::move(...)."),
         "a return's implicit construction reports a move-only copy as the "
         "return's own GTI-S2003, at the returned value");
}

void testEdgeSensitiveMoveFlow() {
  const lang::FrontendResult loopBackedges = lang::Frontend().analyze(
      "loop-backedge-moves.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };

int main() {
  mut Record while_value = Record();
  mut int32_t while_index = 0;
  while (while_index < 2) {
    Record consumed = std::move(while_value);
    while_index++;
  }

  mut Record do_value = Record();
  mut int32_t do_index = 0;
  do {
    Record consumed = std::move(do_value);
    do_index++;
  } while (do_index < 2);

  mut Record for_value = Record();
  for (mut int32_t index = 0; index < 2; index++) {
    Record consumed = std::move(for_value);
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!loopBackedges.canGenerateCode() &&
             countDiagnosticContaining(loopBackedges.diagnostics,
                                       "reachable loop backedge") == 3,
         "while, do-while, and for should reject values consumed before a "
         "reachable backedge");

  const lang::FrontendResult conditionBackedges = lang::Frontend().analyze(
      "condition-backedge-moves.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };
bool consume(Record value) { return value.value == 7; }

int main() {
  mut Record while_value = Record();
  while (consume(std::move(while_value))) {}

  mut Record for_value = Record();
  for (; consume(std::move(for_value));) {}
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!conditionBackedges.canGenerateCode() &&
             countDiagnosticContaining(conditionBackedges.diagnostics,
                                       "reachable loop backedge") == 2,
         "while and for should validate moves performed by a condition before "
         "that condition can execute again");

  const lang::FrontendResult projectedBackedge = lang::Frontend().analyze(
      "projected-loop-backedge.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };
struct Holder { mut Record field = Record();  public: Holder() = default; };

int main() {
  mut Holder holder = Holder();
  mut int32_t index = 0;
  while (index < 2) {
    Record consumed = std::move(holder.field);
    index++;
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!projectedBackedge.canGenerateCode() &&
             hasDiagnostic(projectedBackedge.diagnostics,
                           "Place 'field' is not available at a reachable "
                           "loop backedge"),
         "loop backedges should validate projected field moves as well as "
         "whole bindings");

  const lang::FrontendResult reinitialized = lang::Frontend().analyze(
      "reinitialized-loop-backedges.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };

int main() {
  mut Record while_value = Record();
  mut int32_t while_index = 0;
  while (while_index < 2) {
    Record consumed = std::move(while_value);
    while_value = Record();
    while_index++;
  }

  mut Record do_value = Record();
  mut int32_t do_index = 0;
  do {
    Record consumed = std::move(do_value);
    do_value = Record();
    do_index++;
  } while (do_index < 2);

  mut Record for_value = Record();
  for (mut int32_t index = 0; index < 2; index++) {
    Record consumed = std::move(for_value);
    for_value = Record();
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(reinitialized.canGenerateCode(),
         "a value reinitialized on every reachable backedge should remain "
         "available for the next iteration");

  const lang::FrontendResult shortCircuit = lang::Frontend().analyze(
      "short-circuit-moves.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };
bool consume(Record value) { return value.value == 7; }

int test_and(bool gate) {
  mut Record value = Record();
  bool result = gate && (consume(std::move(value)), true);
  return value.value;
}

int test_or(bool gate) {
  mut Record value = Record();
  bool result = gate || (consume(std::move(value)), false);
  return value.value;
}

int main() { return test_and(false) + test_or(true); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!shortCircuit.canGenerateCode() &&
             countDiagnosticContaining(
                 shortCircuit.diagnostics,
                 "may have been moved on another control-flow path") == 2,
         "unknown logical left operands should merge the skipped and "
         "evaluated RHS move states");

  const lang::FrontendResult skippedLogical = lang::Frontend().analyze(
      "skipped-logical-moves.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };
bool consume(Record value) { return value.value == 7; }

int main() {
  mut Record and_value = Record();
  bool and_result = false && consume(std::move(and_value));
  mut Record or_value = Record();
  bool or_result = true || consume(std::move(or_value));
  return and_value.value + or_value.value - 14;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(skippedLogical.canGenerateCode(),
         "literal short-circuit operands should analyze a skipped RHS "
         "without consuming reachable move state");

  const lang::FrontendResult terminatingArm = lang::Frontend().analyze(
      "terminating-if-move.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };
int read_after(bool stop) {
  mut Record value = Record();
  if (stop) {
    Record consumed = std::move(value);
    return consumed.value;
  }
  return value.value;
}
int main() { return read_after(false) - 7; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(terminatingArm.canGenerateCode(),
         "a moved state from a terminating if arm should not reach following "
         "statements");

  const lang::FrontendResult unreachableRepair = lang::Frontend().analyze(
      "unreachable-move-repair.gti", R"(
struct Record { int32_t value = 7;  public: Record() = default; };
int read_after_switch(int32_t selector) {
  mut Record value = Record();
  switch (selector) {
  case 0:
    Record consumed = std::move(value);
    break;
    value = Record();
  default:
    break;
  }
  return value.value;
}

int main() {
  mut Record value = Record();
  mut int32_t index = 0;
  while (index < 2) {
    Record consumed = std::move(value);
    index++;
    continue;
    value = Record();
  }
  return read_after_switch(0);
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!unreachableRepair.canGenerateCode() &&
             hasDiagnostic(unreachableRepair.diagnostics,
                           "reachable loop backedge") &&
             hasDiagnostic(unreachableRepair.diagnostics,
                           "may have been moved on another control-flow path"),
         "unreachable assignments after continue or break must not repair "
         "reachable move state");

  const lang::FrontendResult unreachableDeclarations =
      lang::Frontend().analyze("unreachable-declarations.gti", R"(
int helper() {
  return 0;
  int32_t declared = 1;
  int32_t copied = declared;
}
int main() { return helper(); }
)");
  expect(unreachableDeclarations.canGenerateCode(),
         "consecutive unreachable statements should retain an isolated scope "
         "for diagnostics and tooling");
  expect(countDiagnosticCode(unreachableDeclarations.diagnostics,
                             "GTI-S2095") == 1,
         "one warning should cover a consecutive unreachable statement tail");
}

void testIndexedPlaceMovesAndInitialization() {
  const lang::PlaceDomain domain{.snapshot = 1, .body = 1, .revision = 0};
  const lang::PlaceKey array{.domain = domain, .root = 1};
  lang::PlaceKey firstElement = array;
  firstElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::ConstantIndex, .index = 0});
  lang::PlaceKey secondElement = array;
  secondElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::ConstantIndex, .index = 1});
  const lang::PlaceKey otherRoot{.domain = domain, .root = 2};
  lang::PlaceKey leftField = array;
  leftField.projections.push_back(
      {.kind = lang::PlaceProjectionKind::Field, .field = 3});
  lang::PlaceKey rightField = array;
  rightField.projections.push_back(
      {.kind = lang::PlaceProjectionKind::Field, .field = 4});
  lang::PlaceKey dynamicElement = array;
  dynamicElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::DynamicIndex, .selection = 1});
  lang::PlaceKey otherDynamicElement = array;
  otherDynamicElement.projections.push_back(
      {.kind = lang::PlaceProjectionKind::DynamicIndex, .selection = 2});
  lang::PlaceKey otherDomain = firstElement;
  otherDomain.domain.body = 2;
  expect(
      lang::placeRelation(firstElement, firstElement).relation ==
              lang::PlaceRelation::Equal &&
          lang::placeRelation(array, firstElement).relation ==
              lang::PlaceRelation::LeftStrictPrefix &&
          lang::placeRelation(firstElement, array).relation ==
              lang::PlaceRelation::RightStrictPrefix &&
          lang::placeRelation(firstElement, secondElement).relation ==
              lang::PlaceRelation::Disjoint &&
          lang::placeRelation(array, otherRoot).relation ==
              lang::PlaceRelation::Disjoint &&
          lang::placeRelation(leftField, rightField).relation ==
              lang::PlaceRelation::Disjoint &&
          lang::placeRelation(dynamicElement, firstElement).relation ==
              lang::PlaceRelation::MayAlias &&
          lang::placeRelation(dynamicElement, dynamicElement).relation ==
              lang::PlaceRelation::Equal &&
          lang::placeRelation(dynamicElement, otherDynamicElement).relation ==
              lang::PlaceRelation::MayAlias &&
          !lang::placeRelation(firstElement, otherDomain).compatibleDomain,
      "the shared place relation should distinguish equality, prefixes, "
      "constant-index disjointness, dynamic may-alias, and domains");

  const lang::FrontendResult firstSnapshot =
      lang::Frontend().analyze("place-snapshot.gti", R"(
int main() { int value = 1; return value - 1; }
)");
  const lang::FrontendResult secondSnapshot =
      lang::Frontend().analyze("place-snapshot.gti", R"(
int main() { int value = 1; return value - 1; }
)");
  expect(firstSnapshot.canGenerateCode() && secondSnapshot.canGenerateCode() &&
             firstSnapshot.hir.module().placeDomain.snapshot != 0 &&
             firstSnapshot.hir.module().placeDomain.snapshot !=
                 secondSnapshot.hir.module().placeDomain.snapshot &&
             lang::MirPrinter().print(firstSnapshot.mir) ==
                 lang::MirPrinter().print(secondSnapshot.mir),
         "separate frontend snapshots should reject stale place identity "
         "while deterministic MIR printing normalizes the process-local "
         "generation");

  const lang::FrontendResult valid =
      analyzeWithMemory("indexed-place-moves.gti", R"(
struct Record {
  int value;
public:
  Record(int initial) : value(initial) {}
};

struct Holder {
  mut Record slots[2] = {Record(3), Record(4)};

  int rotate() mut {
    Record first = std::move(this.slots[0]);
    Record second = std::move(this.slots[1]);
    this.slots[0] = std::move(second);
    this.slots[1] = std::move(first);
    return this.slots[0].value + this.slots[1].value;
  }
public:
  Holder() = default;
};

int branch_restore(bool select) {
  mut Record values[1] = {Record(5)};
  if (select) {
    Record consumed = std::move(values[0]);
    values[0] = std::move(consumed);
  } else {
    Record consumed = std::move(values[0]);
    values[0] = std::move(consumed);
  }
  return values[0].value;
}

int loop_restore() {
  mut Record values[1] = {Record(6)};
  for (mut int index = 0; index < 2; index++) {
    Record consumed = std::move(values[0]);
    values[0] = std::move(consumed);
  }
  return values[0].value;
}

int partial_drop() {
  mut std::unique_ptr<Record> owners[2] = {
      std::make_unique<Record>(7), std::make_unique<Record>(8)};
  auto first = std::move(owners[0]);
  return first->value;
}

int main() {
  mut Record values[2] = {Record(1), Record(2)};
  Record first = std::move(values[1 - 1]);
  Record second = std::move(values[1]);
  values[0] = std::move(first);
  values[1] = std::move(second);
  Record moved[2] = std::move(values);

  mut Holder holder = Holder();
  return moved[0].value + moved[1].value + holder.rotate() +
         branch_restore(true) + loop_restore() + partial_drop() - 26;
}
)");
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected indexed-place diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "constant fixed-array elements should support disjoint moves, exact "
         "restoration, branch joins, loop backedges, field arrays, and "
         "partially moved drop boundaries");

  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const lang::HirFunctionInstance *hirMain = nullptr;
  const lang::MirFunctionInstance *mirMain = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == main) {
      hirMain = &instance;
      break;
    }
  }
  for (const lang::MirFunctionInstance &instance :
       valid.mir.functionInstances()) {
    if (hirMain != nullptr && instance.id == hirMain->id) {
      mirMain = &instance;
      break;
    }
  }

  const lang::HirValue *indexedMove = nullptr;
  const lang::HirValue *indexedRestore = nullptr;
  if (hirMain != nullptr) {
    for (const lang::HirValue &value : hirMain->body.values) {
      if (!value.ownership || value.ownership->place.projections.empty()) {
        continue;
      }
      const lang::PlaceProjection &projection =
          value.ownership->place.projections.back();
      if (projection.kind != lang::PlaceProjectionKind::ConstantIndex) {
        continue;
      }
      if (value.ownership->kind == lang::OwnershipEventKind::Move &&
          indexedMove == nullptr) {
        indexedMove = &value;
      } else if (value.ownership->kind ==
                     lang::OwnershipEventKind::Reinitialize &&
                 indexedRestore == nullptr) {
        indexedRestore = &value;
      }
    }
  }
  const lang::OwnershipEvent *semanticIndexedMove =
      indexedMove == nullptr || indexedMove->source == nullptr
          ? nullptr
          : valid.semantics.findOwnershipEvent(*indexedMove->source);
  expect(hirMain != nullptr && hirMain->body.placeDomain.body != 0 &&
             indexedMove != nullptr && indexedMove->place &&
             indexedMove->place == indexedMove->ownership->place &&
             semanticIndexedMove != nullptr &&
             semanticIndexedMove->place.domain.snapshot ==
                 valid.semantics.placeSnapshot() &&
             indexedMove->ownership->place.domain ==
                 hirMain->body.placeDomain &&
             indexedRestore != nullptr,
         "HIR should retain one body-qualified constant-index place and its "
         "move/reinitialization events");

  const lang::MirInstruction *mirIndexedMove = nullptr;
  const lang::MirInstruction *mirIndexedRestore = nullptr;
  if (mirMain != nullptr) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (!instruction.ownership ||
            instruction.ownership->place.projections.empty()) {
          continue;
        }
        if (instruction.ownership->kind == lang::OwnershipEventKind::Move &&
            mirIndexedMove == nullptr) {
          mirIndexedMove = &instruction;
        } else if (instruction.ownership->kind ==
                       lang::OwnershipEventKind::Reinitialize &&
                   mirIndexedRestore == nullptr) {
          mirIndexedRestore = &instruction;
        }
      }
    }
  }
  const lang::MirPlace *mirMovePlace =
      mirMain == nullptr || mirIndexedMove == nullptr ||
              mirIndexedMove->operands.empty()
          ? nullptr
          : mirMain->body.findPlace(mirIndexedMove->operands.front().place);
  expect(mirMain != nullptr && mirIndexedMove != nullptr &&
             mirIndexedRestore != nullptr && mirMovePlace != nullptr &&
             mirMovePlace->key == mirIndexedMove->ownership->place &&
             !mirMovePlace->projections.empty() &&
             mirMovePlace->projections.back().constantIndex == 0 &&
             lang::verifyMirBody(mirMain->body).valid(),
         "MIR should preserve the semantic constant index and validate its "
         "ownership-state transfer");
  const std::string indexedMirDump =
      mirMain == nullptr ? std::string{}
                         : lang::MirPrinter().print(mirMain->body);
  expect(indexedMirDump.starts_with("mir-body-v93\n") &&
             indexedMirDump.find(" domain=1:") != std::string::npos &&
             indexedMirDump.find(";constant=0;selection=0") !=
                 std::string::npos &&
             indexedMirDump.find(" ownership=event(") != std::string::npos,
         "the deterministic MIR snapshot should expose place domains, "
         "constant indices, and ownership events");

  if (mirMain != nullptr && mirIndexedMove != nullptr &&
      mirIndexedRestore != nullptr) {
    lang::MirBody mismatchedPlace = mirMain->body;
    bool changedMove = false;
    for (lang::MirBlock &block : mismatchedPlace.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == mirIndexedMove->id && instruction.ownership) {
          instruction.ownership->place.projections.back().index = 99;
          changedMove = true;
        }
      }
    }
    expect(changedMove && !lang::verifyMirBody(mismatchedPlace).valid(),
           "the MIR verifier should reject a forged ownership-event place");

    lang::MirBody missingRestore = mirMain->body;
    bool changedRestore = false;
    for (lang::MirBlock &block : missingRestore.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == mirIndexedRestore->id && instruction.ownership) {
          instruction.ownership->reachable = false;
          changedRestore = true;
        }
      }
    }
    expect(changedRestore && !lang::verifyMirBody(missingRestore).valid(),
           "the MIR fixed point should reject a forged missing element "
           "restoration before whole-owner movement");

    const lang::SymbolId arrayRoot = mirIndexedMove->ownership->place.root;
    lang::MirBody missingInitialization = mirMain->body;
    bool changedInitialization = false;
    for (lang::MirBlock &block : missingInitialization.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.destination) {
          continue;
        }
        const lang::MirPlace *destination =
            missingInitialization.findPlace(*instruction.destination);
        if (destination != nullptr && destination->key &&
            destination->key->root == arrayRoot &&
            destination->key->projections.empty()) {
          instruction.kind = lang::MirInstructionKind::Modify;
          instruction.operation = lang::MirOperation::AddAssign;
          changedInitialization = true;
          break;
        }
      }
      if (changedInitialization) {
        break;
      }
    }
    expect(changedInitialization &&
               !lang::verifyMirBody(missingInitialization).valid(),
           "the MIR fixed point should reject access before a local place is "
           "definitely initialized");

    lang::MirBody doubleInitialization = mirMain->body;
    bool changedDrop = false;
    for (lang::MirBlock &block : doubleInitialization.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            !instruction.destination) {
          continue;
        }
        const lang::MirPlace *destination =
            doubleInitialization.findPlace(*instruction.destination);
        if (destination != nullptr && destination->key &&
            destination->key->root == arrayRoot &&
            destination->key->projections.empty()) {
          instruction.kind = lang::MirInstructionKind::Initialize;
          changedDrop = true;
          break;
        }
      }
      if (changedDrop) {
        break;
      }
    }
    expect(changedDrop && !lang::verifyMirBody(doubleInitialization).valid(),
           "the MIR fixed point should reject initialization while a local "
           "place lifetime is still active");
  }

  const lang::FrontendResult partialOwner = lang::Frontend().analyze(
      "partial-index-owner.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int main() {
  mut Record values[2] = {Record(1), Record(2)};
  Record consumed = std::move(values[0]);
  Record moved[2] = std::move(values);
  return consumed.value + moved[1].value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!partialOwner.canGenerateCode() &&
             hasDiagnostic(partialOwner.diagnostics,
                           "one of its owned places remains moved"),
         "whole-array movement should be rejected while one constant element "
         "is moved");

  const std::string dynamicMoveSource = R"(
int main() {
  mut int values[2] = {1, 2};
  mut int index = 0;
  int consumed = std::move(values[index]);
  return consumed;
}
int after() { return 0; }
)";
  const lang::FrontendResult dynamicMove = lang::Frontend().analyze(
      "dynamic-index-move.gti", dynamicMoveSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  const auto dynamicMoveDiagnostic = std::find_if(
      dynamicMove.diagnostics.begin(), dynamicMove.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2018" &&
               diagnostic.message.find(
                   "requires an in-range constant fixed-array "
                   "index") != std::string::npos;
      });
  const std::size_t dynamicBracket = dynamicMoveSource.find("[index]");
  expect(!dynamicMove.canGenerateCode() &&
             dynamicMoveDiagnostic != dynamicMove.diagnostics.end() &&
             countDiagnosticCode(dynamicMove.diagnostics, "GTI-S2018") == 1 &&
             dynamicMoveDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             std::filesystem::path(dynamicMoveDiagnostic->primary.source)
                     .filename() == "dynamic-index-move.gti" &&
             dynamicMoveDiagnostic->primary.start == dynamicBracket &&
             dynamicMoveDiagnostic->primary.end == dynamicBracket + 1 &&
             dynamicMoveDiagnostic->fixes.empty() &&
             findTopLevelFunction(dynamicMove.program, "after") != nullptr,
         "dynamic indexes should diagnose their exact bracket while "
         "preserving later declarations and disabling code generation");

  const lang::FrontendResult branchMove = lang::Frontend().analyze(
      "branch-index-move.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int read(bool select) {
  mut Record values[1] = {Record(1)};
  if (select) {
    Record consumed = std::move(values[0]);
  }
  return values[0].value;
}
int main() { return read(false) - 1; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!branchMove.canGenerateCode() &&
             hasDiagnostic(branchMove.diagnostics,
                           "may have been moved on another control-flow path"),
         "branch joins should retain maybe-moved element state");

  const lang::FrontendResult loopMove = lang::Frontend().analyze(
      "loop-index-move.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int main() {
  mut Record values[1] = {Record(1)};
  mut int index = 0;
  while (index < 2) {
    Record consumed = std::move(values[0]);
    index++;
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!loopMove.canGenerateCode() &&
             hasDiagnostic(loopMove.diagnostics,
                           "not available at a reachable loop backedge"),
         "loop backedges should require an exact moved element to be "
         "restored");

  const lang::FrontendResult dynamicRead = lang::Frontend().analyze(
      "dynamic-index-after-move.gti", R"(
struct Record { int value; Record(int initial) : value(initial) {} };
int read(mut int index) {
  mut Record values[2] = {Record(1), Record(2)};
  Record consumed = std::move(values[0]);
  return values[index].value + consumed.value;
}
int main() { return read(1) - 3; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!dynamicRead.canGenerateCode() &&
             hasDiagnostic(dynamicRead.diagnostics, "has already been moved"),
         "a dynamic index should may-alias an unavailable constant element");
}

void testExclusiveReborrowLoanGraph() {
  const std::string exclusiveSource = R"(
struct Pair {
public:
  mut int left = 1;
  mut int right = 2;
public:
  Pair() = default;
};

void change(mut int& value) { value += 1; }
int observe(int& value) { return value; }
void combine(int& observed, mut int& changed) { changed += observed; }

int endpoint_chain() {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  child += 1;
  return value;
}

int switch_reactivation(int selected) {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  switch (child) {
  case 0:
    parent += 1;
    break;
  default:
    parent += 2;
    break;
  }
  return value;
}

int switch_unmatched_reactivation(int selected) {
  mut int value = selected;
  mut int& parent = value;
  mut int& child = parent;
  switch (child) {
  case 0:
    parent += 1;
    break;
  }
  parent += 1;
  return value;
}

class CheckedOwner {
  mut int value;

public:
  CheckedOwner(int initial) : value(initial) {}
  mut int& operator*() mut { return this.value; }
};

int checked_dereference_reactivation() {
  mut CheckedOwner owner = CheckedOwner(1);
  mut int& parent = *owner;
  mut int& child = parent;
  child += 1;
  parent += 1;
  return *owner - 3;
}

int main() {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  mut int& grandchild = child;
  grandchild += 1;

  mut int& sequential = parent;
  sequential += 1;
  parent += 1;

  mut Pair pair = Pair();
  mut int& left = pair.left;
  mut int& right = pair.right;
  left += 1;
  right += 1;
  combine(left, right);

  mut Pair second_pair = Pair();
  mut Pair& pair_parent = second_pair;
  mut int& projected_child = pair_parent.left;
  pair_parent.right += 1;
  projected_child += 1;

  int& shared = value;
  int& shared_alias = shared;
  return observe(shared_alias) + pair.left + pair.right - 10 +
         checked_dereference_reactivation();
}
)";
  const lang::FrontendResult exclusive =
      lang::Frontend().analyze("exclusive-reborrow.gti", exclusiveSource);
  expect(exclusive.syntaxValid,
         "exclusive reborrow coverage should remain valid GTI syntax");
  const bool valid = exclusive.semanticValid;
  if (!valid) {
    for (const lang::Diagnostic &diagnostic : exclusive.diagnostics) {
      std::cerr << "Unexpected exclusive-reborrow diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid,
         "nested and sequential exclusive reborrows, shared aliases, and "
         "disjoint field loans should validate");

  const lang::FunctionDecl *mainFunction =
      findTopLevelFunction(exclusive.program, "main");
  const auto local = [&](std::size_t index) -> const lang::VariableDecl * {
    return mainFunction == nullptr
               ? nullptr
               : dynamic_cast<const lang::VariableDecl *>(
                     mainFunction->body()->statements().at(index).get());
  };
  const lang::VariableDecl *parent = local(1);
  const lang::VariableDecl *child = local(2);
  const lang::VariableDecl *grandchild = local(3);
  const lang::VariableDecl *left = local(9);
  const lang::VariableDecl *right = local(10);
  const lang::VariableDecl *pairParent = local(15);
  const lang::VariableDecl *projectedChild = local(16);
  const lang::VariableDecl *shared = local(19);
  const lang::VariableDecl *sharedAlias = local(20);
  const MirRetained parentLoan = mirRetained(exclusive, parent);
  const MirRetained childLoan = mirRetained(exclusive, child);
  const MirRetained grandchildLoan = mirRetained(exclusive, grandchild);
  const MirRetained leftLoan = mirRetained(exclusive, left);
  const MirRetained rightLoan = mirRetained(exclusive, right);
  const MirRetained pairParentLoan = mirRetained(exclusive, pairParent);
  const MirRetained projectedChildLoan = mirRetained(exclusive, projectedChild);
  const MirRetained sharedLoan = mirRetained(exclusive, shared);
  const MirRetained sharedAliasLoan = mirRetained(exclusive, sharedAlias);
  // A reborrow's source is its parent loan's place, with no projection.
  const auto reborrowsWhole = [](const MirRetained &reborrow,
                                 const MirRetained &from) {
    const lang::MirPlace *source = mirLoanSource(reborrow);
    return reborrow.loan != nullptr && from.loan != nullptr &&
           reborrow.loan != from.loan &&
           reborrow.loan->parent == from.loan->id && source != nullptr &&
           source->root == lang::MirPlaceRootKind::Loan &&
           source->loan == from.loan->id && source->projections.empty() &&
           reborrow.loan->access == lang::AccessMode::Mutable;
  };
  const lang::MirPlace *parentSource = mirLoanSource(parentLoan);
  expect(parentLoan.loan != nullptr &&
             parentLoan.loan->access == lang::AccessMode::Mutable &&
             parentSource != nullptr &&
             parentSource->root == lang::MirPlaceRootKind::Binding &&
             reborrowsWhole(childLoan, parentLoan) &&
             reborrowsWhole(grandchildLoan, childLoan) &&
             !childLoan.loan->carriers.empty() &&
             !grandchildLoan.loan->carriers.empty(),
         "mutable reborrows should form distinct parent-linked MIR loans over "
         "one stable place, each with its body-local carrier");
  const lang::MirPlace *leftSource = mirLoanSource(leftLoan);
  const lang::MirPlace *rightSource = mirLoanSource(rightLoan);
  expect(leftSource != nullptr && rightSource != nullptr &&
             leftSource->root == lang::MirPlaceRootKind::Binding &&
             leftSource->binding == rightSource->binding &&
             leftSource->projections.size() == 1 &&
             rightSource->projections.size() == 1 &&
             leftSource->projections.front().kind ==
                 lang::MirProjectionKind::Field &&
             rightSource->projections.front().kind ==
                 lang::MirProjectionKind::Field &&
             leftSource->projections.front().field !=
                 rightSource->projections.front().field,
         "named sibling fields should retain disjoint stable projections");
  const lang::MirPlace *projectedSource = mirLoanSource(projectedChildLoan);
  expect(pairParentLoan.loan != nullptr && projectedChildLoan.loan != nullptr &&
             projectedChildLoan.loan->parent == pairParentLoan.loan->id &&
             projectedSource != nullptr &&
             projectedSource->root == lang::MirPlaceRootKind::Loan &&
             projectedSource->loan == pairParentLoan.loan->id &&
             projectedSource->projections.size() == 1 &&
             projectedSource->projections.front().kind ==
                 lang::MirProjectionKind::Field,
         "a projected child should suspend only its overlapping parent place, "
         "not a known-disjoint sibling field");

  const lang::FunctionDecl *checkedDereferenceFunction = findTopLevelFunction(
      exclusive.program, "checked_dereference_reactivation");
  const auto checkedLocal =
      [&](std::size_t index) -> const lang::VariableDecl * {
    return checkedDereferenceFunction == nullptr
               ? nullptr
               : dynamic_cast<const lang::VariableDecl *>(
                     checkedDereferenceFunction->body()
                         ->statements()
                         .at(index)
                         .get());
  };
  const MirRetained checkedOwner = mirRetained(exclusive, checkedLocal(0));
  const MirRetained checkedParentLoan = mirRetained(exclusive, checkedLocal(1));
  const MirRetained checkedChildLoan = mirRetained(exclusive, checkedLocal(2));
  const lang::MirPlace *checkedParentSource = mirLoanSource(checkedParentLoan);
  expect(checkedParentLoan.loan != nullptr &&
             checkedParentLoan.loan->kind == lang::MirLoanKind::CallResult &&
             checkedParentLoan.loan->access == lang::AccessMode::Mutable &&
             checkedParentSource != nullptr &&
             checkedParentSource->root == lang::MirPlaceRootKind::Binding &&
             checkedParentSource->binding == checkedOwner.binding &&
             checkedParentSource->projections.empty() &&
             reborrowsWhole(checkedChildLoan, checkedParentLoan),
         "a checked owner dereference should retain its exact owner place "
         "through exclusive reborrow analysis");
  expect(sharedLoan.loan != nullptr &&
             sharedLoan.loan == sharedAliasLoan.loan &&
             sharedLoan.loan->access == lang::AccessMode::ReadOnly &&
             sharedLoan.loan->carriers.size() == 2,
         "shared read-only aliases should continue sharing one MIR loan");

  const lang::FrontendResult transientResults =
      lang::Frontend().analyze("transient-call-results.gti", R"(
class Box {
  mut int value;

public:
  Box(int initial) : value(initial) {}
  int& read() { return this.value; }
  mut int& write() mut { return this.value; }
};

class Boxes {
public:
  mut Box left = Box(1);
  mut Box right = Box(2);
public:
  Boxes() = default;
};

int sum(int& left, int& right) { return left + right; }

int disjoint_writes(mut Boxes& parent) {
  return sum(parent.left.write(), parent.right.write());
}

int main() {
  mut Boxes parent = Boxes();
  int shared_reads = sum(parent.left.read(), parent.left.read());
  int disjoint = disjoint_writes(parent);
  return shared_reads + disjoint - 5;
}
)");
  expect(transientResults.canGenerateCode(),
         "read-only call results may overlap and mutable call results may "
         "remain live across one outer call when their places are disjoint");

  const lang::FrontendResult retainedProjected =
      lang::Frontend().analyze("retained-projected-results.gti", R"(
class Cell {
public:
  mut int value = 1;
  mut int& write() mut { return this.value; }
public:
  Cell() = default;
};
class Pair {
public:
  mut Cell left = Cell();
  mut int right = 2;
public:
  Pair() = default;
};
int& relay(int& source) { return source; }
int receiver_projection(mut Pair& parent) {
  mut int& child = parent.left.write();
  parent.right += 1;
  child += 1;
  return child + parent.right;
}
int helper_projection(mut Pair& parent) {
  int& child = relay(parent.right);
  int observed = child;
  parent.left.value += 1;
  return observed + parent.left.value;
}
int main() {
  mut Pair pair = Pair();
  return receiver_projection(pair) + helper_projection(pair) - 10;
}
)");
  expect(retainedProjected.canGenerateCode(),
         "retained receiver- and argument-origin results should preserve "
         "their exact caller-visible projected places");
  for (const std::string name : {"receiver_projection", "helper_projection"}) {
    const lang::FunctionDecl *function =
        findTopLevelFunction(retainedProjected.program, name);
    const auto *child = function == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::VariableDecl *>(
                                  function->body()->statements().front().get());
    // The retained call result keeps the caller-visible place below the
    // reference parameter: one dereference and one exact field.
    const MirRetained retained = mirRetained(retainedProjected, child);
    const lang::MirPlace *source = mirLoanSource(retained);
    const lang::MirLoan *entry = mirLoanParent(retained);
    expect(retained.loan != nullptr &&
               retained.loan->kind == lang::MirLoanKind::CallResult &&
               entry != nullptr && entry->entry && source != nullptr &&
               source->root == lang::MirPlaceRootKind::Binding &&
               source->projections.size() == 2 &&
               source->projections[0].kind ==
                   lang::MirProjectionKind::Dereference &&
               source->projections[1].kind == lang::MirProjectionKind::Field,
           "MIR retained loans should preserve one exact field projection "
           "under their entry parent in " +
               name);
  }

  const lang::FrontendResult storedMutableParent =
      lang::Frontend().analyze("stored-mutable-parent.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int direct_parent(mut Box& parent) {
  View retained = View(parent.read());
  return retained.get();
}
int child_parent(mut Box& parent) {
  int& child = parent.read();
  View retained = View(child);
  return retained.get();
}
int main() { return 0; }
)");
  expect(storedMutableParent.canGenerateCode(),
         "single-path shared carriers retain a checked child of mutable "
         "parents and their read-only reborrows");
  bool checkedSharedAccessor = false;
  for (const auto &function : storedMutableParent.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *parent = function.body.findLoan(loan.parent);
      if (loan.kind != lang::MirLoanKind::Stored || parent == nullptr ||
          parent->kind != lang::MirLoanKind::CallResult ||
          parent->access != lang::AccessMode::ReadOnly || parent->parent == 0)
        continue;
      auto missingParent = storedMutableParent.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          missingParent.findFunctionInstance(function.id));
      changed->body.loans[loan.id - 1].parent = 0;
      expect(!lang::verifyMirProgram(missingParent).valid(),
             "a shared stored accessor cannot lose its suspended-owner chain");
      checkedSharedAccessor = true;
    }
  }
  expect(checkedSharedAccessor,
         "shared stored accessor exercises an exact parent chain");
  for (const std::string mutation :
       {"parent.value = 99;", "mut Box& alias = parent; alias.value = 99;"}) {
    const auto invalid =
        lang::Frontend().analyze("invalid-shared-stored-parent.gti",
                                 R"(
class Box { public: mut int value = 2; int& read() { return this.value; } };
class View { int& value; public: View(int& source) : value(source) {}
  int get() { return this.value; } };
int bad(mut Box& parent) { View retained = View(parent.read());
)" + mutation + " return retained.get(); }\nint main() { return 0; }\n");
    expect(!invalid.semanticValid, "shared stored accessor forbids mutable "
                                   "owner/reference invalidation: " +
                                       mutation);
  }

  const lang::FrontendResult ordinaryStoredDependency =
      lang::Frontend().analyze("ordinary-stored-dependency.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  void bump() mut { this.value += 1; }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int main() {
  mut Box owner = Box();
  mut int observed = 0;
  {
    View retained = View(owner.read());
    observed = retained.get();
  }
  owner.bump();
  return observed + owner.value - 5;
}
)");
  expect(ordinaryStoredDependency.canGenerateCode(),
         "ordinary read-only owner dependencies should remain valid for a "
         "lexically bounded stored carrier");

  const lang::FrontendResult mutableTransientConflict =
      lang::Frontend().analyze("mutable-transient-call-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  mut int& write() mut { return this.value; }
public:
  Box() = default;
};
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Box parent = Box();
  return sum(parent.write(), parent.write());
}
)");
  expect(
      !mutableTransientConflict.canGenerateCode() &&
          countDiagnosticCode(mutableTransientConflict.diagnostics,
                              "GTI-S2017") == 1 &&
          hasDiagnostic(mutableTransientConflict.diagnostics,
                        "Call creates overlapping reference accesses") &&
          !hasDiagnosticCode(mutableTransientConflict.diagnostics, "GTI-B0001"),
      "overlapping mutable reference call results should produce one "
      "outer-call exclusivity diagnostic");

  const lang::FrontendResult mutatingSharedTransientConflict =
      lang::Frontend().analyze("mutating-shared-call-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& write() mut { return this.value; }
public:
  Box() = default;
};
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Box parent = Box();
  return sum(parent.write(), parent.write());
}
)");
  expect(!mutatingSharedTransientConflict.canGenerateCode() &&
             countDiagnosticCode(mutatingSharedTransientConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(mutatingSharedTransientConflict.diagnostics,
                           "Call creates overlapping reference accesses") &&
             !hasDiagnosticCode(mutatingSharedTransientConflict.diagnostics,
                                "GTI-B0001"),
         "a mutable producer should retain exclusive evaluation access even "
         "when its returned reference is read-only");

  const std::string resultThenMutationSource = R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
int combine(int& borrowed, int changed) { return borrowed + changed; }
int main() {
  mut Box parent = Box();
  return combine(parent.read(), parent.bump());
}
)";
  const lang::FrontendResult resultThenMutationConflict =
      lang::Frontend().analyze("result-then-mutation-conflict.gti",
                               resultThenMutationSource);
  expect(!resultThenMutationConflict.canGenerateCode() &&
             countDiagnosticCode(resultThenMutationConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(resultThenMutationConflict.diagnostics,
                           "call operand mutates storage borrowed") &&
             !hasDiagnosticCode(resultThenMutationConflict.diagnostics,
                                "GTI-B0001"),
         "a later mutable receiver evaluation should not invalidate an "
         "earlier retained call result");
  const auto conflict =
      std::find_if(resultThenMutationConflict.diagnostics.begin(),
                   resultThenMutationConflict.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2017";
                   });
  expect(conflict != resultThenMutationConflict.diagnostics.end() &&
             conflict->primary.start ==
                 resultThenMutationSource.rfind("bump()") &&
             conflict->primary.end == conflict->primary.start + 4 &&
             conflict->related.size() == 1 &&
             conflict->related.front().span.start ==
                 resultThenMutationSource.rfind("read()") &&
             !conflict->hints.empty() && conflict->fixes.empty(),
         "the conflict should identify the later mutation and earlier borrow "
         "without offering an unsafe automatic reorder");

  const lang::FrontendResult resultThenMutableArgumentConflict =
      lang::Frontend().analyze("result-then-mut-argument-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  void bump() mut { this.value += 1; }
public:
  Box() = default;
};
int mutate(mut Box& box) {
  box.bump();
  return 0;
}
int combine(int& borrowed, int changed) { return borrowed + changed; }
int main() {
  mut Box parent = Box();
  return combine(parent.read(), mutate(parent));
}
)");
  expect(!resultThenMutableArgumentConflict.canGenerateCode() &&
             countDiagnosticCode(resultThenMutableArgumentConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(resultThenMutableArgumentConflict.diagnostics,
                           "later call operand mutates storage borrowed") &&
             !hasDiagnosticCode(resultThenMutableArgumentConflict.diagnostics,
                                "GTI-B0001"),
         "a later mutable-reference helper evaluation should not invalidate "
         "an earlier retained call result");

  const lang::FrontendResult mutationThenResult =
      lang::Frontend().analyze("mutation-then-result.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
int combine(int changed, int& borrowed) { return changed + borrowed; }
int main() {
  mut Box parent = Box();
  return combine(parent.bump(), parent.read());
}
)");
  expect(mutationThenResult.canGenerateCode() &&
             lang::verifyMirProgram(mutationThenResult.mir).valid(),
         "a completed earlier mutation may precede an overlapping borrow "
         "through the ordinary verified ordered-call schedule");
  bool provedBorrowOrder = false;
  bool reorderedInputRejected = false;
  lang::MirProgram reordered = mutationThenResult.mir;
  auto &orderedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      reordered.functionInstances());
  for (auto &function : orderedFunctions) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    auto &body = function.body;
    const auto dominance = lang::computeMirDominance(body);
    struct Position {
      lang::MirBlockId block = 0;
      std::size_t index = 0;
    };
    Position mutation, producer, invocation, firstInput, secondInput;
    lang::HirValueId producerValue = 0;
    for (const auto &block : body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &instruction = block.instructions[index];
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.functionTarget) {
          continue;
        }
        const auto *target =
            reordered.findFunctionInstance(*instruction.functionTarget);
        if (target == nullptr) {
          continue;
        }
        if (target->receiverMutability == lang::ReceiverMutability::Mutable) {
          mutation = {block.id, index};
        } else if (instruction.borrowOrigin ==
                   lang::BorrowOriginKind::Receiver) {
          producer = {block.id, index};
          producerValue = instruction.hirValue;
        } else if (instruction.parameterTypes.size() == 2) {
          invocation = {block.id, index};
          for (const auto &inputBlock : body.blocks) {
            for (std::size_t inputIndex = 0;
                 inputIndex < inputBlock.instructions.size(); ++inputIndex) {
              const auto &input = inputBlock.instructions[inputIndex];
              if (input.kind != lang::MirInstructionKind::CallInput ||
                  input.callSite != instruction.callSite) {
                continue;
              }
              (input.callInputIndex == 0 ? firstInput : secondInput) = {
                  inputBlock.id, inputIndex};
            }
          }
        }
      }
    }
    const auto precedes = [&](Position left, Position right) {
      return dominance && left.block != 0 && right.block != 0 &&
             (left.block == right.block
                  ? left.index < right.index
                  : dominance->dominates(left.block, right.block));
    };
    const bool hasExactLoan = std::any_of(
        body.loans.begin(), body.loans.end(), [&](const lang::MirLoan &loan) {
          return loan.producedBy == producerValue && producerValue != 0 &&
                 loan.source != 0 && loan.access == lang::AccessMode::ReadOnly;
        });
    provedBorrowOrder = precedes(mutation, firstInput) &&
                        precedes(firstInput, producer) &&
                        precedes(producer, secondInput) &&
                        precedes(secondInput, invocation) && hasExactLoan;
    if (!provedBorrowOrder) {
      continue;
    }
    // Preserve identities, types, uses and the first input's dominating value;
    // only reverse the outer call's input schedule across the borrow producer.
    auto &firstBlock = *body.findBlock(firstInput.block);
    auto movedInput = firstBlock.instructions[firstInput.index];
    firstBlock.instructions.erase(firstBlock.instructions.begin() +
                                  firstInput.index);
    if (firstInput.block == secondInput.block &&
        firstInput.index < secondInput.index) {
      --secondInput.index;
    }
    auto &secondBlock = *body.findBlock(secondInput.block);
    secondBlock.instructions.insert(
        secondBlock.instructions.begin() + secondInput.index + 1, movedInput);
    for (auto &value : body.values) {
      if (value.definition == movedInput.id) {
        value.definitionBlock = secondBlock.id;
      }
    }
    reorderedInputRejected = lang::rebuildMirValueUses(body) &&
                             !lang::verifyMirProgram(reordered).valid();
  }
  expect(provedBorrowOrder && reorderedInputRejected,
         "MIR must create the lookup loan only after mutation completes, and "
         "reject forged ordered-input reordering");

  const auto borrowedCarrierInput =
      lang::Frontend().analyze("ordered-borrowed-carrier-input.gti", R"(
class Box {
  mut int value = 1;
public:
  int bump() mut { this.value += 1; return this.value; }
  int& read() { return this.value; }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
View lookup(Box& box) { return View(box.read()); }
int inspect(int changed, View view) { return changed + view.get(); }
int main() {
  mut Box box{};
  return inspect(box.bump(), lookup(box));
}
)");
  expect(borrowedCarrierInput.canGenerateCode() &&
             lang::verifyMirProgram(borrowedCarrierInput.mir).valid(),
         "ordinary calls should share the exact borrowed-carrier input "
         "contract with constructors");
  const auto rejectsCarrierMutation = [&](bool eraseProducerLoan) {
    lang::MirProgram invalid = borrowedCarrierInput.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        invalid.functionInstances());
    for (auto &function : functions) {
      if (function.entryKind == lang::ProgramEntryKind::None) {
        continue;
      }
      auto &body = function.body;
      for (auto &block : body.blocks) {
        for (auto &stage : block.instructions) {
          if (stage.kind != lang::MirInstructionKind::CallInput ||
              stage.callInputKind !=
                  lang::HirCallInputKind::MoveBorrowedTemporary ||
              stage.operands.size() != 1 || !stage.preparedParameterDrop) {
            continue;
          }
          const auto *source = body.findValue(stage.operands.front().value);
          if (source == nullptr) {
            return false;
          }
          if (!eraseProducerLoan) {
            stage.lifecycle.clear();
            return !lang::verifyMirProgram(invalid).valid();
          }
          for (auto &producerBlock : body.blocks) {
            for (auto &producer : producerBlock.instructions) {
              if (producer.id == source->definition && producer.loan) {
                producer.loan.reset();
                return !lang::verifyMirProgram(invalid).valid();
              }
            }
          }
        }
      }
    }
    return false;
  };
  expect(rejectsCarrierMutation(true) && rejectsCarrierMutation(false),
         "ordinary borrowed-carrier inputs must fail closed when the exact "
         "producer loan or unique ownership handoff is erased");

  const lang::FrontendResult nestedTransientConflict =
      lang::Frontend().analyze("nested-transient-call-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  int& expose() mut { return this.value; }
public:
  Box() = default;
};
int& observe(int& value) { return value; }
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Box parent = Box();
  return sum(parent.read(), observe(parent.expose()));
}
)");
  expect(
      !nestedTransientConflict.canGenerateCode() &&
          countDiagnosticCode(nestedTransientConflict.diagnostics,
                              "GTI-S2017") == 1 &&
          hasDiagnostic(nestedTransientConflict.diagnostics,
                        "Call creates overlapping reference accesses") &&
          !hasDiagnosticCode(nestedTransientConflict.diagnostics, "GTI-B0001"),
      "nested borrow-origin wrappers should preserve a mutable producer's "
      "full-expression access");

  const lang::FrontendResult constructorTransientConflict =
      lang::Frontend().analyze("constructor-transient-conflict.gti", R"(
class Box {
  mut int value = 0;

public:
  int& expose() mut { return this.value; }
public:
  Box() = default;
};
class Inspector {
public:
  Inspector(int& first, int& second) {}
public:
  Inspector() = default;
};
int main() {
  mut Box parent = Box();
  Inspector inspector = Inspector(parent.expose(), parent.expose());
  return 0;
}
)");
  expect(!constructorTransientConflict.canGenerateCode() &&
             countDiagnosticCode(constructorTransientConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(constructorTransientConflict.diagnostics,
                           "Call creates overlapping reference accesses") &&
             !hasDiagnosticCode(constructorTransientConflict.diagnostics,
                                "GTI-B0001"),
         "constructor arguments should use recursive transient-production "
         "access when validating aliases");

  const lang::FrontendResult conservativeReceiverConflict =
      lang::Frontend().analyze("conservative-receiver-result.gti", R"(
class Box {
  mut int value = 0;

public:
  int& read() { return this.value; }
  mut int& write() mut { return this.value; }
public:
  Box() = default;
};
class Boxes {
public:
  mut Box left = Box();
  mut Box right = Box();
  int& read_left() { return this.left.read(); }
public:
  Boxes() = default;
};
int sum(int& left, int& right) { return left + right; }
int main() {
  mut Boxes parent = Boxes();
  return sum(parent.read_left(), parent.right.write());
}
)");
  expect(!conservativeReceiverConflict.canGenerateCode() &&
             countDiagnosticCode(conservativeReceiverConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(conservativeReceiverConflict.diagnostics,
                           "Call creates overlapping reference accesses") &&
             !hasDiagnosticCode(conservativeReceiverConflict.diagnostics,
                                "GTI-B0001"),
         "a receiver-derived result should conservatively protect its whole "
         "selected receiver rather than infer a callee-internal field");

  const lang::FrontendResult nestedMutationSurfaces =
      lang::Frontend().analyze("nested-call-mutation-surfaces.gti", R"(
class Box {
public:
  mut int value = 2;

  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
int combine(int& observed, int changed) { return observed + changed; }
int binary_case(mut Box& parent) {
  return combine(parent.read(), 1 + parent.bump());
}
int conditional_case(mut Box& parent, bool choose) {
  return combine(parent.read(), choose ? parent.bump() : 0);
}
int assignment_case(mut Box& parent) {
  return combine(parent.read(), parent.value += 1);
}
int postfix_case(mut Box& parent) {
  return combine(parent.read(), parent.value++);
}
int main() { return 0; }
)");
  expect(
      !nestedMutationSurfaces.canGenerateCode() &&
          countDiagnosticCode(nestedMutationSurfaces.diagnostics,
                              "GTI-S2017") == 4 &&
          !hasDiagnosticCode(nestedMutationSurfaces.diagnostics, "GTI-B0001"),
      "nested binary, conditional, assignment, and postfix mutations "
      "should be visible to full-call borrow validation");

  const lang::FrontendResult movedCallOperand = lang::Frontend().analyze(
      "moved-call-operand.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
public:
  Box() = default;
};
int consume(int& observed, Box moved) { return observed + moved.value; }
int run(mut Box parent) {
  return consume(parent.read(), std::move(parent));
}
int main() { return run(Box()) - 4; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!movedCallOperand.canGenerateCode() &&
             countDiagnosticCode(movedCallOperand.diagnostics, "GTI-S2017") ==
                 1 &&
             hasDiagnostic(movedCallOperand.diagnostics,
                           "call operand mutates storage borrowed") &&
             !hasDiagnosticCode(movedCallOperand.diagnostics, "GTI-B0001"),
         "std::move should count as call-operand invalidation while an "
         "overlapping result remains borrowed");

  const lang::FrontendResult storedCarrierConflicts =
      lang::Frontend().analyze("stored-carrier-call-conflicts.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int forward(View view, int changed) { return view.get() + changed; }
int reverse(int changed, View view) { return changed + view.get(); }
int forward_case(mut Box& parent) {
  return forward(View(parent.read()), parent.bump());
}
int reverse_case(mut Box& parent) {
  return reverse(parent.bump(), View(parent.read()));
}
int main() { return 0; }
)");
  expect(
      !storedCarrierConflicts.canGenerateCode() &&
          countDiagnosticCode(storedCarrierConflicts.diagnostics,
                              "GTI-S2017") == 1 &&
          !hasDiagnosticCode(storedCarrierConflicts.diagnostics, "GTI-B0001"),
      "a borrowed carrier blocks later mutation, not mutation completed before "
      "its construction");

  const lang::FrontendResult disjointStoredCarrier =
      lang::Frontend().analyze("disjoint-stored-carrier-call.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  int bump() mut {
    this.value += 1;
    return this.value;
  }
public:
  Box() = default;
};
class Pair {
public:
  mut Box left = Box();
  mut Box right = Box();
public:
  Pair() = default;
};
class View {
  int& value;
public:
  View(int& source) : value(source) {}
  int get() { return this.value; }
};
int consume(View view, int changed) { return view.get() + changed; }
int main() {
  mut Pair parent = Pair();
  return consume(View(parent.left.read()), parent.right.bump()) - 5;
}
)");
  expect(disjointStoredCarrier.canGenerateCode(),
         "borrowed-state carrier arguments should remain compatible with a "
         "mutation of a disjoint sibling place");

  const lang::FrontendResult transientReceiverConflict =
      lang::Frontend().analyze("transient-receiver-call-conflict.gti", R"(
class Inner {
public:
  mut int value = 1;
  int inspect(int changed) { return this.value + changed; }
public:
  Inner() = default;
};
class Outer {
public:
  mut Inner inner = Inner();
  mut int count = 1;
  Inner& get() { return this.inner; }
  int bump() mut {
    this.count += 1;
    return this.count;
  }
public:
  Outer() = default;
};
int run(mut Outer& parent) {
  return parent.get().inspect(parent.bump());
}
int main() { return 0; }
)");
  expect(!transientReceiverConflict.canGenerateCode() &&
             countDiagnosticCode(transientReceiverConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(transientReceiverConflict.diagnostics,
                           "call operand mutates storage borrowed") &&
             !hasDiagnosticCode(transientReceiverConflict.diagnostics,
                                "GTI-B0001"),
         "a transient borrowed receiver should conflict with overlapping "
         "argument mutation");

  const lang::FrontendResult disjointTransientReceiver =
      lang::Frontend().analyze("disjoint-transient-receiver.gti", R"(
class Inner {
public:
  mut int value = 1;
  int inspect(int changed) { return this.value + changed; }
public:
  Inner() = default;
};
class Side {
public:
  mut Inner inner = Inner();
  mut int count = 1;
  Inner& get() { return this.inner; }
  int bump() mut {
    this.count += 1;
    return this.count;
  }
public:
  Side() = default;
};
class Pair {
public:
  mut Side left = Side();
  mut Side right = Side();
public:
  Pair() = default;
};
int run(mut Pair& parent) {
  return parent.left.get().inspect(parent.right.bump());
}
int main() {
  mut Pair pair = Pair();
  return run(pair) - 3;
}
)");
  expect(disjointTransientReceiver.canGenerateCode(),
         "a transient borrowed receiver should remain compatible with a "
         "mutation of a disjoint sibling place");

  const lang::FrontendResult sameArgumentCommaConflict =
      lang::Frontend().analyze("same-argument-comma-conflict.gti", R"(
class Box {
public:
  mut int value = 2;
  int& read() { return this.value; }
  int& expose() mut { return this.value; }
public:
  Box() = default;
};
int inspect(int& value) { return value; }
int run(mut Box& parent) {
  return inspect((parent.read(), parent.expose()));
}
int main() { return 0; }
)");
  expect(!sameArgumentCommaConflict.canGenerateCode() &&
             countDiagnosticCode(sameArgumentCommaConflict.diagnostics,
                                 "GTI-S2017") == 1 &&
             hasDiagnostic(sameArgumentCommaConflict.diagnostics,
                           "comma expression creates overlapping transient") &&
             !hasDiagnosticCode(sameArgumentCommaConflict.diagnostics,
                                "GTI-B0001"),
         "comma operands should not create overlapping full-expression "
         "borrows within one outer argument");

  const auto rejects = [](std::string source, std::string_view expected) {
    const lang::FrontendResult invalid = lang::Frontend().analyze(
        "invalid-exclusive-reborrow.gti", std::move(source));
    return invalid.syntaxValid && !invalid.canGenerateCode() &&
           hasDiagnosticCode(invalid.diagnostics, "GTI-S2017") &&
           !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001") &&
           hasDiagnostic(invalid.diagnostics, std::string(expected));
  };
  expect(rejects(R"(
int inspect(int& value) { return value; }
void change(mut int& value) { value += 1; }
int main() {
  mut int value = 0;
  int& shared = value;
  mut int& exclusive = value;
  change(exclusive);
  return inspect(shared);
}
)",
                 "Cannot create a mutable borrow"),
         "a direct mutable loan should conflict with a later-live shared "
         "loan of the same ordinary local place");
  expect(rejects(R"(
void collide(int& observed, mut int& changed) { changed += observed; }
int main() {
  mut int value = 1;
  collide(value, value);
  return value;
}
)",
                 "Call creates overlapping reference accesses"),
         "one call should not create overlapping shared and mutable access to "
         "the same place");
  expect(rejects(R"(
class Box {
  mut int value = 0;

public:
  mut int& get() mut { return this.value; }
public:
  Box() = default;
};
void both(int& observed, mut int& changed) { changed += observed; }
int main() {
  mut Box box = Box();
  both(box.get(), box.get());
  return 0;
}
)",
                 "Call creates overlapping reference accesses"),
         "receiver-tied reference call results should retain their stable "
         "place at an outer call boundary");
  expect(rejects(R"(
class Cell {
  mut int value = 0;

  void combine(mut int& other) mut { other += 1; }

public:
  void invalid() mut { combine(this.value); }
public:
  Cell() = default;
};
int main() { return 0; }
)",
                 "Call creates overlapping reference accesses"),
         "an implicit mutable receiver should conflict with an overlapping "
         "mutable reference argument");
  expect(rejects(R"(
int main() {
  mut int value = 0;
  mut int& parent = value;
  mut int& child = parent;
  parent += 1;
  child += 1;
  return value;
}
)",
                 "child reborrow"),
         "a mutable parent should remain suspended until its child endpoint");
  expect(rejects(R"(
int main() {
  mut int value = 0;
  mut int& exclusive = value;
  int snapshot = value;
  exclusive += 1;
  return snapshot;
}
)",
                 "Cannot read storage"),
         "direct owner reads should conflict with a later-live mutable loan");
  expect(rejects(R"(
int main() {
  mut int value = 0;
  int& shared = value;
  mut int& upgraded = shared;
  upgraded += 1;
  return value;
}
)",
                 "cannot be upgraded"),
         "read-only loans should not be upgradeable to mutable children");
  expect(rejects(R"(
void invalid(mut int& parent) {
  mut int& child = parent;
  parent += 1;
  child += 1;
}
int main() { return 0; }
)",
                 "child reborrow"),
         "mutable reference parameters should enter with a parent loan that "
         "can be suspended by a local child");
  const auto accepts = [&](std::string source) {
    const auto result = lang::Frontend().analyze("returned-local-reborrow.gti",
                                                 std::move(source));
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << "Unexpected returned-reborrow diagnostic: "
                  << diagnostic.message << '\n';
    return result.canGenerateCode() &&
           lang::verifyMirProgram(result.mir).valid();
  };
  expect(
      accepts(R"(
class Cell {
  mut int value = 0;

public:
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return child;
  }
public:
  Cell() = default;
};
int main() { return 0; }
)"),
      "local mutable child loans should relay caller-owned receiver storage");
  expect(accepts(R"(
class GroupedCell {
  mut int value = 0;

public:
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return (child);
  }
public:
  GroupedCell() = default;
};

class CommaCell {
  mut int value = 0;

public:
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return (0, child);
  }
public:
  CommaCell() = default;
};
int main() { return 0; }
)"),
         "grouping and comma wrappers should preserve a returned child loan's "
         "caller-owned origin");
  expect(accepts(R"(
class Cell {
  mut int value = 0;

public:
  int& expose() mut {
    mut int& parent = this.value;
    int& child = parent;
    return child;
  }
public:
  Cell() = default;
};
int main() { return 0; }
)"),
         "a read-only child of a mutable local parent should return its "
         "receiver-owned referent without upgrading access");
  expect(accepts(R"(
class View {
  int& value;

public:
  View(int& source) : value(source) {}
  int& get() { return this.value; }
};

class Cell {
  mut int value = 0;

public:
  View expose() mut {
    mut int& parent = this.value;
    int& child = parent;
    return View(child);
  }
public:
  Cell() = default;
};
int main() { return 0; }
)"),
         "a child loan may return in a carrier with the same proven owner");
  const auto returnedChains =
      lang::Frontend().analyze("returned-reborrow-chains.gti", R"(
class Pair {
public:
  mut int left = 1;
  mut int right = 2;
  Pair() = default;
};
mut T& relay<T>(mut T& value) {
  mut T& parent = value;
  mut T& child = parent;
  mut T& grandchild = child;
  return grandchild;
}
int& read_relay(int& value) {
  int& parent = value;
  int& child = parent;
  return child;
}
mut int& projected(mut Pair& pair) {
  mut Pair& parent = pair;
  mut int& child = parent.left;
  return child;
}
mut int& call_relay(mut Pair& pair) {
  mut int& parent = projected(pair);
  mut int& child = parent;
  return child;
}
int main() {
  mut Pair pair = Pair();
  mut int& slot = relay(call_relay(pair));
  slot += 3;
  int& observed = read_relay(pair.left);
  return observed - 4;
}
)");
  expect(returnedChains.canGenerateCode() &&
             lang::verifyMirProgram(returnedChains.mir).valid(),
         "generic, projected, and accessor-result reborrow chains should "
         "preserve an incoming reference's owner through verified MIR");
  bool checkedPublication = false;
  for (const auto &function : returnedChains.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *published = instruction.loan
                                    ? function.body.findLoan(*instruction.loan)
                                    : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Borrow ||
            !published || published->kind != lang::MirLoanKind::Return ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Loan)
          continue;
        const auto *child =
            function.body.findLoan(instruction.operands.front().loan);
        const auto *parent =
            child ? function.body.findLoan(child->parent) : nullptr;
        if (!child || !parent || parent->kind != lang::MirLoanKind::CallResult)
          continue;
        checkedPublication = true;
        auto missingParent = returnedChains.mir;
        auto *body = &const_cast<lang::MirFunctionInstance *>(
                          missingParent.findFunctionInstance(function.id))
                          ->body;
        body->loans[child->id - 1].parent = 0;
        expect(hasMirVerificationError(
                   lang::verifyMirProgram(missingParent),
                   "borrow instruction does not match the loan's parent"),
               "a returned reborrow cannot lose its accessor-result parent");
        auto missingRetirement = returnedChains.mir;
        auto *retiredBody =
            &const_cast<lang::MirFunctionInstance *>(
                 missingRetirement.findFunctionInstance(function.id))
                 ->body;
        for (auto &retiredBlock : retiredBody->blocks)
          std::erase_if(retiredBlock.instructions, [&](const auto &candidate) {
            return candidate.kind == lang::MirInstructionKind::EndBorrow &&
                   candidate.loan == child->id;
          });
        expect(hasMirVerificationError(
                   lang::verifyMirProgram(missingRetirement),
                   "borrow instruction does not match the loan's parent"),
               "return publication must retire the exact body-local child");
      }
    }
  }
  expect(
      checkedPublication,
      "accessor-chain return should publish a fresh capability from its child");
  expect(rejects(R"(
class Cell {
public:
  Cell() = default;
  mut int& invalid() mut {
    mut int local = 1;
    mut int& parent = local;
    mut int& child = parent;
    return child;
  }
};
int main() { return 0; }
)",
                 "must derive from this method's receiver"),
         "a reborrow chain ending in a local object must not escape");
  expect(rejects(R"(
class Cell {
public:
  Cell() = default;
  mut int& invalid(mut int& other) mut {
    mut int& parent = other;
    mut int& child = parent;
    return child;
  }
};
int main() { return 0; }
)",
                 "must derive from this method's receiver"),
         "caller-owned ancestry must not replace the declared return owner");
  expect(rejects(R"(
class Cell {
public:
  mut int value = 1;
  Cell() = default;
  mut int& expose() mut {
    mut int& parent = this.value;
    mut int& child = parent;
    return child;
  }
};
int main() {
  mut Cell cell = Cell();
  mut int& result = cell.expose();
  cell.value += 1;
  return result;
}
)",
                 // MIR rejects the compound assignment's read first.
                 "Cannot read storage while an overlapping mutable borrow"),
         "a returned reborrow must retain the caller's mutable exclusion");
  expect(rejects(R"(
class View {
public:
  int& borrowed;
  mut int owned = 1;
  View(int& source) : borrowed(source) {}
  mut int& invalid() mut {
    mut View local{this.borrowed};
    mut int& parent = local.owned;
    mut int& child = parent;
    return child;
  }
};
int main() { return 0; }
)",
                 // MIR rejects the escape of the local object's own field.
                 "does not outlive the function"),
         "a local carrier's external dependency cannot extend its owned field");
  const lang::FrontendResult disjointIndexes =
      lang::Frontend().analyze("disjoint-index-borrows.gti", R"(
int main() {
  mut int values[2] = {};
  mut int& first = values[0];
  mut int& second = values[1];
  second += 1;
  first += 1;
  return 0;
}
)");
  expect(disjointIndexes.canGenerateCode(),
         "different constant fixed-array elements should be disjoint safe "
         "places");
  expect(rejects(R"(
int main() {
  mut int values[2] = {};
  mut int index = 1;
  mut int& first = values[0];
  mut int& dynamic = values[index];
  dynamic += 1;
  first += 1;
  return 0;
}
)",
                 "Cannot create a mutable borrow"),
         "a dynamic fixed-array index should remain may-alias with every "
         "constant element");
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

void testNonNullReferences() {
  const std::string source = nonNullReferenceSource();

  lang::FrontendResult frontend =
      lang::Frontend().analyze("references.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected reference diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "non-null read-only and mutable references should validate");

  const auto *read = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(1).get());
  const auto *increment = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(2).get());
  const lang::BindingInfo *readParameter =
      read == nullptr
          ? nullptr
          : frontend.semantics.findBinding(read->parameters().front());
  const lang::BindingInfo *writeParameter =
      increment == nullptr
          ? nullptr
          : frontend.semantics.findBinding(increment->parameters().front());
  expect(readParameter != nullptr &&
             readParameter->type.kind == lang::SemanticType::Reference &&
             readParameter->type.referenceAccess ==
                 lang::AccessMode::ReadOnly &&
             writeParameter != nullptr &&
             writeParameter->type.kind == lang::SemanticType::Reference &&
             writeParameter->type.referenceAccess == lang::AccessMode::Mutable,
         "reference bindings should retain borrow access in semantic metadata");

  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "restricted operator calls should retain verified MIR authority");
  const auto referenceParameter = [&](std::string_view name,
                                      lang::AccessMode access) {
    const lang::MirFunctionInstance *function =
        mirFunctionNamed(frontend, name);
    if (function == nullptr || function->parameterTypes.size() != 1)
      return false;
    const lang::SemanticType &parameter = function->parameterTypes.front();
    return parameter.kind == lang::SemanticType::Reference &&
           parameter.referenceAccess == access &&
           parameter.arguments.size() == 1 &&
           parameter.arguments.front().kind == lang::SemanticType::Class;
  };
  // `alias` retains a mutable local loan of the scalar `value` across
  // statements: MIR keeps that loan, rooted at the exact binding.
  bool retainedScalarLoan = false;
  if (const lang::MirFunctionInstance *mainMir =
          mirFunctionNamed(frontend, "main")) {
    for (const lang::MirLoan &loan : mainMir->body.loans) {
      const lang::MirPlace *source = mainMir->body.findPlace(loan.source);
      retainedScalarLoan =
          retainedScalarLoan ||
          (loan.kind == lang::MirLoanKind::Local &&
           loan.access == lang::AccessMode::Mutable && source != nullptr &&
           source->root == lang::MirPlaceRootKind::Binding &&
           source->projections.empty() &&
           mirBindingName(frontend, *mainMir, source->binding) == "value");
    }
  }
  expect(referenceParameter("read", lang::AccessMode::ReadOnly) &&
             referenceParameter("increment", lang::AccessMode::Mutable) &&
             retainedScalarLoan && nativeBackendEmits(frontend),
         "references should preserve read-only and mutable access in MIR "
         "signatures and retain local loans of their exact referent");

  const auto temporaryObserver = lang::Frontend().analyze(
      "scalar-reference-observer.gti",
      "void inspect(int& value) {} int main() { inspect(3); return 0; }");
  expect(temporaryObserver.canGenerateCode() &&
             lang::verifyMirProgram(temporaryObserver.mir).valid() &&
             checkedTemporaryObserver(temporaryObserver, "inspect", false),
         "scalar literals now select the readonly observer with a nonescaping "
         "call loan");

  const lang::FrontendResult invalid =
      analyzeWithMemory("invalid-references.gti", R"(
int& escape(mut int& value) { return value; }
int global_value = 1;
int& global_reference = global_value;

struct InvalidStorage {
  int& field;
};

void inspect(int& value) {}
void modify(mut int& value) {}

int main() {
  int immutable = 1;
  mut int mutable_value = 2;
  int& temporary_binding = 3;
  modify(immutable);
  mut int& invalid_mutable = immutable;
  int& missing;
  int values[2] = {1, 2};
  int[2]& array_reference = values;
  expected<int&, int> inner = immutable;
  expected<expected<int&, int>, int> nested = inner;
  Counter& dangling_owner = *std::make_unique<Counter>();
  int& dangling_member = Counter().value;
  inspect(mutable_value);
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "escaping, temporary, and invalid mutable references should fail");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "cannot derive from a mutable parameter") &&
          hasDiagnostic(invalid.diagnostics,
                        "References cannot be used as a storage") &&
          hasDiagnostic(invalid.diagnostics,
                        "Reference initializer must be an addressable") &&
          hasDiagnostic(invalid.diagnostics, "requires a mutable value") &&
          hasDiagnostic(invalid.diagnostics,
                        "mutable reference requires a mutable initializer") &&
          hasDiagnostic(invalid.diagnostics,
                        "Reference bindings require an initializer") &&
          hasDiagnostic(invalid.diagnostics,
                        "complete checked dependency contract") &&
          hasDiagnostic(invalid.diagnostics, "derived from temporary storage"),
      "reference diagnostics should identify each rejected lifetime rule");

  const std::string formatted = lang::Formatter().format(
      "void inspect(int& value){}void modify(mut int& value){}"
      "int main(){mut int value=1;mut int& alias=value;modify(alias);return "
      "0;}");
  expect(lang::Formatter().format(formatted) == formatted,
         "reference syntax formatting should be idempotent");
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

void testReceiverTiedReferenceReturns() {
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}

  T& get() {
    return this.value;
  }
};

class Buffer<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;

public:
  Buffer(uint64_t capacity)
      : data(std::detail::prefix_owner<T>(capacity)) {}

  void push(T value) mut {
    this.data.append_value(std::move(value));
  }

  T& at(uint64_t index) {
    return this.data.read(index);
  }
};

int main() {
  Box<int> box = Box<int>(7);
  int& boxed = box.get();
  mut Buffer<int> buffer = Buffer<int>(uint64_t(1));
  buffer.push(9);
  int& stored = buffer.at(uint64_t(0));
  return boxed + stored - 16;
}
)";

  lang::FrontendResult frontend = gti_test::analyzeTrustedPrivateMemoryFixture(
      "receiver-tied-references.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected receiver-tied reference diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "methods should return read-only references borrowed from 'this'");

  const lang::FunctionDecl *mainFunction =
      findTopLevelFunction(frontend.program, "main");
  const auto *borrowed =
      mainFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                mainFunction->body()->statements().at(1).get());
  const auto *borrowCall =
      borrowed == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(borrowed->initializer().get());
  const lang::ResolvedCallInfo *resolved =
      borrowCall == nullptr ? nullptr
                            : frontend.semantics.findCall(*borrowCall);
  const lang::ExpressionInfo *expression =
      borrowCall == nullptr ? nullptr
                            : frontend.semantics.findExpression(*borrowCall);
  expect(resolved != nullptr &&
             resolved->returnType.kind == lang::SemanticType::Reference &&
             resolved->borrowOrigin == lang::BorrowOriginKind::Receiver &&
             expression != nullptr &&
             expression->category == lang::ValueCategory::Place &&
             expression->access == lang::AccessMode::ReadOnly,
         "reference calls should retain receiver-tied borrow metadata");

  // Box<int>::get() cannot fail: it returns the exact loaned referent. The
  // checked prefix read keeps its failure edge and publishes the call-result
  // loan it receives rather than a copy of the referent.
  const auto getFunctions = mirFunctionsNamed(frontend, "get");
  const lang::MirFunctionInstance *atFunction =
      mirFunctionNamed(frontend, "at");
  expect(getFunctions.size() == 1 &&
             !getFunctions.front()->mayRaiseDefinedFailure &&
             returnsOnlyEscapingLoan(*getFunctions.front(),
                                     lang::AccessMode::ReadOnly) &&
             atFunction != nullptr &&
             returnsOnlyEscapingLoan(*atFunction, lang::AccessMode::ReadOnly) &&
             returnTransfersCallResultLoan(*atFunction) &&
             hasMirIntrinsicKind(frontend, lang::IntrinsicKind::RawBorrow) &&
             nativeBackendEmits(frontend),
         "receiver-tied borrows should lower through exact read-only loans "
         "and publish the returned loan without copying its referent");

  const lang::FrontendResult nested =
      gti_test::analyzeTrustedPrivateMemoryFixture(
          "nested-receiver-reference.gti", R"(
class ElementBuffer {
  mut std::detail::prefix_owner<int> data;

public:
  ElementBuffer()
      : data(std::detail::prefix_owner<int>(uint64_t(1))) {}

  void set(int value) mut {
    this.data.append_value(std::move(value));
  }

  int& at() {
    return this.data.read(uint64_t(0));
  }
};

class BufferFacade {
  mut ElementBuffer buffer;

public:
  BufferFacade() : buffer(ElementBuffer()) {}

  void set(int value) mut {
    this.buffer.set(value);
  }

  int& at() {
    return this.buffer.at();
  }
};

int main() {
  mut BufferFacade facade = BufferFacade();
  facade.set(42);
  return facade.at() - 42;
}
)");
  if (!nested.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : nested.diagnostics) {
      std::cerr << "Unexpected nested receiver-reference diagnostic: "
                << diagnostic.message << '\n';
    }
  }

  const lang::MirFunctionInstance *nestedReturn = nullptr;
  const lang::MirInstruction *nestedTransfer = nullptr;
  const lang::MirLoan *callResultLoan = nullptr;
  const lang::MirLoan *nestedReturnLoan = nullptr;
  for (const lang::MirFunctionInstance &function :
       nested.mir.functionInstances()) {
    for (const lang::MirLoan &loan : function.body.loans) {
      if (loan.kind != lang::MirLoanKind::Return || !loan.escapes) {
        continue;
      }
      for (const lang::MirBlock &block : function.body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Borrow ||
              instruction.loan != loan.id || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != lang::MirOperandKind::Loan) {
            continue;
          }
          const lang::MirLoan *candidate =
              function.body.findLoan(instruction.operands.front().loan);
          if (candidate != nullptr &&
              candidate->kind == lang::MirLoanKind::CallResult) {
            nestedReturn = &function;
            nestedTransfer = &instruction;
            callResultLoan = candidate;
            nestedReturnLoan = &loan;
          }
        }
      }
    }
  }
  expect(nested.canGenerateCode() &&
             lang::verifyMirProgram(nested.mir).valid() &&
             nestedReturn != nullptr && nestedTransfer != nullptr &&
             callResultLoan != nullptr && nestedReturnLoan != nullptr &&
             callResultLoan->source == nestedReturnLoan->source &&
             callResultLoan->access == nestedReturnLoan->access &&
             nestedReturn->body.returnType.arguments.size() == 1 &&
             nestedTransfer->info.type ==
                 nestedReturn->body.returnType.arguments.front(),
         "a wrapper reference return should retain its receiver as the "
         "semantic owner while transferring the nested call's exact referent");

  if (nestedReturn != nullptr && nestedTransfer != nullptr &&
      callResultLoan != nullptr && nestedReturnLoan != nullptr) {
    expect(nativeBackendEmits(nested),
           "the native backend should accept the verified call-result loan "
           "transfer instead of re-borrowing the wrapper object");

    lang::MirProgram forged = nested.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    lang::MirFunctionInstance &forgedReturn = functions[nestedReturn->id - 1];
    bool corrupted = false;
    for (lang::MirBlock &block : forgedReturn.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == nestedTransfer->id &&
            !instruction.operands.empty()) {
          instruction.operands.front().loan = nestedReturnLoan->id;
          corrupted = true;
        }
      }
    }
    const lang::MirVerificationResult rejected = lang::verifyMirProgram(forged);
    expect(corrupted && !rejected.valid() &&
               hasMirVerificationError(
                   rejected,
                   "borrow instruction does not match the loan's parent"),
           "MIR verification should reject a forged nested-reference return "
           "that no longer transfers its exact call-result loan");
  }

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-reference-returns.gti", R"(
int& escape(int& value) { return value; }

class InvalidReferences {
  int value = 1;

public:
  int& local() {
    int value = 2;
    return value;
  }

  int& parameter(int& value) {
    return value;
  }

  int& literal() {
    return 3;
  }
public:
  InvalidReferences() = default;
};

int main() {
  int& dangling = InvalidReferences().local();
  return dangling;
}
)");
  expect(!invalid.canGenerateCode(),
         "reference returns must not escape locals, parameters, or temporary "
         "receivers");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "must derive from this method's receiver") &&
          hasDiagnostic(invalid.diagnostics,
                        "Reference return requires an addressable value") &&
          hasDiagnostic(invalid.diagnostics, "derived from temporary storage"),
      "reference-return diagnostics should explain each rejected lifetime");

  const lang::FrontendResult invalidated =
      gti_test::analyzeTrustedPrivateMemoryFixture("invalidated-reference.gti",
                                                   R"(
class Buffer<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;

public:
  Buffer(uint64_t capacity)
      : data(std::detail::prefix_owner<T>(capacity)) {}

  T& at(uint64_t index) {
    return this.data.read(index);
  }

  void clear(uint64_t index) mut {
    this.data.pop();
  }

  T invalidate_receiver(uint64_t index) mut {
    T& value = this.data.read(index);
    this.data.pop();
    return value;
  }
};

int main() {
  mut Buffer<int> buffer = Buffer<int>(uint64_t(1));
  int& value = buffer.at(uint64_t(0));
  buffer.clear(uint64_t(0));
  Buffer<int> moved = std::move(buffer);
  return value;
}
)");
  expect(!invalidated.canGenerateCode(),
         "move-only receivers must remain stable while borrowed");
  // The MIR borrow checker reports the first conflict of each concrete body
  // (the uncalled generic method has no instance to check).
  // A move-only receiver names that storage kind in the MIR conflict.
  expect(hasDiagnostic(invalidated.diagnostics,
                       "Mutable method cannot use move-only storage "
                       "while a reference borrowed from it may still "
                       "be live"),
         "borrow diagnostics should prevent receiver invalidation");
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

void testStoredReferenceGroundwork() {
  const std::string source = R"(
struct SingleSentinel { public: SingleSentinel() = default; };

// RFC 0004: the iterator names its referent region, so a dereference
// borrows the range, not the iterator shell.
class BorrowingIterator<T, lifetime a> {
  T&<a> value;
  mut uint64_t position = 0;

public:
  BorrowingIterator(T&<a> source) : value(source) {}

  T&<a> operator*() { return value; }
  void operator++() mut { this.position++; }
  bool operator!=(BorrowingIterator<T, a>& other) {
    return this.position != other.position;
  }
  bool operator!=(SingleSentinel& sentinel) { return this.position == 0; }
};

class SingleRange<T> {
  mut T value;

public:
  SingleRange(T initial) : value(initial) {}

  BorrowingIterator<T> begin() {
    return BorrowingIterator<T>(value);
  }

  SingleSentinel end() { return SingleSentinel(); }

  void replace(T next) mut { this.value = next; }
};

// The adapter's result names the region of its stored range reference, so
// it borrows the range, not the adapter shell.
class WrappedRange<T, lifetime r> {
  SingleRange<T>&<r> internal;

public:
  WrappedRange(SingleRange<T>&<r> source) : internal(source) {}

  BorrowingIterator<T, r> begin() {
    return internal.begin();
  }
};

int main() {
  mut SingleRange<int> range = SingleRange<int>(7);
  mut BorrowingIterator<int> iterator = range.begin();
  int& value = *iterator;
  ++iterator;
  mut BorrowingIterator<int> moved = std::move(iterator);
  ++moved;
  mut BorrowingIterator<int> copied = moved;
  ++copied;
  if (!(moved != copied)) { return 1; }
  mut int total = 0;
  for (int& item : range) { total += item; }
  mut SingleRange<int> wrapped_range = SingleRange<int>(7);
  WrappedRange<int> wrapped = WrappedRange<int>(wrapped_range);
  mut BorrowingIterator<int> wrapped_iterator = wrapped.begin();
  int& wrapped_value = *wrapped_iterator;
  return value + total + wrapped_value - 21;
}
)";

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "stored-references.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected stored-reference diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "unqualified instance fields should retain receiver provenance for "
         "reference and stored-reference returns");

  auto movedWrapperSource = source;
  movedWrapperSource.insert(movedWrapperSource.find("  int& wrapped_value"),
                            "  auto moved_wrapper = std::move(wrapped);\n");
  const auto movedWrapper = lang::Frontend().analyze(
      "stored-reference-external-adapter-move.gti", movedWrapperSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(movedWrapper.canGenerateCode() &&
             lang::verifyMirProgram(movedWrapper.mir).valid(),
         "an adapter relayed through a stored reference does not freeze the "
         "independently movable wrapper shell");
  auto invalidWrapperSource = source;
  invalidWrapperSource.insert(invalidWrapperSource.find("  int& wrapped_value"),
                              "  wrapped_range.replace(9);\n");
  const auto invalidWrapper = lang::Frontend().analyze(
      "stored-reference-external-adapter-reset.gti", invalidWrapperSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidWrapper.canGenerateCode() &&
             hasDiagnosticCode(invalidWrapper.diagnostics, "GTI-S2017"),
         "an externally relayed adapter still freezes its actual range owner");

  const lang::FunctionDecl *storedReferenceMain =
      findTopLevelFunction(frontend.program, "main");
  const lang::VariableDecl *originalIterator =
      storedReferenceMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                storedReferenceMain->body()->statements().at(1).get());
  const lang::VariableDecl *movedIterator =
      storedReferenceMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                storedReferenceMain->body()->statements().at(4).get());
  const lang::BindingInfo *originalIteratorBinding =
      originalIterator == nullptr
          ? nullptr
          : frontend.semantics.findBinding(*originalIterator);
  const lang::BindingInfo *movedIteratorBinding =
      movedIterator == nullptr ? nullptr
                               : frontend.semantics.findBinding(*movedIterator);
  expect(originalIteratorBinding != nullptr &&
             movedIteratorBinding != nullptr &&
             mirRetained(frontend, originalIterator).loan != nullptr &&
             mirRetained(frontend, originalIterator).loan ==
                 mirRetained(frontend, movedIterator).loan,
         "moving borrowed state should transfer one MIR loan identity "
         "instead of creating a second owner dependency");

  const lang::ClassDecl *iteratorClass =
      findTopLevelClass(frontend.program, "BorrowingIterator");
  const lang::ClassLifecycleInfo *lifecycle =
      iteratorClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*iteratorClass);
  expect(lifecycle != nullptr && lifecycle->traits.containsBorrowedState &&
             lifecycle->traits.copyable && lifecycle->traits.movable &&
             !lifecycle->traits.copyAssignable &&
             !lifecycle->traits.moveAssignable &&
             lifecycle->constructors.size() == 1 &&
             lifecycle->constructors.front().borrowParameter == 0,
         "read-only stored-reference classes should copy-construct independent "
         "borrow carriers without becoming assignable");

  const lang::ClassDecl *rangeClass =
      findTopLevelClass(frontend.program, "SingleRange");
  const auto *begin = rangeClass == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::FunctionDecl *>(
                                rangeClass->members().at(3).get());
  const auto *returnStatement =
      begin == nullptr ? nullptr
                       : dynamic_cast<const lang::ReturnStmt *>(
                             begin->body()->statements().front().get());
  const auto *construction =
      returnStatement == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(returnStatement->value().get());
  const lang::ResolvedConstructionInfo *resolvedConstruction =
      construction == nullptr
          ? nullptr
          : frontend.semantics.findConstruction(*construction);
  expect(resolvedConstruction != nullptr &&
             resolvedConstruction->borrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             resolvedConstruction->borrowArgument == 0,
         "stored-reference construction should identify its exact owner "
         "argument before HIR lowering");

  bool foundStoredLoan = false;
  bool foundEscapingFieldLoan = false;
  bool foundReceiverReturnLoan = false;
  for (const lang::MirConstructorInstance &constructorInstance :
       frontend.mir.constructorInstances()) {
    for (const lang::MirLoan &loan : constructorInstance.body.loans) {
      foundEscapingFieldLoan =
          foundEscapingFieldLoan ||
          (loan.kind == lang::MirLoanKind::Stored && loan.escapes &&
           loan.storedField != 0 && loan.source != 0);
    }
  }
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirLoan &loan : function.body.loans) {
      foundStoredLoan =
          foundStoredLoan || (loan.kind == lang::MirLoanKind::Stored &&
                              !loan.escapes && loan.source != 0);
      foundReceiverReturnLoan =
          foundReceiverReturnLoan || (loan.kind == lang::MirLoanKind::Return &&
                                      loan.escapes && loan.source != 0);
    }
  }
  expect(frontend.mir.valid() && foundStoredLoan && foundEscapingFieldLoan &&
             foundReceiverReturnLoan,
         "MIR should retain local, field-stored, and receiver-returned borrow "
         "provenance");

  std::size_t iteratorInstances = 0;
  bool exactIteratorSpecialMembers = true;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.type.classId);
    if (instance.type.kind != lang::SemanticType::Class || info == nullptr ||
        info->qualifiedName != "BorrowingIterator")
      continue;
    ++iteratorInstances;
    // The stored `T& value` field keeps read-only reference access.
    const bool readOnlyStorage =
        !instance.declaredFields.empty() &&
        instance.declaredFields.front().type.kind ==
            lang::SemanticType::Reference &&
        instance.declaredFields.front().type.referenceAccess ==
            lang::AccessMode::ReadOnly;
    exactIteratorSpecialMembers =
        exactIteratorSpecialMembers && readOnlyStorage &&
        instance.copyConstructor == lang::SpecialMemberStatus::Generated &&
        instance.moveConstructor == lang::SpecialMemberStatus::Generated &&
        instance.copyAssignment == lang::SpecialMemberStatus::Deleted;
  }
  expect(iteratorInstances != 0 && exactIteratorSpecialMembers &&
             nativeBackendEmits(frontend),
         "MIR should preserve read-only storage and frontend special-member "
         "decisions on each concrete iterator application");
}

void testMutableStoredDependencies() {
  const std::string validSource = R"(
class MutView {
  using value_type = int;
  mut value_type& referent;
public:
  MutView(mut value_type& source) : referent(source) {}
  mut value_type& get() mut { return this.referent; }
};

MutView make_view(mut int& source) {
  return MutView(source);
}

MutView relay_view(MutView view) {
  return std::move(view);
}

MutView choose_view(mut int& source, bool first) {
  if (first) {
    return MutView(source);
  }
  return MutView(source);
}

mut int& relay_reference(mut int& source) {
  return source;
}

int write_then_exit(mut int& source, bool early) {
  mut MutView view = MutView(source);
  mut int& slot = view.get();
  slot = 10;
  if (early) {
    return 4;
  }
  slot = 11;
  return 5;
}

int main() {
  mut int owner = 1;
  bool update = true;
  {
    mut MutView first = choose_view(owner, true);
    mut MutView moved = relay_view(std::move(first));
    if (update) {
      mut int& slot = moved.get();
      slot = 7;
    }
  }
  owner += 1;
  int branch = write_then_exit(owner, true);
  mut int& relayed = relay_reference(owner);
  relayed += 1;
  return owner + branch - 15;
}
)";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "mutable-stored-dependency.gti", validSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected mutable stored-dependency diagnostic: "
                << diagnostic.code << ' ' << diagnostic.primary.start << ' '
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "one direct mutable stored dependency should construct, return, "
         "move, reborrow, and reactivate its exact owner");
  expect(nativeBackendEmits(valid),
         "the native backend should emit a verified mutable borrow carrier");

  const lang::ClassDecl *carrier = findTopLevelClass(valid.program, "MutView");
  const lang::ClassLifecycleInfo *lifecycle =
      carrier == nullptr ? nullptr
                         : valid.semantics.findClassLifecycle(*carrier);
  expect(lifecycle != nullptr && lifecycle->traits.containsBorrowedState &&
             !lifecycle->traits.copyable && lifecycle->traits.movable &&
             !lifecycle->traits.copyAssignable &&
             !lifecycle->traits.moveAssignable &&
             lifecycle->constructors.size() == 1 &&
             lifecycle->constructors.front().borrowParameter == 0 &&
             lifecycle->constructors.front().borrowAccess ==
                 lang::AccessMode::Mutable,
         "a mutable stored-reference carrier should be move-only and retain "
         "one exact mutable constructor dependency");

  bool hasMutableParameterLoan = false;
  bool hasMutableStoredTransfer = false;
  bool hasMutableReturnLoan = false;
  std::optional<lang::MirBodyAddress> transferAddress;
  lang::MirLoanId consumedTransfer = 0;
  lang::MirLoanId publishedTransfer = 0;
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(valid.mir)) {
    const lang::MirBody *body = lang::findMirBody(valid.mir, address);
    if (body == nullptr) {
      continue;
    }
    for (const lang::MirLoan &loan : body->loans) {
      hasMutableParameterLoan =
          hasMutableParameterLoan ||
          (loan.kind == lang::MirLoanKind::Parameter && loan.entry &&
           loan.access == lang::AccessMode::Mutable);
      hasMutableReturnLoan =
          hasMutableReturnLoan ||
          (loan.kind == lang::MirLoanKind::Return && loan.escapes &&
           loan.access == lang::AccessMode::Mutable);
    }
    for (const lang::MirBlock &block : body->blocks) {
      for (const lang::MirInstruction &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            !call.functionTarget || !call.loan || call.operands.size() != 1) {
          continue;
        }
        const auto *target =
            valid.hir.findFunctionInstance(*call.functionTarget);
        if (target == nullptr || target->source == nullptr ||
            target->source->name().lexeme != "relay_view") {
          continue;
        }
        const lang::MirLoan *published = body->findLoan(*call.loan);
        const lang::MirValue *inputValue =
            body->findValue(call.operands.front().value);
        const lang::MirInstruction *prepared = nullptr;
        for (const lang::MirBlock &candidateBlock : body->blocks) {
          for (const lang::MirInstruction &candidate :
               candidateBlock.instructions) {
            if (inputValue != nullptr &&
                candidate.id == inputValue->definition) {
              prepared = &candidate;
            }
          }
        }
        if (published == nullptr || prepared == nullptr ||
            prepared->kind != lang::MirInstructionKind::CallInput ||
            prepared->callInputKind != lang::HirCallInputKind::MoveValue ||
            !prepared->ownership) {
          continue;
        }
        lang::HirBindingId inputBinding = 0;
        for (const lang::MirPlace &place : body->places) {
          if (place.root == lang::MirPlaceRootKind::Binding &&
              place.symbol == prepared->ownership->place.root &&
              place.projections.empty()) {
            inputBinding = place.binding;
          }
        }
        const lang::MirLoan *consumed = nullptr;
        for (const lang::MirLoan &loan : body->loans) {
          if (loan.kind == lang::MirLoanKind::Stored && inputBinding != 0 &&
              std::find(loan.carriers.begin(), loan.carriers.end(),
                        inputBinding) != loan.carriers.end()) {
            consumed = &loan;
          }
        }
        // A consuming relay preserves the external dependency, not a borrow
        // of the input shell which is retired on the success continuation.
        if (consumed != nullptr &&
            published->kind == lang::MirLoanKind::Stored &&
            published->access == lang::AccessMode::Mutable &&
            !published->entry && !published->escapes &&
            published->id != consumed->id &&
            published->source == consumed->source &&
            published->access == consumed->access &&
            published->parent == consumed->parent &&
            published->parent != consumed->id &&
            published->storedDependency == consumed->storedDependency &&
            published->additionalDependencies ==
                consumed->additionalDependencies) {
          hasMutableStoredTransfer = true;
          transferAddress = address;
          consumedTransfer = consumed->id;
          publishedTransfer = published->id;
        }
      }
    }
  }
  expect(hasMutableParameterLoan && hasMutableStoredTransfer &&
             hasMutableReturnLoan,
         "verified MIR should preserve the mutable parameter origin, the "
         "exclusive stored-carrier transfer, and the escaping mutable "
         "return dependency");

  lang::MirProgram forgedConsumedParent = valid.mir;
  bool changedConsumedParent = false;
  if (transferAddress) {
    if (lang::MirBody *body =
            lang::findMirBody(forgedConsumedParent, *transferAddress)) {
      for (lang::MirLoan &loan : body->loans) {
        if (loan.id == publishedTransfer) {
          loan.parent = consumedTransfer;
          changedConsumedParent = true;
        }
      }
    }
  }
  expect(changedConsumedParent &&
             !lang::verifyMirProgram(forgedConsumedParent).valid(),
         "a returned carrier must not retain its consumed input as a parent");

  lang::MirProgram forgedMissingRetirement = valid.mir;
  bool removedRetirement = false;
  if (transferAddress) {
    if (lang::MirBody *body =
            lang::findMirBody(forgedMissingRetirement, *transferAddress)) {
      for (lang::MirBlock &block : body->blocks) {
        if (block.activeFailure || block.failureParameter) {
          continue;
        }
        const auto ending = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == consumedTransfer;
            });
        if (ending != block.instructions.end()) {
          block.instructions.erase(ending);
          removedRetirement = true;
          break;
        }
      }
    }
  }
  expect(removedRetirement &&
             !lang::verifyMirProgram(forgedMissingRetirement).valid(),
         "a consuming relay must retire its input before publishing the "
         "independent result dependency");

  lang::MirProgram forgedParent = valid.mir;
  bool changedParent = false;
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(forgedParent)) {
    lang::MirBody *body = lang::findMirBody(forgedParent, address);
    if (body == nullptr) {
      continue;
    }
    for (lang::MirLoan &loan : body->loans) {
      if (loan.kind == lang::MirLoanKind::Stored && loan.parent != 0 &&
          loan.access == lang::AccessMode::Mutable) {
        loan.parent = loan.id;
        changedParent = true;
        break;
      }
    }
    if (changedParent) {
      break;
    }
  }
  expect(changedParent && !lang::verifyMirProgram(forgedParent).valid(),
         "MIR verification should reject a forged mutable carrier ancestry");

  lang::MirProgram forgedDoubleEnd = valid.mir;
  bool duplicatedEnd = false;
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(forgedDoubleEnd)) {
    lang::MirBody *body = lang::findMirBody(forgedDoubleEnd, address);
    if (body == nullptr) {
      continue;
    }
    lang::MirLoanId transferred = 0;
    for (const lang::MirLoan &loan : body->loans) {
      if (loan.kind == lang::MirLoanKind::Stored && loan.parent != 0 &&
          loan.access == lang::AccessMode::Mutable) {
        transferred = loan.id;
        break;
      }
    }
    if (transferred == 0) {
      continue;
    }
    lang::MirInstructionId nextInstruction = 1;
    for (const lang::MirBlock &block : body->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        nextInstruction = std::max(nextInstruction, instruction.id + 1);
      }
    }
    for (lang::MirBlock &block : body->blocks) {
      const auto ended = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == transferred;
          });
      if (ended == block.instructions.end()) {
        continue;
      }
      lang::MirInstruction duplicate = *ended;
      duplicate.id = nextInstruction;
      block.instructions.insert(std::next(ended), std::move(duplicate));
      duplicatedEnd = true;
      break;
    }
    if (duplicatedEnd) {
      break;
    }
  }
  const lang::MirVerificationResult doubleEndResult =
      lang::verifyMirProgram(forgedDoubleEnd);
  expect(duplicatedEnd && !doubleEndResult.valid() &&
             hasMirVerificationError(doubleEndResult,
                                     "is ended while it is inactive"),
         "MIR verification should reject ending a transferred mutable "
         "carrier dependency twice");

  const lang::FrontendResult overlapping = lang::Frontend().analyze(
      "mutable-stored-overlap.gti", R"(
class MutView {
  mut int& referent;
public:
  MutView(mut int& source) : referent(source) {}
  mut int& get() mut { return this.referent; }
};
int main() {
  mut int owner = 1;
  mut MutView first = MutView(owner);
  owner = 2;
  mut MutView second = MutView(owner);
  mut int& first_value = first.get();
  mut int& second_value = second.get();
  return first_value + second_value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!overlapping.canGenerateCode() &&
             hasDiagnosticCode(overlapping.diagnostics, "GTI-S2017") &&
             hasDiagnostic(overlapping.diagnostics,
                           "overlapping borrow may still be live"),
         "a live mutable carrier should exclude owner mutation and a second "
         "overlapping carrier");

  const lang::FrontendResult unsupported = lang::Frontend().analyze(
      "mutable-stored-unsupported-shapes.gti", R"(
class MutView {
  mut int& referent;
public:
  MutView(mut int& source) : referent(source) {}
};
class Nested {
  MutView view;
public:
  Nested(MutView source) : view(std::move(source)) {}
};
MutView choose(mut int& left, mut int& right) {
  return MutView(left);
}
mut int global_owner = 1;
MutView global_view = MutView(global_owner);
int main() {
  mut int owner = 1;
  mut MutView first = MutView(owner);
  MutView copied = first;
  mut MutView other = std::move(first);
  other = std::move(copied);
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  // RFC 0004 reports the two-supplier factory as an ambiguous elided region
  // (Rust E0106); the legacy model as an ambiguous body dependency.
  expect(!unsupported.canGenerateCode() &&
             (hasDiagnostic(unsupported.diagnostics,
                            "elided region is ambiguous among 2 "
                            "borrowed parameters")) &&
             hasDiagnostic(unsupported.diagnostics,
                           "cannot have global or static storage") &&
             hasDiagnostic(unsupported.diagnostics,
                           "Cannot initialize 'copied'") &&
             hasDiagnosticCode(unsupported.diagnostics, "GTI-S2003") &&
             hasDiagnostic(unsupported.diagnostics,
                           "Move assignment is unavailable for type 'MutView'"),
         "multi-origin, global, copied, and reseated mutable "
         "dependency graphs should remain rejected");
}

void testComposedCarrierFunctionTransfer() {
  const std::string declarations = R"(
struct Cell { mut int value = 0;  public: Cell() = default; };
struct Context {
  int& step;
  mut Cell& left;
  mut Cell& right;
  Context(int& step, mut Cell& left, mut Cell& right)
      : step(step), left(left), right(right) {}
  void update() mut {
    this.left.value += this.step;
    this.right.value += this.step;
  }
};
T forward_context<T>(T context) { return std::move(context); }
)";
  const std::string setup = R"(
int main() {
  int step = 2;
  mut Cell left{};
  mut Cell right{};
  mut Context initial{step, left, right};
)";
  const std::string forwarding = R"(
  Context intermediate = forward_context<Context>(std::move(initial));
  mut Context active = forward_context<Context>(std::move(intermediate));
)";
  const auto analyze = [&](const std::string &name, const std::string &body,
                           const std::string &extra = "") {
    return lang::Frontend().analyze(name, declarations + extra + setup + body,
                                    {standardLibraryPrelude()}, {},
                                    {standardLibraryRoot()});
  };
  const auto valid = analyze("composed-carrier-forwarding.gti", forwarding + R"(
  active.update();
  active.update();
  left.value += 1;
  return left.value + right.value - 9;
}
)");
  for (const std::string expression :
       {"forward_context<Context>(std::move(initial))",
        "(forward_context<Context>(std::move(initial)))",
        "forward_context<Context>(forward_context<Context>(std::move(initial))"
        ")"}) {
    const auto discarded = analyze("discarded-carrier-forwarding.gti",
                                   "  [[discard]] " + expression + R"(;
  left.value = 3;
  right.value = 4;
  return left.value + right.value - 7;
}
)");
    expect(discarded.canGenerateCode() &&
               lang::verifyMirProgram(discarded.mir).valid(),
           "discarded moved carrier results end their temporary dependencies "
           "without redirecting the consumed input's semantic endpoint");
  }
  if (!valid.canGenerateCode())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << "Composed forwarding diagnostic: " << diagnostic.code << ' '
                << diagnostic.message << '\n';
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "generic forwarding transfers a complete mixed carrier through "
         "ordinary by-value calls and releases both exclusive owners");
  if (valid.canGenerateCode()) {
    const auto *main = findTopLevelFunction(valid.program, "main");
    // RFC 0004: MIR owns loan identity. A consumed carrier's binding keeps no
    // semantic loan; each carrier's MIR Stored loan carries the same facts.
    const lang::MirFunctionInstance *mainMir = nullptr;
    for (const auto &function : valid.mir.functionInstances())
      if (function.entryKind != lang::ProgramEntryKind::None)
        mainMir = &function;
    const auto mirCarrierLoan =
        [&](const std::string &name) -> const lang::MirLoan * {
      const auto *local = findDirectLocal(main, name);
      const auto *hir = mainMir == nullptr
                            ? nullptr
                            : valid.hir.findFunctionInstance(mainMir->id);
      if (local == nullptr || hir == nullptr)
        return nullptr;
      for (const auto &binding : hir->body.bindings)
        if (binding.variable == local)
          for (const auto &loan : mainMir->body.loans)
            if (loan.kind == lang::MirLoanKind::Stored &&
                std::find(loan.carriers.begin(), loan.carriers.end(),
                          binding.id) != loan.carriers.end())
              return &loan;
      return nullptr;
    };
    {
      const auto *mirInitial = mirCarrierLoan("initial");
      for (const auto *loan :
           {mirCarrierLoan("intermediate"), mirCarrierLoan("active")})
        expect(mirInitial != nullptr && loan != nullptr &&
                   mirInitial->access == lang::AccessMode::ReadOnly &&
                   mirInitial->additionalDependencies.size() == 2 &&
                   std::all_of(mirInitial->additionalDependencies.begin(),
                               mirInitial->additionalDependencies.end(),
                               [](const auto &dependency) {
                                 return dependency.access ==
                                        lang::AccessMode::Mutable;
                               }) &&
                   loan->source == mirInitial->source &&
                   loan->access == mirInitial->access &&
                   loan->designatedFields == mirInitial->designatedFields &&
                   loan->additionalDependencies ==
                       mirInitial->additionalDependencies,
               "each forwarding result preserves the exact shared and two "
               "exclusive dependency places, field identities, and "
               "permissions");
    }
    expect(nativeBackendEmits(valid),
           "mixed carrier transfer remains admitted by the verified backend");
    for (const auto &function : valid.mir.functionInstances()) {
      if (function.entryKind == lang::ProgramEntryKind::None)
        continue;
      for (const auto &[consumed, result] :
           {std::pair{"initial", "intermediate"},
            std::pair{"intermediate", "active"}}) {
        const auto *inputLoan = mirCarrierLoan(consumed);
        const auto *resultLoan = mirCarrierLoan(result);
        unsigned endCount = 0;
        lang::MirInstructionId call = 0, end = 0, initialize = 0;
        const lang::MirBlock *callBlock = nullptr, *endBlock = nullptr,
                             *initializeBlock = nullptr;
        for (const auto &block : function.body.blocks)
          for (const auto &instruction : block.instructions) {
            if (inputLoan != nullptr &&
                instruction.kind == lang::MirInstructionKind::EndBorrow &&
                instruction.loan == inputLoan->id && block.activeFailure == 0) {
              // Separate failure continuations retire the same static loan
              // on disjoint paths; count the successful transfer path only.
              ++endCount;
              end = instruction.id;
              endBlock = &block;
            }
            if (resultLoan == nullptr)
              continue;
            if (instruction.kind == lang::MirInstructionKind::Call &&
                instruction.loan == resultLoan->id) {
              call = instruction.id;
              callBlock = &block;
            }
            const auto *destination =
                function.body.findPlace(instruction.destination.value_or(0));
            if (instruction.kind == lang::MirInstructionKind::Initialize &&
                destination != nullptr &&
                destination->root == lang::MirPlaceRootKind::Binding &&
                std::find(resultLoan->carriers.begin(),
                          resultLoan->carriers.end(),
                          destination->binding) != resultLoan->carriers.end()) {
              initialize = instruction.id;
              initializeBlock = &block;
            }
          }
        // A failure-capable callee continues on its Invoke success edge; a
        // failure-free one, such as this forwarder, returns in its own block.
        const bool invokeSuccess =
            callBlock != nullptr && endBlock != nullptr &&
            callBlock->terminator.kind == lang::MirTerminatorKind::Invoke &&
            callBlock->terminator.invokeInstruction == call &&
            callBlock->terminator.target == endBlock->id;
        const bool plainSuccess =
            callBlock != nullptr && callBlock == endBlock && call < end;
        expect(endCount == 1 && call != 0 && end < initialize &&
                   callBlock != nullptr && endBlock != nullptr &&
                   endBlock == initializeBlock &&
                   (invokeSuccess || plainSuccess),
               "each consumed carrier ends exactly once on its call-success "
               "continuation before the returned carrier is initialized");
      }
    }
    if (const auto *active = mirCarrierLoan("active");
        active != nullptr && mainMir != nullptr) {
      auto forged = valid.mir;
      bool changed = false;
      auto *body =
          lang::findMirBody(forged, {lang::MirBodyKind::Function, mainMir->id});
      if (body != nullptr &&
          !body->loans[active->id - 1].additionalDependencies.empty()) {
        body->loans[active->id - 1].additionalDependencies.pop_back();
        changed = true;
      }
      expect(changed && !lang::verifyMirProgram(forged).valid(),
             "independent MIR verification rejects losing either exclusive "
             "dependency during carrier forwarding");
    }
  }
  const auto branched = analyze("composed-carrier-branch-transfer.gti", R"(
  if (step == 2) {
    mut Context active = forward_context<Context>(std::move(initial));
    active.update();
  } else {
    mut Context active = forward_context<Context>(std::move(initial));
    active.update();
  }
  return left.value + right.value - 4;
}
)");
  expect(branched.canGenerateCode() &&
             lang::verifyMirProgram(branched.mir).valid(),
         "mutually exclusive carrier transfers retain distinct statement "
         "endpoints across branch lowering");
  const auto nestedBranch = analyze("composed-carrier-nested-branch.gti", R"(
  if (step == 2) {
    if (step > 0) {
      mut Context active = forward_context<Context>(std::move(initial));
      active.update();
    } else {
      mut Context active = forward_context<Context>(std::move(initial));
      active.update();
    }
  } else {
    mut Context active = forward_context<Context>(std::move(initial));
    active.update();
  }
  return left.value + right.value - 4;
}
)");
  expect(nestedBranch.canGenerateCode() &&
             lang::verifyMirProgram(nestedBranch.mir).valid(),
         "nested branches preserve the exact outer HIR endpoint without "
         "inventing an intermediate semantic endpoint");
  const auto returningBranch = analyze("composed-carrier-returning-arm.gti", R"(
  if (step != 2) {
    return 0;
  } else {
    mut Context active = forward_context<Context>(std::move(initial));
    active.update();
  }
  return left.value + right.value - 4;
}
)");
  expect(returningBranch.canGenerateCode() &&
             lang::verifyMirProgram(returningBranch.mir).valid(),
         "only normally reaching branches contribute call-transfer endpoint "
         "acknowledgements at a join");
  const auto looped = lang::Frontend().analyze(
      "composed-carrier-loop-transfer.gti", declarations + R"(
int main() {
  int step = 2;
  mut Cell left{};
  mut Cell right{};
  for (mut int index = 0; index < 2; index++) {
    Context current{step, left, right};
    mut Context active = forward_context<Context>(std::move(current));
    active.update();
  }
  return left.value + right.value - 8;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(looped.canGenerateCode() && lang::verifyMirProgram(looped.mir).valid(),
         "a loop-local carrier transfer retires its exact loan on each "
         "iteration without leaking statement acknowledgement state");
  const auto reject = [&](const std::string &name, const std::string &body,
                          const std::string &code,
                          const std::string &extra = "") {
    const auto result = analyze(name, body, extra);
    expect(!result.canGenerateCode() &&
               hasDiagnosticCode(result.diagnostics, code) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           name + " remains a source ownership error");
  };
  for (const auto *owner : {"left", "right"})
    reject(std::string("composed-forwarding-live-") + owner + ".gti",
           forwarding + "  " + owner +
               ".value = 9;\n  active.update();\n  return 0;\n}\n",
           "GTI-S2017");
  reject("composed-forwarding-live-child.gti", R"(
  mut Cell& child = initial.left;
  mut Context active = forward_context<Context>(std::move(initial));
  child.value = 9;
  active.update();
  return 0;
}
)",
         "GTI-S2017");
  reject("composed-forwarding-cannot-copy.gti", R"(
  mut Context active = forward_context<Context>(initial);
  active.update();
  return 0;
}
)",
         "GTI-S2018");
  reject("composed-forwarding-cannot-transfer-twice.gti", forwarding + R"(
  Context duplicate = forward_context<Context>(std::move(initial));
  active.update();
  return 0;
}
)",
         "GTI-S2018");
  reject("composed-forwarding-cannot-move-reference.gti", R"(
  mut Context& alias = initial;
  mut Context active = forward_context<Context>(std::move(alias));
  active.update();
  return 0;
}
)",
         "GTI-S2018");
  reject("composed-forwarding-overlapping-sibling-argument.gti", R"(
  mut Context active = forward_with_owner(std::move(initial), left);
  active.update();
  return 0;
}
)",
         "GTI-S2017", R"(
Context forward_with_owner(Context context, mut Cell& other) {
  other.value += 1;
  return std::move(context);
}
)");
}

void testNestedBorrowedFields() {
  const auto arrowUpdate = lang::Frontend().analyze(
      "nested-carrier-arrow-update.gti", R"(
struct Record { mut int value = 7;  public: Record() = default; };
class Cursor {
  mut Record& record;
public:
  Cursor(mut Record& record) : record(record) {}
  mut Record& operator->() mut { return this.record; }
};
class Wrapper<T> {
public:
  mut T cursor;
  Wrapper(T cursor) : cursor(std::move(cursor)) {}
};
int main() {
  mut Record record{};
  { mut Wrapper<Cursor> wrapper{Cursor(record)}; wrapper.cursor->value += 2; }
  return record.value - 9;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(arrowUpdate.canGenerateCode() &&
             lang::verifyMirProgram(arrowUpdate.mir).valid(),
         "a scalar compound store must invoke the selected nested arrow "
         "projection");
  std::size_t arrowCalls = 0;
  bool checkedAddition = false;
  for (const auto &function : arrowUpdate.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::NoArguments) {
      continue;
    }
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *callee = instruction.functionTarget
                                 ? arrowUpdate.mir.findFunctionInstance(
                                       *instruction.functionTarget)
                                 : nullptr;
        if (callee &&
            callee->overloadedOperator == lang::OverloadedOperator::Arrow) {
          ++arrowCalls;
        }
        if (instruction.operation == lang::MirOperation::Add &&
            !instruction.definedFailure.localOrigins.empty()) {
          checkedAddition = true;
        }
      }
    }
  }
  expect(arrowCalls == 1 && checkedAddition,
         "arrow compound update evaluates its target once and retains checked "
         "arithmetic");
  const auto readonlyReturn = lang::Frontend().analyze(
      "mutable-carrier-shared-return.gti", R"(
class View {
  mut int& item;
public:
  View(mut int& value) : item(value) {}
  int& get() { return this.item; }
};
int main() { mut int owner = 42; View view{owner}; return view.get(); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(readonlyReturn.canGenerateCode() &&
             lang::verifyMirProgram(readonlyReturn.mir).valid(),
         "a mutable stored reference can publish a read-only receiver borrow");
  auto strongerReturn = readonlyReturn.mir;
  bool changedReturn = false;
  auto &returnFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      strongerReturn.functionInstances());
  for (auto &function : returnFunctions) {
    for (auto &loan : function.body.loans) {
      if (loan.kind == lang::MirLoanKind::Return &&
          loan.access == lang::AccessMode::ReadOnly) {
        loan.access = lang::AccessMode::Mutable;
        changedReturn = true;
      }
    }
  }
  expect(changedReturn && !lang::verifyMirProgram(strongerReturn).valid(),
         "a read-only result cannot publish stronger source access");
  const auto frontend = lang::Frontend().analyze(
      "nested-borrowed-fields.gti", R"(
class View {
public:
  int& item;
  View(int& value) : item(value) {}
  int& read() { return this.item; }
};
class Box<T> {
public:
  T value;
  bool inserted;
  Box(T value, bool inserted) : value(std::move(value)), inserted(inserted) {}
};
Box<View> make(int& owner) { return Box<View>(View(owner), true); }
int main() {
  int owner = 42;
  auto result = make(owner);
  auto copied = result;
  auto nested = Box<Box<View>>(std::move(result), false);
  return copied.value.read() + nested.value.value.read() - 84;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics) {
      std::cerr << "Nested-borrow diagnostic: " << diagnostic.code << ' '
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "nested one-owner fields must preserve shared aliases through "
         "construction, copying, moving and projected method calls");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto mutate = [&](const auto &edit) {
    auto program = frontend.mir;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            program.constructorInstances());
    bool changed = false;
    for (auto &constructor : constructors) {
      for (auto &loan : constructor.body.loans) {
        if (loan.entry && loan.kind == lang::MirLoanKind::Stored &&
            loan.designatedFields.size() == 2) {
          edit(constructor, loan);
          changed = true;
          break;
        }
      }
      if (changed) {
        break;
      }
    }
    expect(changed && !lang::verifyMirProgram(program).valid(),
           "nested carrier proof corruption must be rejected independently");
  };
  mutate([](auto &, auto &loan) { loan.designatedFields.pop_back(); });
  mutate([](auto &, auto &loan) {
    std::reverse(loan.designatedFields.begin(), loan.designatedFields.end());
  });
  mutate([](auto &, auto &loan) { loan.access = lang::AccessMode::Mutable; });
  mutate([](auto &constructor, auto &entry) {
    for (auto &loan : constructor.body.loans) {
      if (loan.kind == lang::MirLoanKind::Stored && loan.escapes) {
        loan.storedField = entry.designatedFields.back();
      }
    }
  });
  mutate([](auto &constructor, auto &entry) {
    for (auto &block : constructor.body.blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::EndBorrow &&
            instruction.loan == entry.id) {
          instruction.loan.reset();
        }
      }
    }
  });
}

void testProjectedAccessorReturnLoanOrder() {
  const auto stdlib = standardLibraryPrelude().parent_path();
  const auto frontend =
      lang::Frontend().analyze("projected-accessor-return.gti", R"(
import std.vector;
import std.memory;
struct Entry { mut int value; Entry(int v) : value(v) {} };
class Table {
  mut std::vector<std::unique_ptr<Entry>> entries{};
public:
  Table() { this.entries.push_back(std::make_unique<Entry>(3)); }
  int& read() { return this.entries[std::size_t(0)]->value; }
  mut int& write() mut { return this.entries[std::size_t(0)]->value; }
};
int main() {
  mut Table table{};
  { mut int& value = table.write(); value = 5; }
  return table.read() - 5;
}
)",
                               {standardLibraryPrelude()}, {}, {stdlib});
  expect(
      frontend.canGenerateCode() &&
          lang::verifyMirProgram(frontend.mir).valid(),
      "projected entry access must retire nested accessor loans child-first");
  bool mutated = false;
  lang::MirProgram invalid = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      invalid.functionInstances());
  for (auto &function : functions) {
    for (auto &block : function.body.blocks) {
      for (std::size_t index = 1; index < block.instructions.size(); ++index) {
        auto &childEnd = block.instructions[index - 1];
        auto &parentEnd = block.instructions[index];
        if (childEnd.kind != lang::MirInstructionKind::EndBorrow ||
            parentEnd.kind != lang::MirInstructionKind::EndBorrow ||
            !childEnd.loan || !parentEnd.loan) {
          continue;
        }
        const auto *child = function.body.findLoan(*childEnd.loan);
        if (child == nullptr || child->parent != *parentEnd.loan) {
          continue;
        }
        std::swap(childEnd.loan, parentEnd.loan);
        mutated = true;
        break;
      }
      if (mutated) {
        break;
      }
    }
    if (mutated) {
      break;
    }
  }
  expect(mutated && !lang::verifyMirProgram(invalid).valid(),
         "the verifier must reject reversing the child-first return cleanup");
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

void testReferenceArgumentRuntimePlaceCoherence() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "reference-argument-place-coherence.gti", R"(
import std.vector;
import std.iterator;

bool has_any<Iterator, Sentinel>(Iterator& first, Sentinel& last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> {
  return first != last;
}

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1, 2});
  auto first = values.begin();
  auto other = values.begin();
  auto last = values.end();
  return has_any(first, last) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "same-owner iterator arguments should lower to verified exact "
         "runtime places");
  if (!valid.canGenerateCode()) {
    return;
  }

  lang::MirProgram forged = valid.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forged.functionInstances());
  bool substituted = false;
  for (lang::MirFunctionInstance &function : functions) {
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputRole != lang::MirCallInputRole::Argument ||
            instruction.operands.size() != 1 ||
            (instruction.operands.front().kind != lang::MirOperandKind::Loan &&
             instruction.operands.front().kind !=
                 lang::MirOperandKind::BorrowRead &&
             instruction.operands.front().kind !=
                 lang::MirOperandKind::BorrowWrite) ||
            instruction.operands.front().place == 0 || !instruction.ownership) {
          continue;
        }
        lang::MirOperand &operand = instruction.operands.front();
        const lang::MirPlace *original = function.body.findPlace(operand.place);
        if (original == nullptr || !original->key) {
          continue;
        }
        const auto replacement = std::find_if(
            function.body.places.begin(), function.body.places.end(),
            [&](const lang::MirPlace &place) {
              return place.id != original->id && place.type == original->type &&
                     place.key && *place.key != *original->key;
            });
        if (replacement == function.body.places.end()) {
          continue;
        }
        operand.place = replacement->id;
        substituted = true;
        break;
      }
      if (substituted) {
        break;
      }
    }
    if (substituted) {
      break;
    }
  }
  const lang::MirVerificationResult verification =
      lang::verifyMirProgram(forged);
  if (substituted && !verification.valid() &&
      !hasMirVerificationError(verification,
                               "exact runtime carrier for its loan"))
    for (const auto &error : verification.errors)
      std::cerr << "Forged carrier diagnostic: " << error.message << '\n';
  expect(substituted && !verification.valid() &&
             hasMirVerificationError(verification,
                                     "exact runtime carrier for its loan"),
         "MIR verification should reject substituting a distinct same-typed "
         "iterator place that merely shares owner provenance");
}

void testUniqueOwnerSwap() {
  const lang::FrontendResult frontend =
      analyzeWithMemory("unique-owner-swap.gti", R"(
struct Value {
  int32_t number;
  Value(int32_t initial) : number(initial) {}
  int32_t read() { return this.number; }
};

int main() {
  mut std::unique_ptr<Value> first = std::make_unique<Value>(1);
  mut std::unique_ptr<Value> second = std::make_unique<Value>(2);
  first.swap(second);
  return first->read() == 2 and second->read() == 1 ? 0 : 1;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected unique-owner swap diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "unique_ptr::swap should preserve two matching mutable owners");

  lang::HirFunctionInstanceId functionId = 0;
  lang::MirBlockId blockId = 0;
  lang::MirInstructionId instructionId = 0;
  lang::MirPlaceId secondPlace = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::UniqueOwnerSwap ||
            instruction.operands.size() != 2) {
          continue;
        }
        functionId = function.id;
        blockId = block.id;
        instructionId = instruction.id;
        secondPlace = instruction.operands[1].place;
      }
    }
  }
  expect(functionId != 0 && blockId != 0 && instructionId != 0 &&
             secondPlace != 0,
         "MIR should retain the exact two-place unique-owner swap");
  expect(lang::effects(lang::IntrinsicKind::UniqueOwnerSwap).readsPlace &&
             lang::effects(lang::IntrinsicKind::UniqueOwnerSwap).writesPlace &&
             lang::effects(lang::IntrinsicKind::UniqueOwnerSwap).movesValue,
         "unique-owner swap effects should retain its non-copying mutation");

  for (int mutation = 0; mutation < 3; ++mutation) {
    auto malformed = frontend.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(functionId));
    lang::MirBlock *block =
        function == nullptr ? nullptr : function->body.findBlock(blockId);
    auto instruction =
        block == nullptr ? std::vector<lang::MirInstruction>::iterator{}
                         : std::find_if(block->instructions.begin(),
                                        block->instructions.end(),
                                        [&](const auto &candidate) {
                                          return candidate.id == instructionId;
                                        });
    expect(block != nullptr && instruction != block->instructions.end(),
           "unique-owner swap mutation should find its MIR instruction");
    if (block == nullptr || instruction == block->instructions.end()) {
      continue;
    }
    if (mutation == 0) {
      instruction->parameterTypes[1].referenceAccess =
          lang::AccessMode::ReadOnly;
    } else if (mutation == 1) {
      instruction->result = 1;
    } else {
      lang::MirPlace *place =
          const_cast<lang::MirPlace *>(function->body.findPlace(secondPlace));
      expect(place != nullptr,
             "unique-owner swap mutation should find its second place");
      if (place != nullptr) {
        place->access = lang::AccessMode::ReadOnly;
      }
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "MIR should reject a read-only, value-producing, or immutable "
           "unique-owner swap");
  }

  bool swapWithoutReplacementCleanup = false;
  if (const lang::MirFunctionInstance *function =
          frontend.mir.findFunctionInstance(functionId)) {
    for (const lang::MirBlock &block : function->body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        if (instruction.id == instructionId)
          swapWithoutReplacementCleanup = !instruction.replacementCleanup &&
                                          instruction.replacementPair == 0 &&
                                          !instruction.elementCleanup;
  }
  expect(swapWithoutReplacementCleanup && nativeBackendEmits(frontend),
         "the backend should lower unique-owner swap as one exchange without "
         "replacement cleanup");

  const lang::FrontendResult invalid =
      analyzeWithMemory("invalid-unique-owner-swap.gti", R"(
struct Left { public: Left() = default; };
struct Right { public: Right() = default; };
int main() {
  mut std::unique_ptr<Left> left = std::make_unique<Left>();
  mut std::unique_ptr<Right> right = std::make_unique<Right>();
  left.swap(right);
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "unique_ptr::swap should reject owners with different pointee "
         "types before MIR");
}

void testPolymorphicUniqueOwnerUpcast() {
  const lang::FrontendResult frontend =
      analyzeWithMemory("polymorphic-unique-owner-upcast.gti", R"(
[[transfer, share]] interface Layer {
  uint64_t id();
  void update() mut;
};

class DemoLayer : public Layer {
  mut uint64_t updates = 0;

public:
  DemoLayer() {}
  uint64_t id() override { return 7; }
  void update() mut override { this.updates++; }
};

class OtherLayer : public Layer {
  mut uint64_t updates = 0;

public:
  OtherLayer() {}
  uint64_t id() override { return 8; }
  void update() mut override { this.updates++; }
};

int main() {
  mut std::unique_ptr<Layer> layer =
      std::upcast_unique<Layer, DemoLayer>(
          std::make_unique<DemoLayer>());
  mut std::unique_ptr<Layer> other =
      std::upcast_unique<Layer, OtherLayer>(
          std::make_unique<OtherLayer>());
  layer->update();
  other->update();
  return layer->id() == 7 and other->id() == 8 ? 0 : 1;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected unique-owner upcast diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "an explicit unique owner upcast should preserve one polymorphic "
         "owner through MIR");

  const bool hirUpcast = std::any_of(
      frontend.hir.functionInstances().begin(),
      frontend.hir.functionInstances().end(),
      [](const lang::HirFunctionInstance &function) {
        return std::any_of(
            function.body.values.begin(), function.body.values.end(),
            [](const lang::HirValue &value) {
              return value.intrinsic == lang::IntrinsicKind::UniqueOwnerUpcast;
            });
      });
  const bool mirUpcast = std::any_of(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return std::any_of(
            function.body.blocks.begin(), function.body.blocks.end(),
            [](const lang::MirBlock &block) {
              return std::any_of(
                  block.instructions.begin(), block.instructions.end(),
                  [](const lang::MirInstruction &instruction) {
                    return instruction.intrinsic ==
                           lang::IntrinsicKind::UniqueOwnerUpcast;
                  });
            });
      });
  expect(hirUpcast && mirUpcast,
         "HIR and MIR should retain the trusted ownership-preserving upcast "
         "identity");

  lang::MirInstructionId upcastInstruction = 0;
  lang::HirFunctionInstanceId upcastFunction = 0;
  lang::MirBlockId upcastBlock = 0;
  lang::HirClassInstanceId sourcePointeeInstance = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::UniqueOwnerUpcast ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().type.arguments.size() != 1) {
          continue;
        }
        upcastInstruction = instruction.id;
        upcastFunction = function.id;
        upcastBlock = block.id;
        const lang::SemanticType &pointee =
            instruction.operands.front().type.arguments.front();
        for (const lang::MirClassInstance &instance :
             frontend.mir.classInstances()) {
          if (instance.type == pointee) {
            sourcePointeeInstance = instance.id;
          }
        }
        break;
      }
      if (upcastInstruction != 0) {
        break;
      }
    }
    if (upcastInstruction != 0) {
      break;
    }
  }
  expect(upcastInstruction != 0 && upcastFunction != 0 && upcastBlock != 0 &&
             sourcePointeeInstance != 0,
         "unique-owner upcast test should retain its concrete source fact");
  if (sourcePointeeInstance != 0) {
    auto malformed = frontend.mir;
    auto *source = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(sourcePointeeInstance));
    source->traits.transferCapable = false;
    source->traits.shareCapable = false;
    expect(!lang::verifyMirProgram(malformed).valid(),
           "MIR should reject unique-owner erasure that amplifies concurrency "
           "capabilities");
  }
  for (bool retarget : {false, true}) {
    auto malformed = frontend.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(upcastFunction));
    lang::MirBlock *block =
        function == nullptr ? nullptr : function->body.findBlock(upcastBlock);
    auto instruction =
        block == nullptr
            ? std::vector<lang::MirInstruction>::iterator{}
            : std::find_if(block->instructions.begin(),
                           block->instructions.end(), [&](const auto &value) {
                             return value.id == upcastInstruction;
                           });
    expect(block != nullptr && instruction != block->instructions.end(),
           "unique upcast mutation should find its MIR instruction");
    if (block == nullptr || instruction == block->instructions.end()) {
      continue;
    }
    const auto activation = std::find_if(
        instruction->lifecycle.begin(), instruction->lifecycle.end(),
        [](const auto &event) {
          return event.kind == lang::MirLifecycleEventKind::Initialize;
        });
    expect(activation != instruction->lifecycle.end(),
           "unique upcast should activate its result obligation");
    if (activation == instruction->lifecycle.end()) {
      continue;
    }
    if (retarget) {
      activation->target = 0;
    } else {
      instruction->lifecycle.erase(activation);
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "MIR should reject a missing or retargeted unique-upcast result "
           "activation");
  }

  // Each concrete owner selects its own source upcast member; the
  // interface keeps polymorphic destruction through the erased owner.
  std::vector<std::string> upcastOwners;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    const lang::MirClassInstance *owner =
        function.owner ? frontend.mir.findClassInstance(*function.owner)
                       : nullptr;
    if (source == nullptr || source->source == nullptr ||
        source->source->name().lexeme != "upcast" || owner == nullptr ||
        owner->type.arguments.size() != 1 || !hasVerifiedSourceBody(&function))
      continue;
    const lang::ClassTypeInfo *pointee =
        frontend.semantics.findClassType(owner->type.arguments.front().classId);
    if (pointee != nullptr)
      upcastOwners.push_back(pointee->qualifiedName);
  }
  std::sort(upcastOwners.begin(), upcastOwners.end());
  const bool polymorphicInterface = std::any_of(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(), [&](const auto &instance) {
        const lang::ClassTypeInfo *info =
            frontend.semantics.findClassType(instance.type.classId);
        return info != nullptr && info->qualifiedName == "Layer" &&
               instance.polymorphic;
      });
  expect(upcastOwners == std::vector<std::string>{"DemoLayer", "OtherLayer"} &&
             polymorphicInterface && nativeBackendEmits(frontend),
         "the backend should lower the private owner conversion and preserve "
         "polymorphic destruction through one selected method per concrete "
         "owner");

  const lang::FrontendResult invalid =
      analyzeWithMemory("invalid-polymorphic-unique-owner-upcast.gti", R"(
class PlainBase { public: PlainBase() = default; };
class PlainDerived : public PlainBase { public: PlainDerived() = default; };
class Unrelated { public: Unrelated() = default; };
class CapableBase { public: virtual int read() { return 1; }  public: CapableBase() = default; };
[[no_transfer]] class NonTransferable : public CapableBase {
public: int read() override { return 2; }
public:
  NonTransferable() = default;
};
[[no_share]] class NonShareable : public CapableBase {
public: int read() override { return 3; }
public:
  NonShareable() = default;
};
class CleanupBound : public CapableBase {
public:
  ~CleanupBound() {}
  int read() override { return 4; }
public:
  CleanupBound() = default;
};

int main() {
  [[discard]] std::upcast_unique<PlainBase, PlainDerived>(
      std::make_unique<PlainDerived>());
  [[discard]] std::upcast_unique<PlainBase, Unrelated>(
      std::make_unique<Unrelated>());
  [[discard]] std::upcast_unique<CapableBase, NonTransferable>(
      std::make_unique<NonTransferable>());
  [[discard]] std::upcast_unique<CapableBase, NonShareable>(
      std::make_unique<NonShareable>());
  [[discard]] std::upcast_unique<CapableBase, CleanupBound>(
      std::make_unique<CleanupBound>());
  return 0;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "must have polymorphic destruction") &&
             hasDiagnostic(invalid.diagnostics,
                           "the target must be a public base") &&
             hasDiagnostic(invalid.diagnostics,
                           "erasing the concrete type would grant transfer") &&
             hasDiagnostic(invalid.diagnostics,
                           "erasing the concrete type would grant share"),
         "unique-owner upcasts should reject unsafe destruction and "
         "unrelated or capability-amplifying target types before backend "
         "generation");

  const lang::FrontendResult implicit =
      analyzeWithMemory("implicit-polymorphic-owner-conversion.gti", R"(
interface Layer { void update() mut; };
class DemoLayer : public Layer {
public:
  void update() mut override {}
public:
  DemoLayer() = default;
};
void consume(std::unique_ptr<Layer> layer) {}
int main() {
  consume(std::make_unique<DemoLayer>());
  return 0;
}
)");
  expect(!implicit.canGenerateCode() &&
             hasDiagnostic(implicit.diagnostics,
                           "parameter requires 'std::unique_ptr<Layer>'"),
         "owner upcasts should remain explicit and must not enter ordinary "
         "call overload resolution");
}

void testAggregateOwnershipTraits() {
  const std::string source = R"(
class Buffer<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;
  uint64_t reserved;

public:
  Buffer(uint64_t capacity)
      : data(std::detail::prefix_owner<T>(capacity)), reserved(capacity) {}

  uint64_t capacity() {
    return this.reserved;
  }
};

class NestedBuffer {
  Buffer<int> buffer;

public:
  NestedBuffer(uint64_t capacity) : buffer(Buffer<int>(capacity)) {}

  uint64_t capacity() {
    return this.buffer.capacity();
  }
};

struct CopyableValue {
  int value = 1;
public:
  CopyableValue() = default;
};

Buffer<int> transfer(Buffer<int> value) {
  return std::move(value);
}

NestedBuffer transfer_nested(NestedBuffer value) {
  return std::move(value);
}

uint64_t inspect(Buffer<int>& value) {
  return value.capacity();
}

CopyableValue copy_value(CopyableValue value) {
  return value;
}

int main() {
  Buffer<int> buffer = Buffer<int>(uint64_t(2));
  Buffer<int> moved = transfer(std::move(buffer));
  NestedBuffer nested = NestedBuffer(uint64_t(3));
  NestedBuffer moved_nested = transfer_nested(std::move(nested));
  CopyableValue value = CopyableValue();
  CopyableValue copied = copy_value(value);
  return int(inspect(moved) + moved_nested.capacity()) - 5 + copied.value - 1;
}
)";

  lang::FrontendResult frontend = gti_test::analyzeTrustedPrivateMemoryFixture(
      "aggregate-ownership.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected aggregate-owner diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(
      frontend.canGenerateCode(),
      "classes should inherit move-only traits from direct and nested fields");

  const lang::ClassDecl *bufferClass =
      findTopLevelClass(frontend.program, "Buffer");
  const lang::ClassDecl *copyableClass =
      findTopLevelClass(frontend.program, "CopyableValue");
  const lang::FunctionDecl *transfer =
      findTopLevelFunction(frontend.program, "transfer");
  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *buffer = main == nullptr
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().at(0).get());
  const auto *movedNested = main == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::VariableDecl *>(
                                      main->body()->statements().at(3).get());
  const auto *copied = main == nullptr
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().at(5).get());
  const lang::BindingInfo *parameter =
      transfer == nullptr
          ? nullptr
          : frontend.semantics.findBinding(transfer->parameters().front());
  const lang::BindingInfo *bufferBinding =
      buffer == nullptr ? nullptr : frontend.semantics.findBinding(*buffer);
  const lang::BindingInfo *nestedBinding =
      movedNested == nullptr ? nullptr
                             : frontend.semantics.findBinding(*movedNested);
  const lang::BindingInfo *copyableBinding =
      copied == nullptr ? nullptr : frontend.semantics.findBinding(*copied);
  const lang::ClassLifecycleInfo *bufferLifecycle =
      bufferClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*bufferClass);
  const lang::ClassLifecycleInfo *copyableLifecycle =
      copyableClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*copyableClass);
  expect(parameter != nullptr && bufferBinding != nullptr &&
             nestedBinding != nullptr && copyableBinding != nullptr &&
             parameter->traits.ownership == lang::OwnershipKind::Value &&
             !parameter->traits.copyable && parameter->traits.movable &&
             parameter->traits.drop == lang::DropKind::Lexical &&
             !bufferBinding->traits.copyable && bufferBinding->traits.movable &&
             nestedBinding->traits.ownership == lang::OwnershipKind::Value &&
             !nestedBinding->traits.copyable && nestedBinding->traits.movable &&
             copyableBinding->traits.copyable &&
             copyableBinding->traits.movable,
         "ordinary source values recursively retain move-only lexical cleanup");
  expect(bufferLifecycle != nullptr && copyableLifecycle != nullptr &&
             bufferLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Deleted &&
             bufferLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             bufferLifecycle->copyAssignment ==
                 lang::SpecialMemberStatus::Deleted &&
             bufferLifecycle->moveAssignment ==
                 lang::SpecialMemberStatus::Generated &&
             copyableLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             copyableLifecycle->moveAssignment ==
                 lang::SpecialMemberStatus::Generated,
         "class lifecycle metadata should derive special-member availability "
         "from field ownership");

  std::vector<lang::HirConstructorInstanceId> ownerConstructors;
  for (const auto &constructor : frontend.hir.constructorInstances())
    if (constructor.source != nullptr &&
        (constructor.source->name().lexeme == "Buffer" ||
         constructor.source->name().lexeme == "NestedBuffer"))
      ownerConstructors.push_back(constructor.id);
  expect(
      ownerConstructors.size() == 2 &&
          std::all_of(ownerConstructors.begin(), ownerConstructors.end(),
                      [&](auto id) {
                        const lang::MirConstructorInstance *constructor =
                            frontend.mir.findConstructorInstance(id);
                        return constructor != nullptr &&
                               constructor->definitionKind ==
                                   lang::MirDefinitionKind::Source &&
                               !constructor->body.blocks.empty() &&
                               lang::verifyMirBody(constructor->body).valid();
                      }),
      "both concrete owner constructors retain their exact MIR body "
      "identity independently of imported library constructor numbering");
  // The failure-free transfers move their parameter out rather than copy
  // it; the move-only aggregate keeps generated moves and deleted copies.
  const auto failureFreeTransfer = [&](std::string_view name) {
    const lang::MirFunctionInstance *function =
        mirFunctionNamed(frontend, name);
    return hasVerifiedSourceBody(function) &&
           !function->mayRaiseDefinedFailure &&
           mirMovesBinding(frontend, *function, "value");
  };
  const bool moveOnlyBuffer = std::any_of(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(), [&](const auto &instance) {
        const lang::ClassTypeInfo *info =
            frontend.semantics.findClassType(instance.type.classId);
        return info != nullptr && info->qualifiedName == "Buffer" &&
               instance.copyConstructor == lang::SpecialMemberStatus::Deleted &&
               instance.moveConstructor ==
                   lang::SpecialMemberStatus::Generated &&
               instance.copyAssignment == lang::SpecialMemberStatus::Deleted &&
               instance.moveAssignment == lang::SpecialMemberStatus::Generated;
      });
  const lang::MirFunctionInstance *aggregateMain =
      mirFunctionNamed(frontend, "main");
  expect(mirClassInstanceCount(frontend, "Buffer") == 1 &&
             failureFreeTransfer("transfer") &&
             failureFreeTransfer("transfer_nested") && moveOnlyBuffer &&
             aggregateMain != nullptr &&
             aggregateMain->mayRaiseDefinedFailure &&
             nativeBackendEmits(frontend),
         "the backend should keep immutable move-only aggregates transferable");

  const lang::FrontendResult invalid =
      gti_test::analyzeTrustedPrivateMemoryFixture(
          "invalid-aggregate-ownership.gti", R"(
class Buffer {
  mut std::detail::prefix_owner<int> data;

public:
  Buffer() : data(std::detail::prefix_owner<int>(uint64_t(1))) {}
};

class NestedBuffer {
  Buffer buffer;

public:
  NestedBuffer() : buffer(Buffer()) {}
};

void consume(Buffer value) {}

Buffer return_copy(Buffer value) {
  return value;
}

int main() {
  Buffer owner = Buffer();
  Buffer copied = owner;
  consume(owner);
  Buffer moved = std::move(owner);
  Buffer moved_again = std::move(owner);

  NestedBuffer nested = NestedBuffer();
  NestedBuffer nested_copy = nested;
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "copying or reusing move-only aggregates should fail semantically");
  expect(
      hasDiagnosticCode(invalid.diagnostics, "GTI-S2018") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'copied'") &&
          hasDiagnostic(invalid.diagnostics, "would copy a move-only value") &&
          hasDiagnostic(invalid.diagnostics, "has already been moved") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'nested_copy'"),
      "aggregate diagnostics should cover calls, copies, nesting, and use "
      "after move while accepting the exact named return");
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

void testDestructorsAndActiveDropState() {
  const std::string source = R"(
class CleanupBuffer<std::storage_element T> {
  mut std::detail::prefix_owner<T> data;
  mut uint64_t count = 0;

public:
  CleanupBuffer(uint64_t capacity)
      : data(std::detail::prefix_owner<T>(capacity)) {}

  ~CleanupBuffer() {
    while (this.count > 0) {
      this.count--;
      this.data.pop();
    }
  }

  void push(T value) mut {
    this.data.append_value(std::move(value));
    this.count++;
  }
};

CleanupBuffer<int> transfer(CleanupBuffer<int> value) {
  return std::move(value);
}

int main() {
  mut CleanupBuffer<int> values = CleanupBuffer<int>(uint64_t(2));
  values.push(7);
  CleanupBuffer<int> moved = transfer(std::move(values));
  return 0;
}
)";

  lang::FrontendResult frontend =
      gti_test::analyzeTrustedPrivateMemoryFixture("destructor.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected destructor diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "public destructors should support mutable vector-style cleanup");

  const lang::ClassDecl *cleanupClass =
      findTopLevelClass(frontend.program, "CleanupBuffer");
  const auto *destructor = cleanupClass == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::DestructorDecl *>(
                                     cleanupClass->members().at(4).get());
  const lang::ClassLifecycleInfo *lifecycle =
      cleanupClass == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*cleanupClass);
  expect(destructor != nullptr && destructor->name().lexeme == "CleanupBuffer",
         "the parser should retain destructor identity and body structure");
  expect(
      lifecycle != nullptr && lifecycle->declaredDestructor &&
          lifecycle->declaredDestructor->declaration == destructor &&
          lifecycle->destructor == lang::SpecialMemberStatus::Declared &&
          lifecycle->requiresActiveDropState &&
          lifecycle->copyConstructor == lang::SpecialMemberStatus::Deleted &&
          lifecycle->copyAssignment == lang::SpecialMemberStatus::Deleted &&
          lifecycle->moveConstructor == lang::SpecialMemberStatus::Generated &&
          lifecycle->moveAssignment == lang::SpecialMemberStatus::Generated &&
          !lifecycle->traits.copyable && lifecycle->traits.movable,
      "declared cleanup should be explicit, noncopyable, and safely movable");

  const lang::HirDestructorInstance *hirDestructor = nullptr;
  for (const lang::HirDestructorInstance &instance :
       frontend.hir.destructorInstances()) {
    if (instance.source == destructor) {
      hirDestructor = &instance;
      break;
    }
  }
  const lang::HirStatement *cleanupLoop =
      hirDestructor == nullptr || hirDestructor->body.roots.empty()
          ? nullptr
          : hirDestructor->body.findStatement(
                hirDestructor->body.roots.front());
  expect(hirDestructor != nullptr && cleanupLoop != nullptr &&
             cleanupLoop->kind == lang::HirStatementKind::While &&
             cleanupLoop->condition.has_value() &&
             cleanupLoop->body.has_value(),
         "typed HIR should retain concrete destructor control flow");

  const lang::ClassTypeInfo *cleanupType =
      cleanupClass == nullptr ? nullptr
                              : frontend.semantics.findClassType(*cleanupClass);
  bool guardedCleanupClass = false;
  bool sourceDestructorBody = false;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (cleanupType == nullptr ||
        instance.type.kind != lang::SemanticType::Class ||
        instance.type.classId != cleanupType->id)
      continue;
    guardedCleanupClass =
        instance.requiresActiveDropState && instance.destructor &&
        instance.destructorStatus == lang::SpecialMemberStatus::Declared &&
        instance.copyConstructor == lang::SpecialMemberStatus::Deleted &&
        instance.moveConstructor == lang::SpecialMemberStatus::Generated;
    const lang::MirDestructorInstance *cleanup =
        instance.destructor
            ? frontend.mir.findDestructorInstance(*instance.destructor)
            : nullptr;
    sourceDestructorBody =
        cleanup != nullptr &&
        cleanup->definitionKind == lang::MirDefinitionKind::Source &&
        !cleanup->body.blocks.empty() &&
        lang::verifyMirBody(cleanup->body).valid();
  }
  expect(
      guardedCleanupClass && sourceDestructorBody &&
          hasMirIntrinsicKind(frontend, lang::IntrinsicKind::ObjectDestroy) &&
          nativeBackendEmits(frontend),
      "the backend should guard native retirement with the same "
      "initialized active drop state while retaining declared cleanup and "
      "safe moves");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-destructor.gti", R"(
class WrongName {
public:
  ~Other() {}
public:
  WrongName() = default;
};

class DuplicateCleanup {
public:
  ~DuplicateCleanup() {}
  ~DuplicateCleanup() {}
public:
  DuplicateCleanup() = default;
};

class PrivateCleanup {
  ~PrivateCleanup() {}
public:
  PrivateCleanup() = default;
};

class ReturningCleanup {
public:
  ~ReturningCleanup() { return; }
public:
  ReturningCleanup() = default;
};

int main() { return 0; }
)");
  expect(!invalid.canGenerateCode(),
         "invalid destructor declarations should fail in the frontend");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-S2021") &&
             hasDiagnostic(invalid.diagnostics, "Destructor name must match") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot declare more than one destructor") &&
             hasDiagnostic(invalid.diagnostics, "Destructors must be public") &&
             hasDiagnostic(invalid.diagnostics,
                           "Destructors cannot contain return statements"),
         "destructor diagnostics should cover identity, uniqueness, access, "
         "and control flow");

  lang::Lexer lexer;
  lang::Parser parameterParser(lexer.scan(R"(
class ParameterizedCleanup {
public:
  ~ParameterizedCleanup(int value) {}
};
int main() { return 0; }
)"));
  const lang::Program recovered = parameterParser.parse();
  const auto *recoveredMain = recovered.declarations().empty()
                                  ? nullptr
                                  : dynamic_cast<const lang::FunctionDecl *>(
                                        recovered.declarations().back().get());
  expect(parameterParser.hadError() &&
             hasDiagnostic(parameterParser.errors(),
                           "Destructors do not take parameters") &&
             recoveredMain != nullptr && recoveredMain->name().lexeme == "main",
         "destructor parameter errors should recover to later declarations");

  const std::string formatted = lang::Formatter().format(
      "class Trace{mut int state=1;public:~Trace(){while(this.state>0){"
      "this.state--;}} public: Trace() = default; };");
  expect(formatted.find("~Trace() {") != std::string::npos &&
             formatted.find("while (this.state > 0) {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "destructor syntax should format with stable C++-style layout");

  const lang::FrontendResult reservedName = lang::Frontend().analyze(
      "reserved-lifecycle-name.gti",
      "class Collision { mut bool __gti_lifecycle_active_1 = true; };");
  expect(!reservedName.canGenerateCode() &&
             hasDiagnosticCode(reservedName.diagnostics, "GTI-L0008"),
         "source identifiers must not collide with generated lifecycle names");
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

void testConsumingCallableOperators() {
  const std::string source = R"(
class OnceValue {
  mut int value;

public:
  OnceValue(int initial) : value(initial) {}

  int operator()() && {
    return std::move(this.value);
  }
};

class ReadValue {
  int value;

public:
  ReadValue(int initial) : value(initial) {}
  int operator()() { return this.value; }
};

class MutableValue {
  mut int value;

public:
  MutableValue(int initial) : value(initial) {}
  int operator()() mut {
    this.value++;
    return this.value;
  }
};

class DualValue {
  mut int value;

public:
  DualValue(int initial) : value(initial) {}
  int operator()() { return this.value; }
  int operator()() && { return std::move(this.value); }
};

class MutableFallback {
  mut int value;

public:
  MutableFallback(int initial) : value(initial) {}
  int operator()(int amount) { return this.value - amount; }
  int operator()(int amount) mut {
    this.value += amount;
    return this.value;
  }
  int operator()() && { return std::move(this.value); }
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)();
}

int invoke_once_with<Operation>(Operation operation, int amount) {
  return std::move(operation)(amount);
}

int relay_once<Operation>(Operation operation) {
  return invoke_once(std::move(operation));
}

int choose_relay_once<Operation>(Operation operation, bool first) {
  if (first) {
    return invoke_once(std::move(operation));
  }
  return invoke_once(std::move(operation));
}

int choose_once<Operation>(Operation operation, bool first) {
  if (first) {
    return std::move(operation)();
  }
  return std::move(operation)();
}

int main() {
  OnceValue once = OnceValue(7);
  ReadValue read = ReadValue(3);
  MutableValue mutate = MutableValue(4);
  DualValue dual = DualValue(6);
  MutableFallback fallback = MutableFallback(4);
  MutableFallback generic_fallback = MutableFallback(8);
  int consumed = invoke_once(once);
  int observed = invoke_once(read);
  int changed = invoke_once(mutate);
  int selected = choose_once(OnceValue(2), false);
  int relayed = relay_once(OnceValue(5));
  int branch_relayed = choose_relay_once(OnceValue(6), true);
  int peeked = dual();
  int taken = std::move(dual)();
  int changed_after_move = std::move(fallback)(3);
  int generic_changed_after_move = invoke_once_with(generic_fallback, 2);
  return consumed + observed + changed + selected + relayed + branch_relayed +
             peeked + taken + changed_after_move + generic_changed_after_move -
         57;
}
)";

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "consuming-callables.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected consuming-callable diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "operator() && and confined once-callable clients should lower");

  const lang::LanguageQueries queries;
  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const std::size_t consumingOperator = source.find("operator()() &&");
  const std::optional<lang::HoverInfo> consumingHover = queries.hover(
      frontend, unit, consumingOperator + std::string("operator").size());
  expect(consumingHover && consumingHover->signature.find("operator()() &&") !=
                               std::string::npos,
         "hover should preserve the selected consuming receiver qualifier");

  const std::size_t invokeDeclaration = source.find("invoke_once<");
  const std::optional<lang::HoverInfo> invokeHover =
      queries.hover(frontend, unit, invokeDeclaration + 1);
  expect(invokeHover &&
             std::any_of(invokeHover->notes.begin(), invokeHover->notes.end(),
                         [](const std::string &note) {
                           return note.find("once-callable") !=
                                  std::string::npos;
                         }),
         "hover should expose the once-callable generic contract");

  const lang::FunctionDecl *invoke =
      findTopLevelFunction(frontend.program, "invoke_once");
  const lang::FunctionInfo *invokeInfo =
      invoke == nullptr ? nullptr : frontend.semantics.findFunction(*invoke);
  expect(invokeInfo != nullptr && invokeInfo->callableParameters.size() == 1 &&
             invokeInfo->callableParameters.front().signatures.size() == 1 &&
             invokeInfo->callableParameters.front()
                     .signatures.front()
                     .capability == lang::CallableInvocationCapability::Once,
         "semantic callable contracts should classify explicit consumption "
         "as once-callable use");

  bool selectedOnce = false;
  bool selectedRead = false;
  bool selectedMutable = false;
  bool mirOnceReceiverMoved = false;
  bool hirOnceReceiverScheduled = false;
  bool mirOnceReceiverScheduled = false;
  bool hirMovedFallbackScheduled = false;
  bool mirMovedFallbackScheduled = false;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "invoke_once" ||
        instance.callableParameters.size() != 1 ||
        instance.callableParameters.front().signatures.size() != 1) {
      continue;
    }
    const lang::HirCallableSignature &signature =
        instance.callableParameters.front().signatures.front();
    selectedOnce = selectedOnce || signature.selectedCapability ==
                                       lang::CallableInvocationCapability::Once;
    selectedRead = selectedRead || signature.selectedCapability ==
                                       lang::CallableInvocationCapability::Read;
    selectedMutable =
        selectedMutable || signature.selectedCapability ==
                               lang::CallableInvocationCapability::Mutable;
    for (const lang::HirValue &value : instance.body.values) {
      hirOnceReceiverScheduled =
          hirOnceReceiverScheduled ||
          (value.functionTarget &&
           value.callableInvocation ==
               lang::CallableInvocationCapability::Once &&
           value.callPlan && value.callPlan->receiver &&
           value.callPlan->receiver->kind == lang::HirCallInputKind::MoveValue);
      hirMovedFallbackScheduled =
          hirMovedFallbackScheduled ||
          (value.functionTarget &&
           value.callableInvocation ==
               lang::CallableInvocationCapability::Mutable &&
           value.callPlan && value.callPlan->receiver &&
           value.callPlan->receiver->kind == lang::HirCallInputKind::MoveValue);
    }
    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    if (mir == nullptr) {
      continue;
    }
    for (const lang::MirBlock &block : mir->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.callableBoundary == lang::CallableBoundary::Confined &&
            instruction.receiver &&
            instruction.receiver->kind == lang::MirOperandKind::Value) {
          const lang::MirValue *receiver =
              mir->body.findValue(instruction.receiver->value);
          const lang::MirBlock *definitionBlock =
              receiver == nullptr
                  ? nullptr
                  : mir->body.findBlock(receiver->definitionBlock);
          const lang::MirInstruction *definition =
              definitionBlock == nullptr
                  ? nullptr
                  : [&]() -> const lang::MirInstruction * {
            const auto found =
                std::find_if(definitionBlock->instructions.begin(),
                             definitionBlock->instructions.end(),
                             [&](const lang::MirInstruction &candidate) {
                               return candidate.id == receiver->definition;
                             });
            return found == definitionBlock->instructions.end() ? nullptr
                                                                : &*found;
          }();
          if (definition != nullptr &&
              definition->kind == lang::MirInstructionKind::CallInput &&
              definition->operands.size() == 1 &&
              definition->operands.front().kind ==
                  lang::MirOperandKind::Value) {
            const lang::MirValue *preparedSource =
                mir->body.findValue(definition->operands.front().value);
            const lang::MirBlock *sourceBlock =
                preparedSource == nullptr
                    ? nullptr
                    : mir->body.findBlock(preparedSource->definitionBlock);
            if (sourceBlock != nullptr) {
              const auto found = std::find_if(
                  sourceBlock->instructions.begin(),
                  sourceBlock->instructions.end(),
                  [&](const lang::MirInstruction &candidate) {
                    return candidate.id == preparedSource->definition;
                  });
              definition =
                  found == sourceBlock->instructions.end() ? nullptr : &*found;
            }
          }
          mirOnceReceiverMoved =
              mirOnceReceiverMoved ||
              (definition != nullptr &&
               definition->kind == lang::MirInstructionKind::Move &&
               definition->ownership &&
               definition->ownership->kind == lang::OwnershipEventKind::Move);
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget && instruction.callableInvocation &&
            instruction.receiver &&
            instruction.receiver->kind == lang::MirOperandKind::Value) {
          const lang::MirValue *receiver =
              mir->body.findValue(instruction.receiver->value);
          const lang::MirBlock *definitionBlock =
              receiver == nullptr
                  ? nullptr
                  : mir->body.findBlock(receiver->definitionBlock);
          const lang::MirInstruction *definition = nullptr;
          if (definitionBlock != nullptr) {
            const auto found =
                std::find_if(definitionBlock->instructions.begin(),
                             definitionBlock->instructions.end(),
                             [&](const lang::MirInstruction &candidate) {
                               return candidate.id == receiver->definition;
                             });
            definition = found == definitionBlock->instructions.end() ? nullptr
                                                                      : &*found;
          }
          mirOnceReceiverScheduled =
              mirOnceReceiverScheduled ||
              (instruction.callableInvocation ==
                   lang::CallableInvocationCapability::Once &&
               definition != nullptr &&
               definition->kind == lang::MirInstructionKind::CallInput &&
               definition->callInputRole == lang::MirCallInputRole::Receiver &&
               definition->callInputKind == lang::HirCallInputKind::MoveValue);
          mirMovedFallbackScheduled =
              mirMovedFallbackScheduled ||
              (instruction.callableInvocation ==
                   lang::CallableInvocationCapability::Mutable &&
               definition != nullptr &&
               definition->kind == lang::MirInstructionKind::CallInput &&
               definition->callInputRole == lang::MirCallInputRole::Receiver &&
               definition->callInputKind == lang::HirCallInputKind::MoveValue);
        }
      }
    }
  }
  expect(selectedOnce && selectedRead && selectedMutable &&
             mirOnceReceiverMoved && hirOnceReceiverScheduled &&
             mirOnceReceiverScheduled && hirMovedFallbackScheduled &&
             mirMovedFallbackScheduled &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "concrete HIR/MIR should retain once requirements, compatible "
         "selected capabilities, the proving ownership move, and ordered "
         "move checkpoints for consuming and explicitly moved fallback "
         "receivers");

  std::array<bool, 3> callReceivers{};
  std::array<bool, 3> invokedCapabilities{};
  bool failingCallOperator = false;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.overloadedOperator == lang::OverloadedOperator::Call &&
        entryFunction(frontend, function.id)) {
      callReceivers[static_cast<std::size_t>(function.receiverMutability)] =
          true;
      failingCallOperator =
          failingCallOperator || function.mayRaiseDefinedFailure;
    }
    for (const lang::MirBlock &block : function.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.callableInvocation)
          invokedCapabilities[static_cast<std::size_t>(
              *instruction.callableInvocation)] = true;
  }
  expect(std::all_of(callReceivers.begin(), callReceivers.end(),
                     [](bool found) { return found; }) &&
             std::all_of(invokedCapabilities.begin(), invokedCapabilities.end(),
                         [](bool found) { return found; }) &&
             failingCallOperator && nativeBackendEmits(frontend),
         "MIR should preserve read-only, mutable, and consuming call "
         "operators and the exact frontend-selected callable capability");

  const std::string formatted =
      lang::Formatter().format("class Once{public:int operator()()&&{return "
                               "1;} public: Once() = default; };");
  expect(formatted.find("int operator()() && {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "operator() && should format with stable C++-style spacing");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-consuming-callables.gti", R"(
class Once {
public:
  int operator()() && { return 1; }
public:
  Once() = default;
};

int invoke_twice<Operation>(Operation operation) {
  int first = std::move(operation)();
  return std::move(operation)();
}

int main() {
  Once operation = Once();
  int direct = operation();
  return invoke_twice(operation) + direct;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(
              invalid.diagnostics,
              "requires an owned temporary or an explicitly moved receiver") &&
          (hasDiagnostic(invalid.diagnostics, "has already been moved") ||
           hasDiagnostic(invalid.diagnostics,
                         "may have been moved on another")),
      "once-callable lvalue and repeated invocation should fail through "
      "receiver and path-sensitive move diagnostics");

  const lang::FrontendResult implicitForwarding = lang::Frontend().analyze(
      "implicit-once-forwarding.gti", R"(
class Once {
public:
  int operator()() && { return 1; }
public:
  Once() = default;
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)();
}

int middle<Operation>(Operation operation) {
  return invoke_once(std::move(operation));
}

int outer<Operation>(Operation operation) {
  return middle(operation);
}

int main() { return outer(Once()); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!implicitForwarding.canGenerateCode() &&
             countDiagnosticCode(implicitForwarding.diagnostics, "GTI-S2046") ==
                 1 &&
             hasDiagnostic(implicitForwarding.diagnostics,
                           "requires explicit ownership transfer") &&
             hasRelatedDiagnostic(
                 implicitForwarding.diagnostics,
                 "selected confined target consumes this callable") &&
             !hasDiagnosticCode(implicitForwarding.diagnostics, "GTI-B0001"),
         "a transitive once-confined forwarding edge should require an "
         "explicit source move before HIR");

  const lang::FrontendResult repeatedForwarding = lang::Frontend().analyze(
      "repeated-once-forwarding.gti", R"(
class Once {
public:
  int operator()() && { return 1; }
public:
  Once() = default;
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)();
}

int forward_twice<Operation>(Operation operation) {
  int first = invoke_once(std::move(operation));
  return first + invoke_once(std::move(operation));
}

int main() { return forward_twice(Once()); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!repeatedForwarding.canGenerateCode() &&
             hasDiagnosticCode(repeatedForwarding.diagnostics, "GTI-S2018") &&
             hasDiagnostic(repeatedForwarding.diagnostics,
                           "has already been moved") &&
             !hasDiagnosticCode(repeatedForwarding.diagnostics, "GTI-B0001"),
         "two reachable once-confined forwardings should fail through the "
         "ordinary path-sensitive move state before HIR");

  const lang::FrontendResult cleanupReceiver = lang::Frontend().analyze(
      "cleanup-consuming-callables.gti", R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() {}
};

class DirectCleanupCallable {
public:
  DirectCleanupCallable() {}
  ~DirectCleanupCallable() {}
  int operator()() && { return 1; }
};

class NestedCleanupCallable<T> {
  T value;
public:
  NestedCleanupCallable(T initial) : value(std::move(initial)) {}
  int operator()() && { return 2; }
};

class CleanupMutableFallback {
  Cleanup cleanup = Cleanup();
public:
  int operator()(int amount) mut { return amount; }
public:
  CleanupMutableFallback() = default;
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)();
}

int main() {
  DirectCleanupCallable direct{};
  int first = std::move(direct)();
  NestedCleanupCallable<Cleanup> nested =
      NestedCleanupCallable<Cleanup>(Cleanup());
  int second = std::move(nested)();
  CleanupMutableFallback fallback{};
  int third = std::move(fallback)(3);
  return first + second + third;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!cleanupReceiver.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : cleanupReceiver.diagnostics) {
      std::cerr << "Unexpected cleanup-consuming diagnostic ["
                << diagnostic.code << "]: " << diagnostic.message << '\n';
    }
  }
  expect(cleanupReceiver.canGenerateCode(),
         "direct, nested, and mutable-fallback calls retain moved nominal "
         "receivers through full-expression cleanup");

  const lang::FrontendResult genericCleanupReceiver = lang::Frontend().analyze(
      "generic-cleanup-consuming-callable.gti", R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() {}
};

class CleanupCallable {
  Cleanup cleanup = Cleanup();
public:
  int operator()() && { return 1; }
public:
  CleanupCallable() = default;
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)();
}

int main() {
  CleanupCallable generic{};
  return invoke_once(std::move(generic));
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(genericCleanupReceiver.canGenerateCode(),
         "concrete generic once-call selection retains its cleanup-owning "
         "nominal receiver through the full expression");

  lang::Lexer lexer;
  lang::Parser invalidParser(lexer.scan(R"(
class Invalid {
public:
  void method() && {}
  void operator()() mut && {}
};
int main() { return 0; }
)"));
  invalidParser.parse();
  expect(invalidParser.hadError() &&
             hasDiagnostic(invalidParser.errors(),
                           "cannot combine trailing 'mut' and '&&'"),
         "the parser should reject combined mutable and consuming qualifiers");
}

void testConsumingMethodsAndValueOperators() {
  const std::string source = R"(
class Builder {
public:
  mut int value = 0;
  Builder(int n) : value(n) {}
  Builder(Builder& other) = delete;
  Builder(Builder&& other) = default;
  int finish() { return this.value; }
  int finish() mut { return this.value + 10; }
  int finish() && { return this.value + 20; }
  Builder append(int n) && { this.value += n; return std::move(this); }
  Builder operator/(int n) { return Builder(this.value + n); }
  Builder operator/(int n) && { this.value += n; return std::move(this); }
public:
  Builder() = default;
};
Builder make() { return Builder(1); }
int main() {
  Builder base = Builder(1);
  mut Builder writable = Builder(2);
  int read = base.finish();
  int changed = writable.finish();
  int fresh = make().append(2).finish();
  int chain = (base / 3 / 4).finish();
  int moved = std::move(writable).append(3).finish();
  int conditional = (read == 1 ? make() : Builder(4)).finish();
  return read + changed + fresh + chain + moved + conditional + base.finish() - 111;
}
)";
  const auto frontend = lang::Frontend().analyze(
      "consuming-methods.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << "Consuming methods: " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(), "ordinary methods and value operators "
                                     "should select consuming owned receivers");
  const auto formatted = lang::Formatter().format(source);
  expect(formatted.find("append(int n) &&") != std::string::npos &&
             formatted.find("operator/(int n) &&") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "consuming methods and operators must format without losing their "
         "qualifiers");
  if (frontend.canGenerateCode()) {
    const lang::LanguageQueries queries;
    const auto hover = queries.hover(frontend, frontend.sourceGraph.entryUnit(),
                                     source.find("append(int") + 2);
    expect(hover && hover->signature.find("&&") != std::string::npos,
           "method hover must preserve consuming receiver qualification");
    for (int mutation = 0; mutation < 4; ++mutation) {
      lang::MirProgram forged = frontend.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      bool changed = false;
      for (auto &function : functions) {
        if (changed)
          break;
        for (auto &block : function.body.blocks) {
          if (changed)
            break;
          for (auto &instruction : block.instructions) {
            if (mutation == 3 &&
                instruction.kind == lang::MirInstructionKind::Initialize &&
                instruction.destination) {
              const auto *place =
                  function.body.findPlace(*instruction.destination);
              if (place && place->root == lang::MirPlaceRootKind::Temporary &&
                  place->type.kind == lang::SemanticType::Class) {
                instruction = lang::makeMirInstructionTombstone(instruction);
                changed = true;
              }
            }
            if (mutation == 0 && instruction.callableInvocation ==
                                     lang::CallableInvocationCapability::Once) {
              instruction.callableInvocation.reset();
              changed = true;
            } else if (mutation == 1 &&
                       instruction.callInputKind ==
                           lang::HirCallInputKind::MutableTemporaryBorrow) {
              instruction.callInputKind =
                  lang::HirCallInputKind::ReadTemporaryBorrow;
              instruction.operands.front().kind =
                  lang::MirOperandKind::BorrowRead;
              changed = true;
            } else if (mutation == 2 &&
                       instruction.kind == lang::MirInstructionKind::Move &&
                       instruction.operands.size() == 1) {
              const auto *place =
                  function.body.findPlace(instruction.operands.front().place);
              if (place && place->root == lang::MirPlaceRootKind::This &&
                  place->projections.empty()) {
                function.receiverMutability =
                    lang::ReceiverMutability::ReadOnly;
                changed = true;
              }
            }
            if (changed)
              break;
          }
        }
      }
      expect(
          changed && !lang::verifyMirProgram(forged).valid(),
          "MIR must independently reject forged consuming receiver authority");
    }
  }

  const std::vector<std::pair<std::string, std::string>> invalid = {
      {"class C { public: void take() && {}  public: C() = default; }; int "
       "main() { C c{}; c.take(); "
       "return 0; }",
       "requires an owned temporary"},
      {"class C { public: int x = 1; void take() && {}  public: C() = default; "
       "}; int main() { C c{}; "
       "std::move(c).take(); return c.x; }",
       "already been moved"},
      {"class C { public: C take() { return std::move(this); }  public: C() = "
       "default; }; int main() { "
       "return 0; }",
       "std::move(this) requires a consuming receiver"},
      {"class C { public: ~C() {} C take() && { return this; }  public: C() = "
       "default; }; int main() { "
       "return 0; }",
       "Returning this value would copy move-only type"},
      {"class C { public: int x = 1; int take() && { auto owned = "
       "std::move(this); return this.x; }  public: C() = default; }; int "
       "main() { return 0; }",
       "already been moved"},
      {"class C { public: C take() && { auto owned = std::move(this); return "
       "std::move(this); }  public: C() = default; }; int main() { return 0; }",
       "already been moved"},
      {"class C { public: mut int x = 1; C take() && { int old = "
       "std::move(this.x); return std::move(this); }  public: C() = default; "
       "}; int main() { return 0; "
       "}",
       "already been moved"},
      {"class C { public: int x = 1; int& take() && { return this.x; }  "
       "public: C() = default; }; int "
       "main() { return 0; }",
       "consuming method cannot return"},
      {"class C { public: bool operator==(C& other) && { return true; }  "
       "public: C() = default; }; int "
       "main() { return 0; }",
       "value-producing operators"},
      {"class C { public: void operator+=(int n) && {}  public: C() = default; "
       "}; int main() { return "
       "0; }",
       "value-producing operators"},
      {"class C { public: int x = 1; void take() && {}  public: C() = default; "
       "}; int main() { C c{}; "
       "int& alias = c.x; std::move(c).take(); return alias; }",
       "borrow"},
      {"class C { public: int x = 1; C take() && { int& alias = this.x; auto c "
       "= std::move(this); int observed = alias; return std::move(c); }  "
       "public: C() = default; }; int "
       "main() { return 0; }",
       "borrow"},
      {"class C { public: int x = 1; int read() { return this.x; } int take() "
       "&& { auto c = std::move(this); return read(); }  public: C() = "
       "default; }; int main() { return "
       "0; }",
       "already been moved"},
      {"class C { public: int x = 1; int take(bool condition) && { if "
       "(condition) { auto c = std::move(this); } return this.x; }  public: "
       "C() = default; }; int "
       "main() { return 0; }",
       "may have been moved"},
      {"class C { public: mut int x = 1; void take() && { auto c = "
       "std::move(this); this.x = 2; }  public: C() = default; }; int main() { "
       "return 0; }",
       "already been moved"},
      {"class C { public: int x = 1; void take() && { while (true) { auto c = "
       "std::move(this); } }  public: C() = default; }; int main() { return 0; "
       "}",
       "not available at a reachable loop backedge"},
      {"class C { public: void take() && {}  public: C() = default; }; void "
       "run(C& borrowed) { "
       "borrowed.take(); } int main() { return 0; }",
       "requires an owned temporary"},
      {"interface I { int& take() &&; }; int main() { return 0; }",
       "consuming method cannot return"},
      {"class C<T> { public: T take(T value) && { return value; }  public: C() "
       "= default; }; int "
       "main() { C<std::string_view> c{}; auto value = "
       "std::move(c).take(\"\"); return 0; }",
       "consuming method cannot return"},
  };
  for (const auto &[text, fragment] : invalid) {
    const auto result = lang::Frontend().analyze(
        "invalid-consuming-method.gti", text, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    if (!hasDiagnostic(result.diagnostics, fragment)) {
      std::cerr << "Missing diagnostic " << fragment << " for " << text << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.message << '\n';
    }
    expect(!result.canGenerateCode() &&
               hasDiagnostic(result.diagnostics, fragment) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "invalid consuming method use must fail in the frontend");
  }
  const auto generic = lang::Frontend().analyze(
      "generic-consuming-method.gti", R"(
class Box<T> {
public:
  mut T value;
  Box(T value) : value(std::move(value)) {}
  T take() && { return std::move(this.value); }
  Box<T> identity<U>(U ignored) && { return std::move(this); }
};
int main() { return Box<int>(4).identity<int>(0).take() - 4; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!generic.canGenerateCode())
    for (const auto &diagnostic : generic.diagnostics)
      std::cerr << "Generic consuming: " << diagnostic.message << '\n';
  expect(generic.canGenerateCode(),
         "generic methods and generic owner instances must preserve consuming "
         "receivers");
}

void testRangeBasedForAndIteratorProtocol() {
  const std::string source = R"(
interface IteratorContract<T> {
  T& operator*();
  void operator++() mut;
};

struct CounterSentinel {
  int limit;
  CounterSentinel(int value) : limit(value) {}
};

class CounterIterator : public IteratorContract<int> {
  mut int current;

public:
  CounterIterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(CounterSentinel& sentinel) {
    return this.current != sentinel.limit;
  }
};

class CounterRange {
  int first;
  int last;

public:
  CounterRange(int first, int last) : first(first), last(last) {}
  CounterIterator begin() { return CounterIterator(this.first); }
  CounterSentinel end() { return CounterSentinel(this.last); }
};

int main() {
  CounterRange values{1, 5};
  mut int total = 0;
  for (auto& value : values) {
    if (value == 2) { continue; }
    total += value;
  }
  return total - 8;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("range-for.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected range-for diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "range-based for should use the ordinary structural iterator "
         "protocol through generic virtual operator contracts");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == main) {
      mainInstance = &instance;
      break;
    }
  }
  bool hasRangeFor = false;
  bool hasProtocolCalls = false;
  if (mainInstance != nullptr) {
    for (const lang::HirStatementId root : mainInstance->body.roots) {
      const lang::HirStatement *statement =
          mainInstance->body.findStatement(root);
      hasRangeFor =
          hasRangeFor || (statement != nullptr &&
                          statement->kind == lang::HirStatementKind::RangeFor);
    }
    std::size_t selectedCalls = 0;
    for (const lang::HirValue &value : mainInstance->body.values) {
      if (value.functionTarget) {
        ++selectedCalls;
      }
    }
    hasProtocolCalls = selectedCalls >= 5;
  }
  expect(hasRangeFor && hasProtocolCalls,
         "HIR should retain range syntax provenance and concrete begin, end, "
         "comparison, dereference, and increment targets");

  const lang::FrontendResult concreteGeneric = lang::Frontend().analyze(
      "generic-range-for.gti", R"(
import std.vector;
int32_t count_all<T>(std::vector<T>& values) requires std::vector_element<T> {
  mut int32_t total = 0;
  for (auto& value : values) { total = total + 1; }
  return total;
}
int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  return count_all(values) - 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!concreteGeneric.canGenerateCode() ||
      !concreteGeneric.diagnostics.empty()) {
    for (const auto &diagnostic : concreteGeneric.diagnostics)
      std::cerr << "Unexpected generic range-for diagnostic: "
                << diagnostic.message << '\n';
  }
  expect(concreteGeneric.canGenerateCode() &&
             concreteGeneric.diagnostics.empty() &&
             lang::verifyHirProgramPlans(concreteGeneric.semantics,
                                         concreteGeneric.hir)
                 .valid(),
         "a concrete generic range body should verify against its exact HIR "
         "binding and per-instance loan facts");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const auto &occurrences = frontend.semantics.database().occurrences(unit);
  expect(std::none_of(occurrences.begin(), occurrences.end(),
                      [](const lang::SemanticOccurrence &occurrence) {
                        return occurrence.name.rfind("__gti_", 0) == 0;
                      }),
         "compiler-generated range bindings should not leak into semantic "
         "tooling occurrences");

  // The structural loop borrows its range read-only and calls the selected
  // GTI begin/end and iterator members; constructing the sentinel cannot
  // fail, so end() is failure-free while begin() keeps its failure edge.
  const bool readOnlyRangeLoan =
      mirHasRangeIteration(frontend, [&](const lang::MirBody &body,
                                         const lang::MirRangeIteration &range) {
        const lang::MirLoan *loan = body.findLoan(range.rangeLoan);
        return range.strategy == lang::RangeIterationStrategy::Structural &&
               loan != nullptr && loan->access == lang::AccessMode::ReadOnly;
      });
  const auto counterRangeMember = [&](std::string_view name, bool mayRaise) {
    return std::any_of(
        frontend.mir.functionInstances().begin(),
        frontend.mir.functionInstances().end(), [&](const auto &function) {
          const lang::HirFunctionInstance *source =
              frontend.hir.findFunctionInstance(function.id);
          const lang::MirClassInstance *owner =
              function.owner ? frontend.mir.findClassInstance(*function.owner)
                             : nullptr;
          const lang::ClassTypeInfo *info =
              owner == nullptr
                  ? nullptr
                  : frontend.semantics.findClassType(owner->type.classId);
          return source != nullptr && source->source != nullptr &&
                 source->source->name().lexeme == name && info != nullptr &&
                 info->qualifiedName == "CounterRange" &&
                 function.mayRaiseDefinedFailure == mayRaise &&
                 mirCallsFunctionNamed(frontend, name);
        });
  };
  expect(readOnlyRangeLoan && counterRangeMember("begin", true) &&
             counterRangeMember("end", false) &&
             mirCallsOperator(frontend, lang::OverloadedOperator::Dereference,
                              true) &&
             mirCallsOperator(frontend, lang::OverloadedOperator::PreIncrement,
                              true) &&
             nativeBackendEmits(frontend),
         "the native backend should consume the verified range loans and "
         "selected GTI iterator methods");

  const std::string formatted = lang::Formatter().format(
      "class Iterator{public:void operator++()mut{} public: Iterator() = "
      "default; };"
      "int sum(Range& values){mut int total=0;"
      "for(auto& value:values){total+=value;}return total;}");
  expect(formatted.find("void operator++() mut {") != std::string::npos &&
             formatted.find("for (auto& value : values) {") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "prefix increment and range-based for syntax should format "
         "idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-range-for.gti", R"(
class BadIncrement {
  mut int value = 0;
public:
  int& operator*() { return this.value; }
  int operator++() mut { return this.value; }
public:
  BadIncrement() = default;
};

class ReadOnlyIncrement {
public:
  void operator++() {}
public:
  ReadOnlyIncrement() = default;
};

class ValueSentinelIterator {
  mut int value = 0;
public:
  int& operator*() { return this.value; }
  void operator++() mut { this.value++; }
  bool operator!=(ValueSentinelIterator other) {
    return this.value != other.value;
  }
public:
  ValueSentinelIterator() = default;
};

class ValueSentinelRange {
public:
  ValueSentinelIterator begin() { return ValueSentinelIterator(); }
  ValueSentinelIterator end() { return ValueSentinelIterator(); }
public:
  ValueSentinelRange() = default;
};

class MissingBegin { public: MissingBegin() = default; };

int main() {
  mut int value = 1;
  MissingBegin range{};
  for (int item : range) { return item; }
  for (int item : MissingBegin()) { return item; }
  ValueSentinelRange copied_sentinel{};
  for (int item : copied_sentinel) { return item; }
  BadIncrement iterator{};
  ++iterator;
  return value;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "Prefix operator++ must return void") &&
             hasDiagnostic(invalid.diagnostics,
                           "Prefix operator++ must use a mutable receiver") &&
             hasDiagnostic(invalid.diagnostics, "Unknown member 'begin'") &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2022"),
         "range and iterator diagnostics should reject malformed operator "
         "contracts and missing begin members");
  const auto generatedDiagnostic =
      std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.message.find("Unknown member 'begin'") !=
                            std::string::npos;
                   });
  expect(generatedDiagnostic != invalid.diagnostics.end() &&
             generatedDiagnostic->primary.end ==
                 generatedDiagnostic->primary.start + 1,
         "diagnostics from lowered range operations should point at the "
         "source colon rather than a synthetic identifier width");
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

void testSourceOwnedExpectedRangeOrigins() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/source_prefix_expected_range.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the unrelated source-owned range fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = lang::Frontend().analyze(
      fixture, source, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "ordinary and Expected source-owned ranges retain the checked owner");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  std::size_t ranges = 0;
  std::size_t rejected = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &range : function.body.rangeIterations) {
      const auto *element = function.body.findLoan(range.elementLoan);
      const auto *parent = function.body.findLoan(range.rangeLoan);
      expect(element != nullptr && parent != nullptr &&
                 element->source == parent->source &&
                 element->access == lang::AccessMode::ReadOnly &&
                 parent->access == lang::AccessMode::ReadOnly,
             "an opaque source cursor borrows its range owner, not its shell");
      ++ranges;
      if (element == nullptr)
        continue;
      auto forged = frontend.mir;
      auto *body =
          lang::findMirBody(forged, {lang::MirBodyKind::Function, function.id});
      body->loans[element->id - 1].source = range.iterator;
      rejected += !lang::verifyMirProgram(forged).valid();
    }
  }
  expect(ranges == 2 && rejected == 2,
         "both direct and payload ranges reject iterator-shell source forgery");
}

void testMutableRangeLoans() {
  const lang::FrontendResult fixedArrays =
      lang::Frontend().analyze("mutable-fixed-array-ranges.gti", R"(
int mutate_arrays() {
  mut int32_t values[4] = {1, 2, 3, 4};
  int32_t observed[2] = {5, 6};
  mut int32_t total = 0;
  for (mut int32_t& value : values) {
    for (int32_t& item : observed) { total += item; }
    value += 10;
    if (value == 12) { continue; }
    if (value == 13) { break; }
  }
  return values[0] + values[1] + values[2] + values[3] + total - 73;
}

int return_from_mutable_loop() {
  mut int32_t values[2] = {1, 2};
  for (mut auto& value : values) {
    value += 1;
    if (value == 3) { return value - 3; }
  }
  return 1;
}

int main() { return mutate_arrays() + return_from_mutable_loop(); }
)");
  expect(fixedArrays.canGenerateCode() && fixedArrays.mirValid &&
             fixedArrays.mir.valid() &&
             lang::verifyHirProgramPlans(fixedArrays.semantics, fixedArrays.hir)
                 .valid() &&
             lang::verifyMirProgram(fixedArrays.mir).valid(),
         "mutable fixed-array iteration should lower through verified "
         "exclusive range and element loans");

  bool foundMutableFixedArray = false;
  for (const lang::HirFunctionInstance &function :
       fixedArrays.hir.functionInstances()) {
    for (const lang::HirStatement &statement : function.body.statements) {
      if (!statement.rangeIteration ||
          statement.rangeIteration->elementAccess !=
              lang::AccessMode::Mutable) {
        continue;
      }
      foundMutableFixedArray = foundMutableFixedArray ||
                               statement.rangeIteration->strategy ==
                                   lang::RangeIterationStrategy::FixedArray;
    }
  }
  expect(foundMutableFixedArray,
         "HIR should retain a mutable fixed-array range plan");

  bool mutableMirLoanShape = false;
  for (const lang::MirFunctionInstance &function :
       fixedArrays.mir.functionInstances()) {
    for (const lang::MirRangeIteration &range : function.body.rangeIterations) {
      if (range.elementAccess != lang::AccessMode::Mutable) {
        continue;
      }
      const lang::MirLoan *rangeLoan = function.body.findLoan(range.rangeLoan);
      const lang::MirLoan *elementLoan =
          function.body.findLoan(range.elementLoan);
      mutableMirLoanShape = rangeLoan != nullptr && !rangeLoan->escapes &&
                            rangeLoan->access == lang::AccessMode::Mutable &&
                            elementLoan != nullptr && !elementLoan->escapes &&
                            elementLoan->access == lang::AccessMode::Mutable &&
                            elementLoan->parent == range.rangeLoan;
    }
  }
  expect(mutableMirLoanShape,
         "MIR should preserve the exclusive parent/child range-loan shape");

  const bool mutableFixedArrayPlan = mirHasRangeIteration(
      fixedArrays,
      [](const lang::MirBody &body, const lang::MirRangeIteration &range) {
        const lang::MirLoan *element = body.findLoan(range.elementLoan);
        return range.strategy == lang::RangeIterationStrategy::FixedArray &&
               range.elementAccess == lang::AccessMode::Mutable &&
               element != nullptr &&
               element->access == lang::AccessMode::Mutable;
      });
  expect(mutableFixedArrayPlan && nativeBackendEmits(fixedArrays),
         "the native backend should consume mutable fixed-array loans from "
         "the verified MIR plan");

  lang::MirProgram detachedMutableElement = fixedArrays.mir;
  bool corruptedMutableElement = false;
  for (const auto address :
       lang::enumerateMirBodyAddresses(detachedMutableElement)) {
    lang::MirBody *body = lang::findMirBody(detachedMutableElement, address);
    if (body == nullptr || corruptedMutableElement)
      continue;
    for (const lang::MirRangeIteration &range : body->rangeIterations) {
      if (range.elementAccess != lang::AccessMode::Mutable ||
          range.elementLoan == 0 || range.elementLoan > body->loans.size())
        continue;
      body->loans[range.elementLoan - 1].parent = 0;
      corruptedMutableElement = true;
      break;
    }
  }
  expect(corruptedMutableElement &&
             !lang::verifyMirProgram(detachedMutableElement).valid(),
         "MIR verification should reject a mutable element loan detached "
         "from its exclusive range parent");

  lang::MirBody invalidMutableMir;
  bool removedMutableElementEnd = false;
  for (const lang::MirFunctionInstance &function :
       fixedArrays.mir.functionInstances()) {
    const auto mutableRange =
        std::find_if(function.body.rangeIterations.begin(),
                     function.body.rangeIterations.end(),
                     [](const lang::MirRangeIteration &range) {
                       return range.elementAccess == lang::AccessMode::Mutable;
                     });
    if (mutableRange == function.body.rangeIterations.end()) {
      continue;
    }
    invalidMutableMir = function.body;
    for (lang::MirBlock &block : invalidMutableMir.blocks) {
      const auto ending = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == mutableRange->elementLoan;
          });
      if (ending != block.instructions.end()) {
        block.instructions.erase(ending);
        removedMutableElementEnd = true;
        break;
      }
    }
    break;
  }
  expect(removedMutableElementEnd &&
             !lang::verifyMirBody(invalidMutableMir).valid(),
         "MIR verification should reject a mutable element loan that remains "
         "live across an increment or exit edge");

  const lang::FrontendResult standardVector = lang::Frontend().analyze(
      "mutable-vector-range.gti", R"(
import std.vector;

std::vector<int32_t> make_values() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(2);
  values.push_back(3);
  return std::move(values);
}

int mutate_owned_temporary() {
  mut int32_t total = 0;
  for (mut auto& value : make_values()) {
    value += 4;
    total += value;
  }
  for (mut auto& value : std::vector<int32_t>()) { return value; }
  return total - 13;
}

int mutate_vector() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  for (mut auto& value : values) { value += 4; }
  return values[std::size_t(0)] + values[std::size_t(1)] +
         values[std::size_t(2)] - 18;
}

int main() { return mutate_vector() + mutate_owned_temporary(); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(standardVector.canGenerateCode() && standardVector.mir.valid(),
         "std::vector should expose bounded in-place mutable iteration");

  bool foundMutableVector = false;
  for (const lang::MirFunctionInstance &function :
       standardVector.mir.functionInstances()) {
    for (const lang::MirRangeIteration &range : function.body.rangeIterations) {
      if (range.elementAccess != lang::AccessMode::Mutable) {
        continue;
      }
      const lang::MirLoan *rangeLoan = function.body.findLoan(range.rangeLoan);
      const lang::MirLoan *elementLoan =
          function.body.findLoan(range.elementLoan);
      const lang::MirPlace *iterator = function.body.findPlace(range.iterator);
      foundMutableVector =
          range.strategy == lang::RangeIterationStrategy::Structural &&
          rangeLoan != nullptr &&
          rangeLoan->access == lang::AccessMode::Mutable &&
          iterator != nullptr &&
          iterator->root == lang::MirPlaceRootKind::Binding &&
          std::find(rangeLoan->carriers.begin(), rangeLoan->carriers.end(),
                    iterator->binding) != rangeLoan->carriers.end() &&
          elementLoan != nullptr &&
          elementLoan->access == lang::AccessMode::Mutable &&
          elementLoan->parent == range.rangeLoan;
    }
  }
  expect(foundMutableVector,
         "the mutable vector iterator should be a confined carrier of the "
         "range loan and dereference to one mutable child loan");

  bool checkedMutableRead = false;
  bool failingBeginMut = false;
  for (const auto &function : standardVector.mir.functionInstances()) {
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        checkedMutableRead |=
            instruction.intrinsic == lang::IntrinsicKind::RawBorrowMut;
    const lang::HirFunctionInstance *source =
        standardVector.hir.findFunctionInstance(function.id);
    failingBeginMut |= source != nullptr && source->source != nullptr &&
                       source->source->name().lexeme == "begin_mut" &&
                       function.mayRaiseDefinedFailure &&
                       hasVerifiedSourceBody(&function);
  }
  expect(mirClassInstanceCount(standardVector,
                               "std::detail::vector_mut_iterator") == 1 &&
             failingBeginMut && checkedMutableRead &&
             nativeBackendEmits(standardVector),
         "the native backend should emit the mutable iterator's transferred "
         "reference capability and mutable storage read");

  bool mutableTemporaryCleanup = false;
  bool mutableTemporaryFailureCleanup = false;
  for (const lang::MirFunctionInstance &function :
       standardVector.mir.functionInstances()) {
    for (const lang::MirRangeIteration &range : function.body.rangeIterations) {
      if (!range.ownsRangeTemporary ||
          range.elementAccess != lang::AccessMode::Mutable ||
          range.ownerDrop == 0) {
        continue;
      }
      for (const lang::MirBlock &block : function.body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
            if (event.kind != lang::MirLifecycleEventKind::Drop ||
                event.source != range.ownerDrop) {
              continue;
            }
            mutableTemporaryCleanup =
                mutableTemporaryCleanup || !event.failureCleanup;
            mutableTemporaryFailureCleanup =
                mutableTemporaryFailureCleanup || event.failureCleanup;
          }
        }
      }
    }
  }
  expect(mutableTemporaryCleanup && mutableTemporaryFailureCleanup,
         "owned mutable vector ranges should be destroyed exactly once on "
         "normal and defined-failure paths");

  const std::string formattedMutableRange = lang::Formatter().format(
      "int mutate(){mut int values[2]={1,2};"
      "for(mut auto& value:values){value+=1;}return 0;}");
  expect(formattedMutableRange.find("for (mut auto& value : values) {") !=
                 std::string::npos &&
             lang::Formatter().format(formattedMutableRange) ==
                 formattedMutableRange,
         "formatter should preserve mutable range bindings idempotently");

  const lang::FrontendResult invalidFixedArrays =
      lang::Frontend().analyze("invalid-mutable-fixed-array-ranges.gti", R"(
int immutable_owner() {
  int32_t values[2] = {1, 2};
  for (mut int32_t& value : values) { value = 0; }
  return 0;
}

int overlapping_owner() {
  mut int32_t values[2] = {1, 2};
  for (mut int32_t& value : values) {
    for (int32_t& observed : values) { return observed + value; }
  }
  return 0;
}

int& escape_element() {
  mut int32_t values[2] = {1, 2};
  for (mut int32_t& value : values) { return value; }
  return values[0];
}
)");
  expect(!invalidFixedArrays.canGenerateCode() &&
             hasDiagnosticCode(invalidFixedArrays.diagnostics, "GTI-S2083") &&
             hasDiagnosticCode(invalidFixedArrays.diagnostics, "GTI-S2017"),
         "mutable range diagnostics should reject immutable owners, "
         "overlapping loans, and escaping element loans");

  const lang::FrontendResult invalidVector = lang::Frontend().analyze(
      "invalid-mutable-vector-range.gti", R"(
import std.vector;

int reserve_during_iteration() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(1);
  for (mut int32_t& value : values) {
    values.reserve(std::size_t(8));
  }
  return 0;
}

int push_during_iteration() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(1);
  for (mut int32_t& value : values) { values.push_back(2); }
  return 0;
}

int clear_during_iteration() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(1);
  for (mut int32_t& value : values) { values.clear(); }
  return 0;
}

int move_owner_during_iteration() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(1);
  for (mut int32_t& value : values) {
    // Leaving the loop keeps the moved owner from a loop backedge, whose
    // availability error would otherwise reject the program first.
    std::vector<int32_t> moved = std::move(values);
    break;
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidVector.canGenerateCode() &&
             countDiagnosticCode(invalidVector.diagnostics, "GTI-S2017") >= 4,
         "vector reserve, push, clear, and owner moves should be rejected "
         "while a mutable range or element loan is active");

  const lang::FrontendResult missingMutableProtocol =
      lang::Frontend().analyze("missing-mutable-range-protocol.gti", R"(
class Iterator {
  mut int32_t current = 0;
public:
  int32_t& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(Iterator& other) { return this.current != other.current; }
public:
  Iterator() = default;
};

class ReadOnlyRange {
public:
  Iterator begin() { return Iterator(); }
  Iterator end() { return Iterator(); }
public:
  ReadOnlyRange() = default;
};

int main() {
  mut ReadOnlyRange values{};
  for (mut int32_t& value : values) { value = 1; }
  return 0;
}
)");
  expect(
      !missingMutableProtocol.canGenerateCode() &&
          hasDiagnosticCode(missingMutableProtocol.diagnostics, "GTI-S2083") &&
          hasDiagnosticHint(missingMutableProtocol.diagnostics, "begin_mut()"),
      "a structural range without a mutable endpoint should receive the "
      "mutable-range protocol diagnostic and actionable hint");
}

void testLambdas() {
  const std::string source = R"(
int main() {
  int offset = 3;
  auto add = [offset](int value) -> int { return offset + value; };
  auto copied = add;
  auto compose = [copied](int value) -> int {
    return copied(value) + 1;
  };
  int direct = [](int value) -> int { return value * 2; }(4);
  auto inferred = direct;
  return compose(inferred) - 12;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("lambdas.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected lambda diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "explicit value-capture lambdas should pass the frontend");
  expect(frontend.semantics.lambdaCount() == 3 &&
             frontend.hir.lambdaInstances().size() == 3,
         "semantic analysis and HIR should retain each typed closure");

  bool foundCapturedCall = false;
  for (const lang::HirLambda &lambda : frontend.hir.lambdaInstances()) {
    expect(lambda.source != nullptr &&
               lambda.returnType == lang::SemanticType::Int32 &&
               lambda.parameterTypes.size() == 1 &&
               lambda.parameterTypes.front() == lang::SemanticType::Int32,
           "HIR lambdas should retain concrete signatures and source identity");
    for (const lang::HirValue &value : lambda.body.values) {
      foundCapturedCall =
          foundCapturedCall || (value.kind == lang::HirValueKind::Call &&
                                value.lambdaTarget.has_value());
    }
  }
  expect(foundCapturedCall,
         "calls through captured lambda values should resolve in typed HIR");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "MIR should verify concrete closure capture signatures without "
         "reconstructing closure-environment initialization");

  lang::MirProgram wrongLambdaCaptureSize = frontend.mir;
  auto &wrongSizeLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      wrongLambdaCaptureSize.lambdaInstances());
  if (!wrongSizeLambdas.empty()) {
    wrongSizeLambdas.front().captureRequiresActiveCleanup.push_back(false);
  }
  const lang::MirVerificationResult wrongLambdaCaptureSizeResult =
      lang::verifyMirProgram(wrongLambdaCaptureSize);
  expect(!wrongLambdaCaptureSizeResult.valid() &&
             std::any_of(wrongLambdaCaptureSizeResult.errors.begin(),
                         wrongLambdaCaptureSizeResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("wrong size") !=
                                  std::string::npos;
                         }),
         "MIR verification should reject lambda capture cleanup vectors with "
         "a forged size");

  lang::MirProgram wrongLambdaDeclaration = frontend.mir;
  auto &wrongDeclarationLambdas =
      const_cast<std::vector<lang::MirLambdaInstance> &>(
          wrongLambdaDeclaration.lambdaInstances());
  if (!wrongDeclarationLambdas.empty()) {
    wrongDeclarationLambdas.front().declaration = 0;
  }
  const lang::MirVerificationResult wrongLambdaDeclarationResult =
      lang::verifyMirProgram(wrongLambdaDeclaration);
  expect(!wrongLambdaDeclarationResult.valid(),
         "MIR verification should reject a forged zero lambda declaration "
         "identity");

  lang::MirProgram wrongLambdaType = frontend.mir;
  auto &wrongTypeLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      wrongLambdaType.lambdaInstances());
  if (!wrongTypeLambdas.empty()) {
    wrongTypeLambdas.front().type = lang::SemanticType::Int32;
  }
  const lang::MirVerificationResult wrongLambdaTypeResult =
      lang::verifyMirProgram(wrongLambdaType);
  expect(!wrongTypeLambdas.empty() && !wrongLambdaTypeResult.valid() &&
             std::any_of(wrongLambdaTypeResult.errors.begin(),
                         wrongLambdaTypeResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("lambda instance type") !=
                                  std::string::npos;
                         }),
         "MIR verification should reject lambda-instance type metadata that "
         "does not reproduce its exact declaration and physical shape");

  lang::MirProgram wrongLambdaCaptureFlag = frontend.mir;
  auto &wrongFlagLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      wrongLambdaCaptureFlag.lambdaInstances());
  if (!wrongFlagLambdas.empty() &&
      !wrongFlagLambdas.front().captureRequiresActiveCleanup.empty()) {
    wrongFlagLambdas.front().captureRequiresActiveCleanup.front() =
        !wrongFlagLambdas.front().captureRequiresActiveCleanup.front();
  }
  const lang::MirVerificationResult wrongLambdaCaptureFlagResult =
      lang::verifyMirProgram(wrongLambdaCaptureFlag);
  expect(!wrongLambdaCaptureFlagResult.valid(),
         "MIR verification should reject a forged lambda capture cleanup "
         "fact");

  lang::MirProgram wrongLambdaDropInstance = frontend.mir;
  auto &wrongLambdaFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          wrongLambdaDropInstance.functionInstances());
  bool changedLambdaDropInstance = false;
  for (lang::MirFunctionInstance &function : wrongLambdaFunctions) {
    for (lang::MirDropObligation &obligation : function.body.dropObligations) {
      if (obligation.dropType.type.kind == lang::SemanticType::Lambda) {
        obligation.dropType.lambdaInstance.reset();
        changedLambdaDropInstance = true;
        break;
      }
    }
    if (changedLambdaDropInstance) {
      break;
    }
  }
  const lang::MirVerificationResult wrongLambdaDropInstanceResult =
      lang::verifyMirProgram(wrongLambdaDropInstance);
  expect(!changedLambdaDropInstance || !wrongLambdaDropInstanceResult.valid(),
         "MIR verification should bind lambda cleanup to its exact concrete "
         "HIR instance");

  // Closures copy their immutable value captures, including a captured
  // closure, and the immediately invoked lambda is called directly.
  bool copiedScalarCapture = false;
  bool copiedClosureCapture = false;
  bool directLambdaCall = false;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances())
    for (const lang::MirBlock &block : function.body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure)
          for (std::size_t index = 0;
               index < instruction.closureCaptureTypes.size() &&
               index < instruction.closureCaptureModes.size();
               ++index) {
            if (instruction.closureCaptureModes[index] !=
                lang::LambdaCaptureMode::Copy)
              continue;
            copiedScalarCapture |= instruction.closureCaptureTypes[index] ==
                                   lang::SemanticType::Int32;
            copiedClosureCapture |=
                instruction.closureCaptureTypes[index].kind ==
                lang::SemanticType::Lambda;
          }
        directLambdaCall |=
            instruction.kind == lang::MirInstructionKind::Call &&
            instruction.lambdaTarget.has_value() &&
            entryFunction(frontend, function.id);
      }
  const bool verifiedLambdaBodies =
      frontend.mir.lambdaInstances().size() == 3 &&
      std::all_of(frontend.mir.lambdaInstances().begin(),
                  frontend.mir.lambdaInstances().end(),
                  [](const lang::MirLambdaInstance &lambda) {
                    return !lambda.body.blocks.empty() &&
                           lang::verifyMirBody(lambda.body).valid();
                  });
  expect(copiedScalarCapture && copiedClosureCapture && directLambdaCall &&
             verifiedLambdaBodies &&
             nativeBackendEmits(frontend, lang::OptimizationLevel::O1),
         "verified MIR should materialize immutable closure copies, nested "
         "value captures, and direct invocation");

  const lang::FrontendResult generic =
      lang::Frontend().analyze("generic-lambdas.gti", R"(
T add_with<T>(T offset, T value) {
  auto add = [offset](T input) -> T { return input; };
  return add(value);
}

int main() {
  int signed_result = add_with(1, 2);
  uint64_t unsigned_result = add_with(uint64_t(1), uint64_t(2));
  return signed_result - int(unsigned_result);
}
)");
  expect(generic.canGenerateCode() && generic.hir.lambdaInstances().size() == 2,
         "generic function instances should receive distinct concrete "
         "closure bodies");
  bool foundSigned = false;
  bool foundUnsigned = false;
  for (const lang::HirLambda &lambda : generic.hir.lambdaInstances()) {
    foundSigned = foundSigned ||
                  (lambda.returnType == lang::SemanticType::Int32 &&
                   lambda.parameterTypes.front() == lang::SemanticType::Int32);
    foundUnsigned =
        foundUnsigned ||
        (lambda.returnType == lang::SemanticType::UInt64 &&
         lambda.parameterTypes.front() == lang::SemanticType::UInt64);
  }
  expect(foundSigned && foundUnsigned,
         "HIR should substitute lambda signatures per generic instance");
  expect(lang::verifyMirProgram(generic.mir).valid(),
         "generic closure producers should retain their exact concrete "
         "capture signature in verified MIR");

  lang::MirProgram wrongConcreteClosure = generic.mir;
  bool changedConcreteClosure = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           wrongConcreteClosure.functionInstances())) {
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Compute ||
            instruction.operation != lang::MirOperation::Closure ||
            !instruction.lambdaTarget) {
          continue;
        }
        const auto alternate = std::find_if(
            wrongConcreteClosure.lambdaInstances().begin(),
            wrongConcreteClosure.lambdaInstances().end(),
            [&](const lang::MirLambdaInstance &lambda) {
              return lambda.id != *instruction.lambdaTarget &&
                     lambda.captureTypes != instruction.closureCaptureTypes;
            });
        if (alternate != wrongConcreteClosure.lambdaInstances().end()) {
          instruction.lambdaTarget = alternate->id;
          changedConcreteClosure = true;
          break;
        }
      }
      if (changedConcreteClosure) {
        break;
      }
    }
    if (changedConcreteClosure) {
      break;
    }
  }
  expect(changedConcreteClosure &&
             !lang::verifyMirProgram(wrongConcreteClosure).valid(),
         "MIR verification should reject a closure producer retargeted to a "
         "generic lambda with an incompatible capture projection");

  const lang::FrontendResult nestedGeneric = lang::Frontend().analyze(
      "nested-generic-lambdas.gti", R"(
struct CopyableCapture {
  int value = 7;
  CopyableCapture() {}
  CopyableCapture(CopyableCapture& other) = default;
};

int nest<T>(T value) {
  auto outer = [value]() -> int { return 1; };
  auto inner = [outer]() -> int { return outer(); };
  return inner();
}

int main() {
  return nest(1) + nest(CopyableCapture()) - 2;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  bool foundNestedClosureCapture = false;
  for (const lang::MirFunctionInstance &function :
       nestedGeneric.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        foundNestedClosureCapture =
            foundNestedClosureCapture ||
            (instruction.operation == lang::MirOperation::Closure &&
             std::any_of(instruction.closureCaptureTypes.begin(),
                         instruction.closureCaptureTypes.end(),
                         [](const lang::SemanticType &capture) {
                           return capture.kind == lang::SemanticType::Lambda;
                         }));
      }
    }
  }
  expect(nestedGeneric.canGenerateCode() &&
             nestedGeneric.hir.lambdaInstances().size() == 4 &&
             foundNestedClosureCapture &&
             lang::verifyMirProgram(nestedGeneric.mir).valid(),
         "nested generic closure producers should retain and verify the "
         "capture signature of each concrete lambda instance");

  const lang::FrontendResult nestedValueCapture =
      lang::Frontend().analyze("nested-value-capture.gti", R"(
int main() {
  mut int32_t source = 7;
  auto outer = [source]() -> int32_t {
    return [source]() -> int32_t { return source; }();
  };
  return int(outer()) - 7;
}
)");
  expect(nestedValueCapture.canGenerateCode() &&
             nestedValueCapture.hir.lambdaInstances().size() == 2 &&
             lang::verifyMirProgram(nestedValueCapture.mir).valid(),
         "a nested lambda should be able to copy an immutable value capture "
         "from its enclosing lambda environment");

  const lang::FrontendResult expectedLambda = lang::Frontend().analyze(
      "expected-lambda.gti", R"(
int main() {
  auto calculate = [](bool fail) -> expected<int, std::string_view> {
    if (fail) { return unexpected("failed"); }
    return 1;
  };
  expected<int, std::string_view> result = calculate(false);
  return result.value_or(0) - 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  // The result in the lambda signature is an ordinary class instance that
  // the program defines, not a built-in helper.
  const bool expectedLambdaResult = std::any_of(
      expectedLambda.mir.lambdaInstances().begin(),
      expectedLambda.mir.lambdaInstances().end(),
      [&](const lang::MirLambdaInstance &lambda) {
        return isExpectedClass(expectedLambda, lambda.returnType) &&
               std::any_of(expectedLambda.mir.classInstances().begin(),
                           expectedLambda.mir.classInstances().end(),
                           [&](const lang::MirClassInstance &instance) {
                             return instance.type == lambda.returnType;
                           });
      });
  expect(expectedLambda.canGenerateCode() && expectedLambdaResult &&
             nativeBackendEmits(expectedLambda),
         "backend support should account for types nested in lambda "
         "signatures");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-lambdas.gti", R"(
T invoke<T>(T value) { return value; }
auto global_value = 1;

class ThisCapture {
  int value = 1;

public:
  int read() {
    auto invalid = []() -> int { return this.value; };
    return invalid();
  }
public:
  ThisCapture() = default;
};

class BorrowView {
  int& value;

public:
  BorrowView(int& source) : value(source) {}
};

int main() {
  mut int local = 1;
  int& alias = local;
  auto missing_initializer;
  auto inferred_reference = alias;
  auto missing = [](int value) -> int { return local + value; };
  auto duplicate = [local, local]() -> int { return local; };
  auto referenced = [alias]() -> int { return alias; };
  mut auto mutable_lambda = [local](int value) -> int {
    local += value;
    return local;
  };
  auto exact = [](int value) -> int { return value; };
  auto borrowed_return = [](int& source) -> BorrowView {
    return BorrowView(source);
  };
  int wrong_type = exact(true);
  int escaped = invoke(missing);
  return escaped;
}
)");
  expect(!invalid.canGenerateCode(),
         "implicit, duplicate, reference, mutable, and escaping lambdas "
         "should fail");
  expect(
      hasDiagnostic(invalid.diagnostics, "not captured") &&
          hasDiagnostic(invalid.diagnostics, "listed more than once") &&
          hasDiagnostic(invalid.diagnostics,
                        "use an explicit '&' or 'mut &' capture") &&
          hasDiagnostic(invalid.diagnostics,
                        "captured snapshots cannot be made mutable") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot assign to immutable lambda capture") &&
          hasDiagnostic(invalid.diagnostics,
                        "requires an explicit std::move ownership transfer") &&
          hasDiagnostic(invalid.diagnostics,
                        "Lambda return types cannot carry borrowed state") &&
          hasDiagnostic(invalid.diagnostics,
                        "The receiver is not captured; use '[&this]' or '[mut "
                        "&this]'.") &&
          hasDiagnostic(invalid.diagnostics,
                        "Lambda argument 1 has type 'bool'") &&
          hasDiagnostic(invalid.diagnostics, "limited to local bindings") &&
          hasDiagnostic(invalid.diagnostics, "requires an initializer"),
      "lambda diagnostics should explain each restricted lifetime or "
      "capture operation");
  expect(hasRelatedDiagnostic(invalid.diagnostics, "Local binding declared") &&
             hasDiagnosticHint(invalid.diagnostics, "capture list"),
         "missing capture diagnostics should point back to the declaration");

  const lang::FrontendResult moveCapture =
      analyzeWithMemory("move-capture.gti", R"(
int main() {
  std::unique_ptr<int> owner = std::make_unique<int>(1);
  auto invalid = [owner]() -> int { return *owner; };
  return invalid();
}
)");
  expect(!moveCapture.canGenerateCode() &&
             hasDiagnostic(moveCapture.diagnostics, "is not copyable"),
         "move-only values should not enter closures without explicit "
         "ownership-transfer capture syntax");

  const lang::FrontendResult ownedMoveCapture = lang::Frontend().analyze(
      "owned-move-capture.gti", R"(
import std.memory;

class Owner {
  int value = 5;

public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  ~Owner() {}

  int read() { return this.value; }
};

int main() {
  int offset = 2;
  Owner source{};
  auto operation = [offset, owned = std::move(source)]() -> int {
    return offset + owned.read();
  };
  auto relocated = std::move(operation);
  return relocated() - 7;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!ownedMoveCapture.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : ownedMoveCapture.diagnostics) {
      std::cerr << "Unexpected owned-move-capture diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  const lang::FunctionDecl *moveMain =
      findTopLevelFunction(ownedMoveCapture.program, "main");
  const auto *operationDecl =
      moveMain == nullptr || moveMain->body()->statements().size() < 3
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                moveMain->body()->statements()[2].get());
  const auto *moveLambda = operationDecl == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::Lambda *>(
                                     operationDecl->initializer().get());
  const lang::HirLambda *moveHirLambda =
      ownedMoveCapture.hir.lambdaInstances().empty()
          ? nullptr
          : &ownedMoveCapture.hir.lambdaInstances().front();
  const lang::HirFunctionInstance *moveMainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       ownedMoveCapture.hir.functionInstances()) {
    if (instance.source == moveMain) {
      moveMainInstance = &instance;
      break;
    }
  }
  const lang::HirValue *closureValue = nullptr;
  if (moveMainInstance != nullptr) {
    const auto closure = std::find_if(
        moveMainInstance->body.values.begin(),
        moveMainInstance->body.values.end(), [](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Lambda;
        });
    if (closure != moveMainInstance->body.values.end()) {
      closureValue = &*closure;
    }
  }
  const lang::HirValue *moveCaptureOperand =
      closureValue == nullptr || closureValue->operands.size() != 2
          ? nullptr
          : moveMainInstance->body.findValue(closureValue->operands[1]);
  expect(ownedMoveCapture.canGenerateCode() && moveLambda != nullptr &&
             moveLambda->captures().size() == 2 &&
             !moveLambda->captures()[0].explicitInitializer() &&
             moveLambda->captures()[1].explicitInitializer() &&
             moveHirLambda != nullptr && moveHirLambda->captures.size() == 2 &&
             moveHirLambda->captures[0].mode == lang::LambdaCaptureMode::Copy &&
             moveHirLambda->captures[1].mode == lang::LambdaCaptureMode::Move &&
             moveHirLambda->captures[0].bindingSymbol != 0 &&
             moveHirLambda->captures[1].bindingSymbol != 0 &&
             closureValue != nullptr && closureValue->operands.size() == 2 &&
             moveCaptureOperand != nullptr && moveCaptureOperand->ownership &&
             moveCaptureOperand->ownership->kind ==
                 lang::OwnershipEventKind::Move,
         "AST, semantics, and HIR should retain ordered copy and owned-move "
         "closure-environment initialization");

  const lang::MirFunctionInstance *moveMirMain =
      moveMainInstance == nullptr
          ? nullptr
          : ownedMoveCapture.mir.findFunctionInstance(moveMainInstance->id);
  const lang::MirInstruction *closureInstruction = nullptr;
  if (moveMirMain != nullptr) {
    for (const lang::MirBlock &block : moveMirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure) {
          closureInstruction = &instruction;
          break;
        }
      }
      if (closureInstruction != nullptr) {
        break;
      }
    }
  }
  const lang::MirLambdaInstance *moveMirLambda =
      closureInstruction == nullptr || !closureInstruction->lambdaTarget
          ? nullptr
          : ownedMoveCapture.mir.findLambda(*closureInstruction->lambdaTarget);
  const bool exactCapturePlaces =
      moveMirLambda != nullptr &&
      std::all_of(
          moveMirLambda->body.places.begin(), moveMirLambda->body.places.end(),
          [&](const lang::MirPlace &place) {
            return place.capture == 0 ||
                   (place.capture <= moveMirLambda->captureSymbols.size() &&
                    place.symbol ==
                        moveMirLambda->captureSymbols[place.capture - 1] &&
                    place.type ==
                        moveMirLambda->captureTypes[place.capture - 1]);
          });
  expect(
      closureInstruction != nullptr && moveMirLambda != nullptr &&
          closureInstruction->operands.size() == 2 &&
          closureInstruction->closureCaptureModes ==
              std::vector<lang::LambdaCaptureMode>{
                  lang::LambdaCaptureMode::Copy,
                  lang::LambdaCaptureMode::Move} &&
          closureInstruction->operands[0].kind == lang::MirOperandKind::Copy &&
          closureInstruction->operands[1].kind == lang::MirOperandKind::Value &&
          moveMirLambda->captureModes ==
              closureInstruction->closureCaptureModes &&
          moveMirLambda->captureSymbols.size() == 2 && exactCapturePlaces &&
          lang::verifyMirProgram(ownedMoveCapture.mir).valid(),
      "MIR should prove each closure field's exact copy/move initializer, "
      "symbol identity, projection, and cleanup-bearing lambda type");

  lang::MirProgram forgedCaptureMode = ownedMoveCapture.mir;
  bool changedCaptureMode = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           forgedCaptureMode.functionInstances())) {
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure &&
            !instruction.closureCaptureModes.empty()) {
          instruction.closureCaptureModes.front() =
              lang::LambdaCaptureMode::Move;
          changedCaptureMode = true;
          break;
        }
      }
      if (changedCaptureMode) {
        break;
      }
    }
    if (changedCaptureMode) {
      break;
    }
  }
  expect(changedCaptureMode &&
             !lang::verifyMirProgram(forgedCaptureMode).valid(),
         "MIR verification should reject a forged closure capture mode");

  lang::MirProgram invalidCaptureMode = ownedMoveCapture.mir;
  auto &invalidModeLambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
      invalidCaptureMode.lambdaInstances());
  bool changedInvalidCaptureMode = false;
  if (!invalidModeLambdas.empty() &&
      !invalidModeLambdas.front().captureModes.empty()) {
    const auto invalidMode = static_cast<lang::LambdaCaptureMode>(255);
    invalidModeLambdas.front().captureModes.front() = invalidMode;
    for (lang::MirFunctionInstance &function :
         const_cast<std::vector<lang::MirFunctionInstance> &>(
             invalidCaptureMode.functionInstances())) {
      for (lang::MirBlock &block : function.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.operation == lang::MirOperation::Closure &&
              !instruction.closureCaptureModes.empty()) {
            instruction.closureCaptureModes.front() = invalidMode;
            changedInvalidCaptureMode = true;
          }
        }
      }
    }
  }
  expect(changedInvalidCaptureMode &&
             !lang::verifyMirProgram(invalidCaptureMode).valid(),
         "MIR verification should reject an invalid capture mode even when "
         "closure construction and instance metadata agree");

  lang::MirProgram missingCaptureMove = ownedMoveCapture.mir;
  bool changedCaptureMove = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           missingCaptureMove.functionInstances())) {
    std::optional<lang::MirValueId> movedCaptureValue;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure &&
            instruction.operands.size() == 2 &&
            instruction.operands[1].kind == lang::MirOperandKind::Value) {
          movedCaptureValue = instruction.operands[1].value;
        }
      }
    }
    if (!movedCaptureValue) {
      continue;
    }
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.result == movedCaptureValue &&
            instruction.kind == lang::MirInstructionKind::Move) {
          instruction.ownership.reset();
          changedCaptureMove = true;
          break;
        }
      }
      if (changedCaptureMove) {
        break;
      }
    }
    if (changedCaptureMove) {
      break;
    }
  }
  expect(changedCaptureMove &&
             !lang::verifyMirProgram(missingCaptureMove).valid(),
         "MIR verification should reject an owned capture whose ordinary "
         "move-state proof was removed");

  lang::MirProgram aliasedCaptureMove = ownedMoveCapture.mir;
  bool changedCaptureAlias = false;
  for (lang::MirFunctionInstance &function :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           aliasedCaptureMove.functionInstances())) {
    std::optional<lang::MirValueId> movedCaptureValue;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.operation == lang::MirOperation::Closure &&
            instruction.operands.size() == 2 &&
            instruction.operands[1].kind == lang::MirOperandKind::Value) {
          movedCaptureValue = instruction.operands[1].value;
        }
      }
    }
    if (!movedCaptureValue) {
      continue;
    }
    const auto root =
        std::find_if(function.body.places.begin(), function.body.places.end(),
                     [&](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::Value &&
                              place.value == *movedCaptureValue;
                     });
    if (root == function.body.places.end()) {
      continue;
    }
    lang::MirPlace alias = *root;
    alias.id = function.body.places.size() + 1;
    function.body.places.push_back(std::move(alias));
    changedCaptureAlias = lang::rebuildMirValueUses(function.body);
    break;
  }
  expect(changedCaptureAlias &&
             !lang::verifyMirProgram(aliasedCaptureMove).valid(),
         "MIR verification should reject a second value-root path to an "
         "owned capture's moved initializer");

  lang::MirProgram forgedCapturePlace = ownedMoveCapture.mir;
  auto &forgedCaptureLambdas =
      const_cast<std::vector<lang::MirLambdaInstance> &>(
          forgedCapturePlace.lambdaInstances());
  bool changedCapturePlace = false;
  if (!forgedCaptureLambdas.empty()) {
    for (lang::MirPlace &place : forgedCaptureLambdas.front().body.places) {
      if (place.capture != 0) {
        place.capture = forgedCaptureLambdas.front().captureTypes.size() + 1;
        changedCapturePlace = true;
        break;
      }
    }
  }
  expect(changedCapturePlace &&
             !lang::verifyMirProgram(forgedCapturePlace).valid(),
         "MIR verification should reject a capture projection retargeted "
         "outside the exact closure environment");

  lang::MirProgram missingCapturePlace = ownedMoveCapture.mir;
  auto &missingCaptureLambdas =
      const_cast<std::vector<lang::MirLambdaInstance> &>(
          missingCapturePlace.lambdaInstances());
  bool removedCapturePlace = false;
  if (!missingCaptureLambdas.empty()) {
    for (lang::MirPlace &place : missingCaptureLambdas.front().body.places) {
      if (place.capture != 0) {
        place.capture = 0;
        removedCapturePlace = true;
        break;
      }
    }
  }
  expect(removedCapturePlace &&
             !lang::verifyMirProgram(missingCapturePlace).valid(),
         "MIR verification should reject an environment field whose exact "
         "capture projection was erased");

  const lang::FrontendResult parameterMoveCapture = lang::Frontend().analyze(
      "parameter-move-capture.gti", R"(
import std.memory;

class Owner {
  int value = 3;

public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  int read() { return this.value; }
};

int capture_parameter(Owner source) {
  auto action = [owned = std::move(source)]() -> int {
    return owned.read();
  };
  return action();
}

int main() { return capture_parameter(Owner()) - 3; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(parameterMoveCapture.canGenerateCode() &&
             parameterMoveCapture.hir.lambdaInstances().size() == 1 &&
             parameterMoveCapture.hir.lambdaInstances()
                     .front()
                     .captures.front()
                     .mode == lang::LambdaCaptureMode::Move &&
             lang::verifyMirProgram(parameterMoveCapture.mir).valid(),
         "a by-value parameter should be an exact owned move-capture source");

  const lang::FrontendResult invalidOwnedMoveCapture = lang::Frontend().analyze(
      "invalid-owned-move-capture.gti", R"(
import std.memory;

class Owner {
public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
};

class Immobile {
public:
  Immobile() {}
  Immobile(Immobile& other) = delete;
  Immobile(Immobile&& other) = delete;
};

class BorrowView {
  int& value;

public:
  BorrowView(int& value) : value(value) {}
};

int main() {
  Owner first{};
  auto moved = [owned = std::move(first)]() -> int { return 1; };
  auto use_after_move = [first]() -> int { return 2; };
  Owner second{};
  auto once = [owned = std::move(second)]() -> int { return 3; };
  auto twice = [again = std::move(second)]() -> int { return 4; };
  int value = 1;
  auto general = [renamed = value]() -> int { return renamed; };
  Immobile fixed{};
  auto immobile = [owned = std::move(fixed)]() -> int { return 5; };
  mut int number = 1;
  int& alias = number;
  BorrowView view{alias};
  auto borrowed = [owned = std::move(view)]() -> int { return 6; };
  return moved() + use_after_move() + once() + twice() + general() +
         immobile() + borrowed();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalidOwnedMoveCapture.canGenerateCode() &&
          hasDiagnostic(invalidOwnedMoveCapture.diagnostics,
                        "already been moved") &&
          hasDiagnostic(invalidOwnedMoveCapture.diagnostics,
                        "limited to the explicit owned-move form") &&
          hasDiagnostic(invalidOwnedMoveCapture.diagnostics, "is not movable"),
      "owned move capture should update source availability and reject "
      "general or non-movable init captures");

  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(R"(
int main() {
  auto bad_default = [=]() -> int { return 0; };
  auto bad_reference = [&value = source]() -> int { return value; };
  auto valid_syntax = [value = std::move(value)]() -> int { return 1; };
  int recovered = 1;
  return recovered;
}
)"));
  const lang::Program recovered = parser.parse();
  expect(parser.hadError() && !recovered.declarations().empty() &&
             hasDiagnostic(parser.errors(), "capture defaults") &&
             hasDiagnostic(parser.errors(),
                           "borrowing capture names an existing binding"),
         "the parser should reject capture defaults and borrowing init "
         "captures while "
         "recovering around the supported move-capture syntax");

  const std::string formatted = lang::Formatter().format(
      "int main(){int offset=1;auto add=[offset](int value)->int{return "
      "offset+value;};return add(1);}");
  expect(formatted.find("auto add = [offset](int value) -> int {\n") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "lambda syntax should receive stable C++-style formatting");
  const std::string moveCaptureFormatted = lang::Formatter().format(
      "int main(){Owner owner{};auto action=[owned=std::move(owner)]()->int{"
      "return owned.read();};return action();}");
  expect(moveCaptureFormatted.find("[owned = std::move(owner)]() -> int {\n") !=
                 std::string::npos &&
             lang::Formatter().format(moveCaptureFormatted) ==
                 moveCaptureFormatted,
         "owned move captures should receive stable C++-familiar formatting");
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

void testOwnedCallableTransport() {
  const lang::FrontendResult frontend =
      analyzeTrustedPreludeFixture("owned-callable-transport.gti", R"(
mut int cleanup_count = 0;

class DropTracer {
  int value;

public:
  DropTracer(int value) : value(value) {}
  ~DropTracer() { cleanup_count++; }
  int read() { return this.value; }
};

class CallableOwner<T> {
  T value;

public:
  CallableOwner(T value) : value(std::move(value)) {}
};

T relay<T>(T value) { return std::move(value); }

// The unmoved witness's cleanup can fail while the returned owner is pending;
// the moved-from parameter shell retires without a destructor.
CallableOwner<T> own<T>(T value, DropTracer witness) {
  return CallableOwner<T>(std::move(value));
}

void transport() {
  DropTracer tracer = DropTracer(7);
  auto operation = [owned = std::move(tracer)]() -> int {
    return owned.read();
  };
  auto returned = relay(std::move(operation));
  if (returned() != 7) { return; }
  auto owner = own(std::move(returned), DropTracer(1));
}

int main() {
  transport();
  return cleanup_count - 2;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected owned-callable diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "exact generic callable return and owner-field transport should "
         "reach valid MIR");

  const lang::FunctionDecl *relay =
      findTopLevelFunction(frontend.program, "relay");
  const lang::FunctionDecl *own = findTopLevelFunction(frontend.program, "own");
  const lang::FunctionInfo *relayInfo =
      relay == nullptr ? nullptr : frontend.semantics.findFunction(*relay);
  const lang::FunctionInfo *ownInfo =
      own == nullptr ? nullptr : frontend.semantics.findFunction(*own);
  expect(relayInfo != nullptr && relayInfo->callableParameters.size() == 1 &&
             relayInfo->callableParameters.front().boundary ==
                 lang::CallableBoundary::Owned &&
             relayInfo->callableParameters.front().ownedTransport &&
             relayInfo->callableParameters.front().ownedTransport->kind ==
                 lang::CallableOwnedTransportKind::ExactReturn &&
             ownInfo != nullptr && ownInfo->callableParameters.size() == 1 &&
             ownInfo->callableParameters.front().boundary ==
                 lang::CallableBoundary::Owned &&
             ownInfo->callableParameters.front().ownedTransport &&
             ownInfo->callableParameters.front().ownedTransport->kind ==
                 lang::CallableOwnedTransportKind::ExactField &&
             ownInfo->callableParameters.front().ownedTransport->field != 0,
         "semantics should distinguish exact-return and exact-field owned "
         "callable transports");

  std::size_t ownedHirFunctions = 0;
  std::size_t ownedMirFunctions = 0;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr ||
        (instance.source->name().lexeme != "relay" &&
         instance.source->name().lexeme != "own") ||
        instance.callableParameters.empty()) {
      continue;
    }
    const lang::HirCallableParameter &contract =
        instance.callableParameters.front();
    ownedHirFunctions +=
        contract.boundary == lang::CallableBoundary::Owned &&
                contract.callableType.kind == lang::SemanticType::Lambda &&
                contract.ownedTransport.has_value()
            ? 1
            : 0;
    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    ownedMirFunctions +=
        mir != nullptr && mir->callableParameters.size() == 1 &&
                mir->callableParameters.front().boundary ==
                    lang::CallableBoundary::Owned &&
                mir->callableParameters.front().ownedTransport.has_value()
            ? 1
            : 0;
  }
  const bool constructorProof = std::any_of(
      frontend.mir.constructorInstances().begin(),
      frontend.mir.constructorInstances().end(),
      [](const lang::MirConstructorInstance &constructor) {
        return std::any_of(
            constructor.initializers.begin(), constructor.initializers.end(),
            [](const lang::MirConstructorInitializer &initializer) {
              return initializer.ownedParameter ==
                         std::optional<std::size_t>{0} &&
                     initializer.field != 0 &&
                     initializer.arguments.size() == 1;
            });
      });
  const std::string mirDump = lang::MirPrinter().print(frontend.mir);
  expect(ownedHirFunctions == 2 && ownedMirFunctions == 2 && constructorProof &&
             mirDump.starts_with("mir-v121 ") &&
             mirDump.find("boundary=owned;transport=exact-return") !=
                 std::string::npos &&
             mirDump.find("boundary=owned;transport=exact-field") !=
                 std::string::npos &&
             mirDump.find("owned-parameter=0") != std::string::npos,
         "HIR, MIR, and deterministic dumps should retain concrete owned "
         "transport and constructor-field proof metadata");

  for (int mutation = 0; mutation < 3; ++mutation) {
    lang::MirProgram malformed = frontend.mir;
    bool changed = false;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        malformed.functionInstances());
    for (auto &function : functions) {
      const bool exactField = std::any_of(
          function.callableParameters.begin(),
          function.callableParameters.end(), [](const auto &parameter) {
            return parameter.ownedTransport &&
                   parameter.ownedTransport->kind ==
                       lang::CallableOwnedTransportKind::ExactField;
          });
      if (!exactField)
        continue;
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          for (auto &event : instruction.lifecycle) {
            const auto *source = function.body.findDropObligation(event.source);
            const auto *target = function.body.findDropObligation(event.target);
            if (mutation == 0 && target &&
                target->kind == lang::MirDropObligationKind::ReturnResult &&
                event.kind == lang::MirLifecycleEventKind::Reparent) {
              event.target = 0;
              changed = true;
            } else if (source &&
                       source->kind ==
                           lang::MirDropObligationKind::ReturnResult) {
              if (mutation == 1 &&
                  event.kind == lang::MirLifecycleEventKind::TransferOut) {
                event.source = 0;
                changed = true;
              } else if (mutation == 2 &&
                         event.kind == lang::MirLifecycleEventKind::Drop) {
                event.failureCleanup = false;
                changed = true;
              }
            }
          }
        }
      }
    }
    expect(changed && !lang::verifyMirProgram(malformed).valid(),
           "exact-field callable returns reject missing pending ownership, "
           "missing publication and non-failure rollback");
  }

  const lang::FrontendResult missingMove =
      analyzeTrustedPreludeFixture("owned-callable-missing-move.gti", R"(
T relay<T>(T value) { return std::move(value); }
int main() {
  int captured = 1;
  auto operation = [captured]() -> int { return captured; };
  auto escaped = relay(operation);
  return 0;
}
)");
  expect(
      !missingMove.canGenerateCode() &&
          hasDiagnosticCode(missingMove.diagnostics, "GTI-S2046") &&
          hasDiagnostic(missingMove.diagnostics,
                        "requires an explicit std::move ownership transfer") &&
          !hasDiagnosticCode(missingMove.diagnostics, "GTI-B0001"),
      "owned callable transport should require an explicit call-site "
      "ownership move");

  const lang::FrontendResult rawCapture =
      analyzeTrustedPreludeFixture("owned-callable-raw-capture.gti", R"(
T relay<T>(T value) { return std::move(value); }
int main() {
  int value = 1;
  unsafe {
    int* raw = &value;
    auto operation = [raw]() -> int { return 1; };
    auto escaped = relay(std::move(operation));
  }
  return 0;
}
)");
  expect(!rawCapture.canGenerateCode() &&
             hasDiagnosticCode(rawCapture.diagnostics, "GTI-S2046") &&
             hasDiagnostic(rawCapture.diagnostics,
                           "contains borrowed or raw-pointer capture state") &&
             !hasDiagnosticCode(rawCapture.diagnostics, "GTI-B0001"),
         "owned callable transport should reject raw-pointer capture state "
         "before HIR and MIR");

  const lang::FrontendResult nestedRawCapture =
      analyzeTrustedPreludeFixture("owned-callable-nested-raw-capture.gti", R"(
class RawState {
  int* pointer;

public:
  RawState(int* pointer) : pointer(pointer) {}
};

T relay<T>(T value) { return std::move(value); }
int main() {
  int value = 1;
  unsafe {
    int* raw = &value;
    RawState state = RawState(raw);
    auto operation = [state]() -> int { return 1; };
    auto escaped = relay(std::move(operation));
  }
  return 0;
}
)");
  expect(!nestedRawCapture.canGenerateCode() &&
             hasDiagnosticCode(nestedRawCapture.diagnostics, "GTI-S2046") &&
             hasDiagnostic(nestedRawCapture.diagnostics,
                           "contains borrowed or raw-pointer capture state") &&
             !hasDiagnosticCode(nestedRawCapture.diagnostics, "GTI-B0001"),
         "owned callable transport should inspect captured aggregate fields "
         "for raw-pointer state");

  const lang::FrontendResult rawSignature =
      analyzeTrustedPreludeFixture("owned-callable-raw-signature.gti", R"(
T relay<T>(T value) { return std::move(value); }
int main() {
  auto operation = [](int* pointer) -> int {
    return pointer == nullptr ? 0 : 1;
  };
  auto returned = relay(std::move(operation));
  return 0;
}
)");
  expect(rawSignature.canGenerateCode(),
         "raw pointers in a callable signature should not be mistaken for "
         "captured raw-pointer state");

  const lang::FrontendResult borrowedCapture =
      analyzeTrustedPreludeFixture("owned-callable-borrowed-capture.gti", R"(
T relay<T>(T value) { return std::move(value); }
int main() {
  std::string_view text = "borrowed";
  auto operation = [text]() -> int { return 1; };
  auto escaped = relay(std::move(operation));
  return 0;
}
)");
  expect(!borrowedCapture.canGenerateCode() &&
             hasDiagnosticCode(borrowedCapture.diagnostics, "GTI-S2046") &&
             hasDiagnostic(borrowedCapture.diagnostics,
                           "contains borrowed or raw-pointer capture state") &&
             !hasDiagnosticCode(borrowedCapture.diagnostics, "GTI-B0001"),
         "owned callable transport should reject borrowed view capture state");
}

void testExpectedValues() {
  lang::Lexer lexer;
  const std::string source = R"(
class MutableValue {
  mut int stored = 0;
public:
  void set(int value) mut { this.stored = value; }
  int get() { return this.stored; }
public:
  MutableValue() = default;
};

expected<int, std::string_view> calculate(bool fail) {
  if (fail) { return unexpected("calculation failed"); }
  return 42;
}
expected<void, std::string_view> render(bool fail) {
  if (fail) { return unexpected("render failed"); }
  return;
}
int main() {
  expected<int, std::string_view> result = calculate(false);
  if (!result.has_value()) { return 1; }
  int value = result.value_or(0);
  expected<void, std::string_view> rendered = render(false);
  if (!rendered) { return 2; }
  rendered.value();
  mut expected<MutableValue, int> mutable_result = MutableValue();
  mutable_result.value().set(42);
  if (mutable_result.value().get() != 42) { return 3; }
  [[discard]] calculate(false);
  return value - 42;
}
)";
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "expected source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "expected types and values should parse");

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "expected-values.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "expected construction and observers should pass semantic checks");

  // Each of the three result types is a concrete MIR instance of the
  // prelude class, and unexpected() is its ordinary source factory.
  const auto definedResult = [&](const lang::SemanticType &type) {
    return isExpectedClass(frontend, type) &&
           std::any_of(frontend.mir.classInstances().begin(),
                       frontend.mir.classInstances().end(),
                       [&](const lang::MirClassInstance &instance) {
                         return instance.type == type &&
                                !instance.declaredFields.empty();
                       });
  };
  const lang::MirFunctionInstance *calculateMir =
      mirFunctionNamed(frontend, "calculate");
  const lang::MirFunctionInstance *renderResultMir =
      mirFunctionNamed(frontend, "render");
  const bool mutableResult = std::any_of(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(), [&](const auto &instance) {
        const lang::ClassTypeInfo *payload =
            instance.type.arguments.size() == 2 &&
                    instance.type.arguments.front().kind ==
                        lang::SemanticType::Class
                ? frontend.semantics.findClassType(
                      instance.type.arguments.front().classId)
                : nullptr;
        return isExpectedClass(frontend, instance.type) && payload != nullptr &&
               payload->qualifiedName == "MutableValue";
      });
  bool sourceUnexpected = false;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    sourceUnexpected |= source != nullptr && source->source != nullptr &&
                        source->source->name().lexeme == "unexpected" &&
                        hasVerifiedSourceBody(&function);
  }
  expect(calculateMir != nullptr && definedResult(calculateMir->returnType) &&
             renderResultMir != nullptr &&
             definedResult(renderResultMir->returnType) &&
             calculateMir->returnType != renderResultMir->returnType &&
             mutableResult && sourceUnexpected &&
             mirCallsFunctionNamed(frontend, "unexpected") &&
             nativeBackendEmits(frontend),
         "expected values should lower to GTI's own source representation "
         "rather than a host or vendored library");
  // A bare return constructs the void result through its zero-argument
  // implicit constructor.
  const lang::MirFunctionInstance *renderMir = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    if (entryFunction(frontend, function.id) && source->source != nullptr &&
        source->source->name().lexeme == "render")
      renderMir = &function;
  }
  std::size_t emptySuccesses = 0;
  if (renderMir != nullptr) {
    for (const lang::MirBlock &block : renderMir->body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        emptySuccesses +=
            instruction.kind == lang::MirInstructionKind::Construct &&
            isExpectedClass(frontend, instruction.info.type) &&
            instruction.operands.empty();
  }
  expect(emptySuccesses == 1,
         "a void success should still return its empty value");

  const std::string invalidSource = R"(
expected<int, int> bad_success() { return "wrong"; }
expected<int, int> bad_error() { return unexpected("wrong"); }
expected<int, int> valid_result() { return 1; }
expected<void, int> complete() { return; }
int main() {
  valid_result();
  expected<int, int> result = bad_success();
  result.has_value();
  expected<void, int> completion = complete();
  completion.value_or(0);
  return 0;
}
)";
  auto invalidTokens = lexer.scan(invalidSource);
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid expected uses should remain semantic errors");

  // The prelude declares the result class, so the analysis must load it.
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-expected-values.gti", invalidSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(!invalid.semanticValid,
         "invalid expected types, states, and observers should fail");
  expect(std::count_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                       [](const lang::Diagnostic &diagnostic) {
                         return diagnostic.severity ==
                                lang::DiagnosticSeverity::Error;
                       }) == 5,
         "expected validation should produce focused diagnostics");

  // The result class's own static assertion rejects a void error alternative
  // when the concrete instance is formed. That failure is the only error: the
  // instance's layout and members do not report its consequences.
  const lang::FrontendResult voidError = lang::Frontend().analyze(
      "void-expected-error.gti",
      "expected<int, void> invalid_error() { return 1; }\n"
      "int main() { return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!voidError.semanticValid && voidError.diagnostics.size() == 1 &&
             voidError.diagnostics.front().code == "GTI-S2079" &&
             voidError.diagnostics.front().message ==
                 "Static assertion failed. An 'expected' error type cannot "
                 "be 'void'." &&
             voidError.diagnostics.front().primary.source.ends_with(
                 "void-expected-error.gti"),
         "a void error alternative is rejected once, by the result class's "
         "own assertion, at the program that names it");
}

void testFallibleBorrows() {
  const auto nestedPath = std::filesystem::path(__FILE__).parent_path() /
                          "fixtures/fallible_nested_borrow.gti";
  std::ifstream nestedInput(nestedPath);
  const std::string nestedSource{std::istreambuf_iterator<char>(nestedInput),
                                 {}};
  const auto nested =
      analyzeTrustedPreludeFixture("fallible-nested-borrow.gti", nestedSource);
  expect(nested.canGenerateCode() && lang::verifyMirProgram(nested.mir).valid(),
         "fallible read-only results should preserve mutable-receiver and "
         "nested-carrier origins");
  if (nested.canGenerateCode()) {
    std::size_t returnLoans = 0;
    bool checkedRuntimePlace = false;
    for (const auto &function : nested.mir.functionInstances()) {
      if (!entryFunction(nested, function.id) ||
          !isExpectedClass(nested, function.returnType))
        continue;
      for (const auto &loan : function.body.loans) {
        if (loan.kind != lang::MirLoanKind::Return || !loan.escapes)
          continue;
        ++returnLoans;
        expect(loan.access == lang::AccessMode::ReadOnly,
               "a fallible reference success must not inherit mutable receiver "
               "access");
        auto wrongAccess = nested.mir;
        auto *body = lang::findMirBody(
            wrongAccess, {lang::MirBodyKind::Function, function.id});
        body->loans[loan.id - 1].access = lang::AccessMode::Mutable;
        expect(!lang::verifyMirProgram(wrongAccess).valid(),
               "fallible borrowed returns reject a forged mutable publication "
               "capability");
      }
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          // The success constructor stages its referent argument with the
          // exact runtime place on the stored loan operand.
          if ((instruction.kind != lang::MirInstructionKind::Borrow &&
               instruction.kind != lang::MirInstructionKind::CallInput) ||
              instruction.operands.size() != 1 ||
              instruction.operands.front().kind != lang::MirOperandKind::Loan ||
              instruction.operands.front().place == 0)
            continue;
          const auto *place =
              function.body.findPlace(instruction.operands.front().place);
          if (place == nullptr || place->projections.size() != 3)
            continue;
          checkedRuntimePlace = true;
          auto wrongReferent = nested.mir;
          auto *body = lang::findMirBody(
              wrongReferent, {lang::MirBodyKind::Function, function.id});
          body->places[place->id - 1].projections.front().field = 0;
          expect(!lang::verifyMirProgram(wrongReferent).valid(),
                 "fallible nested-carrier return must retain the exact "
                 "designated runtime field path");
        }
      }
    }
    expect(returnLoans == 2 && checkedRuntimePlace,
           "cross-feature fixture must publish both return loans and its "
           "nested referent projection");
    bool checkedCopiedCarrier = false;
    for (const auto &constructor : nested.mir.constructorInstances()) {
      // The prelude's result constructors also store references.
      const lang::HirConstructorInstance *source =
          nested.hir.findConstructorInstance(constructor.id);
      if (source == nullptr ||
          source->sourceUnit != nested.sourceGraph.entryUnit())
        continue;
      for (const auto &loan : constructor.body.loans) {
        if (loan.kind != lang::MirLoanKind::Stored || !loan.escapes ||
            loan.storedField == 0)
          continue;
        for (const auto &block : constructor.body.blocks) {
          for (const auto &instruction : block.instructions) {
            if (instruction.kind != lang::MirInstructionKind::Borrow ||
                instruction.loan != loan.id ||
                instruction.operands.size() != 1 ||
                instruction.operands.front().kind != lang::MirOperandKind::Loan)
              continue;
            checkedCopiedCarrier = true;
            auto missingCarrier = nested.mir;
            auto *body = lang::findMirBody(
                missingCarrier,
                {lang::MirBodyKind::Constructor, constructor.id});
            for (auto &changedBlock : body->blocks)
              for (auto &changed : changedBlock.instructions)
                if (changed.id == instruction.id)
                  changed.operands.front().value = 0;
            expect(!lang::verifyMirProgram(missingCarrier).valid(),
                   "copied nested carrier publication must identify its exact "
                   "initialized class value");
          }
        }
      }
    }
    expect(checkedCopiedCarrier,
           "fallible fixture exercises a generated nested carrier copy");
    expect(nativeBackendEmits(nested),
           "fallible nested-carrier and mutable-receiver results must reach "
           "the native backend");
  }
  const std::string shellEscapeSource = R"(
enum class Error { missing };
struct View { int& item; View(int& item) : item(item) {} };
struct Envelope {
  View view;
  int shell = 5;
  Envelope(View view) : view(view) {}
};
expected<int&, Error> bad(Envelope owner) { return owner.shell; }
int main() { return 0; }
)";
  const auto shellEscape = analyzeTrustedPreludeFixture(
      "fallible-carrier-shell.gti", shellEscapeSource);
  // A class carrier's return obeys the general complete-dependency rule; the
  // shell field is not a caller-owned origin.
  expect(!shellEscape.canGenerateCode() &&
             hasDiagnosticAt(shellEscape.diagnostics, "GTI-S2045",
                             "gti-trusted-fallible-carrier-shell.gti",
                             shellEscapeSource.find("shell; }"),
                             "complete dependency set"),
         "fallible returns cannot substitute a same-typed local carrier field "
         "for its external referent");
  const lang::FrontendResult frontend =
      analyzeTrustedPreludeFixture("fallible-borrows.gti", R"(
enum class lookup_errc { missing };
int32_t global_value = 19;

class Box {
  int32_t value;
public:
  Box(int32_t value) : value(value) {}
  expected<int32_t&, lookup_errc> find(bool present) {
    if (!present) { return unexpected(lookup_errc::missing); }
    return this.value;
  }
};

class owning_error {
  int32_t code;
public:
  owning_error(int32_t code) : code(code + 1) {}
  owning_error(owning_error& other) = delete;
  owning_error(owning_error&& other) = default;
  ~owning_error() {}
};

expected<int32_t&, lookup_errc> borrow_parameter(int32_t& value,
                                                  bool present) {
  if (!present) { return unexpected(lookup_errc::missing); }
  return value;
}

expected<int32_t&, lookup_errc> borrow_global(bool present) {
  if (!present) { return unexpected(lookup_errc::missing); }
  return global_value;
}

expected<int32_t&, lookup_errc> forward(Box& box, bool present) {
  auto result = box.find(present);
  return result;
}

expected<int32_t&, owning_error> borrow_with_owning_error(int32_t& value,
                                                           bool present) {
  if (!present) { return unexpected(owning_error(0)); }
  return value;
}

int main() {
  Box box{7};
  auto found = forward(box, true);
  int32_t& alias = found.value();
  int32_t local = 13;
  expected<int32_t&, lookup_errc> direct = local;
  auto copied = direct;
  auto moved = std::move(copied);
  auto parameter = borrow_parameter(local, true);
  auto global = borrow_global(true);
  auto owned = borrow_with_owning_error(local, true);
  return alias + moved.value() + parameter.value() + global.value() +
         owned.value() - 65;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected fallible-borrow diagnostic: " << diagnostic.code
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "receiver-, parameter-, and global-tied fallible borrows should "
         "reach verified MIR, including local copy, move, and forwarding");
  // The retained `int32_t& alias = found.value()` extraction: its MIR loan
  // is produced by the prelude class's ordinary value() call.
  const MirRetained scalarExtraction = mirRetained(
      frontend,
      findDirectLocal(findTopLevelFunction(frontend.program, "main"), "alias"));
  bool scalarExtractionCall = false;
  if (scalarExtraction.loan != nullptr)
    for (const lang::MirBlock &block : scalarExtraction.function->body.blocks)
      for (const lang::MirInstruction &instruction : block.instructions)
        scalarExtractionCall =
            scalarExtractionCall ||
            (instruction.loan == scalarExtraction.loan->id &&
             isExpectedMemberCall(frontend, instruction, "value"));
  expect(scalarExtraction.loan != nullptr && scalarExtractionCall &&
             scalarExtraction.loan->access == lang::AccessMode::ReadOnly,
         "the fixture retains one scalar fallible-borrow extraction");
  expect(scalarExtraction.loan != nullptr &&
             scalarExtraction.loan->storedDependency == 0,
         "extracting a scalar fallible borrow retains the external "
         "referent, not a physical shell dependency on its carrier");

  std::size_t fallibleFunctions = 0;
  std::size_t escapingReturnLoans = 0;
  bool owningErrorCallHasInvoke = false;
  const auto parameterBorrowHir = std::find_if(
      frontend.hir.functionInstances().begin(),
      frontend.hir.functionInstances().end(),
      [](const lang::HirFunctionInstance &function) {
        return function.source != nullptr &&
               function.source->name().lexeme == "borrow_parameter";
      });
  const lang::MirFunctionInstance *parameterBorrowFunction =
      parameterBorrowHir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(parameterBorrowHir->id);
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::SemanticType &type = function.returnType;
    if (!entryFunction(frontend, function.id) ||
        !isExpectedClass(frontend, type) || type.arguments.size() != 2 ||
        type.arguments.front().kind != lang::SemanticType::Reference) {
      continue;
    }
    ++fallibleFunctions;
    escapingReturnLoans += std::count_if(
        function.body.loans.begin(), function.body.loans.end(),
        [](const lang::MirLoan &loan) {
          return loan.kind == lang::MirLoanKind::Return && loan.escapes;
        });
  }
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (!entryFunction(frontend, function.id))
      continue;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::MirFunctionInstance *target =
            instruction.functionTarget
                ? frontend.mir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (target == nullptr || !target->mayRaiseDefinedFailure ||
            !isExpectedClass(frontend, target->returnType) ||
            target->returnType.arguments.size() != 2 ||
            target->returnType.arguments.front().kind !=
                lang::SemanticType::Reference ||
            target->returnType.arguments.back().kind !=
                lang::SemanticType::Class ||
            !instruction.successResultDrop) {
          continue;
        }
        owningErrorCallHasInvoke =
            block.terminator.kind == lang::MirTerminatorKind::Invoke &&
            block.terminator.invokeInstruction == instruction.id;
      }
    }
  }
  expect(fallibleFunctions == 5 && escapingReturnLoans >= 5,
         "each fallible-borrow producer should publish an escaping Return "
         "loan while preserving its one owner dependency");
  expect(owningErrorCallHasInvoke,
         "a fallible-borrow call with an owning error should arm its result "
         "only on the exact failure-routing Invoke success edge");

  lang::MirProgram forgedReturn = frontend.mir;
  auto &forgedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forgedReturn.functionInstances());
  bool forgedRetirement = false;
  if (parameterBorrowFunction != nullptr) {
    const auto function =
        std::find_if(forgedFunctions.begin(), forgedFunctions.end(),
                     [&](const lang::MirFunctionInstance &candidate) {
                       return candidate.id == parameterBorrowFunction->id;
                     });
    if (function != forgedFunctions.end()) {
      const auto published = std::find_if(
          function->body.loans.begin(), function->body.loans.end(),
          [](const lang::MirLoan &loan) {
            return loan.kind == lang::MirLoanKind::Return && loan.escapes;
          });
      if (published != function->body.loans.end()) {
        for (lang::MirBlock &block : function->body.blocks) {
          for (std::size_t index = 0; index + 1 < block.instructions.size();
               ++index) {
            if (block.instructions[index].kind ==
                    lang::MirInstructionKind::Borrow &&
                block.instructions[index].loan == published->id &&
                block.instructions[index + 1].kind ==
                    lang::MirInstructionKind::EndBorrow) {
              block.instructions[index + 1].loan = published->id;
              forgedRetirement = true;
            }
          }
        }
      }
    }
  }
  expect(forgedRetirement && !lang::verifyMirProgram(forgedReturn).valid(),
         "MIR verification should reject a fallible-borrow return that "
         "retires the published loan instead of its exact source loan");

  // Each success construction, with a passive or an owning error, consumes
  // the verified Stored loan of its referent; failures carry no borrow.
  std::size_t verifiedSuccesses = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (!entryFunction(frontend, function.id))
      continue;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::SemanticType &type = instruction.info.type;
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            !isExpectedClass(frontend, type) || type.arguments.size() != 2 ||
            type.arguments.front().kind != lang::SemanticType::Reference ||
            !instruction.loan)
          continue;
        const lang::MirLoan *loan = function.body.findLoan(*instruction.loan);
        verifiedSuccesses +=
            loan != nullptr && loan->kind == lang::MirLoanKind::Stored;
      }
    }
  }
  expect(verifiedSuccesses == 5 && nativeBackendEmits(frontend),
         "the native backend should initialize passive or owning-error "
         "fallible-borrow representations from a verified loan");

  const lang::FrontendResult liveConflict =
      analyzeTrustedPreludeFixture("fallible-borrow-live-conflict.gti", R"(
enum class lookup_errc { missing };
class Box {
  mut int value;
public:
  Box(int value) : value(value) {}
  expected<int&, lookup_errc> find() { return this.value; }
  void set(int value) mut { this.value = value; }
};
int main() {
  mut Box box{7};
  auto found = box.find();
  box.set(9);
  return found.value();
}
)");
  expect(!liveConflict.canGenerateCode() &&
             (hasDiagnostic(liveConflict.diagnostics,
                            "overlaps a borrow that may still be live") ||
              (hasDiagnostic(liveConflict.diagnostics,
                             "while a reference borrowed from it may still be "
                             "live"))) &&
             hasRelatedDiagnostic(liveConflict.diagnostics,
                                  "Retained borrow originates here"),
         "a live fallible borrow should block mutation of its owner and "
         "identify the producing call");

  const lang::FrontendResult temporaryOwner =
      analyzeTrustedPreludeFixture("fallible-borrow-temporary.gti", R"(
enum class lookup_errc { missing };
class Box {
  int value;
public:
  Box(int value) : value(value) {}
  expected<int&, lookup_errc> find() { return this.value; }
};
int main() {
  auto found = Box(7).find();
  return found.value();
}
)");
  expect(!temporaryOwner.canGenerateCode() &&
             hasDiagnostic(temporaryOwner.diagnostics,
                           "derived from temporary storage"),
         "a fallible borrow must not outlive a temporary receiver");

  const std::string missingOwnerSource = R"(
enum class lookup_errc { missing };
expected<int&, lookup_errc> invalid() {
  int local = 7;
  return local;
}
int main() { return 0; }
)";
  const lang::FrontendResult missingOwner = analyzeTrustedPreludeFixture(
      "fallible-borrow-local-escape.gti", missingOwnerSource);
  // The local is not a caller-owned origin of the returned carrier.
  expect(!missingOwner.canGenerateCode() &&
             hasDiagnosticAt(missingOwner.diagnostics, "GTI-S2045",
                             "gti-trusted-fallible-borrow-local-escape.gti",
                             missingOwnerSource.find("local;"),
                             "complete dependency set"),
         "a local referent must not escape through a fallible borrow");

  const lang::FrontendResult ambiguousOwner =
      analyzeTrustedPreludeFixture("fallible-borrow-ambiguous.gti", R"(
enum class lookup_errc { missing };
expected<int&, lookup_errc> choose(int& first, int& second) { return first; }
int main() { return 0; }
)");
  // Under RFC 0004 signature contracts the elided result region is
  // ambiguous between the two reference parameters (Rust E0106).
  expect(!ambiguousOwner.canGenerateCode() &&
             (hasDiagnostic(ambiguousOwner.diagnostics,
                            "elided region is ambiguous among 2 "
                            "borrowed parameters")),
         "a free fallible borrow should require one unambiguous owner "
         "parameter");

  const std::string forbiddenStorageSource = R"(
enum class lookup_errc { missing };
expected<int&, lookup_errc> global_result;
class Holder { expected<int&, lookup_errc> result; };
void inspect(expected<int&, lookup_errc> result) {}
int main() {
  int local = 1;
  auto lambda = [](int& value) -> expected<int&, lookup_errc> {
    return value;
  };
  return 0;
}
)";
  const lang::FrontendResult forbiddenStorage = analyzeTrustedPreludeFixture(
      "fallible-borrow-storage.gti", forbiddenStorageSource);
  constexpr std::string_view forbiddenStorageFile =
      "gti-trusted-fallible-borrow-storage.gti";
  expect(
      !forbiddenStorage.canGenerateCode() &&
          hasDiagnosticAt(forbiddenStorage.diagnostics, "GTI-S2045",
                          forbiddenStorageFile,
                          forbiddenStorageSource.find("global_result"),
                          "cannot have global or static storage duration") &&
          hasDiagnosticAt(forbiddenStorage.diagnostics, "GTI-S2045",
                          forbiddenStorageFile,
                          forbiddenStorageSource.find("result; }"),
                          "fallible and other non-nominal carriers cannot be "
                          "stored as fields") &&
          hasDiagnosticAt(
              forbiddenStorage.diagnostics, "GTI-S2027", forbiddenStorageFile,
              forbiddenStorageSource.find(
                  "expected<int&", forbiddenStorageSource.find("auto lambda")),
              "Lambda return types cannot carry borrowed state"),
      "fallible borrows should not escape through fields, globals, or "
      "lambdas");
  // A by-value parameter is an ordinary carrier input: it retains the
  // caller's dependency for the call, and an error state carries no borrow.
  const lang::FrontendResult carrierParameter =
      analyzeTrustedPreludeFixture("fallible-borrow-parameter.gti", R"(
enum class lookup_errc { missing };
int read(expected<int&, lookup_errc> result) {
  return result.has_value() ? result.value() : 0;
}
int direct() {
  expected<int&, lookup_errc> result = unexpected(lookup_errc::missing);
  return result.has_value() ? 1 : 0;
}
int main() {
  mut int owner = 5;
  expected<int&, lookup_errc> found = owner;
  int seen = read(found);
  owner = 6;
  return seen + direct() - 5;
}
)");
  expect(carrierParameter.canGenerateCode(),
         "a fallible borrow may be passed by value and formed from an error");

  const std::string unsupportedSource = R"(
enum class lookup_errc { missing };
expected<int&, lookup_errc> borrow(int& value) { return value; }
expected<int&, lookup_errc> propagate(int& value) { return ?borrow(value); }
int fallback(int& value) {
  expected<int&, lookup_errc> result = value;
  return result.value_or(value);
}
int main() { return 0; }
)";
  const lang::FrontendResult unsupportedOperations =
      analyzeTrustedPreludeFixture("fallible-borrow-operations.gti",
                                   unsupportedSource);
  const bool unsupportedContract =
      !unsupportedOperations.canGenerateCode() &&
      countDiagnosticCode(unsupportedOperations.diagnostics, "GTI-S2086") ==
          0 &&
      hasDiagnostic(unsupportedOperations.diagnostics,
                    "Unknown member 'value_or'");
  if (!unsupportedContract) {
    for (const lang::Diagnostic &diagnostic :
         unsupportedOperations.diagnostics) {
      std::cerr << "  " << diagnostic.code << ' ' << diagnostic.primary.source
                << ':' << diagnostic.primary.start << ".."
                << diagnostic.primary.end << ' ' << diagnostic.message << '\n';
    }
  }
  expect(unsupportedContract,
         "an unsupported fallible-borrow fallback should fail semantically "
         "rather than in MIR or native C++");
}

void testExpectedPropagationOperator() {
  const std::string source = R"(
expected<int, int> leaf(bool fail, int error, int value) {
  if (fail) { return unexpected(error); }
  return value;
}

expected<int, int> sum(bool first, bool second) {
  return ?leaf(first, 11, 20) + ?leaf(second, 12, 22);
}

expected<void, int> complete(bool fail) {
  if (fail) { return unexpected(13); }
  return;
}

expected<void, int> forward_void(bool fail) {
  ?complete(fail);
  return;
}

expected<int, int> copy_named(bool fail) {
  expected<int, int> result = leaf(fail, 14, 40);
  return ?result;
}

int main() {
  auto forward = [](bool fail) -> expected<int, int> {
    return ?leaf(fail, 15, 42);
  };
  expected<int, int> value = forward(false);
  return value.value() - 42;
}
)";
  const lang::FrontendResult frontend =
      analyzeTrustedPreludeFixture("expected-propagation.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected propagation diagnostic: " << diagnostic.code
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "expected propagation should reach verified MIR in functions and "
         "lambdas");
  if (!frontend.canGenerateCode()) {
    return;
  }

  std::size_t propagationValues = 0;
  std::size_t copyingPropagations = 0;
  std::size_t movingPropagations = 0;
  // Prefix ? runs the std::tryable conformance: a test of the operand, then
  // its value or its residual, which constructs the enclosing return value.
  const auto inspectBody = [&](const lang::HirBody &body) {
    for (const lang::HirValue &value : body.values) {
      if (value.kind != lang::HirValueKind::Propagation) {
        continue;
      }
      ++propagationValues;
      const lang::HirPropagationPlan *plan =
          value.propagation ? &*value.propagation : nullptr;
      expect(value.source != nullptr && value.fullExpression != 0 &&
                 value.operation == lang::TokenKind::QUESTION &&
                 plan != nullptr && plan->operand != 0 && plan->test != 0 &&
                 plan->output != 0 && plan->residual != 0 &&
                 plan->residualReturn != 0,
             "HIR propagation should retain its source, full-expression, "
             "branch extractions, and unexpected return value");
      if (plan != nullptr && plan->consumesOperand) {
        ++movingPropagations;
      } else {
        ++copyingPropagations;
      }
    }
  };
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (entryFunction(frontend, function.id))
      inspectBody(function.body);
  }
  for (const lang::HirLambda &lambda : frontend.hir.lambdaInstances()) {
    inspectBody(lambda.body);
  }
  expect(propagationValues == 5 && copyingPropagations == 1 &&
             movingPropagations == 4,
         "HIR should distinguish copying a named expected from consuming four "
         "temporary results");

  const auto sumHir =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == "sum";
                   });
  const lang::MirFunctionInstance *sumMir =
      sumHir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(sumHir->id);
  std::size_t branches = 0;
  std::size_t returns = 0;
  std::size_t earlyBoundaries = 0;
  std::size_t primaryBoundaries = 0;
  if (sumMir != nullptr) {
    for (const lang::MirBlock &block : sumMir->body.blocks) {
      branches += block.terminator.kind == lang::MirTerminatorKind::Branch;
      returns += block.terminator.kind == lang::MirTerminatorKind::Return;
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.fullExpressionEnd == 0) {
          continue;
        }
        if (instruction.earlyExitFullExpressionEnd) {
          ++earlyBoundaries;
        } else {
          ++primaryBoundaries;
        }
      }
    }
  }
  expect(sumMir != nullptr && branches == 2 && returns == 3 &&
             earlyBoundaries == 2 && primaryBoundaries == 1,
         "MIR should lower two propagations in one expression to ordered, "
         "mutually exclusive early returns and one normal boundary");

  const lang::FrontendResult loopPropagation =
      analyzeTrustedPreludeFixture("expected-propagation-loop.gti", R"(
enum class parse_errc { bad_digit };
expected<int32_t, parse_errc> digit_value(int32_t value) {
  if (value < 0) { return unexpected(parse_errc::bad_digit); }
  return value;
}
expected<int32_t, parse_errc> sum_three() {
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 3; index++) {
    int32_t digit = ?digit_value(index);
    total = total + digit;
  }
  return total;
}
int main() { return sum_three().value() - 3; }
)");
  expect(loopPropagation.canGenerateCode() &&
             lang::verifyMirProgram(loopPropagation.mir).valid(),
         "a propagation boundary repeated by a loop should remain exclusive "
         "within each dynamic full-expression evaluation");

  const lang::FrontendResult manualExtraction =
      analyzeTrustedPreludeFixture("manual-expected-extraction.gti", R"(
expected<int, int> manual_leaf(bool fail) {
  if (fail) { return unexpected(16); }
  return 44;
}

int manual_extract(bool fail) {
  expected<int, int> result = manual_leaf(fail);
  return std::move(result).value();
}

int main() { return manual_extract(false) - 44; }
)");
  const auto manualExtractHir =
      std::find_if(manualExtraction.hir.functionInstances().begin(),
                   manualExtraction.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == "manual_extract";
                   });
  const lang::MirFunctionInstance *manualExtractMir =
      manualExtractHir == manualExtraction.hir.functionInstances().end()
          ? nullptr
          : manualExtraction.mir.findFunctionInstance(manualExtractHir->id);
  bool manualExtractionConsumesValue = false;
  if (manualExtractMir != nullptr) {
    for (const lang::MirBlock &block : manualExtractMir->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        // The consuming value() member receives the moved result by value.
        const lang::MirInstruction *stage =
            isExpectedMemberCall(manualExtraction, instruction, "value")
                ? definingInstruction(manualExtractMir->body,
                                      instruction.receiver->value)
                : nullptr;
        manualExtractionConsumesValue =
            manualExtractionConsumesValue ||
            (stage != nullptr &&
             stage->kind == lang::MirInstructionKind::CallInput &&
             stage->callInputKind == lang::HirCallInputKind::MoveValue);
      }
    }
  }
  expect(manualExtraction.canGenerateCode() &&
             lang::verifyMirProgram(manualExtraction.mir).valid() &&
             manualExtractionConsumesValue,
         "materializing propagation extractions must not turn an ordinary "
         "std::move(expected).value() receiver into a borrow");

  if (sumMir != nullptr) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    bool changedEarlyBoundary = false;
    for (lang::MirBlock &block : functions.at(sumHir->id - 1).body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.earlyExitFullExpressionEnd) {
          instruction.earlyExitFullExpressionEnd = false;
          changedEarlyBoundary = true;
          break;
        }
      }
      if (changedEarlyBoundary) {
        break;
      }
    }
    const lang::MirVerificationResult result = lang::verifyMirProgram(forged);
    expect(changedEarlyBoundary && !result.valid() &&
               std::any_of(result.errors.begin(), result.errors.end(),
                           [](const lang::MirVerificationError &error) {
                             return error.message.find(
                                        "exactly one primary boundary") !=
                                    std::string::npos;
                           }),
           "the MIR verifier should reject an early propagation endpoint "
           "forged into a second primary full-expression boundary");
  }

  // The branches call the selected std::tryable conformance functions.
  expect(mirCallsFunctionNamed(frontend, "has_value") &&
             mirCallsFunctionNamed(frontend, "take_value") &&
             mirCallsFunctionNamed(frontend, "take_residual") &&
             nativeBackendEmits(frontend),
         "the native backend should emit checked propagation branches through "
         "the selected conformance functions");

  const std::string formatted = lang::Formatter().format(
      "expected<int,int> forward(bool fail){return ? leaf(fail,1,2);}\n");
  expect(formatted.find("return ?leaf(fail, 1, 2);") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should keep '?' tight to its operand and remain "
         "idempotent");

  const lang::FrontendResult invalid =
      analyzeTrustedPreludeFixture("invalid-expected-propagation.gti", R"(
int non_expected_operand() { return ?1; }

int wrong_return_context() {
  expected<int, int> result = 1;
  return ?result;
}

expected<int, bool> wrong_error_type() {
  expected<int, int> result = 1;
  return ?result;
}

class MoveOnly {
public:
  MoveOnly() {}
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};

expected<MoveOnly, int> named_move_only() {
  expected<MoveOnly, int> result = MoveOnly();
  return ?result;
}

int main() { return 0; }
)");
  const bool invalidContract =
      !invalid.canGenerateCode() &&
      countDiagnosticCode(invalid.diagnostics, "GTI-S2086") == 4 &&
      hasDiagnostic(invalid.diagnostics, "expected<T, E>") &&
      hasDiagnostic(invalid.diagnostics, "cannot be initialized from it") &&
      hasDiagnostic(invalid.diagnostics,
                    "no implicit constructor taking exactly that residual") &&
      // The prelude's result class cannot gain a constructor from this
      // program, so the hint converts the failure instead.
      hasDiagnosticHint(invalid.diagnostics, "Convert the operand's failure") &&
      !hasDiagnosticHint(invalid.diagnostics, "Declare one 'implicit") &&
      hasDiagnosticHint(invalid.diagnostics, "std::move(result)");
  if (!invalidContract) {
    for (const lang::Diagnostic &diagnostic : invalid.diagnostics) {
      std::cerr << "Unexpected invalid-propagation diagnostic: "
                << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(invalidContract,
         "invalid propagation operands, return contexts, error types, and "
         "named move-only values should receive focused diagnostics");
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

void testOwnedParameterStageArmsRollback() {
  const std::string source = R"(
class Resource {
public:
  int32_t value;
  Resource(int32_t seed) : value(seed) {}
  ~Resource() {}
};

class Holder {
  Resource moved;
  int32_t checked;

public:
  Holder(Resource moved, int32_t bump)
      : moved(std::move(moved)), checked(bump + bump) {}
  int32_t total() { return this.moved.value + this.checked; }
};

int main() {
  Holder holder = Holder(Resource(3), 4);
  return holder.total() - 11;
}
)";
  const lang::FrontendResult frontend =
      analyzeTrustedPreludeFixture("unarmed-constructor.gti", source);
  expect(frontend.canGenerateCode(),
         "the owned-parameter constructor fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *holderConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() == 2) {
      holderConstructor = &constructor;
    }
  }
  expect(holderConstructor != nullptr,
         "the fixture should lower the owned-parameter constructor");
  if (holderConstructor == nullptr) {
    return;
  }
  const lang::MirBody &holderBody = holderConstructor->body;
  const bool ownedArmed = std::any_of(
      holderBody.dropObligations.begin(), holderBody.dropObligations.end(),
      [&](const lang::MirDropObligation &obligation) {
        return obligation.kind ==
               lang::MirDropObligationKind::ConstructionRollback;
      });
  expect(ownedArmed && lang::mirBodyRoutesFailureEdges(holderBody),
         "a staged owned-parameter field should arm rollback and keep the "
         "constructor routing failure edges");
  expect(!holderBody.failureRecords.empty(),
         "the checked operation after the staged owned-parameter transfer "
         "should keep its defined-failure edge");
  bool failureDropsMovedField = false;
  for (const lang::MirBlock &block : holderBody.blocks) {
    if (block.failureParameter == 0) {
      continue;
    }
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1 ||
          !instruction.lifecycle.front().failureCleanup) {
        continue;
      }
      const lang::MirDropObligation *obligation =
          holderBody.findDropObligation(instruction.lifecycle.front().source);
      failureDropsMovedField =
          failureDropsMovedField ||
          (obligation != nullptr &&
           obligation->kind ==
               lang::MirDropObligationKind::ConstructionRollback);
    }
  }
  expect(failureDropsMovedField,
         "the failure edge should drain the moved-in subobject through its "
         "armed rollback obligation");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "the staged owned-parameter constructor should remain verified MIR");

  const lang::FrontendResult owner =
      analyzeTrustedPreludeFixture("unique-owner-stage.gti", R"(
import std.memory;

class Entity {
public:
  int32_t id = 7;
public:
  Entity() = default;
};

class Slot {
  std::unique_ptr<Entity> value;

public:
  Slot(std::unique_ptr<Entity> value) : value(std::move(value)) {}
};

int main() {
  mut std::unique_ptr<Entity> entity = std::make_unique<Entity>();
  Slot slot = Slot(std::move(entity));
  return 0;
}
)");
  expect(owner.canGenerateCode(),
         "the unique-owner field fixture should pass the frontend");
  if (!owner.canGenerateCode()) {
    return;
  }
  bool uniqueOwnerArmed = false;
  for (const lang::MirConstructorInstance &constructor :
       owner.mir.constructorInstances()) {
    for (const lang::MirDropObligation &obligation :
         constructor.body.dropObligations) {
      uniqueOwnerArmed =
          uniqueOwnerArmed ||
          (obligation.kind ==
               lang::MirDropObligationKind::ConstructionRollback &&
           obligation.dropType.type.kind == lang::SemanticType::UniqueOwner);
    }
  }
  expect(uniqueOwnerArmed && lang::verifyMirProgram(owner.mir).valid(),
         "a unique-owner field completed from one owning argument should arm "
         "rollback and stay verified");
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

void testEphemeralBorrowedCarrierReceivers() {
  const std::string declarations = R"(
class Resource {
  mut int value = 0;
public:
  int add(int amount) mut { this.value += amount; return this.value; }
  int read() { return this.value; }
public:
  Resource() = default;
};
class AuthService {
  mut Resource& resource;
public:
  AuthService(mut Resource& source) : resource(source) {}
  int create_access_token(int amount) mut {
    return this.resource.add(amount);
  }
  int read() { return this.resource.read(); }
  Resource& borrow() { return this.resource; }
  int* pointer() { return nullptr; }
};
class RepositoryService {
  mut Resource& resource;
public:
  RepositoryService(mut Resource& source) : resource(source) {}
  int create(int amount) mut { return this.resource.add(amount); }
};
class Inspector {
  Resource& resource;
  mut int visits = 0;
public:
  Inspector(Resource& source) : resource(source) {}
  int read() { return this.resource.read(); }
  int visit(int amount) mut { this.visits += amount; return this.resource.read(); }
};
class Client {
  mut Resource resource{};
public:
  AuthService auth() mut { return AuthService(this.resource); }
  RepositoryService repositories() mut { return RepositoryService(this.resource); }
  int read() { return this.resource.read(); }
public:
  Client() = default;
};
AuthService make_service(mut Resource& resource) { return AuthService(resource); }
int inspect_service(AuthService& service) { return service.read(); }
int invalidate(Client owner) { return owner.read(); }
class ReadPair {
  Resource& first;
  Resource& second;
public:
  ReadPair(Resource& left, Resource& right) : first(left), second(right) {}
  int read() { return this.first.read() + this.second.read(); }
  int touch() mut { return this.first.read() + this.second.read(); }
};
class CarrierGuard {
public:
  CarrierGuard() {}
  ~CarrierGuard() {}
};
class CleanupReadPair {
  CarrierGuard guard{};
  Resource& first;
  Resource& second;
public:
  CleanupReadPair(Resource& left, Resource& right) : first(left), second(right) {}
  int read() { return this.first.read() + this.second.read(); }
};
class DestructorReadPair {
  CarrierGuard guard{};
  Resource& first;
  Resource& second;
public:
  DestructorReadPair(Resource& left, Resource& right) : first(left), second(right) {}
  ~DestructorReadPair() {
    [[discard]] this.first.read();
    [[discard]] this.second.read();
  }
  int read() { return this.first.read() + this.second.read(); }
};
class CleanupInspector {
  CarrierGuard guard{};
  Resource& resource;
public:
  CleanupInspector(Resource& source) : resource(source) {}
  int read(int divisor) { return this.resource.read() / divisor; }
};
int conditional_cleanup(Resource& resource, bool take) {
  return take ? CleanupInspector(resource).read(1)
              : CleanupInspector(resource).read(2);
}
class VirtualClient {
  mut Resource resource{};
public:
  virtual AuthService auth() mut { return AuthService(this.resource); }
public:
  VirtualClient() = default;
};
int combine(int first, int second) { return first + second; }
Resource global_resource{};
)";
  for (const std::string &body : {std::string(R"(
  mut Client api{};
  int token = api.auth().create_access_token(3);
  int after = api.read();
  int repository = api.repositories().create(2);
  int observed = api.auth().read();
  { mut AuthService service = api.auth(); [[discard]] service.create_access_token(1); }
  return api.read() + token + after + repository + observed;
)"),
                                  std::string(R"(
  mut Resource resource{};
  int direct = AuthService(resource).create_access_token(3);
  int factory = make_service(resource).create_access_token(2);
  int readonly = Inspector(resource).read();
  Resource& shared = resource;
  int inspected = Inspector(resource).visit(resource.read());
  int retained = shared.read();
  return resource.read() + direct + factory + readonly + inspected + retained;
)"),
                                  std::string(R"(
  return Inspector(global_resource).read();
)"),
                                  std::string(R"(
  Resource first{};
  Resource second{};
  return ReadPair(first, second).read();
)"),
                                  std::string(R"(
  Resource first{};
  Resource second{};
  return ReadPair(first, second).touch();
)"),
                                  std::string(R"(
  Resource first{};
  Resource second{};
  return CleanupReadPair(first, second).read();
)"),
                                  std::string(R"(
  Resource first{};
  Resource second{};
  return DestructorReadPair(first, second).read();
)"),
                                  std::string(R"(
  Resource resource{};
  return conditional_cleanup(resource, true);
)")}) {
    const auto frontend =
        lang::Frontend().analyze("ephemeral-carrier-valid.gti",
                                 declarations + "int main() {" + body + "}");
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << "Unexpected ephemeral carrier diagnostic: "
                  << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    expect(frontend.canGenerateCode(),
           "borrowed temporaries support immediate readonly and "
           "mutable method calls, preserving owner reuse between expressions");
    const bool dependentCleanup =
        body.find("DestructorReadPair(first, second)") != std::string::npos;
    if (frontend.canGenerateCode() &&
        (dependentCleanup ||
         body.find("CleanupReadPair(first, second)") != std::string::npos)) {
      bool checkedCarrier = false;
      bool checkedEarlyEnd = false;
      for (const auto &function : frontend.mir.functionInstances()) {
        const auto *hir = frontend.hir.findFunctionInstance(function.id);
        if (hir == nullptr || hir->source == nullptr ||
            hir->source->name().lexeme != "main")
          continue;
        for (const auto &shell : function.body.loans) {
          if (shell.kind != lang::MirLoanKind::CallArgument ||
              shell.storedDependency == 0)
            continue;
          const auto *place = function.body.findPlace(shell.source);
          const auto carrier = place == nullptr
                                   ? lang::MirTemporaryCarrierDependency{}
                                   : lang::findMirTemporaryCarrierDependency(
                                         function.body, *place);
          expect(
              carrier && carrier.loan->lifecycleBound == dependentCleanup &&
                  !shell.lifecycleBound &&
                  carrier.loan->additionalDependencies.size() == 1,
              "temporary retains both owners, marks only actual borrowed "
              "cleanup as lifecycle-bound, and separates its receiver shell");
          if (!carrier)
            continue;
          checkedCarrier = true;
          for (const auto id : {shell.id, carrier.loan->id}) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            changed->body.loans[id - 1].lifecycleBound =
                !changed->body.loans[id - 1].lifecycleBound;
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "temporary receiver cannot gain cleanup authority or "
                   "erase its producer's cleanup obligation");
          }
          for (const auto &block : function.body.blocks) {
            const auto drop = std::find_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto &instruction) {
                  return instruction.kind == lang::MirInstructionKind::Drop &&
                         instruction.destination == carrier.drop->place;
                });
            if (drop == block.instructions.end())
              continue;
            // Fallible destruction ends its retained borrow on the Invoke
            // continuation, not beside the Drop in the originating block.
            const auto *endBlock = &block;
            if (block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                block.terminator.invokeInstruction == drop->id)
              endBlock = function.body.findBlock(block.terminator.target);
            if (!endBlock)
              continue;
            const auto end = std::find_if(
                endBlock->instructions.begin(), endBlock->instructions.end(),
                [&](const auto &instruction) {
                  return instruction.kind ==
                             lang::MirInstructionKind::EndBorrow &&
                         instruction.loan == carrier.loan->id;
                });
            if (end == endBlock->instructions.end() ||
                (endBlock == &block && drop >= end))
              continue;
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            auto &instructions =
                changed->body.blocks[block.id - 1].instructions;
            const auto dropIndex = drop - block.instructions.begin();
            auto &ends = changed->body.blocks[endBlock->id - 1].instructions;
            const auto endIndex = end - endBlock->instructions.begin();
            const auto movedEnd = ends[endIndex];
            ends.erase(ends.begin() + endIndex);
            instructions.insert(instructions.begin() + dropIndex, movedEnd);
            const auto rejected = lang::verifyMirProgram(malformed);
            expect(!rejected.valid(),
                   "cleanup-bearing temporary cannot release its owners "
                   "before the exact full-expression value drop");
            checkedEarlyEnd = true;
          }
        }
      }
      expect(checkedCarrier && checkedEarlyEnd,
             "temporary exercises marker and early-end rejection proofs for "
             "both generated and fallible destructor cleanup");
    }
  }

  const auto temporaryMutex =
      analyzeTrustedPreludeFixture("ephemeral-mutex-escape.gti", R"(
import std.mutex;
int main() {
  std::mutex<int> owner{1};
  int& escaped = owner.lock().get();
  return escaped;
}
)");
  expect(!temporaryMutex.canGenerateCode(),
         "temporary mutex guard cannot export its protected reference");

  const auto argumentObserver = lang::Frontend().analyze(
      "ephemeral-carrier-argument.gti",
      declarations + "int main() { mut Client api{}; return "
                     "inspect_service(api.auth()); }");
  expect(
      argumentObserver.canGenerateCode() &&
          lang::verifyMirProgram(argumentObserver.mir).valid() &&
          checkedTemporaryObserver(argumentObserver, "inspect_service", true),
      "a readonly observer may borrow a fresh carrier while retaining its "
      "stable external owner");

  struct InvalidCase {
    std::string_view body;
    std::string_view reason;
    bool receiverDiagnostic = false;
  };
  const InvalidCase cases[] = {
      {"mut Client api{}; return api.auth().create_access_token(api.read());",
       "a later argument cannot read the exclusively borrowed owner"},
      {"mut Client api{}; return api.auth().create_access_token("
       "api.repositories().create(1));",
       "overlapping service producers cannot share an exclusive dependency"},
      {"mut Client api{}; return combine(api.auth().read(), api.read());",
       "a completed receiver invocation retains its stored dependency until "
       "the whole expression finishes"},
      {"mut Client api{}; return api.auth().read() > 0 && api.read() > 0 ? 1 : "
       "0;",
       "short-circuit evaluation does not release a carrier owner early"},
      {"mut Client api{}; AuthService& service = api.auth(); return 0;",
       "a temporary service cannot be retained through a reference"},
      {"mut Client api{}; [[discard]] api.auth().borrow(); return 0;",
       "even a readonly immediate receiver cannot export a borrowed result",
       true},
      {"mut Client api{}; unsafe { [[discard]] api.auth().pointer(); } return "
       "0;",
       "an immediate borrowed receiver cannot export a pointer-like result",
       true},
      {"return Client().auth().create_access_token(1);",
       "a temporary owner is not stable carrier storage", true},
      {"mut VirtualClient api{}; return api.auth().create_access_token(1);",
       "a virtual producer is not an ordinary directly resolved producer",
       true},
      {"mut Client api{}; return "
       "api.auth().create_access_token(invalidate(std::move(api)));",
       "a later argument cannot consume the borrowed owner"},
  };
  for (const auto &test : cases) {
    const auto frontend = lang::Frontend().analyze(
        "ephemeral-carrier-invalid.gti",
        declarations + "int main() {" + std::string(test.body) + "}");
    expect(!frontend.canGenerateCode(), std::string(test.reason));
    if (test.receiverDiagnostic) {
      const auto found =
          std::find_if(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                       [](const lang::Diagnostic &diagnostic) {
                         return diagnostic.code == "GTI-S2081";
                       });
      expect(found != frontend.diagnostics.end() && !found->related.empty() &&
                 !found->hints.empty(),
             "temporary carrier rejection retains the receiver diagnostic and "
             "related declaration/hint");
    }
  }
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
    {"testExplicitValueMoves", testExplicitValueMoves},
    {"testImplicitReturnMoves", testImplicitReturnMoves},
    {"testEdgeSensitiveMoveFlow", testEdgeSensitiveMoveFlow},
    {"testIndexedPlaceMovesAndInitialization",
     testIndexedPlaceMovesAndInitialization},
    {"testExclusiveReborrowLoanGraph", testExclusiveReborrowLoanGraph},
    {"testDisjointFieldReceiverBorrowsThroughReferenceParameters",
     testDisjointFieldReceiverBorrowsThroughReferenceParameters},
    {"testNonNullReferences", testNonNullReferences},
    {"testGlobalReferencePlaces", testGlobalReferencePlaces},
    {"testGlobalBorrowReturns", testGlobalBorrowReturns},
    {"testReceiverTiedReferenceReturns", testReceiverTiedReferenceReturns},
    {"testArgumentTiedReferenceReturns", testArgumentTiedReferenceReturns},
    {"testStoredReferenceGroundwork", testStoredReferenceGroundwork},
    {"testMutableStoredDependencies", testMutableStoredDependencies},
    {"testComposedCarrierFunctionTransfer",
     testComposedCarrierFunctionTransfer},
    {"testNestedBorrowedFields", testNestedBorrowedFields},
    {"testNestedMemberReferenceReturnForwarding",
     testNestedMemberReferenceReturnForwarding},
    {"testProjectedAccessorReturnLoanOrder",
     testProjectedAccessorReturnLoanOrder},
    {"testReferenceArgumentRuntimePlaceCoherence",
     testReferenceArgumentRuntimePlaceCoherence},
    {"testUniqueOwnerSwap", testUniqueOwnerSwap},
    {"testPolymorphicUniqueOwnerUpcast", testPolymorphicUniqueOwnerUpcast},
    {"testAggregateOwnershipTraits", testAggregateOwnershipTraits},
    {"testDefaultedDefaultLifecycle", testDefaultedDefaultLifecycle},
    {"testDestructorsAndActiveDropState", testDestructorsAndActiveDropState},
    {"testRestrictedMemberOperators", testRestrictedMemberOperators},
    {"testCallableMemberOperators", testCallableMemberOperators},
    {"testConsumingCallableOperators", testConsumingCallableOperators},
    {"testConsumingMethodsAndValueOperators",
     testConsumingMethodsAndValueOperators},
    {"testRangeBasedForAndIteratorProtocol",
     testRangeBasedForAndIteratorProtocol},
    {"testPassiveCleanupRangeTemporary", testPassiveCleanupRangeTemporary},
    {"testSharedPayloadSiblingReborrows", testSharedPayloadSiblingReborrows},
    {"testSourceOwnedExpectedRangeOrigins",
     testSourceOwnedExpectedRangeOrigins},
    {"testMutableRangeLoans", testMutableRangeLoans},
    {"testLambdas", testLambdas},
    {"testConfinedCallableParameters", testConfinedCallableParameters},
    {"testConfinedCallableResults", testConfinedCallableResults},
    {"testOwnedCallableTransport", testOwnedCallableTransport},
    {"testExpectedValues", testExpectedValues},
    {"testFallibleBorrows", testFallibleBorrows},
    {"testExpectedPropagationOperator", testExpectedPropagationOperator},
    {"testFieldInitializerRollbackAndFailureEdges",
     testFieldInitializerRollbackAndFailureEdges},
    {"testConstructorRollbackSpansLocalCleanup",
     testConstructorRollbackSpansLocalCleanup},
    {"testDefaultFieldRollbackEntersConstructor",
     testDefaultFieldRollbackEntersConstructor},
    {"testOwnedParameterStageArmsRollback",
     testOwnedParameterStageArmsRollback},
    {"testConstructorPartialRollbackRepresentation",
     testConstructorPartialRollbackRepresentation},
    {"testEphemeralBorrowedCarrierReceivers",
     testEphemeralBorrowedCarrierReceivers},
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
