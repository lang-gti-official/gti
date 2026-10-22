#include "compiler_test_fixture.h"

#include "gti/mir_effects.h"

namespace {

[[nodiscard]] bool isOptionalIntrinsic(lang::IntrinsicKind intrinsic) {
  return intrinsic == lang::IntrinsicKind::OptionalStorageHasValue ||
         intrinsic == lang::IntrinsicKind::OptionalStorageConstruct ||
         intrinsic == lang::IntrinsicKind::OptionalStorageRead ||
         intrinsic == lang::IntrinsicKind::OptionalStorageReadMut ||
         intrinsic == lang::IntrinsicKind::OptionalStorageReset;
}

void testOptionalCompilerContract() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "optional-compiler-contract.gti", R"(
#include <std/optional>

class payload {
  mut int32_t stored;

public:
  payload(int32_t value) : stored(value) {}
  payload(payload& other) = delete;
  payload(payload&& other) = default;
  ~payload() {}
  void set(int32_t value) mut { this.stored = value; }
  int32_t get() { return this.stored; }
};

int main() {
  std::optional<payload> constructed{payload(int32_t(3))};
  if (constructed.value().get() != int32_t(3)) {
    return 2;
  }
  mut std::optional<payload> value{};
  [[discard]] value.emplace(int32_t(4));
  value.value().set(int32_t(5));
  if (!value.has_value() || value.value().get() != int32_t(5)) {
    return 1;
  }
  mut std::optional<payload> moved{std::move(value)};
  moved.reset();
  std::optional<int32_t> fixed{int32_t(7)};
  return fixed.value() - int32_t(7);
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "the installed optional surface should pass semantic analysis");
  if (!frontend.canGenerateCode()) {
    return;
  }

  bool ordinaryStorage = true;
  bool constructedPayload = false;
  bool extractedPayload = false;
  bool terminalFailure = false;
  for (const auto &instance : frontend.mir.classInstances())
    ordinaryStorage &= instance.compilerCapability !=
                       lang::CompilerCapabilityTypeKind::OptionalStorage;
  for (const auto &function : frontend.hir.functionInstances())
    for (const auto &value : function.body.values)
      ordinaryStorage &= !isOptionalIntrinsic(value.intrinsic);
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = lang::findMirBody(frontend.mir, address);
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        ordinaryStorage &= !isOptionalIntrinsic(instruction.intrinsic);
        constructedPayload |=
            instruction.operation == lang::MirOperation::PayloadConstruct;
        extractedPayload |=
            instruction.operation == lang::MirOperation::PayloadExtract;
        terminalFailure |=
            instruction.intrinsic == lang::IntrinsicKind::FailEmptyOwner;
      }
  }
  expect(ordinaryStorage && constructedPayload && extractedPayload &&
             terminalFailure && lang::verifyMirProgram(frontend.mir).valid(),
         "optional uses ordinary enum construction/extraction and checked "
         "terminal failure, not private storage");
  const auto artifact = gti_test::emitCpp(frontend);
  expect(!artifact.contents.empty(),
         "source optional reaches production native emission");

  // The terminal primitive only chooses the failure category. It grants no
  // owner, value, storage or caller-location authority.
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto forged = frontend.mir;
    bool changed = false;
    for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
      auto *body = lang::findMirBody(forged, address);
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions) {
          if (changed ||
              instruction.intrinsic != lang::IntrinsicKind::FailEmptyOwner)
            continue;
          if (mutation == 0)
            instruction.neverReturns = false;
          if (mutation == 1)
            instruction.tracksCaller = true;
          if (mutation == 2)
            instruction.parameterTypes.push_back(lang::SemanticType::Bool);
          if (mutation == 3)
            instruction.definedFailure.localOrigins.clear();
          if (mutation == 4)
            instruction.definedFailure.localOrigins.front()
                .outcomes.front()
                .code = lang::DefinedFailureCode::Panic;
          changed = true;
        }
    }
    expect(changed && !lang::verifyMirProgram(forged).valid(),
           "terminal owner failure rejects altered arity, termination, caller "
           "or failure identity");
  }

  // General enum proof replaces private optional publication proof. A
  // completed payload still cannot lose its exact type, alternative or drop.
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto forged = frontend.mir;
    bool changed = false;
    for (const auto address : lang::enumerateMirBodyAddresses(forged)) {
      auto *body = lang::findMirBody(forged, address);
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions) {
          if (changed ||
              instruction.operation != lang::MirOperation::PayloadConstruct ||
              instruction.operands.empty() || instruction.lifecycle.empty())
            continue;
          if (mutation == 0)
            instruction.operands.clear();
          if (mutation == 1)
            instruction.enumOwner = 0;
          if (mutation == 2)
            instruction.enumVariant = 999;
          if (mutation == 3)
            instruction.lifecycle.clear();
          changed = true;
        }
    }
    expect(changed && !lang::verifyMirProgram(forged).valid(),
           "optional enum publication rejects missing values, wrong "
           "alternatives and missing lifecycle proof");
  }

  const auto invalidEmplacement = lang::Frontend().analyze(
      "optional-invalid-payload-construction.gti", R"(
#include <std/optional>
struct Payload {
  int value;
  Payload(int value) : value(value) {}
};
int main() {
  mut std::optional<Payload> slot{};
  [[discard]] slot.emplace("invalid");
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalidEmplacement.canGenerateCode() &&
             hasDiagnosticCode(invalidEmplacement.diagnostics, "GTI-S2012") &&
             hasDiagnostic(invalidEmplacement.diagnostics,
                           "No constructor of 'Payload' exactly matches") &&
             !hasDiagnosticCode(invalidEmplacement.diagnostics, "GTI-B0001"),
         "invalid emplacement must use ordinary source constructor diagnostics "
         "rather than reaching the native backend");

  const lang::FrontendResult copied = lang::Frontend().analyze(
      "optional-copy-rejected.gti", R"(
#include <std/optional>
int main() {
  std::optional<int32_t> source{int32_t(1)};
  std::optional<int32_t> copied = source;
  return copied.value();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!copied.canGenerateCode() &&
             hasDiagnostic(copied.diagnostics, "Cannot initialize 'copied'") &&
             hasDiagnosticCode(copied.diagnostics, "GTI-S2003"),
         "optional copies should fail closed before lowering");

  const lang::FrontendResult reusedAfterMove = lang::Frontend().analyze(
      "optional-source-after-move-rejected.gti", R"(
#include <std/optional>
int main() {
  std::optional<int32_t> source{int32_t(1)};
  std::optional<int32_t> destination{std::move(source)};
  return source.has_value();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!reusedAfterMove.canGenerateCode() &&
             hasDiagnosticCode(reusedAfterMove.diagnostics, "GTI-S2018") &&
             hasDiagnostic(reusedAfterMove.diagnostics,
                           "Value 'source' has already been moved"),
         "an optional source should become unavailable after move "
         "construction");

  const lang::FrontendResult aliased = lang::Frontend().analyze(
      "optional-live-borrow-rejected.gti", R"(
#include <std/optional>
int main() {
  mut std::optional<int32_t> value{int32_t(7)};
  mut int32_t& borrowed = value.value();
  value.reset();
  return borrowed;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !aliased.canGenerateCode() &&
          hasDiagnostic(aliased.diagnostics,
                        "while a reference borrowed from it may still be live"),
      "reset should reject while an element reference remains live");

  const lang::FrontendResult forgedSpelling =
      lang::Frontend().analyze("ordinary-optional-storage.gti", R"(
namespace ordinary {
class optional_storage<T> {
public:
  optional_storage() {}
};
}
int main() {
  ordinary::optional_storage<int32_t> value{};
  return 0;
}
)");
  bool ordinaryOnly = forgedSpelling.canGenerateCode();
  for (const lang::MirClassInstance &instance :
       forgedSpelling.mir.classInstances()) {
    ordinaryOnly = ordinaryOnly && instance.compilerCapability ==
                                       lang::CompilerCapabilityTypeKind::None;
  }
  expect(ordinaryOnly,
         "an ordinary class with the optional-storage spelling must not mint "
         "compiler capability identity");
}

void testOptionalResetCleanup() {
  const auto frontend = lang::Frontend().analyze(
      "optional-reset-cleanup.gti", R"(
#include <std/optional>
class Resource {
  int divisor;
public:
  Resource(int divisor) : divisor(divisor) {}
  ~Resource() { int checked = 12 / this.divisor; }
};
void destroy(int divisor) {
  mut std::optional<Resource> slot{Resource(divisor)};
  slot.reset();
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "fallible payload reset must have independently verified MIR");
  if (!frontend.canGenerateCode())
    return;
  bool found = false;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = lang::findMirBody(frontend.mir, address);
    for (const auto &block : body->blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CleanupBeforeAssign ||
            instruction.definedFailure.propagation !=
                lang::FailurePropagationKind::Destructor)
          continue;
        found = true;
        expect(instruction.destination &&
                   instruction.definedFailure.propagation ==
                       lang::FailurePropagationKind::Destructor &&
                   instruction.localFailureSites.empty() &&
                   instruction.definedFailure.localOrigins.empty() &&
                   block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "reset invokes exact payload cleanup without inventing a new "
               "failure identity");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *changedBody = lang::findMirBody(forged, address);
          auto &changedBlock = changedBody->blocks[block.id - 1];
          for (auto &changed : changedBlock.instructions) {
            if (changed.id != instruction.id)
              continue;
            if (mutation == 0)
              changed.destination.reset();
            else if (mutation == 1)
              changed.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
            else {
              changedBlock.terminator.kind = lang::MirTerminatorKind::Goto;
              changedBlock.terminator.invokeInstruction = 0;
              changedBlock.terminator.elseTarget = 0;
              changedBlock.terminator.failureRecord = 0;
            }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "reset cannot erase payload cleanup, failure propagation, "
                 "or the caller cleanup edge");
        }
      }
    }
  }
  expect(found, "source reset must use ordinary fallible assignment cleanup");
  const auto artifact = gti_test::emitCpp(frontend);
  // The generated support preamble still declares unused legacy templates.
  // Inspect instantiated MIR, rather than mistaking their text for use.
  bool ordinaryReset = !artifact.contents.empty();
  for (const auto &instance : frontend.mir.classInstances())
    ordinaryReset &= instance.compilerCapability !=
                     lang::CompilerCapabilityTypeKind::OptionalStorage;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir))
    for (const auto &block : lang::findMirBody(frontend.mir, address)->blocks)
      for (const auto &instruction : block.instructions)
        ordinaryReset &= !isOptionalIntrinsic(instruction.intrinsic);
  expect(ordinaryReset, "source reset emits without any instantiated private "
                        "optional storage or operation");
}

void testNestedOptionalConstruction() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_nested.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = lang::Frontend().analyze(path.string(), source,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &instance : frontend.mir.classInstances()) {
      const auto derived =
          lang::mirTypeRelocationKind(frontend.mir, instance.type);
      if (derived != instance.traits.relocation)
        std::cerr << "relocation mismatch: class " << instance.id << " "
                  << "declaration=" << instance.declaration
                  << " semantic=" << int(instance.traits.relocation)
                  << " mir=" << int(derived)
                  << " move=" << int(instance.moveConstructor) << '\n';
    }
  }
  expect(input.good() && frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "nested construction-only owners retain exact lifecycle and "
         "relocation metadata without requiring assignment");
}

void testStoragePayloadAdmission() {
  const std::string prefix = R"(
#include <std/optional>
#include <std/vector>
class View {
  int& source;
public:
  View(int& value) : source(value) {}
};
)";
  for (const std::string declaration :
       {"std::optional<View> item{};",
        "std::optional<View> item{View(source)};",
        "std::vector<View> item{};"}) {
    const std::string source =
        prefix + "int main() { int source = 7; " + declaration + " return 0; }";
    const auto frontend = lang::Frontend().analyze(
        "borrowed-storage-admission.gti", source, {standardLibraryPrelude()},
        {}, {standardLibraryRoot()});
    if (declaration.find("optional") == std::string::npos) {
      expect(!frontend.canGenerateCode() &&
                 hasDiagnosticCode(frontend.diagnostics, "GTI-S2029") &&
                 hasDiagnostic(frontend.diagnostics, "View") &&
                 hasDiagnostic(frontend.diagnostics, "vector_element") &&
                 !hasDiagnosticCode(frontend.diagnostics, "GTI-B0001"),
             "source-defined owning storage rejects borrowed payloads through "
             "its declared element constraint, without a private handle rule");
      continue;
    }
    // View is a user class containing int&, not std::string_view. Both empty
    // and engaged optional<View> were rejected before promotion. Keep that
    // boundary until this wrapper can express its retained field contract.
    // Empty optional<string_view> remains positively tested below.
    expect(!frontend.canGenerateCode() &&
               hasDiagnosticCode(frontend.diagnostics, "GTI-S2045") &&
               !hasDiagnosticCode(frontend.diagnostics, "GTI-B0001"),
           "a source wrapper cannot invent its field's retained reference "
           "contract");
  }

  const std::string constructorSource = R"(
class Broken<uint64_t N> {
  static_assert(N > 2, "class constraint");
public:
  Broken() {
    static_assert(N > 0, "first constructor constraint");
    static_assert(N > 1, "second constructor constraint");
  }
};
int main() {
  Broken<0> first = Broken<0>();
  return 0;
}
)";
  const auto constructorDiagnostics = lang::Frontend().analyze(
      "class-and-constructor-diagnostics.gti", constructorSource);
  expect(!constructorDiagnostics.canGenerateCode() &&
             countDiagnosticCode(constructorDiagnostics.diagnostics,
                                 "GTI-S2079") == 3,
         "class-owned diagnostics publish once without suppressing distinct "
         "concrete constructor-body errors");
  for (const std::string_view condition : {"N > 2", "N > 0", "N > 1"}) {
    const auto start = constructorSource.find(condition);
    const auto request = condition == "N > 2"
                             ? constructorSource.find("first =")
                             : constructorSource.find("Broken<0>()");
    expect(std::count_if(
               constructorDiagnostics.diagnostics.begin(),
               constructorDiagnostics.diagnostics.end(),
               [&](const lang::Diagnostic &diagnostic) {
                 return diagnostic.code == "GTI-S2079" &&
                        diagnostic.primary.start == start &&
                        diagnostic.primary.end == start + condition.size() &&
                        diagnostic.related.size() == 2 &&
                        diagnostic.related.front().message ==
                            "Concrete substitution: 'N' = '0'." &&
                        diagnostic.related.back().span.start == request &&
                        diagnostic.related.back().span.end ==
                            request + (condition == "N > 2" ? 5 : 6) &&
                        diagnostic.related.back().message ==
                            "Concrete generic instance requested here.";
               }) == 1,
           "class and constructor diagnostics retain their distinct primary "
           "and instantiation sites with concrete substitution notes");
  }

  std::string multipleInstances = constructorSource;
  multipleInstances.insert(multipleInstances.find("  return 0;"),
                           "  Broken<1> second = Broken<1>();\n");
  const auto distinctInstances = lang::Frontend().analyze(
      "distinct-instance-diagnostics.gti", multipleInstances);
  const auto *entry = distinctInstances.sourceGraph.findUnit(
      distinctInstances.sourceGraph.entryUnit());
  expect(!distinctInstances.canGenerateCode() && entry != nullptr &&
             countDiagnosticCode(distinctInstances.diagnostics, "GTI-S2079") ==
                 5,
         "class-owned publication does not merge errors from distinct "
         "concrete instances that share the same generic source span");
  for (const int value : {0, 1}) {
    for (const std::string_view condition : {"N > 2", "N > 0", "N > 1"}) {
      const bool fails = value == 0 || condition != "N > 0";
      const auto start = multipleInstances.find(condition);
      const std::string construction =
          "Broken<" + std::to_string(value) + ">()";
      const auto request =
          condition == "N > 2"
              ? multipleInstances.find(value == 0 ? "first =" : "second =")
              : multipleInstances.find(construction);
      const auto matching = std::count_if(
          distinctInstances.diagnostics.begin(),
          distinctInstances.diagnostics.end(),
          [&](const lang::Diagnostic &diagnostic) {
            return diagnostic.code == "GTI-S2079" && entry != nullptr &&
                   diagnostic.primary.source == entry->path.string() &&
                   diagnostic.primary.start == start &&
                   diagnostic.primary.end == start + condition.size() &&
                   diagnostic.related.size() == 2 &&
                   diagnostic.related.front().message ==
                       "Concrete substitution: 'N' = '" +
                           std::to_string(value) + "'." &&
                   diagnostic.related.back().span.source ==
                       entry->path.string() &&
                   diagnostic.related.back().span.start == request &&
                   diagnostic.related.back().span.end ==
                       request +
                           (condition == "N > 2" ? (value == 0 ? 5 : 6) : 6) &&
                   diagnostic.related.back().message ==
                       "Concrete generic instance requested here.";
          });
      expect(matching == (fails ? 1 : 0),
             "each failing concrete substitution retains its own diagnostic "
             "and requesting location without repeating class diagnostics");
    }
  }

  for (const std::string type : {"int*", "int&", "void"}) {
    const auto frontend = lang::Frontend().analyze(
        "optional-unsupported-payload.gti",
        "#include <std/optional>\nint main() { std::optional<" + type +
            "> item{}; return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    if (type == "int*") {
      expect(frontend.canGenerateCode() &&
                 lang::verifyMirProgram(frontend.mir).valid(),
             "source optional admits ordinary raw pointer values without "
             "dereferencing them");
      continue;
    }
    expect(!frontend.canGenerateCode() &&
               hasDiagnosticCode(frontend.diagnostics,
                                 type == "void" ? "GTI-S2029" : "GTI-S2045") &&
               !hasDiagnosticCode(frontend.diagnostics, "GTI-B0001"),
           "unsupported optional payload shapes must fail at source instead "
           "of reaching the MIR/backend contract");
  }

  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_borrowed_escape_rejected.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto escaped = lang::Frontend().analyze(path.string(), source,
                                                {standardLibraryPrelude()}, {},
                                                {standardLibraryRoot()});
  expect(input.good() && !escaped.canGenerateCode() &&
             hasDiagnosticCode(escaped.diagnostics, "GTI-S2045") &&
             !hasDiagnosticCode(escaped.diagnostics, "GTI-B0001"),
         "an unrelated generic wrapper and destructor cannot hide a borrowed "
         "optional payload escaping its local owner");

  const auto admitted = lang::Frontend().analyze(
      "optional-admitted-payloads.gti", R"(
#include <std/optional>
#include <std/string>
int main() {
  std::optional<std::string_view> text{};
  std::optional<int[2]> array{};
  std::optional<expected<int, int>> result{};
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(admitted.canGenerateCode() &&
             lang::verifyMirProgram(admitted.mir).valid(),
         "optional admission preserves static text views, fixed arrays, and "
         "expected payloads rather than borrowing sparse-storage exclusions");

  for (const std::string argument : {"owner.view()", "\"static\""}) {
    const auto text = lang::Frontend().analyze(
        "optional-text-parameter-storage.gti",
        "#include <std/optional>\n#include <std/string>\n"
        "std::optional<std::string_view> escape() { "
        "std::string owner{\"owned\"}; "
        "return std::optional<std::string_view>(" +
            argument + "); }\nint main() { auto result = escape(); return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    // The constructor publishes its argument's owner at the payload
    // (L-OWN-02): a value over literal text may leave, one over a local
    // owner may not.
    if (argument == "\"static\"") {
      expect(text.canGenerateCode() && lang::verifyMirProgram(text.mir).valid(),
             "an optional over literal text holds ownerless text and may be "
             "returned");
      continue;
    }
    expect(!text.canGenerateCode() &&
               hasDiagnosticCode(text.diagnostics, "GTI-S2045") &&
               hasDiagnostic(text.diagnostics,
                             "holds dynamic text from a local owner") &&
               !hasDiagnosticCode(text.diagnostics, "GTI-B0001"),
           "an optional over a local owner's text cannot be returned");
  }

  const auto literalPayload = lang::Frontend().analyze(
      "literal-text-payload.gti", R"(
enum Text { empty, value(std::string_view text) };
int main() { auto text = Text::value("static"); return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(literalPayload.canGenerateCode() &&
             lang::verifyMirProgram(literalPayload.mir).valid(),
         "a directly literal-backed text payload needs no external owner loan");
  for (const std::string expression : {"owner.view()", "std::move(view)"}) {
    const auto dynamicPayload = lang::Frontend().analyze(
        "dynamic-text-payload.gti",
        "#include <std/string>\nenum Text { empty, value(std::string_view "
        "text) };\n"
        "int main() { std::string owner{\"owned\"}; "
        "std::string_view view = owner.view(); auto text = Text::value(" +
            expression + "); return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!dynamicPayload.canGenerateCode() &&
               hasDiagnosticCode(dynamicPayload.diagnostics, "GTI-S2045") &&
               !hasDiagnosticCode(dynamicPayload.diagnostics, "GTI-B0001"),
           "ordinary enum storage rejects dynamic text, including moved views, "
           "before MIR");
  }

  for (const std::string storage :
       {"std::string_view values[1] = {std::move(view)};",
        "mut std::vector<std::string_view> values{}; "
        "values.push_back(std::move(view));"}) {
    const auto moved = lang::Frontend().analyze(
        "moved-text-storage-rejected.gti",
        "#include <std/string>\n#include <std/vector>\n"
        "struct Holder { std::string_view text; };\n"
        "int main() { std::string owner{\"owned\"}; "
        "std::string_view view = owner.view(); " +
            storage + " return 0; }",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!moved.canGenerateCode() &&
               hasDiagnosticCode(moved.diagnostics, "GTI-S2045") &&
               !hasDiagnosticCode(moved.diagnostics, "GTI-B0001"),
           "moving owner-dependent text does not erase its loan at a "
           "fixed-array or dynamic-storage boundary");
  }
  // A local struct value carries the moved view's owner at its field
  // (L-OWN-02), so the owner stays borrowed while the value is live.
  const auto movedField = lang::Frontend().analyze(
      "moved-text-field-loan.gti",
      "#include <std/string>\n"
      "struct Holder { std::string_view text; };\n"
      "int main() { mut std::string owner{\"owned\"}; "
      "std::string_view view = owner.view(); "
      "Holder value{.text = std::move(view)}; owner.clear(); "
      "return int(value.text.size()); }",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!movedField.canGenerateCode() &&
             hasDiagnosticCode(movedField.diagnostics, "GTI-S2017") &&
             !hasDiagnosticCode(movedField.diagnostics, "GTI-B0001"),
         "moving owner-dependent text into a local value's field keeps its "
         "owner loan");

  const auto localText = lang::Frontend().analyze(
      "moved-text-local-loan.gti", R"(
#include <std/string>
int main() {
  std::string owner{"owned"};
  std::string_view view = owner.view();
  std::string_view moved = std::move(view);
  return moved.size() == std::size_t(5) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(localText.canGenerateCode() &&
             lang::verifyMirProgram(localText.mir).valid(),
         "moving a text borrow into a local binding retains the original "
         "owner loan and remains valid within that owner's lifetime");
  const auto ownerMutation = lang::Frontend().analyze(
      "moved-text-owner-mutation.gti", R"(
#include <std/string>
int main() {
  mut std::string owner{"owned"};
  std::string_view view = owner.view();
  std::string_view moved = std::move(view);
  owner.clear();
  return moved.size() == std::size_t(5) ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!ownerMutation.canGenerateCode() &&
             hasDiagnosticCode(ownerMutation.diagnostics, "GTI-S2017"),
         "a moved local text borrow still prevents mutation of its original "
         "owner rather than merely retaining a borrowed-state flag");
  const auto cText = lang::Frontend().analyze(
      "optional-c-string-escape.gti", R"(
#include <std/optional>
#include <std/string>
std::optional<c_string> escape() {
  std::string owner{"owned"};
  return std::optional<c_string>(owner.c_string());
}
int main() { auto result = escape(); return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!cText.canGenerateCode() &&
             hasDiagnosticCode(cText.diagnostics, "GTI-S2045") &&
             hasDiagnostic(cText.diagnostics,
                           "holds dynamic text from a local owner") &&
             !hasDiagnosticCode(cText.diagnostics, "GTI-B0001"),
         "optional storage applies the same dependency rule to C-string "
         "views without relying on a public library spelling");
}

void testStoragePayloadMirAdmission() {
  const auto frontend = lang::Frontend().analyze(
      "storage-payload-mir-admission.gti", R"(
#include <std/optional>
#include <std/vector>
struct Payload { int value = 7; };
int main() {
  std::optional<Payload> optional{};
  std::vector<Payload> prefix{};
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "the trusted-storage mutation fixture starts as valid MIR");
  if (!frontend.canGenerateCode())
    return;
  const auto payload = std::find_if(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(), [&](const auto &instance) {
        return std::any_of(frontend.hir.classInstances().begin(),
                           frontend.hir.classInstances().end(),
                           [&](const auto &hir) {
                             return hir.id == instance.id && hir.source &&
                                    hir.source->name().lexeme == "Payload";
                           });
      });
  expect(payload != frontend.mir.classInstances().end(),
         "ordinary payload identity is retained independently of optional");
  if (payload == frontend.mir.classInstances().end())
    return;
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = frontend.mir;
    auto *changed = const_cast<lang::MirClassInstance *>(
        forged.findClassInstance(payload->id));
    if (mutation == 0)
      changed->traits.movable = false;
    else if (mutation == 1)
      changed->traits.containsBorrowedState = true;
    else
      changed->declaredFields.front().type =
          lang::SemanticType::referenceTo(lang::SemanticType::Int32);
    expect(!lang::verifyMirProgram(forged).valid(),
           "ordinary class and enum verification rejects forged relocation or "
           "borrow facts");
  }
}

void testValueOrContract() {
  const std::string source = R"(
#include <std/optional>
struct Resource {
  Resource() {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
};
int main() {
  std::optional<Resource> value{};
  auto result = value.value_or(Resource());
  return 0;
}
)";
  const auto invalid = lang::Frontend().analyze("optional-value-or.gti", source,
                                                {standardLibraryPrelude()}, {},
                                                {standardLibraryRoot()});
  const auto *diagnostic =
      findDiagnosticByCode(invalid.diagnostics, "GTI-S2029");
  const auto call = source.find("));") + 1;
  expect(!invalid.canGenerateCode() && diagnostic != nullptr &&
             diagnostic->primary.source.ends_with("optional-value-or.gti") &&
             diagnostic->primary.start == call &&
             diagnostic->primary.end == call + 1 &&
             diagnostic->message.find("std::copy_constructible") !=
                 std::string::npos &&
             diagnostic->fixes.empty() &&
             hasRelatedDiagnostic(
                 invalid.diagnostics,
                 "Required application 'std::copy_constructible<Resource>'") &&
             hasDiagnosticHint(invalid.diagnostics,
                               "copy assignment is not required"),
         "value_or diagnoses its source API contract at the call, with the "
         "copy-construction requirement and no misleading fix");

  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_value_or_contract.gti";
  std::ifstream input(path);
  expect(input.good(), "optional API contract fixture exists");
  const std::string fixture{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
  const auto valid = lang::Frontend().analyze(
      "optional-value-or-valid.gti", fixture, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "value_or permits a copyable user payload and leaves "
         "move-only optional's other members usable");
  bool recordChecked = false;
  for (const auto &instance : valid.mir.classInstances()) {
    const auto hir = std::find_if(
        valid.hir.classInstances().begin(), valid.hir.classInstances().end(),
        [&](const auto &item) { return item.id == instance.id; });
    if (hir != valid.hir.classInstances().end() && hir->source &&
        hir->source->name().lexeme == "Record") {
      recordChecked = true;
      expect(instance.copyConstructor == lang::SpecialMemberStatus::Generated,
             "the positive payload retains generated copy construction");
    }
  }
  expect(recordChecked, "the positive record is retained in concrete MIR");
}

} // namespace

int main() {
  testOptionalCompilerContract();
  testValueOrContract();
  testOptionalResetCleanup();
  testNestedOptionalConstruction();
  testStoragePayloadAdmission();
  testStoragePayloadMirAdmission();
  if (failures != 0) {
    std::cerr << failures << " optional compiler test(s) failed\n";
    return 1;
  }
  std::cout << "optional compiler tests passed\n";
  return 0;
}
