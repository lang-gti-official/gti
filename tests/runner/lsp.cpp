#include "lsp.h"
#include "snapshot.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
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

LspServer::LspServer(fs::path executable, fs::path log)
    : executable(std::move(executable)), log(std::move(log)) {}

LspServer::~LspServer() { stop(); }

bool LspServer::start(std::string &error) {
  int down[2];
  int up[2];
  if (pipe(down) != 0 || pipe(up) != 0) {
    error = "pipe failed";
    return false;
  }
  const pid_t child = fork();
  if (child == 0) {
    dup2(down[0], 0);
    dup2(up[1], 1);
    const int logFd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logFd >= 0)
      dup2(logFd, 2);
    close(down[1]);
    close(up[0]);
    const std::string program = executable.string();
    char *argv[] = {const_cast<char *>(program.c_str()), nullptr};
    execv(argv[0], argv);
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
  json::Object capabilities{
      {"textDocument",
       json::Object{
           {"publishDiagnostics", json::Object{{"relatedInformation", true}}},
           {"documentSymbol",
            json::Object{{"hierarchicalDocumentSymbolSupport", true}}},
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
    if (remaining <= 0)
      return std::nullopt;
    pollfd fd{output, POLLIN, 0};
    const int ready =
        poll(&fd, 1, static_cast<int>(std::min(remaining, 1.0) * 1000) + 1);
    if (ready < 0 && errno != EINTR)
      return std::nullopt;
    if (ready <= 0)
      continue;
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

std::string LspServer::logTail() const {
  const std::string text = readFile(log).value_or("");
  return text.size() > 1500 ? "..." + text.substr(text.size() - 1500) : text;
}

LspResult runLspTest(LspServer &server, const TestFile &test,
                     const fs::path &directory, double deadline) {
  LspResult result;
  const std::string text = test.sections.front().text;
  std::vector<std::string> errors;
  const std::map<std::string, Position> marks = markers(text, errors);
  const auto fail = [&](const std::string &line) {
    result.failure += line + "\n";
  };
  for (const std::string &error : errors)
    fail("invalid test: " + error);
  if (!result.failure.empty())
    return result;

  const fs::path file = directory / test.sections.front().name;
  fs::create_directories(directory);
  writeFile(file, text);
  const std::string uri = uriFor(file);
  const auto document = [&] { return json::Object{{"uri", uri}}; };
  server.send(json::Object{
      {"jsonrpc", "2.0"},
      {"method", "textDocument/didOpen"},
      {"params",
       json::Object{{"textDocument", json::Object{{"uri", uri},
                                                  {"languageId", "gti"},
                                                  {"version", 1},
                                                  {"text", text}}}}}});
  const auto close = [&] {
    server.send(
        json::Object{{"jsonrpc", "2.0"},
                     {"method", "textDocument/didClose"},
                     {"params", json::Object{{"textDocument", document()}}}});
  };
  const auto timeout = [&](const std::string &what) {
    result.crashed = !server.running();
    result.timedOut = !result.crashed;
    fail("no " + what + " before the time limit" +
         (server.running() ? "" : "; gti_lsp exited:\n" + server.logTail()));
  };

  // Diagnostics first: every request then sees the analysed document.
  const auto published = server.waitFor(
      [&](const json::Object &message) {
        const json::Object *params = message.getObject("params");
        return message.getString("method") ==
                   "textDocument/publishDiagnostics" &&
               params && params->getString("uri") == uri &&
               params->getInteger("version") == 1;
      },
      deadline);
  if (!published) {
    timeout("published diagnostics");
    close();
    return result;
  }
  result.published = true;
  if (const json::Object *params = published->getObject("params"))
    if (const json::Array *diagnostics = params->getArray("diagnostics"))
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
        located.section = test.sections.front().name;
        located.line = start ? start->line + 1 : 0;
        result.diagnostics.push_back(std::move(located));
      }

  std::optional<json::Object> tokens;
  const auto at = [&](const Position &position) {
    return json::Object{{"textDocument", document()},
                        {"position", positionJson(position)}};
  };
  for (const LspDirective &directive : test.lspDirectives) {
    const std::string where = "line " + std::to_string(directive.writtenLine) +
                              " (" + directive.words.front() + "): ";
    const std::string &kind = directive.words.front();
    const bool needsMarker = kind != "formatting" && kind != "symbols";
    std::optional<Position> marker;
    if (needsMarker) {
      if (directive.words.size() < 2 || directive.words[1].front() != '@') {
        fail("invalid test: " + where + "write `//@ " + kind +
             " @marker: ...`");
        continue;
      }
      const auto found = marks.find(directive.words[1].substr(1));
      if (found == marks.end()) {
        fail("invalid test: " + where + "no marker /*" + directive.words[1] +
             "*/");
        continue;
      }
      marker = found->second;
    }
    // `@a @b` in the expected value -> positions.
    const auto markerSet = [&](const std::string &value,
                               std::set<Position> &positions) {
      std::istringstream words(value);
      for (std::string word; words >> word;) {
        const auto found =
            word.front() == '@' ? marks.find(word.substr(1)) : marks.end();
        if (found == marks.end()) {
          fail("invalid test: " + where + "unknown marker '" + word + "'");
          return false;
        }
        positions.insert(found->second);
      }
      return true;
    };
    const auto positionsText = [&](const std::set<Position> &positions) {
      std::string text;
      for (const Position &position : positions) {
        std::string name;
        for (const auto &[markerName, markerPosition] : marks)
          if (markerPosition == position)
            name = "@" + markerName;
        text += (text.empty() ? "" : " ") +
                (name.empty() ? describe(position) : name);
      }
      return text.empty() ? "(none)" : text;
    };

    if (kind == "hover") {
      const auto response =
          server.request("textDocument/hover", at(*marker), deadline);
      if (!response) {
        timeout("hover response");
        break;
      }
      const std::string text = hoverText(response->get("result"));
      if (directive.value == "none") {
        if (!text.empty() && text != "null")
          fail(where + "expected no hover, got: " + shorten(text));
      } else if (text.find(directive.value) == std::string::npos) {
        fail(where + "hover does not contain \"" + directive.value +
             "\"; got: " + shorten(text));
      }
    } else if (kind == "definition" || kind == "references") {
      const bool references = kind == "references";
      json::Object params = at(*marker);
      if (references)
        params["context"] = json::Object{{"includeDeclaration", true}};
      const auto response = server.request(
          references ? "textDocument/references" : "textDocument/definition",
          std::move(params), deadline);
      if (!response) {
        timeout(kind + " response");
        break;
      }
      std::set<Position> expected;
      if (directive.value != "none" && !markerSet(directive.value, expected))
        continue;
      std::set<Position> actual;
      std::set<std::string> elsewhere;
      const json::Value *value = response->get("result");
      const auto add = [&](const json::Value &item) {
        const json::Object *location = item.getAsObject();
        if (!location)
          return;
        const auto target = location->getString("uri")
                                ? location->getString("uri")
                                : location->getString("targetUri");
        const auto start = rangeStart(*location);
        if (target && *target == uri && start)
          actual.insert(*start);
        else if (target)
          elsewhere.insert(target->str());
      };
      if (value && value->getAsArray())
        for (const json::Value &item : *value->getAsArray())
          add(item);
      else if (value && value->getAsObject())
        add(*value);
      if (actual != expected || !elsewhere.empty())
        fail(where + "expected " + positionsText(expected) + "; got " +
             positionsText(actual) +
             (elsewhere.empty() ? "" : " and " + joined(elsewhere)));
    } else if (kind == "completion") {
      const auto response =
          server.request("textDocument/completion", at(*marker), deadline);
      if (!response) {
        timeout("completion response");
        break;
      }
      std::set<std::string> labels;
      const json::Value *value = response->get("result");
      const json::Array *items = value ? value->getAsArray() : nullptr;
      if (!items && value && value->getAsObject())
        items = value->getAsObject()->getArray("items");
      if (items)
        for (const json::Value &item : *items)
          if (const json::Object *object = item.getAsObject())
            labels.insert(object->getString("label").value_or("").str());
      std::istringstream words(directive.value);
      for (std::string word; words >> word;) {
        const bool excluded = word.front() == '!';
        const std::string label = excluded ? word.substr(1) : word;
        if (labels.contains(label) == excluded)
          fail(where + "completion " + (excluded ? "offers" : "lacks") + " '" +
               label + "'" +
               (excluded ? std::string()
                         : "; offered: " + shorten(joined(labels), 200)));
      }
    } else if (kind == "token") {
      if (!tokens) {
        tokens = server.request("textDocument/semanticTokens/full",
                                json::Object{{"textDocument", document()}},
                                deadline);
        if (!tokens) {
          timeout("semantic tokens");
          break;
        }
      }
      std::optional<std::string> type;
      const json::Object *body = tokens->getObject("result");
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
        if (cursor.line == marker->line &&
            marker->character >= cursor.character &&
            marker->character < cursor.character + length) {
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
        fail("invalid test: " + where +
             "write `//@ rename @marker NewName: "
             "@a @b` or `: error [text]`");
        continue;
      }
      json::Object params = at(*marker);
      params["newName"] = directive.words[2];
      const auto response =
          server.request("textDocument/rename", std::move(params), deadline);
      if (!response) {
        timeout("rename response");
        break;
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
      std::set<Position> expected;
      if (!markerSet(directive.value, expected))
        continue;
      std::set<Position> actual;
      if (const json::Object *edit = response->getObject("result")) {
        if (const json::Object *changes = edit->getObject("changes"))
          if (const json::Array *edits = changes->getArray(uri))
            for (const json::Value &item : *edits)
              if (const json::Object *object = item.getAsObject())
                if (const auto start = rangeStart(*object))
                  actual.insert(*start);
        if (const json::Array *changes = edit->getArray("documentChanges"))
          for (const json::Value &change : *changes)
            if (const json::Object *object = change.getAsObject())
              if (const json::Array *edits = object->getArray("edits"))
                for (const json::Value &item : *edits)
                  if (const json::Object *e = item.getAsObject())
                    if (const auto start = rangeStart(*e))
                      actual.insert(*start);
      } else if (const json::Object *error = response->getObject("error")) {
        fail(where + "rename failed: " +
             error->getString("message").value_or("").str());
        continue;
      }
      if (actual != expected)
        fail(where + "expected edits at " + positionsText(expected) + "; got " +
             positionsText(actual));
    } else if (kind == "formatting") {
      const auto response = server.request(
          "textDocument/formatting",
          json::Object{{"textDocument", document()},
                       {"options",
                        json::Object{{"tabSize", 2}, {"insertSpaces", true}}}},
          deadline);
      if (!response) {
        timeout("formatting response");
        break;
      }
      const json::Value *value = response->get("result");
      const std::string formatted = value && value->getAsArray()
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
      const auto response =
          server.request("textDocument/documentSymbol",
                         json::Object{{"textDocument", document()}}, deadline);
      if (!response) {
        timeout("document symbols");
        break;
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
  close();
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

json::Value substitute(const json::Value &value,
                       const std::map<std::string, std::string> &names) {
  if (const auto text = value.getAsString()) {
    std::string result = text->str();
    for (const auto &[name, replacement] : names)
      for (std::size_t at = 0;
           (at = result.find(name, at)) != std::string::npos;
           at += replacement.size())
        result.replace(at, name.size(), replacement);
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
                           const fs::path &directory, double deadline) {
  LspResult result;
  const std::string text = readFile(test.path).value_or("");
  LspServer server(executable, directory / "gti_lsp.log");
  fs::create_directories(directory);
  const std::map<std::string, std::string> names = {
      {"$URI", uriFor(directory / "main.gti")},
      {"$DIR_URI", uriFor(directory)},
      {"$DIR", directory.string()}};
  std::string error;
  if (!server.start(error)) {
    result.failure = error + "\n";
    return result;
  }
  std::vector<std::string> seen;
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
      while (!matched && !answered) {
        std::optional<json::Value> message = server.receive(deadline);
        if (!message)
          break;
        matched = matchesExpected(want, *message);
        if (!matched)
          seen.push_back(shorten(print(*message), 200));
        answered = !matched && identifies(*message);
      }
      if (!matched) {
        result.timedOut = !answered && server.running();
        result.failure += where + "expected a message matching " +
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
  server.stop();
  return result;
}

} // namespace gti_test
