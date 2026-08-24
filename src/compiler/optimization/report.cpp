#include "gti/optimization/report.h"

#include "gti/binary_float.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace lang {
namespace {

[[nodiscard]] std::string_view bodyKindName(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
    return "module";
  case MirBodyKind::FieldInitializers:
    return "field-initializers";
  case MirBodyKind::StaticFieldInitializers:
    return "static-field-initializers";
  case MirBodyKind::Function:
    return "function";
  case MirBodyKind::Constructor:
    return "constructor";
  case MirBodyKind::Destructor:
    return "destructor";
  case MirBodyKind::Lambda:
    return "lambda";
  case MirBodyKind::HostedStartup:
    return "hosted-startup";
  }
  return "invalid";
}

[[nodiscard]] std::string_view transformKindName(MirTransformKind kind) {
  switch (kind) {
  case MirTransformKind::ReplaceInstruction:
    return "replace-instruction";
  case MirTransformKind::RemoveInstruction:
    return "remove-instruction";
  case MirTransformKind::ReplaceTerminator:
    return "replace-terminator";
  case MirTransformKind::RemoveBlock:
    return "remove-block";
  case MirTransformKind::PromotePlace:
    return "promote-place";
  case MirTransformKind::Count:
    break;
  }
  return "invalid";
}

[[nodiscard]] std::string_view proofKindName(MirTransformProofKind kind) {
  switch (kind) {
  case MirTransformProofKind::IdentityFold:
    return "identity-fold";
  case MirTransformProofKind::ComputeFold:
    return "compute-fold";
  case MirTransformProofKind::BranchFold:
    return "branch-fold";
  case MirTransformProofKind::UnreachableBlock:
    return "unreachable-block";
  case MirTransformProofKind::DeadUnusedComputation:
    return "dead-unused-computation";
  case MirTransformProofKind::ScalarPromotion:
    return "scalar-promotion";
  case MirTransformProofKind::Count:
    break;
  }
  return "none";
}

void writeJsonString(std::ostream &output, std::string_view value) {
  output << '"';
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
      if (character < 0x20) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(character) << std::dec
               << std::setfill(' ');
      } else {
        output << static_cast<char>(character);
      }
      break;
    }
  }
  output << '"';
}

template <typename Value>
void writeNumberArray(std::ostream &output, const std::vector<Value> &values) {
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    output << values[index];
  }
  output << ']';
}

void writeLiteral(std::ostream &output, const std::optional<Literal> &literal) {
  if (!literal) {
    output << "null";
    return;
  }
  output << '{';
  std::visit(
      [&](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          output << "\"kind\":\"none\"";
        } else if constexpr (std::is_same_v<Value, std::nullptr_t>) {
          output << "\"kind\":\"nullptr\"";
        } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
          output << "\"kind\":\"uint64\",\"value\":" << value;
        } else if constexpr (std::is_same_v<Value, BinaryFloat>) {
          output << "\"kind\":\"binary-float\",\"format\":";
          writeJsonString(output, value.format == BinaryFloatFormat::Binary32
                                      ? "binary32"
                                      : "binary64");
          output << ",\"bits\":";
          std::ostringstream bits;
          bits << "0x" << std::hex << value.bits;
          writeJsonString(output, bits.str());
        } else if constexpr (std::is_same_v<Value, CharacterLiteral>) {
          output << "\"kind\":\"character\",\"value\":"
                 << static_cast<unsigned int>(value.value);
        } else if constexpr (std::is_same_v<Value, std::string>) {
          output << "\"kind\":\"string\",\"value\":";
          writeJsonString(output, value);
        } else if constexpr (std::is_same_v<Value, bool>) {
          output << "\"kind\":\"bool\",\"value\":"
                 << (value ? "true" : "false");
        }
      },
      *literal);
  output << '}';
}

void writeBody(std::ostream &output, MirBodyAddress body) {
  output << "{\"kind\":";
  writeJsonString(output, bodyKindName(body.kind));
  output << ",\"owner\":" << body.owner << '}';
}

void writeRequiredAnalyses(std::ostream &output,
                           const MirTransformRequiredAnalyses &analyses) {
  output << "{\"valueUses\":" << (analyses.valueUses ? "true" : "false")
         << ",\"effects\":" << (analyses.effects ? "true" : "false")
         << ",\"reachability\":" << (analyses.reachability ? "true" : "false")
         << ",\"dominance\":" << (analyses.dominance ? "true" : "false")
         << ",\"scalarPromotion\":"
         << (analyses.scalarPromotion ? "true" : "false") << '}';
}

void writeTransform(std::ostream &output, const MirTransformRecord &record) {
  output << "{\"pass\":";
  writeJsonString(output, record.pass);
  output << ",\"kind\":";
  writeJsonString(output, transformKindName(record.kind));
  output << ",\"proof\":";
  writeJsonString(output, proofKindName(record.proof));
  output << ",\"body\":";
  writeBody(output, record.body);
  output << ",\"block\":" << record.block
         << ",\"instruction\":" << record.instruction
         << ",\"place\":" << record.place
         << ",\"sourceValue\":" << record.sourceValue
         << ",\"sourceStatement\":" << record.sourceStatement
         << ",\"replacementLiteral\":";
  writeLiteral(output, record.replacementLiteral);
  output << ",\"relatedBlocks\":";
  writeNumberArray(output, record.relatedBlocks);
  output << ",\"relatedInstructions\":";
  writeNumberArray(output, record.relatedInstructions);
  output << ",\"relatedValues\":";
  writeNumberArray(output, record.relatedValues);
  output << ",\"requiredAnalyses\":";
  writeRequiredAnalyses(output, record.requiredAnalyses);
  output << '}';
}

void writeRemark(std::ostream &output, const OptimizationRemark &remark) {
  output << "{\"kind\":";
  writeJsonString(output, remark.kind == OptimizationRemarkKind::Applied
                              ? "applied"
                              : "missed");
  output << ",\"pass\":";
  writeJsonString(output, remark.pass);
  output << ",\"message\":";
  writeJsonString(output, remark.message);
  output << ",\"body\":";
  writeBody(output, remark.body);
  output << ",\"block\":" << remark.block
         << ",\"instruction\":" << remark.instruction
         << ",\"place\":" << remark.place
         << ",\"sourceValue\":" << remark.sourceValue
         << ",\"sourceStatement\":" << remark.sourceStatement << ",\"proof\":";
  if (remark.proof == MirTransformProofKind::Count) {
    output << "null";
  } else {
    writeJsonString(output, proofKindName(remark.proof));
  }
  output << '}';
}

void writeInvalidation(std::ostream &output,
                       const MirAnalysisInvalidation &invalidation) {
  output << "{\"instructionFacts\":"
         << (invalidation.instructionFacts ? "true" : "false")
         << ",\"valueUses\":" << (invalidation.valueUses ? "true" : "false")
         << ",\"controlFlow\":" << (invalidation.controlFlow ? "true" : "false")
         << ",\"reachability\":"
         << (invalidation.reachability ? "true" : "false")
         << ",\"dominance\":" << (invalidation.dominance ? "true" : "false")
         << '}';
}

void writeStringArray(std::ostream &output,
                      const std::vector<std::string> &values) {
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    writeJsonString(output, values[index]);
  }
  output << ']';
}

} // namespace

std::string formatOptimizationRemarks(const OptimizationReport &report,
                                      std::string_view optimizationLevel) {
  std::ostringstream output;
  output << "gti-opt: GTI MIR optimization remarks (" << optimizationLevel
         << ")\n";
  for (const OptimizationPassReport &pass : report.passes) {
    for (const OptimizationRemark &remark : pass.remarks) {
      output << "gti-opt: "
             << (remark.kind == OptimizationRemarkKind::Applied ? "applied"
                                                                : "missed")
             << " [" << remark.pass;
      if (remark.proof != MirTransformProofKind::Count) {
        output << '/' << proofKindName(remark.proof);
      }
      output << "] " << bodyKindName(remark.body.kind) << '#'
             << remark.body.owner;
      if (remark.block != 0) {
        output << " bb" << remark.block;
      }
      if (remark.instruction != 0) {
        output << " i" << remark.instruction;
      }
      if (remark.place != 0) {
        output << " p" << remark.place;
      }
      if (remark.sourceValue != 0 || remark.sourceStatement != 0) {
        output << " source(v" << remark.sourceValue << ",s"
               << remark.sourceStatement << ')';
      }
      output << ": " << remark.message << '\n';
    }
  }
  output << "gti-opt: safety operations source="
         << report.safety.sourceOperations
         << " emitted=" << report.safety.emittedOperations
         << " statically-discharged=" << report.safety.staticallyDischarged
         << '\n';
  return output.str();
}

std::string
serializeOptimizationReportJson(const OptimizationReport &report,
                                std::string_view optimizationLevel) {
  std::ostringstream output;
  output << "{\n  \"schema\":\"gti-optimization-report\",\n"
         << "  \"version\":" << optimizationReportSchemaVersion << ",\n"
         << "  \"optimizationLevel\":";
  writeJsonString(output, optimizationLevel);
  output << ",\n  \"verification\":{\"enabled\":"
         << (report.verificationEnabled ? "true" : "false")
         << ",\"inputValid\":"
         << (report.inputVerification.valid() ? "true" : "false")
         << ",\"outputValid\":"
         << (report.outputVerification.valid() ? "true" : "false")
         << "},\n  \"safetyOperations\":{\"source\":"
         << report.safety.sourceOperations
         << ",\"emitted\":" << report.safety.emittedOperations
         << ",\"staticallyDischarged\":" << report.safety.staticallyDischarged
         << "},\n  \"passes\":[";
  for (std::size_t passIndex = 0; passIndex < report.passes.size();
       ++passIndex) {
    if (passIndex != 0) {
      output << ',';
    }
    const OptimizationPassReport &pass = report.passes[passIndex];
    output << "\n    {\"name\":";
    writeJsonString(output, pass.name);
    output << ",\"changed\":" << (pass.changed ? "true" : "false")
           << ",\"iterations\":" << pass.iterations
           << ",\"analyzedBodies\":" << pass.analyzedBodies
           << ",\"candidates\":" << pass.candidates
           << ",\"missed\":" << pass.missed
           << ",\"appliedEdits\":" << pass.appliedEdits
           << ",\"shadowComparisons\":" << pass.shadowComparisons
           << ",\"shadowMismatches\":" << pass.shadowMismatches
           << ",\"valueUsesRebuilt\":"
           << (pass.valueUsesRebuilt ? "true" : "false")
           << ",\"invalidation\":";
    writeInvalidation(output, pass.invalidation);
    output << ",\"invalidatedAnalyses\":";
    writeStringArray(output, pass.invalidatedAnalyses);
    output << ",\"remarks\":[";
    for (std::size_t remarkIndex = 0; remarkIndex < pass.remarks.size();
         ++remarkIndex) {
      if (remarkIndex != 0) {
        output << ',';
      }
      writeRemark(output, pass.remarks[remarkIndex]);
    }
    output << "]}";
  }
  if (!report.passes.empty()) {
    output << '\n' << "  ";
  }
  output << "],\n  \"transformations\":[";
  for (std::size_t index = 0; index < report.transformations.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    output << '\n' << "    ";
    writeTransform(output, report.transformations[index]);
  }
  if (!report.transformations.empty()) {
    output << '\n' << "  ";
  }
  output << "]\n}\n";
  return output.str();
}

} // namespace lang
