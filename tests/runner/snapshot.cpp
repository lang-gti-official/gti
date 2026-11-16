#include "snapshot.h"

#include "gti/lowered_program.h"
#include "gti/mir.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <regex>

namespace gti_test {
namespace {

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    result.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

// Keys whose values are program-wide numeric identities. Their values depend
// on how many declarations, symbols or instances precede the test's own, so
// they are renumbered by first appearance within the snapshot.
bool identityKey(const std::string &key) {
  static const std::set<std::string> keys = {"class",
                                             "concept",
                                             "constructor",
                                             "declaration",
                                             "destructor",
                                             "dispatch-owner",
                                             "enum",
                                             "enum-owner",
                                             "function",
                                             "instance",
                                             "lambda",
                                             "owner",
                                             "owner-class",
                                             "root-owner",
                                             "source-unit",
                                             "symbol",
                                             "target-owner",
                                             "unit",
                                             "call-site",
                                             "hir",
                                             "hir-value",
                                             "hir-statement",
                                             "hir-full-expression",
                                             "field",
                                             "root"};
  return keys.contains(key);
}

bool defaultValue(const std::string &value) {
  return value == "0" || value == "-" || value == "[]" || value == "none";
}

bool keyCharacter(char c) {
  return std::islower(static_cast<unsigned char>(c)) ||
         std::isdigit(static_cast<unsigned char>(c)) || c == '-';
}

// Rewrites `key=value` fields in one line: identities renumbered, default
// fields dropped. Fields are separated by spaces at the top level and by `;`
// inside parenthesised groups.
std::string
normalizeLine(const std::string &line,
              std::map<std::string, std::map<std::string, int>> &numbers) {
  std::string out;
  std::size_t index = 0;
  while (index < line.size()) {
    const char c = line[index];
    const bool fieldStart =
        std::islower(static_cast<unsigned char>(c)) &&
        (index == 0 || line[index - 1] == ' ' || line[index - 1] == ';' ||
         line[index - 1] == '(' || line[index - 1] == ',');
    if (!fieldStart) {
      out += c;
      ++index;
      continue;
    }
    std::size_t keyEnd = index;
    while (keyEnd < line.size() && keyCharacter(line[keyEnd]))
      ++keyEnd;
    if (keyEnd >= line.size() || line[keyEnd] != '=') {
      out.append(line, index, keyEnd - index);
      index = keyEnd;
      continue;
    }
    const std::string key = line.substr(index, keyEnd - index);
    std::size_t valueEnd = keyEnd + 1;
    int depth = 0;
    while (valueEnd < line.size()) {
      const char v = line[valueEnd];
      if (v == '(' || v == '[')
        ++depth;
      else if ((v == ')' || v == ']') && depth > 0)
        --depth;
      else if (depth == 0 && (v == ' ' || v == ';' || v == ')' || v == ','))
        break;
      ++valueEnd;
    }
    const std::string value = line.substr(keyEnd + 1, valueEnd - keyEnd - 1);
    if (defaultValue(value)) {
      // Drop the field and the separator before it.
      if (!out.empty() &&
          (out.back() == ' ' || out.back() == ';' || out.back() == ','))
        out.pop_back();
      index = valueEnd;
      continue;
    }
    // A body's `domain` names its position among all bodies.
    if ((identityKey(key) && std::all_of(value.begin(), value.end(),
                                         [](char d) {
                                           return std::isdigit(
                                               static_cast<unsigned char>(d));
                                         })) ||
        key == "domain") {
      std::map<std::string, int> &table = numbers[key];
      const auto found =
          table.try_emplace(value, static_cast<int>(table.size()) + 1);
      out += key + "=#" + std::to_string(found.first->second);
    } else {
      // Recurse into groups such as type(...) so nested fields are handled.
      out += key + "=" + normalizeLine(value, numbers);
    }
    index = valueEnd;
  }
  // Source-unit references inside spans, such as `unit3:4@115..116`.
  static const std::regex unit(R"(\bunit(\d+):)");
  std::string renumbered;
  auto cursor = out.cbegin();
  for (std::sregex_iterator it(out.begin(), out.end(), unit), end; it != end;
       ++it) {
    renumbered.append(cursor, out.cbegin() + it->position(0));
    std::map<std::string, int> &table = numbers["source-unit"];
    const auto found =
        table.try_emplace((*it)[1].str(), static_cast<int>(table.size()) + 1);
    renumbered += "unit#" + std::to_string(found.first->second) + ":";
    cursor = out.cbegin() + it->position(0) + it->length(0);
  }
  renumbered.append(cursor, out.cend());
  out = std::move(renumbered);
  // A dropped first field leaves `(;` behind.
  for (const char *gap : {"(;", "(,"})
    for (std::size_t at; (at = out.find(gap)) != std::string::npos;)
      out.erase(at + 1, 1);
  return out;
}

std::string escapeRegex(const std::string &text) {
  std::string out;
  for (const char c : text) {
    if (std::string_view("\\^$.|?*+()[]{}").find(c) != std::string_view::npos)
      out += '\\';
    out += c;
  }
  return out;
}

// A pattern: literal text (runs of blanks match any blanks) with `{{regex}}`
// segments.
std::optional<std::regex> compilePattern(const std::string &pattern,
                                         std::string &error) {
  std::string source;
  std::size_t index = 0;
  while (index < pattern.size()) {
    const std::size_t open = pattern.find("{{", index);
    std::string literal = pattern.substr(
        index, open == std::string::npos ? std::string::npos : open - index);
    std::string escaped;
    for (std::size_t at = 0; at < literal.size();) {
      if (literal[at] == ' ' || literal[at] == '\t') {
        escaped += "[ \\t]+";
        while (at < literal.size() &&
               (literal[at] == ' ' || literal[at] == '\t'))
          ++at;
      } else {
        escaped += escapeRegex(std::string(1, literal[at]));
        ++at;
      }
    }
    source += escaped;
    if (open == std::string::npos)
      break;
    const std::size_t close = pattern.find("}}", open + 2);
    if (close == std::string::npos) {
      error = "unterminated {{ in pattern";
      return std::nullopt;
    }
    source += "(?:" + pattern.substr(open + 2, close - open - 2) + ")";
    index = close + 2;
  }
  try {
    return std::regex(source);
  } catch (const std::regex_error &exception) {
    error = std::string("invalid regex: ") + exception.what();
    return std::nullopt;
  }
}

std::string excerpt(const std::vector<std::string> &lines, std::size_t from) {
  std::string out;
  for (std::size_t line = from; line < lines.size() && line < from + 3;
       ++line) {
    std::string text = lines[line];
    if (text.size() > 160)
      text = text.substr(0, 160) + "...";
    out += "\n      " + std::to_string(line + 1) + ": " + text;
  }
  return out.empty() ? "\n      (end of output)" : out;
}

} // namespace

std::string normalizeMir(const std::string &text) {
  std::map<std::string, std::map<std::string, int>> numbers;
  std::string out;
  for (const std::string &line : splitLines(text)) {
    std::string normalized = normalizeLine(line, numbers);
    while (!normalized.empty() && normalized.back() == ' ')
      normalized.pop_back();
    out += normalized + "\n";
  }
  return out;
}

std::string annotateLlvmIr(const std::string &ir,
                           const std::map<std::string, std::string> &sources,
                           const std::string &directory) {
  static const std::regex metadata(
      R"re(^!(\d+) = !\{!"([^"]*)", !"(\d+)", !"(\d+)"\})re");
  static const std::regex reference(R"re(!gti\.source !(\d+))re");
  std::map<std::string, std::string> names;
  const std::vector<std::string> lines = splitLines(ir);
  for (const std::string &line : lines) {
    std::smatch match;
    if (!std::regex_search(line, match, metadata))
      continue;
    const auto source = sources.find(match[2].str());
    if (source == sources.end())
      continue;
    const std::size_t begin = std::stoul(match[3].str());
    const std::size_t end = std::stoul(match[4].str());
    if (begin < end && end <= source->second.size())
      names[match[1].str()] = source->second.substr(begin, end - begin);
  }
  std::string out;
  for (std::string line : lines) {
    std::smatch match;
    if (line.rfind("define ", 0) == 0 &&
        std::regex_search(line, match, reference)) {
      const auto name = names.find(match[1].str());
      if (name != names.end())
        line += " ; " + name->second;
    }
    if (!directory.empty())
      for (std::size_t at; (at = line.find(directory)) != std::string::npos;)
        line.erase(at, directory.size());
    out += line + "\n";
  }
  return out;
}

std::string selectMir(const lang::LoweredProgram &program,
                      const std::vector<std::string> &names,
                      const std::set<std::string> &testSources,
                      std::string &error) {
  const lang::MirProgram &mir = program.mir();
  const lang::MirPrinter printer;
  std::string text;
  std::set<std::string> found;
  for (const lang::MirFunctionInstance &instance : mir.functionInstances()) {
    const lang::LoweredFunctionDeclaration *declaration =
        program.findFunctionDeclaration(instance.declaration);
    if (declaration == nullptr)
      continue;
    const std::string &qualified = declaration->qualifiedName;
    bool selected = false;
    if (names.empty()) {
      const lang::LoweredFunctionInstance *lowered =
          program.findFunctionInstance(instance.id);
      selected =
          lowered != nullptr && testSources.contains(lowered->source.source);
    } else {
      for (const std::string &name : names) {
        if (qualified == name ||
            (qualified.size() > name.size() + 2 && qualified.ends_with(name) &&
             qualified.compare(qualified.size() - name.size() - 2, 2, "::") ==
                 0)) {
          selected = true;
          found.insert(name);
        }
      }
    }
    if (selected)
      text += "function " + qualified + "\n" + printer.print(instance.body);
  }
  for (const std::string &name : names)
    if (!found.contains(name))
      error += "mir-function: no function '" + name + "' has a MIR body\n";
  if (names.empty() && text.empty())
    error += "the test defines no function with a MIR body\n";
  return normalizeMir(text);
}

std::vector<std::string> matchChecks(const std::vector<CheckPattern> &checks,
                                     const std::string &output) {
  const std::vector<std::string> lines = splitLines(output);
  std::vector<std::string> problems;
  // Position after the previous positive match.
  std::size_t line = 0;
  std::size_t column = 0;
  bool matchedAny = false;
  std::size_t pendingNot = 0; // first CHECK-NOT since the last match
  const auto name = [](const CheckPattern &check) {
    return check.kind == CheckPattern::Kind::Check  ? "CHECK"
           : check.kind == CheckPattern::Kind::Next ? "CHECK-NEXT"
                                                    : "CHECK-NOT";
  };
  // The CHECK-NOT patterns in [first, last) must not match between the
  // previous match and (endLine, endColumn).
  const auto checkNots = [&](std::size_t first, std::size_t last,
                             std::size_t fromLine, std::size_t fromColumn,
                             std::size_t endLine, std::size_t endColumn) {
    for (std::size_t index = first; index < last; ++index) {
      std::string error;
      const std::optional<std::regex> regex =
          compilePattern(checks[index].pattern, error);
      if (!regex) {
        problems.push_back("CHECK-NOT at line " +
                           std::to_string(checks[index].writtenLine) + ": " +
                           error);
        continue;
      }
      for (std::size_t at = fromLine; at < lines.size() && at <= endLine;
           ++at) {
        const std::size_t begin = at == fromLine ? fromColumn : 0;
        const std::size_t end = at == endLine ? endColumn : lines[at].size();
        if (begin >= end && !(begin == 0 && end == 0 && lines[at].empty()))
          continue;
        const std::string slice =
            lines[at].substr(begin, end > begin ? end - begin : 0);
        if (std::regex_search(slice, *regex)) {
          problems.push_back("CHECK-NOT at line " +
                             std::to_string(checks[index].writtenLine) + ": `" +
                             checks[index].pattern + "` matched output line " +
                             std::to_string(at + 1) + excerpt(lines, at));
          break;
        }
      }
    }
  };
  for (std::size_t index = 0; index < checks.size(); ++index) {
    const CheckPattern &check = checks[index];
    if (check.kind == CheckPattern::Kind::Not)
      continue;
    std::string error;
    const std::optional<std::regex> regex =
        compilePattern(check.pattern, error);
    const std::string where = std::string(name(check)) + " at line " +
                              std::to_string(check.writtenLine);
    if (!regex) {
      problems.push_back(where + ": " + error);
      return problems;
    }
    std::optional<std::pair<std::size_t, std::size_t>> match;
    std::size_t matchLine = 0;
    if (check.kind == CheckPattern::Kind::Next) {
      matchLine = line + 1;
      std::smatch result;
      if (matchedAny && matchLine < lines.size() &&
          std::regex_search(lines[matchLine], result, *regex))
        match = {
            static_cast<std::size_t>(result.position(0)),
            static_cast<std::size_t>(result.position(0) + result.length(0))};
    } else {
      for (std::size_t at = line; at < lines.size(); ++at) {
        const std::size_t begin = at == line ? column : 0;
        if (begin > lines[at].size())
          continue;
        const std::string slice = lines[at].substr(begin);
        std::smatch result;
        if (std::regex_search(slice, result, *regex)) {
          matchLine = at;
          match = {begin + static_cast<std::size_t>(result.position(0)),
                   begin + static_cast<std::size_t>(result.position(0) +
                                                    result.length(0))};
          break;
        }
      }
    }
    if (!match) {
      problems.push_back(
          where + ": `" + check.pattern + "` not found " +
          (check.kind == CheckPattern::Kind::Next
               ? std::string("on the line after the previous match")
               : "after output line " + std::to_string(line + 1)) +
          excerpt(lines,
                  check.kind == CheckPattern::Kind::Next ? line + 1 : line));
      return problems;
    }
    checkNots(pendingNot, index, line, column, matchLine, match->first);
    line = matchLine;
    column = match->second;
    matchedAny = true;
    pendingNot = index + 1;
  }
  checkNots(pendingNot, checks.size(), line, column,
            lines.empty() ? 0 : lines.size() - 1,
            lines.empty() ? 0 : lines.back().size());
  return problems;
}

std::string lineDiff(const std::string &expected, const std::string &actual,
                     std::size_t limit) {
  const std::vector<std::string> a = splitLines(expected);
  const std::vector<std::string> b = splitLines(actual);
  std::vector<std::string> out;
  if (a.size() * b.size() > 4'000'000) {
    out.push_back("(output too large to diff; " + std::to_string(a.size()) +
                  " expected lines, " + std::to_string(b.size()) +
                  " actual lines)");
  } else {
    std::vector<std::vector<int>> table(a.size() + 1,
                                        std::vector<int>(b.size() + 1, 0));
    for (std::size_t i = a.size(); i-- > 0;)
      for (std::size_t j = b.size(); j-- > 0;)
        table[i][j] = a[i] == b[j] ? table[i + 1][j + 1] + 1
                                   : std::max(table[i + 1][j], table[i][j + 1]);
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() || j < b.size()) {
      if (i < a.size() && j < b.size() && a[i] == b[j]) {
        out.push_back(" " + a[i]);
        ++i;
        ++j;
      } else if (i < a.size() &&
                 (j == b.size() || table[i + 1][j] >= table[i][j + 1])) {
        out.push_back("-" + a[i++]);
      } else {
        out.push_back("+" + b[j++]);
      }
    }
  }
  std::string result;
  for (std::size_t index = 0; index < out.size() && index < limit; ++index)
    result += "    " + out[index] + "\n";
  if (out.size() > limit)
    result +=
        "    ... (" + std::to_string(out.size() - limit) + " more lines)\n";
  return result;
}

std::string compareSnapshot(const TestFile &test, Snapshot snapshot,
                            const std::string &actual, bool bless,
                            bool &blessed) {
  const std::filesystem::path path = snapshotPath(test, snapshot);
  const std::optional<std::string> expected = readFile(path);
  if (expected && *expected == actual)
    return "";
  if (bless) {
    writeFile(path, actual);
    blessed = true;
    return "";
  }
  const std::string name = path.filename().string();
  if (!expected)
    return std::string("snapshot ") + snapshotName(snapshot) + ": no " + name +
           " yet; bless to create it\n";
  return std::string("snapshot ") + snapshotName(snapshot) + " differs from " +
         name + " (-expected +actual):\n" + lineDiff(*expected, actual, 40);
}

} // namespace gti_test
