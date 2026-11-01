#include "gti/compiled_interface_types.h"
#include "gti/compiled_scalar_unit.h"
#include "gti/driver/compiled_scalar_library.h"
#include "gti/persistent_declaration.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace lang;
using namespace lang::driver;
namespace {
void write(const std::filesystem::path &path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(bytes.data(), bytes.size());
  if (!file)
    throw std::runtime_error("cannot write " + path.string());
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::invalid_argument(
          "expected runtime archive and output directory");
    const auto root = std::filesystem::canonical(argv[2]);
    std::filesystem::create_directories(root / "stdlib");
    write(root / "stdlib/prelude.gti", "");
    write(root / "model.gti", R"(export module cost_model;
export namespace cost_model {
struct Point { mut int x = 0; mut int y = 0; };
Point echo(Point value) { return value; }
}
)");
    const auto modelRequest =
        CompilationRequest(
            root / "model.gti", standardLibraryLayout(root / "stdlib"),
            TargetInfo::host(), OptimizationLevel::O3, CppStandard::Cpp23)
            .withBackend(BackendKind::Llvm);
    const auto model = buildCompiledScalarLibraryTarget(
        modelRequest, {"primitive-cost", "model", {"cost_model::echo"}},
        argv[1]);
    if (!model.compilation.succeeded())
      throw std::runtime_error(model.compilation.backendFailureDetails.value_or(
          "cost model producer failed"));
    write(root / "model.gtilib", model.bundle);
    std::filesystem::remove(root / "model.gti");
    write(root / "library.gti", R"(export module primitive_cost;
import "model-api.gti";
export namespace costs {
int64_t identity_i64(int64_t value) { return value; }
double identity_double(double value) { return value; }
void do_nothing() {}
int checked_divide(int value, int divisor) { return value / divisor; }
cost_model::Point identity_point(cost_model::Point value) { return value; }
}
)");
    const auto request =
        CompilationRequest(
            root / "library.gti", standardLibraryLayout(root / "stdlib"),
            TargetInfo::host(), OptimizationLevel::O3, CppStandard::Cpp23)
            .withBackend(BackendKind::Llvm);
    const CompiledLibraryTarget target{
        "primitive-cost",
        "primitive-cost",
        {"costs::identity_i64", "costs::identity_double", "costs::do_nothing",
         "costs::checked_divide", "costs::identity_point"}};
    const std::vector<CompiledLibraryImport> imports{{"primitive-cost", "model",
                                                      root / "model.gtilib",
                                                      root / "model-api.gti"}};
    const auto built =
        buildCompiledScalarLibraryTarget(request, target, argv[1], imports);
    if (!built.compilation.succeeded()) {
      for (const auto &diagnostic : built.compilation.diagnostics)
        std::cerr << diagnostic.message << '\n';
      throw std::runtime_error(built.compilation.backendFailureDetails.value_or(
          "primitive cost producer failed"));
    }
    write(root / "library.o", built.compilation.artifact->contents);
    write(root / "primitive.gtilib", built.bundle);
    // Compiler-owned persistent identities and typed symbol contracts, never
    // inferred from nm ordering or reimplemented by the Python measurement
    // tool.
    const auto signatures = decodeCompiledLibraryInterface(
        decodeLibraryBundle(built.bundle).interfaceMetadata);
    const auto symbol = [&](std::string_view name) {
      for (const auto &signature : signatures) {
        const auto registry =
            compiledInterfaceSyntheticRegistry(signature.typeMetadata);
        if (readPersistentFunctionKey(signature.key, registry).qualifiedName ==
            name)
          return compiledScalarSymbol(signature);
      }
      throw std::runtime_error("missing cost callable " + std::string(name));
    };
    write(root / "symbols.json",
          "{\"int64\":\"" + symbol("costs::identity_i64") + "\",\"double\":\"" +
              symbol("costs::identity_double") + "\",\"void\":\"" +
              symbol("costs::do_nothing") + "\",\"division\":\"" +
              symbol("costs::checked_divide") + "\",\"point\":\"" +
              symbol("costs::identity_point") + "\"}\n");
    std::filesystem::remove(root / "library.gti");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
