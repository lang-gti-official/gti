// Hierarchy classification preserves rejection order, indirect cycle
// boundaries, and the throwing wrapper; validation catches a stale whole-query
// cache answer.
#include "../../src/compiler/llvm/placement.h"
#include "gti/backend.h"
#include "gti/lowered_program.h"
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

void unknown(const MirProgram &program, const SemanticType &type,
             std::string_view reason) {
  const auto result = classifyHierarchyStorage(program, type);
  require(!result.known() && !result.contains() && result.reason == reason,
          "unknown hierarchy outcome or first rejection reason changed");
  try {
    static_cast<void>(containsHierarchyStorage(program, type));
  } catch (const BackendUnsupportedError &error) {
    require(error.what() == reason, "throwing wrapper changed its reason");
    return;
  }
  throw std::runtime_error("throwing wrapper accepted unknown storage");
}

struct ValidationMode {
  bool previous = loweredProgramSealValidation();
  ~ValidationMode() { setLoweredProgramSealValidation(previous); }
};
} // namespace

GTI_UNIT_TEST(llvm_hierarchy_outcomes) {
  ValidationMode restore;
  setLoweredProgramSealValidation(true);
  MirProgram program;
  require(classifyHierarchyStorage(program, SemanticType::Int32).known() &&
              !containsHierarchyStorage(program, SemanticType::Int32),
          "scalar exclusion changed");
  unknown(program, SemanticType::Unknown,
          "LLVM hierarchy exclusion has no concrete inline-storage proof");
  unknown(program, SemanticType::referenceTo(SemanticType::Unknown),
          "LLVM hierarchy exclusion requires an exact indirection type");

  // Deliberately forged MIR tests the query's fail-closed boundaries. No
  // source frontend can produce an inline self-containing object.
  const auto object = SemanticType::classType(1);
  auto &classes =
      const_cast<std::vector<MirClassInstance> &>(program.classInstances());
  MirClassInstance row;
  row.id = 1;
  row.declaration = 1;
  row.type = object;
  classes.push_back(row);
  require(classifyHierarchyStorage(program, object).known() &&
              !containsHierarchyStorage(program, object),
          "ordinary class exclusion changed");
  classes[0].polymorphic = true;
  require(classifyHierarchyStorage(program, object).contains(),
          "polymorphic class lost its hierarchy storage");
  classes[0].declaredFields = {
      {.type = SemanticType::Unknown},
      {.type = SemanticType::referenceTo(SemanticType::Unknown)}};
  unknown(program, object,
          "LLVM hierarchy exclusion has no concrete inline-storage proof");
  std::swap(classes[0].declaredFields[0], classes[0].declaredFields[1]);
  unknown(program, object,
          "LLVM hierarchy exclusion requires an exact indirection type");
  classes[0].declaredFields = {{.type = object}};
  unknown(program, object,
          "LLVM hierarchy exclusion has recursive inline object storage");
  classes[0].declaredFields = {{.type = SemanticType::uniqueOwnerOf(object)}};
  require(classifyHierarchyStorage(program, object).contains(),
          "an indirect owner cycle became an inline cycle");
  classes[0].polymorphic = false;
  require(!containsHierarchyStorage(program, object),
          "indirect owner storage became inline hierarchy storage");
  classes[0].declaredFields.clear();
  auto duplicate = classes[0];
  duplicate.id = 2;
  classes.push_back(duplicate);
  unknown(program, object,
          "LLVM hierarchy exclusion has an ambiguous class type");
  classes.pop_back();

  const auto sum = SemanticType::enumType(1);
  auto &payloads =
      const_cast<std::vector<MirPayloadEnum> &>(program.payloadEnums());
  MirPayloadEnum payload;
  payload.declaration = 1;
  payload.type = sum;
  payload.variants.push_back({.variantIndex = 0,
                              .payloadTypes = {sum},
                              .payloadSymbols = {1},
                              .payloadRequiresActiveCleanup = {false}});
  payloads.push_back(payload);
  unknown(program, sum,
          "LLVM hierarchy exclusion has recursive inline object storage");
  payloads[0].variants[0].payloadTypes = {SemanticType::referenceTo(sum)};
  require(!containsHierarchyStorage(program, sum),
          "borrowed recursive payload acquired inline storage");

  {
    HierarchyStorageScope scope(program);
    const auto first = classifyHierarchyStorage(program, SemanticType::Unknown);
    HierarchyStorageScope nested(program);
    require(classifyHierarchyStorage(program, SemanticType::Unknown) == first,
            "nested cache changed the explicit unknown result");
    unknown(program, SemanticType::Unknown, first.reason);
    require(scope.cachedTypes() == 1 && nested.statistics().queries == 4 &&
                nested.statistics().hits == 3 &&
                nested.statistics().rejections == 4,
            "nonthrowing/throwing queries stopped sharing cache telemetry");
  }
  {
    HierarchyStorageScope scope(program);
    require(!classifyHierarchyStorage(program, object).contains(),
            "cache injection fixture must start with ordinary storage");
    // Violate immutability deliberately: the cached answer now disagrees with
    // its input, as if a wrong memo entry had been injected.
    classes[0].polymorphic = true;
    bool caught = false;
    try {
      static_cast<void>(classifyHierarchyStorage(program, object));
    } catch (const BackendUnsupportedError &) {
      throw std::runtime_error("cache corruption became ordinary rejection");
    } catch (const std::logic_error &error) {
      caught = std::string(error.what())
                   .find("hierarchy-storage outcome cache mismatch") !=
               std::string::npos;
    }
    require(caught,
            "validation failed to detect a wrong hierarchy cache answer");
    classes[0].polymorphic = false;
  }
  return 0;
}
