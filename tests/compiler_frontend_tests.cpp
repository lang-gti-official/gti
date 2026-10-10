#include "compiler_test_fixture.h"
#include "native_symbol_spelling.h"

#include <set>

namespace {

void testDefiniteReturnAnalysis() {
  const lang::FrontendResult valid =
      lang::Frontend().analyze("definite-return.gti", R"(
int choose(bool first) {
  if (first) {
    return 1;
  } else {
    return 2;
  }
}

int literal_branch() {
  if (true) {
    return 3;
  }
}

int spin() {
  while (true) {}
}

int iterate() {
  for (;;) {
    continue;
  }
}

int post_test_spin() {
  do {
    continue;
  } while (true);
}

int post_test_return() {
  do {
    return 4;
  } while (false);
}

int selected_target() {
#if target.os == "never"
  int inactive = 0;
#else
  return 3;
#endif
}

void observe() {}

int main() {}
)");
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "complete branches, proven infinite loops, void functions, and "
         "main fallthrough should pass return analysis");

  const lang::FrontendResult invalidMainReturn =
      lang::Frontend().analyze("main-return.gti", "void main() {}\n");
  const lang::FrontendResult invalidMainParameters = lang::Frontend().analyze(
      "main-parameters.gti", "int main(int value) { return value; }\n");
  const lang::FrontendResult missingMainBody =
      lang::Frontend().analyze("main-body.gti", "int main();\n");
  expect(
      !invalidMainReturn.canGenerateCode() &&
          !invalidMainParameters.canGenerateCode() &&
          !missingMainBody.canGenerateCode() &&
          hasDiagnosticCode(invalidMainReturn.diagnostics, "GTI-S2032") &&
          hasDiagnosticCode(invalidMainParameters.diagnostics, "GTI-S2032") &&
          hasDiagnosticCode(missingMainBody.diagnostics, "GTI-S2032"),
      "the frontend should reject return, parameter, and definition shapes "
      "outside the two supported main entry contracts");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("missing-return.gti", R"(
int branch(bool value) {
  if (value) {
    return 1;
  }
}

int conditional_loop(bool value) {
  while (value) {
    return 1;
  }
}

int breakable_loop(bool leave) {
  while (true) {
    if (leave) {
      break;
    }
  }
}

int post_test_once() {
  do {
    continue;
  } while (false);
}

expected<void, int> expected_result(bool ready) {
  if (ready) {
    return;
  }
}

int missing_target_return() {
#if target.os == "never"
  return 1;
#else
  int selected = 1;
#endif
}

class Broken {
public:
  int value() {}
};

int main() {
  auto incomplete = [](bool ready) -> int {
    if (ready) {
      return 1;
    }
  };
  return 0;
}
)");
  expect(!invalid.canGenerateCode() &&
             countDiagnosticCode(invalid.diagnostics, "GTI-S2031") == 8 &&
             hasDiagnostic(invalid.diagnostics,
                           "can reach the end without returning a value"),
         "every non-void function, method, expected result, and lambda path "
         "should return before backend entry");
}

void testSourceUnitDependencyGraph() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-source-graph";
  const std::filesystem::path entry = root / "main.gti";
  const std::filesystem::path branch = root / "branch.gti";
  const std::filesystem::path leaf = root / "leaf.gti";
  const auto canonical = [](const std::filesystem::path &path) {
    return std::filesystem::weakly_canonical(path).string();
  };
  const std::string entryKey = canonical(entry);
  const std::string branchKey = canonical(branch);
  const std::string leafKey = canonical(leaf);

  lang::Lexer includeLexer;
  const std::vector<lang::Token> includeTokens =
      includeLexer.scan("#include \"branch.gti\"\n");
  expect(!includeLexer.hadError() && includeTokens.size() >= 3 &&
             includeTokens.front().kind == lang::TokenKind::HASH_INCLUDE,
         "#include should have a dedicated directive token");
  const std::vector<lang::Token> identifierTokens =
      includeLexer.scan("int include = 1;\n");
  expect(!includeLexer.hadError() && identifierTokens.size() >= 3 &&
             identifierTokens[1].kind == lang::TokenKind::IDENTIFIER,
         "plain include should remain available as an identifier");
  const lang::FrontendResult includeIdentifier =
      lang::Frontend().analyze(entry, "int include = 1;\n"
                                      "int main() { return include - 1; }\n");
  expect(includeIdentifier.canGenerateCode(),
         "plain include should not remain reserved by source loading");

  const lang::FrontendResult sameUnitStaticAssert = lang::Frontend().analyze(
      entry, "constexpr int32_t limit = 8;\n"
             "static_assert(limit == 8, \"limit must be 8\");\n"
             "int main() { return limit - 8; }\n");
  expect(sameUnitStaticAssert.canGenerateCode() &&
             sameUnitStaticAssert.diagnostics.empty(),
         "a file-scope static assertion should resolve constants declared in "
         "its own source unit");

  const lang::FrontendResult legacyInclude = lang::Frontend().analyze(
      entry,
      "include \"branch.gti\"\n"
      "int main() { return 0; }\n",
      {}, {{branchKey, "int branch_value() { return 1; }\n"}});
  expect(!legacyInclude.sourceValid &&
             countDiagnosticCode(legacyInclude.diagnostics, "GTI-I0009") == 1 &&
             hasDiagnostic(legacyInclude.diagnostics, "use '#include'"),
         "the removed include spelling should receive one migration "
         "diagnostic");

  const lang::FrontendResult valid = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "int main() { return branch_value(); }\n",
      {},
      {{branchKey, "#include \"leaf.gti\"\n"
                   "int branch_value() { return leaf_value(); }\n"},
       {leafKey, "int leaf_value() { return 1; }\n"}});
  expect(valid.canGenerateCode() && valid.diagnostics.empty(),
         "direct source dependencies should be visible within each unit");
  expect(valid.sourceGraph.sourceUnits().size() == 3 &&
             valid.sourceGraph.dependencyEdges().size() == 2,
         "the frontend should retain source units and explicit include edges");
  bool dependencySpansRetained = true;
  for (const lang::SourceDependency &dependency :
       valid.sourceGraph.dependencyEdges()) {
    dependencySpansRetained =
        dependencySpansRetained &&
        dependency.kind == lang::SourceDependencyKind::Include &&
        dependency.directive.has_value();
  }
  expect(dependencySpansRetained,
         "explicit dependency edges should retain their include locations");

  const lang::SourceUnitId entryId =
      valid.sourceGraph.sourceUnitForPath(entryKey);
  const lang::SourceUnitId branchId =
      valid.sourceGraph.sourceUnitForPath(branchKey);
  const lang::SourceUnitId leafId =
      valid.sourceGraph.sourceUnitForPath(leafKey);
  expect(entryId != 0 && branchId != 0 && leafId != 0 &&
             valid.sourceGraph.entryUnit() == entryId &&
             valid.sourceGraph.isVisible(entryId, branchId) &&
             !valid.sourceGraph.isVisible(entryId, leafId) &&
             valid.sourceGraph.isVisible(branchId, leafId),
         "source visibility should include only the current and direct units");
  const lang::SourceUnit *entryUnit = valid.sourceGraph.findUnit(entryId);
  const lang::SourceUnit *branchUnit = valid.sourceGraph.findUnit(branchId);
  const lang::SourceUnit *leafUnit = valid.sourceGraph.findUnit(leafId);
  expect(entryUnit != nullptr && branchUnit != nullptr && leafUnit != nullptr &&
             entryUnit->declarationCount == 1 &&
             branchUnit->declarationCount == 1 &&
             leafUnit->declarationCount == 1,
         "independently parsed units should retain their program ranges");

  const lang::FunctionDecl *leafFunction =
      findTopLevelFunction(valid.program, "leaf_value");
  const lang::FunctionInfo *leafInfo =
      leafFunction == nullptr ? nullptr
                              : valid.semantics.findFunction(*leafFunction);
  bool hirRetainsLeafUnit = false;
  if (leafInfo != nullptr) {
    for (const lang::HirFunctionInstance &instance :
         valid.hir.functionInstances()) {
      if (instance.declaration == leafInfo->id) {
        hirRetainsLeafUnit = instance.sourceUnit == leafId;
      }
    }
  }
  expect(leafInfo != nullptr && leafInfo->sourceUnit == leafId &&
             hirRetainsLeafUnit,
         "semantic and HIR function identities should retain source units");

  const lang::FrontendResult transitiveUse = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "int main() { return leaf_value(); }\n",
      {},
      {{branchKey, "#include \"leaf.gti\"\n"
                   "int branch_value() { return leaf_value(); }\n"},
       {leafKey, "int leaf_value() { return 1; }\n"}});
  expect(
      !transitiveUse.semanticValid &&
          hasDiagnosticCode(transitiveUse.diagnostics, "GTI-S2024") &&
          hasDiagnostic(transitiveUse.diagnostics,
                        "include its file directly") &&
          hasDiagnosticHint(transitiveUse.diagnostics, "#include \"leaf.gti\""),
      "an includer should not inherit a dependency's private includes");

  const lang::FrontendResult hiddenStaticAssert = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "static_assert(leaf_limit == 8, \"limit must be 8\");\n"
      "int main() { return 0; }\n",
      {},
      {{branchKey, "#include \"leaf.gti\"\n"},
       {leafKey, "constexpr int32_t leaf_limit = 8;\n"}});
  expect(!hiddenStaticAssert.semanticValid &&
             hasDiagnosticCode(hiddenStaticAssert.diagnostics, "GTI-S2024") &&
             hasDiagnosticHint(hiddenStaticAssert.diagnostics,
                               "#include \"leaf.gti\""),
         "file-scope static assertions should retain direct-include "
         "visibility diagnostics for truly hidden declarations");

  const lang::FrontendResult directAlias = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "BranchId id = BranchId(1);\n"
      "int main() { return 0; }\n",
      {}, {{branchKey, "using BranchId = uint64_t;\n"}});
  expect(directAlias.canGenerateCode() && directAlias.diagnostics.empty(),
         "type aliases from a direct dependency should be visible");

  const lang::FrontendResult hiddenAlias =
      lang::Frontend().analyze(entry,
                               "#include \"branch.gti\"\n"
                               "LeafId id = LeafId(1);\n"
                               "int main() { return 0; }\n",
                               {},
                               {{branchKey, "#include \"leaf.gti\"\n"},
                                {leafKey, "using LeafId = uint64_t;\n"}});
  expect(
      !hiddenAlias.semanticValid &&
          hasDiagnosticCode(hiddenAlias.diagnostics, "GTI-S2024") &&
          hasDiagnosticHint(hiddenAlias.diagnostics, "#include \"leaf.gti\""),
      "type aliases should follow direct source visibility rules");

  const lang::FrontendResult siblingLeak = lang::Frontend().analyze(
      entry,
      "#include \"branch.gti\"\n"
      "#include \"leaf.gti\"\n"
      "int main() { return branch_value(); }\n",
      {},
      {{branchKey, "int branch_value() { return leaf_value(); }\n"},
       {leafKey, "int leaf_value() { return 1; }\n"}});
  expect(
      !siblingLeak.semanticValid &&
          hasDiagnosticCode(siblingLeak.diagnostics, "GTI-S2024") &&
          hasDiagnostic(siblingLeak.diagnostics, "include its file directly"),
      "a source unit should not see dependencies included by its parent");

  const std::filesystem::path prelude = root / "prelude.gti";
  const std::filesystem::path preludeDetail = root / "prelude_detail.gti";
  const std::string preludeKey = canonical(prelude);
  const std::string preludeDetailKey = canonical(preludeDetail);
  const lang::FrontendResult preludeGraph = lang::Frontend().analyze(
      entry, "int main() { return prelude_value(); }\n", {prelude},
      {{preludeKey, "#include \"prelude_detail.gti\"\n"
                    "int prelude_value() { return prelude_detail(); }\n"},
       {preludeDetailKey, "int prelude_detail() { return 1; }\n"}});
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
      {orderedPreludeAKey, "#include \"ordered-prelude-a-dependency.gti\"\n"
                           "mut int32_t prelude_a = 1;\n"},
      {orderedPreludeADependencyKey, "mut int32_t prelude_a_dependency = 1;\n"},
      {orderedPreludeBKey, "#include \"ordered-prelude-b-dependency.gti\"\n"
                           "mut int32_t prelude_b = 1;\n"},
      {orderedPreludeBDependencyKey, "mut int32_t prelude_b_dependency = 1;\n"},
      {orderedIncludeAKey, "#include \"ordered-shared.gti\"\n"
                           "mut int32_t include_a = 1;\n"},
      {orderedIncludeBKey, "#include \"ordered-shared.gti\"\n"
                           "mut int32_t include_b = 1;\n"},
      {orderedSharedKey, "mut int32_t shared_dependency = 1;\n"}};
  const std::string orderedEntryAB =
      "#include \"ordered-a.gti\"\n"
      "#include \"ordered-b.gti\"\n"
      "#if target.os == \"never\"\n"
      "mut int32_t inactive_target_storage = 1;\n"
      "#else\n"
      "mut int32_t active_target_storage = 1;\n"
      "#endif\n"
      "int main() { return 0; }\n";
  const std::string orderedEntryBA =
      "#include \"ordered-b.gti\"\n"
      "#include \"ordered-a.gti\"\n"
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

  const lang::FrontendResult invalidDependency =
      lang::Frontend().analyze(entry,
                               "#include \"branch.gti\"\n"
                               "int main() { return 0; }\n",
                               {}, {{branchKey, "int broken = 1\n"}});
  expect(!invalidDependency.syntaxValid &&
             hasDiagnosticCode(invalidDependency.diagnostics, "GTI-P0001") &&
             hasRelatedDiagnostic(invalidDependency.diagnostics,
                                  "Included from here"),
         "dependency parser diagnostics should retain the incoming include "
         "location");

  const std::filesystem::path missingPath =
      root / "missing-include-never-created.gti";
  std::error_code removeError;
  std::filesystem::remove(missingPath, removeError);
  const std::string missingKey = canonical(missingPath);
  const lang::FrontendResult missingDependency = lang::Frontend().analyze(
      entry, "#include \"missing-include-never-created.gti\"\n"
             "int main() { return 0; }\n");
  bool missingDiagnosticUsesEntry = false;
  for (const lang::Diagnostic &diagnostic : missingDependency.diagnostics) {
    if (diagnostic.code == "GTI-I0008") {
      missingDiagnosticUsesEntry = diagnostic.primary.source == entryKey;
    }
  }
  expect(!missingDependency.sourceValid &&
             hasDiagnosticCode(missingDependency.diagnostics, "GTI-I0008") &&
             missingDiagnosticUsesEntry &&
             missingDependency.sourceGraph.sourceUnitForPath(missingKey) == 0 &&
             !std::filesystem::exists(missingPath),
         "missing relative includes should stay diagnostics on the includer "
         "without creating a source unit or file");

  const lang::FrontendResult dependencyMain =
      lang::Frontend().analyze(entry, "#include \"branch.gti\"\n", {},
                               {{branchKey, "int main() { return 0; }\n"}});
  expect(!dependencyMain.semanticValid &&
             hasDiagnosticCode(dependencyMain.diagnostics, "GTI-S2025"),
         "only the entry source unit should be allowed to declare main");
}

void testStandardLibraryImports() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-standard-imports";
  const std::filesystem::path entry = root / "main.gti";
  const std::filesystem::path arrayUnit =
      standardLibraryRoot() / "std/array.gti";
  const std::string arrayKey =
      std::filesystem::weakly_canonical(arrayUnit).string();

  const lang::FrontendResult imported = lang::Frontend().analyze(
      entry,
      "#include <std/array>\n"
      "#include <std/array>;\n"
      "class NoDefault {\n"
      "  int value;\n"
      "public:\n"
      "  NoDefault(int value) : value(value) {}\n"
      "};\n"
      "int main() {\n"
      "  int initial[3] = {1, 2, 3};\n"
      "  mut std::array<int, 3> values = std::array<int, 3>(initial);\n"
      "  std::array<int, 0> empty_values = std::array<int, 0>();\n"
      "  NoDefault objects[1] = {NoDefault(7)};\n"
      "  std::array<NoDefault, 1> non_default = "
      "std::array<NoDefault, 1>(objects);\n"
      "  std::size_t value_count = values.size();\n"
      "  std::ptrdiff_t offset = std::ptrdiff_t(-1);\n"
      "  values[1] = 4;\n"
      "  if (value_count == 3 and offset < 0 and !values.empty() and "
      "empty_values.size() == 0 and empty_values.empty() and "
      "values.at(1) == 4) { return 0; }\n"
      "  return 1;\n"
      "}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!imported.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : imported.diagnostics) {
      std::cerr << "Unexpected standard import diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(imported.canGenerateCode(),
         "standard-library imports should expose ordinary GTI declarations");
  const lang::SourceUnitId arrayId =
      imported.sourceGraph.sourceUnitForPath(arrayKey);
  const lang::SourceUnit *loadedArray = imported.sourceGraph.findUnit(arrayId);
  const lang::SourceUnitId importedEntryId = imported.sourceGraph.entryUnit();
  const lang::SourceUnitId importedPreludeId =
      imported.sourceGraph.sourceUnitForPath(
          std::filesystem::weakly_canonical(standardLibraryPrelude()).string());
  std::size_t standardEdges = 0;
  for (const lang::SourceDependency &dependency :
       imported.sourceGraph.dependencyEdges()) {
    if (dependency.kind == lang::SourceDependencyKind::StandardLibrary &&
        dependency.source == importedEntryId && dependency.target == arrayId) {
      ++standardEdges;
    }
  }
  // Transitive source-defined library helpers may grow without changing the
  // include identity rule. Check deduplication by canonical path, not by the
  // historical total of entry, prelude and a self-contained array header.
  std::set<std::filesystem::path> canonicalUnits;
  for (const auto &unit : imported.sourceGraph.sourceUnits()) {
    expect(canonicalUnits.insert(std::filesystem::weakly_canonical(unit.path))
               .second,
           "standard imports must not load a canonical source unit twice");
  }
  expect(
      arrayId != 0 && loadedArray != nullptr &&
          loadedArray->standardLibraryName == "std/array" &&
          standardEdges == 2 &&
          canonicalUnits.contains(std::filesystem::weakly_canonical(entry)) &&
          canonicalUnits.contains(
              std::filesystem::weakly_canonical(arrayUnit)) &&
          canonicalUnits.contains(
              std::filesystem::weakly_canonical(standardLibraryPrelude())),
      "standard imports should retain their logical name and load each "
      "canonical unit once");
  expect(imported.sourceGraph.isCompilerTrusted(arrayId) &&
             imported.sourceGraph.isCompilerTrusted(importedPreludeId) &&
             !imported.sourceGraph.isCompilerTrusted(importedEntryId) &&
             !imported.sourceGraph.isCompilerTrusted(0),
         "compiler trust should come only from prelude or physical standard-"
         "library provenance");

  const std::string uniqueOwnerSource =
      "class Value { public: Value() {} };\n"
      "int main() { auto owner = std::make_unique<Value>(); return 0; }\n";
  const lang::FrontendResult missingMemory = lang::Frontend().analyze(
      root / "missing-memory.gti", uniqueOwnerSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !missingMemory.canGenerateCode() &&
          hasDiagnosticCode(missingMemory.diagnostics, "GTI-S2089") &&
          hasDiagnostic(missingMemory.diagnostics,
                        "Undefined qualified name 'std::make_unique'") &&
          hasDiagnosticHint(missingMemory.diagnostics, "#include <std/memory>"),
      "unique ownership is not ambient and should offer its actual include");

  const lang::FrontendResult indirectMemory = lang::Frontend().analyze(
      root / "indirect-memory.gti",
      "#include <std/cstdio>\n" + uniqueOwnerSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  expect(!indirectMemory.canGenerateCode() &&
             hasDiagnosticCode(indirectMemory.diagnostics, "GTI-S2024") &&
             hasDiagnostic(indirectMemory.diagnostics,
                           "include its file directly"),
         "a standard unit should not re-export its memory dependency");

  const lang::FrontendResult directMemory = lang::Frontend().analyze(
      root / "direct-memory.gti", "#include <std/memory>\n" + uniqueOwnerSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(directMemory.canGenerateCode(),
         "the canonical memory import should expose unique ownership");

  const std::vector<std::pair<std::string, std::string>> moduleConcepts = {
      {"std/atomic", "atomic_scalar"},
      {"std/iterator", "input_iterator"},
      {"std/functional", "once_action"},
  };
  for (const auto &[header, conceptName] : moduleConcepts) {
    const std::string conceptSource =
        "concept selected<T> = std::" + conceptName + "<T>;\n";
    const lang::FrontendResult missingConcept = lang::Frontend().analyze(
        root / ("missing-" + conceptName + ".gti"), conceptSource,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!missingConcept.canGenerateCode() &&
               hasDiagnostic(missingConcept.diagnostics,
                             "Unknown concept 'std::" + conceptName + "'"),
           "a module-owned compiler concept should not remain ambient");

    const lang::FrontendResult directConcept = lang::Frontend().analyze(
        root / ("direct-" + conceptName + ".gti"),
        "#include <" + header + ">\n" + conceptSource,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(directConcept.canGenerateCode(),
           "the canonical import should expose its public compiler concept");
  }

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
      std::filesystem::temp_directory_path() / "gti-stdlib-overlay-main.gti",
      "#include <std/icap_override_only_probe_8f6d41c5>\n", {},
      {{overlayKey,
        "namespace std { int override_only_value() { return 0; } }\n"}},
      {standardLibraryRoot()});
  const lang::SourceUnitId overlayId =
      overlayGraph.sourceUnitForPath(overlayKey);
  const lang::SourceUnit *overlaySource = overlayGraph.findUnit(overlayId);
  expect(!overlayExists && !directOverlayLoader.hadError() &&
             directOverlaySource != nullptr &&
             !directOverlaySource->standardLibraryName.has_value() &&
             !directOverlayGraph.isCompilerTrusted(
                 directOverlayGraph.entryUnit()) &&
             !overlayLoader.hadError() && overlayId != 0 &&
             overlaySource != nullptr &&
             !overlaySource->standardLibraryName.has_value() &&
             !overlayGraph.isCompilerTrusted(overlayId),
         "direct and imported override-only paths beneath a configured root "
         "must not mint compiler trust without a physical standard-library "
         "unit");

  const std::string privateSource =
      "#include <std/memory>\n"
      "namespace gti_internal { class forged {}; }\n"
      "namespace private_alias = gti_internal;\n"
      "using forged_storage = gti_internal::unique_owner<int>;\n"
      "int main() { gti_internal::runtime::write_stdout(\"x\"); return 0; }\n";
  const lang::FrontendResult privateAccess = lang::Frontend().analyze(
      root / "private-access.gti", privateSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  const lang::Diagnostic *privateDiagnostic =
      findDiagnosticByCode(privateAccess.diagnostics, "GTI-S2058");
  expect(
      !privateAccess.canGenerateCode() &&
          countDiagnosticCode(privateAccess.diagnostics, "GTI-S2058") == 4 &&
          privateDiagnostic != nullptr &&
          privateDiagnostic->primary.source ==
              std::filesystem::weakly_canonical(root / "private-access.gti")
                  .string() &&
          !privateDiagnostic->hints.empty() &&
          hasRelatedDiagnostic(privateAccess.diagnostics, "trusted prelude") &&
          !hasDiagnostic(privateAccess.diagnostics, "Unknown namespace") &&
          !hasDiagnostic(privateAccess.diagnostics, "Unknown type"),
      "applications should reject direct declarations, aliases, type "
      "uses, and runtime calls with focused GTI-S2058 diagnostics");

  const lang::FrontendResult forgedSpelling = lang::Frontend().analyze(
      root / "forged-private-spelling.gti",
      "namespace ordinary { class storage<T> {}; }\n"
      "ordinary::storage<int> value = ordinary::storage<int>();\n"
      "int main() { return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(forgedSpelling.canGenerateCode() &&
             !hasDiagnosticCode(forgedSpelling.diagnostics, "GTI-S2058"),
         "an ordinary declaration with a capability leaf spelling should "
         "remain an ordinary nominal type");

  const std::filesystem::path leakyUnit =
      standardLibraryRoot() / "std/array.gti";
  const std::string leakyKey =
      std::filesystem::weakly_canonical(leakyUnit).string();
  const lang::FrontendResult leakyDecomposition = lang::Frontend().analyze(
      root / "leaky-decomposition.gti",
      "#include <std/array>\n"
      "int main() { auto [capability] = std::Leaky(); return 0; }\n",
      {standardLibraryPrelude()},
      {{leakyKey,
        "#include <std/memory>\nnamespace std { struct Leaky { public: "
        "gti_internal::unique_owner<int> capability; }; }\n"}},
      {standardLibraryRoot()});
  expect(!leakyDecomposition.canGenerateCode() &&
             countDiagnosticCode(leakyDecomposition.diagnostics, "GTI-S2058") ==
                 1 &&
             !hasDiagnostic(leakyDecomposition.diagnostics, "Unknown type"),
         "structured bindings must not launder a public field whose selected "
         "type is a compiler-private capability");

  const lang::FrontendResult leakyAlias = lang::Frontend().analyze(
      root / "leaky-alias.gti",
      "#include <std/array>\n"
      "std::leaked value;\nint main() { return 0; }\n",
      {standardLibraryPrelude()},
      {{leakyKey, "#include <std/memory>\nnamespace std { using leaked = "
                  "gti_internal::unique_owner<int>; }\n"}},
      {standardLibraryRoot()});
  expect(!leakyAlias.canGenerateCode() &&
             countDiagnosticCode(leakyAlias.diagnostics, "GTI-S2058") == 1 &&
             !hasDiagnostic(leakyAlias.diagnostics, "Unknown type"),
         "trusted public aliases to private capabilities must remain hidden "
         "with the private-access diagnostic rather than an unknown-type "
         "cascade");

  const lang::FrontendResult mixedOverload = lang::Frontend().analyze(
      root / "mixed-private-overload.gti",
      "#include <std/array>\n"
      "int main() { std::Mixed value = std::Mixed(); "
      "return value.select(1.0); }\n",
      {standardLibraryPrelude()},
      {{leakyKey, "#include <std/memory>\nnamespace std { class Mixed { "
                  "public: Mixed() {} "
                  "int select(int value) { return value; } "
                  "int select(gti_internal::unique_owner<int> value) { "
                  "return 0; } }; }\n"}},
      {standardLibraryRoot()});
  expect(!mixedOverload.canGenerateCode() &&
             countDiagnosticCode(mixedOverload.diagnostics, "GTI-S2012") == 1 &&
             !hasRelatedDiagnostic(mixedOverload.diagnostics,
                                   "gti_internal::unique_owner<int>"),
         "application overload diagnostics must preserve public siblings "
         "without presenting compiler-private signature candidates");

  const lang::FrontendResult inheritedMixedOverload = lang::Frontend().analyze(
      root / "inherited-mixed-private-overload.gti",
      "#include <std/array>\n"
      "class Derived : public std::PrivateBase { public: "
      "int select(int value) { return value; } };\n"
      "int main() { Derived value = Derived(); return value.select(7); }\n",
      {standardLibraryPrelude()},
      {{leakyKey,
        "#include <std/memory>\nnamespace std { class PrivateBase { public: "
        "PrivateBase() {} "
        "int select(gti_internal::unique_owner<int> value) { return 0; } }; "
        "}\n"}},
      {standardLibraryRoot()});
  expect(inheritedMixedOverload.canGenerateCode(),
         "a public overload added by a derived application class must remain "
         "available when a trusted base contributes a private-signature "
         "sibling");

  const lang::FrontendResult privateMemberOccurrence = lang::Frontend().analyze(
      root / "private-member-occurrence.gti",
      "#include <std/array>\n"
      "int main() { std::PrivateMethod value = std::PrivateMethod(); "
      "return value.secret(1); }\n",
      {standardLibraryPrelude()},
      {{leakyKey,
        "#include <std/memory>\nnamespace std { class PrivateMethod { public: "
        "PrivateMethod() {} "
        "int secret(gti_internal::unique_owner<int> value) { return 0; } }; "
        "}\n"}},
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

  const std::string generated = gti_test::emitCppText(imported);
  expect(generated.find("namespace __gti_std") != std::string::npos &&
             generated.find("_array {") != std::string::npos &&
             generated.find("std::array<std::int32_t, 3> values") !=
                 std::string::npos &&
             generated.find("::__gti_program::__gti_std::__gti_type_") !=
                 std::string::npos &&
             generated.find("template <typename T, std::uint64_t N>\n"
                            "    class array") == std::string::npos,
         "std::array should remain source-defined while its selected "
         "application emits as one concrete GTI-owned class");

  const lang::FrontendResult distinctStandardNamespace =
      lang::Frontend().analyze(
          root / "user-gti-std.gti",
          "namespace gti_std {\n"
          "class string_view {\n"
          "  int32_t value;\n"
          "public:\n"
          "  string_view(int32_t value) : value(value) {}\n"
          "  int32_t read() { return this.value; }\n"
          "};\n"
          "}\n"
          "int main() {\n"
          "  gti_std::string_view value = gti_std::string_view(7);\n"
          "  return value.read() - 7;\n"
          "}\n",
          {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(distinctStandardNamespace.canGenerateCode(),
         "a user gti_std namespace should remain distinct in GTI");
  const std::string distinctNamespaceCpp =
      gti_test::emitCppText(distinctStandardNamespace);
  expect(
      distinctNamespaceCpp.find("namespace __gti_std") != std::string::npos &&
          distinctNamespaceCpp.find("namespace gti_std") != std::string::npos,
      "the C++ backend should reserve its std remap without claiming the "
      "user gti_std namespace");

  const std::filesystem::path wrapper =
      standardLibraryRoot() / "std/import_test_wrapper.gti";
  const std::string wrapperKey =
      std::filesystem::weakly_canonical(wrapper).string();
  const lang::FrontendResult hidden = lang::Frontend().analyze(
      entry,
      "#include <std/import_test_wrapper>\n"
      "std::array<int, 1> hidden = std::array<int, 1>();\n"
      "int main() { return std::import_test_value(); }\n",
      {standardLibraryPrelude()},
      {{wrapperKey,
        "#include <std/array>\n"
        "namespace std { int import_test_value() { return 0; } }\n"}},
      {standardLibraryRoot()});
  expect(!hidden.semanticValid &&
             hasDiagnosticCode(hidden.diagnostics, "GTI-S2024") &&
             hasDiagnosticHint(hidden.diagnostics, "#include <std/array>"),
         "standard imports should remain private and provide logical import "
         "hints");

  const std::filesystem::path keywordUnit =
      standardLibraryRoot() / "std/char.gti";
  const std::string keywordKey =
      std::filesystem::weakly_canonical(keywordUnit).string();
  const lang::FrontendResult keywordPath = lang::Frontend().analyze(
      entry,
      "#include <std/char>\n"
      "int main() { return std::keyword_path_value(); }\n",
      {standardLibraryPrelude()},
      {{keywordKey,
        "namespace std { int keyword_path_value() { return 0; } }\n"}},
      {standardLibraryRoot()});
  expect(keywordPath.canGenerateCode(),
         "standard import components should permit GTI keyword spellings");

  const std::filesystem::path cppKeywordUnit =
      standardLibraryRoot() / "std/template.gti";
  const std::string cppKeywordKey =
      std::filesystem::weakly_canonical(cppKeywordUnit).string();
  const lang::FrontendResult cppKeywordPath = lang::Frontend().analyze(
      entry,
      "#include <std/template>\n"
      "int main() { return std::cpp_keyword_path_value(); }\n",
      {standardLibraryPrelude()},
      {{cppKeywordKey,
        "namespace std { int cpp_keyword_path_value() { return 0; } }\n"}},
      {standardLibraryRoot()});
  expect(cppKeywordPath.canGenerateCode(),
         "standard import components should permit reserved C++ keyword "
         "spellings");

  const lang::FrontendResult missing = lang::Frontend().analyze(
      entry, "#include <std/not_present>\nint main() { return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!missing.sourceValid &&
             hasDiagnosticCode(missing.diagnostics, "GTI-I0007") &&
             hasDiagnostic(missing.diagnostics,
                           "<std/not_present>' was not found"),
         "missing standard units should fail during source loading");

  const lang::FrontendResult malformed = lang::Frontend().analyze(
      entry, "#include <array>\nint main() { return 0; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!malformed.sourceValid &&
             hasDiagnosticCode(malformed.diagnostics, "GTI-I0007") &&
             hasDiagnostic(malformed.diagnostics, "#include <std/array>"),
         "malformed standard imports should explain the required spelling");

  const lang::TargetInfo linuxTarget{"linux", "unknown", "x86_64"};
  const lang::FrontendOptions linuxOptions{.target = linuxTarget};
  const lang::FrontendResult tcp =
      lang::Frontend(linuxOptions)
          .analyze(entry,
                   "#include <std/tcp>\n"
                   "int main() {\n"
                   "  mut auto opened = std::tcp::open();\n"
                   "  if (!opened or !opened.value().is_open()) { return 1; }\n"
                   "  auto closed = opened.value().close();\n"
                   "  if (!closed or opened.value().is_open()) { return 2; }\n"
                   "  auto closed_again = opened.value().close();\n"
                   "  if (closed_again or closed_again.error() != "
                   "std::tcp::errc::not_open) { return 3; }\n"
                   "  return 0;\n"
                   "}\n",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!tcp.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : tcp.diagnostics) {
      std::cerr << "Unexpected tcp import diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(tcp.canGenerateCode(),
         "the POSIX std::tcp owner should provide typed open and close");
  const std::string tcpCpp =
      gti_test::emitCppText(tcp, lang::CppStandard::Cpp23, linuxTarget);
  expect(
      tcpCpp.find("class socket") != std::string::npos &&
          tcpCpp.find("runtime::" + gti_test::nativeSpelling(tcpCpp, "socket") +
                      "(") != std::string::npos &&
          tcpCpp.find("open__gti_mir_failure(std::expected") !=
              std::string::npos &&
          tcpCpp.find("runtime::" + gti_test::nativeSpelling(tcpCpp, "close") +
                      "(") != std::string::npos &&
          tcpCpp.find("socket(socket &&other) noexcept") != std::string::npos &&
          tcpCpp.find("// GTI verified-MIR body:") != std::string::npos,
      "the tcp owner should retain private POSIX acquisition and generated "
      "move cleanup");

  const lang::FrontendResult copiedTcp =
      lang::Frontend(linuxOptions)
          .analyze(entry,
                   "#include <std/tcp>\n"
                   "void copy_socket(std::tcp::socket& original) {\n"
                   "  std::tcp::socket copied = original;\n"
                   "}\n"
                   "int main() {\n"
                   "  return 0;\n"
                   "}\n",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      !copiedTcp.semanticValid &&
          hasDiagnostic(copiedTcp.diagnostics, "Cannot initialize 'copied'") &&
          hasDiagnosticHint(copiedTcp.diagnostics,
                            "Move-only owners cannot be copied"),
      "std::tcp::socket should reject copies before backend emission");

  const lang::FrontendResult forgedTcp =
      lang::Frontend(linuxOptions)
          .analyze(entry,
                   "#include <std/tcp>\n"
                   "int main() {\n"
                   "  auto forged = std::make_unique<std::tcp::socket>(\n"
                   "      gti_internal::tcp_socket_handle(1));\n"
                   "  return 0;\n"
                   "}\n",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!forgedTcp.canGenerateCode() &&
             countDiagnosticCode(forgedTcp.diagnostics, "GTI-S2058") == 1 &&
             !hasDiagnostic(forgedTcp.diagnostics,
                            "Constructor of 'socket' is private"),
         "applications should reject private descriptor names directly "
         "without leaking constructor details");

  const lang::TargetInfo unknownTarget{"unknown", "unknown", "unknown"};
  const lang::FrontendOptions unknownOptions{.target = unknownTarget};
  const lang::FrontendResult unsupportedTcp =
      lang::Frontend(unknownOptions)
          .analyze(entry, "#include <std/tcp>\nint main() { return 0; }\n",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!unsupportedTcp.canGenerateCode() &&
             countDiagnosticCode(unsupportedTcp.diagnostics, "GTI-S2062") ==
                 1 &&
             hasDiagnostic(unsupportedTcp.diagnostics,
                           "no supported GTI data layout"),
         "the frontend should reject an unknown target before imported "
         "source can select platform policy");

  const lang::TargetInfo windowsTarget{"windows", "pc", "x86_64"};
  const lang::FrontendOptions windowsOptions{.target = windowsTarget};
  const lang::FrontendResult windowsTcp =
      lang::Frontend(windowsOptions)
          .analyze(entry, "#include <std/tcp>\nint main() { return 0; }\n",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!windowsTcp.semanticValid &&
             hasDiagnostic(windowsTcp.diagnostics,
                           "std/tcp currently exposes only the POSIX socket "
                           "ABI"),
         "std::tcp should reject Windows targets at import time");
}

void testPackageImports() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "gti-package-imports";
  const std::filesystem::path appRoot = root / "app";
  const std::filesystem::path mathRoot = root / "math";
  const std::filesystem::path hiddenRoot = root / "hidden";
  const std::filesystem::path entry = appRoot / "src/main.gti";
  const std::filesystem::path mathUnit = mathRoot / "src/add.gti";
  const std::filesystem::path hiddenUnit = hiddenRoot / "src/value.gti";
  std::filesystem::create_directories(entry.parent_path());
  std::filesystem::create_directories(mathUnit.parent_path());
  std::filesystem::create_directories(hiddenUnit.parent_path());
  {
    std::ofstream output(mathUnit);
    output << "namespace math { int add(int left, int right) { return left + "
              "right; } }\n";
  }
  {
    std::ofstream output(hiddenUnit);
    output << "namespace hidden { int value() { return 7; } }\n";
  }

  const std::vector<lang::PackageSourceRoot> packages{
      {.identity = "app@1.0.0",
       .name = "app",
       .packageRoot = appRoot,
       .sourceRoot = appRoot / "src",
       .dependencies = {{.alias = "math", .targetIdentity = "math@2.0.0"}}},
      {.identity = "hidden@1.0.0",
       .name = "hidden",
       .packageRoot = hiddenRoot,
       .sourceRoot = hiddenRoot / "src"},
      {.identity = "math@2.0.0",
       .name = "math",
       .packageRoot = mathRoot,
       .sourceRoot = mathRoot / "src",
       .dependencies = {
           {.alias = "hidden", .targetIdentity = "hidden@1.0.0"}}}};

  const lang::FrontendResult imported = lang::Frontend().analyze(
      entry,
      "#include <math/add>\n"
      "int main() { return math::add(2, 3) == 5 ? 0 : 1; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()}, packages);
  const lang::SourceUnitId mathId = imported.sourceGraph.sourceUnitForPath(
      std::filesystem::weakly_canonical(mathUnit).string());
  const lang::SourceUnit *mathSource = imported.sourceGraph.findUnit(mathId);
  bool packageEdge = false;
  for (const lang::SourceDependency &dependency :
       imported.sourceGraph.dependencyEdges()) {
    packageEdge =
        packageEdge || dependency.kind == lang::SourceDependencyKind::Package;
  }
  expect(imported.canGenerateCode() && mathSource != nullptr && packageEdge &&
             mathSource->packageIdentity == "math@2.0.0" &&
             mathSource->packageRelativePath == "src/add.gti" &&
             !imported.sourceGraph.isCompilerTrusted(mathId),
         "declared package imports should load ordinary untrusted GTI source "
         "with stable package provenance");

  const lang::FrontendResult transitiveLeak = lang::Frontend().analyze(
      entry,
      "#include <hidden/value>\n"
      "int main() { return hidden::value(); }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()}, packages);
  expect(!transitiveLeak.sourceValid &&
             hasDiagnosticCode(transitiveLeak.diagnostics, "GTI-I0010") &&
             hasDiagnostic(transitiveLeak.diagnostics,
                           "no direct dependency alias 'hidden'"),
         "package imports should expose only the including package's direct "
         "dependency aliases");

  const lang::FrontendResult relativeEscape = lang::Frontend().analyze(
      entry,
      "#include \"../../math/src/add.gti\"\n"
      "int main() { return math::add(2, 3); }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()}, packages);
  expect(!relativeEscape.sourceValid &&
             hasDiagnosticCode(relativeEscape.diagnostics, "GTI-I0010") &&
             hasDiagnostic(relativeEscape.diagnostics,
                           "cannot cross the owning package boundary"),
         "quoted includes should not bypass declared package aliases");
}

void testLoopControlStatements() {
  lang::Lexer lexer;
  const std::string source = R"(
int main() {
  mut int total = 0;
  for (mut int outer = 0; outer < 4; outer++) {
    if (outer == 1) {
      continue;
    }
    while (true) {
      total += outer;
      break;
    }
  }
  return total;
}
)";
  auto tokens = lexer.scan(source, "loop-control.gti");
  expect(!lexer.hadError(), "break and continue should lex as keywords");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "loop control statements should parse");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "break and continue should be valid in nested loop bodies");

  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("switch (__gti_mir_bb)") == std::string::npos &&
             generated.find("__gti_mir_bb =") == std::string::npos &&
             generated.find("goto __gti_mir_bb_") != std::string::npos &&
             generated.find("__gti_mir_bb_1: {") != std::string::npos &&
             generated.find("// GTI verified-MIR body:") != std::string::npos,
         "the C++ backend should lower loop control through direct verified "
         "MIR CFG edges");

  auto invalidTokens = lexer.scan(R"(
void stop() {
  break;
}

void skip() {
  if (true) {
    continue;
  }
}
)",
                                  "invalid-loop-control.gti");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "loop control outside a loop should remain valid syntax");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "loop control outside loops should be rejected semantically");
  expect(
      countDiagnosticCode(invalidSemantic, "GTI-S2010") == 2 &&
          hasDiagnostic(invalidSemantic,
                        "'break' can only be used inside a loop or switch") &&
          hasDiagnostic(invalidSemantic,
                        "'continue' can only be used inside a loop"),
      "invalid break and continue should receive focused diagnostics");

  auto recoveredTokens = lexer.scan(R"(
void recover() {
  while (true) {
    break
    continue;
  }
}
)",
                                    "recover-loop-control.gti");
  lang::Parser recoveredParser(std::move(recoveredTokens));
  lang::Program recoveredProgram = recoveredParser.parse();
  expect(recoveredParser.errors().size() == 1 &&
             !recoveredParser.errors().front().fixes.empty() &&
             recoveredParser.errors().front().fixes.front().replacement == ";",
         "a missing loop-control semicolon should offer an insertion fix");
  lang::SemanticVisitor recoveredSemantic;
  expect(recoveredSemantic.check(recoveredProgram),
         "parser recovery should retain the following loop-control statement");
}

void testDoWhileStatements() {
  const std::string source = R"(
class Marker {
public:
  Marker() {}
  ~Marker() {}
};

int run() {
  mut int value = 0;
  do {
    Marker marker{};
    value++;
    if (value < 2) {
      continue;
    }
    if (value == 4) {
      break;
    }
  } while (value < 5);
  return value;
}

int main() { return run() - 4; }
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("do-while.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected do-while diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid,
         "a do-while loop should pass the complete frontend and MIR pipeline");

  const lang::HirFunctionInstance *run = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr && instance.source->name().lexeme == "run") {
      run = &instance;
      break;
    }
  }
  const lang::HirStatement *loop = nullptr;
  const lang::HirStatement *continueStatement = nullptr;
  const lang::HirStatement *breakStatement = nullptr;
  if (run != nullptr) {
    for (const lang::HirStatement &statement : run->body.statements) {
      if (statement.kind == lang::HirStatementKind::DoWhile) {
        loop = &statement;
      } else if (statement.kind == lang::HirStatementKind::Continue) {
        continueStatement = &statement;
      } else if (statement.kind == lang::HirStatementKind::Break) {
        breakStatement = &statement;
      }
    }
  }
  expect(loop != nullptr && loop->body && loop->condition,
         "typed HIR should preserve a distinct body-first do-while statement");

  const lang::MirFunctionInstance *loweredRun =
      run == nullptr ? nullptr : frontend.mir.findFunctionInstance(run->id);
  const lang::MirBlock *conditionBlock = nullptr;
  if (loweredRun != nullptr && loop != nullptr) {
    for (const lang::MirBlock &block : loweredRun->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Branch &&
          block.terminator.hirStatement == loop->id) {
        conditionBlock = &block;
        break;
      }
    }
  }

  const auto controlBlock =
      [&](const lang::HirStatement *statement) -> const lang::MirBlock * {
    if (loweredRun == nullptr || statement == nullptr) {
      return nullptr;
    }
    for (const lang::MirBlock &block : loweredRun->body.blocks) {
      if (block.terminator.hirStatement == statement->id) {
        return &block;
      }
    }
    return nullptr;
  };
  const lang::MirBlock *continueBlock = controlBlock(continueStatement);
  const lang::MirBlock *breakBlock = controlBlock(breakStatement);
  const auto dropsMarker = [](const lang::MirBlock *block) {
    return block != nullptr &&
           std::any_of(block->instructions.begin(), block->instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                lang::MirInstructionKind::Drop;
                       });
  };
  expect(conditionBlock != nullptr && continueBlock != nullptr &&
             continueBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
             continueBlock->terminator.target == conditionBlock->id &&
             dropsMarker(continueBlock),
         "do-while continue should clean the body scope and target the "
         "condition block");
  expect(conditionBlock != nullptr && breakBlock != nullptr &&
             breakBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
             breakBlock->terminator.target ==
                 conditionBlock->terminator.elseTarget &&
             dropsMarker(breakBlock),
         "do-while break should clean the body scope and target the loop exit");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact generated =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  // The cleanup-gate narrowing admitted this body to general verified-MIR
  // emission (its scope owner's cleanup cannot raise); the body-first MIR
  // structure is pinned above, and the backend now emits the verified
  // schedule instead of the compatibility do-while text.
  expect(generated.contents.find("// GTI verified-MIR body: scalar-cfg-v1") !=
             std::string::npos,
         "the C++ backend should emit the do-while body from verified MIR");

  const lang::FrontendResult invalidCondition = lang::Frontend().analyze(
      "invalid-do-while.gti", "void run() { do {} while (1); }\n");
  expect(!invalidCondition.canGenerateCode() &&
             hasDiagnostic(invalidCondition.diagnostics,
                           "Do-while condition must be bool"),
         "do-while should use GTI's exact contextual-bool rule");

  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("void recover() { do {} while (false) return; }\n",
                 "recover-do-while.gti"));
  lang::Program recovered = parser.parse();
  expect(parser.errors().size() == 1 &&
             !parser.errors().front().fixes.empty() &&
             parser.errors().front().fixes.front().replacement == ";" &&
             !recovered.declarations().empty(),
         "a missing do-while semicolon should offer a focused insertion fix");
}

void testConditionalExpressions() {
  const std::string source = R"(
#include <std/memory>

int choose(bool condition, int left, int right) {
  return condition ? left : right;
}

int branch_effect(bool condition) {
  mut int value = 0;
  int selected = condition ? (value = 7) : (value = 3);
  return selected + value;
}

void set_value(mut int& target, int value) { target = value; }

void choose_void(bool condition, mut int& target) {
  condition ? set_value(target, 1) : set_value(target, 2);
}

struct Item {
  int value;
  Item(int initial) : value(initial) {}
};

std::unique_ptr<Item> choose_owner(
    bool condition,
    std::unique_ptr<Item> left,
    std::unique_ptr<Item> right) {
  return condition ? std::move(left) : std::move(right);
}

int main() {
  mut int result = choose(true, 4, 9);
  result += choose(false, 1, 3);
  result += branch_effect(false);
  mut int side_effect = 0;
  choose_void(true, side_effect);
  result += side_effect;
  auto owner = choose_owner(
      true, std::make_unique<Item>(8), std::make_unique<Item>(9));
  return result - 14;
}
)";

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "conditional-expression.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected conditional-expression diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid,
         "conditional expressions should pass the complete frontend and MIR "
         "pipeline");

  const lang::HirFunctionInstance *choose = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "choose") {
      choose = &instance;
      break;
    }
  }
  const lang::HirValue *conditional = nullptr;
  if (choose != nullptr) {
    for (const lang::HirValue &value : choose->body.values) {
      if (value.kind == lang::HirValueKind::Conditional) {
        conditional = &value;
        break;
      }
    }
  }
  expect(conditional != nullptr && conditional->operands.size() == 3 &&
             conditional->info.type == lang::SemanticType::Int32 &&
             conditional->info.category == lang::ValueCategory::Value,
         "typed HIR should preserve condition, true arm, false arm, and the "
         "owned result type");

  const lang::MirFunctionInstance *loweredChoose =
      choose == nullptr ? nullptr
                        : frontend.mir.findFunctionInstance(choose->id);
  const lang::MirBlock *branch = nullptr;
  if (loweredChoose != nullptr && conditional != nullptr) {
    for (const lang::MirBlock &block : loweredChoose->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Branch &&
          block.terminator.hirValue == conditional->id) {
        branch = &block;
        break;
      }
    }
  }
  const lang::MirBlock *thenBlock =
      branch == nullptr
          ? nullptr
          : loweredChoose->body.findBlock(branch->terminator.target);
  const lang::MirBlock *elseBlock =
      branch == nullptr
          ? nullptr
          : loweredChoose->body.findBlock(branch->terminator.elseTarget);
  const bool armsMerge =
      thenBlock != nullptr && elseBlock != nullptr &&
      thenBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
      elseBlock->terminator.kind == lang::MirTerminatorKind::Goto &&
      thenBlock->terminator.target == elseBlock->terminator.target;
  const lang::MirBlock *mergeBlock =
      !armsMerge ? nullptr
                 : loweredChoose->body.findBlock(thenBlock->terminator.target);
  const auto initializesConditional = [conditional](
                                          const lang::MirBlock *block) {
    return conditional != nullptr && block != nullptr &&
           std::any_of(block->instructions.begin(), block->instructions.end(),
                       [conditional](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                    lang::MirInstructionKind::Initialize &&
                                instruction.hirValue == conditional->id;
                       });
  };
  expect(branch != nullptr && armsMerge && initializesConditional(thenBlock) &&
             initializesConditional(elseBlock) && mergeBlock != nullptr &&
             std::any_of(
                 mergeBlock->instructions.begin(),
                 mergeBlock->instructions.end(),
                 [conditional](const lang::MirInstruction &instruction) {
                   return instruction.kind == lang::MirInstructionKind::Load &&
                          instruction.hirValue == conditional->id;
                 }),
         "MIR should evaluate one arm in separate blocks and load one merged "
         "result");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact generated =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto publication = generated.contents.find(
      "construct_representation_at(__gti_mir_out_result, "
      "std::move(__gti_mir_p_4.get()));");
  const auto retirement =
      generated.contents.find("__gti_mir_p_4.destroy();", publication);
  const auto rightCleanup =
      generated.contents.find("__gti_mir_p_3.destroy();", retirement);
  const auto leftCleanup =
      generated.contents.find("__gti_mir_p_2.destroy();", rightCleanup);
  const auto returnPublication =
      generated.contents.find("return true;", publication);
  expect(generated.contents.find("// GTI verified-MIR body: scalar-cfg-v1") !=
                 std::string::npos &&
             generated.contents.find("if (__gti_mir_v_1) {") !=
                 std::string::npos &&
             generated.contents.find("goto __gti_mir_bb_2;") !=
                 std::string::npos &&
             generated.contents.find("goto __gti_mir_bb_3;") !=
                 std::string::npos &&
             generated.contents.find("switch (__gti_mir_bb)") ==
                 std::string::npos &&
             generated.contents.find(
                 "__gti_mir_p_4.construct(std::move(__gti_mir_p_2.get()));") !=
                 std::string::npos &&
             generated.contents.find(
                 "__gti_mir_p_4.construct(std::move(__gti_mir_p_3.get()));") !=
                 std::string::npos &&
             publication != std::string::npos &&
             retirement != std::string::npos &&
             rightCleanup != std::string::npos &&
             leftCleanup != std::string::npos &&
             returnPublication != std::string::npos &&
             publication < retirement && retirement < rightCleanup &&
             rightCleanup < leftCleanup && leftCleanup < returnPublication,
         "the C++ backend should emit scalar conditional control flow from "
         "verified MIR while preserving explicit ownership transfer");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-conditional-expression.gti", R"(
#include <std/memory>

int invalid_condition() { return 1 ? 2 : 3; }
bool mismatched_arms(bool condition) { return condition ? 1 : false; }
struct Item {};
std::unique_ptr<Item> implicit_copy(
    bool condition,
    std::unique_ptr<Item> left,
    std::unique_ptr<Item> right) {
  return condition ? left : right;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalid.canGenerateCode() &&
             hasDiagnostic(invalid.diagnostics,
                           "Conditional expression condition must be bool") &&
             hasDiagnosticCode(invalid.diagnostics, "GTI-S2050") &&
             countDiagnosticCode(invalid.diagnostics, "GTI-S2052") == 2,
         "conditional expressions should diagnose non-bool conditions, "
         "inexact arm types, and implicit move-only copies");

  const lang::FrontendResult maybeMoved = lang::Frontend().analyze(
      "conditional-move-state.gti", R"(
#include <std/memory>

struct Item {
  int value;
  Item(int initial) : value(initial) {}
};

int inspect(bool condition) {
  mut auto owner = std::make_unique<Item>(1);
  auto selected = condition ? std::move(owner) : std::make_unique<Item>(2);
  return owner->value;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!maybeMoved.canGenerateCode() &&
             hasDiagnostic(maybeMoved.diagnostics,
                           "may have been moved on another control-flow path"),
         "conditional arm states should merge explicit moves across runtime "
         "paths");

  const lang::FrontendResult borrowedResult =
      lang::Frontend().analyze("conditional-borrowed-result.gti", R"(
class BorrowedValue {
  int& value;

public:
  BorrowedValue(int& source) : value(source) {}
};

BorrowedValue choose_borrowed(
    bool condition, BorrowedValue left, BorrowedValue right) {
  return condition ? std::move(left) : std::move(right);
}
)",
                               {standardLibraryPrelude()});
  expect(!borrowedResult.canGenerateCode() &&
             hasDiagnosticCode(borrowedResult.diagnostics, "GTI-S2051"),
         "conditional values should reject branch-selected borrowed state "
         "until MIR can preserve its loan origin");

  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("int recover(bool value) { return value ? 1 2; }\n",
                 "recover-conditional-expression.gti"));
  lang::Program recovered = parser.parse();
  expect(parser.errors().size() == 1 &&
             !parser.errors().front().fixes.empty() &&
             parser.errors().front().fixes.front().replacement == ":" &&
             !recovered.declarations().empty(),
         "a missing conditional colon should offer a focused insertion fix");

  const std::string formatted = lang::Formatter().format(
      "int choose(bool c,int a,int b){return c?a:b?b:a;}");
  expect(formatted == R"(int choose(bool c, int a, int b) {
  return c ? a : b ? b : a;
}
)" && lang::Formatter().format(formatted) == formatted,
         "the formatter should apply stable C++-style conditional spacing");
}

void testSwitchStatements() {
  const std::string source = R"(
enum class Stage : uint8_t { Boot, Ready, Running };

int classify(Stage stage) {
  switch (stage) {
  case Stage::Boot:
    return 0;
  case Stage::Ready:
  case Stage::Running:
    return 1;
  default:
    return 2;
  }
}

int main() {
  mut int total = 0;
  for (mut int index = 0; index < 3; index++) {
    switch (index) {
    case 0:
      continue;
    case 1:
      int local = 1;
      total += local;
      break;
    default:
      int local = 2;
      total += local;
      break;
    }
  }
  char marker = 'G';
  switch (marker) {
  case 'G':
    total += 1;
    break;
  default:
    break;
  }
  uint64_t wide = 7;
  switch (wide) {
  case uint64_t(7):
    total += classify(Stage::Ready);
    break;
  default:
    break;
  }
  return total;
}
)";

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("switch.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected switch diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode(),
         "explicit integer, char, and scoped-enum switches should compile");

  const lang::FunctionDecl *classify =
      findTopLevelFunction(frontend.program, "classify");
  const auto *switchStatement =
      classify == nullptr || classify->body()->statements().empty()
          ? nullptr
          : dynamic_cast<const lang::SwitchStmt *>(
                classify->body()->statements().front().get());
  expect(switchStatement != nullptr && switchStatement->arms().size() == 3 &&
             switchStatement->arms()[1].labels.size() == 2,
         "the AST should group adjacent labels into one executable arm");
  expect(switchStatement != nullptr &&
             frontend.semantics.findSwitchCase(
                 *switchStatement->arms()[0].labels[0].value) != nullptr,
         "semantic analysis should retain normalized case constants");

  const lang::HirFunctionInstance *classifyInstance = nullptr;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "classify") {
      classifyInstance = &function;
      break;
    }
  }
  const lang::HirStatement *loweredSwitch =
      classifyInstance == nullptr || classifyInstance->body.roots.empty()
          ? nullptr
          : classifyInstance->body.findStatement(
                classifyInstance->body.roots.front());
  expect(loweredSwitch != nullptr &&
             loweredSwitch->kind == lang::HirStatementKind::Switch &&
             loweredSwitch->switchArms.size() == 3 &&
             loweredSwitch->switchArms[1].labels.size() == 2 &&
             loweredSwitch->switchArms[1].labels[0].constant.has_value(),
         "HIR should preserve switch arms and normalized labels for backends");

  const std::string generated = gti_test::emitCppText(frontend);
  expect(generated.find("switch (__gti_mir_v_") != std::string::npos &&
             generated.find("case static_cast<::__gti_program::Stage>(0):") !=
                 std::string::npos &&
             generated.find("case static_cast<std::uint64_t>(7):") !=
                 std::string::npos &&
             generated.find("goto __gti_mir_bb_") != std::string::npos &&
             generated.find("switch (__gti_mir_bb)") == std::string::npos &&
             generated.find("__gti_mir_bb_1: {") != std::string::npos &&
             generated.find("// GTI verified-MIR body:") != std::string::npos,
         "the C++ backend should emit normalized labels through arm-local MIR "
         "CFG blocks");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("invalid-switch.gti", R"(
enum class State { Idle, Running };
enum class Alias { First = 1, Second = 1 };
int dynamic_label() { return 1; }

int invalid(uint64_t wide, int value, Alias alias) {
  switch (true) {
  case true:
    break;
  default:
    break;
  }
  switch (wide) {
  case 1:
    break;
  case uint64_t(2):
    break;
  case uint64_t(2):
    break;
  default:
    break;
  default:
    break;
  }
  switch (alias) {
  case Alias::First:
    break;
  case Alias::Second:
    break;
  default:
    break;
  }
  switch (value) {
  case dynamic_label():
    break;
  case 3:
    int missing_terminator = 3;
  default:
    continue;
  }
  return 0;
}
)");
  expect(!invalid.canGenerateCode(),
         "invalid switch subjects, labels, and arms should block lowering");
  expect(
      countDiagnosticCode(invalid.diagnostics, "GTI-S2037") == 7 &&
          hasDiagnostic(invalid.diagnostics,
                        "Switch expression must have an integer") &&
          hasDiagnostic(invalid.diagnostics,
                        "does not exactly match subject type") &&
          hasDiagnostic(invalid.diagnostics, "Duplicate switch case value") &&
          hasDiagnostic(invalid.diagnostics, "only one 'default' label") &&
          hasDiagnostic(invalid.diagnostics,
                        "must be a compile-time integer") &&
          hasDiagnostic(invalid.diagnostics,
                        "without an explicit terminator") &&
          hasDiagnostic(invalid.diagnostics,
                        "'continue' can only be used inside a loop") &&
          hasRelatedDiagnostic(invalid.diagnostics,
                               "First matching case label") &&
          hasDiagnosticHint(invalid.diagnostics,
                            "do not perform implicit conversions"),
      "switch diagnostics should explain exact matching, duplicates, and "
      "explicit termination");

  lang::Lexer lexer;
  lang::Parser recoveryParser(lexer.scan(R"(
void recover(int value) {
  switch (value) {
  case 0
    int skipped_arm = 0;
  case 1:
    break;
  default:
    break;
  }
}
)",
                                         "recover-switch.gti"));
  const lang::Program recovered = recoveryParser.parse();
  expect(recoveryParser.errors().size() == 1 &&
             !recoveryParser.errors().front().fixes.empty() &&
             recoveryParser.errors().front().fixes.front().replacement == ":",
         "a missing case colon should offer an insertion fix and recover");
  lang::SemanticVisitor recoveredSemantic;
  expect(recoveredSemantic.check(recovered),
         "switch recovery should retain later case and default arms");

  const std::string formatted = lang::Formatter().format(
      "void choose(mut int value){switch(value){case 0:case 1:value+=1;"
      "break;default:break;}}");
  expect(formatted == R"(void choose(mut int value) {
  switch (value) {
  case 0:
  case 1:
    value += 1;
    break;
  default:
    break;
  }
}
)" && lang::Formatter().format(formatted) == formatted,
         "switch formatting should match C++ label indentation and remain "
         "idempotent");
}

void testParserRecovery() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int first = ;
int second = ;
class Broken {
  return 1;
  int value = 2;
};
int main() { return 0; }
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();

  expect(parser.errors().size() == 3,
         "parser should report independent declaration errors");
  expect(program.declarations().size() == 2,
         "parser should recover and keep later declarations");
}

void testParserNestingLimit() {
  const auto nested = [](std::string_view prefix, std::string_view value,
                         std::string_view suffix, std::size_t depth) {
    std::string result;
    result.reserve(prefix.size() * depth + value.size() +
                   suffix.size() * depth);
    for (std::size_t index = 0; index < depth; ++index) {
      result += prefix;
    }
    result += value;
    for (std::size_t index = 0; index < depth; ++index) {
      result += suffix;
    }
    return result;
  };

  const std::array<std::pair<std::string, std::string>, 5> cases{{
      {"parentheses",
       "int32_t f() { return " + nested("(", "0", ")", 600) + "; }"},
      {"unary", "bool f() { return " + nested("!", "true", "", 600) + "; }"},
      {"calls",
       "int32_t g(int32_t value) { return value; } int32_t f() { return " +
           nested("g(", "1", ")", 600) + "; }"},
      {"statements", "int32_t f() { " +
                         nested("if (true) {", "return 0;", "}", 600) +
                         " return 0; }"},
      {"pointer-types", "int32_t" + std::string(600, '*') + " f();"},
  }};

  for (const auto &[name, source] : cases) {
    const std::string recoverableSource =
        source + " int32_t recovered() { return 0; }";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(recoverableSource, "deep-" + name + ".gti"));
    const lang::Program program = parser.parse();
    const lang::Diagnostic *diagnostic =
        findDiagnosticByCode(parser.errors(), "GTI-P0005");
    expect(
        diagnostic != nullptr &&
            countDiagnosticCode(parser.errors(), "GTI-P0005") == 1 &&
            diagnostic->message ==
                "Source nesting exceeds the parser limit of 64." &&
            diagnostic->hints ==
                std::vector<std::string>{"Split the expression, type, or "
                                         "statement into shallower parts."} &&
            diagnostic->primary.start < recoverableSource.size() &&
            diagnostic->fixes.empty() &&
            findTopLevelFunction(program, "recovered") != nullptr,
        "deep " + name +
            " should stop at one structured parser-depth diagnostic and "
            "recover the following declaration");
  }

  lang::Lexer shallowLexer;
  lang::Parser shallowParser(shallowLexer.scan(
      "int32_t f() { return " + nested("(", "0", ")", 32) + "; }",
      "shallow-nesting.gti"));
  (void)shallowParser.parse();
  expect(!shallowParser.hadError(),
         "ordinary bounded source nesting should remain accepted");
}

void testExecutablePathDiscovery() {
  const std::filesystem::path executable =
      lang::executablePath("not-the-running-test-binary");
  std::error_code error;
  expect(executable.is_absolute() &&
             std::filesystem::is_regular_file(executable, error),
         "native executable discovery should not depend on argv[0]");

  const lang::StandardLibraryLayout rootLayout =
      lang::standardLibraryLayout("/toolchain/share/gti/stdlib");
  const lang::StandardLibraryLayout legacyLayout =
      lang::standardLibraryLayout("/custom/prelude.gti");
  expect(rootLayout.root == "/toolchain/share/gti/stdlib" &&
             rootLayout.prelude == "/toolchain/share/gti/stdlib/prelude.gti" &&
             legacyLayout.root == "/custom" &&
             legacyLayout.prelude == "/custom/prelude.gti",
         "standard-library discovery should accept roots and legacy prelude "
         "paths");
}

void testFrontendStopPhase() {
  const std::string source = "int main() { return 0; }\n";

  lang::FrontendOptions semanticsOnly;
  semanticsOnly.stopAfter = lang::FrontendPhase::Semantics;
  const lang::FrontendResult stopped =
      lang::Frontend(semanticsOnly).analyze("stop-semantics.gti", source);
  expect(stopped.sourceValid && stopped.syntaxValid && stopped.semanticValid,
         "a semantics-only analysis should validate the frontend phases it "
         "runs");
  expect(stopped.semantics.expressionCount() > 0,
         "a semantics-only analysis should retain the semantic model");
  expect(!stopped.hirValid && !stopped.mirValid,
         "a semantics-only analysis must not mark HIR or MIR valid");
  expect(stopped.hir.functionInstances().empty(),
         "a semantics-only analysis must not lower HIR instances");
  expect(!stopped.canGenerateCode(),
         "a semantics-only analysis must leave code generation disabled");

  lang::FrontendOptions hirOnly;
  hirOnly.stopAfter = lang::FrontendPhase::Hir;
  const lang::FrontendResult hirStopped =
      lang::Frontend(hirOnly).analyze("stop-hir.gti", source);
  expect(hirStopped.hirValid && !hirStopped.mirValid &&
             !hirStopped.canGenerateCode(),
         "a HIR-stopped analysis should validate HIR but not MIR");

  const lang::FrontendResult full =
      lang::Frontend().analyze("stop-full.gti", source);
  expect(full.canGenerateCode(),
         "the default stop phase must keep the full pipeline");
}

void testTargetTripleParsing() {
  const lang::TargetInfo host = lang::TargetInfo::host();
  expect(host.dataLayout.supported() &&
             host.dataLayout.pointerWidthBits() == 64 &&
             host.dataLayout.littleEndian() &&
             host.executionProfile == lang::ExecutionProfile::SingleThreaded,
         "the host target should commit to the supported 64-bit "
         "little-endian layout and default single-threaded profile");
  expect(lang::parseExecutionProfile("single-threaded") ==
                 lang::ExecutionProfile::SingleThreaded &&
             lang::parseExecutionProfile("concurrent") ==
                 lang::ExecutionProfile::Concurrent &&
             !lang::parseExecutionProfile("threads") &&
             lang::executionProfileName(lang::ExecutionProfile::Concurrent) ==
                 "concurrent",
         "execution profiles should use one exact compiler-owned vocabulary");
  expect(lang::parseTargetProperty("os") == lang::TargetProperty::Os &&
             lang::parseTargetProperty("vendor") ==
                 lang::TargetProperty::Vendor &&
             lang::parseTargetProperty("arch") == lang::TargetProperty::Arch &&
             !lang::parseTargetProperty("platform") &&
             lang::targetPropertyName(lang::TargetProperty::Arch) == "arch" &&
             lang::targetEndiannessName(lang::TargetEndianness::Little) ==
                 "little" &&
             lang::targetScalarKindName(lang::TargetScalarKind::Float32) ==
                 "float" &&
             lang::targetScalarKindName(lang::TargetScalarKind::Float64) ==
                 "double",
         "target conditions and scalar facts should use one exact compiler-"
         "owned vocabulary");

  if (!lang::targetTripleParsingAvailable()) {
    expect(!lang::parseTargetTriple("arm64-apple-macos").has_value(),
           "triple parsing must decline cleanly when unavailable");
    return;
  }

  const std::optional<lang::TargetInfo> mac =
      lang::parseTargetTriple("aarch64-apple-darwin");
  expect(mac && mac->arch == "arm64" && mac->vendor == "apple" &&
             mac->os == "macos" && mac->dataLayout.supported() &&
             mac->dataLayout.pointerWidthBits() == 64 &&
             mac->dataLayout.endianness() == lang::TargetEndianness::Little,
         "an aarch64 darwin triple should normalize into GTI's vocabulary");
  const std::optional<lang::TargetInfo> macAlias =
      lang::parseTargetTriple("arm64-apple-macos");
  expect(macAlias && macAlias->arch == "arm64" && macAlias->os == "macos",
         "arm64 and aarch64 spellings should normalize identically");
  const std::optional<lang::TargetInfo> linux64 =
      lang::parseTargetTriple("x86_64-unknown-linux-gnu");
  expect(linux64 && linux64->arch == "x86_64" && linux64->os == "linux" &&
             linux64->vendor == "unknown",
         "a linux gnu triple should map to GTI's linux target");
  const std::optional<lang::TargetInfo> windows =
      lang::parseTargetTriple("x86_64-pc-windows-msvc");
  expect(windows && windows->os == "windows" && windows->vendor == "pc" &&
             windows->dataLayout == mac->dataLayout,
         "a windows msvc triple should map to GTI's windows target");

  const auto scalar = [](const lang::TargetInfo &target,
                         lang::TargetScalarKind kind, std::uint32_t size,
                         std::uint32_t alignment) {
    const std::optional<lang::TargetTypeLayout> layout =
        target.dataLayout.scalarLayout(kind);
    return layout && layout->sizeBytes == size &&
           layout->abiAlignmentBytes == alignment &&
           layout->preferredAlignmentBytes == alignment;
  };
  expect(scalar(*mac, lang::TargetScalarKind::Bool, 1, 1) &&
             scalar(*mac, lang::TargetScalarKind::Char, 1, 1) &&
             scalar(*mac, lang::TargetScalarKind::Int16, 2, 2) &&
             scalar(*mac, lang::TargetScalarKind::UInt32, 4, 4) &&
             scalar(*mac, lang::TargetScalarKind::Int64, 8, 8) &&
             scalar(*mac, lang::TargetScalarKind::Float32, 4, 4) &&
             scalar(*mac, lang::TargetScalarKind::Float64, 8, 8) &&
             scalar(*mac, lang::TargetScalarKind::Pointer, 8, 8),
         "supported triples should expose deterministic GTI-owned scalar "
         "layout facts");

  const lang::TargetTripleParseResult narrow =
      lang::parseTargetTripleResult("i386-pc-windows-msvc");
  const lang::TargetTripleParseResult bigEndian =
      lang::parseTargetTripleResult("aarch64_be-unknown-linux-gnu");
  const lang::TargetTripleParseResult unsupportedOs =
      lang::parseTargetTripleResult("x86_64-unknown-freebsd");
  const lang::TargetTripleParseResult malformed =
      lang::parseTargetTripleResult("banana");
  expect(!narrow.succeeded() &&
             narrow.error == lang::TargetTripleError::UnsupportedArchitecture &&
             !bigEndian.succeeded() &&
             bigEndian.error ==
                 lang::TargetTripleError::UnsupportedEndianness &&
             !unsupportedOs.succeeded() &&
             unsupportedOs.error ==
                 lang::TargetTripleError::UnsupportedOperatingSystem &&
             !malformed.succeeded() &&
             malformed.error == lang::TargetTripleError::Malformed,
         "triple parsing should distinguish malformed and unsupported target "
         "properties");
  expect(
      lang::targetTripleErrorMessage(bigEndian.error).find("little-endian") !=
          std::string_view::npos,
      "triple rejection should expose an actionable stable reason");

  lang::FrontendOptions unsupportedOptions;
  unsupportedOptions.target = {"unsupported", "unknown", "unknown",
                               lang::TargetDataLayout::unsupported()};
  const lang::FrontendResult unsupported =
      lang::Frontend(unsupportedOptions)
          .analyze("unsupported-target.gti", "int main() { return 0; }");
  const lang::Diagnostic *layoutDiagnostic =
      findDiagnosticByCode(unsupported.diagnostics, "GTI-S2062");
  expect(unsupported.sourceValid && !unsupported.canGenerateCode() &&
             layoutDiagnostic != nullptr &&
             layoutDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             layoutDiagnostic->primary.source.ends_with(
                 "unsupported-target.gti") &&
             layoutDiagnostic->primary.start == 0 &&
             layoutDiagnostic->primary.end == 0 &&
             layoutDiagnostic->message.find("no supported GTI data layout") !=
                 std::string::npos &&
             layoutDiagnostic->hints.size() == 1 &&
             layoutDiagnostic->hints.front().find("64-bit little-endian") !=
                 std::string::npos,
         "an unsupported selected layout should fail before parsing or code "
         "generation with one source-facing configuration diagnostic");
}

void testSourceModuleInterfaces() {
  const auto root =
      std::filesystem::temp_directory_path() / "gti-module-interfaces";
  const auto entry = root / "main.gti";
  const auto leaf =
      std::filesystem::weakly_canonical(root / "leaf.gti").string();
  const auto facade =
      std::filesystem::weakly_canonical(root / "facade.gti").string();
  const std::string interface = R"(
export module leaf;
namespace api {
  int helper() { return 40; }
  export int answer() { return helper() + 2; }
  int answer(int value) { return value; }
  struct Secret { int value; };
  export struct Record { int value; Record(int input) : value(input) {} };
  export using Count = int;
  export enum class State { ready };
  export int identity<T>(T value) { return helper() + 2; }
}
)";
  const std::unordered_map<std::string, std::string> sources{
      {leaf, interface},
      {facade, "export module facade;\nexport import \"leaf.gti\";\n"}};
  const auto analyze = [&](std::string source) {
    return lang::Frontend().analyze(entry, std::move(source), {}, sources);
  };
  const auto direct = analyze(R"(import "leaf.gti";
int main() {
  api::Record record{2};
  api::Count count = api::identity(record.value);
  api::State state = api::State::ready;
  return api::answer() == count && state == api::State::ready ? 0 : 1;
}
)");
  if (!direct.canGenerateCode()) {
    for (const auto &diagnostic : direct.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(direct.canGenerateCode(),
         "an imported interface exposes explicit declarations and preserves "
         "private helper/generic bodies");
  const auto reexported =
      analyze("import \"facade.gti\";\nint main() { return api::answer(); }\n");
  expect(reexported.canGenerateCode(),
         "an explicit re-export makes the leaf API visible");
  const auto hiddenFunction =
      analyze("import \"leaf.gti\";\nint main() { return api::helper(); }\n");
  expect(
      !hiddenFunction.semanticValid &&
          hasDiagnostic(hiddenFunction.diagnostics, "not exported"),
      "an interface's private namespace function must not leak to consumers");
  const auto hiddenType =
      analyze("import \"leaf.gti\";\nint main() { api::Secret secret{1}; "
              "return secret.value; }\n");
  expect(!hiddenType.semanticValid &&
             hasDiagnostic(hiddenType.diagnostics, "not exported"),
         "an interface's private type must not leak to consumers");
  const auto hiddenOverload =
      analyze("import \"leaf.gti\";\nint main() { return api::answer(1); }\n");
  expect(!hiddenOverload.semanticValid,
         "exporting one overload must not expose a private overload");
  auto privateSources = sources;
  privateSources[facade] =
      "export module facade;\nimport \"leaf.gti\";\nexport int wrapper() { "
      "return api::answer(); }\n";
  const auto privateImport = lang::Frontend().analyze(
      entry, "import \"facade.gti\";\nint main() { return api::answer(); }\n",
      {}, privateSources);
  expect(!privateImport.semanticValid,
         "ordinary imports must not re-export their dependencies");
  const auto wrapper = lang::Frontend().analyze(
      entry, "import \"facade.gti\";\nint main() { return wrapper(); }\n", {},
      privateSources);
  expect(wrapper.canGenerateCode(),
         "a public wrapper can use a private import internally");
  const auto bypass =
      analyze("#include \"leaf.gti\"\nint main() { return api::helper(); }\n");
  expect(!bypass.sourceValid &&
             hasDiagnosticCode(bypass.diagnostics, "GTI-I0012"),
         "include must not bypass a module interface's exports");
  const auto noInterface =
      lang::Frontend().analyze(entry, "export int value() { return 1; }\n");
  expect(!noInterface.semanticValid &&
             hasDiagnosticCode(noInterface.diagnostics, "GTI-S2098"),
         "export outside an explicit interface must be diagnosed");
  const auto staticExport = lang::Frontend().analyze(
      entry,
      "export module invalid;\nexport static int helper() { return 1; }\n");
  expect(!staticExport.semanticValid &&
             hasDiagnosticCode(staticExport.diagnostics, "GTI-S2098"),
         "internal-linkage declarations cannot be exported");

  auto changed = sources;
  changed[leaf] = R"(export module leaf;
namespace api {
  int answer(int value) { return value; }
  export int answer() { return 42; }
  using Hidden = int;
  export constexpr int size = 2;
}
export namespace visible { int value() { return api::answer(); } }
)";
  const auto reversed = lang::Frontend().analyze(
      entry,
      "import \"leaf.gti\"; static_assert(api::size == 2); int main() { return "
      "visible::value() + api::size; }",
      {}, changed);
  if (!reversed.canGenerateCode()) {
    for (const auto &diagnostic : reversed.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(reversed.canGenerateCode(),
         "exports work after private overloads, in namespace groups, and in "
         "constant contexts");
  const auto hiddenAlias =
      lang::Frontend().analyze(entry,
                               "import \"leaf.gti\"; int main() { api::Hidden "
                               "value = 1; return value; }",
                               {}, changed);
  expect(!hiddenAlias.semanticValid, "private aliases must not leak");

  lang::FrontendOptions cachedOptions;
  cachedOptions.cache = std::make_shared<lang::FrontendCache>();
  const lang::Frontend cached(cachedOptions);
  const std::string consumer =
      "import \"leaf.gti\"; int main() { return api::answer(); }";
  const auto first = cached.analyze(entry, consumer, {}, changed);
  const auto hit = cached.analyze(entry, consumer, {}, changed);
  changed[leaf] =
      "export module leaf; namespace api { int answer() { return 42; } }";
  const auto revoked = cached.analyze(entry, consumer, {}, changed);
  expect(first.canGenerateCode() && hit.canGenerateCode() &&
             !revoked.semanticValid,
         "parsed cache reuse must preserve exports and invalidate after an "
         "export is revoked");

  changed[leaf] = R"(export module leaf;
#define MODULE_SELECTED
#undef MODULE_SELECTED
namespace inactive {
#if defined(MODULE_SELECTED)
  export int answer() { return 1; }
#endif
  int helper() { return 0; }
}
namespace active {
#if !defined(MODULE_SELECTED)
  export int answer() { return 42; }
#endif
}
)";
  const auto inactiveNamespace =
      cached.analyze(entry,
                     "import \"leaf.gti\"; namespace alias = inactive; int "
                     "main() { return 0; }",
                     {}, changed);
  expect(!inactiveNamespace.semanticValid,
         "an inactive export must not publish its private enclosing namespace");
  expect(cached
             .analyze(
                 entry,
                 "import \"leaf.gti\"; int main() { return active::answer(); }",
                 {}, changed)
             .canGenerateCode(),
         "an active conditional export publishes its enclosing namespace");

  const std::string formatted = lang::Formatter().format(
      "export module leaf;export import other.main;export namespace api{int "
      "answer(){return 42;}}");
  expect(formatted.find("export module leaf;") != std::string::npos &&
             formatted.find("export import other.main;") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted,
         "module syntax should format without token resplitting and remain "
         "idempotent");
  for (const std::string &source :
       {std::string("export module vector;\nexport #include <std/vector>\n"
                    "import std.span;\n"),
        std::string("export module vector;\nexport   #include <std/vector>\n"
                    "import std.span;\n"),
        std::string("export module vector;\nexport\n#include <std/vector>\n"
                    "import std.span;\n")}) {
    const std::string republished = lang::Formatter().format(source);
    expect(republished == "export module vector;\nexport #include "
                          "<std/vector>\nimport std.span;\n",
           "export #include keeps its prefix on the directive line: " +
               republished);
  }
}

void testExportPlacement() {
  const auto root =
      std::filesystem::temp_directory_path() / "gti-export-placement";
  const auto entry = root / "main.gti";
  const auto api = std::filesystem::weakly_canonical(root / "api.gti").string();
  const auto analyze = [&](const std::string &interface) {
    return lang::Frontend().analyze(
        entry,
        "import \"api.gti\";\nint main() { return api::checked(1) - 2; }\n", {},
        {{api, interface}});
  };
  const auto interfaceWith = [](const std::string &prefix) {
    return "export module api;\nnamespace api {\n  " + prefix +
           " int checked(int value) { return value + 1; }\n}\n";
  };
  expect(analyze(interfaceWith("export [[track_caller]]")).canGenerateCode(),
         "export precedes a declaration's attribute list");

  struct Misplaced {
    std::string prefix;
    std::optional<std::string> replacement;
  };
  for (const Misplaced &misplaced :
       {Misplaced{"[[track_caller]] export", "export [[track_caller]]"},
        Misplaced{"constexpr export", "export constexpr"},
        Misplaced{"[[track_caller]] /* traced */ [[discardable]]\n  export",
                  "export [[track_caller]] /* traced */ [[discardable]]"},
        Misplaced{"[[track_caller]] /* before */ export", std::nullopt}}) {
    const std::string interface = interfaceWith(misplaced.prefix);
    const auto result = analyze(interface);
    const std::size_t start = interface.find(misplaced.prefix);
    const std::size_t keyword =
        interface.rfind("export", start + misplaced.prefix.size());
    // Recovery publishes the declaration as exported, so the importer's call
    // adds no visibility error to the misplaced-export syntax error.
    expect(result.diagnostics.size() == 1 && !result.canGenerateCode(),
           "a misplaced export is one syntax error: " + misplaced.prefix);
    if (result.diagnostics.size() != 1) {
      continue;
    }
    const lang::Diagnostic &diagnostic = result.diagnostics.front();
    expect(diagnostic.code == "GTI-P0001" &&
               diagnostic.message.find("'export' must begin the declaration") !=
                   std::string::npos &&
               diagnostic.primary.source == api &&
               diagnostic.primary.start == keyword &&
               diagnostic.primary.end == keyword + 6,
           "the misplaced export is reported at its keyword: " +
               misplaced.prefix);
    if (!misplaced.replacement) {
      expect(diagnostic.fixes.empty(),
             "a comment before the misplaced export withholds the edit");
      continue;
    }
    expect(diagnostic.fixes.size() == 1 &&
               diagnostic.fixes.front().span.start == start &&
               diagnostic.fixes.front().span.end == keyword + 6 &&
               diagnostic.fixes.front().replacement == *misplaced.replacement,
           "the edit moves export before the verbatim prefix: " +
               misplaced.prefix);
    if (diagnostic.fixes.size() == 1) {
      std::string fixed = interface;
      fixed.replace(start, keyword + 6 - start, *misplaced.replacement);
      expect(analyze(fixed).canGenerateCode(),
             "applying the edit yields a valid exported declaration: " + fixed);
    }
  }

  const std::string spaced = lang::Formatter().format(
      "export module api;\nexport[[track_caller]] void check(bool ready) {}\n"
      "int choose(bool ready) {\n  if (ready)[[discard]] choose(false);\n"
      "  else[[discard]] choose(true);\n  return 0;\n}\n"
      "int first(int[2] values) { return values[[](int i) -> int { return i; "
      "}(0)]; }\n"
      "export[[track_caller]] [[discardable]] int both() { return 1; }\n");
  expect(
      spaced.find("export [[track_caller]] void check") != std::string::npos &&
          spaced.find("if (ready) [[discard]] choose") != std::string::npos &&
          spaced.find("else [[discard]] choose") != std::string::npos &&
          spaced.find("values[") != std::string::npos &&
          spaced.find("values [") == std::string::npos &&
          spaced.find("export [[track_caller]][[discardable]] int both") !=
              std::string::npos &&
          lang::Formatter().format(spaced) == spaced,
      "an attribute list is spaced from a preceding keyword, while "
      "consecutive lists and a lambda subscript stay attached: " +
          spaced);
  const std::string misplaced =
      "export module api;\n[[track_caller]] export void check(bool ready) {}\n";
  expect(lang::Formatter().format(misplaced) == misplaced,
         "the formatter leaves a misplaced export for the parser's fix");
}

void testOwnLineCommentPlacement() {
  const auto expectLayout = [](const std::string &input,
                               const std::string &expected,
                               const std::string &message) {
    const std::string formatted = lang::Formatter().format(input);
    expect(formatted == expected, message + ":\n" + formatted);
    expect(lang::Formatter().format(formatted) == formatted,
           message + " (idempotent)");
  };

  // A comment alone on its line describes what follows, so formatting keeps
  // it there instead of attaching it to the statement before it.
  const std::string namespaceScope = "int y = 2;\n// own line\nint x = 1;\n";
  expectLayout(namespaceScope, namespaceScope,
               "an own-line comment after a namespace-scope statement");
  const std::string body = "int f() {\n  int y = 2;\n  // own line\n"
                           "  return y;\n}\n";
  expectLayout(body, body, "an own-line comment in a function body");
  expectLayout("int f(){int y=2;\n// own line\nreturn y;}", body,
               "an own-line comment keeps its line while its block reflows");
  const std::string afterFunction =
      "int f() {\n  return 1;\n}\n// next\nint g() {}\n// last\n";
  expectLayout(afterFunction, afterFunction,
               "an own-line comment after a closing brace");
  const std::string interface =
      "export module api;\n// The public surface.\nexport int answer();\n";
  expectLayout(interface, interface,
               "an own-line comment after a module declaration");
  const std::string importer =
      "import \"a.gti\";\n// Entry point.\nint main() {\n  return 0;\n}\n";
  expectLayout(importer, importer, "an own-line comment after an import");
  const std::string ownBlock = "int f() {\n  int y = 2;\n  /* own */\n"
                               "  /* lead */ return y;\n}\n";
  expectLayout(ownBlock, ownBlock,
               "own-line block comments neither trail nor join statements");

  const std::string trailing =
      "int y = 2; // note\nint f() {\n  int z = 3; /* note */\n"
      "  if (z > 0) {\n    return z;\n  } // done\n  return 0;\n}\n";
  expectLayout(trailing, trailing, "trailing comments stay trailing");
  expectLayout("int y=2;// note\nint x=1;/* note */\n",
               "int y = 2; // note\nint x = 1; /* note */\n",
               "trailing comments stay beside reformatted statements");
}

void testSourceModuleResolution() {
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path() / "gti-module-resolution");
  const auto app = root / "app";
  const auto library = root / "library";
  const auto entry = app / "main.gti";
  const auto api = library / "api.gti";
  const auto local = app / "local.gti";
  std::unordered_map<std::string, std::string> sources{
      {api.string(), "export module main; export int answer() { return 42; }"},
      {local.string(),
       "export module local; export int local_value() { return 2; }"}};
  std::vector<lang::PackageSourceRoot> packages{
      {.identity = "app",
       .name = "app",
       .packageRoot = app,
       .sourceRoot = app,
       .dependencies = {{.alias = "lib", .targetIdentity = "library"}},
       .modules = {{"local", local}}},
      {.identity = "library",
       .name = "library",
       .packageRoot = library,
       .sourceRoot = library,
       .modules = {{"main", api}}}};
  const auto analyze = [&](const std::string &source) {
    return lang::Frontend().analyze(entry, source, {}, sources, {}, packages);
  };
  const auto imported = analyze("import lib; import local; int main() { return "
                                "answer() - local_value(); }");
  expect(imported.canGenerateCode(),
         "package default and local public module names resolve");
  expect(analyze("import lib.main; int main() { return answer(); }")
             .canGenerateCode(),
         "explicit dependency module names resolve");
  expect(analyze("int before() { return 0; } import lib; "
                 "struct Item { int value = 1; }; import local; "
                 "namespace n { int value = 2; } import lib.main; "
                 "int main() { return answer() + local_value(); }")
             .canGenerateCode(),
         "imports remain valid between complete functions, records, and "
         "namespaces");
  for (const std::string source :
       {"int main(import lib;) { return answer(); }",
        "int value = import lib; 1;", "int value import lib; = 1;",
        "int before(int value = import lib; 1) { return value; }",
        "struct Item { int value = 1; } import lib; ;"}) {
    const auto result = analyze(source);
    const auto *diagnostic =
        findDiagnosticByCode(result.diagnostics, "GTI-P0001");
    const auto start = source.find("import");
    expect(result.sourceValid && !result.syntaxValid && diagnostic != nullptr &&
               diagnostic->primary.source == entry.string() &&
               diagnostic->primary.start == start &&
               diagnostic->primary.end == start + 6,
           "misplaced imports retain their parser-visible source span: " +
               source);
    expect(!result.canGenerateCode(),
           "a misplaced import cannot be erased into a valid program");
  }
  lang::FrontendOptions cachedOptions;
  cachedOptions.cache = std::make_shared<lang::FrontendCache>();
  const lang::Frontend cached(cachedOptions);
  expect(cached.analyze(entry, "import lib; int main() { return answer(); }",
                        {}, sources, {}, packages)
                 .canGenerateCode() &&
             !cached
                  .analyze(entry, "int main(import lib;) { return answer(); }",
                           {}, sources, {}, packages)
                  .syntaxValid,
         "cached syntax cannot erase an import moved inside a declaration");
  const auto ordinaryLocal = sources[local.string()];
  sources[local.string()] =
      "export module local; int before(export import lib;) { return 0; }";
  const auto misplacedReexport = analyze("import local;");
  const auto *reexportDiagnostic =
      findDiagnosticByCode(misplacedReexport.diagnostics, "GTI-P0001");
  expect(misplacedReexport.sourceValid && !misplacedReexport.syntaxValid &&
             reexportDiagnostic != nullptr &&
             reexportDiagnostic->primary.source == local.string() &&
             reexportDiagnostic->primary.start ==
                 sources[local.string()].find("export import"),
         "a re-export embedded in a declaration retains its original span");
  sources[local.string()] = ordinaryLocal;
  const auto unit = imported.sourceGraph.sourceUnitForPath(api.string());
  const auto *loaded = imported.sourceGraph.findUnit(unit);
  expect(loaded != nullptr && loaded->packageIdentity == "library" &&
             !imported.sourceGraph.isCompilerTrusted(unit),
         "module imports retain package provenance without gaining compiler "
         "trust");

  for (const std::string source :
       {"import absent;", "import lib.absent;", "import transitive.main;",
        "import lib.main.extra;", "import lib.;", "import lib",
        "import \"../library/api.gti\";", "import \"missing.gti\";",
        "import \"main.cpp\";", "namespace n { import lib; }",
        "#if true\nimport lib;\n#endif\n", "export import lib;", "module nope;",
        "int value = 0; export module late;"}) {
    const auto result = analyze(source);
    expect(!result.sourceValid &&
               hasDiagnosticCode(result.diagnostics, "GTI-I0012"),
           "invalid source module import/header must fail before semantics: " +
               source);
  }
  const auto direct =
      lang::Frontend().analyze(entry, "import lib;", {}, sources);
  expect(!direct.sourceValid &&
             hasDiagnostic(direct.diagnostics, "owning package graph"),
         "direct compilation must not discover named modules implicitly");
  packages[0].modules.push_back({"lib", local});
  expect(!analyze("import lib;").sourceValid,
         "a local module/dependency-alias collision is diagnosed");
  packages[0].modules.pop_back();
  sources[api.string()] =
      "export module wrong; export int answer() { return 42; }";
  const auto mismatch = analyze("import lib;");
  const auto *mismatchDiagnostic =
      findDiagnosticByCode(mismatch.diagnostics, "GTI-I0012");
  expect(!mismatch.sourceValid && mismatchDiagnostic != nullptr &&
             mismatchDiagnostic->primary.start == 0 &&
             mismatchDiagnostic->primary.end == 6 &&
             mismatchDiagnostic->related.size() == 1 &&
             mismatchDiagnostic->related[0].span.source == api.string(),
         "module name mismatch retains the import span and exact interface "
         "declaration");
  sources[api.string()] = "int answer() { return 42; }";
  expect(!analyze("import lib;").sourceValid,
         "legacy units cannot be implicitly imported as interfaces");
  sources[api.string()] =
      "export module main; export int answer() { return 42; }";
  const auto duplicate = app / "duplicate.gti";
  sources[duplicate.string()] =
      "export module local; export int another() { return 1; }";
  expect(!analyze("import local; import \"duplicate.gti\";").sourceValid,
         "distinct interface paths cannot declare the same package-local "
         "module name");
  sources[local.string()] = "export module local; import \"duplicate.gti\";";
  sources[duplicate.string()] =
      "export module duplicate; import \"local.gti\";";
  const auto cycle = analyze("import local;");
  expect(!cycle.sourceValid &&
             hasDiagnosticCode(cycle.diagnostics, "GTI-I0001"),
         "module import cycles use the existing source-cycle gate");

  sources[local.string()] = R"(export module local;
#if defined(IMPORTER)
#error "importer flags must not leak"
#endif
#define MODULE_LOCAL
export int local_value() { return 1; }
)";
  const auto flags = analyze(R"(#define IMPORTER
import local;
#if defined(MODULE_LOCAL)
#error "module flags must not leak"
#endif
int main() { return local_value(); }
)");
  expect(flags.canGenerateCode(),
         "source-local flags cannot configure a module or escape it");
  const auto shared = app / "shared.gti";
  sources[shared.string()] =
      "#if defined(IMPORTER)\nint configured = 1;\n#endif\n";
  sources[local.string()] = "export module local;\n#include \"shared.gti\"\n";
  const auto ambiguousFlags =
      analyze("#define IMPORTER\n#include \"shared.gti\"\nimport local;\n");
  expect(!ambiguousFlags.sourceValid &&
             hasDiagnosticCode(ambiguousFlags.diagnostics, "GTI-I0012"),
         "shared legacy units cannot silently inherit inconsistent module flag "
         "contexts");
}

void testSourceModuleConfigurationDomains() {
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path() / "gti-module-configuration");
  const auto entry = root / "main.gti";
  const auto flags = root / "flags.gti";
  const auto parent = root / "parent.gti";
  const auto first = root / "first.gti";
  const auto second = root / "second.gti";
  std::unordered_map<std::string, std::string> sources{
      {flags.string(), "#define ENABLED\n#undef STARTED\n"},
      {parent.string(), "#include \"flags.gti\"\n"}};
  const auto interface = [](std::string name) {
    return "export module " + name + ";\n" + R"(#include "parent.gti"
#if defined(ENABLED) && !defined(STARTED)
export int )" +
           name + R"(_value() { return 1; }
#endif
#undef ENABLED
#include "flags.gti"
#include "parent.gti"
#if defined(ENABLED)
#error "a repeated include must not reapply its define"
#endif
#define MODULE_LOCAL
)";
  };
  sources[first.string()] = interface("first");
  sources[second.string()] = interface("second");
  lang::FrontendOptions options;
  options.configurationFlags = {"STARTED"};
  options.cache = std::make_shared<lang::FrontendCache>();
  const lang::Frontend frontend(options);
  const auto analyze = [&](const std::string &source) {
    return frontend.analyze(entry, source, {}, sources);
  };
  const auto consumer = [](bool reverse) {
    return std::string("#define MODULE_LOCAL\n#undef MODULE_LOCAL\n") +
           (reverse ? "import \"second.gti\";\nimport \"first.gti\";\n"
                    : "import \"first.gti\";\nimport \"second.gti\";\n") +
           R"(#if defined(ENABLED) || defined(MODULE_LOCAL) || !defined(STARTED)
#error "module flags must not enter the importer"
#endif
int main() { return first_value() + second_value(); }
)";
  };
  for (const bool reverse : {false, true}) {
    const auto result = analyze(consumer(reverse));
    expect(result.canGenerateCode(),
           "shared nested includes replay defines and undefs once per module "
           "domain, independently of import order and lexical-cache reuse");
    expect(result.sourceGraph.sourceUnits().size() == 5 &&
               result.sourceGraph.dependencyEdges().size() == 9,
           "configuration replay must retain one source identity and one edge "
           "per written directive, without duplicating shared-unit edges");
  }

  // Reusing a saved parent output flag set would incorrectly restore ENABLED:
  // the child has already run in the second domain and must remain included.
  sources[first.string()] = R"(export module first;
#include "parent.gti"
export int first_value() { return 1; }
)";
  sources[second.string()] = R"(export module second;
#include "flags.gti"
#undef ENABLED
#define STARTED
#include "parent.gti"
#if defined(ENABLED) || !defined(STARTED)
#error "a cached parent must respect this domain's child include history"
#endif
export int second_value() { return 1; }
)";
  for (const bool reverse : {false, true}) {
    expect(
        analyze(consumer(reverse)).canGenerateCode(),
        "a reused parent processes actual nested include history rather than "
        "blindly restoring its original output flags");
  }

  // The canonical syntax cannot depend on whether another module happened to
  // load it first, even when both parent entries have equal incoming flags.
  sources[parent.string()] = R"(#include "flags.gti"
#if defined(ENABLED)
int configured_value() { return 1; }
#endif
)";
  const auto divergent = analyze(consumer(false));
  const auto *mismatch =
      findDiagnosticByCode(divergent.diagnostics, "GTI-I0012");
  const std::size_t includeOffset =
      sources[second.string()].find("#include \"parent.gti\"");
  expect(!divergent.sourceValid && mismatch != nullptr &&
             mismatch->message.find("selects different syntax") !=
                 std::string::npos &&
             mismatch->primary.source == second.string() &&
             mismatch->primary.start == includeOffset &&
             mismatch->primary.end == includeOffset + 8 &&
             mismatch->fixes.empty(),
         "different selected syntax must fail at the importing include span "
         "without guessing a configuration-changing fix");
  expect(!analyze(consumer(true)).sourceValid,
         "incompatible shared configurations must fail in either import order");

  sources[parent.string()] = "#include \"flags.gti\"\n";
  sources[second.string()] = R"(export module second;
#define SECOND_ONLY
#include "parent.gti"
export int second_value() { return 1; }
)";
  for (const bool reverse : {false, true}) {
    expect(analyze(consumer(reverse)).canGenerateCode(),
           "a shared unit replays under different incoming flags when it "
           "selects the same canonical syntax");
  }
  sources[parent.string()] = R"(#include "flags.gti"
#ifdef SECOND_ONLY
int second_only_value() { return 1; }
#endif
)";
  for (const bool reverse : {false, true}) {
    const auto observed = analyze(consumer(reverse));
    expect(!observed.sourceValid &&
               hasDiagnostic(observed.diagnostics, "selects different syntax"),
           "a differing incoming flag the shared unit observes is still "
           "rejected in either import order");
  }
  sources[parent.string()] = "#include \"flags.gti\"\n";

  // These sources are overlays. Mutating an overlay and analyzing again with
  // the same cache must replay the new bytes rather than a previous flag set.
  sources[first.string()] = interface("first");
  sources[second.string()] = interface("second");
  sources[flags.string()] = "#define ENABLED\n";
  expect(!analyze(consumer(false)).semanticValid,
         "editing a shared include overlay invalidates its configuration");
  sources[flags.string()] = "#define ENABLED\n#undef STARTED\n";
  expect(analyze(consumer(false)).canGenerateCode(),
         "restoring a shared include overlay restores every module domain");
  sources[flags.string()] += "#include \"parent.gti\"\n";
  const auto cycle = analyze(consumer(false));
  expect(!cycle.sourceValid &&
             hasDiagnosticCode(cycle.diagnostics, "GTI-I0001"),
         "per-domain include replay must preserve cycle rejection");
}

void testStandardLibraryModules() {
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path() / "gti-standard-modules");
  const auto entry = root / "main.gti";
  const auto facade = root / "facade.gti";
  const auto legacy = root / "legacy.gti";
  const auto nested = root / "nested.gti";
  const auto local = root / "vector.gti";
  std::unordered_map<std::string, std::string> sources{
      {facade.string(), "export module facade;\nexport import std.vector;\n"
                        "export #include \"legacy.gti\"\n"},
      {legacy.string(),
       "#include \"nested.gti\"\nnamespace old { int answer() { return "
       "hidden() + 2; } }\n"},
      {nested.string(), "int hidden() { return 40; }\n"},
      {local.string(),
       "export module vector;\nexport int local_vector() { return 1; }\n"}};
  const auto analyze =
      [&](const std::string &source,
          const std::vector<lang::PackageSourceRoot> &packages = {}) {
        return lang::Frontend().analyze(entry, source,
                                        {standardLibraryPrelude()}, sources,
                                        {standardLibraryRoot()}, packages);
      };
  const auto report = [](const lang::FrontendResult &result) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  };

  const auto imported = analyze(R"(import std.vector;
import std.string;
int main() {
  mut std::vector<int32_t> values{};
  values.push_back(42);
  std::string text = std::string("ready");
  return values[std::size_t(0)] == 42 && text.size() == std::size_t(5) ? 0
                                                                       : 1;
}
)");
  if (!imported.canGenerateCode()) {
    report(imported);
  }
  expect(imported.canGenerateCode(),
         "direct compilation imports standard-library modules by name");
  const lang::SourceUnit *bridge = nullptr;
  const lang::SourceUnit *vectorUnit = nullptr;
  for (const lang::SourceUnit &unit : imported.sourceGraph.sourceUnits()) {
    if (unit.standardLibraryName == "modules/vector") {
      bridge = &unit;
    } else if (unit.standardLibraryName == "std/vector") {
      vectorUnit = &unit;
    }
  }
  bool importEdge = false;
  bool republishEdge = false;
  for (const lang::SourceDependency &dependency :
       imported.sourceGraph.dependencyEdges()) {
    importEdge =
        importEdge || (bridge != nullptr &&
                       dependency.source == imported.sourceGraph.entryUnit() &&
                       dependency.target == bridge->id &&
                       dependency.kind == lang::SourceDependencyKind::Import);
    republishEdge =
        republishEdge ||
        (bridge != nullptr && vectorUnit != nullptr &&
         dependency.source == bridge->id &&
         dependency.target == vectorUnit->id &&
         dependency.kind == lang::SourceDependencyKind::ExportInclude);
  }
  expect(bridge != nullptr && vectorUnit != nullptr &&
             bridge->role == lang::SourceUnitRole::StandardLibrary &&
             bridge->moduleDeclaration &&
             bridge->moduleDeclaration->lexeme == "vector" &&
             vectorUnit->role == lang::SourceUnitRole::StandardLibrary &&
             importEdge && republishEdge,
         "std.vector is a standard-library interface that republishes the "
         "trusted <std/vector> unit");

  const auto nestedHidden =
      analyze("import std.vector;\nint main() { std::span<int32_t> view = "
              "std::span<int32_t>(); return int32_t(view.size()); }\n");
  expect(!nestedHidden.semanticValid &&
             hasDiagnosticCode(nestedHidden.diagnostics, "GTI-S2024"),
         "a republished unit does not republish its own nested includes");

  const auto aggregate = analyze("import std;\nint main() { return 0; }\n");
  expect(!aggregate.sourceValid &&
             hasDiagnostic(aggregate.diagnostics, "no aggregate module"),
         "the standard library has no aggregate module");
  const auto missing =
      analyze("import std.missing;\nint main() { return 0; }\n");
  expect(!missing.sourceValid &&
             hasDiagnostic(missing.diagnostics,
                           "Standard-library module 'std.missing' was not "
                           "found"),
         "an unknown standard-library module is a source-loading error");

  const auto throughFacade = analyze(R"(import "facade.gti";
int main() {
  mut std::vector<int32_t> values{};
  values.push_back(old::answer());
  return values[std::size_t(0)] == 42 ? 0 : 1;
}
)");
  if (!throughFacade.canGenerateCode()) {
    report(throughFacade);
  }
  expect(throughFacade.canGenerateCode(),
         "an application interface re-exports a standard module and "
         "republishes its own legacy unit");
  const auto facadeNested =
      analyze("import \"facade.gti\";\nint main() { return hidden(); }\n");
  expect(!facadeNested.semanticValid &&
             hasDiagnosticCode(facadeNested.diagnostics, "GTI-S2024"),
         "export #include republishes exactly one legacy unit");

  sources[legacy.string()] = "export #include \"nested.gti\"\n";
  const auto outsideInterface =
      analyze("#include \"legacy.gti\"\nint main() { return hidden(); }\n");
  expect(!outsideInterface.sourceValid &&
             hasDiagnostic(outsideInterface.diagnostics,
                           "Republishing an include requires"),
         "export #include is valid only in an interface");
  sources[facade.string()] =
      "export module facade;\nexport #include \"vector.gti\"\n";
  const auto republishedInterface =
      analyze("import \"facade.gti\";\nint main() { return 0; }\n");
  expect(!republishedInterface.sourceValid &&
             hasDiagnostic(republishedInterface.diagnostics,
                           "republished with 'export import'"),
         "an interface is re-exported with export import, never "
         "republished as an include");

  const auto sameName = analyze(R"(import std.vector;
import "vector.gti";
int main() {
  std::vector<int32_t> values{};
  return int32_t(values.size()) + local_vector() - 1;
}
)");
  if (!sameName.canGenerateCode()) {
    report(sameName);
  }
  expect(sameName.canGenerateCode(),
         "an application module may share a standard-library module name");

  const auto mixed = analyze(R"(#define APPLICATION_FLAG
#include <std/vector>
import std.vector;
import std.string;
int main() {
  std::vector<int32_t> values{};
  return int32_t(values.size());
}
)");
  if (!mixed.canGenerateCode()) {
    report(mixed);
  }
  expect(mixed.canGenerateCode(),
         "a legacy standard unit is shared with a module domain whose "
         "source-local flags it never observes");

  std::vector<lang::PackageSourceRoot> packages{{.identity = "app",
                                                 .name = "app",
                                                 .packageRoot = root,
                                                 .sourceRoot = root}};
  const auto project =
      analyze("import std.vector;\nint main() { std::vector<int32_t> values{}; "
              "return int32_t(values.size()); }\n",
              packages);
  if (!project.canGenerateCode()) {
    report(project);
  }
  expect(project.canGenerateCode(),
         "project compilation resolves the reserved std module owner before "
         "package aliases");
}

void testReachableExecutableLowering() {
  const auto entry = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path() / "gti-reachable-lowering.gti");
  const auto analyze = [&](const std::string &source,
                           bool reachableOnly = true) {
    lang::FrontendOptions options;
    options.lowerReachableOnly = reachableOnly;
    return lang::Frontend(options).analyze(
        entry, source, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  const std::string fixture = R"(
class Used {
  int value = 1;
public:
  int read() { return this.value; }
  int unread() { return 2; }
};
class Unused {
  int value = 3;
public:
  int read() { return this.value; }
};
int initialize_only() { return 7; }
int unreferenced_global = initialize_only();
class StaticOnly {
public:
  static int value = initialize_only();
};
using UnusedAlias = Unused;
int unused_helper() {
  Unused unused{};
  return unused.read();
}
int used_helper() {
  Used used{};
  return used.read();
}
[[c_abi]] struct NativeRecord {
  int32_t value;
};
extern "C" {
  int32_t native_declared(NativeRecord record);
}
int main() { return used_helper() - 1; }
)";
  const auto lowered = analyze(fixture);
  if (!lowered.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : lowered.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(lowered.canGenerateCode(),
         "the reachable-lowering fixture should pass the frontend");
  std::set<std::string> functions;
  for (const lang::MirFunctionInstance &instance :
       lowered.mir.functionInstances()) {
    if (const lang::FunctionInfo *info =
            lowered.semantics.findFunction(instance.declaration)) {
      functions.insert(info->qualifiedName);
    }
  }
  std::set<std::string> classes;
  for (const lang::MirClassInstance &instance : lowered.mir.classInstances()) {
    if (const lang::ClassTypeInfo *info =
            lowered.semantics.findClassType(instance.declaration)) {
      classes.insert(info->qualifiedName);
    }
  }
  expect(functions.contains("main") && functions.contains("used_helper") &&
             functions.contains("Used::read") && classes.contains("Used"),
         "code reachable from the hosted entry is lowered");
  expect(!functions.contains("unused_helper") &&
             !functions.contains("Unused::read") &&
             !functions.contains("Used::unread") && !classes.contains("Unused"),
         "unreferenced functions, methods and classes are not lowered");
  expect(functions.contains("native_declared") &&
             classes.contains("NativeRecord"),
         "the C boundary is lowered whether or not GTI uses it");
  expect(functions.contains("initialize_only") &&
             classes.contains("StaticOnly"),
         "unreferenced global and static initialization remain program roots");
  expect(!gti_test::emitCppText(lowered).empty(),
         "reachable lowering retains valid declarations for aliases and roots");
  const auto complete = analyze(fixture, false);
  const bool completeHasUnused = std::any_of(
      complete.mir.functionInstances().begin(),
      complete.mir.functionInstances().end(),
      [&](const lang::MirFunctionInstance &instance) {
        const lang::FunctionInfo *info =
            complete.semantics.findFunction(instance.declaration);
        return info != nullptr && info->qualifiedName == "unused_helper";
      });
  expect(complete.canGenerateCode() && completeHasUnused,
         "without the option every declaration is lowered");

  const auto deadInstanceError = analyze(R"(
class Plain {
public:
  Plain() {}
};
T twice<T>(T value) { return value + value; }
int never_called() {
  Plain plain = Plain();
  Plain doubled = twice(plain);
  return 0;
}
int main() { return 0; }
)");
  expect(!deadInstanceError.canGenerateCode() &&
             lang::hasErrorDiagnostics(deadInstanceError.diagnostics),
         "a generic instance required only by unreachable code is still "
         "analyzed and reports its error");
}

void testSupportFacilities() {
  bool ran = false;
  expect(lang::runGuarded([&ran] { ran = true; }) && ran,
         "guarded execution should run work and report success");
  expect(!lang::endTimeTrace("unused.json"),
         "ending a time trace that was never started must fail cleanly");
  expect(lang::timeTraceAvailable() == lang::targetTripleParsingAvailable(),
         "LLVM-backed facilities should agree on availability");
}

constexpr NamedTest kTests[] = {
    {"testDefiniteReturnAnalysis", testDefiniteReturnAnalysis},
    {"testSourceUnitDependencyGraph", testSourceUnitDependencyGraph},
    {"testSourceModuleInterfaces", testSourceModuleInterfaces},
    {"testExportPlacement", testExportPlacement},
    {"testOwnLineCommentPlacement", testOwnLineCommentPlacement},
    {"testSourceModuleResolution", testSourceModuleResolution},
    {"testSourceModuleConfigurationDomains",
     testSourceModuleConfigurationDomains},
    {"testStandardLibraryImports", testStandardLibraryImports},
    {"testStandardLibraryModules", testStandardLibraryModules},
    {"testReachableExecutableLowering", testReachableExecutableLowering},
    {"testPackageImports", testPackageImports},
    {"testLoopControlStatements", testLoopControlStatements},
    {"testDoWhileStatements", testDoWhileStatements},
    {"testConditionalExpressions", testConditionalExpressions},
    {"testSwitchStatements", testSwitchStatements},
    {"testParserRecovery", testParserRecovery},
    {"testParserNestingLimit", testParserNestingLimit},
    {"testExecutablePathDiscovery", testExecutablePathDiscovery},
    {"testFrontendStopPhase", testFrontendStopPhase},
    {"testTargetTripleParsing", testTargetTripleParsing},
    {"testSupportFacilities", testSupportFacilities},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_frontend_tests");
  return runNamedTests(argc, argv, kTests, "frontend");
}
