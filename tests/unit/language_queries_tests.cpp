// LanguageQueries (hover signatures and documentation, occurrences and their
// roles) and the compile-only opt-out of tooling occurrences: library API
// beneath gti_lsp. (Moved unchanged from compiler_diagnostics_tests.cpp.)
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/semantic_analyzer.h"
#include "unit_test.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__)
             .parent_path()
             .parent_path()
             .parent_path() /
         "stdlib/prelude.gti";
}

[[maybe_unused]] std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

void testLanguageQueries() {
  const std::string source = R"(
/// Chooses the unsigned value.
///
/// The exact overload is selected by semantics.
uint64_t choose(uint64_t value) { return value; }
float choose(float value) { return value; }

namespace engine {
namespace graphics {
int render() { return 1; }
}
}
/// A shorter spelling for the graphics namespace.
namespace gfx = engine::graphics;

class Fixed<T, uint64_t N> {
  T values[N] = {};
public:
  Fixed() = default;
};

/// Owns one integer value.
class Box {
  int value = 0;
public:
  /// Constructs a box from `value`.
  Box(int value) : value(value) {}
public:
  Box() = default;
};

class Lifecycle {
  int value = 0;
public:
  Lifecycle(Lifecycle& other) = default;
  Lifecycle(Lifecycle&& other) = default;
public:
  Lifecycle() = default;
};

class Accessor {
  /// The value returned by `inspect`.
  mut int value = 0;
public:
  int inspect() { return this.value; }
  int inspect() mut {
    this.value += 1;
    return this.value;
  }
public:
  Accessor() = default;
};

/// A type that can render itself.
interface Renderable {
  int render();
};

void relay<Args...>(Args... values) {}

/// This block is deliberately detached.

class Undocumented {
  int value = 0;
public:
  Undocumented() = default;
};

// Ordinary comments are not API documentation.
class OrdinaryCommented {
  int value = 0;
public:
  OrdinaryCommented() = default;
};

int main() {
  auto inferred = choose(uint64_t(1));
  int rendered = gfx::render();
  mut int counter = 0;
  counter++;
  ++counter;
  Box box = Box(1);
  Lifecycle lifecycle_source{};
  Lifecycle lifecycle_copy = Lifecycle(lifecycle_source);
  mut Lifecycle lifecycle_movable{};
  Lifecycle lifecycle_move = Lifecycle(std::move(lifecycle_movable));
  Accessor fixed = Accessor();
  mut Accessor changing = Accessor();
  int fixed_value = fixed.inspect();
  int changing_value = changing.inspect();
  return 0;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "language-queries.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(frontend.semanticValid,
         "language-query fixture should pass semantic analysis");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const auto hoverAt = [&](std::size_t offset) {
    return queries.hover(frontend, unit, offset);
  };

  const std::size_t call = source.rfind("choose(uint64_t");
  const std::optional<lang::HoverInfo> selected = hoverAt(call + 1);
  expect(selected && selected->signature == "uint64_t choose(uint64_t value)",
         "hover should render the exact selected overload in GTI syntax");
  expect(selected && selected->documentationMarkdown ==
                         "Chooses the unsigned value.\n\nThe exact overload "
                         "is selected by semantics.",
         "selected-call hover should carry normalized declaration Markdown");
  expect(selected && selected->signature.find("float") == std::string::npos,
         "selected-call hover should not include unrelated overloads");
  expect(selected && selected->range.start == call &&
             selected->range.end == call + std::string("choose").size(),
         "hover should retain the exact source identifier range");

  const std::size_t interfaceDeclaration = source.find("Renderable");
  const std::optional<lang::HoverInfo> interfaceHover =
      hoverAt(interfaceDeclaration + 1);
  expect(interfaceHover && interfaceHover->signature == "interface Renderable",
         "hover should preserve interface spelling instead of rendering a "
         "class");
  expect(interfaceHover && interfaceHover->documentationMarkdown ==
                               "A type that can render itself.",
         "type hover should carry declaration documentation");

  const std::size_t variadicDeclaration = source.find("relay<Args");
  const std::optional<lang::HoverInfo> variadicHover =
      hoverAt(variadicDeclaration + 1);
  expect(variadicHover &&
             variadicHover->signature == "void relay<Args...>(Args... values)",
         "hover should preserve variadic-pack spelling on declarations");

  const std::size_t undocumentedDeclaration = source.find("Undocumented");
  const std::size_t ordinaryCommentedDeclaration =
      source.find("OrdinaryCommented");
  const std::optional<lang::HoverInfo> undocumentedHover =
      hoverAt(undocumentedDeclaration + 1);
  const std::optional<lang::HoverInfo> ordinaryCommentedHover =
      hoverAt(ordinaryCommentedDeclaration + 1);
  expect(undocumentedHover && !undocumentedHover->documentationMarkdown &&
             ordinaryCommentedHover &&
             !ordinaryCommentedHover->documentationMarkdown,
         "blank lines and ordinary comments should not attach API Markdown");

  const std::size_t readonlyReceiverCall =
      source.find("fixed.inspect") + std::string("fixed.").size();
  const std::size_t mutableReceiverCall =
      source.find("changing.inspect") + std::string("changing.").size();
  const std::optional<lang::HoverInfo> readonlyReceiverHover =
      hoverAt(readonlyReceiverCall + 1);
  const std::optional<lang::HoverInfo> mutableReceiverHover =
      hoverAt(mutableReceiverCall + 1);
  expect(
      readonlyReceiverHover &&
          readonlyReceiverHover->signature == "int32_t Accessor::inspect()" &&
          mutableReceiverHover &&
          mutableReceiverHover->signature == "int32_t Accessor::inspect() mut",
      "hover should expose the exact receiver-qualified method selected by "
      "semantics");

  const std::size_t autoKeyword = source.find("auto inferred");
  const std::optional<lang::HoverInfo> inferredType = hoverAt(autoKeyword + 1);
  expect(inferredType && inferredType->signature == "uint64_t",
         "hover over auto should show the complete inferred type");

  const std::size_t inferredName = source.find("inferred", autoKeyword);
  const std::optional<lang::HoverInfo> binding = hoverAt(inferredName + 1);
  expect(binding && binding->signature == "uint64_t inferred",
         "hover over a binding declaration should show mutability and type");

  const std::size_t construction = source.rfind("Box(1)");
  const std::optional<lang::HoverInfo> constructor = hoverAt(construction + 1);
  expect(constructor && constructor->signature == "Box(int32_t value)",
         "constructor hover should show the selected constructor signature");
  expect(constructor && constructor->documentationMarkdown ==
                            "Constructs a box from `value`.",
         "constructor-call hover should prefer constructor documentation");

  const std::size_t boxTypeUse = source.find("Box box");
  const std::optional<lang::HoverInfo> boxTypeHover = hoverAt(boxTypeUse + 1);
  expect(boxTypeHover &&
             boxTypeHover->documentationMarkdown == "Owns one integer value.",
         "type-use hover should carry documentation from its semantic symbol");

  const std::size_t fieldUse =
      source.find("this.value") + std::string("this.").size();
  const std::optional<lang::HoverInfo> fieldHover = hoverAt(fieldUse + 1);
  expect(fieldHover && fieldHover->documentationMarkdown ==
                           "The value returned by `inspect`.",
         "field-use hover should carry field documentation");

  const std::size_t copyConstruction =
      source.rfind("Lifecycle(lifecycle_source)");
  const std::size_t moveConstruction =
      source.rfind("Lifecycle(std::move(lifecycle_movable))");
  const std::optional<lang::HoverInfo> copyConstructor =
      hoverAt(copyConstruction + 1);
  const std::optional<lang::HoverInfo> moveConstructor =
      hoverAt(moveConstruction + 1);
  expect(copyConstructor &&
             copyConstructor->signature == "Lifecycle(Lifecycle&)" &&
             moveConstructor &&
             moveConstructor->signature == "Lifecycle(Lifecycle&&)",
         "constructor hover should distinguish copy and move policies");

  expect(!hoverAt(source.find("return 0") + std::string("return").size()),
         "hover should return no result for whitespace");
  expect(!frontend.semantics.database().occurrences(unit).empty(),
         "semantic analysis should retain tooling occurrences in its snapshot");

  const auto parameterReference = std::find_if(
      frontend.semantics.database().occurrences(unit).begin(),
      frontend.semantics.database().occurrences(unit).end(),
      [](const lang::SemanticOccurrence &occurrence) {
        return occurrence.name == "value" &&
               !lang::hasRole(occurrence.roles,
                              lang::OccurrenceRole::Declaration) &&
               occurrence.bindingKind == lang::SemanticBindingKind::Parameter;
      });
  expect(parameterReference !=
             frontend.semantics.database().occurrences(unit).end(),
         "semantic occurrences should retain resolved parameter references");
  if (parameterReference !=
      frontend.semantics.database().occurrences(unit).end()) {
    const lang::SymbolRecord *parameterSymbol =
        frontend.semantics.database().findSymbol(parameterReference->symbol);
    expect(parameterSymbol != nullptr &&
               parameterSymbol->kind == lang::SymbolKind::Parameter &&
               parameterSymbol->name == "value",
           "resolved parameter uses should point to a compiler-owned symbol");
    expect(frontend.semantics.database()
                   .occurrencesForSymbol(parameterReference->symbol)
                   .size() >= 2,
           "symbol queries should connect a parameter declaration and use");
  }

  const auto counterMutation = std::find_if(
      frontend.semantics.database().occurrences(unit).begin(),
      frontend.semantics.database().occurrences(unit).end(),
      [](const lang::SemanticOccurrence &occurrence) {
        return occurrence.name == "counter" &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::Read) &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::Write);
      });
  expect(counterMutation !=
             frontend.semantics.database().occurrences(unit).end(),
         "increment operands should retain combined read/write roles");

  const std::optional<lang::DefinitionInfo> selectedDefinition =
      queries.definition(frontend, unit, call + 1);
  const std::size_t firstChoose = source.find("choose(uint64_t");
  expect(selectedDefinition &&
             selectedDefinition->target.start == firstChoose &&
             selectedDefinition->target.end ==
                 firstChoose + std::string("choose").size(),
         "definition should follow the exact selected overload");

  const std::optional<lang::DefinitionInfo> readonlyReceiverDefinition =
      queries.definition(frontend, unit, readonlyReceiverCall + 1);
  const std::optional<lang::DefinitionInfo> mutableReceiverDefinition =
      queries.definition(frontend, unit, mutableReceiverCall + 1);
  const std::size_t readonlyReceiverDeclaration = source.find("inspect()");
  const std::size_t mutableReceiverDeclaration =
      source.find("inspect() mut", readonlyReceiverDeclaration + 1);
  expect(readonlyReceiverDefinition && mutableReceiverDefinition &&
             readonlyReceiverDefinition->target.start ==
                 readonlyReceiverDeclaration &&
             mutableReceiverDefinition->target.start ==
                 mutableReceiverDeclaration,
         "definition should follow receiver-qualified method identities");

  const std::size_t aliasUse = source.find("gfx::render");
  const std::optional<lang::DefinitionInfo> aliasDefinition =
      queries.definition(frontend, unit, aliasUse + 1);
  const std::size_t aliasDeclaration =
      source.find("gfx", source.find("namespace gfx"));
  expect(aliasDefinition && aliasDefinition->target.start == aliasDeclaration,
         "definition should preserve namespace alias identity");

  const std::size_t aliasTarget =
      source.find("engine::graphics", aliasDeclaration);
  const std::optional<lang::DefinitionInfo> namespaceDefinition =
      queries.definition(frontend, unit, aliasTarget + 1);
  const std::size_t namespaceDeclaration =
      source.find("engine", source.find("namespace engine"));
  expect(namespaceDefinition &&
             namespaceDefinition->target.start == namespaceDeclaration,
         "definition on an alias target should resolve its namespace");

  const std::size_t valueParameterUse = source.find("[N]") + 1;
  const std::optional<lang::DefinitionInfo> valueParameterDefinition =
      queries.definition(frontend, unit, valueParameterUse);
  const std::size_t valueParameterDeclaration =
      source.find("N>", source.find("class Fixed"));
  expect(valueParameterDefinition && valueParameterDefinition->target.start ==
                                         valueParameterDeclaration,
         "definition should connect value-generic uses to their parameter");

  const std::optional<lang::DefinitionInfo> classDefinition =
      queries.definition(frontend, unit, boxTypeUse + 1);
  const std::size_t boxClass = source.find("Box", source.find("class Box"));
  expect(classDefinition && classDefinition->target.start == boxClass,
         "definition on a declared type should target the class declaration");

  const std::optional<lang::DefinitionInfo> constructorDefinition =
      queries.definition(frontend, unit, construction + 1);
  const std::size_t boxConstructor = source.find("Box(int value)");
  expect(constructorDefinition &&
             constructorDefinition->target.start == boxConstructor,
         "definition on a construction call should target its constructor");
  expect(!queries.definition(frontend, unit,
                             source.find("return 0") +
                                 std::string("return").size()),
         "definition should fail closed when no resolved symbol is present");

  const auto complete = [&](const std::string &completionSource,
                            std::string_view prefix) {
    const std::size_t offset = completionSource.rfind(prefix);
    expect(offset != std::string::npos,
           "completion test fixture should contain its cursor prefix");
    return queries.complete({.entryPath = "completion.gti",
                             .source = completionSource,
                             .byteOffset = offset + prefix.size()});
  };
  const auto findCandidate = [](const lang::CompletionResult &completion,
                                std::string_view label) {
    return std::find_if(completion.candidates.begin(),
                        completion.candidates.end(),
                        [label](const lang::CompletionCandidate &candidate) {
                          return candidate.label == label;
                        });
  };

  const std::string localSource =
      "int calculate(int parameter) { int local = 1; int sink = loc; "
      "return parameter; }";
  const lang::CompletionResult localCompletion = complete(localSource, "loc");
  const auto local = findCandidate(localCompletion, "local");
  expect(local != localCompletion.candidates.end() &&
             local->kind == lang::CompletionCandidateKind::Variable &&
             local->detail == "int32_t local",
         "unqualified completion should use live semantic locals and types");
  expect(local != localCompletion.candidates.end() &&
             local->replacementRange.start == localSource.rfind("loc") &&
             local->replacementRange.end ==
                 localSource.rfind("loc") + std::string("loc").size(),
         "completion should replace exactly the identifier prefix");

  const std::string namespaceSource =
      "namespace math { uint64_t power(uint64_t base, uint64_t exponent) { "
      "return "
      "base; } float power(float base, float exponent) { return base; } } "
      "int main() { uint64_t result = math::po; return 0; }";
  const lang::CompletionResult namespaceCompletion =
      complete(namespaceSource, "po");
  const std::size_t powerOverloads = static_cast<std::size_t>(
      std::count_if(namespaceCompletion.candidates.begin(),
                    namespaceCompletion.candidates.end(),
                    [](const lang::CompletionCandidate &candidate) {
                      return candidate.label == "power";
                    }));
  const auto power = findCandidate(namespaceCompletion, "power");
  expect(powerOverloads == 2 && power != namespaceCompletion.candidates.end() &&
             power->detail.find("math::power") != std::string::npos &&
             power->snippet &&
             power->snippet->find("${1:base}") != std::string::npos,
         "namespace completion should preserve overloads, GTI signatures, and "
         "parameter snippets");

  const std::string incompleteDeclarationSource =
      "class Client { public: static int create() { return 1; } }; "
      "int unfinished() { auto client = Client::\n}";
  const lang::CompletionResult incompleteDeclarationCompletion =
      complete(incompleteDeclarationSource, "Client::");
  expect(findCandidate(incompleteDeclarationCompletion, "create") !=
             incompleteDeclarationCompletion.candidates.end(),
         "completion should retain a declaration whose initializer reaches "
         "the cursor before its semicolon is written");

  const std::string languageSurfaceSource =
      "interface Renderable { int render(); }; "
      "void relay<Args...>(Args... values) {} "
      "int main() { auto type = Ren; rel; return 0; }";
  const lang::CompletionResult interfaceCompletion =
      complete(languageSurfaceSource, "Ren");
  const auto renderable = findCandidate(interfaceCompletion, "Renderable");
  expect(renderable != interfaceCompletion.candidates.end() &&
             renderable->detail == "interface Renderable",
         "completion detail should preserve interface spelling");
  const lang::CompletionResult variadicCompletion =
      complete(languageSurfaceSource, "rel");
  const auto relay = findCandidate(variadicCompletion, "relay");
  expect(relay != variadicCompletion.candidates.end() &&
             relay->detail == "void relay<Args...>(Args... values)",
         "completion detail should preserve variadic-pack spelling");

  const std::string memberSource =
      "class Box { int hidden = 0; public: int read() { return this.hidden; } "
      " public: Box() = default; }; int main() { Box box{}; int result = "
      "box.rea; return 0; }";
  const lang::CompletionResult memberCompletion = complete(memberSource, "rea");
  expect(findCandidate(memberCompletion, "read") !=
                 memberCompletion.candidates.end() &&
             findCandidate(memberCompletion, "hidden") ==
                 memberCompletion.candidates.end(),
         "member completion should include accessible methods and hide private "
         "fields");

  const std::string enumSource =
      "enum class State { Ready, Running }; int main() { State state = "
      "State::Ru; return 0; }";
  const lang::CompletionResult enumCompletion = complete(enumSource, "Ru");
  const auto running = findCandidate(enumCompletion, "Running");
  expect(running != enumCompletion.candidates.end() &&
             running->kind == lang::CompletionCandidateKind::Enumerator &&
             std::all_of(enumCompletion.candidates.begin(),
                         enumCompletion.candidates.end(),
                         [](const lang::CompletionCandidate &candidate) {
                           return candidate.kind ==
                                  lang::CompletionCandidateKind::Enumerator;
                         }),
         "scoped-enum completion should filter and expose only enum members");
}

void testToolingOccurrenceOptOut() {
  const std::string source =
      "class Holder { int32_t value; public: Holder(int32_t value) : "
      "value(value) {} int32_t read() { return this.value; } };\n"
      "int main() { Holder holder{7}; return holder.read() - 7; }\n";

  const lang::FrontendResult tooling =
      lang::Frontend().analyze("occurrences-on.gti", source);
  expect(tooling.canGenerateCode(),
         "the tooling configuration should still compile");
  const lang::SourceUnitId unit = tooling.sourceGraph.entryUnit();
  expect(!tooling.semantics.database().occurrences(unit).empty(),
         "occurrences are recorded by default for editor queries");
  const std::size_t use = source.rfind("read");
  expect(tooling.semantics.database().findSymbolAt(unit, use + 1) != nullptr,
         "a recorded occurrence should resolve a symbol at its position");

  lang::FrontendOptions compileOnly;
  compileOnly.toolingOccurrences = false;
  const lang::FrontendResult compiled =
      lang::Frontend(compileOnly).analyze("occurrences-off.gti", source);
  expect(compiled.canGenerateCode(),
         "disabling occurrences must not affect code generation");
  expect(compiled.semantics.database()
             .occurrences(compiled.sourceGraph.entryUnit())
             .empty(),
         "a compile-only analysis should record no occurrences");
  expect(!compiled.semantics.database().symbols().empty(),
         "symbols stay recorded because HIR and the emitter resolve member "
         "identity through them");
  expect(compiled.semantics.expressionCount() ==
             tooling.semantics.expressionCount(),
         "occurrence recording must not change semantic analysis results");
}

} // namespace

GTI_UNIT_TEST(language_queries) {
  testLanguageQueries();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(tooling_occurrence_opt_out) {
  testToolingOccurrenceOptOut();
  return failures == 0 ? 0 : 1;
}
