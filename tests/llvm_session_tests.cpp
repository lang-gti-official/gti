#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/places.h"
#include "../src/compiler/llvm/runtime_abi.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/mir_verifier.h"
#include "gti/runtime_execution.h"
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

namespace lang {
struct MirProgramTestAccess {
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
};
} // namespace lang

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
enum Data { empty, number(int value), pair(uint8_t tag, uint64_t value), node(Node value), flag(bool value) };
int recurse(int value) {
  if (value == 0) { return 0; }
  return recurse(value - 1);
}
bool identity(bool value) { return value; }
int place_components(mut int& input, int offset) {
  mut int values[3] = {4, 5, 6};
  mut int& selected = values[offset];
  selected = input;
  return values[1];
}
int raw_components(int* input, int offset) {
  unsafe { return input[offset]; }
}
[[borrowed_from(owner)]] unsafe mut int& raw_claim_components(int* address, mut int& owner) {
  unsafe { return *address; }
}
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
  expect(layouts.get(SemanticType::StringView).value == abi.textView(),
         "text literals, storage, PHIs and ABI values share one physical type");
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
  expect(input.program().mir().payloadEnums().size() == 1,
         "layout fixture retains its concrete payload definition");
  for (const auto &payload : input.program().mir().payloadEnums()) {
    const auto &layout = layouts.payload(payload.type);
    expect(layout.type.addressOnly && layout.type.value->isPointerTy() &&
               layout.type.size == 24 && layout.type.alignment == 8 &&
               layout.type.fieldOffsets == std::vector<std::uint64_t>({0, 8}),
           "payload storage shares one aligned overlap after its exact tag");
    expect(layout.variants.size() == 5 &&
               layout.variants[0]->getNumElements() == 0 &&
               layout.variants[4]->getElementType(0)->isIntegerTy(8),
           "unit alternatives and bool payload storage retain exact shapes");
    expect(&layout == &layouts.payload(payload.type) &&
               layouts.get(payload.type).storage == layout.storage,
           "all payload layout consumers share a session-owned plan");
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

// Exercise the private address component independently of program admission.
// Its place/loan identities come from a verified real source body. This is a
// component oracle, not an alternate executable GTI backend or coverage claim.
void emitPlaceComponentProbe(Session &session) {
  const auto &program = session.program().mir();
  const MirFunctionInstance *source = nullptr;
  for (const auto &candidate : program.functionInstances())
    if (candidate.parameterTypes.size() == 2 &&
        candidate.parameterTypes[0].kind == SemanticType::Reference &&
        candidate.parameterTypes[1] == SemanticType::Int32)
      source = &candidate;
  if (!source)
    throw std::logic_error("missing place-component source fixture");
  const auto &body = *findMirBody(program, {MirBodyKind::Function, source->id});
  const MirPlace *array = nullptr, *indexed = nullptr, *referent = nullptr;
  for (const auto &place : body.places) {
    if (place.type.kind == SemanticType::Array && place.projections.empty())
      array = &place;
    if (place.projections.size() == 1 &&
        place.projections[0].kind == MirProjectionKind::Index &&
        !place.projections[0].constantIndex)
      indexed = &place;
    if (place.binding == source->parameterBindings[0] &&
        place.projections.size() == 1)
      referent = &place;
  }
  if (!array || !indexed || !referent)
    throw std::logic_error("place-component fixture lost selected addresses");
  MirLoanId selected = 0;
  for (const auto &loan : body.loans)
    if (loan.kind == MirLoanKind::Local && !loan.entry)
      selected = loan.id;
  if (!selected)
    throw std::logic_error("place-component fixture lost its local loan");
  const auto pointer = SemanticType::rawPointerTo(SemanticType::Int32);
  const std::vector<SemanticType> parameters{pointer, SemanticType::Int32};
  const auto signature = session.abi().nativeC(SemanticType::Int32, parameters);
  auto *function = llvm::Function::Create(
      signature.type, llvm::GlobalValue::ExternalLinkage,
      "gti_llvm_place_component_probe", session.module());
  signature.applyTo(*function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  std::vector<llvm::Value *> values(body.values.size() + 1);
  values.at(indexed->projections[0].index) = function->getArg(1);
  Places places(
      session, ir, body, values,
      [&](llvm::Value *failed, DefinedFailureOutcome outcome) {
        expect(outcome ==
                   DefinedFailureOutcome{DefinedFailureCode::IndexOutOfBounds,
                                         DefinedFailureDetail::FixedArray},
               "array component requests only the fixed-array bound failure");
        auto *failure = llvm::BasicBlock::Create(session.context(),
                                                 "bound.failed", function);
        auto *success = llvm::BasicBlock::Create(session.context(),
                                                 "bound.success", function);
        ir.CreateCondBr(failed, failure, success);
        ir.SetInsertPoint(failure);
        ir.CreateRet(ir.getInt32(-77));
        ir.SetInsertPoint(success);
      });
  places.allocate({});
  places.parameter({.binding = source->parameterBindings[0],
                    .type = source->parameterTypes[0]},
                   function->getArg(0), false);
  places.parameter({.binding = source->parameterBindings[1],
                    .type = source->parameterTypes[1]},
                   function->getArg(1), false);
  auto *storageType =
      llvm::cast<llvm::ArrayType>(session.layouts().get(array->type).storage);
  auto *initial = llvm::ConstantArray::get(
      storageType, {ir.getInt32(4), ir.getInt32(5), ir.getInt32(6)});
  places.store(initial, places.address(array->id), array->type);
  places.publish(selected, places.address(indexed->id));
  auto *input = places.load(places.address(referent->id), SemanticType::Int32);
  places.store(input, places.loan(selected), SemanticType::Int32);
  // Mutating the index SSA mapping cannot reseat an already selected loan.
  values.at(indexed->projections[0].index) = ir.getInt32(2);
  auto *one = places.index(places.address(array->id), array->type,
                           ir.getInt64(1), true);
  ir.CreateRet(
      ir.CreateAdd(places.load(places.loan(selected), SemanticType::Int32),
                   places.load(one, SemanticType::Int32)));
  session.requireDefinition(*function);
}

void emitRawAddressComponentProbe(Session &session) {
  const auto &program = session.program().mir();
  const MirFunctionInstance *source = nullptr;
  for (const auto &candidate : program.functionInstances())
    if (candidate.parameterTypes.size() == 2 &&
        candidate.parameterTypes[0].kind == SemanticType::RawPointer &&
        candidate.parameterTypes[1] == SemanticType::Int32)
      source = &candidate;
  if (!source)
    throw std::logic_error("missing raw-address component source fixture");
  const auto &body = *findMirBody(program, {MirBodyKind::Function, source->id});
  const MirPlace *indexed = nullptr;
  for (const auto &place : body.places)
    if (place.root == MirPlaceRootKind::Value &&
        place.projections.size() == 1 &&
        place.projections[0].kind == MirProjectionKind::RawIndex)
      indexed = &place;
  if (!indexed)
    throw std::logic_error("missing verified raw-index place");
  const auto signature =
      session.abi().nativeC(SemanticType::Int32, source->parameterTypes);
  auto *function = llvm::Function::Create(
      signature.type, llvm::GlobalValue::ExternalLinkage,
      "gti_llvm_raw_address_component_probe", session.module());
  signature.applyTo(*function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  std::vector<llvm::Value *> values(body.values.size() + 1);
  values.at(indexed->value) = function->getArg(0);
  values.at(indexed->projections[0].index) = function->getArg(1);
  Places places(
      session, ir, body, values, [](llvm::Value *, DefinedFailureOutcome) {
        throw std::logic_error(
            "raw-address obligations must not become checked-array guards");
      });
  places.allocate({});
  auto *address = places.address(indexed->id);
  auto *advanced = places.advance(function->getArg(0), SemanticType::Int32,
                                  function->getArg(1), true, false);
  auto *restored = places.advance(advanced, SemanticType::Int32,
                                  function->getArg(1), true, true);
  auto *valid = ir.CreateAnd(ir.CreateICmpEQ(address, advanced),
                             ir.CreateICmpEQ(restored, function->getArg(0)));
  valid = ir.CreateAnd(
      valid,
      ir.CreateICmpEQ(
          places.difference(advanced, function->getArg(0), SemanticType::Int32),
          ir.CreateSExt(function->getArg(1), ir.getInt64Ty())));
  auto *value = places.load(address, SemanticType::Int32);
  ir.CreateRet(ir.CreateSelect(valid, value, ir.getInt32(-99)));
  session.requireDefinition(*function);
}

void emitRawClaimComponentProbe(Session &session) {
  const auto &program = session.program().mir();
  const MirBody *body = nullptr;
  const MirInstruction *claim = nullptr;
  for (const auto &candidate : program.functionInstances()) {
    const auto *current =
        findMirBody(program, {MirBodyKind::Function, candidate.id});
    if (!current)
      continue;
    for (const auto &block : current->blocks)
      for (const auto &stage : block.instructions)
        if (stage.intrinsic == IntrinsicKind::RawBorrowMut) {
          body = current;
          claim = &stage;
        }
  }
  if (!claim || !claim->loan ||
      claim->borrowOrigin != BorrowOriginKind::Argument ||
      claim->borrowArgument != 1)
    throw std::logic_error(
        "missing verified owner-backed raw reference fixture");
  const std::vector<SemanticType> parameters{
      SemanticType::rawPointerTo(SemanticType::Int32),
      SemanticType::rawPointerTo(SemanticType::Int32)};
  const auto signature = session.abi().nativeC(SemanticType::Int32, parameters);
  auto *function = llvm::Function::Create(
      signature.type, llvm::GlobalValue::ExternalLinkage,
      "gti_llvm_raw_claim_component_probe", session.module());
  signature.applyTo(*function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  std::vector<llvm::Value *> values(body->values.size() + 1);
  Places places(session, ir, *body, values,
                [](llvm::Value *, DefinedFailureOutcome) {
                  throw std::logic_error(
                      "verified raw assertion has no runtime failure check");
                });
  places.allocate({});
  // The owner protects the referent but is not its physical address. Publish
  // the already evaluated pointer, never the witness's protected source place.
  places.publish(*claim->loan, function->getArg(0));
  places.store(ir.getInt32(39), places.loan(*claim->loan), SemanticType::Int32);
  ir.CreateRet(places.load(function->getArg(1), SemanticType::Int32));
  session.requireDefinition(*function);
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
extern int32_t gti_llvm_place_component_probe(int32_t*, int32_t);
extern int32_t gti_llvm_raw_address_component_probe(int32_t*, int32_t);
extern int32_t gti_llvm_raw_claim_component_probe(int32_t*, int32_t*);
int8_t native_c_echo_i8(int8_t value) { return value; }
int main(void) {
  const unsigned char expected[8] = {0, 128, 255, 13, 10, 0, 26, 127};
  for (int i = 0; i < 8; ++i) if (gti_llvm_object_probe[i] != expected[i]) return 1;
  if (gti_llvm_echo_i8(-127) != -127 || gti_llvm_echo_i8(126) != 126) return 2;
  if (gti_llvm_echo_u16(65535) != 65535) return 3;
  if (!gti_llvm_echo_bool(true) || gti_llvm_echo_bool(false)) return 4;
  if (gti_llvm_echo_double(-3.5) != -3.5) return 5;
  if (gti_llvm_call_c_i8(-127) != -127) return 6;
  int32_t claimed = 11, owner = 77;
  if (gti_llvm_raw_claim_component_probe(&claimed, &owner) != 77 || claimed != 39 || owner != 77) return 13;
  int32_t input = 9;
  if (gti_llvm_place_component_probe(&input, 1) != 18 || input != 9) return 7;
  if (gti_llvm_place_component_probe(&input, 0) != 14 || input != 9) return 8;
  if (gti_llvm_place_component_probe(&input, -1) != -77 ||
      gti_llvm_place_component_probe(&input, 3) != -77) return 9;
  int32_t data[4] = {11, 22, 33, 44};
  if (gti_llvm_raw_address_component_probe(data, 2) != 33 ||
      gti_llvm_raw_address_component_probe(data + 1, -1) != 11 ||
      gti_llvm_raw_address_component_probe(data + 1, 2) != 44) return 10;
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
    emitPlaceComponentProbe(session);
    emitRawAddressComponentProbe(session);
    emitRawClaimComponentProbe(session);
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
    const auto executed = driver::invokeProcess({executable.string()});
    expect(executed.succeeded(),
           ("linked C consumer observes exact object bytes (exit " +
            std::to_string(executed.exitCode) + ")")
               .c_str());
  }
}
// Real source -> verified lowered bodies -> private native body component.
// Admission is tested separately; this does not grant a public coverage row.
void testClassBodyComponent() {
  const auto frontend = Frontend().analyze("llvm-class-component.gti", R"(
mut int events = 0;
class Ticket {
public:
  mut int value = 1;
  Ticket(int input) : value(input) { events = events * 10 + 1; }
  ~Ticket() { events = events * 10 + 2; }
  int read() { return this.value; }
};
int divide(int value) { return 1 / value; }
class Field {
  int id;
  mut Field* constructed_at = nullptr;
public:
  Field(int id, bool fail) : id(id) {
    this.constructed_at = &this;
    if (fail) { [[discard]] divide(0); }
  }
  Field(Field&) = delete;
  Field(Field&&) = delete;
  ~Field() {
    if (this.constructed_at != &this) { events = 9; }
    events = events * 10 + this.id;
  }
};
class Partial {
  Field first;
  Field second;
  Field third;
public:
  Partial(bool fail) : first(Field(1, false)), second(Field(2, false)),
                       third(Field(3, fail)) {}
  ~Partial() { events = events * 10 + 4; }
};
int rollback(bool fail) { Partial value{fail}; return 0; }
class Element {
  int id;
  mut Element* constructed_at = nullptr;
public:
  Element(int id, bool fail) : id(id) {
    this.constructed_at = &this;
    if (fail) { [[discard]] divide(0); }
  }
  ~Element() {
    if (this.constructed_at != &this) { events = 9; }
    events = events * 10 + this.id;
  }
};
int array_rollback(bool fail) {
  Field values[3] = {Field(1, false), Field(2, false), Field(3, fail)};
  return 0;
}
int nested_array_rollback(bool fail) {
  Field values[2][2] = {{Field(1, false), Field(2, false)},
                        {Field(3, fail), Field(4, false)}};
  return 0;
}
mut int& select_reference(mut int& value, bool fail) {
  if (fail) { [[discard]] divide(0); }
  return value;
}
class Referenced {
  Field first;
  mut int& selected;
  Field last;
public:
  Referenced(mut int& value, bool fail) : first(Field(1, false)),
      selected(select_reference(value, fail)), last(Field(2, false)) {}
  void update() mut { this.selected = 42; }
};
int reference_rollback(bool fail) {
  mut int value = 0;
  { mut Referenced result = Referenced(value, fail); result.update(); }
  return value == 42 ? 0 : 1;
}
void consume_element(Element element, int other) {}
int argument_rollback(bool fail) {
  consume_element(Element(1, false), divide(fail ? 0 : 1));
  return 0;
}
class Cargo {
  int id;
public:
  Cargo(int id) : id(id) {}
  ~Cargo() { events = events * 10 + this.id; }
};
void consume_array(Cargo values[2], int other) {}
int array_argument_rollback(bool fail) {
  consume_array({Cargo(1), Cargo(2)}, divide(fail ? 0 : 1));
  return 0;
}
class Exit {
  bool fail;
public:
  Exit(bool fail) : fail(fail) {}
  ~Exit() { events = 4; if (this.fail) { [[discard]] divide(0); } }
};
Field return_rollback(bool fail) { Exit guard{fail}; return Field(5, false); }
int main() {
  { Ticket ticket{7}; if (ticket.read() != 7) { return 1; } }
  if (events != 12) { return 2; }
  events = 0;
  [[discard]] rollback(false);
  if (events != 4321) { return 3; }
  events = 0;
  [[discard]] array_rollback(false);
  if (events != 321) { return 4; }
  events = 0;
  [[discard]] nested_array_rollback(false);
  if (events != 4321) { return 8; }
  events = 0;
  if (reference_rollback(false) != 0 || events != 21) { return 9; }
  events = 0;
  [[discard]] argument_rollback(false);
  if (events != 1) { return 5; }
  events = 0;
  [[discard]] array_argument_rollback(false);
  if (events != 21) { return 6; }
  events = 0;
  { Field result = return_rollback(false); }
  return events == 45 ? 0 : 7;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    throw std::runtime_error("invalid native class component fixture");
  }
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  if (!program)
    throw std::runtime_error("unverified native class component fixture");
  const auto root =
      std::filesystem::temp_directory_path() /
      ("gti-llvm-class-component-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  const auto layout = driver::discoverToolchainLayout(nullptr);
  driver::NativeToolchain toolchain;
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    Session session(*program, {.optimization = level});
    const auto admission = planLlvmAdmission(*program);
    expect(admission.admitted(), "real class component has public admission");
    lowerScalarProgram(session, admission);
    auto *registration = llvm::StructType::getTypeByName(
        session.context(), "gti_thread_local_cleanup_entry_v1");
    const auto *shape = session.dataLayout().getStructLayout(registration);
    expect(shape->getSizeInBytes() ==
                   sizeof(gti_thread_local_cleanup_entry_v1) &&
               shape->getElementOffset(1) ==
                   offsetof(gti_thread_local_cleanup_entry_v1, storage) &&
               shape->getElementOffset(2) ==
                   offsetof(gti_thread_local_cleanup_entry_v1, cleanup) &&
               shape->getElementOffset(3) ==
                   offsetof(gti_thread_local_cleanup_entry_v1, artifact) &&
               shape->getElementOffset(4) ==
                   offsetof(gti_thread_local_cleanup_entry_v1, registered),
           "native TLS registration agrees with the public C record layout");
    // A test-only C main observes the real checked GTI call after failure,
    // rather than inferring cleanup from the process's terminal exit code.
    auto *original = session.module().getFunction("main");
    original->setName("gti_class_success_oracle");
    auto *main = llvm::Function::Create(original->getFunctionType(),
                                        llvm::GlobalValue::ExternalLinkage,
                                        "main", session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", main));
    auto *success = ir.CreateCall(original);
    const auto symbol = program->program()
                            .mir()
                            .programInitializationPlan()
                            .steps.front()
                            .symbol;
    auto *events = session.module().getGlobalVariable(
        "__gti_native_v1_g" + std::to_string(symbol), true);
    ir.CreateStore(ir.getInt32(0), events);
    auto *correct = ir.CreateICmpEQ(success, ir.getInt32(0));
    unsigned probes = 0;
    for (const auto &target : program->program().mir().functionInstances()) {
      const auto *declaration =
          program->program().findFunctionDeclaration(target.declaration);
      if (!declaration ||
          (declaration->qualifiedName != "rollback" &&
           declaration->qualifiedName != "array_rollback" &&
           declaration->qualifiedName != "nested_array_rollback" &&
           declaration->qualifiedName != "reference_rollback" &&
           declaration->qualifiedName != "return_rollback" &&
           declaration->qualifiedName != "argument_rollback" &&
           declaration->qualifiedName != "array_argument_rollback"))
        continue;
      const bool returned = declaration->qualifiedName == "return_rollback";
      const bool argument = declaration->qualifiedName == "argument_rollback" ||
                            declaration->qualifiedName == "reference_rollback";
      ir.CreateStore(ir.getInt32(0), events);
      const auto abi =
          session.abi().gti(target.returnType, target.parameterTypes,
                            target.mayRaiseDefinedFailure);
      expect(abi.statusResult && abi.resultDestination,
             "rollback oracle uses the real checked value ABI");
      auto *result =
          ir.CreateAlloca(session.layouts().get(target.returnType).storage);
      auto *failure = ir.CreateAlloca(session.abi().failureChannel());
      auto *status = ir.CreateCall(session.abi().declareFunction(target.id),
                                   {result, failure, ir.getTrue()});
      abi.applyTo(*status);
      auto *code = ir.CreateLoad(
          ir.getInt16Ty(),
          ir.CreateStructGEP(
              session.abi().failureRecord(),
              ir.CreateStructGEP(session.abi().failureChannel(), failure, 0),
              1));
      correct = ir.CreateAnd(
          correct,
          ir.CreateAnd(
              ir.CreateNot(status),
              ir.CreateAnd(
                  ir.CreateICmpEQ(ir.CreateLoad(ir.getInt32Ty(), events),
                                  ir.getInt32(returned   ? 45
                                              : argument ? 1
                                                         : 21)),
                  ir.CreateICmpEQ(code,
                                  ir.getInt16(static_cast<unsigned>(
                                      DefinedFailureCode::DivisionByZero))))));
      ++probes;
    }
    expect(probes == 7,
           "field, array, prepared-argument and return rollback probes found");
    ir.CreateRet(ir.CreateSelect(correct, ir.getInt32(0), ir.getInt32(1)));
    expect(!ir.GetInsertBlock()->empty() &&
               ir.GetInsertBlock()->back().isTerminator(),
           "rollback function was found by sealed declaration identity");
    const auto object = session.emitObject();
    const auto path = root / "probe.o";
    expect(driver::writeArtifact(path, object.contents) ==
               driver::ArtifactWriteStatus::Success,
           "write class component object");
    driver::NativeInputs inputs;
    inputs.finalLibraryFiles.push_back(layout.runtimeLibrary);
    const auto executable = root / "probe";
    const auto linked = toolchain.invoke(driver::NativeLinkRequest(
        driver::discoverNativeCompiler(), path, executable, inputs));
    if (!linked.succeeded())
      std::cerr << linked.output;
    expect(linked.succeeded(), "link class component with the C runtime ABI");
    if (linked.succeeded()) {
      const auto run = driver::invokeProcess({executable.string()});
      if (!run.succeeded())
        std::cerr << run.output;
      expect(run.succeeded(), "source lifecycle success and exact partial "
                              "rollback execute at O0-O3");
    }
  }
  std::filesystem::remove_all(root);
}

void testConcretePackAliases() {
  SemanticType empty;
  empty.kind = SemanticType::TypePack;
  empty.concretePack = true;
  const std::vector<SemanticType> noParameters;
  const std::vector<HirBindingId> noBindings;
  const std::vector<HirBindingId> emptyBinding{7};
  expect(concretePackPrefix(noParameters, emptyBinding, 7, empty) == 0,
         "empty trailing pack retains its exact source formal binding");
  expect(!concretePackPrefix(noParameters, noBindings, 7, empty),
         "an unrelated absent binding cannot witness an empty formal pack");
  auto pair = empty;
  pair.arguments = {SemanticType::Int32, SemanticType::Int32};
  const std::vector<HirBindingId> physicalContinuations{7, 0};
  expect(!concretePackPrefix(pair.arguments, physicalContinuations, 7, pair),
         "unproduced zero-continuation binding compatibility stays rejected");
  const std::vector<SemanticType> nonTrailing{pair, SemanticType::Int32};
  rejects<BackendUnsupportedError>(
      [&] { static_cast<void>(flattenConcreteParameters(nonTrailing)); },
      "non-trailing native packs remain rejected");
  namespace fs = std::filesystem;
  const auto root =
      fs::temp_directory_path() /
      ("gti-llvm-pack-aliases-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  } cleanup{root};
  for (const bool projected : {false, true}) {
    auto frontend = Frontend().analyze("pack-aliases.gti", R"(
struct Pair {
  int left;
  int right;
  Pair(int left, int right) : left(left), right(right) {}
  int sum() { return this.left + this.right; }
};
T create<T, Args...>(Args... args) { return T(args...); }
int main() { Pair value = create<Pair>(20, 22); return value.sum() == 42 ? 0 : 1; }
)");
    expect(frontend.canGenerateCode(), "pack alias source fixture is valid");
    bool duplicated = false;
    for (auto &function : MirProgramTestAccess::functions(frontend.mir)) {
      const MirPlace *selected = nullptr;
      for (const auto &place : function.body.places) {
        if (place.root != MirPlaceRootKind::Binding)
          continue;
        if ((!projected && place.projections.empty() &&
             place.type.kind == SemanticType::TypePack) ||
            (projected && place.projections.size() == 1 &&
             place.projections[0].kind == MirProjectionKind::PackElement)) {
          selected = &place;
          break;
        }
      }
      if (selected) {
        auto invalid = function.body;
        const auto root =
            std::find_if(invalid.places.begin(), invalid.places.end(),
                         [](const auto &place) {
                           return place.type.kind == SemanticType::TypePack &&
                                  place.projections.empty();
                         });
        const auto element =
            std::find_if(invalid.places.begin(), invalid.places.end(),
                         [](const auto &place) {
                           return place.projections.size() == 1 &&
                                  place.projections[0].kind ==
                                      MirProjectionKind::PackElement;
                         });
        if (root != invalid.places.end() && root->key &&
            element != invalid.places.end()) {
          element->key = root->key;
          element->key->projections.push_back(
              {.kind = PlaceProjectionKind::ConstantIndex,
               .index = root->type.arguments.size()});
          expect(!planConcreteParameters(function.parameterTypes,
                                         function.parameterBindings, invalid),
                 "a supplied conflicting projected key cannot be ignored");
        } else {
          expect(false,
                 "alias fixture retains its projected-key negative control");
        }
        auto alias = *selected;
        alias.id = function.body.places.size() + 1;
        function.body.places.push_back(std::move(alias));
        duplicated = true;
        break;
      }
    }
    expect(duplicated, "fixture retains a concrete pack root/element");
    const auto checked = verifyMirProgram(frontend.mir);
    for (const auto &error : checked.errors)
      std::cerr << error.message << '\n';
    expect(checked.valid(), "compatible duplicate pack views are verified MIR");
    if (!checked.valid())
      continue;
    std::vector<LoweredProgramIssue> issues;
    const auto program = verifiedLoweredProgram(
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
    expect(program.has_value(), "duplicate views survive the sealed boundary");
    if (!program)
      continue;
    std::cout << (projected ? "projected" : "root")
              << " pack alias: MIR and sealed snapshot verified\n";
    for (const auto level :
         {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive}) {
      try {
        Session session(*program, {.optimization = level});
        const auto admission = planLlvmAdmission(*program);
        expect(admission.admitted(),
               "concrete pack aliases have native admission");
        lowerScalarProgram(session, admission);
        const auto object = session.emitObject();
        const auto path = root / "probe.o";
        expect(driver::writeArtifact(path, object.contents) ==
                   driver::ArtifactWriteStatus::Success,
               "write concrete pack alias object");
        driver::NativeInputs inputs;
        inputs.finalLibraryFiles.push_back(
            driver::discoverToolchainLayout(nullptr).runtimeLibrary);
        const auto executable = root / "probe";
        const auto linked =
            driver::NativeToolchain().invoke(driver::NativeLinkRequest(
                driver::discoverNativeCompiler(), path, executable, inputs));
        expect(linked.succeeded(), "link concrete pack alias source program");
        if (linked.succeeded())
          expect(driver::invokeProcess({executable.string()}).succeeded(),
                 "duplicate views preserve distinct incoming pack lanes");
      } catch (const std::exception &error) {
        std::cerr << (projected ? "projected" : "root")
                  << " pack alias: " << error.what() << '\n';
        expect(false, "verified aliases must not consume additional ABI lanes");
      }
    }
  }
}

void testPayloadUniqueOwner(const std::filesystem::path &repository,
                            bool sourceOnly) {
  namespace fs = std::filesystem;
  const auto library = fs::canonical(repository / "stdlib/std/memory.gti");
  std::ifstream input(library);
  std::string source(std::istreambuf_iterator<char>{input}, {});
  expect(input.good() || input.eof(),
         "read actual trusted memory declarations");
  // Overlay a real trusted unit, keeping its capability declarations intact.
  // Only this source fixture may name the private scalar owner representation.
  source += R"(
namespace payload_owner_probe {
extern "C" { int32_t payload_owner_counts_v1(); }
enum Packet { owned(gti_internal::unique_owner<int32_t> value) };
gti_internal::unique_owner<int32_t> extract() {
  mut Packet source = Packet::owned(gti_internal::allocate_unique_owner<int32_t>(42));
  switch (std::move(source)) {
  case Packet::owned(value): return std::move(value);
  }
}
int run() {
  {
    gti_internal::unique_owner<int32_t> selected = extract();
    // The source payload is already dead, while its extracted owner is live.
    if (payload_owner_counts_v1() != 10) { return 1; }
    if (gti_internal::unique_owner_borrow(selected) != 42) { return 2; }
  }
  return payload_owner_counts_v1() == 11 ? 0 : 3;
}
}
)";
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  auto frontend = Frontend(options).analyze(
      repository / "tests/payload-owner-entry.gti",
      "#include <std/memory>\n"
      "int main() { return payload_owner_probe::run(); }\n",
      {repository / "stdlib/prelude.gti"}, {{library.string(), source}},
      {repository / "stdlib"});
  for (const auto &diagnostic : frontend.diagnostics)
    if (!frontend.canGenerateCode())
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "trusted scalar-owner payload fixture produces authoritative MIR");
  if (!frontend.canGenerateCode())
    return;
  const auto checked = verifyMirProgram(frontend.mir);
  expect(checked.valid(), "scalar-owner payload MIR verifies without mutation");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  expect(program.has_value(), "scalar-owner payload crosses sealed boundary");
  if (!program)
    return;
  const auto &mir = program->program().mir();
  const auto owner = SemanticType::uniqueOwnerOf(SemanticType::Int32);
  bool exactField = false;
  for (const auto &payload : mir.payloadEnums())
    for (const auto &variant : payload.variants)
      exactField |= variant.payloadTypes == std::vector<SemanticType>{owner};
  bool move = false;
  bool payloadDrop = false;
  bool ownerDrop = false;
  for (const auto &function : mir.functionInstances()) {
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        move |= instruction.operation == MirOperation::PayloadExtract &&
                instruction.info.type == owner &&
                instruction.payloadExtractionKind ==
                    PayloadExtractionKind::MoveValue;
        if (instruction.kind == MirInstructionKind::Drop &&
            instruction.destination) {
          const auto *place = function.body.findPlace(*instruction.destination);
          ownerDrop |= place && place->type == owner;
          payloadDrop |= place && mir.findPayloadEnum(place->type);
        }
      }
  }
  expect(exactField && move && payloadDrop && ownerDrop,
         "sealed body retains scalar UniqueOwner MoveValue and both drops");
  if (!exactField || !move || !payloadDrop || !ownerDrop)
    return;
  std::cout << "scalar UniqueOwner payload: verified source, MoveValue and "
               "both drops\n";
  if (sourceOnly)
    return;

  const auto root =
      fs::temp_directory_path() /
      ("gti-llvm-payload-owner-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  } cleanup{root};
  const auto shim = root / "provider.c";
  expect(driver::writeArtifact(shim, R"(
#define _POSIX_C_SOURCE 200112L
#include <stdint.h>
#include <stdlib.h>
static void *active;
static int32_t allocated, released;
void *gti_rt_posix_memalign_v1(uint64_t bytes, uint64_t alignment, int32_t *status) {
  if (allocated || !bytes || !alignment) abort();
  void *address = NULL;
  *status = posix_memalign(&address, (size_t)alignment, (size_t)bytes);
  if (*status || !address) abort();
  active = address;
  ++allocated;
  return address;
}
void gti_rt_posix_free_v1(void *address) {
  if (!address || address != active || released) abort();
  active = NULL;
  ++released;
  free(address);
}
int32_t payload_owner_counts_v1(void) {
  return allocated * 10 + released;
}
)") == driver::ArtifactWriteStatus::Success,
         "write versioned counting memory provider");
  driver::NativeToolchain toolchain;
  const auto shimObject = root / "provider.o";
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), shim, shimObject, driver::CStandard::C17,
      OptimizationLevel::O2, {}, {}));
  if (!compiled.succeeded())
    std::cerr << compiled.output;
  expect(compiled.succeeded(), "compile exact v1 count/double-free oracle");
  if (!compiled.succeeded())
    return;
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive}) {
    Session session(*program, {.optimization = level});
    const auto admission = planLlvmAdmission(*program);
    expect(admission.admitted(),
           "combined O7/O8 scalar-owner payload admission");
    if (!admission.admitted())
      return;
    lowerScalarProgram(session, admission);
    const auto object = root / "probe.o";
    expect(driver::writeArtifact(object, session.emitObject().contents) ==
               driver::ArtifactWriteStatus::Success,
           "write real scalar-owner payload body object");
    driver::NativeInputs inputs;
    inputs.finalLibraryFiles = {
        shimObject, driver::discoverToolchainLayout(nullptr).runtimeLibrary};
    const auto executable = root / "probe";
    const auto linked = toolchain.invoke(driver::NativeLinkRequest(
        driver::discoverNativeCompiler(), object, executable, inputs));
    if (!linked.succeeded())
      std::cerr << linked.output;
    expect(linked.succeeded(), "link real body with independent memory oracle");
    if (linked.succeeded()) {
      const auto run = driver::invokeProcess({executable.string()});
      if (!run.succeeded())
        std::cerr << run.output;
      expect(run.succeeded(), "scalar payload move has exactly one allocation "
                              "and release, without early or double cleanup");
      if (run.succeeded())
        std::cout << "scalar UniqueOwner payload: "
                  << (level == LlvmCodegenOptimization::None ? "O0" : "O3")
                  << " allocation/release 1/1, no early/double cleanup\n";
    }
  }
}

void testExecutionCAbi() {
  const auto root =
      std::filesystem::temp_directory_path() /
      ("gti-tls-c-oracle-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  const auto layout = driver::discoverToolchainLayout(nullptr);
  const auto source = root / "oracle.c";
  expect(driver::writeArtifact(source, R"(
#include <gti/runtime_execution.h>
static int order;
static bool cleanup(void *value, gti_failure_record_v2 *failure,
                    const gti_failure_record_v2 *primary,
                    const gti_failure_artifact_descriptor_v1 *artifact) {
  (void)failure; (void)primary; (void)artifact;
  order = order * 10 + *(int *)value;
  return true;
}
int main(void) {
  int first = 1, second = 2;
  gti_thread_local_cleanup_entry_v1 a = {0}, b = {0};
  if (gti_rt_thread_locals_attached_v1() || gti_rt_thread_locals_closing_v1()) return 1;
  gti_rt_thread_locals_begin_v1();
  a.storage = &first; a.cleanup = cleanup;
  b.storage = &second; b.cleanup = cleanup;
  gti_rt_thread_locals_register_v1(&a);
  gti_rt_thread_locals_register_v1(&b);
  gti_rt_thread_locals_drain_v2(0, 0);
  gti_rt_thread_locals_drain_v2(0, 0);
  return order == 21 && !a.registered && !b.registered &&
         gti_rt_thread_locals_closing_v1() && !gti_rt_thread_locals_attached_v1() ? 0 : 2;
}
)") == driver::ArtifactWriteStatus::Success,
         "write C TLS oracle");
  driver::NativeToolchain toolchain;
  const auto object = root / "oracle.o";
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), source, object, driver::CStandard::C17,
      OptimizationLevel::O2, {layout.runtimeInclude}, {}));
  expect(compiled.succeeded(),
         "TLS runtime header and callback compile as C17");
  if (compiled.succeeded()) {
    driver::NativeInputs inputs;
    inputs.finalLibraryFiles.push_back(layout.runtimeLibrary);
    const auto executable = root / "oracle";
    const auto linked = toolchain.invoke(driver::NativeLinkRequest(
        driver::discoverNativeCompiler(), object, executable, inputs));
    expect(linked.succeeded(),
           "C TLS oracle links only to the runtime archive");
    if (linked.succeeded())
      expect(driver::invokeProcess({executable.string()}).succeeded(),
             "C TLS registration drains in reverse order exactly once");
  }
  std::filesystem::remove_all(root);
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc >= 3 && std::string_view(argv[1]) == "--payload-unique-owner") {
      testPayloadUniqueOwner(argv[2], argc == 4 && std::string_view(argv[3]) ==
                                                       "--source-only");
      return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--pack-aliases") {
      testConcretePackAliases();
      return failures == 0 ? 0 : 1;
    }
    const auto input = fixture();
    testLayoutAndAbi(input);
    testInvalidModule(input);
    testDiagnostics(input);
    testObjectHandoff(input);
    testClassBodyComponent();
    testConcretePackAliases();
    testExecutionCAbi();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return failures == 0 ? 0 : 1;
}
