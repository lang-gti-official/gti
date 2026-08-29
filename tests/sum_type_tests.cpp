#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/optimizer.h"
#include "gti/parser.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

struct MirProgramTestAccess {
  static std::vector<MirPayloadEnum> &payloadEnums(MirProgram &program) {
    return program.payloadEnums_;
  }
};

} // namespace lang

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

bool hasMessage(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

bool hasMirMessage(const lang::MirVerificationResult &result,
                   std::string_view text) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(text) != std::string::npos;
                     });
}

lang::FrontendResult analyze(std::string_view name, std::string source) {
  return lang::Frontend().analyze(std::string(name), std::move(source));
}

const lang::ClassDecl *findClass(const lang::Program &program,
                                 std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *candidate =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (candidate != nullptr && candidate->name().lexeme == name) {
      return candidate;
    }
  }
  return nullptr;
}

const lang::EnumDecl *findEnum(const lang::Program &program,
                               std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *candidate =
        dynamic_cast<const lang::EnumDecl *>(declaration.get());
    if (candidate != nullptr && candidate->name().lexeme == name) {
      return candidate;
    }
  }
  return nullptr;
}

std::string validSource() {
  return R"(
enum class BitKind : uint8_t { integer, real };

union Bits {
  mut uint32_t integer;
  mut float real;
  mut BitKind kind;
};

enum class Message {
  quit,
  move(int32_t x, int32_t y),
  code(int32_t value),
};

int32_t inspect(Message message) {
  switch (message) {
  case Message::quit:
    return 1;
  case Message::move(x, y):
    return x + y;
  case Message::code(value):
    return value;
  }
}

int main() {
  mut Bits bits{uint32_t(0)};
  unsafe {
    bits.real = 42.0;
  }
  Message message = Message::move(20, 22);
  return inspect(message) - 42;
}
)";
}

void testParserSurface() {
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(R"(
union Bits { uint32_t integer; float real; };
enum class Event { idle, point(int32_t x, int32_t y), };
)");
  expect(!tokens.empty() && tokens.front().kind == lang::TokenKind::UNION &&
             lang::isKeywordToken(tokens.front().kind),
         "union should be a first-class declaration keyword");

  lang::Parser parser(tokens);
  const lang::Program program = parser.parse();
  const lang::ClassDecl *bits = findClass(program, "Bits");
  const lang::EnumDecl *event = findEnum(program, "Event");
  expect(!parser.hadError() && bits != nullptr &&
             bits->kind() == lang::ClassKind::Union &&
             bits->members().size() == 2,
         "native union syntax should produce a union class declaration");
  expect(event != nullptr && event->enumerators().size() == 2 &&
             event->enumerators()[0].payload.empty() &&
             event->enumerators()[1].payload.size() == 2 &&
             event->enumerators()[1].payload[0].name.lexeme == "x" &&
             event->enumerators()[1].payload[1].name.lexeme == "y",
         "payload enum alternatives should retain named typed fields");
}

void testSemanticAndIrModel() {
  const lang::FrontendResult result = analyze("sum-types.gti", validSource());
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
  }
  expect(result.canGenerateCode() && result.diagnostics.empty(),
         "valid unions and payload enums should complete the frontend");

  const lang::ClassDecl *bits = findClass(result.program, "Bits");
  const lang::EnumDecl *message = findEnum(result.program, "Message");
  const lang::ClassTypeInfo *bitsInfo =
      bits == nullptr ? nullptr : result.semantics.findClassType(*bits);
  const lang::EnumTypeInfo *messageInfo =
      message == nullptr ? nullptr : result.semantics.findEnumType(*message);
  expect(bitsInfo != nullptr && bitsInfo->kind == lang::ClassKind::Union &&
             bitsInfo->unionLayout.has_value() &&
             bitsInfo->unionLayout->sizeBytes == 4 &&
             bitsInfo->unionLayout->abiAlignmentBytes == 4 &&
             bitsInfo->unionLayout->fields.size() == 3,
         "semantic union types should retain target-checked max-field layout "
         "including internal integral-enum facts");
  expect(messageInfo != nullptr && messageInfo->payload &&
             messageInfo->enumerators.size() == 3 &&
             messageInfo->enumerators[1].variantIndex == 1 &&
             messageInfo->enumerators[1].payloadTypes.size() == 2 &&
             messageInfo->enumerators[1].payloadTypes[0] ==
                 lang::SemanticType::Int32,
         "semantic payload enums should retain ordered variant metadata");

  bool hirConstruction = false;
  bool hirExtraction = false;
  for (const lang::HirFunctionInstance &function :
       result.hir.functionInstances()) {
    for (const lang::HirValue &value : function.body.values) {
      hirConstruction = hirConstruction ||
                        value.kind == lang::HirValueKind::PayloadConstruction;
      hirExtraction =
          hirExtraction || value.kind == lang::HirValueKind::PayloadExtraction;
    }
  }
  expect(hirConstruction && hirExtraction,
         "HIR should distinguish payload construction from case extraction");

  bool mirConstruction = false;
  bool mirExtraction = false;
  for (const lang::MirFunctionInstance &function :
       result.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        mirConstruction =
            mirConstruction ||
            instruction.operation == lang::MirOperation::PayloadConstruct;
        mirExtraction = mirExtraction || instruction.operation ==
                                             lang::MirOperation::PayloadExtract;
      }
    }
  }
  expect(mirConstruction && mirExtraction,
         "MIR should preserve payload construction and extraction effects");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(result, result.mir, result.mir, optimizations);
  expect(
      artifact.contents.find("union Bits {") != std::string::npos &&
          artifact.contents.find("std::variant<") != std::string::npos &&
          artifact.contents.find(".__gti_value.index()") != std::string::npos &&
          artifact.contents.find("std::get<1>(") != std::string::npos &&
          artifact.contents.find("std::is_union_v<Bits>") != std::string::npos,
      "the C++ backend should use native unions and tagged variant storage");
}

void testUnionSafetyBoundary() {
  const lang::FrontendResult safeAccess = analyze("safe-union-access.gti", R"(
union Bits { uint32_t integer; float real; };
int main() {
  Bits bits{uint32_t(7)};
  return int32_t(bits.integer);
}
)");
  expect(hasCode(safeAccess, "GTI-S2055") &&
             hasMessage(safeAccess, "Union member access"),
         "union member reads should require an unsafe block");

  const lang::FrontendResult generic =
      analyze("generic-union.gti",
              "union Slot<T> { T value; }; int main() { return 0; }");
  expect(hasCode(generic, "GTI-S2066") &&
             hasMessage(generic, "cannot declare generic parameters"),
         "the passive native-union baseline should reject generic storage");

  const lang::FrontendResult behavior = analyze("behavioral-union.gti", R"(
union Bad {
  int value;
  int read() { unsafe { return this.value; } }
};
int main() { return 0; }
)");
  expect(hasCode(behavior, "GTI-S2066") &&
             hasMessage(behavior, "cannot declare methods"),
         "native unions should not acquire class behavior or hidden lifecycle");

  const lang::FrontendResult initializedField =
      analyze("initialized-union-field.gti",
              "union Bad { int value = 1; }; int main() { return 0; }");
  expect(hasCode(initializedField, "GTI-S2066") &&
             hasMessage(initializedField, "cannot have an initializer"),
         "union declarations should not hide an active-field choice");
}

void testPayloadEnumDiagnostics() {
  const lang::FrontendResult cleanupPayload =
      analyze("payload-cleanup-class.gti", R"(
mut int32_t drops = 0;
class leaf {
public:
  leaf() {}
  ~leaf() { drops = drops + 1; }
  mut int32_t value = 7;
};
enum class holder { one(leaf value), none };
int main() {
  holder held = holder::one(leaf());
  switch (held) {
  case holder::one:
    break;
  case holder::none:
    break;
  }
  return 0;
}
)");
  const lang::MirPayloadEnum *cleanupTable =
      cleanupPayload.mir.payloadEnums().empty()
          ? nullptr
          : &cleanupPayload.mir.payloadEnums().front();
  expect(cleanupPayload.canGenerateCode() &&
             lang::verifyMirProgram(cleanupPayload.mir).valid() &&
             cleanupTable != nullptr && cleanupTable->requiresActiveCleanup &&
             cleanupTable->traits.drop == lang::DropKind::Lexical &&
             cleanupTable->variants.size() == 2 &&
             cleanupTable->variants.front().payloadRequiresActiveCleanup ==
                 std::vector<bool>{true},
         "an active-cleanup class payload should reach a variant-aware, "
         "verified MIR lifecycle table");

  if (cleanupPayload.canGenerateCode() && cleanupTable != nullptr) {
    lang::MirProgram forgedTable = cleanupPayload.mir;
    lang::MirProgramTestAccess::payloadEnums(forgedTable)
        .front()
        .variants.front()
        .payloadRequiresActiveCleanup.front() = false;
    const lang::MirVerificationResult forgedTableResult =
        lang::verifyMirProgram(forgedTable);
    expect(!forgedTableResult.valid() &&
               hasMirMessage(forgedTableResult, "payload-enum lifecycle table"),
           "forged payload cleanup facts should be rejected by MIR "
           "verification");

    lang::MirProgram forgedTransfer = cleanupPayload.mir;
    bool removedTransfer = false;
    for (const lang::MirBodyAddress address :
         lang::enumerateMirBodyAddresses(forgedTransfer)) {
      lang::MirBody *body = lang::findMirBody(forgedTransfer, address);
      if (body == nullptr) {
        continue;
      }
      for (lang::MirBlock &block : body->blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.operation != lang::MirOperation::PayloadConstruct) {
            continue;
          }
          const auto transfer = std::find_if(
              instruction.lifecycle.begin(), instruction.lifecycle.end(),
              [](const lang::MirLifecycleEvent &event) {
                return event.kind == lang::MirLifecycleEventKind::TransferOut;
              });
          if (transfer != instruction.lifecycle.end()) {
            instruction.lifecycle.erase(transfer);
            removedTransfer = true;
            break;
          }
        }
        if (removedTransfer) {
          break;
        }
      }
      if (removedTransfer) {
        break;
      }
    }
    const lang::MirVerificationResult forgedTransferResult =
        lang::verifyMirProgram(forgedTransfer);
    expect(removedTransfer && !forgedTransferResult.valid() &&
               hasMirMessage(forgedTransferResult, "payload-enum instruction"),
           "an owning payload construction without its exact transfer event "
           "should be rejected as forged MIR");
  }

  const lang::FrontendResult backing = analyze("payload-backing.gti", R"(
enum class Result : uint8_t { ok(int32_t value), error };
int main() { return 0; }
)");
  expect(hasCode(backing, "GTI-S2067") &&
             hasMessage(backing, "cannot declare an integral backing type"),
         "payload enums should own their tag representation");

  const lang::FrontendResult construction =
      analyze("payload-construction.gti", R"(
enum class Result { ok(int32_t value), error };
int main() {
  Result first = Result::ok();
  Result second = Result::ok("wrong");
  return 0;
}
)");
  expect(hasCode(construction, "GTI-S2067") &&
             hasMessage(construction, "expects 1 argument") &&
             hasMessage(construction, "requires exact type 'int32_t'"),
         "payload construction should enforce exact arity and types");

  const lang::FrontendResult nonExhaustive =
      analyze("payload-exhaustiveness.gti", R"(
enum class Result { ok(int32_t value), error };
int main() {
  Result result = Result::ok(1);
  switch (result) {
  case Result::ok(value):
    return value;
  }
}
)");
  expect(hasCode(nonExhaustive, "GTI-S2067") &&
             hasMessage(nonExhaustive, "not exhaustive") &&
             hasMessage(nonExhaustive, "Result::error"),
         "payload-enum switches should require every variant or default");

  const lang::FrontendResult badPattern = analyze("payload-pattern.gti", R"(
enum class Pair { values(int32_t left, int32_t right) };
int main() {
  Pair pair = Pair::values(1, 2);
  switch (pair) {
  case Pair::values(value, value):
    return value;
  }
}
)");
  expect(hasCode(badPattern, "GTI-S2067") &&
             hasMessage(badPattern, "Duplicate payload binding"),
         "payload case bindings should be unique within an alternative");
}

void testOrdinaryEnumsAndFormatting() {
  const lang::FrontendResult ordinary = analyze("ordinary-enum.gti", R"(
enum class Stage : uint8_t { ready, running = 4 };
int main() {
  Stage stage = Stage::ready;
  switch (stage) {
  case Stage::ready:
    return 0;
  case Stage::running:
    return 1;
  }
}
)");
  expect(ordinary.canGenerateCode() && ordinary.diagnostics.empty(),
         "payload support must not change integral scoped enums");

  const std::string formatted = lang::Formatter().format(
      "union Bits{mut uint32_t integer;float real;};"
      "enum class Event{idle,point(int32_t x,int32_t y),};");
  const bool validFormatting =
      formatted.find("union Bits {") != std::string::npos &&
      formatted.find("point(int32_t x, int32_t y),") != std::string::npos;
  if (!validFormatting) {
    std::cerr << "Formatted sum types:\n" << formatted;
  }
  expect(validFormatting,
         "the formatter should preserve C++-familiar union and payload syntax");
}

void testClassPayloadEnums() {
  const lang::FrontendResult aliasedOwningPayload =
      analyze("aliased-owning-payload.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
class payload_traits {
public:
  using resource_type = resource;
};
enum class holder { one(payload_traits::resource_type value), none };
int main() {
  holder value = holder::one(resource());
  switch (value) {
  case holder::one:
    return 0;
  case holder::none:
    return 1;
  }
}
)");
  expect(aliasedOwningPayload.canGenerateCode() &&
             lang::verifyMirProgram(aliasedOwningPayload.mir).valid(),
         "class-scoped aliases should canonicalize before owning payload "
         "lifecycle analysis and MIR verification");

  const lang::FrontendResult valid = analyze("payload-class-family.gti", R"(
class Point {
public:
  Point(int32_t x, int32_t y) : x_value(x), y_value(y) {}
  int32_t x() { return this.x_value; }
  int32_t y() { return this.y_value; }
private:
  int32_t x_value;
  int32_t y_value;
};

enum class Shape {
  empty,
  dot(Point at),
  segment(Point from, Point to),
};

int32_t weight(Shape shape) {
  switch (shape) {
  case Shape::empty:
    return 0;
  case Shape::dot(at):
    return at.x() + at.y();
  case Shape::segment(from, to):
    return from.x() + from.y() + to.x() + to.y();
  }
}

int main() {
  Shape nothing = Shape::empty;
  Shape one = Shape::dot(Point(1, 2));
  Point start = Point(3, 4);
  Shape line = Shape::segment(start, Point(5, 6));
  bool weights = weight(nothing) == 0 && weight(one) == 3 &&
                 weight(line) == 18;
  return weights ? 0 : 1;
}
)");
  if (!valid.canGenerateCode()) {
    printDiagnostics(valid);
  }
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "cleanup-free class payloads should construct from prvalues and "
         "named copies, match with copied bindings, and reach valid MIR");

  const lang::FrontendResult borrowedPayload =
      analyze("payload-borrowed-class.gti", R"(
class carrier {
  int32_t& source;

public:
  carrier(int32_t& value) : source(value) {}
};

enum class holder { one(carrier value), none };

int main() {
  mut int32_t backing = 3;
  carrier value = carrier(backing);
  holder held = holder::one(value);
  return 0;
}
)");
  expect(hasCode(borrowedPayload, "GTI-S2067") &&
             hasMessage(borrowedPayload, "borrowed state"),
         "a borrowed-state class payload should be rejected at construction");

  const lang::FrontendResult moveOnlyBinding =
      analyze("payload-move-only-binding.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
int main() {
  holder held = holder::one(resource());
  switch (held) {
  case holder::one(value):
    return 1;
  case holder::none:
    return 0;
  }
}
)");
  expect(hasCode(moveOnlyBinding, "GTI-S2067") &&
             hasMessage(moveOnlyBinding, "non-consuming match binding") &&
             hasMessage(moveOnlyBinding, "move-only type"),
         "a copied pattern binding should reject a move-only active payload "
         "with a tag-only-match hint");

  const lang::FrontendResult referenceBoundary =
      analyze("owning-payload-reference-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
int32_t occupied(holder& value) {
  switch (value) {
  case holder::one:
    return 1;
  case holder::none:
    return 0;
  }
}
int main() {
  holder value = holder::one(resource());
  return occupied(value) == 1 ? 0 : 1;
}
)");
  if (!referenceBoundary.canGenerateCode()) {
    printDiagnostics(referenceBoundary);
  }
  expect(referenceBoundary.canGenerateCode() &&
             lang::verifyMirProgram(referenceBoundary.mir).valid(),
         "an owning payload enum should cross a read-only checked-reference "
         "parameter boundary without transferring its cleanup identity");

  const lang::FrontendResult byValueBoundary =
      analyze("owning-payload-by-value-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
holder transfer(holder value) { return holder::none; }
int main() {
  holder destination = transfer(holder::one(resource()));
  return 0;
}
)");
  if (!byValueBoundary.canGenerateCode()) {
    printDiagnostics(byValueBoundary);
  }
  expect(byValueBoundary.canGenerateCode() &&
             lang::verifyMirProgram(byValueBoundary.mir).valid(),
         "an ordinary free function should transport an owning payload enum "
         "by value through verified MIR");

  const lang::FrontendResult constructorBoundary =
      analyze("owning-payload-constructor-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
class consumer {
public:
  consumer(holder value) {}
};
int main() {
  consumer value = consumer(holder::one(resource()));
  return 0;
}
)");
  if (!constructorBoundary.canGenerateCode()) {
    printDiagnostics(constructorBoundary);
  }
  expect(constructorBoundary.canGenerateCode() &&
             lang::verifyMirProgram(constructorBoundary.mir).valid(),
         "an ordinary constructor should accept an owning payload enum by "
         "value through verified MIR");

  const lang::FrontendResult lambdaBoundary =
      analyze("owning-payload-lambda-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
int main() {
  auto transfer = [](holder value) -> holder {
    return holder::none;
  };
  holder destination = transfer(holder::one(resource()));
  return 0;
}
)");
  if (!lambdaBoundary.canGenerateCode()) {
    printDiagnostics(lambdaBoundary);
  }
  expect(lambdaBoundary.canGenerateCode() &&
             lang::verifyMirProgram(lambdaBoundary.mir).valid(),
         "an exact typed lambda should transport an owning payload enum by "
         "value through verified MIR");

  const lang::FrontendResult genericCallableBoundary =
      analyze("owning-payload-generic-callable-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
holder transfer<T>(holder value) { return holder::none; }
int main() { return 0; }
)");
  expect(hasCode(genericCallableBoundary, "GTI-S2067") &&
             hasMessage(genericCallableBoundary,
                        "by-value function parameter boundary") &&
             hasMessage(genericCallableBoundary,
                        "by-value function return boundary"),
         "generic callable families should retain the stable early owning "
         "payload-enum boundary diagnostic");

  const lang::FrontendResult polymorphicBoundary =
      analyze("owning-payload-polymorphic-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
interface relay {
public:
  virtual holder pass(holder value) = 0;
};
int main() { return 0; }
)");
  expect(
      hasCode(polymorphicBoundary, "GTI-S2067") &&
          hasMessage(polymorphicBoundary,
                     "by-value function parameter boundary") &&
          hasMessage(polymorphicBoundary, "by-value function return boundary"),
      "polymorphic callable families should retain the stable early "
      "owning payload-enum boundary diagnostic");

  const lang::FrontendResult genericOwnerBoundary =
      analyze("owning-payload-generic-owner-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
class relay<T> {
public:
  relay(holder value) {}
  holder pass(holder value) { return std::move(value); }
};
int main() { return 0; }
)");
  expect(
      hasCode(genericOwnerBoundary, "GTI-S2067") &&
          hasMessage(genericOwnerBoundary,
                     "by-value constructor parameter boundary") &&
          hasMessage(genericOwnerBoundary,
                     "by-value function parameter boundary") &&
          hasMessage(genericOwnerBoundary, "by-value function return boundary"),
      "methods and constructors owned by a generic class should retain "
      "the stable early owning payload-enum boundary diagnostic");

  const lang::FrontendResult aggregateContainment =
      analyze("owning-payload-aggregate-containment.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
class wrapper {
private:
  holder value;
};
int main() {
  holder values[2] = {holder::one(resource()), holder::none};
  return 0;
}
)");
  expect(hasCode(aggregateContainment, "GTI-S2067") &&
             hasMessage(aggregateContainment, "Instance field 'value'") &&
             hasMessage(aggregateContainment, "Fixed array 'values'") &&
             hasMessage(aggregateContainment, "local-value slice"),
         "class-field and fixed-array containment should reject owning "
         "payload enums before aggregate lowering");

  const lang::FrontendResult wrapperContainment =
      analyze("owning-payload-wrapper-containment.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
class shell<T> {
public:
  shell() {}
};
int main() {
  shell<holder> wrapped = shell<holder>();
  return 0;
}
)");
  expect(
      hasCode(wrapperContainment, "GTI-S2067") &&
          hasMessage(wrapperContainment, "Binding 'wrapped'") &&
          hasMessage(wrapperContainment, "wrapper type") &&
          hasMessage(wrapperContainment, "nesting owning payload-enum state"),
      "generic wrappers should reject nested owning payload-enum state "
      "structurally even when the concrete wrapper has no field");

  const lang::FrontendResult lambdaCapture =
      analyze("owning-payload-lambda-capture.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
int main() {
  holder source = holder::one(resource());
  auto captured = [value = std::move(source)]() -> int32_t {
    return 1;
  };
  return 0;
}
)");
  expect(hasCode(lambdaCapture, "GTI-S2067") &&
             hasMessage(lambdaCapture, "Lambda capture 'value'") &&
             hasMessage(lambdaCapture, "cannot move owning payload-enum type"),
         "owned lambda captures should reject owning payload enums before "
         "callable-environment lowering");

  const lang::FrontendResult capturingTransport =
      analyze("owning-payload-capturing-transport.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
int main() {
  mut int32_t marker = 1;
  auto captured = [marker = std::move(marker)](holder value) -> holder {
    return holder::none;
  };
  return 0;
}
)");
  expect(hasCode(capturingTransport, "GTI-S2067") &&
             hasMessage(capturingTransport,
                        "by-value lambda parameter boundary") &&
             hasMessage(capturingTransport, "by-value lambda return boundary"),
         "capturing lambdas should retain the stable early diagnostic for "
         "owning payload-enum callable transport");

  const lang::FrontendResult nestedOwning =
      analyze("nested-owning-payload-enum.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class inner { one(resource value), none };
enum class outer { nested(inner value), none };
int main() {
  outer value = outer::nested(inner::one(resource()));
  return 0;
}
)");
  expect(hasCode(nestedOwning, "GTI-S2067") &&
             hasMessage(nestedOwning, "not a concrete class value") &&
             hasMessage(nestedOwning, "owning class alternatives only"),
         "nested owning payload enums should remain a diagnosed omission of "
         "the bounded class-payload slice");

  const lang::FrontendResult genericPayload =
      analyze("generic-class-payload-enum.gti", R"(
class shell<T> {
public:
  shell() {}
};
enum class holder { one(shell<int32_t> value), none };
int main() { return 0; }
)");
  expect(hasCode(genericPayload, "GTI-S2067") &&
             hasMessage(genericPayload, "Generic class payload type") &&
             hasMessage(genericPayload, "bounded payload-enum representation"),
         "concrete generic class payloads should retain a stable early "
         "diagnostic until their representation is defined");

  const lang::FrontendResult stableLayout =
      analyze("owning-payload-layout-query.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
uint64_t payload_size() { return sizeof(holder); }
int main() { return 0; }
)");
  expect(hasCode(stableLayout, "GTI-S2063") &&
             hasMessage(stableLayout, "has no GTI layout contract"),
         "owning payload enums should retain the stable layout-query "
         "diagnostic before HIR lowering");
}

} // namespace

int main() {
  testParserSurface();
  testSemanticAndIrModel();
  testUnionSafetyBoundary();
  testPayloadEnumDiagnostics();
  testClassPayloadEnums();
  testOrdinaryEnumsAndFormatting();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "All sum-type tests passed\n";
  return 0;
}
