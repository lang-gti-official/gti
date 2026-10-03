#include "compiler_test_fixture.h"

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
    if (dependency.kind == lang::SourceDependencyKind::StandardLibrary) {
      ++standardEdges;
    }
  }
  expect(arrayId != 0 && loadedArray != nullptr &&
             loadedArray->standardLibraryName == "std/array" &&
             standardEdges == 2 &&
             imported.sourceGraph.sourceUnits().size() == 3,
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
  expect(!missingMemory.canGenerateCode() &&
             hasDiagnosticCode(missingMemory.diagnostics, "GTI-S2000") &&
             hasDiagnostic(missingMemory.diagnostics,
                           "Undefined qualified name 'std::make_unique'"),
         "unique ownership should not remain ambient in the implicit prelude");

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
      "namespace gti_internal { class forged {}; }\n"
      "namespace private_alias = gti_internal;\n"
      "using forged_storage = gti_internal::prefix_storage<int>;\n"
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
      {{leakyKey, "namespace std { struct Leaky { public: "
                  "gti_internal::prefix_storage<int> capability; }; }\n"}},
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
      {{leakyKey, "namespace std { using leaked = "
                  "gti_internal::prefix_storage<int>; }\n"}},
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
      {{leakyKey, "namespace std { class Mixed { public: Mixed() {} "
                  "int select(int value) { return value; } "
                  "int select(gti_internal::prefix_storage<int> value) { "
                  "return 0; } }; }\n"}},
      {standardLibraryRoot()});
  expect(!mixedOverload.canGenerateCode() &&
             countDiagnosticCode(mixedOverload.diagnostics, "GTI-S2012") == 1 &&
             !hasRelatedDiagnostic(mixedOverload.diagnostics,
                                   "gti_internal::prefix_storage"),
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
        "namespace std { class PrivateBase { public: PrivateBase() {} "
        "int select(gti_internal::prefix_storage<int> value) { return 0; } }; "
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
        "namespace std { class PrivateMethod { public: PrivateMethod() {} "
        "int secret(gti_internal::prefix_storage<int> value) { return 0; } }; "
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
  expect(tcpCpp.find("class socket") != std::string::npos &&
             tcpCpp.find("runtime::socket(") != std::string::npos &&
             tcpCpp.find("open__gti_mir_failure(std::expected") !=
                 std::string::npos &&
             tcpCpp.find("runtime::close(") != std::string::npos &&
             tcpCpp.find("socket(socket &&other) noexcept") !=
                 std::string::npos &&
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
  const auto publication =
      generated.contents.find("std::construct_at(__gti_mir_out_result, "
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

  const std::array<std::pair<std::string, std::string>, 4> cases{{
      {"parentheses",
       "int32_t f() { return " + nested("(", "0", ")", 600) + "; }"},
      {"unary", "bool f() { return " + nested("!", "true", "", 600) + "; }"},
      {"calls",
       "int32_t g(int32_t value) { return value; } int32_t f() { return " +
           nested("g(", "1", ")", 600) + "; }"},
      {"statements", "int32_t f() { " +
                         nested("if (true) {", "return 0;", "}", 600) +
                         " return 0; }"},
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
    {"testStandardLibraryImports", testStandardLibraryImports},
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
