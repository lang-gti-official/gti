#include "gti/documentation.h"
#include "gti/standard_library.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

std::vector<std::string> standardLibraryUnits(const std::filesystem::path &root,
                                              std::error_code &error) {
  std::vector<std::string> units;
  const std::filesystem::path directory = root / "std";
  if (!std::filesystem::is_directory(directory, error)) {
    if (!error) {
      error = std::make_error_code(std::errc::not_a_directory);
    }
    return units;
  }
  std::filesystem::recursive_directory_iterator iterator(directory, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && iterator != end) {
    const std::filesystem::directory_entry &entry = *iterator;
    if (entry.is_regular_file(error) && entry.path().extension() == ".gti") {
      std::filesystem::path relative =
          std::filesystem::relative(entry.path(), root, error);
      if (error) {
        return {};
      }
      relative.replace_extension();
      units.push_back(relative.generic_string());
    }
    iterator.increment(error);
  }
  std::sort(units.begin(), units.end());
  units.erase(std::unique(units.begin(), units.end()), units.end());
  return units;
}

std::string entrySource(const std::vector<std::string> &units) {
  std::string source;
  for (const std::string &unit : units) {
    source += "#include <" + unit + ">\n";
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
  std::error_code error;
  const std::vector<std::string> units =
      standardLibraryUnits(layout.root, error);
  if (error || units.empty()) {
    std::cerr << "Could not enumerate standard-library units under '"
              << (layout.root / "std").generic_string() << "'";
    if (error) {
      std::cerr << ": " << error.message();
    }
    std::cerr << ".\n";
    return 1;
  }
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
                   entrySource(units), {layout.prelude}, {}, {layout.root});
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
