#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "cpp_backend_test_support.h"
#include "gti/lowered_program_printer.h"
#include "gti/runtime_failure.h"

#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
struct Channel {
  gti_failure_record_v2 record;
  const gti_failure_artifact_descriptor_v1 *artifact;
};
void write(const std::filesystem::path &path, const std::string &bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output)
    throw std::runtime_error("cannot write C04 evaluation artifact");
}
} // namespace

int main(int argc, char **argv) {
  using namespace lang;
  using namespace lang::native_llvm;
  if (argc != 4)
    return 64;
  try {
    const std::filesystem::path source(argv[1]);
    const std::string prefix(argv[2]);
    const std::string level(argv[3]);
    if (level != "O0" && level != "O3")
      return 64;
    std::ifstream stream(source);
    if (!stream)
      throw std::runtime_error("cannot read C04 source fixture");
    const std::string text(std::istreambuf_iterator<char>{stream}, {});
    const auto frontend = Frontend().analyze(source.filename().string(), text);
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      return 65;
    }
    std::vector<LoweredProgramIssue> issues;
    const auto verified = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
    if (!verified)
      throw std::runtime_error("C04 source did not produce a sealed program");
    const LlvmBackendOptions options{
        .optimization = level == "O0" ? LlvmCodegenOptimization::None
                                      : LlvmCodegenOptimization::Aggressive};
    const auto admission = planLlvmAdmission(*verified, options);
    Session session(*verified, options);
    write(prefix + ".lowered",
          LoweredProgramPrinter().print(verified->program()));
    std::vector<HirFunctionInstanceId> selected;
    const MirFunctionInstance *root = nullptr;
    for (const auto &function : verified->program().mir().functionInstances()) {
      if (function.definitionKind != MirDefinitionKind::Source ||
          function.linkage != LanguageLinkage::Gti || function.owner)
        continue;
      selected.push_back(function.id);
      const auto *declaration =
          verified->program().findFunctionDeclaration(function.declaration);
      if (declaration && declaration->qualifiedName == "sample")
        root = &function;
    }
    // Reject before writing an object, including before the test declaration
    // planner could accidentally make an unsupported return look compatible.
    validateC04Evaluation(session, admission, selected);
    const auto reference =
        SemanticType::referenceTo(SemanticType::Int32, AccessMode::Mutable);
    const bool borrowedCleanup =
        root && root->parameterTypes ==
                    std::vector<SemanticType>{SemanticType::Int32, reference,
                                              reference, reference};
    if (!root || root->returnType != SemanticType::Int32 ||
        (!borrowedCleanup &&
         root->parameterTypes !=
             std::vector<SemanticType>{SemanticType::Int32}) ||
        !root->mayRaiseDefinedFailure || root->tracksCaller)
      throw BackendUnsupportedError(
          "C04 driver requires a fallible int32 -> int32 root");
    const auto abi =
        session.abi().gti(root->returnType, root->parameterTypes, true);
    const bool status = std::string_view(GTI_C04_MODE) == "status";
    session.module().addModuleFlag(llvm::Module::Error, "gti.c04.transport",
                                   status ? 0u : 1u);
    if (abi.statusResult != status ||
        abi.parameters.size() !=
            root->parameterTypes.size() + (status ? 2 : 0) ||
        abi.resultDestination.has_value() != status ||
        abi.failureDestination.has_value() != status ||
        (status && (abi.resultDestination != 0 || abi.failureDestination != 1)))
      throw std::runtime_error("C04 root ABI disagrees with its driver");
    const auto *layout =
        session.dataLayout().getStructLayout(session.abi().failureChannel());
    if (layout->getSizeInBytes() != sizeof(Channel) ||
        layout->getElementOffset(1) != offsetof(Channel, artifact) ||
        session.dataLayout().getTypeAllocSize(session.abi().failureRecord()) !=
            sizeof(gti_failure_record_v2))
      throw std::runtime_error(
          "C04 C++ carrier differs from the exact generated channel layout");
    const auto name = session.abi().declareFunction(root->id)->getName().str();
    lowerC04Evaluation(session, admission, selected);
    session.verify();
    // Retain verified input IR: target emission may lower resume instructions
    // to platform unwind-runtime calls in the in-memory module.
    std::string ir;
    llvm::raw_string_ostream irStream(ir);
    session.module().print(irStream, nullptr);
    const auto object = session.emitObject();
    write(prefix + ".ll", ir);
    write(
        prefix + ".h",
        std::string("#pragma once\n#include <cstdint>\n") +
            "#include <gti/runtime_failure.h>\n"
            "struct Channel { gti_failure_record_v2 record; "
            "const gti_failure_artifact_descriptor_v1* artifact; };\n" +
            "#define GTI_C04_STATUS " + (status ? "1\n" : "0\n") +
            "#define GTI_C04_BORROWED_CLEANUP " +
            (borrowedCleanup ? "1\n" : "0\n") + "#define GTI_C04_ENTRY " +
            name + "\n" + "extern \"C\" " + (status ? "bool " : "int32_t ") +
            name + (status ? "(int32_t*, Channel*, int32_t" : "(int32_t") +
            (borrowedCleanup ? ", int32_t*, int32_t*, int32_t*);\n" : ");\n"));
    write(prefix + ".o", object.contents);
    std::cout << GTI_C04_MODE << ' ' << level
              << " verified scalar closure emitted\n";
    return 0;
  } catch (const BackendUnsupportedError &error) {
    std::cerr << error.what() << '\n';
    return 65;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
