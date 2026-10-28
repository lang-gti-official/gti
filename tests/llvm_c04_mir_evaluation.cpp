#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "cpp_backend_test_support.h"
#include "gti/lowered_program_printer.h"
#include "gti/mir_printer.h"
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

void checkNativeControls(
    lang::native_llvm::Session &session,
    const lang::LlvmAdmissionPlan &admission,
    const std::vector<lang::HirFunctionInstanceId> &selected) {
  using namespace lang;
  using namespace lang::native_llvm;
  unsigned checked = 0;
  for (auto address : admission.requiredBodies()) {
    const auto *body = findMirBody(session.program().mir(), address);
    for (const auto &block : body->blocks)
      for (const auto &stage : block.instructions) {
        const auto *callee = stage.functionTarget
                                 ? session.program().mir().findFunctionInstance(
                                       *stage.functionTarget)
                                 : nullptr;
        if (stage.kind != MirInstructionKind::Call || !callee ||
            callee->linkage != LanguageLinkage::C ||
            stage.unsafeOperation != UnsafeOperationKind::ForeignPointerCall)
          continue;
        auto &mutableStage = const_cast<MirInstruction &>(stage);
        const auto original = stage;
        const auto rejectMutation = [&](const auto &mutate) {
          mutate(mutableStage);
          bool rejected = false;
          try {
            validateC04Evaluation(session, admission, selected,
                                  C04EvaluationProfile::NativeBoundary);
          } catch (const BackendUnsupportedError &) {
            rejected = true;
          }
          mutableStage = original;
          if (!rejected)
            throw std::runtime_error("C04 native mutation escaped admission");
          ++checked;
        };
        rejectMutation(
            [](auto &instruction) { instruction.functionTarget.reset(); });
        rejectMutation(
            [](auto &instruction) { instruction.parameterTypes.clear(); });
        rejectMutation([](auto &instruction) { ++instruction.callSite; });
        rejectMutation([](auto &instruction) {
          instruction.unsafeOperation = UnsafeOperationKind::None;
        });
        rejectMutation([&](auto &instruction) {
          instruction.functionTarget = selected.front();
        });
        auto &mutableCallee = const_cast<MirFunctionInstance &>(*callee);
        const auto originalSymbol = callee->externalSymbol;
        mutableCallee.externalSymbol += "_forged";
        bool rejected = false;
        try {
          validateC04Evaluation(session, admission, selected,
                                C04EvaluationProfile::NativeBoundary);
        } catch (const BackendUnsupportedError &) {
          rejected = true;
        }
        mutableCallee.externalSymbol = originalSymbol;
        if (!rejected)
          throw std::runtime_error(
              "C04 forged native symbol escaped admission");
        ++checked;
      }
  }
  if (checked == 0)
    throw std::runtime_error(
        "C04 native controls require a real pointer import");
  validateC04Evaluation(session, admission, selected,
                        C04EvaluationProfile::NativeBoundary);
  std::cout << "native mutation controls: " << checked << '\n';
}

void checkDynamicControls(
    lang::native_llvm::Session &session,
    const lang::LlvmAdmissionPlan &admission,
    const std::vector<lang::HirFunctionInstanceId> &roots) {
  using namespace lang;
  using namespace lang::native_llvm;
  unsigned checked = 0;
  for (const auto &selection :
       {std::vector<HirFunctionInstanceId>{},
        std::vector<HirFunctionInstanceId>{roots.front(), roots.front()},
        std::vector<HirFunctionInstanceId>{0}}) {
    bool rejected = false;
    try {
      validateC04Evaluation(session, admission, selection,
                            C04EvaluationProfile::DynamicCalls);
    } catch (const BackendUnsupportedError &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error(
          "C04 dynamic root omission/forgery escaped admission");
    ++checked;
  }
  const auto bodies = validateC04Evaluation(session, admission, roots,
                                            C04EvaluationProfile::DynamicCalls);
  for (const auto address : bodies)
    for (const auto &block :
         findMirBody(session.program().mir(), address)->blocks)
      for (const auto &stage : block.instructions) {
        if (stage.intrinsic != IntrinsicKind::FunctionPointerFromItem &&
            stage.intrinsic != IntrinsicKind::FunctionPointerInvoke)
          continue;
        auto &changed = const_cast<MirInstruction &>(stage);
        const auto saved = stage;
        const auto rejects = [&](const auto &mutate) {
          mutate(changed);
          bool rejected = false;
          try {
            validateC04Evaluation(session, admission, roots,
                                  C04EvaluationProfile::DynamicCalls);
          } catch (const BackendUnsupportedError &) {
            rejected = true;
          }
          changed = saved;
          if (!rejected)
            throw std::runtime_error("C04 dynamic mutation escaped admission");
          ++checked;
        };
        rejects([](auto &value) { value.parameterTypes.clear(); });
        rejects([](auto &value) { value.operands.clear(); });
        rejects([&](auto &value) { value.functionTarget = roots.front(); });
        rejects([](auto &value) { value.intrinsic = IntrinsicKind::None; });
        if (stage.intrinsic == IntrinsicKind::FunctionPointerFromItem) {
          rejects(
              [](auto &value) { value.functionPointerFunctionTarget.reset(); });
          rejects([&](auto &value) {
            value.functionPointerFunctionTarget = roots.front();
          });
        } else {
          rejects([](auto &value) { value.info.type = SemanticType::Bool; });
        }
      }
  if (!checked)
    throw std::runtime_error("C04 dynamic controls have no actual invocation");
  std::cout << "dynamic mutation controls: " << checked << '\n';
}

void checkWorkerControls(
    lang::native_llvm::Session &session,
    const lang::LlvmAdmissionPlan &admission,
    const std::vector<lang::HirFunctionInstanceId> &roots) {
  using namespace lang;
  using namespace lang::native_llvm;
  unsigned checked = 0;
  const auto validate = [&] {
    return validateC04Evaluation(session, admission, roots,
                                 C04EvaluationProfile::ScopedWorkers);
  };
  const auto rejects = [&](auto &changed, const auto &mutation) {
    const auto saved = changed;
    mutation(changed);
    bool rejected = false;
    try {
      (void)validate();
    } catch (const BackendUnsupportedError &) {
      rejected = true;
    }
    changed = saved;
    if (!rejected)
      throw std::runtime_error("C04 worker mutation escaped admission");
    ++checked;
  };
  for (auto address : validate())
    for (const auto &block :
         findMirBody(session.program().mir(), address)->blocks)
      for (const auto &stage : block.instructions) {
        if (stage.intrinsic != IntrinsicKind::ThreadScope)
          continue;
        auto &instruction = const_cast<MirInstruction &>(stage);
        rejects(instruction, [](auto &v) { v.scopedThreadTasks.clear(); });
        rejects(instruction, [](auto &v) { v.operands.clear(); });
        rejects(instruction, [](auto &v) { v.parameterTypes.clear(); });
        rejects(instruction, [](auto &v) {
          v.synchronization.kind = SynchronizationOperationKind::None;
        });
        rejects(instruction, [](auto &v) {
          v.definedFailure.propagation = FailurePropagationKind::None;
        });
        rejects(instruction, [](auto &v) { v.info.type = SemanticType::Bool; });
        rejects(instruction,
                [&](auto &v) { v.functionTarget = roots.front(); });
        rejects(instruction,
                [](auto &v) { v.scopedThreadTasks[0].dependency = 0; });
        for (const auto &task : stage.scopedThreadTasks) {
          auto &adapter = const_cast<MirThreadTaskAdapter &>(
              *session.program().mir().findThreadTaskAdapter(task.adapter));
          rejects(adapter, [](auto &v) { v.scoped = false; });
          rejects(adapter, [](auto &v) { v.sourceBody.owner = 0; });
          rejects(adapter, [](auto &v) { v.spawnInstruction = 0; });
          rejects(adapter, [](auto &v) { v.spawnValue = 0; });
          rejects(adapter, [](auto &v) { v.scopeArgument = 99; });
          rejects(adapter, [](auto &v) { v.taskType = SemanticType::Int32; });
          rejects(adapter, [](auto &v) { v.resultType = SemanticType::Int32; });
          rejects(adapter, [](auto &v) { v.functionTarget.reset(); });
          rejects(adapter, [&](auto &v) { v.functionTarget = roots.front(); });
          rejects(adapter, [](auto &v) { v.catchesWorkerFailure = false; });
          rejects(adapter, [](auto &v) { v.catchesNativeExceptions = false; });
        }
      }
  if (!checked)
    throw std::runtime_error("C04 worker controls have no sealed task scope");
  (void)validate();
  std::cout << "worker mutation controls: " << checked << '\n';
}
} // namespace

int main(int argc, char **argv) {
  using namespace lang;
  using namespace lang::native_llvm;
  if (argc < 4 || argc > 7)
    return 64;
  try {
    const std::filesystem::path source(argv[1]);
    const std::string prefix(argv[2]);
    const std::string level(argv[3]);
    const std::string boundaries(argc >= 5 ? argv[4] : "fixed");
    const std::string profileName(argc >= 6 ? argv[5] : "scalar-cleanup");
    if (profileName != "scalar-cleanup" && profileName != "native-boundary" &&
        profileName != "dynamic-calls" && profileName != "scoped-workers")
      return 64;
    const auto profile =
        profileName == "native-boundary"  ? C04EvaluationProfile::NativeBoundary
        : profileName == "dynamic-calls"  ? C04EvaluationProfile::DynamicCalls
        : profileName == "scoped-workers" ? C04EvaluationProfile::ScopedWorkers
                                          : C04EvaluationProfile::ScalarCleanup;
    const bool nativeControls =
        argc == 7 && std::string_view(argv[6]) == "--native-contracts";
    const bool dynamicControls =
        argc == 7 && std::string_view(argv[6]) == "--dynamic-contracts";
    const bool workerControls =
        argc == 7 && std::string_view(argv[6]) == "--worker-contracts";
    const bool workers = profile == C04EvaluationProfile::ScopedWorkers;
    const bool standardLibrary =
        profile == C04EvaluationProfile::DynamicCalls || workers ||
        (argc == 7 && std::string_view(argv[6]) == "--standard-library");
    if (argc == 7 && !nativeControls && !dynamicControls && !workerControls &&
        std::string_view(argv[6]) != "--standard-library")
      return 64;
    if (nativeControls && profile != C04EvaluationProfile::NativeBoundary)
      return 64;
    if (dynamicControls && profile != C04EvaluationProfile::DynamicCalls)
      return 64;
    if (workerControls && !workers)
      return 64;
    if (boundaries != "fixed" && boundaries != "optimizable")
      return 64;
    const bool fixedCallBoundaries = boundaries == "fixed";
    if (level != "O0" && level != "O3")
      return 64;
    std::ifstream stream(source);
    if (!stream)
      throw std::runtime_error("cannot read C04 source fixture");
    const std::string text(std::istreambuf_iterator<char>{stream}, {});
    const auto library =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
    FrontendOptions frontendOptions;
    if (workers) {
      frontendOptions.target.executionProfile = ExecutionProfile::Concurrent;
      frontendOptions.target.runtimeCapabilities.threads = true;
    }
    const auto frontend =
        standardLibrary ? Frontend(frontendOptions)
                              .analyze(source.filename().string(), text,
                                       {library / "prelude.gti"}, {}, {library})
                        : Frontend().analyze(source.filename().string(), text);
    if (!frontend.canGenerateCode()) {
      if (!frontend.mirValid)
        write(prefix + ".invalid-mir", MirPrinter().print(frontend.mir));
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      return 65;
    }
    std::vector<LoweredProgramIssue> issues;
    const auto verified = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                               frontendOptions.target),
        issues);
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
    if (profile == C04EvaluationProfile::DynamicCalls || workers) {
      if (!root)
        throw BackendUnsupportedError(
            "C04 dynamic evaluation requires its exact root");
      selected = {root->id};
    }
    validateC04Evaluation(session, admission, selected, profile);
    if (nativeControls)
      checkNativeControls(session, admission, selected);
    if (dynamicControls)
      checkDynamicControls(session, admission, selected);
    if (workerControls)
      checkWorkerControls(session, admission, selected);
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
    session.module().addModuleFlag(llvm::Module::Error,
                                   "gti.c04.fixed-boundaries",
                                   fixedCallBoundaries ? 1u : 0u);
    session.module().addModuleFlag(
        llvm::Module::Error, "gti.c04.native-boundary",
        profile == C04EvaluationProfile::NativeBoundary ? 1u : 0u);
    session.module().addModuleFlag(
        llvm::Module::Error, "gti.c04.dynamic-calls",
        profile == C04EvaluationProfile::DynamicCalls ? 1u : 0u);
    session.module().addModuleFlag(llvm::Module::Error,
                                   "gti.c04.scoped-workers", workers ? 1u : 0u);
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
    lowerC04Evaluation(session, admission, selected, fixedCallBoundaries,
                       profile);
    session.verify();
    // Retain verified input IR: target emission may lower resume instructions
    // to platform unwind-runtime calls in the in-memory module.
    std::string ir;
    llvm::raw_string_ostream irStream(ir);
    session.module().print(irStream, nullptr);
    const auto object = session.emitObject();
    write(prefix + ".ll", ir);
    std::string codegenIr;
    llvm::raw_string_ostream codegenStream(codegenIr);
    session.module().print(codegenStream, nullptr);
    write(prefix + ".codegen.ll", codegenIr);
    write(
        prefix + ".h",
        std::string("#pragma once\n#include <cstdint>\n") +
            "#include <gti/runtime_failure.h>\n"
            "struct Channel { gti_failure_record_v2 record; "
            "const gti_failure_artifact_descriptor_v1* artifact; };\n" +
            "#define GTI_C04_STATUS " + (status ? "1\n" : "0\n") +
            "#define GTI_C04_NATIVE_BOUNDARY " +
            (profile == C04EvaluationProfile::NativeBoundary ? "1\n" : "0\n") +
            "#define GTI_C04_SCOPED_WORKERS " + (workers ? "1\n" : "0\n") +
            "#define GTI_C04_BORROWED_CLEANUP " +
            (borrowedCleanup ? "1\n" : "0\n") + "#define GTI_C04_ENTRY " +
            name + "\n" + "extern \"C\" " + (status ? "bool " : "int32_t ") +
            name + (status ? "(int32_t*, Channel*, int32_t" : "(int32_t") +
            (borrowedCleanup ? ", int32_t*, int32_t*, int32_t*);\n" : ");\n"));
    write(prefix + ".o", object.contents);
    std::cout << GTI_C04_MODE << ' ' << level << ' ' << profileName
              << " verified closure emitted\n";
    return 0;
  } catch (const BackendUnsupportedError &error) {
    std::cerr << error.what() << '\n';
    return 65;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
