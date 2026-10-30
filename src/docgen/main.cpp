#include "gti/documentation.h"
#include "gti/standard_library.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct Options {
  std::optional<std::filesystem::path> standardLibrary;
  std::optional<std::filesystem::path> output;
};

void printUsage(std::ostream &output, std::string_view executable) {
  output << "Usage: " << executable
         << " [--stdlib <path>] [--output <path|->]\n"
            "\n"
            "Compile every shipped standard-library unit through GTI semantics "
            "and emit\n"
            "the deterministic, pointer-free documentation index as JSON.\n";
}

std::optional<Options> parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      printUsage(std::cout, argv[0]);
      return std::nullopt;
    }
    if (argument != "--stdlib" && argument != "--output") {
      std::cerr << "Unknown argument '" << argument << "'.\n";
      printUsage(std::cerr, argv[0]);
      return std::nullopt;
    }
    if (++index >= argc) {
      std::cerr << "Missing value for '" << argument << "'.\n";
      printUsage(std::cerr, argv[0]);
      return std::nullopt;
    }
    if (argument == "--stdlib") {
      options.standardLibrary = argv[index];
    } else {
      options.output = argv[index];
    }
  }
  return options;
}

// The public standard-library API is exactly the module map the source loader
// authorizes (stdlib/module-map.json); `detail/` units are reached through
// those modules' imports, as a program would reach them. The default module
// is the aggregate that only re-exports the others, so it is not imported.
std::vector<std::string>
standardLibraryModules(const std::filesystem::path &root,
                       std::string &problem) {
  const std::filesystem::path path = root / "module-map.json";
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    problem = "cannot read " + path.generic_string();
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  auto parsed = llvm::json::parse(buffer.str());
  if (!parsed) {
    problem = path.generic_string() + ": " + llvm::toString(parsed.takeError());
    return {};
  }
  const auto *object = parsed->getAsObject();
  const auto *modules = object ? object->getObject("modules") : nullptr;
  const auto standard = object ? object->getString("default") : std::nullopt;
  if (modules == nullptr || !standard) {
    problem = path.generic_string() + " needs 'default' and 'modules'";
    return {};
  }
  std::vector<std::string> names;
  for (const auto &entry : *modules) {
    const std::string name = entry.first.str();
    if (name != *standard)
      names.push_back(standard->str() + "." + name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string entrySource(const std::vector<std::string> &modules) {
  std::string source;
  for (const std::string &module : modules) {
    source += "import " + module + ";\n";
  }
  source += "\nint main() { return 0; }\n";
  return source;
}

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.primary.source << ':' << diagnostic.primary.line
              << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const std::string &hint : diagnostic.hints) {
      std::cerr << "  hint: " << hint << '\n';
    }
  }
}

bool writeOutput(const std::optional<std::filesystem::path> &path,
                 std::string_view contents) {
  if (!path || path->empty() || path->string() == "-") {
    std::cout << contents;
    return static_cast<bool>(std::cout);
  }
  std::error_code error;
  if (path->has_parent_path()) {
    std::filesystem::create_directories(path->parent_path(), error);
    if (error) {
      std::cerr << "Could not create output directory '"
                << path->parent_path().generic_string()
                << "': " << error.message() << '\n';
      return false;
    }
  }
  std::ofstream output(*path, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "Could not open output file '" << path->generic_string()
              << "'.\n";
    return false;
  }
  output << contents;
  if (!output) {
    std::cerr << "Could not write output file '" << path->generic_string()
              << "'.\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const std::optional<Options> parsed = parseOptions(argc, argv);
  if (!parsed) {
    return argc == 2 && (std::string_view(argv[1]) == "--help" ||
                         std::string_view(argv[1]) == "-h")
               ? 0
               : 2;
  }

  const lang::StandardLibraryLayout layout =
      parsed->standardLibrary
          ? lang::standardLibraryLayout(*parsed->standardLibrary)
          : lang::discoverStandardLibrary(argv[0], GTI_BUILD_STDLIB_ROOT);
  std::string problem;
  const std::vector<std::string> modules =
      standardLibraryModules(layout.root, problem);
  if (modules.empty()) {
    std::cerr << "Could not enumerate the public standard-library modules: "
              << (problem.empty() ? "the module map lists none" : problem)
              << ".\n";
    return 1;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(layout.prelude, error) || error) {
    std::cerr << "Could not find the standard-library prelude at '"
              << layout.prelude.generic_string() << "'.\n";
    return 1;
  }

  lang::FrontendOptions frontendOptions;
  frontendOptions.stopAfter = lang::FrontendPhase::Semantics;
  frontendOptions.toolingOccurrences = true;
  const lang::FrontendResult frontend =
      lang::Frontend(frontendOptions)
          .analyze(layout.root / "__gti_reference_index__.gti",
                   entrySource(modules), {layout.prelude}, {}, {layout.root});
  if (!frontend.sourceValid || !frontend.syntaxValid ||
      !frontend.semanticValid || !frontend.diagnostics.empty()) {
    printDiagnostics(frontend);
    return 1;
  }

  const lang::DocumentationIndexResult projected =
      lang::DocumentationQueries().standardLibraryIndex(frontend);
  if (!projected.valid()) {
    for (const std::string &projectionError : projected.errors) {
      std::cerr << "documentation index: " << projectionError << '\n';
    }
    return 1;
  }

  return writeOutput(parsed->output,
                     lang::serializeDocumentationIndexJson(projected.index))
             ? 0
             : 1;
}
