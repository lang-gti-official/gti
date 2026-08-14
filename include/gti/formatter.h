#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace lang {

enum class ReferenceAlignment {
  Left,
  Right,
  Middle,
};

enum class BraceBreakingStyle {
  Attach,
  Allman,
};

enum class RequiresClausePosition {
  // Start the clause on its own line, indented one level past the declaration.
  OwnLine,
  // Keep the clause on the declaration line.
  SingleLine,
};

enum class SpaceBeforeParensStyle {
  Never,
  ControlStatements,
  Always,
};

struct FormatOptions {
  std::size_t indentWidth = 2;
  bool insertSpaces = true;
  ReferenceAlignment referenceAlignment = ReferenceAlignment::Middle;
  BraceBreakingStyle breakBeforeBraces = BraceBreakingStyle::Attach;
  RequiresClausePosition requiresClausePosition =
      RequiresClausePosition::OwnLine;
  SpaceBeforeParensStyle spaceBeforeParens =
      SpaceBeforeParensStyle::ControlStatements;
  bool indentCaseLabels = false;
  std::optional<int> accessModifierOffset;
  std::size_t maxEmptyLinesToKeep = 1;
  std::size_t spacesBeforeTrailingComments = 1;
  bool spaceBeforeAssignmentOperators = true;
  bool disableFormat = false;
};

class Formatter {
public:
  explicit Formatter(FormatOptions options = {});

  std::string format(std::string_view source) const;

private:
  FormatOptions options;
};

} // namespace lang
