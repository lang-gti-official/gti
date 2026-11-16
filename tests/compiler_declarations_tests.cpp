#include "../src/compiler/semantic_diagnostics.h"
#include "compiler_test_fixture.h"
#include "private_stdlib_fixture.h"
#include "temporary_directory.h"

namespace {

// Backend-neutral MIR lookups. Declaration emission facts are asserted on the
// verified MIR a backend consumes, never on generated backend text.
const lang::HirFunctionInstance *
hirFunctionNamed(const lang::FrontendResult &frontend, std::string_view name) {
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr && function.source->name().lexeme == name)
      return &function;
  }
  return nullptr;
}

const lang::MirFunctionInstance *
mirFunctionNamed(const lang::FrontendResult &frontend, std::string_view name) {
  const lang::HirFunctionInstance *function = hirFunctionNamed(frontend, name);
  return function == nullptr ? nullptr
                             : frontend.mir.findFunctionInstance(function->id);
}

std::vector<const lang::MirFunctionInstance *>
mirFunctionsNamed(const lang::FrontendResult &frontend, std::string_view name) {
  std::vector<const lang::MirFunctionInstance *> result;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source == nullptr || function.source->name().lexeme != name)
      continue;
    if (const lang::MirFunctionInstance *instance =
            frontend.mir.findFunctionInstance(function.id))
      result.push_back(instance);
  }
  return result;
}

// The unique MIR function instance `name` owned by class instance `owner`.
const lang::MirFunctionInstance *
mirMethodNamed(const lang::FrontendResult &frontend,
               const lang::MirClassInstance *owner, std::string_view name) {
  if (owner == nullptr)
    return nullptr;
  const lang::MirFunctionInstance *found = nullptr;
  for (const lang::MirFunctionInstance *function :
       mirFunctionsNamed(frontend, name)) {
    if (function->owner != owner->id)
      continue;
    if (found != nullptr)
      return nullptr;
    found = function;
  }
  return found;
}

std::vector<const lang::MirClassInstance *>
mirClassesNamed(const lang::FrontendResult &frontend, std::string_view name) {
  std::vector<const lang::MirClassInstance *> result;
  for (const lang::HirClassInstance &owner : frontend.hir.classInstances()) {
    if (owner.source == nullptr || owner.source->name().lexeme != name)
      continue;
    if (const lang::MirClassInstance *instance =
            frontend.mir.findClassInstance(owner.id))
      result.push_back(instance);
  }
  return result;
}

const lang::MirClassInstance *
mirClassNamed(const lang::FrontendResult &frontend, std::string_view name) {
  const auto classes = mirClassesNamed(frontend, name);
  return classes.empty() ? nullptr : classes.front();
}

template <typename Predicate>
bool anyMirInstruction(const lang::MirBody &body, Predicate predicate) {
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (!instruction.tombstone && predicate(instruction))
        return true;
    }
  }
  return false;
}

// Every executable MIR body of the program, including class field
// initializers and the module body.
template <typename Visitor>
void forEachMirBody(const lang::MirProgram &program, Visitor visitor) {
  visitor(program.module());
  for (const lang::MirFunctionInstance &function : program.functionInstances())
    visitor(function.body);
  for (const lang::MirConstructorInstance &constructor :
       program.constructorInstances())
    visitor(constructor.body);
  for (const lang::MirDestructorInstance &destructor :
       program.destructorInstances())
    visitor(destructor.body);
  for (const lang::MirLambdaInstance &lambda : program.lambdaInstances())
    visitor(lambda.body);
  for (const lang::MirClassInstance &owner : program.classInstances()) {
    visitor(owner.fieldInitializers);
    visitor(owner.staticFieldInitializers);
  }
}

template <typename Predicate>
std::size_t countMirInstructions(const lang::MirProgram &program,
                                 Predicate predicate) {
  std::size_t count = 0;
  forEachMirBody(program, [&](const lang::MirBody &body) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (!instruction.tombstone && predicate(instruction))
          ++count;
      }
    }
  });
  return count;
}

// A source function whose definition is a verified-MIR body the backend
// consumes directly.
bool hasSourceMirBody(const lang::MirFunctionInstance *function) {
  return function != nullptr &&
         function->definitionKind == lang::MirDefinitionKind::Source &&
         !function->body.blocks.empty();
}

// A place rooted at the current object and projecting `field`.
bool isThisFieldPlace(const lang::MirPlace &place) {
  return place.root == lang::MirPlaceRootKind::This &&
         !place.projections.empty() &&
         place.projections.front().kind == lang::MirProjectionKind::Field;
}

// The body loads a value out of a place matching `predicate`.
template <typename Predicate>
bool loadsFromPlace(const lang::MirBody &body, Predicate predicate) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &instruction) {
    if (instruction.kind != lang::MirInstructionKind::Load)
      return false;
    for (const lang::MirOperand &operand : instruction.operands) {
      const lang::MirPlace *place = body.findPlace(operand.place);
      if (place != nullptr && predicate(*place))
        return true;
    }
    return false;
  });
}

// The body explicitly constructs an instance of `owner` through a selected
// constructor with `arguments` explicit operands.
bool constructsThrough(const lang::FrontendResult &frontend,
                       const lang::MirBody &body,
                       const lang::MirClassInstance *owner,
                       std::size_t arguments) {
  return owner != nullptr &&
         anyMirInstruction(body, [&](const lang::MirInstruction &instruction) {
           if (instruction.kind != lang::MirInstructionKind::Construct ||
               !instruction.constructorTarget)
             return false;
           const lang::MirConstructorInstance *selected =
               frontend.mir.findConstructorInstance(
                   *instruction.constructorTarget);
           return selected != nullptr && selected->owner == owner->id &&
                  instruction.operands.size() == arguments;
         });
}

// The body calls a MIR function instance accepted by `predicate`.
template <typename Predicate>
bool callsFunction(const lang::FrontendResult &frontend,
                   const lang::MirBody &body, Predicate predicate) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &instruction) {
    if (instruction.kind != lang::MirInstructionKind::Call ||
        !instruction.functionTarget)
      return false;
    const lang::MirFunctionInstance *target =
        frontend.mir.findFunctionInstance(*instruction.functionTarget);
    return target != nullptr && predicate(*target);
  });
}

// Any executable body of the program calls a function accepted by
// `predicate`.
template <typename Predicate>
bool programCallsFunction(const lang::FrontendResult &frontend,
                          Predicate predicate) {
  bool found = false;
  forEachMirBody(frontend.mir, [&](const lang::MirBody &body) {
    found = found || callsFunction(frontend, body, predicate);
  });
  return found;
}

// The first lowered declaration of `kind` named `name`.
const lang::LoweredDeclaration *
loweredDeclarationNamed(const lang::LoweredProgram &program,
                        lang::LoweredDeclarationKind kind,
                        std::string_view name) {
  for (const lang::LoweredDeclaration &declaration : program.declarations())
    if (declaration.kind == kind && declaration.name == name)
      return &declaration;
  return nullptr;
}

// The shipped backend accepts the verified program.
bool shippedBackendAccepts(
    const lang::FrontendResult &frontend,
    const lang::TargetInfo &target = lang::TargetInfo::host()) {
  std::string error;
  const bool emitted =
      frontend.canGenerateCode() &&
      lang::verifyMirProgram(frontend.mir).valid() &&
      gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0, target,
                          &error);
  if (!emitted && !error.empty())
    std::cerr << "LLVM emission failed: " << error << '\n';
  return emitted;
}

// The shipped backend emits a native object whose symbol table names the
// exact native `symbol`.
bool nativeObjectNames(const lang::FrontendResult &frontend,
                       std::string_view symbol) {
  if (!shippedBackendAccepts(frontend))
    return false;
  const lang::BackendArtifact object = gti_test::emitLlvm(frontend);
  return object.contents.find(symbol) != std::string::npos;
}

void testMovedLoopLocalRedeclarations() {
  const lang::FrontendResult trivialLocals = lang::Frontend().analyze(
      "moved-loop-local-trivial.gti", R"(
int32_t sink(int32_t value) { return value; }

int main() {
  mut int32_t total = 0;
  mut int32_t index = 0;
  while (index < 2) {
    int32_t element = index + 1;
    total = total + std::move(element);
    index++;
  }

  for (mut int32_t iteration = 0; iteration < 2; iteration++) {
    int32_t local = iteration + 1;
    total = total + std::move(local);
  }

  mut int32_t values[4] = {0, 0, 0, 0};
  mut int32_t position = 0;
  while (position < 4) {
    int32_t element = position * 10;
    values[position] = std::move(element);
    position++;
  }

  mut int32_t guard = 0;
  while (guard < 4) {
    int32_t element = guard + 1;
    if (guard % 2 == 0) {
      total = total + sink(std::move(element));
    }
    guard++;
  }
  return total + values[3];
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(trivialLocals.canGenerateCode() && trivialLocals.mirValid &&
             trivialLocals.mir.valid(),
         "a moved trivially dropped loop-local must leave its storage "
         "re-initializable across while, for, indexed, and conditional "
         "backedge forms");

  const lang::FrontendResult genericLocals = lang::Frontend().analyze(
      "moved-loop-local-generic.gti", R"(
T accumulate<std::numeric T>(T seed, T limit) {
  mut T total = seed;
  mut T step = seed;
  while (step < limit) {
    T element = step + T(1);
    total = total + std::move(element);
    step = step + T(1);
  }
  return total;
}

int main() {
  int32_t narrow = accumulate<int32_t>(0, 2);
  int64_t wide = accumulate<int64_t>(int64_t(0), int64_t(2));
  return narrow + int32_t(wide);
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(genericLocals.canGenerateCode() && genericLocals.mirValid &&
             genericLocals.mir.valid(),
         "every generic instance must rearm a moved trivially dropped "
         "loop-local for the next iteration");

  const lang::FrontendResult lexicalLocals = lang::Frontend().analyze(
      "moved-loop-local-lexical.gti", R"(
class Tracker {
  int32_t value;

public:
  Tracker(int32_t initial) : value(initial) {}
  Tracker(Tracker&& other) = default;
  ~Tracker() {}

  int32_t read() { return this.value; }
};

int32_t consume(Tracker taken) { return taken.read(); }

int main() {
  mut int32_t total = 0;
  mut int32_t index = 0;
  while (index < 4) {
    mut Tracker element{index + 1};
    if (index % 2 == 0) {
      total = total + consume(std::move(element));
    }
    index++;
  }
  return total;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(lexicalLocals.canGenerateCode() && lexicalLocals.mirValid &&
             lexicalLocals.mir.valid(),
         "a conditionally moved lexically dropped loop-local should verify "
         "through its scope-end drop across the loop backedge");

  const lang::HirFunctionInstance *lexicalMain = nullptr;
  for (const lang::HirFunctionInstance &function :
       lexicalLocals.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "main") {
      lexicalMain = &function;
      break;
    }
  }
  lang::MirProgram hiddenScopeEnd = lexicalLocals.mir;
  std::size_t hiddenDrops = 0;
  if (lexicalMain != nullptr) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        hiddenScopeEnd.functionInstances());
    for (lang::MirFunctionInstance &function : functions) {
      if (function.id != lexicalMain->id) {
        continue;
      }
      for (lang::MirBlock &block : function.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              !instruction.destination) {
            continue;
          }
          const lang::MirPlace *place =
              function.body.findPlace(*instruction.destination);
          if (place == nullptr ||
              place->root != lang::MirPlaceRootKind::Binding ||
              !place->projections.empty() ||
              place->traits.drop != lang::DropKind::Lexical || !place->key) {
            continue;
          }
          instruction.ownership =
              lang::OwnershipEvent{.kind = lang::OwnershipEventKind::Read,
                                   .place = *place->key,
                                   .reachable = false};
          ++hiddenDrops;
        }
      }
    }
  }
  const lang::MirVerificationResult hiddenScopeEndResult =
      lang::verifyMirProgram(hiddenScopeEnd);
  expect(hiddenDrops != 0 && !hiddenScopeEndResult.valid() &&
             hasMirVerificationError(
                 hiddenScopeEndResult,
                 "initialization targets a place whose lifetime is already "
                 "active"),
         "a lexically dropped loop-local whose scope-end drop is hidden from "
         "the ownership flow model must still fail re-initialization");
}

void testTrustedIntrinsicDeclarations() {
  const lang::FrontendResult trusted = lang::Frontend().analyze(
      "trusted-intrinsic-declaration.gti", R"(
namespace utility = std;

int main() {
  int source = 1;
  int moved = utility::move(source);
  return moved;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(trusted.canGenerateCode(),
         "a namespace alias should retain the trusted move declaration's "
         "intrinsic identity");

  const lang::FunctionDecl *trustedMain =
      findTopLevelFunction(trusted.program, "main");
  const auto *trustedMoved =
      trustedMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                trustedMain->body()->statements().at(1).get());
  const auto *trustedCall =
      trustedMoved == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(trustedMoved->initializer().get());
  const lang::ResolvedCallInfo *trustedResolution =
      trustedCall == nullptr ? nullptr
                             : trusted.semantics.findCall(*trustedCall);
  const lang::FunctionInfo *trustedDeclaration =
      trustedResolution == nullptr || trustedResolution->declaration == nullptr
          ? nullptr
          : trusted.semantics.findFunction(*trustedResolution->declaration);
  const lang::SourceUnit *trustedUnit =
      trustedDeclaration == nullptr
          ? nullptr
          : trusted.sourceGraph.findUnit(trustedDeclaration->sourceUnit);
  expect(trustedResolution != nullptr && trustedResolution->function != 0 &&
             trustedResolution->intrinsic == lang::IntrinsicKind::Move &&
             trustedDeclaration != nullptr &&
             trustedDeclaration->intrinsic == lang::IntrinsicKind::Move &&
             trustedUnit != nullptr && trustedUnit->prelude,
         "resolved intrinsic calls should point to a trusted prelude "
         "declaration instead of deriving behavior from call-site spelling");
  expect(trustedResolution != nullptr &&
             std::none_of(trusted.hir.functionInstances().begin(),
                          trusted.hir.functionInstances().end(),
                          [&](const lang::HirFunctionInstance &instance) {
                            return instance.declaration ==
                                   trustedResolution->function;
                          }),
         "intrinsic declarations should remain semantic operations rather than "
         "bodyless HIR call targets");

  const lang::FrontendResult ordinary =
      lang::Frontend().analyze("ordinary-function-named-move.gti", R"(
namespace std {
T move<T>(T& value) { return value; }
}

int main() {
  int source = 1;
  int copied = std::move(source);
  return source + copied - 2;
}
)");
  expect(ordinary.canGenerateCode(),
         "an ordinary function named std::move should follow ordinary call "
         "semantics outside the trusted prelude");
  const lang::FunctionDecl *ordinaryMain =
      findTopLevelFunction(ordinary.program, "main");
  const auto *ordinarySource =
      ordinaryMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                ordinaryMain->body()->statements().front().get());
  const auto *ordinaryCopied =
      ordinaryMain == nullptr
          ? nullptr
          : dynamic_cast<const lang::VariableDecl *>(
                ordinaryMain->body()->statements().at(1).get());
  const auto *ordinaryCall = ordinaryCopied == nullptr
                                 ? nullptr
                                 : dynamic_cast<const lang::Call *>(
                                       ordinaryCopied->initializer().get());
  const lang::BindingInfo *ordinarySourceBinding =
      ordinarySource == nullptr
          ? nullptr
          : ordinary.semantics.findBinding(*ordinarySource);
  const lang::ResolvedCallInfo *ordinaryResolution =
      ordinaryCall == nullptr ? nullptr
                              : ordinary.semantics.findCall(*ordinaryCall);
  expect(ordinarySourceBinding != nullptr &&
             !ordinarySourceBinding->explicitlyMoved &&
             ordinaryResolution != nullptr &&
             ordinaryResolution->intrinsic == lang::IntrinsicKind::None,
         "untrusted declarations must not acquire ownership effects by using "
         "a standard-library spelling");

  const lang::FrontendResult ordinaryMutableReturn =
      lang::Frontend().analyze("ordinary-mutable-return.gti", R"(
mut int& expose(mut int& value) { return value; }
int main() { return 0; }
)");
  expect(ordinaryMutableReturn.canGenerateCode() &&
             lang::verifyMirProgram(ordinaryMutableReturn.mir).valid(),
         "an ordinary mutable reference relay should use the same exact "
         "argument dependency proof without acquiring an intrinsic");
}

void testThisReceiverKeyword() {
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan("this self");
  expect(tokens.size() >= 3 && tokens[0].kind == lang::TokenKind::THIS &&
             tokens[1].kind == lang::TokenKind::IDENTIFIER,
         "this should be the receiver keyword and self should remain an "
         "ordinary identifier");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("this-receiver.gti", R"(
class Counter {
  int value = 4;
public:
  int read() {
    int self = 1;
    return this.value + self - 1;
  }
public:
  Counter() = default;
};

int main() {
  Counter counter = Counter();
  return counter.read() - 4;
}
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "this member access and self identifiers should compile together");

  bool foundThisValue = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.kind == lang::HirValueKind::This) {
        foundThisValue = true;
      }
    }
  }
  expect(foundThisValue,
         "typed HIR should preserve the current-object expression");

  const lang::MirFunctionInstance *read = mirFunctionNamed(frontend, "read");
  expect(hasSourceMirBody(read) &&
             std::any_of(read->body.places.begin(), read->body.places.end(),
                         isThisFieldPlace) &&
             loadsFromPlace(read->body, isThisFieldPlace) &&
             shippedBackendAccepts(frontend),
         "GTI this should lower as a checked object receiver: a verified MIR "
         "body loads the field through a This-rooted place");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-this.gti", "int main() { return this.value; }");
  expect(
      !invalid.canGenerateCode() &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot use 'this' outside a class or struct method"),
      "this should be rejected outside an instance method");
}

void testClassesStructsAndAccess() {
  lang::Lexer lexer;
  const std::string validSource = R"(
class Vault {
  int secret = 7;

public:
  int reveal() { return this.secret; }
  int reveal_other(Vault other) { return other.secret; }
public:
  Vault() = default;
};

struct Reading {
  int value = 1;

private:
  int hidden = 2;

public:
  int total() { return this.value + this.hidden; }
public:
  Reading() = default;
};

int open(mut Vault vault) { return vault.reveal(); }
int read(Reading reading) { return reading.value; }
int main() {
  Vault vault{};
  Reading reading{};
  return vault.reveal() + reading.total() - 10;
}
)";
  auto validTokens = lexer.scan(validSource);
  expect(!lexer.hadError(), "class and struct access syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "class and struct access syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "public members and same-class private access should resolve");

  const lang::FrontendResult classFrontend = lang::Frontend().analyze(
      "classes-and-access.gti", validSource, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(classFrontend.canGenerateCode(),
         "access-controlled declaration defaults should retain verified MIR");
  if (!classFrontend.canGenerateCode())
    return;
  const auto *readingDeclaration =
      findTopLevelClass(classFrontend.program, "Reading");
  const auto *readingType =
      readingDeclaration == nullptr
          ? nullptr
          : classFrontend.semantics.findClassType(*readingDeclaration);
  const auto reading = std::find_if(
      classFrontend.mir.classInstances().begin(),
      classFrontend.mir.classInstances().end(), [&](const auto &owner) {
        return readingType != nullptr && owner.declaration == readingType->id;
      });
  expect(reading != classFrontend.mir.classInstances().end() &&
             reading->declaredFields.size() == 2 &&
             std::all_of(reading->declaredFields.begin(),
                         reading->declaredFields.end(),
                         [](const auto &field) {
                           return field.access == lang::AccessMode::ReadOnly;
                         }) &&
             std::any_of(classFrontend.mir.constructorInstances().begin(),
                         classFrontend.mir.constructorInstances().end(),
                         [&](const auto &constructor) {
                           return constructor.owner == reading->id &&
                                  constructor.generatedDefault &&
                                  constructor.initializers.size() == 2;
                         }),
         "immutable fields retain read-only MIR identity and execute their "
         "defaults through the selected generated constructor");
  const lang::MirClassInstance *vaultInstance =
      mirClassNamed(classFrontend, "Vault");
  const lang::MirFunctionInstance *reveal =
      mirFunctionNamed(classFrontend, "reveal");
  const lang::MirFunctionInstance *total =
      mirFunctionNamed(classFrontend, "total");
  expect(vaultInstance != nullptr &&
             vaultInstance->kind == lang::ClassKind::Class &&
             reading != classFrontend.mir.classInstances().end() &&
             reading->kind == lang::ClassKind::Struct &&
             hasSourceMirBody(reveal) &&
             reveal->receiverMutability == lang::ReceiverMutability::ReadOnly &&
             hasSourceMirBody(total) &&
             total->receiverMutability == lang::ReceiverMutability::ReadOnly &&
             loadsFromPlace(total->body,
                            [](const lang::MirPlace &place) {
                              return isThisFieldPlace(place) &&
                                     place.access == lang::AccessMode::ReadOnly;
                            }) &&
             shippedBackendAccepts(classFrontend),
         "MIR should preserve declaration kinds, read-only receivers, and "
         "frontend-owned field immutability for the backend");

  auto invalidTokens = lexer.scan(R"(
class A {
  int private_value = 1;
public:
  int visible = 2;
  int inspect(A other) { return other.private_value; }
public:
  A() = default;
};

struct B {
  int public_value = 1;
private:
  int hidden = 2;
public:
  B() = default;
};

A wrong_type(B value) { return value; }
int read_class_private(A value) { return value.private_value; }
int read_struct_private(B value) { return value.hidden; }
int read_missing(B value) { return value.missing; }

class Duplicate {
  int value = 1;
public:
  int value = 2;
public:
  Duplicate() = default;
};

class InvalidFields {
  mut int missing_initializer;
  int invalid_reference = missing_initializer;
  int invalid_receiver = this.private_value;
};

int invalid_global_receiver = this.private_value;
MissingType unresolved();
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid class semantics should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(
      !invalidSemantic.check(invalidProgram),
      "nominal, access, member, field, and receiver errors should be rejected");
  expect(hasDiagnostic(invalidSemantic, "Cannot return a value of type"),
         "different nominal class types should not be assignable");
  expect(hasDiagnostic(invalidSemantic, "of 'A' is private") &&
             hasDiagnostic(invalidSemantic, "of 'B' is private"),
         "class defaults and explicit private labels should be enforced");
  expect(hasDiagnostic(invalidSemantic, "Unknown member 'missing'"),
         "unknown members should be diagnosed on their nominal type");
  expect(hasDiagnostic(invalidSemantic, "Duplicate member declaration"),
         "duplicate fields and methods should be rejected");
  expect(
      hasDiagnostic(invalidSemantic, "fields must have an initializer"),
      "mutable fields should require initialization until constructors exist");
  expect(hasDiagnostic(invalidSemantic, "referenced from field initializers"),
         "field initializers should not depend on member initialization order");
  expect(hasDiagnostic(invalidSemantic, "outside a class or struct method"),
         "this should be rejected in fields and outside methods");
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
public:
  PlatformValue() = default;
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
  expect(
      recoveryParser.errors().size() == 1 &&
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

void testMutableFieldGroups() {
  const lang::TargetInfo target = lang::TargetInfo::host();
  const std::string source = R"(
#define FIELD_GROUP_ACTIVE

struct Counter {
public:
  mut {
    int first = 1;
#ifdef FIELD_GROUP_ACTIVE
    int second = 2;
#else
    int second = 20;
#endif
  }

  int identity = 4;

  void increment() mut {
    this.first++;
    this.second++;
  }

  int value() {
    return this.first + this.second + this.identity;
  }
public:
  Counter() = default;
};

[[c_abi]] struct GroupedNative {
  mut {
    int32_t x;
    int32_t y;
  }
};

[[c_abi]] struct ExpandedNative {
  mut int32_t x;
  mut int32_t y;
};

int main() {
  mut Counter counter = Counter();
  counter.increment();
  return counter.value() == 9 ? 0 : 1;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend({.target = target})
          .analyze("mutable-field-groups.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "mutable field groups should compile as ordinary mutable fields");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::ClassDecl *counter = nullptr;
  const lang::ClassDecl *groupedNative = nullptr;
  const lang::ClassDecl *expandedNative = nullptr;
  for (const lang::StmtPtr &declaration : frontend.program.declarations()) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (classDecl == nullptr) {
      continue;
    }
    if (classDecl->name().lexeme == "Counter") {
      counter = classDecl;
    } else if (classDecl->name().lexeme == "GroupedNative") {
      groupedNative = classDecl;
    } else if (classDecl->name().lexeme == "ExpandedNative") {
      expandedNative = classDecl;
    }
  }
  expect(counter != nullptr && groupedNative != nullptr &&
             expandedNative != nullptr,
         "field-group fixtures should retain their class declarations");
  if (counter == nullptr || groupedNative == nullptr ||
      expandedNative == nullptr) {
    return;
  }

  const lang::MutableFieldGroupDecl *group = nullptr;
  for (const lang::StmtPtr &member : counter->members()) {
    if (const auto *candidate =
            dynamic_cast<const lang::MutableFieldGroupDecl *>(member.get())) {
      group = candidate;
      break;
    }
  }
  expect(group != nullptr && !group->members().empty(),
         "the AST should preserve the mutable field-group source container");
  if (group == nullptr) {
    return;
  }
  const auto *first =
      dynamic_cast<const lang::VariableDecl *>(group->members().front().get());
  expect(first != nullptr && first->isMutable(),
         "grouped field declarations should carry effective mutable "
         "semantics without an individual mut token");

  const lang::ClassTypeInfo *counterInfo =
      frontend.semantics.findClassType(*counter);
  expect(counterInfo != nullptr && counterInfo->fields.size() == 3 &&
             counterInfo->fields[0].declaration->name().lexeme == "first" &&
             counterInfo->fields[1].declaration->name().lexeme == "second" &&
             counterInfo->fields[2].declaration->name().lexeme == "identity" &&
             counterInfo->fields[0].declaration->isMutable() &&
             counterInfo->fields[1].declaration->isMutable() &&
             !counterInfo->fields[2].declaration->isMutable(),
         "semantic collection should flatten active grouped fields in source "
         "order without changing following immutable fields");

  const lang::ClassTypeInfo *groupedInfo =
      frontend.semantics.findClassType(*groupedNative);
  const lang::ClassTypeInfo *expandedInfo =
      frontend.semantics.findClassType(*expandedNative);
  bool matchingAbi = groupedInfo != nullptr && expandedInfo != nullptr &&
                     groupedInfo->cAbiLayout && expandedInfo->cAbiLayout &&
                     groupedInfo->cAbiLayout->sizeBytes ==
                         expandedInfo->cAbiLayout->sizeBytes &&
                     groupedInfo->cAbiLayout->abiAlignmentBytes ==
                         expandedInfo->cAbiLayout->abiAlignmentBytes &&
                     groupedInfo->cAbiLayout->fields.size() ==
                         expandedInfo->cAbiLayout->fields.size();
  if (matchingAbi) {
    for (std::size_t index = 0; index < groupedInfo->cAbiLayout->fields.size();
         ++index) {
      const lang::CAbiRecordFieldLayout &groupedField =
          groupedInfo->cAbiLayout->fields[index];
      const lang::CAbiRecordFieldLayout &expandedField =
          expandedInfo->cAbiLayout->fields[index];
      matchingAbi =
          matchingAbi && groupedField.type == expandedField.type &&
          groupedField.offsetBytes == expandedField.offsetBytes &&
          groupedField.sizeBytes == expandedField.sizeBytes &&
          groupedField.abiAlignmentBytes == expandedField.abiAlignmentBytes;
    }
  }
  expect(matchingAbi,
         "grouped and explicitly mutable C ABI records should have identical "
         "layout facts");

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target);
  std::size_t counterFields = 0;
  bool groupLeakedIntoLowering = false;
  for (const lang::LoweredDeclaration &declaration : lowered.declarations()) {
    groupLeakedIntoLowering =
        groupLeakedIntoLowering ||
        declaration.kind == lang::LoweredDeclarationKind::Other;
    const auto *storage =
        std::get_if<lang::LoweredStorageDeclaration>(&declaration.payload);
    if (storage != nullptr && counterInfo != nullptr &&
        storage->ownerClass == counterInfo->id) {
      ++counterFields;
      if (declaration.name == "first" || declaration.name == "second") {
        expect(storage->mutability == lang::Mutability::Mutable,
               "lowered grouped fields should retain ordinary mutable field "
               "records");
      }
    }
  }
  expect(counterFields == 3 && !groupLeakedIntoLowering,
         "LoweredProgram should contain only flattened ordinary fields and no "
         "mutable-group declaration");

  const lang::MirClassInstance *counterInstance =
      mirClassNamed(frontend, "Counter");
  expect(counterInstance != nullptr &&
             counterInstance->declaredFields.size() == 3 &&
             counterInstance->declaredFields[0].access ==
                 lang::AccessMode::Mutable &&
             counterInstance->declaredFields[1].access ==
                 lang::AccessMode::Mutable &&
             counterInstance->declaredFields[2].access ==
                 lang::AccessMode::ReadOnly &&
             shippedBackendAccepts(frontend, target),
         "the backend should consume flattened ordinary field declarations "
         "only");

  const std::string formatted = lang::Formatter().format(
      "struct Sample{private:mut{int first=0;int second=1;}int id=2; public: "
      "Sample() = default; };");
  const std::string expectedFormatting = R"(struct Sample {
private:
  mut {
    int first = 0;
    int second = 1;
  }
  int id = 2;
public:
  Sample() = default;
};
)";
  expect(formatted == expectedFormatting &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should preserve and idempotently indent mutable field "
         "groups without adding a semicolon");

  const std::array<std::pair<std::string, std::string>, 12> invalidSyntax{{
      {"interface Bad { mut { int value = 0; } };",
       "Interfaces cannot contain mutable field groups"},
      {"union Bad { mut { int value = 0; } };",
       "Unions cannot contain mutable field groups"},
      {"struct Bad { mut {} };",
       "A mutable field group must contain at least one field"},
      {"struct Bad { mut { mut int value = 0; } int recovered = 1; };",
       "already mutable"},
      {"struct Bad { mut { static int value = 0; } int recovered = 1; };",
       "only direct instance fields"},
      {"struct Bad { mut { constexpr int value = 0; } "
       "int recovered = 1; };",
       "cannot contain constexpr fields"},
      {"struct Bad { mut { void run() {} } int recovered = 1; };",
       "Methods and constructors cannot be declared"},
      {"struct Bad { mut { Bad() {} } int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { ~Bad() {} } int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { operator int() {} } int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { private: int value = 0; } "
       "int recovered = 1; };",
       "only direct instance field declarations"},
      {"struct Bad { mut { mut { int value = 0; } } "
       "int recovered = 1; };",
       "already mutable"},
  }};
  for (const auto &[invalidSource, expected] : invalidSyntax) {
    const lang::FrontendResult invalid = lang::Frontend().analyze(
        "invalid-mutable-field-group.gti", invalidSource);
    expect(!invalid.canGenerateCode() &&
               hasDiagnostic(invalid.diagnostics, expected),
           "invalid mutable field-group contents should produce a focused "
           "parser diagnostic");
  }

  const lang::FrontendResult storedReference =
      lang::Frontend().analyze("mutable-field-group-reference.gti", R"(
class Borrowed {
  mut {
    int& value;
  }

public:
  Borrowed(mut int& input) : value(input) {}
};
)");
  expect(storedReference.canGenerateCode() &&
             lang::verifyMirProgram(storedReference.mir).valid(),
         "a mutable field group should form the same exact mutable stored "
         "dependency as an individually mutable reference field");
}

void testConstructorsAndReceiverMutability() {
  lang::Lexer lexer;
  const std::string validSource = R"(
class Counter {
  mut int value;
  int step = 1;

public:
  Counter() : value(0) {}
  Counter(int initial) : value(initial) {}
  Counter(bool reset) : value(0) {}
  int read() { return this.value; }
  int advance(int amount) mut {
    this.value += amount;
    return this.value;
  }
};

struct Origin {
  int x = 0;
  Origin(int initial) : x(initial) {}
public:
  Origin() = default;
};

class LifecycleValue {
  int value = 0;

public:
  LifecycleValue(LifecycleValue& other) = default;
  LifecycleValue(LifecycleValue&& other) = default;
public:
  LifecycleValue() = default;
};

class MoveOnlyPolicy {
  int value = 0;

public:
  MoveOnlyPolicy(MoveOnlyPolicy& other) = delete;
  MoveOnlyPolicy(MoveOnlyPolicy&& other) = default;
public:
  MoveOnlyPolicy() = default;
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
  LifecycleValue source{};
  LifecycleValue copied = LifecycleValue(source);
  mut LifecycleValue movable{};
  LifecycleValue moved = LifecycleValue(std::move(movable));
  mut MoveOnlyPolicy owner{};
  MoveOnlyPolicy transferred = MoveOnlyPolicy(std::move(owner));
  return observed + origin.x;
}
)";
  auto validTokens = lexer.scan(validSource);
  expect(!lexer.hadError(), "constructor and receiver syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "constructor and receiver syntax should parse");

  const lang::FrontendResult validFrontend = lang::Frontend().analyze(
      "constructors-and-receivers.gti", validSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(validFrontend.semanticValid,
         "explicit construction and mutable receiver calls should validate");

  const lang::ClassDecl *counter =
      findTopLevelClass(validFrontend.program, "Counter");
  const lang::ClassDecl *origin =
      findTopLevelClass(validFrontend.program, "Origin");
  const lang::ClassDecl *lifecycleValue =
      findTopLevelClass(validFrontend.program, "LifecycleValue");
  const lang::ClassDecl *moveOnlyPolicy =
      findTopLevelClass(validFrontend.program, "MoveOnlyPolicy");
  const lang::FunctionDecl *main =
      findTopLevelFunction(validFrontend.program, "main");
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
  const auto constructorCallAt = [&](std::size_t index) {
    const auto *declaration =
        main == nullptr ? nullptr
                        : dynamic_cast<const lang::VariableDecl *>(
                              main->body()->statements().at(index).get());
    return declaration == nullptr ? static_cast<const lang::Call *>(nullptr)
                                  : dynamic_cast<const lang::Call *>(
                                        declaration->initializer().get());
  };
  const lang::Call *copyCall = constructorCallAt(9);
  const lang::Call *moveCall = constructorCallAt(11);
  const lang::Call *moveOnlyCall = constructorCallAt(13);
  const lang::ClassLifecycleInfo *counterLifecycle =
      counter == nullptr ? nullptr
                         : validFrontend.semantics.findClassLifecycle(*counter);
  const lang::ClassLifecycleInfo *originLifecycle =
      origin == nullptr ? nullptr
                        : validFrontend.semantics.findClassLifecycle(*origin);
  const lang::ClassLifecycleInfo *valueLifecycle =
      lifecycleValue == nullptr
          ? nullptr
          : validFrontend.semantics.findClassLifecycle(*lifecycleValue);
  const lang::ClassLifecycleInfo *moveOnlyLifecycle =
      moveOnlyPolicy == nullptr
          ? nullptr
          : validFrontend.semantics.findClassLifecycle(*moveOnlyPolicy);
  const lang::ResolvedConstructionInfo *zeroConstruction =
      zeroCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*zeroCall);
  const lang::ResolvedConstructionInfo *fixedConstruction =
      fixedCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*fixedCall);
  const lang::ResolvedConstructionInfo *originConstruction =
      originCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*originCall);
  const lang::ResolvedConstructionInfo *copyConstruction =
      copyCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*copyCall);
  const lang::ResolvedConstructionInfo *moveConstruction =
      moveCall == nullptr ? nullptr
                          : validFrontend.semantics.findConstruction(*moveCall);
  const lang::ResolvedConstructionInfo *moveOnlyConstruction =
      moveOnlyCall == nullptr
          ? nullptr
          : validFrontend.semantics.findConstruction(*moveOnlyCall);
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
  expect(valueLifecycle != nullptr && moveOnlyLifecycle != nullptr &&
             valueLifecycle->declaredCopyConstructor &&
             valueLifecycle->declaredMoveConstructor &&
             valueLifecycle->declaredCopyConstructor->kind ==
                 lang::ConstructorKind::Copy &&
             valueLifecycle->declaredMoveConstructor->kind ==
                 lang::ConstructorKind::Move &&
             valueLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             valueLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             moveOnlyLifecycle->copyConstructor ==
                 lang::SpecialMemberStatus::Deleted &&
             moveOnlyLifecycle->moveConstructor ==
                 lang::SpecialMemberStatus::Generated,
         "declared copy and move policies should override lifecycle "
         "availability without entering ordinary overload sets");
  expect(zeroConstruction != nullptr && fixedConstruction != nullptr &&
             originConstruction != nullptr &&
             zeroConstruction->declaration != fixedConstruction->declaration &&
             !zeroConstruction->generatedDefault &&
             originConstruction->generatedDefault,
         "construction metadata should retain exact overload selection and "
         "generated default construction");
  expect(copyConstruction != nullptr && moveConstruction != nullptr &&
             moveOnlyConstruction != nullptr &&
             copyConstruction->kind == lang::ConstructorKind::Copy &&
             moveConstruction->kind == lang::ConstructorKind::Move &&
             moveOnlyConstruction->kind == lang::ConstructorKind::Move &&
             copyConstruction->declaration != nullptr &&
             moveConstruction->declaration != nullptr,
         "same-type construction should resolve explicitly to copy or move "
         "lifecycle operations");

  const lang::FrontendResult lifecycleFrontend = lang::Frontend().analyze(
      "copy-move-policy.gti", R"(
class Value {
  int value = 0;
public:
  Value(Value& other) = default;
  Value(Value&& other) = default;
public:
  Value() = default;
};
class GenericValue<T> {
  T value;
public:
  GenericValue(T initial) : value(initial) {}
  GenericValue(GenericValue<T>& other) = default;
  GenericValue(GenericValue<T>&& other) = default;
};
int main() {
  Value source{};
  Value copied = Value(source);
  mut Value movable{};
  Value moved = Value(std::move(movable));
  GenericValue<int> generic_source = GenericValue<int>(1);
  GenericValue<int> generic_copy = GenericValue<int>(generic_source);
  mut GenericValue<int> generic_movable = GenericValue<int>(2);
  GenericValue<int> generic_move =
      GenericValue<int>(std::move(generic_movable));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(lifecycleFrontend.canGenerateCode(),
         "copy and move policy calls should pass the complete frontend");
  std::size_t hirCopies = 0;
  std::size_t hirMoves = 0;
  for (const lang::HirFunctionInstance &function :
       lifecycleFrontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      hirCopies += value.constructorKind == lang::ConstructorKind::Copy;
      hirMoves += value.constructorKind == lang::ConstructorKind::Move;
    }
  }
  expect(hirCopies == 2 && hirMoves == 2,
         "typed HIR should preserve special construction kinds without "
         "creating source constructor bodies");

  expect(lifecycleFrontend.mir.valid(),
         "copy and move construction should lower to valid MIR");
  std::size_t mirCopies = 0;
  std::size_t mirMoves = 0;
  for (const lang::MirFunctionInstance &function :
       lifecycleFrontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct) {
          continue;
        }
        mirCopies += instruction.constructorKind == lang::ConstructorKind::Copy;
        mirMoves += instruction.constructorKind == lang::ConstructorKind::Move;
      }
    }
  }
  expect(mirCopies == 2 && mirMoves == 2,
         "MIR construct instructions should retain copy and move identity");

  const lang::MirClassInstance *counterInstance =
      mirClassNamed(validFrontend, "Counter");
  const lang::MirClassInstance *originInstance =
      mirClassNamed(validFrontend, "Origin");
  const lang::MirClassInstance *valueInstance =
      mirClassNamed(validFrontend, "LifecycleValue");
  const lang::MirClassInstance *moveOnlyInstance =
      mirClassNamed(validFrontend, "MoveOnlyPolicy");
  std::size_t counterSourceConstructors = 0;
  bool counterFieldInitialization = true;
  for (const lang::MirConstructorInstance &constructor :
       validFrontend.mir.constructorInstances()) {
    if (counterInstance == nullptr || constructor.owner != counterInstance->id)
      continue;
    ++counterSourceConstructors;
    counterFieldInitialization =
        counterFieldInitialization &&
        constructor.definitionKind == lang::MirDefinitionKind::Source &&
        !constructor.generatedDefault &&
        std::any_of(
            constructor.initializers.begin(), constructor.initializers.end(),
            [](const lang::MirConstructorInitializer &initializer) {
              return initializer.kind ==
                         lang::ConstructorInitializerTargetKind::Field &&
                     !initializer.arguments.empty();
            });
  }
  expect(counterSourceConstructors == 3 && counterFieldInitialization,
         "constructor overloads should lower explicitly with field "
         "initialization");
  const lang::MirFunctionInstance *readMethod =
      mirFunctionNamed(validFrontend, "read");
  const lang::MirFunctionInstance *advanceMethod =
      mirFunctionNamed(validFrontend, "advance");
  expect(hasSourceMirBody(readMethod) && readMethod->receiverMutability ==
                                             lang::ReceiverMutability::ReadOnly,
         "methods should lower as read-only by default");
  expect(hasSourceMirBody(advanceMethod) &&
             advanceMethod->receiverMutability ==
                 lang::ReceiverMutability::Mutable,
         "mutable receiver methods should lower with a mutable receiver");
  const lang::MirFunctionInstance *constructorMain =
      mirFunctionNamed(validFrontend, "main");
  expect(constructorMain != nullptr && originInstance != nullptr &&
             anyMirInstruction(constructorMain->body,
                               [&](const lang::MirInstruction &instruction) {
                                 if (instruction.kind !=
                                         lang::MirInstructionKind::Construct ||
                                     !instruction.constructorTarget)
                                   return false;
                                 const lang::MirConstructorInstance *selected =
                                     validFrontend.mir.findConstructorInstance(
                                         *instruction.constructorTarget);
                                 return selected != nullptr &&
                                        selected->owner == originInstance->id &&
                                        selected->generatedDefault;
                               }),
         "generated default construction should remain explicit at the call "
         "site");
  expect(counterInstance != nullptr && originInstance != nullptr &&
             valueInstance != nullptr && moveOnlyInstance != nullptr &&
             originInstance->defaultConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             counterInstance->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             counterInstance->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             counterInstance->copyAssignment ==
                 lang::SpecialMemberStatus::Generated &&
             counterInstance->moveAssignment ==
                 lang::SpecialMemberStatus::Generated &&
             counterInstance->destructorStatus ==
                 lang::SpecialMemberStatus::Generated &&
             valueInstance->copyConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             valueInstance->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             moveOnlyInstance->copyConstructor ==
                 lang::SpecialMemberStatus::Deleted &&
             moveOnlyInstance->moveConstructor ==
                 lang::SpecialMemberStatus::Generated &&
             shippedBackendAccepts(validFrontend),
         "MIR should carry explicit compiler-generated special-member "
         "availability to the backend");

  const std::string invalidSource = R"(
class MissingInitialization {
  int value;
};

class InvalidConstructor {
  int first;
  int second;

public:
  InvalidConstructor(int value)
      : second(value), first(this.second), second(value) { return; }
  InvalidConstructor(mut int value) : first(value), second(value) {}
};

class ReservedCopy {
  int value = 0;

public:
  ReservedCopy(ReservedCopy& other) : value(other.value) {}
public:
  ReservedCopy() = default;
};

class CleanupCopy {
  int value = 0;

public:
  CleanupCopy(CleanupCopy& other) = default;
  ~CleanupCopy() {}
public:
  CleanupCopy() = default;
};

class DeletedCopy {
  int value = 0;

public:
  DeletedCopy(DeletedCopy& other) = delete;
  DeletedCopy(DeletedCopy&& other) = default;
public:
  DeletedCopy() = default;
};

class DeletedMove {
  int value = 0;

public:
  DeletedMove(DeletedMove& other) = default;
  DeletedMove(DeletedMove&& other) = delete;
public:
  DeletedMove() = default;
};

class InvalidDefaultTarget {
  int value = 0;

public:
  InvalidDefaultTarget(int value) = default;
public:
  InvalidDefaultTarget() = default;
};

class PrivatePolicy {
  PrivatePolicy(PrivatePolicy& other) = delete;
public:
  PrivatePolicy() = default;
};

void invalid_rvalue_reference(DeletedCopy&& value) {}
void consume_deleted(DeletedCopy value) {}

class PrivateValue {
  int value;
  PrivateValue(int initial) : value(initial) {}
};

class MutableValue {
  mut int value = 0;

public:
  void mutate() { this.value = 1; }
  void mutate_other(MutableValue other) mut { other.value = 1; }
  void bump() mut { this.value += 1; }
public:
  MutableValue() = default;
};

class ImmutableField {
  int value = 0;

public:
  void replace() mut { this.value = 1; }
public:
  ImmutableField() = default;
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
  DeletedCopy original{};
  DeletedCopy copied = DeletedCopy(original);
  DeletedCopy copied_directly = original;
  consume_deleted(original);
  mut DeletedMove stationary{};
  DeletedMove moved = DeletedMove(std::move(stationary));
  return 0;
}
)";
  auto invalidTokens = lexer.scan(invalidSource);
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid constructor semantics should remain valid syntax");

  // This fixture intentionally checks more than twenty independent failures.
  lang::FrontendOptions exhaustiveDiagnostics;
  exhaustiveDiagnostics.limits.errorLimit = 0;
  const lang::FrontendResult invalidFrontend =
      lang::Frontend(exhaustiveDiagnostics)
          .analyze("invalid-constructors-and-receivers.gti", invalidSource,
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const std::vector<lang::Diagnostic> &invalidSemantic =
      invalidFrontend.diagnostics;
  expect(!invalidFrontend.semanticValid,
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
                       "Cannot use 'this' in a constructor initializer"),
         "constructor initializers should not observe a partial object");
  expect(hasDiagnostic(invalidSemantic,
                       "Constructors cannot contain return statements"),
         "constructor bodies should reject return statements");
  expect(hasDiagnostic(invalidSemantic,
                       "Custom copy and move constructor bodies require"),
         "custom lifecycle bodies should remain gated on place-aware moves");
  expect(hasDiagnostic(invalidSemantic,
                       "defaulted copy constructor of 'CleanupCopy' is "
                       "unavailable"),
         "a defaulted copy policy should reject structurally noncopyable "
         "state");
  expect(hasDiagnostic(invalidSemantic,
                       "Copy construction of 'DeletedCopy' is deleted"),
         "an explicitly deleted copy constructor should reject construction");
  expect(hasDiagnostic(invalidSemantic, "Cannot initialize 'copied_directly'"),
         "deleted copy policy should govern direct initialization");
  expect(hasDiagnostic(invalidSemantic, "deleted copy constructor"),
         "deleted copy policy should govern by-value calls");
  expect(hasDiagnostic(invalidSemantic, "type 'DeletedMove' is not movable"),
         "an explicitly deleted move constructor should reject std::move");
  expect(hasDiagnostic(invalidSemantic,
                       "must use the exact form 'InvalidDefaultTarget() = "
                       "default;'"),
         "ordinary constructor defaulting should require the exact "
         "zero-argument form");
  expect(
      hasDiagnostic(invalidSemantic, "constructor policies must be public") &&
          hasDiagnostic(invalidSemantic,
                        "'&&' is currently confined to a class or struct's "
                        "exact move constructor policy"),
      "the confined policy layer should reject private policies and "
      "general rvalue references");
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
      "Counter(Counter& other)=default;Counter(Counter&& other)=delete;"
      "int read(){return this.value;}void reset()mut{this.value=0;}};");
  expect(formatted.find("Counter(int initial) : value(initial) {}") !=
                 std::string::npos &&
             formatted.find("Counter(Counter& other) = default;") !=
                 std::string::npos &&
             formatted.find("Counter(Counter&& other) = delete;") !=
                 std::string::npos &&
             formatted.find("void reset() mut {") != std::string::npos,
         "formatter should distinguish constructor policies from logical "
         "operators and switch labels");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted constructors and receiver qualifiers should be idempotent");

  lang::Parser globalQualifierParser(lexer.scan("void invalid() mut {}"));
  globalQualifierParser.parse();
  expect(globalQualifierParser.hadError() &&
             globalQualifierParser.errors().front().message.find(
                 "Only class and struct methods") != std::string::npos,
         "free functions should reject receiver mutability qualifiers");
}

void testInterfaceStaticMethods() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "interface-static-methods.gti", R"(
interface Reading {
  static int initial(int value = 7) { return private_helper(value); }
  static T identity<T>(T value) requires Number<T> { return value; }
  static constexpr int twice(int value) { return value * 2; }
  unsafe static int unchecked() { return 9; }
  static int unselected();
  int read();
private:
  static int private_helper(int value) { return value; }
};
class Meter : public Reading {
public:
  Meter() {}
  int read() override { return Reading::initial(); }
};
static_assert(Reading::twice(3) == 6);
int invoke() { return Reading::initial(); }
int main() {
  Meter meter{};
  unsafe { if (Reading::unchecked() != 9) { return 1; } }
  if (Reading::identity<int>(meter.read()) != 7 || Meter::initial() != 7) { return 2; }
  return invoke() - 7;
}
interface Marker { static int code() { return 1; } };
class Marked : public Marker { public: Marked() = default; };
interface LeftFactory { static int create() { return 1; } };
interface RightFactory { static int create() { return 2; } };
class BothFactories : public LeftFactory, public RightFactory { public: BothFactories() = default; };
concept Number<T> = std::numeric<T>;
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "interface statics should reuse ordinary access, default, generic, "
         "requires, constexpr and unsafe rules");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto *reading = findTopLevelClass(frontend.program, "Reading");
  const auto *meter = findTopLevelClass(frontend.program, "Meter");
  const auto *meterType = frontend.semantics.findClassType(*meter);
  expect(meterType != nullptr && !meterType->abstract,
         "statics and unselected bodyless declarations must not add "
         "implementation requirements");
  const auto *marker = findTopLevelClass(frontend.program, "Marker");
  const auto *marked = findTopLevelClass(frontend.program, "Marked");
  const auto *markerType = frontend.semantics.findClassType(*marker);
  const auto *markedType = frontend.semantics.findClassType(*marked);
  expect(markerType && markerType->abstract && markerType->polymorphic &&
             markedType && !markedType->abstract && markedType->polymorphic,
         "an interface without instance requirements still has polymorphic "
         "destruction, without making its implementation abstract");
  const auto *both = findTopLevelClass(frontend.program, "BothFactories");
  expect(frontend.semantics.findClassType(*both) &&
             !frontend.semantics.findClassType(*both)->abstract,
         "unselected ambiguous inherited statics must not become instance "
         "contracts requiring override unification");
  const lang::FunctionDecl *initial = nullptr;
  const lang::FunctionDecl *read = nullptr;
  for (const auto &member : reading->members()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(member.get());
    if (function && function->name().lexeme == "initial")
      initial = function;
    if (function && function->name().lexeme == "read")
      read = function;
  }
  const auto *initialInfo = frontend.semantics.findFunction(*initial);
  const auto *readInfo = frontend.semantics.findFunction(*read);
  expect(initialInfo && initialInfo->staticMember &&
             !initialInfo->virtualMethod && !initialInfo->pureVirtual &&
             initialInfo->virtualRoots.empty() && readInfo &&
             !readInfo->staticMember && readInfo->virtualMethod &&
             readInfo->pureVirtual && readInfo->virtualRoots.size() == 1,
         "semantics must distinguish direct static functions from pure "
         "instance contracts");
  const auto *invoke = findTopLevelFunction(frontend.program, "invoke");
  bool hirStatic = false, hirDirect = false, mirStatic = false,
       mirDirect = false;
  for (const auto &instance : frontend.hir.functionInstances()) {
    if (instance.source == initial)
      hirStatic = instance.staticMember && !instance.virtualMethod &&
                  !instance.pureVirtual && instance.virtualRoots.empty();
    if (instance.source == invoke)
      for (const auto &value : instance.body.values)
        if (value.kind == lang::HirValueKind::Call)
          hirDirect = value.dispatch == lang::CallDispatch::Static &&
                      !value.receiver.has_value();
  }
  for (const auto &instance : frontend.mir.functionInstances()) {
    if (instance.declaration == initialInfo->id)
      mirStatic = instance.staticMember && !instance.virtualMethod &&
                  !instance.pureVirtual && instance.virtualRoots.empty();
    if (instance.declaration != frontend.semantics.findFunction(*invoke)->id)
      continue;
    for (const auto &block : instance.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call)
          mirDirect = instruction.dispatch == lang::CallDispatch::Static &&
                      !instruction.receiver.has_value();
  }
  expect(hirStatic && hirDirect && mirStatic && mirDirect,
         "HIR and MIR must preserve static identity and receiver-free direct "
         "calls, not rediscover purity from the owner kind");
  expect(shippedBackendAccepts(frontend),
         "verified static and pure instance functions must reach the native "
         "artifact; runtime coverage verifies native signatures independently");

  const auto rejected = [](std::string_view source, std::string_view code) {
    const auto result =
        lang::Frontend({.requireFunctionDefinitions = true})
            .analyze("invalid-interface-static.gti", std::string(source));
    expect(!result.canGenerateCode() &&
               findDiagnosticByCode(result.diagnostics, std::string(code)),
           "invalid interface static boundary must be rejected by " +
               std::string(code));
  };
  rejected("interface I { static int invalid() = 0; };", "GTI-S2042");
  rejected("interface I { virtual static int invalid() { return 1; } };",
           "GTI-S2042");
  rejected("interface I { static int invalid() override { return 1; } };",
           "GTI-S2042");
  rejected("interface I { int invalid() { return 1; } };", "GTI-S2041");
  rejected("interface I { int invalid() = 0; };", "GTI-S2041");
  rejected("interface I { private: int invalid(); };", "GTI-S2041");
  rejected("interface I { static int state = 1; };", "GTI-S2041");
  rejected("interface I { I() {} };", "GTI-S2041");
  rejected("interface I { ~I() {} };", "GTI-S2041");
  rejected("interface I { static int missing(); }; int main() { return "
           "I::missing(); }",
           "GTI-S2106");
  for (const auto source :
       {"interface I { static int read() { return this.read(); } };",
        "interface I { static int read() mut { return 1; } };",
        "interface I { static int read() && { return 1; } };",
        "interface I { private: static int hidden() { return 1; } }; int "
        "main(){ return I::hidden(); }",
        "interface I { unsafe static int raw() { return 1; } }; int main(){ "
        "return I::raw(); }"}) {
    const auto result =
        lang::Frontend().analyze("invalid-static-receiver.gti", source);
    expect(
        !result.canGenerateCode(),
        "ordinary static access, receiver and unsafe restrictions must apply");
  }
}

void testInheritanceAndInterfaces() {
  const std::string source = R"(
interface Renderable {
  int render(int frame);
};

interface Named {
  int name_id();
};

class Entity {
public:
  Entity(int id) : id(id) {}
  virtual int tick(int frame) { return frame + this.id; }
  virtual int tick(float frame) { return 10; }

private:
  int id;
};

class Sprite : public Entity, public Renderable, public Named {
public:
  Sprite(int id) : Entity(id) {}
  int tick(int frame) override { return frame + 2; }
  int inherited_tick() { return tick(1.5); }
  int render(int frame) override { return this.tick(frame); }
  int name_id() override { return 7; }
};

interface Reader<T> {
  T read();
};

class Box<T> : public Reader<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() override { return this.value; }
};

class ClassContract {
public:
  virtual int class_only() = 0;
public:
  ClassContract() = default;
};

class ClassImplementation : public ClassContract {
public:
  int class_only() override { return 1; }
public:
  ClassImplementation() = default;
};

int invoke(Renderable& renderable) {
  return renderable.render(3);
}

int invoke_direct(Sprite& sprite) {
  return sprite.render(3);
}

int main() {
  Sprite sprite{4};
  Box<int> box{3};
  Reader<int>& reader = box;
  int inherited_overload = sprite.tick(1.5);
  return invoke(sprite) + invoke_direct(sprite) + inherited_overload +
         sprite.inherited_tick() + reader.read() - 33;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("inheritance.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected inheritance diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "public state, interface, and generic inheritance should pass the "
         "frontend");

  const auto *renderable = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(0).get());
  const auto *entity = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(2).get());
  const auto *sprite = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(3).get());
  const auto *box = dynamic_cast<const lang::ClassDecl *>(
      frontend.program.declarations().at(5).get());
  const lang::ClassTypeInfo *renderableType =
      renderable == nullptr ? nullptr
                            : frontend.semantics.findClassType(*renderable);
  const lang::ClassTypeInfo *spriteType =
      sprite == nullptr ? nullptr : frontend.semantics.findClassType(*sprite);
  const lang::ClassLifecycleInfo *entityLifecycle =
      entity == nullptr ? nullptr
                        : frontend.semantics.findClassLifecycle(*entity);
  const lang::ClassLifecycleInfo *spriteLifecycle =
      sprite == nullptr ? nullptr
                        : frontend.semantics.findClassLifecycle(*sprite);
  expect(renderableType != nullptr && renderableType->abstract &&
             renderableType->polymorphic &&
             renderableType->kind == lang::ClassKind::Interface,
         "interfaces should be abstract polymorphic semantic types");
  expect(spriteType != nullptr && spriteType->bases.size() == 3 &&
             !spriteType->abstract && spriteType->polymorphic &&
             entityLifecycle != nullptr && entityLifecycle->polymorphic &&
             spriteLifecycle != nullptr && spriteLifecycle->polymorphic,
         "concrete derived classes should retain base and lifecycle metadata");

  const auto findMethod = [](const lang::ClassDecl *owner,
                             std::string_view name) {
    if (owner == nullptr) {
      return static_cast<const lang::FunctionDecl *>(nullptr);
    }
    for (const lang::StmtPtr &member : owner->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == name) {
        return function;
      }
    }
    return static_cast<const lang::FunctionDecl *>(nullptr);
  };
  const lang::FunctionDecl *contract = findMethod(renderable, "render");
  const lang::FunctionDecl *implementation = findMethod(sprite, "render");
  const lang::FunctionDecl *boxRead = findMethod(box, "read");
  const lang::FunctionInfo *contractInfo =
      contract == nullptr ? nullptr
                          : frontend.semantics.findFunction(*contract);
  const lang::FunctionInfo *implementationInfo =
      implementation == nullptr
          ? nullptr
          : frontend.semantics.findFunction(*implementation);
  expect(contract != nullptr && !contract->isPure() &&
             contractInfo != nullptr && contractInfo->virtualMethod &&
             contractInfo->pureVirtual && implementationInfo != nullptr &&
             implementationInfo->virtualMethod &&
             implementationInfo->overrideMethod &&
             implementationInfo->virtualRoots.size() == 1 &&
             implementationInfo->virtualRoots.front() == contractInfo->id,
         "interface signatures should become pure virtual contracts without "
         "requiring a source-level '= 0;'");

  const lang::FunctionDecl *invoke =
      findTopLevelFunction(frontend.program, "invoke");
  const auto *invokeReturn =
      invoke == nullptr || invoke->body()->statements().empty()
          ? nullptr
          : dynamic_cast<const lang::ReturnStmt *>(
                invoke->body()->statements().front().get());
  const auto *virtualCall =
      invokeReturn == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(invokeReturn->value().get());
  const lang::ResolvedCallInfo *callInfo =
      virtualCall == nullptr ? nullptr
                             : frontend.semantics.findCall(*virtualCall);
  expect(callInfo != nullptr &&
             callInfo->dispatch == lang::CallDispatch::Virtual,
         "calls through interface references should carry virtual dispatch");

  const lang::FunctionDecl *invokeDirect =
      findTopLevelFunction(frontend.program, "invoke_direct");
  const auto *directReturn =
      invokeDirect == nullptr || invokeDirect->body()->statements().empty()
          ? nullptr
          : dynamic_cast<const lang::ReturnStmt *>(
                invokeDirect->body()->statements().front().get());
  const auto *directCall =
      directReturn == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(directReturn->value().get());
  const lang::ResolvedCallInfo *directCallInfo =
      directCall == nullptr ? nullptr
                            : frontend.semantics.findCall(*directCall);
  expect(directCallInfo != nullptr && implementationInfo != nullptr &&
             spriteType != nullptr &&
             directCallInfo->function == implementationInfo->id &&
             directCallInfo->dispatch == lang::CallDispatch::Virtual &&
             directCallInfo->dispatchOwner.kind == lang::SemanticType::Class &&
             directCallInfo->dispatchOwner.classId == spriteType->id,
         "a concrete override call should retain its exact target, virtual "
         "dispatch, and concrete dispatch owner in semantics");

  const lang::HirClassInstance *entityHir = nullptr;
  const lang::HirClassInstance *spriteHir = nullptr;
  const lang::HirFunctionInstance *invokeHir = nullptr;
  const lang::HirFunctionInstance *invokeDirectHir = nullptr;
  const lang::HirFunctionInstance *boxReadHir = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == entity) {
      entityHir = &instance;
    }
    if (instance.source == sprite) {
      spriteHir = &instance;
    }
  }
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == invoke) {
      invokeHir = &instance;
    }
    if (instance.source == invokeDirect) {
      invokeDirectHir = &instance;
    }
    if (instance.source == boxRead) {
      boxReadHir = &instance;
    }
  }
  const bool hirHasVirtualCall =
      invokeHir != nullptr &&
      std::any_of(invokeHir->body.values.begin(), invokeHir->body.values.end(),
                  [](const lang::HirValue &value) {
                    return value.kind == lang::HirValueKind::Call &&
                           value.dispatch == lang::CallDispatch::Virtual &&
                           value.dispatchOwner.kind ==
                               lang::SemanticType::Class;
                  });
  const lang::HirValue *directHirCall = nullptr;
  if (invokeDirectHir != nullptr) {
    const auto found = std::find_if(
        invokeDirectHir->body.values.begin(),
        invokeDirectHir->body.values.end(), [&](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Call &&
                 value.source == directCall;
        });
    if (found != invokeDirectHir->body.values.end()) {
      directHirCall = &*found;
    }
  }
  const lang::HirFunctionInstance *directHirTarget =
      directHirCall != nullptr && directHirCall->functionTarget
          ? frontend.hir.findFunctionInstance(*directHirCall->functionTarget)
          : nullptr;
  expect(spriteHir != nullptr && spriteHir->bases.size() == 3 &&
             spriteHir->polymorphic && hirHasVirtualCall &&
             boxReadHir != nullptr && boxReadHir->owner &&
             boxReadHir->virtualMethod && boxReadHir->overrideMethod,
         "HIR should retain base instances and virtual call dispatch");
  expect(directHirCall != nullptr && directHirTarget != nullptr &&
             spriteHir != nullptr &&
             directHirCall->dispatch == lang::CallDispatch::Virtual &&
             directHirCall->dispatchOwner == spriteHir->type &&
             directHirTarget->source == implementation &&
             directHirTarget->virtualRoots.size() == 1 &&
             contractInfo != nullptr &&
             directHirTarget->virtualRoots.front() == contractInfo->id,
         "HIR should preserve the concrete override target and its exact "
         "virtual contract root");
  const lang::MirClassInstance *spriteMir =
      spriteHir == nullptr ? nullptr
                           : frontend.mir.findClassInstance(spriteHir->id);
  expect(spriteMir != nullptr && spriteMir->bases.size() == 3 &&
             spriteMir->polymorphic,
         "MIR should preserve inheritance metadata for future backends");
  const lang::MirFunctionInstance *invokeDirectMir =
      invokeDirectHir == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(invokeDirectHir->id);
  const lang::MirInstruction *directMirCall = nullptr;
  if (invokeDirectMir != nullptr && directHirTarget != nullptr) {
    for (const lang::MirBlock &block : invokeDirectMir->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == directHirTarget->id;
          });
      if (found != block.instructions.end()) {
        directMirCall = &*found;
        break;
      }
    }
  }
  const lang::MirFunctionInstance *directMirTarget =
      directMirCall != nullptr && directMirCall->functionTarget
          ? frontend.mir.findFunctionInstance(*directMirCall->functionTarget)
          : nullptr;
  expect(directMirCall != nullptr && directMirTarget != nullptr &&
             spriteMir != nullptr &&
             directMirCall->dispatch == lang::CallDispatch::Virtual &&
             directMirCall->dispatchOwner == spriteMir->type &&
             directMirCall->definedFailure.propagation ==
                 lang::FailurePropagationKind::VirtualCall &&
             directMirTarget->declaration == implementationInfo->id &&
             directMirTarget->virtualRoots.size() == 1 &&
             contractInfo != nullptr &&
             directMirTarget->virtualRoots.front() == contractInfo->id,
         "MIR should preserve the exact concrete target, virtual owner, and "
         "contract identity without devirtualizing the call");

  const lang::MirConstructorInstance *spriteConstructor = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (spriteMir != nullptr && constructor.owner == spriteMir->id) {
      expect(spriteConstructor == nullptr,
             "Sprite should have exactly one selected constructor");
      spriteConstructor = &constructor;
    }
  }
  const lang::MirConstructorInitializer *baseInitializer =
      spriteConstructor != nullptr &&
              spriteConstructor->initializers.size() == 1
          ? &spriteConstructor->initializers.front()
          : nullptr;
  const lang::MirConstructorInstance *entityConstructor =
      baseInitializer != nullptr && baseInitializer->constructorTarget
          ? frontend.mir.findConstructorInstance(
                *baseInitializer->constructorTarget)
          : nullptr;
  expect(
      baseInitializer != nullptr && entityConstructor != nullptr &&
          entityHir != nullptr &&
          baseInitializer->kind ==
              lang::ConstructorInitializerTargetKind::Base &&
          baseInitializer->base == entityHir->id &&
          baseInitializer->targetType == entityHir->type &&
          entityConstructor->owner == entityHir->id &&
          baseInitializer->explicitArgumentCount == 1 &&
          entityConstructor->parameterTypes ==
              spriteConstructor->parameterTypes,
      "Sprite should retain the exact selected Entity constructor and scalar "
      "base argument signature");
  std::size_t baseStages = 0;
  if (spriteConstructor != nullptr && entityConstructor != nullptr) {
    for (const auto &block : spriteConstructor->body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::InitializeBase)
          continue;
        ++baseStages;
        expect(instruction.constructorInitializer == 1 &&
                   instruction.constructorTarget == entityConstructor->id &&
                   instruction.parameterTypes ==
                       entityConstructor->parameterTypes &&
                   instruction.operands.size() == 1 &&
                   instruction.definedFailure.propagation ==
                       lang::FailurePropagationKind::Constructor &&
                   block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "the selected base stage should preserve constructor failure "
               "propagation through an explicit MIR invoke");
      }
    }
  }
  expect(baseStages == 1,
         "Sprite should initialize its Entity base exactly once");

  const lang::MirClassInstance *renderableMir =
      mirClassNamed(frontend, "Renderable");
  const lang::MirClassInstance *entityMir = mirClassNamed(frontend, "Entity");
  const lang::MirClassInstance *contractMir =
      mirClassNamed(frontend, "ClassContract");
  const lang::MirClassInstance *implementationMir =
      mirClassNamed(frontend, "ClassImplementation");
  const lang::MirClassInstance *boxMir = mirClassNamed(frontend, "Box");
  const lang::MirFunctionInstance *pureContract =
      mirMethodNamed(frontend, contractMir, "class_only");
  const lang::MirFunctionInstance *contractOverride =
      mirMethodNamed(frontend, implementationMir, "class_only");
  const lang::MirFunctionInstance *spriteRender =
      mirMethodNamed(frontend, spriteMir, "render");
  const lang::MirFunctionInstance *spriteNameId =
      mirMethodNamed(frontend, spriteMir, "name_id");
  const lang::MirFunctionInstance *boxReadMir =
      mirMethodNamed(frontend, boxMir, "read");
  const lang::MirFunctionInstance *inheritedTick =
      mirMethodNamed(frontend, spriteMir, "inherited_tick");
  expect(renderableMir != nullptr && renderableMir->abstract &&
             renderableMir->polymorphic &&
             renderableMir->kind == lang::ClassKind::Interface &&
             pureContract != nullptr && pureContract->pureVirtual &&
             pureContract->virtualMethod && contractOverride != nullptr &&
             contractOverride->overrideMethod &&
             hasSourceMirBody(contractOverride) &&
             hasSourceMirBody(spriteRender) && spriteRender->overrideMethod &&
             spriteRender->virtualMethod && hasSourceMirBody(spriteNameId) &&
             spriteNameId->overrideMethod && hasSourceMirBody(boxReadMir) &&
             boxReadMir->overrideMethod,
         "MIR should carry pure contracts, overrides, polymorphic interface "
         "destruction, and exactly one name_id definition to the backend");
  expect(entityMir != nullptr && hasSourceMirBody(inheritedTick) &&
             anyMirInstruction(inheritedTick->body,
                               [&](const lang::MirInstruction &instruction) {
                                 if (instruction.kind !=
                                         lang::MirInstructionKind::Call ||
                                     !instruction.functionTarget)
                                   return false;
                                 const lang::MirFunctionInstance *target =
                                     frontend.mir.findFunctionInstance(
                                         *instruction.functionTarget);
                                 return target != nullptr &&
                                        target->owner == entityMir->id &&
                                        target->parameterTypes.size() == 1 &&
                                        target->parameterTypes.front().kind ==
                                            lang::SemanticType::Float;
                               }) &&
             shippedBackendAccepts(frontend),
         "the backend should receive inheritance, base construction, and the "
         "GTI-selected inherited virtual overload from MIR");

  const std::string formatted = lang::Formatter().format(
      "interface Renderable{int render(int frame);};"
      "class Sprite:public Entity,public Renderable{public:"
      "Sprite(int id):Entity(id){}int render(int frame)override{return frame;}"
      " public: Sprite() = default; };");
  if (formatted.find("interface Renderable {") == std::string::npos ||
      formatted.find("class Sprite : public Entity, public Renderable {") ==
          std::string::npos ||
      formatted.find("int render(int frame) override {") == std::string::npos ||
      lang::Formatter().format(formatted) != formatted) {
    std::cerr << "Inheritance formatted output was:\n" << formatted;
  }
  expect(
      formatted.find("interface Renderable {") != std::string::npos &&
          formatted.find("int render(int frame);") != std::string::npos &&
          formatted.find("int render(int frame) = 0;") == std::string::npos &&
          formatted.find("class Sprite : public Entity, public Renderable {") !=
              std::string::npos &&
          formatted.find("int render(int frame) override {") !=
              std::string::npos &&
          lang::Formatter().format(formatted) == formatted,
      "inheritance and override syntax should format idempotently");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-inheritance.gti", R"(
interface Broken {
public:
  Broken() {}
  int state = 0;
  int body() { return 1; }
};

interface LegacyPureSyntax {
  int old_style() = 0;
};

class Left { public: Left() = default; };
class Right { public: Right() = default; };
class TooMany : public Left, public Right { public: TooMany() = default; };
class HiddenBase : Left { public: HiddenBase() = default; };

interface RootContract { int root(); };
interface LeftContract : public RootContract {};
interface RightContract : public RootContract {};
interface DiamondContract : public LeftContract, public RightContract {};
interface InvalidInterfaceBase : public Left { int invalid(); };

class CycleLeft : public CycleRight {};
class CycleRight : public CycleLeft {};

class Abstract {
public:
  virtual int work(int value) = 0;
public:
  Abstract() = default;
};

class Missing : public Abstract { public: Missing() = default; };
class Accidental : public Abstract {
public:
  int work(int value) { return value; }
public:
  Accidental() = default;
};
class ReturnMismatch : public Abstract {
public:
  bool work(int value) override { return true; }
public:
  ReturnMismatch() = default;
};

class RequiresArgument {
  int stored;

public:
  RequiresArgument(int value) : stored(value) {}
};
class MissingBaseInitializer : public RequiresArgument {
public:
  MissingBaseInitializer() {}
};

class Plain {
public:
  int read() { return 1; }
public:
  Plain() = default;
};
class HidesPlain : public Plain {
public:
  int read() override { return 2; }
public:
  HidesPlain() = default;
};

class PrivateState {
  int secret = 1;
public:
  PrivateState() = default;
};
class LeaksPrivateState : public PrivateState {
public:
  int leak() { return secret; }
public:
  LeaksPrivateState() = default;
};

class Concrete : public Abstract {
public:
  int work(int value) override { return value; }
public:
  Concrete() = default;
};
int consume(Abstract& value) { return value.work(1); }
int main() {
  Missing missing{};
  Concrete concrete{};
  Abstract sliced = concrete;
  return consume(concrete);
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid inheritance and interface programs should be rejected");
  expect(hasDiagnostic(invalid.diagnostics, "Interfaces are behavior-only") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interfaces cannot declare constructors") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interface methods are pure contracts") &&
             hasDiagnostic(invalid.diagnostics,
                           "Interface methods are implicitly pure"),
         "interfaces should permit only declaration-only public behavior "
         "contracts and reject redundant '= 0;' syntax");
  expect(hasDiagnostic(invalid.diagnostics, "only one state-bearing base") &&
             hasDiagnostic(invalid.diagnostics, "Inheritance must be public"),
         "inheritance should be public with at most one state-bearing base");
  expect(hasDiagnostic(invalid.diagnostics,
                       "An interface can inherit only from other interfaces") &&
             hasDiagnostic(invalid.diagnostics,
                           "inheritance diamonds are not supported") &&
             hasDiagnostic(invalid.diagnostics, "Inheritance cycle"),
         "interface bases, inheritance diamonds, and cycles should be "
         "diagnosed before lowering");
  expect(hasDiagnostic(invalid.diagnostics, "Overriding methods must use") &&
             hasDiagnostic(invalid.diagnostics,
                           "Override return type must exactly match") &&
             hasDiagnostic(invalid.diagnostics,
                           "same signature as a non-virtual base method"),
         "override checking should be exact, explicit, and virtual-only");
  expect(
      hasDiagnostic(invalid.diagnostics, "must explicitly initialize base") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot construct abstract class 'Missing'"),
      "derived construction should initialize bases and reject abstract "
      "objects");
  expect(hasDiagnostic(invalid.diagnostics,
                       "Member 'secret' of 'PrivateState' is private") &&
             hasDiagnostic(invalid.diagnostics, "Cannot initialize 'sliced'"),
         "private state access and object slicing should stay rejected while "
         "public-derived lvalues bind to base reference parameters");
}

void testInheritedGenericTargetInstances() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("inherited-generic-targets.gti", R"(
class Base<T> {
public:
  T echo(T value) { return value; }
public:
  Base() = default;
};

class Derived<First, Second> : public Base<Second> {
public:
  Second indirect(Second value) { return echo(value); }
public:
  Derived() = default;
};

class SizedBase<T, uint64_t N> {
public:
  uint64_t extent() { return N; }
public:
  SizedBase() = default;
};

class SizedDerived<T, uint64_t N> : public SizedBase<T, N> { public: SizedDerived() = default; };

int main() {
  Derived<int32_t, uint64_t> value{};
  SizedDerived<int32_t, 3> sized{};
  uint64_t direct = value.echo(uint64_t(7));
  uint64_t indirect = value.indirect(uint64_t(8));
  uint64_t extent = sized.extent();
  return int32_t(direct + indirect + extent) - 18;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected inherited-generic diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "inherited methods should retain their resolved generic base "
         "instance through HIR and MIR");

  const lang::ClassDecl *base = findTopLevelClass(frontend.program, "Base");
  const lang::FunctionDecl *echo = nullptr;
  if (base != nullptr) {
    for (const lang::StmtPtr &member : base->members()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(member.get());
      if (function != nullptr && function->name().lexeme == "echo") {
        echo = function;
        break;
      }
    }
  }

  std::vector<const lang::HirClassInstance *> baseInstances;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == base) {
      baseInstances.push_back(&instance);
    }
  }
  const lang::HirClassInstance *concreteBase =
      baseInstances.size() == 1 ? baseInstances.front() : nullptr;
  expect(concreteBase != nullptr && concreteBase->typeArguments.size() == 1 &&
             concreteBase->typeArguments.front() == lang::SemanticType::UInt64,
         "inherited calls should enqueue only the resolved Base<uint64_t> "
         "owner, not a symbolic or derived-argument-shaped base");

  const lang::ClassDecl *sizedBase =
      findTopLevelClass(frontend.program, "SizedBase");
  std::vector<const lang::HirClassInstance *> sizedBaseInstances;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == sizedBase) {
      sizedBaseInstances.push_back(&instance);
    }
  }
  const lang::HirClassInstance *concreteSizedBase =
      sizedBaseInstances.size() == 1 ? sizedBaseInstances.front() : nullptr;
  expect(concreteSizedBase != nullptr &&
             concreteSizedBase->typeArguments.size() == 1 &&
             concreteSizedBase->typeArguments.front() ==
                 lang::SemanticType::Int32 &&
             concreteSizedBase->valueArguments.size() == 1 &&
             concreteSizedBase->valueArguments.front().kind ==
                 lang::CompileTimeValue::UInt64 &&
             concreteSizedBase->valueArguments.front().value == 3,
         "inherited calls should preserve the resolved base's type and value "
         "arguments together");

  std::vector<const lang::HirFunctionInstance *> echoInstances;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == echo) {
      echoInstances.push_back(&instance);
    }
  }
  const lang::HirFunctionInstance *concreteEcho =
      echoInstances.size() == 1 ? echoInstances.front() : nullptr;
  expect(concreteEcho != nullptr && concreteEcho->owner.has_value() &&
             concreteBase != nullptr &&
             *concreteEcho->owner == concreteBase->id &&
             concreteEcho->returnType == lang::SemanticType::UInt64 &&
             concreteEcho->parameterTypes.size() == 1 &&
             concreteEcho->parameterTypes.front() == lang::SemanticType::UInt64,
         "the inherited method target should use the concrete base owner and "
         "signature selected by semantics");

  const auto containsUnresolvedType =
      [&](const auto &self, const lang::SemanticType &type) -> bool {
    return type.kind == lang::SemanticType::TypeParameter ||
           type.kind == lang::SemanticType::TypePack ||
           std::any_of(type.arguments.begin(), type.arguments.end(),
                       [&](const lang::SemanticType &argument) {
                         return self(self, argument);
                       });
  };
  bool concreteBody = concreteEcho != nullptr;
  if (concreteEcho != nullptr) {
    concreteBody =
        std::none_of(concreteEcho->body.bindings.begin(),
                     concreteEcho->body.bindings.end(),
                     [&](const lang::HirBinding &binding) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     binding.info.type);
                     }) &&
        std::none_of(concreteEcho->body.values.begin(),
                     concreteEcho->body.values.end(),
                     [&](const lang::HirValue &value) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     value.info.type);
                     });
  }
  expect(concreteBody,
         "a concrete inherited method must not retain symbolic class types in "
         "its executable HIR body");

  bool callsUseConcreteTarget = concreteEcho != nullptr;
  if (concreteEcho != nullptr) {
    std::size_t calls = 0;
    for (const lang::HirFunctionInstance &function :
         frontend.hir.functionInstances()) {
      for (const lang::HirValue &value : function.body.values) {
        if (value.functionTarget == concreteEcho->id) {
          ++calls;
          callsUseConcreteTarget =
              callsUseConcreteTarget &&
              value.dispatchOwner.kind == lang::SemanticType::Class &&
              value.dispatchOwner.classId == concreteBase->declaration &&
              value.dispatchOwner.arguments.size() == 1 &&
              value.dispatchOwner.arguments.front() ==
                  lang::SemanticType::UInt64;
        }
      }
    }
    callsUseConcreteTarget = callsUseConcreteTarget && calls == 2;
  }
  expect(callsUseConcreteTarget,
         "direct and unqualified inherited calls should share the same "
         "resolved concrete HIR target");

  const lang::MirFunctionInstance *mirEcho =
      concreteEcho == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(concreteEcho->id);
  bool concreteMir = mirEcho != nullptr &&
                     mirEcho->body.returnType == lang::SemanticType::UInt64;
  if (mirEcho != nullptr) {
    concreteMir =
        concreteMir &&
        std::none_of(mirEcho->body.places.begin(), mirEcho->body.places.end(),
                     [&](const lang::MirPlace &place) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     place.type);
                     }) &&
        std::none_of(mirEcho->body.values.begin(), mirEcho->body.values.end(),
                     [&](const lang::MirValue &value) {
                       return containsUnresolvedType(containsUnresolvedType,
                                                     value.info.type);
                     });
  }
  expect(concreteMir,
         "MIR for a concrete inherited method should contain only substituted "
         "types");
}

void testDirectBraceConstruction() {
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T initial) : value(initial) {}
  T read() { return this.value; }
};

using IntBox = Box<int>;

class Holder {
  Box<int> box{3};

public:
  int read() { return this.box.read(); }
public:
  Holder() = default;
};

class Pair {
  int left;
  int right;

public:
  Pair(int left, int right) : left(left), right(right) {}
  int sum() { return this.left + this.right; }
};

struct Point {
  int x = 4;
public:
  Point() = default;
};

int main() {
  Box<int> boxed{1};
  mut Box<int> changing{2};
  IntBox aliased{3,};
  Holder holder{};
  Point point{};
  Pair pair{5, 6,};
  return boxed.read() + changing.read() + aliased.read() + holder.read() +
         point.x + pair.sum();
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("direct-initialization.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected direct-initialization diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "class direct initialization should pass the shared frontend");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *boxed = main == nullptr || main->body()->statements().empty()
                          ? nullptr
                          : dynamic_cast<const lang::VariableDecl *>(
                                main->body()->statements().front().get());
  const auto *initializer = boxed == nullptr
                                ? nullptr
                                : dynamic_cast<const lang::DirectInitializer *>(
                                      boxed->initializer().get());
  const lang::ResolvedConstructionInfo *construction =
      initializer == nullptr
          ? nullptr
          : frontend.semantics.findConstruction(*initializer);
  expect(initializer != nullptr && initializer->arguments().size() == 1 &&
             lang::AstPrinter().print(*initializer) == "(direct-init 1)",
         "the AST should preserve direct constructor arguments");
  expect(construction != nullptr && construction->constructor != 0 &&
             construction->constructedType.kind == lang::SemanticType::Class,
         "semantics should retain the exact selected constructor identity");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainInstance = &instance;
      break;
    }
  }
  const lang::HirStatement *boxedStatement =
      mainInstance == nullptr || mainInstance->body.roots.empty()
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots.front());
  const lang::HirValue *boxedValue =
      boxedStatement == nullptr || !boxedStatement->value
          ? nullptr
          : mainInstance->body.findValue(*boxedStatement->value);
  expect(boxedValue != nullptr &&
             boxedValue->kind == lang::HirValueKind::DirectInitializer &&
             boxedValue->operands.size() == 1 &&
             boxedValue->constructorTarget.has_value(),
         "HIR should retain direct initialization and its constructor edge");

  const lang::MirFunctionInstance *braceMain =
      mirFunctionNamed(frontend, "main");
  expect(hasSourceMirBody(braceMain) &&
             constructsThrough(frontend, braceMain->body,
                               mirClassNamed(frontend, "Box"), 1) &&
             constructsThrough(frontend, braceMain->body,
                               mirClassNamed(frontend, "Holder"), 0) &&
             constructsThrough(frontend, braceMain->body,
                               mirClassNamed(frontend, "Pair"), 2) &&
             shippedBackendAccepts(frontend),
         "the backend should lower braces through explicit constructor calls");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-direct-initialization.gti", R"(
class Sample {
public:
  Sample(int value) {}
public:
  Sample() = default;
};

int main() {
  int primitive{1};
  int values[1]{1};
  auto inferred{1};
  Sample copy_list = {1};
  Sample& reference{1};
  Sample mismatch{true};
  Sample missing;
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "unsupported brace forms and constructor mismatches should fail");
  expect(
      countDiagnosticCode(invalid.diagnostics, "GTI-S2038") == 3 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2092") == 1 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2028") == 1 &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2012") == 1 &&
          hasDiagnostic(invalid.diagnostics,
                        "limited to classes and structs") &&
          hasDiagnostic(invalid.diagnostics, "initialize a fixed array") &&
          hasDiagnostic(invalid.diagnostics,
                        "Collection initialization requires") &&
          hasDiagnostic(invalid.diagnostics, "cannot initialize a reference") &&
          hasDiagnostic(invalid.diagnostics, "require explicit construction"),
      "direct initialization should diagnose each unsupported form at the "
      "GTI layer");

  lang::Lexer lexer;
  lang::Parser recoveryParser(lexer.scan(R"(
class Sample {
public:
  Sample(int value) {}
};
int main() {
  Sample broken{1;
  Sample retained{2};
  return retained.value;
}
)",
                                         "direct-recovery.gti"));
  const lang::Program recovered = recoveryParser.parse();
  const lang::FunctionDecl *recoveredMain =
      findTopLevelFunction(recovered, "main");
  expect(recoveryParser.hadError() && recoveredMain != nullptr &&
             recoveredMain->body()->statements().size() == 2,
         "a malformed direct initializer should preserve later declarations");

  lang::Parser parenthesizedParser(lexer.scan(R"(
class Sample {
public:
  Sample(int value) {}
};
int main() {
  mut Sample value(1);
  return 0;
}
)"));
  parenthesizedParser.parse();
  expect(parenthesizedParser.hadError() &&
             hasDiagnostic(parenthesizedParser.errors(),
                           "use 'Type name{arguments};'"),
         "block-scope parenthesized declarations should direct users to the "
         "unambiguous brace form");

  const std::string formatted = lang::Formatter().format(
      "class Box<T>{public:Box(T value){} public: Box() = default; };int "
      "main(){mut Box<int> "
      "value { 1, };Box<int> empty { };int values[1]={1};return 0;}");
  if (formatted.find("mut Box<int> value{1,};") == std::string::npos ||
      formatted.find("Box<int> empty{};") == std::string::npos ||
      formatted.find("int values[1] = {1};") == std::string::npos) {
    std::cerr << "Direct-initialization formatted output was:\n" << formatted;
  }
  expect(formatted.find("mut Box<int> value{1,};") != std::string::npos &&
             formatted.find("Box<int> empty{};") != std::string::npos &&
             formatted.find("int values[1] = {1};") != std::string::npos,
         "formatter should keep direct braces compact and distinct from "
         "array initialization");
  expect(lang::Formatter().format(formatted) == formatted,
         "direct brace formatting should be idempotent");
}

void testRecursiveClassLayouts() {
  const std::string directSource = R"(
struct Cell {
  int value;
  Cell inner;

public:
  Cell(int value, Cell cell) : value(value), inner(cell) {}
};
)";
  const lang::FrontendResult direct =
      lang::Frontend().analyze("recursive-class-layout.gti", directSource);
  const lang::Diagnostic *directDiagnostic =
      findDiagnosticByCode(direct.diagnostics, "GTI-S2082");
  const std::size_t fieldType = directSource.find("Cell inner");
  expect(!direct.canGenerateCode() && directDiagnostic != nullptr &&
             directDiagnostic->primary.start == fieldType &&
             directDiagnostic->primary.end == fieldType + 4 &&
             countDiagnosticCode(direct.diagnostics, "GTI-S2082") == 1 &&
             countDiagnosticCode(direct.diagnostics, "GTI-S2003") == 0 &&
             hasRelatedDiagnostic(direct.diagnostics,
                                  "recursively contained type") &&
             hasDiagnosticHint(direct.diagnostics, "std::unique_ptr<Cell>"),
         "ordinary classes should reject direct by-value recursion at the "
         "field without cascading into constructor conversion diagnostics");

  const lang::FrontendResult generic =
      lang::Frontend().analyze("recursive-generic-layout.gti", R"(
class Node<T> {
  T value;
  Node<T> next;

public:
  Node(T value, Node<T> next) : value(value), next(next) {}
};
)");
  expect(!generic.canGenerateCode() &&
             countDiagnosticCode(generic.diagnostics, "GTI-S2082") == 1 &&
             countDiagnosticCode(generic.diagnostics, "GTI-S2003") == 0 &&
             hasDiagnosticHint(generic.diagnostics, "std::unique_ptr<Node<T>>"),
         "generic self-layout diagnostics should preserve the written owner "
         "type in their indirection hint");

  const lang::FrontendResult fixedArray =
      lang::Frontend().analyze("recursive-array-layout.gti", R"(
struct Branch {
  Branch children[2];
};
)");
  expect(!fixedArray.canGenerateCode() &&
             countDiagnosticCode(fixedArray.diagnostics, "GTI-S2082") == 1 &&
             hasDiagnostic(fixedArray.diagnostics, "through field 'children'"),
         "fixed arrays should preserve their by-value recursive layout edge");

  for (const auto &field :
       {"expected<Cell, int> inner;", "expected<int, Cell> inner;",
        "expected<Cell, int> inner[2];"}) {
    const std::string source =
        "struct Cell {\n  " + std::string(field) + "\n};\n";
    const auto result = lang::Frontend().analyze(
        "recursive-expected-layout.gti", source, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    const auto *diagnostic =
        findDiagnosticByCode(result.diagnostics, "GTI-S2082");
    const auto start = source.find("expected");
    expect(!result.canGenerateCode() && diagnostic != nullptr &&
               diagnostic->primary.start == start &&
               diagnostic->primary.end == start + 8 &&
               countDiagnosticCode(result.diagnostics, "GTI-S2082") == 1 &&
               countDiagnosticCode(result.diagnostics, "GTI-B0001") == 0 &&
               hasRelatedDiagnostic(result.diagnostics,
                                    "recursively contained type") &&
               hasDiagnosticHint(result.diagnostics, "std::unique_ptr<Cell>"),
           "inline Expected alternatives preserve the recursive layout edge "
           "at the source field, including arrays and the error alternative");
  }
}

void testNamedGenerics() {
  lang::Lexer lexer;
  const std::string source = R"(
class Box<T> {
  mut T value;

public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
  U echo<U>(U replacement) { return replacement; }
  void set(T replacement) mut { this.value = replacement; }
};

T identity<T>(T value) { return value; }
T unbox<T>(Box<T> box) { return box.get(); }
U relay<T, U>(Box<T> box, U value) { return box.echo<U>(value); }

int main() {
  mut Box<int> box = Box<int>(identity(7));
  box.set(identity<int>(9));
  int value = unbox(box);
  int relayed = relay(box, box.echo<int>(value));
  std::string_view text = identity<std::string_view>("generic");
  return relayed;
}

)";
  auto validTokens = lexer.scan(source);
  expect(!lexer.hadError(), "named generic source should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(),
         "generic classes, functions, and applications should parse");

  const lang::FrontendResult validFrontend = lang::Frontend().analyze(
      "named-generics.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!validFrontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : validFrontend.diagnostics) {
      std::cerr << "Unexpected generic diagnostic: "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(validFrontend.canGenerateCode(),
         "generic substitution and exact inference should validate");

  const auto genericBoxes = mirClassesNamed(validFrontend, "Box");
  expect(genericBoxes.size() == 1 &&
             genericBoxes.front()->type.kind == lang::SemanticType::Class,
         "a selected nameable generic class should lower as one GTI-owned "
         "concrete class instance");
  const auto identities = mirFunctionsNamed(validFrontend, "identity");
  const auto unboxes = mirFunctionsNamed(validFrontend, "unbox");
  const auto concreteGeneric = [](const lang::MirFunctionInstance *function) {
    return hasSourceMirBody(function) && function->typeArguments.size() == 1 &&
           function->typeArguments.front().kind != lang::SemanticType::Unknown;
  };
  expect(
      identities.size() == 2 &&
          std::all_of(identities.begin(), identities.end(), concreteGeneric) &&
          identities[0]->typeArguments != identities[1]->typeArguments &&
          unboxes.size() == 1 && concreteGeneric(unboxes.front()),
      "nameable generic functions should lower as GTI-selected concrete "
      "functions");
  const lang::MirFunctionInstance *genericMain =
      mirFunctionNamed(validFrontend, "main");
  const auto callsInstance =
      [&](const lang::MirFunctionInstance *caller,
          const std::vector<const lang::MirFunctionInstance *> &targets) {
        return caller != nullptr &&
               anyMirInstruction(
                   caller->body, [&](const lang::MirInstruction &instruction) {
                     return instruction.kind ==
                                lang::MirInstructionKind::Call &&
                            instruction.functionTarget &&
                            std::any_of(targets.begin(), targets.end(),
                                        [&](const auto *target) {
                                          return target->id ==
                                                 *instruction.functionTarget;
                                        });
                   });
      };
  const auto sets = mirFunctionsNamed(validFrontend, "set");
  const auto echoes = mirFunctionsNamed(validFrontend, "echo");
  expect(hasSourceMirBody(genericMain) && !genericBoxes.empty() &&
             constructsThrough(validFrontend, genericMain->body,
                               genericBoxes.front(), 1) &&
             callsInstance(genericMain, identities) &&
             callsInstance(genericMain, sets) && sets.size() == 1 &&
             sets.front()->owner == genericBoxes.front()->id,
         "applied types and explicit generic calls should lower recursively");
  expect(!echoes.empty() &&
             std::all_of(echoes.begin(), echoes.end(),
                         [&](const lang::MirFunctionInstance *echo) {
                           return concreteGeneric(echo) &&
                                  !genericBoxes.empty() &&
                                  echo->owner == genericBoxes.front()->id;
                         }) &&
             callsInstance(genericMain, echoes) &&
             callsInstance(mirFunctionNamed(validFrontend, "relay"), echoes) &&
             shippedBackendAccepts(validFrontend),
         "the backend should receive the concrete selected generic method "
         "without reopening dependent-template lookup");

  auto invalidTokens = lexer.scan(R"(
class Duplicate<T, T> { public: Duplicate() = default; };
class SameName<SameName> { public: SameName() = default; };
class Shadow<T> {
public:
  T replace<T>(T value) { return value; }
public:
  Shadow() = default;
};
class Box<T> {
  T value;
public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
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
  Box<int, bool> excessive = Box<int, bool>(1);
  Box<void> impossible = Box<void>(1);
  int mismatch = identity<int>(true);
  int conflict = choose(1, true);
  int unknown = make();
  int excessive_types = identity<int, bool>(1);
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
  expect(
      hasDiagnostic(invalidSemantic, "No constructor of 'Box' exactly matches"),
      "void metadata must not make an incompatible constructor viable");
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
      "this.value;}};T identity<T>(T value){return value;}int main(){Box<"
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

void testNativeGenericCarrierClosure() {
  const std::string source = R"(
class Leaf<T> {
  T value;

public:
  Leaf(T value) : value(value) {}
  T get() { return this.value; }
};

class DormantCarrier<T> {
  Leaf<T> value;

public:
  DormantCarrier(Leaf<T> value) : value(value) {}
};

int main() {
  Leaf<int> value = Leaf<int>(7);
  return value.get();
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("native-generic-carrier-closure.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected carrier-closure diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "a dormant dependent generic carrier should validate");

  const auto leaves = mirClassesNamed(frontend, "Leaf");
  expect(leaves.size() == 1 &&
             mirClassesNamed(frontend, "DormantCarrier").empty() &&
             constructsThrough(frontend,
                               mirFunctionNamed(frontend, "main")->body,
                               leaves.front(), 1) &&
             shippedBackendAccepts(frontend),
         "a dormant dependent generic carrier should not be instantiated while "
         "concrete calls keep their direct concrete instance");
}

void testUninstantiatedGenericSignatureClosure() {
  const auto frontend =
      lang::Frontend().analyze("generic-signature-closure.gti", R"(
struct Result<T> { T value; };
class Dormant<T> {
  Result<char> field = {.value = 'x'};
public:
  Dormant(Result<int8_t> input) {}
  Result<int> result();
  void accept(Result<bool> input);
  Result<T> dependent(Result<T> input) { return input; }
public:
  Dormant() = default;
};
int main() { return 0; }
)");
  expect(frontend.canGenerateCode(),
         "unused generic signatures should reach verified MIR");
  if (!frontend.canGenerateCode())
    return;
  std::size_t concrete = 0;
  bool dependentOwner = false;
  for (const auto &instance : frontend.hir.classInstances()) {
    if (instance.source == nullptr)
      continue;
    if (instance.source->name().lexeme == "Result") {
      ++concrete;
      expect(instance.typeArguments.size() == 1 &&
                 instance.typeArguments.front().kind !=
                     lang::SemanticType::TypeParameter,
             "only concrete signature components should be instantiated");
    }
    dependentOwner |= instance.source->name().lexeme == "Dormant";
  }
  expect(concrete == 4 && !dependentOwner,
         "return, parameter, constructor and field types must close without a "
         "dependent owner");
  expect(
      frontend.hir.functionInstances().size() == 1,
      "signature discovery must not instantiate an unused generic method body");
  expect(mirClassesNamed(frontend, "Result").size() == 4 &&
             shippedBackendAccepts(frontend),
         "each concrete declaration signature type must have a representation "
         "identity");
}

void testDependentCarrierRepresentationClosure() {
  const auto frontend = analyzeTrustedPreludeFixture("carrier-closure.gti", R"(
struct Leaf<T> { T value; Leaf(T value) : value(std::move(value)) {} };
struct Wrapper<T> { Leaf<T> leaf; Wrapper(T value) : leaf(Leaf<T>(std::move(value))) {} };
struct Independent<T> { T value; };
class Owner<T> {
  using value_type = T;
public:
  Owner() {}
  Wrapper<T> result(T value) { return Wrapper<T>(std::move(value)); }
  Independent<int> concrete();
};
int main() { Owner<int> owner{}; auto value = owner.result(42); return value.leaf.value - 42; }
)");
  expect(frontend.canGenerateCode(), "dependent carrier closure must verify");
  if (!frontend.canGenerateCode())
    return;
  const auto concreteInstances = [&](std::string_view name) {
    const auto classes = mirClassesNamed(frontend, name);
    return classes.size() == 1 &&
           classes.front()->type.kind == lang::SemanticType::Class;
  };
  expect(concreteInstances("Owner") && concreteInstances("Wrapper") &&
             concreteInstances("Leaf") && shippedBackendAccepts(frontend),
         "dependent owner result and transitive field must close as concrete "
         "MIR class instances of the selected application");
  expect(concreteInstances("Independent"),
         "fully concrete signature references must retain a direct concrete "
         "class instance");
}

void testMoveConstructionCapability() {
  const auto valid = lang::Frontend().analyze(
      "move-construction-capability.gti", R"(
class Cursor {
  int& source;
public:
  Cursor(int& value) : source(value) {}
};
bool accepts<T>(T& value) requires std::move_constructible<T> { return true; }
bool from_movable<std::movable T>(T& value) { return accepts(value); }
bool from_relocation<std::relocatable T>(T& value) { return accepts(value); }
bool from_either<T>(T& value)
  requires std::movable<T> || std::relocatable<T> { return accepts(value); }
int main() {
  int source = 7;
  Cursor cursor{source};
  if (accepts(cursor) && from_movable(source) && from_relocation(source) &&
      from_either(source)) { return 0; }
  return 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid() &&
             shippedBackendAccepts(valid),
         "construction-only user cursors and stronger symbolic contracts "
         "should satisfy move construction without requiring assignment");

  const std::string invalidSource = R"(
class Frozen {
public:
  Frozen() {}
  Frozen(Frozen&& other) = delete;
};
class Cursor {
  int& source;
public:
  Cursor(int& value) : source(value) {}
};
bool constructible<T>(T& value) requires std::move_constructible<T> { return true; }
bool assignable<T>(T& value) requires std::movable<T> { return true; }
bool cannot_strengthen<std::move_constructible T>(T& value) {
  return assignable(value);
}
int main() {
  Frozen frozen;
  int source = 7;
  Cursor cursor{source};
  [[discard]] constructible(frozen);
  [[discard]] assignable(cursor);
  return 0;
}
)";
  const auto invalid = lang::Frontend().analyze(
      "move-construction-rejected.gti", invalidSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2029") &&
             hasDiagnostic(invalid.diagnostics, "std::move_constructible") &&
             hasDiagnostic(invalid.diagnostics, "std::movable") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "move assignment is not required") &&
             !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001"),
         "deleted construction and unavailable assignment must remain "
         "distinct source-level constraint failures");
  const auto failure = std::find_if(
      invalid.diagnostics.begin(), invalid.diagnostics.end(),
      [](const auto &item) {
        return item.code == "GTI-S2029" &&
               item.message.find("type 'Frozen'") != std::string::npos;
      });
  const auto argument = invalidSource.find("constructible(frozen)") +
                        std::string("constructible(").size();
  expect(
      failure != invalid.diagnostics.end() &&
          failure->primary.start == argument &&
          failure->primary.end == argument + std::string("frozen").size() &&
          !failure->related.empty() && failure->fixes.empty(),
      "move-construction failure points at the argument, relates its contract, "
      "and offers no ownership-changing automatic fix");
}

void testFiniteWrapperTraits() {
  const auto frontend = lang::Frontend().analyze(
      "finite-wrapper-traits.gti", R"(
struct Wrapper<T> { T value; };
struct Closed { Wrapper<int> nested; };
struct RawField { int* pointer = nullptr;  public: RawField() = default; };
struct ClosedRaw { Wrapper<RawField> nested; };
int main() {
  Wrapper<Closed> finite = {.value = {.nested = {.value = 7}}};
  Wrapper<ClosedRaw> raw = {.value = {.nested = {.value = {.pointer = nullptr}}}};
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "a closed declaration breaks generic growth dependence, not exact "
         "cycle detection");
  const auto *main = findTopLevelFunction(frontend.program, "main");
  const auto traits =
      [&](const char *name) -> const lang::SemanticTypeTraits * {
    const auto *local = findDirectLocal(main, name);
    const auto *binding =
        local ? frontend.semantics.findBinding(*local) : nullptr;
    return binding ? &binding->traits : nullptr;
  };
  const auto *finite = traits("finite");
  const auto *raw = traits("raw");
  expect(finite && finite->movable && finite->copyable &&
             finite->relocation == lang::RelocationKind::Bitwise &&
             finite->transferCapable && finite->shareCapable,
         "finite passive wrappers retain their structural capabilities");
  expect(raw && raw->movable && !raw->transferCapable && !raw->shareCapable,
         "finite wrapper traversal must reach a raw field rather than "
         "treating a repeated generic name as a harmless recursive edge");
}

void testCompilerPrivateMemberRequirements() {
  // Trusted source may name a compiler-private probe type, so its direct
  // member queries must see that type's members exactly as a call written
  // there would, including a member constrained by a private source concept.
  // Equality policy no longer has a raw compiler-bound declaration.
  const auto trusted =
      analyzeTrustedPreludeFixture("compiler-private-member-probe.gti", R"(
namespace gti_internal {
concept probe_equality<E> = std::equality_comparable<E>;
struct plain_probe<T> {
  bool matches() { return true; }
public:
  plain_probe() = default;
};
struct underlying_probe<E, U> {
  bool matches() requires gti_internal::probe_equality<E> && std::enum_underlying<E, U> {
    return true;
  }
public:
  underlying_probe() = default;
};
}
enum class small : uint8_t { low, high };
int inline_width<E>() { return 8; }
int boxed_width<E>() { return 0; }
int underlying_width<E>() {
  if constexpr (requires(gti_internal::underlying_probe<E, uint8_t>& probe) {
                  { probe.matches() } -> bool;
                }) {
    return inline_width<E>();
  } else {
    return boxed_width<E>();
  }
}
bool call_matches<P>(P& probe)
    requires requires(P& item) { { item.matches() } -> bool; } {
  return probe.matches();
}
int main() {
  static_assert(requires(gti_internal::plain_probe<int>& probe) {
    { probe.matches() } -> bool;
  });
  static_assert(requires(gti_internal::underlying_probe<small, uint8_t>& probe) {
    { probe.matches() } -> bool;
  });
  static_assert(
      !(requires(gti_internal::underlying_probe<small, int32_t>& probe) {
        { probe.matches() } -> bool;
      }));
  gti_internal::plain_probe<int> probe{};
  return call_matches(probe) ? underlying_width<small>() - 8 : 1;
}
)");
  for (const auto &diagnostic : trusted.diagnostics) {
    std::cerr << "Compiler-private member probe diagnostic: "
              << diagnostic.message << '\n';
  }
  expect(trusted.canGenerateCode(),
         "trusted direct member requirements must select compiler-private "
         "members, including through a requires-requires clause");
  const auto instantiated = [&](std::string_view name) {
    return std::any_of(trusted.hir.functionInstances().begin(),
                       trusted.hir.functionInstances().end(),
                       [&](const lang::HirFunctionInstance &instance) {
                         return instance.source != nullptr &&
                                instance.source->name().lexeme == name;
                       });
  };
  expect(instantiated("inline_width") && !instantiated("boxed_width"),
         "a private source member constraint must select the satisfied "
         "if-constexpr probe branch, not silently fall back");

  // A named concept is a public predicate: it never observes a
  // compiler-private member, even when trusted source declares and applies
  // it, and the failure names the reason instead of claiming the member is
  // not public.
  const auto named =
      analyzeTrustedPreludeFixture("compiler-private-member-concept.gti", R"(
namespace gti_internal {
struct plain_probe<T> {
  bool matches() { return true; }
public:
  plain_probe() = default;
};
}
concept probe_matches<P> = requires(P& item) { { item.matches() } -> bool; };
bool call_named<P>(P& probe) requires probe_matches<P> {
  return probe.matches();
}
int main() {
  gti_internal::plain_probe<int> probe{};
  return call_named(probe) ? 0 : 1;
}
)");
  expect(!named.canGenerateCode() &&
             hasDiagnosticCode(named.diagnostics, "GTI-S2029") &&
             hasRelatedDiagnostic(named.diagnostics,
                                  "Member 'matches()' is compiler-private"),
         "named concepts must not admit compiler-private members");
}

void testValueGenerics() {
  const std::string source = R"(
constexpr uint64_t default_array_base = 2;
constexpr uint64_t default_array_size = default_array_base + 1;

class StaticArray<T, uint64_t N> {
  T values[N] = {};

public:
  uint64_t size() { return N; }
  T first() { return this.values[0]; }
public:
  StaticArray() = default;
};

class WrappedArray<T, uint64_t N> {
  StaticArray<T, N> value = StaticArray<T, N>();

public:
  uint64_t size() { return this.value.size(); }
public:
  WrappedArray() = default;
};

class DefaultArray<T = int, uint64_t N = default_array_size> {
  T values[N] = {};

public:
  uint64_t size() { return N; }
public:
  DefaultArray() = default;
};

int main() {
  StaticArray<int, 4> four = StaticArray<int, 4>();
  StaticArray<int, 2 + 2> arithmetic = StaticArray<int, 2 + 2>();
  StaticArray<int, 8> eight = StaticArray<int, 8>();
  WrappedArray<int, 4> wrapped = WrappedArray<int, 4>();
  DefaultArray defaulted = DefaultArray();
  if (four.size() == 4 and arithmetic.size() == 4 and eight.size() == 8 and
      wrapped.size() == 4 and four.first() == 0 and defaulted.size() == 3) {
    return 0;
  }
  return 1;
}
)";

  lang::FrontendResult valid =
      lang::Frontend().analyze("value-generics.gti", source);
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected value generic diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "uint64_t value parameters should support class identity and array "
         "extents");

  bool foundFour = false;
  bool foundEight = false;
  bool foundDefaultThree = false;
  for (const lang::HirClassInstance &instance : valid.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "DefaultArray" &&
        instance.typeArguments.size() == 1 &&
        instance.typeArguments.front() == lang::SemanticType::Int32 &&
        instance.valueArguments.size() == 1 &&
        instance.valueArguments.front().kind ==
            lang::CompileTimeValue::UInt64 &&
        instance.valueArguments.front().value == 3) {
      foundDefaultThree = true;
    }
    if (instance.source == nullptr ||
        instance.source->name().lexeme != "StaticArray" ||
        instance.valueArguments.size() != 1 || instance.fields.empty()) {
      continue;
    }
    const lang::CompileTimeValue length = instance.valueArguments.front();
    const lang::SemanticType &storage = instance.fields.front().info.type;
    if (length.kind == lang::CompileTimeValue::UInt64 &&
        storage.kind == lang::SemanticType::Array &&
        storage.arrayLengthParameterId == 0 &&
        storage.arrayLength == length.value) {
      foundFour = foundFour || length.value == 4;
      foundEight = foundEight || length.value == 8;
    }
  }
  expect(foundFour && foundEight && foundDefaultThree,
         "HIR should keep value arguments in class identity and substitute "
         "symbolic array extents, including declaration-owned defaults");

  const std::size_t namedDefault =
      source.find("N = default_array_size") + std::string("N = ").size();
  const lang::SourceUnitId valueGenericsUnit = valid.sourceGraph.entryUnit();
  const lang::SemanticOccurrence *namedDefaultOccurrence =
      valid.semantics.database().findOccurrence(valueGenericsUnit,
                                                namedDefault + 1);
  const lang::SymbolRecord *namedDefaultSymbol =
      valid.semantics.database().findSymbolAt(valueGenericsUnit,
                                              namedDefault + 1);
  expect(namedDefaultOccurrence != nullptr &&
             lang::hasRole(namedDefaultOccurrence->roles,
                           lang::OccurrenceRole::Reference) &&
             lang::hasRole(namedDefaultOccurrence->roles,
                           lang::OccurrenceRole::Read) &&
             namedDefaultSymbol != nullptr &&
             namedDefaultSymbol->kind == lang::SymbolKind::GlobalVariable &&
             namedDefaultSymbol->name == "default_array_size",
         "an early namespace constexpr generic default should retain an exact "
         "tooling reference to its declaration after registration-time "
         "normalization");

  const auto staticArrays = mirClassesNamed(valid, "StaticArray");
  bool mirFour = false;
  bool mirEight = false;
  for (const lang::MirClassInstance *instance : staticArrays) {
    if (instance->declaredFields.empty())
      continue;
    const lang::SemanticType &storage = instance->declaredFields.front().type;
    if (storage.kind != lang::SemanticType::Array ||
        storage.arrayLengthParameterId != 0)
      continue;
    mirFour = mirFour || storage.arrayLength == 4;
    mirEight = mirEight || storage.arrayLength == 8;
  }
  expect(staticArrays.size() == 2 && mirFour && mirEight &&
             mirClassesNamed(valid, "WrappedArray").size() == 1 &&
             mirClassesNamed(valid, "DefaultArray").size() == 1 &&
             shippedBackendAccepts(valid),
         "the backend should receive each GTI-selected mixed type/value "
         "application as an ordinary concrete class with substituted extents "
         "and no native template selection or defaults");

  lang::Lexer lexer;
  lang::Parser invalidParser(lexer.scan(R"(
class WrongType<T, int N> { public: WrongType() = default; };
class WrongOrder<uint64_t N, T> { public: WrongOrder() = default; };
class NeedsValue<T, uint64_t N> { public: NeedsValue() = default; };
class ImmutableValue<uint64_t N> {
public:
  void overwrite() { N = 2; }
public:
  ImmutableValue() = default;
};
uint64_t invalid_function<uint64_t N>() { return N; }
T same_default<T>(T value) { return value; }
T same_default<T = int>(T value) { return value; }
NeedsValue<int, int> wrong_value;
NeedsValue<4, 4> wrong_type;
NeedsValue<int, Missing> missing_value;
int invalid_extent[Missing] = {};
class MissingAfterDefault<T = int, U> { public: MissingAfterDefault() = default; };
class ForwardDefault<T = U, U = int> { public: ForwardDefault() = default; };
class ForwardValue<uint64_t N = M, uint64_t M = 2> { public: ForwardValue() = default; };
class BadValueDefault<uint64_t N = 1 / 0> { public: BadValueDefault() = default; };
class BadValueUnderflow<uint64_t N = 1 - 2> { public: BadValueUnderflow() = default; };
class BadValueOverflow<uint64_t N = 18446744073709551615 + 1> { public: BadValueOverflow() = default; };
class BadValueName<uint64_t N = Missing> { public: BadValueName() = default; };
void default_pack<T = int, Args...>() {}
class ConstructorDefault {
public:
  ConstructorDefault<T = int>(T value) {}
public:
  ConstructorDefault() = default;
};
)"));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid value generic programs should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "invalid value generic declarations and arguments should fail");
  expect(hasDiagnostic(invalidSemantic, "currently require type uint64_t") &&
             hasDiagnostic(invalidSemantic,
                           "type parameters must appear before value") &&
             hasDiagnostic(invalidSemantic,
                           "requires a uint64_t compile-time value") &&
             hasDiagnostic(invalidSemantic, "requires a type argument") &&
             hasDiagnostic(invalidSemantic,
                           "Cannot assign to compile-time value parameter") &&
             hasDiagnostic(invalidSemantic,
                           "is not an in-scope constexpr integer or uint64_t "
                           "value generic") &&
             hasDiagnostic(invalidSemantic,
                           "without a default cannot follow a defaulted") &&
             hasDiagnostic(invalidSemantic,
                           "may reference only visible types and earlier") &&
             hasDiagnostic(invalidSemantic,
                           "may reference only earlier value parameters") &&
             hasDiagnostic(invalidSemantic,
                           "cannot divide or take modulo by zero") &&
             hasDiagnostic(invalidSemantic, "underflows uint64_t") &&
             hasDiagnostic(invalidSemantic, "overflows uint64_t") &&
             hasDiagnostic(invalidSemantic, "must use uint64_t literals") &&
             hasDiagnostic(invalidSemantic,
                           "parameter list that contains a type pack") &&
             hasDiagnostic(invalidSemantic,
                           "Constructor-local generic parameters are "
                           "inference-only") &&
             hasDiagnostic(invalidSemantic,
                           "Duplicate overload signature for 'same_default'"),
         "value generic diagnostics should identify unsupported parameter "
         "types, ordering, scope, defaults, and argument kind without "
         "requiring every value to be inferable");

  const std::string invalidDefaultsSource =
      "T constrained<std::integral T = double>() { return T(); }\n"
      "uint64_t selected<uint64_t N>() { return N; }\n"
      "uint64_t dependent_underflow<uint64_t N, uint64_t M = N - 2>() { "
      "return M; }\n"
      "int main() { return int(selected<1 / 0>() + "
      "dependent_underflow<1>()); }\n";
  const lang::FrontendResult invalidDefaults = lang::Frontend().analyze(
      "invalid-generic-defaults.gti", invalidDefaultsSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const auto invalidExpression = std::find_if(
      invalidDefaults.diagnostics.begin(), invalidDefaults.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2026" &&
               diagnostic.message.find(
                   "Function value generic argument arithmetic cannot") !=
                   std::string::npos;
      });
  const auto dependentUnderflow = std::find_if(
      invalidDefaults.diagnostics.begin(), invalidDefaults.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2026" &&
               diagnostic.message.find(
                   "underflows uint64_t for the selected") != std::string::npos;
      });
  expect(!invalidDefaults.canGenerateCode() &&
             hasDiagnostic(invalidDefaults.diagnostics,
                           "does not satisfy generic constraint") &&
             hasDiagnostic(invalidDefaults.diagnostics,
                           "Function value generic argument arithmetic "
                           "cannot divide or take modulo by zero") &&
             invalidExpression != invalidDefaults.diagnostics.end() &&
             invalidExpression->primary.source.ends_with(
                 "/invalid-generic-defaults.gti") &&
             invalidExpression->primary.start ==
                 invalidDefaultsSource.find('/') &&
             dependentUnderflow != invalidDefaults.diagnostics.end() &&
             dependentUnderflow->primary.start ==
                 invalidDefaultsSource.find("N - 2") + 2,
         "invalid constrained defaults and closed value expressions should "
         "remain precisely anchored GTI diagnostics rather than native "
         "substitution failures");

  const lang::FrontendResult lateDefault = lang::Frontend().analyze(
      "late-generic-default.gti", "class LateDefault<uint64_t N = later> { "
                                  "public: LateDefault() = default; };\n"
                                  "constexpr uint64_t later = 3;\n");
  expect(!lateDefault.canGenerateCode() &&
             hasDiagnostic(lateDefault.diagnostics,
                           "must use uint64_t literals, visible uint64_t "
                           "constexpr bindings, or earlier value parameters"),
         "generic defaults should not see a namespace constexpr binding "
         "declared later in source order");

  const std::string formatted = lang::Formatter().format(
      "class StaticArray<T,uint64_t N>{T values[N]={}; public: StaticArray() = "
      "default; };StaticArray<int,4> "
      "values=StaticArray<int,4>();T choose<T=int,uint64_t N=2+1>(){return "
      "T();}");
  expect(formatted.find("class StaticArray<T, uint64_t N> {") !=
                 std::string::npos &&
             formatted.find("StaticArray<int, 4> values =") !=
                 std::string::npos &&
             formatted.find("T choose<T = int, uint64_t N = 2 + 1>() {") !=
                 std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "value generic defaults and dependent expressions should format "
         "idempotently");

  lang::Parser recoveryParser(
      lexer.scan("T broken<T = >(); int recovered() { return 0; }"));
  lang::Program recovered = recoveryParser.parse();
  expect(recoveryParser.hadError() &&
             findTopLevelFunction(recovered, "recovered") != nullptr,
         "a malformed generic default should recover at the declaration "
         "boundary and preserve the following function");

  lang::Parser typeArgumentParser(lexer.scan(R"(
class Widget { public: Widget() = default; };
T identity<T>(T value) { return value; }
void accept_type_arguments(Widget* pointer) {
  identity<Widget*>(pointer);
}
class CallbackHolder<T> { public: CallbackHolder() = default; };
void accept_callback(CallbackHolder<(int)->void> callback) {}
)"));
  (void)typeArgumentParser.parse();
  if (typeArgumentParser.hadError()) {
    for (const lang::ParseDiagnostic &diagnostic :
         typeArgumentParser.errors()) {
      std::cerr << "Unexpected generic type-argument parser diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(!typeArgumentParser.hadError(),
         "dependent value arithmetic lookahead should preserve pointer and "
         "native-function generic type arguments");
}

void testContextualArrayArguments() {
  const std::string source = R"(
int total<uint64_t N>(int values[N]) {
  mut int result = 0;
  for (mut uint64_t index = 0; index < N; index++) {
    result += values[index];
  }
  return result;
}

T first<T, uint64_t N>(T fallback, T values[N]) { return values[0]; }

constexpr uint64_t exact_extent<uint64_t N>() { return N; }

int relay_total<uint64_t N>(int values[N]) { return total<N>(values); }

int choose(int values[2]) { return 2; }
int choose(int values[3]) { return 3; }

class Total {
  int stored;

public:
  Total<uint64_t N>(int values[N]) : stored(total(values)) {}
  int read() { return this.stored; }
  int add<uint64_t N>(int values[N]) { return this.stored + total(values); }
};

int main() {
  int summed = total({1, 2, 3});
  int selected = first(0, {4, 5});
  int extent = choose({6, 7, 8});
  Total object = Total({9, 10});
  int method = object.add({2, 3});
  int named_values[2] = {11, 12};
  int named = total(named_values);
  int explicit_total = total<3>({3, 4, 5});
  int explicit_mixed = first<int, 2>(0, {6, 7});
  int relayed = relay_total<2>({8, 9});
  uint64_t explicit_extent = exact_extent<4>();
  int explicit_method = object.add<2>({1, 1});
  return summed + selected + extent + object.read() + method + named +
         explicit_total + explicit_mixed + relayed + int(explicit_extent) +
         explicit_method - 139;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("contextual-arrays.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected contextual-array diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "brace call arguments should form exact contextual fixed-array "
         "values through semantics, HIR, and MIR");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *summed = main == nullptr || main->body()->statements().empty()
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements().front().get());
  const auto *sumCall =
      summed == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(summed->initializer().get());
  const auto *sumInitializer =
      sumCall == nullptr || sumCall->arguments().empty()
          ? nullptr
          : dynamic_cast<const lang::ArrayInitializer *>(
                sumCall->arguments().front().get());
  const lang::ResolvedCallInfo *resolvedSum =
      sumCall == nullptr ? nullptr : frontend.semantics.findCall(*sumCall);
  expect(sumInitializer != nullptr && sumInitializer->elements().size() == 3 &&
             resolvedSum != nullptr &&
             resolvedSum->valueArguments.size() == 1 &&
             resolvedSum->valueArguments.front().kind ==
                 lang::CompileTimeValue::UInt64 &&
             resolvedSum->valueArguments.front().value == 3 &&
             resolvedSum->parameterTypes.size() == 1 &&
             resolvedSum->parameterTypes.front().kind ==
                 lang::SemanticType::Array &&
             resolvedSum->parameterTypes.front().arrayLength == 3,
         "the AST and semantic call record should retain the brace elements "
         "and inferred exact extent");

  const auto *object = main == nullptr || main->body()->statements().size() < 4
                           ? nullptr
                           : dynamic_cast<const lang::VariableDecl *>(
                                 main->body()->statements()[3].get());
  const auto *objectCall =
      object == nullptr
          ? nullptr
          : dynamic_cast<const lang::Call *>(object->initializer().get());
  const lang::ResolvedConstructionInfo *resolvedObject =
      objectCall == nullptr ? nullptr
                            : frontend.semantics.findConstruction(*objectCall);
  expect(resolvedObject != nullptr &&
             resolvedObject->valueArguments.size() == 1 &&
             resolvedObject->valueArguments.front().kind ==
                 lang::CompileTimeValue::UInt64 &&
             resolvedObject->valueArguments.front().value == 2 &&
             resolvedObject->parameterTypes.size() == 1 &&
             resolvedObject->parameterTypes.front().kind ==
                 lang::SemanticType::Array &&
             resolvedObject->parameterTypes.front().arrayLength == 2,
         "generic constructors should infer and retain a brace argument's "
         "fixed-array extent");

  const lang::FunctionDecl *total =
      findTopLevelFunction(frontend.program, "total");
  bool foundTotalThree = false;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != total || instance.valueArguments.size() != 1) {
      continue;
    }
    foundTotalThree =
        foundTotalThree ||
        (instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 3 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 3);
  }

  bool foundConstructorTwo = false;
  for (const lang::HirConstructorInstance &instance :
       frontend.hir.constructorInstances()) {
    foundConstructorTwo =
        foundConstructorTwo ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "Total" &&
         instance.valueArguments.size() == 1 &&
         instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 2 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 2);
  }
  bool foundMethodTwo = false;
  bool foundExtentFour = false;
  bool foundRelayTwo = false;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    foundMethodTwo =
        foundMethodTwo ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "add" &&
         instance.valueArguments.size() == 1 &&
         instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 2 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 2);
    foundExtentFour =
        foundExtentFour || (instance.source != nullptr &&
                            instance.source->name().lexeme == "exact_extent" &&
                            instance.valueArguments.size() == 1 &&
                            instance.valueArguments.front().kind ==
                                lang::CompileTimeValue::UInt64 &&
                            instance.valueArguments.front().value == 4 &&
                            instance.parameterTypes.empty());
    foundRelayTwo =
        foundRelayTwo ||
        (instance.source != nullptr &&
         instance.source->name().lexeme == "relay_total" &&
         instance.valueArguments.size() == 1 &&
         instance.valueArguments.front().kind ==
             lang::CompileTimeValue::UInt64 &&
         instance.valueArguments.front().value == 2 &&
         instance.parameterTypes.size() == 1 &&
         instance.parameterTypes.front().kind == lang::SemanticType::Array &&
         instance.parameterTypes.front().arrayLength == 2);
  }
  expect(foundTotalThree && foundConstructorTwo && foundMethodTwo &&
             foundExtentFour && foundRelayTwo,
         "HIR instance identity should include inferred and explicit function "
         "and constructor value arguments, including methods and symbolic "
         "forwarding");

  std::size_t mirroredValueFunctions = 0;
  bool exactMirIdentity = true;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr || instance.valueArguments.empty()) {
      continue;
    }
    const std::string_view name = instance.source->name().lexeme;
    if (name != "total" && name != "add" && name != "exact_extent" &&
        name != "relay_total") {
      continue;
    }
    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    exactMirIdentity = exactMirIdentity && mir != nullptr &&
                       mir->typeArguments == instance.typeArguments &&
                       mir->valueArguments == instance.valueArguments;
    ++mirroredValueFunctions;
  }
  expect(exactMirIdentity && mirroredValueFunctions >= 4,
         "MIR function identity should preserve every concrete HIR type and "
         "value argument rather than reconstructing it from parameter types");

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "LoweredProgram should retain and verify the exact MIR function "
         "value-generic identity");

  lang::MirProgram forged = frontend.mir;
  auto &forgedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forged.functionInstances());
  const auto forgedValue =
      std::find_if(forgedFunctions.begin(), forgedFunctions.end(),
                   [](const lang::MirFunctionInstance &instance) {
                     return !instance.valueArguments.empty() &&
                            instance.valueArguments.front().kind ==
                                lang::CompileTimeValue::UInt64;
                   });
  if (forgedValue != forgedFunctions.end()) {
    ++forgedValue->valueArguments.front().value;
  }
  const lang::LoweredProgramBuild rejected =
      lang::LoweredProgramBuilder().build(frontend.program, frontend.semantics,
                                          frontend.hir, forged, forged, {},
                                          lang::TargetInfo::host());
  expect(forgedValue != forgedFunctions.end() && !rejected.valid(),
         "the HIR-to-MIR boundary should reject a forged function value "
         "argument before backend planning");

  const auto isInt32Array = [](const lang::SemanticType &type,
                               std::uint64_t length) {
    return type.kind == lang::SemanticType::Array &&
           type.arrayLengthParameterId == 0 && type.arrayLength == length &&
           type.arguments.size() == 1 &&
           type.arguments.front() == lang::SemanticType::Int32;
  };
  const lang::MirFunctionInstance *arrayMain =
      mirFunctionNamed(frontend, "main");
  const auto materializesArray = [&](std::uint64_t length) {
    return hasSourceMirBody(arrayMain) &&
           anyMirInstruction(
               arrayMain->body, [&](const lang::MirInstruction &instruction) {
                 return instruction.operation ==
                            lang::MirOperation::Aggregate &&
                        isInt32Array(instruction.info.type, length);
               });
  };
  const auto totals = mirFunctionsNamed(frontend, "total");
  expect(materializesArray(3) && materializesArray(2) &&
             std::any_of(totals.begin(), totals.end(),
                         [&](const lang::MirFunctionInstance *total) {
                           return hasSourceMirBody(total) &&
                                  total->parameterTypes.size() == 1 &&
                                  isInt32Array(total->parameterTypes.front(),
                                               3);
                         }) &&
             shippedBackendAccepts(frontend),
         "MIR should materialize an explicit fixed-array value and pass it to "
         "a concrete extent instance, never delegating list semantics to "
         "backend overload resolution");

  const std::string invalidSource = R"(
int scalar(int value) { return value; }
int fixed(int values[2]) { return values[0]; }
int ambiguous(int values[2]) { return 1; }
int ambiguous(int64_t values[2]) { return 2; }
int duplicate<uint64_t N>(int values[N]) { return 1; }
int duplicate<uint64_t M>(int values[M]) { return 2; }
int uninferable<uint64_t N>() { return int(N); }
int explicit_extent<uint64_t N>(int values[N]) { return values[0]; }
T no_common_type<T, uint64_t N>(T values[N]) { return values[0]; }
T type_only<T>(T value) { return value; }
int two_extents<uint64_t N, uint64_t M>(int first[N], int second[M]) {
  return first[0] + second[0];
}

class DuplicateConstructor {
public:
  DuplicateConstructor<uint64_t N>(int values[N]) {}
  DuplicateConstructor<uint64_t M>(int values[M]) {}
public:
  DuplicateConstructor() = default;
};

class ShadowedConstructor<uint64_t N> {
public:
  ShadowedConstructor<uint64_t N>(int values[N]) {}
public:
  ShadowedConstructor() = default;
};

int main() {
  int no_context = scalar({1});
  int wrong_count = fixed({1, 2, 3});
  int wrong_element = fixed({1, true});
  int unresolved = uninferable();
  int no_common = no_common_type({1, 2});
  int explicit_value = explicit_extent<2>({1, 2});
  int wrong_kind = type_only<2>(1);
  int missing_type_before_value = no_common_type<2>({1, 2});
  int wrong_value_count = two_extents<2>({1, 2}, {3, 4});
  int value_then_type = no_common_type<2, int>({1, 2});
  int non_generic_value = scalar<2>(1);
  int ambiguous_value = ambiguous({1, 2});
  return 0;
}
)";
  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-contextual-arrays.gti", invalidSource);
  expect(
      !invalid.canGenerateCode() &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2011") == 2 &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot infer generic type parameter 'T'") &&
          hasDiagnostic(invalid.diagnostics,
                        "wrong number of type arguments") &&
          hasDiagnostic(invalid.diagnostics,
                        "wrong number of explicit value arguments") &&
          hasDiagnostic(invalid.diagnostics,
                        "A function value generic argument must be an "
                        "in-scope uint64_t compile-time value") &&
          hasDiagnostic(
              invalid.diagnostics,
              "Non-generic functions do not take generic arguments") &&
          hasDiagnostic(invalid.diagnostics,
                        "Constructor-local generic parameters cannot shadow") &&
          hasDiagnostic(invalid.diagnostics,
                        "A brace argument requires one exact fixed-array") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot initialize array element") &&
          hasDiagnostic(invalid.diagnostics, "is ambiguous"),
      "contextual arrays and explicit value arguments should reject duplicate "
      "generic shapes, missing array context, invalid argument kinds/counts, "
      "and ambiguous overloads at the GTI layer");

  // Resolution knows that the written 'int' occupies a value slot. Retain
  // PR111's owning value-kind diagnostic rather than the old syntactic-order
  // message; accepting the program or moving its anchor to the whole call
  // would still fail this regression.
  const auto wrongValueKind = std::find_if(
      invalid.diagnostics.begin(), invalid.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2026" &&
               diagnostic.message ==
                   "A function value generic argument must be an in-scope "
                   "uint64_t compile-time value.";
      });
  const auto valueKindOffset = invalidSource.find("int>({1, 2})");
  expect(wrongValueKind != invalid.diagnostics.end() &&
             valueKindOffset != std::string::npos &&
             wrongValueKind->primary.source.ends_with(
                 "invalid-contextual-arrays.gti") &&
             wrongValueKind->primary.start == valueKindOffset &&
             wrongValueKind->primary.end == valueKindOffset + 3,
         "a type in an explicit value slot has the exact owning diagnostic "
         "and written-argument span");

  const auto wrongExtent =
      std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2015" &&
                            diagnostic.message.find("provides 3 elements") !=
                                std::string::npos;
                   });
  expect(wrongExtent != invalid.diagnostics.end() &&
             wrongExtent->primary.source.ends_with(
                 "/invalid-contextual-arrays.gti") &&
             wrongExtent->primary.start == invalidSource.find("{1, 2, 3}") &&
             wrongExtent->primary.end == wrongExtent->primary.start + 1 &&
             wrongExtent->fixes.empty(),
         "a contextual-array extent diagnostic should point at the written "
         "brace and avoid an unsafe mechanical conversion");

  const std::string formatted = lang::Formatter().format(
      "int total<uint64_t N>(int values[N]){return values[0];}int "
      "main(){return total<3>({1,2,3});}");
  expect(formatted.find("total<3>({1, 2, 3})") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "contextual brace arguments and explicit value generics should "
         "format idempotently");
}

void testExactFunctionOverloadsAndConversions() {
  const std::string source = R"(
namespace math {
uint64_t pow(uint64_t base, uint64_t exponent) { return base * exponent; }
float pow(float base, float exponent) { return base * exponent; }
}

struct Selector<T, U> {
  T apply(T value) { return value; }
  U apply(U value) { return value; }
public:
  Selector() = default;
};

struct Receiver {
  mut int value = 0;
  int inspect() { return this.value; }
  int inspect() mut {
    this.value += 1;
    return this.value;
  }
public:
  Receiver() = default;
};

int main() {
  uint64_t base = uint64_t(2);
  uint64_t exponent = uint64_t(8);
  uint64_t whole = math::pow(base, exponent);
  float decimal = math::pow(2.0, 8.0);
  Selector<int, float> selector = Selector<int, float>();
  int selected = selector.apply(2);
  float selected_decimal = selector.apply(2.0);
  Receiver read_only = Receiver();
  mut Receiver writable = Receiver();
  int read_result = read_only.inspect();
  int write_result = writable.inspect();
  return int(whole) + selected + read_result + write_result;
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
      frontend.semantics.functionCount() == 7 &&
          frontend.semantics.resolvedCallCount() == 6,
      "semantic analysis should retain function identities and selected calls");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto selectedReceiver = [&](std::size_t statementIndex) {
    if (main == nullptr ||
        statementIndex >= main->body()->statements().size()) {
      return static_cast<const lang::ResolvedCallInfo *>(nullptr);
    }
    const auto *variable = dynamic_cast<const lang::VariableDecl *>(
        main->body()->statements()[statementIndex].get());
    const auto *call =
        variable == nullptr
            ? nullptr
            : dynamic_cast<const lang::Call *>(variable->initializer().get());
    return call == nullptr ? nullptr : frontend.semantics.findCall(*call);
  };
  const lang::ResolvedCallInfo *readCall = selectedReceiver(9);
  const lang::ResolvedCallInfo *writeCall = selectedReceiver(10);
  expect(readCall != nullptr && readCall->declaration != nullptr &&
             readCall->declaration->receiverMutability() ==
                 lang::ReceiverMutability::ReadOnly &&
             writeCall != nullptr && writeCall->declaration != nullptr &&
             writeCall->declaration->receiverMutability() ==
                 lang::ReceiverMutability::Mutable,
         "method overload selection should choose the receiver-qualified "
         "declaration exactly");

  using lang::SemanticType;
  const auto pows = mirFunctionsNamed(frontend, "pow");
  const auto powWith =
      [&](SemanticType type) -> const lang::MirFunctionInstance * {
    for (const lang::MirFunctionInstance *function : pows)
      if (function->parameterTypes == std::vector<SemanticType>{type, type})
        return function;
    return nullptr;
  };
  const lang::MirFunctionInstance *integerPow = powWith(SemanticType::UInt64);
  const lang::MirFunctionInstance *floatPow = powWith(SemanticType::Float);
  const lang::MirFunctionInstance *overloadMain =
      mirFunctionNamed(frontend, "main");
  const auto selectors = mirClassesNamed(frontend, "Selector");
  std::size_t selectorApplies = 0;
  bool calledEveryApply =
      selectors.size() == 1 && hasSourceMirBody(overloadMain);
  for (const lang::MirFunctionInstance *apply :
       mirFunctionsNamed(frontend, "apply")) {
    if (selectors.size() != 1 || apply->owner != selectors.front()->id)
      continue;
    ++selectorApplies;
    calledEveryApply =
        calledEveryApply &&
        callsFunction(frontend, overloadMain->body,
                      [&](const lang::MirFunctionInstance &target) {
                        return &target == apply;
                      });
  }
  expect(hasSourceMirBody(integerPow) && hasSourceMirBody(floatPow) &&
             integerPow != floatPow && integerPow->mayRaiseDefinedFailure &&
             !floatPow->mayRaiseDefinedFailure &&
             callsFunction(frontend, overloadMain->body,
                           [&](const lang::MirFunctionInstance &target) {
                             return &target == integerPow;
                           }) &&
             callsFunction(frontend, overloadMain->body,
                           [&](const lang::MirFunctionInstance &target) {
                             return &target == floatPow;
                           }) &&
             selectorApplies == 2 && calledEveryApply,
         "MIR should call the exact function identity selected by GTI, with "
         "checked integer and failure-free float overloads");
  const auto converts = [&](SemanticType type) {
    return hasSourceMirBody(overloadMain) &&
           anyMirInstruction(overloadMain->body,
                             [&](const lang::MirInstruction &instruction) {
                               return instruction.operation ==
                                          lang::MirOperation::Convert &&
                                      instruction.info.type == type;
                             });
  };
  expect(converts(SemanticType::UInt64) && converts(SemanticType::Int32) &&
             anyMirInstruction(overloadMain->body,
                               [](const lang::MirInstruction &instruction) {
                                 return instruction.operation ==
                                            lang::MirOperation::Literal &&
                                        instruction.info.type ==
                                            SemanticType::Float;
                               }) &&
             shippedBackendAccepts(frontend),
         "explicit conversions and float literals should lower to matching "
         "typed MIR operations");

  lang::Lexer lexer;
  auto invalidTokens = lexer.scan(R"(
int same(int value) { return value; }
float same(int value) { return float(value); }

int mutate(int value) { return value; }
int mutate(mut int value) { return value; }

T echo<T>(T value) { return value; }
U echo<U>(U value) { return value; }

struct MutableOnly {
  int inspect() mut { return 1; }
public:
  MutableOnly() = default;
};

int choose(int value) { return value; }
T choose<T>(T value) { return value; }

uint64_t width(uint64_t value) { return value; }
float width(float value) { return value; }
float only_float(float value) { return value; }
void function_value() {}

int main(int value) { return value; }
int main() {
  MutableOnly read_only = MutableOnly();
  int invalid_receiver = read_only.inspect();
  function_value;
  width;
  float mismatch = width(true);
  int ambiguous = choose(1);
  float inexact = only_float(1);
  uint8_t too_small = uint8_t(300);
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
  expect(countDiagnosticCode(invalidSemantic, "GTI-S2011") == 4,
         "return type, by-value mutability, and generic spelling should not "
         "create distinct overload signatures");
  expect(
      hasDiagnostic(invalidSemantic, "main entry point cannot be overloaded"),
      "the native entry point should remain a unique function");
  expect(hasDiagnostic(invalidSemantic, "No overload of 'width'") &&
             hasDiagnostic(invalidSemantic, "argument types (bool)"),
         "overload lookup should reject calls without an exact candidate");
  expect(hasDiagnostic(invalidSemantic, "ambiguous") &&
             hasDiagnostic(invalidSemantic, "exactly match"),
         "generic and concrete exact matches should be diagnosed as ambiguous");
  expect(hasDiagnostic(invalidSemantic, "parameter requires 'float'"),
         "single functions should also require exact argument types");
  expect(hasDiagnostic(invalidSemantic,
                       "Mutable method requires a mutable receiver"),
         "receiver-qualified overloads should reject mutable-only methods on "
         "read-only objects");
  expect(
      countDiagnosticCode(invalidSemantic, "GTI-S2090") == 1 &&
          hasDiagnostic(
              invalidSemantic,
              "function item is ambiguous without an exact callable context"),
      "function overload sets should not escape unresolved into the backend");
  expect(hasDiagnostic(invalidSemantic, "outside the range of 'uint8_t'") &&
             hasDiagnostic(invalidSemantic,
                           "numeric conversions require a numeric value"),
         "explicit conversions should reject invalid literals and domains");

  const std::string formatted = lang::Formatter().format(
      "uint64_t pow(uint64_t base,uint64_t exponent){return base*exponent;}int "
      "main(){uint64_t value=uint64_t(2);return int(value);}");
  expect(formatted.find("uint64_t value = uint64_t(2);") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "explicit conversion syntax should format like a C++ functional cast");
}

void testFreshArrayOwnership() {
  const std::string prefix = R"(
class Pinned {
public:
  Pinned() {}
  Pinned(int value) {}
  Pinned(Pinned&) = delete;
  Pinned(Pinned&&) = delete;
};
)";
  auto valid = lang::Frontend().analyze("fresh-arrays.gti", prefix + R"(
int main() {
  Pinned values[2] = {Pinned(1), Pinned(2)};
  Pinned nested[2][1] = {{Pinned(3)}, {Pinned(4)}};
  Pinned empty[2] = {};
  return 0;
}
)");
  expect(valid.canGenerateCode(),
         "fresh array initialization does not require element movement");
  const auto *main = findTopLevelFunction(valid.program, "main");
  const auto *variable = findDirectLocal(main, "values");
  const auto *binding =
      variable ? valid.semantics.findBinding(*variable) : nullptr;
  expect(binding && !binding->traits.movable && !binding->traits.copyable,
         "fresh publication does not manufacture copy or move capabilities");
  const auto invalid =
      lang::Frontend().analyze("copy-array-element.gti", prefix + R"(
int main() { Pinned original{}; Pinned values[1] = {original}; return 0; }
)");
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot copy array element of non-copyable type"),
         "each array element still requires its exact ownership operation");
}

void testFixedArrays() {
  const std::string source = R"(
uint32_t video[64 * 32] = {};

int extent(int values[2]) { return 2; }
int extent(int values[3]) { return 3; }
int first(int values[3]) { return values[0]; }

struct Buffers {
public:
  mut int samples[3] = {1, 2, 3};

  void bump() mut { this.samples[1] += 4; }
  uint64_t count() { return this.samples.size(); }
public:
  Buffers() = default;
};

int main() {
  mut int buffer[2 + 3] = {1, 2, 3, 4, 5};
  buffer[2] = 10;
  buffer[1]++;
  int copy[5] = buffer;
  int matrix[0x2][0b11] = {{1, 2, 3}, {4, 5, 6}};
  mut Buffers buffers = Buffers();
  buffers.bump();
  uint64_t count = buffer.size();
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

  const lang::MirFunctionInstance *bumpMir = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "bump") {
      bumpMir = frontend.mir.findFunctionInstance(instance.id);
      break;
    }
  }
  const lang::MirInstruction *boundsRead = nullptr;
  const lang::MirInstruction *checkedAdd = nullptr;
  const lang::MirInstruction *arrayCommit = nullptr;
  if (bumpMir != nullptr) {
    for (const lang::MirBlock &block : bumpMir->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Load &&
            !instruction.localFailureSites.empty()) {
          boundsRead = &instruction;
        } else if (instruction.kind == lang::MirInstructionKind::Compute &&
                   instruction.operation == lang::MirOperation::Add &&
                   !instruction.localFailureSites.empty()) {
          checkedAdd = &instruction;
        } else if (instruction.kind == lang::MirInstructionKind::Assign &&
                   instruction.operation == lang::MirOperation::Assign &&
                   instruction.localFailureSites.empty()) {
          arrayCommit = &instruction;
        }
      }
    }
  }
  const auto hasFailureOutcome = [&](const lang::MirInstruction *instruction,
                                     lang::DefinedFailureCode code) {
    return instruction != nullptr &&
           std::any_of(
               instruction->localFailureSites.begin(),
               instruction->localFailureSites.end(),
               [&](lang::FailureSiteId id) {
                 const lang::FailureSiteDescriptor *site =
                     frontend.mir.failureMetadata().findSite(id);
                 return site != nullptr &&
                        std::any_of(
                            site->outcomes.begin(), site->outcomes.end(),
                            [&](const lang::DefinedFailureOutcome &outcome) {
                              return outcome.code == code;
                            });
               });
  };
  expect(boundsRead != nullptr && boundsRead->result &&
             hasFailureOutcome(boundsRead,
                               lang::DefinedFailureCode::IndexOutOfBounds) &&
             checkedAdd != nullptr && checkedAdd->result &&
             hasFailureOutcome(checkedAdd,
                               lang::DefinedFailureCode::IntegerOverflow) &&
             checkedAdd->operands.size() == 2 &&
             checkedAdd->operands.front().value == *boundsRead->result &&
             arrayCommit != nullptr && arrayCommit->operands.size() == 1 &&
             arrayCommit->operands.front().value == *checkedAdd->result,
         "MIR should order fixed-array compound updates as checked read, "
         "checked arithmetic, and failure-free commit");

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
      lang::SemanticType::uniqueOwnerOf(lang::SemanticType::Int32), 2);
  expect(!lang::semanticTraits(moveOnlyArray).copyable &&
             lang::semanticTraits(moveOnlyArray).movable,
         "fixed arrays should inherit element copy and move traits");

  const lang::SemanticType int5 =
      lang::SemanticType::arrayOf(lang::SemanticType::Int32, 5);
  const lang::SemanticType matrix = lang::SemanticType::arrayOf(
      lang::SemanticType::arrayOf(lang::SemanticType::Int32, 3), 2);
  const lang::SemanticType video =
      lang::SemanticType::arrayOf(lang::SemanticType::UInt32, 2048);
  const lang::LoweredProgram arrayProgram =
      gti_test::lowerOptimizedProgram(frontend);
  const bool videoStorage = std::any_of(
      arrayProgram.declarations().begin(), arrayProgram.declarations().end(),
      [&](const lang::LoweredDeclaration &declaration) {
        const auto *storage =
            std::get_if<lang::LoweredStorageDeclaration>(&declaration.payload);
        return declaration.name == "video" && storage != nullptr &&
               storage->type == video && storage->hasInitializer;
      });
  const lang::MirFunctionInstance *arrayMain =
      mirFunctionNamed(frontend, "main");
  const auto hasPlaceOfType = [&](const lang::SemanticType &type) {
    return hasSourceMirBody(arrayMain) &&
           std::any_of(arrayMain->body.places.begin(),
                       arrayMain->body.places.end(),
                       [&](const lang::MirPlace &place) {
                         return place.type == type && place.projections.empty();
                       });
  };
  const auto indexedPlace = [&](const lang::MirPlace &place) {
    return !place.projections.empty() &&
           place.projections.back().kind == lang::MirProjectionKind::Index;
  };
  const bool checkedIndexedWrite =
      hasSourceMirBody(arrayMain) &&
      anyMirInstruction(
          arrayMain->body, [&](const lang::MirInstruction &instruction) {
            const lang::MirPlace *destination =
                instruction.destination
                    ? arrayMain->body.findPlace(*instruction.destination)
                    : nullptr;
            return instruction.kind == lang::MirInstructionKind::Assign &&
                   destination != nullptr && indexedPlace(*destination);
          });
  const bool checkedIndexedAdd =
      hasSourceMirBody(arrayMain) &&
      anyMirInstruction(
          arrayMain->body, [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Compute &&
                   instruction.operation == lang::MirOperation::Add &&
                   instruction.info.type == lang::SemanticType::Int32 &&
                   !instruction.definedFailure.localOrigins.empty();
          });
  expect(videoStorage && hasPlaceOfType(int5) && hasPlaceOfType(matrix) &&
             loadsFromPlace(arrayMain->body, indexedPlace) &&
             checkedIndexedWrite && checkedIndexedAdd &&
             shippedBackendAccepts(frontend),
         "MIR should preserve array values, storage extents, and checked "
         "indexed reads, writes, and arithmetic for the backend");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-arrays.gti", R"(
struct NoDefault {
  int stored;
  NoDefault(int value) : stored(value) {}
};

class ReadOnlyArrayReceiver {
  mut int slots[1] = {0};

public:
  void write() { slots[0] = 1; }
public:
  ReadOnlyArrayReceiver() = default;
};

int overflow_extent[18446744073709551615 + 1] = {};
int negative_extent[1 - 2] = {};
int zero_divisor_extent[4 / 0] = {};

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
  uint64_t hidden = other.length();
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid fixed array operations should fail semantic analysis");
  expect(
      hasDiagnostic(invalid.diagnostics, "requires exactly 3") &&
          hasDiagnostic(invalid.diagnostics, "immutable fixed array") &&
          hasDiagnostic(invalid.diagnostics,
                        "Cannot modify field 'slots' because method "
                        "'write' has no trailing 'mut' receiver qualifier") &&
          hasDiagnosticHint(invalid.diagnostics, "trailing 'mut'") &&
          hasRelatedDiagnostic(invalid.diagnostics,
                               "is declared mutable, but it is writable") &&
          hasDiagnostic(invalid.diagnostics, "index must have an integer") &&
          hasDiagnostic(invalid.diagnostics, "valid range [0, 2)") &&
          hasDiagnostic(invalid.diagnostics, "require an initializer") &&
          hasDiagnostic(invalid.diagnostics, "default-initializable") &&
          hasDiagnostic(invalid.diagnostics, "No overload of 'choose'") &&
          hasDiagnostic(invalid.diagnostics, "Unknown fixed array member") &&
          hasDiagnostic(invalid.diagnostics,
                        "extent arithmetic overflows uint64_t") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot produce a negative value") &&
          hasDiagnostic(invalid.diagnostics,
                        "cannot divide or take modulo by zero"),
      "fixed array diagnostics should explain extent, access, and bounds");

  lang::Lexer lexer;
  lang::Parser malformed(lexer.scan("int broken[] = {}; int recovered = 1;"));
  const lang::Program recovered = malformed.parse();
  expect(malformed.hadError() && recovered.declarations().size() == 1,
         "parser recovery should continue after a missing array extent");

  const std::string formatted = lang::Formatter().format(
      "mut int buffer[1+2]={1,2,3};int matrix[0x2][0b10]={{1,2},{3,4}};");
  expect(formatted == "mut int buffer[1 + 2] = {1, 2, 3};\n"
                      "int matrix[0x2][0b10] = {{1, 2}, {3, 4}};\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatter should preserve compact C++-style array declarations");
  const std::string constructorFormatted = lang::Formatter().format(
      "class Pair<T>{T values[2];public:Pair(T left,T right):"
      "values({left,right}){}};");
  expect(constructorFormatted.find("values({left, right}) {}") !=
             std::string::npos,
         "formatter should keep array constructor initializers compact");
}

// A dynamic fixed-array element write keeps its index_out_of_bounds failure
// site, and MIR must pair that site with an Invoke terminator so the checked
// write helper's status feeds the failure successor exactly like the read
// form. Losing the edge silently no-ops the out-of-bounds write.
void testFixedArrayDynamicWriteFailureEdge() {
  const std::string source = R"(
int main() {
  mut int32_t cells[4] = {};
  mut uint64_t index = uint64_t(0);
  while (index < uint64_t(9)) {
    index++;
  }
  cells[index] = 7;
  return int(cells[uint64_t(0)]);
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("dynamic-array-write.gti", source);
  expect(frontend.canGenerateCode(),
         "the dynamic array-write fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *mainMir = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainMir = frontend.mir.findFunctionInstance(instance.id);
      break;
    }
  }
  const lang::MirBlock *writeBlock = nullptr;
  const lang::MirInstruction *write = nullptr;
  if (mainMir != nullptr) {
    for (const lang::MirBlock &block : mainMir->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.operation == lang::MirOperation::Assign &&
            instruction.destination && !instruction.localFailureSites.empty()) {
          writeBlock = &block;
          write = &instruction;
        }
      }
    }
  }
  const bool boundsOutcome =
      write != nullptr &&
      std::any_of(
          write->localFailureSites.begin(), write->localFailureSites.end(),
          [&](lang::FailureSiteId id) {
            const lang::FailureSiteDescriptor *site =
                frontend.mir.failureMetadata().findSite(id);
            return site != nullptr &&
                   std::any_of(
                       site->outcomes.begin(), site->outcomes.end(),
                       [&](const lang::DefinedFailureOutcome &outcome) {
                         return outcome.code ==
                                lang::DefinedFailureCode::IndexOutOfBounds;
                       });
          });
  expect(write != nullptr && boundsOutcome &&
             lang::requiresMirFailureControlFlow(*write),
         "a dynamic fixed-array element write should stay eligible for MIR "
         "failure control flow");
  expect(writeBlock != nullptr && write != nullptr &&
             !writeBlock->instructions.empty() &&
             writeBlock->instructions.back().id == write->id &&
             writeBlock->terminator.kind == lang::MirTerminatorKind::Invoke &&
             writeBlock->terminator.invokeInstruction == write->id &&
             writeBlock->terminator.elseTarget != 0 &&
             mainMir->body.blocks[writeBlock->terminator.elseTarget - 1]
                     .failureParameter == writeBlock->terminator.failureRecord,
         "the dynamic array write must terminate its block with an Invoke "
         "whose failure successor consumes the checked write status");

  expect(shippedBackendAccepts(frontend),
         "the backend should accept the checked array write together with "
         "its MIR failure successor");
}

// A chained fixed-array element access with a dynamic nonterminal index is one
// checked MIR access with a bounds site over the complete projected place, and
// the backend must accept it rather than decline the body (GTI-B0001). The
// C++ emitter's per-dimension guard text is checked in
// cpp_mir_declarations_emission_tests.cpp.
void testFixedArrayChainedDynamicIndex() {
  const std::string source = R"(
int main() {
  mut int32_t matrix[2][3] = {};
  mut uint64_t row = uint64_t(0);
  while (row < uint64_t(9)) {
    row = row + uint64_t(1);
  }
  uint64_t column = uint64_t(1);
  matrix[row][column] = 7;
  return int(matrix[row][column]);
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("chained-array-index.gti", source);
  expect(frontend.canGenerateCode(),
         "the chained array-index fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *chainedMain =
      mirFunctionNamed(frontend, "main");
  const auto boundsChecked = [&](const lang::MirInstruction &instruction) {
    return std::any_of(
        instruction.localFailureSites.begin(),
        instruction.localFailureSites.end(), [&](lang::FailureSiteId id) {
          const lang::FailureSiteDescriptor *site =
              frontend.mir.failureMetadata().findSite(id);
          return site != nullptr &&
                 std::any_of(
                     site->outcomes.begin(), site->outcomes.end(),
                     [](const lang::DefinedFailureOutcome &outcome) {
                       return outcome.code ==
                              lang::DefinedFailureCode::IndexOutOfBounds;
                     });
        });
  };
  const auto chainedPlace = [](const lang::MirPlace &place) {
    return place.projections.size() == 2 &&
           place.projections[0].kind == lang::MirProjectionKind::Index &&
           place.projections[1].kind == lang::MirProjectionKind::Index;
  };
  const auto checkedChainedAccess = [&](lang::MirInstructionKind kind) {
    return hasSourceMirBody(chainedMain) &&
           anyMirInstruction(
               chainedMain->body, [&](const lang::MirInstruction &instruction) {
                 if (instruction.kind != kind || !boundsChecked(instruction))
                   return false;
                 if (instruction.destination) {
                   const lang::MirPlace *destination =
                       chainedMain->body.findPlace(*instruction.destination);
                   if (destination != nullptr && chainedPlace(*destination))
                     return true;
                 }
                 return std::any_of(
                     instruction.operands.begin(), instruction.operands.end(),
                     [&](const lang::MirOperand &operand) {
                       const lang::MirPlace *place =
                           chainedMain->body.findPlace(operand.place);
                       return place != nullptr && chainedPlace(*place);
                     });
               });
  };
  expect(checkedChainedAccess(lang::MirInstructionKind::Load) &&
             checkedChainedAccess(lang::MirInstructionKind::Assign),
         "a chained dynamic element read and write should each be one "
         "bounds-checked MIR access over the complete projected place");
  expect(shippedBackendAccepts(frontend),
         "a chained dynamic element access should be accepted by the backend "
         "rather than declined");
}

void testStructuredBindings() {
  const std::string source = R"(
struct Pair<T, U> {
  T first;
  U second;

  Pair(T first, U second) : first(first), second(second) {}
  ~Pair() {}
};

int main() {
  int values[2] = {2, 3};
  auto [left, right] = values;
  auto [number, enabled] = Pair<int, bool>(4, true);
  mut int result = 1;
  if (left == 2 && right == 3 && number == 4 && enabled) {
    result = 0;
  }
  return result;
}
)";

  lang::FrontendResult frontend =
      lang::Frontend().analyze("structured-bindings.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected structured-binding diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "fixed arrays and public direct aggregate fields should support "
         "structured bindings");

  const lang::FunctionDecl *main =
      findTopLevelFunction(frontend.program, "main");
  const auto *arrayBinding =
      main == nullptr ? nullptr
                      : dynamic_cast<const lang::StructuredBindingDecl *>(
                            main->body()->statements().at(1).get());
  const auto *pairBinding =
      main == nullptr ? nullptr
                      : dynamic_cast<const lang::StructuredBindingDecl *>(
                            main->body()->statements().at(2).get());
  expect(arrayBinding != nullptr && pairBinding != nullptr &&
             arrayBinding->bindings().size() == 2 &&
             pairBinding->bindings().size() == 2,
         "the AST should retain ordered structured-binding declarations");

  const lang::StructuredBindingInfo *arrayInfo =
      arrayBinding == nullptr
          ? nullptr
          : frontend.semantics.findStructuredBinding(*arrayBinding);
  const lang::StructuredBindingInfo *pairInfo =
      pairBinding == nullptr
          ? nullptr
          : frontend.semantics.findStructuredBinding(*pairBinding);
  expect(arrayInfo != nullptr && arrayInfo->elements.size() == 2 &&
             arrayInfo->elements[0].projection ==
                 lang::StructuredBindingProjectionKind::ArrayElement &&
             arrayInfo->elements[0].index == 0 &&
             arrayInfo->elements[1].index == 1 &&
             arrayInfo->elements[0].binding.type == lang::SemanticType::Int32 &&
             arrayInfo->elements[0].binding.access ==
                 lang::AccessMode::ReadOnly,
         "array decomposition should record exact immutable element "
         "projections");
  expect(pairInfo != nullptr &&
             pairInfo->source.type.kind == lang::SemanticType::Class &&
             pairInfo->elements.size() == 2 &&
             pairInfo->elements[0].projection ==
                 lang::StructuredBindingProjectionKind::Field &&
             pairInfo->elements[0].field != 0 &&
             pairInfo->elements[1].field != 0 &&
             pairInfo->elements[0].binding.type == lang::SemanticType::Int32 &&
             pairInfo->elements[1].binding.type == lang::SemanticType::Bool &&
             pairInfo->elements[0].binding.symbol != 0 &&
             pairInfo->elements[1].binding.symbol != 0,
         "aggregate decomposition should substitute field types and retain "
         "source-visible symbols");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const std::size_t leftDeclaration = source.find("left");
  const std::size_t leftUse = source.rfind("left");
  const std::optional<lang::HoverInfo> leftHover =
      queries.hover(frontend, unit, leftDeclaration + 1);
  const std::optional<lang::DefinitionInfo> leftDefinition =
      queries.definition(frontend, unit, leftUse + 1);
  expect(leftHover && leftHover->signature == "int32_t left" &&
             leftDefinition && leftDefinition->target.start == leftDeclaration,
         "shared semantic queries should expose hover and definition for "
         "each decomposed name");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == main) {
      mainInstance = &instance;
      break;
    }
  }
  const lang::HirStatement *arrayStatement =
      mainInstance == nullptr || mainInstance->body.roots.size() < 3
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[1]);
  const lang::HirStatement *pairStatement =
      mainInstance == nullptr || mainInstance->body.roots.size() < 3
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[2]);
  expect(arrayStatement != nullptr && pairStatement != nullptr &&
             arrayStatement->kind ==
                 lang::HirStatementKind::StructuredBinding &&
             pairStatement->kind == lang::HirStatementKind::StructuredBinding &&
             arrayStatement->binding && pairStatement->binding &&
             arrayStatement->structuredBindings.size() == 2 &&
             pairStatement->structuredBindings.size() == 2 &&
             arrayStatement->structuredBindings[0].index &&
             pairStatement->structuredBindings[0].field != 0,
         "HIR should retain one hidden owner and typed array/field "
         "projections");

  const lang::MirFunctionInstance *mirMain =
      mainInstance == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(mainInstance->id);
  const auto findSourcePlace =
      [&](const lang::HirStatement *statement) -> const lang::MirPlace * {
    if (mirMain == nullptr || statement == nullptr || !statement->binding) {
      return nullptr;
    }
    const auto found =
        std::find_if(mirMain->body.places.begin(), mirMain->body.places.end(),
                     [&](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::Binding &&
                              place.binding == *statement->binding &&
                              place.projections.empty();
                     });
    return found == mirMain->body.places.end() ? nullptr : &*found;
  };
  const lang::MirPlace *arraySource = findSourcePlace(arrayStatement);
  const lang::MirPlace *pairSource = findSourcePlace(pairStatement);
  std::size_t arrayInitializations = 0;
  std::size_t pairInitializations = 0;
  std::size_t pairDrops = 0;
  if (mirMain != nullptr) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        arrayInitializations +=
            arrayStatement != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.hirStatement == arrayStatement->id
                ? 1
                : 0;
        pairInitializations +=
            pairStatement != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.hirStatement == pairStatement->id
                ? 1
                : 0;
        pairDrops +=
            pairSource != nullptr &&
                    instruction.kind == lang::MirInstructionKind::Drop &&
                    instruction.destination == pairSource->id
                ? 1
                : 0;
      }
    }
  }
  const bool hasArrayProjection =
      mirMain != nullptr && arrayStatement != nullptr &&
      std::any_of(mirMain->body.places.begin(), mirMain->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == *arrayStatement->binding &&
                           std::any_of(
                               place.projections.begin(),
                               place.projections.end(),
                               [](const lang::MirPlaceProjection &projection) {
                                 return projection.kind ==
                                        lang::MirProjectionKind::Index;
                               });
                  });
  const bool hasFieldProjection =
      mirMain != nullptr && pairStatement != nullptr &&
      std::any_of(mirMain->body.places.begin(), mirMain->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == *pairStatement->binding &&
                           std::any_of(
                               place.projections.begin(),
                               place.projections.end(),
                               [](const lang::MirPlaceProjection &projection) {
                                 return projection.kind ==
                                        lang::MirProjectionKind::Field;
                               });
                  });
  expect(arraySource != nullptr && pairSource != nullptr &&
             arrayInitializations == 1 && pairInitializations == 1 &&
             pairDrops == 1 && hasArrayProjection && hasFieldProjection,
         "MIR should initialize and destroy each hidden owner once while "
         "visible names remain projected places");

  std::unordered_set<lang::SymbolId> loadedPairFields;
  if (mirMain != nullptr && pairStatement != nullptr) {
    loadsFromPlace(mirMain->body, [&](const lang::MirPlace &place) {
      if (place.root == lang::MirPlaceRootKind::Binding &&
          place.binding == *pairStatement->binding &&
          place.projections.size() == 1 &&
          place.projections.front().kind == lang::MirProjectionKind::Field)
        loadedPairFields.insert(place.projections.front().field);
      return false;
    });
  }
  expect(loadedPairFields.size() == 2 && mirMain != nullptr &&
             loadsFromPlace(mirMain->body,
                            [&](const lang::MirPlace &place) {
                              return arrayStatement != nullptr &&
                                     place.root ==
                                         lang::MirPlaceRootKind::Binding &&
                                     place.binding ==
                                         *arrayStatement->binding &&
                                     !place.projections.empty() &&
                                     place.projections.front().kind ==
                                         lang::MirProjectionKind::Index;
                            }) &&
             shippedBackendAccepts(frontend),
         "the backend should read validated structured bindings through MIR "
         "projections of their hidden owners, not a decomposition "
         "declaration");

  const std::string formatted = lang::Formatter().format(
      "int main(){int values[2]={1,2};auto[left,right]=values;return left;}");
  expect(formatted.find("auto [left, right] = values;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "structured bindings should receive stable C++-style formatting");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-structured-bindings.gti", R"(
class Hidden {
  int secret = 1;
public:
  Hidden() = default;
};

struct Base {
  int base = 1;
public:
  Base() = default;
};

struct Derived : public Base {
  int own = 2;
public:
  Derived() = default;
};

struct MoveOnly {
  int value = 1;

  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
public:
  MoveOnly() = default;
};

int main() {
  int values[2] = {1, 2};
  auto [too, many, names] = values;
  auto [scalar] = 1;
  Hidden hidden{};
  auto [secret] = hidden;
  Derived derived{};
  auto [base, own] = derived;
  auto [same, same] = values;
  auto [first, second] = values;
  first = 3;
  auto moved = std::move(second);
  MoveOnly owner{};
  auto [copied] = owner;
  auto [untyped_left, untyped_right] = {1, 2};
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2048") &&
             hasDiagnostic(invalid.diagnostics,
                           "declares 3 names but the source provides 2") &&
             hasDiagnostic(invalid.diagnostics,
                           "require a fixed array or a class/struct") &&
             hasDiagnostic(invalid.diagnostics,
                           "every direct instance field to be public") &&
             hasDiagnostic(invalid.diagnostics,
                           "do not decompose inherited storage") &&
             hasDiagnostic(invalid.diagnostics, "Duplicate declaration") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot assign to immutable binding 'first'") &&
             hasDiagnostic(invalid.diagnostics,
                           "cannot partially move structured binding") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot initialize structured binding by copying") &&
             hasDiagnostic(invalid.diagnostics,
                           "requires a fixed array type from its context"),
         "structured-binding diagnostics should reject ambiguous ownership, "
         "shape, access, mutation, and untyped initialization");

  const auto rangePattern =
      lang::Frontend(
          {.sourceDebugProfile = lang::SourceDebugProfile::LexicalV1})
          .analyze("range-pattern.gti", R"(
struct Entry<T> { mut T key; mut T value; Entry(T k, T v) : key(k), value(v) {} };
int main() {
  mut Entry<int> entries[2] = {Entry<int>(1, 2), Entry<int>(3, 4)};
  mut int sum = 0;
  for (auto [key, value] : entries) { sum += key + value; }
  for (auto& [key, value] : entries) { sum += key + value; }
  for (mut auto& [key, value] : entries) { key += 1; value += key; }
  return sum - 20;
}
)");
  for (const auto &diagnostic : rangePattern.diagnostics)
    std::cerr << "range-pattern: " << diagnostic.code << " "
              << diagnostic.message << '\n';
  expect(rangePattern.canGenerateCode(),
         "owning and borrowed range patterns should reach verified MIR");

  std::size_t projectedDebugLocals = 0;
  for (const auto &body : rangePattern.semantics.sourceDebugBodies()) {
    if (body.facts.tables) {
      for (const auto &local : body.facts.tables->locals)
        projectedDebugLocals += local.name == "key" || local.name == "value";
    }
  }
  expect(projectedDebugLocals == 6 &&
             rangePattern.semantics.verifySourceDebug().empty(),
         "range pattern names retain ordinary lexical debug-local records");
  const auto retainedProjections = std::count_if(
      rangePattern.hir.sourceStatementOwners().begin(),
      rangePattern.hir.sourceStatementOwners().end(), [](const auto &owner) {
        const auto *declaration = lang::stmtAs<lang::VariableDecl>(owner.get());
        return declaration != nullptr &&
               (declaration->name().lexeme == "key" ||
                declaration->name().lexeme == "value");
      });
  expect(retainedProjections == 6,
         "HIR owns projection syntax after concrete semantic models expire");

  const std::string rangePrelude = R"(
struct Item { mut int first; int second; Item(int a, int b) : first(a), second(b) {} };
)";
  const auto rejectRange = [&](std::string body, std::string_view reason) {
    auto checked = lang::Frontend().analyze(
        "invalid-range-pattern.gti",
        rangePrelude + "int main() { mut Item items[1] = {Item(1, 2)}; " +
            body + " return 0; }");
    expect(!checked.canGenerateCode(), std::string(reason));
    expect(std::none_of(checked.diagnostics.begin(), checked.diagnostics.end(),
                        [](const auto &d) { return d.code == "GTI-B0001"; }),
           "invalid range patterns must not cause internal diagnostics");
  };
  rejectRange("for (auto& [only] : items) {}",
              "range patterns require exact arity");
  rejectRange("for (auto& [a, a] : items) {}",
              "range patterns require distinct names");
  rejectRange("for (auto& [a, b] : items) { a = 7; }",
              "range projections remain read-only");
  rejectRange(
      "for (auto& [a, b] : items) { items[0].first = 7; int copy = a; }",
      "owner mutation cannot invalidate a projected borrow");
  rejectRange("for (mut auto& [a, b] : items) {}",
              "mutable patterns cannot upgrade immutable fields");
  rejectRange("for (mut auto [a, b] : items) {}",
              "mutable range patterns require a reference");
  rejectRange(
      "Item frozen[1] = {Item(1, 2)}; for (mut auto& [a, b] : frozen) {}",
      "mutable range patterns require mutable storage");
  rejectRange("for (mut auto& [a, b] : items) { items[0].first = 7; a += b; }",
              "mutable patterns retain exclusive access to the owner");
  rejectRange("for (auto& [a, b] : items) {} return a;",
              "pattern names end with the iteration");
  auto escapedRange =
      lang::Frontend().analyze("escaping-range-pattern.gti", rangePrelude + R"(
int& escape(Item& fallback) {
  Item items[1] = {Item(1, 2)};
  for (auto& [a, b] : items) { return a; }
  return fallback.first;
}
)");
  expect(!escapedRange.canGenerateCode(),
         "projected references cannot escape local range storage");
  rejectRange("int values[1] = {1}; for (auto [a] : values) {}",
              "scalar range elements cannot be decomposed");
  const auto ownedEscape =
      lang::Frontend().analyze("escaping-owned-pattern.gti", rangePrelude + R"(
int& escape(Item& fallback) {
  Item items[1] = {Item(1, 2)};
  for (auto [a, b] : items) { return a; }
  return fallback.first;
}
)");
  expect(!ownedEscape.canGenerateCode(),
         "an owned iteration projection cannot escape its element");

  const std::string rangeFormatted = lang::Formatter().format(
      "int "
      "main(){for(auto[a,b]:items){use(a,b);}for(auto&"
      "[a,b]:items){use(a,b);}for(mut auto&[a,b]:items){a+=b;}}");
  expect(rangeFormatted.find("auto [a, b] : items") != std::string::npos &&
             rangeFormatted.find("auto& [a, b] : items") != std::string::npos &&
             rangeFormatted.find("mut auto& [a, b] : items") !=
                 std::string::npos &&
             lang::Formatter().format(rangeFormatted) == rangeFormatted,
         "range patterns format consistently and idempotently");
  lang::FormatOptions rightAligned;
  rightAligned.referenceAlignment = lang::ReferenceAlignment::Right;
  const std::string rightPattern =
      lang::Formatter(rightAligned)
          .format("int main(){for(auto&[a,b]:items){use(a,b);}}");
  expect(rightPattern.find("auto &[a, b] : items") != std::string::npos &&
             lang::Formatter(rightAligned).format(rightPattern) == rightPattern,
         "range patterns honor configured reference alignment");

  lang::Lexer lexer;
  const auto expectSyntaxError = [&](std::string input,
                                     std::string_view message) {
    lang::Parser parser(lexer.scan(input));
    (void)parser.parse();
    expect(parser.hadError() &&
               hasDiagnostic(parser.errors(), std::string(message)),
           "structured-binding syntax should diagnose " + std::string(message));
  };
  expectSyntaxError("auto [left, right] = value;", "local declarations");
  expectSyntaxError("class Bad { auto [left, right] = value; };",
                    "cannot declare class or struct fields");
  expectSyntaxError("int main() { mut auto [left, right] = values; return 0; }",
                    "Structured bindings are immutable");
  expectSyntaxError("int main() { auto& [left, right] = values; return 0; }",
                    "Reference structured bindings are not supported");
  expectSyntaxError(
      "int main() { for (auto [left, right] = values; true; left++) {} "
      "return 0; }",
      "Structured for bindings require a range");
  lang::Parser autoArrayParser(
      lexer.scan("int main() { auto[2] values = {1, 2}; return 0; }"));
  (void)autoArrayParser.parse();
  expect(!autoArrayParser.hadError(),
         "structured-binding lookahead should preserve typed array syntax for "
         "semantic auto diagnostics");
  lang::Parser recoveryParser(lexer.scan(
      "int main() { int broken = 1 auto [left, right] = values; return left; "
      "}"));
  const lang::Program recovered = recoveryParser.parse();
  const lang::FunctionDecl *recoveredMain =
      findTopLevelFunction(recovered, "main");
  expect(recoveryParser.hadError() && recoveredMain != nullptr &&
             !recoveredMain->body()->statements().empty() &&
             dynamic_cast<const lang::StructuredBindingDecl *>(
                 recoveredMain->body()->statements().front().get()) != nullptr,
         "parser recovery should resume at a structured binding after a "
         "missing semicolon");
}

void testTypeAliases() {
  lang::Lexer lexer;
  const std::string source = R"(
using LaterSize = Size;
using Size = uint64_t;
using Triple = int[3];

class Box<T> {
  using stored_type = T;
  T value;
public:
  using value_type = stored_type;
  Box(T value) : value(value) {}
  value_type get() { return this.value; }
};

class Traits {
public:
  using value_type = hidden_type;
private:
  using hidden_type = int;
public:
  value_type identity(value_type value) { return value; }
public:
  Traits() = default;
};

using IntBox = Box<int>;
using TraitValue = Traits::value_type;
using BoxValue = IntBox::value_type;
using Completion = Box<bool>;
using Result = int;

Completion complete() { return Completion(true); }

Result main() {
  LaterSize count = Size(3);
  Triple values = {1, 2, 3};
  IntBox box = IntBox(values[1]);
  Traits traits{};
  BoxValue input = 4;
  TraitValue trait_value = traits.identity(input);
  Completion completion = complete();
  if (count == 3 and box.get() == 2 and trait_value == 4 and
      completion.get()) { return 0; }
  return 1;
}
)";
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "type aliases should lex as declarations");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "namespace-scoped type aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "type aliases should canonicalize before semantic checks");

  const lang::FrontendResult aliasFrontend = analyzeEmissionFixture(source);
  const auto boxType = [&](lang::SemanticType argument) {
    for (const lang::HirClassInstance &instance :
         aliasFrontend.hir.classInstances())
      if (instance.source != nullptr &&
          instance.source->name().lexeme == "Box" &&
          instance.typeArguments == std::vector<lang::SemanticType>{argument})
        return instance.type;
    return lang::SemanticType(lang::SemanticType::Unknown);
  };
  std::unordered_map<std::string, lang::SemanticType> loweredAliases;
  if (aliasFrontend.canGenerateCode()) {
    const lang::LoweredProgram aliasProgram =
        gti_test::lowerOptimizedProgram(aliasFrontend);
    for (const lang::LoweredDeclaration &declaration :
         aliasProgram.declarations()) {
      if (const auto *alias = std::get_if<lang::LoweredTypeAliasDeclaration>(
              &declaration.payload))
        loweredAliases.emplace(declaration.name, alias->type);
    }
  }
  const auto aliasIs = [&](const std::string &name,
                           const lang::SemanticType &type) {
    const auto found = loweredAliases.find(name);
    return found != loweredAliases.end() && found->second == type &&
           type.kind != lang::SemanticType::Unknown;
  };
  const lang::MirFunctionInstance *aliasMain =
      mirFunctionNamed(aliasFrontend, "main");
  expect(
      aliasIs("LaterSize", lang::SemanticType::UInt64) &&
          aliasIs("Triple",
                  lang::SemanticType::arrayOf(lang::SemanticType::Int32, 3)) &&
          aliasIs("IntBox", boxType(lang::SemanticType::Int32)) &&
          aliasIs("TraitValue", lang::SemanticType::Int32) &&
          aliasIs("Completion", boxType(lang::SemanticType::Bool)) &&
          hasSourceMirBody(aliasMain) &&
          aliasMain->returnType == lang::SemanticType::Int32 &&
          anyMirInstruction(
              aliasMain->body,
              [](const lang::MirInstruction &instruction) {
                return (instruction.operation == lang::MirOperation::Convert ||
                        instruction.operation == lang::MirOperation::Literal) &&
                       instruction.info.type == lang::SemanticType::UInt64;
              }) &&
          shippedBackendAccepts(aliasFrontend),
      "the lowered program should carry canonical, declaration-order-"
      "independent aliases and MIR should use their canonical types");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("class-type-aliases.gti", source);
  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const std::size_t aliasDeclaration = source.find("value_type = hidden_type");
  const std::size_t aliasUse = source.find("Traits::value_type");
  const std::optional<lang::HoverInfo> hover =
      queries.hover(frontend, unit, aliasUse + 9);
  const std::optional<lang::DefinitionInfo> definition =
      queries.definition(frontend, unit, aliasUse + 9);
  expect(frontend.canGenerateCode() && hover &&
             hover->signature == "using Traits::value_type = int32_t" &&
             definition && definition->target.start == aliasDeclaration,
         "class-scoped aliases should participate in shared hover and "
         "definition queries");
  const std::size_t concreteAliasUse = source.find("IntBox::value_type");
  const auto concreteAliasDefinition =
      queries.definition(frontend, unit, concreteAliasUse + 9);
  const auto concreteAliasHover =
      queries.hover(frontend, unit, concreteAliasUse + 9);
  const auto qualifierDefinition =
      queries.definition(frontend, unit, concreteAliasUse + 1);
  expect(concreteAliasDefinition && qualifierDefinition && concreteAliasHover &&
             concreteAliasHover->signature ==
                 "using Box::value_type = int32_t" &&
             concreteAliasDefinition->target.start ==
                 source.find("value_type = stored_type") &&
             qualifierDefinition->target.start == source.find("IntBox ="),
         "concrete alias qualifiers and their members should retain distinct "
         "source declaration identities for editor navigation");

  const lang::FrontendResult constrainedAlias = lang::Frontend().analyze(
      "generic-class-type-alias.gti",
      R"(
class NumericBox<std::numeric T> {
public:
  using value_type = T;
  value_type copy(value_type input) { return value_type(input); }
public:
  NumericBox() = default;
};
int main() {
  NumericBox<int> box{};
  return box.copy(7) - 7;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(constrainedAlias.canGenerateCode(),
         "a generic class alias should preserve the owner's capabilities and "
         "substitute its concrete target in an instantiated member body");

  const auto qualifiedAliases =
      lang::Frontend().analyze("concrete-alias-members.gti", R"(
using Value = Concrete::nested_type::value_type;
using Concrete = Outer<int>;
struct Inner<T> { using value_type = T;  public: Inner() = default; };
struct Outer<T> { using nested_type = Inner<T>;  public: Outer() = default; };
int main() { Value value = 7; return value - 7; }
)");
  expect(qualifiedAliases.canGenerateCode(),
         "concrete aliases should qualify nested type members with canonical "
         "substitution independently of declaration order");
  const auto privateQualifier =
      lang::Frontend().analyze("private-alias-qualifier.gti", R"(
struct Inner<T> { using value_type = T;  public: Inner() = default; };
class Outer<T> { using hidden = Inner<T>;  public: Outer() = default; };
using Concrete = Outer<int>;
using Leaked = Concrete::hidden::value_type;
int main() { return 0; }
)");
  expect(!privateQualifier.canGenerateCode() &&
             hasDiagnosticCode(privateQualifier.diagnostics, "GTI-S2007"),
         "a public final member must not bypass a private alias qualifier");

  const lang::FrontendResult classKinds =
      lang::Frontend().analyze("class-kind-type-aliases.gti",
                               R"(
struct RecordTraits { using value_type = int;  public: RecordTraits() = default; };
interface ContractTraits { using value_type = int; };
union UnionTraits {
  using value_type = int;
  int value;
};
using RecordValue = RecordTraits::value_type;
using ContractValue = ContractTraits::value_type;
using UnionValue = UnionTraits::value_type;
int main() {
  RecordValue record = 1;
  ContractValue contract = 2;
  UnionValue variant = 3;
  return record + contract + variant - 6;
}
)");
  expect(classKinds.canGenerateCode(),
         "struct, interface, and union aliases should follow their public "
         "default access and remain usable as direct type members");

  const std::string completionSource =
      "class Traits { public: using value_type = int;  public: Traits() = "
      "default; }; "
      "using Value = Traits::val; int main() { return 0; }";
  const lang::CompletionResult completion = lang::LanguageQueries().complete(
      {.entryPath = "class-alias-completion.gti",
       .source = completionSource,
       .byteOffset = completionSource.rfind("val") + 3});
  const auto completionAlias =
      std::find_if(completion.candidates.begin(), completion.candidates.end(),
                   [](const lang::CompletionCandidate &candidate) {
                     return candidate.label == "value_type";
                   });
  expect(completionAlias != completion.candidates.end() &&
             completionAlias->kind ==
                 lang::CompletionCandidateKind::TypeAlias &&
             completionAlias->detail == "using Traits::value_type = int32_t",
         "qualified completion should expose accessible class-scoped type "
         "aliases with canonical GTI details");

  const auto completeAlias = [](const std::string &source) {
    return lang::LanguageQueries().complete(
        {.entryPath = "concrete-alias-completion.gti",
         .source = source,
         .byteOffset = source.rfind("val") + 3});
  };
  const std::string concreteCompletionSource = R"(
class Box<T> {
  using value_hidden = T;
public:
  using value_type = T;
public:
  Box() = default;
};
using Concrete = Box<int>;
using Partial = Concrete::val;
int main() { return 0; }
)";
  const auto concreteCompletion = completeAlias(concreteCompletionSource);
  const auto concreteCandidate = std::find_if(
      concreteCompletion.candidates.begin(),
      concreteCompletion.candidates.end(),
      [](const auto &candidate) { return candidate.label == "value_type"; });
  expect(
      concreteCandidate != concreteCompletion.candidates.end() &&
          concreteCandidate->kind == lang::CompletionCandidateKind::TypeAlias &&
          concreteCandidate->detail == "using Concrete::value_type = int32_t" &&
          std::none_of(concreteCompletion.candidates.begin(),
                       concreteCompletion.candidates.end(),
                       [](const auto &candidate) {
                         return candidate.label == "value_hidden";
                       }),
      "concrete alias completion should substitute member types and retain "
      "private filtering");
  const auto nestedCompletion = completeAlias(R"(
struct Inner<T> { using value_type = T;  public: Inner() = default; };
struct Outer<T> { using nested_type = Inner<T>;  public: Outer() = default; };
using Concrete = Outer<uint64_t>;
using Partial = Concrete::nested_type::val;
int main() { return 0; }
)");
  expect(std::any_of(
             nestedCompletion.candidates.begin(),
             nestedCompletion.candidates.end(),
             [](const auto &candidate) {
               return candidate.label == "value_type" &&
                      candidate.detail ==
                          "using Concrete::nested_type::value_type = uint64_t";
             }),
         "chained concrete alias completion should preserve every owner "
         "substitution");
  const auto hiddenCompletion = completeAlias(R"(
struct Inner<T> { using value_type = T;  public: Inner() = default; };
class Outer<T> { using hidden = Inner<T>;  public: Outer() = default; };
using Concrete = Outer<int>;
using Partial = Concrete::hidden::val;
int main() { return 0; }
)");
  expect(std::none_of(hiddenCompletion.candidates.begin(),
                      hiddenCompletion.candidates.end(),
                      [](const auto &candidate) {
                        return candidate.label == "value_type";
                      }),
         "qualified completion must not bypass a private intermediate alias");

  auto invalidTokens = lexer.scan(R"(
using First = Second;
using Second = First;
using Borrow = int&;
using Missing = MissingType;
using Duplicate = int;
using Duplicate = uint64_t;
int main() { return 0; }
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid alias relationships should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "cycles, reference aliases, missing targets, and duplicates should "
         "be rejected");
  expect(hasDiagnostic(invalidSemantic, "Type alias cycle") &&
             hasDiagnostic(invalidSemantic, "Reference aliases") &&
             hasDiagnostic(invalidSemantic, "Unknown type 'MissingType'") &&
             hasDiagnostic(invalidSemantic, "Duplicate declaration"),
         "invalid aliases should produce focused diagnostics");

  const lang::FrontendResult invalidClassAliases =
      lang::Frontend().analyze("invalid-class-type-aliases.gti", R"(
class PrivateTraits {
  using secret_type = int;
public:
  secret_type identity(secret_type value) { return value; }
public:
  PrivateTraits() = default;
};
using Leaked = PrivateTraits::secret_type;

struct DuplicateMember {
  using value_type = int;
  int value_type = 0;
public:
  DuplicateMember() = default;
};

class CallbackOwner {
public:
  using Callback = (int) -> void;
public:
  CallbackOwner() = default;
};

class GenericOwner<T> {
public:
  using value_type = T;
public:
  GenericOwner() = default;
};
using UnboundGenericMember = GenericOwner::value_type;

class BaseOwner {
public:
  using inherited_type = int;
public:
  BaseOwner() = default;
};
class DerivedOwner : public BaseOwner { public: DerivedOwner() = default; };
using UninheritedMember = DerivedOwner::inherited_type;

int main() { return 0; }
)");
  const auto privateDiagnostic =
      std::find_if(invalidClassAliases.diagnostics.begin(),
                   invalidClassAliases.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2007";
                   });
  expect(!invalidClassAliases.canGenerateCode() &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2007") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "secret_type' of 'PrivateTraits' is private") &&
             privateDiagnostic != invalidClassAliases.diagnostics.end() &&
             !privateDiagnostic->related.empty() &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2006") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "already denotes a class-scoped type alias") &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2076") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "Native callback aliases must remain at namespace "
                           "scope") &&
             hasDiagnosticCode(invalidClassAliases.diagnostics, "GTI-S2030") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "belongs to a generic class") &&
             hasDiagnostic(invalidClassAliases.diagnostics,
                           "Unknown type 'DerivedOwner::inherited_type'"),
         "class-scoped aliases should enforce access, name uniqueness, and "
         "the deliberately bounded generic, inheritance, and native callback "
         "boundaries");

  lang::Parser localAliasParser(
      lexer.scan("int main() { using Local = int; return 0; }"));
  (void)localAliasParser.parse();
  expect(
      localAliasParser.hadError() &&
          hasDiagnostic(localAliasParser.errors(), "namespace or class scope"),
      "local type aliases should be rejected by the parser");

  const std::string formatted = lang::Formatter().format(
      "class Traits{using hidden=int;public:using value_type=hidden; public: "
      "Traits() = default; };");
  expect(formatted == R"(class Traits {
  using hidden = int;
public:
  using value_type = hidden;
public:
  Traits() = default;
};
)" && lang::Formatter().format(formatted) == formatted,
         "the formatter should indent class-scoped aliases as ordinary "
         "members and preserve access-label spacing");
}

void testNamespacesAndAliases() {
  lang::Lexer lexer;
  const std::string source = R"(
namespace engine {
namespace graphics {
class Renderer { public: Renderer() = default; };
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
)";
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "namespace source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "nested namespaces and aliases should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "qualified calls and namespace aliases should resolve");

  const lang::FrontendResult namespaced = analyzeEmissionFixture(source);
  if (namespaced.canGenerateCode()) {
    const lang::LoweredProgram namespaceProgram =
        gti_test::lowerOptimizedProgram(namespaced);
    using Kind = lang::LoweredDeclarationKind;
    const lang::LoweredDeclaration *engine =
        loweredDeclarationNamed(namespaceProgram, Kind::Namespace, "engine");
    const lang::LoweredDeclaration *graphics =
        loweredDeclarationNamed(namespaceProgram, Kind::Namespace, "graphics");
    expect(engine != nullptr && graphics != nullptr &&
               graphics->parent == engine->id,
           "the lowered program should preserve nested namespaces");
    const lang::LoweredDeclaration *alias =
        loweredDeclarationNamed(namespaceProgram, Kind::NamespaceAlias, "gfx");
    const auto *aliasPayload =
        alias == nullptr ? nullptr
                         : std::get_if<lang::LoweredNamespaceAliasDeclaration>(
                               &alias->payload);
    expect(aliasPayload != nullptr &&
               aliasPayload->target ==
                   std::vector<std::string>{"engine", "graphics"},
           "the lowered program should preserve namespace aliases");
    const lang::LoweredDeclaration *createRenderer = loweredDeclarationNamed(
        namespaceProgram, Kind::Function, "createRenderer");
    const auto *createPayload =
        createRenderer == nullptr
            ? nullptr
            : std::get_if<lang::LoweredFunctionDeclaration>(
                  &createRenderer->payload);
    const lang::MirClassInstance *renderer =
        mirClassNamed(namespaced, "Renderer");
    expect(createPayload != nullptr && renderer != nullptr &&
               createPayload->returnType == renderer->type,
           "qualified types should resolve through namespace aliases");
    const auto renders = mirFunctionsNamed(namespaced, "render");
    const lang::MirFunctionInstance *renderTwice =
        mirFunctionNamed(namespaced, "renderTwice");
    const lang::MirFunctionInstance *namespacedMain =
        mirFunctionNamed(namespaced, "main");
    const auto isRender = [&](const lang::MirFunctionInstance &target) {
      return renders.size() == 1 && &target == renders.front();
    };
    expect(hasSourceMirBody(namespacedMain) && hasSourceMirBody(renderTwice) &&
               callsFunction(namespaced, namespacedMain->body, isRender) &&
               callsFunction(namespaced, namespacedMain->body,
                             [&](const lang::MirFunctionInstance &target) {
                               return &target == renderTwice;
                             }) &&
               countMirInstructions(
                   namespaced.mir,
                   [&](const lang::MirInstruction &instruction) {
                     return instruction.kind ==
                                lang::MirInstructionKind::Call &&
                            renders.size() == 1 &&
                            instruction.functionTarget == renders.front()->id;
                   }) == 3 &&
               shippedBackendAccepts(namespaced),
           "qualified calls should select the namespaced functions");
  }

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

  std::string shorthand = source;
  const std::string opening = "namespace engine {\nnamespace graphics {";
  shorthand.replace(shorthand.find(opening), opening.size(),
                    "namespace engine::graphics {");
  const std::string closing = "}\n}\n\nnamespace gfx";
  shorthand.replace(shorthand.find(closing), closing.size(),
                    "}\n\nnamespace gfx");
  const lang::FrontendResult compact =
      lang::Frontend().analyze("namespace-shorthand.gti", shorthand);
  expect(compact.canGenerateCode(),
         "namespace shorthand should complete the same verified MIR pipeline "
         "as nested blocks");
  const auto *outer = compact.program.declarations().empty()
                          ? nullptr
                          : lang::stmtAs<lang::NamespaceDecl>(
                                compact.program.declarations().front().get());
  const auto *inner = outer == nullptr || outer->declarations().empty()
                          ? nullptr
                          : lang::stmtAs<lang::NamespaceDecl>(
                                outer->declarations().front().get());
  expect(outer != nullptr && inner != nullptr &&
             outer->name().lexeme == "engine" &&
             inner->name().lexeme == "graphics" &&
             outer->name().position == shorthand.find("engine") &&
             inner->name().position == shorthand.find("graphics") &&
             outer->extent() && inner->extent() &&
             outer->extent()->start == shorthand.find("namespace") &&
             inner->extent()->start == shorthand.find("graphics") &&
             outer->extent()->end == inner->extent()->end,
         "expanded namespace nodes must preserve each written token and "
         "nested source extent");
  const lang::LanguageQueries queries;
  const auto definition =
      queries.definition(compact, compact.sourceGraph.entryUnit(),
                         shorthand.find("engine::graphics::render") + 9);
  expect(definition && definition->target.start == shorthand.find("graphics") &&
             definition->target.end == shorthand.find("graphics") + 8,
         "qualified namespace uses should navigate to the exact shorthand "
         "component");
  const auto symbols =
      queries.documentSymbols(compact, compact.sourceGraph.entryUnit());
  expect(!symbols.empty() && symbols.front().name == "engine" &&
             !symbols.front().children.empty() &&
             symbols.front().children.front().name == "graphics" &&
             symbols.front().children.front().selectionRange.start ==
                 shorthand.find("graphics"),
         "document symbols should retain the ordinary nested hierarchy and "
         "exact shorthand name ranges");

  const lang::FrontendResult reopened =
      lang::Frontend().analyze("namespace-shorthand-reopen.gti", R"(
namespace outer::inner { int original() { return 3; } }
namespace outer {
  namespace inner::detail { int value() { return original(); } }
}
namespace outer::inner::detail {
  int forward() { return value(); }
}
namespace shortcut = outer::inner::detail;
int main() { return shortcut::forward() - 3; }
)");
  expect(reopened.canGenerateCode(),
         "mixed shorthand and explicit blocks should reopen the same "
         "namespaces and retain parent-scope lookup and alias targets");
  const lang::FrontendResult collision = lang::Frontend().analyze(
      "namespace-shorthand-collision.gti",
      "class outer {}; namespace outer::inner {} int main() { return 0; }");
  expect(collision.syntaxValid && !collision.semanticValid,
         "namespace shorthand must not bypass existing declaration conflicts");

  const auto moduleRoot =
      gti_test::temporaryDirectory() / "gti-namespace-shorthand";
  const auto modulePath =
      std::filesystem::weakly_canonical(moduleRoot / "api.gti").string();
  const std::unordered_map<std::string, std::string> modules{
      {modulePath,
       "export module api;\n"
       "export namespace public_api::nested { int value() { return 3; } }\n"
       "namespace private_api::nested { int secret() { return 4; } }\n"}};
  const auto exported = lang::Frontend().analyze(
      moduleRoot / "main.gti",
      "import \"api.gti\"; int main() { return public_api::nested::value(); }",
      {}, modules);
  expect(exported.canGenerateCode(),
         "an exported shorthand namespace should export its ordinary nested "
         "declarations");
  const auto privateMember =
      lang::Frontend().analyze(moduleRoot / "main.gti",
                               "import \"api.gti\"; int main() { return "
                               "private_api::nested::secret(); }",
                               {}, modules);
  expect(privateMember.syntaxValid && !privateMember.semanticValid,
         "shorthand must not export private module declarations");

  const std::string formatted =
      lang::Formatter().format("namespace outer :: inner{namespace "
                               "detail::api{int value(){return 3;}}}");
  expect(formatted == "namespace outer::inner {\n"
                      "  namespace detail::api {\n"
                      "    int value() {\n      return 3;\n    }\n  }\n}\n" &&
             lang::Formatter().format(formatted) == formatted,
         "formatting should preserve shorthand with canonical scope spacing "
         "and idempotent namespace indentation");

  const std::string missingName = "namespace outer:: {}";
  lang::Parser missingComponent(
      lexer.scan(missingName, "namespace-missing-component.gti"));
  (void)missingComponent.parse();
  expect(!missingComponent.errors().empty() &&
             missingComponent.errors().front().code == "GTI-P0001" &&
             missingComponent.errors().front().phase ==
                 lang::DiagnosticPhase::Parsing &&
             missingComponent.errors().front().message ==
                 "Expect namespace name after '::'." &&
             missingComponent.errors().front().primary.source ==
                 "namespace-missing-component.gti" &&
             missingComponent.errors().front().primary.start ==
                 missingName.find('{') &&
             missingComponent.errors().front().primary.end ==
                 missingName.find('{') + 1 &&
             missingComponent.errors().front().fixes.empty(),
         "a missing path component should diagnose its exact boundary without "
         "guessing a namespace name");

  for (const std::string_view invalid :
       {"namespace outer:: {}", "namespace outer::::inner {}",
        "namespace ::outer {}", "namespace outer::inner = target;"}) {
    lang::Parser malformed(
        lexer.scan(std::string(invalid) + " int recovered() { return 0; }"));
    const lang::Program recovered = malformed.parse();
    expect(malformed.hadError() &&
               std::any_of(
                   recovered.declarations().begin(),
                   recovered.declarations().end(),
                   [](const lang::StmtPtr &statement) {
                     const auto *function =
                         lang::stmtAs<lang::FunctionDecl>(statement.get());
                     return function && function->name().lexeme == "recovered";
                   }),
           "malformed namespace paths should diagnose and preserve later "
           "declarations");
  }
  std::string deep = "namespace n";
  for (int index = 1; index < 64; ++index) {
    deep += "::n";
  }
  lang::Parser bounded(lexer.scan(deep + " {}"));
  (void)bounded.parse();
  expect(!bounded.hadError(),
         "64 empty namespace levels should fit the existing parser budget");
  lang::Parser excessive(lexer.scan(deep + "::n {}"));
  (void)excessive.parse();
  expect(excessive.hadError() &&
             std::any_of(excessive.errors().begin(), excessive.errors().end(),
                         [](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == "GTI-P0005";
                         }),
         "shorthand components must not bypass the parser nesting limit");
}

void testExternCInterop() {
  lang::Lexer lexer;
  const std::vector<lang::Token> keywordTokens = lexer.scan("extern \"C\" {}");
  expect(!keywordTokens.empty() &&
             keywordTokens.front().kind == lang::TokenKind::EXTERN,
         "extern should have a dedicated keyword token");

  const std::string source = R"(
extern "C" {
  int32_t socket(int32_t domain, int32_t type, int32_t protocol);
  int32_t close(int32_t descriptor);
}

int main() {
  int32_t descriptor = socket(2, 1, 0);
  if (descriptor >= 0) {
    [[discard]] close(descriptor);
  }
  return 0;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("extern-c.gti", source);
  expect(frontend.canGenerateCode(),
         "fixed-width extern C declarations and calls should compile");
  expect(frontend.diagnostics.empty(),
         "valid extern C declarations should not produce diagnostics");

  const auto *block = frontend.program.declarations().empty()
                          ? nullptr
                          : dynamic_cast<const lang::ExternCDecl *>(
                                frontend.program.declarations().front().get());
  expect(block != nullptr && block->declarations().size() == 2,
         "the AST should retain an extern C linkage block and its prototypes");
  const auto *socketDeclaration =
      block == nullptr || block->declarations().empty()
          ? nullptr
          : dynamic_cast<const lang::FunctionDecl *>(
                block->declarations().front().get());
  expect(socketDeclaration != nullptr && socketDeclaration->hasCLinkage() &&
             !socketDeclaration->body(),
         "functions inside the linkage block should retain bodyless C "
         "linkage identity");

  const lang::FunctionInfo *socketInfo =
      socketDeclaration == nullptr
          ? nullptr
          : frontend.semantics.findFunction(*socketDeclaration);
  expect(socketInfo != nullptr &&
             socketInfo->linkage == lang::LanguageLinkage::C &&
             socketInfo->externalSymbol == "socket",
         "semantics should own the exact native linkage and symbol");
  const auto hirSocket =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.externalSymbol == "socket";
                   });
  expect(hirSocket != frontend.hir.functionInstances().end() &&
             hirSocket->linkage == lang::LanguageLinkage::C,
         "HIR should retain the selected C linkage and native symbol");
  const lang::MirFunctionInstance *mirSocket =
      hirSocket == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(hirSocket->id);
  expect(mirSocket != nullptr &&
             mirSocket->linkage == lang::LanguageLinkage::C &&
             mirSocket->externalSymbol == "socket",
         "MIR should retain C linkage for future backend consumption");

  const lang::MirFunctionInstance *externMain =
      mirFunctionNamed(frontend, "main");
  const auto callsCSymbol = [](const lang::FrontendResult &program,
                               const lang::MirFunctionInstance *caller,
                               std::string_view symbol,
                               std::size_t parameters) {
    return hasSourceMirBody(caller) &&
           callsFunction(program, caller->body,
                         [&](const lang::MirFunctionInstance &target) {
                           return target.linkage == lang::LanguageLinkage::C &&
                                  target.externalSymbol == symbol &&
                                  target.parameterTypes ==
                                      std::vector<lang::SemanticType>(
                                          parameters,
                                          lang::SemanticType::Int32);
                         });
  };
  expect(callsCSymbol(frontend, externMain, "socket", 3) &&
             callsCSymbol(frontend, externMain, "close", 1),
         "resolved C calls use the exact prototype identities bound to native "
         "symbols and scalar ABI types");
  expect(nativeObjectNames(frontend, "socket") &&
             nativeObjectNames(frontend, "close"),
         "the native object should reference the exact C symbols");

  const lang::FrontendResult linkedName =
      lang::Frontend().analyze("extern-c-link-name.gti", R"(
namespace cli11 {
extern "C" {
  [[link_name("gti_cli11_app_create")]]
  int32_t app_create(int32_t value);
}
}
int main() { return cli11::app_create(42) - 42; }
)");
  expect(linkedName.canGenerateCode() && linkedName.diagnostics.empty(),
         "link_name should separate GTI source lookup from a portable C "
         "symbol");
  const auto linkedHir =
      std::find_if(linkedName.hir.functionInstances().begin(),
                   linkedName.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.externalSymbol == "gti_cli11_app_create";
                   });
  const lang::MirFunctionInstance *linkedMir =
      linkedHir == linkedName.hir.functionInstances().end()
          ? nullptr
          : linkedName.mir.findFunctionInstance(linkedHir->id);
  expect(linkedHir != linkedName.hir.functionInstances().end() &&
             linkedHir->source != nullptr &&
             linkedHir->source->name().lexeme == "app_create" &&
             linkedHir->linkage == lang::LanguageLinkage::C &&
             linkedMir != nullptr &&
             linkedMir->externalSymbol == "gti_cli11_app_create",
         "semantic, HIR, and MIR identities should retain the source and link "
         "names independently");
  expect(callsCSymbol(linkedName, mirFunctionNamed(linkedName, "main"),
                      "gti_cli11_app_create", 1) &&
             nativeObjectNames(linkedName, "gti_cli11_app_create"),
         "the backend should declare and call only the selected native link "
         "symbol");

  const lang::FrontendResult lateDeclaration =
      lang::Frontend().analyze("late-extern-c.gti", R"(
namespace platform {
int32_t invoke_close(int32_t descriptor) { return close(descriptor); }
extern "C" { int32_t close(int32_t descriptor); }
}
int main() { return platform::invoke_close(-1); }
)");
  expect(lateDeclaration.canGenerateCode(),
         "namespace-scoped C declarations should resolve before their source "
         "position");
  expect(callsCSymbol(lateDeclaration,
                      mirFunctionNamed(lateDeclaration, "invoke_close"),
                      "close", 1) &&
             nativeObjectNames(lateDeclaration, "close"),
         "a namespace-scoped C prototype should bind earlier callers to the "
         "native symbol");

  const lang::FrontendResult ordinaryPrototype =
      lang::Frontend().analyze("ordinary-prototype.gti", R"(
int32_t native_like(int32_t value);
int main() { return native_like(7); }
)");
  expect(ordinaryPrototype.canGenerateCode(),
         "ordinary GTI bodyless functions should remain valid declarations");
  const lang::MirFunctionInstance *nativeLike =
      mirFunctionNamed(ordinaryPrototype, "native_like");
  expect(nativeLike != nullptr &&
             nativeLike->linkage == lang::LanguageLinkage::Gti &&
             nativeLike->externalSymbol.empty() &&
             nativeLike->definitionKind ==
                 lang::MirDefinitionKind::Declaration &&
             callsFunction(ordinaryPrototype,
                           mirFunctionNamed(ordinaryPrototype, "main")->body,
                           [&](const lang::MirFunctionInstance &target) {
                             return &target == nativeLike;
                           }) &&
             lang::verifyMirProgram(ordinaryPrototype.mir).valid(),
         "ordinary prototypes should retain GTI linkage unless linkage is "
         "explicitly C");

  const lang::FrontendResult runtime =
      analyzeTrustedStandalonePreludeFixture("extern-runtime.gti", R"(
namespace gti_internal {
class text_view {};
namespace runtime {
using native_text = gti_internal::text_view;
extern "C" {
int32_t gti_rt_write_stdout(native_text value);
int64_t gti_rt_open_file_read(native_text path);
}

void write_stdout(gti_internal::text_view value) {
  [[discard]] gti_rt_write_stdout(value);
}

int64_t open_file_read(gti_internal::text_view path) {
  return gti_rt_open_file_read(path);
}
}
}

int main() {
  gti_internal::runtime::write_stdout("hello");
  [[discard]] gti_internal::runtime::open_file_read("input");
  return 0;
}
)");
  expect(runtime.canGenerateCode(),
         "counted text buffers should support runtime C ABI migration");
  const lang::MirFunctionInstance *writeStdout =
      mirFunctionNamed(runtime, "gti_rt_write_stdout");
  expect(writeStdout != nullptr &&
             writeStdout->linkage == lang::LanguageLinkage::C &&
             writeStdout->parameterTypes.size() == 1 &&
             writeStdout->parameterTypes.front() ==
                 lang::SemanticType::StringView &&
             nativeObjectNames(runtime, "gti_rt_write_stdout"),
         "text views should cross C linkage through the explicit public "
         "counted-buffer ABI");

  const lang::FrontendResult shadowedRecord =
      analyzeTrustedStandalonePreludeFixture("shadowed-c-record.gti", R"(
namespace gti_internal { class text_view {}; }
namespace native {
struct gti_c_string_view { public: gti_c_string_view() = default; };
extern "C" {
int32_t measure_text(gti_internal::text_view value);
}
int32_t measure() { return measure_text("x"); }
}
int main() { return native::measure() - 1; }
)");
  expect(shadowedRecord.canGenerateCode(),
         "a GTI namespace should be allowed to reuse the public record's "
         "unqualified spelling");
  expect(nativeObjectNames(shadowedRecord, "measure_text"),
         "C ABI text lowering should not be confused by a source namespace "
         "that shadows the public record spelling");

  const lang::FrontendResult legacyRuntime =
      analyzeTrustedStandalonePreludeFixture("legacy-runtime.gti", R"(
namespace gti_internal {
class text_view {};
namespace runtime {
@runtime("stdout.write")
void write_stdout(gti_internal::text_view value);
}
}
int main() {
  gti_internal::runtime::write_stdout("compatibility");
  return 0;
}
)");
  expect(legacyRuntime.canGenerateCode(),
         "the closed compiler-owned runtime attribute should remain a "
         "compatible internal declaration surface");
  const lang::MirFunctionInstance *legacyWrite =
      mirFunctionNamed(legacyRuntime, "write_stdout");
  expect(legacyWrite != nullptr &&
             legacyWrite->definitionKind ==
                 lang::MirDefinitionKind::RuntimeBinding &&
             legacyWrite->linkage == lang::LanguageLinkage::Gti &&
             lang::verifyMirProgram(legacyRuntime.mir).valid(),
         "legacy runtime bindings should stay separate from public C ABI "
         "lowering");

  const lang::FrontendResult invalidLegacyRuntime =
      analyzeTrustedStandalonePreludeFixture("invalid-legacy-runtime.gti", R"(
namespace gti_internal { class text_view {}; }
@runtime("stdout.write")
void fake_write(gti_internal::text_view value);
int main() { fake_write("hello"); return 0; }
)");
  expect(!invalidLegacyRuntime.semanticValid &&
             hasDiagnostic(invalidLegacyRuntime.diagnostics,
                           "Invalid declaration for runtime binding"),
         "the legacy runtime attribute should remain compiler-owned rather "
         "than becoming a second general FFI");

  auto wrongLanguageTokens =
      lexer.scan("extern \"Rust\" {} int main() { return 0; }");
  lang::Parser wrongLanguageParser(std::move(wrongLanguageTokens));
  wrongLanguageParser.parse();
  expect(wrongLanguageParser.hadError() &&
             hasDiagnostic(wrongLanguageParser.errors(),
                           "only the extern \"C\" linkage"),
         "the parser should reject unknown linkage languages");

  auto bodyTokens = lexer.scan(R"(
extern "C" { int32_t native() { return 1; } }
int main() { return 0; }
)");
  lang::Parser bodyParser(std::move(bodyTokens));
  lang::Program recoveredBody = bodyParser.parse();
  expect(bodyParser.hadError() &&
             hasDiagnostic(bodyParser.errors(), "must be a bodyless") &&
             findTopLevelFunction(recoveredBody, "main") != nullptr,
         "extern C definitions should be rejected without losing following "
         "declarations during recovery");

  auto variableTokens = lexer.scan(R"(
extern "C" { int32_t not_a_function; }
int main() { return 0; }
)");
  lang::Parser variableParser(std::move(variableTokens));
  variableParser.parse();
  expect(
      !variableParser.hadError(),
      "extern C blocks should accept bodyless foreign variable declarations");

  auto duplicateAttributeTokens = lexer.scan(R"(
extern "C" {
  [[link_name("first")]] [[link_name("second")]] int32_t renamed();
  int32_t recovered();
}
int main() { return 0; }
)");
  lang::Parser duplicateAttributeParser(std::move(duplicateAttributeTokens));
  lang::Program duplicateAttributeProgram = duplicateAttributeParser.parse();
  expect(duplicateAttributeParser.hadError() &&
             hasDiagnostic(duplicateAttributeParser.errors(),
                           "Duplicate [[link_name") &&
             findTopLevelFunction(duplicateAttributeProgram, "main") != nullptr,
         "duplicate link_name attributes should diagnose and recover through "
         "the enclosing linkage block");

  auto nestedTokens = lexer.scan(R"(
extern "C" {
  extern "C" { int32_t nested(); }
  int32_t recovered();
}
int main() { return 0; }
)");
  lang::Parser nestedParser(std::move(nestedTokens));
  lang::Program recoveredNested = nestedParser.parse();
  const auto *recoveredBlock =
      recoveredNested.declarations().empty()
          ? nullptr
          : dynamic_cast<const lang::ExternCDecl *>(
                recoveredNested.declarations().front().get());
  expect(nestedParser.hadError() && recoveredBlock != nullptr &&
             recoveredBlock->declarations().size() == 1 &&
             findTopLevelFunction(recoveredNested, "main") != nullptr,
         "invalid nested linkage blocks should recover at the next prototype");

  auto misplacedTokens = lexer.scan(R"(
class Holder {
  extern "C" { int32_t member_native(); }
};
int main() {
  extern "C" { int32_t local_native(); }
  return 0;
}
)");
  lang::Parser misplacedParser(std::move(misplacedTokens));
  lang::Program misplacedProgram = misplacedParser.parse();
  expect(misplacedParser.hadError() &&
             countDiagnosticContaining(misplacedParser.errors(),
                                       "limited to namespace scope") == 2 &&
             findTopLevelFunction(misplacedProgram, "main") != nullptr,
         "class and block linkage declarations should fail clearly without "
         "breaking declaration recovery");

  const lang::FrontendResult externalMain =
      lang::Frontend().analyze("external-main.gti", R"(
namespace native {
extern "C" { [[link_name("main")]] int32_t native_main(); }
}
int main() { return native::native_main(); }
)");
  expect(!externalMain.semanticValid &&
             hasDiagnostic(externalMain.diagnostics,
                           "C symbol 'main' is reserved"),
         "a namespaced C declaration must not collide with the unmangled GTI "
         "entry point");

  const lang::FrontendResult externalStorage =
      lang::Frontend().analyze("external-storage-collision.gti", R"(
int32_t native_counter = 0;
namespace native {
extern "C" {
  [[link_name("native_counter")]] int32_t read_counter();
}
}
int main() { return 0; }
)");
  expect(!externalStorage.semanticValid &&
             hasDiagnostic(externalStorage.diagnostics,
                           "conflicts with a root namespace GTI variable"),
         "an exact C symbol must not alias root GTI storage through another "
         "source namespace");

  const std::string formatted = lang::Formatter().format(
      "extern \"C\"{[[c_array(count)]][[link_name(\"native_values\")]]"
      "int32_t* values(int32_t* count);int32_t close(int32_t descriptor);}");
  expect(formatted == "extern \"C\" {\n"
                      "  [[c_array(count)]][[link_name(\"native_values\")]] "
                      "int32_t* values(int32_t* count);\n"
                      "  int32_t close(int32_t descriptor);\n"
                      "}\n" &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should normalize extern C blocks idempotently");

  const lang::FrontendResult invalid =
      analyzeTrustedPreludeFixture("invalid-extern-c.gti", R"(
class Handle { public: Handle() = default; };
enum class Mode { One };
[[link_name("native_plain")]] int32_t plain();
extern "C" {
  [[link_name("bad-symbol")]] int32_t bad_link_name();
  [[link_name("shared_symbol")]] int32_t first_name();
  [[link_name("shared_symbol")]] int32_t second_name();
  [[link_name("overload_i32")]] int32_t overloaded_name(int32_t value);
  [[link_name("overload_i64")]] int32_t overloaded_name(int64_t value);
  bool bad_bool(bool value);
  char bad_char(char value);
  int32_t bad_reference(int32_t& value);
  int32_t bad_array(int32_t values[4]);
  Handle bad_class(Handle value);
  Mode bad_enum(Mode value);
  expected<int32_t, int32_t> bad_expected(int32_t value);
  gti_internal::text_view bad_text_return();
  static int32_t bad_static(int32_t value);
  int32_t bad_generic<T>(T value);
  int32_t duplicate(int32_t value);
  int32_t duplicate(int64_t value);
}
namespace other {
extern "C" { int32_t duplicate(uint32_t value); }
}
int main() { return 0; }
)");
  expect(
      !invalid.semanticValid &&
          countDiagnosticCode(invalid.diagnostics, "GTI-S2054") >= 15 &&
          hasDiagnostic(invalid.diagnostics, "valid only on an extern \"C\"") &&
          hasDiagnostic(invalid.diagnostics,
                        "is not a portable C identifier") &&
          hasDiagnostic(invalid.diagnostics,
                        "C symbol 'shared_symbol' is declared more than "
                        "once") &&
          hasDiagnostic(invalid.diagnostics,
                        "extern \"C\" functions cannot be overloaded") &&
          hasDiagnostic(invalid.diagnostics,
                        "C symbol 'duplicate' is declared more than once"),
      "semantics should reject unsafe ABI types, modifiers, generics, and "
      "global C-symbol collisions before backend emission");
}

void testScopedEnums() {
  const std::string source = R"(
namespace engine {
enum class State : uint8_t {
  Idle,
  Running = 4,
  Stopped,
};
enum class Mode { editor, game };
}

using StateAlias = engine::State;

StateAlias next(StateAlias state) {
  if (state == StateAlias::Idle) {
    return engine::State::Running;
  }
  return StateAlias::Stopped;
}

int main() {
  auto state = next(StateAlias::Idle);
  if (state == engine::State::Running) {
    return 0;
  }
  return 1;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("scoped-enums.gti", source);
  expect(frontend.canGenerateCode(),
         "scoped enum declarations and qualified enumerators should compile");
  expect(frontend.diagnostics.empty(),
         "valid scoped enums should not produce diagnostics");
  expect(frontend.hir.enumDeclarations().size() == 2,
         "HIR should retain scoped enum declarations");
  const lang::HirEnum *stateEnum =
      frontend.hir.enumDeclarations().empty()
          ? nullptr
          : &frontend.hir.enumDeclarations().front();
  expect(stateEnum != nullptr &&
             stateEnum->underlyingType == lang::SemanticType::UInt8 &&
             stateEnum->enumerators.size() == 3 &&
             stateEnum->enumerators[2].value ==
                 lang::EnumConstant{.magnitude = 5},
         "HIR should retain the backing type and evaluated enumerator values");

  bool foundResolvedEnumerator = false;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      if (value.kind == lang::HirValueKind::QualifiedName && value.enumOwner &&
          value.enumValue) {
        foundResolvedEnumerator = true;
      }
    }
  }
  expect(foundResolvedEnumerator,
         "HIR enum references should carry resolved nominal value identity");

  const lang::HirEnum *modeEnum = frontend.hir.enumDeclarations().size() == 2
                                      ? &frontend.hir.enumDeclarations().back()
                                      : nullptr;
  const lang::MirIntegralEnum *stateMir =
      stateEnum == nullptr ? nullptr
                           : frontend.mir.findIntegralEnum(stateEnum->type);
  const lang::MirIntegralEnum *modeMir =
      modeEnum == nullptr ? nullptr
                          : frontend.mir.findIntegralEnum(modeEnum->type);
  const lang::MirFunctionInstance *nextState =
      mirFunctionNamed(frontend, "next");
  expect(stateMir != nullptr &&
             stateMir->underlyingType == lang::SemanticType::UInt8 &&
             modeMir != nullptr &&
             modeMir->underlyingType == lang::SemanticType::Int32 &&
             stateEnum->enumerators.size() == 3 &&
             stateEnum->enumerators[1].value ==
                 lang::EnumConstant{.magnitude = 4} &&
             hasSourceMirBody(nextState) &&
             anyMirInstruction(nextState->body,
                               [](const lang::MirInstruction &instruction) {
                                 return instruction.operation ==
                                            lang::MirOperation::EnumConstant &&
                                        instruction.enumValue ==
                                            lang::EnumConstant{.magnitude = 4};
                               }) &&
             shippedBackendAccepts(frontend),
         "MIR should carry fixed-backing scoped enums and their exact "
         "enumerator values to the backend");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-scoped-enums.gti", R"(
enum class Tiny : uint8_t {
  First,
  TooLarge = 256,
  First = 2,
};
enum class Other { First };

int main() {
  mut Tiny uninitialized;
  Tiny value = 1;
  int raw = Tiny::First;
  if (Tiny::First) {
    return 1;
  }
  bool mixed = Tiny::First == Other::First;
  auto missing = Tiny::Missing;
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid scoped enum uses should block code generation");
  expect(hasDiagnostic(invalid.diagnostics,
                       "Scoped enum value does not fit its backing type") &&
             hasDiagnostic(invalid.diagnostics, "Duplicate enumerator") &&
             hasDiagnostic(invalid.diagnostics,
                           "Equality operands have incompatible types") &&
             hasDiagnostic(invalid.diagnostics,
                           "explicit enumerator initializer") &&
             hasDiagnostic(invalid.diagnostics, "Unknown enumerator"),
         "scoped enums should diagnose range, scope, and nominal type errors");

  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("enum Legacy { Value }; int main() { return 0; }"));
  const lang::Program recovered = parser.parse();
  expect(!parser.hadError() && recovered.declarations().size() == 2,
         "optional class spelling parses as the same scoped enum construct");

  const std::string formatted = lang::Formatter().format(
      "enum class State:uint8_t{Idle,Running=4,Stopped,};");
  expect(formatted == R"(enum class State : uint8_t {
  Idle,
  Running = 4,
  Stopped,
};
)",
         "formatter should produce stable C++-style scoped enum layout");
  expect(lang::Formatter().format(formatted) == formatted,
         "scoped enum formatting should be idempotent");
}

// A generic declaration's lifecycle describes its open primary. Each concrete
// application records its own special members: a payload that cannot be
// copied deletes the application's copy, and a declared default constructor
// whose requirements fail for the arguments does not exist on that
// application, because construction never selects it.
void testConcreteApplicationSpecialMembers() {
  const std::string declarations = R"(
class Tracked {
  int id;
public:
  Tracked(int value) : id(value) {}
  ~Tracked() {}
};

enum class state<T> { empty, engaged(T value) };

class maybe<T> {
  mut state<T> slot;
public:
  maybe() : slot(state<T>::empty) {}
  maybe(T value) : slot(state<T>::engaged(std::move(value))) {}
  int get() { return 1; }
};

class box<T> {
  mut T item;
public:
  box() requires std::default_initializable<T> : item(T()) {}
  box(T value) : item(std::move(value)) {}
  int get() { return 1; }
};
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "concrete-application-special-members.gti", declarations + R"(
int main() {
  maybe<Tracked> owned = maybe<Tracked>(Tracked(3));
  maybe<int> scalar = maybe<int>();
  box<Tracked> boxed = box<Tracked>(Tracked(5));
  box<int> zero = box<int>();
  return owned.get() + scalar.get() + boxed.get() + zero.get() - 4;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "generic applications over move-only and scalar payloads should "
         "verify");
  if (!frontend.canGenerateCode()) {
    return;
  }

  using Status = lang::SpecialMemberStatus;
  const lang::ClassDecl *boxDeclaration =
      findTopLevelClass(frontend.program, "box");
  const lang::ClassLifecycleInfo *primary =
      boxDeclaration == nullptr
          ? nullptr
          : frontend.semantics.findClassLifecycle(*boxDeclaration);
  expect(primary != nullptr &&
             primary->defaultConstructor == Status::Declared &&
             primary->copyConstructor == Status::Generated &&
             primary->copyAssignment == Status::Generated,
         "the open primary keeps its declared default constructor and "
         "generated copies");

  const auto application = [&](std::string_view name, bool tracked) {
    const auto found =
        std::find_if(frontend.hir.classInstances().begin(),
                     frontend.hir.classInstances().end(),
                     [&](const lang::HirClassInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->name().lexeme == name &&
                              instance.typeArguments.size() == 1 &&
                              (tracked ? instance.typeArguments.front().kind ==
                                             lang::SemanticType::Class
                                       : instance.typeArguments.front() ==
                                             lang::SemanticType::Int32);
                     });
    return found == frontend.hir.classInstances().end() ? nullptr : &*found;
  };
  const auto statuses = [&](const lang::HirClassInstance *instance,
                            Status defaultConstructor, Status copy) {
    const lang::MirClassInstance *sealed =
        instance == nullptr ? nullptr
                            : frontend.mir.findClassInstance(instance->id);
    return instance != nullptr && sealed != nullptr &&
           instance->defaultConstructor == defaultConstructor &&
           instance->copyConstructor == copy &&
           instance->copyAssignment == copy &&
           instance->moveConstructor == Status::Generated &&
           instance->moveAssignment == Status::Generated &&
           sealed->defaultConstructor == instance->defaultConstructor &&
           sealed->copyConstructor == instance->copyConstructor &&
           sealed->copyAssignment == instance->copyAssignment &&
           sealed->moveConstructor == instance->moveConstructor &&
           sealed->moveAssignment == instance->moveAssignment;
  };
  expect(
      statuses(application("maybe", true), Status::Declared, Status::Deleted),
      "a move-only payload deletes maybe<Tracked>'s copies, and its "
      "unconstrained default constructor remains");
  expect(statuses(application("maybe", false), Status::Declared,
                  Status::Generated),
         "maybe<int> keeps its default constructor and generated copies");
  expect(statuses(application("box", true), Status::Deleted, Status::Deleted),
         "box<Tracked> has neither copies nor the default constructor its "
         "requirements exclude");
  expect(
      statuses(application("box", false), Status::Declared, Status::Generated),
      "box<int> satisfies the default constructor's requirements");

  const lang::FrontendResult defaulted = lang::Frontend().analyze(
      "concrete-application-excluded-default.gti", declarations + R"(
int main() {
  box<Tracked> empty = box<Tracked>();
  return empty.get() - 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!defaulted.canGenerateCode() &&
             hasDiagnostic(defaulted.diagnostics, "std::default_initializable"),
         "construction rejects the default constructor box<Tracked>'s "
         "lifecycle excludes");
}

void testDefaultParameters() {
  {
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(
        "int choose(int required, int fallback = 2) { return required; }"));
    const lang::Program parsed = parser.parse();
    const auto *function = parsed.declarations().empty()
                               ? nullptr
                               : dynamic_cast<const lang::FunctionDecl *>(
                                     parsed.declarations().front().get());
    expect(!parser.hadError() && function != nullptr &&
               function->parameters().size() == 2 &&
               !function->parameters()[0].hasDefault() &&
               function->parameters()[1].hasDefault(),
           "the parser should retain a trailing default expression on its "
           "parameter");
  }

  const std::string source = R"(
mut int side_effects = 0;

int advance() {
  side_effects += 1;
  return side_effects;
}

int ordinary(int first, int second = advance(), int third = advance()) {
  return first + second + third;
}

class Token {
public:
  Token() {}
  int method(int value = advance()) { return value; }
  static int static_method(int value = advance()) { return value; }
};

int consume(Token token = Token()) { return 11; }

class Base {
public:
  Base(int value = 8) {}
};

class Derived : public Base {
public:
  Derived() {}
};

T generic_default<T>(T value = 7) { return value; }

constexpr int constant_default(int value = 6) { return value; }
constexpr int folded = constant_default();

int main() {
  int omitted = ordinary(1);
  int explicit_values = ordinary(1, 8, 9);
  Token token = Token();
  int method_value = token.method();
  int static_value = Token::static_method();
  int owned_value = consume();
  Derived derived = Derived();
  int generic_value = generic_default<int>();
  return omitted + explicit_values + method_value + static_value +
         owned_value + generic_value + folded;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("default-parameters.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected default-parameter diagnostic ["
                << diagnostic.code << "]: " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.hir.valid() &&
             frontend.mirValid && frontend.mir.valid() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "free-function, method, constructor, base-constructor, owned, "
         "generic, and constexpr defaults should lower through verified MIR");

  const lang::FunctionDecl *ordinary =
      findTopLevelFunction(frontend.program, "ordinary");
  const lang::FunctionInfo *ordinaryInfo =
      ordinary == nullptr ? nullptr
                          : frontend.semantics.findFunction(*ordinary);
  if (ordinaryInfo != nullptr) {
    const lang::SignaturePrinter signatures(frontend.semantics);
    expect(
        signatures.function(*ordinaryInfo) ==
                "int32_t ordinary(int32_t first, int32_t second = <default>, "
                "int32_t third = <default>)" &&
            signatures.parameterLabels(ordinaryInfo->parameterTypes,
                                       &ordinary->parameters(), false) ==
                std::vector<std::string>{"int32_t first",
                                         "int32_t second = <default>",
                                         "int32_t third = <default>"},
        "compiler-owned signatures should expose which trailing "
        "parameters have defaults");
  } else {
    expect(false, "the default-parameter fixture should retain ordinary()");
  }

  const std::size_t omittedCall = source.rfind("ordinary(1)");
  const std::optional<lang::SignatureHelpInfo> signatureHelp =
      lang::LanguageQueries().signatureHelp(
          frontend, frontend.sourceGraph.entryUnit(),
          omittedCall + std::string("ordinary(").size());
  expect(signatureHelp &&
             signatureHelp->label ==
                 "int32_t ordinary(int32_t first, int32_t second = <default>, "
                 "int32_t third = <default>)" &&
             signatureHelp->parameterLabels.size() == 3 &&
             signatureHelp->parameterLabels[1] == "int32_t second = <default>",
         "signature help should retain omitted parameters and their default "
         "markers");

  const lang::VariableDecl *folded = nullptr;
  for (const lang::StmtPtr &declaration : frontend.program.declarations()) {
    const auto *variable =
        dynamic_cast<const lang::VariableDecl *>(declaration.get());
    if (variable != nullptr && variable->name().lexeme == "folded") {
      folded = variable;
      break;
    }
  }
  const lang::BindingInfo *foldedBinding =
      folded == nullptr ? nullptr : frontend.semantics.findBinding(*folded);
  expect(foldedBinding != nullptr && foldedBinding->constant.has_value(),
         "constexpr evaluation should evaluate an omitted default at its "
         "call site");

  std::size_t hirDefaultInputs = 0;
  bool omittedOrdinaryHasTwoDefaults = false;
  bool explicitOrdinaryHasNoDefaults = false;
  const auto inspectHirBody = [&](const lang::HirBody &body) {
    for (const lang::HirValue &value : body.values) {
      if (!value.callPlan) {
        continue;
      }
      const std::size_t defaults = std::count_if(
          value.callPlan->arguments.begin(), value.callPlan->arguments.end(),
          [](const lang::HirCallArgument &argument) {
            return argument.defaultArgument;
          });
      hirDefaultInputs += defaults;
      const lang::HirFunctionInstance *target =
          value.functionTarget
              ? frontend.hir.findFunctionInstance(*value.functionTarget)
              : nullptr;
      const auto *call = dynamic_cast<const lang::Call *>(value.source);
      if (target == nullptr || target->source == nullptr || call == nullptr ||
          target->source->name().lexeme != "ordinary") {
        continue;
      }
      if (call->arguments().size() == 1) {
        omittedOrdinaryHasTwoDefaults = defaults == 2;
      } else if (call->arguments().size() == 3) {
        explicitOrdinaryHasNoDefaults = defaults == 0;
      }
    }
  };
  inspectHirBody(frontend.hir.module());
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    inspectHirBody(function.body);
  }
  for (const lang::HirConstructorInstance &constructor :
       frontend.hir.constructorInstances()) {
    inspectHirBody(constructor.body);
  }
  if (!(hirDefaultInputs >= 6 && omittedOrdinaryHasTwoDefaults &&
        explicitOrdinaryHasNoDefaults)) {
    std::cerr << "Default-parameter HIR evidence: inputs=" << hirDefaultInputs
              << " omitted-ordinary=" << omittedOrdinaryHasTwoDefaults
              << " explicit-ordinary=" << explicitOrdinaryHasNoDefaults << '\n';
  }
  expect(hirDefaultInputs >= 6 && omittedOrdinaryHasTwoDefaults &&
             explicitOrdinaryHasNoDefaults,
         "HIR call plans should append omitted defaults in parameter order "
         "without marking explicit arguments");

  bool hirImplicitBaseDefault = false;
  for (const lang::HirConstructorInstance &constructor :
       frontend.hir.constructorInstances()) {
    hirImplicitBaseDefault =
        hirImplicitBaseDefault ||
        std::any_of(constructor.initializers.begin(),
                    constructor.initializers.end(),
                    [](const lang::HirConstructorInitializer &initializer) {
                      return initializer.kind ==
                                 lang::ConstructorInitializerTargetKind::Base &&
                             initializer.explicitArgumentCount == 0 &&
                             initializer.arguments.size() == 1;
                    });
  }
  bool mirImplicitBaseDefault = false;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    mirImplicitBaseDefault =
        mirImplicitBaseDefault ||
        std::any_of(constructor.initializers.begin(),
                    constructor.initializers.end(),
                    [](const lang::MirConstructorInitializer &initializer) {
                      return initializer.kind ==
                                 lang::ConstructorInitializerTargetKind::Base &&
                             initializer.explicitArgumentCount == 0 &&
                             initializer.arguments.size() == 1;
                    });
  }
  expect(hirImplicitBaseDefault && mirImplicitBaseDefault,
         "implicit base construction should retain its expanded default and "
         "zero explicit arguments in HIR and MIR");

  std::size_t mirDefaultInputs = 0;
  const auto inspectMirBody = [&](const lang::MirBody &body) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Argument &&
            instruction.defaultArgument) {
          ++mirDefaultInputs;
        }
      }
    }
  };
  inspectMirBody(frontend.mir.module());
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    inspectMirBody(function.body);
  }
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    inspectMirBody(constructor.body);
  }
  if (mirDefaultInputs < 6) {
    std::cerr << "Default-parameter MIR inputs=" << mirDefaultInputs << '\n';
  }
  expect(mirDefaultInputs >= 6,
         "verified MIR should preserve caller-expanded default-argument "
         "provenance on prepared call inputs");

  lang::MirProgram forgedDefaultSuffix = frontend.mir;
  bool forged = false;
  auto &forgedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forgedDefaultSuffix.functionInstances());
  for (lang::MirFunctionInstance &function : forgedFunctions) {
    std::unordered_map<lang::HirValueId, std::vector<lang::MirInstruction *>>
        callInputs;
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputRole == lang::MirCallInputRole::Argument) {
          callInputs[instruction.callSite].push_back(&instruction);
        }
      }
    }
    for (auto &[_, inputs] : callInputs) {
      std::ranges::sort(inputs, {}, [](const lang::MirInstruction *input) {
        return input->callInputIndex;
      });
      if (inputs.size() >= 2 &&
          std::none_of(inputs.begin(), inputs.end(),
                       [](const lang::MirInstruction *input) {
                         return input->defaultArgument;
                       })) {
        inputs.front()->defaultArgument = true;
        forged = true;
        break;
      }
    }
    if (forged) {
      break;
    }
  }
  const lang::MirVerificationResult forgedVerification =
      lang::verifyMirProgram(forgedDefaultSuffix);
  expect(forged && !forgedVerification.valid() &&
             std::any_of(forgedVerification.errors.begin(),
                         forgedVerification.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find(
                                      "trailing parameter suffix") !=
                                  std::string::npos;
                         }),
         "the MIR verifier should reject forged non-trailing default-input "
         "provenance");

  const std::string nonTrailingSource =
      "int invalid(int optional = 1, int required) { return required; }";
  const lang::FrontendResult nonTrailing =
      lang::Frontend().analyze("default-non-trailing.gti", nonTrailingSource);
  const lang::Diagnostic *nonTrailingDiagnostic =
      findDiagnosticByCode(nonTrailing.diagnostics, "GTI-S2077");
  const std::size_t required = nonTrailingSource.find("required");
  const bool nonTrailingRejected =
      !nonTrailing.canGenerateCode() && nonTrailingDiagnostic != nullptr &&
      std::filesystem::path(nonTrailingDiagnostic->primary.source).filename() ==
          "default-non-trailing.gti" &&
      nonTrailingDiagnostic->primary.start == required &&
      nonTrailingDiagnostic->primary.end ==
          required + std::string("required").size() &&
      hasRelatedDiagnostic(nonTrailing.diagnostics,
                           "default-argument suffix") &&
      hasDiagnosticHint(nonTrailing.diagnostics, "Move required parameters");
  if (!nonTrailingRejected && nonTrailingDiagnostic != nullptr) {
    std::cerr << "Non-trailing diagnostic span="
              << nonTrailingDiagnostic->primary.start << ".."
              << nonTrailingDiagnostic->primary.end
              << " source=" << nonTrailingDiagnostic->primary.source << '\n';
  }
  expect(nonTrailingRejected,
         "GTI-S2077 should point at a required parameter after the default "
         "suffix and explain the repair");

  const lang::FrontendResult parameterReference = lang::Frontend().analyze(
      "default-parameter-reference.gti",
      "int dependent(int first, int second = first) { return second; }");
  expect(!parameterReference.canGenerateCode() &&
             hasDiagnosticCode(parameterReference.diagnostics, "GTI-S2077") &&
             hasDiagnostic(parameterReference.diagnostics,
                           "cannot reference a parameter") &&
             hasDiagnosticHint(parameterReference.diagnostics, "function body"),
         "defaults should resolve in declaration scope without parameter "
         "bindings");

  const lang::FrontendResult receiverReference = lang::Frontend().analyze(
      "default-receiver-reference.gti",
      "class Item { public: int read(int value = this.read()) { return "
      "value; }  public: Item() = default; };");
  expect(!receiverReference.canGenerateCode() &&
             hasDiagnosticCode(receiverReference.diagnostics, "GTI-S2077") &&
             hasDiagnostic(receiverReference.diagnostics, "cannot use 'this'"),
         "method defaults should not acquire an implicit receiver");

  const lang::FrontendResult wrongType = lang::Frontend().analyze(
      "default-type.gti", "int invalid(int value = true) { return value; }");
  expect(
      !wrongType.canGenerateCode() &&
          hasDiagnosticCode(wrongType.diagnostics, "GTI-S2077") &&
          hasDiagnostic(wrongType.diagnostics, "parameter requires 'int32_t'"),
      "a default expression should be checked against its concrete "
      "parameter type");

  const lang::FrontendResult recursive = lang::Frontend().analyze(
      "default-cycle.gti",
      "int recurse(int value = recurse()) { return value; }");
  expect(!recursive.canGenerateCode() &&
             hasDiagnosticCode(recursive.diagnostics, "GTI-S2077") &&
             hasDiagnostic(recursive.diagnostics, "recursive expansion") &&
             hasDiagnosticHint(recursive.diagnostics, "Break the cycle"),
         "recursive default expansion should be rejected before HIR "
         "lowering");

  const lang::FrontendResult ambiguous = lang::Frontend().analyze(
      "default-overload.gti",
      "int choose(int value) { return value; }\n"
      "int choose(int value, int fallback = 1) { return value + fallback; }\n"
      "int main() { return choose(1); }\n");
  expect(!ambiguous.canGenerateCode() &&
             hasDiagnosticCode(ambiguous.diagnostics, "GTI-S2013") &&
             hasDiagnostic(ambiguous.diagnostics, "2 overloads exactly match"),
         "defaults should affect viability without silently changing exact "
         "overload ranking");

  const lang::FrontendResult ambiguousImplicitBase = lang::Frontend().analyze(
      "default-base-ambiguity.gti",
      "class Base { public: Base(int value = 1) {} Base(bool value = true) "
      "{} };\n"
      "class Derived : public Base { public: Derived() {} };\n"
      "int main() { return 0; }\n");
  expect(
      !ambiguousImplicitBase.canGenerateCode() &&
          hasDiagnosticCode(ambiguousImplicitBase.diagnostics, "GTI-S2013") &&
          hasDiagnostic(ambiguousImplicitBase.diagnostics,
                        "Implicit construction of base") &&
          hasDiagnosticHint(ambiguousImplicitBase.diagnostics,
                            "explicit base initializer") &&
          hasRelatedDiagnostic(ambiguousImplicitBase.diagnostics,
                               "Candidate: Base(int") &&
          hasRelatedDiagnostic(ambiguousImplicitBase.diagnostics,
                               "Candidate: Base(bool"),
      "implicit base construction should reject multiple constructors "
      "that accept zero explicit arguments");

  const lang::FrontendResult uninferred =
      lang::Frontend().analyze("default-generic-inference.gti",
                               "T make<T>(T value = 1) { return value; }\n"
                               "int main() { return make(); }\n");
  expect(!uninferred.canGenerateCode() &&
             hasDiagnosticCode(uninferred.diagnostics, "GTI-S2023") &&
             hasDiagnostic(uninferred.diagnostics,
                           "provide explicit type arguments"),
         "an omitted default should not infer a generic argument");

  struct UnsupportedDefaultCase {
    std::string name;
    std::string source;
    std::string message;
  };
  const std::array<UnsupportedDefaultCase, 7> unsupported{{
      {"default-extern-c.gti", "extern \"C\" { int native(int value = 1); }",
       "extern \"C\" declarations"},
      {"default-operator.gti",
       "class Number { public: int operator[](uint64_t value = 1) { return "
       "int(value); "
       "}  public: Number() = default; };",
       "operator declarations"},
      {"default-virtual.gti",
       "class Base { public: virtual int inspect(int value = 1) { return "
       "value; }  public: Base() = default; };",
       "virtual, override, or interface methods"},
      {"default-lambda.gti",
       "int main() { auto fn = [](int value = 1) -> int { return value; }; "
       "return 0; }",
       "lambda parameters"},
      {"default-pack.gti", "void relay<Args...>(Args... values = 1) {}",
       "parameter pack"},
      {"default-reference.gti", "int inspect(int& value = 1) { return value; }",
       "owned or plain value"},
      {"default-main.gti", "int main(int value = 0) { return value; }",
       "the main entry point"},
  }};
  for (const UnsupportedDefaultCase &test : unsupported) {
    const lang::FrontendResult result =
        lang::Frontend().analyze(test.name, test.source);
    const bool rejected = !result.canGenerateCode() &&
                          hasDiagnosticCode(result.diagnostics, "GTI-S2077") &&
                          hasDiagnostic(result.diagnostics, test.message);
    if (!rejected) {
      for (const lang::Diagnostic &diagnostic : result.diagnostics) {
        std::cerr << test.name << " [" << diagnostic.code
                  << "]: " << diagnostic.message << '\n';
      }
    }
    expect(rejected,
           "GTI-S2077 should reject the unsupported default context in " +
               test.name);
  }

  const std::string formatted = lang::Formatter().format(
      "int choose(int required,int fallback=2){return required+fallback;}");
  expect(formatted == R"(int choose(int required, int fallback = 2) {
  return required + fallback;
}
)" && lang::Formatter().format(formatted) == formatted,
         "the formatter should space default parameters and remain "
         "idempotent");
}

constexpr NamedTest kTests[] = {
    {"testMovedLoopLocalRedeclarations", testMovedLoopLocalRedeclarations},
    {"testTrustedIntrinsicDeclarations", testTrustedIntrinsicDeclarations},
    {"testThisReceiverKeyword", testThisReceiverKeyword},
    {"testClassesStructsAndAccess", testClassesStructsAndAccess},
    {"testMutableFieldGroups", testMutableFieldGroups},
    {"testConstructorsAndReceiverMutability",
     testConstructorsAndReceiverMutability},
    {"testInheritanceAndInterfaces", testInheritanceAndInterfaces},
    {"testInterfaceStaticMethods", testInterfaceStaticMethods},
    {"testInheritedGenericTargetInstances",
     testInheritedGenericTargetInstances},
    {"testDirectBraceConstruction", testDirectBraceConstruction},
    {"testRecursiveClassLayouts", testRecursiveClassLayouts},
    {"testNamedGenerics", testNamedGenerics},
    {"testNativeGenericCarrierClosure", testNativeGenericCarrierClosure},
    {"testUninstantiatedGenericSignatureClosure",
     testUninstantiatedGenericSignatureClosure},
    {"testDependentCarrierRepresentationClosure",
     testDependentCarrierRepresentationClosure},
    {"testMoveConstructionCapability", testMoveConstructionCapability},
    {"testFiniteWrapperTraits", testFiniteWrapperTraits},
    {"testCompilerPrivateMemberRequirements",
     testCompilerPrivateMemberRequirements},
    {"testValueGenerics", testValueGenerics},
    {"testContextualArrayArguments", testContextualArrayArguments},
    {"testExactFunctionOverloadsAndConversions",
     testExactFunctionOverloadsAndConversions},
    {"testFixedArrays", testFixedArrays},
    {"testFreshArrayOwnership", testFreshArrayOwnership},
    {"testFixedArrayDynamicWriteFailureEdge",
     testFixedArrayDynamicWriteFailureEdge},
    {"testFixedArrayChainedDynamicIndex", testFixedArrayChainedDynamicIndex},
    {"testStructuredBindings", testStructuredBindings},
    {"testTypeAliases", testTypeAliases},
    {"testNamespacesAndAliases", testNamespacesAndAliases},
    {"testExternCInterop", testExternCInterop},
    {"testScopedEnums", testScopedEnums},
    {"testConcreteApplicationSpecialMembers",
     testConcreteApplicationSpecialMembers},
    {"testDefaultParameters", testDefaultParameters},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_declarations_tests");
  return runNamedTests(argc, argv, kTests, "declarations");
}
