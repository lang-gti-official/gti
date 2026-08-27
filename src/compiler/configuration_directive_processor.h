#pragma once

#include "gti/diagnostic.h"
#include "gti/source_graph.h"
#include "gti/target.h"
#include "gti/token.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace lang {

struct ConfigurationDirectiveStep {
  // True only for #define/#undef lines, which do not enter parser tokens.
  bool consumed = false;
  std::size_t lastConsumedIndex = 0;
  bool conditionalBoundary = false;
};

// Incremental by design: SourceLoader invokes this state machine in lexical
// order and may recursively load an include between steps. This preserves the
// existing compilation-wide #define/#undef ordering without giving directive
// processing include resolution or graph-recursion authority.
class ConfigurationDirectiveProcessor final {
public:
  ConfigurationDirectiveProcessor(
      std::vector<Token> &tokens, const TargetInfo &target,
      std::unordered_set<std::string> &activeFlags,
      std::unordered_set<std::string> &declaredFlags,
      std::vector<Diagnostic> &diagnostics);

  [[nodiscard]] ConfigurationDirectiveStep process(std::size_t index);

  [[nodiscard]] bool currentPathActive() const;

  [[nodiscard]] int conditionalDepth() const;

  void retainInactive(const Token &token);

  void finish();

  [[nodiscard]] std::vector<SourceSpan> takeInactiveSpans();

  [[nodiscard]] std::vector<ConfigurationToken> takeConfigurationTokens();

private:
  struct ConditionalFrame {
    Token opener;
    bool parentActive = true;
    bool branchTaken = false;
    bool active = false;
    bool sawElse = false;
  };

  void report(const Token &token, std::string message,
              std::string code = "GTI-I0000");

  void reportFlagName(const Token &directive);

  std::vector<Token> &tokens;
  const TargetInfo &target;
  std::unordered_set<std::string> &activeFlags;
  std::unordered_set<std::string> &declaredFlags;
  std::vector<Diagnostic> &diagnostics;
  std::vector<ConditionalFrame> conditionals;
  std::vector<SourceSpan> inactiveSpans;
  std::vector<ConfigurationToken> configurationTokens;
  bool finished = false;
};

} // namespace lang
