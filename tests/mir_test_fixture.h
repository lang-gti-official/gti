#pragma once

// Backend-neutral fixture for MIR-level tests: analysis entry points, the
// example corpus, expectations and the named-test runner. Nothing here
// depends on a code generator.

#include "gti/frontend.h"
#include "gti/mir.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/loops.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gti_test::mir_fixture {

int failures = 0;
std::optional<std::filesystem::path> corpusSource;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<std::filesystem::path> exampleCorpusSources() {
  if (corpusSource.has_value()) {
    if (std::filesystem::is_regular_file(*corpusSource) &&
        corpusSource->extension() == ".gti") {
      return {*corpusSource};
    }
    return {};
  }

  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path examples = repository / "examples";
  std::vector<std::filesystem::path> sources;
  if (std::filesystem::is_directory(examples)) {
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(examples)) {
      if (entry.is_regular_file() && entry.path().extension() == ".gti") {
        sources.push_back(entry.path());
      }
    }
  }
  std::sort(sources.begin(), sources.end());
  return sources;
}

lang::FrontendResult analyze(std::string name, std::string source) {
  lang::FrontendResult result =
      lang::Frontend().analyze(std::move(name), std::move(source));
  if (!result.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  return result;
}

lang::FrontendResult analyzeWithStandardLibrary(std::string name,
                                                std::string source) {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  lang::FrontendResult result = lang::Frontend().analyze(
      std::move(name), std::move(source), {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  if (!result.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  return result;
}

const lang::MirFunctionInstance *
firstSourceFunction(const lang::MirProgram &program) {
  const auto found = std::find_if(
      program.functionInstances().begin(), program.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.definitionKind == lang::MirDefinitionKind::Source;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
}

struct NamedTest {
  std::string_view name;
  void (*run)();
};

template <std::size_t N>
int runNamedTests(int argc, char **argv, const NamedTest (&tests)[N],
                  std::string_view suite) {
  if (argc == 2 && std::string_view(argv[1]) == "--list") {
    for (const NamedTest &test : tests) {
      std::cout << test.name << '\n';
    }
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--test") {
    const std::string_view requested = argv[2];
    for (const NamedTest &test : tests) {
      if (test.name == requested) {
        test.run();
        return failures == 0 ? 0 : 1;
      }
    }
    std::cerr << "unknown " << suite << " test: " << requested << '\n';
    return 2;
  }
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " [--list | --test NAME]\n";
    return 2;
  }
  for (const NamedTest &test : tests) {
    test.run();
  }
  if (failures != 0) {
    std::cerr << failures << ' ' << suite << " test(s) failed\n";
    return 1;
  }
  std::cout << suite << " tests passed (" << N << ")\n";
  return 0;
}

} // namespace gti_test::mir_fixture

using namespace gti_test::mir_fixture;
