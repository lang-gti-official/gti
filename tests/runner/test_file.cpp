#include "test_file.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace gti_test {
namespace {

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return std::string(text.substr(begin, end - begin));
}

std::string rtrim(std::string_view text) {
  std::size_t end = text.size();
  while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return std::string(text.substr(0, end));
}

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

std::vector<std::string> splitWords(const std::string &text) {
  std::istringstream stream(text);
  std::vector<std::string> words;
  for (std::string word; stream >> word;)
    words.push_back(word);
  return words;
}

bool validCode(const std::string &code) {
  // GTI-<letters><digits>, such as GTI-S2042 or GTI-R0007.
  if (code.size() < 6 || code.rfind("GTI-", 0) != 0)
    return false;
  std::size_t index = 4;
  while (index < code.size() &&
         std::isupper(static_cast<unsigned char>(code[index])))
    ++index;
  if (index == 4 || index == code.size())
    return false;
  for (; index < code.size(); ++index)
    if (!std::isdigit(static_cast<unsigned char>(code[index])))
      return false;
  return true;
}

// The code part of a line (before the first `//~`), or the whole line.
std::string codePart(const std::string &line) {
  const std::size_t marker = line.find("//~");
  return marker == std::string::npos ? line : line.substr(0, marker);
}

bool isSectionMarker(const std::string &line, std::string &name) {
  const std::string prefix = "// --- ";
  if (line.rfind(prefix, 0) != 0)
    return false;
  name = trim(line.substr(prefix.size()));
  return !name.empty() && name.find_first_of(" \t") == std::string::npos;
}

bool selfTestEnabled() {
  const char *value = std::getenv("GTI_TEST_SELF_TEST");
  return value != nullptr && std::string(value) == "1";
}

bool underSlowDirectory(const std::filesystem::path &path) {
  for (const auto &part : path)
    if (part == "slow")
      return true;
  return false;
}

} // namespace

const char *modeName(Mode mode) {
  switch (mode) {
  case Mode::CheckPass:
    return "check-pass";
  case Mode::CheckFail:
    return "check-fail";
  case Mode::BuildPass:
    return "build-pass";
  case Mode::RunPass:
    return "run-pass";
  case Mode::RunFail:
    return "run-fail";
  }
  return "?";
}

bool modeRuns(Mode mode) {
  return mode == Mode::RunPass || mode == Mode::RunFail;
}

bool modeBuilds(Mode mode) { return mode == Mode::BuildPass || modeRuns(mode); }

const char *severityName(Severity severity) {
  return severity == Severity::Error ? "ERROR" : "WARN";
}

std::optional<std::string> readFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool writeFile(const std::filesystem::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

TestFile parseTestFile(const std::filesystem::path &path, std::string display) {
  TestFile test;
  test.path = path;
  test.display = std::move(display);
  const std::optional<std::string> contents = readFile(path);
  if (!contents) {
    test.errors.push_back("cannot read the test file");
    return test;
  }
  const std::vector<std::string> lines = splitLines(*contents);
  const auto error = [&](int line, const std::string &message) {
    test.errors.push_back("line " + std::to_string(line) + ": " + message);
  };

  // Sections.
  std::vector<int> markers;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    std::string name;
    if (isSectionMarker(lines[index], name)) {
      markers.push_back(static_cast<int>(index));
      Section section;
      section.name = name;
      section.firstLine = static_cast<int>(index) + 2;
      test.sections.push_back(section);
    }
  }
  if (test.sections.empty()) {
    Section whole;
    whole.name = "main.gti";
    whole.firstLine = 1;
    whole.text = *contents;
    test.sections.push_back(std::move(whole));
  } else {
    for (int index = 0; index < markers.front(); ++index) {
      const std::string text = trim(lines[static_cast<std::size_t>(index)]);
      if (!text.empty() && text.rfind("//", 0) != 0)
        error(index + 1, "code before the first `// --- name` section");
    }
    std::set<std::string> names;
    for (std::size_t section = 0; section < test.sections.size(); ++section) {
      Section &current = test.sections[section];
      if (!names.insert(current.name).second)
        error(markers[section] + 1, "duplicate section '" + current.name + "'");
      const std::filesystem::path name(current.name);
      if (name.is_absolute() ||
          std::any_of(name.begin(), name.end(),
                      [](const auto &part) { return part == ".."; }))
        error(markers[section] + 1,
              "section name must be a relative path inside the test");
      const std::size_t begin = static_cast<std::size_t>(markers[section]) + 1;
      const std::size_t end =
          section + 1 < markers.size()
              ? static_cast<std::size_t>(markers[section + 1])
              : lines.size();
      for (std::size_t line = begin; line < end; ++line)
        current.text += lines[line] + "\n";
    }
    if (test.sections.front().name.size() < 4 ||
        !test.sections.front().name.ends_with(".gti"))
      error(markers.front() + 1,
            "the first section (the entry) must be a .gti file");
  }
  const auto locate = [&](int fileLine, std::string &section,
                          int &sectionLine) {
    for (std::size_t index = 0; index < test.sections.size(); ++index) {
      const int first = test.sections[index].firstLine;
      const int last = index + 1 < markers.size()
                           ? markers[index + 1]
                           : static_cast<int>(lines.size());
      if (fileLine >= first && fileLine <= last) {
        section = test.sections[index].name;
        sectionLine = fileLine - first + 1;
        return true;
      }
    }
    return false;
  };

  // Directives. Run inputs and expectations are parsed into a RunSpec: the
  // shared one, or a revision's overrides (`//@[name] directive`).
  std::vector<std::string> modes;
  std::string imports;
  RunSpec shared;
  struct Override {
    RunSpec spec;
    std::set<std::string> given; // directive names the revision sets
    std::optional<Mode> mode;
    int line = 0;
  };
  std::map<std::string, Override> overrides;
  bool revisionsDeclared = false;
  const auto runDirective = [&](RunSpec &target, const std::string &name,
                                const std::string &value,
                                const std::string &trimmed, int lineNumber) {
    if (name == "stdout") {
      target.hasInlineStdout = true;
      // Exactly one separating space is dropped; the rest is literal.
      target.stdoutLines.push_back(
          !value.empty() && value.front() == ' ' ? value.substr(1) : value);
    } else if (name == "stderr-contains") {
      if (trimmed.empty())
        error(lineNumber, "stderr-contains needs text");
      target.stderrContains.push_back(trimmed);
    } else if (name == "exit-code") {
      char *end = nullptr;
      const long code = std::strtol(trimmed.c_str(), &end, 10);
      if (trimmed.empty() || *end != '\0' || code < 0 || code > 255)
        error(lineNumber, "exit-code must be an integer from 0 to 255");
      else if (target.exitCode)
        error(lineNumber, "duplicate exit-code");
      else
        target.exitCode = static_cast<int>(code);
    } else if (name == "args") {
      for (std::string &word : splitWords(trimmed))
        target.args.push_back(std::move(word));
    } else if (name == "stdin") {
      std::string line =
          !value.empty() && value.front() == ' ' ? value.substr(1) : value;
      target.stdinText = target.stdinText.value_or("") + line + "\n";
    } else {
      return false;
    }
    return true;
  };
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const int lineNumber = static_cast<int>(index) + 1;
    const std::string text = trim(lines[index]);
    if (text.rfind("import ", 0) == 0)
      imports += text + "\n";
    if (text.rfind("//@", 0) != 0)
      continue;
    std::string body = text.substr(3);
    std::string revision;
    bool revisionPrefixed = false;
    if (!body.empty() && body.front() == '[') {
      const std::size_t close = body.find(']');
      if (close == std::string::npos) {
        error(lineNumber, "revision directives are written `//@[name] ...`");
        continue;
      }
      revision = body.substr(1, close - 1);
      revisionPrefixed = true;
      body = body.substr(close + 1);
    }
    if (body.empty() || body.front() != ' ') {
      error(lineNumber,
            "directives are written `//@ name` or `//@ name: value`");
      continue;
    }
    body = body.substr(1);
    std::string name = body;
    std::string value;
    bool hasValue = false;
    if (const std::size_t colon = body.find(':'); colon != std::string::npos) {
      name = body.substr(0, colon);
      value = body.substr(colon + 1);
      hasValue = true;
    }
    name = trim(name);
    const std::string trimmed = trim(value);
    if (revisionPrefixed) {
      Override &entry = overrides[revision];
      if (entry.line == 0)
        entry.line = lineNumber;
      if (name == "run-pass" || name == "run-fail") {
        if (hasValue)
          error(lineNumber, "'" + name + "' takes no value");
        if (entry.mode)
          error(lineNumber,
                "more than one mode for revision '" + revision + "'");
        entry.mode = name == "run-pass" ? Mode::RunPass : Mode::RunFail;
      } else if (runDirective(entry.spec, name, value, trimmed, lineNumber)) {
        entry.given.insert(name);
      } else {
        error(lineNumber, "only run-pass, run-fail, stdout, stderr-contains, "
                          "exit-code, args and stdin vary by revision");
      }
      continue;
    }
    if (name == "check-pass" || name == "check-fail" || name == "build-pass" ||
        name == "run-pass" || name == "run-fail") {
      if (hasValue)
        error(lineNumber, "'" + name + "' takes no value");
      modes.push_back(name);
    } else if (runDirective(shared, name, value, trimmed, lineNumber)) {
    } else if (name == "revisions") {
      if (revisionsDeclared)
        error(lineNumber, "duplicate revisions");
      revisionsDeclared = true;
      std::set<std::string> seen;
      for (std::string &word : splitWords(trimmed)) {
        const bool valid = std::all_of(word.begin(), word.end(), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                 c == '-';
        });
        if (!valid)
          error(lineNumber, "revision names use letters, digits, '_' and '-'");
        else if (!seen.insert(word).second)
          error(lineNumber, "duplicate revision '" + word + "'");
        else
          test.revisions.push_back(std::move(word));
      }
      if (test.revisions.size() < 2)
        error(lineNumber, "revisions needs at least two names");
    } else if (name == "opt") {
      if (trimmed == "O3")
        test.optimize = true;
      else if (trimmed != "O0")
        error(lineNumber, "opt must be O0 or O3");
    } else if (name == "slow") {
      if (hasValue)
        error(lineNumber, "'slow' takes no value");
      test.slow = true;
    } else if (name == "reason") {
      if (trimmed.empty())
        error(lineNumber, "reason needs text");
      test.reason = trimmed;
    } else if (name == "only") {
      for (std::string &word : splitWords(trimmed)) {
        if (word != "linux" && word != "macos" && word != "arm64" &&
            word != "x86_64")
          error(lineNumber, "only: accepts linux, macos, arm64, x86_64");
        test.only.push_back(std::move(word));
      }
    } else if (name == "runner-fault") {
      if (!selfTestEnabled())
        error(lineNumber, "runner-fault is reserved for gti-test's own tests");
      test.runnerFault = trimmed;
    } else {
      error(lineNumber, "unknown directive '" + name + "'");
    }
  }
  for (const auto &[revision, entry] : overrides)
    if (std::find(test.revisions.begin(), test.revisions.end(), revision) ==
        test.revisions.end())
      error(entry.line, "revision '" + revision +
                            "' is not declared by `//@ revisions:`");
  test.stdoutLines = shared.stdoutLines;
  test.hasInlineStdout = shared.hasInlineStdout;
  test.stderrContains = shared.stderrContains;
  test.exitCode = shared.exitCode;
  test.args = shared.args;
  test.stdinText = shared.stdinText;

  // Annotations.
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const int lineNumber = static_cast<int>(index) + 1;
    const std::string &line = lines[index];
    std::size_t position = line.find("//~");
    while (position != std::string::npos) {
      const std::size_t next = line.find("//~", position + 3);
      std::string spec = line.substr(position + 3, next == std::string::npos
                                                       ? std::string::npos
                                                       : next - position - 3);
      position = next;
      Annotation annotation;
      annotation.writtenLine = lineNumber;
      int target = lineNumber;
      std::size_t cursor = 0;
      if (!spec.empty() && spec[0] == '?') {
        annotation.unlocated = true;
        cursor = 1;
      } else if (!spec.empty() && spec[0] == '|') {
        if (test.annotations.empty() || test.annotations.back().unlocated) {
          error(lineNumber, "`//~|` needs a located annotation before it");
          continue;
        }
        annotation.section = test.annotations.back().section;
        annotation.line = test.annotations.back().line;
        cursor = 1;
        target = 0;
      } else {
        while (cursor < spec.size() && spec[cursor] == '^') {
          --target;
          ++cursor;
        }
        while (cursor < spec.size() && spec[cursor] == 'v') {
          ++target;
          ++cursor;
        }
      }
      const std::vector<std::string> words = splitWords(spec.substr(cursor));
      if (words.size() < 2 ||
          (words[0] != "ERROR" && words[0] != "WARN" &&
           words[0] != "WARNING") ||
          !validCode(words[1])) {
        error(lineNumber,
              "annotations are written `//~ ERROR GTI-X0000 [message text]`");
        continue;
      }
      annotation.severity =
          words[0] == "ERROR" ? Severity::Error : Severity::Warning;
      annotation.code = words[1];
      const std::size_t codeAt = spec.find(words[1], cursor);
      annotation.substring = trim(spec.substr(codeAt + words[1].size()));
      if (!annotation.unlocated && target != 0 &&
          !locate(target, annotation.section, annotation.line)) {
        error(lineNumber, "annotation points outside every section");
        continue;
      }
      test.annotations.push_back(std::move(annotation));
    }
  }

  // Mode and consistency.
  const bool hasErrors = std::any_of(
      test.annotations.begin(), test.annotations.end(),
      [](const Annotation &a) { return a.severity == Severity::Error; });
  if (modes.size() > 1)
    error(0, "more than one mode directive");
  if (!modes.empty()) {
    test.modeExplicit = true;
    const std::string &mode = modes.front();
    test.mode = mode == "check-pass"   ? Mode::CheckPass
                : mode == "check-fail" ? Mode::CheckFail
                : mode == "build-pass" ? Mode::BuildPass
                : mode == "run-pass"   ? Mode::RunPass
                                       : Mode::RunFail;
  } else {
    test.mode = hasErrors ? Mode::CheckFail : Mode::CheckPass;
  }
  if (test.mode == Mode::CheckFail && !hasErrors)
    error(0, "check-fail test has no `//~ ERROR` expectation");
  if (test.mode != Mode::CheckFail && hasErrors)
    error(0, std::string(modeName(test.mode)) +
                 " test cannot expect errors; use check-fail");
  if (!modeRuns(test.mode) &&
      (test.hasInlineStdout || !test.stderrContains.empty() || test.exitCode ||
       !test.args.empty() || test.stdinText))
    error(0, "stdout/stderr-contains/exit-code/args/stdin need run-pass or "
             "run-fail");
  if (!test.revisions.empty() && !modeRuns(test.mode))
    error(0, "revisions need run-pass or run-fail");
  if ((test.optimize || !test.only.empty()) && test.reason.empty())
    error(0, "`opt: O3` and `only:` need a `//@ reason:` line");
  if (test.slow && !underSlowDirectory(path))
    error(0, "`slow` is only legal under a slow/ directory");

  // The executions: one per revision, each the shared directives with the
  // revision's own replacing any it repeats.
  const std::filesystem::path sharedStdout = stdoutPathFor(path, "");
  if (std::filesystem::exists(sharedStdout)) {
    if (!modeRuns(test.mode))
      error(0, "an expected .stdout file needs run-pass or run-fail");
    else if (!test.revisions.empty())
      error(0, "a test with revisions keeps expected output in "
               "name.<revision>.stdout");
    if (test.hasInlineStdout)
      error(0, "stdout is given both inline and in a .stdout file");
    test.stdoutFile = sharedStdout;
  }
  if (modeRuns(test.mode)) {
    const std::vector<std::string> names =
        test.revisions.empty() ? std::vector<std::string>{""} : test.revisions;
    for (const std::string &revision : names) {
      RunSpec run = shared;
      run.revision = revision;
      run.mode = test.mode;
      run.stdoutFile = test.stdoutFile;
      if (const auto found = overrides.find(revision);
          !revision.empty() && found != overrides.end()) {
        const Override &entry = found->second;
        if (entry.mode)
          run.mode = *entry.mode;
        if (entry.given.contains("stdout")) {
          run.stdoutLines = entry.spec.stdoutLines;
          run.hasInlineStdout = true;
        }
        if (entry.given.contains("stderr-contains"))
          run.stderrContains = entry.spec.stderrContains;
        if (entry.given.contains("exit-code"))
          run.exitCode = entry.spec.exitCode;
        if (entry.given.contains("args"))
          run.args = entry.spec.args;
        if (entry.given.contains("stdin"))
          run.stdinText = entry.spec.stdinText;
      }
      const std::string where =
          revision.empty() ? "" : "revision '" + revision + "': ";
      if (!revision.empty()) {
        // A revision's own .stdout file replaces shared inline stdout.
        const std::filesystem::path file = stdoutPathFor(path, revision);
        const auto found = overrides.find(revision);
        if (std::filesystem::exists(file)) {
          if (found != overrides.end() &&
              found->second.given.contains("stdout"))
            error(0, where + "stdout is given both inline and in " +
                         file.filename().string());
          run.hasInlineStdout = false;
          run.stdoutFile = file;
        }
      }
      if (run.mode == Mode::RunPass && run.exitCode && *run.exitCode != 0)
        error(0, where + "run-pass expects exit code 0; use run-fail");
      if (run.mode == Mode::RunFail && run.exitCode && *run.exitCode == 0)
        error(0, where + "run-fail cannot expect exit code 0");
      test.runs.push_back(std::move(run));
    }
  }

  test.groupKey = std::string(modeBuilds(test.mode) ? "build" : "check") +
                  (test.optimize ? ":O3" : ":O0");
  for (const Section &section : test.sections)
    test.groupKey += "|" + section.name;
  test.groupKey += "\n" + imports;
  return test;
}

bool hostAdmits(const TestFile &test) {
  if (test.only.empty())
    return true;
#if defined(__APPLE__)
  const std::string os = "macos";
#else
  const std::string os = "linux";
#endif
#if defined(__aarch64__) || defined(__arm64__)
  const std::string arch = "arm64";
#else
  const std::string arch = "x86_64";
#endif
  bool osGate = false;
  bool osMatch = false;
  bool archGate = false;
  bool archMatch = false;
  for (const std::string &gate : test.only) {
    if (gate == "linux" || gate == "macos") {
      osGate = true;
      osMatch = osMatch || gate == os;
    } else {
      archGate = true;
      archMatch = archMatch || gate == arch;
    }
  }
  return (!osGate || osMatch) && (!archGate || archMatch);
}

std::string
blessAnnotations(const TestFile &test,
                 const std::vector<LocatedDiagnostic> &diagnostics) {
  const std::optional<std::string> contents = readFile(test.path);
  std::vector<std::string> lines = splitLines(contents.value_or(""));
  std::vector<bool> removed(lines.size(), false);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (lines[index].find("//~") == std::string::npos)
      continue;
    const std::string code = codePart(lines[index]);
    if (trim(code).empty())
      removed[index] = true;
    lines[index] = rtrim(code);
  }
  // Keep the message substring of an annotation that still matches.
  std::vector<bool> used(test.annotations.size(), false);
  std::vector<std::string> trailing(lines.size());
  std::vector<std::string> unlocated;
  for (const LocatedDiagnostic &diagnostic : diagnostics) {
    std::string substring;
    for (std::size_t index = 0; index < test.annotations.size(); ++index) {
      const Annotation &annotation = test.annotations[index];
      if (used[index] || annotation.severity != diagnostic.severity ||
          annotation.code != diagnostic.code)
        continue;
      const bool located = !diagnostic.section.empty();
      if (annotation.unlocated ? located
                               : (annotation.section != diagnostic.section ||
                                  annotation.line != diagnostic.line))
        continue;
      if (!annotation.substring.empty() &&
          diagnostic.message.find(annotation.substring) == std::string::npos)
        continue;
      used[index] = true;
      substring = annotation.substring;
      break;
    }
    std::string text =
        std::string(severityName(diagnostic.severity)) + " " + diagnostic.code;
    if (!substring.empty())
      text += " " + substring;
    if (diagnostic.section.empty()) {
      unlocated.push_back("//~? " + text);
      continue;
    }
    int fileLine = 0;
    for (const Section &section : test.sections)
      if (section.name == diagnostic.section)
        fileLine = section.firstLine + diagnostic.line - 1;
    if (fileLine < 1 || fileLine > static_cast<int>(lines.size())) {
      unlocated.push_back("//~? " + text);
      continue;
    }
    const std::size_t index = static_cast<std::size_t>(fileLine - 1);
    removed[index] = false;
    trailing[index] += (trailing[index].empty() ? "" : " ") + ("//~ " + text);
  }
  std::string result;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (removed[index])
      continue;
    std::string line = lines[index];
    if (!trailing[index].empty())
      line += (trim(line).empty() ? "" : "  ") + trailing[index];
    result += line + "\n";
  }
  for (const std::string &line : unlocated)
    result += line + "\n";
  return result;
}

std::filesystem::path stdoutPathFor(const std::filesystem::path &test,
                                    const std::string &revision) {
  std::filesystem::path result = test;
  result.replace_extension(revision.empty() ? ".stdout"
                                            : "." + revision + ".stdout");
  return result;
}

std::string blessInlineStdout(const TestFile &test, const std::string &output,
                              const std::string &revision) {
  const std::optional<std::string> contents = readFile(test.path);
  const std::vector<std::string> lines = splitLines(contents.value_or(""));
  const std::string prefix =
      revision.empty() ? std::string("//@") : "//@[" + revision + "]";
  const std::string marker = prefix + " stdout:";
  std::vector<std::string> replacement;
  std::string remaining = output;
  if (!remaining.empty() && remaining.back() == '\n')
    remaining.pop_back();
  if (!output.empty()) {
    std::size_t start = 0;
    while (true) {
      const std::size_t end = remaining.find('\n', start);
      replacement.push_back(marker + " " +
                            remaining.substr(start, end == std::string::npos
                                                        ? end
                                                        : end - start));
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
  }
  // Inline lines replace the first existing `stdout:` directive, or follow
  // the last header directive (the revision's own last one, if any).
  std::size_t insertAt = lines.size();
  std::size_t lastDirective = 0;
  bool sawDirective = false;
  std::size_t lastOwn = 0;
  bool sawOwn = false;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string text = trim(lines[index]);
    if (text.rfind(marker, 0) == 0 && insertAt == lines.size())
      insertAt = index;
    if (text.rfind("//@", 0) == 0) {
      lastDirective = index;
      sawDirective = true;
    }
    if (!revision.empty() && text.rfind(prefix + " ", 0) == 0) {
      lastOwn = index;
      sawOwn = true;
    }
  }
  if (insertAt == lines.size())
    insertAt = sawOwn         ? lastOwn + 1
               : sawDirective ? lastDirective + 1
                              : 1;
  std::string result;
  for (std::size_t index = 0; index <= lines.size(); ++index) {
    if (index == insertAt)
      for (const std::string &line : replacement)
        result += line + "\n";
    if (index == lines.size())
      break;
    if (trim(lines[index]).rfind(marker, 0) == 0)
      continue;
    result += lines[index] + "\n";
  }
  return result;
}

} // namespace gti_test
