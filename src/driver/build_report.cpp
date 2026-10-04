#include "gti/driver/build_report.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace lang::driver {

namespace {

std::string jsonEscape(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20U) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<int>(character) << std::dec;
      } else {
        output << static_cast<char>(character);
      }
      break;
    }
  }
  return output.str();
}

void writeJsonString(std::ostream &stream, std::string_view value) {
  stream << '"' << jsonEscape(value) << '"';
}

void writeOptionalJsonString(std::ostream &stream,
                             const std::optional<std::string> &value) {
  if (value) {
    writeJsonString(stream, *value);
  } else {
    stream << "null";
  }
}

void writeBuildFileJson(std::ostream &stream,
                        const BuildFileProvenance &provenance) {
  stream << "{\"canonical_path\":";
  writeJsonString(stream, provenance.canonicalPath.string());
  stream << ",\"bytes\":" << provenance.bytes << ",\"sha256\":";
  writeJsonString(stream, provenance.sha256);
  stream << '}';
}

std::string_view sourceRoleName(SourceUnitRole role) {
  switch (role) {
  case SourceUnitRole::Application:
    return "application";
  case SourceUnitRole::Prelude:
    return "prelude";
  case SourceUnitRole::StandardLibrary:
    return "standard-library";
  }
  return "application";
}

std::optional<SourceUnitRole> parseSourceRoleName(std::string_view name) {
  if (name == "application") {
    return SourceUnitRole::Application;
  }
  if (name == "prelude") {
    return SourceUnitRole::Prelude;
  }
  if (name == "standard-library") {
    return SourceUnitRole::StandardLibrary;
  }
  return std::nullopt;
}

std::string_view sourceDependencyKindName(SourceDependencyKind kind) {
  switch (kind) {
  case SourceDependencyKind::Include:
    return "include";
  case SourceDependencyKind::Prelude:
    return "prelude";
  case SourceDependencyKind::StandardLibrary:
    return "standard-library";
  case SourceDependencyKind::Package:
    return "package";
  case SourceDependencyKind::Import:
    return "import";
  case SourceDependencyKind::ExportImport:
    return "export-import";
  }
  return "include";
}

std::optional<SourceDependencyKind>
parseSourceDependencyKindName(std::string_view name) {
  if (name == "include") {
    return SourceDependencyKind::Include;
  }
  if (name == "prelude") {
    return SourceDependencyKind::Prelude;
  }
  if (name == "standard-library") {
    return SourceDependencyKind::StandardLibrary;
  }
  if (name == "package") {
    return SourceDependencyKind::Package;
  }
  if (name == "import") {
    return SourceDependencyKind::Import;
  }
  if (name == "export-import") {
    return SourceDependencyKind::ExportImport;
  }
  return std::nullopt;
}

std::optional<CompilerSourceState>
parseCompilerSourceStateName(std::string_view name) {
  if (name == "clean") {
    return CompilerSourceState::Clean;
  }
  if (name == "dirty") {
    return CompilerSourceState::Dirty;
  }
  if (name == "unavailable") {
    return CompilerSourceState::Unavailable;
  }
  return std::nullopt;
}

std::optional<BuildRuntimeHeaderKind>
parseRuntimeHeaderKindName(std::string_view name) {
  if (name == "runtime") {
    return BuildRuntimeHeaderKind::Runtime;
  }
  if (name == "compatibility") {
    return BuildRuntimeHeaderKind::Compatibility;
  }
  return std::nullopt;
}

// A deliberately small reader for the exact shape this driver writes. It is
// strict rather than permissive: trailing content, unterminated strings, and
// unknown escapes are errors, and nesting is bounded so a hostile sidecar
// cannot exhaust the stack.
class JsonValue final {
public:
  enum class Kind {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
  };

  Kind kind = Kind::Null;
  bool boolean = false;
  // Numbers keep their literal spelling; sizes are converted exactly rather
  // than through a floating-point round trip.
  std::string text;
  // Object members store their name on the child value. Keeping object and
  // array children in the same recursive vector avoids instantiating
  // pair<string, JsonValue> while JsonValue is incomplete, which is rejected
  // by some standard-library implementations.
  std::string memberName;
  std::vector<JsonValue> elements;

  [[nodiscard]] const JsonValue *member(std::string_view name) const {
    if (kind != Kind::Object) {
      return nullptr;
    }
    for (const JsonValue &entry : elements) {
      if (entry.memberName == name) {
        return &entry;
      }
    }
    return nullptr;
  }
};

constexpr std::size_t maximumJsonDepth = 64;

class JsonParser final {
public:
  explicit JsonParser(std::string_view input) : text(input) {}

  [[nodiscard]] std::optional<JsonValue> parseDocument(std::string &error) {
    std::optional<JsonValue> value = parseValue(0, error);
    if (!value) {
      return std::nullopt;
    }
    skipWhitespace();
    if (cursor != text.size()) {
      error = "unexpected trailing content at offset " + std::to_string(cursor);
      return std::nullopt;
    }
    return value;
  }

private:
  void skipWhitespace() {
    while (cursor < text.size() &&
           (text[cursor] == ' ' || text[cursor] == '\t' ||
            text[cursor] == '\n' || text[cursor] == '\r')) {
      ++cursor;
    }
  }

  [[nodiscard]] bool consume(char expected) {
    skipWhitespace();
    if (cursor < text.size() && text[cursor] == expected) {
      ++cursor;
      return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<JsonValue> parseValue(std::size_t depth,
                                                    std::string &error) {
    if (depth > maximumJsonDepth) {
      error = "nesting exceeds the supported depth";
      return std::nullopt;
    }
    skipWhitespace();
    if (cursor >= text.size()) {
      error = "unexpected end of report";
      return std::nullopt;
    }
    const char character = text[cursor];
    if (character == '{') {
      return parseObject(depth, error);
    }
    if (character == '[') {
      return parseArray(depth, error);
    }
    if (character == '"') {
      std::string value;
      if (!parseString(value, error)) {
        return std::nullopt;
      }
      JsonValue result;
      result.kind = JsonValue::Kind::String;
      result.text = std::move(value);
      return result;
    }
    if (text.compare(cursor, 4, "true") == 0) {
      cursor += 4;
      JsonValue result;
      result.kind = JsonValue::Kind::Boolean;
      result.boolean = true;
      return result;
    }
    if (text.compare(cursor, 5, "false") == 0) {
      cursor += 5;
      JsonValue result;
      result.kind = JsonValue::Kind::Boolean;
      result.boolean = false;
      return result;
    }
    if (text.compare(cursor, 4, "null") == 0) {
      cursor += 4;
      return JsonValue{};
    }
    return parseNumber(error);
  }

  [[nodiscard]] std::optional<JsonValue> parseObject(std::size_t depth,
                                                     std::string &error) {
    ++cursor;
    JsonValue result;
    result.kind = JsonValue::Kind::Object;
    skipWhitespace();
    if (consume('}')) {
      return result;
    }
    while (true) {
      skipWhitespace();
      std::string name;
      if (!parseString(name, error)) {
        return std::nullopt;
      }
      if (std::any_of(result.elements.begin(), result.elements.end(),
                      [&name](const JsonValue &member) {
                        return member.memberName == name;
                      })) {
        error = "duplicate object member '" + name + "'";
        return std::nullopt;
      }
      if (!consume(':')) {
        error = "expected ':' at offset " + std::to_string(cursor);
        return std::nullopt;
      }
      std::optional<JsonValue> value = parseValue(depth + 1, error);
      if (!value) {
        return std::nullopt;
      }
      value->memberName = std::move(name);
      result.elements.push_back(std::move(*value));
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return result;
      }
      error = "expected ',' or '}' at offset " + std::to_string(cursor);
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<JsonValue> parseArray(std::size_t depth,
                                                    std::string &error) {
    ++cursor;
    JsonValue result;
    result.kind = JsonValue::Kind::Array;
    skipWhitespace();
    if (consume(']')) {
      return result;
    }
    while (true) {
      std::optional<JsonValue> value = parseValue(depth + 1, error);
      if (!value) {
        return std::nullopt;
      }
      result.elements.push_back(std::move(*value));
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return result;
      }
      error = "expected ',' or ']' at offset " + std::to_string(cursor);
      return std::nullopt;
    }
  }

  [[nodiscard]] bool parseString(std::string &value, std::string &error) {
    skipWhitespace();
    if (cursor >= text.size() || text[cursor] != '"') {
      error = "expected a string at offset " + std::to_string(cursor);
      return false;
    }
    ++cursor;
    value.clear();
    while (cursor < text.size()) {
      const char character = text[cursor++];
      if (character == '"') {
        return true;
      }
      if (character != '\\') {
        if (static_cast<unsigned char>(character) < 0x20U) {
          error = "unescaped control character in string";
          return false;
        }
        value.push_back(character);
        continue;
      }
      if (cursor >= text.size()) {
        break;
      }
      const char escape = text[cursor++];
      switch (escape) {
      case '"':
        value.push_back('"');
        break;
      case '\\':
        value.push_back('\\');
        break;
      case '/':
        value.push_back('/');
        break;
      case 'b':
        value.push_back('\b');
        break;
      case 'f':
        value.push_back('\f');
        break;
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      case 'u': {
        if (cursor + 4 > text.size()) {
          error = "truncated \\u escape";
          return false;
        }
        unsigned int code = 0;
        const char *begin = text.data() + cursor;
        const auto [end, parseError] =
            std::from_chars(begin, begin + 4, code, 16);
        if (parseError != std::errc{} || end != begin + 4) {
          error = "malformed \\u escape";
          return false;
        }
        cursor += 4;
        // The writer only escapes C0 control characters this way; anything
        // else would need full UTF-16 surrogate handling that this format
        // never produces.
        if (code > 0x7fU) {
          error = "unsupported non-ASCII \\u escape";
          return false;
        }
        value.push_back(static_cast<char>(code));
        break;
      }
      default:
        error = "unsupported string escape";
        return false;
      }
    }
    error = "unterminated string";
    return false;
  }

  [[nodiscard]] std::optional<JsonValue> parseNumber(std::string &error) {
    const std::size_t begin = cursor;
    if (cursor < text.size() && (text[cursor] == '-' || text[cursor] == '+')) {
      ++cursor;
    }
    while (cursor < text.size() &&
           ((text[cursor] >= '0' && text[cursor] <= '9') ||
            text[cursor] == '.' || text[cursor] == 'e' || text[cursor] == 'E' ||
            text[cursor] == '-' || text[cursor] == '+')) {
      ++cursor;
    }
    if (cursor == begin) {
      error = "expected a value at offset " + std::to_string(begin);
      return std::nullopt;
    }
    JsonValue result;
    result.kind = JsonValue::Kind::Number;
    result.text = std::string(text.substr(begin, cursor - begin));
    return result;
  }

  std::string_view text;
  std::size_t cursor = 0;
};

bool requireMember(const JsonValue &object, std::string_view name,
                   JsonValue::Kind kind, const std::string &path,
                   const JsonValue *&found, std::string &error) {
  found = object.member(name);
  if (found == nullptr) {
    error = "missing '" + path + "." + std::string(name) + "'";
    return false;
  }
  if (found->kind != kind) {
    error = "'" + path + "." + std::string(name) + "' has the wrong type";
    return false;
  }
  return true;
}

bool requireString(const JsonValue &object, std::string_view name,
                   const std::string &path, std::string &value,
                   std::string &error) {
  const JsonValue *found = nullptr;
  if (!requireMember(object, name, JsonValue::Kind::String, path, found,
                     error)) {
    return false;
  }
  value = found->text;
  return true;
}

bool requireOptionalString(const JsonValue &object, std::string_view name,
                           const std::string &path,
                           std::optional<std::string> &value,
                           std::string &error) {
  const JsonValue *found = object.member(name);
  if (found == nullptr) {
    error = "missing '" + path + "." + std::string(name) + "'";
    return false;
  }
  if (found->kind == JsonValue::Kind::Null) {
    value = std::nullopt;
    return true;
  }
  if (found->kind != JsonValue::Kind::String) {
    error = "'" + path + "." + std::string(name) + "' has the wrong type";
    return false;
  }
  value = found->text;
  return true;
}

bool requireBoolean(const JsonValue &object, std::string_view name,
                    const std::string &path, bool &value, std::string &error) {
  const JsonValue *found = nullptr;
  if (!requireMember(object, name, JsonValue::Kind::Boolean, path, found,
                     error)) {
    return false;
  }
  value = found->boolean;
  return true;
}

bool parseUnsigned(const JsonValue &value, const std::string &path,
                   std::uint64_t &parsed, std::string &error) {
  if (value.kind != JsonValue::Kind::Number) {
    error = "'" + path + "' is not a number";
    return false;
  }
  const char *begin = value.text.data();
  const char *end = begin + value.text.size();
  const auto [stopped, parseError] = std::from_chars(begin, end, parsed);
  if (parseError != std::errc{} || stopped != end) {
    error = "'" + path + "' is not a whole number";
    return false;
  }
  return true;
}

bool requireUnsigned(const JsonValue &object, std::string_view name,
                     const std::string &path, std::uint64_t &value,
                     std::string &error) {
  const JsonValue *found = object.member(name);
  if (found == nullptr) {
    error = "missing '" + path + "." + std::string(name) + "'";
    return false;
  }
  return parseUnsigned(*found, path + "." + std::string(name), value, error);
}

bool parseBuildFile(const JsonValue &value, const std::string &path,
                    BuildFileProvenance &file, std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "'" + path + "' is not an object";
    return false;
  }
  std::string canonicalPath;
  std::uint64_t bytes = 0;
  if (!requireString(value, "canonical_path", path, canonicalPath, error) ||
      !requireUnsigned(value, "bytes", path, bytes, error) ||
      !requireString(value, "sha256", path, file.sha256, error)) {
    return false;
  }
  file.canonicalPath = std::filesystem::path(canonicalPath);
  file.bytes = static_cast<std::uintmax_t>(bytes);
  return true;
}

bool requireBuildFile(const JsonValue &object, std::string_view name,
                      const std::string &path, BuildFileProvenance &file,
                      std::string &error) {
  const JsonValue *found = object.member(name);
  if (found == nullptr) {
    error = "missing '" + path + "." + std::string(name) + "'";
    return false;
  }
  return parseBuildFile(*found, path + "." + std::string(name), file, error);
}

bool requireIdArray(const JsonValue &object, std::string_view name,
                    const std::string &path, std::vector<SourceUnitId> &values,
                    std::string &error) {
  const JsonValue *found = nullptr;
  if (!requireMember(object, name, JsonValue::Kind::Array, path, found,
                     error)) {
    return false;
  }
  for (std::size_t index = 0; index < found->elements.size(); ++index) {
    std::uint64_t value = 0;
    if (!parseUnsigned(found->elements[index],
                       path + "." + std::string(name) + "[" +
                           std::to_string(index) + "]",
                       value, error)) {
      return false;
    }
    values.push_back(static_cast<SourceUnitId>(value));
  }
  return true;
}

std::string describeFile(const BuildFileProvenance &file) {
  return file.sha256 + " (" + std::to_string(file.bytes) + " bytes)";
}

void compareFile(std::vector<BuildProvenanceDifference> &differences,
                 std::string subject, const BuildFileProvenance &recorded,
                 const BuildFileProvenance &observed) {
  if (recorded.sha256 != observed.sha256 || recorded.bytes != observed.bytes) {
    differences.push_back({.subject = std::move(subject),
                           .field = "content",
                           .expected = describeFile(recorded),
                           .actual = describeFile(observed)});
  }
}

void compareValue(std::vector<BuildProvenanceDifference> &differences,
                  std::string subject, std::string field,
                  const std::string &recorded, const std::string &observed) {
  if (recorded != observed) {
    differences.push_back({.subject = std::move(subject),
                           .field = std::move(field),
                           .expected = recorded,
                           .actual = observed});
  }
}

std::string unitKey(const BuildSourceUnitProvenance &unit) {
  return std::string(sourceRoleName(unit.role)) + " " +
         unit.logicalIdentity.value_or(unit.material.canonicalPath.string());
}

std::string headerKey(const BuildRuntimeHeaderProvenance &header) {
  return std::string(buildRuntimeHeaderKindName(header.kind)) + " " +
         header.logicalIdentity;
}

std::string describeProject(const BuildProjectProvenance &project) {
  return project.packageName + "@" + project.packageVersion + " " +
         project.targetName + " [" + project.profileName + ", " +
         project.targetTriple + "]";
}

void compareProject(std::vector<BuildProvenanceDifference> &differences,
                    const std::optional<BuildProjectProvenance> &recorded,
                    const std::optional<BuildProjectProvenance> &observed) {
  if (recorded.has_value() != observed.has_value()) {
    differences.push_back(
        {.subject = "project",
         .field = "selection",
         .expected = recorded ? describeProject(*recorded) : "<direct build>",
         .actual = observed ? describeProject(*observed) : "<direct build>"});
    return;
  }
  if (!recorded) {
    return;
  }
  compareValue(differences, "project", "package name", recorded->packageName,
               observed->packageName);
  compareValue(differences, "project", "package version",
               recorded->packageVersion, observed->packageVersion);
  compareValue(differences, "project", "target name", recorded->targetName,
               observed->targetName);
  compareValue(differences, "project", "target kind", recorded->targetKind,
               observed->targetKind);
  compareValue(differences, "project", "profile name", recorded->profileName,
               observed->profileName);
  compareValue(differences, "project", "target triple", recorded->targetTriple,
               observed->targetTriple);
  compareValue(differences, "project", "model identity",
               recorded->modelIdentity, observed->modelIdentity);
}

void compareSourceUnits(std::vector<BuildProvenanceDifference> &differences,
                        const BuildSourceProvenance &recorded,
                        const BuildSourceProvenance &observed) {
  std::map<std::string, const BuildSourceUnitProvenance *> observedUnits;
  for (const BuildSourceUnitProvenance &unit : observed.units) {
    observedUnits.emplace(unitKey(unit), &unit);
  }
  std::map<std::string, const BuildSourceUnitProvenance *> recordedUnits;
  for (const BuildSourceUnitProvenance &unit : recorded.units) {
    recordedUnits.emplace(unitKey(unit), &unit);
  }
  for (const auto &[key, unit] : recordedUnits) {
    const auto match = observedUnits.find(key);
    if (match == observedUnits.end()) {
      differences.push_back({.subject = "source unit " + key,
                             .field = "presence",
                             .expected = describeFile(unit->material),
                             .actual = "<not consumed by this build>"});
      continue;
    }
    compareFile(differences, "source unit " + key, unit->material,
                match->second->material);
    if (unit->entry != match->second->entry) {
      differences.push_back(
          {.subject = "source unit " + key,
           .field = "entry",
           .expected = unit->entry ? "true" : "false",
           .actual = match->second->entry ? "true" : "false"});
    }
    if (unit->prelude != match->second->prelude) {
      differences.push_back(
          {.subject = "source unit " + key,
           .field = "prelude",
           .expected = unit->prelude ? "true" : "false",
           .actual = match->second->prelude ? "true" : "false"});
    }
  }
  for (const auto &[key, unit] : observedUnits) {
    if (recordedUnits.find(key) == recordedUnits.end()) {
      differences.push_back({.subject = "source unit " + key,
                             .field = "presence",
                             .expected = "<not recorded>",
                             .actual = describeFile(unit->material)});
    }
  }
}

void compareRuntime(std::vector<BuildProvenanceDifference> &differences,
                    const BuildRuntimeProvenance &recorded,
                    const BuildRuntimeProvenance &observed) {
  std::map<std::string, const BuildRuntimeHeaderProvenance *> observedHeaders;
  for (const BuildRuntimeHeaderProvenance &header : observed.headers) {
    observedHeaders.emplace(headerKey(header), &header);
  }
  std::map<std::string, const BuildRuntimeHeaderProvenance *> recordedHeaders;
  for (const BuildRuntimeHeaderProvenance &header : recorded.headers) {
    recordedHeaders.emplace(headerKey(header), &header);
  }
  for (const auto &[key, header] : recordedHeaders) {
    const auto match = observedHeaders.find(key);
    if (match == observedHeaders.end()) {
      differences.push_back({.subject = "runtime header " + key,
                             .field = "presence",
                             .expected = describeFile(header->material),
                             .actual = "<not consumed by this build>"});
      continue;
    }
    compareFile(differences, "runtime header " + key, header->material,
                match->second->material);
  }
  for (const auto &[key, header] : observedHeaders) {
    if (recordedHeaders.find(key) == recordedHeaders.end()) {
      differences.push_back({.subject = "runtime header " + key,
                             .field = "presence",
                             .expected = "<not recorded>",
                             .actual = describeFile(header->material)});
    }
  }
  compareFile(differences, "runtime archive", recorded.archive,
              observed.archive);
  compareValue(differences, "runtime materials", "aggregate sha256",
               recorded.materialsSha256, observed.materialsSha256);
}

void compareNativeCommand(std::vector<BuildProvenanceDifference> &differences,
                          const std::vector<std::string> &recordedCommand,
                          const std::vector<std::string> &observedCommand) {
  const std::vector<std::string> recorded =
      normalizeNativeCommandForComparison(recordedCommand);
  const std::vector<std::string> observed =
      normalizeNativeCommandForComparison(observedCommand);
  const std::size_t shared = std::min(recorded.size(), observed.size());
  for (std::size_t index = 0; index < shared; ++index) {
    if (recorded[index] != observed[index]) {
      differences.push_back(
          {.subject = "native argv[" + std::to_string(index) + "]",
           .field = "operand",
           .expected = recorded[index],
           .actual = observed[index]});
    }
  }
  for (std::size_t index = shared; index < recorded.size(); ++index) {
    differences.push_back(
        {.subject = "native argv[" + std::to_string(index) + "]",
         .field = "operand",
         .expected = recorded[index],
         .actual = "<absent>"});
  }
  for (std::size_t index = shared; index < observed.size(); ++index) {
    differences.push_back(
        {.subject = "native argv[" + std::to_string(index) + "]",
         .field = "operand",
         .expected = "<absent>",
         .actual = observed[index]});
  }
}

} // namespace

std::string serializeBuildProvenanceJson(const BuildProvenance &provenance) {
  std::ostringstream stream;
  stream << "{\n  \"schema\": " << provenance.schema
         << ",\n  \"debug_info\": " << (provenance.debugInfo ? "true" : "false")
         << ",\n  \"producer\": {\n    \"executable\": ";
  writeBuildFileJson(stream, provenance.producerExecutable);
  stream << ",\n    \"compiler_build\": {\"state\":";
  writeJsonString(stream,
                  compilerSourceStateName(provenance.compilerBuild.state));
  stream << ",\"commit\":";
  writeOptionalJsonString(stream, provenance.compilerBuild.commit);
  stream << "}\n  },\n  \"project\": ";
  if (provenance.project) {
    stream << "{\n    \"package_name\":";
    writeJsonString(stream, provenance.project->packageName);
    stream << ",\n    \"package_version\":";
    writeJsonString(stream, provenance.project->packageVersion);
    stream << ",\n    \"target_name\":";
    writeJsonString(stream, provenance.project->targetName);
    stream << ",\n    \"target_kind\":";
    writeJsonString(stream, provenance.project->targetKind);
    stream << ",\n    \"profile_name\":";
    writeJsonString(stream, provenance.project->profileName);
    stream << ",\n    \"target_triple\":";
    writeJsonString(stream, provenance.project->targetTriple);
    stream << ",\n    \"model_identity\":";
    writeJsonString(stream, provenance.project->modelIdentity);
    stream << "\n  }";
  } else {
    stream << "null";
  }
  stream << ",\n  \"sources\": {\n    \"entry_root\": "
         << provenance.source.entryRoot << ",\n    \"prelude_roots\": [";
  for (std::size_t index = 0; index < provenance.source.preludeRoots.size();
       ++index) {
    if (index != 0) {
      stream << ',';
    }
    stream << provenance.source.preludeRoots[index];
  }
  stream << "],\n    \"compilation_order\": [";
  for (std::size_t index = 0; index < provenance.source.compilationOrder.size();
       ++index) {
    if (index != 0) {
      stream << ',';
    }
    stream << provenance.source.compilationOrder[index];
  }
  stream << "],\n    \"materials_sha256\":";
  writeJsonString(stream, provenance.source.materialsSha256);
  stream << ",\n    \"units\": [";
  for (std::size_t index = 0; index < provenance.source.units.size(); ++index) {
    const BuildSourceUnitProvenance &unit = provenance.source.units[index];
    stream << (index == 0 ? "\n      " : ",\n      ") << "{\"id\":" << unit.id
           << ",\"role\":";
    writeJsonString(stream, sourceRoleName(unit.role));
    stream << ",\"logical_identity\":";
    writeOptionalJsonString(stream, unit.logicalIdentity);
    stream << ",\"standard_library_name\":";
    writeOptionalJsonString(stream, unit.standardLibraryName);
    stream << ",\"package_identity\":";
    writeOptionalJsonString(stream, unit.packageIdentity);
    stream << ",\"package_relative_path\":";
    writeOptionalJsonString(stream, unit.packageRelativePath);
    stream << ",\"entry\":" << (unit.entry ? "true" : "false")
           << ",\"prelude\":" << (unit.prelude ? "true" : "false")
           << ",\"material\":";
    writeBuildFileJson(stream, unit.material);
    stream << '}';
  }
  if (!provenance.source.units.empty()) {
    stream << '\n';
  }
  stream << "    ],\n    \"dependencies\": [";
  for (std::size_t index = 0; index < provenance.source.dependencies.size();
       ++index) {
    const BuildSourceDependencyProvenance &dependency =
        provenance.source.dependencies[index];
    stream << (index == 0 ? "\n      " : ",\n      ")
           << "{\"source\":" << dependency.source
           << ",\"target\":" << dependency.target << ",\"kind\":";
    writeJsonString(stream, sourceDependencyKindName(dependency.kind));
    stream << ",\"include_spelling\":";
    writeJsonString(stream, dependency.includeSpelling);
    stream << ",\"include_occurrence\":" << dependency.includeOccurrence << '}';
  }
  if (!provenance.source.dependencies.empty()) {
    stream << '\n';
  }
  stream << "    ]\n  },\n  \"native\": {\n    \"compiler_executable\": ";
  writeBuildFileJson(stream, provenance.nativeCompilerExecutable);
  stream << ",\n    \"argv\": [";
  for (std::size_t index = 0; index < provenance.nativeCommand.size();
       ++index) {
    if (index != 0) {
      stream << ',';
    }
    writeJsonString(stream, provenance.nativeCommand[index]);
  }
  stream << "],\n    \"runtime\": {\n      \"materials_sha256\":";
  writeJsonString(stream, provenance.runtime.materialsSha256);
  stream << ",\n      \"headers\": [";
  for (std::size_t index = 0; index < provenance.runtime.headers.size();
       ++index) {
    const BuildRuntimeHeaderProvenance &header =
        provenance.runtime.headers[index];
    stream << (index == 0 ? "\n        " : ",\n        ") << "{\"kind\":";
    writeJsonString(stream, buildRuntimeHeaderKindName(header.kind));
    stream << ",\"logical_identity\":";
    writeJsonString(stream, header.logicalIdentity);
    stream << ",\"material\":";
    writeBuildFileJson(stream, header.material);
    stream << '}';
  }
  if (!provenance.runtime.headers.empty()) {
    stream << '\n';
  }
  stream << "      ],\n      \"archive\": ";
  writeBuildFileJson(stream, provenance.runtime.archive);
  stream << "\n    }\n  },\n  \"artifact\": ";
  writeBuildFileJson(stream, provenance.artifact);
  stream << "\n}\n";
  return stream.str();
}

std::optional<BuildProvenance>
parseBuildProvenanceJson(std::string_view text, std::string &errorMessage) {
  JsonParser parser(text);
  const std::optional<JsonValue> document = parser.parseDocument(errorMessage);
  if (!document) {
    return std::nullopt;
  }
  if (document->kind != JsonValue::Kind::Object) {
    errorMessage = "the report is not a JSON object";
    return std::nullopt;
  }

  BuildProvenance provenance;
  std::uint64_t schema = 0;
  if (!requireUnsigned(*document, "schema", "report", schema, errorMessage)) {
    return std::nullopt;
  }
  if (schema != static_cast<std::uint64_t>(currentBuildProvenanceSchema)) {
    errorMessage = "report schema " + std::to_string(schema) +
                   " is not the supported schema " +
                   std::to_string(currentBuildProvenanceSchema);
    return std::nullopt;
  }
  provenance.schema = currentBuildProvenanceSchema;
  if (document->member("debug_info") != nullptr &&
      !requireBoolean(*document, "debug_info", "report", provenance.debugInfo,
                      errorMessage)) {
    return std::nullopt;
  }

  const JsonValue *producer = nullptr;
  if (!requireMember(*document, "producer", JsonValue::Kind::Object, "report",
                     producer, errorMessage) ||
      !requireBuildFile(*producer, "executable", "report.producer",
                        provenance.producerExecutable, errorMessage)) {
    return std::nullopt;
  }
  const JsonValue *compilerBuild = nullptr;
  std::string compilerState;
  if (!requireMember(*producer, "compiler_build", JsonValue::Kind::Object,
                     "report.producer", compilerBuild, errorMessage) ||
      !requireString(*compilerBuild, "state", "report.producer.compiler_build",
                     compilerState, errorMessage) ||
      !requireOptionalString(*compilerBuild, "commit",
                             "report.producer.compiler_build",
                             provenance.compilerBuild.commit, errorMessage)) {
    return std::nullopt;
  }
  const std::optional<CompilerSourceState> state =
      parseCompilerSourceStateName(compilerState);
  if (!state) {
    errorMessage = "unknown compiler source state '" + compilerState + "'";
    return std::nullopt;
  }
  provenance.compilerBuild.state = *state;

  const JsonValue *project = document->member("project");
  if (project == nullptr) {
    errorMessage = "missing 'report.project'";
    return std::nullopt;
  }
  if (project->kind == JsonValue::Kind::Object) {
    BuildProjectProvenance recorded;
    if (!requireString(*project, "package_name", "report.project",
                       recorded.packageName, errorMessage) ||
        !requireString(*project, "package_version", "report.project",
                       recorded.packageVersion, errorMessage) ||
        !requireString(*project, "target_name", "report.project",
                       recorded.targetName, errorMessage) ||
        !requireString(*project, "target_kind", "report.project",
                       recorded.targetKind, errorMessage) ||
        !requireString(*project, "profile_name", "report.project",
                       recorded.profileName, errorMessage) ||
        !requireString(*project, "target_triple", "report.project",
                       recorded.targetTriple, errorMessage) ||
        !requireString(*project, "model_identity", "report.project",
                       recorded.modelIdentity, errorMessage)) {
      return std::nullopt;
    }
    provenance.project = std::move(recorded);
  } else if (project->kind != JsonValue::Kind::Null) {
    errorMessage = "'report.project' has the wrong type";
    return std::nullopt;
  }

  const JsonValue *sources = nullptr;
  if (!requireMember(*document, "sources", JsonValue::Kind::Object, "report",
                     sources, errorMessage)) {
    return std::nullopt;
  }
  std::uint64_t entryRoot = 0;
  if (!requireUnsigned(*sources, "entry_root", "report.sources", entryRoot,
                       errorMessage) ||
      !requireIdArray(*sources, "prelude_roots", "report.sources",
                      provenance.source.preludeRoots, errorMessage) ||
      !requireIdArray(*sources, "compilation_order", "report.sources",
                      provenance.source.compilationOrder, errorMessage) ||
      !requireString(*sources, "materials_sha256", "report.sources",
                     provenance.source.materialsSha256, errorMessage)) {
    return std::nullopt;
  }
  provenance.source.entryRoot = static_cast<SourceUnitId>(entryRoot);

  const JsonValue *units = nullptr;
  if (!requireMember(*sources, "units", JsonValue::Kind::Array,
                     "report.sources", units, errorMessage)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < units->elements.size(); ++index) {
    const JsonValue &element = units->elements[index];
    const std::string path =
        "report.sources.units[" + std::to_string(index) + "]";
    if (element.kind != JsonValue::Kind::Object) {
      errorMessage = "'" + path + "' is not an object";
      return std::nullopt;
    }
    BuildSourceUnitProvenance unit;
    std::uint64_t id = 0;
    std::string role;
    if (!requireUnsigned(element, "id", path, id, errorMessage) ||
        !requireString(element, "role", path, role, errorMessage) ||
        !requireOptionalString(element, "logical_identity", path,
                               unit.logicalIdentity, errorMessage) ||
        !requireOptionalString(element, "standard_library_name", path,
                               unit.standardLibraryName, errorMessage) ||
        !requireOptionalString(element, "package_identity", path,
                               unit.packageIdentity, errorMessage) ||
        !requireOptionalString(element, "package_relative_path", path,
                               unit.packageRelativePath, errorMessage) ||
        !requireBoolean(element, "entry", path, unit.entry, errorMessage) ||
        !requireBoolean(element, "prelude", path, unit.prelude, errorMessage) ||
        !requireBuildFile(element, "material", path, unit.material,
                          errorMessage)) {
      return std::nullopt;
    }
    const std::optional<SourceUnitRole> parsedRole = parseSourceRoleName(role);
    if (!parsedRole) {
      errorMessage = "unknown source unit role '" + role + "'";
      return std::nullopt;
    }
    unit.id = static_cast<SourceUnitId>(id);
    unit.role = *parsedRole;
    provenance.source.units.push_back(std::move(unit));
  }

  const JsonValue *dependencies = nullptr;
  if (!requireMember(*sources, "dependencies", JsonValue::Kind::Array,
                     "report.sources", dependencies, errorMessage)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < dependencies->elements.size(); ++index) {
    const JsonValue &element = dependencies->elements[index];
    const std::string path =
        "report.sources.dependencies[" + std::to_string(index) + "]";
    if (element.kind != JsonValue::Kind::Object) {
      errorMessage = "'" + path + "' is not an object";
      return std::nullopt;
    }
    BuildSourceDependencyProvenance dependency;
    std::uint64_t source = 0;
    std::uint64_t target = 0;
    std::uint64_t occurrence = 0;
    std::string kind;
    if (!requireUnsigned(element, "source", path, source, errorMessage) ||
        !requireUnsigned(element, "target", path, target, errorMessage) ||
        !requireString(element, "kind", path, kind, errorMessage) ||
        !requireString(element, "include_spelling", path,
                       dependency.includeSpelling, errorMessage) ||
        !requireUnsigned(element, "include_occurrence", path, occurrence,
                         errorMessage)) {
      return std::nullopt;
    }
    const std::optional<SourceDependencyKind> parsedKind =
        parseSourceDependencyKindName(kind);
    if (!parsedKind) {
      errorMessage = "unknown source dependency kind '" + kind + "'";
      return std::nullopt;
    }
    dependency.source = static_cast<SourceUnitId>(source);
    dependency.target = static_cast<SourceUnitId>(target);
    dependency.kind = *parsedKind;
    dependency.includeOccurrence = static_cast<std::size_t>(occurrence);
    provenance.source.dependencies.push_back(std::move(dependency));
  }

  const JsonValue *native = nullptr;
  if (!requireMember(*document, "native", JsonValue::Kind::Object, "report",
                     native, errorMessage) ||
      !requireBuildFile(*native, "compiler_executable", "report.native",
                        provenance.nativeCompilerExecutable, errorMessage)) {
    return std::nullopt;
  }
  const JsonValue *argv = nullptr;
  if (!requireMember(*native, "argv", JsonValue::Kind::Array, "report.native",
                     argv, errorMessage)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < argv->elements.size(); ++index) {
    if (argv->elements[index].kind != JsonValue::Kind::String) {
      errorMessage =
          "'report.native.argv[" + std::to_string(index) + "]' is not a string";
      return std::nullopt;
    }
    provenance.nativeCommand.push_back(argv->elements[index].text);
  }

  const JsonValue *runtime = nullptr;
  if (!requireMember(*native, "runtime", JsonValue::Kind::Object,
                     "report.native", runtime, errorMessage) ||
      !requireString(*runtime, "materials_sha256", "report.native.runtime",
                     provenance.runtime.materialsSha256, errorMessage) ||
      !requireBuildFile(*runtime, "archive", "report.native.runtime",
                        provenance.runtime.archive, errorMessage)) {
    return std::nullopt;
  }
  const JsonValue *headers = nullptr;
  if (!requireMember(*runtime, "headers", JsonValue::Kind::Array,
                     "report.native.runtime", headers, errorMessage)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < headers->elements.size(); ++index) {
    const JsonValue &element = headers->elements[index];
    const std::string path =
        "report.native.runtime.headers[" + std::to_string(index) + "]";
    if (element.kind != JsonValue::Kind::Object) {
      errorMessage = "'" + path + "' is not an object";
      return std::nullopt;
    }
    BuildRuntimeHeaderProvenance header;
    std::string kind;
    if (!requireString(element, "kind", path, kind, errorMessage) ||
        !requireString(element, "logical_identity", path,
                       header.logicalIdentity, errorMessage) ||
        !requireBuildFile(element, "material", path, header.material,
                          errorMessage)) {
      return std::nullopt;
    }
    const std::optional<BuildRuntimeHeaderKind> parsedKind =
        parseRuntimeHeaderKindName(kind);
    if (!parsedKind) {
      errorMessage = "unknown runtime header kind '" + kind + "'";
      return std::nullopt;
    }
    header.kind = *parsedKind;
    provenance.runtime.headers.push_back(std::move(header));
  }

  if (!requireBuildFile(*document, "artifact", "report", provenance.artifact,
                        errorMessage)) {
    return std::nullopt;
  }
  return provenance;
}

std::vector<std::string>
normalizeNativeCommandForComparison(const std::vector<std::string> &command) {
  std::vector<std::string> normalized = command;
  for (std::size_t index = 0; index + 1 < normalized.size(); ++index) {
    if (normalized[index] == "-o") {
      normalized[index + 1] = "<staged-output>";
      ++index;
      continue;
    }
    if (normalized[index] == "-MF") {
      normalized[index + 1] = "<dependency-file>";
      ++index;
    }
  }
  return normalized;
}

std::vector<BuildProvenanceDifference>
compareBuildProvenance(const BuildProvenance &recorded,
                       const BuildProvenance &observed) {
  std::vector<BuildProvenanceDifference> differences;
  compareValue(differences, "report", "schema", std::to_string(recorded.schema),
               std::to_string(observed.schema));
  compareValue(differences, "build policy", "debug info",
               recorded.debugInfo ? "true" : "false",
               observed.debugInfo ? "true" : "false");
  compareProject(differences, recorded.project, observed.project);
  compareFile(differences, "producer executable", recorded.producerExecutable,
              observed.producerExecutable);
  compareValue(
      differences, "compiler build", "source state",
      std::string(compilerSourceStateName(recorded.compilerBuild.state)),
      std::string(compilerSourceStateName(observed.compilerBuild.state)));
  compareValue(differences, "compiler build", "source commit",
               recorded.compilerBuild.commit.value_or("<unavailable>"),
               observed.compilerBuild.commit.value_or("<unavailable>"));
  compareSourceUnits(differences, recorded.source, observed.source);
  compareValue(differences, "source materials", "aggregate sha256",
               recorded.source.materialsSha256,
               observed.source.materialsSha256);
  compareFile(differences, "native compiler executable",
              recorded.nativeCompilerExecutable,
              observed.nativeCompilerExecutable);
  compareNativeCommand(differences, recorded.nativeCommand,
                       observed.nativeCommand);
  compareRuntime(differences, recorded.runtime, observed.runtime);
  compareFile(differences, "built artifact", recorded.artifact,
              observed.artifact);
  return differences;
}

} // namespace lang::driver
