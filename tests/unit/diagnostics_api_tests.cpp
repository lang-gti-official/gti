// Diagnostic infrastructure without a source-level observable: token
// categories and SourceManager line/column mapping, bounded move-origin
// witnesses, analysis error-limit options, cache and raw visitor budget, and
// the merge of a trusted library instance's repeated internal failure
// (tests/fixtures/library_report_merge_prelude.gti, a custom prelude). Moved
// unchanged from compiler_diagnostics_tests.cpp; the source-observable parts
// are tests/ui/diagnostics/{foundation,error_limit} cases.
#include "../../src/compiler/semantic_diagnostics.h"
#include "gti/diagnostic.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "unit_test.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testDiagnosticFoundation() {
  for (const auto &[spelling, kind] : lang::keywords) {
    expect(lang::isKeywordToken(kind) || lang::isTypeKeywordToken(kind) ||
               lang::isOperatorToken(kind),
           "compiler token categories should cover keyword spelling '" +
               std::string(spelling) + "'");
  }
  expect(lang::isDirectiveToken(lang::TokenKind::HASH_IF) &&
             lang::isDirectiveToken(lang::TokenKind::HASH_INCLUDE) &&
             !lang::isDirectiveToken(lang::TokenKind::IDENTIFIER),
         "compiler token categories should own the directive range");

  lang::SourceManager sources;
  const std::string unicodePrefix = "\xF0\x9F\x99\x82value";
  sources.set("unicode.gti", unicodePrefix);
  const lang::SourceLocation unicodeLocation =
      sources.locate(lang::SourceSpan{"unicode.gti", 4, 9, 1});
  expect(unicodeLocation.line == 1 && unicodeLocation.column == 2,
         "source locations should count a UTF-8 scalar as one CLI column");

  const std::string multiLine = "first line\nsecond \xCE\xB2 line\nthird\n";
  sources.set("lines.gti", multiLine);
  const lang::SourceLocation secondLine = sources.locate(
      lang::SourceSpan{"lines.gti", multiLine.find("\xCE\xB2"), 0, 1});
  expect(secondLine.line == 2 && secondLine.column == 8 &&
             secondLine.lineStart == multiLine.find("second") &&
             secondLine.lineEnd == multiLine.find("\nthird"),
         "the line index should locate an interior line exactly");
  expect(sources.line(lang::SourceSpan{"lines.gti", multiLine.find("third"), 0,
                                       1}) == "third",
         "line() should return exact line content from the index");
  const lang::SourceLocation atNewline =
      sources.locate(lang::SourceSpan{"lines.gti", multiLine.find('\n'), 0, 1});
  expect(atNewline.line == 1 && atNewline.lineEnd == multiLine.find('\n'),
         "an offset at a line terminator should stay on its own line");
  const lang::SourceLocation pastEnd = sources.locate(lang::SourceSpan{
      "lines.gti", multiLine.size() + 10, multiLine.size() + 11, 1});
  expect(pastEnd.line == 4 && pastEnd.lineStart == multiLine.size(),
         "an out-of-range offset should clamp to the final line");
  const lang::SourceLocation missingSource =
      sources.locate(lang::SourceSpan{"absent.gti", 0, 0, 7});
  expect(missingSource.line == 7,
         "a missing source should fall back to the span line hint");

  lang::Lexer lexer;
  const std::string invalidEscape =
      "std::string_view value = \"first\nbad\\q\";";
  lexer.scan(invalidEscape, "escape.gti");
  expect(
      lexer.errors().size() == 1 &&
          lexer.errors().front().code == "GTI-L0005" &&
          lexer.errors().front().primary.start == invalidEscape.find("\\q") &&
          lexer.errors().front().primary.end == invalidEscape.find("\\q") + 2 &&
          lexer.errors().front().primary.line == 2,
      "lexical diagnostics should identify the exact invalid escape span");

  const std::string missingSemicolon = "int first = 1\nint second = 2;\n";
  lang::Parser parser(lexer.scan(missingSemicolon, "parse.gti"));
  parser.parse();
  expect(parser.errors().size() == 1 &&
             parser.errors().front().code == "GTI-P0001" &&
             parser.errors().front().fixes.size() == 1 &&
             parser.errors().front().fixes.front().replacement == ";" &&
             parser.errors().front().fixes.front().span.start ==
                 missingSemicolon.find("int second"),
         "missing punctuation should carry an insertion fix-it");

  auto semanticTokens = lexer.scan(R"(
int duplicate = 1;
int duplicate = 2;
int main() {
  int fixed = 1;
  fixed = 2;
  int value = "text";
  return 0;
}
)",
                                   "semantic.gti");
  lang::Parser semanticParser(std::move(semanticTokens));
  lang::Program program = semanticParser.parse();
  lang::SemanticVisitor semantic;
  expect(!semantic.check(program),
         "rich semantic diagnostic source should fail");

  const lang::Diagnostic *duplicate = nullptr;
  const lang::Diagnostic *immutable = nullptr;
  const lang::Diagnostic *mismatch = nullptr;
  for (const lang::Diagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == "GTI-S2006") {
      duplicate = &diagnostic;
    } else if (diagnostic.code == "GTI-S2002") {
      immutable = &diagnostic;
    } else if (diagnostic.code == "GTI-S2003") {
      mismatch = &diagnostic;
    }
  }
  expect(duplicate != nullptr && duplicate->related.size() == 1,
         "duplicate declarations should reference the original declaration");
  expect(immutable != nullptr && immutable->related.size() == 1 &&
             !immutable->hints.empty(),
         "immutability diagnostics should explain the declaration and remedy");
  expect(mismatch != nullptr &&
             mismatch->message.find("int32_t") != std::string::npos &&
             mismatch->message.find("std::string_view") != std::string::npos,
         "type mismatches should name expected and actual GTI types");
}

void testSemanticMoveOriginBounds() {
  lang::SemanticMoveOrigins witnesses;
  for (std::size_t index = 0; index < 32; ++index) {
    witnesses = lang::SemanticMoveOrigins::merge(
        witnesses, {{"bounded.gti", index, index + 1, 1}});
    expect(witnesses.sites.size() <= 4,
           "producer storage is bounded before diagnostic rendering");
  }
  const auto omitted = witnesses.omittedLowerBound;
  for (int iteration = 0; iteration < 100; ++iteration)
    witnesses = lang::SemanticMoveOrigins::merge(witnesses, witnesses);
  expect(omitted > 0 && witnesses.omittedLowerBound == omitted,
         "repeated loop joins cannot invent additional omitted transfer sites");
  witnesses.clear();
  expect(witnesses.sites.empty() && witnesses.omittedLowerBound == 0,
         "restoration clears both visible and omitted move evidence");
}

void testAnalysisErrorLimit() {
  std::string source = "void broken() {\n";
  for (int index = 0; index < 230; ++index)
    source += "missing_" + std::to_string(index) + "();\n";
  source += "}\n";
  for (std::size_t limit : {20U, 1U, 7U, 0U}) {
    lang::FrontendOptions options;
    options.limits.errorLimit = limit;
    const auto result =
        lang::Frontend(options).analyze("error-limit.gti", source);
    const std::size_t expected = limit == 0 ? 230 : limit + 1;
    expect(lang::errorDiagnosticCount(result.diagnostics) == expected,
           "frontend error count respects the analysis budget");
    expect(!result.canGenerateCode(), "limited analysis cannot generate code");
    if (limit != 0) {
      expect(result.diagnostics.back().code == "GTI-D0002" &&
                 result.diagnostics.back().primary.source.find(
                     "error-limit.gti") != std::string::npos,
             "terminal error limit diagnostic retains its source location");
    }
  }
  // This inspects the analyzer's raw records before the frontend presentation
  // cap. It proves that later statements were not analyzed.
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "error-limit.gti"));
  const auto program = parser.parse();
  lang::SemanticVisitor semantic;
  expect(!semantic.check(program) &&
             lang::errorDiagnosticCount(semantic.errors()) == 20,
         "semantic analysis actually stops after twenty failing statements");
  lang::Parser goodParser(lexer.scan("void valid() {}", "valid.gti"));
  expect(semantic.check(goodParser.parse()),
         "limited semantic visitors reset on reuse");
  std::string parseErrors;
  for (int index = 0; index < 230; ++index)
    parseErrors += "int bad_" + std::to_string(index) + " = ;\n";
  for (const std::string &invalid : {std::string(230, '`'), parseErrors}) {
    const auto result = lang::Frontend().analyze("syntax-limit.gti", invalid);
    expect(lang::errorDiagnosticCount(result.diagnostics) == 21 &&
               result.diagnostics.back().code == "GTI-D0002",
           "lexical and parser recovery stop at the same default limit");
  }
  const auto prefix = lang::Frontend().analyze(
      "prefix-limit.gti", "void completed() {}\n" + parseErrors);
  expect(
      prefix.program.declarations().size() == 1,
      "parser error limits retain completed declarations for editor queries");
  auto cache = std::make_shared<lang::FrontendCache>();
  std::string imports;
  for (int index = 0; index < 40; ++index)
    imports += "#include <unused>\n";
  for (const auto &input : {parseErrors, std::string(40, '`'), imports}) {
    lang::FrontendOptions limited;
    limited.cache = cache;
    limited.limits.errorLimit = 3;
    const auto first =
        lang::Frontend(limited).analyze("cache-limit.gti", input);
    expect(first.diagnostics.back().code == "GTI-D0002",
           "cache seed is limited");
    limited.limits.errorLimit = 0;
    const auto all = lang::Frontend(limited).analyze("cache-limit.gti", input);
    expect(
        lang::errorDiagnosticCount(all.diagnostics) >= 40 &&
            all.diagnostics.back().code != "GTI-D0002",
        "an unlimited request never reuses truncated source or parse caches");
  }
  std::string probes = "struct Sample {}; int main() {\n";
  for (int index = 0; index < 40; ++index)
    probes += "static_assert(!(requires(Sample& x) { { x.absent() } -> Sample; "
              "}));\n";
  probes += "return 0; }\n";
  lang::FrontendOptions probeOptions;
  probeOptions.limits.errorLimit = 1;
  expect(lang::Frontend(probeOptions)
             .analyze("probes.gti", probes)
             .canGenerateCode(),
         "discarded speculative errors do not consume the analysis budget");
  std::vector<lang::Diagnostic> warnings(40);
  for (auto &diagnostic : warnings)
    diagnostic.severity = lang::DiagnosticSeverity::Warning;
  expect(!lang::applyErrorLimit(warnings, 20) && warnings.size() == 40,
         "warnings do not consume the error budget");
}

void testLibraryReportMerge() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
  const auto count = [](const lang::FrontendResult &result,
                        std::string_view code) {
    return std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                         [&](const lang::Diagnostic &diagnostic) {
                           return diagnostic.code == code;
                         });
  };
  // The trusted fixture calls the mapper twice; both calls reject its
  // parameter type for the one request.
  const auto mapper = lang::Frontend().analyze(
      "duplicate-library-instance.gti", R"(
int main() {
  auto flag = [](bool value) -> int32_t { return 1; };
  return fixture::twice(flag);
}
)",
      {root / "stdlib/prelude.gti",
       root / "tests/fixtures/library_report_merge_prelude.gti"},
      {}, {root / "stdlib"});
  const auto rejected =
      std::find_if(mapper.diagnostics.begin(), mapper.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2003";
                   });
  const auto libraryNote = [](const lang::RelatedDiagnostic &related) {
    return related.message == "Reported inside this library instance.";
  };
  const auto libraryNotes =
      rejected == mapper.diagnostics.end()
          ? 0
          : std::count_if(rejected->related.begin(), rejected->related.end(),
                          libraryNote);
  if (mapper.diagnostics.size() != 1 || libraryNotes != 2) {
    for (const auto &diagnostic : mapper.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      for (const auto &related : diagnostic.related)
        std::cerr << "  note: " << related.message << '\n';
    }
  }
  expect(!mapper.canGenerateCode() && mapper.diagnostics.size() == 1 &&
             count(mapper, "GTI-S2003") == 1 && libraryNotes == 2 &&
             std::is_partitioned(rejected->related.begin(),
                                 rejected->related.end(), libraryNote),
         "one library request reports one diagnostic with each internal site "
         "ahead of its request notes");
}

// Retained member-constraint evidence is bounded and shared: long concept
// application chains keep their outer and innermost entries, nested member
// failures share one immutable leaf, and a sampled disjunction counts the
// branches it omits. (Restored from the former compiler_declarations_tests.cpp
// testConstraintFailureProvenance; its GTI sources became tests/ui cases.)
void testConstraintEvidenceRetention() {
  lang::SemanticConstraintFailure evidence;
  evidence.argument = lang::SemanticType::Bool;
  for (int index = 0; index < 24; ++index)
    evidence.context.push_back({"Contract" + std::to_string(index), {}, {}});
  auto retained =
      lang::SemanticMemberConstraintCause::retain(std::move(evidence));
  const auto leaf = retained.leaf;
  expect(leaf->context.size() == 8 && leaf->context.capacity() == 8 &&
             retained.omittedApplications == 16 &&
             leaf->context.front().name == "Contract0" &&
             leaf->context.back().name == "Contract23",
         "bounded member evidence preserves outer and innermost applications");
  for (int depth = 0; depth < 64; ++depth) {
    lang::SemanticConstraintFailure wrapper;
    wrapper.memberFailure.emplace();
    wrapper.memberFailure->constraintCause = std::move(retained);
    retained = lang::SemanticMemberConstraintCause::retain(std::move(wrapper));
  }
  expect(retained.leaf == leaf && retained.omittedMemberSteps == 64 &&
             retained.omittedApplications == 16 && !leaf->memberFailure,
         "nested member evidence shares one immutable leaf rather than a "
         "recursive tree");
  lang::SemanticConstraintFailure branchEvidence;
  branchEvidence.alternatives.resize(2);
  branchEvidence.alternatives.front().memberFailure.emplace();
  branchEvidence.alternatives.front().memberFailure->constraintCause = retained;
  const auto representative =
      lang::SemanticMemberConstraintCause::retain(std::move(branchEvidence));
  expect(
      representative.leaf == leaf && representative.omittedAlternatives == 1,
      "sampling a failed disjunction explicitly accounts for other branches");
}

} // namespace

GTI_UNIT_TEST(diagnostic_foundation) {
  testDiagnosticFoundation();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(semantic_move_origin_bounds) {
  testSemanticMoveOriginBounds();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(analysis_error_limit) {
  testAnalysisErrorLimit();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(library_report_merge) {
  testLibraryReportMerge();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(constraint_evidence_retention) {
  testConstraintEvidenceRetention();
  return failures == 0 ? 0 : 1;
}
