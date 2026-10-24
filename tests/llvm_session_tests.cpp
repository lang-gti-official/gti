#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/runtime_abi.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/runtime_failure.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/IRBuilder.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
using namespace lang;
using namespace lang::native_llvm;
int failures = 0;
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
template <typename Exception, typename Action>
void rejects(Action action, const char *message) {
  bool rejected = false;
  try {
    action();
  } catch (const Exception &) {
    rejected = true;
  }
  expect(rejected, message);
}

VerifiedLoweredProgram fixture() {
  const auto frontend = Frontend().analyze("llvm-session.gti", R"(
struct Node { mut Node* next = nullptr; mut int32_t value = 0; };
[[c_abi]] struct Packet { mut uint8_t tag; mut uint64_t value; };
[[c_abi]] union Overlay { mut uint64_t bits; mut double value; };
int recurse(int value) {
  if (value == 0) { return 0; }
  return recurse(value - 1);
}
bool identity(bool value) { return value; }
int main() {
  Node node{};
  uint64_t sizes = sizeof(Packet) + sizeof(Overlay);
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("invalid LLVM session test fixture");
  }
  std::vector<LoweredProgramIssue> issues;
  auto verified = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!verified)
    throw std::runtime_error("unverified LLVM session fixture");
  return *verified;
}

void testLayoutAndAbi(const VerifiedLoweredProgram &input) {
  Session session(input, {});
  auto &layouts = session.layouts();
  auto &abi = session.abi();
  const auto boolean = layouts.get(SemanticType::Bool);
  expect(boolean.value->isIntegerTy(1) && boolean.storage->isIntegerTy(8) &&
             boolean.size == 1,
         "bool has distinct value/storage forms");
  const auto reference =
      layouts.get(SemanticType::referenceTo(SemanticType::Int32));
  expect(reference.value->isPointerTy() && reference.size == 8,
         "a reference transports the selected address");
  const auto array =
      layouts.get(SemanticType::arrayOf(SemanticType::UInt16, 5));
  expect(array.addressOnly && array.size == 10 && array.stride == 2 &&
             array.alignment == 2,
         "arrays retain their exact stride and aggregate storage");
  const auto empty =
      layouts.get(SemanticType::arrayOf(SemanticType::UInt32, 0));
  expect(empty.size == 0 && empty.alignment == 4,
         "zero-sized storage retains alignment");
  rejects<BackendUnsupportedError>(
      [&] {
        static_cast<void>(layouts.get(SemanticType::arrayOf(
            SemanticType::UInt64, std::numeric_limits<std::uint64_t>::max())));
      },
      "oversized arrays reject before LLVM size computation");
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(layouts.get(SemanticType::typeParameter(1))); },
      "unresolved types never acquire a guessed layout");
  for (const auto &instance : input.program().mir().classInstances()) {
    const auto layout = layouts.get(instance.type);
    const auto *declaration =
        input.program().findClassDeclaration(instance.declaration);
    if (declaration->qualifiedName == "Packet")
      expect(layout.size == 16 && layout.alignment == 8 &&
                 layout.fieldOffsets == std::vector<std::uint64_t>({0, 8}),
             "C records agree with sealed offsets, size and alignment");
    if (declaration->qualifiedName == "Overlay")
      expect(layout.size == 8 &&
                 layout.fieldOffsets == std::vector<std::uint64_t>({0, 0}),
             "union fields share offset zero and sealed extent");
    if (declaration->qualifiedName == "Node")
      expect(
          layout.size == 16 &&
              layout.fieldOffsets == std::vector<std::uint64_t>({0, 8}),
          "recursive pointer records are planned without recursive expansion");
  }
  rejects<BackendUnsupportedError>(
      [&] {
        verifyTargetLayout(input.program().target(),
                           llvm::DataLayout("E-p:64:64"), session.context());
      },
      "wrong endianness rejects");
  rejects<BackendUnsupportedError>(
      [&] {
        verifyTargetLayout(input.program().target(),
                           llvm::DataLayout("e-p:32:32"), session.context());
      },
      "wrong pointer width rejects");
  rejects<BackendUnsupportedError>(
      [&] {
        verifyTargetLayout(input.program().target(),
                           llvm::DataLayout("e-p:64:64-i64:32:32"),
                           session.context());
      },
      "wrong scalar alignment rejects");
  const auto direct = abi.gti(SemanticType::Int32, {}, false);
  expect(direct.type->getReturnType()->isIntegerTy(32) &&
             direct.parameters.empty(),
         "failure-free scalar returns directly");
  const auto status = abi.gti(SemanticType::Int32, {}, true, true, true);
  expect(status.statusResult && status.type->getReturnType()->isIntegerTy(1) &&
             status.resultDestination == 0 && status.failureDestination == 1 &&
             status.parameters.size() == 5 &&
             status.parameters[2].channel == AbiChannel::Receiver &&
             status.parameters[3].channel == AbiChannel::CallerSite,
         "failure-capable ABI has explicit result/failure/receiver/caller "
         "channels");
  const auto aggregate =
      abi.gti(SemanticType::arrayOf(SemanticType::Int32, 2), {}, false);
  expect(aggregate.resultDestination == 0 &&
             aggregate.type->getReturnType()->isVoidTy(),
         "address-only returns use a destination without native C sret "
         "assumptions");
  const auto borrowed =
      abi.gti(SemanticType::referenceTo(SemanticType::Int32), {}, false);
  expect(borrowed.type->getReturnType()->isPointerTy() &&
             !borrowed.resultDestination,
         "reference results are addresses, not copied referents");
  const std::vector<SemanticType> narrow{SemanticType::Bool, SemanticType::Int8,
                                         SemanticType::UInt16};
  const auto native = abi.nativeC(SemanticType::Bool, narrow);
  expect(native.resultExtension == AbiExtension::Unsigned &&
             native.parameters[1].extension == AbiExtension::Signed &&
             native.parameters[2].extension == AbiExtension::Unsigned,
         "native narrow scalar ABI retains required extension attributes");
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(abi.nativeC(SemanticType::StringView, {})); },
      "native aggregates cannot borrow private GTI classification");
  const auto *record =
      session.dataLayout().getStructLayout(abi.failureRecord());
  expect(record->getSizeInBytes() == sizeof(gti_failure_record_v2) &&
             record->getElementOffset(6) ==
                 offsetof(gti_failure_record_v2, message_size) &&
             record->getElementOffset(8) ==
                 offsetof(gti_failure_record_v2, message),
         "failure record matches the versioned C ABI");
  const auto *artifact =
      session.dataLayout().getStructLayout(abi.failureArtifact());
  expect(artifact->getSizeInBytes() ==
                 sizeof(gti_failure_artifact_descriptor_v1) &&
             artifact->getElementOffset(7) ==
                 offsetof(gti_failure_artifact_descriptor_v1,
                          canonical_descriptor_size),
         "failure descriptor matches the versioned C ABI");
  expect(session.dataLayout().getTypeAllocSize(abi.failureSite()) ==
                 sizeof(gti_failure_site_descriptor_v1) &&
             session.dataLayout().getTypeAllocSize(abi.failureOutcome()) ==
                 sizeof(gti_failure_outcome_descriptor_v1) &&
             session.dataLayout().getTypeAllocSize(abi.failureEmergency()) ==
                 sizeof(gti_failure_emergency_v2) &&
             session.dataLayout().getTypeAllocSize(abi.textView()) ==
                 sizeof(gti_c_string_view),
         "all required runtime records agree with native C sizes");
  const auto *begin =
      declareRuntimeService(session, RuntimeService::BeginThreadLocals);
  expect(begin->arg_empty() && begin->getReturnType()->isVoidTy(),
         "startup is an actual C void entry");
  const auto *terminate =
      declareRuntimeService(session, RuntimeService::TerminateFailure);
  expect(terminate->arg_size() == 4 && terminate->doesNotReturn(),
         "failure containment uses the versioned C terminal ABI");
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(session.context()), false),
      llvm::GlobalValue::ExternalLinkage, "gti_rt_failure_validate_v2",
      session.module());
  rejects<std::logic_error>(
      [&] {
        static_cast<void>(
            declareRuntimeService(session, RuntimeService::ValidateFailure));
      },
      "runtime declarations reject conflicting exact C signatures");
  std::vector<HirFunctionInstanceId> ids;
  for (const auto &function : input.program().mir().functionInstances())
    ids.push_back(function.id);
  abi.declareFunctions(ids);
  for (const auto id : ids) {
    auto *function = abi.declareFunction(id);
    expect(function->isDeclaration() && function->getMetadata("gti.source"),
           "all function signatures/provenance exist before body lowering");
  }
  session.verify();
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(session.emitObject()); },
      "unlowered planned definitions cannot escape as linkable objects");
  rejects<BackendUnsupportedError>(
      [&] { Session invalid(input, {.debugInfo = true}); },
      "debug requests do not silently lose debug metadata");
  rejects<std::invalid_argument>(
      [&] {
        Session invalid(
            input, {.optimization = static_cast<LlvmCodegenOptimization>(99)});
      },
      "invalid optimization identity rejects");
}

void testInvalidModule(const VerifiedLoweredProgram &input) {
  Session session(input, {});
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(session.context()), false),
      llvm::GlobalValue::ExternalLinkage, "malformed", session.module());
  llvm::BasicBlock::Create(session.context(), "unterminated", function);
  rejects<std::logic_error>(
      [&] { session.verify(); },
      "LLVM verifier rejects malformed CFG before code generation");
  rejects<std::logic_error>([&] { static_cast<void>(session.emitObject()); },
                            "malformed modules cannot publish object bytes");
}

void testDiagnostics(const VerifiedLoweredProgram &input) {
  struct Diagnostic final : llvm::DiagnosticInfo {
    explicit Diagnostic(llvm::DiagnosticSeverity severity)
        : DiagnosticInfo(llvm::DK_FirstPluginKind, severity) {}
    void print(llvm::DiagnosticPrinter &printer) const override {
      printer << "LLVM session diagnostic probe";
    }
  };
  Session session(input, {});
  session.context().diagnose(Diagnostic(llvm::DS_Warning));
  expect(session.diagnostics().size() == 1 &&
             session.diagnostics().front().find("diagnostic probe") !=
                 std::string::npos,
         "LLVM diagnostics stay in the owning session");
  session.verify();
  session.context().diagnose(Diagnostic(llvm::DS_Error));
  rejects<std::runtime_error>([&] { session.verify(); },
                              "LLVM errors prevent artifact publication");
  Session separate(input, {});
  expect(separate.diagnostics().empty(),
         "independent sessions do not share diagnostics or IR");
  separate.module().setDataLayout("e-p:32:32");
  rejects<std::logic_error>(
      [&] { separate.verify(); },
      "module target-layout mutation cannot bypass the target contract");
}

void testObjectHandoff(const VerifiedLoweredProgram &input) {
  namespace fs = std::filesystem;
  const auto root =
      fs::temp_directory_path() /
      ("gti-llvm-object-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(root);
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  } cleanup{root};
  const std::string consumer = R"(
#include <stdint.h>
#include <stdbool.h>
extern const unsigned char gti_llvm_object_probe[8];
extern int8_t gti_llvm_echo_i8(int8_t);
extern uint16_t gti_llvm_echo_u16(uint16_t);
extern bool gti_llvm_echo_bool(bool);
extern double gti_llvm_echo_double(double);
extern int8_t gti_llvm_call_c_i8(int8_t);
int8_t native_c_echo_i8(int8_t value) { return value; }
int main(void) {
  const unsigned char expected[8] = {0, 128, 255, 13, 10, 0, 26, 127};
  for (int i = 0; i < 8; ++i) if (gti_llvm_object_probe[i] != expected[i]) return 1;
  if (gti_llvm_echo_i8(-127) != -127 || gti_llvm_echo_i8(126) != 126) return 2;
  if (gti_llvm_echo_u16(65535) != 65535) return 3;
  if (!gti_llvm_echo_bool(true) || gti_llvm_echo_bool(false)) return 4;
  if (gti_llvm_echo_double(-3.5) != -3.5) return 5;
  if (gti_llvm_call_c_i8(-127) != -127) return 6;
  return 0;
}
)";
  const auto csource = root / "consumer.c";
  expect(driver::writeArtifact(csource, consumer) ==
             driver::ArtifactWriteStatus::Success,
         "write C object consumer");
  driver::NativeToolchain toolchain;
  expect(toolchain
             .invoke(driver::NativeCCompileRequest(
                 driver::discoverCCompiler(), csource, root / "consumer.o",
                 driver::CStandard::C17, OptimizationLevel::O2, {}, {}))
             .succeeded(),
         "compile native object consumer");
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    Session session(input, {.optimization = level});
    auto &abi = session.abi();
    // Native ABI oracle only: these identity bodies are not GTI instruction
    // lowering, a hosted entry, or evidence of any admitted language family.
    for (const auto &[name, type] :
         std::vector<std::pair<std::string, SemanticType>>{
             {"gti_llvm_echo_i8", SemanticType::Int8},
             {"gti_llvm_echo_u16", SemanticType::UInt16},
             {"gti_llvm_echo_bool", SemanticType::Bool},
             {"gti_llvm_echo_double", SemanticType::Double}}) {
      const std::vector<SemanticType> parameters{type};
      const auto plan = abi.nativeC(type, parameters);
      auto *function =
          llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                                 name, session.module());
      plan.applyTo(*function);
      auto *block =
          llvm::BasicBlock::Create(session.context(), "entry", function);
      llvm::ReturnInst::Create(session.context(), function->getArg(0), block);
    }
    const std::vector<std::uint8_t> payload{0, 128, 255, 13, 10, 0, 26, 127};
    const std::vector<SemanticType> cParameters{SemanticType::Int8};
    const auto cPlan = abi.nativeC(SemanticType::Int8, cParameters);
    auto *cTarget =
        llvm::Function::Create(cPlan.type, llvm::GlobalValue::ExternalLinkage,
                               "native_c_echo_i8", session.module());
    cPlan.applyTo(*cTarget);
    auto *wrapper =
        llvm::Function::Create(cPlan.type, llvm::GlobalValue::ExternalLinkage,
                               "gti_llvm_call_c_i8", session.module());
    cPlan.applyTo(*wrapper);
    llvm::IRBuilder<> builder(
        llvm::BasicBlock::Create(session.context(), "entry", wrapper));
    auto *call = builder.CreateCall(cTarget, {wrapper->getArg(0)});
    cPlan.applyTo(*call);
    builder.CreateRet(call);
    auto *constant = llvm::ConstantDataArray::get(session.context(), payload);
    new llvm::GlobalVariable(session.module(), constant->getType(), true,
                             llvm::GlobalValue::ExternalLinkage, constant,
                             "gti_llvm_object_probe");
    const auto object = session.emitObject();
    expect(object.kind == BackendArtifactKind::Object &&
               object.extension == ".o" && object.contents.size() > 100 &&
               object.contents.find('\0') != std::string::npos,
           "target machine emits actual binary object data at every "
           "optimization level");
    const auto staged = root / "probe.staged";
    const auto published = root / "probe.o";
    expect(driver::writeArtifact(staged, object.contents) ==
                   driver::ArtifactWriteStatus::Success &&
               driver::publishArtifact(staged, published).succeeded(),
           "driver atomically publishes object bytes");
    std::ifstream stream(published, std::ios::binary);
    const std::string restored{std::istreambuf_iterator<char>(stream), {}};
    expect(
        restored == object.contents,
        "object handoff preserves every byte including NUL and high-bit bytes");
    driver::NativeInputs inputs;
    inputs.libraryFiles.push_back(root / "consumer.o");
    const auto executable = root / "consumer";
    expect(
        toolchain
            .invoke(driver::NativeLinkRequest(driver::discoverNativeCompiler(),
                                              published, executable, inputs))
            .succeeded(),
        "NativeLinkRequest links LLVM objects without a source compilation "
        "step");
    expect(driver::invokeProcess({executable.string()}).succeeded(),
           "linked C consumer observes exact object bytes");
  }
}
} // namespace

int main() {
  try {
    const auto input = fixture();
    testLayoutAndAbi(input);
    testInvalidModule(input);
    testDiagnostics(input);
    testObjectHandoff(input);
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return failures == 0 ? 0 : 1;
}
