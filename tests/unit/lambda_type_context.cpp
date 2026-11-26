// Substituting a generic lambda preserves the original context and compares
// independently constructed lambda identities by all four enclosing lists.
#include "gti/semantic_model.h"
#include "unit_test.h"

#include <array>
#include <stdexcept>

GTI_UNIT_TEST(lambda_type_context) {
  using namespace lang;
  const auto make = [](SemanticType classType, SemanticType functionType,
                       CompileTimeValue classValue,
                       CompileTimeValue functionValue) {
    return SemanticType::lambdaType(
        7, SemanticType::Void, {}, {}, std::array{classType},
        std::array{functionType}, std::array{classValue},
        std::array{functionValue});
  };
  const auto parameter = SemanticType::typeParameter(1);
  const auto count = CompileTimeValue::parameter(2);
  const auto one = CompileTimeValue::uint64(1);
  const auto two = CompileTimeValue::uint64(2);
  const auto open = make(parameter, parameter, count, count);
  const auto copy = open;
  const auto concrete = substituteSemanticType(
      copy, {.types = {{1, SemanticType::Char}}, .values = {{2, one}}});
  if (open != make(parameter, parameter, count, count) || copy != open)
    throw std::runtime_error(
        "substitution changed the shared generic identity");
  if (concrete != make(SemanticType::Char, SemanticType::Char, one, one) ||
      concrete == make(SemanticType::Bool, SemanticType::Char, one, one) ||
      concrete == make(SemanticType::Char, SemanticType::Bool, one, one) ||
      concrete == make(SemanticType::Char, SemanticType::Char, two, one) ||
      concrete == make(SemanticType::Char, SemanticType::Char, one, two))
    throw std::runtime_error("lambda identity lost an enclosing type or value");
  return 0;
}
