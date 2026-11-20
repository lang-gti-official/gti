// An embedded driver reads installed sources; an opted-in host enforces bundle
// admission.
#include "../temporary_directory.h"
#include "gti/driver/compiled_scalar_library.h"
#include "gti/driver/native_toolchain.h"
#include "gti/driver/project.h"
#include "unit_test.h"

#include <fstream>
#include <stdexcept>

GTI_UNIT_TEST(embedded_stdlib) {
  using namespace lang;
  using namespace lang::driver;
  gti_test::TemporaryDirectory temporary;
  const auto toolchain = discoverToolchainLayout(argv[0]);
  const auto root = temporary.root() / "stdlib";
  std::filesystem::copy(toolchain.standardLibrary.root, root,
                        std::filesystem::copy_options::recursive);
  const auto entry = temporary.root() / "main.gti";
  std::ofstream(entry) << "import std.array;\nint main() { return 0; }\n";
  auto target = TargetInfo::host();
  target.runtimeCapabilities.threads = true;
  std::filesystem::create_directories(temporary.root() / "precompiled-stdlib" /
                                      targetTriple(target) / "release");
  auto layout = standardLibraryLayout(root);
  const auto load = [&] {
    const CompilationRequest request(entry, layout, target,
                                     OptimizationLevel::O3, CppStandard::Cpp23);
    return loadStandardLibraryInputs(request.withBackend(BackendKind::Llvm),
                                     toolchain.runtimeLibrary);
  };
  if (!load().compilation.succeeded())
    throw std::runtime_error(
        "embedded host failed to load std.array from source");
  layout.useInstalledBundles = true;
  try {
    load();
  } catch (const std::invalid_argument &error) {
    if (std::string_view(error.what()) ==
        "compiled library input must be a regular file")
      return 0;
    throw;
  }
  throw std::runtime_error("missing opted-in bundle was accepted");
}
