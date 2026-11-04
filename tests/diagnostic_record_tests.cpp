#include "gti/driver/diagnostic_record.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
  namespace fs = std::filesystem;
  const auto root =
      fs::temp_directory_path() /
      ("gti-diagnostic-record-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(root);
  try {
    lang::SourceManager sources;
    const std::string original = "int main() { return missing; }\n";
    sources.set((root / "main.gti").string(), original);
    std::ofstream(root / "main.gti") << "changed on disk\n";
    lang::Diagnostic d = lang::makeDiagnostic(
        "GTI-S2001", lang::DiagnosticPhase::Semantics,
        lang::SourceSpan{(root / "main.gti").string(), 20, 27, 1},
        "unknown name");
    d.related.push_back({d.primary, "related source"});
    d.hints.push_back("review spelling");
    d.fixes.push_back({d.primary, "0", "replace name"});
    lang::driver::DiagnosticRecordContext context{
        .projectRoot = root, .arguments = {"build"}, .compilerVersion = "test"};
    const auto first =
        lang::driver::recordDiagnostics(context, std::vector{d}, sources);
    if (first.path.empty())
      throw std::runtime_error(first.error);
    const auto second =
        lang::driver::recordDiagnostics(context, std::vector{d}, sources);
    if (first.path != second.path)
      throw std::runtime_error("identical failure was not deduplicated");
    const auto exported = lang::driver::exportDiagnosticRecord(
        first.path, root / "export", "expected a declaration");
    if (exported.path.empty())
      throw std::runtime_error(exported.error);
    if (!fs::is_regular_file(root / "export/report.txt"))
      throw std::runtime_error("missing readable report");
    std::ifstream input(root / "export/report.json");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    for (const auto *required :
         {"missing", "related source", "review spelling", "replace name",
          "expected a declaration", "\"occurrences\":2"})
      if (text.find(required) == text.npos)
        throw std::runtime_error(required);
    if (text.find("changed on disk") != text.npos)
      throw std::runtime_error(
          "captured current disk instead of compiler buffer");
    if (lang::driver::exportDiagnosticRecord(first.path, root / "export")
            .error.empty())
      throw std::runtime_error("overwrote export");
    sources.set((root / "main.gti").string(), original + "// new input\n");
    const auto changed =
        lang::driver::recordDiagnostics(context, std::vector{d}, sources);
    if (changed.path.empty() || changed.path == first.path)
      throw std::runtime_error("lost changed source observation");
    sources.set((root / "binary.gti").string(),
                std::string(1, static_cast<char>(0xff)));
    const auto binary =
        lang::driver::recordDiagnostics(context, std::vector{d}, sources);
    if (binary.path.empty())
      throw std::runtime_error(binary.error);
    std::ifstream binaryInput(binary.path);
    const std::string binaryText((std::istreambuf_iterator<char>(binaryInput)),
                                 {});
    if (binaryText.find("base64") == binaryText.npos ||
        binaryText.find("/w==") == binaryText.npos)
      throw std::runtime_error("invalid UTF-8 snapshot was not preserved");
    for (int i = 0; i < 34; ++i) {
      d.message = "distinct observation " + std::to_string(i);
      if (lang::driver::recordDiagnostics(context, std::vector{d}, sources)
              .path.empty())
        throw std::runtime_error("retention recording failed");
    }
    std::size_t retained = 0;
    for (const auto &entry :
         fs::directory_iterator(root / ".gti/diagnostic-reports"))
      if (entry.path().extension() == ".json")
        ++retained;
    if (retained != 32)
      throw std::runtime_error("retention bound failed");
    sources.set("oversized.gti", std::string(8 * 1024 * 1024 + 1, 'x'));
    if (lang::driver::recordDiagnostics(context, std::vector{d}, sources)
            .error.empty())
      throw std::runtime_error("source size limit failed");
    sources.clear();
    d.severity = lang::DiagnosticSeverity::Warning;
    if (!lang::driver::recordDiagnostics(context, std::vector{d}, sources)
             .path.empty())
      throw std::runtime_error("recorded warning-only compilation");
    fs::remove_all(root / ".gti");
    fs::create_directory_symlink(root / "export", root / ".gti");
    d.severity = lang::DiagnosticSeverity::Error;
    if (lang::driver::recordDiagnostics(context, std::vector{d}, sources)
            .error.empty())
      throw std::runtime_error("followed storage symlink");
    fs::remove_all(root);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    fs::remove_all(root);
    return 1;
  }
}
