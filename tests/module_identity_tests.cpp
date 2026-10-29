#include "gti/frontend.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void expectValid(const lang::FrontendResult &result, const char *message) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.message << '\n';
  expect(result.sourceValid && result.syntaxValid && result.semanticValid,
         message);
}
} // namespace

int main() {
  const auto root = std::filesystem::current_path() / "module-identity-fixture";
  const auto entry = root / "main.gti";
  const auto left = root / "left.gti";
  const auto right = root / "right.gti";
  const auto body = root / "body.gti";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  options.suggestIncludes = false;
  options.cache = std::make_shared<lang::FrontendCache>();
  std::unordered_map<std::string, std::string> sources{
      {left.string(), R"GTI(export module left;
using Number = int;
struct Worker { Number value; };
int helper() { Worker value{.value = 20}; return value.value; }
export int first() { return helper(); }
)GTI"},
      {right.string(), R"GTI(export module right;
using Number = int;
struct Worker { Number value; };
int helper() { Worker value{.value = 22}; return value.value; }
export int second() { return helper(); }
)GTI"}};
  const std::string entrySource = "import \"left.gti\"; import \"right.gti\"; "
                                  "int main() { return first() + second(); }";
  for (int pass = 0; pass != 2; ++pass)
    expectValid(
        lang::Frontend(options).analyze(entry, entrySource, {}, sources),
        "private types, aliases and functions have module identity");

  sources[left.string()] = R"GTI(export module left;
export struct Worker { int value; int read() { return value; } };
export Worker first() { Worker result{.value = 20}; return result; }
)GTI";
  sources[right.string()] = R"GTI(export module right;
export struct Worker { int value; int read() { return value; } };
export Worker second() { Worker result{.value = 22}; return result; }
)GTI";
  expectValid(lang::Frontend(options).analyze(
                  entry,
                  "import \"left.gti\"; import \"right.gti\"; int main() { "
                  "auto a = first(); auto b = second(); "
                  "return a.read() + b.read(); }",
                  {}, sources),
              "same-spelled exported types remain distinct and unused "
              "ambiguous names are legal");
  const auto ambiguous = lang::Frontend(options).analyze(
      entry,
      "import \"left.gti\"; import \"right.gti\"; int main() { Worker "
      "value{.value = 1}; "
      "return value.read(); }",
      {}, sources);
  expect(!ambiguous.semanticValid &&
             std::any_of(ambiguous.diagnostics.begin(),
                         ambiguous.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2006" &&
                                  diagnostic.related.size() >= 2;
                         }),
         "using an ambiguous type names both candidate declarations");
  const auto nominal = lang::Frontend(options).analyze(
      entry,
      "import \"left.gti\"; import \"right.gti\"; int main() { auto a = "
      "first(); "
      "auto b = second(); a = b; return 0; }",
      {}, sources);
  expect(!nominal.semanticValid,
         "equal type spelling must not collapse module-owned nominal identity");
  sources[right.string()] = "export module right; export using Worker = int;";
  const auto categoryConflict = lang::Frontend(options).analyze(
      entry,
      "import \"left.gti\"; import \"right.gti\"; int main() { Worker "
      "value{.value = 1}; return 0; }",
      {}, sources);
  expect(!categoryConflict.semanticValid &&
             std::any_of(categoryConflict.diagnostics.begin(),
                         categoryConflict.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2006";
                         }),
         "visible aliases and nominal declarations cannot silently shadow each "
         "other");
  sources[left.string()] =
      "export module left; export int choose(int value) { return value; }";
  sources[right.string()] =
      "export module right; export int choose(bool value) { return 1; }";
  const std::string overloadConsumer =
      "import \"left.gti\"; import \"right.gti\"; int main() { return "
      "choose(42); }";
  expectValid(
      lang::Frontend(options).analyze(entry, overloadConsumer, {}, sources),
      "distinct visible function signatures compose across modules");
  sources[right.string()] =
      "export module right; export int choose(int value) { return value; }";
  expectValid(
      lang::Frontend(options).analyze(
          entry,
          "import \"left.gti\"; import \"right.gti\"; int main() { return 0; }",
          {}, sources),
      "unused same-signature exported functions do not conflict at import "
      "time");
  const auto ambiguousCall =
      lang::Frontend(options).analyze(entry, overloadConsumer, {}, sources);
  expect(!ambiguousCall.semanticValid &&
             std::any_of(ambiguousCall.diagnostics.begin(),
                         ambiguousCall.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2013" &&
                                  diagnostic.related.size() >= 2;
                         }),
         "same-signature functions from distinct modules stay distinct "
         "candidates at a call");

  sources[left.string()] = R"GTI(export module left;
struct Hidden { int value; int read() { return value; } };
int helper(int value) { return value; }
export Hidden make() { Hidden result{.value = 42}; return result; }
export int operation(int value = helper(42));
)GTI";
  sources[body.string()] = R"GTI(module left;
int operation(int value) { return helper(value); }
)GTI";
  lang::SourceOwnerDescriptor owner{
      .identity = "fixture",
      .name = "fixture",
      .packageRoot = root,
      .sourceRoot = root,
      .modules = {{"left", left}, {"right", right}},
      .moduleImplementations = {{"left", {body}}}};
  const std::string consumer = "import left; int main() { auto value = make(); "
                               "return value.read() + operation(); }";
  for (int pass = 0; pass != 2; ++pass) {
    const auto result = lang::Frontend(options).analyze(entry, consumer, {},
                                                        sources, {}, {owner});
    expectValid(
        result,
        "registered bodies and private return/default types remain reachable");
    const auto interfaceId =
        result.sourceGraph.sourceUnitForPath(left.string());
    const auto implementationId =
        result.sourceGraph.sourceUnitForPath(body.string());
    expect(result.sourceGraph.sameModule(interfaceId, implementationId),
           "a registered body shares the interface identity");
  }
  const auto inaccessible = lang::Frontend(options).analyze(
      entry, "import left; int main() { return helper(42); }", {}, sources, {},
      {owner});
  expect(
      !inaccessible.semanticValid,
      "implementation membership must not expose private helpers to importers");

  sources[body.string()] =
      "module left; bool operation(int value) { return true; }";
  const auto mismatched = lang::Frontend(options).analyze(entry, consumer, {},
                                                          sources, {}, {owner});
  expect(!mismatched.semanticValid &&
             std::any_of(mismatched.diagnostics.begin(),
                         mismatched.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2011" &&
                                  !diagnostic.related.empty();
                         }),
         "a body must preserve the declared return type with related "
         "declaration evidence");
  sources[left.string()] = R"GTI(export module left;
export struct Box<T, uint64_t Count> {
  T value;
  T read();
  uint64_t count();
  U convert<U>(U input);
};
)GTI";
  sources[body.string()] = R"GTI(module left;
U Box<U, N>::read() { return value; }
uint64_t Box<U, N>::count() { return N; }
V Box<U, N>::convert<V>(V result) { return result; }
)GTI";
  const std::string genericConsumer =
      "import left; int main() { Box<int, 3> value{.value = 42}; "
      "return value.read() + int(value.count()) + value.convert(0) - 45; }";
  for (int pass = 0; pass != 2; ++pass)
    expectValid(lang::Frontend(options).analyze(entry, genericConsumer, {},
                                                sources, {}, {owner}),
                "generic class and method bodies preserve canonical owner and "
                "method parameters");
  sources[body.string()] = R"GTI(module left;
bool Box<U, N>::read() { return true; }
)GTI";
  const auto genericMismatch = lang::Frontend(options).analyze(
      entry, genericConsumer, {}, sources, {}, {owner});
  expect(
      !genericMismatch.semanticValid &&
          std::any_of(genericMismatch.diagnostics.begin(),
                      genericMismatch.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2120";
                      }),
      "generic method definitions must preserve their declared return types");
  for (const auto *definition :
       {"U Box<U>::read() { return value; }",
        "int Box<int, N>::read() { return 0; }",
        "U Box<U&, N>::read() { return value; }",
        "U Box<U, N>::convert<U>(U input) { return input; }",
        "V Box<U, N>::convert<V = int>(V input) { return input; }"}) {
    sources[body.string()] = "module left; " + std::string(definition);
    const auto invalid = lang::Frontend(options).analyze(
        entry, "import left; int main() { return 0; }", {}, sources, {},
        {owner});
    expect(!invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2120" &&
                                    !diagnostic.primary.source.empty();
                           }),
           "generic definitions reject incomplete, concrete, patterned, "
           "shadowed and redeclared-default bindings");
  }
  sources[left.string()] = R"GTI(export module left;
export namespace api {
class Key<T> { public: Key() = default; int code() { return 0; } };
concept Counted<T> { int count(T& value); };
}
)GTI";
  sources[body.string()] = R"GTI(module left;
class api::Key<int> { public: Key() = default; int code() { return 42; } };
for concept api::Counted<int> { int count(int& value) { return value; } };
)GTI";
  const std::string coherentConsumer =
      "import left; int main() { int value = 42; api::Key<int> key{}; "
      "return key.code() + api::Counted<int>::count(value) - 84; }";
  expectValid(lang::Frontend(options).analyze(entry, coherentConsumer, {},
                                              sources, {}, {owner}),
              "registered primary/concept implementations own canonical "
              "specializations and conformances");
  sources[right.string()] = R"GTI(export module right;
import left;
class api::Key<int> { public: Key() = default; int code() { return 42; } };
for concept api::Counted<int> { int count(int& value) { return value; } };
)GTI";
  owner.moduleImplementations.clear();
  const auto foreignModule = lang::Frontend(options).analyze(
      entry, "import right; int main() { return 0; }", {}, sources, {},
      {owner});
  for (const auto *code : {"GTI-S2078", "GTI-S2123"})
    expect(std::any_of(
               foreignModule.diagnostics.begin(),
               foreignModule.diagnostics.end(),
               [&](const auto &diagnostic) { return diagnostic.code == code; }),
           "a different module in the same package cannot own a foreign "
           "primitive contract");

  const auto application = root / "application";
  const auto model = application / "model.gti";
  lang::SourceOwnerDescriptor appOwner{.identity = "application",
                                       .name = "application",
                                       .packageRoot = application,
                                       .sourceRoot = application,
                                       .dependencies = {{"lib", "fixture"}},
                                       .modules = {{"model", model}}};
  sources[model.string()] = R"GTI(export module model;
export import lib.left;
export struct Payload { int value; };
class api::Key<Payload> { public: Key() = default; int code() { return 42; } };
for concept api::Counted<Payload> {
  int count(Payload& value) { return value.value; }
};
)GTI";
  expectValid(lang::Frontend(options).analyze(
                  application / "main.gti",
                  "import model; int main() { Payload value{.value = 42}; "
                  "api::Key<Payload> key{}; "
                  "return key.code() + "
                  "api::Counted<Payload>::count(value) - 84; }",
                  {}, sources, {}, {owner, appOwner}),
              "an unrelated nominal argument owner can customize a foreign "
              "package's template and concept");
  std::cout << "module semantic identity checks passed\n";
}
