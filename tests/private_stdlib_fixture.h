#pragma once

#include "gti/frontend.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace gti_test {

// Explicit white-box entry: source ownership permits quoted private imports,
// while this nonexistent test path grants no compiler capability provenance.
inline lang::FrontendResult
analyzePrivateMemoryFixture(std::string_view name, std::string source,
                            lang::FrontendOptions options = {},
                            std::filesystem::path root = {}) {
  if (root.empty())
    root =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto entry =
      root / "__private_tests__" / std::filesystem::path(name).filename();
  source.insert(0, "import std.memory; "
                   "import \"../std/detail/memory_storage.gti\";\n");
  return lang::Frontend(std::move(options))
      .analyze(entry, std::move(source), {root / "prelude.gti"}, {}, {root});
}

// Preserve the separate, deliberately trusted prelude-fixture contract used by
// compiler-capability and optimizer tests. Callers must opt into this by name.
inline lang::FrontendResult
analyzeTrustedPrivateMemoryFixture(std::string_view name, std::string source,
                                   lang::FrontendOptions options = {}) {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto entry =
      root / "__private_tests__" / std::filesystem::path(name).filename();
  source.insert(0, "import std.memory; "
                   "import \"../std/detail/memory_storage.gti\";\n");
  return lang::Frontend(std::move(options))
      .analyze(entry, source, {root / "prelude.gti", entry},
               {{std::filesystem::weakly_canonical(entry).string(), source}},
               {root});
}

} // namespace gti_test
