#include "project_presentation.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace lang::cli {
namespace {

std::string jsonEscape(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20U) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<int>(character) << std::dec;
      } else {
        output << static_cast<char>(character);
      }
      break;
    }
  }
  return output.str();
}

void writeJsonString(std::ostream &stream, std::string_view value) {
  stream << '"' << jsonEscape(value) << '"';
}

void writeJsonStrings(std::ostream &stream,
                      const std::vector<std::string> &values) {
  stream << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      stream << ", ";
    }
    writeJsonString(stream, values[index]);
  }
  stream << ']';
}

void writeJsonPaths(std::ostream &stream,
                    const std::vector<std::filesystem::path> &paths) {
  stream << '[';
  for (std::size_t index = 0; index < paths.size(); ++index) {
    if (index != 0) {
      stream << ", ";
    }
    writeJsonString(stream, paths[index].string());
  }
  stream << ']';
}

std::string_view nativeLinkOperandKindName(driver::NativeLinkOperandKind kind) {
  switch (kind) {
  case driver::NativeLinkOperandKind::File:
    return "file";
  case driver::NativeLinkOperandKind::Library:
    return "library";
  case driver::NativeLinkOperandKind::Framework:
    return "framework";
  }
  return "file";
}

void writeOrderedLinkOperands(
    std::ostream &stream,
    const std::vector<driver::NativeLinkOperand> &operands) {
  stream << '[';
  for (std::size_t index = 0; index < operands.size(); ++index) {
    if (index != 0) {
      stream << ", ";
    }
    stream << "{\"kind\": ";
    writeJsonString(stream, nativeLinkOperandKindName(operands[index].kind));
    stream << ", \"value\": ";
    writeJsonString(stream, operands[index].value);
    stream << '}';
  }
  stream << ']';
}

void writeNativeInputs(std::ostream &stream,
                       const driver::NativeInputs &inputs) {
  stream << "{\"includeDirectories\": ";
  writeJsonPaths(stream, inputs.includeDirectories);
  stream << ", \"cSources\": ";
  writeJsonPaths(stream, inputs.cSources);
  stream << ", \"cStandard\": ";
  writeJsonString(stream, driver::cStandardName(inputs.cStandard.value_or(
                              driver::CStandard::C17)));
  stream << ", \"cCompileArguments\": ";
  writeJsonStrings(stream, inputs.cCompilerArguments);
  stream << ", \"cppSources\": ";
  writeJsonPaths(stream, inputs.cppSources);
  stream << ", \"compileArguments\": ";
  writeJsonStrings(stream, inputs.compilerArguments);
  stream << ", \"libraryDirectories\": ";
  writeJsonPaths(stream, inputs.libraryDirectories);
  stream << ", \"linkFiles\": ";
  writeJsonPaths(stream, inputs.libraryFiles);
  stream << ", \"libraries\": ";
  writeJsonStrings(stream, inputs.libraries);
  stream << ", \"frameworks\": ";
  writeJsonStrings(stream, inputs.frameworks);
  stream << ", \"orderedLinkOperands\": ";
  writeOrderedLinkOperands(stream, inputs.orderedLinkOperands);
  stream << ", \"linkArguments\": ";
  writeJsonStrings(stream, inputs.linkerArguments);
  stream << ", \"rawArguments\": ";
  writeJsonStrings(stream, inputs.trailingArguments);
  stream << '}';
}

const driver::ProjectBuildPlan *
findMetadataPlan(const driver::ProjectMetadata &metadata,
                 std::string_view target, std::string_view profile) {
  const auto found = std::find_if(
      metadata.plans().begin(), metadata.plans().end(),
      [target, profile](const driver::ProjectBuildPlan &plan) {
        return plan.targetName() == target && plan.profileName() == profile;
      });
  return found == metadata.plans().end() ? nullptr : &*found;
}

} // namespace

int optimizationNumber(OptimizationLevel optimization) {
  switch (optimization) {
  case OptimizationLevel::O0:
    return 0;
  case OptimizationLevel::O1:
    return 1;
  case OptimizationLevel::O2:
    return 2;
  case OptimizationLevel::O3:
    return 3;
  }
  return 0;
}

std::string_view cppStandardName(CppStandard standard) {
  return standard == CppStandard::Cpp23 ? "c++23" : "c++20";
}

void writeProjectMetadata(std::ostream &stream,
                          const driver::ProjectMetadata &metadata) {
  const driver::ProjectManifest &manifest = metadata.manifest();
  const TargetInfo &target = metadata.target();
  stream << "{\n  \"schemaVersion\": " << projectMetadataSchemaVersion
         << ",\n  \"manifestVersion\": " << driver::currentManifestVersion
         << ",\n  \"manifest\": ";
  writeJsonString(stream, manifest.path().string());
  stream << ",\n  \"package\": {\n    \"name\": ";
  writeJsonString(stream, manifest.package().name);
  stream << ",\n    \"version\": ";
  writeJsonString(stream, manifest.package().version);
  stream << ",\n    \"root\": ";
  writeJsonString(stream, manifest.packageRoot().string());
  stream << "\n  },\n  \"target\": {\n    \"triple\": ";
  writeJsonString(stream, driver::targetTriple(target));
  stream << ",\n    \"arch\": ";
  writeJsonString(stream, target.arch);
  stream << ",\n    \"vendor\": ";
  writeJsonString(stream, target.vendor);
  stream << ",\n    \"os\": ";
  writeJsonString(stream, target.os);
  stream << "\n  },\n  \"profiles\": [";

  for (std::size_t index = 0; index < manifest.profiles().size(); ++index) {
    const driver::ProjectProfile &profile = manifest.profiles()[index];
    stream << (index == 0 ? "\n" : ",\n") << "    {\"name\": ";
    writeJsonString(stream, profile.name);
    stream << ", \"optimization\": " << optimizationNumber(profile.optimization)
           << ", \"cppStandard\": ";
    writeJsonString(stream, cppStandardName(profile.cppStandard));
    stream << ", \"executionProfile\": ";
    writeJsonString(stream, executionProfileName(profile.executionProfile));
    stream << ", \"keepCpp\": " << (profile.keepCpp ? "true" : "false") << '}';
  }
  if (!manifest.profiles().empty()) {
    stream << '\n';
  }
  stream << "  ],\n  \"targets\": [";

  for (std::size_t targetIndex = 0; targetIndex < manifest.targets().size();
       ++targetIndex) {
    const driver::ProjectTarget &projectTarget =
        manifest.targets()[targetIndex];
    stream << (targetIndex == 0 ? "\n" : ",\n") << "    {\n      \"name\": ";
    writeJsonString(stream, projectTarget.name);
    stream << ",\n      \"kind\": ";
    writeJsonString(stream, driver::projectTargetKindName(projectTarget.kind));
    stream << ",\n      \"root\": ";
    writeJsonString(stream, projectTarget.root.string());
    stream << ",\n      \"outputs\": [";
    for (std::size_t profileIndex = 0;
         profileIndex < manifest.profiles().size(); ++profileIndex) {
      const driver::ProjectProfile &profile = manifest.profiles()[profileIndex];
      const driver::ProjectBuildPlan *plan =
          findMetadataPlan(metadata, projectTarget.name, profile.name);
      stream << (profileIndex == 0 ? "\n" : ",\n") << "        {\"profile\": ";
      writeJsonString(stream, profile.name);
      stream << ", \"executable\": ";
      writeJsonString(stream, plan == nullptr ? std::string_view{}
                                              : plan->output().string());
      stream << ", \"generatedCpp\": ";
      writeJsonString(stream, plan == nullptr
                                  ? std::string_view{}
                                  : plan->generatedSource().string());
      stream << ", \"native\": ";
      if (plan == nullptr) {
        writeNativeInputs(stream, {});
      } else {
        writeNativeInputs(stream, plan->nativeInputs());
      }
      stream << '}';
    }
    if (!manifest.profiles().empty()) {
      stream << '\n';
    }
    stream << "      ]\n    }";
  }
  if (!manifest.targets().empty()) {
    stream << '\n';
  }
  stream << "  ]\n}\n";
}

} // namespace lang::cli
