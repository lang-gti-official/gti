#include "compiler_test_fixture.h"

namespace {

void callableSignatureFormatting() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"using Callback=std::function < void(Event &) >;",
       "using Callback = std::function<void(Event&)>;\n"},
      {"using Callback=std::function < void(mut Event &) >;",
       "using Callback = std::function<void(mut Event&)>;\n"},
      {"using Callback=Handler < Result(Event &, Other *) mut >;",
       "using Callback = Handler<Result(Event&, Other*) mut>;\n"},
      {"using Callback=Box < Handler < void(Event &) >>;",
       "using Callback = Box<Handler<void(Event&)>>;\n"},
      {"using Callback=Handler < void(Event &, Box<Other> &) && >;",
       "using Callback = Handler<void(Event&, Box<Other>&) &&>;\n"},
      {"using Signature=Result(Event &, mut Other &);",
       "using Signature = Result(Event&, mut Other&);\n"},
      {"using Callback=Handler < void(Event & /* borrow */) >;",
       "using Callback = Handler<void(Event& /* borrow */)>;\n"},
      {"interface Window{using Callback=Handler < void(Event &) >;};",
       "interface Window {\n  using Callback = Handler<void(Event&)>;\n};\n"},
      {"namespace psych { export std::unique_ptr < Window > CreateWindow(); }",
       "namespace psych {\n  export std::unique_ptr<Window> "
       "CreateWindow();\n}\n"},
      {"export Owner < Window > create();", "export Owner<Window> create();\n"},
      {"export /* factory */ Owner < Window > create();",
       "export /* factory */ Owner<Window> create();\n"},
      {"export unsafe static constexpr Owner < Window > create();",
       "export unsafe static constexpr Owner<Window> create();\n"},
      {"constexpr Owner < Window > create();",
       "constexpr Owner<Window> create();\n"},
      {"bool test(){return left<read(value&mask)>right;}",
       "bool test() {\n  return left < read(value & mask) > right;\n}\n"},
      {"bool test(){return left<read(value&mask)&&right>count;}",
       "bool test() {\n  return left < read(value & mask) && right > "
       "count;\n}\n"}};
  for (const auto &[source, expected] : cases) {
    const std::string formatted = lang::Formatter().format(source);
    expect(formatted == expected,
           "callable signature spacing: " + source + "\nactual: " + formatted);
    expect(lang::Formatter().format(formatted) == formatted,
           "callable signature formatting must be idempotent: " + source);
  }
  for (const auto alignment :
       {lang::ReferenceAlignment::Left, lang::ReferenceAlignment::Right,
        lang::ReferenceAlignment::Middle}) {
    lang::FormatOptions options;
    options.referenceAlignment = alignment;
    const std::string formatted = lang::Formatter(options).format(
        "using Callback=Handler<void(Event &, mut Other &)>;");
    const std::string marker =
        alignment == lang::ReferenceAlignment::Left ? "&" : " &";
    expect(formatted == "using Callback = Handler<void(Event" + marker +
                            ", mut Other" + marker + ")>;\n" &&
               lang::Formatter(options).format(formatted) == formatted,
           "unnamed signature parameters must respect ReferenceAlignment");
  }
  for (const std::string source :
       {"using Callback=Handler<void(Event &, Other *)>;",
        "using Callback=Handler<void(Event & /* borrow */)>;",
        "using Callback=Handler<void(Event &", "if(left<read(value&",
        "export Owner < Window > create();"}) {
    const std::string formatted = lang::Formatter().format(source);
    lang::Lexer beforeLexer;
    lang::Lexer afterLexer;
    const auto before = beforeLexer.scan(source);
    const auto after = afterLexer.scan(formatted);
    bool same = !beforeLexer.hadError() && !afterLexer.hadError() &&
                before.size() == after.size();
    // The EOF sentinel's spelling can include trailing whitespace; only
    // significant token spellings belong to the preservation contract.
    for (std::size_t i = 0; same && i < before.size(); ++i)
      same = before[i].kind == after[i].kind &&
             (before[i].kind == lang::TokenKind::END_OF_FILE ||
              before[i].lexeme == after[i].lexeme);
    expect(same, "signature tokens must be preserved: " + source);
    expect(lang::Formatter().format(formatted) == formatted,
           "signature formatting must be idempotent: " + source);
  }
}

void classSignaturePatternSyntax() {
  const std::vector<std::string> patterns = {
      "R(Args...)",      "R()",          "void(Args...)",
      "R(int, Args...)", "R&(R&)",       "Box<R>(std::Pair<R, int>, Args...)",
      "R(Args...) mut",  "R(Args...) &&"};
  for (const std::string &pattern : patterns) {
    const std::string source = "class Callback<" + pattern + "> {};";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(source));
    const auto program = parser.parse();
    expect(!lexer.hadError() && !parser.hadError() &&
               program.declarations().size() == 1,
           "headerless signature pattern should parse: " + pattern);
    if (parser.hadError() || program.declarations().size() != 1) {
      continue;
    }
    const auto *declaration =
        lang::stmtAs<lang::ClassDecl>(program.declarations().front().get());
    expect(declaration && !declaration->isExactSpecialization() &&
               declaration->genericParameters().empty() &&
               declaration->genericArgumentPatterns().size() == 1 &&
               declaration->genericArgumentPatterns()
                   .front()
                   .isFunctionSignature(),
           "patterned primary must not become a flat parameter list or an "
           "exact specialization");
    const std::string formatted = lang::Formatter().format(source);
    lang::Parser formattedParser(lexer.scan(formatted));
    const auto formattedProgram = formattedParser.parse();
    expect(!formattedParser.hadError() &&
               formattedProgram.declarations().size() == 1 &&
               lang::Formatter().format(formatted) == formatted,
           "formatting a patterned primary must preserve parsability and be "
           "idempotent: " +
               pattern);
  }

  const std::string source =
      "struct Callback<R(Args...)> { R call(Args... args);  public: Callback() = default; };";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source));
  const auto program = parser.parse();
  expect(!parser.hadError(), "pattern body should retain ordinary members");
  if (!parser.hadError() && !program.declarations().empty()) {
    const auto *declaration =
        lang::stmtAs<lang::ClassDecl>(program.declarations().front().get());
    const auto &pattern = declaration->genericArgumentPatterns().front();
    expect(pattern.arguments.back().name.last().position == source.find("R(") &&
               pattern.arguments.front().name.last().position ==
                   source.find("Args") &&
               pattern.arguments.front().packExpansion &&
               pattern.arguments.front().packExpansion->position ==
                   source.find("..."),
           "signature result, binder, and expansion retain source offsets");
    expect(declaration->members().size() == 1,
           "pattern head must not consume its class body");
  }

  for (const std::string ordinary :
       {"class Box<T> { public: Box() = default; };", "class Types<Args...> { public: Types() = default; };",
        "class Tag<S = void()> { public: Tag() = default; };", "class Constrained<SomeConcept T> { public: Constrained() = default; };",
        "class Sized<uint64_t N = (2 + 3)> { public: Sized() = default; };"}) {
    lang::Parser ordinaryParser(lexer.scan(ordinary));
    const auto parsed = ordinaryParser.parse();
    expect(!ordinaryParser.hadError() && parsed.declarations().size() == 1,
           "ordinary generic syntax remains accepted: " + ordinary);
    if (!ordinaryParser.hadError() && parsed.declarations().size() == 1) {
      const auto *declaration =
          lang::stmtAs<lang::ClassDecl>(parsed.declarations().front().get());
      expect(declaration && declaration->genericArgumentPatterns().empty() &&
                 !declaration->genericParameters().empty(),
             "defaults/constraints must not turn into argument patterns");
    }
  }

  for (const std::string malformed :
       {"class Bad<R(Args...> {};", "class Bad<R(,)> {};",
        "class Bad<R(Args...) {};"}) {
    lang::Parser brokenParser(lexer.scan(malformed));
    (void)brokenParser.parse();
    expect(brokenParser.hadError(),
           "malformed signature pattern should report a parser error");
  }

  const std::string pending =
      "class Callback<R(Args...)> { public: Callback() = default; }; Callback<int> bad{};";
  const auto frontend =
      lang::Frontend().analyze("pending-class-pattern.gti", pending);
  expect(!frontend.canGenerateCode() &&
             hasDiagnosticCode(frontend.diagnostics, "GTI-S2023") &&
             !hasDiagnosticCode(frontend.diagnostics, "GTI-B0001"),
         "a nonsignature must not enter a signature-pattern class");
  const auto diagnostic =
      std::find_if(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                   [](const auto &value) { return value.code == "GTI-S2023"; });
  if (diagnostic != frontend.diagnostics.end()) {
    expect(diagnostic->primary.start == pending.rfind("Callback") &&
               diagnostic->primary.end == pending.rfind("Callback") + 8 &&
               diagnostic->primary.source ==
                   std::filesystem::absolute("pending-class-pattern.gti")
                       .string() &&
               diagnostic->phase == lang::DiagnosticPhase::Semantics &&
               diagnostic->message.find("do not match") != std::string::npos &&
               diagnostic->fixes.empty(),
           "pending pattern diagnostic names the declaration without a "
           "speculative syntax rewrite");
  }
}

void classSignaturePatternBinding() {
  const std::string source = R"(
using R = bool;
using Binary = int(int, bool);
class Signature<R(Args...)> {
public:
  using signature = R(Args...);
  int value() { return 7; }
  T extra<T>(T value) { return value; }
public:
  Signature() = default;
};
class Stored<R()> {
  R item;
public:
  Stored(R value) : item(value) {}
  R read() { return item; }
};
class Identity<R(R)> {
public:
  R call(R value) { return value; }
public:
  Identity() = default;
};
class Action<void(Args...)> {
public:
  void call(Args... args) {}
public:
  Action() = default;
};
class Prefix<R(int, Args...)> {
public:
  int value() { return 7; }
public:
  Prefix() = default;
};
class Forward<S> {
public:
  Signature<S> child{};
  int value() { return child.value(); }
public:
  Forward() = default;
};
int inspect<S>(Signature<S>& tag) { return tag.value(); }
int main() {
  Signature<void()> empty{};
  Signature<Binary> binary{};
  Signature<int(int, bool)> same{};
  Signature<int&(int&)> reference_metadata{};
  Identity<int(int)> identity{};
  Action<void()> nullary{};
  Action<void(int, bool)> action{};
  Prefix<void(int)> prefix{};
  Forward<void()> forward{};
  Stored<int()> stored{7};
  nullary.call();
  action.call(3, true);
  return inspect(empty) + binary.value() + same.value() +
      reference_metadata.value() + identity.call(7) + prefix.value() +
      forward.value() + stored.read() + binary.extra(7) - 63;
}
)";
  const auto frontend =
      lang::Frontend().analyze("class-signature-pattern.gti", source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "patterned classes should bind a signature without flattening its "
         "public argument list");
  if (frontend.canGenerateCode()) {
    const auto hover = lang::LanguageQueries().hover(
        frontend, frontend.sourceGraph.entryUnit(), source.find("Signature<R"));
    expect(hover && hover->signature == "class Signature<R(Args...)>",
           "patterned declaration hover retains its public signature shape");
    const auto definition = lang::LanguageQueries().definition(
        frontend, frontend.sourceGraph.entryUnit(),
        source.find("class Identity<R(R)") + 17);
    expect(definition && definition->target.start ==
                             source.find("class Identity<R(R)") + 15,
           "repeated pattern binder resolves to its declaration, not an outer "
           "alias");
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "patterned primary instances must reach verified MIR");
    expect(!emitAnalyzedSource(source).empty(),
           "patterned class methods and aliases must lower from resolved "
           "bindings");
    int signatures = 0;
    for (const auto &instance : frontend.hir.classInstances()) {
      if (instance.source && instance.source->name().lexeme == "Signature") {
        ++signatures;
        expect(instance.typeArguments.size() == 1 &&
                   instance.typeArguments.front().kind ==
                       lang::SemanticType::FunctionSignature,
               "public class identity retains one whole function signature");
      }
    }
    expect(signatures == 3, "aliases must share patterned class identity");
    auto lowered = gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
    bool changedProjection = false;
    for (auto &row : const_cast<std::vector<lang::LoweredDeclaration> &>(
             lowered.declarations())) {
      auto *declaration =
          std::get_if<lang::LoweredClassDeclaration>(&row.payload);
      if (declaration && !declaration->genericArgumentPatterns.empty()) {
        expect(!declaration->genericParameters.front().typeArgumentPath.empty(),
               "lowering preserves resolved binder projections");
        declaration->genericParameters.front().typeArgumentPath = {1000};
        changedProjection = true;
        break;
      }
    }
    expect(changedProjection && !lang::verifyLoweredProgram(lowered).empty(),
           "lowered verification rejects a forged binder projection");
  }
  const std::vector<std::string> invalid = {
      "class C<R(R)> { public: C() = default; }; C<int(bool)> bad{};",
      "class C<R(int, Args...)> { public: C() = default; }; C<void(bool)> bad{};",
      "class C<R(Args...)> { public: C() = default; }; C<int> bad{};",
      "class C<R(Args...)> { public: C() = default; }; C<int, int> bad{};",
      "class C<R()> { public: C() = default; }; C<int(int)> bad{};",
      "class C<R(R...)> { public: C() = default; };",
      "class C<R(Args..., int)> { public: C() = default; };",
      "class C<R(Args..., Other...)> { public: C() = default; };",
      "class C<void()> { public: C() = default; };",
      "class C<R(Args...)> { public: C() = default; }; class C<S> {};",
      "class C<R(Args...)> { public: C() = default; }; using Bad = C<int>;",
      "class C<R(Args...)> { public: C() = default; }; class F<S> { C<S> field{};  public: F() = default; }; F<int> bad{};",
      "class C<R(R)> { public: C() = default; }; class F<S> { C<S> field{};  public: F() = default; }; F<int(bool)> bad{};",
  };
  for (const auto &input : invalid) {
    const auto result =
        lang::Frontend().analyze("bad-class-pattern.gti", input);
    expect(!result.canGenerateCode() &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "invalid class pattern must fail in the frontend: " + input);
  }
}

void signatureTypes() {
  const std::string source = R"(
using Action = void();
using Predicate = bool(int);
using Borrowing = int&(int&);
class SignatureTag<S> {
public:
  using signature = S;
  int value() { return 7; }
public:
  SignatureTag() = default;
};
class Description<T> {
public:
  using signature = T(T&);
public:
  Description() = default;
};
int identify<S>(SignatureTag<S>& value) { return value.value(); }
int main() {
  SignatureTag<void()> first{};
  SignatureTag<Action> second{};
  SignatureTag<Predicate> third{};
  SignatureTag<int&(int&)> fourth{};
  Description<int> descriptor{};
  return identify(first) + identify(second) + identify(third) +
         identify(fourth) - 28;
}
)";
  const auto frontend = lang::Frontend().analyze("signature-types.gti", source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "signatures should be usable through aliases and generic arguments");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto tagInstances = std::count_if(
      frontend.hir.classInstances().begin(),
      frontend.hir.classInstances().end(), [](const auto &instance) {
        return instance.type.arguments.size() == 1 &&
               instance.type.arguments.front().kind ==
                   lang::SemanticType::FunctionSignature;
      });
  expect(tagInstances == 3,
         "aliases and literal signatures must share concrete generic identity");
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "signature generic metadata should survive MIR verification");
  bool mutated = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.body.values.empty()) {
      auto body = function.body;
      body.values.front().info.type =
          lang::SemanticType::functionSignature(lang::SemanticType::Void, {});
      expect(!lang::verifyMirBody(body).valid(),
             "MIR must reject a signature masquerading as a runtime value");
      mutated = true;
      break;
    }
  }
  expect(mutated, "signature verifier test should mutate a live body");
  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("using Action = void();") != std::string::npos &&
             generated.find("using Predicate = bool(std::int32_t);") !=
                 std::string::npos,
         "signature aliases should lower as types without function pointers");
  const auto unit = frontend.sourceGraph.entryUnit();
  const auto hover = lang::LanguageQueries().hover(
      frontend, unit, source.find("SignatureTag<Predicate>") + 14);
  expect(hover && hover->signature.find("bool(int32_t)") != std::string::npos,
         "compiler hover should describe the resolved signature");
  const auto signature =
      lang::SemanticType::functionSignature(lang::SemanticType::Void, {});
  expect(!lang::semanticTraits(signature).copyable &&
             !lang::semanticTraits(signature).movable,
         "a signature must not acquire value lifecycle traits");
  const std::string formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted &&
             lang::Frontend()
                 .analyze("formatted-signatures.gti", formatted)
                 .canGenerateCode(),
         "formatting signatures should be idempotent and preserve meaning");
}

void receiverQualifiedSignatures() {
  const std::string source = R"(
using Read = int(int);
using Mutable = int(int) mut;
using Once = int(int) &&;
class Tag<S> { public: Tag() = default; };
int choose<R, Args...>(Tag<R(Args...)>& tag) { return 1; }
int choose<R, Args...>(Tag<R(Args...) mut>& tag) { return 2; }
int choose<R, Args...>(Tag<R(Args...) &&>& tag) { return 3; }
class MutablePattern<R(Args...) mut> { public: using signature = R(Args...) mut;  public: MutablePattern() = default; };
int main() {
  Tag<Read> read{};
  Tag<Mutable> write{};
  Tag<Once> once{};
  Tag<int(int) mut> same{};
  MutablePattern<Mutable> pattern{};
  return choose(read) + choose(write) + choose(once) + choose(same) - 8;
}
)";
  const auto frontend =
      lang::Frontend().analyze("qualified-signatures.gti", source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "receiver qualifiers retain exact identity through aliases, patterns "
         "and generic overload deduction");
  if (!frontend.canGenerateCode())
    return;
  int tags = 0;
  for (const auto &instance : frontend.hir.classInstances())
    if (instance.source && instance.source->name().lexeme == "Tag")
      ++tags;
  expect(
      tags == 3,
      "read, mutable and consuming signatures are distinct; aliases are not");
  const auto hover =
      lang::LanguageQueries().hover(frontend, frontend.sourceGraph.entryUnit(),
                                    source.find("Tag<Mutable>") + 4);
  expect(hover &&
             hover->signature.find("int32_t(int32_t) mut") != std::string::npos,
         "hover preserves the resolved mutable receiver qualifier");
  const auto formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted &&
             lang::Frontend()
                 .analyze("formatted-qualified.gti", formatted)
                 .canGenerateCode(),
         "formatting preserves qualified signature semantics");
  const auto generated = emitAnalyzedSource(source);
  expect(generated.find("using Mutable = std::int32_t(std::int32_t) &;") !=
                 std::string::npos &&
             generated.find("using Once = std::int32_t(std::int32_t) &&;") !=
                 std::string::npos,
         "the native metadata representation distinguishes qualifiers without "
         "pointers");
  const auto dump = lang::MirPrinter().print(frontend.mir);
  expect(dump.find("signature-receiver=1") != std::string::npos &&
             dump.find("signature-receiver=2") != std::string::npos,
         "MIR serialization retains both qualified signature identities");
  auto lowered = gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  bool changedQualifier = false;
  for (auto &row : const_cast<std::vector<lang::LoweredDeclaration> &>(
           lowered.declarations())) {
    auto *declaration =
        std::get_if<lang::LoweredClassDeclaration>(&row.payload);
    if (declaration && !declaration->genericArgumentPatterns.empty()) {
      declaration->genericArgumentPatterns.front().signatureReceiver =
          lang::ReceiverMutability::ReadOnly;
      changedQualifier = true;
      break;
    }
  }
  expect(changedQualifier && !lang::verifyLoweredProgram(lowered).empty(),
         "the lowered seal rejects erasing a signature receiver qualifier");

  const auto mismatch =
      lang::Frontend().analyze("qualified-pattern-mismatch.gti",
                               "class C<R(Args...) mut> { public: C() = default; }; C<int()> bad{};");
  expect(!mismatch.canGenerateCode() &&
             hasDiagnosticCode(mismatch.diagnostics, "GTI-S2023") &&
             !hasDiagnosticCode(mismatch.diagnostics, "GTI-B0001"),
         "read-only signatures cannot enter a mutable-pattern class");
}

void mutableSignatureCallback() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/mutable_signature_callback.gti";
  std::ifstream input(path);
  expect(input.good(), "mutable callback fixture exists");
  std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto valid =
      analyzeTrustedPreludeFixture("mutable-callback.gti", source);
  for (const auto &diagnostic : valid.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(valid.canGenerateCode(), "qualified patterns compose with retained "
                                  "mutable targets and interface dispatch");
  if (valid.canGenerateCode()) {
    expect(!gti_test::emitCpp(valid).contents.empty(),
           "mutable targets with generated construction and source cleanup "
           "must have a complete MIR emission path");
    bool checkedTransfer = false;
    for (const auto &function : valid.mir.functionInstances()) {
      if (checkedTransfer)
        break;
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::CallInput ||
              instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
              !instruction.preparedParameterDrop || !instruction.result ||
              instruction.operands.size() != 1 ||
              instruction.operands.front().kind != lang::MirOperandKind::Value)
            continue;
          const auto *prepared = function.body.findDropObligation(
              *instruction.preparedParameterDrop);
          const bool freshConstruction = std::any_of(
              function.body.blocks.begin(), function.body.blocks.end(),
              [&](const auto &sourceBlock) {
                return std::any_of(
                    sourceBlock.instructions.begin(),
                    sourceBlock.instructions.end(), [&](const auto &producer) {
                      return producer.result ==
                                 instruction.operands.front().value &&
                             (producer.kind ==
                                  lang::MirInstructionKind::Construct ||
                              (producer.kind ==
                                   lang::MirInstructionKind::Call &&
                               !producer.functionTarget && !producer.receiver &&
                               producer.operands.empty()));
                    });
              });
          if (prepared == nullptr ||
              !prepared->dropType.requiresActiveCleanup || !freshConstruction)
            continue;
          const lang::MirInstruction *invocation = nullptr;
          for (const auto &candidateBlock : function.body.blocks)
            for (const auto &candidate : candidateBlock.instructions)
              if ((candidate.kind == lang::MirInstructionKind::Call ||
                   candidate.kind == lang::MirInstructionKind::Construct) &&
                  candidate.callSite == instruction.callSite &&
                  std::any_of(
                      candidate.operands.begin(), candidate.operands.end(),
                      [&](const auto &operand) {
                        return operand.kind == lang::MirOperandKind::Value &&
                               operand.value == *instruction.result;
                      }) &&
                  std::count_if(
                      candidate.lifecycle.begin(), candidate.lifecycle.end(),
                      [&](const auto &event) {
                        return event.kind ==
                                   lang::MirLifecycleEventKind::TransferOut &&
                               event.source ==
                                   *instruction.preparedParameterDrop;
                      }) == 1)
                invocation = &candidate;
          if (invocation == nullptr)
            continue;
          auto forged = valid.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          for (auto &changedBlock : body->blocks)
            for (auto &changed : changedBlock.instructions)
              if (changed.id == invocation->id)
                std::erase_if(changed.lifecycle, [&](const auto &event) {
                  return event.kind ==
                             lang::MirLifecycleEventKind::TransferOut &&
                         event.source == *instruction.preparedParameterDrop;
                });
          expect(
              !lang::verifyMirProgram(forged).valid(),
              "fresh constructor arguments cannot lose their cleanup transfer");
          checkedTransfer = true;
          break;
        }
        if (checkedTransfer)
          break;
      }
    }
    expect(checkedTransfer,
           "active mutable target has a verified argument transfer");
  }
  const auto declaration =
      source.find("mut MutableCallback<int(int) mut> callback");
  expect(declaration != std::string::npos,
         "immutable negative locates the wrapper binding");
  if (declaration == std::string::npos)
    return;
  source.erase(declaration, 4);
  const auto invalid =
      analyzeTrustedPreludeFixture("immutable-callback.gti", source);
  const auto receiverError = std::find_if(
      invalid.diagnostics.begin(), invalid.diagnostics.end(),
      [](const auto &diagnostic) {
        return diagnostic.phase == lang::DiagnosticPhase::Semantics &&
               diagnostic.message.find("requires a mutable receiver") !=
                   std::string::npos;
      });
  expect(!invalid.canGenerateCode() &&
             receiverError != invalid.diagnostics.end() &&
             !hasDiagnosticCode(invalid.diagnostics, "GTI-B0001"),
         "a mutable callback cannot be invoked through a read-only wrapper");
}

void invalidSignatures() {
  const std::vector<std::string> cases = {
      "using Bad = void(void);",
      "using A = void(); using Bad = A(int);",
      "using Bad = int[2]();",
      "using A = void(); A value;",
      "using A = void(); struct Box { A value; };",
      "using A = void(); void use(A value) {}",
      "using A = void(); A make() {}",
      "using A = void(); using Bad = A*;",
      "using A = void(); using Bad = A[2];",
      "using A = void(); void use(A& value) {}",
  };
  for (const auto &source : cases) {
    const auto frontend = lang::Frontend().analyze("bad-signature.gti", source);
    if (!hasDiagnosticCode(frontend.diagnostics, "GTI-S2093")) {
      for (const auto &diagnostic : frontend.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      }
    }
    expect(!frontend.canGenerateCode() &&
               hasDiagnosticCode(frontend.diagnostics, "GTI-S2093") &&
               !hasDiagnosticCode(frontend.diagnostics, "GTI-B0001"),
           "invalid signature use must fail in semantics: " + source);
  }
  // The prelude's expected is an ordinary generic class: storing a signature
  // as its success alternative fails at that payload, as for any user class.
  const auto signatureExpected = analyzeTrustedPreludeFixture(
      "signature-expected.gti",
      "using A = void(); int main() { expected<A, int> bad = unexpected(1); "
      "return 0; }");
  expect(!signatureExpected.canGenerateCode() &&
             hasDiagnosticCode(signatureExpected.diagnostics, "GTI-S2067") &&
             !hasDiagnosticCode(signatureExpected.diagnostics, "GTI-B0001"),
         "a signature cannot become an expected payload");
  // Instantiating an ordinary generic value constructor with a signature
  // must fail too; the existing construction diagnostic owns T{}.
  const auto genericStorage = lang::Frontend().analyze(
      "signature-field.gti", "class Box<T> { T value{};  public: Box() = default; }; using A = void(); "
                             "int main() { Box<A> bad{}; return 0; }");
  expect(!genericStorage.canGenerateCode() &&
             hasDiagnosticCode(genericStorage.diagnostics, "GTI-S2038") &&
             !hasDiagnosticCode(genericStorage.diagnostics, "GTI-B0001"),
         "generic signature storage must fail before backend entry");

  const std::string invalidParameter = "using Bad = void(void);";
  const auto invalid =
      lang::Frontend().analyze("signature-span.gti", invalidParameter);
  const auto diagnostic =
      std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                   [](const auto &item) { return item.code == "GTI-S2093"; });
  expect(diagnostic != invalid.diagnostics.end() &&
             diagnostic->phase == lang::DiagnosticPhase::Semantics &&
             diagnostic->primary.start == invalidParameter.rfind("void") &&
             diagnostic->primary.end == invalidParameter.rfind("void") + 4,
         "signature diagnostics must point to the invalid component");
}

void signatureDeduction() {
  const std::string source = R"(
class Tag<S = void> {
public:
  using type = S;
  int value() { return 7; }
public:
  Tag() = default;
};
int describe<R, A>(Tag<R(A)>& tag) { return tag.value(); }
int describe_refs<R, A>(Tag<R&(A&)>& tag) { return tag.value(); }
int describe_empty<R>(Tag<R()>& tag) { return tag.value(); }
int metadata<T = void>() { Tag<T> tag{}; return tag.value(); }
int variadic<R, Args...>(Tag<R()>& tag, Args... values) {
  return tag.value();
}
T* pointer_identity<T>(T* pointer) { return pointer; }
int main() {
  Tag<void(int)> action{};
  Tag<bool(int)> predicate{};
  Tag<void()> empty{};
  Tag<int&(int&)> borrowing{};
  Tag defaulted{};
  void* pointer = nullptr;
  void* copied = pointer_identity(pointer);
  return describe(action) + describe(predicate) + describe(borrowing) +
         describe_empty(empty) + metadata<void>() + metadata() +
         variadic(empty, 1, true) + defaulted.value() - 56;
}
)";
  const auto frontend =
      lang::Frontend().analyze("signature-deduction.gti", source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "generic code must decompose signatures including void results");
  if (frontend.canGenerateCode()) {
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "signature deduction must preserve valid MIR");
    const auto inferred = std::find_if(
        frontend.hir.functionInstances().begin(),
        frontend.hir.functionInstances().end(), [](const auto &instance) {
          return instance.source &&
                 instance.source->name().lexeme == "describe" &&
                 instance.typeArguments ==
                     std::vector<lang::SemanticType>{lang::SemanticType::Void,
                                                     lang::SemanticType::Int32};
        });
    expect(inferred != frontend.hir.functionInstances().end(),
           "signature components must be selected by semantics before HIR");
    const auto pointer = std::find_if(
        frontend.mir.functionInstances().begin(),
        frontend.mir.functionInstances().end(), [](const auto &instance) {
          return instance.returnType.kind == lang::SemanticType::RawPointer &&
                 instance.returnType.arguments ==
                     std::vector<lang::SemanticType>{lang::SemanticType::Void};
        });
    expect(pointer != frontend.mir.functionInstances().end() &&
               !pointer->mayRaiseDefinedFailure,
           "copying an opaque void pointer cannot introduce defined failure");
    const std::string declarations =
        source.substr(0, source.find("int main()"));
    const std::vector<std::string> bodies = {
        "Tag<void(int)> tag{}; return describe(tag) - 7;",
        "Tag<bool(int)> tag{}; return describe(tag) - 7;",
        "Tag<int&(int&)> tag{}; return describe(tag) - 7;",
        "Tag<int&(int&)> tag{}; return describe_refs(tag) - 7;",
        "Tag<void()> tag{}; return describe_empty(tag) - 7;",
        "return metadata<void>() - 7;",
        "return metadata() - 7;",
        "Tag<void()> tag{}; return variadic(tag, 1, true) - 7;",
        "Tag tag{}; return tag.value() - 7;",
        "void* pointer = nullptr; void* copied = pointer_identity(pointer); "
        "return 0;",
    };
    for (const auto &body : bodies) {
      try {
        expect(!emitAnalyzedSource(declarations + "int main() {" + body + "}")
                    .empty(),
               "void metadata must have a complete lowered representation");
      } catch (const std::logic_error &error) {
        expect(false,
               "signature deduction emission: " + body + "\n" + error.what());
      }
    }
  }

  const std::vector<std::string> invalid = {
      "class Box<T> { T field{};  public: Box() = default; }; int main() { Box<void> box{}; return 0; }",
      "void local<T>() { mut T value; } int main() { local<void>(); return 0; "
      "}",
      "void parameter<T>(T value) {} int main() { parameter<void>(1); return "
      "0; }",
      "void empty() {} void parameter<T>(T value) {} int main() { "
      "parameter<void>(empty()); return 0; }",
      "class Box<T> { T[2] field = {};  public: Box() = default; }; int main() { Box<void> box{}; return "
      "0; }",
      "class Box<T> { T& field; }; int main() { Box<void> box{}; return 0; }",
      "class Box<T> { using Bad = int(T);  public: Box() = default; }; int main() { Box<void> box{}; "
      "return 0; }",
      "class Box<T> { using Bad = T[2];  public: Box() = default; }; int main() { Box<void> box{}; "
      "return 0; }",
      "class Box<T> { using Bad = T&(T&);  public: Box() = default; }; int main() { Box<void> box{}; "
      "return 0; }",
      "int main() { void* pointer = nullptr; unsafe { pointer + 1; } return 0; "
      "}",
      "class Tag<S> { public: Tag() = default; }; int consistent<T>(Tag<T(T)>& tag) { return 0; } "
      "int main() { Tag<int(bool)> tag{}; return consistent(tag); }",
      "class Tag<S> { public: Tag() = default; }; int fixed<R, A>(Tag<R(A)>& tag) { return 0; } "
      "int main() { Tag<int(int, int)> tag{}; return fixed(tag); }",
  };
  for (const auto &input : invalid) {
    const auto result = lang::Frontend().analyze("void-storage.gti", input);
    expect(!result.canGenerateCode() &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "void remains invalid in object positions after substitution: " +
               input);
  }
  const std::string invalidAlias = "class Box<T> { using Bad = int(T);  public: Box() = default; }; "
                                   "int main() { Box<void> box{}; return 0; }";
  const auto alias =
      lang::Frontend().analyze("void-alias-span.gti", invalidAlias);
  const auto error = std::find_if(
      alias.diagnostics.begin(), alias.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2093"; });
  expect(error != alias.diagnostics.end() &&
             error->phase == lang::DiagnosticPhase::Semantics &&
             error->primary.start == invalidAlias.find("int(T)") + 4 &&
             error->primary.end == invalidAlias.find("int(T)") + 5 &&
             !error->related.empty(),
         "a concrete alias diagnostic must identify its component and "
         "instantiation");
}

void classTypePacks() {
  const std::string source = R"(
class Signature<R, Args...> {
public:
  using type = R(Args...);
  int value() { return 7; }
public:
  Signature() = default;
};
class Forward<R, Args...> {
  using nested = Signature<R, Args...>;
  nested stored{};
public:
  int value() { return stored.value(); }
public:
  Forward() = default;
};
class Types<Args...> {
public:
  int value() { return 7; }
public:
  Types() = default;
};
int main() {
  Signature<void> empty{};
  Signature<int, int, bool> binary{};
  Signature<int&, int&> borrowing{};
  Forward<void, int, bool> forwarded{};
  Forward<void> forwarded_empty{};
  Types<> no_types{};
  auto constructed = Types<>();
  Types<void, int&> metadata{};
  return empty.value() + binary.value() + borrowing.value() +
         forwarded.value() + forwarded_empty.value() + no_types.value() +
         metadata.value() - 49;
}
)";
  const auto frontend =
      lang::Frontend().analyze("class-type-packs.gti", source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "class packs must form signatures and forward exact type lists");
  if (frontend.canGenerateCode()) {
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "class-pack instances must have valid concrete MIR");
    expect(!emitAnalyzedSource(source).empty(),
           "class-pack metadata must lower without a backend type system");
    int emptyTypes = 0;
    int referenceSignatures = 0;
    for (const auto &instance : frontend.hir.classInstances()) {
      if (instance.source == nullptr) {
        continue;
      }
      if (instance.source->name().lexeme == "Types" &&
          instance.typeArguments.empty()) {
        ++emptyTypes;
      }
      if (instance.source->name().lexeme == "Signature" &&
          instance.typeArguments.size() == 2 &&
          instance.typeArguments.front().kind ==
              lang::SemanticType::Reference) {
        ++referenceSignatures;
        expect(!instance.traits.containsBorrowedState,
               "reference metadata must not introduce a stored borrow");
      }
      expect(std::none_of(
                 instance.typeArguments.begin(), instance.typeArguments.end(),
                 [](const auto &argument) {
                   return argument.kind == lang::SemanticType::TypePack;
                 }),
             "concrete class identities must flatten packs before HIR");
    }
    expect(emptyTypes == 1 && referenceSignatures == 1,
           "empty constructors must share identity and reference metadata must "
           "remain exact");
    const std::string formatted = lang::Formatter().format(source);
    expect(lang::Formatter().format(formatted) == formatted &&
               lang::Frontend()
                   .analyze("formatted-packs.gti", formatted)
                   .canGenerateCode(),
           "class type-list formatting must preserve syntax and be idempotent");
  }
}

void invalidClassTypePacks() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"class Bad<Args..., T> { public: Bad() = default; };", "GTI-S2023"},
      {"class Bad<Args..., More...> { public: Bad() = default; };", "GTI-S2023"},
      {"class Bad<uint64_t N, Args...> { public: Bad() = default; };", "GTI-S2023"},
      {"class Bad<T = int, Args...> { public: Bad() = default; };", "GTI-S2023"},
      {"class Bad<Args...> { Args field; };", "GTI-S2023"},
      {"class Bad<T> { using type = void(T...);  public: Bad() = default; };", "GTI-S2023"},
      {"class Bad<Args...> { using type = void(Args&...);  public: Bad() = default; };", "GTI-S2023"},
      {"class Bad<Args...> { using type = void(Args..., int);  public: Bad() = default; };", "GTI-S2023"},
      {"class Fixed<T> { public: Fixed() = default; }; class Bad<Args...> { using type = Fixed<Args...>; "
       " public: Bad() = default; };",
       "GTI-S2023"},
      {"class Fixed<T> { public: Fixed() = default; }; void bad<Args...>(Fixed<Args...> value, Args... "
       "args) {}",
       "GTI-S2023"},
      {"class Pack<T, Args...> { public: Pack() = default; }; class Bad<Args...> { using type = "
       "Pack<Args...>;  public: Bad() = default; };",
       "GTI-S2023"},
      {"class Pack<Args...> { public: Pack() = default; }; class Bad<Args...> { using type = "
       "Pack<Args..., int>;  public: Bad() = default; };",
       "GTI-S2023"},
      {"class Pack<T, Args...> { public: Pack() = default; }; int main() { Pack<> value{}; return 0; }",
       "GTI-S2023"},
      {"class Pack<Args...> { using type = void(Args...);  public: Pack() = default; }; int main() { "
       "Pack<void> value{}; return 0; }",
       "GTI-S2093"},
      {"class Pack<Args...> { using type = void(Args...);  public: Pack() = default; }; int main() { "
       "Pack<void()> value{}; return 0; }",
       "GTI-S2093"},
      {"class Pack<Args...> { public: Pack() = default; }; int main() { Pack<void&> value{}; return 0; }",
       "GTI-S2017"},
      {"class Pack<Args...> { public: Pack() = default; }; int main() { Pack<int[2]&> value{}; return "
       "0; }",
       "GTI-S2017"},
      {"class Box<T> { T field{};  public: Box() = default; }; int main() { Box<int&> value{}; return 0; "
       "}",
       "GTI-S2045"},
      {"class Box<T> { using Bad = T&(T&);  public: Box() = default; }; int main() { Box<int&> value{}; "
       "return 0; }",
       "GTI-S2017"},
      {"void bad<T>() { mut T local; } int main() { bad<int&>(); return 0; }",
       // Uniform local initialization rejects the generic definition before
       // substituting T = int&. Concrete uninitialized references retain their
       // dedicated reference diagnostic below.
       "GTI-S2000"},
      {"int main() { mut int& local; return 0; }", "GTI-S2017"},
  };
  for (const auto &[source, code] : cases) {
    const auto result =
        lang::Frontend().analyze("invalid-class-pack.gti", source);
    if (result.canGenerateCode() ||
        !hasDiagnosticCode(result.diagnostics, code)) {
      for (const auto &diagnostic : result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      }
    }
    expect(!result.canGenerateCode() &&
               hasDiagnosticCode(result.diagnostics, code) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "invalid class pack must be rejected by semantics: " + source);
  }
}

void signaturePackDeduction() {
  const std::string source = R"(
class Tag<S> { public: Tag() = default; };
class Types<Args...> { public: int value() { return 7; }  public: Types() = default; };
int unpack<R, Args...>(Tag<R(Args...)>& tag) {
  Types<Args...> types{};
  return types.value();
}
int relay<R, Args...>(Tag<R(Args...)>& tag) { return unpack(tag); }
int repeated<R, Args...>(Tag<R(Args...)>& tag, Tag<void(Args...)>& same) { return 7; }
Types<Args...> extract<R, Args...>(Tag<R(Args...)>& tag) { return Types<Args...>(); }
int nominal<Args...>(Types<Args...>& types) { return types.value(); }
int prefix<R, Args...>(Tag<R(int, Args...)>& tag) { return 7; }
int with_values<R, Args...>(Tag<R(Args...)>& tag, Args... args) { return 7; }
int main() {
  Tag<void()> empty{};
  Tag<int(int, bool)> pair{};
  Tag<void(int, bool)> action{};
  Tag<int&(int&)> borrow{};
  auto types = extract(pair);
  return unpack(empty) + unpack(pair) + unpack(borrow) + relay(pair) +
         repeated(pair, action) + unpack<int, int, bool>(pair) + types.value() +
         nominal(types) + prefix(pair) + with_values(pair, 1, true) - 70;
}
)";
  const auto frontend =
      lang::Frontend().analyze("signature-pack-deduction.gti", source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "signature metadata must deduce the exact trailing type pack");
  if (frontend.canGenerateCode()) {
    expect(lang::verifyMirProgram(frontend.mir).valid() &&
               !emitAnalyzedSource(source).empty(),
           "deduced signature packs must preserve selected instances through "
           "MIR and emission");
    std::size_t unpackInstances = 0;
    bool empty = false;
    bool pair = false;
    bool reference = false;
    for (const auto &function : frontend.hir.functionInstances()) {
      if (function.source == nullptr ||
          function.source->name().lexeme != "unpack") {
        continue;
      }
      ++unpackInstances;
      const auto &types = function.typeArguments;
      empty = empty || types == std::vector<lang::SemanticType>{
                                    lang::SemanticType::Void};
      pair = pair ||
             types == std::vector<lang::SemanticType>{lang::SemanticType::Int32,
                                                      lang::SemanticType::Int32,
                                                      lang::SemanticType::Bool};
      reference =
          reference || (types.size() == 2 &&
                        types[0].kind == lang::SemanticType::Reference &&
                        types[1].kind == lang::SemanticType::Reference);
    }
    expect(unpackInstances == 3 && empty && pair && reference,
           "explicit and inferred signature arguments must share one exact "
           "concrete HIR identity");
  }
  const std::string declaration =
      "class Tag<S> { public: Tag() = default; }; class Types<Args...> { public: Types() = default; }; "
      "int unpack<R, Args...>(Tag<R(Args...)>& tag) { return 0; } "
      "int repeated<R, Args...>(Tag<R(Args...)>& first, Tag<void(Args...)>& "
      "second) { return 0; } "
      "int with_values<R, Args...>(Tag<R(Args...)>& tag, Args... args) { "
      "return 0; } ";
  const std::vector<std::string> invalid = {
      "int main() { Tag<int(int)> a{}; Tag<void(bool)> b{}; return repeated(a, "
      "b); }",
      "int main() { Tag<int()> a{}; Tag<void(int)> b{}; return repeated(a, b); "
      "}",
      "int main() { Tag<int(int)> a{}; return unpack<int, bool>(a); }",
      "int main() { Tag<int(int)> a{}; return with_values(a, true); }",
      "int main() { Tag<int(int&)> a{}; return with_values(a, 1); }",
      "int main() { Tag<int> a{}; return unpack(a); }",
      "int main() { Tag<int(int)> a{}; return unpack(a, 1); }",
  };
  for (const auto &body : invalid) {
    const auto result = lang::Frontend().analyze("invalid-signature-pack.gti",
                                                 declaration + body);
    expect(!result.canGenerateCode() &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "signature pack deduction must reject conflicting or mismatched "
           "exact types: " +
               body);
  }
  const std::string mismatch = declaration + invalid[2];
  const auto error =
      lang::Frontend().analyze("signature-pack-conflict.gti", mismatch);
  const auto diagnostic = std::find_if(
      error.diagnostics.begin(), error.diagnostics.end(),
      [](const auto &value) {
        return value.code == "GTI-S2023" &&
               value.message.find("one exact type pack") != std::string::npos;
      });
  expect(diagnostic != error.diagnostics.end() &&
             diagnostic->primary.start == mismatch.rfind("(a)") + 2 &&
             diagnostic->primary.end == diagnostic->primary.start + 1 &&
             diagnostic->related.size() == 1 &&
             diagnostic->related.front().span.start ==
                 mismatch.find("Args...>(Tag") &&
             diagnostic->fixes.empty(),
         "failed pack deduction must identify the call and declared pack "
         "without a guessed fix");
  const std::string constrained =
      "class Types<Args...> { public: Types() = default; }; "
      "int integral<std::integral Args...>(Types<Args...>& types) { return 0; "
      "} ";
  const auto validConstraint = lang::Frontend().analyze(
      "constrained-signature-pack.gti",
      constrained + "int main() { Types<int, uint64_t> types{}; Types<> "
                    "empty{}; return integral(types) + integral(empty); }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const auto invalidConstraint = lang::Frontend().analyze(
      "invalid-constrained-pack.gti",
      constrained +
          "int main() { Types<int, bool> types{}; return integral(types); }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(validConstraint.canGenerateCode() &&
             !invalidConstraint.canGenerateCode() &&
             hasDiagnosticCode(invalidConstraint.diagnostics, "GTI-S2029"),
         "metadata-deduced pack constraints must check every element and admit "
         "an empty pack");
  const auto ambiguous =
      lang::Frontend().analyze("ambiguous-signature-pack.gti", R"(
class Tag<S> { public: Tag() = default; };
int pick<S>(Tag<S>& tag) { return 1; }
int pick<R, Args...>(Tag<R(Args...)>& tag) { return 2; }
int main() { Tag<void(int)> tag{}; return pick(tag); }
)");
  expect(!ambiguous.canGenerateCode() &&
             hasDiagnostic(ambiguous.diagnostics, "ambiguous"),
         "signature pack deduction must not introduce specialization ranking "
         "between exact overloads");
  const auto unbound = lang::Frontend().analyze(
      "unbound-metadata-pack.gti",
      "class Types<Args...> { public: Types() = default; }; Types<Args...> unbound<Args...>() { return "
      "Types<Args...>(); }");
  expect(
      !unbound.canGenerateCode() &&
          hasDiagnosticCode(unbound.diagnostics, "GTI-S2023"),
      "a return-only type pack must not invent a deduced empty argument list");
}

void classParameterPacks() {
  const std::string source = R"(
void consume<Values...>(Values... values) {}
class Relay<Args...> {
public:
  void send(Args... args) { consume(args...); }
  int inspect<T>(T first, Args... args) { consume(first, args...); return 7; }
  void twice(Args... args) { send(args...); send(args...); }
  int operator()(Args... args) { consume(args...); return 7; }
public:
  Relay() = default;
};
interface Invocable<Args...> { int invoke(Args... args); };
class Target<Args...> : public Invocable<Args...> {
public:
  int invoke(Args... args) override { consume(args...); return 7; }
public:
  Target() = default;
};
int dispatch(Invocable<int, bool>& target) { return target.invoke(1, true); }
int main() {
  Relay<> empty{};
  Relay<int, bool> pair{};
  Target<int, bool> target{};
  empty.send();
  pair.send(1, true);
  pair.twice(2, false);
  return empty.inspect(1) + pair.inspect(1, 2, true) + empty() + pair(1, true) + dispatch(target) - 35;
}
)";
  const auto frontend =
      lang::Frontend().analyze("class-parameter-packs.gti", source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << " at " << diagnostic.primary.start << ": "
              << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "methods must bind their owner's exact argument pack");
  if (!frontend.canGenerateCode()) {
    std::cerr << "stages semantic=" << frontend.semanticValid
              << " hir=" << frontend.hirValid
              << " failures=" << frontend.failureMetadataValid
              << " mir=" << frontend.mirValid << '\n';
  }
  if (frontend.canGenerateCode()) {
    expect(lang::verifyMirProgram(frontend.mir).valid(),
           "class-owned parameter packs must have verified MIR");
    expect(!emitAnalyzedSource(source).empty(),
           "owner-pack methods must emit from the verified MIR body");
    bool mutatedSource = false;
    bool mutatedDeclaration = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (!function.owner) {
        continue;
      }
      expect(
          std::none_of(function.parameterTypes.begin(),
                       function.parameterTypes.end(),
                       [](const auto &type) {
                         return type.kind == lang::SemanticType::TypePack;
                       }),
          "concrete owner method signatures must not depend on pack grouping");
      auto forged = frontend.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      auto &body = functions[function.id - 1].body;
      for (auto &place : body.places) {
        if (place.type.kind == lang::SemanticType::TypePack &&
            place.type.concretePack && place.type.arguments.size() == 2) {
          place.type.arguments.pop_back();
          expect(!lang::verifyMirProgram(forged).valid(),
                 "MIR must reject an owner pack that loses an argument");
          if (function.pureVirtual) {
            mutatedDeclaration = true;
          } else {
            mutatedSource = true;
          }
          break;
        }
      }
      if (mutatedSource && mutatedDeclaration) {
        break;
      }
    }
    expect(mutatedSource && mutatedDeclaration,
           "owner-pack verification must exercise source and interface pack "
           "places");
  }
  const std::string declaration =
      "class Relay<Args...> { public: "
      "void send(Args... args) {} "
      "int operator()(Args... args) { return 0; }  public: Relay() = default; }; ";
  expect(
      lang::Frontend()
          .analyze("borrowed-owner-pack.gti",
                   declaration +
                       "int main() { Relay<int&> value{}; int number = 1; "
                       "value.send(number); return value(number); }")
          .canGenerateCode(),
      "read-only reference elements are valid concrete owner-pack parameters");
  const auto invalidElement = lang::Frontend().analyze(
      "invalid-borrow-pack.gti",
      declaration + "int main() { Relay<void> value{}; return 0; }");
  const auto packError = std::find_if(
      invalidElement.diagnostics.begin(), invalidElement.diagnostics.end(),
      [](const auto &diagnostic) {
        return diagnostic.code == "GTI-S2023" &&
               diagnostic.message.find("or references") != std::string::npos;
      });
  expect(
      packError != invalidElement.diagnostics.end(),
      "invalid pack elements explain the supported value/reference boundary");
  if (packError != invalidElement.diagnostics.end()) {
    const auto ellipsis = declaration.find("Args... args") + 4;
    expect(
        packError->primary.start == ellipsis &&
            packError->primary.end == ellipsis + 3 &&
            packError->phase == lang::DiagnosticPhase::Semantics &&
            packError->primary.source ==
                std::filesystem::absolute("invalid-borrow-pack.gti").string() &&
            packError->fixes.empty(),
        "pack element diagnostic anchors the declaration ellipsis without "
        "changing borrow intent");
  }
  const std::vector<std::pair<std::string, std::string>> invalid = {
      {declaration +
           "int main() { Relay<int, bool> value{}; value.send(1); return 0; }",
       "GTI-S2005"},
      {declaration + "int main() { Relay<> value{}; value.send(1); return 0; }",
       "GTI-S2005"},
      {declaration + "int main() { Relay<int, bool> value{}; value.send(true, "
                     "1); return 0; }",
       "GTI-S2003"},
      {declaration +
           "int main() { Relay<int, bool> value{}; return value(true, 1); }",
       "GTI-S2022"},
      {declaration + "int main() { Relay<void> value{}; return 0; }",
       "GTI-S2023"},
      {declaration + "int main() { Relay<void()> value{}; return 0; }",
       "GTI-S2023"},
      {"class Relay<Args...> { void send(mut Args... args) {}  public: Relay() = default; };", "GTI-S2023"},
      {"class Relay<Args...> { void send(Args... args, int last) {}  public: Relay() = default; };",
       "GTI-S2023"},
      {"class Token { public: ~Token() {}  public: Token() = default; }; "
       "void consume<T...>(T... values) {} "
       "class Relay<Args...> { public: void twice(Args... args) { "
       "consume(args...); consume(args...); }  public: Relay() = default; }; "
       "int main() { Relay<Token> relay{}; relay.twice(Token()); return 0; }",
       "GTI-S2018"},
  };
  for (const auto &[input, code] : invalid) {
    const auto result =
        lang::Frontend().analyze("invalid-owner-pack.gti", input);
    if (result.canGenerateCode() ||
        !hasDiagnosticCode(result.diagnostics, code)) {
      for (const auto &diagnostic : result.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      }
    }
    expect(!result.canGenerateCode() &&
               hasDiagnosticCode(result.diagnostics, code) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "owner-pack errors must be frontend diagnostics: " + input);
  }
}

void signatureReceiverFamily() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/signature_receiver_family.gti";
  std::ifstream input(path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  expect(!source.empty(), "receiver family fixture must exist");
  const auto result = lang::Frontend().analyze(path.string(), source);
  for (const auto &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(result.canGenerateCode(),
         "disjoint signature receivers select distinct definitions");
  if (result.canGenerateCode()) {
    expect(!gti_test::emitCpp(result).contents.empty(),
           "receiver family must emit from verified MIR");
    auto lowered = gti_test::lowerProgram(result, result.mir, result.mir);
    bool changed = false;
    for (auto &row : const_cast<std::vector<lang::LoweredDeclaration> &>(
             lowered.declarations())) {
      auto *declaration =
          std::get_if<lang::LoweredClassDeclaration>(&row.payload);
      if (declaration && declaration->exactSpecializationPrimary != 0) {
        declaration->exactTypeArguments.clear();
        changed = true;
        break;
      }
    }
    expect(changed && !lang::verifyLoweredProgram(lowered).empty(),
           "exact patterned specializations cannot lose their public argument");
  }
  for (const auto &invalid :
       {"class C<R(Args...)> { public: C() = default; }; class C<T(Ts...)> {};",
        "class C<R(Args...) mut> { public: C() = default; }; class C<T()> { public: C() = default; }; class C<T() mut> {};",
        "class C<T> { public: C() = default; }; class C<R(Args...) mut> {};",
        "class C<R(Args...)> { public: C() = default; }; struct C<R(Args...) mut> {};"}) {
    const auto rejected =
        lang::Frontend().analyze("duplicate-pattern.gti", invalid);
    expect(!rejected.canGenerateCode() &&
               hasDiagnosticCode(rejected.diagnostics, "GTI-S2006") &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
           "signature definitions must be disjoint, same-kind patterns");
  }
  for (const auto &invalid :
       {"class C<R(Args...)> { public: C() = default; }; class C<R(Args...) mut> { public: C() = default; }; C<int() &&> "
        "bad{};",
        "class C<R(R)> { public: C() = default; }; class C<R(R) mut> { public: C() = default; }; C<int(bool) mut> bad{};",
        "class C<R(Args...)> { public: C() = default; }; class D<T> { C<T() mut> bad{};  public: D() = default; };"}) {
    const auto rejected =
        lang::Frontend().analyze("unmatched-pattern.gti", invalid);
    expect(!rejected.canGenerateCode() &&
               hasDiagnosticCode(rejected.diagnostics, "GTI-S2023") &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
           "signature families must retain exact matching without conversions");
  }
  const auto root = std::filesystem::weakly_canonical(
                        std::filesystem::temp_directory_path()) /
                    "gti-receiver-family";
  const std::string mismatch =
      "class C<R(Args...)> { public: C() = default; }; class C<R(R) mut> { public: C() = default; }; C<int(bool) mut> bad{};";
  const auto mismatchResult =
      lang::Frontend().analyze("family-mismatch.gti", mismatch);
  const auto mismatchDiagnostic = std::find_if(
      mismatchResult.diagnostics.begin(), mismatchResult.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2023"; });
  expect(mismatchDiagnostic != mismatchResult.diagnostics.end() &&
             mismatchDiagnostic->primary.start == mismatch.find("C<int") &&
             mismatchDiagnostic->primary.end == mismatch.find("C<int") + 1 &&
             mismatchDiagnostic->primary.source ==
                 std::filesystem::absolute("family-mismatch.gti").string() &&
             mismatchDiagnostic->related.size() == 1 &&
             mismatchDiagnostic->related.front().span.start ==
                 mismatch.find("C<R(R)") &&
             mismatchDiagnostic->fixes.empty(),
         "a component mismatch relates the selected receiver definition "
         "without guessing a fix");
  std::unordered_map<std::string, std::string> overlays{
      {(root / "read.gti").string(), "class C<R(Args...)> { public: C() = default; };"},
      {(root / "mut.gti").string(), "class C<R(Args...) mut> { public: C() = default; };"}};
  const std::string entry = "#include \"read.gti\"\n#include \"mut.gti\"\n"
                            "int main() { C<int() mut> value{}; return 0; }";
  const auto included =
      lang::Frontend().analyze(root / "main.gti", entry, {}, overlays);
  for (const auto &diagnostic : included.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(included.canGenerateCode(),
         "included same-package receiver definitions compose");
  overlays[(root / "read.gti").string()] +=
      " int hidden() { C<int() mut> invisible{}; return 0; }";
  const auto hidden =
      lang::Frontend().analyze(root / "main.gti", entry, {}, overlays);
  expect(!hidden.canGenerateCode() &&
             !hasDiagnosticCode(hidden.diagnostics, "GTI-B0001"),
         "an entry's sibling include must not expose a variant inside another "
         "source unit");
}

void mutableSignatureArguments() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owning_function_mutable_arguments.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  expect(!source.empty(), "mutable-reference callback fixture exists");
  const auto result =
      analyzeTrustedPreludeFixture("mutable-arguments.gti", source);
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(),
         "mutable reference packs reach verified MIR");
  if (result.canGenerateCode()) {
    expect(!gti_test::emitCpp(result).contents.empty(),
           "mutable arguments emit through MIR");
    bool checked = false;
    for (const auto &function : result.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        const auto *place = function.body.findPlace(loan.source);
        if (!loan.entry || place == nullptr || place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        expect(
            loan.access == lang::AccessMode::Mutable && loan.semanticLoan != 0,
            "pack reference entries retain semantic identity and exact access");
        auto forged = result.mir;
        auto *body = lang::findMirBody(
            forged, {lang::MirBodyKind::Function, function.id});
        body->loans[loan.id - 1].access = lang::AccessMode::ReadOnly;
        expect(!lang::verifyMirProgram(forged).valid(),
               "pack entry access cannot be downgraded");
        forged = result.mir;
        body = lang::findMirBody(forged,
                                 {lang::MirBodyKind::Function, function.id});
        body->loans[loan.id - 1].escapes = true;
        expect(!lang::verifyMirProgram(forged).valid(),
               "pack entry capability cannot escape its invocation");
        forged = result.mir;
        body = lang::findMirBody(forged,
                                 {lang::MirBodyKind::Function, function.id});
        auto duplicate = loan;
        duplicate.id = body->loans.size() + 1;
        duplicate.semanticLoan = 1000000;
        body->loans.push_back(duplicate);
        expect(
            !lang::verifyMirProgram(forged).valid(),
            "each mutable reference element has exactly one entry capability");
        checked = true;
        break;
      }
      if (checked)
        break;
    }
    expect(checked, "mutable callback contains an element entry loan");
  }
  for (const std::string body :
       {"void change(mut int& value) {} int main() { std::function<void(mut "
        "int&)> fn{change}; int x = 0; fn(x); return 0; }",
        "void change(mut int& value) {} int main() { std::function<void(mut "
        "int&)> fn{change}; fn(1); return 0; }",
        "void change(mut int& left, mut int& right) {} int main() { "
        "std::function<void(mut int&, mut int&)> fn{change}; mut int x = 0; "
        "fn(x, x); return 0; }",
        "void change(mut int& left, int& right) {} int main() { "
        "std::function<void(mut int&, int&)> fn{change}; mut int x = 0; fn(x, "
        "x); return 0; }",
        "void change(int& left, mut int& right) {} int main() { "
        "std::function<void(int&, mut int&)> fn{change}; mut int x = 0; fn(x, "
        "x); return 0; }",
        "void read(int& value) {} int main() { std::function<void(mut int&)> "
        "fn{read}; return 0; }"}) {
    const auto rejected = analyzeTrustedPreludeFixture(
        "invalid-mutable-arguments.gti", "#include <std/functional>\n" + body);
    expect(!rejected.canGenerateCode() &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001"),
           "mutable references reject immutable/rvalue inputs, aliasing and "
           "signature mismatch");
  }
  const std::string invalid = "using Bad = void(mut int);";
  const auto rejected =
      lang::Frontend().analyze("invalid-mutable-signature.gti", invalid);
  const auto error = std::find_if(
      rejected.diagnostics.begin(), rejected.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2093"; });
  expect(
      error != rejected.diagnostics.end() &&
          error->primary.start == invalid.find("mut") &&
          error->primary.end == invalid.find("mut") + 3 && error->fixes.empty(),
      "mut signature metadata requires a reference without guessing an edit");
  const auto failurePath =
      std::filesystem::path(__FILE__).parent_path() /
      "fixtures/owning_function_mutable_arguments_failure.gti";
  std::ifstream failureInput(failurePath);
  const std::string failureSource{std::istreambuf_iterator<char>(failureInput),
                                  {}};
  expect(!failureSource.empty(), "borrowed callback failure fixture exists");
  const auto failureResult = analyzeTrustedPreludeFixture(
      "mutable-arguments-failure.gti", failureSource);
  expect(failureResult.canGenerateCode(),
         "borrowed arguments preserve conditional-argument and failure cleanup "
         "paths");
  if (failureResult.canGenerateCode()) {
    expect(
        !gti_test::emitCpp(failureResult).contents.empty(),
        "peer borrowed arguments cross a scalar conditional before invocation");
  }
}

} // namespace

int main() {
  callableSignatureFormatting();
  classSignaturePatternSyntax();
  signatureReceiverFamily();
  mutableSignatureArguments();
  classSignaturePatternBinding();
  signatureTypes();
  receiverQualifiedSignatures();
  mutableSignatureCallback();
  invalidSignatures();
  signatureDeduction();
  classTypePacks();
  invalidClassTypePacks();
  signaturePackDeduction();
  classParameterPacks();
  return failures == 0 ? 0 : 1;
}
