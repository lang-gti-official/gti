#include "gti/documentation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryStandardLibrary {
public:
  TemporaryStandardLibrary()
      : root(std::filesystem::temp_directory_path() /
             ("gti-documentation-index-" +
              std::to_string(std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count()))) {
    std::filesystem::create_directories(root / "std");
    write(root / "prelude.gti", R"(
namespace std {
// The synthetic fixture exercises concept presentation, not the real
// standard library's complete construction/assignment capability policy.
concept copyable<T> = requires(T& source) { { T(source) } -> T; };
}
)");
    write(root / "std/sample.gti", R"(
namespace std {
/// Requires a copyable value.
concept sample_copyable<T> = std::copyable<T>;

/// A "quoted" widget.
/// Second line.
class widget {
  int hidden = 0;

public:
  /// Constructs a widget.
  widget(int value) : hidden(value) {}

  /// Returns the stored value.
  int value() {
    return this.hidden;
  }

  int operator[](int index) {
    return index;
  }

  implicit operator int64_t() {
    return int64_t(this.hidden);
  }

private:
  /// This must not be published.
  int secret = 1;
};

/// A move-only documented owner.
class owner {
public:
  owner(owner& other) = delete;
  owner(owner&& other) = default;
};

/// Doubles a value.
int twice(int value) {
  return value * 2;
}
}
)");
  }

  ~TemporaryStandardLibrary() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  TemporaryStandardLibrary(const TemporaryStandardLibrary &) = delete;
  TemporaryStandardLibrary &
  operator=(const TemporaryStandardLibrary &) = delete;

  std::filesystem::path root;

private:
  static void write(const std::filesystem::path &path,
                    std::string_view contents) {
    std::ofstream output(path, std::ios::binary);
    output << contents;
  }
};

lang::FrontendResult analyze(const TemporaryStandardLibrary &library,
                             bool toolingOccurrences) {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  options.toolingOccurrences = toolingOccurrences;
  return lang::Frontend(options).analyze(library.root / "reference-entry.gti",
                                         R"(
#include <std/sample>
int main() {
  std::widget item{3};
  return std::twice(item.value());
}
)",
                                         {library.root / "prelude.gti"}, {},
                                         {library.root});
}

const lang::DocumentationDeclaration *
findDeclaration(const lang::DocumentationIndex &index,
                std::string_view qualifiedName, lang::DocumentationKind kind) {
  const auto found = std::find_if(
      index.declarations.begin(), index.declarations.end(),
      [&](const lang::DocumentationDeclaration &entry) {
        return entry.qualifiedName == qualifiedName && entry.kind == kind;
      });
  return found == index.declarations.end() ? nullptr : &*found;
}

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

} // namespace

int main() {
  const TemporaryStandardLibrary library;
  const lang::FrontendResult first = analyze(library, true);
  if (!first.semanticValid || !first.diagnostics.empty()) {
    printDiagnostics(first);
  }
  expect(first.sourceValid && first.syntaxValid && first.semanticValid &&
             first.diagnostics.empty(),
         "the synthetic standard library should complete semantic analysis");

  const lang::DocumentationIndexResult projected =
      lang::DocumentationQueries().standardLibraryIndex(first);
  for (const std::string &error : projected.errors) {
    std::cerr << "documentation index: " << error << '\n';
  }
  expect(projected.valid(),
         "a valid tooling snapshot should produce a documentation index");
  expect(!projected.index.declarations.empty(),
         "the documentation index should contain public library declarations");

  const lang::DocumentationDeclaration *widget = findDeclaration(
      projected.index, "std::widget", lang::DocumentationKind::Class);
  expect(widget != nullptr, "the public class should be indexed");
  expect(widget != nullptr && widget->signature == "class std::widget",
         "class signatures should come from the semantic signature printer");
  expect(widget != nullptr && widget->documentationMarkdown ==
                                  "A \"quoted\" widget.\nSecond line.",
         "declaration documentation should cross the projection boundary");
  expect(widget != nullptr && widget->source.unit == "std/sample" &&
             widget->source.line > 1,
         "source locations should use stable standard-library identities");
  expect(widget != nullptr && widget->traits &&
             widget->traits->ownership == "value",
         "type traits should be projected as presentation data");

  const lang::DocumentationDeclaration *owner = findDeclaration(
      projected.index, "std::owner", lang::DocumentationKind::Class);
  expect(owner != nullptr && owner->traits && !owner->traits->copyable &&
             owner->traits->movable,
         "class traits should use effective lifecycle policy rather than "
         "the declaration symbol's provisional field traits");

  const lang::DocumentationDeclaration *conceptEntry =
      findDeclaration(projected.index, "std::sample_copyable",
                      lang::DocumentationKind::Concept);
  expect(conceptEntry != nullptr &&
             conceptEntry->signature == "concept std::sample_copyable<T> = "
                                        "std::copyable<T>" &&
             conceptEntry->documentationMarkdown ==
                 "Requires a copyable value.",
         "concepts should retain their exact source composition and docs");

  const lang::DocumentationDeclaration *method = findDeclaration(
      projected.index, "std::widget::value", lang::DocumentationKind::Method);
  expect(method != nullptr &&
             method->signature == "int32_t std::widget::value()" &&
             method->ownerQualifiedName == "std::widget",
         "member entries should retain their owner and canonical signature");

  const lang::DocumentationDeclaration *constructor =
      findDeclaration(projected.index, "std::widget::widget",
                      lang::DocumentationKind::Constructor);
  expect(constructor != nullptr &&
             constructor->signature == "std::widget(int32_t value)" &&
             constructor->ownerQualifiedName == "std::widget",
         "constructors should use the semantic constructor signature");

  const lang::DocumentationDeclaration *subscript =
      findDeclaration(projected.index, "std::widget::operator[]",
                      lang::DocumentationKind::Operator);
  expect(subscript != nullptr && subscript->name == "operator[]" &&
             subscript->ownerQualifiedName == "std::widget",
         "operator identities should use source-facing names, not compiler "
         "mangling names");
  const lang::DocumentationDeclaration *conversion =
      findDeclaration(projected.index, "std::widget::operator int64_t",
                      lang::DocumentationKind::Operator);
  expect(conversion != nullptr && conversion->name == "operator int64_t" &&
             conversion->signature &&
             *conversion->signature ==
                 "implicit std::widget::operator int64_t()",
         "a conversion operator is named by its target and keeps `implicit` "
         "in its signature");

  const lang::DocumentationDeclaration *function = findDeclaration(
      projected.index, "std::twice", lang::DocumentationKind::Function);
  expect(function != nullptr &&
             function->signature == "int32_t std::twice(int32_t value)" &&
             function->documentationMarkdown == "Doubles a value.",
         "free functions should retain signatures and documentation");

  expect(
      findDeclaration(projected.index, "std::widget::hidden",
                      lang::DocumentationKind::Field) == nullptr &&
          findDeclaration(projected.index, "std::widget::secret",
                          lang::DocumentationKind::Field) == nullptr,
      "private class state must not cross the public documentation boundary");
  expect(std::none_of(projected.index.declarations.begin(),
                      projected.index.declarations.end(),
                      [](const lang::DocumentationDeclaration &entry) {
                        return entry.qualifiedName == "main";
                      }),
         "application declarations must not enter the standard-library index");

  const lang::FrontendResult second = analyze(library, true);
  const lang::DocumentationIndexResult projectedAgain =
      lang::DocumentationQueries().standardLibraryIndex(second);
  const std::string firstJson =
      lang::serializeDocumentationIndexJson(projected.index);
  const std::string secondJson =
      lang::serializeDocumentationIndexJson(projectedAgain.index);
  expect(projectedAgain.valid() && firstJson == secondJson,
         "equal source snapshots should serialize byte-identically");
  expect(firstJson.find("\"schema\": \"gti.documentation-index\"") !=
                 std::string::npos &&
             firstJson.find("A \\\"quoted\\\" widget.\\nSecond line.") !=
                 std::string::npos,
         "the JSON should name its schema and escape documentation text");
  expect(firstJson.find(library.root.generic_string()) == std::string::npos &&
             firstJson.find("symbolId") == std::string::npos,
         "serialized output must omit machine paths and snapshot identities");

  const lang::FrontendResult withoutOccurrences = analyze(library, false);
  const lang::DocumentationIndexResult rejected =
      lang::DocumentationQueries().standardLibraryIndex(withoutOccurrences);
  expect(!rejected.valid(),
         "the query should fail closed when exact tooling occurrences were "
         "disabled");

  lang::FrontendResult invalid;
  expect(!lang::DocumentationQueries().standardLibraryIndex(invalid).valid(),
         "an invalid frontend snapshot should be rejected");

  if (failures != 0) {
    std::cerr << failures << " documentation-index test(s) failed\n";
    return 1;
  }
  std::cout << "documentation index tests passed\n";
  return 0;
}
