#include "gti/driver/diagnostic_record.h"
#include "gti/build_identity_config.h"
#include "sha256.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <stdexcept>

namespace lang::driver {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t limit = 16 * 1024 * 1024;
std::string encode(const llvm::json::Value &value) {
  std::string text;
  llvm::raw_string_ostream out(text);
  out << value;
  return text;
}
std::string hash(std::string_view text) {
  Sha256 value;
  value.update(text);
  return value.finishHex();
}
std::string read(const fs::path &path) {
  if (fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path) ||
      fs::file_size(path) > limit)
    throw std::runtime_error(
        "expected a regular diagnostic record below 16 MiB");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open diagnostic record");
  std::string text((std::istreambuf_iterator<char>(input)), {});
  if (input.bad())
    throw std::runtime_error("cannot read diagnostic record");
  return text;
}
void write(const fs::path &path, std::string_view text) {
  if (text.size() > limit)
    throw std::runtime_error("diagnostic record exceeds 16 MiB");
  std::ofstream output(path, std::ios::binary);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  if (!output)
    throw std::runtime_error("cannot write diagnostic record");
}
void directory(const fs::path &path) {
  if (fs::is_symlink(fs::symlink_status(path)))
    throw std::runtime_error("diagnostic storage is a symbolic link");
  if (fs::create_directory(path))
    fs::permissions(path, fs::perms::owner_all);
  if (!fs::is_directory(path))
    throw std::runtime_error("diagnostic storage is not a directory");
}
llvm::json::Object location(const SourceSpan &span,
                            const SourceManager &sources) {
  llvm::json::Object result{{"source", span.source},
                            {"start", span.start},
                            {"end", span.end},
                            {"line", span.line}};
  const auto *text = sources.find(span.source);
  result["source_available"] = text != nullptr;
  if (!text)
    return result;
  const auto at = sources.locate(span);
  const auto first = at.line > 25 ? at.line - 25 : 1;
  auto endSpan = span;
  endSpan.start = span.end;
  const auto last = sources.locate(endSpan).line + 25;
  std::size_t line = 1, begin = 0, end = text->size();
  for (std::size_t i = 0; i < text->size(); ++i) {
    if (line < first)
      begin = i + 1;
    if ((*text)[i] == '\n') {
      if (line == last) {
        end = i + 1;
        break;
      }
      ++line;
    }
  }
  result["line"] = at.line;
  result["column"] = at.column;
  result["first_line"] = first;
  result["snippet"] = text->substr(begin, end - begin);
  return result;
}
llvm::json::Object message(const Diagnostic &d, const SourceManager &sources) {
  llvm::json::Array related, fixes, hints;
  for (const auto &r : d.related)
    related.push_back(llvm::json::Object{{"span", location(r.span, sources)},
                                         {"message", r.message}});
  for (const auto &f : d.fixes)
    fixes.push_back(
        llvm::json::Object{{"span", location(f.span, sources)},
                           {"replacement", f.replacement},
                           {"message", f.message},
                           {"preferred", f.preferred},
                           {"applicability", static_cast<int>(f.applicability)},
                           {"repair_id", f.repairId}});
  for (const auto &h : d.hints)
    hints.push_back(h);
  return llvm::json::Object{{"code", d.code},
                            {"phase", std::string(phaseName(d.phase))},
                            {"severity", static_cast<int>(d.severity)},
                            {"group", d.group},
                            {"message", d.message},
                            {"primary", location(d.primary, sources)},
                            {"related", std::move(related)},
                            {"fixes", std::move(fixes)},
                            {"hints", std::move(hints)}};
}
struct Lock {
  fs::path path;
  ~Lock() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};
} // namespace
DiagnosticRecordResult
recordDiagnostics(const DiagnosticRecordContext &context,
                  std::span<const Diagnostic> diagnostics,
                  const SourceManager &sources) {
  if (!hasErrorDiagnostics(diagnostics))
    return {};
  try {
    llvm::json::Array messages, inputs, arguments;
    for (const auto &d : diagnostics)
      messages.push_back(message(d, sources));
    auto names = sources.names();
    std::sort(names.begin(), names.end());
    std::size_t bytes = 0;
    for (const auto &name : names) {
      const auto &text = *sources.find(name);
      bytes += text.size();
      if (bytes > limit / 2)
        throw std::runtime_error("source snapshot exceeds 8 MiB");
      inputs.push_back(llvm::json::Object{
          {"path", name}, {"sha256", hash(text)}, {"text", text}});
    }
    for (const auto &arg : context.arguments)
      arguments.push_back(arg);
    llvm::json::Object evidence{{"schema", "gti-diagnostic-record-v1"},
                                {"compiler_version", context.compilerVersion},
                                {"compiler_commit", GTI_COMPILER_SOURCE_COMMIT},
                                {"compiler_state", GTI_COMPILER_SOURCE_STATE},
                                {"project_root", context.projectRoot.string()},
                                {"arguments", std::move(arguments)},
                                {"target", context.target},
                                {"backend", context.backend},
                                {"profile", context.profile},
                                {"diagnostics", std::move(messages)},
                                {"sources", std::move(inputs)}};
    const auto id =
        hash(encode(llvm::json::Value(llvm::json::Object(evidence))));
    const auto root = fs::canonical(context.projectRoot);
    directory(root / ".gti");
    const auto storage = root / ".gti" / "diagnostic-reports";
    directory(storage);
    if (!fs::create_directory(storage / ".lock"))
      throw std::runtime_error("diagnostic recorder is busy");
    Lock lock{storage / ".lock"};
    if (!fs::exists(fs::symlink_status(storage / ".gitignore")))
      write(storage / ".gitignore", "*\n");
    const auto path = storage / (id + ".json");
    std::int64_t count = 1;
    if (fs::exists(fs::symlink_status(path))) {
      auto previous = llvm::json::parse(read(path));
      if (!previous) {
        llvm::consumeError(previous.takeError());
        throw std::runtime_error("invalid existing record");
      }
      auto *object = previous->getAsObject();
      if (!object || !object->getInteger("occurrences") ||
          *object->getInteger("occurrences") < 1 ||
          *object->getInteger("occurrences") >= INT64_MAX)
        throw std::runtime_error("invalid record occurrence count");
      count += *object->getInteger("occurrences");
    }
    evidence["occurrences"] = count;
    evidence["last_seen"] = static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    write(lock.path / "record.json",
          encode(llvm::json::Value(std::move(evidence))) + "\n");
    fs::rename(lock.path / "record.json", path);
    std::vector<fs::directory_entry> records;
    for (const auto &entry : fs::directory_iterator(storage)) {
      const auto stem = entry.path().stem().string();
      if (!entry.is_symlink() && entry.is_regular_file() &&
          entry.path().extension() == ".json" && stem.size() == 64 &&
          stem.find_first_not_of("0123456789abcdef") == std::string::npos)
        records.push_back(entry);
    }
    std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
      return a.last_write_time() > b.last_write_time();
    });
    for (std::size_t i = 32; i < records.size(); ++i)
      fs::remove(records[i]);
    return {.path = path};
  } catch (const std::exception &error) {
    return {.error = error.what()};
  }
}
DiagnosticRecordResult exportDiagnosticRecord(const fs::path &record,
                                              const fs::path &destination,
                                              std::string_view note) {
  try {
    auto parsed = llvm::json::parse(read(record));
    if (!parsed) {
      llvm::consumeError(parsed.takeError());
      throw std::runtime_error("invalid diagnostic JSON");
    }
    auto *object = parsed->getAsObject();
    if (!object || object->getString("schema") != "gti-diagnostic-record-v1")
      throw std::runtime_error("unsupported diagnostic record");
    (*object)["user_note"] = std::string(note);
    const auto content = encode(*parsed) + "\n";
    if (content.size() > limit)
      throw std::runtime_error("export exceeds 16 MiB");
    if (fs::exists(fs::symlink_status(destination)) ||
        !fs::create_directory(destination))
      throw std::runtime_error("export requires a new directory");
    fs::permissions(destination, fs::perms::owner_all);
    write(destination / "report.json", content);
    write(destination / "README.txt",
          "GTI diagnostic evidence\n\nreport.json preserves ordered "
          "diagnostics, captured source buffers, and invocation metadata. "
          "Original source paths are data, never extraction paths. External "
          "native dependencies and environment are not captured; this is not a "
          "hermetic replay. Review source and arguments before sharing.\n");
    return {.path = destination};
  } catch (const std::exception &error) {
    return {.error = error.what()};
  }
}
} // namespace lang::driver
