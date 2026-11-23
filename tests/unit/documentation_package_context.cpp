// Documentation imports contextual modules without publishing fixture metadata.
#include "../../src/docgen/frontend.h"
#include "gti/documentation.h"
#include "gti/driver/native_toolchain.h"
#include "unit_test.h"

#include <stdexcept>

GTI_UNIT_TEST(documentation_package_context) {
  const auto layout = lang::driver::discoverToolchainLayout(argv[0]).standardLibrary;
  const auto frontend = lang::docgen::analyzeModules(
      layout, {"std.string", "std.package"});
  if (!frontend.sourceValid || !frontend.syntaxValid ||
      !frontend.semanticValid || !frontend.diagnostics.empty())
    throw std::runtime_error("documentation consumer cannot import std.package");
  const auto index = lang::DocumentationQueries().standardLibraryIndex(frontend);
  if (!index.valid() || index.index.declarations.empty())
    throw std::runtime_error("documentation index lost the standard-library API");
  const auto json = lang::serializeDocumentationIndexJson(index.index);
  if (json.find("gti-reference-consumer") != std::string::npos ||
      json.find("std::package::name") != std::string::npos)
    throw std::runtime_error("synthetic package constants leaked into the index");
  return 0;
}
