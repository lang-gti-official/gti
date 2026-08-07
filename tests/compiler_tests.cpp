#include "gti/ast_printer.h"
#include "gti/backend.h"
#include "gti/cpp_backend.h"
#include "gti/cpp_emitter.h"
#include "gti/executable_path.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/optimizer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasDiagnostic(const lang::SemanticVisitor &semantic,
                   const std::string &text) {
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                   const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                       const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

std::size_t countDiagnosticCode(const lang::SemanticVisitor &semantic,
                                const std::string &code) {
  std::size_t count = 0;
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

void testFrontendBackendAndOptimizationPipeline() {
  const std::string source = R"(
int main() {
  bool folded = (1 < 2) and !false;
  int arithmetic = 1 + 2;
  if (folded) {
    return 0;
  }
  return arithmetic;
}
)";
  lang::FrontendResult frontend =
      lang::Frontend().analyze("frontend.gti", source);
  expect(frontend.canGenerateCode(),
         "the shared frontend should produce a checked program");
  expect(frontend.diagnostics.empty(),
         "valid frontend input should not produce diagnostics");
  expect(frontend.semantics.expressionCount() > 0,
         "the frontend should retain expression type information");

  const lang::OptimizationResult unoptimized = lang::OptimizationPipeline().run(
      frontend.program, frontend.semantics, lang::OptimizationLevel::O0);
  expect(unoptimized.foldedExpressionCount() == 0,
         "-O0 should not rewrite expressions");

  const lang::OptimizationResult optimized = lang::OptimizationPipeline().run(
      frontend.program, frontend.semantics, lang::OptimizationLevel::O1);
  expect(optimized.foldedExpressionCount() > 0,
         "-O1 should record safe constant expressions");

  std::unique_ptr<lang::Backend> backend = std::make_unique<lang::CppBackend>();
  const lang::BackendArtifact artifact =
      backend->generate({.program = frontend.program,
                         .semantics = frontend.semantics,
                         .optimizations = optimized});
  expect(backend->name() == "cpp" &&
             artifact.kind == lang::BackendArtifactKind::Source &&
             artifact.extension == ".cpp",
         "the C++ emitter should be available through the backend contract");
  expect(artifact.contents.find("const bool folded = true") !=
             std::string::npos,
         "the C++ backend should consume optimization results");
  expect(
      artifact.contents.find("const std::int32_t arithmetic = (1 + 2)") !=
          std::string::npos,
      "arithmetic should remain unfolded until overflow semantics are defined");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-frontend.gti", "int main() { return 0 }");
  expect(!invalid.canGenerateCode() && !invalid.syntaxValid,
         "the frontend should block code generation after syntax errors");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-P0001"),
         "the shared frontend should retain parser diagnostics");

  const std::filesystem::path overlayDirectory =
      std::filesystem::temp_directory_path();
  const std::filesystem::path overlayEntry =
      overlayDirectory / "gti-source-overlay-entry.gti";
  const std::filesystem::path overlayDependency =
      overlayDirectory / "gti-source-overlay-dependency.gti";
  const std::string dependencyKey =
      std::filesystem::weakly_canonical(overlayDependency).string();
  const lang::FrontendResult overlaid = lang::Frontend().analyze(
      overlayEntry,
      "include \"gti-source-overlay-dependency.gti\"\n"
      "int main() { return dependency_value; }\n",
      {}, {{dependencyKey, "int dependency_value = 0;\n"}});
  expect(overlaid.canGenerateCode() && overlaid.diagnostics.empty(),
         "the frontend should analyze unsaved included-source overlays");
}

void testOwnershipSemanticFoundation() {
  const lang::SemanticType reference = lang::SemanticType::referenceTo(
      lang::SemanticType::Int32, lang::AccessMode::Mutable);
  const lang::SemanticType unique =
      lang::SemanticType::uniquePointerTo(lang::SemanticType::Int32);
  const lang::SemanticType shared =
      lang::SemanticType::sharedPointerTo(lang::SemanticType::Int32);
  const lang::SemanticType storage =
      lang::SemanticType::storageOf(lang::SemanticType::Int32);
  const lang::SemanticTypeTraits referenceTraits =
      lang::semanticTraits(reference);
  const lang::SemanticTypeTraits uniqueTraits = lang::semanticTraits(unique);
  const lang::SemanticTypeTraits sharedTraits = lang::semanticTraits(shared);
  const lang::SemanticTypeTraits storageTraits = lang::semanticTraits(storage);
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
  expect(storageTraits.ownership == lang::OwnershipKind::Unique &&
             storageTraits.drop == lang::DropKind::Lexical &&
             !storageTraits.copyable && storageTraits.movable,
         "compiler-private storage should be a move-only lexical owner");

  lang::FrontendResult frontend =
      lang::Frontend().analyze("ownership-foundation.gti", R"(
struct Counter {
  mut int value = 0;
  void bump() mut { self.value += 1; }
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
  expect(frontend.semantics.bindingCount() == 6,
         "the semantic model should retain fields, parameters, and locals");

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

void testNonNullReferences() {
  const std::string source = R"(
struct Counter {
public:
  mut int value = 0;
  void bump() mut { self.value += 1; }
};

int read(Counter& counter) { return counter.value; }
void increment(mut Counter& counter) { counter.bump(); }

int main() {
  mut Counter counter = Counter();
  Counter& read_only = counter;
  mut Counter& writable = counter;
  increment(writable);

  mut int value = 1;
  mut int& alias = value;
  alias += 2;
  if (read(read_only) == 1 and value == 3) {
    return 0;
  }
  return 1;
}
)";

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
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("const Counter &counter") != std::string::npos &&
          artifact.contents.find("Counter &counter") != std::string::npos &&
          artifact.contents.find("std::int32_t &alias = value") !=
              std::string::npos,
      "references should lower to C++ references with matching const access");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-references.gti", R"(
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
  expected<int&, string> nested = 1;
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
                    "References cannot be used as a function return type") &&
          hasDiagnostic(invalid.diagnostics,
                        "References cannot be used as a storage") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot bind a reference to a temporary") &&
          hasDiagnostic(invalid.diagnostics, "requires a mutable value") &&
          hasDiagnostic(invalid.diagnostics,
                        "mutable reference requires a mutable initializer") &&
          hasDiagnostic(invalid.diagnostics,
                        "Reference bindings require an initializer") &&
          hasDiagnostic(invalid.diagnostics, "References to fixed arrays") &&
          hasDiagnostic(invalid.diagnostics, "References cannot be nested") &&
          hasDiagnostic(invalid.diagnostics, "derived from temporary storage"),
      "reference diagnostics should identify each rejected lifetime rule");

  const std::string formatted = lang::Formatter().format(
      "void inspect(int& value){}void modify(mut int& value){}"
      "int main(){mut int value=1;mut int& alias=value;modify(alias);return "
      "0;}");
  expect(lang::Formatter().format(formatted) == formatted,
         "reference syntax formatting should be idempotent");
}

void testSelfTiedReferenceReturns() {
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}

  T& get() {
    return self.value;
  }
};

class Buffer<T> {
  mut gti_internal::storage<T> data;

public:
  Buffer(uint64 capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  void push(T value) mut {
    gti_internal::storage_construct(self.data, uint64(0), value);
  }

  T& at(uint64 index) {
    return gti_internal::storage_read(self.data, index);
  }
};

int main() {
  Box<int> box = Box<int>(7);
  int& boxed = box.get();
  mut Buffer<int> buffer = Buffer<int>(uint64(1));
  buffer.push(9);
  int& stored = buffer.at(uint64(0));
  return boxed + stored - 16;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("self-tied-references.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected self-tied reference diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "methods should return read-only references borrowed from self");

  const auto *mainFunction = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(2).get());
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
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("const T &") != std::string::npos &&
             artifact.contents.find("inline const T &storage_read") !=
                 std::string::npos &&
             artifact.contents.find("const std::int32_t &boxed") !=
                 std::string::npos,
         "self-tied borrows should lower to const C++ references");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-reference-returns.gti", R"(
int& escape(int& value) { return value; }

class InvalidReferences {
  int value = 1;

public:
  int& local() {
    int temporary = 2;
    return temporary;
  }

  int& parameter(int& value) {
    return value;
  }

  int& literal() {
    return 3;
  }
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
                    "References cannot be used as a function return type") &&
          hasDiagnostic(invalid.diagnostics,
                        "Method reference returns must borrow from self") &&
          hasDiagnostic(invalid.diagnostics,
                        "Reference return requires an addressable value") &&
          hasDiagnostic(invalid.diagnostics, "derived from temporary storage"),
      "reference-return diagnostics should explain each rejected lifetime");

  const lang::FrontendResult invalidated =
      lang::Frontend().analyze("invalidated-reference.gti", R"(
class Buffer<T> {
  mut gti_internal::storage<T> data;

public:
  Buffer(uint64 capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  T& at(uint64 index) {
    return gti_internal::storage_read(self.data, index);
  }

  void clear(uint64 index) mut {
    gti_internal::storage_destroy(self.data, index);
  }

  int invalidate_self(uint64 index) mut {
    int& value = gti_internal::storage_read(self.data, index);
    gti_internal::storage_destroy(self.data, index);
    return value;
  }
};

int main() {
  mut Buffer<int> buffer = Buffer<int>(uint64(1));
  int& value = buffer.at(uint64(0));
  buffer.clear(uint64(0));
  Buffer<int> moved = std::move(buffer);
  return value;
}
)");
  expect(!invalidated.canGenerateCode(),
         "move-only receivers must remain stable while borrowed");
  expect(hasDiagnostic(invalidated.diagnostics,
                       "Mutable method cannot use move-only storage") &&
             hasDiagnostic(invalidated.diagnostics,
                           "Cannot move storage while a reference borrowed") &&
             hasDiagnostic(invalidated.diagnostics,
                           "cannot mutate self storage while a reference"),
         "borrow diagnostics should prevent receiver invalidation");
}

void testUniqueOwnershipAndAllocation() {
  const std::string source = R"(
struct Widget {
public:
  mut int value = 0;

  Widget(int initial) : value(initial) {}
  int read() { return self.value; }
  void increment() mut { self.value += 1; }
};

int inspect(Widget& widget) { return widget.read(); }

std::unique_ptr<Widget> create(int value) {
  std::unique_ptr<Widget> widget = std::make_unique<Widget>(value);
  return std::move(widget);
}

int main() {
  mut std::unique_ptr<Widget> widget = create(4);
  widget->increment();
  if (widget and widget != nullptr and inspect(*widget) == 5) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("unique-ownership.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected unique-owner diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(), "unique allocation, checked access, and "
                                     "explicit transfer should validate");

  const auto *create = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(2).get());
  const auto *local = create == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                create->body()->statements().front().get());
  const lang::BindingInfo *binding =
      local == nullptr ? nullptr : frontend.semantics.findBinding(*local);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::UniquePointer &&
             binding->traits.ownership == lang::OwnershipKind::Unique &&
             !binding->traits.copyable && binding->traits.movable &&
             binding->traits.drop == lang::DropKind::Lexical,
         "allocated owners should retain move-only lexical ownership metadata");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("std::unique_ptr<Widget>") !=
                 std::string::npos &&
             artifact.contents.find(
                 "gti_internal::backend::make_unique<Widget>(value)") !=
                 std::string::npos &&
             artifact.contents.find("return std::move(widget)") !=
                 std::string::npos &&
             artifact.contents.find(
                 "gti_internal::backend::owner_access(widget)") !=
                 std::string::npos &&
             artifact.contents.find("const std::unique_ptr<Widget>") ==
                 std::string::npos,
         "the C++ backend should use RAII without physically const move-only "
         "owners");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-unique-ownership.gti", R"(
struct Widget {
public:
  int value = 0;
  Widget() {}
  int read() { return self.value; }
};

void consume(std::unique_ptr<Widget> widget) {}

std::unique_ptr<Widget> return_copy(std::unique_ptr<Widget> widget) {
  return widget;
}

std::unique_ptr<Widget> global_owner = nullptr;

struct InvalidStorage {
  std::unique_ptr<Widget> field = nullptr;
};

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
  std::unique_ptr<Widget>& owner_reference = moved;
  std::unique_ptr<Widget> generic = identity(std::move(moved));
  std::unique_ptr<int> primitive = std::make_unique<int>(1);
  return after_move + maybe_moved + wrong_member;
}
)");
  expect(!invalid.canGenerateCode(),
         "copies, invalid storage, and unsafe unique-owner use should fail");
  expect(
      hasDiagnostic(invalid.diagnostics,
                    "Cannot return a value of type 'std::unique_ptr") &&
          hasDiagnostic(invalid.diagnostics,
                        "Unique owners can only be local bindings") &&
          hasDiagnostic(invalid.diagnostics, "would copy a unique owner") &&
          hasDiagnostic(invalid.diagnostics, "already been moved") &&
          hasDiagnostic(invalid.diagnostics,
                        "may have been moved on another control-flow path") &&
          hasDiagnostic(invalid.diagnostics,
                        "std::move requires a named unique owner") &&
          hasDiagnostic(invalid.diagnostics, "Unique-owner members use '->'") &&
          hasDiagnostic(invalid.diagnostics,
                        "Unique owner bindings require an initializer") &&
          hasDiagnostic(invalid.diagnostics,
                        "References to unique owners are not supported") &&
          hasDiagnostic(
              invalid.diagnostics,
              "Generic functions cannot be instantiated with unique") &&
          hasDiagnostic(invalid.diagnostics, "requires a class or struct type"),
      "unique-owner diagnostics should cover transfer, flow, and surface "
      "limits");

  const std::string formatted = lang::Formatter().format(
      "std::unique_ptr<Widget> make(){return std::make_unique<Widget>();}"
      "int read(mut std::unique_ptr<Widget> value){return value->read();}");
  expect(formatted.find("value->read()") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "unique-owner spelling should format idempotently");
}

void testCompilerPrivateStorage() {
  const std::string source = R"(
class Buffer<T> {
  mut gti_internal::storage<T> data;
  mut uint64 count = 0;

public:
  Buffer(uint64 capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  uint64 capacity() {
    return gti_internal::storage_capacity(self.data);
  }

  void push(T value) mut {
    gti_internal::storage_construct(self.data, self.count, value);
    self.count++;
  }

  T& at(uint64 index) {
    return gti_internal::storage_read(self.data, index);
  }

  void grow(uint64 capacity) mut {
    mut gti_internal::storage<T> replacement =
        gti_internal::allocate_storage<T>(capacity);
    gti_internal::storage_relocate(self.data, replacement, self.count);
    self.data = std::move(replacement);
  }

  void pop() mut {
    self.count--;
    gti_internal::storage_destroy(self.data, self.count);
  }
};

int main() {
  mut Buffer<int> values = Buffer<int>(uint64(2));
  values.push(7);
  values.push(9);
  values.grow(uint64(4));
  if (values.capacity() == 4 and values.at(uint64(0)) == 7 and
      values.at(uint64(1)) == 9) {
    values.pop();
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("internal-storage.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected internal-storage diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "compiler-private storage should support vector-style growth and "
         "element lifetime operations");

  const auto *buffer = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().front().get());
  const auto *field = buffer == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                buffer->members().front().get());
  const lang::BindingInfo *binding =
      field == nullptr ? nullptr : frontend.semantics.findBinding(*field);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Storage &&
             binding->traits.ownership == lang::OwnershipKind::Unique &&
             !binding->traits.copyable && binding->traits.movable,
         "storage fields should retain move-only ownership metadata");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("gti_internal::backend::storage<T> data") !=
              std::string::npos &&
          artifact.contents.find(
              "gti_internal::backend::allocate_storage<T>(capacity)") !=
              std::string::npos &&
          artifact.contents.find("gti_internal::backend::storage_relocate") !=
              std::string::npos &&
          artifact.contents.find("std::construct_at") != std::string::npos &&
          artifact.contents.find("std::destroy_at") != std::string::npos,
      "the C++ backend should lower storage through its private RAII helper");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-internal-storage.gti", R"(
gti_internal::storage<int> global =
    gti_internal::allocate_storage<int>(uint64(1));

int main() {
  mut gti_internal::storage<int> values =
      gti_internal::allocate_storage<int>(uint64(2));
  gti_internal::storage_construct(values, 0, 1);
  gti_internal::storage_construct(values, uint64(0), true);
  gti_internal::storage<int> copied = values;
  gti_internal::storage<int> moved = std::move(values);
  uint64 capacity = gti_internal::storage_capacity(values);
  return int(capacity);
}
)");
  expect(!invalid.canGenerateCode(),
         "storage misuse should be rejected before backend generation");
  expect(
      hasDiagnosticCode(invalid.diagnostics, "GTI-S2019") &&
          hasDiagnostic(invalid.diagnostics,
                        "only be used as a local binding or class field") &&
          hasDiagnostic(invalid.diagnostics, "requires a uint64") &&
          hasDiagnostic(invalid.diagnostics, "this storage contains 'int32'") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'copied'") &&
          hasDiagnostic(invalid.diagnostics, "has already been moved"),
      "storage diagnostics should cover placement, exact types, copying, "
      "and use after move");
}

void testAggregateOwnershipTraits() {
  const std::string source = R"(
class Buffer<T> {
  mut gti_internal::storage<T> data;

public:
  Buffer(uint64 capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  uint64 capacity() {
    return gti_internal::storage_capacity(self.data);
  }
};

class NestedBuffer {
  Buffer<int> buffer;

public:
  NestedBuffer(uint64 capacity) : buffer(Buffer<int>(capacity)) {}

  uint64 capacity() {
    return self.buffer.capacity();
  }
};

struct CopyableValue {
  int value = 1;
};

Buffer<int> transfer(Buffer<int> value) {
  return std::move(value);
}

NestedBuffer transfer_nested(NestedBuffer value) {
  return std::move(value);
}

uint64 inspect(Buffer<int>& value) {
  return value.capacity();
}

CopyableValue copy_value(CopyableValue value) {
  return value;
}

int main() {
  Buffer<int> buffer = Buffer<int>(uint64(2));
  Buffer<int> moved = transfer(std::move(buffer));
  NestedBuffer nested = NestedBuffer(uint64(3));
  NestedBuffer moved_nested = transfer_nested(std::move(nested));
  CopyableValue value = CopyableValue();
  CopyableValue copied = copy_value(value);
  return int(inspect(moved) + moved_nested.capacity()) - 5 + copied.value - 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("aggregate-ownership.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected aggregate-owner diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(
      frontend.canGenerateCode(),
      "classes should inherit move-only traits from direct and nested fields");

  const auto *bufferClass = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(0).get());
  const auto *copyableClass = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(2).get());
  const auto *transfer = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(3).get());
  const auto *main = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().at(7).get());
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
             parameter->traits.ownership == lang::OwnershipKind::Unique &&
             !parameter->traits.copyable && parameter->traits.movable &&
             parameter->traits.drop == lang::DropKind::Lexical &&
             !bufferBinding->traits.copyable && bufferBinding->traits.movable &&
             nestedBinding->traits.ownership == lang::OwnershipKind::Unique &&
             !nestedBinding->traits.copyable && nestedBinding->traits.movable &&
             copyableBinding->traits.copyable &&
             copyableBinding->traits.movable,
         "binding metadata should recursively propagate aggregate traits");
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
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("const Buffer<std::int32_t> value") ==
              std::string::npos &&
          artifact.contents.find("const Buffer<std::int32_t> buffer") ==
              std::string::npos &&
          artifact.contents.find("return std::move(value)") !=
              std::string::npos &&
          artifact.contents.find("Buffer(const Buffer &) = delete;") !=
              std::string::npos &&
          artifact.contents.find("Buffer(Buffer &&) = default;") !=
              std::string::npos &&
          artifact.contents.find(
              "Buffer &operator=(const Buffer &) = delete;") !=
              std::string::npos &&
          artifact.contents.find("Buffer &operator=(Buffer &&) = default;") !=
              std::string::npos,
      "the backend should keep immutable move-only aggregates transferable");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-aggregate-ownership.gti", R"(
class Buffer {
  mut gti_internal::storage<int> data;

public:
  Buffer() : data(gti_internal::allocate_storage<int>(uint64(1))) {}
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
          hasDiagnostic(invalid.diagnostics,
                        "Cannot return a value of type 'Buffer'") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'copied'") &&
          hasDiagnostic(invalid.diagnostics, "would copy a move-only value") &&
          hasDiagnostic(invalid.diagnostics, "has already been moved") &&
          hasDiagnostic(invalid.diagnostics, "Cannot initialize 'nested_copy'"),
      "aggregate diagnostics should cover return, call, copy, nesting, and "
      "use after move");
}

void testCompletePipeline() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int twice(int value) {
  return value * 2;
}

int main() {
  int result = twice(4);
  if (result == 8) {
    return 0;
  }
  return 1;
}
)");
  expect(!lexer.hadError(), "valid source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "valid source should parse");
  expect(program.declarations().size() == 2,
         "program should contain both functions");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program), "valid program should pass semantic checks");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find(
             "std::int32_t twice(const std::int32_t value)") !=
             std::string::npos,
         "emitter should lower function signatures");
  expect(generated.find("const std::int32_t result = twice(4)") !=
             std::string::npos,
         "emitter should make variables const by default");
  expect(generated.find("#include <iostream>") == std::string::npos,
         "emitter should not include print runtime support");
}

void testLoopControlStatements() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int main() {
  mut int total = 0;
  for (mut int outer = 0; outer < 4; outer++) {
    if (outer == 1) {
      continue;
    }
    while (true) {
      total += outer;
      break;
    }
  }
  return total;
}
)",
                           "loop-control.gti");
  expect(!lexer.hadError(), "break and continue should lex as keywords");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "loop control statements should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "break and continue should be valid in nested loop bodies");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(program, semantic.model(),
                                       lang::OptimizationLevel::O1);
  const std::string generated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       &optimizations)
          .emit(program);
  expect(generated.find("continue;") != std::string::npos &&
             generated.find("break;") != std::string::npos,
         "the C++ backend should preserve loop control statements");

  auto invalidTokens = lexer.scan(R"(
void stop() {
  break;
}

void skip() {
  if (true) {
    continue;
  }
}
)",
                                  "invalid-loop-control.gti");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "loop control outside a loop should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "loop control outside loops should be rejected semantically");
  expect(countDiagnosticCode(invalidSemantic, "GTI-S2010") == 2 &&
             hasDiagnostic(invalidSemantic,
                           "'break' can only be used inside a loop") &&
             hasDiagnostic(invalidSemantic,
                           "'continue' can only be used inside a loop"),
         "invalid break and continue should receive focused diagnostics");

  auto recoveredTokens = lexer.scan(R"(
void recover() {
  while (true) {
    break
    continue;
  }
}
)",
                                    "recover-loop-control.gti");
  lang::Parser recoveredParser(std::move(recoveredTokens));
  lang::Program recoveredProgram = recoveredParser.parse();
  expect(recoveredParser.errors().size() == 1 &&
             !recoveredParser.errors().front().fixes.empty() &&
             recoveredParser.errors().front().fixes.front().replacement == ";",
         "a missing loop-control semicolon should offer an insertion fix");
  lang::SemanticVisitor recoveredSemantic;
  expect(recoveredSemantic.check(recoveredProgram),
         "parser recovery should retain the following loop-control statement");
}

void testFixedWidthIntegers() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int8 minimum8 = -128;
int16 widened16 = minimum8;
int32 maximum32 = 2147483647;
int default32 = maximum32;
int64 maximum64 = 9223372036854775807;
int64 minimum64 = -9223372036854775808;
uint8 maximum_u8 = 255;
uint16 widened_u16 = maximum_u8;
uint32 maximum_u32 = 4294967295;
uint default_u32 = maximum_u32;
uint64 maximum_u64 = 18446744073709551615;
int64 signed_widening = maximum_u32;

int64 add_wide(int16 left, int64 right) {
  return left + right;
}

uint64 add_unsigned(uint16 left, uint64 right) {
  return left + right;
}

int main() {
  int8 maximum8 = 127;
  int16 minimum16 = -32768;
  int32 promoted = maximum8 + minimum16;
  uint8 unsigned_left = 1;
  uint8 unsigned_right = 2;
  int32 promoted_unsigned = unsigned_left + unsigned_right;
  uint32 counter = 1;
  uint32 next = counter + 1;
  bool has_next = next > 0;
  return promoted;
}
)");
  expect(!lexer.hadError(), "fixed-width integer source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "fixed-width integer declarations should parse");

  lang::SemanticVisitor semantic;
  const bool valid = semantic.check(program);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
      std::cerr << "Unexpected fixed-width diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid,
         "in-range literals and widening conversions should be valid");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("#include <cstdint>") != std::string::npos &&
             generated.find("const std::int8_t minimum8 = (-128)") !=
                 std::string::npos &&
             generated.find("const std::int16_t widened16 = minimum8") !=
                 std::string::npos &&
             generated.find("const std::int32_t default32 = maximum32") !=
                 std::string::npos &&
             generated.find("const std::int64_t maximum64 = "
                            "9223372036854775807") != std::string::npos &&
             generated.find("const std::int64_t minimum64 = "
                            "(-9223372036854775807LL - 1)") !=
                 std::string::npos &&
             generated.find("const std::uint8_t maximum_u8 = 255") !=
                 std::string::npos &&
             generated.find("const std::uint16_t widened_u16 = maximum_u8") !=
                 std::string::npos &&
             generated.find("const std::uint32_t default_u32 = maximum_u32") !=
                 std::string::npos &&
             generated.find("const std::uint64_t maximum_u64 = "
                            "18446744073709551615ULL") != std::string::npos &&
             generated.find("int main()") != std::string::npos,
         "integer widths should lower to cstdint types while main stays valid");

  auto invalidTokens = lexer.scan(R"(
int8 too_high = 128;
int8 too_low = -129;
int16 wide = 1;
int8 narrowing = wide;
int alias_overflow = 2147483648;
int64 signed_overflow = 9223372036854775808;
uint8 unsigned_negative = -1;
uint8 unsigned_overflow = 256;
uint16 unsigned_wide = 1;
uint8 unsigned_narrowing = unsigned_wide;
int32 signed_value = 1;
uint32 unsigned_value = 1;
uint32 signed_to_unsigned = signed_value;
int32 unsafe_sum = signed_value + unsigned_value;
bool unsafe_comparison = signed_value < unsigned_value;
uint32 unsafe_negation = -unsigned_value;
uint alias_unsigned_overflow = 4294967296;
)");
  expect(!lexer.hadError(),
         "signed range errors should be diagnosed semantically");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "out-of-range source should still parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "out-of-range literals and narrowing should be rejected");
  expect(invalidSemantic.errors().size() == 13,
         "each invalid fixed-width integer conversion should be diagnosed");

  auto lexicalOverflow =
      lexer.scan("uint64 too_large = 18446744073709551616;");
  (void)lexicalOverflow;
  expect(lexer.hadError() && lexer.errors().size() == 1,
         "integer literals larger than uint64 should fail during lexing");

  const std::string formatted = lang::Formatter().format(
      "int8 small=1;int64 large=small;uint8 byte=255;uint64 wide=byte;");
  expect(formatted == "int8 small = 1;\nint64 large = small;\n"
                      "uint8 byte = 255;\nuint64 wide = byte;\n",
         "formatter should preserve fixed-width type keywords");
}

void testIntegerBitwiseAndModuloOperators() {
  lang::Lexer lexer;

  lang::Parser bitwisePrecedence(lexer.scan("1 | 2 ^ 3 & 4"));
  lang::ExprPtr bitwiseExpression = bitwisePrecedence.parseExpression();
  expect(bitwiseExpression != nullptr &&
             lang::AstPrinter().print(*bitwiseExpression) ==
                 "(| 1 (^ 2 (& 3 4)))",
         "bitwise operators should follow C++ precedence");

  lang::Parser shiftPrecedence(lexer.scan("1 < 2 << 3 + 4 % 2"));
  lang::ExprPtr shiftExpression = shiftPrecedence.parseExpression();
  expect(shiftExpression != nullptr &&
             lang::AstPrinter().print(*shiftExpression) ==
                 "(< 1 (<< 2 (+ 3 (% 4 2))))",
         "modulo and shifts should integrate with arithmetic precedence");

  lang::Parser unaryPrecedence(lexer.scan("~1 * 2"));
  lang::ExprPtr unaryExpression = unaryPrecedence.parseExpression();
  expect(unaryExpression != nullptr &&
             lang::AstPrinter().print(*unaryExpression) == "(* (~ 1) 2)",
         "bitwise complement should bind as a unary operator");

  lang::Parser separatedShift(lexer.scan("1 > > 2"));
  expect(separatedShift.parseExpression() == nullptr &&
             separatedShift.hadError(),
         "spaced angle tokens should not become a shift operator");

  auto validTokens = lexer.scan(R"(
int combine(int left, int right) {
  return ((left & right) | (left ^ right)) % 17;
}

int shift_small(uint8 value) { return (value << 3) >> 1; }
int64 mix_widths(int64 left, uint32 right) { return left & right; }
uint64 unsigned_bits(uint64 left, uint64 right) { return left | right; }

int main() {
  int8 small = 3;
  int promoted = ~small;
  int flags = ((5 & 3) | 8) ^ 2;
  int shifted = (flags << 2) >> 1;
  int remainder = combine(shifted, 5);
  int wrapped = 1 << 31;
  if (promoted == -4 and remainder == 6 and wrapped == -2147483648) {
    return 0;
  }
  return 1;
}
)");
  expect(!lexer.hadError(), "integer bitwise source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "integer bitwise and modulo operators should parse");

  lang::SemanticVisitor validSemantic;
  const bool valid = validSemantic.check(validProgram);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : validSemantic.errors()) {
      std::cerr << "Unexpected integer operator diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid,
         "valid integer bitwise and modulo operations should type-check");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("gti_internal::backend::modulo(") !=
                 std::string::npos &&
             generated.find("gti_internal::backend::shift_left(") !=
                 std::string::npos &&
             generated.find("gti_internal::backend::shift_right(") !=
                 std::string::npos,
         "modulo and shifts should lower through checked integer helpers");
  expect(generated.find("(left & right)") != std::string::npos &&
             generated.find("(left ^ right)") != std::string::npos,
         "ordinary bitwise operators should lower directly");
  expect(generated.find("modulo by zero") != std::string::npos &&
             generated.find("std::bit_cast") != std::string::npos,
         "generated helpers should define invalid modulo and shift behavior");

  auto invalidTokens = lexer.scan(R"(
float decimal = 1.0;
bool condition = true;
int invalid_modulo = decimal % 2;
int invalid_and = condition & true;
int invalid_shift = 1 << decimal;
int invalid_complement = ~decimal;
int zero_modulo = 7 % 0;
int negative_shift = 1 << -1;
int wide_shift = 1 >> 32;
int32 signed_value = 1;
uint32 unsigned_value = 1;
int unsafe_bits = signed_value | unsigned_value;
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid integer operator types should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid integer operators should be rejected semantically");
  expect(hasDiagnostic(invalidSemantic, "requires integer operands") &&
             hasDiagnostic(invalidSemantic,
                           "Bitwise complement requires an integer"),
         "floats and bools should not gain bitwise behavior");
  expect(hasDiagnostic(invalidSemantic, "Modulo divisor cannot be zero"),
         "literal modulo by zero should be rejected before lowering");
  expect(hasDiagnostic(invalidSemantic, "Shift count cannot be negative") &&
             hasDiagnostic(invalidSemantic, "Shift count must be less than 32"),
         "invalid literal shift counts should be diagnosed");
  expect(hasDiagnostic(invalidSemantic, "no safe common type"),
         "bitwise operations should preserve safe signed/unsigned rules");

  const std::string formatted = lang::Formatter().format(
      "int value=(mask&3)|((mask^1)<<2);int mod=value%7;"
      "int inv=~value;int shifted=value>>1;");
  expect(formatted == "int value = (mask & 3) | ((mask ^ 1) << 2);\n"
                      "int mod = value % 7;\n"
                      "int inv = ~value;\n"
                      "int shifted = value >> 1;\n",
         "formatter should use C++ spacing for integer operators");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted integer operators should be idempotent");
}

void testParserRecovery() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int first = ;
int second = ;
class Broken {
  return 1;
  int value = 2;
};
int main() { return 0; }
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();

  expect(parser.errors().size() == 3,
         "parser should report independent declaration errors");
  expect(program.declarations().size() == 2,
         "parser should recover and keep later declarations");
}

void testSemanticDiagnostics() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int main() {
  if (1) { missing = 3; }
  return 0;
}
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "semantic test source should parse");

  lang::SemanticVisitor semantic;
  expect(!semantic.check(program), "invalid semantics should be rejected");
  expect(semantic.errors().size() == 2,
         "condition type and undefined variable should both be reported");
  expect(!semantic.errors().empty() &&
             semantic.errors().front().primary.line == 3,
         "semantic diagnostics should preserve literal source lines");
}

void testDiagnosticFoundation() {
  lang::SourceManager sources;
  const std::string unicodePrefix = "\xF0\x9F\x99\x82value";
  sources.set("unicode.gti", unicodePrefix);
  const lang::SourceLocation unicodeLocation =
      sources.locate(lang::SourceSpan{"unicode.gti", 4, 9, 1});
  expect(unicodeLocation.line == 1 && unicodeLocation.column == 2,
         "source locations should count a UTF-8 scalar as one CLI column");

  lang::Lexer lexer;
  const std::string invalidEscape = "string value = \"first\nbad\\q\";";
  lexer.scan(invalidEscape, "escape.gti");
  expect(
      lexer.errors().size() == 1 &&
          lexer.errors().front().code == "GTI-L0005" &&
          lexer.errors().front().primary.start == invalidEscape.find("\\q") &&
          lexer.errors().front().primary.end == invalidEscape.find("\\q") + 2 &&
          lexer.errors().front().primary.line == 2,
      "lexical diagnostics should identify the exact invalid escape span");

  const std::string missingSemicolon = "int first = 1\nint second = 2;\n";
  lang::Parser parser(lexer.scan(missingSemicolon, "parse.gti"));
  parser.parse();
  expect(parser.errors().size() == 1 &&
             parser.errors().front().code == "GTI-P0001" &&
             parser.errors().front().fixes.size() == 1 &&
             parser.errors().front().fixes.front().replacement == ";" &&
             parser.errors().front().fixes.front().span.start ==
                 missingSemicolon.find("int second"),
         "missing punctuation should carry an insertion fix-it");

  auto semanticTokens = lexer.scan(R"(
int duplicate = 1;
int duplicate = 2;
int main() {
  int fixed = 1;
  fixed = 2;
  int value = "text";
  return 0;
}
)",
                                   "semantic.gti");
  lang::Parser semanticParser(std::move(semanticTokens));
  lang::Program program = semanticParser.parse();
  lang::SemanticVisitor semantic;
  expect(!semantic.check(program),
         "rich semantic diagnostic source should fail");

  const lang::Diagnostic *duplicate = nullptr;
  const lang::Diagnostic *immutable = nullptr;
  const lang::Diagnostic *mismatch = nullptr;
  for (const lang::Diagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == "GTI-S2006") {
      duplicate = &diagnostic;
    } else if (diagnostic.code == "GTI-S2002") {
      immutable = &diagnostic;
    } else if (diagnostic.code == "GTI-S2003") {
      mismatch = &diagnostic;
    }
  }
  expect(duplicate != nullptr && duplicate->related.size() == 1,
         "duplicate declarations should reference the original declaration");
  expect(immutable != nullptr && immutable->related.size() == 1 &&
             !immutable->hints.empty(),
         "immutability diagnostics should explain the declaration and remedy");
  expect(mismatch != nullptr &&
             mismatch->message.find("int32") != std::string::npos &&
             mismatch->message.find("string") != std::string::npos,
         "type mismatches should name expected and actual GTI types");
}

void testExecutablePathDiscovery() {
  const std::filesystem::path executable =
      lang::executablePath("not-the-running-test-binary");
  std::error_code error;
  expect(executable.is_absolute() &&
             std::filesystem::is_regular_file(executable, error),
         "native executable discovery should not depend on argv[0]");
}

void testDefaultImmutability() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
int identity(int value) { return value; }
int main() {
  int fixed = 1;
  mut int moving = 1;
  moving++;
  return identity(fixed);
}
)");
  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "mutability syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "mutable bindings should permit mutation");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find(
             "std::int32_t identity(const std::int32_t value)") !=
             std::string::npos,
         "parameters should be const by default");
  expect(generated.find("const std::int32_t fixed = 1") !=
             std::string::npos,
         "immutable variables should lower to const");
  expect(generated.find("std::int32_t moving = 1") != std::string::npos,
         "mut variables should lower without const");

  auto invalidTokens = lexer.scan(R"(
class Box {
  int value = 1;
  int change() { self.value = 2; return self.value; }
};
int changeParameter(int value) { value = 2; return value; }
int main() {
  int fixed = 1;
  fixed = 2;
  int missingInitializer;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "immutability error source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "immutable bindings should reject mutation");
  expect(invalidSemantic.errors().size() == 4,
         "members, parameters, locals, and missing initializers should fail");
}

void testClassesStructsAndAccess() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
class Vault {
  int secret = 7;

public:
  int reveal() { return self.secret; }
  int reveal_other(Vault other) { return other.secret; }
};

struct Reading {
  int value = 1;

private:
  int hidden = 2;

public:
  int total() { return self.value + self.hidden; }
};

int open(mut Vault vault) { return vault.reveal(); }
int read(Reading reading) { return reading.value; }
)");
  expect(!lexer.hadError(), "class and struct access syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "class and struct access syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "public members and same-class private access should resolve");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("class Vault;") != std::string::npos &&
             generated.find("struct Reading;") != std::string::npos &&
             generated.find("class Vault {") != std::string::npos &&
             generated.find("struct Reading {") != std::string::npos &&
             generated.find("public:\n  std::int32_t reveal()") !=
                 std::string::npos &&
             generated.find("private:\n  std::int32_t hidden = 2") !=
                 std::string::npos,
         "emitter should preserve declaration kinds, access labels, and "
         "frontend-owned field immutability");

  auto invalidTokens = lexer.scan(R"(
class A {
  int private_value = 1;
public:
  int visible = 2;
  int inspect(A other) { return other.private_value; }
};

struct B {
  int public_value = 1;
private:
  int hidden = 2;
};

A wrong_type(B value) { return value; }
int read_class_private(A value) { return value.private_value; }
int read_struct_private(B value) { return value.hidden; }
int read_missing(B value) { return value.missing; }

class Duplicate {
  int value = 1;
public:
  int value = 2;
};

class InvalidFields {
  mut int missing_initializer;
  int invalid_reference = missing_initializer;
  int invalid_self = self.private_value;
};

int invalid_global_self = self.private_value;
MissingType unresolved();
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid class semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "nominal, access, member, field, and self errors should be rejected");
  expect(hasDiagnostic(invalidSemantic, "Cannot return a value of type"),
         "different nominal class types should not be assignable");
  expect(hasDiagnostic(invalidSemantic, "of 'A' is private") &&
             hasDiagnostic(invalidSemantic, "of 'B' is private"),
         "class defaults and explicit private labels should be enforced");
  expect(hasDiagnostic(invalidSemantic, "Unknown member 'missing'"),
         "unknown members should be diagnosed on their nominal type");
  expect(hasDiagnostic(invalidSemantic, "Duplicate member declaration"),
         "duplicate fields and methods should be rejected");
  expect(hasDiagnostic(invalidSemantic, "fields must have an initializer"),
         "mutable fields should require initialization until constructors exist");
  expect(hasDiagnostic(invalidSemantic, "referenced from field initializers"),
         "field initializers should not depend on member initialization order");
  expect(hasDiagnostic(invalidSemantic, "outside a class or struct method"),
         "self should be rejected in fields and outside methods");
  expect(hasDiagnostic(invalidSemantic, "Unknown type 'MissingType'"),
         "unknown nominal types should be diagnosed");

  auto conditionalTokens = lexer.scan(R"(
class PlatformValue {
#if target.os == "public-os"
public:
#else
private:
#endif
  int value = 1;
};
int read_platform(PlatformValue value) { return value.value; }
)");
  lang::Parser conditionalParser(std::move(conditionalTokens));
  lang::Program conditionalProgram = conditionalParser.parse();
  expect(!conditionalParser.hadError(),
         "conditional access labels should parse in class bodies");

  lang::SemanticVisitor publicTarget(
      lang::TargetInfo{.os = "public-os", .vendor = "test", .arch = "test"});
  lang::SemanticVisitor privateTarget(
      lang::TargetInfo{.os = "private-os", .vendor = "test", .arch = "test"});
  expect(publicTarget.check(conditionalProgram),
         "the active public access branch should expose following members");
  expect(!privateTarget.check(conditionalProgram) &&
             hasDiagnostic(privateTarget, "is private"),
         "the active private access branch should hide following members");

  auto recoveryTokens = lexer.scan(R"(
struct Recovered {
  public int value = 1;
private:
  int hidden = 2;
};
int main() { return 0; }
)");
  lang::Parser recoveryParser(std::move(recoveryTokens));
  lang::Program recoveryProgram = recoveryParser.parse();
  expect(recoveryParser.errors().size() == 1 &&
             recoveryProgram.declarations().size() == 2,
         "parser recovery should resume at access labels and later declarations");

  const std::string formatted = lang::Formatter().format(
      "class Box{public:int value=1;private:int hidden=2;};"
      "struct Point{int x=0;};");
  expect(formatted == "class Box {\npublic:\n  int value = 1;\nprivate:\n"
                      "  int hidden = 2;\n};\nstruct Point {\n"
                      "  int x = 0;\n};\n",
         "formatter should outdent C++-style access labels");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted class access labels should be idempotent");
}

void testConstructorsAndReceiverMutability() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
class Counter {
  mut int value;
  int step = 1;

public:
  Counter() : value(0) {}
  Counter(int initial) : value(initial) {}
  Counter(bool reset) : value(0) {}
  int read() { return self.value; }
  int advance(int amount) mut {
    self.value += amount;
    return self.value;
  }
};

struct Origin {
  int x = 0;
  Origin(int initial) : x(initial) {}
};

int inspect(Counter counter) { return counter.read(); }
int main() {
  Counter zero = Counter();
  Counter fixed = Counter(1);
  mut int observed = fixed.read();
  mut Counter moving = Counter(observed);
  observed = moving.advance(2);
  Origin origin = Origin();
  Origin shifted = Origin(4);
  Counter reset = Counter(true);
  return observed + origin.x;
}
)");
  expect(!lexer.hadError(), "constructor and receiver syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "constructor and receiver syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "explicit construction and mutable receiver calls should validate");

  const auto *counter = dynamic_cast<const lang::ClassDecl *>(
      validProgram.declarations().at(0).get());
  const auto *origin = dynamic_cast<const lang::ClassDecl *>(
      validProgram.declarations().at(1).get());
  const auto *main = dynamic_cast<const lang::FunctionDecl *>(
      validProgram.declarations().at(3).get());
  const auto *zero = main == nullptr
                         ? nullptr
                         : dynamic_cast<const lang::VariableDecl *>(
                               main->body()->statements().at(0).get());
  const auto *fixed = main == nullptr
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                main->body()->statements().at(1).get());
  const auto *defaultOrigin = main == nullptr
                                  ? nullptr
                                  : dynamic_cast<const lang::VariableDecl *>(
                                        main->body()->statements().at(5).get());
  const auto *zeroCall =
      zero == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(zero->initializer().get());
  const auto *fixedCall =
      fixed == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(fixed->initializer().get());
  const auto *originCall = defaultOrigin == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::Call *>(
                                     defaultOrigin->initializer().get());
  const lang::ClassLifecycleInfo *counterLifecycle =
      counter == nullptr ? nullptr
                         : validSemantic.model().findClassLifecycle(*counter);
  const lang::ClassLifecycleInfo *originLifecycle =
      origin == nullptr ? nullptr
                        : validSemantic.model().findClassLifecycle(*origin);
  const lang::ResolvedConstructionInfo *zeroConstruction =
      zeroCall == nullptr ? nullptr
                          : validSemantic.model().findConstruction(*zeroCall);
  const lang::ResolvedConstructionInfo *fixedConstruction =
      fixedCall == nullptr ? nullptr
                           : validSemantic.model().findConstruction(*fixedCall);
  const lang::ResolvedConstructionInfo *originConstruction =
      originCall == nullptr
          ? nullptr
          : validSemantic.model().findConstruction(*originCall);
  expect(counterLifecycle != nullptr && originLifecycle != nullptr &&
             counterLifecycle->constructors.size() == 3 &&
             counterLifecycle->defaultConstructor ==
                 lang::SpecialMemberStatus::Declared &&
             counterLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             counterLifecycle->moveAssignment ==
                 lang::SpecialMemberStatus::Generated &&
             originLifecycle->defaultConstructor ==
                 lang::SpecialMemberStatus::Generated,
         "class lifecycle metadata should distinguish declared, generated, "
         "and available special members");
  expect(zeroConstruction != nullptr && fixedConstruction != nullptr &&
             originConstruction != nullptr &&
             zeroConstruction->declaration != fixedConstruction->declaration &&
             !zeroConstruction->generatedDefault &&
             originConstruction->generatedDefault,
         "construction metadata should retain exact overload selection and "
         "generated default construction");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  const std::string lifecycleGenerated =
      lang::CppEmitter(lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       nullptr, &validSemantic.model())
          .emit(validProgram);
  expect(generated.find(
             "explicit Counter(const std::int32_t initial) : value(initial)") !=
             std::string::npos,
         "constructor overloads should lower explicitly with field "
         "initialization");
  expect(generated.find("std::int32_t read() const") != std::string::npos,
         "methods should lower as read-only by default");
  expect(
      generated.find("std::int32_t advance(const std::int32_t amount) const") ==
              std::string::npos &&
          generated.find("std::int32_t advance(const std::int32_t amount)") !=
              std::string::npos,
      "mutable receiver methods should lower without C++ const");
  expect(generated.find("Origin origin = Origin()") != std::string::npos,
         "generated default construction should remain explicit at the call "
         "site");
  expect(lifecycleGenerated.find("Origin() = default;") != std::string::npos &&
             lifecycleGenerated.find("Counter(const Counter &) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find("Counter(Counter &&) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find(
                 "Counter &operator=(const Counter &) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find(
                 "Counter &operator=(Counter &&) = default;") !=
                 std::string::npos &&
             lifecycleGenerated.find("~Counter() noexcept = default;") !=
                 std::string::npos,
         "the backend should explicitly emit compiler-generated special "
         "members");

  auto invalidTokens = lexer.scan(R"(
class MissingInitialization {
  int value;
};

class InvalidConstructor {
  int first;
  int second;

public:
  InvalidConstructor(int value)
      : second(value), first(self.second), second(value) { return; }
  InvalidConstructor(mut int value) : first(value), second(value) {}
};

class ReservedCopy {
  int value = 0;

public:
  ReservedCopy(ReservedCopy& other) : value(other.value) {}
};

class PrivateValue {
  int value;
  PrivateValue(int initial) : value(initial) {}
};

class MutableValue {
  mut int value = 0;

public:
  void mutate() { self.value = 1; }
  void mutate_other(MutableValue other) mut { other.value = 1; }
  void bump() mut { self.value += 1; }
};

class ImmutableField {
  int value = 0;

public:
  void replace() mut { self.value = 1; }
};

int main() {
  PrivateValue hidden = PrivateValue(1);
  MutableValue fixed = MutableValue();
  fixed.bump();
  mut MutableValue moving = MutableValue();
  moving.bump();
  mut MutableValue uninitialized;
  InvalidConstructor mismatch = InvalidConstructor(true);
  MutableValue implicit_value = 1;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid constructor semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid construction and receiver use should be rejected");
  expect(hasDiagnostic(invalidSemantic, "fields must have an initializer"),
         "a class without a constructor should still initialize every field");
  expect(hasDiagnostic(invalidSemantic,
                       "Duplicate constructor overload signature"),
         "constructor overload signatures should be unique by exact parameter "
         "type");
  expect(hasDiagnostic(invalidSemantic, "field declaration order") &&
             hasDiagnostic(invalidSemantic, "initialized more than once"),
         "constructor initializer order and uniqueness should be enforced");
  expect(hasDiagnostic(invalidSemantic,
                       "Cannot use 'self' in a constructor initializer"),
         "constructor initializers should not observe a partial object");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructors cannot contain return statements"),
         "constructor bodies should reject return statements");
  expect(hasDiagnostic(invalidSemantic,
                       "Copy and move constructors are compiler-generated"),
         "source constructors should not replace compiler-owned lifecycle "
         "members");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructor of 'PrivateValue' is private"),
         "constructor access should follow class access labels");
  expect(hasDiagnostic(invalidSemantic,
                       "Mutable method requires a mutable receiver") &&
             hasDiagnostic(invalidSemantic,
                           "Cannot mutate through a read-only receiver"),
         "mutable methods and field writes should require mutable receivers");
  expect(hasDiagnostic(invalidSemantic, "Member is immutable"),
         "frontend field immutability should remain enforced independently of "
         "the C++ representation");
  expect(
      hasDiagnostic(invalidSemantic,
                    "No constructor of 'InvalidConstructor'") &&
          hasDiagnostic(invalidSemantic, "Cannot initialize 'implicit_value'"),
      "constructor calls should reject mismatched and implicit conversions");
  expect(hasDiagnostic(invalidSemantic, "require explicit construction"),
         "class variables should never invoke construction implicitly");

  const std::string formatted = lang::Formatter().format(
      "class Counter{mut int value;public:Counter(int initial):value(initial){}"
      "int read(){return self.value;}void reset()mut{self.value=0;}};");
  expect(formatted.find("Counter(int initial) : value(initial) {}") !=
                 std::string::npos &&
             formatted.find("void reset() mut {") != std::string::npos,
         "formatter should distinguish initializer and access-label colons");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted constructors and receiver qualifiers should be idempotent");

  lang::Parser globalQualifierParser(lexer.scan("void invalid() mut {}"));
  globalQualifierParser.parse();
  expect(globalQualifierParser.hadError() &&
             globalQualifierParser.errors().front().message.find(
                 "Only class and struct methods") != std::string::npos,
         "free functions should reject receiver mutability qualifiers");
}

void testDestructorsAndActiveDropState() {
  const std::string source = R"(
class CleanupBuffer<T> {
  mut gti_internal::storage<T> data;
  mut uint64 count = 0;

public:
  CleanupBuffer(uint64 capacity)
      : data(gti_internal::allocate_storage<T>(capacity)) {}

  ~CleanupBuffer() {
    while (self.count > 0) {
      self.count--;
      gti_internal::storage_destroy(self.data, self.count);
    }
  }

  void push(T value) mut {
    gti_internal::storage_construct(self.data, self.count, value);
    self.count++;
  }
};

CleanupBuffer<int> transfer(CleanupBuffer<int> value) {
  return std::move(value);
}

int main() {
  mut CleanupBuffer<int> values = CleanupBuffer<int>(uint64(2));
  values.push(7);
  CleanupBuffer<int> moved = transfer(std::move(values));
  return 0;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("destructor.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected destructor diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "public destructors should support mutable vector-style cleanup");

  const auto *cleanupClass = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().front().get());
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find(
          "~CleanupBuffer() noexcept { __gti_lifecycle_cleanup_1(); }") !=
              std::string::npos &&
          artifact.contents.find("bool __gti_lifecycle_active_1 = true;") !=
              std::string::npos &&
          artifact.contents.find(
              "if (!__gti_lifecycle_active_1) { return; }") !=
              std::string::npos &&
          artifact.contents.find("CleanupBuffer(const CleanupBuffer &) = "
                                 "delete;") != std::string::npos &&
          artifact.contents.find(
              "CleanupBuffer(CleanupBuffer &&other) noexcept") !=
              std::string::npos &&
          artifact.contents.find("other.__gti_lifecycle_active_1 = false;") !=
              std::string::npos &&
          artifact.contents.find("__gti_lifecycle_cleanup_1();") !=
              std::string::npos &&
          artifact.contents.find("gti_internal::backend::storage_destroy") !=
              std::string::npos,
      "the backend should lower declared cleanup through an active drop state "
      "and generated safe moves");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-destructor.gti", R"(
class WrongName {
public:
  ~Other() {}
};

class DuplicateCleanup {
public:
  ~DuplicateCleanup() {}
  ~DuplicateCleanup() {}
};

class PrivateCleanup {
  ~PrivateCleanup() {}
};

class ReturningCleanup {
public:
  ~ReturningCleanup() { return; }
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
      "class Trace{mut int state=1;public:~Trace(){while(self.state>0){"
      "self.state--;}}};");
  expect(formatted.find("~Trace() {") != std::string::npos &&
             formatted.find("while (self.state > 0) {") != std::string::npos &&
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

  void increment() mut { self.value += 1; }
};

class Handle {
  mut Payload payload = Payload();
  mut int values[2] = {1, 2};

public:
  Payload& operator->() { return self.payload; }
  mut Payload& operator->() mut { return self.payload; }
  int& operator*() { return self.values[0]; }
  mut int& operator*() mut { return self.values[0]; }
  int& operator[](uint64 index) { return self.values[index]; }
  mut int& operator[](uint64 index) mut { return self.values[index]; }
  bool operator==(nullptr_t other) { return false; }
  bool operator!=(nullptr_t other) { return true; }
  operator bool() { return true; }
};

int main() {
  mut Handle handle = Handle();
  handle->increment();
  *handle = 7;
  handle[uint64(1)] += 3;
  if (handle and handle != nullptr) {
    return *handle + handle[uint64(1)] - 12;
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

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(
      artifact.contents.find("___gti_operator_arrow") != std::string::npos &&
          artifact.contents.find("___gti_operator_dereference") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_subscript") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_not_equal(nullptr)") !=
              std::string::npos &&
          artifact.contents.find("___gti_operator_bool()") !=
              std::string::npos &&
          artifact.contents.find(" operator*(") == std::string::npos,
      "the backend should emit calls to semantically selected methods instead "
      "of C++ operator overloads");

  const std::string formatted = lang::Formatter().format(
      "class Handle{public:mut int& operator*()mut{return self.value;}"
      "operator bool(){return true;}};");
  expect(formatted.find("mut int & operator*() mut {") != std::string::npos &&
             formatted.find("operator bool() {") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "operator declarations should format with stable C++-style layout");

  const lang::FrontendResult invalidContracts =
      lang::Frontend().analyze("invalid-operator-contracts.gti", R"(
class InvalidOperators {
  mut int value = 0;
public:
  int operator*() { return self.value; }
  int operator->() { return self.value; }
  int& operator[](uint64 first, uint64 second) { return self.value; }
  bool operator==(nullptr_t other) mut { return true; }
  operator bool() mut { return true; }
};
int main() { return 0; }
)");
  expect(!invalidContracts.canGenerateCode() &&
             hasDiagnosticCode(invalidContracts.diagnostics, "GTI-S2022") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "operator* must return a checked reference") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "operator-> must return a checked reference") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "operator[] expects 1 parameter") &&
             hasDiagnostic(invalidContracts.diagnostics,
                           "must use a read-only receiver"),
         "operator declarations should enforce the restricted contracts");

  const lang::FrontendResult invalidUses =
      lang::Frontend().analyze("invalid-operator-uses.gti", R"(
class ReadOnlyHandle {
  int value = 0;
public:
  int& operator*() { return self.value; }
  bool operator==(int other) { return true; }
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

  lang::Lexer lexer;
  lang::Parser unsupportedParser(lexer.scan(R"(
class Unsupported {
public:
  int operator+(int other) { return other; }
};
int main() { return 0; }
)"));
  const lang::Program recovered = unsupportedParser.parse();
  expect(unsupportedParser.hadError() &&
             hasDiagnostic(unsupportedParser.errors(),
                           "Supported overloads are operator*") &&
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
};
class MutableOnly {
  mut int value = 0;
public:
  mut int& operator*() mut { return self.value; }
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

void testNamedGenerics() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
class Box<T> {
  mut T value;

public:
  Box(T value) : value(value) {}
  T get() { return self.value; }
  U echo<U>(U replacement) { return replacement; }
  void set(T replacement) mut { self.value = replacement; }
};

T identity<T>(T value) { return value; }
T unbox<T>(Box<T> box) { return box.get(); }
U relay<T, U>(Box<T> box, U value) { return box.echo<U>(value); }

int main() {
  mut Box<int> box = Box<int>(identity(7));
  box.set(identity<int>(9));
  int value = unbox(box);
  int relayed = relay(box, box.echo<int>(value));
  string text = identity<string>("generic");
  return relayed;
}
)");
  expect(!lexer.hadError(), "named generic source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "generic classes, functions, and applications should parse");

  lang::SemanticVisitor validSemantic;
  const bool valid = validSemantic.check(validProgram);
  if (!valid) {
    for (const lang::SemanticDiagnostic &diagnostic : validSemantic.errors()) {
      std::cerr << "Unexpected generic diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(valid, "generic substitution and exact inference should validate");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("template <typename T>\nclass Box;") !=
                 std::string::npos &&
             generated.find("template <typename T>\nclass Box {") !=
                 std::string::npos,
         "generic classes should lower with matching C++ forward declarations");
  expect(generated.find("template <typename T>\nT identity(const T value)") !=
                 std::string::npos &&
             generated.find("template <typename T>\nT unbox(") !=
                 std::string::npos,
         "generic functions should lower as C++ function templates");
  expect(generated.find("Box<std::int32_t> box = "
                        "Box<std::int32_t>(identity(7))") !=
                 std::string::npos &&
             generated.find("identity<std::int32_t>(9)") != std::string::npos,
         "applied types and explicit generic calls should lower recursively");
  expect(generated.find(".template echo<std::int32_t>(") != std::string::npos,
         "the backend should hide C++ dependent-template disambiguation");

  auto invalidTokens = lexer.scan(R"(
class Duplicate<T, T> {};
class SameName<SameName> {};
class Shadow<T> {
public:
  T replace<T>(T value) { return value; }
};
class Box<T> {
  T value;
public:
  Box(T value) : value(value) {}
  T get() { return self.value; }
};

T identity<T>(T value) { return value; }
T choose<T>(T left, T right) { return left; }
T unsupported_add<T>(T left, T right) { return left + right; }
bool unsupported_equal<T>(T left, T right) { return left == right; }
T make<T>();
int ordinary(int value) { return value; }
int main<T>() { return 0; }

int use() {
  Box missing = Box(1);
  Box<int, string> excessive = Box<int, string>(1);
  Box<void> impossible = Box<void>(1);
  int mismatch = identity<int>(true);
  int conflict = choose(1, true);
  int unknown = make();
  int excessive_types = identity<int, string>(1);
  int not_generic = ordinary<int>(1);
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid generic semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid generic applications should be rejected semantically");
  expect(hasDiagnostic(invalidSemantic, "Duplicate generic type parameter"),
         "generic parameter names should be unique");
  expect(hasDiagnostic(invalidSemantic, "same name as its declaration") &&
             hasDiagnostic(invalidSemantic, "cannot shadow"),
         "generic parameters should not collide with enclosing declarations");
  expect(hasDiagnostic(invalidSemantic, "requires 1 generic type argument"),
         "generic class applications should enforce arity");
  expect(hasDiagnostic(invalidSemantic, "cannot be void"),
         "void should not be accepted as a user generic argument");
  expect(hasDiagnostic(invalidSemantic, "Conflicting types inferred"),
         "repeated generic parameters should infer one exact type");
  expect(
      hasDiagnostic(invalidSemantic, "numeric operands"),
      "unconstrained type parameters should not gain operators by duck typing");
  expect(hasDiagnostic(invalidSemantic, "Equality operands"),
         "generic equality should wait for an explicit contract model");
  expect(hasDiagnostic(invalidSemantic, "Cannot infer generic type parameter"),
         "return-only generic parameters should require explicit arguments");
  expect(
      hasDiagnostic(invalidSemantic, "wrong number of type arguments") &&
          hasDiagnostic(invalidSemantic, "Non-generic functions do not take"),
      "explicit function type arguments should enforce generic arity");
  expect(hasDiagnostic(invalidSemantic, "main entry point cannot be generic"),
         "the native entry point should remain non-generic");

  const std::string formatted = lang::Formatter().format(
      "class Box<T>{T value;public:Box(T value):value(value){}T get(){return "
      "self.value;}};T identity<T>(T value){return value;}int main(){Box<"
      "Box<int>> nested=Box<Box<int>>(Box<int>(1));int value=identity<int>(1);"
      "return value;}");
  expect(formatted.find("class Box<T> {") != std::string::npos &&
             formatted.find("T identity<T>(T value) {") != std::string::npos &&
             formatted.find("Box<Box<int>> nested = Box<Box<int>>(") !=
                 std::string::npos &&
             formatted.find("identity<int>(1)") != std::string::npos,
         "formatter should preserve compact generic angle brackets");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted generic syntax should be idempotent");

  const std::string comparison =
      lang::Formatter().format("bool result=a < b > c;");
  expect(comparison == "bool result = a < b > c;\n",
         "formatter should not treat relational expressions as generic types");

  lang::Parser malformedParser(lexer.scan("class Broken<> {}; int okay = 1;"));
  const lang::Program recovered = malformedParser.parse();
  expect(malformedParser.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after malformed generic parameters");
}

void testExactFunctionOverloadsAndConversions() {
  const std::string source = R"(
namespace math {
uint64 pow(uint64 base, uint64 exponent) { return base * exponent; }
float pow(float base, float exponent) { return base * exponent; }
}

struct Selector<T, U> {
  T apply(T value) { return value; }
  U apply(U value) { return value; }
};

int main() {
  uint64 base = uint64(2);
  uint64 exponent = uint64(8);
  uint64 whole = math::pow(base, exponent);
  float decimal = math::pow(2.0, 8.0);
  Selector<int, float> selector = Selector<int, float>();
  int selected = selector.apply(2);
  float selected_decimal = selector.apply(2.0);
  return int(whole) + selected;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("overloads.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected overload diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "exact namespace and method overloads should validate");
  expect(
      frontend.semantics.functionCount() == 5 &&
          frontend.semantics.resolvedCallCount() == 4,
      "semantic analysis should retain function identities and selected calls");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("__gti_fn_1_pow") != std::string::npos &&
             artifact.contents.find("__gti_fn_2_pow") != std::string::npos &&
             artifact.contents.find("math::__gti_fn_1_pow(base, exponent)") !=
                 std::string::npos &&
             artifact.contents.find(".__gti_fn_3_apply(2)") !=
                 std::string::npos,
         "the C++ backend should emit the function identity selected by GTI");
  expect(artifact.contents.find("numeric_cast<std::uint64_t>(2)") !=
                 std::string::npos &&
             artifact.contents.find("numeric_cast<std::int32_t>(whole)") !=
                 std::string::npos &&
             artifact.contents.find("2.00000000F") != std::string::npos,
         "explicit conversions and float literals should lower to matching C++ "
         "types");

  lang::Lexer lexer;
  auto invalidTokens = lexer.scan(R"(
int same(int value) { return value; }
float same(int value) { return float(value); }

int mutate(int value) { return value; }
int mutate(mut int value) { return value; }

T echo<T>(T value) { return value; }
U echo<U>(U value) { return value; }

struct Receiver {
  int inspect(int value) { return value; }
  int inspect(int value) mut { return value; }
};

int choose(int value) { return value; }
T choose<T>(T value) { return value; }

uint64 width(uint64 value) { return value; }
float width(float value) { return value; }
float only_float(float value) { return value; }
void function_value() {}

int main(int value) { return value; }
int main() {
  function_value;
  float mismatch = width(1);
  int ambiguous = choose(1);
  float inexact = only_float(1);
  uint8 too_small = uint8(300);
  int not_numeric = int("text");
  return 0;
}
)");
  expect(!lexer.hadError(), "invalid overload cases should lex");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid overload cases should remain syntactically valid");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid overloads and conversions should fail semantics");
  expect(countDiagnosticCode(invalidSemantic, "GTI-S2011") == 5,
         "return type, by-value mutability, and generic spelling should not "
         "create distinct overload signatures");
  expect(
      hasDiagnostic(invalidSemantic, "main entry point cannot be overloaded"),
      "the native entry point should remain a unique function");
  expect(hasDiagnostic(invalidSemantic, "No overload of 'width'") &&
             hasDiagnostic(invalidSemantic, "argument types (int32)"),
         "overload lookup should reject calls without an exact candidate");
  expect(hasDiagnostic(invalidSemantic, "ambiguous") &&
             hasDiagnostic(invalidSemantic, "exactly match"),
         "generic and concrete exact matches should be diagnosed as ambiguous");
  expect(hasDiagnostic(invalidSemantic, "parameter requires 'float'"),
         "single functions should also require exact argument types");
  expect(
      hasDiagnostic(invalidSemantic, "function values are not supported"),
      "function overload sets should not escape unresolved into the backend");
  expect(hasDiagnostic(invalidSemantic, "outside the range of 'uint8'") &&
             hasDiagnostic(invalidSemantic,
                           "numeric conversions require a numeric value"),
         "explicit conversions should reject invalid literals and domains");

  const std::string formatted = lang::Formatter().format(
      "uint64 pow(uint64 base,uint64 exponent){return base*exponent;}int "
      "main(){uint64 value=uint64(2);return int(value);}");
  expect(formatted.find("uint64 value = uint64(2);") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "explicit conversion syntax should format like a C++ functional cast");
}

void testFixedArrays() {
  const std::string source = R"(
int extent(int values[2]) { return 2; }
int extent(int values[3]) { return 3; }
int first(int values[3]) { return values[0]; }

struct Buffers {
public:
  mut int samples[3] = {1, 2, 3};

  void bump() mut { self.samples[1] += 4; }
  uint64 count() { return self.samples.size(); }
};

int main() {
  mut int buffer[5] = {1, 2, 3, 4, 5};
  buffer[2] = 10;
  buffer[1]++;
  int copy[5] = buffer;
  int matrix[2][3] = {{1, 2, 3}, {4, 5, 6}};
  mut Buffers buffers = Buffers();
  buffers.bump();
  uint64 count = buffer.size();
  if (count == 5 and first(matrix[1]) == 4 and extent(matrix[0]) == 3 and
      buffers.count() == 3 and copy[4] == 5) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("arrays.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected fixed array diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays should support value semantics and checked indexing");

  const auto *main = dynamic_cast<const lang::FunctionDecl *>(
      frontend.program.declarations().back().get());
  const auto *buffer = main == nullptr
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().front().get());
  const lang::BindingInfo *binding =
      buffer == nullptr ? nullptr : frontend.semantics.findBinding(*buffer);
  expect(binding != nullptr &&
             binding->type.kind == lang::SemanticType::Array &&
             binding->type.arrayLength == 5 &&
             binding->type.arguments.size() == 1 &&
             binding->type.arguments[0] == lang::SemanticType::Int32 &&
             binding->access == lang::AccessMode::Mutable,
         "fixed array bindings should retain length, element type, and access");

  const lang::SemanticType moveOnlyArray = lang::SemanticType::arrayOf(
      lang::SemanticType::uniquePointerTo(lang::SemanticType::Int32), 2);
  expect(!lang::semanticTraits(moveOnlyArray).copyable &&
             lang::semanticTraits(moveOnlyArray).movable,
         "fixed arrays should inherit element copy and move traits");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.program, frontend.semantics,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .optimizations = optimizations});
  expect(artifact.contents.find(
             "std::array<std::int32_t, 5> buffer = {1, 2, 3, 4, 5}") !=
                 std::string::npos &&
             artifact.contents.find(
                 "std::array<std::array<std::int32_t, 3>, 2> matrix") !=
                 std::string::npos &&
             artifact.contents.find("backend::array_at") != std::string::npos &&
             artifact.contents.find("static_cast<std::uint64_t>") !=
                 std::string::npos,
         "the C++ backend should preserve array values and checked operations");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-arrays.gti", R"(
struct NoDefault {
  int stored;
  NoDefault(int value) : stored(value) {}
};

int choose(int values[2]) { return 2; }
int choose(int values[3]) { return 3; }

int main() {
  int wrong_count[3] = {1, 2};
  int immutable[1] = {1};
  immutable[0] = 2;
  int values[2] = {1, 2};
  int bad_index = values[1.5];
  int negative = values[-1];
  int past_end = values[2];
  mut int missing[2];
  NoDefault objects[1] = {};
  int other[4] = {1, 2, 3, 4};
  int mismatch = choose(other);
  uint64 hidden = other.length();
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid fixed array operations should fail semantic analysis");
  expect(hasDiagnostic(invalid.diagnostics, "requires exactly 3") &&
             hasDiagnostic(invalid.diagnostics, "immutable fixed array") &&
             hasDiagnostic(invalid.diagnostics, "index must have an integer") &&
             hasDiagnostic(invalid.diagnostics, "valid range [0, 2)") &&
             hasDiagnostic(invalid.diagnostics, "require an initializer") &&
             hasDiagnostic(invalid.diagnostics, "default-initializable") &&
             hasDiagnostic(invalid.diagnostics, "No overload of 'choose'") &&
             hasDiagnostic(invalid.diagnostics, "Unknown fixed array member"),
         "fixed array diagnostics should explain extent, access, and bounds");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan("int broken[] = {}; int recovered = 1;"));
  const lang::Program recovered = malformed.parse();
  expect(malformed.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after a missing array extent");

  const std::string formatted = lang::Formatter().format(
      "mut int buffer[3]={1,2,3};int matrix[2][2]={{1,2},{3,4}};");
  expect(formatted == "mut int buffer[3] = {1, 2, 3};\n"
                      "int matrix[2][2] = {{1, 2}, {3, 4}};\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should preserve compact C++-style array declarations");
  const std::string constructorFormatted = lang::Formatter().format(
      "class Pair<T>{T values[2];public:Pair(T left,T right):"
      "values({left,right}){}};");
  expect(constructorFormatted.find("values({left, right}) {}") !=
             std::string::npos,
         "formatter should keep array constructor initializers compact");
}

void testDefaultNodiscard() {
  lang::Lexer lexer;
  auto validTokens = lexer.scan(R"(
int calculate() { return 7; }
void perform() {}
int main() {
  [[discard]] calculate();
  perform();
  mut int count = 0;
  count++;
  return 0;
}
)");
  expect(!lexer.hadError(), "discard attribute syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "discard attribute syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "explicit discard and void calls should pass semantic checks");

  const std::string generated = lang::CppEmitter().emit(validProgram);
  expect(generated.find("calculate();") != std::string::npos &&
             generated.find("[[discard]]") == std::string::npos,
         "discard should be a GTI-only call-site attribute");

  auto invalidTokens = lexer.scan(R"(
int calculate() { return 7; }
void perform() {}
int main() {
  calculate();
  [[discard]] perform();
  [[discard]] 1 + 2;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid discard uses should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "ignored function results and invalid discard attributes should fail");
  expect(invalidSemantic.errors().size() == 3,
         "nodiscard should produce focused diagnostics for all invalid uses");
}

void testExpectedValues() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
expected<int, string> calculate(bool fail) {
  if (fail) { return unexpected("calculation failed"); }
  return 42;
}
expected<void, string> render(bool fail) {
  if (fail) { return unexpected("render failed"); }
  return;
}
int main() {
  expected<int, string> result = calculate(false);
  if (!result.has_value()) { return 1; }
  int value = result.value_or(0);
  expected<void, string> rendered = render(false);
  if (!rendered) { return 2; }
  rendered.value();
  [[discard]] calculate(false);
  return value - 42;
}
)");
  expect(!lexer.hadError(), "expected source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "expected types and values should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "expected construction and observers should pass semantic checks");

  const std::string cpp23 = lang::CppEmitter().emit(program);
  expect(cpp23.find("#include <expected>") != std::string::npos &&
             cpp23.find("std::expected<std::int32_t, std::string>") !=
                 std::string::npos &&
             cpp23.find("std::unexpected(") != std::string::npos &&
             cpp23.find("return {};") != std::string::npos,
         "C++23 should lower expected values to the standard library");

  const std::string cpp20 =
      lang::CppEmitter(lang::CppStandard::Cpp20).emit(program);
  expect(cpp20.find("#include <nonstd/expected.hpp>") != std::string::npos &&
             cpp20.find("nonstd::expected<std::int32_t, std::string>") !=
                 std::string::npos &&
             cpp20.find("nonstd::make_unexpected(") != std::string::npos,
         "C++20 should lower expected values to the vendored implementation");

  auto invalidTokens = lexer.scan(R"(
expected<int, void> invalid_error() { return 1; }
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
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid expected uses should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid expected types, states, and observers should fail");
  expect(invalidSemantic.errors().size() == 6,
         "expected validation should produce focused diagnostics");
}

void testPrintIsAnIdentifier() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int print(int value) { return value; }
int main() { return print(0); }
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "print should parse as an ordinary identifier");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "user code should be able to declare a function named print");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find(
             "std::int32_t print(const std::int32_t value)") !=
             std::string::npos,
         "print should lower as a normal function");
}

void testNamespacesAndAliases() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
namespace engine {
namespace graphics {
class Renderer {};
void render() {}
void renderTwice() {
  render();
  render();
}
}
}

namespace gfx = engine::graphics;
gfx::Renderer createRenderer();

int main() {
  engine::graphics::render();
  gfx::renderTwice();
  return 0;
}
)");
  expect(!lexer.hadError(), "namespace source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "nested namespaces and aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "qualified calls and namespace aliases should resolve");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("namespace engine {") != std::string::npos &&
             generated.find("namespace graphics {") != std::string::npos,
         "emitter should preserve nested namespaces");
  expect(generated.find("namespace gfx = engine::graphics;") !=
             std::string::npos,
         "emitter should preserve namespace aliases");
  expect(generated.find("gfx::Renderer createRenderer();") !=
             std::string::npos,
         "qualified types should parse and emit through namespace aliases");
  expect(generated.find("engine::graphics::render();") != std::string::npos &&
             generated.find("gfx::renderTwice();") != std::string::npos,
         "emitter should preserve qualified calls");

  auto invalidTokens = lexer.scan(R"(
namespace engine {}
namespace gfx = engine::missing;
int main() {
  engine::missing::render();
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "invalid namespace source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "unknown namespace paths should be rejected");
  expect(invalidSemantic.errors().size() == 2,
         "alias targets and qualified calls should both be diagnosed");
}

void testCompileTimeConditionals() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
#if target.vendor == "apple"
int platform_value() { return 101; }
#if target.arch == "arm64"
int nested_value() { return 64; }
#else
int nested_value() { return 32; }
#endif
#elif target.os == "windows"
int platform_value() { return 202; }
#else
int platform_value() { return 303; }
#endif

#if target.os == "never"
expected<int, int> inactive_error() { return missing_name; }
@runtime("stdout.write")
void inactive_runtime(string value);
#endif

class PlatformInfo {
#if target.arch == "arm64"
  int bits = 64;
#else
  int bits = 32;
#endif
};

int main() {
#if target.os != "windows"
  int value = platform_value();
#else
  int value = platform_value();
#endif
  return value;
}
)");
  expect(!lexer.hadError(), "compile-time directives should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "compile-time branches should parse");

  const lang::TargetInfo apple{"macos", "apple", "arm64"};
  lang::SemanticVisitor appleSemantic(apple);
  expect(appleSemantic.check(program),
         "inactive branches should not participate in Apple semantics");
  const std::string appleCpp =
      lang::CppEmitter(lang::CppStandard::Cpp23, apple).emit(program);
  expect(appleCpp.find("return 101;") != std::string::npos &&
             appleCpp.find("return 64;") != std::string::npos &&
             appleCpp.find("std::int32_t bits = 64") != std::string::npos &&
             appleCpp.find("missing_name") == std::string::npos &&
             appleCpp.find("#include <expected>") == std::string::npos &&
             appleCpp.find("#include <gti/runtime.hpp>") == std::string::npos &&
             appleCpp.find("#if") == std::string::npos,
         "Apple lowering should emit only active branches without C++ macros");

  const lang::TargetInfo windows{"windows", "pc", "x86_64"};
  lang::SemanticVisitor windowsSemantic(windows);
  expect(windowsSemantic.check(program),
         "Windows should select the elif and else branches");
  const std::string windowsCpp =
      lang::CppEmitter(lang::CppStandard::Cpp23, windows).emit(program);
  expect(windowsCpp.find("return 202;") != std::string::npos &&
             windowsCpp.find("std::int32_t bits = 32") != std::string::npos &&
             windowsCpp.find("nested_value") == std::string::npos &&
             windowsCpp.find("return 101;") == std::string::npos,
         "target selection should distinguish vendor, OS, and architecture");

  auto malformedTokens = lexer.scan(R"(
#if target.os == "never"
int broken = ;
#else
int valid = 1;
#endif
)");
  lang::Parser malformedParser(std::move(malformedTokens));
  malformedParser.parse();
  expect(malformedParser.errors().size() == 1,
         "inactive branches must still be syntactically valid");

  auto invalidConditionTokens = lexer.scan(R"(
#if target.platform == "macos"
int value = 1;
#endif
)");
  lang::Parser invalidConditionParser(std::move(invalidConditionTokens));
  invalidConditionParser.parse();
  expect(invalidConditionParser.hadError(),
         "unknown target properties should be diagnosed");
}

void testRuntimeBackedStdlibSurface() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
namespace gti_internal {
namespace runtime {
@runtime("stdout.write")
void write_stdout(string value);
}
}

namespace std {
void print(string value) {
  gti_internal::runtime::write_stdout(value);
}
}

int main() {
  std::print("hello");
  return 0;
}
)");
  expect(!lexer.hadError(), "runtime-backed stdlib source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "runtime-backed stdlib source should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "runtime binding and string call signatures should validate");

  const std::string generated = lang::CppEmitter().emit(program);
  expect(generated.find("#include <gti/runtime.hpp>") != std::string::npos,
         "runtime-backed programs should include the native adapter");
  expect(generated.find("namespace gti_std") != std::string::npos &&
             generated.find("gti_std::print(std::string{\"hello\", 5})") !=
                 std::string::npos,
         "GTI std should lower outside the reserved C++ std namespace");
  expect(generated.find("const std::string &value") != std::string::npos,
         "immutable string parameters should lower by const reference");

  auto invalidTokens = lexer.scan(R"(
@runtime("stdout.write")
void fake_write(string value);
int main() { fake_write("hello"); return 0; }
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "invalid runtime declaration should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "runtime bindings outside the compiler-owned symbol should fail");
  expect(invalidSemantic.errors().size() == 1,
         "invalid runtime binding should produce one focused diagnostic");
}

void testFormatting() {
  const std::string source = R"(include   "math.gti"

namespace engine{class Counter{mut int value=0;
#if target.arch=="arm64"
int word_bits=64;
#endif
int tick(int amount)mut{if(amount>0){self.value+=amount;}else{self.value-=1;}return self.value;}};}
#if target.vendor=="apple"
int main(){for(mut int i=0;i<3;i++){if(i==1){continue ;}std::println("frame"); // keep this comment
if(i>1){break ;}
}return -1;}
#else
int main(){[[discard]] engine::run();return 0;}
#endif
)";

  const std::string expected = R"(include "math.gti"

namespace engine {
  class Counter {
    mut int value = 0;
#if target.arch == "arm64"
    int word_bits = 64;
#endif
    int tick(int amount) mut {
      if (amount > 0) {
        self.value += amount;
      } else {
        self.value -= 1;
      }
      return self.value;
    }
  };
}
#if target.vendor == "apple"
int main() {
  for (mut int i = 0; i < 3; i++) {
    if (i == 1) {
      continue;
    }
    std::println("frame"); // keep this comment
    if (i > 1) {
      break;
    }
  }
  return -1;
}
#else
int main() {
  [[discard]] engine::run();
  return 0;
}
#endif
)";

  const std::string formatted = lang::Formatter().format(source);
  if (formatted != expected) {
    std::cerr << "Formatted output was:\n" << formatted;
  }
  expect(formatted == expected,
         "formatter should produce stable C++-style GTI layout");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatting should be idempotent");

  const std::string tabIndented =
      lang::Formatter({.indentWidth = 4, .insertSpaces = false})
          .format("int main(){if(true){return 0;}}");
  expect(tabIndented.find("\n\tif (true) {\n\t\treturn 0;") !=
             std::string::npos,
         "formatter should honor tab indentation requested by an editor");
}

} // namespace

int main() {
  testFrontendBackendAndOptimizationPipeline();
  testOwnershipSemanticFoundation();
  testNonNullReferences();
  testSelfTiedReferenceReturns();
  testUniqueOwnershipAndAllocation();
  testCompilerPrivateStorage();
  testAggregateOwnershipTraits();
  testCompletePipeline();
  testLoopControlStatements();
  testFixedWidthIntegers();
  testIntegerBitwiseAndModuloOperators();
  testParserRecovery();
  testSemanticDiagnostics();
  testDiagnosticFoundation();
  testExecutablePathDiscovery();
  testDefaultImmutability();
  testClassesStructsAndAccess();
  testConstructorsAndReceiverMutability();
  testDestructorsAndActiveDropState();
  testRestrictedMemberOperators();
  testNamedGenerics();
  testExactFunctionOverloadsAndConversions();
  testFixedArrays();
  testDefaultNodiscard();
  testExpectedValues();
  testPrintIsAnIdentifier();
  testNamespacesAndAliases();
  testCompileTimeConditionals();
  testRuntimeBackedStdlibSurface();
  testFormatting();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }

  std::cout << "All compiler tests passed\n";
  return 0;
}
