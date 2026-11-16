// Source-loader and trusted-library provenance checks that need overlays of
// files under the standard-library root, configured preludes or a registered
// standard module, which a tests/ui case cannot express. (Moved unchanged
// from compiler_frontend_tests.cpp testSourceUnitDependencyGraph and
// testStandardLibraryImports; their GTI-source cases became tests/ui files.)
#include "../temporary_directory.h"
#include "gti/frontend.h"
#include "gti/source_loader.h"
#include "unit_test.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] bool
hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
              const std::string &text) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

[[maybe_unused]] bool
hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &code) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

[[maybe_unused]] const lang::Diagnostic *
findDiagnosticByCode(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code)
      return &diagnostic;
  }
  return nullptr;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                    const std::string &code) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

[[maybe_unused]] bool
hasRelatedDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      if (related.message.find(text) != std::string::npos)
        return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticHint(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const std::string &hint : diagnostic.hints) {
      if (hint.find(text) != std::string::npos)
        return true;
    }
  }
  return false;
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

void testOrderedPreludes() {
  const std::filesystem::path root =
      gti_test::temporaryDirectory() / "gti-source-graph";
  const std::filesystem::path entry = root / "main.gti";
  const auto canonical = [](const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(path).string();
  };
  const std::string entryKey = canonical(entry);

  const std::filesystem::path prelude = root / "prelude.gti";
  const std::filesystem::path preludeDetail = root / "prelude_detail.gti";
  const std::string preludeKey = canonical(prelude);
  const std::string preludeDetailKey = canonical(preludeDetail);
  const lang::FrontendResult preludeGraph = lang::Frontend().analyze(
      entry, "int main() { return prelude_value(); }\n", {prelude},
      {{preludeKey, R"GTIMODULE(import "prelude_detail.gti";
int prelude_value() { return prelude_detail(); }
)GTIMODULE"},
       {preludeDetailKey, R"GTIMODULE(export module prelude_detail;
export int prelude_detail() { return 1; }
)GTIMODULE"}});
  const lang::SourceUnitId preludeEntryId =
      preludeGraph.sourceGraph.sourceUnitForPath(entryKey);
  const lang::SourceUnitId preludeId =
      preludeGraph.sourceGraph.sourceUnitForPath(preludeKey);
  const lang::SourceUnitId preludeDetailId =
      preludeGraph.sourceGraph.sourceUnitForPath(preludeDetailKey);
  expect(preludeGraph.canGenerateCode() &&
             preludeGraph.sourceGraph.compilationOrder().size() == 3 &&
             preludeGraph.sourceGraph.isVisible(preludeEntryId, preludeId) &&
             !preludeGraph.sourceGraph.isVisible(preludeEntryId,
                                                 preludeDetailId) &&
             preludeGraph.sourceGraph.isVisible(preludeId, preludeDetailId),
         "implicit preludes should remain acyclic without re-exporting their "
         "private dependencies");

  const std::filesystem::path orderedPreludeA = root / "ordered-prelude-a.gti";
  const std::filesystem::path orderedPreludeADependency =
      root / "ordered-prelude-a-dependency.gti";
  const std::filesystem::path orderedPreludeB = root / "ordered-prelude-b.gti";
  const std::filesystem::path orderedPreludeBDependency =
      root / "ordered-prelude-b-dependency.gti";
  const std::filesystem::path orderedIncludeA = root / "ordered-a.gti";
  const std::filesystem::path orderedIncludeB = root / "ordered-b.gti";
  const std::filesystem::path orderedShared = root / "ordered-shared.gti";
  const std::string orderedPreludeAKey = canonical(orderedPreludeA);
  const std::string orderedPreludeADependencyKey =
      canonical(orderedPreludeADependency);
  const std::string orderedPreludeBKey = canonical(orderedPreludeB);
  const std::string orderedPreludeBDependencyKey =
      canonical(orderedPreludeBDependency);
  const std::string orderedIncludeAKey = canonical(orderedIncludeA);
  const std::string orderedIncludeBKey = canonical(orderedIncludeB);
  const std::string orderedSharedKey = canonical(orderedShared);
  const std::unordered_map<std::string, std::string> orderedOverrides{
      {orderedPreludeAKey,
       R"GTIMODULE(import "ordered-prelude-a-dependency.gti";
mut int32_t prelude_a = 1;
)GTIMODULE"},
      {orderedPreludeADependencyKey,
       R"GTIMODULE(export module ordered_prelude_a_dependency;
export mut int32_t prelude_a_dependency = 1;
)GTIMODULE"},
      {orderedPreludeBKey,
       R"GTIMODULE(import "ordered-prelude-b-dependency.gti";
mut int32_t prelude_b = 1;
)GTIMODULE"},
      {orderedPreludeBDependencyKey,
       R"GTIMODULE(export module ordered_prelude_b_dependency;
export mut int32_t prelude_b_dependency = 1;
)GTIMODULE"},
      {orderedIncludeAKey, R"GTIMODULE(export module ordered_a;
import "ordered-shared.gti";
export mut int32_t include_a = 1;
)GTIMODULE"},
      {orderedIncludeBKey, R"GTIMODULE(export module ordered_b;
import "ordered-shared.gti";
export mut int32_t include_b = 1;
)GTIMODULE"},
      {orderedSharedKey, R"GTIMODULE(export module ordered_shared;
export mut int32_t shared_dependency = 1;
)GTIMODULE"}};
  const std::string orderedEntryAB =
      "import \"ordered-a.gti\";\n"
      "import \"ordered-b.gti\";\n"
      "#if target.os == \"never\"\n"
      "mut int32_t inactive_target_storage = 1;\n"
      "#else\n"
      "mut int32_t active_target_storage = 1;\n"
      "#endif\n"
      "int main() { return 0; }\n";
  const std::string orderedEntryBA =
      "import \"ordered-b.gti\";\n"
      "import \"ordered-a.gti\";\n"
      "#if target.os == \"never\"\n"
      "mut int32_t inactive_target_storage = 1;\n"
      "#else\n"
      "mut int32_t active_target_storage = 1;\n"
      "#endif\n"
      "int main() { return 0; }\n";
  lang::FrontendOptions orderedOptions;
  orderedOptions.stopAfter = lang::FrontendPhase::Hir;
  const lang::FrontendResult orderedAB =
      lang::Frontend(orderedOptions)
          .analyze(entry, orderedEntryAB, {orderedPreludeA, orderedPreludeB},
                   orderedOverrides);
  const lang::FrontendResult orderedBA =
      lang::Frontend(orderedOptions)
          .analyze(entry, orderedEntryBA, {orderedPreludeB, orderedPreludeA},
                   orderedOverrides);
  const auto unitNames = [](const lang::FrontendResult &result) {
    std::vector<std::string> names;
    for (const lang::SourceUnitId id :
         result.semantics.programInitializationPlan().unitOrder) {
      const lang::SourceUnit *unit = result.sourceGraph.findUnit(id);
      names.push_back(unit == nullptr ? std::string{}
                                      : unit->path.filename().string());
    }
    return names;
  };
  const auto storageNames = [](const lang::FrontendResult &result) {
    std::vector<std::string> names;
    for (const lang::ProgramInitializationStep &step :
         result.semantics.programInitializationPlan().steps) {
      names.push_back(step.declaration == nullptr
                          ? std::string{}
                          : step.declaration->name().lexeme);
    }
    return names;
  };
  const std::vector<std::string> orderedABUnits = unitNames(orderedAB);
  const std::vector<std::string> orderedABStorage = storageNames(orderedAB);
  const std::vector<std::string> orderedBAUnits = unitNames(orderedBA);
  const std::vector<std::string> orderedBAStorage = storageNames(orderedBA);
  expect(
      orderedAB.semanticValid && orderedAB.hirValid &&
          orderedBA.semanticValid && orderedBA.hirValid &&
          orderedABUnits ==
              std::vector<std::string>{
                  "ordered-prelude-a-dependency.gti", "ordered-prelude-a.gti",
                  "ordered-prelude-b-dependency.gti", "ordered-prelude-b.gti",
                  "ordered-shared.gti", "ordered-a.gti", "ordered-b.gti",
                  "main.gti"} &&
          orderedABStorage ==
              std::vector<std::string>{"prelude_a_dependency", "prelude_a",
                                       "prelude_b_dependency", "prelude_b",
                                       "shared_dependency", "include_a",
                                       "include_b", "active_target_storage"} &&
          orderedBAUnits ==
              std::vector<std::string>{
                  "ordered-prelude-b-dependency.gti", "ordered-prelude-b.gti",
                  "ordered-prelude-a-dependency.gti", "ordered-prelude-a.gti",
                  "ordered-shared.gti", "ordered-b.gti", "ordered-a.gti",
                  "main.gti"} &&
          orderedBAStorage ==
              std::vector<std::string>{"prelude_b_dependency", "prelude_b",
                                       "prelude_a_dependency", "prelude_a",
                                       "shared_dependency", "include_b",
                                       "include_a", "active_target_storage"} &&
          std::count(orderedABUnits.begin(), orderedABUnits.end(),
                     "ordered-shared.gti") == 1 &&
          std::none_of(orderedABStorage.begin(), orderedABStorage.end(),
                       [](const std::string &name) {
                         return name == "inactive_target_storage";
                       }),
      "program initialization should use dependency-first first-visit order, "
      "configured prelude order, lexical include order, and only the active "
      "target branch");

  lang::SourceLoader loadedOrderedLoader;
  lang::SourceGraph loadedOrderedGraph = loadedOrderedLoader.load(
      entry, orderedEntryAB, {orderedPreludeA, orderedPreludeB},
      orderedOverrides);
  const lang::SourceGraph copiedOrderedGraph = loadedOrderedGraph;
  const lang::FrontendResult loadedOrdered =
      lang::Frontend(orderedOptions)
          .analyzeLoaded(entry, std::move(loadedOrderedGraph),
                         loadedOrderedLoader.sources());
  expect(!loadedOrderedLoader.hadError() &&
             copiedOrderedGraph.preludeRoots() ==
                 orderedAB.sourceGraph.preludeRoots() &&
             loadedOrdered.semanticValid && loadedOrdered.hirValid &&
             loadedOrdered.sourceGraph.preludeRoots() ==
                 orderedAB.sourceGraph.preludeRoots() &&
             loadedOrdered.semantics.programInitializationPlan().unitOrder ==
                 orderedAB.semantics.programInitializationPlan().unitOrder &&
             loadedOrdered.semantics.analysisSeal().sourceGraph ==
                 orderedAB.semantics.analysisSeal().sourceGraph,
         "SourceGraph copy and Frontend::analyzeLoaded should preserve exact "
         "ordered prelude and initialization identities");

  lang::SourceLoader reusableOrderedLoader;
  const lang::SourceGraph reusableFirst = reusableOrderedLoader.load(
      entry, orderedEntryAB, {orderedPreludeA, orderedPreludeB},
      orderedOverrides);
  const lang::SourceGraph reusableSecond =
      reusableOrderedLoader.load(entry, "int main() { return 0; }\n", {}, {});
  expect(reusableFirst.preludeRoots().size() == 2 &&
             reusableSecond.preludeRoots().empty() &&
             reusableSecond.sourceUnits().size() == 1,
         "SourceLoader reuse should clear stale configured prelude roots and "
         "source-graph state before the next load");
}

void testStandardLibraryProvenance() {
  const std::filesystem::path root =
      gti_test::temporaryDirectory() / "gti-standard-imports";
  const std::filesystem::path entry = root / "main.gti";
  const std::filesystem::path arrayUnit =
      standardLibraryRoot() / "std/array.gti";
  const std::string arrayKey =
      std::filesystem::weakly_canonical(arrayUnit).string();

  lang::SourceLoader directStandardLoader;
  const lang::SourceGraph directStandard = directStandardLoader.load(
      standardLibraryRoot() / "std/../std/array.gti",
      "namespace std { int direct_standard_entry() { return 0; } }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot() / "."});
  const lang::SourceUnitId directStandardId =
      directStandard.sourceUnitForPath(arrayKey);
  const lang::SourceUnit *directStandardUnit =
      directStandard.findUnit(directStandardId);
  expect(!directStandardLoader.hadError() && directStandardId != 0 &&
             directStandardUnit != nullptr && directStandardUnit->entry &&
             directStandardUnit->standardLibraryName == "std/array" &&
             directStandard.isCompilerTrusted(directStandardId),
         "opening a real standard-library file directly should retain its "
         "canonical logical provenance while using an editor overlay");

  const std::filesystem::path overlayUnit =
      standardLibraryRoot() / "std/icap_override_only_probe_8f6d41c5.gti";
  std::error_code overlayError;
  const bool overlayExists =
      std::filesystem::is_regular_file(overlayUnit, overlayError);
  const std::string overlayKey =
      std::filesystem::weakly_canonical(overlayUnit).string();
  lang::SourceLoader directOverlayLoader;
  const lang::SourceGraph directOverlayGraph = directOverlayLoader.load(
      overlayUnit,
      "namespace std { int direct_override_value() { return 0; } }\n", {}, {},
      {standardLibraryRoot()});
  const lang::SourceUnit *directOverlaySource =
      directOverlayGraph.findUnit(directOverlayGraph.entryUnit());
  lang::SourceLoader overlayLoader;
  const lang::SourceGraph overlayGraph = overlayLoader.load(
      gti_test::temporaryDirectory() / "gti-stdlib-overlay-main.gti",
      "import std.icap_override_only_probe_8f6d41c5;\n", {},
      {{overlayKey,
        R"GTIMODULE(export module icap_override_only_probe_8f6d41c5;
namespace std { export int override_only_value() { return 0; } }
)GTIMODULE"}},
      {standardLibraryRoot()});
  const lang::SourceUnitId overlayId =
      overlayGraph.sourceUnitForPath(overlayKey);
  expect(!overlayExists && !directOverlayLoader.hadError() &&
             directOverlaySource != nullptr &&
             !directOverlaySource->standardLibraryName.has_value() &&
             !directOverlayGraph.isCompilerTrusted(
                 directOverlayGraph.entryUnit()) &&
             overlayLoader.hadError() && overlayId == 0 &&
             hasDiagnosticCode(overlayLoader.errors(), "GTI-I0012"),
         "direct overlay paths do not mint compiler trust and unregistered "
         "standard imports cannot discover overlay files");

  const std::filesystem::path leakyUnit =
      standardLibraryRoot() / "std/array.gti";
  const std::string leakyKey =
      std::filesystem::weakly_canonical(leakyUnit).string();
  const lang::FrontendResult leakyDecomposition = lang::Frontend().analyze(
      root / "leaky-decomposition.gti",
      "import std.array;\n"
      "int main() { auto [capability] = std::Leaky(); return 0; }\n",
      {standardLibraryPrelude()},
      {{leakyKey,
        R"GTIMODULE(export module array;
import std.memory;
import "detail/memory_capabilities.gti";
namespace std { export struct Leaky { public: gti_internal::unique_owner<int> capability; }; }
)GTIMODULE"}},
      {standardLibraryRoot()});
  expect(!leakyDecomposition.canGenerateCode() &&
             countDiagnosticCode(leakyDecomposition.diagnostics, "GTI-S2058") ==
                 1 &&
             !hasDiagnostic(leakyDecomposition.diagnostics, "Unknown type"),
         "structured bindings must not launder a public field whose selected "
         "type is a compiler-private capability");

  const lang::FrontendResult leakyAlias = lang::Frontend().analyze(
      root / "leaky-alias.gti",
      "import std.array;\n"
      "std::leaked value;\nint main() { return 0; }\n",
      {standardLibraryPrelude()}, {{leakyKey, R"GTIMODULE(export module array;
import std.memory;
import "detail/memory_capabilities.gti";
namespace std { export using leaked = gti_internal::unique_owner<int>; }
)GTIMODULE"}},
      {standardLibraryRoot()});
  expect(!leakyAlias.canGenerateCode() &&
             countDiagnosticCode(leakyAlias.diagnostics, "GTI-S2058") == 1 &&
             !hasDiagnostic(leakyAlias.diagnostics, "Unknown type"),
         "trusted public aliases to private capabilities must remain hidden "
         "with the private-access diagnostic rather than an unknown-type "
         "cascade");

  const lang::FrontendResult mixedOverload = lang::Frontend().analyze(
      root / "mixed-private-overload.gti",
      "import std.array;\n"
      "int main() { std::Mixed value = std::Mixed(); "
      "return value.select(1.0); }\n",
      {standardLibraryPrelude()}, {{leakyKey, R"GTIMODULE(export module array;
import std.memory;
import "detail/memory_capabilities.gti";
namespace std { export class Mixed { public: Mixed() {} int select(int value) { return value; } int select(gti_internal::unique_owner<int> value) { return 0; } }; }
)GTIMODULE"}},
      {standardLibraryRoot()});
  // The private-signature overload is not a candidate for application code,
  // so the one public overload reports its own mismatch (issue #396).
  expect(!mixedOverload.canGenerateCode() &&
             countDiagnosticCode(mixedOverload.diagnostics, "GTI-S2003") == 1 &&
             countDiagnosticCode(mixedOverload.diagnostics, "GTI-S2012") == 0 &&
             hasRelatedDiagnostic(mixedOverload.diagnostics,
                                  "Only candidate accepting 1 argument") &&
             !hasRelatedDiagnostic(mixedOverload.diagnostics,
                                   "gti_internal::unique_owner<int>"),
         "application overload diagnostics must preserve public siblings "
         "without presenting compiler-private signature candidates");

  const lang::FrontendResult inheritedMixedOverload = lang::Frontend().analyze(
      root / "inherited-mixed-private-overload.gti",
      "import std.array;\n"
      "class Derived : public std::PrivateBase { public: "
      "int select(int value) { return value; }  public: Derived() = default; "
      "};\n"
      "int main() { Derived value = Derived(); return value.select(7); }\n",
      {standardLibraryPrelude()},
      {{leakyKey,
        R"GTIMODULE(export module array;
import std.memory;
import "detail/memory_capabilities.gti";
namespace std { export class PrivateBase { public: PrivateBase() {} int select(gti_internal::unique_owner<int> value) { return 0; } }; }
)GTIMODULE"}},
      {standardLibraryRoot()});
  expect(inheritedMixedOverload.canGenerateCode(),
         "a public overload added by a derived application class must remain "
         "available when a trusted base contributes a private-signature "
         "sibling");

  const lang::FrontendResult privateMemberOccurrence = lang::Frontend().analyze(
      root / "private-member-occurrence.gti",
      "import std.array;\n"
      "int main() { std::PrivateMethod value = std::PrivateMethod(); "
      "return value.secret(1); }\n",
      {standardLibraryPrelude()},
      {{leakyKey,
        R"GTIMODULE(export module array;
import std.memory;
import "detail/memory_capabilities.gti";
namespace std { export class PrivateMethod { public: PrivateMethod() {} int secret(gti_internal::unique_owner<int> value) { return 0; } }; }
)GTIMODULE"}},
      {standardLibraryRoot()});
  const lang::SourceUnitId privateMemberUnit =
      privateMemberOccurrence.sourceGraph.entryUnit();
  const auto &privateMemberOccurrences =
      privateMemberOccurrence.semantics.database().occurrences(
          privateMemberUnit);
  const auto rejectedMember = std::find_if(
      privateMemberOccurrences.begin(), privateMemberOccurrences.end(),
      [](const lang::SemanticOccurrence &occurrence) {
        return occurrence.name == "secret";
      });
  expect(!privateMemberOccurrence.canGenerateCode() &&
             countDiagnosticCode(privateMemberOccurrence.diagnostics,
                                 "GTI-S2058") >= 1 &&
             (rejectedMember == privateMemberOccurrences.end() ||
              rejectedMember->symbol == 0),
         "rejected private-signature member occurrences must remain hidden "
         "from compiler-owned tooling queries");

  const lang::FrontendResult imported = lang::Frontend().analyze(
      entry, "int main() { return 0; }\n", {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  const std::filesystem::path wrapper =
      standardLibraryRoot() / "std/import_test_wrapper.gti";
  const std::string wrapperKey =
      std::filesystem::weakly_canonical(wrapper).string();
  auto fixtureStandard =
      *imported.sourceGraph.sourceOwners()->findImplicitAlias("std");
  fixtureStandard.modules.emplace_back("import_test_wrapper", wrapper);
  const lang::FrontendResult hidden = lang::Frontend().analyze(
      entry,
      "import std.import_test_wrapper;\n"
      "std::array<int, 1> hidden = std::array<int, 1>();\n"
      "int main() { return std::import_test_value(); }\n",
      {standardLibraryPrelude()},
      {{wrapperKey,
        R"GTIMODULE(export module import_test_wrapper;
import std.array;
namespace std { export int import_test_value() { return 0; } }
)GTIMODULE"}},
      {standardLibraryRoot()}, {fixtureStandard});
  expect(!hidden.semanticValid &&
             hasDiagnosticCode(hidden.diagnostics, "GTI-S2024") &&
             hasDiagnosticHint(hidden.diagnostics, "import std.array;"),
         "standard imports should remain private and provide logical import "
         "hints");

  const std::filesystem::path keywordUnit =
      standardLibraryRoot() / "std/char_unit.gti";
  const std::string keywordKey =
      std::filesystem::weakly_canonical(keywordUnit).string();
  fixtureStandard.modules.emplace_back("char_unit", keywordUnit);
  const lang::FrontendResult keywordPath = lang::Frontend().analyze(
      entry,
      "import std.char_unit;\n"
      "int main() { return std::keyword_path_value(); }\n",
      {standardLibraryPrelude()},
      {{keywordKey,
        R"GTIMODULE(export module char_unit;
namespace std { export int keyword_path_value() { return 0; } }
)GTIMODULE"}},
      {standardLibraryRoot()}, {fixtureStandard});
  expect(keywordPath.canGenerateCode(),
         "standard import components resolve legal names derived from GTI "
         "keywords");

  const std::filesystem::path cppKeywordUnit =
      standardLibraryRoot() / "std/template_unit.gti";
  const std::string cppKeywordKey =
      std::filesystem::weakly_canonical(cppKeywordUnit).string();
  fixtureStandard.modules.emplace_back("template_unit", cppKeywordUnit);
  const lang::FrontendResult cppKeywordPath = lang::Frontend().analyze(
      entry,
      "import std.template_unit;\n"
      "int main() { return std::cpp_keyword_path_value(); }\n",
      {standardLibraryPrelude()},
      {{cppKeywordKey,
        R"GTIMODULE(export module template_unit;
namespace std { export int cpp_keyword_path_value() { return 0; } }
)GTIMODULE"}},
      {standardLibraryRoot()}, {fixtureStandard});
  expect(cppKeywordPath.canGenerateCode(),
         "standard import components resolve legal names derived from C++ "
         "keywords");
}

} // namespace

GTI_UNIT_TEST(source_loader_provenance) {
  testOrderedPreludes();
  testStandardLibraryProvenance();
  return failures == 0 ? 0 : 1;
}
