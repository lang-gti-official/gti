#include "../src/compiler/llvm/session.h"
#include "gti/driver/build.h"
#include "gti/target.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/MachO.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using llvm::json::Array;
using llvm::json::Object;

void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}

template <class Machine>
const llvm::MCSubtargetInfo &subtarget(const Machine &machine) {
  auto &&value = machine.getMCSubtargetInfo();
  if constexpr (std::is_pointer_v<std::remove_reference_t<decltype(value)>>) {
    require(value, "factory has no MC subtarget");
    return *value;
  } else {
    return value;
  }
}

template <class Target>
std::unique_ptr<llvm::MCSubtargetInfo>
selection(const Target &target, const llvm::Triple &triple, llvm::StringRef cpu,
          llvm::StringRef features) {
  // LLVM versions differ at this API boundary. Neither path creates a machine.
  if constexpr (requires {
                  target.createMCSubtargetInfo(triple, cpu, features);
                })
    return std::unique_ptr<llvm::MCSubtargetInfo>(
        target.createMCSubtargetInfo(triple, cpu, features));
  else
    return std::unique_ptr<llvm::MCSubtargetInfo>(
        target.createMCSubtargetInfo(triple.str(), cpu, features));
}

template <class Feature> llvm::StringRef featureName(const Feature &feature) {
  if constexpr (requires { feature.key(); })
    return feature.key();
  else
    return feature.Key;
}

Object features(const llvm::MCSubtargetInfo &info) {
  Array enabled;
  for (const auto &feature : info.getAllProcessorFeatures())
    if (info.getFeatureBits()[feature.Value])
      enabled.emplace_back(featureName(feature).str());
  const auto &schedule = info.getSchedModel();
  return Object{{"cpu", info.getCPU().str()},
                {"tune_cpu", info.getTuneCPU().str()},
                {"requested_features", info.getFeatureString().str()},
                {"enabled_features", std::move(enabled)},
                {"schedule_processor", schedule.getProcessorID()},
                {"schedule_issue_width", schedule.IssueWidth},
                {"schedule_load_latency", schedule.LoadLatency}};
}

struct ObjectFacts {
  llvm::Triple::ArchType arch;
  unsigned addressBytes;
  bool littleEndian;
  bool macho = false;
  bool debug = false;
  bool sanitizer = false;
  bool stackGuard = false;
  bool unwind = false;
  bool executable = false;
  bool pie = false;
  std::uint32_t platform = 0, minimum = 0, sdk = 0;
};

ObjectFacts objectFacts(llvm::StringRef path) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  require(bool(buffer), "cannot read actual object: " + path.str());
  auto parsed =
      llvm::object::ObjectFile::createObjectFile((*buffer)->getMemBufferRef());
  if (!parsed)
    throw std::runtime_error(llvm::toString(parsed.takeError()));
  const auto &file = **parsed;
  ObjectFacts result{file.getArch(), file.getBytesInAddress(),
                     file.isLittleEndian()};
  for (const auto &section : file.sections()) {
    auto name = section.getName();
    if (!name)
      throw std::runtime_error(llvm::toString(name.takeError()));
    result.debug |= name->starts_with(".debug") || name->starts_with("__debug");
    result.unwind |= name->contains("eh_frame") ||
                     name->contains("compact_unwind") ||
                     name->contains("unwind_info");
  }
  for (const auto &symbol : file.symbols()) {
    auto name = symbol.getName();
    if (!name)
      throw std::runtime_error(llvm::toString(name.takeError()));
    result.sanitizer |=
        name->contains("__asan_") || name->contains("__tsan_") ||
        name->contains("__ubsan_") || name->contains("__hwasan_");
    result.stackGuard |= name->contains("__stack_chk_");
  }
  if (const auto *macho =
          llvm::dyn_cast<llvm::object::MachOObjectFile>(&file)) {
    result.macho = true;
    const auto flags = macho->is64Bit() ? macho->getHeader64().flags
                                        : macho->getHeader().flags;
    const auto type = macho->is64Bit() ? macho->getHeader64().filetype
                                       : macho->getHeader().filetype;
    result.executable = type == llvm::MachO::MH_EXECUTE;
    result.pie = flags & llvm::MachO::MH_PIE;
    for (const auto &command : macho->load_commands()) {
      if (command.C.cmd == llvm::MachO::LC_BUILD_VERSION) {
        const auto version = macho->getBuildVersionLoadCommand(command);
        result.platform = version.platform;
        result.minimum = version.minos;
        result.sdk = version.sdk;
      } else if (command.C.cmd == llvm::MachO::LC_VERSION_MIN_MACOSX) {
        const auto version = macho->getVersionMinLoadCommand(command);
        result.platform = llvm::MachO::PLATFORM_MACOS;
        result.minimum = version.version;
        result.sdk = version.sdk;
      }
    }
  }
  return result;
}

Object describe(const ObjectFacts &facts) {
  return Object{{"arch", unsigned(facts.arch)},
                {"address_bytes", facts.addressBytes},
                {"little_endian", facts.littleEndian},
                {"macho", facts.macho},
                {"platform", facts.platform},
                {"minimum_os_encoded", facts.minimum},
                {"sdk_encoded", facts.sdk},
                {"executable", facts.executable},
                {"pie", facts.pie},
                {"debug_sections", facts.debug},
                {"sanitizer_symbols", facts.sanitizer},
                {"stack_guard_symbols", facts.stackGuard},
                {"unwind_sections", facts.unwind}};
}

std::string string(const Object &input, llvm::StringRef key) {
  auto value = input.getString(key);
  require(value.has_value(), "missing actual-input field: " + key.str());
  return value->str();
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(
        argc == 2 || argc == 3,
        "usage: cost target probe --factory | --compare actual-inputs.json");
    const bool compare = std::string_view(argv[1]) == "--compare";
    const bool lifetime = std::string_view(argv[1]) == "--metadata-lifetime";
    require(compare ? argc == 3
                    : lifetime || std::string_view(argv[1]) == "--factory",
            "unknown target probe mode");
    const lang::LlvmBackendOptions options{
        .optimization = lang::LlvmCodegenOptimization::Aggressive};
    // This is the sole machine construction: the unchanged guarded factory.
    auto machine =
        lang::native_llvm::makeMachine(lang::TargetInfo::host(), options);
    const auto &reference = subtarget(*machine);
    const auto build = lang::driver::currentCompilerBuildIdentity();
    Object output{
        {"schema", 1},
        {"factory_machine_count", 1},
        {"compiler_state",
         std::string(lang::driver::compilerSourceStateName(build.state))},
        {"compiler_commit", build.commit.value_or("")},
        {"llvm_version", LLVM_VERSION_STRING},
        {"triple", machine->getTargetTriple().str()},
        {"data_layout", machine->createDataLayout().getStringRepresentation()},
        {"factory_cpu", machine->getTargetCPU()},
        {"code_model", unsigned(machine->getCodeModel())},
        {"relocation", unsigned(machine->getRelocationModel())},
        {"codegen_optimization", unsigned(machine->getOptLevel())},
        {"fp_fusion", unsigned(machine->Options.AllowFPOpFusion)},
        {"debug_info", false},
        {"instrumentation", "none"},
        {"effective_subtarget", features(reference)},
        {"normalization_accepted", false}};
    if (lifetime) {
      const auto cpu = reference.getCPU().str();
      std::string requested;
      for (const auto &feature : reference.getAllProcessorFeatures())
        if (reference.getFeatureBits()[feature.Value]) {
          requested = "+" + featureName(feature).str();
          break;
        }
      require(!requested.empty() && requested != reference.getFeatureString(),
              "lifetime control needs a distinct explicit enabled feature");
      auto triple = machine->getTargetTriple();
      std::string error;
      const auto *target =
          llvm::TargetRegistry::lookupTarget("", triple, error);
      require(target, error);
      Object snapshot;
      {
        auto temporary = selection(*target, triple, cpu, requested);
        require(
            bool(temporary) &&
                temporary->getFeatureBits() == reference.getFeatureBits(),
            "MC dependency normalization changed an already-enabled feature");
        snapshot = features(*temporary);
      }
      // Reuse the destroyed MC allocation and short-string storage before
      // serialization. A non-owning JSON StringRef cannot satisfy this check.
      std::vector<std::unique_ptr<llvm::MCSubtargetInfo>> replacements;
      std::vector<std::string> strings;
      for (unsigned index = 0; index < 32; ++index) {
        replacements.push_back(selection(*target, triple, cpu, ""));
        strings.emplace_back(requested.size(), char('a' + index % 20));
      }
      std::string serialized;
      llvm::raw_string_ostream stream(serialized);
      stream << llvm::json::Value(std::move(snapshot));
      stream.flush();
      auto decoded = llvm::json::parse(serialized);
      require(bool(decoded) && decoded->getAsObject(),
              "metadata JSON did not round-trip");
      const auto *saved = decoded->getAsObject();
      require(saved->getString("cpu") == cpu &&
                  saved->getString("requested_features") == requested,
              "metadata strings did not survive MC destruction and heap reuse");
      output["status"] =
          "passed-mc-normalization-and-metadata-lifetime-controls";
      output["raw_feature_requests_differ"] = true;
      output["normalized_effective_features_equal"] = true;
      output["mc_object_destroyed_before_serialization"] = true;
      output["heap_reuse_allocations"] = 32;
      output["metadata_json_round_trip"] = true;
      output["normalization_accepted"] = false;
      llvm::outs() << llvm::json::Value(std::move(output)) << '\n';
      return 0;
    }
    if (!compare) {
      output["status"] = "factory-reference-only";
      llvm::outs() << llvm::json::Value(std::move(output)) << '\n';
      return 0;
    }
    auto configuration = llvm::MemoryBuffer::getFile(argv[2]);
    require(bool(configuration), "cannot read actual-input record");
    auto parsed = llvm::json::parse((*configuration)->getBuffer());
    if (!parsed)
      throw std::runtime_error(llvm::toString(parsed.takeError()));
    const auto *input = parsed->getAsObject();
    require(input, "actual-input record must be an object");
    auto bitcode = llvm::MemoryBuffer::getFile(string(*input, "cpp_bitcode"));
    require(bool(bitcode), "cannot read retained actual C++ bitcode");
    llvm::LLVMContext context;
    auto module =
        llvm::parseBitcodeFile((*bitcode)->getMemBufferRef(), context);
    if (!module)
      throw std::runtime_error(llvm::toString(module.takeError()));
    const auto &cpp = **module;
    const llvm::Triple triple(cpp.getTargetTriple());
    std::string targetError;
    auto lookupTriple = triple;
    const auto *target =
        llvm::TargetRegistry::lookupTarget("", lookupTriple, targetError);
    require(target, targetError);
    Array errors, selections;
    const auto check = [&](bool valid, llvm::StringRef message) {
      if (!valid)
        errors.emplace_back(message);
    };
    check(cpp.getDataLayout() == machine->createDataLayout(),
          "actual C++ DataLayout differs");
    check(triple.getArch() == machine->getTargetTriple().getArch() &&
              triple.getVendor() == machine->getTargetTriple().getVendor() &&
              triple.getEnvironment() ==
                  machine->getTargetTriple().getEnvironment() &&
              triple.getObjectFormat() ==
                  machine->getTargetTriple().getObjectFormat(),
          "actual C++ target architecture/vendor/environment/format differs");
    check(cpp.getPICLevel() == llvm::PICLevel::BigPIC &&
              machine->getRelocationModel() == llvm::Reloc::PIC_,
          "PIC policy differs");
    check(cpp.getCodeModel() && *cpp.getCodeModel() == machine->getCodeModel(),
          "actual C++ code model is absent or differs");
    check(machine->Options.AllowFPOpFusion == llvm::FPOpFusion::Strict,
          "factory floating contract is not strict");
    check(!cpp.getNamedMetadata("llvm.dbg.cu"),
          "actual C++ bitcode has debug information");
    unsigned definitions = 0, floating = 0;
    for (const auto &function : cpp) {
      if (function.isDeclaration())
        continue;
      ++definitions;
      const auto cpuAttribute = function.getFnAttribute("target-cpu");
      const auto featureAttribute = function.getFnAttribute("target-features");
      const auto cpu = cpuAttribute.isStringAttribute()
                           ? cpuAttribute.getValueAsString().str()
                           : string(*input, "cpp_default_cpu");
      const auto requested = featureAttribute.isStringAttribute()
                                 ? featureAttribute.getValueAsString().str()
                                 : string(*input, "cpp_default_features");
      auto actual = selection(*target, triple, cpu, requested);
      require(bool(actual), "cannot normalize actual C++ MC features");
      llvm::SmallVector<llvm::StringRef> flags;
      llvm::StringRef(requested).split(flags, ',', -1, false);
      for (auto flag : flags) {
        const auto name = flag.drop_front();
        check((flag.starts_with("+") || flag.starts_with("-")) &&
                  std::any_of(actual->getAllProcessorFeatures().begin(),
                              actual->getAllProcessorFeatures().end(),
                              [&](const auto &feature) {
                                return name == featureName(feature);
                              }),
              "actual C++ feature name is unknown to the producer LLVM");
      }
      check(actual->getFeatureBits() == reference.getFeatureBits(),
            "effective MC features differ after dependency normalization");
      check(actual->getCPU() == reference.getCPU() &&
                actual->getTuneCPU() == reference.getTuneCPU() &&
                actual->getSchedModel().getProcessorID() ==
                    reference.getSchedModel().getProcessorID(),
            "CPU/tuning/scheduling selection differs");
      selections.emplace_back(
          Object{{"function", function.getName().str()},
                 {"effective_subtarget", features(*actual)}});
      check(!function.hasFnAttribute(llvm::Attribute::SanitizeAddress) &&
                !function.hasFnAttribute(llvm::Attribute::SanitizeThread) &&
                !function.hasFnAttribute(llvm::Attribute::SanitizeMemory) &&
                !function.hasFnAttribute(llvm::Attribute::SanitizeHWAddress),
            "actual C++ function has sanitizer instrumentation");
      for (const auto &block : function)
        for (const auto &instruction : block) {
          if (const auto *math =
                  llvm::dyn_cast<llvm::FPMathOperator>(&instruction)) {
            ++floating;
            check(!math->getFastMathFlags().any(),
                  "actual C++ FP operation relaxes the strict contract");
          }
          if (const auto *intrinsic =
                  llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
            check(intrinsic->getIntrinsicID() != llvm::Intrinsic::fma &&
                      intrinsic->getIntrinsicID() != llvm::Intrinsic::fmuladd,
                  "actual C++ bitcode contains floating contraction");
        }
    }
    check(definitions != 0, "actual C++ bitcode has no definitions");
    const auto cppObject = objectFacts(string(*input, "cpp_object"));
    const auto llvmObject = objectFacts(string(*input, "llvm_object"));
    const auto cppExecutable = objectFacts(string(*input, "cpp_executable"));
    const auto llvmExecutable = objectFacts(string(*input, "llvm_executable"));
    for (const auto &facts :
         {cppObject, llvmObject, cppExecutable, llvmExecutable}) {
      check(facts.arch == reference.getTargetTriple().getArch() &&
                facts.addressBytes ==
                    machine->createDataLayout().getPointerSize() &&
                facts.littleEndian ==
                    machine->createDataLayout().isLittleEndian(),
            "actual object/executable architecture or scalar ABI differs");
      check(!facts.debug && !facts.sanitizer,
            "actual object/executable debug/sanitizer state differs");
    }
    check(cppObject.macho && llvmObject.macho && cppExecutable.macho &&
              llvmExecutable.macho,
          "deployment/ABI object validation currently requires the bounded "
          "Mach-O profile");
    check(cppObject.stackGuard == llvmObject.stackGuard,
          "actual user-object stack-guard hardening differs");
    check(cppObject.platform != 0 &&
              cppObject.platform == llvmObject.platform &&
              cppObject.minimum == llvmObject.minimum,
          "actual object deployment/platform differs");
    check(cppExecutable.executable && llvmExecutable.executable &&
              cppExecutable.platform != 0 &&
              cppExecutable.platform == llvmExecutable.platform &&
              cppExecutable.minimum == llvmExecutable.minimum &&
              cppExecutable.sdk == llvmExecutable.sdk &&
              cppExecutable.pie == llvmExecutable.pie,
          "actual executable deployment/SDK/PIE differs");
    output["cpp_source_filename"] = cpp.getSourceFileName();
    output["cpp_triple"] = triple.str();
    output["cpp_pic_level"] = unsigned(cpp.getPICLevel());
    output["cpp_pie_level"] = unsigned(cpp.getPIELevel());
    output["cpp_definitions"] = definitions;
    output["cpp_fp_operations"] = floating;
    output["cpp_selections"] = std::move(selections);
    output["cpp_object"] = describe(cppObject);
    output["llvm_object"] = describe(llvmObject);
    output["cpp_executable"] = describe(cppExecutable);
    output["llvm_executable"] = describe(llvmExecutable);
    output["limitations"] =
        Array{"Selected target/ABI/FP/mode properties only; not all compiler "
              "policy equivalence",
              "Internal calling conventions, representations and optimization "
              "implementation remain backend-specific",
              "Runtime/archive/implicit dependency provenance must be retained "
              "separately",
              "No controlled runtime timing is performed by this probe"};
    const bool valid = errors.empty();
    output["normalization_accepted"] = valid;
    output["status"] = valid ? "passed-declared-target-profile"
                             : "failed-or-unproven-target-profile";
    output["errors"] = std::move(errors);
    llvm::outs() << llvm::json::Value(std::move(output)) << '\n';
    return valid ? 0 : 1;
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n';
    return 1;
  }
}
