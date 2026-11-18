#pragma once

#include "gti/mir.h"
#include "type_hash.h"

#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace lang::native_llvm {
// Built once for a sealed admission plan; owns keys and borrows rows from that
// immutable program. Class matches retain source order and duplicates. Enum
// lookups retain MirProgram's first-match rule, including malformed duplicates.
class AdmissionTypeIndex final {
public:
  AdmissionTypeIndex(const MirProgram &program, bool validate)
      : program(program), validate(validate) {
    for (const auto &row : program.classInstances())
      classRows[row.type].push_back(&row);
    for (const auto &row : program.payloadEnums())
      payloadRows.try_emplace(row.type, &row);
    for (const auto &row : program.integralEnums())
      integralRows.try_emplace(row.type, &row);
  }

  std::span<const MirClassInstance *const>
  classes(const SemanticType &type) const {
    const auto found = classRows.find(type);
    const std::span<const MirClassInstance *const> result =
        found == classRows.end() ? std::span<const MirClassInstance *const>{}
                                 : found->second;
    if (validate) {
      std::size_t index = 0;
      for (const auto &row : program.classInstances())
        if (row.type == type) {
          if (index == result.size() || result[index++] != &row)
            mismatch("classes", type);
        }
      if (index != result.size())
        mismatch("classes", type);
    }
    return result;
  }

  const MirPayloadEnum *payloadEnum(const SemanticType &type) const {
    const auto found = payloadRows.find(type);
    const auto *result = found == payloadRows.end() ? nullptr : found->second;
    if (validate && result != program.findPayloadEnum(type))
      mismatch("payload-enum", type);
    return result;
  }

  const MirIntegralEnum *integralEnum(const SemanticType &type) const {
    const auto found = integralRows.find(type);
    const auto *result = found == integralRows.end() ? nullptr : found->second;
    if (validate && result != program.findIntegralEnum(type))
      mismatch("integral-enum", type);
    return result;
  }

private:
  [[noreturn]] static void mismatch(const char *query,
                                    const SemanticType &type) {
    throw std::logic_error(
        std::string("LLVM admission type index mismatch for ") + query +
        " (kind " + std::to_string(type.kind) + ", class " +
        std::to_string(type.classId) + ", enum " + std::to_string(type.enumId) +
        ")");
  }
  const MirProgram &program;
  bool validate;
  std::unordered_map<SemanticType, std::vector<const MirClassInstance *>,
                     TypeBucketHash>
      classRows;
  std::unordered_map<SemanticType, const MirPayloadEnum *, TypeBucketHash>
      payloadRows;
  std::unordered_map<SemanticType, const MirIntegralEnum *, TypeBucketHash>
      integralRows;
};
} // namespace lang::native_llvm
