// Defined-integer arithmetic authority with no source-level observable, moved
// unchanged from defined_integer_tests.cpp (its GTI-source cases became
// tests/ui/types/defined_integers files): the compiler's private
// evaluateDefinedIntegerBinary at its signed and unsigned limits for 8- to
// 64-bit domains.
#include "gti/checked_integer.h"
#include "unit_test.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectIntegerValue(const std::optional<lang::CheckedIntegerValue> &actual,
                        lang::CheckedIntegerValue expected,
                        std::string_view context) {
  expect(actual && *actual == lang::normalizeCheckedInteger(expected),
         std::string(context));
}

void testPrivateArithmeticAuthority() {
  for (const std::uint8_t width : {8, 16, 32, 64}) {
    const lang::CheckedIntegerDomain signedDomain{.width = width,
                                                  .signedValue = true};
    const std::uint64_t signedLimit = std::uint64_t{1} << (width - 1);
    const lang::CheckedIntegerValue signedMinimum{.negative = true,
                                                  .magnitude = signedLimit};
    const lang::CheckedIntegerValue signedMaximum{.magnitude = signedLimit - 1};
    const lang::CheckedIntegerValue one{.magnitude = 1};
    const lang::CheckedIntegerValue negativeOne{.negative = true,
                                                .magnitude = 1};
    const lang::CheckedIntegerValue two{.magnitude = 2};

    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, signedMaximum,
                           one, signedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       signedMinimum, "signed wrapping add should reach min");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract,
                           signedMinimum, one, signedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       signedMaximum, "signed wrapping sub should reach max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           signedMaximum, two, signedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       {.negative = true, .magnitude = 2},
                       "signed wrapping mul should preserve low bits");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, signedMaximum,
                           one, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMaximum, "signed saturating add should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, signedMinimum,
                           negativeOne, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMinimum, "signed saturating add should clamp min");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract,
                           signedMinimum, one, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMinimum, "signed saturating sub should clamp min");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract,
                           signedMaximum, negativeOne, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMaximum, "signed saturating sub should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           signedMinimum, negativeOne, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMaximum, "signed saturating mul should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           signedMinimum, two, signedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       signedMinimum, "signed saturating mul should clamp min");

    const lang::CheckedIntegerDomain unsignedDomain{.width = width};
    const lang::CheckedIntegerValue zero{};
    const lang::CheckedIntegerValue unsignedMaximum{
        .magnitude = lang::checkedIntegerMask(unsignedDomain)};
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Add, unsignedMaximum,
                           one, unsignedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       zero, "unsigned wrapping add should reach zero");
    expectIntegerValue(
        lang::evaluateDefinedIntegerBinary(
            lang::CheckedIntegerOperation::Subtract, zero, one, unsignedDomain,
            lang::IntegerArithmeticMode::Wrapping),
        unsignedMaximum, "unsigned wrapping sub should reach max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Multiply,
                           unsignedMaximum, two, unsignedDomain,
                           lang::IntegerArithmeticMode::Wrapping),
                       {.magnitude = unsignedMaximum.magnitude - 1},
                       "unsigned wrapping mul should preserve low bits");
    expectIntegerValue(
        lang::evaluateDefinedIntegerBinary(
            lang::CheckedIntegerOperation::Add, unsignedMaximum, one,
            unsignedDomain, lang::IntegerArithmeticMode::Saturating),
        unsignedMaximum, "unsigned saturating add should clamp max");
    expectIntegerValue(lang::evaluateDefinedIntegerBinary(
                           lang::CheckedIntegerOperation::Subtract, zero, one,
                           unsignedDomain,
                           lang::IntegerArithmeticMode::Saturating),
                       zero, "unsigned saturating sub should clamp zero");
    expectIntegerValue(
        lang::evaluateDefinedIntegerBinary(
            lang::CheckedIntegerOperation::Multiply, unsignedMaximum, two,
            unsignedDomain, lang::IntegerArithmeticMode::Saturating),
        unsignedMaximum, "unsigned saturating mul should clamp max");

    for (const lang::IntegerArithmeticMode mode :
         {lang::IntegerArithmeticMode::Wrapping,
          lang::IntegerArithmeticMode::Saturating}) {
      expectIntegerValue(
          lang::evaluateDefinedIntegerBinary(lang::CheckedIntegerOperation::Add,
                                             {.magnitude = 2}, {.magnitude = 3},
                                             signedDomain, mode),
          {.magnitude = 5}, "in-range arithmetic should preserve exact value");
    }
    expect(!lang::evaluateDefinedIntegerBinary(
               lang::CheckedIntegerOperation::Add, {.magnitude = 2},
               {.magnitude = 3}, signedDomain,
               lang::IntegerArithmeticMode::OverflowPredicate),
           "checked-result arithmetic should retain its explicit outcome");
  }

  expect(!lang::evaluateDefinedIntegerBinary(
             lang::CheckedIntegerOperation::Divide, {.magnitude = 4},
             {.magnitude = 2}, {.width = 8, .signedValue = true},
             lang::IntegerArithmeticMode::Wrapping),
         "defined arithmetic should reject unsupported operations");
  expect(!lang::evaluateDefinedIntegerBinary(
             lang::CheckedIntegerOperation::Add, {.magnitude = 1},
             {.magnitude = 1}, {}, lang::IntegerArithmeticMode::Wrapping),
         "defined arithmetic should reject invalid domains");
}

} // namespace

GTI_UNIT_TEST(defined_integer_evaluation) {
  testPrivateArithmeticAuthority();
  return failures == 0 ? 0 : 1;
}
