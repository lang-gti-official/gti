// Admission indexes preserve ordered duplicate matches, full type equality on
// hash collisions, first enum matches, and validation of hits and misses.
#include "../../src/compiler/llvm/type_index.h"
#include "unit_test.h"

#include <stdexcept>
#include <string>

namespace {
using namespace lang;
using namespace lang::native_llvm;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace

GTI_UNIT_TEST(llvm_admission_type_index) {
  MirProgram program;
  auto &classes =
      const_cast<std::vector<MirClassInstance> &>(program.classInstances());
  auto &payloads =
      const_cast<std::vector<MirPayloadEnum> &>(program.payloadEnums());
  auto &integrals =
      const_cast<std::vector<MirIntegralEnum> &>(program.integralEnums());
  const auto first =
      SemanticType::classType(1, {}, {CompileTimeValue::uint64(1)});
  const auto collision =
      SemanticType::classType(1, {}, {CompileTimeValue::uint64(2)});
  const auto absent = SemanticType::classType(2);
  require(
      first != collision &&
          TypeBucketHash{}(first) == TypeBucketHash{}(collision),
      "collision control must exercise full equality, not separate buckets");
  MirClassInstance row;
  row.id = 1;
  row.type = first;
  classes.push_back(row);
  row.id = 2;
  row.type = collision;
  classes.push_back(row);
  row.id = 3;
  row.type = first;
  classes.push_back(row);
  const auto sum = SemanticType::enumType(1);
  const auto integral = SemanticType::enumType(2);
  payloads.push_back({.declaration = 1, .type = sum});
  payloads.push_back({.declaration = 2, .type = sum});
  integrals.push_back({.declaration = 3,
                       .type = integral,
                       .underlyingType = SemanticType::Int32});
  integrals.push_back({.declaration = 4,
                       .type = integral,
                       .underlyingType = SemanticType::UInt64});
  for (bool validate : {false, true}) {
    AdmissionTypeIndex index(program, validate);
    const auto duplicates = index.classes(first);
    require(duplicates.size() == 2 && duplicates[0] == &classes[0] &&
                duplicates[1] == &classes[2],
            "class duplicates must survive in original order");
    const auto distinct = index.classes(collision);
    require(distinct.size() == 1 && distinct.front() == &classes[1],
            "hash collision must not alias a different specialization");
    require(index.classes(absent).empty(), "missing class acquired a match");
    require(index.payloadEnum(sum) == &payloads[0] &&
                index.integralEnum(integral) == &integrals[0],
            "enum duplicates must preserve the old scan's first match");
    require(index.payloadEnum(integral) == nullptr &&
                index.integralEnum(sum) == nullptr,
            "payload and integral inventories must remain distinct");
  }
  // Simulate stale indexed facts without reallocating the sealed row vectors.
  // Production never mutates this input; validation must catch a wrong answer
  // rather than allowing it to influence ordinary admission rejection.
  AdmissionTypeIndex checked(program, true);
  classes[0].type = absent;
  for (const auto &query : {first, absent}) {
    bool rejected = false;
    try {
      static_cast<void>(checked.classes(query));
    } catch (const std::logic_error &error) {
      rejected =
          std::string(error.what()).find("type index mismatch for classes") !=
          std::string::npos;
    }
    require(rejected, "validation missed a stale class hit or miss");
  }
  payloads[0].type = integral;
  bool payloadRejected = false;
  try {
    static_cast<void>(checked.payloadEnum(sum));
  } catch (const std::logic_error &error) {
    payloadRejected =
        std::string(error.what()).find("payload-enum") != std::string::npos;
  }
  require(payloadRejected, "validation missed a stale first payload match");
  integrals[0].type = sum;
  bool integralRejected = false;
  try {
    static_cast<void>(checked.integralEnum(integral));
  } catch (const std::logic_error &error) {
    integralRejected =
        std::string(error.what()).find("integral-enum") != std::string::npos;
  }
  require(integralRejected, "validation missed a stale first integral match");
  return 0;
}
