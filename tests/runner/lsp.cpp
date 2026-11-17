#include "lsp.h"
#include "snapshot.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
namespace json = llvm::json;

namespace gti_test {

double lspClock() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {

std::string print(const json::Value &value) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  stream << value;
  return text;
}

std::string shorten(std::string text, std::size_t limit = 300) {
  if (text.size() > limit)
    text = text.substr(0, limit) + "...";
  return text;
}

bool writeAll(int fd, const std::string &text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t count =
        write(fd, text.data() + written, text.size() - written);
    if (count <= 0) {
      if (count < 0 && errno == EINTR)
        continue;
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

std::string uriFor(const fs::path &path) {
  std::string uri = "file://";
  for (const unsigned char c : path.string()) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      uri += static_cast<char>(c);
    } else {
      static const char hex[] = "0123456789ABCDEF";
      uri += '%';
      uri += hex[c >> 4];
      uri += hex[c & 15];
    }
  }
  return uri;
}

// A source position in LSP coordinates (UTF-16 code units).
struct Position {
  int line = 0;
  int character = 0;
  friend bool operator==(const Position &, const Position &) = default;
  friend bool operator<(const Position &a, const Position &b) {
    return a.line != b.line ? a.line < b.line : a.character < b.character;
  }
};

Position positionAt(const std::string &text, std::size_t offset) {
  Position position;
  for (std::size_t index = 0; index < offset && index < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[index]);
    if (c == '\n') {
      ++position.line;
      position.character = 0;
      ++index;
      continue;
    }
    const std::size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    position.character += length == 4 ? 2 : 1;
    index += length;
  }
  return position;
}

std::string describe(const Position &position) {
  return std::to_string(position.line + 1) + ":" +
         std::to_string(position.character + 1);
}

// `/*@name*/` markers: the position just after the comment.
std::map<std::string, Position> markers(const std::string &text,
                                        std::vector<std::string> &errors) {
  std::map<std::string, Position> result;
  for (std::size_t at = text.find("/*@"); at != std::string::npos;
       at = text.find("/*@", at + 3)) {
    const std::size_t close = text.find("*/", at);
    if (close == std::string::npos)
      break;
    // Directive lines may mention markers; they never define one.
    const std::size_t lineStart = text.rfind('\n', at);
    const std::size_t first = text.find_first_not_of(
        " \t", lineStart == std::string::npos ? 0 : lineStart + 1);
    if (text.compare(first, 3, "//@") == 0)
      continue;
    const std::string name = text.substr(at + 3, close - at - 3);
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        })) {
      errors.push_back("marker names are letters, digits and _: /*@" + name +
                       "*/");
      continue;
    }
    if (!result.emplace(name, positionAt(text, close + 2)).second)
      errors.push_back("duplicate marker @" + name);
  }
  return result;
}

std::optional<Position> positionOf(const json::Value *value) {
  const json::Object *object = value ? value->getAsObject() : nullptr;
  if (!object)
    return std::nullopt;
  const auto line = object->getInteger("line");
  const auto character = object->getInteger("character");
  if (!line || !character)
    return std::nullopt;
  return Position{static_cast<int>(*line), static_cast<int>(*character)};
}

std::optional<Position> rangeStart(const json::Object &object) {
  const json::Object *range = object.getObject("range");
  if (!range)
    range = object.getObject("targetSelectionRange");
  return range ? positionOf(range->get("start")) : std::nullopt;
}

json::Value positionJson(const Position &position) {
  return json::Object{{"line", position.line},
                      {"character", position.character}};
}

// The text of a hover result, whatever shape the server chose.
std::string hoverText(const json::Value *result) {
  if (!result)
    return "";
  if (const auto text = result->getAsString())
    return text->str();
  if (const json::Object *object = result->getAsObject()) {
    if (const json::Value *contents = object->get("contents"))
      return hoverText(contents);
    if (const auto value = object->getString("value"))
      return value->str();
  }
  if (const json::Array *array = result->getAsArray()) {
    std::string text;
    for (const json::Value &item : *array)
      text += hoverText(&item) + "\n";
    return text;
  }
  return print(*result);
}

// Applies LSP text edits (non-overlapping) to `text`.
std::string applyEdits(const std::string &text, const json::Array &edits) {
  std::vector<std::size_t> lineStarts{0};
  for (std::size_t index = 0; index < text.size(); ++index)
    if (text[index] == '\n')
      lineStarts.push_back(index + 1);
  const auto offset = [&](const Position &position) {
    if (position.line >= static_cast<int>(lineStarts.size()))
      return text.size();
    std::size_t at = lineStarts[static_cast<std::size_t>(position.line)];
    int units = 0;
    while (at < text.size() && text[at] != '\n' && units < position.character) {
      const unsigned char c = static_cast<unsigned char>(text[at]);
      const std::size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      units += length == 4 ? 2 : 1;
      at += length;
    }
    return at;
  };
  struct Edit {
    std::size_t begin;
    std::size_t end;
    std::string text;
  };
  std::vector<Edit> ordered;
  for (const json::Value &value : edits) {
    const json::Object *edit = value.getAsObject();
    const json::Object *range = edit ? edit->getObject("range") : nullptr;
    if (!range)
      continue;
    const auto start = positionOf(range->get("start"));
    const auto end = positionOf(range->get("end"));
    if (!start || !end)
      continue;
    ordered.push_back({offset(*start), offset(*end),
                       edit->getString("newText").value_or("").str()});
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const Edit &a, const Edit &b) { return a.begin > b.begin; });
  std::string result = text;
  for (const Edit &edit : ordered)
    result.replace(edit.begin, edit.end - edit.begin, edit.text);
  return result;
}

void collectSymbolNames(const json::Array &symbols,
                        std::set<std::string> &names) {
  for (const json::Value &value : symbols) {
    const json::Object *symbol = value.getAsObject();
    if (!symbol)
      continue;
    if (const auto name = symbol->getString("name"))
      names.insert(name->str());
    if (const json::Array *children = symbol->getArray("children"))
      collectSymbolNames(*children, names);
  }
}

std::string joined(const std::set<std::string> &items) {
  std::string text;
  for (const std::string &item : items)
    text += (text.empty() ? "" : " ") + item;
  return text.empty() ? "(none)" : text;
}

} // namespace

bool auditing(const std::vector<std::string> &serverArguments) {
  return std::any_of(serverArguments.begin(), serverArguments.end(),
                     [](const std::string &argument) {
                       return argument == "--raw-pointer-audit=warn" ||
                              argument == "--raw-pointer-audit=strict";
                     });
}

LspServer::LspServer(fs::path executable, fs::path log,
                     std::vector<std::string> arguments)
    : executable(std::move(executable)), log(std::move(log)),
      arguments(std::move(arguments)) {}

LspServer::~LspServer() { stop(); }

bool LspServer::start(std::string &error) {
  int down[2];
  int up[2];
  if (pipe(down) != 0 || pipe(up) != 0) {
    error = "pipe failed";
    return false;
  }
  const std::string program = executable.string();
  std::vector<char *> argv{const_cast<char *>(program.c_str())};
  for (const std::string &argument : arguments)
    argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
  const pid_t child = fork();
  if (child == 0) {
    dup2(down[0], 0);
    dup2(up[1], 1);
    const int logFd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logFd >= 0)
      dup2(logFd, 2);
    close(down[1]);
    close(up[0]);
    // A raw-pointer audit publishes as soon as it finishes, not after the
    // editor's quiet period.
    if (auditing(arguments))
      setenv("GTI_LSP_AUDIT_QUIET_MS", "0", 1);
    execv(argv[0], argv.data());
    _exit(127);
  }
  close(down[0]);
  close(up[1]);
  if (child < 0) {
    close(down[1]);
    close(up[0]);
    error = "fork failed";
    return false;
  }
  pid = child;
  exitCode = -1;
  input = down[1];
  output = up[0];
  buffer.clear();
  pending.clear();
  warm = false;
  return true;
}

bool LspServer::startInitialized(double seconds, std::string &error) {
  if (!start(error))
    return false;
  // Code actions need literal support (and report document versions with
  // documentChanges); hover asks for markdown or plain text.
  json::Object capabilities{
      {"workspace", json::Object{{"workspaceEdit",
                                  json::Object{{"documentChanges", true}}}}},
      {"textDocument",
       json::Object{
           {"publishDiagnostics", json::Object{{"relatedInformation", true}}},
           {"documentSymbol",
            json::Object{{"hierarchicalDocumentSymbolSupport", true}}},
           {"codeAction",
            json::Object{
                {"codeActionLiteralSupport",
                 json::Object{
                     {"codeActionKind",
                      json::Object{{"valueSet", json::Array{"quickfix"}}}}}}}},
           {"hover", json::Object{{"contentFormat",
                                   json::Array{"markdown", "plaintext"}}}}}}};
  const auto initialized =
      request("initialize",
              json::Object{{"processId", nullptr},
                           {"rootUri", nullptr},
                           {"capabilities", std::move(capabilities)}},
              lspClock() + seconds);
  if (!initialized) {
    error = "gti_lsp did not answer initialize\n" + logTail();
    stop();
    return false;
  }
  tokenTypes.clear();
  if (const json::Object *result = initialized->getObject("result"))
    if (const json::Object *caps = result->getObject("capabilities"))
      if (const json::Object *tokens =
              caps->getObject("semanticTokensProvider"))
        if (const json::Object *legend = tokens->getObject("legend"))
          if (const json::Array *types = legend->getArray("tokenTypes"))
            for (const json::Value &type : *types)
              tokenTypes.push_back(type.getAsString().value_or("").str());
  send(json::Object{{"jsonrpc", "2.0"},
                    {"method", "initialized"},
                    {"params", json::Object{}}});
  return true;
}

void LspServer::stop() {
  if (pid > 0) {
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
  }
  if (input >= 0)
    close(input);
  if (output >= 0)
    close(output);
  pid = -1;
  input = output = -1;
}

void LspServer::send(const json::Value &message) {
  const std::string payload = print(message);
  if (input >= 0 &&
      !writeAll(input, "Content-Length: " + std::to_string(payload.size()) +
                           "\r\n\r\n" + payload))
    stop();
}

std::optional<json::Value> LspServer::receive(double deadline) {
  while (pid > 0) {
    const std::size_t header = buffer.find("\r\n\r\n");
    if (header != std::string::npos) {
      std::size_t length = 0;
      std::istringstream headers(buffer.substr(0, header));
      for (std::string line; std::getline(headers, line);)
        if (line.rfind("Content-Length:", 0) == 0)
          length = std::stoul(line.substr(15));
      if (buffer.size() >= header + 4 + length) {
        const std::string body = buffer.substr(header + 4, length);
        buffer.erase(0, header + 4 + length);
        llvm::Expected<json::Value> parsed = json::parse(body);
        if (!parsed) {
          llvm::consumeError(parsed.takeError());
          continue;
        }
        const json::Object *object = parsed->getAsObject();
        // Answer server-to-client requests so the server never waits.
        if (object && object->get("id") && object->get("method"))
          send(json::Object{{"jsonrpc", "2.0"},
                            {"id", *object->get("id")},
                            {"result", nullptr}});
        return std::move(*parsed);
      }
    }
    const double remaining = deadline - lspClock();
    pollfd fd{output, POLLIN, 0};
    const int ready = poll(
        &fd, 1,
        remaining <= 0 ? 0
                       : static_cast<int>(std::min(remaining, 1.0) * 1000) + 1);
    if (ready < 0 && errno != EINTR)
      return std::nullopt;
    if (ready <= 0) {
      if (remaining <= 0)
        return std::nullopt;
      continue;
    }
    char chunk[65536];
    const ssize_t count = read(output, chunk, sizeof chunk);
    if (count <= 0) {
      if (count < 0 && errno == EINTR)
        continue;
      int status = 0;
      waitpid(pid, &status, 0);
      exitCode =
          WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      pid = -1;
      return std::nullopt;
    }
    buffer.append(chunk, static_cast<std::size_t>(count));
  }
  return std::nullopt;
}

std::optional<json::Object> LspServer::request(const std::string &method,
                                               json::Value params,
                                               double deadline) {
  const long id = nextId++;
  send(json::Object{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"method", method},
                    {"params", std::move(params)}});
  while (true) {
    std::optional<json::Value> message = receive(deadline);
    if (!message)
      return std::nullopt;
    json::Object *object = message->getAsObject();
    if (!object)
      continue;
    if (!object->get("method") && object->getInteger("id") == id)
      return std::move(*object);
    pending.push_back(std::move(*object));
  }
}

std::optional<json::Object>
LspServer::waitFor(const std::function<bool(const json::Object &)> &predicate,
                   double deadline) {
  for (auto it = pending.begin(); it != pending.end(); ++it)
    if (predicate(*it)) {
      json::Object found = std::move(*it);
      pending.erase(it);
      return found;
    }
  while (true) {
    std::optional<json::Value> message = receive(deadline);
    if (!message)
      return std::nullopt;
    json::Object *object = message->getAsObject();
    if (!object)
      continue;
    if (predicate(*object))
      return std::move(*object);
    pending.push_back(std::move(*object));
    if (pending.size() > 256)
      pending.pop_front();
  }
}

void LspServer::drain() {
  // A deadline in the past reads only what is already buffered or readable.
  while (std::optional<json::Value> message = receive(0)) {
    if (json::Object *object = message->getAsObject())
      pending.push_back(std::move(*object));
    if (pending.size() > 256)
      pending.pop_front();
  }
}

std::optional<json::Object>
LspServer::latest(const std::function<bool(const json::Object &)> &predicate) {
  drain();
  std::optional<json::Object> found;
  for (auto it = pending.begin(); it != pending.end();) {
    if (predicate(*it)) {
      found = std::move(*it);
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
  return found;
}

double LspServer::cpuSeconds() const {
  if (pid <= 0)
    return -1;
#if defined(__APPLE__)
  rusage_info_v2 info{};
  if (proc_pid_rusage(pid, RUSAGE_INFO_V2,
                      reinterpret_cast<rusage_info_t *>(&info)) != 0)
    return -1;
  // Mach absolute-time units, not nanoseconds, on Apple silicon.
  mach_timebase_info_data_t base{};
  mach_timebase_info(&base);
  const double ticks =
      static_cast<double>(info.ri_user_time + info.ri_system_time);
  return ticks * base.numer / base.denom / 1e9;
#else
  const std::string stat =
      readFile("/proc/" + std::to_string(pid) + "/stat").value_or("");
  // Fields after the parenthesised command: state is the 3rd, utime the
  // 14th and stime the 15th.
  const std::size_t close = stat.rfind(')');
  if (close == std::string::npos)
    return -1;
  std::istringstream fields(stat.substr(close + 2));
  std::string field;
  double ticks = 0;
  for (int index = 3; fields >> field && index <= 15; ++index)
    if (index >= 14)
      ticks += std::strtod(field.c_str(), nullptr);
  return ticks / static_cast<double>(sysconf(_SC_CLK_TCK));
#endif
}

std::string LspServer::logTail() const {
  const std::string text = readFile(log).value_or("");
  return text.size() > 1500 ? "..." + text.substr(text.size() - 1500) : text;
}

namespace {

// One file of a marker test: a document when opened.
struct Document {
  std::string name; // the section name
  fs::path path;
  std::string uri;
  std::string text;
  int version = 1;
  bool open = false;
  std::map<std::string, Position> marks;
  // The diagnostics last published for the current version, as sent.
  json::Array published;
};

// A position in one of the test's documents.
struct Place {
  std::size_t document = 0;
  Position position;
  friend bool operator==(const Place &, const Place &) = default;
  friend bool operator<(const Place &a, const Place &b) {
    return a.document != b.document ? a.document < b.document
                                    : a.position < b.position;
  }
};

// `@std:std/functional.gti` or `@std:std/functional.gti#name`: a location in
// a standard-library file (relative to its root), optionally on a line that
// names `symbol`.
struct LibraryPlace {
  std::string path;
  std::string symbol;
};

struct LibraryLocation {
  std::string path; // relative to the standard library root
  Position position;
  std::string line; // the text of that line
};

std::string percentDecoded(const std::string &text) {
  std::string out;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%' && index + 2 < text.size() &&
        std::isxdigit(static_cast<unsigned char>(text[index + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[index + 2]))) {
      out +=
          static_cast<char>(std::stoi(text.substr(index + 1, 2), nullptr, 16));
      index += 2;
    } else {
      out += text[index];
    }
  }
  return out;
}

// The path a `file://` URI names, made canonical where it exists.
fs::path uriPath(const std::string &uri) {
  if (uri.rfind("file://", 0) != 0)
    return {};
  const fs::path path = percentDecoded(uri.substr(7));
  std::error_code error;
  const fs::path canonical = fs::weakly_canonical(path, error);
  return error ? path.lexically_normal() : canonical;
}

// Removes `/*^name*/` cursor markers from `text` (outside `//@` lines) and
// returns the remaining text; their positions, and those of `/*@name*/`
// markers in the remaining text, go to `marks`.
std::string prepare(const std::string &text,
                    std::map<std::string, Position> &marks,
                    std::vector<std::string> &errors) {
  std::string stripped;
  std::vector<std::pair<std::string, std::size_t>> cursors;
  std::size_t copied = 0;
  for (std::size_t at = text.find("/*^"); at != std::string::npos;
       at = text.find("/*^", at + 3)) {
    const std::size_t close = text.find("*/", at);
    if (close == std::string::npos)
      break;
    const std::size_t lineStart = text.rfind('\n', at);
    const std::size_t first = text.find_first_not_of(
        " \t", lineStart == std::string::npos ? 0 : lineStart + 1);
    if (text.compare(first, 3, "//@") == 0)
      continue;
    stripped += text.substr(copied, at - copied);
    cursors.emplace_back(text.substr(at + 3, close - at - 3), stripped.size());
    copied = close + 2;
    at = close;
  }
  stripped += text.substr(copied);
  marks = markers(stripped, errors);
  for (const auto &[name, offset] : cursors) {
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        })) {
      errors.push_back("marker names are letters, digits and _: /*^" + name +
                       "*/");
      continue;
    }
    if (!marks.emplace(name, positionAt(stripped, offset)).second)
      errors.push_back("duplicate marker @" + name);
  }
  return stripped;
}

// `text` as code, for comparing a code action's result with an edit step:
// without `//` comment lines (the purpose line, directives), `//~`
// annotations, `/*@name*/` markers and blank lines, with runs of blanks as
// one space.
std::string asCode(const std::string &text) {
  std::string result;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line.compare(first, 2, "//") == 0)
      continue;
    if (const std::size_t annotation = line.find("//~");
        annotation != std::string::npos)
      line.erase(annotation);
    for (std::size_t at = line.find("/*@"); at != std::string::npos;
         at = line.find("/*@", at)) {
      const std::size_t close = line.find("*/", at);
      if (close == std::string::npos)
        break;
      line.erase(at, close + 2 - at);
    }
    std::string squeezed;
    for (const char c : line) {
      const bool blank = c == ' ' || c == '\t';
      if (blank && (squeezed.empty() || squeezed.back() == ' '))
        continue;
      squeezed += blank ? ' ' : c;
    }
    while (!squeezed.empty() && squeezed.back() == ' ')
      squeezed.pop_back();
    if (!squeezed.empty())
      result += squeezed + "\n";
  }
  return result;
}

bool contains(const Position &start, const Position &end,
              const Position &position) {
  return !(position < start) && !(end < position);
}

} // namespace

LspResult runLspTest(LspServer &server, const TestFile &test,
                     const fs::path &directory, const fs::path &standardLibrary,
                     double hangGuard) {
  LspResult result;
  // Wall time is only a hang guard, renewed for every awaited message.
  const auto deadline = [&] { return lspClock() + hangGuard; };
  const auto fail = [&](const std::string &line) {
    result.failure += line + "\n";
  };
  std::error_code ignored;
  const fs::path libraryRoot = fs::weakly_canonical(standardLibrary, ignored);

  // File sections become documents; each step replaces one document's text.
  struct Step {
    std::size_t section = 0;
    std::size_t document = 0;
    int number = 0;
    std::string text;
    std::map<std::string, Position> marks;
  };
  std::vector<Document> documents;
  std::vector<Step> steps;
  // The document each section is (or edits).
  std::vector<std::size_t> sectionDocument(test.sections.size(), 0);
  fs::create_directories(directory);
  for (std::size_t index = 0; index < test.sections.size(); ++index) {
    const Section &section = test.sections[index];
    std::vector<std::string> errors;
    std::map<std::string, Position> marks;
    std::string text = prepare(section.text, marks, errors);
    for (const std::string &error : errors)
      fail("invalid test: " + section.name + ": " + error);
    if (const std::optional<LspStep> step = lspStep(section.name)) {
      const std::string target =
          step->file.empty() ? test.sections.front().name : step->file;
      for (std::size_t document = 0; document < documents.size(); ++document)
        if (documents[document].name == target)
          sectionDocument[index] = document;
      steps.push_back({index, sectionDocument[index], step->number,
                       std::move(text), std::move(marks)});
      continue;
    }
    Document document;
    document.name = section.name;
    document.path = (directory / section.name).lexically_normal();
    document.uri = uriFor(document.path);
    document.text = std::move(text);
    document.marks = std::move(marks);
    document.open = section.name.ends_with(".gti") &&
                    std::find(test.diskOnly.begin(), test.diskOnly.end(),
                              section.name) == test.diskOnly.end();
    sectionDocument[index] = documents.size();
    documents.push_back(std::move(document));
  }
  if (!result.failure.empty())
    return result;
  for (const Document &document : documents) {
    fs::create_directories(document.path.parent_path());
    writeFile(document.path, document.text);
  }

  const auto documentId = [&](std::size_t document) {
    return json::Object{{"uri", documents[document].uri}};
  };
  const auto closeAll = [&] {
    for (const Document &document : documents)
      if (document.open)
        server.send(json::Object{
            {"jsonrpc", "2.0"},
            {"method", "textDocument/didClose"},
            {"params", json::Object{{"textDocument",
                                     json::Object{{"uri", document.uri}}}}}});
  };
  const auto timeout = [&](const std::string &what) {
    result.crashed = !server.running();
    result.timedOut = !result.crashed;
    fail("no " + what + " within the hang guard (" +
         std::to_string(hangGuard).substr(0, 5) + " s of wall time)" +
         (server.running() ? "" : "; gti_lsp exited:\n" + server.logTail()));
  };
  const auto isPublication = [&](const json::Object &message,
                                 const Document &document) {
    const json::Object *params = message.getObject("params");
    return message.getString("method") == "textDocument/publishDiagnostics" &&
           params && params->getString("uri") == document.uri &&
           params->getInteger("version") == document.version;
  };
  // The diagnostics of a document's current version: with a raw-pointer
  // audit, its second publication (the edit path's, then the audit's
  // additions).
  const bool audited = auditing(test.serverArguments);
  const auto publication = [&](const Document &document) {
    std::optional<json::Object> published;
    for (int round = 0; round < (audited ? 2 : 1); ++round) {
      published = server.waitFor(
          [&](const json::Object &message) {
            return isPublication(message, document);
          },
          deadline());
      if (!published)
        break;
    }
    return published;
  };
  // Records a publication's diagnostics against `section`'s annotations.
  const auto record = [&](std::size_t document, std::size_t section,
                          const json::Object &published) {
    result.published = true;
    documents[document].published.clear();
    const json::Object *params = published.getObject("params");
    const json::Array *diagnostics =
        params ? params->getArray("diagnostics") : nullptr;
    if (!diagnostics)
      return;
    documents[document].published = *diagnostics;
    for (const json::Value &value : *diagnostics) {
      const json::Object *diagnostic = value.getAsObject();
      if (!diagnostic)
        continue;
      const auto severity = diagnostic->getInteger("severity").value_or(1);
      if (severity != 1 && severity != 2)
        continue;
      LocatedDiagnostic located;
      located.severity = severity == 1 ? Severity::Error : Severity::Warning;
      located.code = diagnostic->getString("code").value_or("").str();
      located.message = diagnostic->getString("message").value_or("").str();
      const auto start = rangeStart(*diagnostic);
      located.section = test.sections[section].name;
      located.line = start ? start->line + 1 : 0;
      result.diagnostics.push_back(std::move(located));
    }
  };

  // Every directive of one step, against the documents' current texts.
  // Returns false once the server stopped answering.
  const auto ask = [&](std::size_t stepIndex) -> bool {
    std::map<std::size_t, json::Object> tokenCache;
    for (const LspDirective &directive : test.lspDirectives) {
      if (directive.step != static_cast<int>(stepIndex))
        continue;
      const std::size_t home =
          sectionDocument[static_cast<std::size_t>(directive.section)];
      const std::string &kind = directive.words.front();
      const std::string where =
          "line " + std::to_string(directive.writtenLine) + " (" + kind + "): ";
      bool invalid = false;
      const auto invalidTest = [&](const std::string &message) {
        fail("invalid test: " + where + message);
        invalid = true;
      };
      // `@m` (this document first, then any other), or `@file:m`.
      const auto resolve =
          [&](const std::string &word) -> std::optional<Place> {
        if (word.size() < 2 || word.front() != '@') {
          invalidTest("expected a marker, got '" + word + "'");
          return std::nullopt;
        }
        const std::string name = word.substr(1);
        const std::size_t colon = name.find(':');
        if (colon != std::string::npos) {
          const std::string file = name.substr(0, colon);
          for (std::size_t document = 0; document < documents.size();
               ++document)
            if (documents[document].name == file) {
              const auto found =
                  documents[document].marks.find(name.substr(colon + 1));
              if (found != documents[document].marks.end())
                return Place{document, found->second};
            }
          invalidTest("no marker " + word);
          return std::nullopt;
        }
        if (const auto found = documents[home].marks.find(name);
            found != documents[home].marks.end())
          return Place{home, found->second};
        std::vector<Place> candidates;
        for (std::size_t document = 0; document < documents.size(); ++document)
          if (const auto found = documents[document].marks.find(name);
              found != documents[document].marks.end())
            candidates.push_back({document, found->second});
        if (candidates.size() == 1)
          return candidates.front();
        invalidTest(candidates.empty()
                        ? "no marker /*@" + name + "*/"
                        : "marker @" + name +
                              " is in several files; write @file:" + name);
        return std::nullopt;
      };
      const auto describePlace = [&](const Place &place) {
        const Document &document = documents[place.document];
        for (const auto &[name, position] : document.marks)
          if (position == place.position)
            return "@" + (place.document == home ? "" : document.name + ":") +
                   name;
        return (place.document == home ? "" : document.name + ":") +
               describe(place.position);
      };
      const auto placesText = [&](const std::set<Place> &places) {
        std::string text;
        for (const Place &place : places)
          text += (text.empty() ? "" : " ") + describePlace(place);
        return text.empty() ? "(none)" : text;
      };
      const auto at = [&](const Place &place) {
        return json::Object{{"textDocument", documentId(place.document)},
                            {"position", positionJson(place.position)}};
      };
      // Where a location lands: one of the documents, the standard library,
      // or elsewhere (its path).
      struct Landing {
        std::optional<Place> place;
        std::optional<LibraryLocation> library;
        std::string elsewhere;
      };
      const auto land = [&](const std::string &uri,
                            const Position &start) -> Landing {
        Landing landing;
        for (std::size_t document = 0; document < documents.size(); ++document)
          if (documents[document].uri == uri) {
            landing.place = Place{document, start};
            return landing;
          }
        const fs::path path = uriPath(uri);
        for (std::size_t document = 0; document < documents.size(); ++document)
          if (!path.empty() && documents[document].path == path) {
            landing.place = Place{document, start};
            return landing;
          }
        const fs::path relative = path.lexically_relative(libraryRoot);
        if (!path.empty() && !relative.empty() && *relative.begin() != "..") {
          std::string line;
          std::istringstream text(readFile(path).value_or(""));
          for (int index = 0; index <= start.line && std::getline(text, line);
               ++index) {
          }
          landing.library =
              LibraryLocation{relative.generic_string(), start, line};
          return landing;
        }
        landing.elsewhere = path.empty() ? uri : path.string();
        return landing;
      };
      // `@a @file:b @std:path#name` expectations.
      const auto expectations = [&](const std::string &value,
                                    std::set<Place> &places,
                                    std::vector<LibraryPlace> &library) {
        std::istringstream words(value);
        for (std::string word; words >> word;) {
          if (word.rfind("@std:", 0) == 0) {
            const std::string spec = word.substr(5);
            const std::size_t hash = spec.find('#');
            library.push_back(
                {spec.substr(0, hash),
                 hash == std::string::npos ? "" : spec.substr(hash + 1)});
            if (library.back().path.empty())
              invalidTest("write @std:path/in/stdlib.gti[#symbol]");
            continue;
          }
          if (const auto place = resolve(word))
            places.insert(*place);
        }
        return !invalid;
      };
      const auto libraryText = [](const std::vector<LibraryLocation> &found) {
        std::string text;
        for (const LibraryLocation &location : found)
          text += " @std:" + location.path + ":" + describe(location.position);
        return text;
      };
      // Checks landed locations against the expected places.
      const auto compareLocations = [&](const std::string &value,
                                        const std::vector<Landing> &landed) {
        std::set<Place> expected;
        std::vector<LibraryPlace> expectedLibrary;
        if (value != "none" && !expectations(value, expected, expectedLibrary))
          return;
        std::set<Place> actual;
        std::vector<LibraryLocation> library;
        std::set<std::string> elsewhere;
        for (const Landing &landing : landed) {
          if (landing.place)
            actual.insert(*landing.place);
          else if (landing.library)
            library.push_back(*landing.library);
          else
            elsewhere.insert(landing.elsewhere);
        }
        const auto matches = [](const LibraryPlace &want,
                                const LibraryLocation &location) {
          return location.path == want.path &&
                 (want.symbol.empty() ||
                  location.line.find(want.symbol) != std::string::npos);
        };
        bool same = actual == expected && elsewhere.empty();
        for (const LibraryPlace &want : expectedLibrary)
          same = same && std::any_of(library.begin(), library.end(),
                                     [&](const LibraryLocation &location) {
                                       return matches(want, location);
                                     });
        for (const LibraryLocation &location : library)
          same = same &&
                 std::any_of(expectedLibrary.begin(), expectedLibrary.end(),
                             [&](const LibraryPlace &want) {
                               return matches(want, location);
                             });
        if (same)
          return;
        std::string want = expected.empty() ? "" : placesText(expected);
        for (const LibraryPlace &place : expectedLibrary)
          want += (want.empty() ? "@std:" : " @std:") + place.path +
                  (place.symbol.empty() ? "" : "#" + place.symbol);
        std::string got = actual.empty() ? "" : placesText(actual);
        got += libraryText(library);
        for (const std::string &path : elsewhere)
          got += " " + path;
        const std::size_t first = got.find_first_not_of(' ');
        got = first == std::string::npos ? "(none)" : got.substr(first);
        fail(where + "expected " + (want.empty() ? "(none)" : want) + "; got " +
             got);
      };
      // Locations from a definition/references result.
      const auto landings = [&](const json::Value *value) {
        std::vector<Landing> landed;
        const auto add = [&](const json::Value &item) {
          const json::Object *location = item.getAsObject();
          if (!location)
            return;
          const auto target = location->getString("uri")
                                  ? location->getString("uri")
                                  : location->getString("targetUri");
          const auto start = rangeStart(*location);
          if (target && start)
            landed.push_back(land(target->str(), *start));
        };
        if (value && value->getAsArray())
          for (const json::Value &item : *value->getAsArray())
            add(item);
        else if (value && value->getAsObject())
          add(*value);
        return landed;
      };
      // Text edits of a WorkspaceEdit, by URI.
      const auto workspaceEdits =
          [&](const json::Object &edit,
              std::map<std::string, json::Array> &byUri,
              std::map<std::string, std::int64_t> &versions) {
            if (const json::Object *changes = edit.getObject("changes"))
              for (const auto &[uri, edits] : *changes)
                if (const json::Array *array = edits.getAsArray())
                  for (const json::Value &item : *array)
                    byUri[uri.str()].push_back(item);
            if (const json::Array *changes = edit.getArray("documentChanges"))
              for (const json::Value &change : *changes) {
                const json::Object *object = change.getAsObject();
                const json::Object *document =
                    object ? object->getObject("textDocument") : nullptr;
                const json::Array *edits =
                    object ? object->getArray("edits") : nullptr;
                if (!document || !edits)
                  continue;
                const std::string uri =
                    document->getString("uri").value_or("").str();
                if (const auto version = document->getInteger("version"))
                  versions[uri] = *version;
                for (const json::Value &item : *edits)
                  byUri[uri].push_back(item);
              }
          };
      const auto expectText = [&](const std::string &what,
                                  const std::string &text) {
        // `none`, `!text` (absent) or `text` (present).
        if (directive.value == "none") {
          if (!text.empty() && text != "null")
            fail(where + "expected no " + what + ", got: " + shorten(text));
        } else if (directive.value.front() == '!') {
          if (text.find(directive.value.substr(1)) != std::string::npos)
            fail(where + what + " contains \"" + directive.value.substr(1) +
                 "\"; got: " + shorten(text));
        } else if (text.find(directive.value) == std::string::npos) {
          fail(where + what + " does not contain \"" + directive.value +
               "\"; got: " + shorten(text));
        }
      };

      const bool needsMarker = kind != "formatting" && kind != "symbols";
      std::optional<Place> marker;
      if (needsMarker) {
        if (directive.words.size() < 2 || directive.words[1].front() != '@') {
          invalidTest("write `//@ " + kind + " @marker: ...`");
          continue;
        }
        marker = resolve(directive.words[1]);
        if (!marker)
          continue;
      }
      if ((kind == "hover" || kind == "signature") && directive.value.empty()) {
        invalidTest("expected text, `!text` or `none`");
        continue;
      }

      if (kind == "hover") {
        const auto response =
            server.request("textDocument/hover", at(*marker), deadline());
        if (!response) {
          timeout("hover response");
          return false;
        }
        expectText("hover", hoverText(response->get("result")));
      } else if (kind == "signature") {
        const auto response = server.request("textDocument/signatureHelp",
                                             at(*marker), deadline());
        if (!response) {
          timeout("signature help");
          return false;
        }
        std::string text;
        if (const json::Object *help = response->getObject("result"))
          if (const json::Array *signatures = help->getArray("signatures"))
            for (const json::Value &value : *signatures)
              if (const json::Object *signature = value.getAsObject()) {
                text += signature->getString("label").value_or("").str();
                if (const json::Value *documentation =
                        signature->get("documentation"))
                  text += " " + hoverText(documentation);
                text += "\n";
              }
        expectText("signature help", text);
      } else if (kind == "definition" || kind == "references") {
        const bool references = kind == "references";
        json::Object params = at(*marker);
        if (references)
          params["context"] = json::Object{{"includeDeclaration", true}};
        const auto response = server.request(
            references ? "textDocument/references" : "textDocument/definition",
            std::move(params), deadline());
        if (!response) {
          timeout(kind + " response");
          return false;
        }
        compareLocations(directive.value, landings(response->get("result")));
      } else if (kind == "completion" || kind == "completion-detail") {
        if (kind == "completion-detail" &&
            (directive.words.size() != 3 || directive.value.empty())) {
          invalidTest("write `//@ completion-detail @marker label: text`");
          continue;
        }
        const auto response =
            server.request("textDocument/completion", at(*marker), deadline());
        if (!response) {
          timeout("completion response");
          return false;
        }
        std::map<std::string, std::string> details;
        const json::Value *value = response->get("result");
        const json::Array *items = value ? value->getAsArray() : nullptr;
        if (!items && value && value->getAsObject())
          items = value->getAsObject()->getArray("items");
        if (items)
          for (const json::Value &item : *items)
            if (const json::Object *object = item.getAsObject())
              details[object->getString("label").value_or("").str()] +=
                  object->getString("detail").value_or("").str();
        std::set<std::string> labels;
        for (const auto &[label, detail] : details)
          labels.insert(label);
        if (kind == "completion-detail") {
          const std::string &label = directive.words[2];
          const auto found = details.find(label);
          if (found == details.end())
            fail(where + "completion lacks '" + label +
                 "'; offered: " + shorten(joined(labels), 200));
          else if (found->second.find(directive.value) == std::string::npos)
            fail(where + "detail of '" + label + "' does not contain \"" +
                 directive.value + "\"; got: " + shorten(found->second));
          continue;
        }
        std::istringstream words(directive.value);
        for (std::string word; words >> word;) {
          const bool excluded = word.front() == '!';
          const std::string label = excluded ? word.substr(1) : word;
          if (labels.contains(label) == excluded)
            fail(where + "completion " + (excluded ? "offers" : "lacks") +
                 " '" + label + "'" +
                 (excluded ? std::string()
                           : "; offered: " + shorten(joined(labels), 200)));
        }
      } else if (kind == "token") {
        auto cached = tokenCache.find(marker->document);
        if (cached == tokenCache.end()) {
          auto tokens = server.request(
              "textDocument/semanticTokens/full",
              json::Object{{"textDocument", documentId(marker->document)}},
              deadline());
          if (!tokens) {
            timeout("semantic tokens");
            return false;
          }
          cached =
              tokenCache.emplace(marker->document, std::move(*tokens)).first;
        }
        std::optional<std::string> type;
        const json::Object *body = cached->second.getObject("result");
        const json::Array *data = body ? body->getArray("data") : nullptr;
        Position cursor;
        for (std::size_t index = 0; data && index + 4 < data->size() + 0;
             index += 5) {
          const int deltaLine =
              static_cast<int>((*data)[index].getAsInteger().value_or(0));
          const int deltaStart =
              static_cast<int>((*data)[index + 1].getAsInteger().value_or(0));
          const int length =
              static_cast<int>((*data)[index + 2].getAsInteger().value_or(0));
          const auto typeIndex = (*data)[index + 3].getAsInteger().value_or(0);
          cursor.character =
              deltaLine == 0 ? cursor.character + deltaStart : deltaStart;
          cursor.line += deltaLine;
          if (cursor.line == marker->position.line &&
              marker->position.character >= cursor.character &&
              marker->position.character < cursor.character + length) {
            type = typeIndex >= 0 && static_cast<std::size_t>(typeIndex) <
                                         server.tokenTypes.size()
                       ? server.tokenTypes[static_cast<std::size_t>(typeIndex)]
                       : std::to_string(typeIndex);
            break;
          }
        }
        const std::string actual = type.value_or("none");
        if (actual != directive.value)
          fail(where + "expected token " + directive.value + ", got " + actual);
      } else if (kind == "rename") {
        if (directive.words.size() != 3) {
          invalidTest("write `//@ rename @marker NewName: @a @b` or "
                      "`: error [text]`");
          continue;
        }
        json::Object params = at(*marker);
        params["newName"] = directive.words[2];
        const auto response = server.request("textDocument/rename",
                                             std::move(params), deadline());
        if (!response) {
          timeout("rename response");
          return false;
        }
        if (directive.value.rfind("error", 0) == 0) {
          const json::Object *error = response->getObject("error");
          const std::string expected =
              directive.value.size() > 5 ? directive.value.substr(6) : "";
          const std::string message =
              error ? error->getString("message").value_or("").str() : "";
          if (!error)
            fail(where + "expected an error; got " +
                 shorten(print(json::Value(json::Object(*response)))));
          else if (message.find(expected) == std::string::npos)
            fail(where + "error does not contain \"" + expected +
                 "\": " + message);
          continue;
        }
        if (const json::Object *error = response->getObject("error")) {
          fail(where + "rename failed: " +
               error->getString("message").value_or("").str());
          continue;
        }
        std::set<Place> expected;
        std::vector<LibraryPlace> library;
        if (!expectations(directive.value, expected, library))
          continue;
        std::set<Place> actual;
        std::set<std::string> elsewhere;
        std::map<std::string, json::Array> edits;
        std::map<std::string, std::int64_t> versions;
        if (const json::Object *edit = response->getObject("result"))
          workspaceEdits(*edit, edits, versions);
        for (const auto &[uri, list] : edits)
          for (const json::Value &item : list)
            if (const json::Object *object = item.getAsObject())
              if (const auto start = rangeStart(*object)) {
                const Landing landing = land(uri, *start);
                if (landing.place)
                  actual.insert(*landing.place);
                else
                  elsewhere.insert(landing.elsewhere.empty()
                                       ? "@std:" + landing.library->path
                                       : landing.elsewhere);
              }
        if (actual != expected || !elsewhere.empty())
          fail(where + "expected edits at " + placesText(expected) + "; got " +
               placesText(actual) +
               (elsewhere.empty() ? "" : " and " + joined(elsewhere)));
      } else if (kind == "code-action" || kind == "no-code-action") {
        // `"title"` (a substring of the action's title), then optionally
        // `=> step N` or `=> text`.
        std::string title;
        std::string expectation;
        std::string rest = directive.value;
        if (!rest.empty() && rest.front() == '"') {
          const std::size_t close = rest.find('"', 1);
          if (close == std::string::npos) {
            invalidTest("an unterminated \"title\"");
            continue;
          }
          title = rest.substr(1, close - 1);
          rest = rest.substr(close + 1);
        }
        const std::size_t first = rest.find_first_not_of(" \t");
        rest = first == std::string::npos ? "" : rest.substr(first);
        if (rest.rfind("=>", 0) == 0) {
          const std::size_t text = rest.find_first_not_of(" \t", 2);
          expectation = text == std::string::npos ? "" : rest.substr(text);
          if (expectation.empty()) {
            invalidTest("`=>` needs `step N` or text");
            continue;
          }
        } else if (!rest.empty()) {
          invalidTest("write `//@ code-action @marker: \"title\" [=> step N "
                      "| => text]`");
          continue;
        }
        if (kind == "code-action" && title.empty()) {
          invalidTest("write `//@ code-action @marker: \"title\"`");
          continue;
        }
        if (kind == "no-code-action" && !expectation.empty()) {
          invalidTest("no-code-action takes at most a \"title\"");
          continue;
        }
        Document &document = documents[marker->document];
        // The newest publication of this version, so the request names
        // the diagnostics the server holds now.
        if (const auto newer = server.latest([&](const json::Object &message) {
              return isPublication(message, document);
            }))
          if (const json::Object *params = newer->getObject("params"))
            if (const json::Array *diagnostics =
                    params->getArray("diagnostics"))
              document.published = *diagnostics;
        json::Array context;
        std::optional<Position> rangeStartAt;
        std::optional<Position> rangeEndAt;
        for (const json::Value &value : document.published) {
          const json::Object *diagnostic = value.getAsObject();
          const json::Object *range =
              diagnostic ? diagnostic->getObject("range") : nullptr;
          const auto start =
              range ? positionOf(range->get("start")) : std::nullopt;
          const auto end = range ? positionOf(range->get("end")) : std::nullopt;
          if (!start || !end || !contains(*start, *end, marker->position))
            continue;
          context.push_back(value);
          if (!rangeStartAt || *start < *rangeStartAt)
            rangeStartAt = start;
          if (!rangeEndAt || *rangeEndAt < *end)
            rangeEndAt = end;
        }
        if (context.empty()) {
          invalidTest("no published diagnostic covers " + directive.words[1] +
                      "; code actions answer diagnostics");
          continue;
        }
        const auto response = server.request(
            "textDocument/codeAction",
            json::Object{
                {"textDocument", documentId(marker->document)},
                {"range", json::Object{{"start", positionJson(*rangeStartAt)},
                                       {"end", positionJson(*rangeEndAt)}}},
                {"context", json::Object{{"diagnostics", std::move(context)},
                                         {"only", json::Array{"quickfix"}}}}},
            deadline());
        if (!response) {
          timeout("code actions");
          return false;
        }
        std::vector<const json::Object *> actions;
        std::set<std::string> titles;
        if (const json::Value *value = response->get("result"))
          if (const json::Array *array = value->getAsArray())
            for (const json::Value &item : *array)
              if (const json::Object *action = item.getAsObject()) {
                const std::string name =
                    action->getString("title").value_or("").str();
                titles.insert("\"" + name + "\"");
                if (name.find(title) != std::string::npos)
                  actions.push_back(action);
              }
        if (kind == "no-code-action") {
          if (!actions.empty())
            fail(where + "expected no code action" +
                 (title.empty() ? "" : " titled \"" + title + "\"") +
                 "; offered: " + joined(titles));
          continue;
        }
        if (actions.empty()) {
          fail(where + "no code action titled \"" + title +
               "\"; offered: " + joined(titles));
          continue;
        }
        const json::Object *edit = actions.front()->getObject("edit");
        std::map<std::string, json::Array> edits;
        std::map<std::string, std::int64_t> versions;
        if (edit)
          workspaceEdits(*edit, edits, versions);
        if (edits.empty()) {
          fail(where + "the code action carries no edit");
          continue;
        }
        std::map<std::size_t, std::string> edited;
        for (const auto &[uri, list] : edits) {
          const Landing landing = land(uri, Position{});
          if (!landing.place) {
            fail(where + "the code action edits " +
                 (landing.elsewhere.empty() ? "@std:" + landing.library->path
                                            : landing.elsewhere) +
                 ", outside the test");
            continue;
          }
          const Document &target = documents[landing.place->document];
          if (const auto version = versions.find(uri);
              version != versions.end() && version->second != target.version)
            fail(where + "the edit is for version " +
                 std::to_string(version->second) + " of " + target.name +
                 ", which is at version " + std::to_string(target.version));
          edited[landing.place->document] = applyEdits(target.text, list);
        }
        if (expectation.empty())
          continue;
        const std::string after = edited.contains(marker->document)
                                      ? edited[marker->document]
                                      : document.text;
        if (const std::optional<LspStep> step = lspStep(expectation)) {
          const auto found =
              std::find_if(steps.begin(), steps.end(), [&](const Step &s) {
                return s.number == step->number;
              });
          if (found == steps.end() || !step->file.empty()) {
            invalidTest("no `// --- " + expectation + "` section");
            continue;
          }
          const std::string want = asCode(found->text);
          const std::string got = asCode(after);
          if (want != got)
            fail(where + "the edited document differs from " + expectation +
                 " (-" + expectation + " +edited):\n" +
                 lineDiff(want, got, 20));
        } else if (after.find(expectation) == std::string::npos) {
          fail(where + "the edited document does not contain \"" + expectation +
               "\":\n" + shorten(after, 600));
        }
      } else if (kind == "formatting") {
        const auto response = server.request(
            "textDocument/formatting",
            json::Object{{"textDocument", documentId(home)},
                         {"options", json::Object{{"tabSize", 2},
                                                  {"insertSpaces", true}}}},
            deadline());
        if (!response) {
          timeout("formatting response");
          return false;
        }
        const std::string &text = documents[home].text;
        const json::Value *value = response->get("result");
        const std::string formatted =
            value && value->getAsArray()
                ? applyEdits(text, *value->getAsArray())
                : text;
        if (directive.value == "unchanged") {
          if (formatted != text)
            fail(where + "formatting changes the file (-before +after):\n" +
                 lineDiff(text, formatted, 20));
        } else if (formatted.find(directive.value) == std::string::npos) {
          fail(where + "formatted text does not contain \"" + directive.value +
               "\":\n" + shorten(formatted, 600));
        }
      } else if (kind == "symbols") {
        const auto response = server.request(
            "textDocument/documentSymbol",
            json::Object{{"textDocument", documentId(home)}}, deadline());
        if (!response) {
          timeout("document symbols");
          return false;
        }
        std::set<std::string> names;
        if (const json::Value *value = response->get("result"))
          if (const json::Array *symbols = value->getAsArray())
            collectSymbolNames(*symbols, names);
        std::istringstream words(directive.value);
        for (std::string word; words >> word;) {
          const bool excluded = word.front() == '!';
          const std::string name = excluded ? word.substr(1) : word;
          if (names.contains(name) == excluded)
            fail(where + "document symbols " + (excluded ? "include" : "lack") +
                 " '" + name + "'; symbols: " + shorten(joined(names), 200));
        }
      }
    }
    return true;
  };

  // Open every document, then wait for each one's diagnostics: every request
  // then sees analysed documents.
  for (const Document &document : documents)
    if (document.open)
      server.send(json::Object{
          {"jsonrpc", "2.0"},
          {"method", "textDocument/didOpen"},
          {"params", json::Object{{"textDocument",
                                   json::Object{{"uri", document.uri},
                                                {"languageId", "gti"},
                                                {"version", 1},
                                                {"text", document.text}}}}}});
  for (std::size_t document = 0; document < documents.size(); ++document) {
    if (!documents[document].open)
      continue;
    const auto published = publication(documents[document]);
    if (!published) {
      timeout("published diagnostics for " + documents[document].name);
      closeAll();
      return result;
    }
    record(document, document, *published);
  }
  // Sections and documents share indices up to the first step.
  if (ask(0))
    for (std::size_t index = 0; index < steps.size(); ++index) {
      Step &step = steps[index];
      Document &document = documents[step.document];
      document.text = step.text;
      document.marks = step.marks;
      document.version = step.number;
      server.send(json::Object{
          {"jsonrpc", "2.0"},
          {"method", "textDocument/didChange"},
          {"params",
           json::Object{
               {"textDocument", json::Object{{"uri", document.uri},
                                             {"version", document.version}}},
               {"contentChanges",
                json::Array{json::Object{{"text", document.text}}}}}}});
      const auto published = publication(document);
      if (!published) {
        timeout(test.sections[step.section].name + ": published diagnostics");
        break;
      }
      record(step.document, step.section, *published);
      if (!ask(index + 1))
        break;
    }
  closeAll();
  server.warm = true;
  return result;
}

namespace {

// Expected-message matching: objects match by subset, arrays element-wise
// with equal length, and the string "$ANY" matches any value.
bool matchesExpected(const json::Value &expected, const json::Value &actual) {
  if (const auto text = expected.getAsString(); text && *text == "$ANY")
    return true;
  if (const json::Object *object = expected.getAsObject()) {
    const json::Object *other = actual.getAsObject();
    if (!other)
      return false;
    for (const auto &[key, value] : *object) {
      const json::Value *found = other->get(key);
      if (!found || !matchesExpected(value, *found))
        return false;
    }
    return true;
  }
  if (const json::Array *array = expected.getAsArray()) {
    const json::Array *other = actual.getAsArray();
    if (!other || other->size() != array->size())
      return false;
    for (std::size_t index = 0; index < array->size(); ++index)
      if (!matchesExpected((*array)[index], (*other)[index]))
        return false;
    return true;
  }
  return expected == actual;
}

// `names` in order: a name that another starts with (`$DIR` of `$DIR_URI`)
// comes after it.
using Substitutions = std::vector<std::pair<std::string, std::string>>;

json::Value substitute(const json::Value &value, const Substitutions &names) {
  if (const auto text = value.getAsString()) {
    // One left-to-right pass: a replacement is never rescanned.
    const std::string source = text->str();
    std::string result;
    for (std::size_t at = 0; at < source.size();) {
      const auto name =
          std::find_if(names.begin(), names.end(), [&](const auto &entry) {
            return source.compare(at, entry.first.size(), entry.first) == 0;
          });
      if (name == names.end()) {
        result += source[at++];
        continue;
      }
      result += name->second;
      at += name->first.size();
    }
    return result;
  }
  if (const json::Object *object = value.getAsObject()) {
    json::Object result;
    for (const auto &[key, item] : *object)
      result[key] = substitute(item, names);
    return result;
  }
  if (const json::Array *array = value.getAsArray()) {
    json::Array result;
    for (const json::Value &item : *array)
      result.push_back(substitute(item, names));
    return result;
  }
  return value;
}

} // namespace

LspResult runLspTranscript(const fs::path &executable, const TestFile &test,
                           const fs::path &directory, double deadline,
                           double quiet) {
  LspResult result;
  const std::string text = readFile(test.path).value_or("");
  LspServer server(executable, directory / "gti_lsp.log", test.serverArguments);
  fs::create_directories(directory);
  const Substitutions names = {{"$DIR_URI", uriFor(directory)},
                               {"$DIR", directory.string()},
                               {"$URI", uriFor(directory / "main.gti")}};
  std::string error;
  if (!server.start(error)) {
    result.failure = error + "\n";
    return result;
  }
  std::vector<std::string> seen;
  // Once the server has published diagnostics (its standard library is
  // analysed), `quiet` seconds without any message means the awaited one is
  // not coming: fail then, naming it, instead of at the test's deadline.
  bool warm = false;
  double lastMessage = lspClock();
  std::istringstream lines(text);
  int number = 0;
  for (std::string line; std::getline(lines, line);) {
    ++number;
    if (line.empty() || line.rfind("//", 0) == 0)
      continue;
    const std::string where = "line " + std::to_string(number) + ": ";
    llvm::Expected<json::Value> parsed = json::parse(line);
    if (!parsed) {
      result.failure +=
          "invalid test: " + where + llvm::toString(parsed.takeError()) + "\n";
      break;
    }
    const json::Object *step = parsed->getAsObject();
    if (!step || step->size() != 1) {
      result.failure += "invalid test: " + where +
                        "each line is one of {\"file\":…}, {\"send\":…}, "
                        "{\"expect\":…}, {\"exit\":N}\n";
      break;
    }
    if (const json::Object *file = step->getObject("file")) {
      const fs::path path =
          directory / file->getString("name").value_or("main.gti").str();
      fs::create_directories(path.parent_path());
      writeFile(path, file->getString("text").value_or("").str());
    } else if (const json::Value *message = step->get("send")) {
      server.send(substitute(*message, names));
    } else if (const json::Value *expected = step->get("expect")) {
      const json::Value want = substitute(*expected, names);
      bool matched = false;
      bool answered = false;
      // The message the expectation is about: the response to its id, or the
      // notification with its method, uri and version. Once that arrives
      // and differs, waiting longer cannot help.
      const auto identifies = [&](const json::Value &message) {
        const json::Object *expected = want.getAsObject();
        const json::Object *actual = message.getAsObject();
        if (!expected || !actual)
          return false;
        if (const json::Value *id = expected->get("id"))
          return !actual->get("method") && actual->get("id") &&
                 *actual->get("id") == *id;
        const auto method = expected->getString("method");
        const json::Object *params = expected->getObject("params");
        if (!method || !params || !params->get("version") ||
            actual->getString("method") != method)
          return false;
        const json::Object *other = actual->getObject("params");
        return other && other->get("uri") && params->get("uri") &&
               *other->get("uri") == *params->get("uri") &&
               other->get("version") &&
               *other->get("version") == *params->get("version");
      };
      bool silent = false;
      bool idle = false;
      // CPU the server used by the start of the current silent second.
      double idleSince = lspClock();
      double idleCpu = server.cpuSeconds();
      while (!matched && !answered) {
        const double wait = std::min(
            {warm ? lastMessage + quiet : deadline, deadline, lspClock() + 1});
        std::optional<json::Value> message = server.receive(wait);
        if (!message) {
          if (!server.running() || lspClock() >= deadline)
            break;
          if (warm && lspClock() >= lastMessage + quiet) {
            silent = true;
            break;
          }
          // Silent and idle for a second: the server is waiting for us, so
          // the awaited message is not coming (warm or not).
          const double now = lspClock();
          const double cpu = server.cpuSeconds();
          if (cpu >= 0 && idleCpu >= 0 && now - idleSince >= 1.0 &&
              now - lastMessage >= 1.0) {
            if (cpu - idleCpu < 0.02) {
              idle = true;
              break;
            }
            idleSince = now;
            idleCpu = cpu;
          }
          continue;
        }
        lastMessage = lspClock();
        idleSince = lastMessage;
        idleCpu = server.cpuSeconds();
        if (const json::Object *object = message->getAsObject())
          warm = warm || object->getString("method") ==
                             "textDocument/publishDiagnostics";
        matched = matchesExpected(want, *message);
        if (!matched)
          seen.push_back(shorten(print(*message), 200));
        answered = !matched && identifies(*message);
      }
      if (!matched) {
        result.timedOut = !answered && !silent && !idle && server.running();
        result.failure +=
            where +
            (silent ? "no message for " + std::to_string(quiet).substr(0, 4) +
                          " s while awaiting one matching "
             : idle ? std::string("gti_lsp fell idle (no message and no CPU "
                                  "for 1 s) while awaiting one matching ")
                    : std::string("expected a message matching ")) +
            shorten(print(want)) + "\n";
        const std::size_t first = seen.size() > 5 ? seen.size() - 5 : 0;
        for (std::size_t index = first; index < seen.size(); ++index)
          result.failure += "  seen: " + seen[index] + "\n";
        if (!server.running())
          result.failure += "gti_lsp exited:\n" + server.logTail();
        break;
      }
      seen.clear();
    } else if (const auto code = step->getInteger("exit")) {
      // Wait for the server to exit with this status.
      while (server.running() && lspClock() < deadline)
        (void)server.receive(deadline);
      if (server.running()) {
        result.timedOut = true;
        result.failure += where + "gti_lsp did not exit\n";
      } else if (server.exitCode != *code) {
        result.failure += where + "gti_lsp exited with " +
                          std::to_string(server.exitCode) + ", expected " +
                          std::to_string(*code) + "\n";
      }
    } else {
      result.failure += "invalid test: " + where + "unknown step\n";
      break;
    }
  }
  if (const double cpu = server.cpuSeconds(); cpu >= 0)
    result.serverCpu = cpu;
  server.stop();
  return result;
}

} // namespace gti_test
