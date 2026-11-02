#include "compiler_test_fixture.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "private_stdlib_fixture.h"

namespace {

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

lang::FrontendResult analyzeWithMemory(std::string_view name,
                                       std::string source) {
  source.insert(0, "import std.memory;\n");
  return lang::Frontend().analyze(std::filesystem::temp_directory_path() /
                                      std::string(name),
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

lang::FrontendResult analyzeSourceStorageFixture(std::string_view name,
                                                 std::string source) {
  // This helper is ordinary application policy: allocation failure is explicit,
  // and the tests exercise the public source-defined storage class.
  source.insert(0, R"(
std::storage<T> fixture_storage<T>(uint64_t count) requires std::storage_element<T> {
  std::allocator allocator{};
  auto allocated = allocator.try_storage<T>(count);
  return std::move(allocated).expect("test storage allocation");
}
)");
  return analyzeWithMemory(name, std::move(source));
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
  expect(
      artifact.contents.find("const std::int32_t value") == std::string::npos &&
          artifact.contents.find("const std::int32_t source") ==
              std::string::npos &&
          artifact.contents.find("const Record record") == std::string::npos &&
          artifact.contents.find(
              "// GTI verified-MIR body: scalar-cfg-failure-v1 "
              "function-instance") != std::string::npos &&
          artifact.contents.find("__gti_mir_v_1 = std::move(__gti_mir_p_1);") !=
              std::string::npos &&
          artifact.contents.find("__gti_entry__gti_mir_failure(") !=
              std::string::npos,
      "the C++ backend should keep explicitly consumed immutable bindings "
      "physically movable");

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

void testProjectedFieldMoves() {
  const std::string source = R"(
struct Node<T> {
  T value;
  mut std::unique_ptr<Node<T>> next;

public:
  Node(T initial, std::unique_ptr<Node<T>> next_node)
      : value(initial), next(std::move(next_node)) {}
};

class LinkedList<T> {
  mut std::unique_ptr<Node<T>> head = std::unique_ptr<Node<T>>();

public:
  void push_front(T value) mut {
    this.head =
        std::make_unique<Node<T>>(value, std::move(this.head));
  }

  void pop_front() mut {
    mut auto removed = std::move(this.head);
    this.head = std::move(removed->next);
  }

  T front() { return this.head->value; }
public:
  LinkedList() = default;
};

int main() {
  mut LinkedList<int> values = LinkedList<int>();
  values.push_front(1);
  values.push_front(2);
  values.pop_front();
  return values.front() - 1;
}
)";

  const lang::FrontendResult valid =
      analyzeWithMemory("projected-field-moves.gti", source);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected projected-move diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "writable fields should support explicit moves with definite "
         "reinitialization");

  bool foundReceiverFieldMove = false;
  bool foundDereferencedFieldMove = false;
  for (const lang::MirFunctionInstance &instance :
       valid.mir.functionInstances()) {
    for (const lang::MirBlock &block : instance.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Move ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Move) {
          continue;
        }
        const lang::MirPlace *place =
            instance.body.findPlace(instruction.operands.front().place);
        if (place == nullptr || place->projections.empty()) {
          continue;
        }
        const bool hasField = std::any_of(
            place->projections.begin(), place->projections.end(),
            [](const lang::MirPlaceProjection &projection) {
              return projection.kind == lang::MirProjectionKind::Field;
            });
        const bool hasDereference = std::any_of(
            place->projections.begin(), place->projections.end(),
            [](const lang::MirPlaceProjection &projection) {
              return projection.kind == lang::MirProjectionKind::Dereference;
            });
        foundReceiverFieldMove =
            foundReceiverFieldMove ||
            (place->root == lang::MirPlaceRootKind::This && hasField);
        foundDereferencedFieldMove =
            foundDereferencedFieldMove ||
            (hasField &&
             (hasDereference || place->root == lang::MirPlaceRootKind::Loan));
      }
    }
  }
  expect(foundReceiverFieldMove && foundDereferencedFieldMove,
         "MIR should retain receiver, dereference, and field projections on "
         "moves");

  const lang::FrontendResult restored =
      analyzeWithMemory("restored-owned-field.gti", R"(
struct Link {
  mut std::unique_ptr<int32_t> next = std::unique_ptr<int32_t>();
public:
  Link() = default;
};
std::unique_ptr<Link> detach() {
  mut auto owner = std::make_unique<Link>();
  auto previous = std::move(owner->next);
  owner->next = std::unique_ptr<int32_t>();
  return std::move(owner);
}
Link direct() {
  mut Link owner = Link();
  auto previous = std::move(owner.next);
  owner.next = std::unique_ptr<int32_t>();
  return std::move(owner);
}
int main() {
  auto indirect = detach();
  auto value = direct();
  return 0;
}
)");
  expect(restored.canGenerateCode(),
         "restoring direct and checked-arrow fields should permit moving the "
         "whole owner through verified MIR");
  bool foundRestoration = false;
  for (const lang::HirFunctionInstance &function :
       restored.hir.functionInstances()) {
    if (function.source == nullptr ||
        function.source->name().lexeme != "detach") {
      continue;
    }
    const lang::MirFunctionInstance *mir =
        restored.mir.findFunctionInstance(function.id);
    for (const lang::HirValue &value : function.body.values) {
      if (!value.ownership ||
          value.ownership->kind != lang::OwnershipEventKind::Reinitialize ||
          value.ownership->before != lang::OwnershipStateSet::Moved) {
        continue;
      }
      const lang::OwnershipEvent *semantic =
          value.source == nullptr
              ? nullptr
              : restored.semantics.findOwnershipEvent(*value.source);
      const lang::HirValue *arrow =
          value.operands.empty()
              ? nullptr
              : function.body.findValue(value.operands.front());
      expect(semantic != nullptr &&
                 semantic->kind == lang::OwnershipEventKind::Reinitialize &&
                 semantic->before == lang::OwnershipStateSet::Moved &&
                 value.kind == lang::HirValueKind::MemberSet &&
                 value.operation == lang::TokenKind::EQUAL &&
                 value.place == value.ownership->place && arrow != nullptr &&
                 arrow->kind == lang::HirValueKind::Call &&
                 arrow->functionTarget && arrow->callPlan && mir != nullptr,
             "HIR must retain the semantic restoration and selected arrow "
             "call separately from the field assignment");
      if (mir == nullptr) {
        continue;
      }
      for (const lang::MirBlock &block : mir->body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.hirValue != value.id ||
              instruction.kind != lang::MirInstructionKind::Assign) {
            continue;
          }
          const lang::MirPlace *destination =
              instruction.destination
                  ? mir->body.findPlace(*instruction.destination)
                  : nullptr;
          expect(instruction.ownership && destination != nullptr &&
                     destination->key == instruction.ownership->place &&
                     destination->root == lang::MirPlaceRootKind::Loan &&
                     instruction.operation == lang::MirOperation::Assign &&
                     lang::verifyMirBody(mir->body).valid(),
                 "MIR must store through the checked arrow loan and restore "
                 "the exact moved ownership key");
          foundRestoration = true;
          for (int mutation = 0; mutation < 3; ++mutation) {
            lang::MirBody forged = mir->body;
            for (lang::MirBlock &changedBlock : forged.blocks) {
              for (lang::MirInstruction &changed : changedBlock.instructions) {
                if (changed.id != instruction.id || !changed.ownership) {
                  continue;
                }
                if (mutation == 0) {
                  changed.ownership.reset();
                } else if (mutation == 1) {
                  changed.ownership->place.root += 10000;
                } else {
                  changed.ownership->before =
                      lang::OwnershipStateSet::Available;
                }
              }
            }
            expect(!lang::verifyMirBody(forged).valid(),
                   "missing, retargeted, or wrong-state field restoration "
                   "must fail MIR verification before whole-owner movement");
          }
        }
      }
    }
  }
  expect(foundRestoration,
         "checked field restoration must reach an executable MIR assignment");
  const lang::BackendArtifact restoredArtifact =
      gti_test::emitCpp(restored, restored.mir, restored.mir,
                        lang::OptimizationPipeline().run(
                            restored.hir, lang::OptimizationLevel::O0));
  expect(restoredArtifact.contents.find("// GTI verified-MIR body:") !=
             std::string::npos,
         "checked field restoration must emit from the production MIR path");

  const lang::FrontendResult invalid =
      analyzeWithMemory("invalid-projected-field-moves.gti", R"(
struct Item {
  int value;
public:
  Item(int initial) : value(initial) {}
  int read() { return this.value; }
};

struct Pair {
  mut std::unique_ptr<Item> left = std::make_unique<Item>(1);
  mut std::unique_ptr<Item> right = std::make_unique<Item>(2);

  int use_after_move() mut {
    auto removed = std::move(this.left);
    return this.left->read();
  }

  void leave_uninitialized() mut {
    auto removed = std::move(this.left);
  }
public:
  Pair() = default;
};

struct ImmutableField {
  std::unique_ptr<Item> value = std::make_unique<Item>(3);

  void take() mut {
    auto removed = std::move(this.value);
  }
public:
  ImmutableField() = default;
};

int main() {
  mut Pair pair = Pair();
  if (true) {
    auto removed = std::move(pair.left);
  }
  int maybe_moved = pair.left->read();

  mut Pair partial = Pair();
  auto removed = std::move(partial.left);
  Pair transferred = std::move(partial);
  return maybe_moved + transferred.right->read();
}
)");
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics, "has already been moved") &&
          hasDiagnostic(invalid.diagnostics,
                        "Method cannot return while part of 'this' remains "
                        "moved") &&
          hasDiagnostic(invalid.diagnostics,
                        "requires a writable field or constant-index") &&
          hasDiagnostic(invalid.diagnostics,
                        "one of its owned places remains moved"),
      "projected moves should reject unavailable fields, incomplete receiver "
      "returns, immutable places, and whole-owner transfer");

  const lang::FrontendResult nestedRead =
      analyzeWithMemory("nested-projected-read.gti", R"(
struct Item {
  int read() { return 1; }
public:
  Item() = default;
};

struct Holder {
  mut std::unique_ptr<Item> value = std::make_unique<Item>();
public:
  Holder() = default;
};

int observe(int value) { return value; }

int main() {
  mut Holder holder = Holder();
  auto removed = std::move(holder.value);
  [[discard]] std::move(observe(holder.value->read()));
  return 0;
}
)");
  expect(!nestedRead.canGenerateCode() &&
             hasDiagnostic(nestedRead.diagnostics,
                           "Place 'value' has already been moved") &&
             hasDiagnostic(nestedRead.diagnostics,
                           "temporaries are already values"),
         "projection-base analysis should not suppress unavailable nested "
         "field reads");
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
  return observe(shared_alias) + pair.left + pair.right - 10;
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
  const lang::BindingInfo *parentBinding =
      parent == nullptr ? nullptr : exclusive.semantics.findBinding(*parent);
  const lang::BindingInfo *childBinding =
      child == nullptr ? nullptr : exclusive.semantics.findBinding(*child);
  const lang::BindingInfo *grandchildBinding =
      grandchild == nullptr ? nullptr
                            : exclusive.semantics.findBinding(*grandchild);
  const lang::BindingInfo *leftBinding =
      left == nullptr ? nullptr : exclusive.semantics.findBinding(*left);
  const lang::BindingInfo *rightBinding =
      right == nullptr ? nullptr : exclusive.semantics.findBinding(*right);
  const lang::BindingInfo *pairParentBinding =
      pairParent == nullptr ? nullptr
                            : exclusive.semantics.findBinding(*pairParent);
  const lang::BindingInfo *projectedChildBinding =
      projectedChild == nullptr
          ? nullptr
          : exclusive.semantics.findBinding(*projectedChild);
  const lang::BindingInfo *sharedBinding =
      shared == nullptr ? nullptr : exclusive.semantics.findBinding(*shared);
  const lang::BindingInfo *sharedAliasBinding =
      sharedAlias == nullptr ? nullptr
                             : exclusive.semantics.findBinding(*sharedAlias);
  const lang::SemanticLoanInfo *parentLoan =
      parentBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(parentBinding->retainedLoan);
  const lang::SemanticLoanInfo *childLoan =
      childBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(childBinding->retainedLoan);
  const lang::SemanticLoanInfo *grandchildLoan =
      grandchildBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(grandchildBinding->retainedLoan);
  const lang::SemanticLoanInfo *leftLoan =
      leftBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(leftBinding->retainedLoan);
  const lang::SemanticLoanInfo *rightLoan =
      rightBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(rightBinding->retainedLoan);
  const lang::SemanticLoanInfo *pairParentLoan =
      pairParentBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(pairParentBinding->retainedLoan);
  const lang::SemanticLoanInfo *projectedChildLoan =
      projectedChildBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(projectedChildBinding->retainedLoan);
  expect(parentLoan != nullptr && childLoan != nullptr &&
             grandchildLoan != nullptr && childLoan->id != parentLoan->id &&
             grandchildLoan->id != childLoan->id &&
             childLoan->parent == parentLoan->id &&
             grandchildLoan->parent == childLoan->id &&
             parentLoan->access == lang::AccessMode::Mutable &&
             childLoan->access == lang::AccessMode::Mutable &&
             grandchildLoan->access == lang::AccessMode::Mutable &&
             parentLoan->place == childLoan->place &&
             childLoan->place == grandchildLoan->place,
         "mutable reborrows should form distinct parent-linked loans over one "
         "stable place");
  expect(leftLoan != nullptr && rightLoan != nullptr &&
             leftLoan->place.root == rightLoan->place.root &&
             leftLoan->place.projections.size() == 1 &&
             rightLoan->place.projections.size() == 1 &&
             leftLoan->place.projections.front().kind ==
                 lang::SemanticLoanPlaceProjectionKind::Field &&
             rightLoan->place.projections.front().kind ==
                 lang::SemanticLoanPlaceProjectionKind::Field &&
             leftLoan->place.projections.front().field !=
                 rightLoan->place.projections.front().field,
         "named sibling fields should retain disjoint stable projections");
  expect(pairParentLoan != nullptr && projectedChildLoan != nullptr &&
             projectedChildLoan->parent == pairParentLoan->id &&
             projectedChildLoan->place.projections.size() == 1,
         "a projected child should suspend only its overlapping parent place, "
         "not a known-disjoint sibling field");

  const lang::FunctionDecl *checkedDereferenceFunction = findTopLevelFunction(
      exclusive.program, "checked_dereference_reactivation");
  const lang::VariableDecl *checkedParent =
      checkedDereferenceFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                checkedDereferenceFunction->body()->statements().at(1).get());
  const lang::BindingInfo *checkedParentBinding =
      checkedParent == nullptr
          ? nullptr
          : exclusive.semantics.findBinding(*checkedParent);
  const lang::SemanticLoanInfo *checkedParentLoan =
      checkedParentBinding == nullptr
          ? nullptr
          : exclusive.semantics.findLoan(checkedParentBinding->retainedLoan);
  expect(checkedParentLoan != nullptr &&
             checkedParentLoan->place.projections.size() == 1 &&
             checkedParentLoan->place.projections.front().kind ==
                 lang::SemanticLoanPlaceProjectionKind::Dereference,
         "a checked owner dereference should retain its canonical stable-place "
         "projection through exclusive reborrow analysis");
  expect(sharedBinding != nullptr && sharedAliasBinding != nullptr &&
             sharedBinding->retainedLoan != 0 &&
             sharedBinding->retainedLoan == sharedAliasBinding->retainedLoan,
         "shared read-only aliases should continue sharing one semantic loan");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &function :
       exclusive.hir.functionInstances()) {
    if (function.source == mainFunction) {
      mainInstance = &function;
      break;
    }
  }
  const lang::HirLoan *hirChild =
      mainInstance == nullptr || childLoan == nullptr
          ? nullptr
          : mainInstance->body.findLoan(childLoan->id);
  const lang::HirLoan *hirGrandchild =
      mainInstance == nullptr || grandchildLoan == nullptr
          ? nullptr
          : mainInstance->body.findLoan(grandchildLoan->id);
  expect(exclusive.hirValid && hirChild != nullptr &&
             hirGrandchild != nullptr && hirChild->parent == parentLoan->id &&
             hirGrandchild->parent == childLoan->id &&
             !hirChild->carriers.empty() && !hirGrandchild->carriers.empty(),
         "HIR should preserve semantic loan identity, ancestry, place, and "
         "body-local carriers");

  const lang::Stmt *grandchildUse =
      mainFunction == nullptr ? nullptr
                              : mainFunction->body()->statements().at(4).get();
  const lang::HirStatement *useStatement = nullptr;
  if (mainInstance != nullptr) {
    const auto found = std::find_if(mainInstance->body.statements.begin(),
                                    mainInstance->body.statements.end(),
                                    [&](const lang::HirStatement &statement) {
                                      return statement.source == grandchildUse;
                                    });
    if (found != mainInstance->body.statements.end()) {
      useStatement = &*found;
    }
  }
  expect(useStatement != nullptr && useStatement->endedLoans.size() >= 2 &&
             useStatement->endedLoans[0] == grandchildLoan->id &&
             useStatement->endedLoans[1] == childLoan->id &&
             std::find(useStatement->endedLoans.begin(),
                       useStatement->endedLoans.end(),
                       parentLoan->id) == useStatement->endedLoans.end(),
         "descendant uses should extend ancestors without ending a parent "
         "that is used again later");
  const lang::Stmt *parentUse =
      mainFunction == nullptr ? nullptr
                              : mainFunction->body()->statements().at(7).get();
  expect(parentLoan != nullptr && parentUse != nullptr &&
             hasLoanEndpoint(parentLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             parentUse),
         "a parent's endpoint should follow the last descendant or direct "
         "parent use");

  const lang::FunctionDecl *endpointFunction =
      findTopLevelFunction(exclusive.program, "endpoint_chain");
  const auto *endpointParent =
      endpointFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                endpointFunction->body()->statements().at(1).get());
  const auto *endpointChild =
      endpointFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                endpointFunction->body()->statements().at(2).get());
  const lang::BindingInfo *endpointParentBinding =
      endpointParent == nullptr
          ? nullptr
          : exclusive.semantics.findBinding(*endpointParent);
  const lang::BindingInfo *endpointChildBinding =
      endpointChild == nullptr
          ? nullptr
          : exclusive.semantics.findBinding(*endpointChild);
  const lang::SemanticLoanId endpointParentLoan =
      endpointParentBinding == nullptr ? 0
                                       : endpointParentBinding->retainedLoan;
  const lang::SemanticLoanId endpointChildLoan =
      endpointChildBinding == nullptr ? 0 : endpointChildBinding->retainedLoan;
  const lang::HirFunctionInstance *endpointInstance = nullptr;
  for (const lang::HirFunctionInstance &function :
       exclusive.hir.functionInstances()) {
    if (function.source == endpointFunction) {
      endpointInstance = &function;
      break;
    }
  }
  const lang::Stmt *endpointUse =
      endpointFunction == nullptr
          ? nullptr
          : endpointFunction->body()->statements().at(3).get();
  const lang::HirStatement *endpointUseStatement = nullptr;
  if (endpointInstance != nullptr) {
    const auto found = std::find_if(endpointInstance->body.statements.begin(),
                                    endpointInstance->body.statements.end(),
                                    [&](const lang::HirStatement &statement) {
                                      return statement.source == endpointUse;
                                    });
    if (found != endpointInstance->body.statements.end()) {
      endpointUseStatement = &*found;
    }
  }
  expect(endpointParentLoan != 0 && endpointChildLoan != 0 &&
             endpointUseStatement != nullptr &&
             endpointUseStatement->endedLoans.size() >= 2 &&
             endpointUseStatement->endedLoans[0] == endpointChildLoan &&
             endpointUseStatement->endedLoans[1] == endpointParentLoan,
         "when a chain shares one final use, HIR should end descendants "
         "before their parents");

  const auto verifySwitchEndpoints = [&](const std::string &name,
                                         std::size_t armCount,
                                         bool unmatchedExit) {
    const lang::FunctionDecl *function =
        findTopLevelFunction(exclusive.program, name);
    const auto *child = function == nullptr
                            ? nullptr
                            : dynamic_cast<const lang::VariableDecl *>(
                                  function->body()->statements().at(2).get());
    const auto *selection =
        function == nullptr ? nullptr
                            : dynamic_cast<const lang::SwitchStmt *>(
                                  function->body()->statements().at(3).get());
    const lang::BindingInfo *binding =
        child == nullptr ? nullptr : exclusive.semantics.findBinding(*child);
    const lang::SemanticLoanId loan =
        binding == nullptr ? 0 : binding->retainedLoan;
    const lang::SemanticLoanInfo *semanticLoan =
        exclusive.semantics.findLoan(loan);
    const std::size_t semanticArmEnds =
        semanticLoan == nullptr
            ? 0
            : static_cast<std::size_t>(std::count_if(
                  semanticLoan->endpoints.begin(),
                  semanticLoan->endpoints.end(),
                  [&](const lang::SemanticLoanEndpoint &endpoint) {
                    return endpoint.kind ==
                               lang::SemanticLoanEndKind::SwitchArmEntry &&
                           endpoint.statement == selection;
                  }));
    const lang::HirFunctionInstance *instance = nullptr;
    for (const lang::HirFunctionInstance &candidate :
         exclusive.hir.functionInstances()) {
      if (candidate.source == function) {
        instance = &candidate;
        break;
      }
    }
    const lang::HirStatement *hirSwitch = nullptr;
    if (instance != nullptr) {
      const auto found = std::find_if(instance->body.statements.begin(),
                                      instance->body.statements.end(),
                                      [&](const lang::HirStatement &statement) {
                                        return statement.source == selection;
                                      });
      if (found != instance->body.statements.end()) {
        hirSwitch = &*found;
      }
    }
    const bool hirArmEnds =
        hirSwitch != nullptr && hirSwitch->switchArms.size() == armCount &&
        std::all_of(hirSwitch->switchArms.begin(), hirSwitch->switchArms.end(),
                    [&](const lang::HirSwitchArm &arm) {
                      return std::find(arm.entryEndedLoans.begin(),
                                       arm.entryEndedLoans.end(),
                                       loan) != arm.entryEndedLoans.end();
                    });
    const bool semanticExitEnd =
        !unmatchedExit ||
        hasLoanEndpoint(semanticLoan, lang::SemanticLoanEndKind::AfterStatement,
                        selection);
    const bool hirExitEnd =
        !unmatchedExit ||
        (hirSwitch != nullptr &&
         std::find(hirSwitch->endedLoans.begin(), hirSwitch->endedLoans.end(),
                   loan) != hirSwitch->endedLoans.end());
    expect(loan != 0 && semanticArmEnds == armCount && semanticExitEnd &&
               hirArmEnds && hirExitEnd,
           "a switch-subject final use should reactivate its parent at each "
           "selected arm and at the unmatched exit in " +
               name);
  };
  verifySwitchEndpoints("switch_reactivation", 2, false);
  verifySwitchEndpoints("switch_unmatched_reactivation", 1, true);

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
    const lang::BindingInfo *binding =
        child == nullptr ? nullptr
                         : retainedProjected.semantics.findBinding(*child);
    const lang::SemanticLoanInfo *semanticLoan =
        binding == nullptr
            ? nullptr
            : retainedProjected.semantics.findLoan(binding->retainedLoan);
    const lang::HirFunctionInstance *instance = nullptr;
    for (const lang::HirFunctionInstance &candidate :
         retainedProjected.hir.functionInstances()) {
      if (candidate.source == function) {
        instance = &candidate;
        break;
      }
    }
    const lang::HirLoan *hirLoan =
        instance == nullptr || binding == nullptr
            ? nullptr
            : instance->body.findLoan(binding->retainedLoan);
    const bool semanticProjection =
        semanticLoan != nullptr && semanticLoan->parent != 0 &&
        semanticLoan->place.projections.size() == 1 &&
        semanticLoan->place.projections.front().kind ==
            lang::SemanticLoanPlaceProjectionKind::Field;
    const bool hirProjection =
        semanticLoan != nullptr && hirLoan != nullptr && hirLoan->parent != 0 &&
        hirLoan->place.projections == semanticLoan->place.projections;
    expect(semanticProjection && hirProjection,
           "semantic and HIR retained loans should preserve one exact field "
           "projection in " +
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
                 "local child reborrow cannot escape"),
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
                 "Cannot assign to a field"),
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
                 "local child reborrow cannot escape"),
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
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(disjoint.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(disjoint, disjoint.mir, disjoint.mir, optimizations);
    expect(!artifact.contents.empty(),
           "the C++ backend should emit disjoint field receiver borrows "
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "restricted operator calls should retain verified MIR authority");
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find(
             "const ::__gti_program::Counter & __gti_mir_arg_0") !=
                 std::string::npos &&
             artifact.contents.find(
                 "::__gti_program::Counter & __gti_mir_arg_0") !=
                 std::string::npos &&
             artifact.contents.find("std::int32_t *__gti_mir_p_") !=
                 std::string::npos,
         "references should preserve const access in MIR signatures and use "
         "pointer carriers for retained local loans");

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
  inspect(3);
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
                        "cannot bind a reference to a temporary") &&
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
  std::array<const lang::SemanticLoanInfo *, 6> semanticLoans{};
  bool loansRetainGlobalPlaces = true;
  for (std::size_t index = 0; index < aliases.size(); ++index) {
    const lang::VariableDecl *alias =
        findDirectLocal(main, std::string(aliases[index]));
    const lang::BindingInfo *binding =
        alias == nullptr ? nullptr : valid.semantics.findBinding(*alias);
    semanticLoans[index] =
        binding == nullptr ? nullptr
                           : valid.semantics.findLoan(binding->retainedLoan);
    loansRetainGlobalPlaces = loansRetainGlobalPlaces &&
                              semanticLoans[index] != nullptr &&
                              semanticLoans[index]->place.root != 0 &&
                              !semanticLoans[index]->place.receiver;
  }
  expect(loansRetainGlobalPlaces,
         "semantic loans should retain a nonzero symbol-rooted place for "
         "every borrowed global or static binding");
  const auto constantIndex = [](const lang::SemanticLoanInfo *loan,
                                std::uint64_t index) {
    return loan != nullptr && loan->place.projections.size() == 1 &&
           loan->place.projections.front().kind ==
               lang::SemanticLoanPlaceProjectionKind::ConstantIndex &&
           loan->place.projections.front().index == index;
  };
  expect(constantIndex(semanticLoans[4], 0) &&
             constantIndex(semanticLoans[5], 1) &&
             semanticLoans[4]->place.root == semanticLoans[5]->place.root,
         "global fixed-array borrows should retain one shared symbol root "
         "with disjoint constant-index projections");

  const lang::HirFunctionInstance *mainHir = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == main) {
      mainHir = &instance;
      break;
    }
  }
  const lang::MirFunctionInstance *mainMir =
      mainHir == nullptr ? nullptr
                         : valid.mir.findFunctionInstance(mainHir->id);
  bool mirLoansUseSymbolPlaces = mainMir != nullptr;
  if (mainMir != nullptr) {
    for (const lang::SemanticLoanInfo *semanticLoan : semanticLoans) {
      const auto mirLoan =
          std::find_if(mainMir->body.loans.begin(), mainMir->body.loans.end(),
                       [&](const lang::MirLoan &candidate) {
                         return semanticLoan != nullptr &&
                                candidate.semanticLoan == semanticLoan->id;
                       });
      const lang::MirPlace *source =
          mirLoan == mainMir->body.loans.end()
              ? nullptr
              : mainMir->body.findPlace(mirLoan->source);
      mirLoansUseSymbolPlaces =
          mirLoansUseSymbolPlaces && source != nullptr &&
          source->root == lang::MirPlaceRootKind::Symbol && source->symbol != 0;
    }
  }
  expect(mirLoansUseSymbolPlaces,
         "MIR should lower semantic global-loan roots to concrete symbol "
         "places rather than producing an invalid empty operand");

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
  expect(mirDump.starts_with("mir-v119 ") &&
             mirDump.find("return-borrow-place=origin(root=") !=
                 std::string::npos &&
             mirDump.find("borrow-place=origin(root=") != std::string::npos,
         "deterministic MIR should serialize global function and call origin "
         "places");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(valid.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
  const std::string emittedGet =
      getInfo == nullptr ? std::string{}
                         : "static ::__gti_program::Application &__gti_fn_" +
                               std::to_string(getInfo->id) + "_Get()";
  const std::string emittedFailureGet = getInfo == nullptr
                                            ? std::string{}
                                            : "static bool __gti_fn_" +
                                                  std::to_string(getInfo->id) +
                                                  "_Get__gti_mir_failure(";
  // Reading the static pointer cannot fail, so the accessor has only its
  // plain form, which returns the loaned referent.
  const std::size_t getBody =
      getInfo == nullptr
          ? std::string::npos
          : artifact.contents.find("Application::__gti_fn_" +
                                   std::to_string(getInfo->id) + "_Get() {");
  const std::size_t getReturn =
      getBody == std::string::npos
          ? std::string::npos
          : artifact.contents.find("return *__gti_mir_loan_1;", getBody);
  expect(!emittedGet.empty() && !emittedFailureGet.empty() &&
             artifact.contents.find(emittedGet) != std::string::npos &&
             artifact.contents.find(emittedFailureGet) == std::string::npos &&
             artifact.contents.find("::__gti_program::Application::app;") !=
                 std::string::npos &&
             getReturn != std::string::npos &&
             getReturn < artifact.contents.find("\n  }", getBody),
         "the C++ backend should emit the static mutable-reference accessor "
         "without exposing unsafe syntax at its call sites");

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
  expect(!overlapping.canGenerateCode() &&
             countDiagnosticCode(overlapping.diagnostics, "GTI-S2017") == 1 &&
             hasDiagnostic(overlapping.diagnostics,
                           "Cannot create a mutable borrow while an "
                           "overlapping borrow") &&
             hasRelatedDiagnostic(overlapping.diagnostics,
                                  "Retained borrow originates here"),
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

// Ordinary source prefix ownership: constructor publication, relocation and
// receiver loans are established by general GTI calls and object lifetimes.
void testPrefixStorageCapability() {
  const auto valid =
      gti_test::analyzePrivateMemoryFixture("source-prefix-owner.gti", R"(
concept prefix_fixture_element<T> = std::storage_element<T> && std::relocatable<T>;
class Prefixed<prefix_fixture_element T> {
  mut std::detail::prefix_owner<T> data;
public:
  Prefixed(uint64_t capacity) : data(std::detail::prefix_owner<T>(capacity)) {}
  void push(T value) mut { this.data.append_value(std::move(value)); }
  void pop() mut { this.data.pop(); }
  T& at(uint64_t index) { return this.data.read(index); }
  mut T& at_mut(uint64_t index) mut { return this.data.read_mut(index); }
  uint64_t length() { return this.data.length(); }
  void take_from(mut Prefixed<T>& source) mut { source.data.relocate_to(this.data); }
};
int main() {
  mut Prefixed<int> values{uint64_t(4)};
  values.push(7);
  values.push(9);
  if (values.length() != uint64_t(2) or values.at(uint64_t(0)) != 7) return 1;
  mut int& slot = values.at_mut(uint64_t(1));
  slot = 11;
  values.pop();
  mut Prefixed<int> moved{uint64_t(4)};
  moved.take_from(values);
  return moved.length() == uint64_t(1) && moved.at(uint64_t(0)) == 7 ? 0 : 2;
}
)");
  expect(valid.canGenerateCode(), "ordinary source prefix owner must pass");
  if (valid.canGenerateCode()) {
    bool checkedRead = false, checkedWrite = false;
    for (const auto &function : valid.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          checkedRead |=
              instruction.intrinsic == lang::IntrinsicKind::RawBorrow;
          checkedWrite |=
              instruction.intrinsic == lang::IntrinsicKind::RawBorrowMut;
        }
    const auto artifact = gti_test::emitCppText(valid);
    expect(concreteBackendSymbols(artifact, "__gti_type_", "Prefixed").size() ==
                   1 &&
               artifact.find("construct_representation_at(") !=
                   std::string::npos &&
               artifact.find("mir_object_relocate_v1(") != std::string::npos &&
               checkedRead && checkedWrite &&
               artifact.find("class prefix_storage") == std::string::npos &&
               artifact.find("mir_prefix_") == std::string::npos,
           "source prefix policy emits ordinary classes and general "
           "lifetime/borrow operations");
  }

  const auto optionalMisuse = gti_test::analyzeTrustedPrivateMemoryFixture(
      "source-prefix-optional-misuse.gti", R"(
import std.optional;
int main() {
  mut std::detail::prefix_owner<int> values{uint64_t(1)};
  return int(gti_internal::optional_storage_read(values));
}
)");
  expect(!optionalMisuse.canGenerateCode() &&
             hasDiagnostic(optionalMisuse.diagnostics,
                           "Undefined qualified name") &&
             !hasDiagnosticCode(optionalMisuse.diagnostics, "GTI-B0001"),
         "retired optional private operations are no longer declared by the "
         "standard library");

  const auto retired =
      analyzeTrustedMemoryFixture("retired-prefix-operations.gti", R"(
int main() {
  mut gti_internal::prefix_storage<int> values =
      gti_internal::allocate_prefix_storage<int>(uint64_t(1));
  return int(gti_internal::prefix_storage_read(values, uint64_t(0)));
}
)");
  expect(!retired.canGenerateCode() &&
             hasDiagnostic(retired.diagnostics, "Unknown type") &&
             hasDiagnostic(retired.diagnostics, "Undefined qualified name"),
         "retired prefix declarations are no longer provided by the prelude");
  const auto ordinarySpelling =
      analyzeTrustedMemoryFixture("ordinary-prefix-spelling.gti", R"(
namespace gti_internal {
  class prefix_storage<std::copy_constructible T> {
  public:
    T value;
    prefix_storage(T initial) : value(initial) {}
  };
}
int main() {
  gti_internal::prefix_storage<int> first{7};
  gti_internal::prefix_storage<int> second = first;
  return second.value - 7;
}
)");
  expect(ordinarySpelling.canGenerateCode(),
         "even a trusted declaration with the old spelling has no special "
         "capability identity");

  const auto rejectsLiveLoan = [&](std::string_view borrowed,
                                   std::string_view invocation) {
    const auto result =
        gti_test::analyzePrivateMemoryFixture("source-prefix-live-loan.gti",
                                              R"(
int main() {
  mut std::detail::prefix_owner<int> values{uint64_t(2)};
  mut std::detail::prefix_owner<int> other{uint64_t(2)};
  values.append_value(7);
  other.append_value(9);
  int& value = )" + std::string(borrowed) + R"(.read(uint64_t(0));
)" + std::string(invocation) + R"(
  return value;
}
)");
    expect(
        !result.canGenerateCode() &&
            hasDiagnosticCode(result.diagnostics, "GTI-S2017") &&
            (hasDiagnostic(result.diagnostics, "overlapping borrow") ||
             hasDiagnostic(result.diagnostics, "reference borrowed from it") ||
             hasDiagnostic(result.diagnostics,
                           "Mutable reference argument overlaps a borrow")),
        "ordinary prefix mutation must reject its live " +
            std::string(borrowed) + " loan");
  };
  rejectsLiveLoan("values", "values.pop();");
  rejectsLiveLoan("values", "values.append_value(11);");
  rejectsLiveLoan("values", "values.insert(uint64_t(0), 11);");
  rejectsLiveLoan("values", "values.erase(uint64_t(0));");
  rejectsLiveLoan("values", "mut int& edit = values.read_mut(uint64_t(0));");
  rejectsLiveLoan("values", "values.relocate_to(other);");
  rejectsLiveLoan("other", "values.relocate_to(other);");

  const auto immutable =
      gti_test::analyzePrivateMemoryFixture("source-prefix-immutable.gti", R"(
int main() {
  std::detail::prefix_owner<int> values{uint64_t(1)};
  values.pop();
  return 0;
}
)");
  expect(!immutable.canGenerateCode() &&
             hasDiagnosticCode(immutable.diagnostics, "GTI-S2000") &&
             hasDiagnostic(immutable.diagnostics,
                           "Mutable method requires a mutable receiver"),
         "ordinary prefix mutators require a writable receiver");

  const auto endedLoan =
      gti_test::analyzePrivateMemoryFixture("source-prefix-ended-loan.gti", R"(
int main() {
  mut std::detail::prefix_owner<int> values{uint64_t(2)};
  mut std::detail::prefix_owner<int> other{uint64_t(2)};
  values.append_value(7);
  other.append_value(9);
  int& value = values.read(uint64_t(0));
  other.pop();
  int copied = value;
  values.pop();
  other.append_value(9);
  other.relocate_to(values);
  mut int& edit = values.read_mut(uint64_t(0));
  edit = copied;
  values.pop();
  return 0;
}
)");
  expect(
      endedLoan.canGenerateCode(),
      "prefix mutations permit disjoint owners and ended shared/mutable loans");
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // Box<int>::get() cannot fail: its plain form returns the exact loaned
  // referent. The checked prefix read keeps its failure-form loan output.
  const std::size_t plainGet = artifact.contents.find("_get() const {");
  expect(
      plainGet != std::string::npos &&
          artifact.contents.find("return *__gti_mir_loan_1;", plainGet) <
              artifact.contents.find("\n  }", plainGet) &&
          concreteBackendSymbols(artifact.contents, "__gti_member_", "get")
                  .size() == 1 &&
          artifact.contents.find("const std::int32_t *__gti_mir_loan_1{};") !=
              std::string::npos &&
          artifact.contents.find("const std::int32_t *__gti_mir_loan_2{};") !=
              std::string::npos &&
          artifact.contents.find("__gti_mir_loan_2 = __gti_mir_loan_1;") !=
              std::string::npos &&
          artifact.contents.find("*__gti_mir_out_result = __gti_mir_loan_2;") !=
              std::string::npos &&
          hasMirIntrinsicKind(frontend, lang::IntrinsicKind::RawBorrow) &&
          artifact.contents.find("mir_prefix_read_v1") == std::string::npos,
      "receiver-tied borrows should lower through exact const loan carriers "
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
    const lang::OptimizationResult nestedOptimizations =
        lang::OptimizationPipeline().run(nested.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact nestedArtifact =
        gti_test::emitCpp(nested, nested.mir, nested.mir, nestedOptimizations);
    const std::string transferText =
        "__gti_mir_loan_" + std::to_string(nestedReturnLoan->id) +
        " = __gti_mir_loan_" + std::to_string(callResultLoan->id) + ";";
    expect(nestedArtifact.contents.find(transferText) != std::string::npos,
           "the C++ backend should emit the verified call-result pointer "
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
  expect(countDiagnosticCode(invalidated.diagnostics, "GTI-S2017") == 3 &&
             hasDiagnostic(invalidated.diagnostics,
                           "Cannot move storage while an overlapping borrow") &&
             hasDiagnostic(invalidated.diagnostics,
                           "Mutable method cannot use move-only storage"),
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // The regression the runtime test catches: without an instruction that
  // names it, the emitted callee published a default-initialized pointer.
  expect(
      !artifact.contents.empty() &&
          artifact.contents.find("auto *__gti_mir_loan_1 = __gti_mir_p_1;") !=
              std::string::npos &&
          artifact.contents.find("__gti_mir_loan_2 = __gti_mir_loan_1;") !=
              std::string::npos,
      "the C++ backend should alias the reference parameter's carrier and "
      "publish it through the returned loan");
}

void testStoredReferenceGroundwork() {
  const std::string source = R"(
struct SingleSentinel { public: SingleSentinel() = default; };

class BorrowingIterator<T> {
  T& value;
  mut uint64_t position = 0;

public:
  BorrowingIterator(T& source) : value(source) {}

  T& operator*() { return value; }
  void operator++() mut { this.position++; }
  bool operator!=(BorrowingIterator<T>& other) {
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

class WrappedRange<T> {
  SingleRange<T>& internal;

public:
  WrappedRange(SingleRange<T>& source) : internal(source) {}

  BorrowingIterator<T> begin() {
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
             originalIteratorBinding->retainedLoan != 0 &&
             originalIteratorBinding->retainedLoan ==
                 movedIteratorBinding->retainedLoan,
         "moving borrowed state should transfer one semantic loan identity "
         "instead of creating a second owner dependency");

  const lang::FrontendResult standardStringRange = lang::Frontend().analyze(
      "standard-string-range.gti",
      "import std.string;\n"
      "class PathRange { std::string internal_path; public: "
      "PathRange(std::string value) : internal_path(std::move(value)) {} "
      "std::string::const_iterator begin() { return "
      "internal_path.begin(); } };\n"
      "int main() { std::string value = std::string(\"gti\"); "
      "mut uint64_t count = 0; for (char character : value) { count++; } "
      "PathRange path = PathRange(std::string(\"gti\")); "
      "mut auto iterator = path.begin(); char first = *iterator; "
      "return int(count) - 3; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!standardStringRange.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : standardStringRange.diagnostics) {
      std::cerr << "Unexpected std::string range diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(standardStringRange.canGenerateCode(),
         "the source-defined standard string should expose read-only "
         "owner-tied iteration");

  const lang::FrontendResult releasedStandardStringBorrow =
      lang::Frontend().analyze(
          "released-standard-string-borrow.gti",
          "import std.string;\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); char first = *iterator; "
          "value.push_back('!'); return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!releasedStandardStringBorrow.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic :
         releasedStandardStringBorrow.diagnostics) {
      std::cerr << "Unexpected released-borrow diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(releasedStandardStringBorrow.canGenerateCode(),
         "a retained string iterator should release its owner after its last "
         "straight-line use");
  const lang::FunctionDecl *releasedMain =
      findTopLevelFunction(releasedStandardStringBorrow.program, "main");
  const lang::VariableDecl *releasedIterator =
      releasedMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                releasedMain->body()->statements().at(1).get());
  const lang::Stmt *releasedLastUse =
      releasedMain == nullptr ? nullptr
                              : releasedMain->body()->statements().at(2).get();
  const lang::BindingInfo *releasedBinding =
      releasedIterator == nullptr
          ? nullptr
          : releasedStandardStringBorrow.semantics.findBinding(
                *releasedIterator);
  const lang::SemanticLoanInfo *releasedLoan =
      releasedBinding == nullptr
          ? nullptr
          : releasedStandardStringBorrow.semantics.findLoan(
                releasedBinding->retainedLoan);
  expect(releasedLoan != nullptr && releasedLoan->protectsStorage &&
             hasLoanEndpoint(releasedLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             releasedLastUse),
         "semantic loan metadata should identify the owner dependency and its "
         "proven last-use statement");

  const auto releasedHirFunction =
      releasedMain == nullptr
          ? releasedStandardStringBorrow.hir.functionInstances().end()
          : std::find_if(
                releasedStandardStringBorrow.hir.functionInstances().begin(),
                releasedStandardStringBorrow.hir.functionInstances().end(),
                [&](const lang::HirFunctionInstance &candidate) {
                  return candidate.source == releasedMain;
                });
  const lang::HirStatement *releasedHirLastUse = nullptr;
  if (releasedHirFunction !=
      releasedStandardStringBorrow.hir.functionInstances().end()) {
    const auto statement =
        std::find_if(releasedHirFunction->body.statements.begin(),
                     releasedHirFunction->body.statements.end(),
                     [&](const lang::HirStatement &candidate) {
                       return candidate.source == releasedLastUse;
                     });
    if (statement != releasedHirFunction->body.statements.end()) {
      releasedHirLastUse = &*statement;
    }
  }
  expect(releasedHirLastUse != nullptr && releasedBinding != nullptr &&
             std::find(releasedHirLastUse->endedLoans.begin(),
                       releasedHirLastUse->endedLoans.end(),
                       releasedBinding->retainedLoan) !=
                 releasedHirLastUse->endedLoans.end(),
         "HIR should carry the semantic loan endpoint without recomputing it");

  const lang::MirFunctionInstance *releasedMirFunction =
      releasedHirFunction ==
              releasedStandardStringBorrow.hir.functionInstances().end()
          ? nullptr
          : releasedStandardStringBorrow.mir.findFunctionInstance(
                releasedHirFunction->id);
  const lang::MirLoan *releasedMirLoan = nullptr;
  if (releasedMirFunction != nullptr && releasedBinding != nullptr) {
    const auto loan = std::find_if(releasedMirFunction->body.loans.begin(),
                                   releasedMirFunction->body.loans.end(),
                                   [&](const lang::MirLoan &candidate) {
                                     return candidate.semanticLoan ==
                                            releasedBinding->retainedLoan;
                                   });
    if (loan != releasedMirFunction->body.loans.end()) {
      releasedMirLoan = &*loan;
    }
  }
  bool foundLastUseEnd = false;
  if (releasedMirFunction != nullptr && releasedMirLoan != nullptr &&
      releasedHirLastUse != nullptr) {
    for (const lang::MirBlock &block : releasedMirFunction->body.blocks) {
      foundLastUseEnd =
          foundLastUseEnd ||
          std::any_of(block.instructions.begin(), block.instructions.end(),
                      [&](const lang::MirInstruction &instruction) {
                        return instruction.kind ==
                                   lang::MirInstructionKind::EndBorrow &&
                               instruction.loan == releasedMirLoan->id &&
                               instruction.hirStatement ==
                                   releasedHirLastUse->id;
                      });
    }
  }
  expect(foundLastUseEnd,
         "MIR should end the retained loan at the frontend-proven statement");

  const lang::FrontendResult invalidStandardStringBorrow =
      lang::Frontend().analyze(
          "invalid-standard-string-borrow.gti",
          "import std.string;\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); value.push_back('!'); "
          "char first = *iterator; return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidStandardStringBorrow.canGenerateCode() &&
             hasDiagnostic(invalidStandardStringBorrow.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a retained string iterator should prevent storage-invalidating "
         "mutation");

  const lang::FrontendResult controlFlowBorrow = lang::Frontend().analyze(
      "control-flow-string-borrow.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { char first = "
      "*iterator; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!controlFlowBorrow.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : controlFlowBorrow.diagnostics) {
      std::cerr << "Unexpected conditional-borrow diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(controlFlowBorrow.canGenerateCode(),
         "an unshared retained borrow should end after an if join when its "
         "final use is inside a branch");
  const lang::FunctionDecl *controlFlowMain =
      findTopLevelFunction(controlFlowBorrow.program, "main");
  const auto *controlFlowIterator =
      controlFlowMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                controlFlowMain->body()->statements().at(1).get());
  const lang::Stmt *controlFlowIf =
      controlFlowMain == nullptr
          ? nullptr
          : controlFlowMain->body()->statements().at(2).get();
  const lang::BindingInfo *controlFlowBinding =
      controlFlowIterator == nullptr
          ? nullptr
          : controlFlowBorrow.semantics.findBinding(*controlFlowIterator);
  const lang::SemanticLoanInfo *controlFlowLoan =
      controlFlowBinding == nullptr ? nullptr
                                    : controlFlowBorrow.semantics.findLoan(
                                          controlFlowBinding->retainedLoan);
  expect(hasLoanEndpoint(controlFlowLoan,
                         lang::SemanticLoanEndKind::AfterStatement,
                         controlFlowIf),
         "semantic loan metadata should project a branch-local final use to "
         "the enclosing if join");
  const auto controlFlowHirFunction =
      controlFlowMain == nullptr
          ? controlFlowBorrow.hir.functionInstances().end()
          : std::find_if(controlFlowBorrow.hir.functionInstances().begin(),
                         controlFlowBorrow.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == controlFlowMain;
                         });
  const lang::HirStatement *controlFlowHirIf = nullptr;
  if (controlFlowHirFunction !=
      controlFlowBorrow.hir.functionInstances().end()) {
    const auto statement =
        std::find_if(controlFlowHirFunction->body.statements.begin(),
                     controlFlowHirFunction->body.statements.end(),
                     [&](const lang::HirStatement &candidate) {
                       return candidate.source == controlFlowIf;
                     });
    if (statement != controlFlowHirFunction->body.statements.end()) {
      controlFlowHirIf = &*statement;
    }
  }
  expect(controlFlowHirIf != nullptr && controlFlowBinding != nullptr &&
             std::find(controlFlowHirIf->endedLoans.begin(),
                       controlFlowHirIf->endedLoans.end(),
                       controlFlowBinding->retainedLoan) !=
                 controlFlowHirIf->endedLoans.end(),
         "HIR should retain the semantic loan endpoint on the if statement");
  const lang::MirFunctionInstance *controlFlowMirFunction =
      controlFlowHirFunction == controlFlowBorrow.hir.functionInstances().end()
          ? nullptr
          : controlFlowBorrow.mir.findFunctionInstance(
                controlFlowHirFunction->id);
  bool foundConditionalEnd = false;
  if (controlFlowMirFunction != nullptr && controlFlowHirIf != nullptr &&
      controlFlowBinding != nullptr) {
    const auto loan = std::find_if(controlFlowMirFunction->body.loans.begin(),
                                   controlFlowMirFunction->body.loans.end(),
                                   [&](const lang::MirLoan &candidate) {
                                     return candidate.semanticLoan ==
                                            controlFlowBinding->retainedLoan;
                                   });
    if (loan != controlFlowMirFunction->body.loans.end()) {
      for (const lang::MirBlock &block : controlFlowMirFunction->body.blocks) {
        foundConditionalEnd =
            foundConditionalEnd ||
            std::any_of(block.instructions.begin(), block.instructions.end(),
                        [&](const lang::MirInstruction &instruction) {
                          return instruction.kind ==
                                     lang::MirInstructionKind::EndBorrow &&
                                 instruction.loan == loan->id &&
                                 instruction.hirStatement ==
                                     controlFlowHirIf->id;
                        });
      }
    }
  }
  expect(foundConditionalEnd,
         "MIR should end the retained loan after lowering the if merge");

  const lang::FrontendResult bothBranchBorrow = lang::Frontend().analyze(
      "both-branch-string-borrow.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { char left = "
      "*iterator; } else { char right = *iterator; } value.push_back('!'); "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(bothBranchBorrow.canGenerateCode(),
         "a retained borrow used on both conditional paths should end once "
         "after their merge");

  const lang::FrontendResult nestedConditionalBorrow = lang::Frontend().analyze(
      "nested-conditional-string-borrow.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { if (true) { "
      "char first = *iterator; } } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(nestedConditionalBorrow.canGenerateCode(),
         "a nested conditional use should project to the outer join that "
         "dominates the following mutation");
  const lang::FunctionDecl *nestedConditionalMain =
      findTopLevelFunction(nestedConditionalBorrow.program, "main");
  const auto *nestedConditionalIterator =
      nestedConditionalMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                nestedConditionalMain->body()->statements().at(1).get());
  const lang::BindingInfo *nestedConditionalBinding =
      nestedConditionalIterator == nullptr
          ? nullptr
          : nestedConditionalBorrow.semantics.findBinding(
                *nestedConditionalIterator);
  const lang::SemanticLoanInfo *nestedConditionalLoan =
      nestedConditionalBinding == nullptr
          ? nullptr
          : nestedConditionalBorrow.semantics.findLoan(
                nestedConditionalBinding->retainedLoan);
  expect(hasLoanEndpoint(
             nestedConditionalLoan, lang::SemanticLoanEndKind::AfterStatement,
             nestedConditionalMain->body()->statements().at(2).get()),
         "nested conditional uses should end at the outermost required join");

  const lang::FrontendResult conditionalCarrierMove = lang::Frontend().analyze(
      "conditional-carrier-move.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { mut auto moved = "
      "std::move(iterator); char first = *moved; } value.push_back('!'); "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!conditionalCarrierMove.canGenerateCode() &&
             hasDiagnostic(conditionalCarrierMove.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a carrier moved on only one branch must keep its owner loan "
         "conservative after the join");

  const lang::FrontendResult terminatingConditionalBorrow =
      lang::Frontend().analyze(
          "terminating-conditional-string-borrow.gti",
          "import std.string;\n"
          "int consume(bool choose) { mut std::string value = "
          "std::string(\"gti\"); mut auto iterator = value.begin(); if "
          "(choose) { char first = *iterator; return 1; } else { return 0; "
          "} }\n"
          "int main() { return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(terminatingConditionalBorrow.canGenerateCode(),
         "a terminating conditional should rely on path cleanup instead of "
         "creating an unreachable join endpoint");

  const lang::FrontendResult laterConditionalUse = lang::Frontend().analyze(
      "later-conditional-string-borrow.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { char first = "
      "*iterator; } value.push_back('!'); char second = *iterator; "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!laterConditionalUse.canGenerateCode() &&
             hasDiagnostic(laterConditionalUse.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a use after an if must keep the retained loan live across an "
         "intervening invalidation");

  const lang::FrontendResult branchLocalInvalidation = lang::Frontend().analyze(
      "branch-local-string-invalidation.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (value.empty()) { char first = "
      "*iterator; value.push_back('!'); } else { value.push_back('?'); } "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(branchLocalInvalidation.canGenerateCode(),
         "a retained borrow should end independently on both conditional "
         "paths before a branch-local invalidation");
  const lang::FunctionDecl *branchLocalMain =
      findTopLevelFunction(branchLocalInvalidation.program, "main");
  const auto *branchLocalIterator =
      branchLocalMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                branchLocalMain->body()->statements().at(1).get());
  const auto *branchLocalIf =
      branchLocalMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                branchLocalMain->body()->statements().at(2).get());
  const auto *branchLocalThen = branchLocalIf == nullptr
                                    ? nullptr
                                    : dynamic_cast<const lang::BlockStmt *>(
                                          branchLocalIf->thenBranch().get());
  const lang::Stmt *branchLocalLastUse =
      branchLocalThen == nullptr ? nullptr
                                 : branchLocalThen->statements().front().get();
  const lang::BindingInfo *branchLocalBinding =
      branchLocalIterator == nullptr
          ? nullptr
          : branchLocalInvalidation.semantics.findBinding(*branchLocalIterator);
  const lang::SemanticLoanInfo *branchLocalLoan =
      branchLocalBinding == nullptr
          ? nullptr
          : branchLocalInvalidation.semantics.findLoan(
                branchLocalBinding->retainedLoan);
  expect(branchLocalLoan != nullptr && branchLocalLoan->endpoints.size() == 2 &&
             hasLoanEndpoint(branchLocalLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             branchLocalLastUse) &&
             hasLoanEndpoint(branchLocalLoan,
                             lang::SemanticLoanEndKind::ElseBranchEntry,
                             branchLocalIf),
         "semantic loan metadata should describe both path-specific endings");

  const auto branchLocalHirFunction =
      branchLocalMain == nullptr
          ? branchLocalInvalidation.hir.functionInstances().end()
          : std::find_if(
                branchLocalInvalidation.hir.functionInstances().begin(),
                branchLocalInvalidation.hir.functionInstances().end(),
                [&](const lang::HirFunctionInstance &candidate) {
                  return candidate.source == branchLocalMain;
                });
  const lang::HirStatement *branchLocalHirIf = nullptr;
  const lang::HirStatement *branchLocalHirLastUse = nullptr;
  if (branchLocalHirFunction !=
      branchLocalInvalidation.hir.functionInstances().end()) {
    for (const lang::HirStatement &statement :
         branchLocalHirFunction->body.statements) {
      if (statement.source == branchLocalIf) {
        branchLocalHirIf = &statement;
      }
      if (statement.source == branchLocalLastUse) {
        branchLocalHirLastUse = &statement;
      }
    }
  }
  const lang::SemanticLoanId branchLocalLoanId =
      branchLocalBinding == nullptr ? 0 : branchLocalBinding->retainedLoan;
  expect(branchLocalHirIf != nullptr && branchLocalHirLastUse != nullptr &&
             std::find(branchLocalHirLastUse->endedLoans.begin(),
                       branchLocalHirLastUse->endedLoans.end(),
                       branchLocalLoanId) !=
                 branchLocalHirLastUse->endedLoans.end() &&
             std::find(branchLocalHirIf->elseEntryEndedLoans.begin(),
                       branchLocalHirIf->elseEntryEndedLoans.end(),
                       branchLocalLoanId) !=
                 branchLocalHirIf->elseEntryEndedLoans.end(),
         "HIR should preserve statement and branch-entry loan endpoints");

  const lang::MirFunctionInstance *branchLocalMirFunction =
      branchLocalHirFunction ==
              branchLocalInvalidation.hir.functionInstances().end()
          ? nullptr
          : branchLocalInvalidation.mir.findFunctionInstance(
                branchLocalHirFunction->id);
  std::size_t branchLocalEndCount = 0;
  bool branchLocalLastUseEnd = false;
  bool branchLocalElseEntryEnd = false;
  if (branchLocalMirFunction != nullptr) {
    const auto loan =
        std::find_if(branchLocalMirFunction->body.loans.begin(),
                     branchLocalMirFunction->body.loans.end(),
                     [&](const lang::MirLoan &candidate) {
                       return candidate.semanticLoan == branchLocalLoanId;
                     });
    if (loan != branchLocalMirFunction->body.loans.end()) {
      for (const lang::MirBlock &block : branchLocalMirFunction->body.blocks) {
        if (block.failureParameter != 0) {
          continue;
        }
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
              instruction.loan != loan->id || instruction.hirStatement == 0) {
            continue;
          }
          ++branchLocalEndCount;
          branchLocalLastUseEnd =
              branchLocalLastUseEnd ||
              (branchLocalHirLastUse != nullptr &&
               instruction.hirStatement == branchLocalHirLastUse->id);
          branchLocalElseEntryEnd =
              branchLocalElseEntryEnd ||
              (branchLocalHirIf != nullptr &&
               instruction.hirStatement == branchLocalHirIf->id);
        }
      }
    }
  }
  expect(branchLocalMirFunction != nullptr && branchLocalEndCount == 2 &&
             branchLocalLastUseEnd && branchLocalElseEntryEnd &&
             lang::verifyMirBody(branchLocalMirFunction->body).valid(),
         "MIR should end the same loan once on each conditional path");

  const lang::FrontendResult unbracedBranchInvalidation =
      lang::Frontend().analyze(
          "unbraced-branch-invalidation.gti",
          "import std.string;\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); mut char observed = 'x'; if "
          "(true) observed = *iterator; else value.push_back('!'); "
          "value.push_back('?'); if (observed == 'g') { return 0; } return "
          "1; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(unbracedBranchInvalidation.canGenerateCode(),
         "unbraced if arms should receive the same path-specific loan "
         "endpoints as braced arms");
  const lang::FunctionDecl *unbracedMain =
      findTopLevelFunction(unbracedBranchInvalidation.program, "main");
  const auto *unbracedIterator =
      unbracedMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                unbracedMain->body()->statements().at(1).get());
  const auto *unbracedIf =
      unbracedMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                unbracedMain->body()->statements().at(3).get());
  const lang::Stmt *unbracedThen =
      unbracedIf == nullptr ? nullptr : unbracedIf->thenBranch().get();
  const lang::BindingInfo *unbracedBinding =
      unbracedIterator == nullptr
          ? nullptr
          : unbracedBranchInvalidation.semantics.findBinding(*unbracedIterator);
  const lang::SemanticLoanInfo *unbracedLoan =
      unbracedBinding == nullptr
          ? nullptr
          : unbracedBranchInvalidation.semantics.findLoan(
                unbracedBinding->retainedLoan);
  expect(unbracedLoan != nullptr && unbracedLoan->endpoints.size() == 2 &&
             hasLoanEndpoint(unbracedLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             unbracedThen) &&
             hasLoanEndpoint(unbracedLoan,
                             lang::SemanticLoanEndKind::ElseBranchEntry,
                             unbracedIf),
         "unbraced semantic endpoints should identify the direct statement "
         "and unused branch entry");

  const auto unbracedHirFunction =
      unbracedMain == nullptr
          ? unbracedBranchInvalidation.hir.functionInstances().end()
          : std::find_if(
                unbracedBranchInvalidation.hir.functionInstances().begin(),
                unbracedBranchInvalidation.hir.functionInstances().end(),
                [&](const lang::HirFunctionInstance &candidate) {
                  return candidate.source == unbracedMain;
                });
  const lang::HirStatement *unbracedHirIf = nullptr;
  const lang::HirStatement *unbracedHirThen = nullptr;
  if (unbracedHirFunction !=
      unbracedBranchInvalidation.hir.functionInstances().end()) {
    for (const lang::HirStatement &statement :
         unbracedHirFunction->body.statements) {
      if (statement.source == unbracedIf) {
        unbracedHirIf = &statement;
      }
      if (statement.source == unbracedThen) {
        unbracedHirThen = &statement;
      }
    }
  }
  const lang::SemanticLoanId unbracedLoanId =
      unbracedBinding == nullptr ? 0 : unbracedBinding->retainedLoan;
  expect(unbracedHirIf != nullptr && unbracedHirThen != nullptr &&
             std::find(unbracedHirThen->endedLoans.begin(),
                       unbracedHirThen->endedLoans.end(),
                       unbracedLoanId) != unbracedHirThen->endedLoans.end() &&
             std::find(unbracedHirIf->elseEntryEndedLoans.begin(),
                       unbracedHirIf->elseEntryEndedLoans.end(),
                       unbracedLoanId) !=
                 unbracedHirIf->elseEntryEndedLoans.end(),
         "HIR should retain loan endpoints on recursively lowered unbraced "
         "statements");
  const lang::MirFunctionInstance *unbracedMirFunction =
      unbracedHirFunction ==
              unbracedBranchInvalidation.hir.functionInstances().end()
          ? nullptr
          : unbracedBranchInvalidation.mir.findFunctionInstance(
                unbracedHirFunction->id);
  std::size_t unbracedEndCount = 0;
  bool unbracedThenEnd = false;
  bool unbracedElseEnd = false;
  if (unbracedMirFunction != nullptr) {
    const auto loan =
        std::find_if(unbracedMirFunction->body.loans.begin(),
                     unbracedMirFunction->body.loans.end(),
                     [&](const lang::MirLoan &candidate) {
                       return candidate.semanticLoan == unbracedLoanId;
                     });
    if (loan != unbracedMirFunction->body.loans.end()) {
      for (const lang::MirBlock &block : unbracedMirFunction->body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
              instruction.loan != loan->id || instruction.hirStatement == 0) {
            continue;
          }
          ++unbracedEndCount;
          unbracedThenEnd = unbracedThenEnd ||
                            (unbracedHirThen != nullptr &&
                             instruction.hirStatement == unbracedHirThen->id);
          unbracedElseEnd = unbracedElseEnd ||
                            (unbracedHirIf != nullptr &&
                             instruction.hirStatement == unbracedHirIf->id);
        }
      }
    }
  }
  expect(unbracedMirFunction != nullptr && unbracedEndCount == 2 &&
             unbracedThenEnd && unbracedElseEnd &&
             lang::verifyMirBody(unbracedMirFunction->body).valid(),
         "MIR should materialize both unbraced path endpoints exactly once");

  const lang::FrontendResult elseIfInvalidation = lang::Frontend().analyze(
      "else-if-invalidation.gti",
      "import std.string;\n"
      "int choose(bool left, bool middle) { mut std::string value = "
      "std::string(\"gti\"); mut auto iterator = value.begin(); mut char "
      "observed = 'x'; if (left) { value.push_back('!'); } else if "
      "(middle) { observed = *iterator; } else { observed = *iterator; } "
      "value.push_back('?'); return 0; }\n"
      "int main() { return choose(false, true); }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(elseIfInvalidation.canGenerateCode(),
         "an else-if merge endpoint should survive recursive HIR lowering");
  const lang::FunctionDecl *elseIfChoose =
      findTopLevelFunction(elseIfInvalidation.program, "choose");
  const auto *elseIfIterator =
      elseIfChoose == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                elseIfChoose->body()->statements().at(1).get());
  const auto *elseIfOuter =
      elseIfChoose == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                elseIfChoose->body()->statements().at(3).get());
  const auto *elseIfNested =
      elseIfOuter == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(elseIfOuter->elseBranch().get());
  const lang::BindingInfo *elseIfBinding =
      elseIfIterator == nullptr
          ? nullptr
          : elseIfInvalidation.semantics.findBinding(*elseIfIterator);
  const lang::SemanticLoanInfo *elseIfLoan =
      elseIfBinding == nullptr
          ? nullptr
          : elseIfInvalidation.semantics.findLoan(elseIfBinding->retainedLoan);
  expect(elseIfLoan != nullptr && elseIfLoan->endpoints.size() == 2 &&
             hasLoanEndpoint(elseIfLoan,
                             lang::SemanticLoanEndKind::ThenBranchEntry,
                             elseIfOuter) &&
             hasLoanEndpoint(elseIfLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             elseIfNested),
         "else-if semantics should end the unused outer path at entry and "
         "the used nested paths at their merge");
  const auto elseIfHirFunction =
      elseIfChoose == nullptr
          ? elseIfInvalidation.hir.functionInstances().end()
          : std::find_if(elseIfInvalidation.hir.functionInstances().begin(),
                         elseIfInvalidation.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == elseIfChoose;
                         });
  const lang::HirStatement *elseIfHirNested = nullptr;
  if (elseIfHirFunction != elseIfInvalidation.hir.functionInstances().end()) {
    const auto found = std::find_if(elseIfHirFunction->body.statements.begin(),
                                    elseIfHirFunction->body.statements.end(),
                                    [&](const lang::HirStatement &statement) {
                                      return statement.source == elseIfNested;
                                    });
    if (found != elseIfHirFunction->body.statements.end()) {
      elseIfHirNested = &*found;
    }
  }
  const lang::SemanticLoanId elseIfLoanId =
      elseIfBinding == nullptr ? 0 : elseIfBinding->retainedLoan;
  const lang::MirFunctionInstance *elseIfMirFunction =
      elseIfHirFunction == elseIfInvalidation.hir.functionInstances().end()
          ? nullptr
          : elseIfInvalidation.mir.findFunctionInstance(elseIfHirFunction->id);
  expect(elseIfHirNested != nullptr &&
             std::find(elseIfHirNested->endedLoans.begin(),
                       elseIfHirNested->endedLoans.end(),
                       elseIfLoanId) != elseIfHirNested->endedLoans.end() &&
             elseIfMirFunction != nullptr &&
             lang::verifyMirBody(elseIfMirFunction->body).valid(),
         "else-if HIR and MIR should preserve and verify the nested merge "
         "endpoint");

  const lang::FrontendResult nestedBranchInvalidation =
      lang::Frontend().analyze(
          "nested-branch-string-invalidation.gti",
          "import std.string;\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); if (true) { if (true) { "
          "char first = *iterator; } value.push_back('!'); } return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(nestedBranchInvalidation.canGenerateCode(),
         "a nested conditional loan should end at its reachable merge before "
         "a following branch-local invalidation");
  const lang::FunctionDecl *nestedBranchMain =
      findTopLevelFunction(nestedBranchInvalidation.program, "main");
  const auto *nestedBranchIterator =
      nestedBranchMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                nestedBranchMain->body()->statements().at(1).get());
  const auto *nestedBranchOuterIf =
      nestedBranchMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                nestedBranchMain->body()->statements().at(2).get());
  const auto *nestedBranchOuterThen =
      nestedBranchOuterIf == nullptr
          ? nullptr
          : dynamic_cast<const lang::BlockStmt *>(
                nestedBranchOuterIf->thenBranch().get());
  const auto *nestedBranchInnerIf =
      nestedBranchOuterThen == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                nestedBranchOuterThen->statements().front().get());
  const lang::BindingInfo *nestedBranchBinding =
      nestedBranchIterator == nullptr
          ? nullptr
          : nestedBranchInvalidation.semantics.findBinding(
                *nestedBranchIterator);
  const lang::SemanticLoanInfo *nestedBranchLoan =
      nestedBranchBinding == nullptr
          ? nullptr
          : nestedBranchInvalidation.semantics.findLoan(
                nestedBranchBinding->retainedLoan);
  expect(nestedBranchLoan != nullptr &&
             nestedBranchLoan->endpoints.size() == 2 &&
             hasLoanEndpoint(nestedBranchLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             nestedBranchInnerIf) &&
             hasLoanEndpoint(nestedBranchLoan,
                             lang::SemanticLoanEndKind::ElseBranchEntry,
                             nestedBranchOuterIf),
         "nested loan metadata should end the falling inner path at its merge "
         "and the unused outer path at entry");

  const auto nestedBranchHirFunction =
      nestedBranchMain == nullptr
          ? nestedBranchInvalidation.hir.functionInstances().end()
          : std::find_if(
                nestedBranchInvalidation.hir.functionInstances().begin(),
                nestedBranchInvalidation.hir.functionInstances().end(),
                [&](const lang::HirFunctionInstance &candidate) {
                  return candidate.source == nestedBranchMain;
                });
  const lang::HirStatement *nestedBranchHirOuterIf = nullptr;
  const lang::HirStatement *nestedBranchHirInnerIf = nullptr;
  if (nestedBranchHirFunction !=
      nestedBranchInvalidation.hir.functionInstances().end()) {
    for (const lang::HirStatement &statement :
         nestedBranchHirFunction->body.statements) {
      if (statement.source == nestedBranchOuterIf) {
        nestedBranchHirOuterIf = &statement;
      }
      if (statement.source == nestedBranchInnerIf) {
        nestedBranchHirInnerIf = &statement;
      }
    }
  }
  const lang::SemanticLoanId nestedBranchLoanId =
      nestedBranchBinding == nullptr ? 0 : nestedBranchBinding->retainedLoan;
  expect(nestedBranchHirOuterIf != nullptr &&
             nestedBranchHirInnerIf != nullptr &&
             std::find(nestedBranchHirInnerIf->endedLoans.begin(),
                       nestedBranchHirInnerIf->endedLoans.end(),
                       nestedBranchLoanId) !=
                 nestedBranchHirInnerIf->endedLoans.end() &&
             std::find(nestedBranchHirOuterIf->elseEntryEndedLoans.begin(),
                       nestedBranchHirOuterIf->elseEntryEndedLoans.end(),
                       nestedBranchLoanId) !=
                 nestedBranchHirOuterIf->elseEntryEndedLoans.end(),
         "HIR should preserve nested merge and outer edge loan endpoints");

  const lang::MirFunctionInstance *nestedBranchMirFunction =
      nestedBranchHirFunction ==
              nestedBranchInvalidation.hir.functionInstances().end()
          ? nullptr
          : nestedBranchInvalidation.mir.findFunctionInstance(
                nestedBranchHirFunction->id);
  std::size_t nestedBranchEndCount = 0;
  bool nestedBranchMergeEnd = false;
  bool nestedBranchOuterElseEnd = false;
  if (nestedBranchMirFunction != nullptr) {
    const auto loan =
        std::find_if(nestedBranchMirFunction->body.loans.begin(),
                     nestedBranchMirFunction->body.loans.end(),
                     [&](const lang::MirLoan &candidate) {
                       return candidate.semanticLoan == nestedBranchLoanId;
                     });
    if (loan != nestedBranchMirFunction->body.loans.end()) {
      for (const lang::MirBlock &block : nestedBranchMirFunction->body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
              instruction.loan != loan->id || instruction.hirStatement == 0) {
            continue;
          }
          ++nestedBranchEndCount;
          nestedBranchMergeEnd =
              nestedBranchMergeEnd ||
              (nestedBranchHirInnerIf != nullptr &&
               instruction.hirStatement == nestedBranchHirInnerIf->id);
          nestedBranchOuterElseEnd =
              nestedBranchOuterElseEnd ||
              (nestedBranchHirOuterIf != nullptr &&
               instruction.hirStatement == nestedBranchHirOuterIf->id);
        }
      }
    }
  }
  expect(nestedBranchMirFunction != nullptr && nestedBranchEndCount == 2 &&
             nestedBranchMergeEnd && nestedBranchOuterElseEnd &&
             lang::verifyMirBody(nestedBranchMirFunction->body).valid(),
         "MIR should end the nested loan exactly once on each reachable outer "
         "path and satisfy loan-flow verification");

  const lang::FrontendResult nestedArmInvalidations = lang::Frontend().analyze(
      "nested-arm-string-invalidations.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { if "
      "(value.empty()) { char first = *iterator; "
      "value.push_back('!'); } else { value.push_back('?'); } "
      "value.push_back('.'); } "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(nestedArmInvalidations.canGenerateCode(),
         "nested arms should end a retained loan independently before "
         "owner-invalidating calls and keep it ended after their merge");

  const lang::FrontendResult terminatingNestedArm = lang::Frontend().analyze(
      "terminating-nested-arm.gti",
      "import std.string;\n"
      "int consume(bool stop) { mut std::string value = "
      "std::string(\"gti\"); mut auto iterator = value.begin(); if "
      "(true) { if (stop) { char first = *iterator; return 1; } "
      "value.push_back('!'); } return 0; }\n"
      "int main() { return consume(false); }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(terminatingNestedArm.canGenerateCode(),
         "a terminating nested arm should clean up its loan while the "
         "fallthrough arm ends at the nested merge before mutation");

  const lang::FrontendResult terminatingReturnUse = lang::Frontend().analyze(
      "terminating-return-borrow-use.gti",
      "import std.string;\n"
      "char choose(bool stop) { mut std::string value = "
      "std::string(\"gti\"); mut auto iterator = value.begin(); if "
      "(stop) { return *iterator; } value.push_back('!'); return value[0]; "
      "}\n"
      "int main() { if (choose(true) == 'g' and choose(false) == 'g') { "
      "return 0; } return 1; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(terminatingReturnUse.canGenerateCode(),
         "a borrow used directly by a returning arm should rely on return "
         "cleanup while the fallthrough edge ends before mutation");

  const lang::FrontendResult terminatingConflictPath = lang::Frontend().analyze(
      "terminating-conflict-path.gti",
      "import std.string;\n"
      "int consume(bool stop) { mut std::string value = "
      "std::string(\"gti\"); mut auto iterator = value.begin(); if "
      "(true) { if (stop) { value.push_back('!'); return 1; } char "
      "first = *iterator; } return 0; }\n"
      "int main() { return consume(false); }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(terminatingConflictPath.canGenerateCode(),
         "a nested path that mutates and terminates should end at its branch "
         "entry without shortening a later sibling-path use");

  const lang::FrontendResult allTerminatingConflicts = lang::Frontend().analyze(
      "all-terminating-conflicts.gti",
      "import std.string;\n"
      "char choose(bool left) { mut std::string value = "
      "std::string(\"gti\"); mut auto iterator = value.begin(); if "
      "(left) { char first = *iterator; value.push_back('!'); return "
      "first; } else { value.push_back('?'); return value[0]; } }\n"
      "int main() { if (choose(true) == 'g' and choose(false) == 'g') { "
      "return 0; } return 1; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(allTerminatingConflicts.canGenerateCode(),
         "an all-terminating if should still end a retained loan separately "
         "on both arms before branch-local mutation");
  const lang::FunctionDecl *allTerminatingChoose =
      findTopLevelFunction(allTerminatingConflicts.program, "choose");
  const auto *allTerminatingIterator =
      allTerminatingChoose == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                allTerminatingChoose->body()->statements().at(1).get());
  const auto *allTerminatingIf =
      allTerminatingChoose == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                allTerminatingChoose->body()->statements().at(2).get());
  const auto *allTerminatingThen =
      allTerminatingIf == nullptr ? nullptr
                                  : dynamic_cast<const lang::BlockStmt *>(
                                        allTerminatingIf->thenBranch().get());
  const lang::Stmt *allTerminatingLastUse =
      allTerminatingThen == nullptr
          ? nullptr
          : allTerminatingThen->statements().front().get();
  const lang::BindingInfo *allTerminatingBinding =
      allTerminatingIterator == nullptr
          ? nullptr
          : allTerminatingConflicts.semantics.findBinding(
                *allTerminatingIterator);
  const lang::SemanticLoanInfo *allTerminatingLoan =
      allTerminatingBinding == nullptr
          ? nullptr
          : allTerminatingConflicts.semantics.findLoan(
                allTerminatingBinding->retainedLoan);
  expect(allTerminatingLoan != nullptr &&
             allTerminatingLoan->endpoints.size() == 2 &&
             hasLoanEndpoint(allTerminatingLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             allTerminatingLastUse) &&
             hasLoanEndpoint(allTerminatingLoan,
                             lang::SemanticLoanEndKind::ElseBranchEntry,
                             allTerminatingIf),
         "all-terminating semantic paths should retain only their two real "
         "arm endpoints and no synthetic merge endpoint");

  const lang::FrontendResult nestedBothReturn = lang::Frontend().analyze(
      "nested-both-return.gti",
      "import std.string;\n"
      "char choose(bool outer, bool inner) { mut std::string value = "
      "std::string(\"gti\"); mut auto iterator = value.begin(); if "
      "(outer) { if (inner) { return *iterator; } else { return "
      "*iterator; } } else { value.push_back('!'); } return value[0]; }\n"
      "int main() { if (choose(false, false) == 'g' and "
      "choose(true, false) == 'g' and choose(true, true) == 'g') { return "
      "0; } return 1; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(nestedBothReturn.canGenerateCode(),
         "a nested if whose two arms return should leave an unreachable MIR "
         "merge and not constrain its enclosing join");
  const lang::FunctionDecl *nestedBothReturnChoose =
      findTopLevelFunction(nestedBothReturn.program, "choose");
  const auto *nestedBothReturnOuterIf =
      nestedBothReturnChoose == nullptr
          ? nullptr
          : dynamic_cast<const lang::IfStmt *>(
                nestedBothReturnChoose->body()->statements().at(2).get());
  const auto *nestedBothReturnOuterThen =
      nestedBothReturnOuterIf == nullptr
          ? nullptr
          : dynamic_cast<const lang::BlockStmt *>(
                nestedBothReturnOuterIf->thenBranch().get());
  const lang::Stmt *nestedBothReturnInnerIf =
      nestedBothReturnOuterThen == nullptr
          ? nullptr
          : nestedBothReturnOuterThen->statements().front().get();
  const auto nestedBothReturnHirFunction =
      nestedBothReturnChoose == nullptr
          ? nestedBothReturn.hir.functionInstances().end()
          : std::find_if(nestedBothReturn.hir.functionInstances().begin(),
                         nestedBothReturn.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == nestedBothReturnChoose;
                         });
  const lang::HirStatement *nestedBothReturnHirInnerIf = nullptr;
  if (nestedBothReturnHirFunction !=
      nestedBothReturn.hir.functionInstances().end()) {
    const auto found =
        std::find_if(nestedBothReturnHirFunction->body.statements.begin(),
                     nestedBothReturnHirFunction->body.statements.end(),
                     [&](const lang::HirStatement &statement) {
                       return statement.source == nestedBothReturnInnerIf;
                     });
    if (found != nestedBothReturnHirFunction->body.statements.end()) {
      nestedBothReturnHirInnerIf = &*found;
    }
  }
  const lang::MirFunctionInstance *nestedBothReturnMirFunction =
      nestedBothReturnHirFunction ==
              nestedBothReturn.hir.functionInstances().end()
          ? nullptr
          : nestedBothReturn.mir.findFunctionInstance(
                nestedBothReturnHirFunction->id);
  const lang::MirBlock *nestedBothReturnMerge = nullptr;
  if (nestedBothReturnMirFunction != nullptr &&
      nestedBothReturnHirInnerIf != nullptr) {
    const auto found =
        std::find_if(nestedBothReturnMirFunction->body.blocks.begin(),
                     nestedBothReturnMirFunction->body.blocks.end(),
                     [&](const lang::MirBlock &block) {
                       return block.terminator.kind ==
                                  lang::MirTerminatorKind::Unreachable &&
                              block.terminator.hirStatement ==
                                  nestedBothReturnHirInnerIf->id;
                     });
    if (found != nestedBothReturnMirFunction->body.blocks.end()) {
      nestedBothReturnMerge = &*found;
    }
  }
  expect(nestedBothReturnMerge != nullptr &&
             !nestedBothReturnMerge->reachable &&
             nestedBothReturnMerge->instructions.empty() &&
             nestedBothReturnMirFunction != nullptr &&
             lang::verifyMirBody(nestedBothReturnMirFunction->body).valid(),
         "an all-terminating if merge should be explicitly unreachable and "
         "contain no phantom loan cleanup");

  const lang::FrontendResult literalTerminatingArm = lang::Frontend().analyze(
      "literal-terminating-arm.gti",
      "import std.string;\n"
      "char choose(bool outer) { mut std::string value = "
      "std::string(\"gti\"); mut auto iterator = value.begin(); if "
      "(outer) { if (true) { return *iterator; } } else { "
      "value.push_back('!'); } return value[0]; }\n"
      "int main() { if (choose(false) == 'g' and choose(true) == 'g') { "
      "return 0; } return 1; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(literalTerminatingArm.canGenerateCode(),
         "loan reachability should match MIR's two runtime successors even "
         "for a literal if condition");

  const lang::FrontendResult deeplyNestedInvalidations =
      lang::Frontend().analyze(
          "deeply-nested-string-invalidations.gti",
          "import std.string;\n"
          "int main() { mut std::string value = std::string(\"gti\"); "
          "mut auto iterator = value.begin(); if (true) { if (true) { if "
          "(true) { char first = *iterator; value.push_back('!'); } else "
          "{ value.push_back('?'); } } else { value.push_back('.'); } } "
          "else { value.push_back(','); } return 0; }\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(deeplyNestedInvalidations.canGenerateCode(),
         "recursive endpoint planning should cover multiple nested if edges");

  const lang::FrontendResult mutationBeforeNestedUse = lang::Frontend().analyze(
      "mutation-before-nested-use.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); if (true) { if (true) { "
      "value.push_back('!'); char first = *iterator; } } return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const auto nestedConflict =
      std::find_if(mutationBeforeNestedUse.diagnostics.begin(),
                   mutationBeforeNestedUse.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2017";
                   });
  expect(!mutationBeforeNestedUse.canGenerateCode() &&
             nestedConflict != mutationBeforeNestedUse.diagnostics.end() &&
             !nestedConflict->related.empty() &&
             std::any_of(
                 nestedConflict->hints.begin(), nestedConflict->hints.end(),
                 [](const std::string &hint) {
                   return hint.find("narrower block") != std::string::npos;
                 }),
         "mutation before a later nested use should remain rejected with "
         "borrow-origin context and the narrowing hint");

  const lang::FrontendResult loopBorrow = lang::Frontend().analyze(
      "loop-string-borrow.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); mut int count = 0; while (count "
      "< 2) { char current = *iterator; count++; } value.push_back('!'); "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!loopBorrow.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : loopBorrow.diagnostics) {
      std::cerr << "Unexpected loop-loan diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(loopBorrow.canGenerateCode(),
         "a retained loan should remain active across loop backedges and end "
         "on the shared loop exit");
  const lang::FunctionDecl *loopMain =
      findTopLevelFunction(loopBorrow.program, "main");
  const lang::VariableDecl *loopIterator =
      loopMain == nullptr ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                loopMain->body()->statements().at(1).get());
  const lang::WhileStmt *loopStatement =
      loopMain == nullptr ? nullptr
                          : dynamic_cast<const lang::WhileStmt *>(
                                loopMain->body()->statements().at(3).get());
  const lang::BindingInfo *loopBinding =
      loopIterator == nullptr ? nullptr
                              : loopBorrow.semantics.findBinding(*loopIterator);
  const lang::SemanticLoanInfo *loopLoan =
      loopBinding == nullptr
          ? nullptr
          : loopBorrow.semantics.findLoan(loopBinding->retainedLoan);
  expect(loopStatement != nullptr &&
             hasLoanEndpoint(loopLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             loopStatement),
         "semantic loan flow should make the loop statement the proven "
         "endpoint for a loop-carried loan");

  const auto loopHirFunction =
      loopMain == nullptr
          ? loopBorrow.hir.functionInstances().end()
          : std::find_if(loopBorrow.hir.functionInstances().begin(),
                         loopBorrow.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == loopMain;
                         });
  const lang::HirStatement *loopHirStatement = nullptr;
  if (loopHirFunction != loopBorrow.hir.functionInstances().end()) {
    const auto found = std::find_if(loopHirFunction->body.statements.begin(),
                                    loopHirFunction->body.statements.end(),
                                    [&](const lang::HirStatement &candidate) {
                                      return candidate.source == loopStatement;
                                    });
    if (found != loopHirFunction->body.statements.end()) {
      loopHirStatement = &*found;
    }
  }
  expect(loopHirStatement != nullptr && loopBinding != nullptr &&
             std::find(loopHirStatement->endedLoans.begin(),
                       loopHirStatement->endedLoans.end(),
                       loopBinding->retainedLoan) !=
                 loopHirStatement->endedLoans.end(),
         "HIR should carry the semantic loop-exit endpoint without "
         "recomputing loan flow");

  const lang::MirFunctionInstance *loopMirFunction =
      loopHirFunction == loopBorrow.hir.functionInstances().end()
          ? nullptr
          : loopBorrow.mir.findFunctionInstance(loopHirFunction->id);
  const lang::MirLoan *loopMirLoan = nullptr;
  if (loopMirFunction != nullptr && loopBinding != nullptr) {
    const auto found = std::find_if(
        loopMirFunction->body.loans.begin(), loopMirFunction->body.loans.end(),
        [&](const lang::MirLoan &candidate) {
          return candidate.semanticLoan == loopBinding->retainedLoan;
        });
    if (found != loopMirFunction->body.loans.end()) {
      loopMirLoan = &*found;
    }
  }
  std::size_t reachableLoopEnds = 0;
  if (loopMirFunction != nullptr && loopMirLoan != nullptr &&
      loopHirStatement != nullptr) {
    for (const lang::MirBlock &block : loopMirFunction->body.blocks) {
      if (!block.reachable) {
        continue;
      }
      reachableLoopEnds += static_cast<std::size_t>(std::count_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == loopMirLoan->id &&
                   instruction.hirStatement == loopHirStatement->id;
          }));
    }
  }
  expect(loopMirFunction != nullptr && reachableLoopEnds == 1 &&
             lang::verifyMirBody(loopMirFunction->body).valid(),
         "MIR should retain the loan at the header and backedge, then end it "
         "exactly once in the reachable exit block");

  const lang::FrontendResult zeroIterationLoop = lang::Frontend().analyze(
      "zero-iteration-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); while (false) { char current = "
      "*iterator; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(zeroIterationLoop.canGenerateCode(),
         "the condition-false edge should end a loop-carried loan even when "
         "the body executes zero times");

  const lang::FrontendResult unbracedLoop = lang::Frontend().analyze(
      "unbraced-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut char observed = '?'; while "
      "(false) observed = *iterator; value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!unbracedLoop.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : unbracedLoop.diagnostics) {
      std::cerr << "Unexpected unbraced-loop diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(unbracedLoop.canGenerateCode(),
         "an unbraced loop body should have its own loan-flow region and "
         "project a pre-existing carrier to the loop exit");

  const lang::FrontendResult breakLoop = lang::Frontend().analyze(
      "break-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); while (true) { char current = "
      "*iterator; break; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(breakLoop.canGenerateCode(),
         "a loop break should converge through the same loan-exit state as "
         "the condition-false edge");

  const lang::FrontendResult continueLoop = lang::Frontend().analyze(
      "continue-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; while (count < 2) "
      "{ count++; if (count == 1) { continue; } char current = *iterator; "
      "} value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(continueLoop.canGenerateCode(),
         "continue should preserve a loop-carried loan until the next "
         "condition and eventual exit");

  const lang::FrontendResult forLoop = lang::Frontend().analyze(
      "for-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); for (mut int count = 0; count < 2; "
      "count++) { char current = *iterator; if (count == 0) { continue; } "
      "} value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(forLoop.canGenerateCode(),
         "classic for-loop increment and continue edges should preserve a "
         "pre-existing retained loan until cleanup exits the loop");

  const lang::FrontendResult doWhileLoop = lang::Frontend().analyze(
      "do-while-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; do { char current "
      "= *iterator; count++; if (count < 2) { continue; } } while (count < "
      "2); value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(doWhileLoop.canGenerateCode(),
         "body-first loop condition and continue edges should preserve the "
         "loan until the shared exit");

  const lang::FrontendResult loopReturn = lang::Frontend().analyze(
      "loop-return-loan.gti",
      "import std.string;\n"
      "int run(bool leave) { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); while (leave) { char current = "
      "*iterator; return 0; } value.push_back('!'); return 0; } int main() "
      "{ return run(false) + run(true); }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(loopReturn.canGenerateCode(),
         "a return path should use normal scope cleanup while the reachable "
         "condition-false path uses the loop endpoint");

  const lang::FrontendResult nestedLoops = lang::Frontend().analyze(
      "nested-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int outer = 0; while (outer < 2) "
      "{ mut int inner = 0; while (inner < 1) { char current = *iterator; "
      "inner++; } outer++; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(nestedLoops.canGenerateCode(),
         "a loan created before nested loops should remain active until the "
         "outermost repeating use can no longer execute");

  const lang::FrontendResult perIterationInnerLoop = lang::Frontend().analyze(
      "per-iteration-inner-loop-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "int outer = 0; while (outer < 2) { mut auto iterator = "
      "value.begin(); mut int inner = 0; while (inner < 1) { char "
      "current = *iterator; inner++; } value.push_back('!'); outer++; "
      "} return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(perIterationInnerLoop.canGenerateCode(),
         "a loan created per outer iteration may end at an inner-loop exit "
         "before the owner changes and the next iteration recreates it");

  const lang::FrontendResult forInitializerLoan = lang::Frontend().analyze(
      "for-initializer-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); for "
      "(mut auto iterator = value.begin(); false; ++iterator) { char "
      "current = *iterator; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(forInitializerLoan.canGenerateCode(),
         "a loan first created in a for initializer should retain lexical "
         "loop-scope cleanup instead of gaining a duplicate loop endpoint");
  const lang::FunctionDecl *forInitializerMain =
      findTopLevelFunction(forInitializerLoan.program, "main");
  const lang::ForStmt *forInitializerStatement =
      forInitializerMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::ForStmt *>(
                forInitializerMain->body()->statements().at(1).get());
  const lang::VariableDecl *forInitializerIterator =
      forInitializerStatement == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                forInitializerStatement->initializer().get());
  const lang::BindingInfo *forInitializerBinding =
      forInitializerIterator == nullptr
          ? nullptr
          : forInitializerLoan.semantics.findBinding(*forInitializerIterator);
  const lang::SemanticLoanInfo *forInitializerSemanticLoan =
      forInitializerBinding == nullptr
          ? nullptr
          : forInitializerLoan.semantics.findLoan(
                forInitializerBinding->retainedLoan);
  expect(forInitializerSemanticLoan != nullptr &&
             forInitializerSemanticLoan->endpoints.empty(),
         "a for-initializer loan should be excluded from loop projection so "
         "its lexical cleanup remains the only MIR endpoint");

  const lang::FrontendResult transferredForLoan = lang::Frontend().analyze(
      "transferred-for-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto original = value.begin(); for (mut auto iterator = "
      "std::move(original); false; ++iterator) { char current = *iterator; "
      "} value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::FunctionDecl *transferredForMain =
      findTopLevelFunction(transferredForLoan.program, "main");
  const auto transferredForHir =
      transferredForMain == nullptr
          ? transferredForLoan.hir.functionInstances().end()
          : std::find_if(transferredForLoan.hir.functionInstances().begin(),
                         transferredForLoan.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == transferredForMain;
                         });
  const lang::MirFunctionInstance *transferredForMir =
      transferredForHir == transferredForLoan.hir.functionInstances().end()
          ? nullptr
          : transferredForLoan.mir.findFunctionInstance(transferredForHir->id);
  const lang::VariableDecl *transferredOriginal =
      transferredForMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                transferredForMain->body()->statements().at(1).get());
  const lang::ForStmt *transferredForStatement =
      transferredForMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::ForStmt *>(
                transferredForMain->body()->statements().at(2).get());
  const lang::VariableDecl *transferredIterator =
      transferredForStatement == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                transferredForStatement->initializer().get());
  const lang::BindingInfo *transferredOriginalBinding =
      transferredOriginal == nullptr
          ? nullptr
          : transferredForLoan.semantics.findBinding(*transferredOriginal);
  const lang::BindingInfo *transferredIteratorBinding =
      transferredIterator == nullptr
          ? nullptr
          : transferredForLoan.semantics.findBinding(*transferredIterator);
  if (!transferredForLoan.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : transferredForLoan.diagnostics) {
      std::cerr << "Unexpected transferred-for-loan diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(transferredForLoan.canGenerateCode() && transferredForMir != nullptr &&
             lang::verifyMirBody(transferredForMir->body).valid(),
         "moving a pre-existing retained carrier into a for initializer "
         "should keep one loan active until the loop exit without duplicate "
         "cleanup");
  expect(transferredOriginalBinding != nullptr &&
             transferredIteratorBinding != nullptr &&
             transferredOriginalBinding->symbol != 0 &&
             transferredIteratorBinding->symbol != 0 &&
             transferredOriginalBinding->retainedLoan != 0 &&
             transferredOriginalBinding->retainedLoan ==
                 transferredIteratorBinding->retainedLoan,
         "inferred carrier bindings should retain semantic symbols and the "
         "same transferred loan identity through HIR/MIR lowering");

  const lang::FrontendResult transferInsideLoop = lang::Frontend().analyze(
      "transfer-inside-loop.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); while (false) { mut auto moved = "
      "std::move(iterator); } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!transferInsideLoop.canGenerateCode() &&
             hasDiagnostic(transferInsideLoop.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live") &&
             !hasDiagnosticCode(transferInsideLoop.diagnostics, "GTI-B0001"),
         "a carrier transferred conditionally inside a loop should retain "
         "conservative owner protection rather than silently gaining a loop "
         "endpoint");

  const lang::FrontendResult loopConditionUse = lang::Frontend().analyze(
      "loop-condition-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; while (*iterator "
      "== 'g' and count < 1) { count++; } value.push_back('!'); return 0; "
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(loopConditionUse.canGenerateCode(),
         "a retained use in the loop condition should remain live on the "
         "backedge and end only on the false edge");

  const lang::FrontendResult forIncrementUse = lang::Frontend().analyze(
      "for-increment-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); for (mut int count = 0; count < 2; "
      "++iterator) { count++; } value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(forIncrementUse.canGenerateCode(),
         "a retained use in a for increment should remain live through the "
         "condition and end at for-loop exit");

  const lang::FrontendResult perIterationConditional = lang::Frontend().analyze(
      "per-iteration-conditional-loan.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "int count = 0; while (count < 2) { mut auto iterator = "
      "value.begin(); if (count == 0) { char current = *iterator; } "
      "else { char current = *iterator; } value.push_back('!'); "
      "count++; } return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(perIterationConditional.canGenerateCode(),
         "nested-if endpoints should remain available to iteration-local "
         "loans and reach the backedge inactive");

  const lang::FrontendResult backedgeInvalidation = lang::Frontend().analyze(
      "loop-backedge-invalidation.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; while (count < 2) "
      "{ char current = *iterator; value.push_back('!'); count++; } return "
      "0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const auto backedgeConflict =
      std::find_if(backedgeInvalidation.diagnostics.begin(),
                   backedgeInvalidation.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2017";
                   });
  expect(!backedgeInvalidation.canGenerateCode() &&
             backedgeConflict != backedgeInvalidation.diagnostics.end() &&
             !backedgeConflict->related.empty() &&
             std::any_of(
                 backedgeConflict->hints.begin(), backedgeConflict->hints.end(),
                 [](const std::string &hint) {
                   return hint.find("narrower block") != std::string::npos;
                 }) &&
             !hasDiagnosticCode(backedgeInvalidation.diagnostics, "GTI-B0001"),
         "owner invalidation before a possible backedge reuse should remain "
         "a semantic error with origin context and no MIR fallback failure");

  const lang::FrontendResult continueInvalidation = lang::Frontend().analyze(
      "continue-loop-invalidation.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; while (*iterator "
      "== 'g' and count < 2) { value.push_back('!'); count++; continue; } "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!continueInvalidation.canGenerateCode() &&
             hasDiagnostic(continueInvalidation.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live") &&
             !hasDiagnosticCode(continueInvalidation.diagnostics, "GTI-B0001"),
         "an owner mutation before continue must remain rejected because the "
         "next condition reuses the retained carrier");

  const lang::FrontendResult breakPathInvalidation = lang::Frontend().analyze(
      "break-path-invalidation.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); while (true) { char current = "
      "*iterator; value.push_back('!'); break; } return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(breakPathInvalidation.canGenerateCode(),
         "a final use before owner invalidation on an immediately-breaking "
         "path should end the retained loan on that path");

  const lang::FrontendResult switchBreakInvalidation = lang::Frontend().analyze(
      "switch-break-loop-invalidation.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; while (count < "
      "1) { switch (count) { case 0: { char current = *iterator; "
      "break; } default: { break; } } value.push_back('!'); count++; } "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !switchBreakInvalidation.canGenerateCode() &&
          hasDiagnostic(switchBreakInvalidation.diagnostics,
                        "while a reference borrowed from it may still be "
                        "live") &&
          !hasDiagnosticCode(switchBreakInvalidation.diagnostics, "GTI-B0001"),
      "a switch break nested in a loop must not be mistaken for a loop "
      "exit before the next backedge");

  const lang::FrontendResult laterUseAfterLoop = lang::Frontend().analyze(
      "later-use-after-loop.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); while (false) { char current = "
      "*iterator; } value.push_back('!'); char later = *iterator; return "
      "0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!laterUseAfterLoop.canGenerateCode() &&
             hasDiagnostic(laterUseAfterLoop.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live"),
         "a later carrier use must suppress an otherwise valid loop-exit "
         "endpoint");

  const lang::FrontendResult conditionalInsideLoop = lang::Frontend().analyze(
      "conditional-inside-loop.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); "
      "mut auto iterator = value.begin(); while (false) if (true) { "
      "char first = *iterator; value.push_back('!'); } else { "
      "value.push_back('?'); } return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!conditionalInsideLoop.canGenerateCode() &&
             hasDiagnostic(conditionalInsideLoop.diagnostics,
                           "while a reference borrowed from it may still be "
                           "live") &&
             !hasDiagnosticCode(conditionalInsideLoop.diagnostics, "GTI-B0001"),
         "an unbraced conditional inside a loop must retain conservative "
         "semantic loan extent instead of relying on MIR backedge failure");

  const lang::FrontendResult scopedBorrow = lang::Frontend().analyze(
      "scoped-string-borrow.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); { "
      "mut auto iterator = value.begin(); char first = *iterator; } "
      "value.push_back('!'); return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(scopedBorrow.canGenerateCode(),
         "ending a nested lexical scope should deactivate its semantic owner "
         "loan before following statements");

  const lang::FrontendResult applicationStorageBorrow =
      lang::Frontend().analyze("application-storage-borrow.gti", R"(
class StorageBorrow<T> {
  gti_internal::text_view& data;
public:
  StorageBorrow(gti_internal::text_view& source) : data(source) {}
};
int main() { return 0; }
)");
  expect(
      !applicationStorageBorrow.canGenerateCode() &&
          countDiagnosticCode(applicationStorageBorrow.diagnostics,
                              "GTI-S2058") == 2 &&
          !hasDiagnostic(applicationStorageBorrow.diagnostics, "Unknown type"),
      "ordinary source must receive focused private-access diagnostics "
      "without unknown-type cascades");

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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::unordered_set<std::string> iteratorTypes = concreteBackendSymbols(
      artifact.contents, "__gti_type_", "BorrowingIterator");
  const bool exactIteratorSpecialMembers =
      !iteratorTypes.empty() &&
      std::all_of(iteratorTypes.begin(), iteratorTypes.end(),
                  [&](const std::string &name) {
                    return artifact.contents.find(
                               "const std::int32_t & value") !=
                               std::string::npos &&
                           artifact.contents.find(name + "(const " + name +
                                                  " &other) = default") !=
                               std::string::npos &&
                           artifact.contents.find(name + "(" + name +
                                                  " &&) = default") !=
                               std::string::npos &&
                           artifact.contents.find("operator=(const " + name +
                                                  " &other) = delete") !=
                               std::string::npos;
                  });
  expect(exactIteratorSpecialMembers,
         "the C++ backend should preserve read-only storage and frontend "
         "special-member decisions on each concrete iterator application");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-stored-references.gti", R"(
class BorrowingIterator<T> {
  T& value;
public:
  BorrowingIterator(T& source) : value(source) {}
};

class SingleRange<T> {
  mut T value;
public:
  SingleRange(T initial) : value(initial) {}
  BorrowingIterator<T> begin() {
    return BorrowingIterator<T>(this.value);
  }
  void replace(T next) mut { this.value = next; }
};

class TwoReferences {
  mut int& left;
  mut int& right;
public:
  TwoReferences(mut int& source) : left(source), right(source) {}
};

class MissingBinding {
  int& value;
public:
  MissingBinding(int& source) {}
};

class NestedBorrow {
  BorrowingIterator<int> iterator;
public:
  NestedBorrow(BorrowingIterator<int> source)
      : iterator(std::move(source)) {}
};

class CleanupBorrow {
  int& value;
public:
  CleanupBorrow(int& source) : value(source) {}
  ~CleanupBorrow() {}
};

class GenericNest<T> {
  T nested;
public:
  GenericNest(T value) : nested(std::move(value)) {}
};

BorrowingIterator<int> escape(int& value) {
  return BorrowingIterator<int>(value);
}

SingleRange<int> global_range = SingleRange<int>(1);
BorrowingIterator<int> global_iterator = global_range.begin();

int main() {
  mut SingleRange<int> range = SingleRange<int>(1);
  mut BorrowingIterator<int> iterator = range.begin();
  BorrowingIterator<int> other = range.begin();
  iterator = std::move(other);
  BorrowingIterator<int> nested_source = range.begin();
  GenericNest<BorrowingIterator<int>> nested =
      GenericNest<BorrowingIterator<int>>(std::move(nested_source));
  BorrowingIterator<int> dangling = SingleRange<int>(2).begin();
  range.replace(2);
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot duplicate an exclusive dependency") &&
          hasDiagnostic(invalid.diagnostics,
                        "Every constructor of a stored-reference class") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot have global or static storage") &&
          hasDiagnostic(invalid.diagnostics, "Cannot assign a value of type") &&
          hasDiagnostic(invalid.diagnostics,
                        "Stored-reference value is derived from temporary "
                        "storage"),
      "stored-reference diagnostics should reject unsupported reference "
      "graphs, assignment, and temporary owners");
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
  expect(valid.canGenerateCode() && !gti_test::emitCppText(valid).empty(),
         "the C++ backend should emit a verified mutable borrow carrier");

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
  expect(!unsupported.canGenerateCode() &&
             hasDiagnostic(unsupported.diagnostics,
                           "Borrowed return dependency is ambiguous") &&
             hasDiagnostic(unsupported.diagnostics,
                           "mutable reference or direct mutable "
                           "borrowed-value parameter") &&
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
    const auto loanFor = [&](const std::string &name) {
      const auto *local = findDirectLocal(main, name);
      const auto *binding =
          local == nullptr ? nullptr : valid.semantics.findBinding(*local);
      return binding == nullptr
                 ? nullptr
                 : valid.semantics.findLoan(binding->retainedLoan);
    };
    const auto *initial = loanFor("initial");
    const auto *intermediate = loanFor("intermediate");
    const auto *active = loanFor("active");
    for (const auto *loan : {intermediate, active})
      expect(initial != nullptr && loan != nullptr &&
                 initial->access == lang::AccessMode::ReadOnly &&
                 initial->additionalDependencies.size() == 2 &&
                 std::all_of(initial->additionalDependencies.begin(),
                             initial->additionalDependencies.end(),
                             [](const auto &dependency) {
                               return dependency.access ==
                                      lang::AccessMode::Mutable;
                             }) &&
                 loan->place == initial->place &&
                 loan->access == initial->access &&
                 loan->designatedFields == initial->designatedFields &&
                 loan->additionalDependencies ==
                     initial->additionalDependencies,
             "each forwarding result preserves the exact shared and two "
             "exclusive dependency places, field identities, and permissions");
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
      expect(!gti_test::emitCppText(valid, standard).empty(),
             "mixed carrier transfer remains admitted by the verified backend");
    for (const auto &function : valid.mir.functionInstances()) {
      if (function.entryKind == lang::ProgramEntryKind::None)
        continue;
      for (const auto &[consumed, result] : {std::pair{initial, intermediate},
                                             std::pair{intermediate, active}}) {
        const auto findLoan =
            [&](const auto *semantic) -> const lang::MirLoan * {
          if (semantic != nullptr)
            for (const auto &loan : function.body.loans)
              if (loan.semanticLoan == semantic->id)
                return &loan;
          return nullptr;
        };
        const auto *inputLoan = findLoan(consumed);
        const auto *resultLoan = findLoan(result);
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
    if (initial != nullptr) {
      auto repeatedEndpoint = valid.hir;
      bool changed = false;
      auto &functions = const_cast<std::vector<lang::HirFunctionInstance> &>(
          repeatedEndpoint.functionInstances());
      for (auto &function : functions) {
        if (function.entryKind == lang::ProgramEntryKind::None)
          continue;
        bool acknowledged = false;
        for (auto &statement : function.body.statements) {
          if (acknowledged) {
            statement.endedLoans.push_back(initial->id);
            changed = true;
            break;
          }
          acknowledged = std::find(statement.endedLoans.begin(),
                                   statement.endedLoans.end(),
                                   initial->id) != statement.endedLoans.end();
        }
      }
      expect(changed && !lang::MirLowerer()
                             .lower(repeatedEndpoint, valid.failureMetadata)
                             .valid(),
             "a call-transfer endpoint cannot acknowledge the same retired "
             "loan again at a later statement");
    }
    if (active != nullptr) {
      auto forged = valid.mir;
      bool changed = false;
      for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
        auto *body = lang::findMirBody(forged, address);
        if (body == nullptr)
          continue;
        for (auto &loan : body->loans)
          if (loan.semanticLoan == active->id &&
              !loan.additionalDependencies.empty()) {
            loan.additionalDependencies.pop_back();
            changed = true;
          }
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
  if (branched.canGenerateCode()) {
    auto oneSidedEndpoint = branched.hir;
    bool changed = false;
    auto &functions = const_cast<std::vector<lang::HirFunctionInstance> &>(
        oneSidedEndpoint.functionInstances());
    for (auto &function : functions) {
      if (function.entryKind == lang::ProgramEntryKind::None)
        continue;
      for (auto &statement : function.body.statements) {
        if (statement.kind != lang::HirStatementKind::If ||
            statement.endedLoans.empty())
          continue;
        // Only the then path now performs a call transfer. The else path
        // explicitly ends the same loan at entry, so HIR's repeated endpoint
        // at the join must not borrow the then path's acknowledgement.
        statement.elseBranch.reset();
        statement.elseEntryEndedLoans = statement.endedLoans;
        changed = true;
        break;
      }
    }
    expect(changed && !lang::MirLowerer()
                           .lower(oneSidedEndpoint, branched.failureMetadata)
                           .valid(),
           "a one-sided call-transfer acknowledgement cannot hide a forged "
           "repeated endpoint on the other branch");
    const auto *main = findTopLevelFunction(branched.program, "main");
    const auto *initial = findDirectLocal(main, "initial");
    const auto *binding =
        initial == nullptr ? nullptr : branched.semantics.findBinding(*initial);
    auto missingEndpoint = branched.mir;
    bool removed = false;
    unsigned successfulEndpoints = 0;
    for (const auto address :
         lang::enumerateMirBodyAddresses(missingEndpoint)) {
      auto *body = lang::findMirBody(missingEndpoint, address);
      if (body == nullptr || binding == nullptr)
        continue;
      for (auto &block : body->blocks) {
        if (block.activeFailure != 0)
          continue;
        for (auto instruction = block.instructions.begin();
             instruction != block.instructions.end();) {
          const auto *loan = body->findLoan(instruction->loan.value_or(0));
          if (instruction->kind == lang::MirInstructionKind::EndBorrow &&
              loan != nullptr && loan->semanticLoan == binding->retainedLoan) {
            ++successfulEndpoints;
            if (!removed) {
              instruction = block.instructions.erase(instruction);
              removed = true;
              continue;
            }
          }
          ++instruction;
        }
      }
    }
    expect(removed && successfulEndpoints == 2 &&
               !lang::verifyMirProgram(missingEndpoint).valid(),
           "independent MIR verification rejects losing the executable "
           "carrier endpoint on exactly one normally reaching branch");
  }
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

void testStoredCarrierReturnProvenance() {
  const std::string projectedSource = R"(
struct Counter { mut int value = 4;  public: Counter() = default; };
class ProjectedCarrier {
  Counter& owner;
public:
  ProjectedCarrier(Counter& source) : owner(source) {}
  int& relay() { return this.read(); }
  int& read() { return this.owner.value; }
};
int main() {
  mut Counter owner{};
  ProjectedCarrier original = ProjectedCarrier(owner);
  ProjectedCarrier& alias = original;
  int& value = alias.relay();
  ProjectedCarrier moved = std::move(original);
  return value + moved.read() - 8;
}
)";
  const auto projected = lang::Frontend().analyze(
      "stored-carrier-projected-relay.gti", projectedSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      projected.canGenerateCode() &&
          lang::verifyMirProgram(projected.mir).valid(),
      "an accessor relay through a stored reference must retain the external "
      "field, not freeze the independently movable carrier shell");
  if (!projected.canGenerateCode())
    for (const auto &diagnostic : projected.diagnostics)
      std::cerr << "Projected relay diagnostic: " << diagnostic.code << ' '
                << diagnostic.message << '\n';
  auto invalidProjected = projectedSource;
  invalidProjected.insert(invalidProjected.find("  return value"),
                          "  owner.value = 9;\n");
  const auto invalidProjection = lang::Frontend().analyze(
      "stored-carrier-projected-relay-owner-write.gti", invalidProjected,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidProjection.canGenerateCode() &&
             hasDiagnosticCode(invalidProjection.diagnostics, "GTI-S2017"),
         "moving a relay's carrier must not allow mutation of its still "
         "borrowed external field");

  const std::string ownedAccessorSource = R"(
class ValueBox<T> {
  mut T payload;
public:
  ValueBox(T value) : payload(value) {}
  T& get() { return this.payload; }
  void set(T value) mut { this.payload = value; }
};
class BorrowedBox<T> {
  ValueBox<T>& owner;
public:
  BorrowedBox(ValueBox<T>& owner) : owner(owner) {}
  T& get() { return this.owner.get(); }
};
int main() {
  mut ValueBox<int> owner{4};
  BorrowedBox<int> carrier{owner};
  int& value = carrier.get();
  auto moved = std::move(carrier);
  return value + moved.get() - 8;
}
)";
  const auto ownedAccessor = lang::Frontend().analyze(
      "stored-carrier-generic-owned-accessor.gti", ownedAccessorSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(ownedAccessor.canGenerateCode() &&
             lang::verifyMirProgram(ownedAccessor.mir).valid(),
         "generic accessor composition must preserve the called method's "
         "receiver-owner contract without exposing its private field path");
  if (!ownedAccessor.canGenerateCode())
    for (const auto &diagnostic : ownedAccessor.diagnostics)
      std::cerr << "Owned accessor relay diagnostic: " << diagnostic.code << ' '
                << diagnostic.message << '\n';
  auto invalidOwnedAccessorSource = ownedAccessorSource;
  invalidOwnedAccessorSource.insert(
      invalidOwnedAccessorSource.find("  return value"), "  owner.set(9);\n");
  const auto invalidOwnedAccessor = lang::Frontend().analyze(
      "stored-carrier-generic-owned-accessor-write.gti",
      invalidOwnedAccessorSource, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(!invalidOwnedAccessor.canGenerateCode() &&
             hasDiagnosticCode(invalidOwnedAccessor.diagnostics, "GTI-S2017"),
         "a composed owned-storage accessor still freezes its external owner "
         "after the passive carrier moves");

  const std::string mutableStorageSource = R"(
class MixedStorage<T> {
  T& external;
  mut T local;
public:
  MixedStorage(T& external, T local) : external(external), local(local) {}
  T& get() { return this.local; }
  mut T& get_mut() mut { return this.local; }
  T implicit_read() { T& value = get(); return value; }
  T& external_value() { return this.external; }
  ~MixedStorage() {}
};
int main() {
  int external = 4;
  mut MixedStorage<int> owner{external, 7};
  mut int& element = owner.get_mut();
  element = 9;
  return owner.implicit_read() + owner.external_value() - 13;
}
)";
  const auto mutableStorage = lang::Frontend().analyze(
      "stored-carrier-mutable-owned-storage.gti", mutableStorageSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(mutableStorage.canGenerateCode() &&
             lang::verifyMirProgram(mutableStorage.mir).valid(),
         "mutable owned storage is independent of the carrier's read-only "
         "external dependency");
  if (!mutableStorage.canGenerateCode())
    for (const auto &diagnostic : mutableStorage.diagnostics)
      std::cerr << "Mutable owned-storage diagnostic: " << diagnostic.code
                << ' ' << diagnostic.message << '\n';
  auto invalidMutableStorageSource = mutableStorageSource;
  invalidMutableStorageSource.replace(
      invalidMutableStorageSource.find("owner.get_mut()"),
      std::string("owner.get_mut()").size(), "owner.external_value()");
  const auto invalidMutableStorage = lang::Frontend().analyze(
      "stored-carrier-read-only-external-storage.gti",
      invalidMutableStorageSource, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(!invalidMutableStorage.canGenerateCode() &&
             hasDiagnosticCode(invalidMutableStorage.diagnostics, "GTI-S2017"),
         "mutable carrier storage cannot upgrade its read-only external "
         "reference to mutable access");

  const std::string factorySource = R"(
class Cell {
  int& resource;
  mut int value = 7;
public:
  Cell(int& resource) : resource(resource) {}
  int read() { return this.value; }
  void reset() mut { this.value = 9; }
  ~Cell() {}
};
class Reference<T> {
  T& owner;
public:
  Reference(T& owner) : owner(owner) {}
  T& get() { return this.owner; }
};
Reference<T> bind<T>(T& owner) { return Reference<T>(owner); }
int main() {
  int resource = 4;
  mut Cell owner{resource};
  auto reference = bind(owner);
  return reference.get().read() - 7;
}
)";
  const auto factory = lang::Frontend().analyze(
      "stored-carrier-reference-factory.gti", factorySource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(factory.canGenerateCode() &&
             lang::verifyMirProgram(factory.mir).valid(),
         "a source factory retains both its argument's physical shell and "
         "the borrowed owners inside that shell");
  if (!factory.canGenerateCode())
    for (const auto &diagnostic : factory.diagnostics)
      std::cerr << "Reference factory diagnostic: " << diagnostic.code << ' '
                << diagnostic.message << '\n';
  auto invalidFactorySource = factorySource;
  invalidFactorySource.insert(invalidFactorySource.find("  return reference"),
                              "  owner.reset();\n");
  const auto invalidFactory = lang::Frontend().analyze(
      "stored-carrier-reference-factory-reset.gti", invalidFactorySource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidFactory.canGenerateCode() &&
             hasDiagnosticCode(invalidFactory.diagnostics, "GTI-S2017"),
         "a reference factory must not allow mutation of its carrier shell "
         "while the returned wrapper still uses it");

  const std::string adapterSource = R"(
class Cell {
  int& resource;
  mut int value = 7;
public:
  Cell(int& resource) : resource(resource) {}
  int read() { return this.value; }
  int& get() { return this.value; }
  void reset() mut { this.value = 9; }
  ~Cell() {}
};
class Cursor {
  Cell& cell;
public:
  Cursor(Cell& cell) : cell(cell) {}
  int read() { return this.cell.read(); }
  int& operator*() { return this.cell.get(); }
};
Cursor relay(Cursor cursor) { return cursor; }
class Collection {
  mut Cell cell;
public:
  Collection(int& resource) : cell(Cell(resource)) {}
  Cursor cursor() { return Cursor(this.cell); }
  void reset() mut { this.cell.reset(); }
};
int main() {
  int resource = 4;
  mut Collection owner{resource};
  auto cursor = owner.cursor();
  auto copied = relay(cursor);
  int& element = *copied;
  int result = element;
  owner.reset();
  return result - 7;
}
)";
  const auto adapter = lang::Frontend().analyze(
      "stored-carrier-value-receiver-adapter.gti", adapterSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(adapter.canGenerateCode() &&
             lang::verifyMirProgram(adapter.mir).valid(),
         "a value-returning receiver adapter and its copies retain physical "
         "storage until their final use, not the receiver's external resource");
  if (!adapter.canGenerateCode())
    for (const auto &diagnostic : adapter.diagnostics)
      std::cerr << "Receiver adapter diagnostic: " << diagnostic.code << ' '
                << diagnostic.message << '\n';
  for (const auto &mutation : {std::string("owner.reset();"),
                               std::string("auto moved = std::move(owner);")}) {
    auto invalidAdapterSource = adapterSource;
    invalidAdapterSource.insert(invalidAdapterSource.find("  int result"),
                                "  " + mutation + "\n");
    const auto invalidAdapter = lang::Frontend().analyze(
        "stored-carrier-value-adapter-invalidation.gti", invalidAdapterSource,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!invalidAdapter.canGenerateCode() &&
               hasDiagnosticCode(invalidAdapter.diagnostics, "GTI-S2017"),
           "an independently copied adapter rejects physical owner "
           "invalidation before its last use: " +
               mutation);
  }

  const std::string validSource = R"(
class ReadCarrier {
public:
  int& referent;
  mut int shell = 0;
  ReadCarrier(int& source) : referent(source) {}
  int& identity() { return this.referent; }
  int& relay_call() { return this.identity(); }
};

int& relay_reference(int& source) { return source; }
int& relay_external(ReadCarrier carrier) {
  int& ordinary = relay_reference(carrier.referent);
  return carrier.referent;
}
ReadCarrier relay_carrier(ReadCarrier carrier) {
  mut int unrelated_owner = 11;
  ReadCarrier unrelated = ReadCarrier(unrelated_owner);
  ReadCarrier unused = unrelated;
  return carrier;
}

int main() {
  mut int owner = 7;
  ReadCarrier first = ReadCarrier(owner);
  ReadCarrier copied = first;
  ReadCarrier moved = std::move(copied);
  int& exact = relay_external(moved);
  int& relayed_exact = moved.relay_call();
  ReadCarrier relayed = relay_carrier(std::move(moved));
  return exact + relayed_exact + relayed.referent - 21;
}
)";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "stored-carrier-exact-relay.gti", validSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected stored-carrier relay diagnostic: "
                << diagnostic.code << ' ' << diagnostic.primary.start << ' '
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "a read-only by-value carrier should copy, move, and relay only its "
         "exact external referent");

  const lang::HirFunctionInstance *relayHir = nullptr;
  const lang::HirFunctionInstance *relayCallHir = nullptr;
  const lang::HirFunctionInstance *relayCarrierHir = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "relay_external") {
      relayHir = &instance;
    } else if (instance.source != nullptr &&
               instance.source->name().lexeme == "relay_call") {
      relayCallHir = &instance;
    } else if (instance.source != nullptr &&
               instance.source->name().lexeme == "relay_carrier") {
      relayCarrierHir = &instance;
    }
  }
  const lang::MirFunctionInstance *relayMir =
      relayHir == nullptr ? nullptr
                          : valid.mir.findFunctionInstance(relayHir->id);
  const lang::MirLoan *entryLoan = nullptr;
  const lang::MirLoan *returnLoan = nullptr;
  const lang::MirInstruction *returnTransfer = nullptr;
  if (relayMir != nullptr) {
    for (const lang::MirLoan &loan : relayMir->body.loans) {
      if (loan.kind == lang::MirLoanKind::Stored && loan.entry) {
        entryLoan = &loan;
      } else if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
        returnLoan = &loan;
      }
    }
    if (returnLoan != nullptr) {
      for (const lang::MirBlock &block : relayMir->body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Borrow &&
              instruction.loan == returnLoan->id) {
            returnTransfer = &instruction;
          }
        }
      }
    }
  }
  expect(
      relayMir != nullptr && entryLoan != nullptr && returnLoan != nullptr &&
          returnTransfer != nullptr && returnTransfer->operands.size() == 1 &&
          !entryLoan->designatedFields.empty() &&
          relayMir->body.findPlace(entryLoan->source) != nullptr &&
          relayMir->body.findPlace(entryLoan->source)->projections.empty() &&
          returnTransfer->operands.front().kind == lang::MirOperandKind::Loan &&
          returnTransfer->operands.front().loan == entryLoan->id &&
          returnTransfer->operands.front().place != 0,
      "a reference relay should publish an exact runtime place from its "
      "stored entry dependency");

  const lang::MirFunctionInstance *relayCallMir =
      relayCallHir == nullptr
          ? nullptr
          : valid.mir.findFunctionInstance(relayCallHir->id);
  const lang::MirLoan *callResultLoan = nullptr;
  const lang::MirLoan *callReturnLoan = nullptr;
  const lang::MirInstruction *callReturnTransfer = nullptr;
  const lang::MirInstruction *callResultRetire = nullptr;
  if (relayCallMir != nullptr) {
    for (const lang::MirLoan &loan : relayCallMir->body.loans) {
      if (loan.kind == lang::MirLoanKind::CallResult) {
        callResultLoan = &loan;
      } else if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
        callReturnLoan = &loan;
      }
    }
    if (callReturnLoan != nullptr && callResultLoan != nullptr) {
      for (const lang::MirBlock &block : relayCallMir->body.blocks) {
        for (std::size_t index = 0; index < block.instructions.size();
             ++index) {
          const lang::MirInstruction &instruction = block.instructions[index];
          if (instruction.kind == lang::MirInstructionKind::Borrow &&
              instruction.loan == callReturnLoan->id) {
            callReturnTransfer = &instruction;
            if (index + 1 < block.instructions.size()) {
              callResultRetire = &block.instructions[index + 1];
            }
          }
        }
      }
    }
  }
  expect(
      relayCallMir != nullptr && callResultLoan != nullptr &&
          callReturnLoan != nullptr && callReturnTransfer != nullptr &&
          callReturnTransfer->operands.size() == 1 &&
          callReturnTransfer->operands.front().kind ==
              lang::MirOperandKind::Loan &&
          callReturnTransfer->operands.front().loan == callResultLoan->id &&
          callReturnTransfer->operands.front().place == 0 &&
          callReturnTransfer->operands.front().value == 0 &&
          callReturnLoan->source == callResultLoan->source &&
          callReturnLoan->access == callResultLoan->access &&
          callReturnLoan->producedBy == callResultLoan->producedBy &&
          callResultRetire != nullptr &&
          callResultRetire->kind == lang::MirInstructionKind::EndBorrow &&
          callResultRetire->loan == callResultLoan->id,
      "a reference-returning call relay should preserve the summarized owner "
      "origin while transferring the exact call-result pointer");

  lang::MirProgram forgedCallRelay = valid.mir;
  auto &callRelayFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          forgedCallRelay.functionInstances());
  bool forgedCallResultTransfer = false;
  if (relayCallHir != nullptr) {
    const auto candidate =
        std::find_if(callRelayFunctions.begin(), callRelayFunctions.end(),
                     [&](const lang::MirFunctionInstance &instance) {
                       return instance.id == relayCallHir->id;
                     });
    if (candidate != callRelayFunctions.end()) {
      const auto retained = std::find_if(
          candidate->body.loans.begin(), candidate->body.loans.end(),
          [](const lang::MirLoan &loan) {
            return loan.kind == lang::MirLoanKind::CallResult;
          });
      if (retained != candidate->body.loans.end()) {
        retained->escapes = true;
        forgedCallResultTransfer = true;
      }
    }
  }
  const lang::MirVerificationResult forgedCallResult =
      lang::verifyMirProgram(forgedCallRelay);
  expect(forgedCallResultTransfer && !forgedCallResult.valid() &&
             hasMirVerificationError(forgedCallResult,
                                     "borrow instruction does not match the "
                                     "loan's parent"),
         "MIR verification should reject a return transfer whose retained "
         "call-result loan has been forged to escape independently");

  const auto mutateRelay = [&](const auto &mutation) {
    lang::MirProgram forged = valid.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    lang::MirFunctionInstance *candidate = nullptr;
    if (relayHir != nullptr) {
      const auto found =
          std::find_if(functions.begin(), functions.end(),
                       [&](const lang::MirFunctionInstance &instance) {
                         return instance.id == relayHir->id;
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

  const auto forgedProvenance = mutateRelay([](lang::MirFunctionInstance &fn) {
    const auto loan = std::find_if(
        fn.body.loans.begin(), fn.body.loans.end(),
        [](const lang::MirLoan &item) {
          return item.kind == lang::MirLoanKind::Return && item.escapes;
        });
    if (loan != fn.body.loans.end()) {
      loan->source = 0;
    }
  });
  expect(forgedProvenance.first && !forgedProvenance.second.valid() &&
             hasMirVerificationError(forgedProvenance.second,
                                     "has no source identity"),
         "MIR verification should independently reject a return loan with "
         "forged provenance");

  const auto forgedDependency = mutateRelay([](lang::MirFunctionInstance &fn) {
    lang::MirLoanId returned = 0;
    for (const lang::MirLoan &loan : fn.body.loans) {
      if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
        returned = loan.id;
      }
    }
    for (lang::MirBlock &block : fn.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Borrow &&
            instruction.loan == returned && !instruction.operands.empty()) {
          instruction.operands.front().loan = returned;
          return;
        }
      }
    }
  });
  expect(forgedDependency.first && !forgedDependency.second.valid() &&
             hasMirVerificationError(forgedDependency.second,
                                     "exact entry dependency"),
         "MIR verification should independently reject a return transfer "
         "that does not consume the stored entry dependency");

  const auto forgedType = mutateRelay([](lang::MirFunctionInstance &fn) {
    lang::MirLoanId returned = 0;
    for (const lang::MirLoan &loan : fn.body.loans) {
      if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
        returned = loan.id;
      }
    }
    for (lang::MirBlock &block : fn.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Borrow &&
            instruction.loan == returned && !instruction.operands.empty()) {
          instruction.operands.front().type = lang::SemanticType::Bool;
          instruction.info.type = lang::SemanticType::Bool;
          return;
        }
      }
    }
  });
  expect(forgedType.first && !forgedType.second.valid() &&
             hasMirVerificationError(forgedType.second, "payload type"),
         "MIR verification should independently reject a return transfer "
         "whose dependency payload type is forged");

  lang::SymbolId shellField = 0;
  if (entryLoan != nullptr && relayMir != nullptr) {
    const lang::MirPlace *entry = relayMir->body.findPlace(entryLoan->source);
    if (entry != nullptr) {
      const auto carrier = std::find_if(
          valid.mir.classInstances().begin(), valid.mir.classInstances().end(),
          [&](const lang::MirClassInstance &instance) {
            return instance.type == entry->type;
          });
      if (carrier != valid.mir.classInstances().end()) {
        const auto shell = std::find_if(
            carrier->declaredFields.begin(), carrier->declaredFields.end(),
            [&](const lang::MirClassFieldInfo &field) {
              return field.symbol != entryLoan->designatedFields.front() &&
                     field.type == lang::SemanticType::Int32;
            });
        if (shell != carrier->declaredFields.end()) {
          shellField = shell->symbol;
        }
      }
    }
  }
  const auto forgedRuntimePlace =
      mutateRelay([shellField](lang::MirFunctionInstance &fn) {
        lang::MirLoanId returned = 0;
        for (const lang::MirLoan &loan : fn.body.loans) {
          if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
            returned = loan.id;
          }
        }
        for (lang::MirBlock &block : fn.body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            if (instruction.kind == lang::MirInstructionKind::Borrow &&
                instruction.loan == returned && !instruction.operands.empty()) {
              lang::MirPlace *runtime = const_cast<lang::MirPlace *>(
                  fn.body.findPlace(instruction.operands.front().place));
              if (runtime != nullptr && shellField != 0 &&
                  runtime->projections.size() == 2 &&
                  runtime->projections.front().kind ==
                      lang::MirProjectionKind::Field) {
                runtime->projections.front().field = shellField;
              }
              return;
            }
          }
        }
      });
  expect(forgedRuntimePlace.first && !forgedRuntimePlace.second.valid() &&
             hasMirVerificationError(forgedRuntimePlace.second,
                                     "designated stored referent place"),
         "MIR verification should independently reject a return transfer "
         "whose same-typed runtime place is the carrier shell field");

  const auto forgedCallResultPlace =
      mutateRelay([](lang::MirFunctionInstance &fn) {
        lang::MirLoanId returned = 0;
        lang::MirInstruction *transfer = nullptr;
        for (const lang::MirLoan &loan : fn.body.loans) {
          if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
            returned = loan.id;
          }
        }
        for (lang::MirBlock &block : fn.body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            if (instruction.kind == lang::MirInstructionKind::Borrow &&
                instruction.loan == returned && !instruction.operands.empty()) {
              transfer = &instruction;
            }
          }
        }
        if (transfer == nullptr) {
          return;
        }
        for (const lang::MirPlace &place : fn.body.places) {
          const lang::MirLoan *loan = fn.body.findLoan(place.loan);
          if (place.root == lang::MirPlaceRootKind::Loan &&
              place.type == transfer->operands.front().type &&
              loan != nullptr && loan->kind == lang::MirLoanKind::CallResult) {
            transfer->operands.front().place = place.id;
            return;
          }
        }
      });
  expect(forgedCallResultPlace.first && !forgedCallResultPlace.second.valid() &&
             hasMirVerificationError(forgedCallResultPlace.second,
                                     "designated stored referent place"),
         "MIR verification should independently reject an ordinary "
         "reference-return call result forged into the runtime place");

  lang::MirProgram forgedCarrier = valid.mir;
  auto &carrierFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forgedCarrier.functionInstances());
  bool forgedUnrelatedCarrier = false;
  if (relayCarrierHir != nullptr) {
    const auto candidate =
        std::find_if(carrierFunctions.begin(), carrierFunctions.end(),
                     [&](const lang::MirFunctionInstance &instance) {
                       return instance.id == relayCarrierHir->id;
                     });
    if (candidate != carrierFunctions.end()) {
      const auto entry = std::find_if(
          candidate->body.loans.begin(), candidate->body.loans.end(),
          [](const lang::MirLoan &loan) {
            return loan.kind == lang::MirLoanKind::Stored && loan.entry &&
                   !loan.designatedFields.empty();
          });
      lang::MirValueId alternateValue = 0;
      for (const lang::MirValue &value : candidate->body.values) {
        const lang::MirBlock *definition =
            candidate->body.findBlock(value.definitionBlock);
        if (definition == nullptr || value.info.type != candidate->returnType) {
          continue;
        }
        const auto producer = std::find_if(
            definition->instructions.begin(), definition->instructions.end(),
            [&](const lang::MirInstruction &instruction) {
              return instruction.id == value.definition;
            });
        if (producer == definition->instructions.end() ||
            producer->operands.size() != 1) {
          continue;
        }
        const lang::MirPlace *source =
            candidate->body.findPlace(producer->operands.front().place);
        if (source != nullptr &&
            source->root == lang::MirPlaceRootKind::Binding &&
            source->projections.empty() &&
            entry != candidate->body.loans.end() &&
            std::find(entry->carriers.begin(), entry->carriers.end(),
                      source->binding) == entry->carriers.end()) {
          alternateValue = value.id;
          break;
        }
      }
      lang::MirLoanId returned = 0;
      for (const lang::MirLoan &loan : candidate->body.loans) {
        if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
          returned = loan.id;
        }
      }
      for (lang::MirBlock &block : candidate->body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Borrow &&
              instruction.loan == returned && !instruction.operands.empty() &&
              alternateValue != 0) {
            instruction.operands.front().value = alternateValue;
            if (block.terminator.kind == lang::MirTerminatorKind::Return &&
                block.terminator.returnLoan == returned &&
                block.terminator.value &&
                block.terminator.value->kind == lang::MirOperandKind::Value) {
              block.terminator.value->value = alternateValue;
              forgedUnrelatedCarrier = true;
            }
          }
        }
      }
    }
  }
  const lang::MirVerificationResult unrelatedCarrierResult =
      lang::verifyMirProgram(forgedCarrier);
  expect(forgedUnrelatedCarrier && !unrelatedCarrierResult.valid() &&
             hasMirVerificationError(unrelatedCarrierResult,
                                     "exact runtime carrier"),
         "MIR verification should correlate a carrier return with the exact "
         "entry loan instead of accepting an unrelated same-typed carrier");

  const std::string shellSource = R"(
class ReadCarrier {
public:
  int& referent;
  mut int shell = 0;
  ReadCarrier(int& source) : referent(source) {}
};
int& escape_shell(ReadCarrier carrier) { return carrier.shell; }
)";
  const lang::FrontendResult shell =
      lang::Frontend().analyze("stored-carrier-shell-escape.gti", shellSource);
  expect(!shell.canGenerateCode() &&
             hasDiagnosticAt(shell.diagnostics, "GTI-S2085",
                             "stored-carrier-shell-escape.gti",
                             shellSource.rfind("shell"),
                             "projection of the by-value carrier shell"),
         "a carrier object's ordinary field should fail at the returned "
         "field with the carrier-shell provenance reason");

  const std::string shellProjectionSource = R"(
class Shell {
public:
  int value = 0;
public:
  Shell() = default;
};
class ReadCarrier {
public:
  int& referent;
  Shell shell = Shell();
  ReadCarrier(int& source) : referent(source) {}
};
int& escape_shell_projection(ReadCarrier carrier) {
  return carrier.shell.value;
}
)";
  const lang::FrontendResult shellProjection = lang::Frontend().analyze(
      "stored-carrier-shell-projection.gti", shellProjectionSource);
  expect(!shellProjection.canGenerateCode() &&
             hasDiagnosticAt(shellProjection.diagnostics, "GTI-S2085",
                             "stored-carrier-shell-projection.gti",
                             shellProjectionSource.rfind("value"),
                             "projection of the by-value carrier shell"),
         "a nested projection rooted in the carrier shell should be rejected "
         "at the escaping expression");

  const std::string referentProjectionSource = R"(
class Node {
public:
  Node& next;
  Node(Node& source) : next(source) {}
};
class NodeCarrier {
public:
  Node& referent;
  NodeCarrier(Node& source) : referent(source) {}
};
Node& escape_referent_projection(NodeCarrier carrier) {
  return carrier.referent.next;
}
)";
  const lang::FrontendResult referentProjection = lang::Frontend().analyze(
      "stored-carrier-referent-projection.gti", referentProjectionSource);
  expect(!referentProjection.canGenerateCode() &&
             hasDiagnosticAt(referentProjection.diagnostics, "GTI-S2085",
                             "stored-carrier-referent-projection.gti",
                             referentProjectionSource.rfind("next"),
                             "projection of the carried referent"),
         "a same-typed projection beyond the designated stored referent "
         "should not masquerade as the exact external dependency");

  const std::string helperProjectionSource = R"(
class Node {
public:
  Node& next;
  Node(Node& source) : next(source) {}
};
class NodeCarrier {
public:
  Node& referent;
  NodeCarrier(Node& source) : referent(source) {}
};
Node& project(Node& source) { return source.next; }
Node& escape_through_helper(NodeCarrier carrier) {
  return project(carrier.referent);
}
)";
  const lang::FrontendResult helperProjection = lang::Frontend().analyze(
      "stored-carrier-helper-projection.gti", helperProjectionSource);
  expect(!helperProjection.canGenerateCode() &&
             hasDiagnosticAt(helperProjection.diagnostics, "GTI-S2085",
                             "stored-carrier-helper-projection.gti",
                             helperProjectionSource.rfind(')'),
                             "does not relay the exact external owner "
                             "dependency"),
         "an ordinary reference-return summary should not launder a "
         "same-typed projection of the carried referent");

  const std::string incompatibleSource = R"(
class ReadCarrier {
public:
  int& referent;
  mut uint64_t position = 0;
  ReadCarrier(int& source) : referent(source) {}
};
uint64_t& escape_incompatible(ReadCarrier carrier) {
  return carrier.position;
}
)";
  const lang::FrontendResult incompatible = lang::Frontend().analyze(
      "stored-carrier-incompatible-referent.gti", incompatibleSource);
  expect(!incompatible.canGenerateCode() &&
             hasDiagnosticAt(incompatible.diagnostics, "GTI-S2085",
                             "stored-carrier-incompatible-referent.gti",
                             incompatibleSource.rfind("position"),
                             "proves only 'int32_t'"),
         "a carrier-shell reference with a type unlike the external referent "
         "should report the incoherent owner-dependency type");

  const std::string copyAssignmentSource = R"(
class ReadCarrier {
public:
  int& referent;
  ReadCarrier(int& source) : referent(source) {}
};
int use() {
  mut int owner = 1;
  mut ReadCarrier left = ReadCarrier(owner);
  ReadCarrier right = ReadCarrier(owner);
  left = right;
  return 0;
}
)";
  const lang::FrontendResult copyAssignment = lang::Frontend().analyze(
      "stored-carrier-copy-assignment.gti", copyAssignmentSource);
  expect(!copyAssignment.canGenerateCode() &&
             hasDiagnosticAt(copyAssignment.diagnostics, "GTI-S2003",
                             "stored-carrier-copy-assignment.gti",
                             copyAssignmentSource.rfind("right"),
                             "Cannot assign a value of type"),
         "read-only carriers should reject copy assignment at the source "
         "value because assignment could reseat their dependency");

  const std::string mutableCopySource = R"(
import std.vector;
int use() {
  mut std::vector<int> values = std::vector<int>({1});
  mut auto cursor = values.begin_mut();
  auto copied = cursor;
  return 0;
}
)";
  const lang::FrontendResult mutableCopy = lang::Frontend().analyze(
      "mutable-carrier-copy.gti", mutableCopySource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(!mutableCopy.canGenerateCode() &&
             hasDiagnosticAt(mutableCopy.diagnostics, "GTI-S2003",
                             "mutable-carrier-copy.gti",
                             mutableCopySource.rfind("cursor"),
                             "copying move-only type"),
         "a mutable owner-loan carrier should reject copy construction at "
         "the attempted source alias");

  const std::string aliasLifetimeSource = R"(
import std.vector;
int main() {
  mut std::vector<int> values = std::vector<int>({1, 2, 3});
  {
    mut auto first = values.begin();
    {
      auto copied = first;
      mut auto moved = std::move(copied);
      ++moved;
      int observed = *moved;
    }
    int original = *first;
  }
  values.push_back(4);
  return 0;
}
)";
  const lang::FrontendResult aliasLifetime = lang::Frontend().analyze(
      "stored-carrier-alias-lifetime.gti", aliasLifetimeSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(aliasLifetime.canGenerateCode(),
         "copying, moving, and dropping one read-only carrier alias should "
         "leave the shared owner loan live until the final alias ends");

  const std::string liveAliasSource = R"(
import std.vector;
int use() {
  mut std::vector<int> values = std::vector<int>({1, 2});
  mut auto first = values.begin();
  {
    auto copied = first;
    mut auto moved = std::move(copied);
    int observed = *moved;
  }
  values.push_back(3);
  int still_live = *first;
  return still_live;
}
)";
  const lang::FrontendResult liveAlias = lang::Frontend().analyze(
      "stored-carrier-live-alias.gti", liveAliasSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::size_t invalidation =
      liveAliasSource.find(')', liveAliasSource.find("push_back"));
  expect(!liveAlias.canGenerateCode() &&
             hasDiagnosticAt(liveAlias.diagnostics, "GTI-S2017",
                             "stored-carrier-live-alias.gti", invalidation,
                             "while a reference borrowed from it may still "
                             "be live"),
         "dropping a copied or moved alias must not end the owner loan while "
         "another carrier remains live");
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

void testCallSiblingArgumentBorrowConflicts() {
  // f(v.begin_mut(), v.end()): the first argument's exclusive owner loan is
  // live while the second argument evaluates its receiver read. Semantics
  // must reject this before MIR construction refuses the overlap.
  const std::string mutThenReadSource = R"(
import std.vector;

void consume<A, B>(A first, B last) {}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  consume(values.begin_mut(), values.end());
  return 0;
}
)";
  const lang::FrontendResult mutThenRead = lang::Frontend().analyze(
      "sibling-argument-mut-then-read.gti", mutThenReadSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!mutThenRead.canGenerateCode() &&
             countDiagnosticCode(mutThenRead.diagnostics, "GTI-S2017") == 1 &&
             hasDiagnosticAt(mutThenRead.diagnostics, "GTI-S2017",
                             "sibling-argument-mut-then-read.gti",
                             mutThenReadSource.rfind("end()"),
                             "Cannot read storage while an exclusive mutable "
                             "borrow created by an earlier operand") &&
             hasRelatedDiagnostic(mutThenRead.diagnostics,
                                  "stays live until the call completes") &&
             hasDiagnosticHint(mutThenRead.diagnostics,
                               "Whole-container algorithms take the "
                               "container itself"),
         "a sibling read of an owner with a live exclusive argument borrow "
         "should be a semantic rejection, not a MIR construction failure");

  // The same conflict without any borrow in the read result: size() only
  // reads the owner ephemerally, but the exclusive loan is still live.
  const std::string mutThenPlainReadSource = R"(
import std.vector;

void consume<A, B>(A first, B count) {}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  consume(values.begin_mut(), values.size());
  return 0;
}
)";
  const lang::FrontendResult mutThenPlainRead = lang::Frontend().analyze(
      "sibling-argument-mut-then-plain-read.gti", mutThenPlainReadSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!mutThenPlainRead.canGenerateCode() &&
             countDiagnosticCode(mutThenPlainRead.diagnostics, "GTI-S2017") ==
                 1 &&
             hasDiagnosticAt(mutThenPlainRead.diagnostics, "GTI-S2017",
                             "sibling-argument-mut-then-plain-read.gti",
                             mutThenPlainReadSource.rfind("size()"),
                             "Cannot read storage while an exclusive mutable "
                             "borrow created by an earlier operand"),
         "an ephemeral receiver read after a sibling exclusive argument "
         "borrow of the same owner should be rejected at semantics");

  // Two exclusive borrows of one owner in a single call stay rejected by the
  // pairwise overlap check.
  const std::string mutThenMutSource = R"(
import std.vector;

void consume<A, B>(A first, B last) {}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  consume(values.begin_mut(), values.begin_mut());
  return 0;
}
)";
  const lang::FrontendResult mutThenMut = lang::Frontend().analyze(
      "sibling-argument-mut-then-mut.gti", mutThenMutSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!mutThenMut.canGenerateCode() &&
             countDiagnosticCode(mutThenMut.diagnostics, "GTI-S2017") == 1 &&
             hasDiagnostic(mutThenMut.diagnostics,
                           "overlapping reference accesses"),
         "two exclusive sibling argument borrows of one owner should stay "
         "rejected by the call overlap check");

  // Reversed order is sound: end() finishes its ephemeral receiver read
  // before begin_mut() creates the exclusive loan.
  const std::string readThenMutSource = R"(
import std.vector;

void consume<A, B>(A last, B first) {}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  consume(values.end(), values.begin_mut());
  return 0;
}
)";
  const lang::FrontendResult readThenMut = lang::Frontend().analyze(
      "sibling-argument-read-then-mut.gti", readThenMutSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(readThenMut.canGenerateCode() &&
             lang::verifyMirProgram(readThenMut.mir).valid(),
         "an ephemeral read completed before a sibling exclusive borrow "
         "begins should stay accepted through MIR verification");

  // An unretained production loan survives a nested consuming call: the
  // exclusive iterator is consumed inside the first operand, but its owner
  // loan lasts through the full expression, so the sibling read conflicts.
  const std::string consumedCarrierSource = R"(
import std.vector;

int32_t drain<Iterator>(Iterator cursor) {
  return 0;
}

void consume<A, B>(A first, B last) {}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  consume(drain(values.begin_mut()), values.end());
  return 0;
}
)";
  const lang::FrontendResult consumedCarrier = lang::Frontend().analyze(
      "sibling-argument-consumed-carrier.gti", consumedCarrierSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!consumedCarrier.canGenerateCode() &&
             countDiagnosticCode(consumedCarrier.diagnostics, "GTI-S2017") ==
                 1 &&
             hasDiagnosticAt(consumedCarrier.diagnostics, "GTI-S2017",
                             "sibling-argument-consumed-carrier.gti",
                             consumedCarrierSource.rfind("end()"),
                             "Cannot read storage while an exclusive mutable "
                             "borrow created by an earlier operand"),
         "an exclusive borrow consumed by a nested call still holds its "
         "owner loan through the full expression, so a sibling read is "
         "rejected at semantics");

  // The mirrored nesting: a read carrier consumed inside the first operand
  // still holds a read loan, so a later operand mutation conflicts.
  const std::string nestedReadThenMutSource = R"(
import std.vector;

int32_t drain<Iterator>(Iterator cursor) {
  return 0;
}

int32_t bump(mut std::vector<int32_t>& sink) {
  sink.push_back(9);
  return 0;
}

void consume<A, B>(A first, B second) {}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  consume(drain(values.begin()), bump(values));
  return 0;
}
)";
  const lang::FrontendResult nestedReadThenMut = lang::Frontend().analyze(
      "sibling-argument-nested-read-then-mut.gti", nestedReadThenMutSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!nestedReadThenMut.canGenerateCode() &&
             countDiagnosticCode(nestedReadThenMut.diagnostics, "GTI-S2017") ==
                 1 &&
             hasDiagnostic(nestedReadThenMut.diagnostics,
                           "A later call operand mutates storage borrowed by "
                           "an earlier operand"),
         "a read borrow consumed by a nested call still holds its owner loan, "
         "so a later sibling mutation is rejected at semantics");

  // A completed nested mutable-reference call holds no loan afterwards, so
  // a later sibling read of the same owner stays valid.
  const std::string completedMutationSource = R"(
import std.vector;

int32_t bump(mut std::vector<int32_t>& sink) {
  sink.push_back(9);
  return 0;
}

void consume<A, B>(A first, B count) {}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
  consume(bump(values), values.size());
  return 0;
}
)";
  const lang::FrontendResult completedMutation = lang::Frontend().analyze(
      "sibling-argument-completed-mutation.gti", completedMutationSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(completedMutation.canGenerateCode() &&
             lang::verifyMirProgram(completedMutation.mir).valid(),
         "a nested mutable-reference call completed in an earlier operand "
         "should not block a later sibling read");

  // Borrows of two different owners in one call never overlap.
  const std::string twoOwnersSource = R"(
import std.vector;

void consume<A, B>(A first, B last) {}

int use() {
  mut std::vector<int32_t> left = std::vector<int32_t>({1});
  mut std::vector<int32_t> right = std::vector<int32_t>({2});
  consume(left.begin_mut(), right.end());
  return 0;
}
)";
  const lang::FrontendResult twoOwners = lang::Frontend().analyze(
      "sibling-argument-two-owners.gti", twoOwnersSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(twoOwners.canGenerateCode() &&
             lang::verifyMirProgram(twoOwners.mir).valid(),
         "sibling argument borrows of two different owners should stay "
         "accepted through MIR verification");
}

void testForwardIteratorContractsAndBinarySearch() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "forward-iterator-contracts.gti", R"(
import std.algorithm;
import std.iterator;
import std.vector;

int main() {
  mut std::vector<int32_t> values =
      std::vector<int32_t>({1, 3, 5, 7});
  mut auto first = values.begin();
  mut auto copied = first;
  ++copied;
  std::size_t distance = std::size_t(1);
  mut auto advanced = std::next(std::move(copied), distance);
  int32_t sought = 5;
  bool found = std::binary_search(values.begin(), values.end(), sought);
  return (*first == 1 && *advanced == 5 && found) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected forward-iterator diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "a copied owner-tied read iterator should satisfy forward_iterator "
         "and drive std::next and std::binary_search");

  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const auto bindingAt = [&](std::size_t index) -> const lang::BindingInfo * {
    if (main == nullptr || index >= main->body()->statements().size()) {
      return nullptr;
    }
    const auto *binding = dynamic_cast<const lang::VariableDecl *>(
        main->body()->statements().at(index).get());
    return binding == nullptr ? nullptr : valid.semantics.findBinding(*binding);
  };
  const lang::BindingInfo *first = bindingAt(1);
  const lang::BindingInfo *copied = bindingAt(2);
  const lang::BindingInfo *advanced = bindingAt(5);
  expect(first != nullptr && copied != nullptr && advanced != nullptr &&
             first->traits.copyable && !first->traits.copyAssignable &&
             first->retainedLoan != 0 &&
             first->retainedLoan == copied->retainedLoan &&
             first->retainedLoan == advanced->retainedLoan,
         "forward read cursors should copy-construct independent positions "
         "while preserving one read-only owner dependency");

  const lang::HirFunctionInstance *nextHir = nullptr;
  const lang::HirFunctionInstance *binarySearchHir = nullptr;
  const lang::HirFunctionInstance *countedSearchHir = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "next" &&
        instance.parameterTypes.size() == 2 &&
        instance.returnType.kind == lang::SemanticType::Class) {
      nextHir = &instance;
    } else if (instance.source->name().lexeme == "binary_search") {
      binarySearchHir = &instance;
    } else if (instance.source->name().lexeme == "binary_search_counted") {
      countedSearchHir = &instance;
    }
  }
  const bool hirEntryStored =
      nextHir != nullptr &&
      std::any_of(nextHir->body.loans.begin(), nextHir->body.loans.end(),
                  [](const lang::HirLoan &loan) {
                    return loan.kind == lang::SemanticLoanKind::StoredValue &&
                           loan.entry &&
                           loan.access == lang::AccessMode::ReadOnly;
                  });
  expect(nextHir != nullptr && binarySearchHir != nullptr &&
             countedSearchHir != nullptr && hirEntryStored &&
             nextHir->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             nextHir->returnBorrowParameter == 0 &&
             nextHir->returnBorrowAccess == lang::AccessMode::ReadOnly,
         "concrete HIR should retain the owner-tied parameter and return "
         "contracts used by forward-iterator utilities");

  const lang::MirFunctionInstance *nextMir =
      nextHir == nullptr ? nullptr
                         : valid.mir.findFunctionInstance(nextHir->id);
  const lang::MirLoan *entryStored = nullptr;
  const lang::MirLoan *returnLoan = nullptr;
  if (nextMir != nullptr) {
    for (const lang::MirLoan &loan : nextMir->body.loans) {
      if (loan.kind == lang::MirLoanKind::Stored && loan.entry) {
        entryStored = &loan;
      } else if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
        returnLoan = &loan;
      }
    }
  }
  bool exactReturnTransfer = false;
  if (nextMir != nullptr && entryStored != nullptr && returnLoan != nullptr) {
    for (const lang::MirBlock &block : nextMir->body.blocks) {
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        const lang::MirInstruction &borrow = block.instructions[index];
        const lang::MirInstruction &end = block.instructions[index + 1];
        exactReturnTransfer =
            exactReturnTransfer ||
            (borrow.kind == lang::MirInstructionKind::Borrow && borrow.loan &&
             *borrow.loan == returnLoan->id && borrow.operands.size() == 1 &&
             borrow.operands.front().kind == lang::MirOperandKind::Loan &&
             borrow.operands.front().loan == entryStored->id &&
             end.kind == lang::MirInstructionKind::EndBorrow && end.loan &&
             *end.loan == entryStored->id);
      }
    }
  }
  expect(nextMir != nullptr && entryStored != nullptr &&
             returnLoan != nullptr && exactReturnTransfer &&
             nextMir->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             nextMir->returnBorrowParameter == 0,
         "MIR should transfer a borrowed-state class return from its stored "
         "parameter loan into a distinct escaping return loan");

  if (valid.canGenerateCode()) {
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(valid.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
    expect(!artifact.contents.empty(),
           "the C++ backend should emit the complete owner-tied forward "
           "iterator and binary-search client");
  }

  const std::string copyBlockedSource = R"(
import std.iterator;

class CopyBlockedIterator {
  int& value;
public:
  CopyBlockedIterator(int& source) : value(source) {}
  CopyBlockedIterator(CopyBlockedIterator& other) = delete;
  CopyBlockedIterator(CopyBlockedIterator&& other) = default;
  int& operator*() { return this.value; }
  void operator++() mut {}
  bool operator!=(CopyBlockedIterator& other) { return false; }
};

bool accepts_forward<Iterator>(Iterator iterator)
  requires std::forward_iterator<Iterator> {
  return true;
}

int use() {
  mut int value = 1;
  mut CopyBlockedIterator blocked = CopyBlockedIterator(value);
  bool result = accepts_forward(std::move(blocked));
  return 0;
}
)";
  const lang::FrontendResult copyBlocked = lang::Frontend().analyze(
      "forward-iterator-copy-blocked.gti", copyBlockedSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::size_t copyBlockedCall =
      copyBlockedSource.rfind("accepts_forward");
  expect(
      !copyBlocked.canGenerateCode() &&
          hasDiagnosticAt(copyBlocked.diagnostics, "GTI-S2029",
                          "forward-iterator-copy-blocked.gti",
                          copyBlockedSource.find(';', copyBlockedCall) - 2,
                          "does not satisfy concept 'std::forward_iterator<"),
      "forward_iterator should reject a cursor without copy construction at "
      "the constrained call");

  const std::string distinctSentinelSource = R"(
import std.iterator;

class DistinctSentinel { public: DistinctSentinel() = default; };

class DistinctOnlyIterator {
  int& value;
public:
  DistinctOnlyIterator(int& source) : value(source) {}
  int& operator*() { return this.value; }
  void operator++() mut {}
  bool operator!=(DistinctSentinel& other) { return false; }
};

bool accepts_forward<Iterator>(Iterator iterator)
  requires std::forward_iterator<Iterator> {
  return true;
}

int use() {
  mut int value = 1;
  DistinctOnlyIterator distinct = DistinctOnlyIterator(value);
  bool result = accepts_forward(distinct);
  return 0;
}
)";
  const lang::FrontendResult distinctSentinel = lang::Frontend().analyze(
      "forward-iterator-distinct-sentinel.gti", distinctSentinelSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::size_t distinctCall =
      distinctSentinelSource.rfind("accepts_forward");
  expect(!distinctSentinel.canGenerateCode() &&
             hasDiagnosticAt(distinctSentinel.diagnostics, "GTI-S2029",
                             "forward-iterator-distinct-sentinel.gti",
                             distinctSentinelSource.find(';', distinctCall) - 1,
                             "satisfy concept 'std::forward_iterator"),
         "forward_iterator should reject a distinct-sentinel-only cursor at "
         "the constrained call with the structural reason");

  const std::string mutableCursorSource = R"(
import std.iterator;
import std.vector;

bool accepts_forward<Iterator>(Iterator iterator)
  requires std::forward_iterator<Iterator> {
  return true;
}

int use() {
  mut std::vector<int> values = std::vector<int>({1});
  mut auto cursor = values.begin_mut();
  bool result = accepts_forward(std::move(cursor));
  return 0;
}
)";
  const lang::FrontendResult mutableCursor = lang::Frontend().analyze(
      "forward-iterator-mutable-cursor.gti", mutableCursorSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::size_t mutableCall = mutableCursorSource.rfind("accepts_forward");
  expect(
      !mutableCursor.canGenerateCode() &&
          hasDiagnosticAt(mutableCursor.diagnostics, "GTI-S2029",
                          "forward-iterator-mutable-cursor.gti",
                          mutableCursorSource.find(';', mutableCall) - 2,
                          "does not satisfy concept 'std::forward_iterator<"),
      "forward_iterator should reject a mutable owner-loan cursor at the "
      "constrained call");

  const std::string mutCursorReadReceiverSource = R"(
import std.iterator;
import std.vector;

bool probe<Iterator, Sentinel>(mut Iterator first, Sentinel last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> {
  return first != last;
}

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>(std::size_t(2));
  auto last = values.end();
  bool inside = probe(values.begin_mut(), last);
  return inside ? 0 : 1;
}
)";
  const lang::FrontendResult mutCursorReadReceiver = lang::Frontend().analyze(
      "mut-cursor-read-receiver.gti", mutCursorReadReceiverSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!mutCursorReadReceiver.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic :
         mutCursorReadReceiver.diagnostics) {
      std::cerr << "  " << diagnostic.code << ": " << diagnostic.message
                << '\n';
    }
  }
  expect(mutCursorReadReceiver.canGenerateCode() &&
             lang::verifyMirProgram(mutCursorReadReceiver.mir).valid(),
         "a read-receiver method on a by-value mut cursor parameter must "
         "pass MIR verification: the entry loan protects the caller's "
         "storage, not the carrier object itself");

  const std::string mutCursorLoopSource = R"(
import std.iterator;
import std.vector;

int32_t drain<Iterator, Sentinel>(mut Iterator first, Sentinel last)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::dereferences_to<Iterator, int32_t> {
  mut int32_t total = 0;
  while (first != last) {
    total = total + *first;
    ++first;
  }
  return total;
}

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>({3, 4, 5});
  auto last = values.end();
  return drain(values.begin_mut(), last) == 12 ? 0 : 1;
}
)";
  const lang::FrontendResult mutCursorLoop = lang::Frontend().analyze(
      "mut-cursor-loop.gti", mutCursorLoopSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  if (!mutCursorLoop.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : mutCursorLoop.diagnostics) {
      std::cerr << "  " << diagnostic.code << ": " << diagnostic.message
                << '\n';
    }
  }
  expect(mutCursorLoop.canGenerateCode() &&
             lang::verifyMirProgram(mutCursorLoop.mir).valid(),
         "an iterator-pair loop whose condition read-borrows a by-value mut "
         "cursor parameter must pass MIR verification across the loop "
         "backedge");
}

void testWritableAndMovementIteratorCapabilities() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "writable-movement-iterators.gti", R"(
import std.algorithm;
import std.iterator;
import std.vector;

int main() {
  mut std::vector<int32_t> source = std::vector<int32_t>();
  source.push_back(5);
  source.push_back(1);
  source.push_back(4);
  mut std::vector<int32_t> destination = std::vector<int32_t>(std::size_t(3));
  std::copy(source.begin(), source.end(), destination.begin_mut());
  int32_t nine = 9;
  std::fill(destination, nine);
  mut std::vector<int32_t> doubled = std::vector<int32_t>(std::size_t(3));
  std::transform(source, doubled,
                 [](int32_t& value) -> int32_t { return value * 2; });
  std::sort(source);
  std::reverse(source);
  int32_t head = source[std::size_t(0)];
  mut auto cursor = source.begin();
  ++cursor;
  --cursor;
  cursor += std::ptrdiff_t(2);
  cursor -= std::ptrdiff_t(1);
  auto last = source.end();
  std::ptrdiff_t left = cursor - last;
  auto stepped = std::prev(std::next(source.begin(), std::ptrdiff_t(2)));
  return (*cursor == 4 && left == std::ptrdiff_t(2) && *stepped == 4 &&
          destination[std::size_t(0)] == 9 &&
          doubled[std::size_t(0)] == 10 && head == 5)
             ? 0
             : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected writable-movement diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "writable, bidirectional, random-access, and sized-sentinel "
         "capabilities should drive copy/fill/transform/sort/reverse and "
         "iterator movement to valid MIR");

  const lang::FrontendResult readDestination = lang::Frontend().analyze(
      "writable-read-destination.gti", R"(
import std.algorithm;
import std.vector;

int main() {
  mut std::vector<int32_t> source = std::vector<int32_t>();
  source.push_back(1);
  mut std::vector<int32_t> destination = std::vector<int32_t>(std::size_t(1));
  std::copy(source.begin(), source.end(), destination.begin());
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!readDestination.canGenerateCode() &&
             hasDiagnostic(readDestination.diagnostics,
                           "satisfy concept 'std::output_iterator"),
         "a read-only iterator destination should fail std::output_iterator");

  const lang::FrontendResult forwardOnly = lang::Frontend().analyze(
      "bidirectional-forward-only.gti", R"(
import std.iterator;

class ForwardOnly {
  mut int32_t value = 0;

public:
  ForwardOnly() {}
  int32_t& operator*() { return this.value; }
  void operator++() mut {}
  bool operator!=(ForwardOnly& other) { return false; }
};

void walk_back<Iterator>(mut Iterator iterator)
  requires std::bidirectional_iterator<Iterator> {
  --iterator;
}

int use() {
  mut ForwardOnly cursor = ForwardOnly();
  walk_back(std::move(cursor));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!forwardOnly.canGenerateCode() &&
             hasDiagnostic(forwardOnly.diagnostics,
                           "satisfy concept 'std::bidirectional_iterator"),
         "a forward-only cursor should fail std::bidirectional_iterator");

  const lang::FrontendResult noJumps = lang::Frontend().analyze(
      "random-access-no-jumps.gti", R"(
import std.iterator;

class StepOnly {
  mut int32_t value = 0;

public:
  StepOnly() {}
  int32_t& operator*() { return this.value; }
  void operator++() mut {}
  void operator--() mut {}
  bool operator!=(StepOnly& other) { return false; }
};

void jump<Iterator>(mut Iterator iterator)
  requires std::random_access_iterator<Iterator> {
  iterator += std::ptrdiff_t(2);
}

int use() {
  mut StepOnly cursor = StepOnly();
  jump(std::move(cursor));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!noJumps.canGenerateCode() &&
             hasDiagnostic(noJumps.diagnostics,
                           "satisfy concept 'std::random_access_iterator"),
         "a step-only cursor should fail std::random_access_iterator");

  const lang::FrontendResult wrongElement = lang::Frontend().analyze(
      "writes-to-wrong-element.gti", R"(
import std.iterator;
import std.vector;

void stamp<Iterator>(mut Iterator iterator, int16_t value)
  requires std::writes_to<Iterator, int16_t> {
  *iterator = value;
}

int use() {
  mut std::vector<int32_t> values = std::vector<int32_t>(std::size_t(1));
  stamp(values.begin_mut(), int16_t(3));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!wrongElement.canGenerateCode() &&
             hasDiagnostic(wrongElement.diagnostics,
                           "satisfy concept 'std::writes_to"),
         "writes_to should reject a destination whose element type differs "
         "from the required referent");

  const lang::FrontendResult unsizedSentinel = lang::Frontend().analyze(
      "sized-sentinel-missing.gti", R"(
import std.iterator;

class PlainSentinel { public: PlainSentinel() = default; };

class PlainIterator {
  mut int32_t value = 0;

public:
  PlainIterator() {}
  int32_t& operator*() { return this.value; }
  void operator++() mut {}
  bool operator!=(PlainSentinel& other) { return false; }
};

std::ptrdiff_t measure<Iterator, Sentinel>(Iterator first, Sentinel last)
  requires std::sized_sentinel_for<Sentinel, Iterator> {
  return first - last;
}

int use() {
  std::ptrdiff_t left = measure(PlainIterator(), PlainSentinel());
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!unsizedSentinel.canGenerateCode() &&
             hasDiagnostic(unsizedSentinel.diagnostics,
                           "satisfy concept 'std::sized_sentinel_for"),
         "sized_sentinel_for should reject an iterator without the signed "
         "distance operator");

  const lang::FrontendResult userCursor = lang::Frontend().analyze(
      "user-cursor-symbolic-writes.gti", R"(
import std.iterator;

class grid_cursor {
  mut int32_t[6] cells = {};
  mut std::ptrdiff_t position = 0;

public:
  grid_cursor() {}

  mut int32_t& operator*() mut {
    return this.cells[std::size_t(this.position)];
  }

  void operator++() mut { this.position++; }

  mut int32_t& operator[](std::ptrdiff_t offset) mut {
    return this.cells[std::size_t(this.position + offset)];
  }

  int32_t read(std::size_t index) { return this.cells[index]; }
};

void stamp<Cursor, T>(mut Cursor& cursor, T& value)
  requires std::writes_to<Cursor, T> {
  *cursor = value;
}

void poke<Cursor, T>(mut Cursor& cursor, std::ptrdiff_t offset, T& value)
  requires std::subscripts_to_mut<Cursor, T> {
  cursor[offset] = value;
}

int main() {
  mut grid_cursor cursor = grid_cursor();
  int32_t five = 5;
  stamp(cursor, five);
  int32_t seven = 7;
  poke(cursor, std::ptrdiff_t(1), seven);
  return (cursor.read(std::size_t(0)) == 5 &&
          cursor.read(std::size_t(1)) == 7)
             ? 0
             : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!userCursor.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : userCursor.diagnostics) {
      std::cerr << "Unexpected user-cursor diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(userCursor.canGenerateCode() &&
             lang::verifyMirProgram(userCursor.mir).valid(),
         "symbolic mutable dereference and subscript writes should "
         "instantiate against a user cursor and reach valid MIR");
}

void testCompleteRangeConceptsAndAlgorithms() {
  for (const std::string &supported :
       {std::string("void invalid(int[3]& values) {}"),
        std::string("using Triple = int[3]; void invalid(Triple& values) {}"),
        std::string("using Callback = void(int[3]&);")}) {
    const auto valid =
        lang::Frontend().analyze("array-reference-boundary.gti", supported);
    expect(valid.canGenerateCode(),
           "explicit, aliased and signature array references use the same "
           "complete array type as generic range substitution");
  }
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "complete-range-concepts.gti", R"(
import std.algorithm;
import std.iterator;
import std.vector;

class number_range {
  mut std::vector<int32_t> values = std::vector<int32_t>({1, 2, 3});

public:
  number_range() = default;
  std::vector<int32_t>::const_iterator begin() {
    return this.values.begin();
  }
  std::vector<int32_t>::iterator begin_mut() mut {
    return this.values.begin_mut();
  }
  std::vector<int32_t>::sentinel end() { return this.values.end(); }
  std::size_t size() { return this.values.size(); }
  int32_t at(std::size_t index) { return this.values[index]; }
};

class positive {
public:
  bool operator()(int32_t& value) { return value > 0; }
public:
  positive() = default;
};

std::size_t measured_size<Range>(Range& values)
  requires std::sized_range<Range> {
  return values.size();
}

int main() {
  mut number_range values{};
  mut int32_t[3] fixed = {1, 2, 3};
  if (!std::all_of(values, positive()) ||
      !std::all_of(fixed, positive())) {
    return 1;
  }
  int32_t replacement = 4;
  std::fill(values, replacement);
  std::fill(fixed, replacement);
  return (values.at(std::size_t(1)) == 4 && fixed[1] == 4 &&
          measured_size(values) == std::size_t(3) &&
          measured_size(fixed) == std::size_t(3))
             ? 0
             : 2;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected complete-range diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode() && valid.mir.valid() &&
             lang::verifyHirProgramPlans(valid.semantics, valid.hir).valid() &&
             lang::verifyMirProgram(valid.mir).valid(),
         "complete-range concepts should instantiate read-only, mutable, and "
         "sized generic bodies for nominal ranges and fixed arrays");

  std::size_t structuralRanges = 0;
  std::size_t fixedRanges = 0;
  bool exactConcreteProtocol = true;
  bool fixedInstancesUseConcreteArithmetic = true;
  bool fixedSizedRangeUsesArrayIntrinsic = false;
  for (const lang::HirFunctionInstance &hir : valid.hir.functionInstances()) {
    bool fixedAlgorithmInstance = false;
    const bool fixedSizedRangeInstance =
        hir.source != nullptr && hir.source->name().lexeme == "measured_size" &&
        std::any_of(hir.typeArguments.begin(), hir.typeArguments.end(),
                    [](const lang::SemanticType &type) {
                      return type.kind == lang::SemanticType::Array;
                    });
    for (const lang::HirStatement &statement : hir.body.statements) {
      if (!statement.rangeIteration) {
        continue;
      }
      const lang::HirRangeIteration &range = *statement.rangeIteration;
      if (range.strategy == lang::RangeIterationStrategy::Structural) {
        ++structuralRanges;
        exactConcreteProtocol =
            exactConcreteProtocol && range.beginFunction != 0 &&
            range.endFunction != 0 && range.compareFunction != 0 &&
            range.dereferenceFunction != 0 && range.incrementFunction != 0;
      } else {
        ++fixedRanges;
        fixedAlgorithmInstance = true;
        exactConcreteProtocol =
            exactConcreteProtocol && range.arrayLength == 3 &&
            range.beginFunction == 0 && range.endFunction == 0 &&
            range.compareFunction == 0 && range.dereferenceFunction == 0 &&
            range.incrementFunction == 0;
      }
    }
    const lang::MirFunctionInstance *mir =
        fixedAlgorithmInstance || fixedSizedRangeInstance
            ? valid.mir.findFunctionInstance(hir.id)
            : nullptr;
    if (mir == nullptr && fixedAlgorithmInstance) {
      fixedInstancesUseConcreteArithmetic = false;
      continue;
    }
    if (mir == nullptr) {
      continue;
    }
    for (const lang::MirBlock &block : mir->body.blocks) {
      fixedSizedRangeUsesArrayIntrinsic =
          fixedSizedRangeUsesArrayIntrinsic ||
          (fixedSizedRangeInstance &&
           std::any_of(block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                    lang::MirInstructionKind::Call &&
                                instruction.intrinsic ==
                                    lang::IntrinsicKind::ArraySize;
                       }));
      fixedInstancesUseConcreteArithmetic =
          fixedInstancesUseConcreteArithmetic &&
          std::none_of(block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                lang::MirInstructionKind::Modify;
                       });
    }
  }
  expect(structuralRanges == 2 && fixedRanges == 2 && exactConcreteProtocol,
         "concrete HIR should replace symbolic range carriers with exact "
         "nominal protocol identities or the fixed-array strategy");
  expect(fixedInstancesUseConcreteArithmetic,
         "fixed-array generic instances should suppress symbolic iterator "
         "operators and retain checked concrete index arithmetic in MIR");
  expect(fixedSizedRangeUsesArrayIntrinsic,
         "fixed-array sized_range instances should retain the verified "
         "ArraySize intrinsic instead of relying on backend member lookup");

  if (valid.canGenerateCode()) {
    const lang::OptimizationResult optimizations =
        lang::OptimizationPipeline().run(valid.hir,
                                         lang::OptimizationLevel::O0);
    const lang::BackendArtifact artifact =
        gti_test::emitCpp(valid, valid.mir, valid.mir, optimizations);
    expect(!artifact.contents.empty(),
           "the backend should consume verified complete-range MIR without "
           "reconstructing the structural concept proof");
  }

  const lang::FrontendResult missingRange = lang::Frontend().analyze(
      "complete-range-missing-endpoints.gti", R"(
import std.iterator;
class not_a_range { public: not_a_range() = default; };
bool accepts<Range>(Range& values) requires std::input_range<Range> {
  for (auto& value : values) {}
  return true;
}
int main() {
  not_a_range values{};
  return accepts(values) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!missingRange.canGenerateCode() &&
             hasDiagnosticCode(missingRange.diagnostics, "GTI-S2029") &&
             hasDiagnosticHint(missingRange.diagnostics, "begin()/end()"),
         "input_range should reject a type without the exact complete-range "
         "protocol and explain the missing endpoints");

  const lang::FrontendResult wrongElement = lang::Frontend().analyze(
      "complete-range-wrong-element.gti", R"(
import std.iterator;
bool accepts<Range>(Range& values) requires std::range_of<Range, int16_t> {
  return true;
}
int main() {
  int32_t[2] values = {1, 2};
  return accepts(values) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !wrongElement.canGenerateCode() &&
          hasDiagnosticCode(wrongElement.diagnostics, "GTI-S2029") &&
          hasDiagnosticHint(wrongElement.diagnostics,
                            "exactly the required element type"),
      "range_of should reject a complete range whose exact referent differs");

  const lang::FrontendResult missingMutable = lang::Frontend().analyze(
      "complete-range-missing-mutable.gti", R"(
import std.iterator;
import std.vector;
class read_range {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
public:
  std::vector<int32_t>::const_iterator begin() {
    return this.values.begin();
  }
  std::vector<int32_t>::sentinel end() { return this.values.end(); }
public:
  read_range() = default;
};
bool accepts<Range>(mut Range& values)
  requires std::mutable_range_of<Range, int32_t> {
  return true;
}
int main() {
  mut read_range values{};
  return accepts(values) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!missingMutable.canGenerateCode() &&
             hasDiagnosticCode(missingMutable.diagnostics, "GTI-S2029") &&
             hasDiagnosticHint(missingMutable.diagnostics, "begin_mut()/end()"),
         "mutable_range_of should reject a read-only complete range and "
         "explain its exact mutable endpoint contract");

  const lang::FrontendResult wrongSize = lang::Frontend().analyze(
      "complete-range-wrong-size.gti", R"(
import std.iterator;
import std.vector;
class wrong_size_range {
  mut std::vector<int32_t> values = std::vector<int32_t>({1});
public:
  std::vector<int32_t>::const_iterator begin() {
    return this.values.begin();
  }
  std::vector<int32_t>::sentinel end() { return this.values.end(); }
  int32_t size() { return 1; }
public:
  wrong_size_range() = default;
};
bool accepts<Range>(Range& values) requires std::sized_range<Range> {
  return values.size() == std::size_t(1);
}
int main() {
  wrong_size_range values{};
  return accepts(values) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!wrongSize.canGenerateCode() &&
             hasDiagnosticCode(wrongSize.diagnostics, "GTI-S2029") &&
             hasDiagnosticHint(wrongSize.diagnostics,
                               "size() returning std::size_t"),
         "sized_range should reject a range with a non-size_t observer");

  const lang::FrontendResult wrongPredicate = lang::Frontend().analyze(
      "complete-range-wrong-predicate.gti", R"(
import std.functional;
import std.iterator;
class wrong_predicate {
public:
  bool operator()(int16_t& value) { return value > 0; }
public:
  wrong_predicate() = default;
};
bool accepts<Range, Predicate>(Range& values, Predicate predicate)
  requires std::range_predicate<Predicate, Range> {
  return true;
}
int main() {
  int32_t[2] values = {1, 2};
  return accepts(values, wrong_predicate()) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!wrongPredicate.canGenerateCode() &&
             hasDiagnosticCode(wrongPredicate.diagnostics, "GTI-S2029") &&
             hasDiagnosticHint(wrongPredicate.diagnostics,
                               "range's read-only element"),
         "range_predicate should derive and enforce the exact range element");

  const lang::FrontendResult directEndpoint = lang::Frontend().analyze(
      "complete-range-direct-endpoint.gti", R"(
import std.iterator;
bool accepts<Range>(Range& values) requires std::input_range<Range> {
  auto iterator = values.begin();
  return true;
}
int main() {
  int32_t[2] values = {1, 2};
  return accepts(values) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!directEndpoint.canGenerateCode() &&
             hasDiagnosticCode(directEndpoint.diagnostics, "GTI-S2029") &&
             hasDiagnostic(directEndpoint.diagnostics,
                           "Direct structural range endpoint"),
         "complete-range proof carriers should remain private to generated "
         "range iteration until public endpoint extraction has a complete "
         "owner-dependent iterator contract");
}

void testGeneralOwnerDependentReturns() {
  const lang::FrontendResult valid =
      analyzeWithMemory("owner-dependent-returns.gti", R"(
class Reading<T> {
  mut T value;

public:
  Reading(T initial) : value(initial) {}
  T& read() { return this.value; }
  void replace(T next) mut { this.value = next; }
};

class ReadingView<T> {
  T& value;

public:
  ReadingView(T& source) : value(source) {}
  T& get() { return this.value; }
};

class IntViewFactory {
public:
  static ReadingView<int> make(int& source) {
    return ReadingView<int>(source);
  }
public:
  IntViewFactory() = default;
};

class DirectPayload {
public:
  int value;
  DirectPayload(int initial) : value(initial) {}
};

class ViewCallable<T> {
  T& source;

public:
  ViewCallable(T& value) : source(value) {}
  ReadingView<T> operator()() {
    return ReadingView<T>(this.source);
  }
  ReadingView<T> operator[](uint64_t index) {
    return ReadingView<T>(this.source);
  }
};

int main() {
  mut Reading<int> reading = Reading<int>(7);
  mut ReadingView<int> view = make_view<int>(reading.read());
  mut ReadingView<int> forwarded =
      forward_view<int>(std::move(view));
  int& first = forwarded.get();
  int& relayed = forward_reference<int>(first);
  ReadingView<int> static_view = IntViewFactory::make(reading.read());
  ViewCallable<int> callable = ViewCallable<int>(reading.read());
  ReadingView<int> called_view = callable();
  ReadingView<int> indexed_view = callable[uint64_t(0)];
  std::unique_ptr<DirectPayload> direct_owner =
      std::make_unique<DirectPayload>(7);
  DirectPayload& direct_value = direct_owner_value(direct_owner);
  mut ReadingView<int> relay_source = make_view<int>(reading.read());
  ReadingView<int> generic_relay =
      unconstrained_relay<ReadingView<int>>(std::move(relay_source));
  ViewCallable<int> relocated_callable = std::move(callable);
  return relayed + static_view.get() + called_view.get() + indexed_view.get() +
      direct_value.value + generic_relay.get() + shared_aliases() - 42;
}

ReadingView<T> make_view<T>(T& source) {
  return ReadingView<T>(source);
}

ReadingView<T> forward_view<T>(ReadingView<T> view) {
  return std::move(view);
}

T unconstrained_relay<T>(T value) { return std::move(value); }

T& forward_reference<T>(T& source) { return source; }

DirectPayload& direct_owner_value(std::unique_ptr<DirectPayload>& owner) {
  return *owner;
}

int shared_aliases() {
  mut Reading<int> reading = Reading<int>(5);
  {
    ReadingView<int> view = ReadingView<int>(reading.read());
    int& first = view.get();
    int& second = first;
    int sum = first + second;
  }
  reading.replace(6);
  return reading.read() - 6;
}
)");
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected owner-dependency diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "free, static, and generic borrowed returns should preserve a single "
         "read-only owner dependency");

  const lang::FunctionDecl *makeView =
      findTopLevelFunction(valid.program, "make_view");
  const lang::FunctionDecl *forwardView =
      findTopLevelFunction(valid.program, "forward_view");
  const lang::FunctionDecl *forwardReference =
      findTopLevelFunction(valid.program, "forward_reference");
  const lang::FunctionInfo *makeViewInfo =
      makeView == nullptr ? nullptr : valid.semantics.findFunction(*makeView);
  const lang::FunctionInfo *forwardViewInfo =
      forwardView == nullptr ? nullptr
                             : valid.semantics.findFunction(*forwardView);
  const lang::FunctionInfo *forwardReferenceInfo =
      forwardReference == nullptr
          ? nullptr
          : valid.semantics.findFunction(*forwardReference);
  const lang::ClassDecl *factory =
      findTopLevelClass(valid.program, "IntViewFactory");
  const lang::FunctionDecl *staticFactory = nullptr;
  if (factory != nullptr) {
    for (const lang::StmtPtr &member : factory->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == "make") {
        staticFactory = function;
      }
    }
  }
  const lang::FunctionInfo *staticFactoryInfo =
      staticFactory == nullptr ? nullptr
                               : valid.semantics.findFunction(*staticFactory);
  expect(makeViewInfo != nullptr && forwardViewInfo != nullptr &&
             forwardReferenceInfo != nullptr && staticFactoryInfo != nullptr &&
             makeViewInfo->returnBorrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             forwardViewInfo->returnBorrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             forwardReferenceInfo->returnBorrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             staticFactoryInfo->returnBorrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             makeViewInfo->returnBorrowParameter == 0 &&
             forwardViewInfo->returnBorrowParameter == 0 &&
             forwardReferenceInfo->returnBorrowParameter == 0 &&
             staticFactoryInfo->returnBorrowParameter == 0,
         "borrowed-return declarations should publish their exact source "
         "parameter independent of declaration order");

  const lang::HirFunctionInstance *forwardViewInstance = nullptr;
  if (forwardViewInfo != nullptr) {
    for (const lang::HirFunctionInstance &instance :
         valid.hir.functionInstances()) {
      if (instance.declaration == forwardViewInfo->id) {
        forwardViewInstance = &instance;
      }
    }
  }
  const lang::HirBinding *forwardViewParameter = nullptr;
  if (forwardViewInstance != nullptr &&
      !forwardViewInstance->parameterBindings.empty()) {
    const lang::HirBindingId parameter =
        forwardViewInstance->parameterBindings.front();
    const auto binding =
        std::find_if(forwardViewInstance->body.bindings.begin(),
                     forwardViewInstance->body.bindings.end(),
                     [parameter](const lang::HirBinding &candidate) {
                       return candidate.id == parameter;
                     });
    if (binding != forwardViewInstance->body.bindings.end()) {
      forwardViewParameter = &*binding;
    }
  }
  const bool parameterUseMatches =
      forwardViewParameter != nullptr &&
      forwardViewParameter->info.symbol != 0 &&
      std::any_of(forwardViewInstance->body.values.begin(),
                  forwardViewInstance->body.values.end(),
                  [&](const lang::HirValue &value) {
                    return value.symbol == forwardViewParameter->info.symbol;
                  });
  expect(parameterUseMatches,
         "a concrete generic parameter declaration and its uses should share "
         "one nonzero HIR symbol identity");

  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const auto *view = main == nullptr
                         ? nullptr
                         : dynamic_cast<const lang::VariableDecl *>(
                               main->body()->statements().at(1).get());
  const auto *forwarded = main == nullptr
                              ? nullptr
                              : dynamic_cast<const lang::VariableDecl *>(
                                    main->body()->statements().at(2).get());
  const lang::BindingInfo *viewBinding =
      view == nullptr ? nullptr : valid.semantics.findBinding(*view);
  const lang::BindingInfo *forwardedBinding =
      forwarded == nullptr ? nullptr : valid.semantics.findBinding(*forwarded);
  expect(viewBinding != nullptr && forwardedBinding != nullptr &&
             viewBinding->retainedLoan != 0 &&
             viewBinding->retainedLoan == forwardedBinding->retainedLoan,
         "std::move through a borrowed-value relay should transfer one loan "
         "identity");

  const auto *relaySource = main == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::VariableDecl *>(
                                      main->body()->statements().at(11).get());
  const auto *genericRelay = main == nullptr
                                 ? nullptr
                                 : dynamic_cast<const lang::VariableDecl *>(
                                       main->body()->statements().at(12).get());
  const lang::BindingInfo *relaySourceBinding =
      relaySource == nullptr ? nullptr
                             : valid.semantics.findBinding(*relaySource);
  const lang::BindingInfo *genericRelayBinding =
      genericRelay == nullptr ? nullptr
                              : valid.semantics.findBinding(*genericRelay);
  expect(relaySourceBinding != nullptr && genericRelayBinding != nullptr &&
             relaySourceBinding->retainedLoan != 0 &&
             relaySourceBinding->retainedLoan ==
                 genericRelayBinding->retainedLoan,
         "a concrete unconstrained generic borrowed-value return should "
         "publish and transfer its inferred owner dependency");

  const auto *callable = main == nullptr
                             ? nullptr
                             : dynamic_cast<const lang::VariableDecl *>(
                                   main->body()->statements().at(6).get());
  const auto *calledView = main == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::VariableDecl *>(
                                     main->body()->statements().at(7).get());
  const auto *indexedView = main == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::VariableDecl *>(
                                      main->body()->statements().at(8).get());
  const auto *operatorCall =
      calledView == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(calledView->initializer().get());
  const lang::ResolvedOperatorInfo *resolvedOperator =
      operatorCall == nullptr ? nullptr
                              : valid.semantics.findOperator(*operatorCall);
  const lang::ResolvedOperatorInfo *resolvedIndexOperator =
      indexedView == nullptr
          ? nullptr
          : valid.semantics.findOperator(*indexedView->initializer());
  const lang::BindingInfo *callableBinding =
      callable == nullptr ? nullptr : valid.semantics.findBinding(*callable);
  const lang::BindingInfo *calledViewBinding =
      calledView == nullptr ? nullptr
                            : valid.semantics.findBinding(*calledView);
  const lang::BindingInfo *indexedViewBinding =
      indexedView == nullptr ? nullptr
                             : valid.semantics.findBinding(*indexedView);
  expect(
      resolvedOperator != nullptr &&
          resolvedOperator->borrowOrigin == lang::BorrowOriginKind::Receiver &&
          resolvedOperator->borrowAccess == lang::AccessMode::ReadOnly &&
          resolvedIndexOperator != nullptr &&
          resolvedIndexOperator->borrowOrigin ==
              lang::BorrowOriginKind::Receiver &&
          resolvedIndexOperator->borrowAccess == lang::AccessMode::ReadOnly &&
          callableBinding != nullptr && calledViewBinding != nullptr &&
          indexedViewBinding != nullptr && callableBinding->retainedLoan != 0 &&
          callableBinding->retainedLoan == calledViewBinding->retainedLoan &&
          callableBinding->retainedLoan == indexedViewBinding->retainedLoan,
      "operator() and operator[] returning stored-reference values should "
      "preserve their receiver dependency and semantic loan identity");

  const lang::HirValue *hirOperatorCall = nullptr;
  const lang::HirValue *hirIndexOperator = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    for (const lang::HirValue &value : instance.body.values) {
      if (value.source == operatorCall) {
        hirOperatorCall = &value;
      }
      if (indexedView != nullptr &&
          value.source == indexedView->initializer().get()) {
        hirIndexOperator = &value;
      }
    }
  }
  expect(
      hirOperatorCall != nullptr && hirIndexOperator != nullptr &&
          hirOperatorCall->borrowOrigin == lang::BorrowOriginKind::Receiver &&
          hirIndexOperator->borrowOrigin == lang::BorrowOriginKind::Receiver &&
          hirOperatorCall->borrowAccess == lang::AccessMode::ReadOnly &&
          hirIndexOperator->borrowAccess == lang::AccessMode::ReadOnly,
      "HIR should consume operator owner-dependency metadata from the "
      "semantic resolution record");

  bool callsPreserveArgumentDependency = main != nullptr;
  if (main != nullptr) {
    for (const std::size_t index :
         {std::size_t(1), std::size_t(2), std::size_t(4), std::size_t(5),
          std::size_t(10)}) {
      const auto *binding = dynamic_cast<const lang::VariableDecl *>(
          main->body()->statements().at(index).get());
      const auto *call =
          binding == nullptr
              ? nullptr
              : dynamic_cast<const lang::Call *>(binding->initializer().get());
      const lang::ResolvedCallInfo *resolved =
          call == nullptr ? nullptr : valid.semantics.findCall(*call);
      callsPreserveArgumentDependency =
          callsPreserveArgumentDependency && resolved != nullptr &&
          resolved->borrowOrigin == lang::BorrowOriginKind::Argument &&
          resolved->borrowArgument == 0 &&
          resolved->borrowAccess == lang::AccessMode::ReadOnly;
    }
  }
  expect(callsPreserveArgumentDependency,
         "resolved free and static calls should carry the selected owner "
         "argument into lowering");

  const std::string externalOperatorSource = R"(
class ReadingView<T> {
  T& value;
public:
  ReadingView(T& value) : value(value) {}
  T& get() { return this.value; }
};
class ViewCallable<T> {
  T& source;
public:
  ViewCallable(T& value) : source(value) {}
  ReadingView<T> operator()() { return ReadingView<T>(this.source); }
  ReadingView<T> operator[](uint64_t index) {
    return ReadingView<T>(this.source);
  }
};
int main() {
  mut int owner = 7;
  ViewCallable<int> callable{owner};
)";
  for (const auto *operation : {"callable()", "callable[uint64_t(0)]"}) {
    const auto invalidOwner = analyzeWithMemory(
        "operator-external-owner-invalidation.gti",
        externalOperatorSource + "  auto view = " + operation + R"(;
  ViewCallable<int> relocated = std::move(callable);
  owner = 8;
  return view.get();
}
)");
    expect(!invalidOwner.canGenerateCode() &&
               hasDiagnosticCode(invalidOwner.diagnostics, "GTI-S2017") &&
               hasDiagnostic(invalidOwner.diagnostics,
                             "Cannot assign to storage while an overlapping "
                             "borrow") &&
               !hasDiagnostic(invalidOwner.diagnostics,
                              "Cannot move storage while an overlapping "
                              "borrow") &&
               !hasDiagnosticCode(invalidOwner.diagnostics, "GTI-B0001"),
           "moving an operator wrapper must not release the returned view's "
           "external owner loan");
  }

  const lang::FrontendResult invalid =
      analyzeWithMemory("invalid-owner-dependent-returns.gti", R"(
class DirectPayload {
public:
  int value;
  DirectPayload(int initial) : value(initial) {}
};

class View {
  int& value;

public:
  View(int& source) : value(source) {}
  int& get() { return this.value; }
};

int global_value = 1;

int& ambiguous(int& left, int& right) { return left; }
int& mutable_source(mut int& source) { return source; }

int& local_source(int& source) {
  int local = 1;
  return local;
}

int& global_source(int& source) { return global_value; }

View local_view(int& source) {
  int local = 1;
  return View(local);
}

class WrongFactory {
public:
  View make(int& source) { return View(source); }
public:
  WrongFactory() = default;
};

class Owner {
  mut int value;

public:
  Owner(int initial) : value(initial) {}
  int& read() { return this.value; }
  void replace(int next) mut { this.value = next; }
};

int shared_invalidation() {
  mut Owner owner = Owner(1);
  View view = View(owner.read());
  int& first = view.get();
  int& second = first;
  owner.replace(2);
  return second;
}

int mutable_alias() {
  mut int value = 1;
  mut int& first = value;
  int& alias = first;
  return alias;
}

int main() { return 0; }
)");
  expect(!invalid.canGenerateCode(),
         "ambiguous, mutable, and unrelated borrowed returns and aliases "
         "must be rejected");
  expect(hasDiagnostic(invalid.diagnostics, "dependency is ambiguous") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot derive from a mutable parameter") &&
             hasDiagnostic(invalid.diagnostics,
                           "must derive from read-only parameter 'source'") &&
             hasDiagnostic(invalid.diagnostics,
                           "must derive from this method's receiver") &&
             hasDiagnostic(invalid.diagnostics,
                           "Mutable method receiver overlaps a borrow"),
         "owner-dependency diagnostics should identify the unsafe source or "
         "alias graph");

  // A result's value() is a reference tied to its receiver, and an owning
  // handle's checked pointee is storage that receiver contains: the pointee
  // may be retained or returned from a read-only parameter, as for any class.
  const auto nestedOwner = analyzeWithMemory("nested-owner-projection.gti", R"(
class DirectPayload {
public:
  int value;
  DirectPayload(int initial) : value(initial) {}
};

DirectPayload& nested_owner_return(
    expected<std::unique_ptr<DirectPayload>, int>& opened) {
  return *opened.value();
}

int nested_owner_retention() {
  expected<std::unique_ptr<DirectPayload>, int> opened =
      std::make_unique<DirectPayload>(1);
  DirectPayload& nested = *opened.value();
  return nested.value;
}

int main() { return nested_owner_retention() - 1; }
)");
  expect(nestedOwner.canGenerateCode(),
         "a result's owned pointee may be retained or returned from its "
         "read-only owner parameter");
}

void testSwitchAndBreakLoanFlow() {
  const std::string source = R"(
import std.string;

int switch_merge(int selected) {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  switch (selected) {
    case 0:
      char first = *iterator;
      break;
    default:
      break;
  }
  value.push_back('!');
  return 0;
}

int switch_arm_paths(int selected) {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  switch (selected) {
    case 0:
      value.push_back('!');
      break;
    default:
      char first = *iterator;
      value.push_back('?');
      break;
  }
  return 0;
}

int switch_without_default(int selected) {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  switch (selected) {
    case 0:
      char first = *iterator;
      break;
  }
  value.push_back('!');
  return 0;
}

int while_break_path(bool leave) {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  mut int count = 0;
  while (count < 2) {
    if (leave) {
      char first = *iterator;
      value.push_back('!');
      break;
    }
    char repeated = *iterator;
    count++;
  }
  value.push_back('?');
  return 0;
}

int while_break_entry(bool leave) {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  while (true) {
    if (leave) {
      value.push_back('!');
      break;
    }
    char retained = *iterator;
    return retained == 'g' ? 0 : 1;
  }
  return 0;
}

int do_break_path() {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  do {
    char first = *iterator;
    value.push_back('!');
    break;
  } while (true);
  return 0;
}

int for_break_path() {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  for (mut int index = 0; index < 1; index++) {
    char first = *iterator;
    value.push_back('!');
    break;
  }
  return 0;
}

int generic_switch<T>(T input, int selected) {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  switch (selected) {
    case 0:
      char first = *iterator;
      break;
    default:
      break;
  }
  value.push_back('!');
  return selected;
}

int main() {
  return switch_merge(0) + switch_arm_paths(0) +
         switch_without_default(1) + while_break_path(true) +
         while_break_entry(true) + do_break_path() + for_break_path() +
         generic_switch<int>(0, 0);
}
)";

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "switch-break-loan-flow.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected switch/break loan diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "switch exits and paths proven to terminate with break should end "
         "one retained loan before owner invalidation");

  const lang::FunctionDecl *switchFunction =
      findTopLevelFunction(frontend.program, "switch_merge");
  const auto *switchIterator =
      switchFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                switchFunction->body()->statements().at(1).get());
  const auto *switchStatement =
      switchFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::SwitchStmt *>(
                switchFunction->body()->statements().at(2).get());
  const lang::BindingInfo *switchBinding =
      switchIterator == nullptr
          ? nullptr
          : frontend.semantics.findBinding(*switchIterator);
  const lang::SemanticLoanInfo *switchLoan =
      switchBinding == nullptr
          ? nullptr
          : frontend.semantics.findLoan(switchBinding->retainedLoan);
  expect(switchStatement != nullptr &&
             hasLoanEndpoint(switchLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             switchStatement),
         "semantic loan flow should project a pre-existing carrier to the "
         "switch exit");

  const auto switchHirFunction =
      switchFunction == nullptr
          ? frontend.hir.functionInstances().end()
          : std::find_if(frontend.hir.functionInstances().begin(),
                         frontend.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == switchFunction;
                         });
  const lang::HirStatement *switchHirStatement = nullptr;
  if (switchHirFunction != frontend.hir.functionInstances().end()) {
    const auto found =
        std::find_if(switchHirFunction->body.statements.begin(),
                     switchHirFunction->body.statements.end(),
                     [&](const lang::HirStatement &statement) {
                       return statement.source == switchStatement;
                     });
    if (found != switchHirFunction->body.statements.end()) {
      switchHirStatement = &*found;
    }
  }
  expect(switchHirStatement != nullptr && switchBinding != nullptr &&
             std::find(switchHirStatement->endedLoans.begin(),
                       switchHirStatement->endedLoans.end(),
                       switchBinding->retainedLoan) !=
                 switchHirStatement->endedLoans.end(),
         "HIR should carry the semantic switch-exit proof");

  const lang::MirFunctionInstance *switchMirFunction =
      switchHirFunction == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(switchHirFunction->id);
  std::size_t reachableSwitchEnds = 0;
  if (switchMirFunction != nullptr && switchBinding != nullptr) {
    const auto loan = std::find_if(switchMirFunction->body.loans.begin(),
                                   switchMirFunction->body.loans.end(),
                                   [&](const lang::MirLoan &candidate) {
                                     return candidate.semanticLoan ==
                                            switchBinding->retainedLoan;
                                   });
    if (loan != switchMirFunction->body.loans.end()) {
      for (const lang::MirBlock &block : switchMirFunction->body.blocks) {
        if (!block.reachable) {
          continue;
        }
        reachableSwitchEnds += static_cast<std::size_t>(std::count_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == loan->id &&
                     instruction.hirStatement != 0;
            }));
      }
    }
  }
  expect(switchMirFunction != nullptr && reachableSwitchEnds == 2 &&
             lang::verifyMirBody(switchMirFunction->body).valid(),
         "MIR should normalize the retained loan on both switch exits before "
         "their CFG join");

  const lang::FunctionDecl *armFunction =
      findTopLevelFunction(frontend.program, "switch_arm_paths");
  const auto *armSwitch =
      armFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::SwitchStmt *>(
                armFunction->body()->statements().at(2).get());
  const auto *armIterator =
      armFunction == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                armFunction->body()->statements().at(1).get());
  const lang::BindingInfo *armBinding =
      armIterator == nullptr ? nullptr
                             : frontend.semantics.findBinding(*armIterator);
  const lang::SemanticLoanInfo *armLoan =
      armBinding == nullptr
          ? nullptr
          : frontend.semantics.findLoan(armBinding->retainedLoan);
  expect(
      armSwitch != nullptr && armLoan != nullptr &&
          std::any_of(armLoan->endpoints.begin(), armLoan->endpoints.end(),
                      [&](const lang::SemanticLoanEndpoint &endpoint) {
                        return endpoint.kind ==
                                   lang::SemanticLoanEndKind::SwitchArmEntry &&
                               endpoint.statement == armSwitch;
                      }),
      "a switch arm that does not use the carrier should end the loan at "
      "that arm's entry before mutation");

  const auto armHirFunction =
      armFunction == nullptr
          ? frontend.hir.functionInstances().end()
          : std::find_if(frontend.hir.functionInstances().begin(),
                         frontend.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == armFunction;
                         });
  const lang::HirStatement *armHirSwitch = nullptr;
  if (armHirFunction != frontend.hir.functionInstances().end()) {
    const auto found = std::find_if(armHirFunction->body.statements.begin(),
                                    armHirFunction->body.statements.end(),
                                    [&](const lang::HirStatement &statement) {
                                      return statement.source == armSwitch;
                                    });
    if (found != armHirFunction->body.statements.end()) {
      armHirSwitch = &*found;
    }
  }
  const lang::SemanticLoanId armLoanId =
      armBinding == nullptr ? 0 : armBinding->retainedLoan;
  expect(armHirSwitch != nullptr && !armHirSwitch->switchArms.empty() &&
             std::find(armHirSwitch->switchArms.front().entryEndedLoans.begin(),
                       armHirSwitch->switchArms.front().entryEndedLoans.end(),
                       armLoanId) !=
                 armHirSwitch->switchArms.front().entryEndedLoans.end(),
         "HIR should preserve the semantic loan ending at an unused switch "
         "arm's entry");

  for (const char *const name :
       {"switch_arm_paths", "switch_without_default", "while_break_path",
        "while_break_entry", "do_break_path", "for_break_path",
        "generic_switch"}) {
    const lang::FunctionDecl *function =
        findTopLevelFunction(frontend.program, name);
    const auto hir =
        function == nullptr
            ? frontend.hir.functionInstances().end()
            : std::find_if(frontend.hir.functionInstances().begin(),
                           frontend.hir.functionInstances().end(),
                           [&](const lang::HirFunctionInstance &candidate) {
                             return candidate.source == function;
                           });
    const lang::MirFunctionInstance *mir =
        hir == frontend.hir.functionInstances().end()
            ? nullptr
            : frontend.mir.findFunctionInstance(hir->id);
    expect(mir != nullptr && lang::verifyMirBody(mir->body).valid(),
           "every switch/break endpoint function should retain verifier-valid "
           "MIR: " +
               std::string{name});
  }

  const auto loanEndsBeforePushBack = [&](const std::string &name) {
    const lang::FunctionDecl *function =
        findTopLevelFunction(frontend.program, name);
    if (function == nullptr || function->body()->statements().size() < 2) {
      return false;
    }
    const auto *iterator = dynamic_cast<const lang::VariableDecl *>(
        function->body()->statements().at(1).get());
    const lang::BindingInfo *binding =
        iterator == nullptr ? nullptr
                            : frontend.semantics.findBinding(*iterator);
    if (binding == nullptr || binding->retainedLoan == 0) {
      return false;
    }
    const auto hir =
        std::find_if(frontend.hir.functionInstances().begin(),
                     frontend.hir.functionInstances().end(),
                     [&](const lang::HirFunctionInstance &candidate) {
                       return candidate.source == function;
                     });
    if (hir == frontend.hir.functionInstances().end()) {
      return false;
    }
    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(hir->id);
    if (mir == nullptr) {
      return false;
    }
    const auto loan =
        std::find_if(mir->body.loans.begin(), mir->body.loans.end(),
                     [&](const lang::MirLoan &candidate) {
                       return candidate.semanticLoan == binding->retainedLoan;
                     });
    if (loan == mir->body.loans.end()) {
      return false;
    }
    for (const lang::MirBlock &block : mir->body.blocks) {
      if (!block.reachable) {
        continue;
      }
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
            instruction.loan != loan->id) {
          continue;
        }
        for (std::size_t later = index + 1; later < block.instructions.size();
             ++later) {
          const lang::MirInstruction &candidate = block.instructions[later];
          if (candidate.kind != lang::MirInstructionKind::Call ||
              !candidate.functionTarget) {
            continue;
          }
          const lang::HirFunctionInstance *target =
              frontend.hir.findFunctionInstance(*candidate.functionTarget);
          if (target != nullptr && target->source != nullptr &&
              target->source->name().lexeme == "push_back") {
            return true;
          }
        }
      }
    }
    return false;
  };
  expect(loanEndsBeforePushBack("while_break_path") &&
             loanEndsBeforePushBack("do_break_path") &&
             loanEndsBeforePushBack("for_break_path"),
         "MIR should end each proven break-path loan before the invalidating "
         "push_back call");

  const lang::FrontendResult continuePath = lang::Frontend().analyze(
      "continue-loan-path.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; while (count < 2) "
      "{ char current = *iterator; value.push_back('!'); count++; continue; "
      "} return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!continuePath.canGenerateCode() &&
             hasDiagnosticCode(continuePath.diagnostics, "GTI-S2017"),
         "continue must keep a loop-carried loan active for the next "
         "backedge");

  const lang::FrontendResult separatedBreak = lang::Frontend().analyze(
      "separated-break-loan-path.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); while (true) { char current = "
      "*iterator; value.push_back('!'); int marker = 0; break; } return 0; "
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!separatedBreak.canGenerateCode() &&
             hasDiagnosticCode(separatedBreak.diagnostics, "GTI-S2017"),
         "an intervening statement must keep the immediate-break proof "
         "conservative");

  const lang::FrontendResult laterUse = lang::Frontend().analyze(
      "later-use-after-break.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); while (true) { char current = "
      "*iterator; value.push_back('!'); break; } char later = *iterator; "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!laterUse.canGenerateCode() &&
             hasDiagnosticCode(laterUse.diagnostics, "GTI-S2017"),
         "a use after the loop must prevent a premature break-path endpoint");

  const lang::FrontendResult nestedSwitchBreak = lang::Frontend().analyze(
      "switch-break-in-loop.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut int count = 0; while (count < 2) "
      "{ switch (count) { case 0: char current = *iterator; "
      "value.push_back('!'); break; default: break; } count++; } return 0; "
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!nestedSwitchBreak.canGenerateCode() &&
             hasDiagnosticCode(nestedSwitchBreak.diagnostics, "GTI-S2017"),
         "a switch break nested in a loop must not be mistaken for a "
         "terminating loop break");
}

void testSharedReadOnlyLoanAliases() {
  const std::string source = R"(
import std.string;

int sequential_aliases() {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  std::string::const_iterator& alias = iterator;
  std::string::const_iterator& second_alias = alias;
  char first = *iterator;
  char second = *alias;
  char third = *second_alias;
  value.push_back('!');
  return 0;
}

int conditional_aliases(bool use_original) {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  std::string::const_iterator& alias = iterator;
  if (use_original) {
    char first = *iterator;
  } else {
    char second = *alias;
  }
  value.push_back('!');
  return 0;
}

int loop_aliases() {
  mut std::string value = std::string("gti");
  mut auto iterator = value.begin();
  std::string::const_iterator& alias = iterator;
  mut int index = 0;
  while (index < 2) {
    if (index == 0) {
      char first = *iterator;
    } else {
      char second = *alias;
    }
    index++;
  }
  value.push_back('!');
  return 0;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "shared-read-only-aliases.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected shared-alias diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "shared read-only aliases should end after every carrier's final "
         "reachable use");

  const lang::FunctionDecl *sequential =
      findTopLevelFunction(frontend.program, "sequential_aliases");
  const auto *iterator =
      sequential == nullptr || sequential->body()->statements().size() < 7
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                sequential->body()->statements().at(1).get());
  const auto *alias =
      sequential == nullptr || sequential->body()->statements().size() < 7
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                sequential->body()->statements().at(2).get());
  const auto *secondAlias =
      sequential == nullptr || sequential->body()->statements().size() < 7
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                sequential->body()->statements().at(3).get());
  const lang::Stmt *lastSequentialUse =
      sequential == nullptr || sequential->body()->statements().size() < 7
          ? nullptr
          : sequential->body()->statements().at(6).get();
  const lang::BindingInfo *iteratorBinding =
      iterator == nullptr ? nullptr : frontend.semantics.findBinding(*iterator);
  const lang::BindingInfo *aliasBinding =
      alias == nullptr ? nullptr : frontend.semantics.findBinding(*alias);
  const lang::BindingInfo *secondAliasBinding =
      secondAlias == nullptr ? nullptr
                             : frontend.semantics.findBinding(*secondAlias);
  const lang::SemanticLoanId sharedLoan =
      iteratorBinding == nullptr ? 0 : iteratorBinding->retainedLoan;
  const lang::SemanticLoanInfo *semanticLoan =
      frontend.semantics.findLoan(sharedLoan);
  const lang::SemanticLoanInfo *shellLoan =
      aliasBinding == nullptr
          ? nullptr
          : frontend.semantics.findLoan(aliasBinding->retainedLoan);
  expect(sharedLoan != 0 && aliasBinding != nullptr && shellLoan != nullptr &&
             shellLoan->storedDependency == sharedLoan &&
             secondAliasBinding != nullptr &&
             secondAliasBinding->retainedLoan == shellLoan->id &&
             semanticLoan != nullptr &&
             semanticLoan->access == lang::AccessMode::ReadOnly &&
             semanticLoan->carriers.size() == 1 &&
             shellLoan->carriers.size() == 2 &&
             hasLoanEndpoint(semanticLoan,
                             lang::SemanticLoanEndKind::AfterStatement,
                             lastSequentialUse),
         "semantic analysis should distinguish the physical aliases from "
         "their shared external dependency and retain its aggregate final use");

  const auto sequentialHir =
      sequential == nullptr
          ? frontend.hir.functionInstances().end()
          : std::find_if(frontend.hir.functionInstances().begin(),
                         frontend.hir.functionInstances().end(),
                         [&](const lang::HirFunctionInstance &candidate) {
                           return candidate.source == sequential;
                         });
  const lang::HirStatement *hirLastUse = nullptr;
  if (sequentialHir != frontend.hir.functionInstances().end()) {
    const auto found =
        std::find_if(sequentialHir->body.statements.begin(),
                     sequentialHir->body.statements.end(),
                     [&](const lang::HirStatement &statement) {
                       return statement.source == lastSequentialUse;
                     });
    if (found != sequentialHir->body.statements.end()) {
      hirLastUse = &*found;
    }
  }
  expect(hirLastUse != nullptr &&
             std::find(hirLastUse->endedLoans.begin(),
                       hirLastUse->endedLoans.end(),
                       sharedLoan) != hirLastUse->endedLoans.end(),
         "HIR should preserve the frontend-selected shared-loan endpoint");

  const lang::MirFunctionInstance *sequentialMir =
      sequentialHir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(sequentialHir->id);
  const lang::MirLoan *mirLoan = nullptr;
  if (sequentialMir != nullptr) {
    const auto found = std::find_if(
        sequentialMir->body.loans.begin(), sequentialMir->body.loans.end(),
        [&](const lang::MirLoan &candidate) {
          return candidate.semanticLoan == sharedLoan;
        });
    if (found != sequentialMir->body.loans.end()) {
      mirLoan = &*found;
    }
  }
  std::size_t exactReferenceAddressSources = 0;
  if (sequentialMir != nullptr && mirLoan != nullptr) {
    for (const lang::MirBlock &block : sequentialMir->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Loan ||
            sequentialMir->body.findLoan(instruction.operands.front().loan) ==
                nullptr ||
            sequentialMir->body.findLoan(instruction.operands.front().loan)
                    ->storedDependency != mirLoan->id) {
          continue;
        }
        const lang::MirPlace *destination =
            sequentialMir->body.findPlace(*instruction.destination);
        const lang::MirPlace *address =
            sequentialMir->body.findPlace(instruction.operands.front().place);
        if (destination != nullptr && address != nullptr &&
            destination->type.kind == lang::SemanticType::Reference &&
            destination->type.arguments.size() == 1 &&
            instruction.operands.front().type ==
                destination->type.arguments.front() &&
            address->type == instruction.operands.front().type) {
          ++exactReferenceAddressSources;
        }
      }
    }
  }
  bool endsBeforeMutation = false;
  if (sequentialMir != nullptr && mirLoan != nullptr) {
    for (const lang::MirBlock &block : sequentialMir->body.blocks) {
      bool ended = false;
      for (const lang::MirInstruction &instruction : block.instructions) {
        ended =
            ended || (instruction.kind == lang::MirInstructionKind::EndBorrow &&
                      instruction.loan == mirLoan->id);
        if (!ended || instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.functionTarget) {
          continue;
        }
        const lang::HirFunctionInstance *target =
            frontend.hir.findFunctionInstance(*instruction.functionTarget);
        endsBeforeMutation = endsBeforeMutation ||
                             (target != nullptr && target->source != nullptr &&
                              target->source->name().lexeme == "push_back");
      }
    }
  }
  expect(
      sequentialMir != nullptr && mirLoan != nullptr &&
          mirLoan->carriers.size() == 1 && exactReferenceAddressSources == 2 &&
          endsBeforeMutation &&
          lang::verifyMirBody(sequentialMir->body).valid(),
      "MIR should retain an independent shared dependency, each alias's exact "
      "runtime address source, and the endpoint before owner mutation");

  if (sequentialMir != nullptr && mirLoan != nullptr) {
    lang::MirBody missingAddress = sequentialMir->body;
    bool mutated = false;
    for (lang::MirBlock &block : missingAddress.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (!mutated &&
            instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Loan &&
            missingAddress.findLoan(instruction.operands.front().loan) !=
                nullptr &&
            missingAddress.findLoan(instruction.operands.front().loan)
                    ->storedDependency == mirLoan->id) {
          instruction.operands.front().place = 0;
          mutated = true;
        }
      }
    }
    expect(mutated && !lang::verifyMirBody(missingAddress).valid(),
           "MIR verification should reject a reference initializer whose "
           "physical address source was removed");
  }

  for (const char *const name : {"conditional_aliases", "loop_aliases"}) {
    const lang::FunctionDecl *function =
        findTopLevelFunction(frontend.program, name);
    const auto hir =
        function == nullptr
            ? frontend.hir.functionInstances().end()
            : std::find_if(frontend.hir.functionInstances().begin(),
                           frontend.hir.functionInstances().end(),
                           [&](const lang::HirFunctionInstance &candidate) {
                             return candidate.source == function;
                           });
    const lang::MirFunctionInstance *mir =
        hir == frontend.hir.functionInstances().end()
            ? nullptr
            : frontend.mir.findFunctionInstance(hir->id);
    expect(mir != nullptr && lang::verifyMirBody(mir->body).valid(),
           "shared aliases should retain verifier-valid path-aware MIR in " +
               std::string{name});
  }

  const lang::FrontendResult laterUse = lang::Frontend().analyze(
      "shared-alias-later-use.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); std::string::const_iterator& "
      "alias = iterator; char first = *iterator; value.push_back('!'); "
      "char second = *alias; return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!laterUse.canGenerateCode() &&
             hasDiagnosticCode(laterUse.diagnostics, "GTI-S2017"),
         "a later use through any shared alias must keep the owner loan live");

  const lang::FrontendResult mutableAlias = lang::Frontend().analyze(
      "mutable-loan-alias.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut "
      "std::string::const_iterator& alias = iterator; return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(mutableAlias.canGenerateCode() &&
             lang::verifyMirProgram(mutableAlias.mir).valid(),
         "a mutable carrier shell reference is independent of read-only owner "
         "access");
  const auto mutableElement = lang::Frontend().analyze(
      "mutable-carrier-read-only-element.gti",
      "import std.string;\n"
      "int main() { mut std::string value = std::string(\"gti\"); mut "
      "auto iterator = value.begin(); mut "
      "std::string::const_iterator& alias = iterator; *alias = 'x'; "
      "return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !mutableElement.canGenerateCode() &&
          hasDiagnosticCode(mutableElement.diagnostics, "GTI-S2002") &&
          hasDiagnostic(mutableElement.diagnostics,
                        "Dereference assignment requires mutable access"),
      "mutable carrier access never upgrades the external read-only referent");
}

void testUniqueOwnershipAndAllocation() {
  const std::string source = R"(
struct Widget {
public:
  mut int value = 0;

  Widget(int initial) : value(initial) {}
  int read() { return this.value; }
  void increment() mut { this.value += 1; }
public:
  Widget() = default;
};

struct Holder {
  std::unique_ptr<Widget> widget = std::unique_ptr<Widget>();
public:
  Holder() = default;
};

int inspect(Widget& widget) { return widget.read(); }

std::unique_ptr<Widget> create(int value) {
  std::unique_ptr<Widget> widget = std::make_unique<Widget>(value);
  return std::move(widget);
}

int main() {
  mut std::unique_ptr<Widget> widget = create(4);
  expected<std::unique_ptr<Widget>, int> opened = create(6);
  Holder holder = Holder();
  std::unique_ptr<Widget> owners[1] = {std::make_unique<Widget>(9)};
  widget->increment();
  if (widget and widget != nullptr and inspect(*widget) == 5 and
      opened.has_value()) {
    return 0;
  }
  std::unique_ptr<Widget>& owner_reference = widget;
  return 1;
}
)";

  lang::FrontendResult frontend =
      analyzeWithMemory("unique-ownership.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected unique-owner diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(), "unique allocation, checked access, and "
                                     "explicit transfer should validate");

  const lang::FunctionDecl *create =
      findTopLevelFunction(frontend.program, "create");
  const auto *local = create == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                create->body()->statements().front().get());
  const lang::BindingInfo *binding =
      local == nullptr ? nullptr : frontend.semantics.findBinding(*local);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Class &&
             binding->traits.ownership == lang::OwnershipKind::Unique &&
             !binding->traits.copyable && binding->traits.movable &&
             binding->traits.drop == lang::DropKind::Lexical,
         "allocated owners should retain move-only lexical ownership metadata");

  bool foundOwningExpectedObligation = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (!entryFunction(frontend, function.id))
      continue;
    foundOwningExpectedObligation =
        foundOwningExpectedObligation ||
        std::any_of(function.body.dropObligations.begin(),
                    function.body.dropObligations.end(),
                    [&](const lang::HirDropObligation &obligation) {
                      return isExpectedClass(frontend,
                                             obligation.dropType.type) &&
                             obligation.dropType.requiresActiveCleanup;
                    });
  }
  expect(foundOwningExpectedObligation,
         "an expected value containing a unique owner should retain its "
         "recursive active-cleanup obligation");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  std::size_t scheduledNullOwnerFields = 0;
  for (const auto &constructor : frontend.hir.constructorInstances()) {
    const auto owner = std::find_if(
        frontend.hir.classInstances().begin(),
        frontend.hir.classInstances().end(),
        [&](const auto &item) { return item.id == constructor.owner; });
    if (!binding || owner == frontend.hir.classInstances().end() ||
        owner->type != binding->type || !constructor.generatedDefault)
      continue;
    for (const auto &initializer : constructor.initializers) {
      const auto *value =
          initializer.arguments.size() == 1
              ? constructor.body.findValue(initializer.arguments.front())
              : nullptr;
      scheduledNullOwnerFields +=
          initializer.targetType.kind == lang::SemanticType::UniqueOwner &&
          value && value->info.type == lang::SemanticType::NullPtr;
    }
  }
  expect(artifact.contents.find("class unique_ptr") != std::string::npos &&
             artifact.contents.find("std::unique_ptr<T> owner;") !=
                 std::string::npos &&
             artifact.contents.find("std::unique_ptr<T> owner = nullptr") ==
                 std::string::npos &&
             scheduledNullOwnerFields == 1 &&
             artifact.contents.find(
                 "__gti_std::unique_ptr<::__gti_program::Widget>") !=
                 std::string::npos,
         "the nominal unique_ptr wrapper should initialize its empty owner "
         "through one selected schedule, not a duplicate native default");
  expect(
      artifact.contents.find(
          "::gti_internal::backend::make_unique<::__gti_program::Widget>(") ==
              std::string::npos &&
          artifact.contents.find(
              "mir_object_allocation_v1<::__gti_program::Widget>") !=
              std::string::npos &&
          artifact.contents.find("__gti_std::__gti_fn_") != std::string::npos &&
          // The migrated caller emits from verified MIR and selects the
          // concrete transformed factory sibling. Its out-result type carries
          // the otherwise non-deducible owner argument.
          artifact.contents.find("_make_unique__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1") !=
              std::string::npos &&
          artifact.contents.find(
              "::gti_internal::backend::owner_access((*this).owner)") ==
              std::string::npos &&
          artifact.contents.find("unique_owner_is_null") != std::string::npos &&
          artifact.contents.find("unique_owner_has_value") == std::string::npos,
      "the public factory should remain ordinary GTI while its concrete "
      "verified-MIR sibling lowers only narrow owner capabilities");
  expect(
      // The factory's verified-MIR sibling publishes the owner by move
      // into the caller's receiving local.
      artifact.contents.find(
          "construct_representation_at(__gti_mir_out_result, "
          "std::move(__gti_mir_p_2.get()))") != std::string::npos &&
          artifact.contents.find(
              "unique_ptr(const unique_ptr &other) = delete") !=
              std::string::npos &&
          artifact.contents.find("unique_ptr(unique_ptr &&other) noexcept : "
                                 "owner(std::move(other.owner)) {\n      }") !=
              std::string::npos &&
          artifact.contents.find("unique_ptr() noexcept {}") !=
              std::string::npos &&
          artifact.contents.find("~unique_ptr() noexcept {}") !=
              std::string::npos &&
          artifact.contents.find(
              "const __gti_std::unique_ptr<Widget> widget =") ==
              std::string::npos,
      "nominal unique owners should remain move-only and physically movable");

  const lang::FrontendResult nullState =
      analyzeWithMemory("unique-owner-null-state.gti", R"(
struct Widget {
public:
  Widget() {}
};

class Nullable {
  mut bool empty = false;

public:
  Nullable(nullptr_t value) : empty(true) {}
  void operator=(nullptr_t value) mut { this.empty = true; }
  bool is_empty() { return this.empty; }
public:
  Nullable() = default;
};

void consume(std::unique_ptr<Widget> value) {}

int main() {
  mut Nullable nullable = nullptr;
  nullable = nullptr;
  if (!nullable.is_empty()) { return 1; }

  mut std::unique_ptr<Widget> owner = nullptr;
  owner.reset();
  owner = nullptr;
  owner = std::make_unique<Widget>();
  owner = nullptr;
  return 0;
}
)");
  if (!nullState.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : nullState.diagnostics) {
      std::cerr << "Unexpected unique-owner null-state diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(nullState.canGenerateCode() &&
             lang::verifyMirProgram(nullState.mir).valid(),
         "empty unique-owner construction, repeated reset, nullptr "
         "assignment, and later replacement should validate through MIR");

  const lang::FunctionDecl *nullMain =
      findTopLevelFunction(nullState.program, "main");
  const lang::VariableDecl *emptyOwner = nullptr;
  const lang::Assign *nullAssignment = nullptr;
  if (nullMain != nullptr) {
    for (const lang::StmtPtr &statement : nullMain->body()->statements()) {
      if (const auto *variable =
              dynamic_cast<const lang::VariableDecl *>(statement.get());
          variable != nullptr && variable->name().lexeme == "owner") {
        emptyOwner = variable;
      }
      const auto *expression =
          dynamic_cast<const lang::ExpressionStmt *>(statement.get());
      const auto *assignment = expression == nullptr
                                   ? nullptr
                                   : dynamic_cast<const lang::Assign *>(
                                         expression->expression().get());
      if (assignment != nullptr && assignment->name().lexeme == "owner" &&
          dynamic_cast<const lang::LiteralExpr *>(assignment->value().get()) !=
              nullptr) {
        nullAssignment = assignment;
      }
    }
  }
  const lang::ResolvedConstructionInfo *emptyConstruction =
      emptyOwner == nullptr || !emptyOwner->initializer()
          ? nullptr
          : nullState.semantics.findConstruction(*emptyOwner->initializer());
  const lang::ResolvedOperatorInfo *assignmentResolution =
      nullAssignment == nullptr
          ? nullptr
          : nullState.semantics.findOperator(*nullAssignment);
  expect(emptyConstruction != nullptr &&
             emptyConstruction->parameterTypes ==
                 std::vector<lang::SemanticType>{lang::SemanticType::NullPtr} &&
             assignmentResolution != nullptr &&
             assignmentResolution->kind ==
                 lang::OverloadedOperator::Assignment &&
             assignmentResolution->parameterTypes ==
                 std::vector<lang::SemanticType>{lang::SemanticType::NullPtr},
         "nullptr initialization and assignment should retain their exact "
         "source-defined constructor and operator identities");

  bool foundNullConstruction = false;
  bool foundAssignmentCall = false;
  for (const lang::HirFunctionInstance &function :
       nullState.hir.functionInstances()) {
    if (function.source == nullptr ||
        function.source->name().lexeme != "main") {
      continue;
    }
    for (const lang::HirValue &value : function.body.values) {
      foundNullConstruction =
          foundNullConstruction ||
          (value.kind == lang::HirValueKind::DirectInitializer &&
           value.constructorTarget.has_value() && value.operands.size() == 1 &&
           value.parameterTypes ==
               std::vector<lang::SemanticType>{lang::SemanticType::NullPtr});
      if (value.kind != lang::HirValueKind::Call || !value.functionTarget) {
        continue;
      }
      const lang::HirFunctionInstance *target =
          nullState.hir.findFunctionInstance(*value.functionTarget);
      foundAssignmentCall = foundAssignmentCall ||
                            (target != nullptr && target->source != nullptr &&
                             target->source->operatorName() &&
                             target->source->operatorName()->kind ==
                                 lang::OverloadedOperator::Assignment);
    }
  }
  bool foundMirAssignmentCall = false;
  for (const lang::MirFunctionInstance &function :
       nullState.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::MirFunctionInstance *target =
            instruction.functionTarget ? nullState.mir.findFunctionInstance(
                                             *instruction.functionTarget)
                                       : nullptr;
        foundMirAssignmentCall =
            foundMirAssignmentCall ||
            (instruction.kind == lang::MirInstructionKind::Call &&
             target != nullptr &&
             target->overloadedOperator ==
                 lang::OverloadedOperator::Assignment);
      }
    }
  }
  expect(foundNullConstruction && foundAssignmentCall && foundMirAssignmentCall,
         "typed HIR and MIR should retain null construction and exact "
         "assignment calls instead of treating nullptr as a class value");

  const lang::OptimizationResult nullOptimizations =
      lang::OptimizationPipeline().run(nullState.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact nullArtifact = gti_test::emitCpp(
      nullState, nullState.mir, nullState.mir, nullOptimizations);
  expect(
      nullArtifact.contents.find(
          "explicit unique_ptr(const std::nullptr_t empty);") !=
              std::string::npos &&
          nullArtifact.contents.find(
              "unique_ptr<::__gti_program::Widget>::unique_ptr(std::nullptr_t "
              "__gti_mir_arg_0) : owner(::gti_internal::backend::"
              "mir_empty_state_v1<std::remove_cv_t<decltype(owner)>>()) {") !=
              std::string::npos &&
          // Resetting to null cannot fail for this owner, so the selected
          // assignment and its reset call keep their exact plain identities.
          nullArtifact.contents.find(
              "unique_ptr<::__gti_program::Widget>::operator=(std::nullptr_t "
              "__gti_mir_arg_0)") != std::string::npos &&
          containsMangledFunction(nullArtifact.contents, "::", "reset", "();"),
      "the C++ backend should retain source-defined null construction and "
      "assignment identities without a public-type special case");

  const lang::FrontendResult rejectedNullArgument =
      analyzeWithMemory("unique-owner-null-argument.gti", R"(
struct Widget { public: Widget() = default; };
void consume(std::unique_ptr<Widget> value) {}
int main() {
  consume(nullptr);
  return 0;
}
)");
  expect(!rejectedNullArgument.canGenerateCode() &&
             countDiagnosticCode(rejectedNullArgument.diagnostics,
                                 "GTI-S2003") == 1 &&
             hasDiagnostic(rejectedNullArgument.diagnostics,
                           "has type 'nullptr_t' but the parameter requires "
                           "'std::unique_ptr<Widget>'"),
         "target-typed nullptr construction must not become an implicit "
         "class conversion for ordinary call arguments");

  const lang::FrontendResult rejectedPointerNullConstructor =
      lang::Frontend().analyze("pointer-null-constructor.gti", R"(
class PointerOnly {
public:
  PointerOnly(int* value) {}
public:
  PointerOnly() = default;
};
int main() {
  PointerOnly value = nullptr;
  return 0;
}
)");
  expect(!rejectedPointerNullConstructor.canGenerateCode() &&
             countDiagnosticCode(rejectedPointerNullConstructor.diagnostics,
                                 "GTI-S2012") == 1,
         "target-typed nullptr construction should require an exact "
         "nullptr_t constructor rather than a raw-pointer overload");

  const lang::FrontendResult rejectedPointerNullAssignment =
      lang::Frontend().analyze("pointer-null-assignment.gti", R"(
class PointerOnly {
public:
  PointerOnly() {}
  void operator=(int* value) mut {}
};
int main() {
  mut PointerOnly value = PointerOnly();
  value = nullptr;
  return 0;
}
)");
  expect(!rejectedPointerNullAssignment.canGenerateCode() &&
             countDiagnosticCode(rejectedPointerNullAssignment.diagnostics,
                                 "GTI-S2003") == 1,
         "nullptr assignment should require an exact nullptr_t operator "
         "rather than a raw-pointer overload");

  const lang::FrontendResult invalid =
      analyzeWithMemory("invalid-unique-ownership.gti", R"(
struct Widget {
public:
  int value = 0;
  Widget() {}
  int read() { return this.value; }
};

void consume(std::unique_ptr<Widget> widget) {}

std::unique_ptr<Widget> return_copy(std::unique_ptr<Widget> widget) {
  return widget;
}

std::unique_ptr<Widget> global_owner = std::unique_ptr<Widget>();
std::unique_ptr<Widget> global_owners[1] = {
    std::unique_ptr<Widget>()};

T identity<T>(T value) { return value; }

int main() {
  mut std::unique_ptr<Widget> owner = std::make_unique<Widget>();
  std::unique_ptr<Widget> copied = owner;
  consume(owner);
  std::unique_ptr<Widget> moved = std::move(owner);
  int after_move = owner->read();

  mut std::unique_ptr<Widget> conditional = std::make_unique<Widget>();
  if (true) {
    consume(std::move(conditional));
  }
  int maybe_moved = conditional->read();

  [[discard]] std::move(std::make_unique<Widget>());
  int wrong_member = moved.value;
  mut std::unique_ptr<Widget> missing;
  std::unique_ptr<Widget> generic = identity(std::move(moved));
  return after_move + maybe_moved + wrong_member;
}
)");
  expect(!invalid.canGenerateCode(),
         "copies, invalid global storage, and unsafe unique-owner use should "
         "fail");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "Unique owners can only be local bindings") &&
          hasDiagnostic(invalid.diagnostics, "would copy a unique owner") &&
          hasDiagnostic(invalid.diagnostics, "already been moved") &&
          hasDiagnostic(invalid.diagnostics,
                        "temporaries are already values") &&
          hasDiagnostic(invalid.diagnostics,
                        "Unknown member 'value' on 'unique_ptr'") &&
          hasDiagnostic(invalid.diagnostics, "require explicit construction"),
      "unique-owner diagnostics should cover transfer, flow, and surface "
      "limits");

  const lang::FrontendResult invalidPointee =
      analyzeWithMemory("invalid-unique-pointee.gti", R"(
int main() {
  auto unsupported = std::make_unique<std::string_view>("view");
  return 0;
}
)");
  expect(!invalidPointee.canGenerateCode() &&
             hasDiagnostic(
                 invalidPointee.diagnostics,
                 "requires a class, struct, scalar, or generic object type") &&
             hasRelatedDiagnostic(invalidPointee.diagnostics,
                                  "Concrete generic instance requested here"),
         "ordinary stdlib generic factories should diagnose invalid nested "
         "capability use at their concrete instantiation site");

  const lang::FrontendResult invalidConstructor =
      analyzeWithMemory("invalid-unique-constructor.gti", R"(
struct Widget {
public:
  Widget(int value) {}
public:
  Widget() = default;
};

int main() {
  auto owner = std::make_unique<Widget>(true);
  return 0;
}
)");
  expect(
      !invalidConstructor.canGenerateCode() &&
          hasDiagnostic(invalidConstructor.diagnostics,
                        "No constructor of 'Widget' exactly matches argument "
                        "types (bool)") &&
          hasRelatedDiagnostic(invalidConstructor.diagnostics,
                               "Concrete generic instance requested here"),
      "concrete forwarding through an ordinary stdlib factory should retain "
      "frontend constructor checking");

  const lang::FrontendResult invalidCapabilities =
      analyzeTrustedMemoryFixture("invalid-owner-capabilities.gti", R"(
struct Widget {
public:
  int read() { return 0; }
public:
  Widget() = default;
};

int misuse_internal_owner() {
  gti_internal::unique_owner<Widget> owner =
      gti_internal::allocate_unique_owner<Widget>();
  mut Widget& writable =
      gti_internal::unique_owner_borrow_mut(owner);
  Widget& dangling = gti_internal::unique_owner_borrow(
      gti_internal::allocate_unique_owner<Widget>());
  return (*owner).read();
}
)");
  expect(!invalidCapabilities.canGenerateCode() &&
             hasDiagnostic(invalidCapabilities.diagnostics,
                           "requires mutable unique-owner storage") &&
             hasDiagnostic(invalidCapabilities.diagnostics,
                           "requires stable unique-owner storage") &&
             hasDiagnostic(invalidCapabilities.diagnostics,
                           "Dereference requires a class defining operator*"),
         "compiler-private owner capabilities should preserve their checked "
         "borrow boundary");

  const lang::FrontendResult hiddenOwnerPolicy =
      analyzeTrustedMemoryFixture("hidden-owner-policy.gti", R"(
struct Widget { public: Widget() = default; };

int main() {
  gti_internal::unique_owner<Widget> owner =
      gti_internal::allocate_unique_owner<Widget>();
  bool engaged = gti_internal::unique_owner_has_value(owner);
  return 0;
}
)");
  expect(!hiddenOwnerPolicy.canGenerateCode() &&
             hasDiagnostic(hiddenOwnerPolicy.diagnostics,
                           "Undefined qualified name "
                           "'gti_internal::unique_owner_has_value'"),
         "the intrinsic owner should expose null representation rather than a "
         "stdlib-shaped has_value policy query");

  const std::string formatted = lang::Formatter().format(
      "std::unique_ptr<Widget> make(){return std::make_unique<Widget>();}"
      "int read(mut std::unique_ptr<Widget> value){return value->read();}"
      "class Nullable{public:void operator=(nullptr_t empty)mut{} public: "
      "Nullable() = default; };");
  expect(formatted.find("value->read()") != std::string::npos &&
             formatted.find("void operator=(nullptr_t empty) mut {") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "unique-owner and exact assignment spelling should format "
         "idempotently");
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

  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  expect(artifact.contents.find(".swap(") != std::string::npos,
         "the backend should lower unique-owner swap to native unique_ptr "
         "swap without replacement cleanup");

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

  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  const std::unordered_set<std::string> upcastMembers =
      concreteBackendSymbols(artifact.contents, "__gti_member_", "upcast");
  const auto hasSelectedNativeOwnerSpecialization =
      [&](std::string_view owner) {
        const std::string prefix =
            "template <> bool "
            "__gti_program::__gti_std::unique_ptr<::__gti_program::" +
            std::string(owner) + ">::";
        const std::size_t declaration = artifact.contents.find(prefix);
        if (declaration == std::string::npos) {
          return false;
        }
        const auto member =
            std::find_if(upcastMembers.begin(), upcastMembers.end(),
                         [&](const std::string &candidate) {
                           return artifact.contents.find(
                                      prefix + candidate + "__gti_mir_failure(",
                                      declaration) == declaration;
                         });
        if (member == upcastMembers.end()) {
          return false;
        }
        const std::string specialization =
            prefix + *member + "__gti_mir_failure(";
        return artifact.contents.find(specialization,
                                      declaration + specialization.size()) !=
               std::string::npos;
      };
  expect(artifact.contents.find("unique_owner_upcast<") != std::string::npos &&
             artifact.contents.find("virtual ~Layer() noexcept = default;") !=
                 std::string::npos &&
             upcastMembers.size() == 2 &&
             hasSelectedNativeOwnerSpecialization("DemoLayer") &&
             hasSelectedNativeOwnerSpecialization("OtherLayer"),
         "the backend should lower the private owner conversion and preserve "
         "polymorphic destruction while declaring a selected method on its "
         "native generic owner before use");

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

void testSourceDefinedStorage() {
  const std::string source = R"(
concept buffer_element<T> =
    std::storage_element<T> && std::movable<T> && std::relocatable<T>;
class Buffer<buffer_element T> {
  mut std::storage<T> data;
  mut uint64_t count = 0;
  mut uint64_t reserved = 0;

public:
  Buffer(uint64_t capacity)
      : data(fixture_storage<T>(capacity)), reserved(capacity) {}

  uint64_t capacity() { return this.reserved; }

  void push(T value) mut {
    this.data.construct(this.count, std::move(value));
    this.count++;
  }

  T& at(uint64_t index) { return this.data.get(index); }

  void grow(uint64_t capacity) mut {
    mut std::storage<T> replacement = fixture_storage<T>(capacity);
    this.data.relocate_to(replacement, this.count);
    this.data = std::move(replacement);
    this.reserved = capacity;
  }

  void pop() mut {
    this.count--;
    this.data.destroy(this.count);
  }
};

int main() {
  mut Buffer<int> values = Buffer<int>(uint64_t(2));
  mut Buffer<char> characters = Buffer<char>(uint64_t(1));
  values.push(7);
  values.push(9);
  characters.push('G');
  values.grow(uint64_t(4));
  if (values.capacity() == 4 and values.at(uint64_t(0)) == 7 and
      values.at(uint64_t(1)) == 9 and characters.at(uint64_t(0)) == 'G') {
    values.pop();
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      analyzeSourceStorageFixture("source-storage.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics)
      std::cerr << "Unexpected source-storage diagnostic: "
                << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(), "source-defined storage supports growth, "
                                     "element lifetimes and receiver borrows");
  if (!frontend.canGenerateCode())
    return;

  const lang::ClassDecl *buffer = findTopLevelClass(frontend.program, "Buffer");
  const auto *field = buffer == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                buffer->members().front().get());
  const lang::BindingInfo *binding =
      field == nullptr ? nullptr : frontend.semantics.findBinding(*field);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Class &&
             binding->traits.ownership == lang::OwnershipKind::Value &&
             binding->traits.drop == lang::DropKind::Lexical &&
             !binding->traits.copyable && binding->traits.movable &&
             !binding->traits.containsBorrowedState,
         "source storage is a noncopyable, movable lexical value, not a "
         "compiler-private unique-owner kind");

  bool construct = false, destroy = false, relocate = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        construct |=
            instruction.intrinsic == lang::IntrinsicKind::ObjectConstruct;
        destroy |= instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy;
        relocate |=
            instruction.intrinsic == lang::IntrinsicKind::ObjectRelocate;
      }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      construct && destroy && relocate,
      "source storage uses the general construct/destroy/relocate operations");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "source storage object lifetimes have independently verified MIR");
  const auto buffers =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "Buffer");
  expect(buffers.size() == 2,
         "source storage emits both ordinary Buffer instances: " +
             std::to_string(buffers.size()));
  const auto storageInstances = std::count_if(
      frontend.hir.classInstances().begin(),
      frontend.hir.classInstances().end(), [](const auto &instance) {
        return instance.source && instance.source->name().lexeme == "storage";
      });
  // Adding source iterator aliases makes storage's definition eligible for
  // ordinary template representation. Concrete GTI instances remain typed and
  // verified; digest-named C++ classes are not the language's layout contract.
  expect(
      storageInstances >= 2 &&
          (concreteBackendSymbols(artifact.contents, "__gti_type_", "storage")
                   .size() >= 2 ||
           (artifact.contents.find("class storage {") != std::string::npos &&
            artifact.contents.find("::__gti_std::storage<std::int32_t>") !=
                std::string::npos &&
            artifact.contents.find("::__gti_std::storage<char8_t>") !=
                std::string::npos)),
      "source storage emits an ordinary class/template with both typed "
      "storage instances");
  expect(artifact.contents.find("construct_representation_at(") !=
             std::string::npos,
         "source storage emits general object construction");
  expect(artifact.contents.find("::gti_internal::backend::storage<") ==
                 std::string::npos &&
             artifact.contents.find("mir_storage_") == std::string::npos &&
             artifact.contents.find("mir_allocate_storage_v1") ==
                 std::string::npos,
         "source storage never uses the retired sparse backend");

  const lang::FrontendResult invalid =
      analyzeSourceStorageFixture("invalid-source-storage.gti", R"(
int main() {
  mut std::storage<int> values = fixture_storage<int>(uint64_t(2));
  values.construct(true, 1);
  values.construct(uint64_t(0), true);
  std::storage<int> copied = values;
  std::storage<int> moved = std::move(values);
  return values.get(uint64_t(0));
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics, "Cannot initialize 'copied'") &&
             hasDiagnostic(invalid.diagnostics, "has already been moved") &&
             hasDiagnostic(invalid.diagnostics, "bool"),
         "public storage rejects wrong index/element types, copies and reuse "
         "after movement");

  // Private placement restrictions do not apply nominally to source owners.
  // Keep the retained unique-owner capability's separate boundary regression.
  const auto invalidPrivatePlacement =
      analyzeTrustedMemoryFixture("invalid-unique-owner-placement.gti", R"(
gti_internal::unique_owner<int> global =
    gti_internal::allocate_unique_owner<int>(1);
int main() { return 0; }
)");
  expect(!invalidPrivatePlacement.canGenerateCode() &&
             hasDiagnostic(
                 invalidPrivatePlacement.diagnostics,
                 "only be local bindings, function values, or class fields"),
         "compiler-private unique ownership retains its placement boundary");

  const lang::FrontendResult recoverableStorage =
      analyzeWithMemory("recoverable-source-storage.gti", R"(
int main() {
  std::allocator allocator{};
  auto allocated = allocator.try_storage<int>(uint64_t(2), std::alignment(uint64_t(64)));
  if (!allocated) { return 3; }
  mut auto values = std::move(allocated).value();
  if (values.capacity() != uint64_t(2) || values.initialized(uint64_t(1))) {
    return 1;
  }
  values.construct(uint64_t(1), 42);
  if (!values.initialized(uint64_t(1)) || values.initialized(uint64_t(0))) {
    return 2;
  }
  return values.get(uint64_t(1)) - 42;
}
)");
  std::array<bool, 3> hirOperations{};
  std::array<bool, 3> mirOperations{};
  for (const lang::HirFunctionInstance &function :
       recoverableStorage.hir.functionInstances()) {
    if (function.source == nullptr)
      continue;
    const auto &name = function.source->name().lexeme;
    const auto *mir = recoverableStorage.mir.findFunctionInstance(function.id);
    std::optional<std::size_t> index;
    if (name == "try_storage" &&
        isExpectedClass(recoverableStorage, function.returnType))
      index = 0;
    else if (name == "capacity" &&
             function.returnType == lang::SemanticType::UInt64)
      index = 1;
    else if (name == "initialized" &&
             function.returnType == lang::SemanticType::Bool)
      index = 2;
    if (index) {
      hirOperations[*index] = true;
      mirOperations[*index] =
          mir != nullptr && mir->returnType == function.returnType;
    }
  }
  const lang::OptimizationResult recoverableOptimizations =
      lang::OptimizationPipeline().run(recoverableStorage.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact recoverableArtifact =
      gti_test::emitCpp(recoverableStorage, recoverableStorage.mir,
                        recoverableStorage.mir, recoverableOptimizations);
  expect(recoverableStorage.canGenerateCode() &&
             lang::verifyMirProgram(recoverableStorage.mir).valid() &&
             std::all_of(hirOperations.begin(), hirOperations.end(),
                         [](bool found) { return found; }) &&
             std::all_of(mirOperations.begin(), mirOperations.end(),
                         [](bool found) { return found; }) &&
             recoverableArtifact.contents.find(
                 "construct_representation_at(") != std::string::npos &&
             recoverableArtifact.contents.find("storage_try_allocate(") ==
                 std::string::npos &&
             recoverableArtifact.contents.find("storage_initialized(") ==
                 std::string::npos,
         "recoverable allocation and sparse-state observers retain ordinary "
         "source identities and typed results through HIR/MIR");

  const lang::FrontendResult invalidRecoverableStorage =
      analyzeSourceStorageFixture("invalid-recoverable-source-storage.gti", R"(
int main() {
  std::allocator allocator{};
  auto fixed = fixture_storage<int>(uint64_t(1));
  fixed.construct(uint64_t(0), 1);
  auto wrong_count = allocator.try_storage<int>(true);
  auto extra_alignment = allocator.try_storage<int>(uint64_t(1), std::alignment(uint64_t(8)), true);
  mut auto values = fixture_storage<int>(uint64_t(1));
  uint64_t extra_capacity = values.capacity(uint64_t(0));
  bool wrong_index = values.initialized(true);
  return 0;
}
)");
  expect(!invalidRecoverableStorage.canGenerateCode() &&
             hasDiagnostic(invalidRecoverableStorage.diagnostics,
                           "Mutable method requires a mutable receiver") &&
             hasDiagnostic(invalidRecoverableStorage.diagnostics,
                           "No overload of 'try_storage' exactly matches "
                           "argument types (bool)") &&
             hasDiagnostic(invalidRecoverableStorage.diagnostics,
                           "No overload of 'try_storage' exactly matches "
                           "argument types (uint64_t, std::alignment, bool)") &&
             hasDiagnostic(invalidRecoverableStorage.diagnostics,
                           "Function expects 0 arguments but received 1") &&
             hasDiagnostic(invalidRecoverableStorage.diagnostics,
                           "Argument 1 has type 'bool' but the parameter "
                           "requires 'uint64_t'"),
         "ordinary method diagnostics preserve mutable access, scalar types "
         "and arity checks");

  const lang::FrontendResult publicStorage = lang::Frontend().analyze(
      "public-storage.gti", R"(
import std.memory;

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated = allocator.try_storage<int32_t>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<int32_t> values = std::move(allocated).value();
  values.construct(std::size_t(0), int32_t(42));
  int32_t observed = values.get(std::size_t(0));
  mut auto arena = std::try_make_arena<int32_t>(std::size_t(1));
  mut auto pool = std::try_make_pool<int32_t>(std::size_t(1));
  if (!arena || !pool) { return 2; }
  return observed - 42;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  bool ordinaryStorageFactory = false;
  bool ordinaryArenaFactory = false;
  bool ordinaryPoolFactory = false;
  for (const lang::HirFunctionInstance &function :
       publicStorage.hir.functionInstances()) {
    if (function.source == nullptr ||
        function.source->name().lexeme != "main") {
      continue;
    }
    for (const lang::HirValue &value : function.body.values) {
      const lang::HirFunctionInstance *target =
          value.functionTarget
              ? publicStorage.hir.findFunctionInstance(*value.functionTarget)
              : nullptr;
      if (target == nullptr || target->source == nullptr ||
          value.intrinsic != lang::IntrinsicKind::None) {
        continue;
      }
      ordinaryStorageFactory = ordinaryStorageFactory ||
                               target->source->name().lexeme == "try_storage";
      ordinaryArenaFactory = ordinaryArenaFactory ||
                             target->source->name().lexeme == "try_make_arena";
      ordinaryPoolFactory = ordinaryPoolFactory ||
                            target->source->name().lexeme == "try_make_pool";
    }
  }
  expect(
      publicStorage.canGenerateCode() && ordinaryStorageFactory &&
          ordinaryArenaFactory && ordinaryPoolFactory,
      "std allocator, storage, arena, and pool policy should remain ordinary "
      "source-defined calls while only private implementation calls are "
      "intrinsic");

  const lang::FrontendResult checkedConstructorStorage =
      lang::Frontend().analyze("checked-constructor-storage.gti", R"(
import std.memory;

class checked_value {
  mut int32_t stored;

public:
  checked_value(int32_t value) : stored(value) {
    this.stored = value + int32_t(0);
  }

  checked_value(checked_value& other) = delete;
  checked_value(checked_value&& other) = default;
};

int main() {
  std::allocator allocator = std::allocator();
  mut auto allocated =
      allocator.try_storage<checked_value>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<checked_value> values =
      std::move(allocated).value();
  values.construct(std::size_t(0), checked_value(int32_t(7)));
  return 0;
}
)",
                               {standardLibraryPrelude()}, {},
                               {standardLibraryRoot()});
  const lang::BackendArtifact checkedConstructorArtifact =
      checkedConstructorStorage.canGenerateCode()
          ? gti_test::emitCpp(
                checkedConstructorStorage, checkedConstructorStorage.mir,
                checkedConstructorStorage.mir,
                lang::OptimizationPipeline().run(checkedConstructorStorage.hir,
                                                 lang::OptimizationLevel::O0))
          : lang::BackendArtifact{};
  const lang::MirInstruction *checkedConstruction = nullptr;
  const lang::MirConstructorInstance *checkedTarget = nullptr;
  for (const lang::MirFunctionInstance &function :
       checkedConstructorStorage.mir.functionInstances()) {
    const lang::HirFunctionInstance *hirFunction =
        checkedConstructorStorage.hir.findFunctionInstance(function.id);
    if (hirFunction == nullptr || hirFunction->source == nullptr ||
        hirFunction->source->name().lexeme != "main") {
      continue;
    }
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        const lang::MirConstructorInstance *target =
            instruction.constructorTarget
                ? checkedConstructorStorage.mir.findConstructorInstance(
                      *instruction.constructorTarget)
                : nullptr;
        const lang::HirConstructorInstance *hirTarget =
            target == nullptr
                ? nullptr
                : checkedConstructorStorage.hir.findConstructorInstance(
                      target->id);
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.constructorKind == lang::ConstructorKind::Ordinary &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Constructor &&
            hirTarget != nullptr && hirTarget->source != nullptr &&
            hirTarget->source->name().lexeme == "checked_value") {
          checkedConstruction = &instruction;
          checkedTarget = target;
        }
      }
    }
  }
  const std::string checkedSuccess =
      checkedConstruction == nullptr
          ? ""
          : "__gti_mir_construct_success_" +
                std::to_string(checkedConstruction->id);
  const std::string checkedTag =
      checkedTarget == nullptr ? ""
                               : "mir_failure_constructor_tag_v1<" +
                                     std::to_string(checkedTarget->id) + ">{}";
  bool checkedInvoke = false;
  if (checkedConstruction != nullptr) {
    for (const lang::MirFunctionInstance &function :
         checkedConstructorStorage.mir.functionInstances()) {
      checkedInvoke =
          checkedInvoke ||
          std::any_of(function.body.blocks.begin(), function.body.blocks.end(),
                      [&](const lang::MirBlock &block) {
                        return block.terminator.kind ==
                                   lang::MirTerminatorKind::Invoke &&
                               block.terminator.invokeInstruction ==
                                   checkedConstruction->id;
                      });
    }
  }
  expect(
      checkedConstructorStorage.canGenerateCode() &&
          lang::verifyMirProgram(checkedConstructorStorage.mir).valid() &&
          checkedConstruction != nullptr && checkedTarget != nullptr &&
          checkedInvoke &&
          checkedConstruction->definedFailure.propagation ==
              lang::FailurePropagationKind::Constructor &&
          checkedConstructorArtifact.contents.find(
              "bool " + checkedSuccess + " = false;") != std::string::npos &&
          checkedConstructorArtifact.contents.find(checkedTag) !=
              std::string::npos &&
          checkedConstructorArtifact.contents.find(", &" + checkedSuccess) !=
              std::string::npos &&
          checkedConstructorArtifact.contents.find("if (" + checkedSuccess +
                                                   ")") != std::string::npos,
      "a may-raise constructor staged into a by-value storage call must use "
      "the verified failure ABI and publish its prepared-parameter slot only "
      "on the Invoke success edge");

  const lang::FrontendResult invalidPublicStorage = lang::Frontend().analyze(
      "invalid-public-storage.gti", R"(
import std.memory;

class fixed_value {
public:
  fixed_value() {}
  fixed_value(fixed_value& other) = delete;
  fixed_value(fixed_value&& other) = delete;
};

int main() {
  std::storage<int32_t> forged = std::storage<int32_t>();
  std::allocator allocator = std::allocator();
  auto pinned = allocator.try_storage<fixed_value>(std::size_t(1));
  if (!pinned) { return 2; }
  mut auto fixed = std::move(pinned).value();
  fixed.construct(std::size_t(0), fixed_value());
  mut auto allocated = allocator.try_storage<int32_t>(std::size_t(1));
  if (!allocated) { return 1; }
  mut std::storage<int32_t> values = std::move(allocated).value();
  values.construct(std::size_t(0), int32_t(42));
  values.construct(std::size_t(0), int32_t(1), int32_t(2));
  int32_t& borrowed = values.get(std::size_t(0));
  values.destroy(std::size_t(0));
  std::storage<int32_t> copied = values;
  return borrowed;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !invalidPublicStorage.canGenerateCode() &&
          hasDiagnostic(invalidPublicStorage.diagnostics,
                        "No constructor of 'storage'") &&
          hasDiagnosticCode(invalidPublicStorage.diagnostics, "GTI-S2029") &&
          hasDiagnostic(invalidPublicStorage.diagnostics,
                        "'std::movable<fixed_value>'") &&
          hasDiagnostic(invalidPublicStorage.diagnostics,
                        "Function expects 2 arguments but received 3") &&
          hasDiagnostic(invalidPublicStorage.diagnostics,
                        "while a reference borrowed from it may still be "
                        "live") &&
          hasDiagnostic(invalidPublicStorage.diagnostics,
                        "Cannot initialize 'copied'"),
      "completed-value insertion should require movability; storage prevents "
      "owner forgery, extra construct arguments, copies, and mutation while a "
      "receiver-tied element borrow remains live");
}

void testFixedArrayPackForwarding() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/storage_fixed_array_emplacement.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  expect(!source.empty(), "fixed-array forwarding regression must load");
  const auto frontend =
      analyzeWithMemory("fixed-array-pack-forwarding.gti", source);
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "fixed-array arguments pass through ordinary forwarding, storage and "
         "vector emplacement");
  if (!frontend.canGenerateCode())
    return;

  const auto array = lang::SemanticType::arrayOf(lang::SemanticType::Int32, 3);
  const auto item =
      std::find_if(frontend.hir.classInstances().begin(),
                   frontend.hir.classInstances().end(),
                   [](const lang::HirClassInstance &instance) {
                     return instance.source != nullptr &&
                            instance.source->name().lexeme == "Item";
                   });
  expect(item != frontend.hir.classInstances().end(),
         "passive class-array forwarding must retain its exact element type");
  if (item == frontend.hir.classInstances().end())
    return;
  const auto itemArray = lang::SemanticType::arrayOf(item->type, 2);
  bool expandedArray = false;
  bool expandedClassArray = false;
  bool sourceHelper = false;
  bool sparseArrayForwarding = false, prefixArrayForwarding = false;
  for (const auto &function : frontend.hir.functionInstances()) {
    sourceHelper |= function.source != nullptr &&
                    function.source->name().lexeme == "forward_construct";
    for (const auto &value : function.body.values)
      for (const auto &element : value.packExpansionElements)
        if (element.type == array || element.type == itemArray) {
          expandedArray |= element.type == array;
          expandedClassArray |= element.type == itemArray;
          if (element.type == array && function.source) {
            sparseArrayForwarding |=
                function.source->name().lexeme == "emplace";
            prefixArrayForwarding |=
                function.source->name().lexeme == "append_unchecked";
          }
          expect(element.kind == lang::HirCallInputKind::Value &&
                     element.traits.copyable &&
                     !element.traits.containsBorrowedState,
                 "HIR records an exact passive array snapshot, not pointer "
                 "decay or a loan");
        }
  }
  bool rawConstruction = false, mixedPrefix = false;
  bool stagedClassArray = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hirFunction = frontend.hir.findFunctionInstance(function.id);
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            (instruction.info.type == array ||
             instruction.info.type == itemArray)) {
          expect(instruction.callInputKind == lang::HirCallInputKind::Value &&
                     !instruction.preparedParameterDrop &&
                     instruction.lifecycle.empty(),
                 "MIR array staging preserves value copying without an owner "
                 "handoff");
          stagedClassArray |= instruction.info.type == itemArray;
          mixedPrefix |=
              hirFunction != nullptr && hirFunction->source != nullptr &&
              hirFunction->source->name().lexeme == "forward_with_fixed";
        }
        if (instruction.intrinsic == lang::IntrinsicKind::ObjectConstruct &&
            instruction.constructorTarget &&
            instruction.parameterTypes.size() == 2 &&
            instruction.parameterTypes[1] == array) {
          const auto *selected = frontend.hir.findConstructorInstance(
              *instruction.constructorTarget);
          if (selected && selected->source) {
            rawConstruction |= selected->source->name().lexeme == "ArrayHolder";
          }
        }
      }
  }
  expect(sourceHelper && expandedArray && expandedClassArray &&
             stagedClassArray && mixedPrefix && rawConstruction &&
             sparseArrayForwarding && prefixArrayForwarding &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "sparse and prefix forwarding retain the exact array and share the "
         "general object-construction primitive");
  const auto artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  expect(artifact.contents.find("class ArrayHolder {") != std::string::npos &&
             artifact.contents.find("construct_representation_at(") !=
                 std::string::npos &&
             artifact.contents.find("forward_pack_argument(args)...") ==
                 std::string::npos,
         "array forwarding is emitted from concrete verified stages");

  const std::string prefix = R"(
class Holder {
  int32_t values[3];
public:
  Holder(int32_t initial[3]) : values(initial) {}
};
T forward_construct<T, Args...>(Args... args) { return T(args...); }
)";
  const auto wrongExtent =
      analyzeWithMemory("array-pack-wrong-extent.gti", prefix + R"(
int main() {
  int32_t seed[2] = {1, 2};
  Holder result = forward_construct<Holder>(seed);
  return 0;
}
)");
  const auto wrongElement =
      analyzeWithMemory("array-pack-wrong-element.gti", prefix + R"(
int main() {
  bool seed[3] = {true, false, true};
  Holder result = forward_construct<Holder>(seed);
  return 0;
}
)");
  expect(!wrongExtent.canGenerateCode() &&
             hasDiagnostic(wrongExtent.diagnostics,
                           "No constructor of 'Holder' exactly matches") &&
             !wrongElement.canGenerateCode() &&
             hasDiagnostic(wrongElement.diagnostics,
                           "No constructor of 'Holder' exactly matches"),
         "pack forwarding retains exact array extents and element types");
  const auto borrowed =
      analyzeWithMemory("array-pack-borrowed-elements.gti", R"(
class BorrowCarrier {
  int32_t& source;
public:
  BorrowCarrier(int32_t& source) : source(source) {}
};
int32_t consume<Args...>(Args... args) { return 0; }
int main() {
  int32_t source = 1;
  BorrowCarrier values[1] = {BorrowCarrier(source)};
  return consume(values);
}
)");
  expect(
      !borrowed.canGenerateCode() &&
          hasDiagnosticCode(borrowed.diagnostics, "GTI-S2045") &&
          hasDiagnostic(borrowed.diagnostics,
                        "Borrowed state cannot be nested"),
      "passive array forwarding does not erase element lifetime dependencies");
}

void testVariadicStorageConstructionAndVector() {
  const lang::FrontendResult frontend =
      analyzeSourceStorageFixture("variadic-storage-construction.gti", R"(
import std.memory;

class DefaultValue {
  int32_t stored = 17;

public:
  int32_t value() {
    return this.stored;
  }
public:
  DefaultValue() = default;
};

class Pair {
  int32_t first;
  bool active;

public:
  Pair(int32_t left, bool enabled) : first(left), active(enabled) {}

  int32_t value() {
    if (this.active) {
      return this.first;
    }
    return 0;
  }
};

class MoveOnlyValue {
  std::unique_ptr<Pair> value;

public:
  MoveOnlyValue(int32_t left, bool active)
      : value(std::make_unique<Pair>(left, active)) {}

  int32_t read() {
    return this.value->value();
  }
};

class EmplacingBuffer<std::storage_element T> {
  mut std::storage<T> data;
  mut uint64_t count = 0;

public:
  EmplacingBuffer(uint64_t capacity)
      : data(fixture_storage<T>(capacity)) {}

  mut T& emplace<Args...>(Args... args) mut {
    uint64_t index = this.count;
    this.data.emplace(index, args...);
    this.count++;
    return this.data.get_mut(index);
  }
};

int main() {
  mut EmplacingBuffer<Pair> pairs =
      EmplacingBuffer<Pair>(uint64_t(2));
  Pair& first = pairs.emplace(7, true);
  int32_t observed = first.value();
  [[discard]] pairs.emplace(9, false);

  mut EmplacingBuffer<MoveOnlyValue> owned =
      EmplacingBuffer<MoveOnlyValue>(uint64_t(1));
  MoveOnlyValue pending = MoveOnlyValue(23, true);
  MoveOnlyValue& placed = owned.emplace(std::move(pending));
  int32_t moved_value = placed.read();

  mut std::storage<DefaultValue> defaults =
      fixture_storage<DefaultValue>(uint64_t(1));
  defaults.emplace(uint64_t(0));
  int32_t default_value =
      defaults.get(uint64_t(0)).value();

  mut std::storage<int32_t> numbers =
      fixture_storage<int32_t>(uint64_t(2));
  numbers.emplace(uint64_t(0));
  numbers.emplace(uint64_t(1), 5);
  if (observed == 7 and moved_value == 23 and default_value == 17 and
      numbers.get(uint64_t(0)) == 0 and
      numbers.get(uint64_t(1)) == 5) {
    return 0;
  }
  return 1;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected variadic-storage diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "source storage emplace should support default construction, exact "
         "primitive values, and concrete variadic constructor arguments");
  if (!frontend.canGenerateCode())
    return;

  bool foundVariadicHirConstruction = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.intrinsic != lang::IntrinsicKind::ObjectConstruct ||
          !value.constructorTarget) {
        continue;
      }
      const lang::HirConstructorInstance *constructor =
          frontend.hir.findConstructorInstance(*value.constructorTarget);
      if (constructor != nullptr && constructor->source != nullptr &&
          constructor->source->name().lexeme == "Pair" &&
          constructor->parameterTypes.size() == 2 &&
          constructor->parameterTypes[0] == lang::SemanticType::Int32 &&
          constructor->parameterTypes[1] == lang::SemanticType::Bool &&
          value.parameterTypes.size() == 3 &&
          value.parameterTypes[0].kind == lang::SemanticType::RawPointer &&
          value.parameterTypes[1] == lang::SemanticType::Int32 &&
          value.parameterTypes[2] == lang::SemanticType::Bool) {
        foundVariadicHirConstruction = true;
      }
    }
  }
  expect(foundVariadicHirConstruction,
         "HIR should retain the exact multi-argument constructor target on "
         "the general object_construct pack expansion beneath source emplace");

  bool foundVariadicMirConstruction = false;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            instruction.intrinsic != lang::IntrinsicKind::ObjectConstruct ||
            !instruction.constructorTarget) {
          continue;
        }
        const lang::HirConstructorInstance *constructor =
            frontend.hir.findConstructorInstance(
                *instruction.constructorTarget);
        bool exactInputs = instruction.callSite != 0 &&
                           instruction.operands.size() == 3 &&
                           instruction.parameterTypes.size() == 3;
        for (std::size_t index = 0;
             exactInputs && index < instruction.operands.size(); ++index) {
          const lang::MirOperand &operand = instruction.operands[index];
          const lang::MirValue *value =
              operand.kind == lang::MirOperandKind::Value
                  ? function.body.findValue(operand.value)
                  : nullptr;
          const lang::MirInstruction *definition = nullptr;
          if (value != nullptr) {
            for (const lang::MirBlock &candidateBlock : function.body.blocks) {
              for (const lang::MirInstruction &candidate :
                   candidateBlock.instructions) {
                if (candidate.id == value->definition) {
                  definition = &candidate;
                  break;
                }
              }
              if (definition != nullptr) {
                break;
              }
            }
          }
          exactInputs =
              definition != nullptr &&
              definition->kind == lang::MirInstructionKind::CallInput &&
              definition->callSite == instruction.callSite &&
              definition->callInputRole == lang::MirCallInputRole::Argument &&
              definition->callInputIndex == index;
        }
        if (constructor != nullptr && constructor->source != nullptr &&
            constructor->source->name().lexeme == "Pair" &&
            constructor->parameterTypes.size() == 2 && exactInputs &&
            instruction.parameterTypes[0].kind ==
                lang::SemanticType::RawPointer &&
            instruction.parameterTypes[1] == lang::SemanticType::Int32 &&
            instruction.parameterTypes[2] == lang::SemanticType::Bool) {
          foundVariadicMirConstruction = true;
        }
      }
    }
  }
  expect(frontend.mir.valid() && foundVariadicMirConstruction,
         "MIR should retain the nested constructor identity on a variadic "
         "storage call and stage each concrete pack element explicitly");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      concreteBackendSymbols(artifact.contents, "__gti_type_",
                             "EmplacingBuffer")
                  .size() == 2 &&
          artifact.contents.find("class Pair {") != std::string::npos &&
          concreteBackendSymbols(artifact.contents, "__gti_member_", "emplace")
                  .size() >= 2 &&
          artifact.contents.find("_emplace__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("construct_representation_at(") !=
              std::string::npos &&
          artifact.contents.find("mir_storage_construct_v1") ==
              std::string::npos &&
          artifact.contents.find("::gti_internal::backend::storage<") ==
              std::string::npos &&
          artifact.contents.find("forward_pack_argument(args)...") ==
              std::string::npos,
      "the backend should pass each staged concrete pack input directly to "
      "storage construction without a temporary element or AST forwarding");

  const lang::FrontendResult vectorFrontend = lang::Frontend().analyze(
      "standard-vector-emplace.gti", R"(
import std.vector;
import std.memory;

class Reading {
  int32_t left;
  int32_t right;

public:
  Reading(int32_t first, int32_t second) : left(first), right(second) {}

  int32_t total() {
    return this.left + this.right;
  }
};

int main() {
  mut std::vector<Reading> readings = std::vector<Reading>();
  readings.reserve(std::size_t(1));
  Reading& first = readings.emplace_back(2, 3);
  int32_t observed = first.total();
  [[discard]] readings.emplace_back(5, 7);

  mut int32_t total = 0;
  for (Reading& reading : readings) {
    total += reading.total();
  }

  mut std::vector<int32_t> zeros =
      std::vector<int32_t>(std::size_t(2));
  zeros[std::size_t(1)] = 11;

  mut std::vector<std::unique_ptr<Reading>> owned =
      std::vector<std::unique_ptr<Reading>>();
  owned.push_back(std::make_unique<Reading>(1, 1));
  owned.insert(std::size_t(0), std::make_unique<Reading>(4, 5));
  owned.erase(std::size_t(0));
  if (observed == 5 and total == 17 and readings.size() == 2 and
      readings.capacity() == 2 and zeros[std::size_t(0)] == 0 and
      zeros[std::size_t(1)] == 11 and owned.size() == 1 and
      owned[std::size_t(0)]->total() == 2) {
    return 0;
  }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!vectorFrontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : vectorFrontend.diagnostics) {
      std::cerr << "Unexpected std::vector diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(vectorFrontend.canGenerateCode() && vectorFrontend.mir.valid(),
         "std::vector should exercise multi-argument emplace, growth, "
         "read-only iteration, value-initialized size construction, and "
         "move-only indexed insertion and erasure");

  const auto hasHirIntrinsic = [&](lang::IntrinsicKind intrinsic) {
    return std::any_of(vectorFrontend.hir.functionInstances().begin(),
                       vectorFrontend.hir.functionInstances().end(),
                       [&](const lang::HirFunctionInstance &function) {
                         return std::any_of(function.body.values.begin(),
                                            function.body.values.end(),
                                            [&](const lang::HirValue &value) {
                                              return value.intrinsic ==
                                                     intrinsic;
                                            });
                       });
  };
  const auto hasMirIntrinsic = [&](lang::IntrinsicKind intrinsic) {
    return std::any_of(
        vectorFrontend.mir.functionInstances().begin(),
        vectorFrontend.mir.functionInstances().end(),
        [&](const lang::MirFunctionInstance &function) {
          return std::any_of(
              function.body.blocks.begin(), function.body.blocks.end(),
              [&](const lang::MirBlock &block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const lang::MirInstruction &instruction) {
                      return instruction.intrinsic == intrinsic;
                    });
              });
        });
  };
  expect(hasHirIntrinsic(lang::IntrinsicKind::ObjectRelocate) &&
             hasHirIntrinsic(lang::IntrinsicKind::ObjectDestroy) &&
             hasMirIntrinsic(lang::IntrinsicKind::ObjectRelocate) &&
             hasMirIntrinsic(lang::IntrinsicKind::ObjectDestroy),
         "source vector shifts preserve general relocation/destruction facts "
         "through HIR and MIR, not nominal vector operations");

  const lang::BackendArtifact vectorArtifact =
      gti_test::emitCpp(vectorFrontend, vectorFrontend.mir, vectorFrontend.mir,
                        lang::OptimizationPipeline().run(
                            vectorFrontend.hir, lang::OptimizationLevel::O0));
  expect(vectorArtifact.contents.find("mir_object_relocate_v1(") !=
                 std::string::npos &&
             vectorArtifact.contents.find("mir_object_destroy_v1(") !=
                 std::string::npos &&
             vectorArtifact.contents.find("mir_prefix_") == std::string::npos,
         "the backend lowers source shifts through general exact-address "
         "lifetimes");

  const lang::FrontendResult forwardedPointers = lang::Frontend().analyze(
      "standard-vector-forwarded-pointers.gti", R"(
import std.vector;

class PointerBox {
  const int32_t* pointer = nullptr;

public:
  PointerBox(const int32_t* value) : pointer(value) {}
public:
  PointerBox() = default;
};

int main() {
  mut int32_t value = 5;
  mut int32_t* pointer = nullptr;
  unsafe { pointer = &value; }

  mut std::vector<PointerBox> boxes = std::vector<PointerBox>();
  [[discard]] boxes.emplace_back(pointer);
  [[discard]] boxes.emplace_back(nullptr);
  return boxes.size() == 2 ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!forwardedPointers.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : forwardedPointers.diagnostics) {
      std::cerr << "Unexpected forwarded-pointer diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(forwardedPointers.canGenerateCode() && forwardedPointers.mir.valid(),
         "vector emplace should preserve safe T* to const T* and nullptr "
         "compatibility through a concrete forwarded pack");

  const lang::FrontendResult rangeMutation = lang::Frontend().analyze(
      "vector-range-mutation.gti", R"(
import std.vector;

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  [[discard]] values.emplace_back(1);
  for (int32_t& value : values) {
    values.reserve(std::size_t(4));
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  bool duplicateBorrowOrigin = false;
  bool foundBorrowOrigin = false;
  for (const lang::Diagnostic &diagnostic : rangeMutation.diagnostics) {
    if (diagnostic.code != "GTI-S2017") {
      continue;
    }
    foundBorrowOrigin = foundBorrowOrigin || !diagnostic.related.empty();
    for (std::size_t left = 0; left < diagnostic.related.size(); ++left) {
      for (std::size_t right = left + 1; right < diagnostic.related.size();
           ++right) {
        const lang::RelatedDiagnostic &first = diagnostic.related[left];
        const lang::RelatedDiagnostic &second = diagnostic.related[right];
        duplicateBorrowOrigin =
            duplicateBorrowOrigin || (first.message == second.message &&
                                      first.span.source == second.span.source &&
                                      first.span.start == second.span.start &&
                                      first.span.end == second.span.end &&
                                      first.span.line == second.span.line);
      }
    }
  }
  expect(!rangeMutation.canGenerateCode() && foundBorrowOrigin &&
             !duplicateBorrowOrigin,
         "range mutation diagnostics should report each identical borrow "
         "origin span and message only once");

  const lang::FrontendResult invalidConstruction = analyzeSourceStorageFixture(
      "invalid-variadic-storage-construction.gti", R"(
class Pair {
public:
  Pair(int32_t first, bool second) {}
public:
  Pair() = default;
};

class Secret {
  Secret(int32_t value) {}
public:
  Secret() = default;
};

class Slot<std::storage_element T> {
  mut std::storage<T> data;

public:
  Slot() : data(fixture_storage<T>(uint64_t(1))) {}

  void emplace<Args...>(Args... args) mut {
    this.data.emplace(uint64_t(0), args...);
  }
};

int main() {
  mut Slot<Pair> missing = Slot<Pair>();
  missing.emplace(1);
  mut Slot<Pair> wrong = Slot<Pair>();
  wrong.emplace(1, 2);
  mut Slot<Secret> hidden = Slot<Secret>();
  hidden.emplace(3);
  return 0;
}
)");
  const std::size_t constructionDiagnosticsWithInstantiation =
      static_cast<std::size_t>(std::count_if(
          invalidConstruction.diagnostics.begin(),
          invalidConstruction.diagnostics.end(),
          [](const lang::Diagnostic &diagnostic) {
            return std::any_of(
                diagnostic.related.begin(), diagnostic.related.end(),
                [](const lang::RelatedDiagnostic &related) {
                  return related.message.find(
                             "Concrete generic instance requested here") !=
                         std::string::npos;
                });
          }));
  // HIR lowering ran (its instances exist) and its concrete reanalysis
  // reports these source errors as semantic failures.
  expect(!invalidConstruction.canGenerateCode() &&
             !invalidConstruction.semanticValid &&
             !invalidConstruction.hirValid &&
             !invalidConstruction.hir.functionInstances().empty() &&
             hasDiagnostic(invalidConstruction.diagnostics,
                           "No constructor of 'Pair' exactly matches argument "
                           "types (int32_t)") &&
             hasDiagnostic(invalidConstruction.diagnostics,
                           "No constructor of 'Pair' exactly matches argument "
                           "types (int32_t, int32_t)") &&
             hasDiagnostic(invalidConstruction.diagnostics,
                           "Constructor of 'Secret' is private") &&
             constructionDiagnosticsWithInstantiation >= 3,
         "concrete emplace reanalysis should diagnose bad arity, exact-type "
         "mismatch, and constructor access with each instantiation site");

  const lang::FrontendResult borrowedStorage =
      analyzeSourceStorageFixture("borrowed-state-storage.gti", R"(
class BorrowCarrier {
  int32_t& value;

public:
  BorrowCarrier(int32_t& source) : value(source) {}
};

int main() {
  int32_t source = 1;
  mut std::storage<BorrowCarrier> forbidden =
      fixture_storage<BorrowCarrier>(uint64_t(1));
  return source;
}
)");
  expect(
      !borrowedStorage.canGenerateCode() &&
          hasDiagnostic(borrowedStorage.diagnostics, "std::storage_element"),
      "source storage constraints reject elements that retain borrowed state");

  const lang::FrontendResult nonmovableRelocation =
      analyzeSourceStorageFixture("nonmovable-storage-relocation.gti", R"(
class Stationary {
  int32_t value = 0;

public:
  Stationary(Stationary&& other) = delete;
public:
  Stationary() = default;
};

int main() {
  mut std::storage<Stationary> source =
      fixture_storage<Stationary>(uint64_t(1));
  mut std::storage<Stationary> destination =
      fixture_storage<Stationary>(uint64_t(1));
  source.emplace(uint64_t(0));
  source.relocate_to(destination, uint64_t(1));
  return 0;
}
)");
  expect(!nonmovableRelocation.canGenerateCode() &&
             hasDiagnostic(nonmovableRelocation.diagnostics, "std::movable"),
         "storage relocation should reject an element with deleted move "
         "construction before backend generation");
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::unordered_set<std::string> bufferTypes =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "Buffer");
  const std::string concreteBuffer =
      bufferTypes.size() == 1 ? *bufferTypes.begin() : std::string();
  const std::size_t emittedProgram =
      artifact.contents.find("namespace __gti_program {");
  std::vector<lang::HirConstructorInstanceId> ownerConstructors;
  for (const auto &constructor : frontend.hir.constructorInstances())
    if (constructor.source != nullptr &&
        (constructor.source->name().lexeme == "Buffer" ||
         constructor.source->name().lexeme == "NestedBuffer"))
      ownerConstructors.push_back(constructor.id);
  expect(ownerConstructors.size() == 2 &&
             std::all_of(ownerConstructors.begin(), ownerConstructors.end(),
                         [&](auto id) {
                           return artifact.contents.find(
                                      "scalar-cfg-constructor-failure-v1 "
                                      "constructor-instance " +
                                      std::to_string(id) + "\n") !=
                                  std::string::npos;
                         }),
         "both concrete owner constructors retain their exact MIR body "
         "identity independently of imported library constructor numbering");
  expect(
      !concreteBuffer.empty() && emittedProgram != std::string::npos &&
          // The failure-free transfers stage the moved parameter in their
          // returned value's slot and move it out after its source drop.
          containsMangledFunction(artifact.contents, "", "transfer", "(") &&
          containsMangledFunction(artifact.contents, "", "transfer_nested",
                                  "(") &&
          artifact.contents.find(
              "__gti_mir_v_1.construct(std::move(__gti_mir_p_1.get()));") !=
              std::string::npos &&
          artifact.contents.find("return std::move(__gti_mir_v_1.get());") !=
              std::string::npos &&
          artifact.contents.find("mir_prefix_allocate_v1") ==
              std::string::npos &&
          artifact.contents.find("__gti_entry__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("return std::move(value)", emittedProgram) ==
              std::string::npos &&
          artifact.contents.find(concreteBuffer + "(const " + concreteBuffer +
                                 " &other) = delete;") != std::string::npos &&
          artifact.contents.find(concreteBuffer + "(" + concreteBuffer +
                                 " &&) = default;") != std::string::npos &&
          artifact.contents.find(concreteBuffer + " &operator=(const " +
                                 concreteBuffer + " &other) = delete;") !=
              std::string::npos &&
          artifact.contents.find(concreteBuffer + " &operator=(" +
                                 concreteBuffer + " &&) = default;") !=
              std::string::npos,
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

  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir,
                        lang::OptimizationPipeline().run(
                            frontend.hir, lang::OptimizationLevel::O0));
  expect(
      artifact.contents.find("ExplicitStructural();") != std::string::npos &&
          artifact.contents.find("~ExplicitStructural() noexcept = default;") !=
              std::string::npos &&
          artifact.contents.find("ExplicitStructural::ExplicitStructural(") !=
              std::string::npos &&
          artifact.contents.find("std::int32_t value;") != std::string::npos &&
          artifact.contents.find("std::int32_t value =") == std::string::npos &&
          artifact.contents.find("__gti_lifecycle_active_") ==
              std::string::npos,
      "the backend should emit the selected generated constructor while "
      "keeping structural destruction and no duplicate default or active-drop "
      "adapter");

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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const lang::ClassTypeInfo *cleanupType =
      cleanupClass == nullptr ? nullptr
                              : frontend.semantics.findClassType(*cleanupClass);
  const std::string lifecycleSuffix =
      cleanupType == nullptr ? "" : std::to_string(cleanupType->id);
  const std::string cleanupName = "__gti_lifecycle_cleanup_" + lifecycleSuffix;
  const std::string activeName = "__gti_lifecycle_active_" + lifecycleSuffix;
  const std::unordered_set<std::string> cleanupTypes =
      concreteBackendSymbols(artifact.contents, "__gti_type_", "CleanupBuffer");
  const std::string concreteCleanup =
      cleanupTypes.size() == 1 ? *cleanupTypes.begin() : std::string();
  expect(
      cleanupType != nullptr && !concreteCleanup.empty() &&
          artifact.contents.find("~" + concreteCleanup + "() noexcept { if (" +
                                 activeName + ") { " + cleanupName +
                                 "(); } }") != std::string::npos &&
          artifact.contents.find("bool " + activeName + " = true;") !=
              std::string::npos &&
          artifact.contents.find("if (!" + activeName + ") { return true; }") !=
              std::string::npos &&
          artifact.contents.find(concreteCleanup + "(const " + concreteCleanup +
                                 " &other) = delete;") != std::string::npos &&
          artifact.contents.find(concreteCleanup + "(" + concreteCleanup +
                                 " &&other) noexcept") != std::string::npos &&
          artifact.contents.find("other." + activeName + " = false;") !=
              std::string::npos &&
          artifact.contents.find(cleanupName + "();") != std::string::npos &&
          artifact.contents.find(
              "GTI verified-MIR body: scalar-cfg-destructor-failure-v1") !=
              std::string::npos &&
          hasMirIntrinsicKind(frontend, lang::IntrinsicKind::ObjectDestroy) &&
          artifact.contents.find("mir_prefix_pop_v1") == std::string::npos,
      "the backend should guard native retirement with the same initialized "
      "active drop state while retaining declared cleanup and safe moves");

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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::size_t emittedProgram =
      artifact.contents.find("namespace __gti_program {");
  expect(artifact.contents.find("___gti_operator_arrow") != std::string::npos &&
             artifact.contents.find("___gti_operator_dereference") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_subscript") !=
                 std::string::npos &&
             artifact.contents.find(
                 "friend const std::int32_t & "
                 "__gti_operator_dereference(const Handle &__gti_receiver)") !=
                 std::string::npos &&
             artifact.contents.find("::operator!=(__gti_mir_v_") !=
                 std::string::npos &&
             // The field-returning arrow cannot fail and publishes its loan
             // directly; the checked-index dereference keeps its failure form.
             artifact.contents.find("___gti_operator_arrow());") !=
                 std::string::npos &&
             artifact.contents.find(
                 "___gti_operator_dereference__gti_mir_failure(&"
                 "__gti_mir_loan_") != std::string::npos &&
             artifact.contents.find("___gti_operator_bool()") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_plus") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_add_assign") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_pre_decrement") !=
                 std::string::npos &&
             emittedProgram != std::string::npos &&
             artifact.contents.find(" operator*(", emittedProgram) ==
                 std::string::npos,
         "the backend should retain selected method calls while exposing only "
         "validated comparisons as C++ operators for generic representation");

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

// `->` names the place its selected checked access step returns, and members
// are looked up on that place's type. A builtin text referent supplies its own
// members; a referent with none is an ordinary source error.
void testArrowNonClassReferents() {
  const auto analyze = [](std::string_view name, std::string source) {
    source.insert(0, "import std.string;\nimport std.optional;\n");
    return lang::Frontend().analyze(
        std::filesystem::temp_directory_path() / std::string(name),
        std::move(source), {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
  };
  const auto accepted = analyze("arrow-text-referents.gti", R"(
class Label {
public:
  std::string_view text;
  Label(std::string_view text) : text(text) {}
  std::string_view& operator->() { return this.text; }
};
int main() {
  Label label = Label("label");
  std::string_view text = "text";
  std::optional<std::string_view> maybe = text;
  uint64_t total = label->size() + maybe->size() + label->end().position();
  return label->empty() || total != 14 ? 1 : 0;
}
)");
  for (const lang::Diagnostic &diagnostic : accepted.diagnostics)
    std::cerr << "Unexpected arrow diagnostic: " << diagnostic.code << ": "
              << diagnostic.message << '\n';
  expect(accepted.canGenerateCode() &&
             lang::verifyMirProgram(accepted.mir).valid(),
         "text members are called through a checked operator->");
  // Each member resolved to its builtin observer; none reaches MIR untyped.
  bool untyped = false;
  for (const auto &function : accepted.mir.functionInstances())
    for (const auto &value : function.body.values)
      untyped |= value.info.type == lang::SemanticType::Unknown;
  expect(
      !untyped &&
          hasMirIntrinsicKind(accepted, lang::IntrinsicKind::StringViewSize) &&
          hasMirIntrinsicKind(accepted, lang::IntrinsicKind::StringViewEmpty),
      "arrow text members lower to their builtin observers");

  const std::string cell =
      "class Cell { int value = 3; public: int& "
      "operator->() { return this.value; }  public: Cell() = default; }; ";
  const std::string text = "class Text { public: c_string text; Text(c_string "
                           "text) : text(text) {} c_string& operator->() { "
                           "return this.text; } }; ";
  const std::string label = "class Label { public: std::string_view text; "
                            "Label(std::string_view text) : text(text) {} "
                            "std::string_view& operator->() { return "
                            "this.text; } }; ";
  struct Rejected {
    std::string source;
    const char *code;
  };
  for (const Rejected &entry : {
           Rejected{cell + "int read(Cell& c) { return c->size(); }",
                    "GTI-S2000"},
           Rejected{cell + "int read(Cell& c) { return c->value; }",
                    "GTI-S2000"},
           Rejected{text + "int read(Text& t) { return int(t->size()); }",
                    "GTI-S2000"},
           Rejected{label + "int read(Label& l) { return int(l->missing()); }",
                    "GTI-S2035"},
           Rejected{label + "int read(Label& l) { return int(l->size(1)); }",
                    "GTI-S2035"},
       }) {
    const auto rejected =
        analyze("arrow-rejected-referent.gti",
                entry.source + "\nint main() { return 0; }\n");
    expect(!rejected.canGenerateCode() &&
               hasDiagnosticCode(rejected.diagnostics, entry.code) &&
               std::none_of(rejected.diagnostics.begin(),
                            rejected.diagnostics.end(),
                            [](const lang::Diagnostic &diagnostic) {
                              return diagnostic.code.starts_with("GTI-B");
                            }),
           "an arrow referent's missing member is a source error: " +
               entry.source);
  }
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("___gti_operator_call") != std::string::npos &&
             artifact.contents.find(" operator()(") == std::string::npos,
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("___gti_operator_call() &&;") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_call() const &;") !=
                 std::string::npos &&
             artifact.contents.find("___gti_operator_call(const "
                                    "std::int32_t amount) &;") !=
                 std::string::npos &&
             containsMangledFunction(artifact.contents, "std::int32_t ",
                                     "__gti_operator_call", "() &&;") &&
             containsMangledFunction(artifact.contents, "std::int32_t ",
                                     "__gti_operator_call", "() const &;") &&
             containsMangledFunction(artifact.contents, "std::int32_t ",
                                     "__gti_operator_call",
                                     "(const std::int32_t amount) &;") &&
             artifact.contents.find(
                 "std::move(__gti_mir_p_1.get()).::__gti_program::OnceValue::"
                 "__gti_fn_") != std::string::npos &&
             artifact.contents.find(
                 "__gti_mir_p_1.get().::__gti_program::MutableFallback::"
                 "__gti_fn_") != std::string::npos &&
             artifact.contents.find("___gti_operator_call__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find(
                 "exact_call<::gti_internal::backend::callable_capability::"
                 "once_call>") != std::string::npos &&
             artifact.contents.find(
                 "exact_call<::gti_internal::backend::callable_capability::"
                 "mutable_call") != std::string::npos &&
             artifact.contents.find("callable_capability::read_call") !=
                 std::string::npos,
         "the C++ backend should preserve consuming member qualification and "
         "the exact frontend-selected callable capability");

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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const bool emittedRangeCore =
      artifact.contents.find(
          "const ::__gti_program::CounterRange *__gti_mir_loan_") !=
          std::string::npos &&
      artifact.contents.find("_begin__gti_mir_failure(") != std::string::npos &&
      // Constructing the sentinel cannot fail, so end() is selected plainly.
      containsMangledFunction(artifact.contents, "CounterRange::", "end",
                              "()") &&
      artifact.contents.find(".__gti_operator_dereference__gti_mir_failure(") !=
          std::string::npos &&
      artifact.contents.find(
          ".__gti_operator_pre_increment__gti_mir_failure(") !=
          std::string::npos &&
      artifact.contents.find(".begin()") == std::string::npos &&
      artifact.contents.find(".end()") == std::string::npos;
  expect(emittedRangeCore,
         "the C++ backend should emit the verified range loans and selected "
         "GTI iterator methods without native C++ range lookup");

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

void testReadOnlyRangeLoans() {
  const std::string source = R"(
interface IteratorContract<T> {
  T& operator*();
  void operator++() mut;
};

class CounterIterator : public IteratorContract<int> {
  mut int current;
public:
  CounterIterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(CounterIterator& other) {
    return this.current != other.current;
  }
};

class OwnedRange {
  int first;
  int last;
public:
  OwnedRange(int first, int last) : first(first), last(last) {}
  OwnedRange(OwnedRange& other) = delete;
  OwnedRange(OwnedRange&& other) = default;
  ~OwnedRange() {}
  CounterIterator begin() { return CounterIterator(this.first); }
  CounterIterator end() { return CounterIterator(this.last); }
};

int nested_arrays() {
  int first[3] = {1, 2, 3};
  int second[2] = {4, 5};
  mut int total = 0;
  for (auto& left : first) {
    for (int right : second) {
      if (right == 4) { continue; }
      total += left + right;
    }
  }
  return total - 21;
}

int temporary_complete() {
  mut int total = 0;
  for (auto& value : OwnedRange(1, 5)) {
    if (value == 2) { continue; }
    if (value == 4) { break; }
    total += value;
  }
  for (auto& value : OwnedRange(0, 0)) { return value; }
  return total - 4;
}

int temporary_return() {
  for (auto& value : OwnedRange(3, 6)) {
    if (value == 4) { return value - 4; }
  }
  return 1;
}

int main() {
  return nested_arrays() + temporary_complete() + temporary_return();
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "read-only-range-loans.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected read-only range diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             frontend.mir.valid() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "fixed arrays and move-only owned temporary ranges should lower "
         "through verified semantic, HIR, and MIR range plans");

  std::size_t fixedHirRanges = 0;
  std::size_t temporaryHirRanges = 0;
  bool hirLoansAreReadOnly = true;
  const lang::HirFunctionInstance *temporaryCompleteHir = nullptr;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "temporary_complete") {
      temporaryCompleteHir = &function;
    }
    for (const lang::HirStatement &statement : function.body.statements) {
      if (!statement.rangeIteration) {
        continue;
      }
      const lang::HirRangeIteration &range = *statement.rangeIteration;
      fixedHirRanges +=
          range.strategy == lang::RangeIterationStrategy::FixedArray;
      temporaryHirRanges += range.ownsRangeTemporary;
      const lang::HirLoan *rangeLoan = function.body.findLoan(range.rangeLoan);
      const lang::HirLoan *elementLoan =
          function.body.findLoan(range.elementLoan);
      hirLoansAreReadOnly =
          hirLoansAreReadOnly &&
          range.elementAccess == lang::AccessMode::ReadOnly &&
          rangeLoan != nullptr &&
          rangeLoan->access == lang::AccessMode::ReadOnly &&
          (range.elementLoan == 0 ||
           (elementLoan != nullptr &&
            elementLoan->access == lang::AccessMode::ReadOnly));
      if (range.strategy == lang::RangeIterationStrategy::FixedArray) {
        hirLoansAreReadOnly =
            hirLoansAreReadOnly && range.arrayLength != 0 &&
            range.beginFunction == 0 && range.endFunction == 0 &&
            range.compareFunction == 0 && range.dereferenceFunction == 0 &&
            range.incrementFunction == 0;
      }
    }
  }
  expect(fixedHirRanges == 2 && temporaryHirRanges == 3 && hirLoansAreReadOnly,
         "HIR should retain fixed-array indexed strategy, structural "
         "temporary ownership, and exact read-only loan identities");

  std::size_t fixedMirRanges = 0;
  std::size_t temporaryMirRanges = 0;
  bool boundedLoans = true;
  bool normalTemporaryDrop = false;
  bool failureTemporaryDrop = false;
  const lang::MirBody *temporaryCompleteMir = nullptr;
  for (const lang::HirFunctionInstance &hirFunction :
       frontend.hir.functionInstances()) {
    const lang::MirFunctionInstance *mirFunction =
        frontend.mir.findFunctionInstance(hirFunction.id);
    if (mirFunction == nullptr) {
      continue;
    }
    if (hirFunction.source != nullptr &&
        hirFunction.source->name().lexeme == "temporary_complete") {
      temporaryCompleteMir = &mirFunction->body;
    }
    for (const lang::MirRangeIteration &range :
         mirFunction->body.rangeIterations) {
      fixedMirRanges +=
          range.strategy == lang::RangeIterationStrategy::FixedArray;
      temporaryMirRanges += range.ownsRangeTemporary;
      const lang::MirLoan *rangeLoan =
          mirFunction->body.findLoan(range.rangeLoan);
      const lang::MirLoan *elementLoan =
          mirFunction->body.findLoan(range.elementLoan);
      boundedLoans = boundedLoans &&
                     range.elementAccess == lang::AccessMode::ReadOnly &&
                     rangeLoan != nullptr && !rangeLoan->escapes &&
                     (range.elementLoan == 0 ||
                      (elementLoan != nullptr && !elementLoan->escapes));
      if (!range.ownsRangeTemporary || range.ownerDrop == 0) {
        continue;
      }
      for (const lang::MirBlock &block : mirFunction->body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
            if (event.kind != lang::MirLifecycleEventKind::Drop ||
                event.source != range.ownerDrop) {
              continue;
            }
            normalTemporaryDrop = normalTemporaryDrop || !event.failureCleanup;
            failureTemporaryDrop = failureTemporaryDrop || event.failureCleanup;
          }
        }
      }
    }
  }
  expect(fixedMirRanges == 2 && temporaryMirRanges == 3 && boundedLoans &&
             normalTemporaryDrop && failureTemporaryDrop,
         "MIR should preserve loop-bounded read-only loans and exact-once "
         "owned-range cleanup on normal and defined-failure paths");

  const lang::FrontendResult sharedTraversal = lang::Frontend().analyze(
      "nested-read-only-range-loans.gti", R"(
import std.string;
import std.vector;

class Holder {
public:
  Holder() {}
  mut std::vector<int32_t> items = std::vector<int32_t>();
};

int main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  for (auto& first : values) {
    for (auto& second : values) {
      for (auto& third : values) {}
    }
  }

  mut Holder holder = Holder();
  for (auto& first : holder.items) {
    for (auto& second : holder.items) {}
  }

  mut std::string text = std::string("ab");
  mut int32_t count = 0;
  for (auto& first : text) {
    for (auto& second : text) { count += 1; }
  }
  return count - 4;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!sharedTraversal.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : sharedTraversal.diagnostics) {
      std::cerr << "Unexpected nested read-only range diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(sharedTraversal.canGenerateCode() && sharedTraversal.mir.valid() &&
             lang::verifyMirProgram(sharedTraversal.mir).valid(),
         "nested and triple read-only traversals of the same local or field "
         "range should coexist through verified MIR loans");

  lang::HirProgram invalidHir = frontend.hir;
  bool corruptedHirRange = false;
  auto &invalidHirFunctions =
      const_cast<std::vector<lang::HirFunctionInstance> &>(
          invalidHir.functionInstances());
  for (lang::HirFunctionInstance &function : invalidHirFunctions) {
    for (lang::HirStatement &statement : function.body.statements) {
      if (statement.rangeIteration) {
        statement.rangeIteration->elementAccess = lang::AccessMode::Mutable;
        corruptedHirRange = true;
        break;
      }
    }
    if (corruptedHirRange) {
      break;
    }
  }
  expect(
      corruptedHirRange &&
          !lang::verifyHirProgramPlans(frontend.semantics, invalidHir).valid(),
      "HIR verification should reject a range plan that widens the "
      "semantic element loan");

  lang::MirBody invalidMir =
      temporaryCompleteMir == nullptr ? lang::MirBody{} : *temporaryCompleteMir;
  if (!invalidMir.rangeIterations.empty()) {
    invalidMir.rangeIterations.front().elementAccess =
        lang::AccessMode::Mutable;
  }
  expect(temporaryCompleteHir != nullptr && temporaryCompleteMir != nullptr &&
             !invalidMir.rangeIterations.empty() &&
             !lang::verifyMirBody(invalidMir).valid(),
         "MIR verification should reject a writable range plan forged after "
         "semantic analysis");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("std::array<std::int32_t, 3>") !=
                 std::string::npos &&
             artifact.contents.find("std::uint64_t{0}") != std::string::npos,
         "the C++ backend should consume the fixed-array MIR strategy rather "
         "than rediscovering source range syntax");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-read-only-range-loans.gti", R"(
int& escape_element() {
  int values[2] = {1, 2};
  for (auto& value : values) { return value; }
  return values[0];
}

int main() {
  int values[2] = {1, 2};
  for (auto& value : values) { value = 3; }
  for (mut auto& value : values) { value = 4; }
  for (int value : 42) { return value; }
  return 0;
}
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2002") &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2017") &&
             countDiagnosticCode(invalid.diagnostics, "GTI-S2083") >= 2,
         "read-only range diagnostics should reject mutation, escaping "
         "element loans, mutable bindings, and unsupported range forms");

  const lang::FrontendResult movedReuse = lang::Frontend().analyze(
      "moved-range-reuse.gti", R"(
interface IteratorContract<T> { T& operator*(); void operator++() mut; };
class Iterator : public IteratorContract<int> {
  mut int current;
public:
  Iterator(int value) : current(value) {}
  int& operator*() override { return this.current; }
  void operator++() mut override { this.current++; }
  bool operator!=(Iterator& other) { return this.current != other.current; }
};
class Range {
public:
  Range() {}
  Range(Range& other) = delete;
  Range(Range&& other) = default;
  Iterator begin() { return Iterator(0); }
  Iterator end() { return Iterator(1); }
};
int main() {
  mut Range range = Range();
  for (auto& value : std::move(range)) { break; }
  Iterator invalid = range.begin();
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!movedReuse.canGenerateCode() &&
             hasDiagnosticCode(movedReuse.diagnostics, "GTI-S2018"),
         "moving an owned range into loop storage should invalidate later "
         "reuse of the original owner");
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
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("Span") != std::string::npos,
         "the C++ backend should emit a passive-cleanup range temporary");
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

void testExpectedPayloadRangeLoans() {
  const std::string source = R"(
import std.string;

expected<std::string, int32_t> make_text() {
  return std::string("ab");
}

interface CounterIteratorContract {
  int32_t& operator*();
  void operator++() mut;
};

class CounterIterator : public CounterIteratorContract {
  mut int32_t current;
public:
  CounterIterator(int32_t value) : current(value) {}
  int32_t& operator*() override { return this.current; }
  void operator++() mut override { this.current += 1; }
  bool operator!=(CounterIterator& other) {
    return this.current != other.current;
  }
};

class CounterRange {
  int32_t first;
  int32_t last;
public:
  CounterRange(int32_t first, int32_t last) : first(first), last(last) {}
  CounterIterator begin() { return CounterIterator(this.first); }
  CounterIterator end() { return CounterIterator(this.last); }
};

expected<CounterRange, int32_t> make_range(int32_t first, int32_t last) {
  return CounterRange(first, last);
}

// Read-only owners select the read-only value() accessor; a `mut` owner
// selects the mutable one, whose read-only loop shares its mutable loan.
int count_parameter(expected<std::string, int32_t> text) {
  mut int count = 0;
  for (auto& outer : text.value()) {
    for (auto& inner : text.value()) {
      count += 1;
      if (count == 1) { continue; }
    }
    if (count == 4) { break; }
  }
  return count;
}

int return_from_loop(expected<std::string, int32_t> text) {
  for (auto& byte : text.value()) { return 2; }
  return 1;
}

int nominal_ranges() {
  auto values = make_range(1, 4);
  mut int32_t total = 0;
  for (auto& value : values.value()) { total += value; }
  auto empty = make_range(0, 0);
  for (auto& value : empty.value()) { return 1; }
  return total - 6;
}

int main() {
  auto first = make_text();
  mut int count = 0;
  for (auto& byte : first.value()) { count += 1; }
  auto second = make_text();
  auto third = make_text();
  return count + count_parameter(std::move(second)) +
         return_from_loop(std::move(third)) + nominal_ranges() - 8;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "expected-payload-range-loans.gti", source, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected Expected payload range diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             frontend.mir.valid() &&
             lang::verifyHirProgramPlans(frontend.semantics, frontend.hir)
                 .valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "named Expected payloads should form verified read-only range loans");

  // A named result's payload range borrows the reference that an ordinary
  // value() call of the prelude class returns from that named owner.
  const auto payloadSource =
      [&](const lang::HirBody &body,
          const lang::HirRangeIteration &range) -> const lang::HirValue * {
    const lang::HirValue *source = body.findValue(range.source);
    const lang::HirValue *receiver = source == nullptr || !source->receiver
                                         ? nullptr
                                         : body.findValue(*source->receiver);
    const lang::HirFunctionInstance *target =
        source == nullptr || !source->functionTarget
            ? nullptr
            : frontend.hir.findFunctionInstance(*source->functionTarget);
    return source != nullptr && source->kind == lang::HirValueKind::Call &&
                   receiver != nullptr &&
                   isExpectedClass(frontend, receiver->info.type) &&
                   target != nullptr && target->source != nullptr &&
                   target->source->name().lexeme == "value"
               ? source
               : nullptr;
  };
  std::size_t hirRanges = 0;
  bool exactHirPlans = true;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (!entryFunction(frontend, function.id))
      continue;
    for (const lang::HirStatement &statement : function.body.statements) {
      if (!statement.rangeIteration)
        continue;
      const lang::HirRangeIteration &range = *statement.rangeIteration;
      const lang::HirValue *payload = payloadSource(function.body, range);
      if (payload == nullptr)
        continue;
      ++hirRanges;
      const lang::HirLoan *loan = function.body.findLoan(range.rangeLoan);
      const auto owner = std::find_if(function.body.bindings.begin(),
                                      function.body.bindings.end(),
                                      [&](const lang::HirBinding &binding) {
                                        return binding.id == range.owner;
                                      });
      exactHirPlans =
          exactHirPlans && !range.ownsRangeTemporary &&
          payload->borrowOrigin == lang::BorrowOriginKind::Receiver &&
          payload->borrowAccess == lang::AccessMode::ReadOnly &&
          loan != nullptr && loan->access == lang::AccessMode::ReadOnly &&
          owner != function.body.bindings.end() &&
          owner->info.type.kind == lang::SemanticType::Reference;
    }
  }
  expect(hirRanges == 6 && exactHirPlans,
         "HIR should retain six exact Expected.value read-only range plans");

  std::size_t mirRanges = 0;
  bool exactMirPlans = true;
  const lang::MirBody *firstExpectedBody = nullptr;
  lang::MirLoanId firstExpectedLoan = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (!entryFunction(frontend, function.id))
      continue;
    for (const lang::MirRangeIteration &range : function.body.rangeIterations) {
      // The checked extraction is the value() call producing the range loan.
      const lang::MirInstruction *producer = nullptr;
      const lang::MirBlock *definitionBlock = nullptr;
      for (const lang::MirBlock &block : function.body.blocks) {
        for (const lang::MirInstruction &instruction : block.instructions) {
          if (instruction.loan == range.rangeLoan &&
              isExpectedMemberCall(frontend, instruction, "value")) {
            producer = &instruction;
            definitionBlock = &block;
          }
        }
      }
      if (producer == nullptr) {
        continue;
      }
      ++mirRanges;
      if (firstExpectedBody == nullptr) {
        firstExpectedBody = &function.body;
        firstExpectedLoan = range.rangeLoan;
      }
      const lang::MirValue *payload =
          producer->result ? function.body.findValue(*producer->result)
                           : nullptr;
      const lang::MirLoan *loan = function.body.findLoan(range.rangeLoan);
      const lang::MirPlace *sourcePlace =
          loan == nullptr ? nullptr : function.body.findPlace(loan->source);
      const lang::MirInstruction *receiverStage =
          definingInstruction(function.body, producer->receiver->value);
      const std::size_t payloadRoots =
          payload == nullptr || loan == nullptr
              ? 0
              : std::count_if(
                    function.body.places.begin(), function.body.places.end(),
                    [&](const lang::MirPlace &place) {
                      return place.root == lang::MirPlaceRootKind::Loan &&
                             place.loan == loan->id &&
                             place.projections.empty() &&
                             place.type == range.rangeType &&
                             place.sourceValue == payload->sourceValue;
                    });
      exactMirPlans =
          exactMirPlans && payload != nullptr && loan != nullptr &&
          sourcePlace != nullptr && !range.ownsRangeTemporary &&
          receiverStage != nullptr &&
          receiverStage->kind == lang::MirInstructionKind::CallInput &&
          receiverStage->callInputRole == lang::MirCallInputRole::Receiver &&
          receiverStage->operands.size() == 1 &&
          receiverStage->operands.front().kind ==
              lang::MirOperandKind::BorrowRead &&
          receiverStage->operands.front().place == loan->source &&
          producer->borrowOrigin == lang::BorrowOriginKind::Receiver &&
          producer->borrowAccess == lang::AccessMode::ReadOnly &&
          definitionBlock->terminator.kind == lang::MirTerminatorKind::Invoke &&
          definitionBlock->terminator.invokeInstruction == producer->id &&
          loan->kind == lang::MirLoanKind::CallResult && !loan->escapes &&
          loan->access == lang::AccessMode::ReadOnly &&
          isExpectedClass(frontend, sourcePlace->type) &&
          sourcePlace->type.arguments.size() == 2 &&
          sourcePlace->type.arguments.front() == range.rangeType &&
          payloadRoots == 1;
    }
  }
  expect(mirRanges == 6 && exactMirPlans,
         "MIR should retain checked Expected extraction, Invoke failure, and "
         "one exact payload loan root for every range");

  lang::HirProgram invalidHir = frontend.hir;
  bool removedHirAuthorization = false;
  auto &invalidHirFunctions =
      const_cast<std::vector<lang::HirFunctionInstance> &>(
          invalidHir.functionInstances());
  for (lang::HirFunctionInstance &function : invalidHirFunctions) {
    if (!entryFunction(frontend, function.id))
      continue;
    for (lang::HirStatement &statement : function.body.statements) {
      // A payload range borrows its named owner; reclassifying it as an
      // owning temporary removes that semantic authorization.
      if (statement.rangeIteration &&
          payloadSource(function.body, *statement.rangeIteration) != nullptr) {
        statement.rangeIteration->ownsRangeTemporary = true;
        removedHirAuthorization = true;
        break;
      }
    }
    if (removedHirAuthorization) {
      break;
    }
  }
  expect(
      removedHirAuthorization &&
          !lang::verifyHirProgramPlans(frontend.semantics, invalidHir).valid(),
      "HIR verification should reject removal of semantic Expected payload "
      "range authorization");

  lang::MirBody missingExtraction =
      firstExpectedBody == nullptr ? lang::MirBody{} : *firstExpectedBody;
  bool detachedExtraction = false;
  for (lang::MirBlock &block : missingExtraction.blocks) {
    for (lang::MirInstruction &instruction : block.instructions) {
      // Detach the range loan from the checked value() call producing it.
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.loan == firstExpectedLoan) {
        instruction.loan.reset();
        detachedExtraction = true;
      }
    }
  }
  expect(firstExpectedBody != nullptr && detachedExtraction &&
             !lang::verifyMirBody(missingExtraction).valid(),
         "MIR verification should reject an Expected range without its "
         "checked payload extraction");

  expect(!gti_test::emitCppText(frontend).empty(),
         "the C++ backend should consume only verifier-certified Expected "
         "payload range plans");

  const std::string invalidPrefix = R"(
import std.string;

expected<std::string, int32_t> make_text() {
  return std::string("ab");
}

class box {
public:
  box(expected<std::string, int32_t> initial)
      : value(std::move(initial)) {}
  mut expected<std::string, int32_t> value;
};

)";
  // value() is an ordinary accessor, so a field receiver and a mutable
  // payload loop follow the general range rules rather than a bounded form.
  const auto generalRanges = lang::Frontend().analyze(
      "general-expected-payload-range-loan.gti",
      invalidPrefix + R"(void field_receiver(mut box& owner) {
  for (auto& byte : owner.value.value()) {}
}
void mutable_payload() {
  mut auto text = make_text();
  for (mut auto& byte : text.value()) { byte = 'z'; }
}
int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(generalRanges.canGenerateCode(),
         "field-receiver and mutable payload ranges use the general range "
         "rules");
  const std::pair<std::string_view, std::string_view> invalidCases[] = {
      {R"(void replace_owner() {
  mut auto text = make_text();
  for (auto& byte : text.value()) { text = make_text(); }
})",
       "Cannot assign to storage while an overlapping borrow"},
      {R"(void move_owner() {
  mut auto text = make_text();
  for (auto& byte : text.value()) { auto moved = std::move(text); }
})",
       "Cannot move storage while an overlapping borrow"},
      {R"(char& escape_element(expected<std::string, int32_t> text) {
  for (auto& byte : text.value()) { return byte; }
  return text.value()[0];
})",
       "borrowed return requires"},
  };
  for (const auto &[body, message] : invalidCases) {
    const auto invalid = lang::Frontend().analyze(
        "invalid-expected-payload-range-loan.gti",
        invalidPrefix + std::string(body) + "\nint main() { return 0; }\n",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!invalid.canGenerateCode() &&
               hasDiagnosticCode(invalid.diagnostics, "GTI-S2017") &&
               hasDiagnostic(invalid.diagnostics, std::string(message)),
           "each invalid Expected payload range retains its own lifetime "
           "diagnostic: " +
               std::string(body));
    if (body.find("void move_owner") != std::string_view::npos)
      expect(
          hasDiagnosticCode(invalid.diagnostics, "GTI-S2018"),
          "consuming a payload owner also violates loop-backedge availability");
  }

  const lang::FrontendResult owningTemporary = lang::Frontend().analyze(
      "owning-temporary-expected-payload-range.gti", R"(
import std.string;
expected<std::string, int32_t> make_text() {
  return std::string("ab");
}
int main() {
  mut int count = 0;
  for (auto& byte : make_text().value()) { count += 1; }
  return count - 2;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  bool exactOwningTemporary = false;
  for (const lang::HirFunctionInstance &function :
       owningTemporary.hir.functionInstances()) {
    for (const lang::HirStatement &statement : function.body.statements) {
      if (statement.rangeIteration) {
        exactOwningTemporary = exactOwningTemporary ||
                               statement.rangeIteration->ownsRangeTemporary;
      }
    }
  }
  expect(owningTemporary.canGenerateCode() && exactOwningTemporary,
         "an rvalue Expected payload should remain an owning range temporary, "
         "not masquerade as a retained named-owner projection");
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
  bool fixedArrayLoanShape = true;
  for (const lang::HirFunctionInstance &function :
       fixedArrays.hir.functionInstances()) {
    for (const lang::HirStatement &statement : function.body.statements) {
      if (!statement.rangeIteration ||
          statement.rangeIteration->elementAccess !=
              lang::AccessMode::Mutable) {
        continue;
      }
      const lang::HirRangeIteration &range = *statement.rangeIteration;
      const lang::HirLoan *rangeLoan = function.body.findLoan(range.rangeLoan);
      const lang::HirLoan *elementLoan =
          function.body.findLoan(range.elementLoan);
      foundMutableFixedArray =
          foundMutableFixedArray ||
          range.strategy == lang::RangeIterationStrategy::FixedArray;
      fixedArrayLoanShape = fixedArrayLoanShape && rangeLoan != nullptr &&
                            rangeLoan->access == lang::AccessMode::Mutable &&
                            elementLoan != nullptr &&
                            elementLoan->access == lang::AccessMode::Mutable &&
                            elementLoan->parent == range.rangeLoan;
    }
  }
  expect(foundMutableFixedArray && fixedArrayLoanShape,
         "HIR should retain a mutable fixed-array range loan and one exact "
         "mutable child element loan");

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

  const lang::OptimizationResult fixedArrayOptimizations =
      lang::OptimizationPipeline().run(fixedArrays.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact fixedArrayArtifact = gti_test::emitCpp(
      fixedArrays, fixedArrays.mir, fixedArrays.mir, fixedArrayOptimizations);
  expect(fixedArrayArtifact.contents.find("GTI verified-MIR body") !=
                 std::string::npos &&
             fixedArrayArtifact.contents.find(
                 "std::int32_t *__gti_mir_loan_") != std::string::npos,
         "the C++ backend should consume mutable fixed-array loans from the "
         "verified MIR plan");

  lang::HirProgram invalidMutableHir = fixedArrays.hir;
  bool corruptedMutableHir = false;
  auto &invalidMutableHirFunctions =
      const_cast<std::vector<lang::HirFunctionInstance> &>(
          invalidMutableHir.functionInstances());
  for (lang::HirFunctionInstance &function : invalidMutableHirFunctions) {
    for (const lang::HirStatement &statement : function.body.statements) {
      if (!statement.rangeIteration ||
          statement.rangeIteration->elementAccess !=
              lang::AccessMode::Mutable) {
        continue;
      }
      for (lang::HirLoan &loan : function.body.loans) {
        if (loan.semanticLoan == statement.rangeIteration->elementLoan) {
          loan.parent = 0;
          corruptedMutableHir = true;
          break;
        }
      }
      if (corruptedMutableHir) {
        break;
      }
    }
    if (corruptedMutableHir) {
      break;
    }
  }
  expect(corruptedMutableHir && !lang::verifyHirProgramPlans(
                                     fixedArrays.semantics, invalidMutableHir)
                                     .valid(),
         "HIR verification should reject a mutable element loan detached "
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
  for (const lang::HirFunctionInstance &function :
       standardVector.hir.functionInstances()) {
    for (const lang::HirStatement &statement : function.body.statements) {
      if (!statement.rangeIteration ||
          statement.rangeIteration->elementAccess !=
              lang::AccessMode::Mutable) {
        continue;
      }
      const lang::HirRangeIteration &range = *statement.rangeIteration;
      const lang::HirLoan *rangeLoan = function.body.findLoan(range.rangeLoan);
      const lang::HirLoan *elementLoan =
          function.body.findLoan(range.elementLoan);
      foundMutableVector =
          range.strategy == lang::RangeIterationStrategy::Structural &&
          rangeLoan != nullptr &&
          rangeLoan->access == lang::AccessMode::Mutable &&
          std::find(rangeLoan->carriers.begin(), rangeLoan->carriers.end(),
                    range.iterator) != rangeLoan->carriers.end() &&
          elementLoan != nullptr &&
          elementLoan->access == lang::AccessMode::Mutable &&
          elementLoan->parent == range.rangeLoan;
    }
  }
  expect(foundMutableVector,
         "the mutable vector iterator should be a confined carrier of the "
         "range loan and dereference to one mutable child loan");

  const lang::OptimizationResult vectorOptimizations =
      lang::OptimizationPipeline().run(standardVector.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact vectorArtifact =
      gti_test::emitCpp(standardVector, standardVector.mir, standardVector.mir,
                        vectorOptimizations);
  bool checkedMutableRead = false;
  for (const auto &function : standardVector.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        checkedMutableRead |=
            instruction.intrinsic == lang::IntrinsicKind::RawBorrowMut;
  expect(vectorArtifact.contents.find("vector_mut_iterator") !=
                 std::string::npos &&
             vectorArtifact.contents.find("begin_mut__gti_mir_failure") !=
                 std::string::npos &&
             checkedMutableRead,
         "the C++ backend should emit the mutable iterator's transferred "
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
    std::vector<int32_t> moved = std::move(values);
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("auto __gti_mir_closure_factory_") !=
              std::string::npos &&
          artifact.contents.find("using __gti_mir_closure_type_") !=
              std::string::npos &&
          artifact.contents.find("mir_lifetime_slot<__gti_mir_closure_type_") !=
              std::string::npos &&
          artifact.contents.find(".construct(__gti_mir_p_2.get())") !=
              std::string::npos &&
          artifact.contents.find("return __gti_mir_closure_environment_") !=
              std::string::npos &&
          artifact.contents.find("using __gti_mir_lambda_identity = ") !=
              std::string::npos &&
          artifact.contents.find("GTI verified-MIR body: scalar-cfg-failure-v1 "
                                 "lambda-instance 3") != std::string::npos,
      "the MIR-owned C++ backend should materialize immutable closure "
      "copies, nested value captures, and fused direct invocation");

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
  const std::string cpp20 =
      expectedLambda.canGenerateCode()
          ? gti_test::emitCppText(expectedLambda, lang::CppStandard::Cpp20)
          : std::string{};
  // The result in the lambda signature is an ordinary class instance, so the
  // backend must define it rather than rely on a built-in helper.
  const auto lambdaResults =
      concreteBackendSymbols(cpp20, "__gti_type_", "expected");
  expect(expectedLambda.canGenerateCode() &&
             std::any_of(lambdaResults.begin(), lambdaResults.end(),
                         [&](const std::string &name) {
                           return cpp20.find("class " + name + " {") !=
                                  std::string::npos;
                         }),
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
          hasDiagnostic(invalid.diagnostics, "cannot capture 'this'") &&
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("// GTI verified-MIR body: scalar-cfg-failure-v1 "
                             "function-instance") != std::string::npos &&
          artifact.contents.find("callable_capability::read_call") !=
              std::string::npos &&
          // The generic owner's bridge keeps its exact result while delaying
          // completeness checks until the owner is instantiated.
          artifact.contents.find("friend std::conditional_t<true, void, "
                                 "Replace> __gti_invoke(") !=
              std::string::npos &&
          artifact.contents.find(
              "::__gti_program::Replace<std::int32_t>::__gti_fn_") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_call__gti_mir_failure(") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_call") != std::string::npos &&
          [&] {
            const auto count = [&](std::string_view spelling) {
              std::size_t result = 0;
              for (std::size_t at = 0;
                   (at = artifact.contents.find(spelling, at)) !=
                   std::string::npos;
                   at += spelling.size())
                ++result;
              return result;
            };
            // Only compiler-generated closure environments expose a native
            // call operator. Source nominal operators still use exact bridges.
            return count(" operator()(") ==
                   count("struct __gti_mir_closure_environment_");
          }(),
      "C++ lowering should bridge exact GTI call operators without "
      "exposing native overload resolution");

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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("// GTI verified-MIR body: scalar-cfg-failure-v1 "
                             "function-instance") != std::string::npos &&
          artifact.contents.find("callable_capability::read_call") !=
              std::string::npos &&
          artifact.contents.find("friend bool __gti_invoke(") !=
              std::string::npos &&
          artifact.contents.find("::__gti_program::GreaterThan::__gti_fn_") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_call(__gti_mir_v_") !=
              std::string::npos &&
          artifact.contents.find("*__gti_mir_out_result = __gti_mir_v_4;") !=
              std::string::npos,
      "C++ lowering should preserve predicate values through the exact "
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

void testBorrowedCallableConditionals() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("borrowed-callable-conditionals.gti", R"(
class Counter {
public:
  mut int value = 0;
  Counter() {}
  Counter(Counter& other) = delete;
  Counter(Counter&& other) = default;
  int operator()(int n) mut { this.value += n; return this.value; }
};
int call_mut<F>(mut F& f) { return f(2); }
int relay<F>(mut F& f) { return call_mut(f); }
int choose<F>(F& f, bool ok) {
  auto result = ok ? f(4) : 7;
  return result;
}
int choose_reverse<F>(F& f, bool ok) {
  auto result = ok ? 7 : f(4);
  return result;
}
int choose_both<F>(F& f, bool ok) {
  auto result = ok ? f(4) : f(7);
  return result;
}
void choose_void<F>(F& f, bool ok) { ok ? f(1) : f(2); }
T choose_nested<F, T>(F& f, T fallback, bool ok) {
  auto result = (ok ? (ok ? f(4) : fallback) : f(7));
  return result;
}
int main() {
  mut Counter counter{};
  int value = relay(counter);
  auto f = [](int n) -> int { return n + 1; };
  auto noop = [](int n) -> void {};
  choose_void(noop, true);
  auto floating = [](int n) -> double { return 4.0d; };
  if (choose_nested(f, 0, true) != 5 ||
      choose_nested(floating, 0.0d, false) != 4.0d) { return 1; }
  return value + counter.value + choose(f, true) +
         choose_reverse(f, false) + choose_both(f, false) - 22;
}
)");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "borrowed callables and deferred exact conditional joins must lower");
  if (!frontend.canGenerateCode())
    return;

  bool foundMutable = false;
  bool foundLambda = false;
  bool rejectedAccess = false;
  bool rejectedOwner = false;
  bool rejectedConsumption = false;
  for (const auto &instance : frontend.mir.functionInstances()) {
    for (const auto &parameter : instance.callableParameters) {
      expect(parameter.callableType.kind == lang::SemanticType::Reference &&
                 parameter.boundary == lang::CallableBoundary::Confined &&
                 !parameter.ownedTransport,
             "borrowed callable metadata must retain the reference boundary");
      foundMutable |= parameter.access == lang::AccessMode::Mutable;
      foundLambda |= parameter.callableType.arguments.front().kind ==
                     lang::SemanticType::Lambda;
      for (const auto &signature : parameter.signatures)
        expect(signature.returnType != lang::SemanticType::Unknown &&
                   (signature.lambdaTarget || signature.functionTarget),
               "every inferred signature must be concrete before MIR");
      if (parameter.signatures.empty())
        continue;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(instance.id));
        auto &contract = changed->callableParameters.front();
        if (mutation == 0)
          contract.access = parameter.access == lang::AccessMode::Mutable
                                ? lang::AccessMode::ReadOnly
                                : lang::AccessMode::Mutable;
        else if (mutation == 1)
          changed->parameterBindings[parameter.parameterIndex] = 999999;
        else
          contract.signatures.front().requiredCapability =
              lang::CallableInvocationCapability::Once;
        const bool rejected = !lang::verifyMirProgram(malformed).valid();
        if (mutation == 0)
          rejectedAccess |= rejected;
        if (mutation == 1)
          rejectedOwner |= rejected;
        if (mutation == 2)
          rejectedConsumption |= rejected;
        expect(rejected, "forged borrowed callable contract must fail MIR");
      }
    }
  }
  expect(foundMutable && foundLambda && rejectedAccess && rejectedOwner &&
             rejectedConsumption,
         "test both concrete callable families and independent contract "
         "mutations");

  const auto reject = [](std::string_view name, std::string source,
                         std::string_view fragment) {
    const auto invalid = analyzeWithMemory(name, std::move(source));
    const bool rejected =
        !invalid.canGenerateCode() &&
        !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001") &&
        hasDiagnostic(invalid.diagnostics, std::string(fragment));
    if (!rejected) {
      std::cerr << name << " expected " << fragment << '\n';
      for (const auto &diagnostic : invalid.diagnostics)
        std::cerr << diagnostic.message << '\n';
    }
    expect(rejected,
           "invalid borrowed invocation or join must fail in semantics");
  };
  reject("mismatched-conditional.gti", R"(
int choose<F>(F& f) { auto result = true ? f(1) : 2; return result; }
int main() {
  auto f = [](int n) -> bool { return true; };
  return choose(f);
})",
         "same exact type");
  reject("borrowed-mutable-call.gti", R"(
class Counter { public: int operator()(int n) mut { return n; }  public: Counter() = default; };
int apply<F>(F& f) { return f(1); }
int main() { Counter counter{}; return apply(counter); }
)",
         "read-callable");
  reject("borrowed-once-call.gti", R"(
int apply<F>(mut F& f) { return std::move(f)(1); }
int main() { return 0; }
)",
         "reference");
  reject("borrowed-callable-escape.gti", R"(
F& escape<F>(F& f) { int n = f(1); return f; }
int main() { return 0; }
)",
         "cannot escape");
  reject("borrowed-callable-capture.gti", R"(
void capture<F>(F& f) {
  int n = f(1);
  auto retained = [f]() -> int { return 1; };
}
int main() { return 0; }
)",
         "cannot be captured");
  reject("borrowed-result-conditional.gti", R"(
class Source { int value = 1; public: int& operator()(int n) { return this.value; }  public: Source() = default; };
int apply<F>(F& f) { auto result = true ? f(1) : 2; return result; }
int main() { Source source{}; return apply(source); }
)",
         "without references");
  reject("borrowed-callable-upgrade.gti", R"(
int apply<F>(mut F& f) { return f(1); }
int relay<F>(F& f) { return apply(f); }
int main() { auto f = [](int n) -> int { return n; }; return relay(f); }
)",
         "mutable");
  reject("borrowed-callable-overlap.gti", R"(
class Counter { public: int operator()(int n) mut { return n; }  public: Counter() = default; };
int apply<F>(mut F& first, mut F& second) {
  int a = first(1); int b = second(2); return a + b;
}
int main() { mut Counter f{}; return apply(f, f); }
)",
         "overlapping reference accesses");
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
             mirDump.starts_with("mir-v119 ") &&
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

  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  for (const std::string *cpp : {&cpp23, &cpp20}) {
    // Each of the three result types is an emitted instance of the prelude
    // class, and unexpected() is its ordinary source factory.
    const auto results =
        concreteBackendSymbols(*cpp, "__gti_type_", "expected");
    const auto defined = std::count_if(
        results.begin(), results.end(), [&](const std::string &name) {
          return cpp->find("class " + name + " {") != std::string::npos &&
                 cpp->find("::__gti_program::" + name) != std::string::npos;
        });
    expect(cpp->find("#include <expected>") == std::string::npos &&
               cpp->find("nonstd/expected.hpp") == std::string::npos &&
               cpp->find("::nonstd::") == std::string::npos &&
               cpp->find("std::expected<") == std::string::npos &&
               cpp->find("::gti_internal::backend::expected") ==
                   std::string::npos &&
               results.size() == 3 && defined == 3 &&
               containsMangledFunction(*cpp, "::__gti_program::", "unexpected",
                                       ""),
           "both C++ standards should lower expected values to GTI's own "
           "representation rather than a host or vendored library");
  }
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
    expect(!gti_test::emitCppText(nested).empty(),
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
  std::size_t scalarExtractions = 0;
  for (const auto &loan : frontend.semantics.loans()) {
    const auto *call = dynamic_cast<const lang::Call *>(loan.origin);
    const auto *projection =
        call == nullptr ? nullptr : frontend.semantics.findCall(*call);
    // value() is an ordinary member of the prelude class.
    const lang::FunctionInfo *member =
        projection == nullptr
            ? nullptr
            : frontend.semantics.findFunction(projection->function);
    const lang::ClassTypeInfo *owner =
        member == nullptr || member->ownerClass == 0
            ? nullptr
            : frontend.semantics.findClassType(member->ownerClass);
    if (owner != nullptr && owner->qualifiedName == "expected" &&
        projection->declaration != nullptr &&
        projection->declaration->name().lexeme == "value" &&
        projection->returnType.kind == lang::SemanticType::Reference &&
        projection->returnType.arguments.size() == 1 &&
        projection->returnType.arguments.front().kind ==
            lang::SemanticType::Int32) {
      ++scalarExtractions;
      expect(loan.storedDependency == 0,
             "extracting a scalar fallible borrow retains the external "
             "referent, not a physical shell dependency on its carrier");
    }
  }
  expect(scalarExtractions == 1,
         "the fixture retains one scalar fallible-borrow extraction");

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

  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  // The checked-reference profile keeps its success in an ordinary payload
  // alternative, natively a rebindable wrapper rather than a reference member.
  const auto pointerBacked = [](const std::string &cpp) {
    return cpp.find("std::reference_wrapper<const std::int32_t> "
                    "__gti_field_0") != std::string::npos &&
           cpp.find("expected<std::int32_t &") == std::string::npos &&
           cpp.find("::gti_internal::backend::borrowed_expected") ==
               std::string::npos;
  };
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
  expect(pointerBacked(cpp23) && pointerBacked(cpp20) && verifiedSuccesses == 5,
         "both native standards should use GTI's private pointer-backed "
         "fallible-borrow carrier and initialize passive or owning-error "
         "representations from a verified loan");

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
             hasDiagnostic(liveConflict.diagnostics,
                           "overlaps a borrow that may still be live") &&
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
  expect(!ambiguousOwner.canGenerateCode() &&
             hasDiagnostic(ambiguousOwner.diagnostics,
                           "Borrowed return dependency is ambiguous") &&
             hasRelatedDiagnostic(ambiguousOwner.diagnostics,
                                  "Eligible owner dependency source"),
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

  const std::string cpp23 = gti_test::emitCppText(frontend);
  const std::string cpp20 =
      gti_test::emitCppText(frontend, lang::CppStandard::Cpp20);
  // The branches call the selected std::tryable conformance functions.
  const auto checkedBranches = [](const std::string &cpp) {
    return containsMangledFunction(cpp, "::", "has_value", "(") &&
           containsMangledFunction(cpp, "::", "take_value", "") &&
           containsMangledFunction(cpp, "::", "take_residual", "");
  };
  expect(checkedBranches(cpp23) && checkedBranches(cpp20),
         "both C++ backends should emit checked propagation branches instead "
         "of delegating the operator to C++");

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
      {"mut Client api{}; return inspect_service(api.auth());",
       "receiver permission does not admit a temporary borrowed carrier as a "
       "reference argument"},
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
  expect(!gti_test::emitCppText(frontend).empty(),
         "the C++ backend should consume verified temporary receiver stages");

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
  expect(!gti_test::emitCppText(frontend).empty(),
         "the C++ backend should consume verified virtual temporary receiver "
         "stages");

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
  expect(!gti_test::emitCppText(frontend).empty(),
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
  expect(!gti_test::emitCppText(frontend).empty(),
         "the C++ backend should consume verified temporary argument slots "
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
  expect(!borrowedState.canGenerateCode() &&
             hasDiagnosticCode(borrowedState.diagnostics, "GTI-S2017"),
         "a fresh value carrying tracked borrowed state should remain "
         "ineligible");

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
  expect(!gti_test::emitCppText(frontend).empty(),
         "the C++ backend should emit borrows of CFG-promoted temporary "
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(!artifact.contents.empty(),
         "the C++ backend should emit retained borrowed call results without "
         "declining the retained loan's place");
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(!artifact.contents.empty(),
         "the C++ backend should emit retained borrows built from another "
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
  expect(!gti_test::emitCppText(frontend).empty(),
         "the C++ backend should emit both verified transfers without "
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
  const auto conflict =
      std::find_if(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                   [&](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2017" &&
                            diagnostic.primary.start == assignment;
                   });
  expect(!rejected.canGenerateCode() && rejected.diagnostics.size() == 1 &&
             conflict != rejected.diagnostics.end() &&
             conflict->message.find("overlapping borrow may still be live") !=
                 std::string::npos,
         "a field assignment whose value is a logical expression should still "
         "report the live borrow of its root after the join");
}

// The result of a built-in assignment is a copy of its committed target. A
// move-only target has no copy, so only a discarded assignment may store one:
// every use of the result is GTI-S2003 at the assignment operator.
void testMoveOnlyAssignmentResultUse() {
  const std::string prelude = R"(
import std.memory;
import std.utility;
class Tracked {
public:
  int32_t x;
  Tracked(int32_t x) : x(x) {}
  ~Tracked() {}
  int32_t get() { return this.x; }
};
struct Holder {
  mut Tracked t;
  Holder(Tracked t) : t(std::move(t)) {}
};
int32_t take(Tracked t) { return t.x; }
T relay<T>(mut T a, T b) { return (a = std::move(b)); }
)";
  const auto analyzeBody = [&](const std::string &name,
                               const std::string &body) {
    return lang::Frontend().analyze(name, prelude + body,
                                    {standardLibraryPrelude()}, {},
                                    {standardLibraryRoot()});
  };
  // Each used result, keyed by the assignment whose operator is reported.
  const std::vector<std::pair<std::string, std::string>> used{
      {"int32_t f(mut Tracked p, Tracked q) { Tracked r = (p = std::move(q)); "
       "return r.x; }",
       "p = std::move(q)"},
      {"Tracked f(mut Tracked p, Tracked q) { return (p = std::move(q)); }",
       "p = std::move(q)"},
      {"int32_t f(mut Tracked p, Tracked q) { return take(p = std::move(q)); }",
       "p = std::move(q)"},
      {"int32_t f(mut Tracked p, Tracked q) { return (p = std::move(q)).get(); "
       "}",
       "p = std::move(q)"},
      {"int32_t f(mut Tracked p, Tracked q) { return (p = std::move(q)).x; }",
       "p = std::move(q)"},
      {"int32_t f(mut Tracked a, mut Tracked b, Tracked q) { a = b = "
       "std::move(q); return a.x; }",
       "b = std::move(q)"},
      {"int32_t f(mut Tracked p, Tracked q) { Tracked r = (1, p = "
       "std::move(q)); return r.x; }",
       "p = std::move(q)"},
      {"int32_t f(bool c, mut Tracked p, Tracked q, Tracked r) { Tracked s = c "
       "? (p = std::move(q)) : std::move(r); return s.x; }",
       "p = std::move(q)"},
      {"int32_t f(mut Holder h, Tracked q) { return (h.t = std::move(q)).x; }",
       "h.t = std::move(q)"},
      {"int32_t f(Tracked q) { mut Tracked[1] ts = {Tracked(1)}; Tracked r = "
       "(ts[0] = std::move(q)); return r.x; }",
       "ts[0] = std::move(q)"},
      {"Tracked f(mut Tracked& p, Tracked q) { return (p = std::move(q)); }",
       "p = std::move(q)"},
      {"int32_t f(mut std::unique_ptr<int32_t> u, std::unique_ptr<int32_t> v) "
       "{ std::unique_ptr<int32_t> w = (u = std::move(v)); return *w; }",
       "u = std::move(v)"},
      {"int32_t f() { return relay(Tracked(1), Tracked(5)).x; }",
       "a = std::move(b)"}};
  for (const auto &[body, assignment] : used) {
    const std::string source = prelude + body;
    const lang::FrontendResult result =
        analyzeBody("move-only-result.gti", body);
    const std::size_t at = source.find(assignment);
    const std::size_t oper = source.find(" = ", at) + 1;
    const auto reported =
        std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const lang::Diagnostic &diagnostic) {
                        return diagnostic.code == "GTI-S2003";
                      });
    const auto *error = findDiagnosticByCode(result.diagnostics, "GTI-S2003");
    expect(!result.canGenerateCode() && reported == 1 && error != nullptr &&
               at != std::string::npos && error->primary.start == oper &&
               error->primary.end == oper + 1 &&
               error->message.find("Cannot use the result of an assignment of "
                                   "move-only type '") != std::string::npos &&
               error->message.find("it would copy the target") !=
                   std::string::npos &&
               hasDiagnosticHint(result.diagnostics,
                                 "Assign in its own statement") &&
               error->fixes.empty(),
           "a used move-only assignment result should report one GTI-S2003 "
           "at its operator: " +
               body);
  }

  // Discarded stores, and every copyable class result, stay valid.
  const std::vector<std::string> accepted{
      "int32_t f(mut Tracked p, Tracked q) { p = std::move(q); return p.x; }",
      "int32_t f(mut Tracked p, Tracked q) { (p = std::move(q)); return p.x; "
      "}",
      "int32_t f(mut Tracked p, Tracked q) { int32_t n = ((p = std::move(q)), "
      "3); return p.x + n; }",
      "int32_t f(mut Tracked p, Tracked q) { (1, p = std::move(q)); return "
      "p.x; }",
      "int32_t f(mut Tracked p) { for (mut int32_t i = 0; i < 2; p = "
      "Tracked(i)) { i++; } return p.x; }",
      "int32_t f(bool c, mut Tracked p, Tracked q, Tracked r) { c ? (p = "
      "std::move(q)) : (p = std::move(r)); return p.x; }",
      "struct Point { int32_t x; Point(int32_t x) : x(x) {} };\n"
      "int32_t f(mut Point p, Point q) { Point r = (p = q); return r.x; }"};
  for (const std::string &body : accepted) {
    const lang::FrontendResult result =
        analyzeBody("discarded-result.gti", body);
    for (const lang::Diagnostic &diagnostic : result.diagnostics)
      std::cerr << "Unexpected discarded-result diagnostic: "
                << diagnostic.message << '\n';
    expect(result.diagnostics.empty(),
           "a discarded move-only store or a copyable result should be "
           "accepted: " +
               body);
  }
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
    {"testProjectedFieldMoves", testProjectedFieldMoves},
    {"testIndexedPlaceMovesAndInitialization",
     testIndexedPlaceMovesAndInitialization},
    {"testExclusiveReborrowLoanGraph", testExclusiveReborrowLoanGraph},
    {"testDisjointFieldReceiverBorrowsThroughReferenceParameters",
     testDisjointFieldReceiverBorrowsThroughReferenceParameters},
    {"testNonNullReferences", testNonNullReferences},
    {"testGlobalReferencePlaces", testGlobalReferencePlaces},
    {"testGlobalBorrowReturns", testGlobalBorrowReturns},
    {"testPrefixStorageCapability", testPrefixStorageCapability},
    {"testReceiverTiedReferenceReturns", testReceiverTiedReferenceReturns},
    {"testArgumentTiedReferenceReturns", testArgumentTiedReferenceReturns},
    {"testStoredReferenceGroundwork", testStoredReferenceGroundwork},
    {"testMutableStoredDependencies", testMutableStoredDependencies},
    {"testComposedCarrierFunctionTransfer",
     testComposedCarrierFunctionTransfer},
    {"testStoredCarrierReturnProvenance", testStoredCarrierReturnProvenance},
    {"testNestedBorrowedFields", testNestedBorrowedFields},
    {"testNestedMemberReferenceReturnForwarding",
     testNestedMemberReferenceReturnForwarding},
    {"testProjectedAccessorReturnLoanOrder",
     testProjectedAccessorReturnLoanOrder},
    {"testReferenceArgumentRuntimePlaceCoherence",
     testReferenceArgumentRuntimePlaceCoherence},
    {"testCallSiblingArgumentBorrowConflicts",
     testCallSiblingArgumentBorrowConflicts},
    {"testForwardIteratorContractsAndBinarySearch",
     testForwardIteratorContractsAndBinarySearch},
    {"testWritableAndMovementIteratorCapabilities",
     testWritableAndMovementIteratorCapabilities},
    {"testCompleteRangeConceptsAndAlgorithms",
     testCompleteRangeConceptsAndAlgorithms},
    {"testGeneralOwnerDependentReturns", testGeneralOwnerDependentReturns},
    {"testSwitchAndBreakLoanFlow", testSwitchAndBreakLoanFlow},
    {"testSharedReadOnlyLoanAliases", testSharedReadOnlyLoanAliases},
    {"testUniqueOwnershipAndAllocation", testUniqueOwnershipAndAllocation},
    {"testUniqueOwnerSwap", testUniqueOwnerSwap},
    {"testPolymorphicUniqueOwnerUpcast", testPolymorphicUniqueOwnerUpcast},
    {"testSourceDefinedStorage", testSourceDefinedStorage},
    {"testFixedArrayPackForwarding", testFixedArrayPackForwarding},
    {"testVariadicStorageConstructionAndVector",
     testVariadicStorageConstructionAndVector},
    {"testAggregateOwnershipTraits", testAggregateOwnershipTraits},
    {"testDefaultedDefaultLifecycle", testDefaultedDefaultLifecycle},
    {"testDestructorsAndActiveDropState", testDestructorsAndActiveDropState},
    {"testRestrictedMemberOperators", testRestrictedMemberOperators},
    {"testArrowNonClassReferents", testArrowNonClassReferents},
    {"testCallableMemberOperators", testCallableMemberOperators},
    {"testConsumingCallableOperators", testConsumingCallableOperators},
    {"testConsumingMethodsAndValueOperators",
     testConsumingMethodsAndValueOperators},
    {"testRangeBasedForAndIteratorProtocol",
     testRangeBasedForAndIteratorProtocol},
    {"testReadOnlyRangeLoans", testReadOnlyRangeLoans},
    {"testPassiveCleanupRangeTemporary", testPassiveCleanupRangeTemporary},
    {"testSharedPayloadSiblingReborrows", testSharedPayloadSiblingReborrows},
    {"testExpectedPayloadRangeLoans", testExpectedPayloadRangeLoans},
    {"testSourceOwnedExpectedRangeOrigins",
     testSourceOwnedExpectedRangeOrigins},
    {"testMutableRangeLoans", testMutableRangeLoans},
    {"testLambdas", testLambdas},
    {"testConfinedCallableParameters", testConfinedCallableParameters},
    {"testConfinedCallableResults", testConfinedCallableResults},
    {"testBorrowedCallableConditionals", testBorrowedCallableConditionals},
    {"testOwnedCallableTransport", testOwnedCallableTransport},
    {"testExpectedValues", testExpectedValues},
    {"testFallibleBorrows", testFallibleBorrows},
    {"testExpectedPropagationOperator", testExpectedPropagationOperator},
    {"testFieldInitializerRollbackAndFailureEdges",
     testFieldInitializerRollbackAndFailureEdges},
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
    {"testMoveOnlyAssignmentResultUse", testMoveOnlyAssignmentResultUse},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_ownership_tests");
  return runNamedTests(argc, argv, kTests, "ownership");
}
