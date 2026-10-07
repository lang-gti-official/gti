#include "compiler_test_fixture.h"

namespace {
void checkConsumingWrapperSemantics() {
  const std::string prefix =
      "#include <std/functional>\n#include <std/memory>\n";
  const auto positive =
      analyzeTrustedPreludeFixture("consuming-wrappers.gti", prefix + R"gti(
class Increment { public: int operator()(int value) && { return value + 1; } };
int main() {
  std::function<int(int) &&> callback{Increment()};
  auto copy = callback.clone();
  int first = std::move(callback)(2);
  int second = std::move(copy)(3);
  auto resource = std::make_unique<int>(5);
  std::move_only_function<int() &&> owned{
      [value = std::move(resource)]() -> int { return *value; }};
  return first + second + std::move(owned)() - 12;
}
)gti");
  expect(positive.canGenerateCode(),
         "public consuming signatures support explicit moves, clone and "
         "move-only targets");
  const std::vector<std::pair<std::string, std::string>> negatives{
      {"consuming invocation requires move",
       "std::function<int() &&> callback{[]() -> int { return 3; }}; int value "
       "= callback();"},
      {"second invocation is rejected",
       "std::function<int() &&> callback{[]() -> int { return 3; }}; int first "
       "= std::move(callback)(); int second = std::move(callback)();"},
      {"wrapper copy must be explicit clone",
       "std::function<int() &&> callback{[]() -> int { return 3; }}; auto copy "
       "= callback;"},
      {"move-only wrapper has no clone",
       "std::move_only_function<int() &&> callback{[]() -> int { return 3; }}; "
       "auto copy = callback.clone();"},
      {"copyable wrapper rejects move-only target",
       "auto resource = std::make_unique<int>(3); auto target = [value = "
       "std::move(resource)]() -> int { return *value; }; std::function<int() "
       "&&> callback{std::move(target)};"},
      {"target parameter types must match signature",
       "std::function<int(bool) &&> callback{[](int value) -> int { return "
       "value; }};"},
      {"target result must match signature",
       "std::function<int() &&> callback{[]() -> bool { return true; }};"},
      {"call argument types remain exact",
       "std::function<int(int) &&> callback{[](int value) -> int { return "
       "value; }}; int value = std::move(callback)(true);"},
      {"call result is not implicitly converted",
       "std::function<int() &&> callback{[]() -> int { return 3; }}; bool "
       "value = std::move(callback)();"},
  };
  for (const auto &[label, body] : negatives) {
    const auto result = analyzeTrustedPreludeFixture(
        "invalid-consuming-wrapper.gti",
        prefix + "int main() { " + body + " return 0; }");
    const std::string_view expectedCode =
        label == "target parameter types must match signature" ? "GTI-S2003"
        : label == "target result must match signature"        ? "GTI-S2046"
                                                               : "";
    // Concrete generic bodies are checked by semantic reanalysis requested
    // during HIR instance discovery; semanticValid records the earlier
    // symbolic pass. Require an actual semantic diagnostic before MIR.
    expect(result.syntaxValid && !result.canGenerateCode() &&
               (!result.semanticValid || !result.hirValid) &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&](const auto &diagnostic) {
                             return expectedCode.empty()
                                        ? diagnostic.code.starts_with("GTI-S")
                                        : diagnostic.code == expectedCode;
                           }) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           label);
  }
}
} // namespace

int main() {
  checkConsumingWrapperSemantics();
  const auto nominalPath = std::filesystem::path(__FILE__).parent_path() /
                           "fixtures/nested_nominal_owned_callback.gti";
  std::ifstream nominalInput(nominalPath);
  expect(nominalInput.good(), "owning nominal-result callback fixture exists");
  const std::string nominalSource{std::istreambuf_iterator<char>(nominalInput),
                                  {}};
  const auto nominal =
      analyzeTrustedPreludeFixture(nominalPath.string(), nominalSource);
  expect(nominal.canGenerateCode(),
         "a stored owning target may return a class value");
  if (nominal.canGenerateCode()) {
    const auto text = gti_test::emitCppText(nominal);
    const auto declaration =
        text.find("friend ::__gti_program::Packet __gti_invoke(const Target &");
    const auto definition =
        text.find("inline ::__gti_program::Packet __gti_invoke(const Target &");
    const auto layout = text.find("struct Packet {");
    expect(declaration != std::string::npos &&
               definition != std::string::npos && layout != std::string::npos &&
               layout < definition && declaration < definition,
           "the ADL adapter is declared in its owner and defined after the "
           "result layout");
    if (declaration != std::string::npos)
      expect(text.find(';', declaration) < text.find('{', declaration),
             "the in-class friend must not instantiate an incomplete class "
             "result");
  }
  const auto orderPath =
      nominalPath.parent_path() / "callable_adapter_declaration_order.gti";
  std::ifstream orderInput(orderPath);
  expect(orderInput.good(), "cyclic adapter/layout dependency fixture exists");
  const std::string orderSource{std::istreambuf_iterator<char>(orderInput), {}};
  const auto ordered =
      analyzeTrustedPreludeFixture(orderPath.string(), orderSource);
  expect(ordered.canGenerateCode(),
         "a callable may return a class containing that callable by value");
  if (ordered.canGenerateCode()) {
    const auto text = gti_test::emitCppText(ordered);
    const auto declaration =
        text.find("friend ::__gti_program::api::Packet __gti_invoke(");
    const auto layout = text.find("struct Packet {");
    const auto definition =
        text.find("inline ::__gti_program::api::Packet __gti_invoke(");
    expect(declaration != std::string::npos && layout != std::string::npos &&
               definition != std::string::npos && declaration < layout &&
               layout < definition,
           "adapter code must not create a false recursive layout dependency");
  }
  const auto mutablePath = std::filesystem::path(__FILE__).parent_path() /
                           "fixtures/owning_function_mutable.gti";
  std::ifstream mutableInput(mutablePath);
  const std::string mutableSource{std::istreambuf_iterator<char>(mutableInput),
                                  {}};
  expect(!mutableSource.empty(), "mutable public wrapper fixture exists");
  const auto mutableResult =
      analyzeTrustedPreludeFixture("mutable-function.gti", mutableSource);
  for (const auto &diagnostic : mutableResult.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(mutableResult.canGenerateCode(),
         "public mutable wrappers select ordinary GTI definitions");
  auto readOnlyBinding = mutableSource;
  const auto binding = readOnlyBinding.find("mut std::function<int(int) mut>");
  expect(binding != std::string::npos,
         "mutable fixture contains tested binding");
  if (binding != std::string::npos) {
    readOnlyBinding.erase(binding, 4);
    const auto rejected = analyzeTrustedPreludeFixture(
        "readonly-mutable-function.gti", readOnlyBinding);
    expect(!rejected.canGenerateCode() &&
               !hasDiagnosticCode(rejected.diagnostics, "GTI-B0001") &&
               std::any_of(rejected.diagnostics.begin(),
                           rejected.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.message.find(
                                        "requires a mutable receiver") !=
                                    std::string::npos;
                           }),
           "a mutable signature does not grant mutation through an immutable "
           "binding");
  }
  const auto foreign = analyzeTrustedPreludeFixture(
      "foreign-function-family.gti", "#include <std/functional>\nnamespace std "
                                     "{ class function<R(Args...) &&> {}; }\n");
  expect(!foreign.canGenerateCode() &&
             hasDiagnosticCode(foreign.diagnostics, "GTI-S2006"),
         "applications cannot extend a foreign signature family");
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owning_function.gti";
  std::ifstream input(path);
  expect(input.good(), "public owning callback fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto valid =
      analyzeTrustedPreludeFixture("owning-function.gti", source);
  expect(valid.canGenerateCode(),
         "source-defined public wrappers support clone, replacement, a setter, "
         "void invocation and a move-only capture through valid MIR");

  const auto copyPath =
      path.parent_path() / "owning_function_fallible_copy.gti";
  std::ifstream copyInput(copyPath);
  expect(copyInput.good(), "custom-copy callback rejection fixture exists");
  const std::string copySource{std::istreambuf_iterator<char>(copyInput), {}};
  const auto customCopy =
      analyzeTrustedPreludeFixture(copyPath.string(), copySource);
  expect(!customCopy.canGenerateCode() &&
             hasDiagnosticCode(customCopy.diagnostics, "GTI-S2020") &&
             !hasDiagnosticCode(customCopy.diagnostics, "GTI-B0001") &&
             std::any_of(
                 customCopy.diagnostics.begin(), customCopy.diagnostics.end(),
                 [](const auto &diagnostic) {
                   return diagnostic.code == "GTI-S2020" &&
                          diagnostic.message.find(
                              "Custom copy and move constructor bodies") !=
                              std::string::npos;
                 }),
         "callback cloning does not bypass the language restriction on custom "
         "copy constructor bodies");

  const auto itemPath = path.parent_path() / "owning_function_items.gti";
  std::ifstream itemInput(itemPath);
  expect(itemInput.good(), "stored function-item fixture exists");
  const std::string itemSource{std::istreambuf_iterator<char>(itemInput), {}};
  const auto items =
      analyzeTrustedPreludeFixture("function-items.gti", itemSource);
  for (const auto &diagnostic : items.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(items.canGenerateCode(),
         "named functions retain exact identity through storage, capture, "
         "return and source-defined erased callback wrappers");
  bool checkedItemCall = false;
  for (const auto &function : items.mir.functionInstances()) {
    if (checkedItemCall)
      break;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.functionTarget || instruction.operands.empty())
          continue;
        const auto *target =
            items.mir.findFunctionInstance(*instruction.functionTarget);
        if (!target || target->owner ||
            target->linkage != lang::LanguageLinkage::Gti ||
            instruction.intrinsic != lang::IntrinsicKind::None)
          continue;
        expect(!instruction.receiver,
               "an exact namespace target has no method receiver");
        auto forged = items.mir;
        auto *body = lang::findMirBody(
            forged, {lang::MirBodyKind::Function, function.id});
        for (auto &changed : body->blocks[block.id - 1].instructions)
          if (changed.id == instruction.id)
            changed.receiver = changed.operands.front();
        const auto verified = lang::verifyMirProgram(forged);
        expect(!verified.valid() &&
                   std::any_of(verified.errors.begin(), verified.errors.end(),
                               [](const auto &error) {
                                 return error.message.find(
                                            "unexpected receiver") !=
                                        std::string::npos;
                               }),
               "MIR rejects attaching a method receiver to a namespace "
               "function call");
        checkedItemCall = true;
        break;
      }
      if (checkedItemCall)
        break;
    }
  }
  expect(checkedItemCall,
         "stored callback fixture contains an exact namespace call");

  const auto schemaPath = path.parent_path() / "owning_function_schema.gti";
  std::ifstream schemaInput(schemaPath);
  expect(schemaInput.good(), "recursive command-schema fixture exists");
  const std::string schemaSource{std::istreambuf_iterator<char>(schemaInput),
                                 {}};
  const auto schema =
      analyzeTrustedPreludeFixture("callback-schema.gti", schemaSource);
  for (const auto &diagnostic : schema.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(schema.canGenerateCode(),
         "recursive command schemas can retain and invoke owned handlers");

  const auto borrowPath = path.parent_path() / "owning_function_borrows.gti";
  std::ifstream borrowInput(borrowPath);
  expect(borrowInput.good(), "borrowed callback fixture exists");
  const std::string borrowSource{std::istreambuf_iterator<char>(borrowInput),
                                 {}};
  const auto borrowed =
      analyzeTrustedPreludeFixture("borrowed-callback.gti", borrowSource);
  expect(borrowed.canGenerateCode(),
         "reference packs forward scalar and noncopyable object borrows "
         "through callbacks");
  bool projectedBorrow = false;
  bool lambdaBorrow = false;
  for (const auto &function : borrowed.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputRole != lang::MirCallInputRole::Argument ||
            instruction.callInputKind != lang::HirCallInputKind::ReadBorrow ||
            instruction.operands.size() != 1)
          continue;
        const auto &operand = instruction.operands.front();
        const auto *place = function.body.findPlace(operand.place);
        if (!place || place->type.kind != lang::SemanticType::Reference ||
            place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        projectedBorrow = true;
        expect(operand.kind == lang::MirOperandKind::BorrowRead &&
                   !instruction.preparedParameterDrop &&
                   instruction.lifecycle.empty(),
               "borrowed pack elements have no ownership transfer or prepared "
               "cleanup");
        for (const auto &callBlock : function.body.blocks)
          for (const auto &call : callBlock.instructions)
            lambdaBorrow =
                lambdaBorrow ||
                (call.lambdaTarget && call.callSite == instruction.callSite);
        for (int mutation = 0; mutation < 3; ++mutation) {
          auto forged = borrowed.mir;
          auto *body = lang::findMirBody(
              forged, {lang::MirBodyKind::Function, function.id});
          if (mutation == 2) {
            body->places[place->id - 1].projections.front().constantIndex = 999;
          } else {
            auto &changedBlock = body->blocks[block.id - 1];
            for (auto &changed : changedBlock.instructions) {
              if (changed.id != instruction.id)
                continue;
              if (mutation == 0)
                changed.callInputKind = lang::HirCallInputKind::MoveValue;
              else
                changed.callInputIndex = 999;
            }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "borrowed argument packs reject moved borrows and forged "
                 "argument/element indices");
        }
      }
    }
  }
  expect(projectedBorrow && lambdaBorrow,
         "public callbacks reach projected reference arguments and exact "
         "lambda dispatch");

  const auto wrongBorrow = analyzeTrustedPreludeFixture("wrong-borrow.gti", R"(
#include <std/functional>
struct MutableTarget { void operator()(mut int& value) { value++; } };
int main() {
  MutableTarget callable{};
  std::function<void(int&)> callback{std::move(callable)};
  return 0;
}
)");
  expect(!wrongBorrow.canGenerateCode() &&
             hasDiagnosticCode(wrongBorrow.diagnostics, "GTI-S2022") &&
             !hasDiagnosticCode(wrongBorrow.diagnostics, "GTI-B0001"),
         "a read-only signature cannot provide mutable callback arguments");

  const auto escapedBorrow =
      analyzeTrustedPreludeFixture("escaped-pack.gti", R"(
int& escape<Args...>(Args... args) { int local = 7; return local; }
int main() { int local = 4; return escape<int&>(local); }
)");
  expect(
      !escapedBorrow.canGenerateCode() &&
          hasDiagnosticCode(escapedBorrow.diagnostics, "GTI-S2017") &&
          !hasDiagnosticCode(escapedBorrow.diagnostics, "GTI-B0001"),
      "a borrowed parameter pack cannot authorize returning a local reference");

  const std::string nominalPack = R"(
struct Nullary { int operator()() { return 4; } };
struct Binary { int operator()(int value, bool enabled) { return enabled ? value : 0; } };
int forward<Callable, Args...>(Callable callable, Args... args) {
  return callable(args...);
}
int main() {
  Nullary zero{};
  Binary two{};
  int first = forward(std::move(zero));
  return forward(std::move(two), first, true) - 4;
}
)";
  expect(
      analyzeTrustedPreludeFixture("nominal-pack.gti", nominalPack)
          .canGenerateCode(),
      "ordinary nominal operator calls expand empty and mixed argument packs");
  auto wrongPack = nominalPack;
  wrongPack.replace(wrongPack.find("first, true"), 11, "true, first");
  expect(!analyzeTrustedPreludeFixture("wrong-nominal-pack.gti", wrongPack)
              .canGenerateCode(),
         "concrete nominal pack forwarding retains exact argument order and "
         "types");

  for (const auto &invalid : {std::string{R"(
auto owned = std::make_unique<int>(7);
auto callback = [owned = std::move(owned)]() -> int { return *owned; };
std::function<int()> wrapper{std::move(callback)};
)"},
                              std::string{R"(
auto callback = []() -> bool { return true; };
std::function<int()> wrapper{std::move(callback)};
)"},
                              std::string{R"(
auto callback = [](int value) -> int { return value; };
std::function<int(bool)> wrapper{std::move(callback)};
)"},
                              std::string{R"(
auto callback = []() -> int { return 7; };
std::function<int()> wrapper{std::move(callback)};
auto copy = wrapper;
)"}}) {
    const auto result = analyzeTrustedPreludeFixture(
        "invalid-owning-function.gti",
        "#include <std/functional>\n#include <std/memory>\nint main() {\n" +
            invalid + "\nreturn 0;\n}\n");
    expect(!result.canGenerateCode() &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           "owning callbacks reject noncopyable clone targets, inexact "
           "signatures and implicit wrapper copies without a compiler ICE");
  }
  if (failures == 0)
    std::cout << "Owning function tests passed.\n";
  return failures == 0 ? 0 : 1;
}
