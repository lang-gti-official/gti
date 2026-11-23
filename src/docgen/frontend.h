#pragma once

#include "gti/frontend.h"
#include "gti/standard_library.h"

namespace lang::docgen {

// The index entry is a synthetic consumer, not a toolchain source. Give it
// explicit package metadata so contextual modules use their normal loader
// path. These fixture values are not public standard-library declarations.
inline FrontendResult analyzeModules(const StandardLibraryLayout &layout,
                                     const std::vector<std::string> &modules) {
  const auto root = layout.root / "__gti_reference_consumer__";
  PackageSourceRoot owner;
  owner.identity = "gti-reference-consumer";
  owner.name = "gti-reference-consumer";
  owner.packageRoot = root;
  owner.sourceRoot = root;
  owner.packageVersion = "0.0.0";
  std::string source;
  for (const auto &module : modules)
    source += "import " + module + ";\n";
  source += "\nint main() { return 0; }\n";
  FrontendOptions options;
  options.stopAfter = FrontendPhase::Semantics;
  options.toolingOccurrences = true;
  return Frontend(options).analyze(root / "main.gti", source, {layout.prelude},
                                   {}, {layout.root}, {owner});
}

} // namespace lang::docgen
