#include "../src/compiler/configuration_directive_processor.h"
#include "../src/compiler/loaded_lexical_unit.h"

#include "gti/lexer.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testLoadedLexicalUnitCacheBoundary() {
  auto cache = std::make_shared<lang::FrontendCache>();
  lang::FrontendWorkMetrics metrics;
  const std::string source = "int answer = 42;\n";
  const lang::LexicalUnitRequest request{
      .path = "/virtual/source-unit.gti",
      .canonicalPath = "/virtual/source-unit.gti",
      .exactSource = source,
      .sourceGeneration = 7,
      .cacheContext = "source-unit-service-test-v1",
  };

  const lang::LoadedLexicalUnit first =
      lang::LoadedLexicalUnitLoader(&metrics, cache).load(request);
  expect(!first.cacheHit && !first.hadError && first.diagnostics.empty() &&
             first.source == source && first.sourceDigest != 0 &&
             first.tokens != nullptr && !first.tokens->empty(),
         "a first exact-source request should own bytes, digest, tokens, and "
         "lexical status without a cache hit");
  expect(
      metrics.loadedUnits == std::vector<std::string>{request.canonicalPath} &&
          metrics.lexedUnits ==
              std::vector<std::string>{request.canonicalPath} &&
          metrics.lexCacheHits.empty() && metrics.sourceBytes == source.size(),
      "the lexical service should own cold-load work accounting");

  const lang::LoadedLexicalUnit second =
      lang::LoadedLexicalUnitLoader(&metrics, cache).load(request);
  const bool sameTokens =
      second.tokens != nullptr &&
      second.tokens->size() == first.tokens->size() &&
      std::equal(
          second.tokens->begin(), second.tokens->end(), first.tokens->begin(),
          [](const lang::Token &left, const lang::Token &right) {
            return left.kind == right.kind && left.position == right.position &&
                   left.line == right.line && left.lexeme == right.lexeme;
          });
  expect(second.cacheHit && !second.hadError && second.source == first.source &&
             second.sourceDigest == first.sourceDigest && sameTokens,
         "an identical request should recover the exact cached lexical row");
  expect(metrics.lexCacheHits ==
                 std::vector<std::string>{request.canonicalPath} &&
             metrics.sourceBytes == source.size() * 2,
         "the lexical service should account cache hits and observed bytes");

  const std::string changed = "int answer = 43;\n";
  lang::LexicalUnitRequest changedRequest = request;
  changedRequest.exactSource = changed;
  const lang::LoadedLexicalUnit changedUnit =
      lang::LoadedLexicalUnitLoader(&metrics, cache).load(changedRequest);
  expect(!changedUnit.cacheHit &&
             changedUnit.sourceDigest != first.sourceDigest,
         "exact overlay bytes should prevent a digest-only cache collision");
}

void testIncrementalDirectiveState() {
  lang::Lexer lexer;
  std::vector<lang::Token> tokens = lexer.scan(R"(
#define FEATURE
#if defined(FEATURE)
import "dependency.gti";
#else
int inactive = 1;
#endif
#undef FEATURE
)",
                                               "directives.gti");
  expect(!lexer.hadError(), "the directive service fixture should lex");

  std::unordered_set<std::string> activeFlags;
  std::unordered_set<std::string> declaredFlags;
  std::vector<lang::Diagnostic> diagnostics;
  lang::ConfigurationDirectiveProcessor processor(
      tokens, lang::TargetInfo::host(), activeFlags, declaredFlags,
      diagnostics);
  bool includeSawFeature = false;
  int includeDepth = 0;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const lang::ConfigurationDirectiveStep step = processor.process(index);
    if (step.consumed) {
      index = step.lastConsumedIndex;
      continue;
    }
    if (tokens[index].kind == lang::TokenKind::IMPORT) {
      includeSawFeature = activeFlags.contains("FEATURE");
      includeDepth = processor.conditionalDepth();
    }
    if (!processor.currentPathActive() && !step.conditionalBoundary) {
      processor.retainInactive(tokens[index]);
    }
  }
  processor.finish();

  const std::vector<lang::SourceSpan> inactive = processor.takeInactiveSpans();
  const std::vector<lang::ConfigurationToken> configuration =
      processor.takeConfigurationTokens();
  expect(includeSawFeature && includeDepth == 1,
         "incremental processing should expose pre-import flag state and "
         "conditional depth before recursion");
  expect(!activeFlags.contains("FEATURE") && declaredFlags.contains("FEATURE"),
         "post-import directives should run only after the caller resumes");
  expect(diagnostics.empty() && !inactive.empty() && configuration.size() >= 3,
         "directive processing should own inactive spans and configuration "
         "token publication without changing valid diagnostics");
}

} // namespace

int main() {
  testLoadedLexicalUnitCacheBoundary();
  testIncrementalDirectiveState();
  if (failures != 0) {
    std::cerr << failures << " source-unit service test(s) failed\n";
    return 1;
  }
  std::cout << "source-unit service tests passed\n";
  return 0;
}
