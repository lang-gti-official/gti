#include "gti/cpp_backend.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/optimizer.h"
#include "gti/parser.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

struct MirProgramTestAccess {
  static std::vector<MirPayloadEnum> &payloadEnums(MirProgram &program) {
    return program.payloadEnums_;
  }
  static std::vector<MirClassInstance> &classInstances(MirProgram &program) {
    return program.classes;
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

std::size_t countCode(const lang::FrontendResult &result,
                      std::string_view code) {
  return std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
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

bool hasHint(const lang::FrontendResult &result, std::string_view text) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return std::any_of(
                           diagnostic.hints.begin(), diagnostic.hints.end(),
                           [&](const std::string &hint) {
                             return hint.find(text) != std::string::npos;
                           });
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

lang::FrontendResult analyzeWithPrelude(std::string_view name,
                                        std::string source) {
  const std::filesystem::path library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(std::string(name), std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
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
          artifact.contents.find(
              "::gti_internal::backend::payload_variant<std::monostate") !=
              std::string::npos &&
          artifact.contents.find(".__gti_value.index()") != std::string::npos &&
          artifact.contents.find("::gti_internal::backend::payload_get<2>(") !=
              std::string::npos &&
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
  expect(borrowedPayload.canGenerateCode() &&
             lang::verifyMirProgram(borrowedPayload.mir).valid(),
         "a passive borrowed class payload retains its exact owner through "
         "verified MIR");

  const auto danglingPayload = analyze("payload-borrowed-class-escape.gti", R"(
class carrier {
  int32_t& source;
public:
  carrier(int32_t& value) : source(value) {}
};
enum holder { one(carrier value), none };
[[borrowed_from(owner)]] holder escape(int32_t& owner) {
  int32_t local = 3;
  carrier value = carrier(local);
  return holder::one(value);
}
int main() {
  int32_t owner = 7;
  holder result = escape(owner);
  return 0;
}
)");
  expect(danglingPayload.syntaxValid && !danglingPayload.semanticValid &&
             !hasCode(danglingPayload, "GTI-B0001"),
         "a borrowed class payload cannot substitute a dead local for its "
         "declared caller origin");

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
      analyzeWithPrelude("owning-payload-generic-callable-boundary.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
holder transfer<T>(holder value) { return std::move(value); }
int main() {
  holder result = transfer<int32_t>(holder::one(resource()));
  return 0;
}
)");
  if (!genericCallableBoundary.canGenerateCode()) {
    printDiagnostics(genericCallableBoundary);
  }
  expect(genericCallableBoundary.canGenerateCode() &&
             lang::verifyMirProgram(genericCallableBoundary.mir).valid(),
         "concrete generic callables should preserve owning payload-enum "
         "argument and result obligations");

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
      analyzeWithPrelude("owning-payload-generic-owner-boundary.gti", R"(
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
int main() {
  relay<int32_t> receiver = relay<int32_t>(holder::one(resource()));
  holder result = receiver.pass(holder::one(resource()));
  return 0;
}
)");
  if (!genericOwnerBoundary.canGenerateCode()) {
    printDiagnostics(genericOwnerBoundary);
  }
  expect(genericOwnerBoundary.canGenerateCode() &&
             lang::verifyMirProgram(genericOwnerBoundary.mir).valid(),
         "concrete generic owners should preserve owning payload-enum "
         "constructor and method transport");

  const lang::FrontendResult aggregateContainment =
      analyzeWithPrelude("owning-payload-aggregate-containment.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
class wrapper {
public:
  holder value = holder::one(resource());
};
struct default_wrapper {
  holder value = holder::one(resource());
};
class generic_wrapper<T> {
  T value;
public:
  generic_wrapper(T value) : value(std::move(value)) {}
};
int main() {
  wrapper first = wrapper();
  default_wrapper second = default_wrapper();
  generic_wrapper<holder> third = generic_wrapper<holder>(holder::none);
  return 0;
}
)");
  if (!aggregateContainment.canGenerateCode()) {
    printDiagnostics(aggregateContainment);
  }
  std::size_t activeOwners = 0;
  bool exactActiveFieldRows = true;
  for (const lang::MirClassInstance &owner :
       aggregateContainment.mir.classInstances()) {
    const bool ownsPayload =
        std::any_of(owner.declaredFields.begin(), owner.declaredFields.end(),
                    [](const lang::MirClassFieldInfo &field) {
                      return field.type.kind == lang::SemanticType::Enum &&
                             field.requiresActiveCleanup;
                    });
    if (!ownsPayload) {
      continue;
    }
    ++activeOwners;
    exactActiveFieldRows =
        exactActiveFieldRows && owner.requiresActiveCleanup &&
        owner.traits.drop == lang::DropKind::Lexical &&
        owner.declaredFields.size() == 1 && owner.fields.size() == 1 &&
        owner.fieldDropOrder.size() == 1 &&
        owner.declaredFields.front().requiresActiveCleanup &&
        owner.fields.front().requiresActiveCleanup &&
        owner.fieldDropOrder.front().requiresActiveCleanup;
  }
  expect(aggregateContainment.canGenerateCode() &&
             lang::verifyMirProgram(aggregateContainment.mir).valid() &&
             activeOwners == 3 && exactActiveFieldRows,
         "ordinary class and struct fields should retain one exact active "
         "payload-enum cleanup row through verified MIR");

  const lang::BackendArtifact containmentArtifact =
      gti_test::emitCpp(aggregateContainment);
  const std::size_t enumDefinition =
      containmentArtifact.contents.find("struct holder {");
  const std::size_t wrapperDefinition =
      containmentArtifact.contents.find("class wrapper {");
  expect(enumDefinition != std::string::npos &&
             wrapperDefinition != std::string::npos &&
             enumDefinition < wrapperDefinition &&
             containmentArtifact.contents.find(
                 "::gti_internal::backend::payload_variant<std::monostate") !=
                 std::string::npos,
         "the C++ type schedule should define an embedded payload enum before "
         "its owning class and retain the private disengaged shell");

  lang::MirProgram forgedContainment = aggregateContainment.mir;
  auto &forgedClasses =
      lang::MirProgramTestAccess::classInstances(forgedContainment);
  const auto forgedOwner =
      std::find_if(forgedClasses.begin(), forgedClasses.end(),
                   [](const lang::MirClassInstance &owner) {
                     return !owner.type.arguments.empty() &&
                            !owner.declaredFields.empty() &&
                            owner.declaredFields.front().type.kind ==
                                lang::SemanticType::Enum &&
                            owner.declaredFields.front().requiresActiveCleanup;
                   });
  if (forgedOwner != forgedClasses.end()) {
    forgedOwner->declaredFields.front().requiresActiveCleanup = false;
    const lang::MirVerificationResult forgedResult =
        lang::verifyMirProgram(forgedContainment);
    expect(!forgedResult.valid() && (hasMirMessage(forgedResult, "field") ||
                                     hasMirMessage(forgedResult, "cleanup")),
           "a forged container field cleanup row should fail MIR "
           "verification");
  }

  const lang::FrontendResult arrayContainment =
      analyze("owning-payload-array-containment.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
int main() {
  holder values[2] = {holder::one(resource()), holder::none};
  return 0;
}
)");
  expect(hasCode(arrayContainment, "GTI-S2067") &&
             hasMessage(arrayContainment, "Fixed array 'values'") &&
             hasMessage(arrayContainment, "bounded containment slice"),
         "fixed arrays should retain the stable owning payload-enum "
         "containment diagnostic");

  const lang::FrontendResult unsupportedFieldOwners =
      analyze("owning-payload-unsupported-field-owners.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
class generic_wrapper<T> { holder value; };
class polymorphic_wrapper {
private:
  holder value;
public:
  virtual int32_t marker() { return 0; }
};
int main() { return 0; }
)");
  expect(countCode(unsupportedFieldOwners, "GTI-S2067") == 1 &&
             hasHint(unsupportedFieldOwners,
                     "direct owning payload-enum field is supported only") &&
             hasMessage(unsupportedFieldOwners, "Instance field 'value'"),
         "only polymorphic owners retain the direct-field containment "
         "diagnostic; concrete generic ownership is structural");

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
  expect(wrapperContainment.canGenerateCode() &&
             lang::verifyMirProgram(wrapperContainment.mir).valid(),
         "a generic argument alone must not invent a stored ownership edge");

  const lang::FrontendResult resultContainment =
      analyzeWithPrelude("owning-payload-result-containment.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class holder { one(resource value), none };
int main() {
  expected<holder, int32_t> result = holder::none;
  return 0;
}
)");
  expect(!resultContainment.canGenerateCode() &&
             hasCode(resultContainment, "GTI-S2067") &&
             hasMessage(resultContainment, "Binding 'result'") &&
             hasMessage(resultContainment, "result carrier") &&
             hasHint(resultContainment, "ordinary class or struct"),
         "direct enum alternatives in result carriers retain a structural "
         "source diagnostic, rather than reaching backend admission");
  expect(std::any_of(resultContainment.diagnostics.begin(),
                     resultContainment.diagnostics.end(),
                     [](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == "GTI-S2067" &&
                              std::filesystem::path(diagnostic.primary.source)
                                      .filename() ==
                                  "owning-payload-result-containment.gti" &&
                              diagnostic.primary.line == 9 &&
                              diagnostic.primary.end -
                                      diagnostic.primary.start ==
                                  std::string_view("result").size() &&
                              diagnostic.fixes.empty();
                     }),
         "unsupported result containment points to the binding and does not "
         "offer a policy-changing automatic fix");

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
  expect(nestedOwning.canGenerateCode() &&
             lang::verifyMirProgram(nestedOwning.mir).valid(),
         "finite nested owning payload enums use structural cleanup recipes");
  if (nestedOwning.canGenerateCode())
    expect(!gti_test::emitCppText(nestedOwning).empty(),
           "non-generic nested owning payloads reach native representation");

  const lang::FrontendResult genericPayload =
      analyze("generic-class-payload-enum.gti", R"(
class shell<T> {
public:
  shell() {}
};
enum class holder { one(shell<int32_t> value), none };
int main() { return 0; }
)");
  expect(genericPayload.canGenerateCode() &&
             lang::verifyMirProgram(genericPayload.mir).valid(),
         "unused concrete generic class alternatives must still discover "
         "their exact lifecycle representation");

  const lang::FrontendResult genericOwnedPayload =
      analyze("generic-owning-class-payload-enum.gti", R"(
class resource<T> {
  T value;
public:
  resource(T value) : value(value) {}
  ~resource() {}
};
enum class holder { one(resource<int32_t> value), none };
int main() { return 0; }
)");
  expect(genericOwnedPayload.canGenerateCode() &&
             lang::verifyMirProgram(genericOwnedPayload.mir).valid(),
         "an unused generic owning alternative must have an exact concrete "
         "class lifecycle row");
  if (genericOwnedPayload.canGenerateCode()) {
    lang::MirProgram forged = genericOwnedPayload.mir;
    auto &payloadRows = lang::MirProgramTestAccess::payloadEnums(forged);
    expect(!payloadRows.empty(), "the owning generic fixture needs a sum row");
    if (!payloadRows.empty()) {
      auto &variant = payloadRows.front().variants.front();
      variant.payloadTypes.front().arguments.front() =
          lang::SemanticType::UInt64;
      const auto checked = lang::verifyMirProgram(forged);
      expect(!checked.valid() &&
                 hasMirMessage(checked,
                               "payload-enum inline layout is recursive or "
                               "unresolved"),
             "a forged alternative cannot borrow another generic "
             "instantiation's cleanup proof");
    }
  }

  const lang::FrontendResult recursiveLayout =
      analyze("recursive-class-payload-layout.gti", R"(
enum class tree { one(node value), none };
class node { tree data = tree::none; };
int main() { return 0; }
)");
  expect(hasCode(recursiveLayout, "GTI-S2082") &&
             hasMessage(recursiveLayout, "cannot contain itself by value"),
         "a by-value cycle through a payload enum must be rejected before "
         "HIR or native layout planning");

  const auto cleanup = analyze("enum-cleanup-selectors.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class data { one(resource first, resource second), none };
int main() { data value = data::one(resource(), resource()); return 0; }
)");
  expect(cleanup.canGenerateCode() &&
             lang::verifyMirProgram(cleanup.mir).valid(),
         "owning enum cleanup needs a verified active-field recipe");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = cleanup.mir;
    bool changed = false;
    for (const auto &function : forged.functionInstances()) {
      auto &body = const_cast<lang::MirBody &>(function.body);
      for (auto &drop : body.dropObligations) {
        if (changed || drop.dropType.type.kind != lang::SemanticType::Enum ||
            drop.dropType.aggregate.size() != 2)
          continue;
        if (mutation == 0)
          drop.dropType.aggregate.front().payloadVariant = 999;
        else if (mutation == 1)
          drop.dropType.aggregate.front().payloadField.reset();
        else
          std::swap(drop.dropType.aggregate[0], drop.dropType.aggregate[1]);
        changed = true;
      }
    }
    expect(
        changed && !lang::verifyMirProgram(forged).valid(),
        "wrong variant, absent field, or reversed cleanup order must reject");
  }

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

void testBorrowedPayloadPatterns() {
  const std::string prefix = R"(
class resource {
  mut int32_t count = 3;
public:
  resource() {}
  resource(resource& other) = delete;
  resource(resource&& other) = default;
  void add() mut { this.count++; }
  int32_t read() { return this.count; }
};
enum class data { empty, some(resource item) };
)";
  const auto shared = analyze("shared-pattern.gti", prefix + R"(
int main() {
  data value = data::some(resource());
  switch (value) {
    case data::some(auto& item): return item.read();
    case data::empty: return 0;
  }
}
)");
  if (!shared.canGenerateCode()) {
    printDiagnostics(shared);
  }
  expect(shared.canGenerateCode(),
         "a shared pattern borrows a move-only payload");
  if (shared.canGenerateCode()) {
    expect(lang::verifyMirProgram(shared.mir).valid(),
           "shared payload borrowing must verify end to end");
    for (int mutation = 0; mutation != 4; ++mutation) {
      lang::MirProgram forged = shared.mir;
      bool changed = false;
      for (const auto &function : forged.functionInstances()) {
        auto &body = const_cast<lang::MirBody &>(function.body);
        for (auto &block : body.blocks) {
          for (auto &instruction : block.instructions) {
            if (changed ||
                instruction.operation != lang::MirOperation::PayloadExtract ||
                instruction.info.category != lang::ValueCategory::Place)
              continue;
            if (mutation == 0) {
              instruction.payloadIndex = 999;
            } else if (mutation == 1) {
              for (auto &predecessor : body.blocks) {
                if (predecessor.terminator.kind ==
                    lang::MirTerminatorKind::Switch)
                  predecessor.terminator.target = block.id;
              }
            } else if (mutation == 2) {
              for (auto &loan : body.loans)
                if (loan.producedBy == instruction.hirValue)
                  loan.access = lang::AccessMode::Mutable;
            } else {
              instruction.operands.front().place = 999999;
            }
            changed = true;
          }
        }
      }
      expect(changed && !lang::verifyMirProgram(forged).valid(),
             "forged payload field, unguarded edge, upgraded loan, or absent "
             "subject must fail "
             "MIR verification");
    }
  }
  const auto mutablePattern = analyze("mutable-pattern.gti", prefix + R"(
int main() {
  mut data value = data::some(resource());
  switch (value) {
    case data::some(mut auto& item): item.add(); return item.read();
    case data::empty: return 0;
  }
}
)");
  if (!mutablePattern.canGenerateCode())
    printDiagnostics(mutablePattern);
  expect(mutablePattern.canGenerateCode(),
         "an exclusive pattern can mutate its payload");
  const auto mixed = analyze("mixed-copy-borrow-pattern.gti", R"(
enum class data { some(int32_t first, int32_t second) };
int main() {
  data value = data::some(1, 7);
  switch (value) {
    case data::some(copy, auto& reference): return reference;
  }
}
)");
  expect(mixed.canGenerateCode() && lang::verifyMirProgram(mixed.mir).valid(),
         "copied and borrowed bindings compose within one arm");
  const auto immutable = analyze("immutable-pattern.gti", prefix + R"(
int main() {
  data value = data::some(resource());
  switch (value) {
    case data::some(mut auto& item): item.add(); return 0;
    case data::empty: return 0;
  }
}
)");
  expect(hasCode(immutable, "GTI-S2067") &&
             hasMessage(immutable, "mutable enum"),
         "an exclusive pattern cannot upgrade an immutable enum");
  const auto temporary = analyze("temporary-pattern.gti", prefix + R"(
int main() {
  switch (data::some(resource())) {
    case data::some(auto& item): return item.read();
    case data::empty: return 0;
  }
}
)");
  expect(hasCode(temporary, "GTI-S2067") &&
             hasMessage(temporary, "stable enum"),
         "pattern borrows require stable original owner storage");
  const auto copied = analyze("copy-pattern.gti", prefix + R"(
int main() {
  data value = data::some(resource());
  switch (value) {
    case data::some(item): return item.read();
    case data::empty: return 0;
  }
}
)");
  expect(hasCode(copied, "GTI-S2067") && hasMessage(copied, "move-only"),
         "plain patterns continue to copy and reject move-only payloads");
  const auto sharedMutation =
      analyze("shared-pattern-mutation.gti", prefix + R"(
int main() {
  mut data value = data::some(resource());
  switch (value) {
    case data::some(auto& item): item.add(); return 0;
    case data::empty: return 0;
  }
}
)");
  expect(!sharedMutation.canGenerateCode(),
         "a shared payload borrow cannot mutate");
  const auto ownerReplacement =
      analyze("borrowed-pattern-replacement.gti", prefix + R"(
int main() {
  mut data value = data::some(resource());
  switch (value) {
    case data::some(auto& item): value = data::empty; return item.read();
    case data::empty: return 0;
  }
}
)");
  expect(!ownerReplacement.canGenerateCode(),
         "a live payload loan blocks enum replacement");
  const auto localEscape =
      analyze("borrowed-pattern-local-escape.gti", prefix + R"(
resource& escape() {
  data value = data::some(resource());
  switch (value) {
    case data::some(auto& item): return item;
    default: while (true) {}
  }
}
)");
  expect(!localEscape.canGenerateCode(),
         "a payload borrow cannot outlive a local enum");
  const auto siblings = analyze("borrowed-pattern-siblings.gti", prefix + R"(
enum class pair_data { pair(resource left, resource right) };
int main() {
  mut pair_data value = pair_data::pair(resource(), resource());
  switch (value) {
    case pair_data::pair(mut auto& left, mut auto& right):
      left.add(); right.add(); return 0;
  }
}
)");
  expect(!siblings.canGenerateCode(),
         "exclusive payload loans conservatively conflict at the enum owner");
  const std::string formatted = lang::Formatter().format(
      "switch(value){case data::some(mut auto& item):item.add();break;}");
  expect(formatted.find("mut auto& item") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter preserves borrowed patterns and remains idempotent");
}

// The consuming extractions of one function body, grouped by arm block.
struct ConsumingArm {
  std::vector<std::size_t> extractions;
  std::size_t transfers = 0;
};

std::vector<ConsumingArm> consumingArms(const lang::MirBody &body) {
  std::vector<ConsumingArm> arms;
  for (const lang::MirBlock &block : body.blocks) {
    ConsumingArm arm;
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const lang::MirInstruction &instruction = block.instructions[index];
      if (instruction.operation != lang::MirOperation::PayloadExtract ||
          instruction.operands.size() != 1 ||
          instruction.operands.front().kind != lang::MirOperandKind::Move)
        continue;
      arm.extractions.push_back(index);
      arm.transfers += std::count_if(
          instruction.lifecycle.begin(), instruction.lifecycle.end(),
          [](const lang::MirLifecycleEvent &event) {
            return event.kind == lang::MirLifecycleEventKind::TransferOut;
          });
    }
    if (!arm.extractions.empty())
      arms.push_back(std::move(arm));
  }
  return arms;
}

// Applies `mutate` to the first block holding at least `extractions` consuming
// extractions of the body that has them, then reports whether verification
// still accepts the program.
template <typename Mutation>
bool consumingMutationRejected(const lang::FrontendResult &result,
                               std::size_t extractions, Mutation mutate) {
  lang::MirProgram forged = result.mir;
  bool changed = false;
  for (const auto &function : forged.functionInstances()) {
    auto &body = const_cast<lang::MirBody &>(function.body);
    for (auto &block : body.blocks) {
      std::vector<lang::MirInstruction *> moves;
      for (auto &instruction : block.instructions)
        if (instruction.operation == lang::MirOperation::PayloadExtract &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Move)
          moves.push_back(&instruction);
      if (changed || moves.size() < extractions)
        continue;
      changed = mutate(body, block, moves);
    }
  }
  return changed && !lang::verifyMirProgram(forged).valid();
}

void testConsumingPayloadPatterns() {
  const std::string prefix = R"(
class resource {
  int32_t id;
public:
  resource(int32_t value) : id(value) {}
  ~resource() {}
  int32_t read() { return this.id; }
};
enum class data {
  empty,
  some(resource item),
  pair(resource left, resource right),
  mixed(resource item, int32_t code),
};
data make(int32_t id) { return data::some(resource(id)); }
)";
  const auto temporary =
      analyzeWithPrelude("consuming-temporary.gti", prefix + R"(
int32_t total(data value) {
  switch (std::move(value)) {
  case data::some(item):
    return item.read();
  case data::pair(left, right):
    return left.read() + right.read();
  case data::mixed(item, code):
    return item.read() * code;
  case data::empty:
    return 0;
  }
}
int main() {
  switch (make(3)) {
  case data::some(item):
    return total(data::pair(resource(1), resource(2))) - item.read();
  default:
    return 1;
  }
}
)");
  if (!temporary.canGenerateCode()) {
    printDiagnostics(temporary);
  }
  expect(temporary.canGenerateCode() &&
             lang::verifyMirProgram(temporary.mir).valid(),
         "a prvalue or explicitly moved subject moves move-only payloads into "
         "by-value bindings through verified MIR");
  bool exactSchedule = temporary.canGenerateCode();
  std::size_t armCount = 0;
  std::size_t drops = 0;
  for (const auto &function : temporary.mir.functionInstances()) {
    for (const ConsumingArm &arm : consumingArms(function.body)) {
      ++armCount;
      exactSchedule = exactSchedule && arm.transfers == 1;
    }
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.info.type.kind == lang::SemanticType::Enum)
          ++drops;
  }
  expect(exactSchedule && armCount == 4 && drops >= 2,
         "each binding arm moves its fields out once and retires the subject "
         "with one transfer; tag-only and default arms drop the subject");

  if (temporary.canGenerateCode()) {
    expect(consumingMutationRejected(
               temporary, 1,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 moves.back()->lifecycle.clear();
                 return true;
               }),
           "a consuming arm without its subject transfer must fail "
           "verification");
    expect(consumingMutationRejected(
               temporary, 2,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 moves.front()->payloadIndex = moves.back()->payloadIndex;
                 return true;
               }),
           "a consuming arm must take every field exactly once");
    expect(consumingMutationRejected(
               temporary, 2,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 std::swap(moves.front()->lifecycle, moves.back()->lifecycle);
                 return true;
               }),
           "no field may move out of a subject whose obligation already "
           "transferred");
    expect(consumingMutationRejected(
               temporary, 2,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 std::swap(*moves.front(), *moves.back());
                 return true;
               }),
           "reordered consuming extractions cannot reuse a retired carrier "
           "dependency set");
    expect(consumingMutationRejected(
               temporary, 2,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 moves.front()->lifecycle = moves.back()->lifecycle;
                 return true;
               }),
           "duplicate subject transfers cannot preserve a carrier fact "
           "through later field extraction");
    expect(consumingMutationRejected(
               temporary, 1,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 for (auto &event : moves.back()->lifecycle)
                   if (event.kind == lang::MirLifecycleEventKind::TransferOut) {
                     event.source = 0;
                     return true;
                   }
                 return false;
               }),
           "a final transfer with no exact shell obligation cannot justify "
           "retained extraction facts");
    expect(consumingMutationRejected(
               temporary, 1,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 moves.front()->enumVariant = *moves.front()->enumVariant == 0
                                                  ? std::size_t{1}
                                                  : std::size_t{0};
                 return true;
               }),
           "a consuming extraction must read the variant its arm edge "
           "selected");
    expect(consumingMutationRejected(
               temporary, 1,
               [](lang::MirBody &body, lang::MirBlock &block,
                  std::vector<lang::MirInstruction *> &) {
                 for (auto &predecessor : body.blocks)
                   if (predecessor.terminator.kind ==
                       lang::MirTerminatorKind::Switch)
                     for (auto &target : predecessor.terminator.switchTargets)
                       if (target.target == block.id)
                         predecessor.terminator.target = block.id;
                 return true;
               }),
           "a consuming extraction must not be reachable from the default "
           "edge");
    expect(consumingMutationRejected(
               temporary, 1,
               [](lang::MirBody &, lang::MirBlock &,
                  std::vector<lang::MirInstruction *> &moves) {
                 moves.back()->operands.front().kind =
                     lang::MirOperandKind::Copy;
                 return true;
               }),
           "a copied extraction cannot carry the subject transfer");
  }

  const auto movedLocal =
      analyzeWithPrelude("consuming-moved-local.gti", prefix + R"(
int main() {
  data value = make(1);
  switch (std::move(value)) {
  case data::some(item):
    return item.read() - 1;
  default:
    return 2;
  }
}
)");
  if (!movedLocal.canGenerateCode()) {
    printDiagnostics(movedLocal);
  }
  expect(movedLocal.canGenerateCode() &&
             lang::verifyMirProgram(movedLocal.mir).valid(),
         "std::move of a named owner consumes it into the match");

  const auto reused =
      analyzeWithPrelude("consuming-reused-local.gti", prefix + R"(
int main() {
  data value = make(1);
  switch (std::move(value)) {
  case data::some:
    break;
  default:
    break;
  }
  switch (value) {
  case data::some:
    return 1;
  default:
    return 0;
  }
}
)");
  expect(hasCode(reused, "GTI-S2018") && hasMessage(reused, "moved"),
         "the moved switch subject stays moved on every path, including "
         "tag-only arms");

  const auto borrowed =
      analyzeWithPrelude("consuming-borrowed-binding.gti", prefix + R"(
int main() {
  switch (make(1)) {
  case data::some(auto& item):
    return item.read();
  default:
    return 0;
  }
}
)");
  expect(hasCode(borrowed, "GTI-S2067") &&
             hasMessage(borrowed, "stable enum place") &&
             hasMessage(borrowed, "consumes its subject") &&
             hasHint(borrowed, "by value"),
         "a consumed subject cannot supply a borrowed payload binding");

  const auto movedBorrow =
      analyzeWithPrelude("consuming-moved-borrow.gti", prefix + R"(
int main() {
  mut data value = make(1);
  switch (std::move(value)) {
  case data::some(mut auto& item):
    return item.read();
  default:
    return 0;
  }
}
)");
  expect(hasCode(movedBorrow, "GTI-S2067") &&
             hasMessage(movedBorrow, "consumes its subject"),
         "an explicitly moved subject cannot supply a mutable payload borrow");

  const auto observed =
      analyzeWithPrelude("observed-move-only-binding.gti", prefix + R"(
int main() {
  data value = make(1);
  switch (value) {
  case data::some(item):
    return item.read();
  default:
    return 0;
  }
}
)");
  expect(hasCode(observed, "GTI-S2067") &&
             hasMessage(observed, "non-consuming match binding") &&
             hasHint(observed, "consumed enum"),
         "a named subject is observed, so a move-only binding stays rejected "
         "with a hint to consume the enum");

  const auto terminal = analyzeWithPrelude(
      "consuming-terminal-subject.gti", "#include <std/panic>\n" + prefix + R"(
[[noreturn]] data never() { std::panic("never"); }
int main() {
  switch (never()) {
  case data::some(item):
    return item.read();
  default:
    return 0;
  }
}
)");
  if (!terminal.canGenerateCode()) {
    printDiagnostics(terminal);
  }
  expect(terminal.canGenerateCode() &&
             lang::verifyMirProgram(terminal.mir).valid(),
         "a terminal consumed subject leaves the whole switch unreachable");

  const auto generic = analyzeWithPrelude("consuming-generic.gti", R"(
class resource {
public:
  resource() {}
  ~resource() {}
};
enum class state<T> { empty, engaged(T value) };
T unwrap<T>(state<T> source, T fallback) {
  switch (std::move(source)) {
  case state<T>::engaged(value):
    return value;
  case state<T>::empty:
    return fallback;
  }
}
int main() {
  resource taken =
      unwrap<resource>(state<resource>::engaged(resource()), resource());
  return unwrap<int32_t>(state<int32_t>::empty, 0);
}
)");
  if (!generic.canGenerateCode()) {
    printDiagnostics(generic);
  }
  expect(generic.canGenerateCode() &&
             lang::verifyMirProgram(generic.mir).valid(),
         "concrete generic instances consume owning and passive payloads");
}

// A payload enum whose alternative holds a move-only class without a
// destructor needs no cleanup but cannot be copied. A switch observing a
// named place read-borrows it, MIR verification rejects any copy of it, and
// the C++ backend moves the plain local that carries a by-value argument.
void testCleanupFreeMoveOnlyPayloads() {
  const auto program =
      analyzeWithPrelude("cleanup-free-move-only-payload.gti", R"(
struct token {
  int32_t id;
  token(int32_t raw) : id(raw) {}
  token(token& other) = delete;
  token(token&& other) = default;
};
enum class choice {
  none,
  some(token item),
};
token pick(choice value) {
  switch (std::move(value)) {
  case choice::some(item):
    return item;
  case choice::none:
    return token(0);
  }
}
int32_t tag(choice value) {
  switch (value) {
  case choice::some:
    return 1;
  case choice::none:
    return 0;
  }
}
int main() {
  choice named = choice::some(token(2));
  switch (named) {
  case choice::some:
    break;
  case choice::none:
    return 1;
  }
  return pick(std::move(named)).id + tag(choice::none) - 2;
}
)");
  if (!program.canGenerateCode()) {
    printDiagnostics(program);
  }
  const lang::MirPayloadEnum *enumeration =
      program.mir.payloadEnums().size() == 1
          ? &program.mir.payloadEnums().front()
          : nullptr;
  expect(program.canGenerateCode() &&
             lang::verifyMirProgram(program.mir).valid() &&
             enumeration != nullptr && !enumeration->requiresActiveCleanup &&
             enumeration->traits.drop == lang::DropKind::Trivial &&
             !enumeration->traits.copyable && enumeration->traits.movable,
         "a payload enum over a move-only class without a destructor is "
         "cleanup-free and move-only through verified MIR");
  if (!program.canGenerateCode() || enumeration == nullptr)
    return;

  const auto isChoice = [&](const lang::SemanticType &type) {
    return type == enumeration->type;
  };
  std::size_t borrowedSubjects = 0;
  bool copied = false;
  for (const auto &function : program.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions)
        for (const auto &operand : instruction.operands)
          copied = copied || (operand.kind == lang::MirOperandKind::Copy &&
                              isChoice(operand.type));
      const auto &terminator = block.terminator;
      if (terminator.kind == lang::MirTerminatorKind::Switch &&
          terminator.value && isChoice(terminator.value->type)) {
        copied = copied ||
                 terminator.value->kind != lang::MirOperandKind::BorrowRead;
        borrowedSubjects +=
            terminator.value->kind == lang::MirOperandKind::BorrowRead;
      }
    }
  }
  expect(!copied && borrowedSubjects == 3,
         "observing switches over a named local and a parameter, and the "
         "consuming switch's hidden owner, read-borrow their places; nothing "
         "copies the move-only enum");

  // Restoring a copy of the move-only enum must fail verification. Only the
  // observing switches are forged: a consuming switch's hidden owner also
  // feeds its arms' extractions, which other proofs guard.
  const auto rejectsCopy = [&](auto mutate) {
    lang::MirProgram forged = program.mir;
    bool changed = false;
    for (const auto &function : forged.functionInstances()) {
      auto &body = const_cast<lang::MirBody &>(function.body);
      const bool consuming = std::any_of(
          body.blocks.begin(), body.blocks.end(), [](const auto &block) {
            return std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [](const lang::MirInstruction &instruction) {
                  return instruction.operation ==
                             lang::MirOperation::PayloadExtract &&
                         !instruction.operands.empty() &&
                         instruction.operands.front().kind ==
                             lang::MirOperandKind::Move;
                });
          });
      for (auto &block : body.blocks)
        changed = changed || (!consuming && mutate(body, block));
    }
    // Carry the effect summaries the copying schedule derives, so only the
    // copy itself can be what verification rejects.
    const lang::MirProgramEffectSummaries derived =
        lang::deriveMirProgramEffectSummaries(forged);
    for (std::size_t index = 0; index < derived.functions.size(); ++index)
      const_cast<lang::MirFunctionInstance &>(forged.functionInstances()[index])
          .effectSummary = derived.functions[index];
    const lang::MirVerificationResult verified = lang::verifyMirProgram(forged);
    return changed &&
           std::any_of(verified.errors.begin(), verified.errors.end(),
                       [](const lang::MirVerificationError &error) {
                         return error.message.find(
                                    "move-only payload enum is copied") !=
                                std::string::npos;
                       });
  };
  const auto observedSubject = [&](const lang::MirBlock &block) {
    const auto &terminator = block.terminator;
    return terminator.kind == lang::MirTerminatorKind::Switch &&
           terminator.value && isChoice(terminator.value->type) &&
           terminator.value->kind == lang::MirOperandKind::BorrowRead;
  };
  // The schedule MIR produced before observing switches borrowed a
  // non-copyable enum: a Load copying the place, under the subject's read
  // event, and a switch over the copy.
  expect(rejectsCopy([&](lang::MirBody &body, lang::MirBlock &block) {
           if (!observedSubject(block))
             return false;
           const auto read = std::find_if(
               block.instructions.rbegin(), block.instructions.rend(),
               [](const lang::MirInstruction &instruction) {
                 return instruction.ownership && instruction.hirValue != 0;
               });
           if (read == block.instructions.rend())
             return false;
           lang::MirInstructionId next = 0;
           for (const auto &candidate : body.blocks)
             for (const auto &instruction : candidate.instructions)
               next = std::max(next, instruction.id);
           lang::MirInstruction load;
           load.id = next + 1;
           load.kind = lang::MirInstructionKind::Load;
           load.hirValue = read->hirValue;
           load.ownership = read->ownership;
           load.info = {.type = enumeration->type,
                        .category = lang::ValueCategory::Place,
                        .traits = enumeration->traits};
           lang::MirValue copy;
           copy.id = body.values.size() + 1;
           copy.sourceValue = read->hirValue;
           copy.info = load.info;
           copy.definitionBlock = block.id;
           copy.definition = load.id;
           load.result = copy.id;
           load.operands = {{.kind = lang::MirOperandKind::Copy,
                             .place = block.terminator.value->place,
                             .type = enumeration->type}};
           block.terminator.value =
               lang::MirOperand{.kind = lang::MirOperandKind::Value,
                                .value = copy.id,
                                .type = enumeration->type};
           block.instructions.insert(std::prev(read.base()), load);
           body.values.push_back(copy);
           return lang::rebuildMirValueUses(body);
         }),
         "an observing switch cannot copy the move-only enum it reads");
  expect(rejectsCopy([&](lang::MirBody &, lang::MirBlock &block) {
           if (!observedSubject(block))
             return false;
           block.terminator.value->kind = lang::MirOperandKind::Copy;
           return true;
         }),
         "a switch terminator cannot copy the move-only enum it observes");

  const std::string text = gti_test::emitCpp(program).contents;
  const std::size_t observer =
      text.find("_tag(::__gti_program::choice __gti_mir_arg_0) {");
  const std::size_t observerEnd =
      observer == std::string::npos ? observer : text.find("\n  }\n", observer);
  const std::string observerBody =
      observerEnd == std::string::npos
          ? std::string{}
          : text.substr(observer, observerEnd - observer);
  expect(text.find("_pick(std::move(__gti_mir_v_") != std::string::npos &&
             text.find("_tag(std::move(__gti_mir_v_") != std::string::npos &&
             observerBody.find("switch (__gti_mir_p_") != std::string::npos &&
             observerBody.find("switch (__gti_mir_v_") == std::string::npos,
         "by-value arguments move their staged move-only locals, and an "
         "observing switch reads its parameter in place");
}

// The blocks of a body whose switch read-borrows a fixed-array element.
std::vector<const lang::MirBlock *> elementSwitches(const lang::MirBody &body) {
  std::vector<const lang::MirBlock *> switches;
  for (const lang::MirBlock &block : body.blocks) {
    const lang::MirTerminator &terminator = block.terminator;
    const lang::MirPlace *subject =
        terminator.kind == lang::MirTerminatorKind::Switch &&
                terminator.value &&
                terminator.value->kind == lang::MirOperandKind::BorrowRead
            ? body.findPlace(terminator.value->place)
            : nullptr;
    if (subject != nullptr &&
        std::any_of(subject->projections.begin(), subject->projections.end(),
                    [](const lang::MirPlaceProjection &projection) {
                      return projection.kind == lang::MirProjectionKind::Index;
                    }))
      switches.push_back(&block);
  }
  return switches;
}

// The checked AddressOf over the subject place's prefix of `length`
// projections whose successful Invoke edge dominates the switch block.
const lang::MirInstruction *
checkedElementSelection(const lang::MirBody &body,
                        const lang::MirDominanceInfo &dominance,
                        const lang::MirBlock &switchBlock,
                        const lang::MirPlace &subject, std::size_t length) {
  for (const lang::MirBlock &block : body.blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Invoke)
      continue;
    for (const lang::MirInstruction &check : block.instructions) {
      const lang::MirPlace *selected =
          check.id == block.terminator.invokeInstruction &&
                  check.operation == lang::MirOperation::AddressOf &&
                  check.operands.size() == 1
              ? body.findPlace(check.operands.front().place)
              : nullptr;
      if (selected != nullptr && selected->root == subject.root &&
          selected->binding == subject.binding &&
          selected->symbol == subject.symbol &&
          selected->projections.size() == length &&
          std::equal(selected->projections.begin(), selected->projections.end(),
                     subject.projections.begin()) &&
          dominance.dominates(block.terminator.target, switchBlock.id))
        return &check;
    }
  }
  return nullptr;
}

// An observing switch over a fixed-array element read-borrows the element in
// place. Each index the subject place selects is checked first, at an
// AddressOf whose failure edge reports the bound; program verification
// requires that check to dominate the switch. The arms' payload reads share
// the switch's subject, and the C++ backend spells the discriminant and every
// payload read over the same element subscription.
void testFixedArrayElementSubjects() {
  const auto program = analyzeWithPrelude("fixed-array-element-subject.gti", R"(
struct token {
  mut int32_t id;
  token(int32_t raw) : id(raw) {}
};
enum class choice {
  none,
  some(token item),
  pair(int32_t left, int32_t right),
};
struct record {
  choice state;
  record(choice value) : state(value) {}
};
class shelf {
  choice[2] items;

public:
  shelf() : items({choice::none, choice::some(token(3))}) {}
  int32_t at(int32_t index) {
    switch (this.items[index]) {
    case choice::some(auto& item):
      return item.id;
    default:
      return 0;
    }
  }
};
choice[2] table = {choice::none, choice::some(token(5))};
int32_t dynamic(int32_t index) {
  choice[3] items = {choice::none, choice::some(token(4)), choice::pair(5, 6)};
  switch (items[index]) {
  case choice::some(auto& item):
    return item.id;
  case choice::pair(left, auto& right):
    return left * 10 + right;
  case choice::none:
    return 0;
  }
}
int32_t constant() {
  choice[2] items = {choice::none, choice::some(token(4))};
  switch (items[1]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t nested(int32_t row, int32_t column) {
  choice[2][2] grid = {{choice::none, choice::some(token(7))},
                       {choice::pair(1, 2), choice::none}};
  switch (grid[row][column]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t global(int32_t index) {
  switch (table[index]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t field() {
  record[2] records = {record(choice::none), record(choice::some(token(8)))};
  switch (records[1].state) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int32_t update(int32_t index) {
  mut choice[2] items = {choice::none, choice::some(token(9))};
  switch (items[index]) {
  case choice::some(mut auto& item):
    item.id = item.id + 1;
    break;
  default:
    break;
  }
  switch (items[index]) {
  case choice::some(auto& item):
    return item.id;
  default:
    return 0;
  }
}
int main() {
  shelf books{};
  return dynamic(1) + dynamic(2) + constant() + nested(0, 1) + global(1) +
         field() + update(1) + books.at(1) - 97;
}
)");
  if (!program.canGenerateCode()) {
    printDiagnostics(program);
  }
  expect(program.canGenerateCode() &&
             lang::verifyMirProgram(program.mir).valid(),
         "observing switches over fixed-array elements verify end to end");
  if (!program.canGenerateCode())
    return;

  std::size_t switches = 0;
  std::size_t checks = 0;
  std::size_t extractions = 0;
  bool exactChecks = true;
  bool sharedSubjects = true;
  for (const auto &function : program.mir.functionInstances()) {
    const lang::MirBody &body = function.body;
    const std::optional<lang::MirDominanceInfo> dominance =
        lang::computeMirDominance(body);
    for (const lang::MirBlock *block : elementSwitches(body)) {
      ++switches;
      const lang::MirOperand &subject = *block->terminator.value;
      const lang::MirPlace &place = *body.findPlace(subject.place);
      for (std::size_t length = 1; length <= place.projections.size();
           ++length) {
        if (place.projections[length - 1].kind !=
            lang::MirProjectionKind::Index)
          continue;
        const lang::MirInstruction *check =
            dominance ? checkedElementSelection(body, *dominance, *block, place,
                                                length)
                      : nullptr;
        checks += check != nullptr;
        exactChecks = exactChecks && check != nullptr &&
                      check->localFailureSites.size() == 1 &&
                      check->definedFailure.localOrigins.size() == 1 &&
                      check->definedFailure.localOrigins.front().outcomes ==
                          std::vector<lang::DefinedFailureOutcome>{
                              {lang::DefinedFailureCode::IndexOutOfBounds,
                               lang::DefinedFailureDetail::FixedArray}};
      }
      for (const lang::MirBlock &arm : body.blocks)
        for (const lang::MirInstruction &instruction : arm.instructions)
          if (instruction.operation == lang::MirOperation::PayloadExtract &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().place == subject.place) {
            ++extractions;
            sharedSubjects =
                sharedSubjects && instruction.operands.front() == subject;
          }
    }
  }
  expect(switches == 8 && checks == 9 && exactChecks,
         "each element switch follows the bounds-checked selection of every "
         "fixed-array index in its subject place");
  expect(extractions == 10 && sharedSubjects,
         "borrowed and copied payload bindings read the switch's element "
         "subject");

  // A switch reached past an index check that has lost its failure edge must
  // fail verification, including the outer index of a nested selection.
  const auto rejectsUncheckedSelection = [&](std::size_t indices) {
    lang::MirProgram forged = program.mir;
    bool changed = false;
    for (const auto &function : forged.functionInstances()) {
      auto &body = const_cast<lang::MirBody &>(function.body);
      const std::optional<lang::MirDominanceInfo> dominance =
          lang::computeMirDominance(body);
      for (const lang::MirBlock *block : elementSwitches(body)) {
        const lang::MirPlace &place =
            *body.findPlace(block->terminator.value->place);
        if (changed || !dominance ||
            static_cast<std::size_t>(std::count_if(
                place.projections.begin(), place.projections.end(),
                [](const lang::MirPlaceProjection &projection) {
                  return projection.kind == lang::MirProjectionKind::Index;
                })) != indices)
          continue;
        const auto first = std::find_if(
            place.projections.begin(), place.projections.end(),
            [](const lang::MirPlaceProjection &projection) {
              return projection.kind == lang::MirProjectionKind::Index;
            });
        const lang::MirInstruction *check = checkedElementSelection(
            body, *dominance, *block, place,
            static_cast<std::size_t>(first - place.projections.begin()) + 1);
        if (check == nullptr)
          continue;
        const lang::MirInstructionId checkId = check->id;
        for (lang::MirBlock &candidate : body.blocks) {
          lang::MirTerminator &edge = candidate.terminator;
          if (edge.kind != lang::MirTerminatorKind::Invoke ||
              edge.invokeInstruction != checkId)
            continue;
          edge.kind = lang::MirTerminatorKind::Goto;
          edge.invokeInstruction = 0;
          edge.failureRecord = 0;
          edge.elseTarget = 0;
          changed = true;
        }
      }
    }
    return changed && hasMirMessage(lang::verifyMirProgram(forged),
                                    "before its checked selection");
  };
  expect(rejectsUncheckedSelection(1),
         "an element switch past an index check without its failure edge "
         "must fail verification");
  expect(rejectsUncheckedSelection(2),
         "a nested element switch with an unchecked outer index must fail "
         "verification");

  std::string emitted;
  try {
    emitted = gti_test::emitCpp(program).contents;
  } catch (const std::exception &error) {
    std::cerr << "element subject emission: " << error.what() << '\n';
  }
  expect(emitted.find("mir_checked_array_bound_v1(") != std::string::npos &&
             emitted.find("].__gti_value.index()") != std::string::npos &&
             emitted.find("].__gti_value).__gti_field_") != std::string::npos &&
             emitted.find("].state.__gti_value.index()") != std::string::npos,
         "the C++ backend checks the bound, then switches on and reads "
         "payloads from the element itself");
}

} // namespace

int main() {
  testParserSurface();
  testSemanticAndIrModel();
  testUnionSafetyBoundary();
  testPayloadEnumDiagnostics();
  testClassPayloadEnums();
  testOrdinaryEnumsAndFormatting();
  testBorrowedPayloadPatterns();
  testConsumingPayloadPatterns();
  testCleanupFreeMoveOnlyPayloads();
  testFixedArrayElementSubjects();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "All sum-type tests passed\n";
  return 0;
}
