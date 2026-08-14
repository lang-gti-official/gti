#include "gti/frontend.h"
#include "gti/mir_dominance.h"
#include "gti/mir_printer.h"
#include "gti/optimization/effects.h"
#include "gti/optimization/rewrite.h"
#include "gti/optimizer.h"
#include "gti/support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const lang::MirBody *findFunction(const lang::FrontendResult &frontend,
                                  const std::string &name) {
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr || instance.source->name().lexeme != name) {
      continue;
    }
    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    return mir == nullptr ? nullptr : &mir->body;
  }
  return nullptr;
}

const lang::HirFunctionInstance *
findHirFunction(const lang::FrontendResult &frontend, const std::string &name) {
  const auto found =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [&](const lang::HirFunctionInstance &instance) {
                     return instance.source != nullptr &&
                            instance.source->name().lexeme == name;
                   });
  return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
}

bool hasIntegerValue(const std::optional<lang::CheckedIntegerOutcome> &outcome,
                     lang::CheckedIntegerValue expected) {
  if (!outcome) {
    return false;
  }
  const auto *value = std::get_if<lang::CheckedIntegerValue>(&*outcome);
  return value != nullptr && *value == expected;
}

bool hasIntegerFailure(
    const std::optional<lang::CheckedIntegerOutcome> &outcome,
    lang::CheckedIntegerFailure expected) {
  if (!outcome) {
    return false;
  }
  const auto *failure = std::get_if<lang::CheckedIntegerFailure>(&*outcome);
  return failure != nullptr && *failure == expected;
}

void testCheckedIntegerContract() {
  using Operation = lang::CheckedIntegerOperation;
  using Failure = lang::CheckedIntegerFailure;
  using Value = lang::CheckedIntegerValue;

  const lang::CheckedIntegerDomain signed8{.width = 8, .signedValue = true};
  const lang::CheckedIntegerDomain signed32{.width = 32, .signedValue = true};
  const lang::CheckedIntegerDomain unsigned8{.width = 8};
  const lang::CheckedIntegerDomain unsigned64{.width = 64};

  expect(hasIntegerValue(lang::evaluateCheckedIntegerBinary(
                             Operation::Add, Value{.magnitude = 40},
                             Value{.magnitude = 2}, signed8),
                         Value{.magnitude = 42}),
         "checked integer addition should produce an in-range value");
  expect(hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                               Operation::Add, Value{.magnitude = 127},
                               Value{.magnitude = 1}, signed8),
                           Failure::Overflow) &&
             hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                                   Operation::Subtract, Value{.magnitude = 0},
                                   Value{.magnitude = 1}, unsigned8),
                               Failure::Overflow),
         "signed overflow and unsigned underflow should be explicit outcomes");
  const Value uint64Maximum{.magnitude =
                                std::numeric_limits<std::uint64_t>::max()};
  expect(hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                               Operation::Add, uint64Maximum,
                               Value{.magnitude = 1}, unsigned64),
                           Failure::Overflow) &&
             hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                                   Operation::Multiply, uint64Maximum,
                                   Value{.magnitude = 2}, unsigned64),
                               Failure::Overflow),
         "uint64_t arithmetic should detect host-magnitude overflow before "
         "performing it");
  expect(hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                               Operation::Multiply, Value{.magnitude = 64},
                               Value{.magnitude = 2}, signed8),
                           Failure::Overflow) &&
             hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                                   Operation::Divide,
                                   Value{.negative = true, .magnitude = 128},
                                   Value{.negative = true, .magnitude = 1},
                                   signed8),
                               Failure::Overflow),
         "multiplication and signed minimum division should detect overflow");
  expect(hasIntegerFailure(
             lang::evaluateCheckedIntegerBinary(
                 Operation::Divide, Value{.magnitude = 7}, Value{}, signed8),
             Failure::DivisionByZero) &&
             hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                                   Operation::Remainder, Value{.magnitude = 7},
                                   Value{}, signed8),
                               Failure::ModuloByZero),
         "division and remainder should retain distinct zero-divisor traps");
  expect(hasIntegerValue(lang::evaluateCheckedIntegerBinary(
                             Operation::Remainder,
                             Value{.negative = true, .magnitude = 128},
                             Value{.negative = true, .magnitude = 1}, signed8),
                         Value{}),
         "signed minimum modulo -1 should be the defined zero result");
  expect(
      hasIntegerFailure(lang::evaluateCheckedIntegerUnary(
                            Operation::Negate,
                            Value{.negative = true, .magnitude = 128}, signed8),
                        Failure::Overflow) &&
          hasIntegerValue(lang::evaluateCheckedIntegerUnary(
                              Operation::BitwiseNot, Value{}, signed8),
                          Value{.negative = true, .magnitude = 1}),
      "unary negation should trap at the minimum while complement remains "
      "defined by bit pattern");
  expect(hasIntegerValue(
             lang::evaluateCheckedIntegerBinary(
                 Operation::ShiftLeft, Value{.magnitude = 1},
                 Value{.magnitude = 31}, signed32),
             Value{.negative = true, .magnitude = std::uint64_t{1} << 31U}) &&
             hasIntegerValue(lang::evaluateCheckedIntegerBinary(
                                 Operation::ShiftRight,
                                 Value{.negative = true, .magnitude = 2},
                                 Value{.magnitude = 1}, signed32),
                             Value{.negative = true, .magnitude = 1}),
         "left shift should wrap by bit pattern and signed right shift should "
         "be arithmetic");
  expect(
      hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                            Operation::ShiftLeft, Value{.magnitude = 1},
                            Value{.negative = true, .magnitude = 1}, signed32),
                        Failure::NegativeShiftCount) &&
          hasIntegerFailure(lang::evaluateCheckedIntegerBinary(
                                Operation::ShiftRight, Value{.magnitude = 1},
                                Value{.magnitude = 32}, signed32),
                            Failure::ShiftCountOutOfRange),
      "invalid shift counts should retain their exact failure category");
  expect(
      hasIntegerValue(lang::evaluateCheckedIntegerBinary(
                          Operation::BitwiseAnd,
                          Value{.negative = true, .magnitude = 1},
                          Value{.magnitude = 15}, signed8),
                      Value{.magnitude = 15}) &&
          hasIntegerValue(lang::evaluateCheckedIntegerBinary(
                              Operation::BitwiseXor, Value{.magnitude = 0xAA},
                              Value{.magnitude = 0x0F}, unsigned8),
                          Value{.magnitude = 0xA5}) &&
          hasIntegerValue(
              lang::evaluateCheckedIntegerUnary(
                  Operation::BitwiseNot, Value{.magnitude = 0x0F}, unsigned8),
              Value{.magnitude = 0xF0}),
      "bitwise operations should use the domain-width bit pattern");
  expect(
      !lang::evaluateCheckedIntegerBinary(Operation::Add, Value{.magnitude = 1},
                                          Value{.magnitude = 1},
                                          {.width = 0, .signedValue = true}) &&
          !lang::evaluateCheckedIntegerBinary(
              Operation::Add, Value{.magnitude = 1}, Value{.magnitude = 1},
              {.width = 65}) &&
          !lang::evaluateCheckedIntegerBinary(Operation::Add,
                                              Value{.magnitude = 128},
                                              Value{.magnitude = 1}, signed8) &&
          !lang::evaluateCheckedIntegerUnary(Operation::Negate,
                                             Value{.magnitude = 1}, unsigned8),
      "invalid domains, operands, and unsupported operations should be "
      "rejected");

  const auto signedValue = [](int value) {
    return Value{.negative = value < 0,
                 .magnitude = static_cast<std::uint64_t>(
                     value < 0 ? -static_cast<std::int64_t>(value) : value)};
  };
  const auto matchesSigned8 = [&](const auto &outcome, int expected) {
    return expected < -128 || expected > 127
               ? hasIntegerFailure(outcome, Failure::Overflow)
               : hasIntegerValue(outcome, signedValue(expected));
  };
  bool signed8Exhaustive = true;
  for (int left = -128; left <= 127; ++left) {
    for (int right = -128; right <= 127; ++right) {
      signed8Exhaustive &= matchesSigned8(
          lang::evaluateCheckedIntegerBinary(Operation::Add, signedValue(left),
                                             signedValue(right), signed8),
          left + right);
      signed8Exhaustive &=
          matchesSigned8(lang::evaluateCheckedIntegerBinary(
                             Operation::Subtract, signedValue(left),
                             signedValue(right), signed8),
                         left - right);
      signed8Exhaustive &=
          matchesSigned8(lang::evaluateCheckedIntegerBinary(
                             Operation::Multiply, signedValue(left),
                             signedValue(right), signed8),
                         left * right);
      const auto division = lang::evaluateCheckedIntegerBinary(
          Operation::Divide, signedValue(left), signedValue(right), signed8);
      const auto remainder = lang::evaluateCheckedIntegerBinary(
          Operation::Remainder, signedValue(left), signedValue(right), signed8);
      if (right == 0) {
        signed8Exhaustive &=
            hasIntegerFailure(division, Failure::DivisionByZero) &&
            hasIntegerFailure(remainder, Failure::ModuloByZero);
      } else {
        signed8Exhaustive &= matchesSigned8(division, left / right);
        signed8Exhaustive &=
            hasIntegerValue(remainder, signedValue(left % right));
      }
    }
  }
  expect(signed8Exhaustive,
         "checked signed arithmetic should match every int8_t input pair");

  const auto matchesUnsigned8 = [&](const auto &outcome, int expected) {
    return expected < 0 || expected > 255
               ? hasIntegerFailure(outcome, Failure::Overflow)
               : hasIntegerValue(
                     outcome,
                     Value{.magnitude = static_cast<std::uint64_t>(expected)});
  };
  bool unsigned8Exhaustive = true;
  for (int left = 0; left <= 255; ++left) {
    for (int right = 0; right <= 255; ++right) {
      const Value leftValue{.magnitude = static_cast<std::uint64_t>(left)};
      const Value rightValue{.magnitude = static_cast<std::uint64_t>(right)};
      unsigned8Exhaustive &= matchesUnsigned8(
          lang::evaluateCheckedIntegerBinary(Operation::Add, leftValue,
                                             rightValue, unsigned8),
          left + right);
      unsigned8Exhaustive &= matchesUnsigned8(
          lang::evaluateCheckedIntegerBinary(Operation::Subtract, leftValue,
                                             rightValue, unsigned8),
          left - right);
      unsigned8Exhaustive &= matchesUnsigned8(
          lang::evaluateCheckedIntegerBinary(Operation::Multiply, leftValue,
                                             rightValue, unsigned8),
          left * right);
    }
  }
  expect(unsigned8Exhaustive,
         "checked unsigned arithmetic should match every uint8_t input pair");
}

void testMirIntegrityAndIdentityPipeline() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-foundation.gti", R"(
extern "C" {
int32_t native_probe(int32_t value);
}

class Counter {
  int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  ~Counter() {}
  int32_t read() { return this.value; }
  int32_t& borrow() { return this.value; }
};

class IntView {
  int32_t& source;

public:
  IntView(int32_t ignored, int32_t& value) : source(value) {}
  int32_t read() { return this.source; }
};

class ViewCallable {
  int32_t& source;

public:
  ViewCallable(int32_t& value) : source(value) {}
  IntView operator()() { return IntView(0, this.source); }
};

class RangeSentinel {
  int32_t final_position;

public:
  RangeSentinel(int32_t value) : final_position(value) {}
  int32_t position() { return this.final_position; }
};

class RangeIterator {
  mut int32_t current;

public:
  RangeIterator(int32_t value) : current(value) {}
  int32_t& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(RangeSentinel& sentinel) {
    return this.current != sentinel.position();
  }
};

class IntRange {
  int32_t count;

public:
  IntRange(int32_t value) : count(value) {}
  RangeIterator begin() { return RangeIterator(0); }
  RangeSentinel end() { return RangeSentinel(this.count); }
};

int32_t range_sum() {
  IntRange values = IntRange(3);
  mut int32_t sum = 0;
  for (int32_t value : values) {
    sum += value;
  }
  return sum;
}

int32_t choose(bool condition) {
  if (condition) {
    return 1;
  }
  return 2;
}

int32_t& relay(int32_t& value, int32_t ignored) {
  return value;
}

int32_t& relay_alias(int32_t& value) {
  int32_t& local_alias = value;
  return local_alias;
}

int32_t& relay_branch_alias(bool condition, int32_t& value) {
  int32_t& local_alias = value;
  if (condition) {
    int32_t& branch_alias = local_alias;
    return branch_alias;
  }
  return local_alias;
}

int32_t& relay_independent_aliases(int32_t& value) {
  int32_t& first = value;
  int32_t& second = value;
  return first;
}

int32_t borrowed_read() {
  mut int32_t value = 7;
  int32_t& reference = value;
  return reference;
}

int32_t borrow_across_branch(bool condition) {
  mut int32_t value = 9;
  int32_t& reference = value;
  if (condition) {
    int32_t observed = reference;
  }
  return reference;
}

int32_t borrow_ends_at_join(bool condition) {
  mut int32_t value = 9;
  int32_t& reference = value;
  if (condition) {
    int32_t left = reference;
  } else {
    int32_t right = reference;
  }
  return value;
}

int32_t branch_local_alias(bool condition) {
  mut int32_t value = 9;
  if (condition) {
    int32_t& first = value;
    int32_t& second = first;
    int32_t observed = second;
  }
  return value;
}

int32_t borrow_in_loop_condition() {
  Counter counter = Counter(1);
  mut int32_t iterations = 0;
  while (counter.borrow() > iterations) {
    iterations++;
  }
  return iterations;
}

int32_t two_borrows() {
  mut int32_t first = 1;
  mut int32_t second = 2;
  int32_t& first_ref = first;
  int32_t& second_ref = second;
  return first_ref + second_ref;
}

int32_t aliased_borrow() {
  mut int32_t value = 3;
  int32_t& first = value;
  int32_t& second = first;
  return second;
}

int main() {
  Counter counter = Counter(1);
  mut int32_t value = 1;
  int32_t ignored = 0;
  int32_t& relayed = relay(value, ignored);
  IntView direct = IntView(ignored, value);
  ViewCallable callable = ViewCallable(value);
  IntView called = callable();
  return choose(counter.read() == 1) + relayed + direct.read() +
         called.read() + range_sum() - 6;
}
)");
  expect(frontend.canGenerateCode(),
         "optimizer fixture should produce valid frontend IR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirVerificationResult verified =
      lang::verifyMirProgram(frontend.mir);
  expect(verified.valid(),
         "the reusable verifier should accept frontend-produced MIR");

  const lang::HirFunctionInstance *rangeHir =
      findHirFunction(frontend, "range_sum");
  std::vector<lang::SymbolId> generatedRangeSymbols;
  if (rangeHir != nullptr) {
    for (const lang::HirBinding &binding : rangeHir->body.bindings) {
      if (binding.variable != nullptr &&
          binding.variable->name().lexeme.starts_with("__gti_")) {
        generatedRangeSymbols.push_back(binding.info.symbol);
      }
    }
  }
  std::sort(generatedRangeSymbols.begin(), generatedRangeSymbols.end());
  expect(rangeHir != nullptr && generatedRangeSymbols.size() == 3 &&
             generatedRangeSymbols.front() != 0 &&
             std::adjacent_find(generatedRangeSymbols.begin(),
                                generatedRangeSymbols.end()) ==
                 generatedRangeSymbols.end(),
         "range-for lowering should assign distinct semantic symbols to its "
         "generated range, iterator, and sentinel bindings even though their "
         "tokens share one source anchor");

  const lang::HirFunctionInstance *relayHir =
      findHirFunction(frontend, "relay");
  const lang::MirFunctionInstance *relayMir =
      relayHir == nullptr ? nullptr
                          : frontend.mir.findFunctionInstance(relayHir->id);
  expect(relayHir != nullptr && relayMir != nullptr &&
             relayHir->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             relayHir->returnBorrowParameter == 0 &&
             relayHir->returnBorrowAccess == lang::AccessMode::ReadOnly &&
             relayHir->parameterBindings.size() == 2 &&
             relayMir->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             relayMir->returnBorrowParameter == 0 &&
             relayMir->returnBorrowAccess == lang::AccessMode::ReadOnly &&
             relayMir->parameterBindings == relayHir->parameterBindings,
         "HIR and MIR should preserve the exact read-only argument return "
         "dependency and its formal binding identities");

  const lang::MirLoan *relayReturnLoan = nullptr;
  if (relayMir != nullptr) {
    for (const lang::MirBlock &block : relayMir->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Return &&
          block.terminator.returnLoan) {
        relayReturnLoan = relayMir->body.findLoan(*block.terminator.returnLoan);
        break;
      }
    }
  }
  expect(relayReturnLoan != nullptr && relayReturnLoan->entry &&
             relayReturnLoan->kind == lang::MirLoanKind::Return &&
             relayReturnLoan->escapes &&
             relayReturnLoan->carriers.size() == 1 && relayHir != nullptr &&
             relayReturnLoan->carriers.front() ==
                 relayHir->parameterBindings.front(),
         "a direct borrowed formal return should remain one entry loan and "
         "be attached explicitly to its return terminator");

  const lang::HirFunctionInstance *aliasHir =
      findHirFunction(frontend, "relay_alias");
  const lang::MirFunctionInstance *aliasMir =
      aliasHir == nullptr ? nullptr
                          : frontend.mir.findFunctionInstance(aliasHir->id);
  const lang::HirFunctionInstance *branchAliasHir =
      findHirFunction(frontend, "relay_branch_alias");
  const lang::MirFunctionInstance *branchAliasMir =
      branchAliasHir == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(branchAliasHir->id);
  const auto entryLoan = [](const lang::MirFunctionInstance *instance) {
    if (instance == nullptr) {
      return static_cast<const lang::MirLoan *>(nullptr);
    }
    const auto found =
        std::find_if(instance->body.loans.begin(), instance->body.loans.end(),
                     [](const lang::MirLoan &loan) { return loan.entry; });
    return found == instance->body.loans.end() ? nullptr : &*found;
  };
  const auto returnedLoan = [](const lang::MirFunctionInstance *instance) {
    if (instance == nullptr) {
      return static_cast<const lang::MirLoan *>(nullptr);
    }
    for (const lang::MirBlock &block : instance->body.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Return &&
          block.terminator.returnLoan) {
        return instance->body.findLoan(*block.terminator.returnLoan);
      }
    }
    return static_cast<const lang::MirLoan *>(nullptr);
  };
  const lang::MirLoan *aliasLoan = entryLoan(aliasMir);
  const lang::MirLoan *branchAliasLoan = entryLoan(branchAliasMir);
  const lang::MirLoan *aliasReturnLoan = returnedLoan(aliasMir);
  const lang::MirLoan *branchAliasReturnLoan = returnedLoan(branchAliasMir);
  expect(aliasHir != nullptr && aliasMir != nullptr && aliasLoan != nullptr &&
             aliasReturnLoan != nullptr && aliasLoan->source != 0 &&
             aliasLoan->carriers.size() >= 2 &&
             std::find(aliasLoan->carriers.begin(), aliasLoan->carriers.end(),
                       aliasHir->parameterBindings.front()) !=
                 aliasLoan->carriers.end() &&
             aliasReturnLoan->id == aliasLoan->id &&
             aliasReturnLoan->source == aliasLoan->source &&
             aliasReturnLoan->carriers.size() >= 2 &&
             lang::verifyMirBody(aliasMir->body).valid(),
         "a direct read-only local reference alias should share the formal "
         "entry loan and extend its carrier set");
  expect(branchAliasHir != nullptr && branchAliasMir != nullptr &&
             branchAliasLoan != nullptr && branchAliasReturnLoan != nullptr &&
             branchAliasReturnLoan->id == branchAliasLoan->id &&
             branchAliasReturnLoan->source == branchAliasLoan->source &&
             branchAliasReturnLoan->carriers.size() >= 3 &&
             std::find(branchAliasLoan->carriers.begin(),
                       branchAliasLoan->carriers.end(),
                       branchAliasHir->parameterBindings[1]) !=
                 branchAliasLoan->carriers.end() &&
             lang::verifyMirBody(branchAliasMir->body).valid(),
         "read-only aliases across a branch should share the formal entry loan "
         "while retaining its source");

  const lang::HirFunctionInstance *independentHir =
      findHirFunction(frontend, "relay_independent_aliases");
  const lang::MirFunctionInstance *independentMir =
      independentHir == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(independentHir->id);
  std::vector<const lang::MirLoan *> independentChildren;
  if (independentMir != nullptr) {
    for (const lang::MirLoan &loan : independentMir->body.loans) {
      if (!loan.entry) {
        independentChildren.push_back(&loan);
      }
    }
  }
  const lang::MirLoan *independentEntry = entryLoan(independentMir);
  expect(independentMir != nullptr && independentChildren.empty() &&
             independentEntry != nullptr &&
             independentEntry->semanticLoan != 0 &&
             independentEntry->carriers.size() >= 3 &&
             lang::verifyMirBody(independentMir->body).valid(),
         "independent read-only aliases of one formal parameter should share "
         "one semantic loan and lexical endpoint");

  const lang::HirFunctionInstance *mainHir = findHirFunction(frontend, "main");
  const lang::MirFunctionInstance *mainMir =
      mainHir == nullptr ? nullptr
                         : frontend.mir.findFunctionInstance(mainHir->id);
  const lang::MirInstruction *relayCall = nullptr;
  if (mainMir != nullptr && relayHir != nullptr) {
    for (const lang::MirBlock &block : mainMir->body.blocks) {
      const auto call = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == relayHir->id;
          });
      if (call != block.instructions.end()) {
        relayCall = &*call;
        break;
      }
    }
  }
  lang::MirPlaceId relayArgumentPlace = 0;
  if (relayCall != nullptr && !relayCall->operands.empty() &&
      relayCall->operands.front().kind == lang::MirOperandKind::Value &&
      mainMir != nullptr) {
    const lang::MirValue *prepared =
        mainMir->body.findValue(relayCall->operands.front().value);
    const lang::MirBlock *definitionBlock =
        prepared == nullptr
            ? nullptr
            : mainMir->body.findBlock(prepared->definitionBlock);
    const auto definition =
        definitionBlock == nullptr
            ? std::vector<lang::MirInstruction>::const_iterator{}
            : std::find_if(definitionBlock->instructions.begin(),
                           definitionBlock->instructions.end(),
                           [&](const lang::MirInstruction &instruction) {
                             return instruction.id == prepared->definition;
                           });
    if (definitionBlock != nullptr &&
        definition != definitionBlock->instructions.end() &&
        definition->kind == lang::MirInstructionKind::CallInput &&
        definition->operands.size() == 1) {
      relayArgumentPlace = definition->operands.front().place;
    }
  }
  expect(relayCall != nullptr && relayCall->loan &&
             relayCall->borrowOrigin == lang::BorrowOriginKind::Argument &&
             relayCall->borrowArgument == 0 &&
             relayCall->borrowAccess == lang::AccessMode::ReadOnly &&
             !relayCall->operands.empty() && relayArgumentPlace != 0 &&
             mainMir != nullptr &&
             mainMir->body.findLoan(*relayCall->loan) != nullptr &&
             mainMir->body.findLoan(*relayCall->loan)->source ==
                 relayArgumentPlace,
         "a relay call result should retain its selected argument and actual "
         "caller-side source place");

  const lang::HirFunctionInstance *callOperatorHir =
      [&]() -> const lang::HirFunctionInstance * {
    const auto found =
        std::find_if(frontend.hir.functionInstances().begin(),
                     frontend.hir.functionInstances().end(),
                     [](const lang::HirFunctionInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->operatorName() &&
                              instance.source->operatorName()->kind ==
                                  lang::OverloadedOperator::Call;
                     });
    return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
  }();
  const lang::MirFunctionInstance *callOperatorMir =
      callOperatorHir == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(callOperatorHir->id);
  const lang::MirInstruction *carrierOperatorCall = nullptr;
  if (mainMir != nullptr && callOperatorHir != nullptr) {
    for (const lang::MirBlock &block : mainMir->body.blocks) {
      const auto call = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == callOperatorHir->id;
          });
      if (call != block.instructions.end()) {
        carrierOperatorCall = &*call;
        break;
      }
    }
  }
  expect(callOperatorMir != nullptr &&
             callOperatorMir->returnBorrowOrigin ==
                 lang::BorrowOriginKind::Receiver &&
             carrierOperatorCall != nullptr && carrierOperatorCall->receiver &&
             carrierOperatorCall->loan &&
             carrierOperatorCall->borrowOrigin ==
                 lang::BorrowOriginKind::Receiver &&
             mainMir != nullptr &&
             mainMir->body.findLoan(*carrierOperatorCall->loan) != nullptr,
         "operator() returning a direct stored-reference carrier should retain "
         "its selected Receiver summary, exact MIR receiver, and result loan");

  const lang::HirConstructorInstance *viewConstructorHir =
      [&]() -> const lang::HirConstructorInstance * {
    const auto found =
        std::find_if(frontend.hir.constructorInstances().begin(),
                     frontend.hir.constructorInstances().end(),
                     [](const lang::HirConstructorInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->name().lexeme == "IntView";
                     });
    return found == frontend.hir.constructorInstances().end() ? nullptr
                                                              : &*found;
  }();
  const lang::MirConstructorInstance *viewConstructorMir =
      viewConstructorHir == nullptr
          ? nullptr
          : frontend.mir.findConstructorInstance(viewConstructorHir->id);
  const lang::MirInstruction *viewConstruct = nullptr;
  if (mainMir != nullptr && viewConstructorHir != nullptr) {
    for (const lang::MirBlock &block : mainMir->body.blocks) {
      const auto construct = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Construct &&
                   instruction.constructorTarget == viewConstructorHir->id;
          });
      if (construct != block.instructions.end()) {
        viewConstruct = &*construct;
        break;
      }
    }
  }
  expect(viewConstructorHir != nullptr && viewConstructorMir != nullptr &&
             viewConstructorHir->borrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             viewConstructorHir->borrowParameter == 1 &&
             viewConstructorHir->parameterBindings.size() == 2 &&
             viewConstructorMir->borrowOrigin ==
                 lang::BorrowOriginKind::Argument &&
             viewConstructorMir->borrowParameter == 1 &&
             viewConstructorMir->borrowAccess == lang::AccessMode::ReadOnly &&
             viewConstructorMir->parameterBindings ==
                 viewConstructorHir->parameterBindings &&
             viewConstruct != nullptr && viewConstruct->loan &&
             viewConstruct->borrowOrigin == lang::BorrowOriginKind::Argument &&
             viewConstruct->borrowArgument == 1,
         "stored-reference constructor targets and Construct instructions "
         "should retain the exact summarized reference parameter and access");

  const auto hasProgramVerificationMessage = [](const lang::MirProgram &program,
                                                std::string_view text) {
    const lang::MirVerificationResult result = lang::verifyMirProgram(program);
    return !result.valid() &&
           std::any_of(result.errors.begin(), result.errors.end(),
                       [&](const lang::MirVerificationError &error) {
                         return error.message.find(text) != std::string::npos;
                       });
  };

  if (aliasHir != nullptr && aliasMir != nullptr && aliasLoan != nullptr &&
      aliasReturnLoan != nullptr) {
    lang::MirBody missingFormalCarrier = aliasMir->body;
    lang::MirLoan &missingCarrier =
        missingFormalCarrier.loans[aliasLoan->id - 1];
    std::erase(missingCarrier.carriers, aliasHir->parameterBindings.front());
    const lang::MirVerificationResult missingCarrierResult =
        lang::verifyMirBody(missingFormalCarrier);
    expect(!missingCarrierResult.valid() &&
               std::any_of(missingCarrierResult.errors.begin(),
                           missingCarrierResult.errors.end(),
                           [](const lang::MirVerificationError &error) {
                             return error.message.find(
                                        "remains one of its carriers") !=
                                    std::string::npos;
                           }),
           "the verifier should reject an entry loan that drops its formal "
           "source binding from the carrier set");

    lang::MirProgram wrongFormalCarrier = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongFormalCarrier.functionInstances());
    lang::MirFunctionInstance &wrongAlias = functions[aliasHir->id - 1];
    lang::MirLoan &wrongLoan = wrongAlias.body.loans[aliasReturnLoan->id - 1];
    const auto localCarrier =
        std::find_if(wrongLoan.carriers.begin(), wrongLoan.carriers.end(),
                     [&](lang::HirBindingId binding) {
                       return binding != aliasHir->parameterBindings.front();
                     });
    const lang::HirBindingId localAlias =
        localCarrier == wrongLoan.carriers.end() ? 0 : *localCarrier;
    const auto aliasPlace = std::find_if(
        wrongAlias.body.places.begin(), wrongAlias.body.places.end(),
        [&](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.binding == localAlias;
        });
    if (aliasPlace != wrongAlias.body.places.end()) {
      wrongLoan.source = aliasPlace->id;
    }
    expect(localCarrier != wrongLoan.carriers.end() &&
               aliasPlace != wrongAlias.body.places.end() &&
               hasProgramVerificationMessage(
                   wrongFormalCarrier,
                   "does not originate from the summarized formal parameter"),
           "additional aliases may be carriers, but the verifier should reject "
           "one substituted for the summarized formal source");
  }

  if (relayMir != nullptr) {
    lang::MirProgram invalidSummary = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        invalidSummary.functionInstances());
    functions[relayMir->id - 1].returnBorrowParameter =
        functions[relayMir->id - 1].parameterTypes.size();
    expect(hasProgramVerificationMessage(invalidSummary,
                                         "outside the formal parameters"),
           "the verifier should reject an out-of-range return dependency "
           "summary index");

    lang::MirProgram wrongReturnSource = frontend.mir;
    auto &wrongReturnFunctions =
        const_cast<std::vector<lang::MirFunctionInstance> &>(
            wrongReturnSource.functionInstances());
    lang::MirFunctionInstance &wrongRelay =
        wrongReturnFunctions[relayMir->id - 1];
    const lang::HirBindingId wrongBinding = wrongRelay.parameterBindings[1];
    const auto wrongPlace = std::find_if(
        wrongRelay.body.places.begin(), wrongRelay.body.places.end(),
        [&](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.binding == wrongBinding;
        });
    if (wrongPlace != wrongRelay.body.places.end()) {
      for (lang::MirLoan &loan : wrongRelay.body.loans) {
        if (loan.kind == lang::MirLoanKind::Return && loan.escapes) {
          loan.source = wrongPlace->id;
          loan.carriers = {wrongBinding};
        }
      }
    }
    expect(wrongPlace != wrongRelay.body.places.end() &&
               hasProgramVerificationMessage(
                   wrongReturnSource,
                   "does not originate from the summarized formal parameter"),
           "the verifier should reject an escaping return loan whose source "
           "is a different formal parameter");
  }

  if (mainMir != nullptr && relayCall != nullptr && relayCall->loan) {
    lang::MirProgram wrongCallSource = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongCallSource.functionInstances());
    lang::MirFunctionInstance &wrongMain = functions[mainHir->id - 1];
    lang::MirInstruction *wrongCall = nullptr;
    for (lang::MirBlock &block : wrongMain.body.blocks) {
      const auto call = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == relayHir->id;
          });
      if (call != block.instructions.end()) {
        wrongCall = &*call;
        break;
      }
    }
    if (wrongCall != nullptr && wrongCall->loan) {
      lang::MirLoan &loan = wrongMain.body.loans[*wrongCall->loan - 1];
      const auto different = std::find_if(
          wrongMain.body.places.begin(), wrongMain.body.places.end(),
          [&](const lang::MirPlace &place) {
            return place.id != loan.source &&
                   place.root == lang::MirPlaceRootKind::Binding;
          });
      if (different != wrongMain.body.places.end()) {
        loan.source = different->id;
      }
    }
    expect(wrongCall != nullptr &&
               hasProgramVerificationMessage(
                   wrongCallSource,
                   "does not preserve the selected receiver or argument "
                   "source identity"),
           "the verifier should reject call-result metadata whose loan loses "
           "the selected caller-side source identity");
  }

  if (mainMir != nullptr && viewConstruct != nullptr && viewConstruct->loan &&
      viewConstructorHir != nullptr) {
    lang::MirProgram wrongConstructParameter = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongConstructParameter.functionInstances());
    lang::MirFunctionInstance &wrongMain = functions[mainHir->id - 1];
    lang::MirInstruction *wrongConstruct = nullptr;
    for (lang::MirBlock &block : wrongMain.body.blocks) {
      const auto construct = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Construct &&
                   instruction.constructorTarget == viewConstructorHir->id;
          });
      if (construct != block.instructions.end()) {
        wrongConstruct = &*construct;
        break;
      }
    }
    const auto ignoredBinding = std::find_if(
        mainHir->body.bindings.begin(), mainHir->body.bindings.end(),
        [](const lang::HirBinding &binding) {
          return binding.variable != nullptr &&
                 binding.variable->name().lexeme == "ignored";
        });
    const auto ignoredPlace =
        ignoredBinding == mainHir->body.bindings.end()
            ? wrongMain.body.places.end()
            : std::find_if(
                  wrongMain.body.places.begin(), wrongMain.body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Binding &&
                           place.binding == ignoredBinding->id;
                  });
    if (wrongConstruct != nullptr && wrongConstruct->loan &&
        ignoredPlace != wrongMain.body.places.end()) {
      wrongConstruct->borrowArgument = 0;
      wrongMain.body.loans[*wrongConstruct->loan - 1].source = ignoredPlace->id;
    }
    expect(wrongConstruct != nullptr &&
               ignoredBinding != mainHir->body.bindings.end() &&
               ignoredPlace != wrongMain.body.places.end() &&
               hasProgramVerificationMessage(
                   wrongConstructParameter,
                   "does not match the target constructor summary"),
           "the verifier should reject a Construct that substitutes a "
           "different argument and source for the selected constructor "
           "dependency");
  }

  if (viewConstructorMir != nullptr && viewConstructorHir != nullptr &&
      viewConstructorMir->parameterBindings.size() == 2) {
    lang::MirProgram wrongStoredSource = frontend.mir;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            wrongStoredSource.constructorInstances());
    lang::MirConstructorInstance &wrongConstructor =
        constructors[viewConstructorHir->id - 1];
    const auto wrongParameterPlace = std::find_if(
        wrongConstructor.body.places.begin(),
        wrongConstructor.body.places.end(), [&](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.binding == wrongConstructor.parameterBindings[0];
        });
    const auto storedLoan = std::find_if(
        wrongConstructor.body.loans.begin(), wrongConstructor.body.loans.end(),
        [](const lang::MirLoan &loan) {
          return loan.kind == lang::MirLoanKind::Stored && loan.escapes;
        });
    if (wrongParameterPlace != wrongConstructor.body.places.end() &&
        storedLoan != wrongConstructor.body.loans.end()) {
      storedLoan->source = wrongParameterPlace->id;
    }
    expect(wrongParameterPlace != wrongConstructor.body.places.end() &&
               storedLoan != wrongConstructor.body.loans.end() &&
               hasProgramVerificationMessage(
                   wrongStoredSource,
                   "does not originate from the summarized formal parameter"),
           "the verifier should reject a constructor body whose escaping "
           "stored-reference loan is redirected to another formal binding");
  }

  const auto external =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &instance) {
                     return instance.linkage == lang::LanguageLinkage::C;
                   });
  expect(external != frontend.mir.functionInstances().end() &&
             external->externalSymbol == "native_probe",
         "lowered MIR should retain exact external function identity");

  lang::MirProgram missingExternalSymbol = frontend.mir;
  auto &missingExternalFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          missingExternalSymbol.functionInstances());
  const auto missingExternal = std::find_if(
      missingExternalFunctions.begin(), missingExternalFunctions.end(),
      [](const lang::MirFunctionInstance &instance) {
        return instance.linkage == lang::LanguageLinkage::C;
      });
  if (missingExternal != missingExternalFunctions.end()) {
    missingExternal->externalSymbol.clear();
  }
  const lang::MirVerificationResult missingExternalResult =
      lang::verifyMirProgram(missingExternalSymbol);
  expect(!missingExternalResult.valid() &&
             std::any_of(missingExternalResult.errors.begin(),
                         missingExternalResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("external symbol") !=
                                  std::string::npos;
                         }),
         "the verifier should reject a C-linkage function whose exact symbol "
         "was dropped by a MIR pass");

  lang::MirProgram unexpectedExternalSymbol = frontend.mir;
  auto &unexpectedExternalFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          unexpectedExternalSymbol.functionInstances());
  const auto ordinaryFunction = std::find_if(
      unexpectedExternalFunctions.begin(), unexpectedExternalFunctions.end(),
      [](const lang::MirFunctionInstance &instance) {
        return instance.linkage == lang::LanguageLinkage::Gti;
      });
  if (ordinaryFunction != unexpectedExternalFunctions.end()) {
    ordinaryFunction->externalSymbol = "unexpected";
  }
  const lang::MirVerificationResult unexpectedExternalResult =
      lang::verifyMirProgram(unexpectedExternalSymbol);
  expect(!unexpectedExternalResult.valid() &&
             std::any_of(unexpectedExternalResult.errors.begin(),
                         unexpectedExternalResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("external C symbol") !=
                                  std::string::npos;
                         }),
         "the verifier should reject external C identity on an ordinary GTI "
         "function");

  if (mainHir != nullptr) {
    lang::MirProgram unexpectedEntryAdapter = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        unexpectedEntryAdapter.functionInstances());
    lang::MirFunctionInstance &entry = functions[mainHir->id - 1];
    entry.entryArgumentAppendTarget = 1;
    expect(hasProgramVerificationMessage(
               unexpectedEntryAdapter,
               "no-argument entry point has invalid adapter metadata"),
           "the verifier should reject an argument adapter attached to the "
           "no-argument entry form");

    lang::MirProgram malformedOwnedEntry = frontend.mir;
    auto &ownedFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        malformedOwnedEntry.functionInstances());
    lang::MirFunctionInstance &ownedEntry = ownedFunctions[mainHir->id - 1];
    ownedEntry.entryKind = lang::ProgramEntryKind::OwnedArguments;
    ownedEntry.entryArgumentAppendTarget = 1;
    expect(hasProgramVerificationMessage(
               malformedOwnedEntry,
               "owned-argument entry point has invalid adapter metadata"),
           "the verifier should reject an owned-argument entry contract whose "
           "parameter shape was lost by a MIR pass");
  }

  const std::string before = lang::MirPrinter().print(frontend.mir);
  const std::string repeated = lang::MirPrinter().print(frontend.mir);
  expect(
      before == repeated && before.find("function @") != std::string::npos &&
          before.find("defined=bb") != std::string::npos &&
          before.find(" uses ") != std::string::npos,
      "MIR printing should be deterministic and expose definitions and uses");

  const lang::OptimizedProgram optimized = lang::OptimizationPipeline().run(
      lang::OptimizationRequest{.hir = frontend.hir,
                                .mir = frontend.mir,
                                .level = lang::OptimizationLevel::O2});
  expect(optimized.valid() && optimized.report.verificationEnabled &&
             optimized.report.passes.size() == 1 &&
             optimized.report.passes.front().name ==
                 "fold-literal-identities" &&
             optimized.report.passes.front().shadowMismatches == 0,
         "the owned MIR pipeline should verify and report its bounded shadow "
         "transform");

  const lang::OptimizedProgram unchecked =
      lang::OptimizationPipeline().run(lang::OptimizationRequest{
          .hir = frontend.hir,
          .mir = frontend.mir,
          .level = lang::OptimizationLevel::O0,
          .options = lang::OptimizationOptions{.verifyMir = false}});
  expect(unchecked.valid() && !unchecked.report.verificationEnabled &&
             before == lang::MirPrinter().print(unchecked.mir),
         "disabling verification should not alter the identity snapshot");

  const lang::MirBody *function = findFunction(frontend, "choose");
  expect(function != nullptr, "optimizer fixture should expose choose MIR");
  if (function == nullptr) {
    return;
  }

  lang::MirBody staleUses = *function;
  staleUses.valueUses.clear();
  const lang::MirVerificationResult staleUseResult =
      lang::verifyMirBody(staleUses);
  expect(!staleUseResult.valid() && !staleUseResult.errors.empty() &&
             staleUseResult.errors.front().message.find("value-use index") !=
                 std::string::npos,
         "the verifier should diagnose a stale value-use index");
  expect(lang::rebuildMirValueUses(staleUses) &&
             lang::verifyMirBody(staleUses).valid(),
         "the shared use-index repair should restore valid MIR");

  lang::MirBody staleReachability = *function;
  for (lang::MirBlock &block : staleReachability.blocks) {
    block.reachable = !block.reachable;
  }
  const lang::MirVerificationResult staleReachabilityResult =
      lang::verifyMirBody(staleReachability);
  expect(!staleReachabilityResult.valid() &&
             !staleReachabilityResult.errors.empty() &&
             staleReachabilityResult.errors.front().message.find(
                 "reachability") != std::string::npos,
         "the verifier should diagnose stale reachability facts");
  lang::rebuildMirReachability(staleReachability);
  expect(lang::verifyMirBody(staleReachability).valid(),
         "the shared reachability repair should restore valid MIR");

  lang::MirBody malformed = *function;
  malformed.blocks.front().terminator.kind = lang::MirTerminatorKind::Goto;
  malformed.blocks.front().terminator.target = malformed.blocks.size() + 1;
  const lang::MirVerificationResult malformedResult =
      lang::verifyMirBody(malformed);
  expect(!malformedResult.valid() && !malformedResult.errors.empty() &&
             malformedResult.errors.front().block ==
                 malformed.blocks.front().id &&
             malformedResult.errors.front().message.find("goto target") !=
                 std::string::npos,
         "a malformed CFG rewrite should fail with a useful block diagnostic");

  const lang::MirBody *borrowed = findFunction(frontend, "borrowed_read");
  const lang::MirBody *branched =
      findFunction(frontend, "borrow_across_branch");
  const lang::MirBody *branchEnded =
      findFunction(frontend, "borrow_ends_at_join");
  const lang::MirBody *branchLocalAlias =
      findFunction(frontend, "branch_local_alias");
  const lang::MirBody *loopBorrow =
      findFunction(frontend, "borrow_in_loop_condition");
  const lang::MirBody *twoBorrows = findFunction(frontend, "two_borrows");
  const lang::MirBody *aliasedBorrow = findFunction(frontend, "aliased_borrow");
  expect(borrowed != nullptr && branched != nullptr && branchEnded != nullptr &&
             branchLocalAlias != nullptr && loopBorrow != nullptr &&
             twoBorrows != nullptr && aliasedBorrow != nullptr &&
             lang::verifyMirBody(*borrowed).valid() &&
             lang::verifyMirBody(*branched).valid() &&
             lang::verifyMirBody(*branchEnded).valid() &&
             lang::verifyMirBody(*branchLocalAlias).valid() &&
             lang::verifyMirBody(*loopBorrow).valid() &&
             lang::verifyMirBody(*twoBorrows).valid() &&
             lang::verifyMirBody(*aliasedBorrow).valid(),
         "frontend MIR should balance lexical loans through straight-line and "
         "branching control flow and loop backedges");
  if (borrowed == nullptr || branched == nullptr || branchEnded == nullptr ||
      branchLocalAlias == nullptr || loopBorrow == nullptr ||
      twoBorrows == nullptr || aliasedBorrow == nullptr ||
      borrowed->loans.empty() || branched->loans.empty() ||
      branchEnded->loans.empty() || loopBorrow->loans.empty() ||
      twoBorrows->loans.size() < 2 || aliasedBorrow->loans.empty()) {
    return;
  }
  expect(aliasedBorrow->loans.size() == 1 &&
             aliasedBorrow->loans.front().semanticLoan != 0 &&
             aliasedBorrow->loans.front().carriers.size() == 2,
         "reference aliases should share one semantic and MIR loan while "
         "retaining both carrier bindings");

  bool loopConditionEndsBorrow = false;
  for (const lang::MirBlock &block : loopBorrow->blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Branch) {
      continue;
    }
    const auto producer = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Call &&
                 instruction.loan.has_value();
        });
    const auto end = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::EndBorrow;
        });
    loopConditionEndsBorrow = producer != block.instructions.end() &&
                              end != block.instructions.end() && producer < end;
  }
  expect(loopConditionEndsBorrow,
         "a non-retained receiver borrow should end after the loop condition "
         "before its backedge");

  const auto hasVerificationMessage = [](const lang::MirBody &body,
                                         std::string_view text) {
    const lang::MirVerificationResult result = lang::verifyMirBody(body);
    return !result.valid() && !result.errors.empty() &&
           result.errors.front().message.find(text) != std::string::npos;
  };
  const auto nextInstructionId = [](const lang::MirBody &body) {
    lang::MirInstructionId result = 1;
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        result = std::max(result, instruction.id + 1);
      }
    }
    return result;
  };

  lang::MirBody missingEnd = *borrowed;
  for (lang::MirBlock &block : missingEnd.blocks) {
    std::erase_if(
        block.instructions, [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::EndBorrow;
        });
  }
  expect(hasVerificationMessage(missingEnd, "remains active"),
         "the verifier should reject a non-escaping loan left active at a "
         "normal exit");

  lang::MirBody missingProducer = *borrowed;
  for (lang::MirBlock &block : missingProducer.blocks) {
    std::erase_if(block.instructions,
                  [](const lang::MirInstruction &instruction) {
                    return instruction.kind == lang::MirInstructionKind::Borrow;
                  });
  }
  expect(hasVerificationMessage(missingProducer, "one producing instruction"),
         "the verifier should require one explicit producer for every loan");

  lang::MirBody doubleEnd = *borrowed;
  bool duplicatedEnd = false;
  for (lang::MirBlock &block : doubleEnd.blocks) {
    const auto end = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::EndBorrow;
        });
    if (end == block.instructions.end()) {
      continue;
    }
    lang::MirInstruction duplicate = *end;
    duplicate.id = nextInstructionId(doubleEnd);
    block.instructions.insert(std::next(end), std::move(duplicate));
    duplicatedEnd = true;
    break;
  }
  expect(duplicatedEnd &&
             hasVerificationMessage(doubleEnd, "while it is inactive"),
         "the verifier should reject ending the same loan twice on one path");

  lang::MirBody balancedBranchEnds = *branchEnded;
  for (lang::MirBlock &block : balancedBranchEnds.blocks) {
    std::erase_if(
        block.instructions, [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::EndBorrow;
        });
  }
  const lang::MirBlock *balancedEntry =
      balancedBranchEnds.findBlock(balancedBranchEnds.entry);
  bool endedOnBothBranches = false;
  if (balancedEntry != nullptr &&
      balancedEntry->terminator.kind == lang::MirTerminatorKind::Branch) {
    lang::MirBlock &thenBlock =
        balancedBranchEnds.blocks[balancedEntry->terminator.target - 1];
    lang::MirBlock &elseBlock =
        balancedBranchEnds.blocks[balancedEntry->terminator.elseTarget - 1];
    thenBlock.instructions.push_back(
        {.id = nextInstructionId(balancedBranchEnds),
         .kind = lang::MirInstructionKind::EndBorrow,
         .loan = balancedBranchEnds.loans.front().id});
    elseBlock.instructions.push_back(
        {.id = nextInstructionId(balancedBranchEnds),
         .kind = lang::MirInstructionKind::EndBorrow,
         .loan = balancedBranchEnds.loans.front().id});
    endedOnBothBranches = true;
  }
  expect(endedOnBothBranches && lang::verifyMirBody(balancedBranchEnds).valid(),
         "the verifier should accept one loan ending independently on every "
         "incoming path");

  lang::MirBody useAfterEnd = *borrowed;
  bool movedEndBeforeUse = false;
  for (lang::MirBlock &block : useAfterEnd.blocks) {
    const auto borrow = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Borrow;
        });
    const auto end = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::EndBorrow;
        });
    if (borrow == block.instructions.end() || end == block.instructions.end() ||
        borrow >= end) {
      continue;
    }
    const std::size_t borrowIndex = static_cast<std::size_t>(
        std::distance(block.instructions.begin(), borrow));
    lang::MirInstruction earlyEnd = *end;
    block.instructions.erase(end);
    block.instructions.insert(block.instructions.begin() +
                                  static_cast<std::ptrdiff_t>(borrowIndex + 1),
                              std::move(earlyEnd));
    movedEndBeforeUse = true;
    break;
  }
  expect(movedEndBeforeUse && hasVerificationMessage(useAfterEnd, "used after"),
         "the verifier should reject a borrowed binding used after EndBorrow");

  lang::MirBody inconsistentJoin = *branched;
  const lang::MirBlock *entry =
      inconsistentJoin.findBlock(inconsistentJoin.entry);
  bool endedOnOneBranch = false;
  if (entry != nullptr &&
      entry->terminator.kind == lang::MirTerminatorKind::Branch) {
    lang::MirBlock &thenBlock =
        inconsistentJoin.blocks[entry->terminator.target - 1];
    thenBlock.instructions.push_back(
        {.id = nextInstructionId(inconsistentJoin),
         .kind = lang::MirInstructionKind::EndBorrow,
         .loan = inconsistentJoin.loans.front().id});
    endedOnOneBranch = true;
  }
  expect(endedOnOneBranch &&
             hasVerificationMessage(inconsistentJoin, "at CFG join"),
         "the verifier should reject control-flow joins whose incoming loan "
         "states disagree");

  lang::MirBody duplicateSemanticLoan = *twoBorrows;
  duplicateSemanticLoan.loans[1].semanticLoan =
      duplicateSemanticLoan.loans.front().semanticLoan;
  expect(duplicateSemanticLoan.loans.front().semanticLoan != 0 &&
             hasVerificationMessage(duplicateSemanticLoan,
                                    "represented by more than one MIR loan"),
         "the verifier should reject duplicate semantic loan identities");

  lang::MirBody duplicateCarrier = *aliasedBorrow;
  duplicateCarrier.loans.front().carriers.push_back(
      duplicateCarrier.loans.front().carriers.front());
  expect(hasVerificationMessage(duplicateCarrier, "duplicate carrier binding"),
         "the verifier should reject duplicate carrier identities");
}

void testOrderedCallInputVerification() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("ordered-call-verification.gti", R"(
class OrderedReceiver {
public:
  OrderedReceiver() {}

  int ordered_member(int scalar, int& observed, mut int& changed) mut {
    changed += scalar;
    return observed + changed;
  }
};

int ordered_free(int scalar, int& observed, mut int& changed) {
  changed += scalar;
  return observed + changed;
}

int main() {
  mut OrderedReceiver receiver = OrderedReceiver();
  mut int observed = 2;
  mut int changed = 3;
  int first = ordered_free(1, observed, changed);
  int second = receiver.ordered_member(4, observed, changed);
  return first + second;
}
)");
  const lang::HirFunctionInstance *member =
      findHirFunction(frontend, "ordered_member");
  const lang::HirFunctionInstance *main = findHirFunction(frontend, "main");
  const lang::MirFunctionInstance *mirMain =
      main == nullptr ? nullptr : frontend.mir.findFunctionInstance(main->id);
  expect(frontend.canGenerateCode() && member != nullptr && main != nullptr &&
             mirMain != nullptr && lang::verifyMirProgram(frontend.mir).valid(),
         "the ordered-call verifier fixture should produce valid MIR");
  if (!frontend.canGenerateCode() || member == nullptr || main == nullptr ||
      mirMain == nullptr) {
    return;
  }

  const auto callTo = [](lang::MirBody &body,
                         lang::HirFunctionInstanceId target) {
    for (lang::MirBlock &block : body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == target;
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto inputFor = [](lang::MirBody &body,
                           const lang::MirOperand &operand) {
    if (operand.kind != lang::MirOperandKind::Value) {
      return static_cast<lang::MirInstruction *>(nullptr);
    }
    const lang::MirValue *value = body.findValue(operand.value);
    if (value == nullptr || value->definitionBlock == 0 ||
        value->definitionBlock > body.blocks.size()) {
      return static_cast<lang::MirInstruction *>(nullptr);
    }
    lang::MirBlock &block = body.blocks[value->definitionBlock - 1];
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.id == value->definition;
                     });
    return found == block.instructions.end() ? nullptr : &*found;
  };
  const auto hasMessage = [](const lang::MirBody &body, std::string_view text) {
    const lang::MirVerificationResult result = lang::verifyMirBody(body);
    return std::any_of(result.errors.begin(), result.errors.end(),
                       [&](const lang::MirVerificationError &error) {
                         return error.message.find(text) != std::string::npos;
                       });
  };

  lang::MirBody baseline = mirMain->body;
  lang::MirInstruction *baselineCall = callTo(baseline, member->id);
  expect(baselineCall != nullptr && baselineCall->callSite != 0 &&
             baselineCall->receiver && baselineCall->operands.size() == 3 &&
             lang::verifyMirBody(baseline).valid(),
         "the member invocation should expose one ordered receiver and three "
         "ordered arguments");
  if (baselineCall == nullptr || !baselineCall->receiver ||
      baselineCall->operands.size() != 3) {
    return;
  }

  lang::MirBody missingTarget = baseline;
  lang::MirInstruction *missingTargetCall = callTo(missingTarget, member->id);
  if (missingTargetCall != nullptr) {
    missingTargetCall->functionTarget.reset();
  }
  expect(missingTargetCall != nullptr &&
             hasMessage(missingTarget, "defined-failure metadata"),
         "the verifier should reject an ordered ordinary call that loses its "
         "exact function target");

  lang::MirBody wrongSite = baseline;
  lang::MirInstruction *wrongSiteCall = callTo(wrongSite, member->id);
  lang::MirInstruction *wrongSiteReceiver =
      wrongSiteCall == nullptr || !wrongSiteCall->receiver
          ? nullptr
          : inputFor(wrongSite, *wrongSiteCall->receiver);
  if (wrongSiteReceiver != nullptr) {
    wrongSiteReceiver->callSite += 100000;
  }
  expect(wrongSiteReceiver != nullptr &&
             hasMessage(wrongSite,
                        "call input use does not match its ordered invocation"),
         "the verifier should reject a receiver checkpoint assigned to a "
         "different call site");

  lang::MirBody duplicateInput = baseline;
  lang::MirInstruction *duplicateCall = callTo(duplicateInput, member->id);
  if (duplicateCall != nullptr && duplicateCall->operands.size() == 3) {
    duplicateCall->operands[2] = duplicateCall->operands[1];
    (void)lang::rebuildMirValueUses(duplicateInput);
  }
  expect(duplicateCall != nullptr &&
             hasMessage(duplicateInput,
                        "call input must have exactly one executable use"),
         "the verifier should reject duplicate and abandoned call inputs");

  lang::MirBody bypassInput = baseline;
  lang::MirInstruction *bypassCall = callTo(bypassInput, member->id);
  lang::MirInstruction *bypassed =
      bypassCall == nullptr || bypassCall->operands.empty()
          ? nullptr
          : inputFor(bypassInput, bypassCall->operands.front());
  if (bypassCall != nullptr && bypassed != nullptr &&
      bypassed->operands.size() == 1) {
    bypassCall->operands.front() = bypassed->operands.front();
    (void)lang::rebuildMirValueUses(bypassInput);
  }
  expect(bypassed != nullptr &&
             hasMessage(bypassInput,
                        "call input must have exactly one executable use"),
         "the verifier should reject an invocation that bypasses its prepared "
         "argument");

  lang::MirBody wrongType = baseline;
  lang::MirInstruction *wrongTypeCall = callTo(wrongType, member->id);
  if (wrongTypeCall != nullptr && !wrongTypeCall->parameterTypes.empty()) {
    wrongTypeCall->parameterTypes.front() = lang::SemanticType::Bool;
  }
  expect(
      wrongTypeCall != nullptr &&
          hasMessage(wrongType,
                     "ordered invocation argument is not prepared by its exact "
                     "indexed input"),
      "the verifier should reject a prepared argument whose exact selected "
      "parameter type changes");

  lang::MirBody reordered = baseline;
  lang::MirInstruction *reorderedCall = callTo(reordered, member->id);
  lang::MirInstruction *receiverInput =
      reorderedCall == nullptr || !reorderedCall->receiver
          ? nullptr
          : inputFor(reordered, *reorderedCall->receiver);
  lang::MirInstruction *lastArgument =
      reorderedCall == nullptr || reorderedCall->operands.size() != 3
          ? nullptr
          : inputFor(reordered, reorderedCall->operands.back());
  bool swapped = false;
  if (receiverInput != nullptr && lastArgument != nullptr) {
    for (lang::MirBlock &block : reordered.blocks) {
      const auto receiver =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == receiverInput->id;
                       });
      const auto argument =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == lastArgument->id;
                       });
      if (receiver != block.instructions.end() &&
          argument != block.instructions.end()) {
        std::iter_swap(receiver, argument);
        swapped = true;
        break;
      }
    }
  }
  expect(
      swapped &&
          hasMessage(reordered,
                     "ordered invocation inputs must form a strict receiver, "
                     "argument, invocation chain"),
      "the verifier should reject a receiver/argument schedule reordered "
      "before invocation");
}

void testOrderedClassValueCallInputVerification() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "ordered-class-call-verification.gti", R"(
class Copyable {
public:
  int value;
  Copyable(int input) : value(input) {}
};

class Movable {
public:
  int value;
  Movable(int input) : value(input) {}
  Movable(Movable& other) = delete;
  Movable(Movable&& other) = default;
  ~Movable() {}
};

int accept(Copyable copied, Copyable other, Movable moved) {
  return copied.value + other.value + moved.value;
}

int main() {
  Copyable copied = Copyable(1);
  Copyable other = Copyable(2);
  Movable moved = Movable(3);
  return accept(copied, other, std::move(moved)) - 6;
}
)",
      {std::filesystem::path(__FILE__).parent_path().parent_path() /
       "stdlib/prelude.gti"});
  const lang::HirFunctionInstance *accept = findHirFunction(frontend, "accept");
  const lang::HirFunctionInstance *main = findHirFunction(frontend, "main");
  const lang::MirFunctionInstance *mirMain =
      main == nullptr ? nullptr : frontend.mir.findFunctionInstance(main->id);
  expect(frontend.canGenerateCode() && accept != nullptr && main != nullptr &&
             mirMain != nullptr && lang::verifyMirProgram(frontend.mir).valid(),
         "the class-value call-input verifier fixture should produce valid "
         "MIR");
  if (!frontend.canGenerateCode() || accept == nullptr || main == nullptr ||
      mirMain == nullptr) {
    return;
  }

  const auto callTo = [](lang::MirBody &body,
                         lang::HirFunctionInstanceId target) {
    for (lang::MirBlock &block : body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == target;
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto inputFor = [](lang::MirBody &body,
                           const lang::MirOperand &operand) {
    if (operand.kind != lang::MirOperandKind::Value) {
      return static_cast<lang::MirInstruction *>(nullptr);
    }
    const lang::MirValue *value = body.findValue(operand.value);
    lang::MirBlock *block = nullptr;
    if (value != nullptr) {
      const auto foundBlock =
          std::find_if(body.blocks.begin(), body.blocks.end(),
                       [&](const lang::MirBlock &candidate) {
                         return candidate.id == value->definitionBlock;
                       });
      block = foundBlock == body.blocks.end() ? nullptr : &*foundBlock;
    }
    if (block == nullptr) {
      return static_cast<lang::MirInstruction *>(nullptr);
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.id == value->definition;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };
  const auto hasMessage = [](const lang::MirBody &body, std::string_view text) {
    const lang::MirVerificationResult result = lang::verifyMirBody(body);
    return std::any_of(result.errors.begin(), result.errors.end(),
                       [&](const lang::MirVerificationError &error) {
                         return error.message.find(text) != std::string::npos;
                       });
  };

  lang::MirBody baseline = mirMain->body;
  lang::MirInstruction *baselineCall = callTo(baseline, accept->id);
  lang::MirInstruction *baselineCopy =
      baselineCall == nullptr || baselineCall->operands.size() != 3
          ? nullptr
          : inputFor(baseline, baselineCall->operands[0]);
  lang::MirInstruction *baselineOtherCopy =
      baselineCall == nullptr || baselineCall->operands.size() != 3
          ? nullptr
          : inputFor(baseline, baselineCall->operands[1]);
  lang::MirInstruction *baselineMove =
      baselineCall == nullptr || baselineCall->operands.size() != 3
          ? nullptr
          : inputFor(baseline, baselineCall->operands[2]);
  expect(baselineCall != nullptr && baselineCopy != nullptr &&
             baselineOtherCopy != nullptr && baselineMove != nullptr &&
             baselineCopy->callInputKind == lang::HirCallInputKind::CopyValue &&
             baselineOtherCopy->callInputKind ==
                 lang::HirCallInputKind::CopyValue &&
             baselineMove->callInputKind == lang::HirCallInputKind::MoveValue &&
             baselineMove->lifecycle.size() == 1 &&
             lang::verifyMirBody(baseline).valid(),
         "the baseline should expose exact class copy and move checkpoints");
  if (baselineCall == nullptr || baselineCopy == nullptr ||
      baselineOtherCopy == nullptr || baselineMove == nullptr ||
      baselineMove->lifecycle.empty()) {
    return;
  }

  lang::MirBody substitutedCopyPlace = baseline;
  lang::MirInstruction *substitutedCall =
      callTo(substitutedCopyPlace, accept->id);
  lang::MirInstruction *substitutedCopy =
      substitutedCall == nullptr
          ? nullptr
          : inputFor(substitutedCopyPlace, substitutedCall->operands[0]);
  lang::MirInstruction *substituteSource =
      substitutedCall == nullptr
          ? nullptr
          : inputFor(substitutedCopyPlace, substitutedCall->operands[1]);
  if (substitutedCopy != nullptr && substituteSource != nullptr) {
    substitutedCopy->operands.front().place =
        substituteSource->operands.front().place;
  }
  expect(substitutedCopy != nullptr && substituteSource != nullptr &&
             hasMessage(substitutedCopyPlace, "invalid shape or reference"),
         "the verifier should reject substituting another same-typed place "
         "for the exact class-copy source");

  lang::MirBody forgedCopyMode = baseline;
  lang::MirInstruction *forgedCopyCall = callTo(forgedCopyMode, accept->id);
  lang::MirInstruction *forgedCopy =
      forgedCopyCall == nullptr
          ? nullptr
          : inputFor(forgedCopyMode, forgedCopyCall->operands[0]);
  if (forgedCopy != nullptr) {
    forgedCopy->callInputKind = lang::HirCallInputKind::MoveValue;
  }
  expect(forgedCopy != nullptr &&
             hasMessage(forgedCopyMode, "invalid shape or reference"),
         "the verifier should reject a class place relabeled as a move-value "
         "checkpoint");

  lang::MirBody forgedMoveMode = baseline;
  lang::MirInstruction *forgedMoveCall = callTo(forgedMoveMode, accept->id);
  lang::MirInstruction *forgedMove =
      forgedMoveCall == nullptr
          ? nullptr
          : inputFor(forgedMoveMode, forgedMoveCall->operands[2]);
  if (forgedMove != nullptr) {
    forgedMove->callInputKind = lang::HirCallInputKind::CopyValue;
  }
  expect(forgedMove != nullptr &&
             hasMessage(forgedMoveMode, "invalid shape or reference"),
         "the verifier should reject an owned value relabeled as a class "
         "copy checkpoint");

  lang::MirBody missingTransfer = baseline;
  lang::MirInstruction *missingTransferCall =
      callTo(missingTransfer, accept->id);
  lang::MirInstruction *missingTransferInput =
      missingTransferCall == nullptr
          ? nullptr
          : inputFor(missingTransfer, missingTransferCall->operands[2]);
  if (missingTransferInput != nullptr) {
    missingTransferInput->lifecycle.clear();
  }
  expect(missingTransferInput != nullptr &&
             hasMessage(missingTransfer, "invalid shape or reference"),
         "the verifier should reject a move-value checkpoint that leaves its "
         "exact temporary obligation active");

  lang::MirBody transferOnCopy = baseline;
  lang::MirInstruction *transferOnCopyCall = callTo(transferOnCopy, accept->id);
  lang::MirInstruction *transferOnCopyInput =
      transferOnCopyCall == nullptr
          ? nullptr
          : inputFor(transferOnCopy, transferOnCopyCall->operands[0]);
  if (transferOnCopyInput != nullptr) {
    transferOnCopyInput->lifecycle = baselineMove->lifecycle;
  }
  expect(transferOnCopyInput != nullptr &&
             hasMessage(transferOnCopy, "invalid shape or reference"),
         "the verifier should reject ownership transfer forged onto a class "
         "copy checkpoint");

  lang::MirBody legacyCallTransfer = baseline;
  lang::MirInstruction *legacyCall = callTo(legacyCallTransfer, accept->id);
  lang::MirInstruction *legacyMove =
      legacyCall == nullptr
          ? nullptr
          : inputFor(legacyCallTransfer, legacyCall->operands[2]);
  if (legacyCall != nullptr && legacyMove != nullptr) {
    legacyCall->lifecycle = legacyMove->lifecycle;
    legacyMove->lifecycle.clear();
  }
  expect(legacyMove != nullptr &&
             hasMessage(legacyCallTransfer, "invalid shape or reference"),
         "the verifier should reject moving class-parameter transfer back to "
         "the undifferentiated call instruction");
}

void testMirLiteralIdentityFoldAndEditor() {
  const std::string source = R"(
class FoldContainer {
  int32_t value;

public:
  FoldContainer(int32_t input) : value(input) {}
  ~FoldContainer() {}
  int32_t read() { return this.value; }
};

int32_t grouped_integer() { return (((42))); }
float grouped_float() { return ((1.5)); }
char grouped_character() { return (('x')); }
bool grouped_boolean() { return ((true)); }
int32_t* grouped_null() { return ((nullptr)); }
int32_t grouped_string() { auto value = (("shadow")); return 0; }
int32_t grouped_dynamic(int32_t value) { return ((value)); }
int32_t grouped_expression() { return ((20 + 22)); }
int main() {
  FoldContainer value = FoldContainer(grouped_integer());
  auto zero = []() -> int32_t { return 0; };
  return value.read() - 42 + zero();
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("mir-literal-identity-fold.gti", source);
  expect(frontend.canGenerateCode(),
         "the literal-identity fixture should produce valid frontend IR");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected literal-identity diagnostic: "
                << diagnostic.message << '\n';
    }
    return;
  }

  const std::string original = lang::MirPrinter().print(frontend.mir);
  lang::MirProgram enumerationProgram = frontend.mir;
  const std::vector<lang::MirBodyAddress> bodyAddresses =
      lang::MirProgramEditor(enumerationProgram).bodies();
  const auto hasBodyKind = [&](lang::MirBodyKind kind) {
    return std::any_of(
        bodyAddresses.begin(), bodyAddresses.end(),
        [kind](lang::MirBodyAddress address) { return address.kind == kind; });
  };
  expect(hasBodyKind(lang::MirBodyKind::Module) &&
             hasBodyKind(lang::MirBodyKind::FieldInitializers) &&
             hasBodyKind(lang::MirBodyKind::StaticFieldInitializers) &&
             hasBodyKind(lang::MirBodyKind::Function) &&
             hasBodyKind(lang::MirBodyKind::Constructor) &&
             hasBodyKind(lang::MirBodyKind::Destructor) &&
             hasBodyKind(lang::MirBodyKind::Lambda),
         "the editor should enumerate every MIR body family in deterministic "
         "program order");
  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility = pipeline.run(
      frontend.hir, lang::OptimizationLevel::O1, lang::TargetInfo::host());

  const lang::OptimizedProgram o0 = pipeline.run(
      lang::OptimizationRequest{.hir = frontend.hir,
                                .mir = frontend.mir,
                                .level = lang::OptimizationLevel::O0,
                                .compatibility = &compatibility});
  expect(o0.valid() && o0.report.passes.empty() &&
             lang::MirPrinter().print(o0.mir) == original,
         "O0 should preserve the owned MIR snapshot byte-for-byte and schedule "
         "no transform");

  const lang::OptimizedProgram o1 = pipeline.run(
      lang::OptimizationRequest{.hir = frontend.hir,
                                .mir = frontend.mir,
                                .level = lang::OptimizationLevel::O1,
                                .compatibility = &compatibility});
  expect(o1.valid() && o1.report.passes.size() == 1,
         "O1 should run one verified shadow transform");
  if (!o1.valid() || o1.report.passes.size() != 1) {
    return;
  }
  const lang::OptimizationPassReport &report = o1.report.passes.front();
  expect(
      report.name == "fold-literal-identities" && report.changed &&
          report.appliedEdits != 0 &&
          report.shadowComparisons == report.appliedEdits &&
          report.shadowMismatches == 0 && report.valueUsesRebuilt &&
          report.invalidation.instructionFacts &&
          report.invalidation.valueUses && !report.invalidation.controlFlow &&
          !report.invalidation.reachability && !report.invalidation.dominance,
      "the literal-identity report should expose exact edits, shadow "
      "agreement, repair, and bounded invalidation");
  expect(lang::verifyMirProgram(o1.mir).valid() &&
             lang::MirPrinter().print(o1.mir) != original &&
             lang::MirPrinter().print(frontend.mir) == original,
         "the shadow pass should produce valid changed MIR without mutating "
         "the frontend-owned input");

  const lang::FrontendResult independent =
      lang::Frontend().analyze("mir-literal-identity-fold.gti", source);
  const lang::OptimizationResult independentCompatibility = pipeline.run(
      independent.hir, lang::OptimizationLevel::O1, lang::TargetInfo::host());
  const lang::OptimizedProgram independentO1 = pipeline.run(
      lang::OptimizationRequest{.hir = independent.hir,
                                .mir = independent.mir,
                                .level = lang::OptimizationLevel::O1,
                                .compatibility = &independentCompatibility});
  const lang::OptimizationPassReport *independentReport =
      independentO1.report.passes.size() == 1
          ? &independentO1.report.passes.front()
          : nullptr;
  expect(independent.canGenerateCode() && independentO1.valid() &&
             independentReport != nullptr &&
             lang::MirPrinter().print(independentO1.mir) ==
                 lang::MirPrinter().print(o1.mir) &&
             independentReport->name == report.name &&
             independentReport->changed == report.changed &&
             independentReport->appliedEdits == report.appliedEdits &&
             independentReport->shadowComparisons == report.shadowComparisons &&
             independentReport->shadowMismatches == report.shadowMismatches,
         "independent analyses should produce byte-identical shadow MIR and "
         "deterministic pass reports");

  struct PatchCandidate {
    lang::MirInstructionAddress address;
    lang::MirInstructionId instruction = 0;
    lang::Literal literal;
  };
  std::vector<PatchCandidate> candidates;
  std::size_t compatibilityMatches = 0;
  const std::vector<std::string> scalarFunctions{
      "grouped_integer", "grouped_float", "grouped_character",
      "grouped_boolean", "grouped_null"};
  for (const std::string &name : scalarFunctions) {
    const lang::HirFunctionInstance *hir = findHirFunction(frontend, name);
    const lang::MirFunctionInstance *before =
        hir == nullptr ? nullptr : frontend.mir.findFunctionInstance(hir->id);
    const lang::MirFunctionInstance *after =
        hir == nullptr ? nullptr : o1.mir.findFunctionInstance(hir->id);
    expect(before != nullptr && after != nullptr,
           "each scalar grouping function should have HIR and MIR bodies");
    if (before == nullptr || after == nullptr) {
      continue;
    }
    for (std::size_t blockIndex = 0; blockIndex < before->body.blocks.size();
         ++blockIndex) {
      const lang::MirBlock &beforeBlock = before->body.blocks[blockIndex];
      const lang::MirBlock &afterBlock = after->body.blocks[blockIndex];
      for (std::size_t instructionIndex = 0;
           instructionIndex < beforeBlock.instructions.size();
           ++instructionIndex) {
        const lang::MirInstruction &oldInstruction =
            beforeBlock.instructions[instructionIndex];
        const lang::MirInstruction &newInstruction =
            afterBlock.instructions[instructionIndex];
        if (oldInstruction.operation != lang::MirOperation::Identity ||
            newInstruction.operation != lang::MirOperation::Literal ||
            !newInstruction.literal) {
          continue;
        }
        candidates.push_back(
            {.address = {.body = {.kind = lang::MirBodyKind::Function,
                                  .owner = hir->id},
                         .block = beforeBlock.id,
                         .index = instructionIndex},
             .instruction = oldInstruction.id,
             .literal = *newInstruction.literal});
        const lang::ConstantEvaluation evaluated =
            lang::evaluateConstantLiteral(
                *newInstruction.literal,
                lang::constantIntegerDomain(newInstruction.info.type));
        const lang::ConstantValue *expected =
            compatibility.replacement(oldInstruction.hirValue);
        if (evaluated.value && expected != nullptr &&
            *evaluated.value == *expected &&
            oldInstruction.id == newInstruction.id &&
            oldInstruction.result == newInstruction.result &&
            oldInstruction.hirValue == newInstruction.hirValue &&
            oldInstruction.hirStatement == newInstruction.hirStatement &&
            oldInstruction.info.type == newInstruction.info.type) {
          ++compatibilityMatches;
        }
      }
    }
  }
  expect(candidates.size() == report.appliedEdits &&
             compatibilityMatches == candidates.size(),
         "every rewritten scalar grouping should preserve identity and match "
         "the compatibility HIR constant exactly");

  const auto operationCount = [](const lang::MirBody *body,
                                 lang::MirOperation operation) {
    std::size_t count = 0;
    if (body != nullptr) {
      for (const lang::MirBlock &block : body->blocks) {
        count += static_cast<std::size_t>(
            std::count_if(block.instructions.begin(), block.instructions.end(),
                          [operation](const lang::MirInstruction &instruction) {
                            return instruction.operation == operation;
                          }));
      }
    }
    return count;
  };
  for (const std::string &name :
       {"grouped_string", "grouped_dynamic", "grouped_expression"}) {
    const lang::HirFunctionInstance *hir = findHirFunction(frontend, name);
    const lang::MirFunctionInstance *before =
        hir == nullptr ? nullptr : frontend.mir.findFunctionInstance(hir->id);
    const lang::MirFunctionInstance *after =
        hir == nullptr ? nullptr : o1.mir.findFunctionInstance(hir->id);
    expect(before != nullptr && after != nullptr &&
               operationCount(&before->body, lang::MirOperation::Identity) ==
                   operationCount(&after->body, lang::MirOperation::Identity),
           "string, dynamic, and computed groupings should remain conservative "
           "near-misses");
  }

  const lang::OptimizedProgram repeated = pipeline.run(
      lang::OptimizationRequest{.hir = frontend.hir,
                                .mir = o1.mir,
                                .level = lang::OptimizationLevel::O1,
                                .compatibility = &compatibility});
  expect(repeated.valid() && repeated.report.passes.size() == 1 &&
             !repeated.report.passes.front().changed &&
             repeated.report.passes.front().appliedEdits == 0 &&
             lang::MirPrinter().print(repeated.mir) ==
                 lang::MirPrinter().print(o1.mir),
         "literal identity folding should be deterministic and idempotent");

  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O2, lang::OptimizationLevel::O3}) {
    const lang::OptimizedProgram optimized = pipeline.run(
        lang::OptimizationRequest{.hir = frontend.hir,
                                  .mir = frontend.mir,
                                  .level = level,
                                  .compatibility = &compatibility});
    expect(optimized.valid() && lang::MirPrinter().print(optimized.mir) ==
                                    lang::MirPrinter().print(o1.mir),
           "O1, O2, and O3 should schedule the same first bounded shadow "
           "transform");
  }

  const lang::OptimizedProgram unchecked =
      pipeline.run(lang::OptimizationRequest{
          .hir = frontend.hir,
          .mir = frontend.mir,
          .level = lang::OptimizationLevel::O1,
          .options = lang::OptimizationOptions{.verifyMir = false},
          .compatibility = &compatibility});
  expect(unchecked.valid() && !unchecked.report.verificationEnabled &&
             unchecked.report.passes.size() == 1 &&
             unchecked.report.passes.front().changed,
         "disabling pipeline verification should not disable the shadow "
         "transform or its atomic editor safety boundary");

  expect(candidates.size() >= 2,
         "the editor fixture should expose multiple independent patches");
  if (candidates.size() < 2) {
    return;
  }

  const auto queue = [](lang::MirProgramEditor &editor,
                        const PatchCandidate &candidate) {
    editor.queueLiteralReplacement(candidate.address, candidate.instruction,
                                   lang::MirOperation::Identity,
                                   candidate.literal);
  };
  {
    lang::MirProgram invalid = frontend.mir;
    const std::string before = lang::MirPrinter().print(invalid);
    lang::MirProgramEditor editor(invalid);
    queue(editor, candidates.front());
    lang::MirInstructionAddress outside = candidates.back().address;
    outside.index = std::numeric_limits<std::size_t>::max();
    editor.queueLiteralReplacement(outside, candidates.back().instruction,
                                   lang::MirOperation::Identity,
                                   candidates.back().literal);
    const lang::MirEditResult edited = editor.apply();
    expect(!edited.valid() && !edited.changed &&
               lang::MirPrinter().print(invalid) == before,
           "a mixed valid/out-of-range patch batch should fail atomically");
  }
  {
    lang::MirProgram invalid = frontend.mir;
    const std::string before = lang::MirPrinter().print(invalid);
    lang::MirProgramEditor editor(invalid);
    queue(editor, candidates.front());
    queue(editor, candidates.front());
    const lang::MirEditResult edited = editor.apply();
    expect(!edited.valid() && !edited.changed &&
               lang::MirPrinter().print(invalid) == before,
           "duplicate patches should be rejected without partial mutation");
  }
  {
    lang::MirProgram invalid = frontend.mir;
    const std::string before = lang::MirPrinter().print(invalid);
    lang::MirProgramEditor editor(invalid);
    editor.queueLiteralReplacement(
        candidates.front().address, candidates.front().instruction + 1,
        lang::MirOperation::Identity, candidates.front().literal);
    const lang::MirEditResult edited = editor.apply();
    expect(!edited.valid() && !edited.changed &&
               lang::MirPrinter().print(invalid) == before,
           "a stale instruction guard should reject the complete patch set");
  }
  {
    lang::MirProgram invalid = frontend.mir;
    const std::string before = lang::MirPrinter().print(invalid);
    lang::MirProgramEditor editor(invalid);
    editor.queueLiteralReplacement(
        candidates.front().address, candidates.front().instruction,
        lang::MirOperation::Identity, lang::Literal{true});
    const lang::MirEditResult edited = editor.apply();
    expect(!edited.valid() && !edited.changed &&
               !edited.verification.errors.empty() &&
               lang::MirPrinter().print(invalid) == before,
           "fresh verification should reject a literal whose alternative does "
           "not match its result type");
  }
  {
    lang::MirProgram invalid = frontend.mir;
    const std::string before = lang::MirPrinter().print(invalid);
    lang::MirProgramEditor editor(invalid);
    editor.queueLiteralReplacement(
        candidates.front().address, candidates.front().instruction,
        lang::MirOperation::Identity,
        lang::Literal{std::numeric_limits<std::uint64_t>::max()});
    const lang::MirEditResult edited = editor.apply();
    expect(!edited.valid() && !edited.changed &&
               !edited.verification.errors.empty() &&
               lang::MirPrinter().print(invalid) == before,
           "the editor should reject an integer replacement outside its exact "
           "result domain without committing any patch");
  }

  lang::MirProgram forward = frontend.mir;
  lang::MirProgram reverse = frontend.mir;
  lang::MirProgramEditor forwardEditor(forward);
  lang::MirProgramEditor reverseEditor(reverse);
  for (const PatchCandidate &candidate : candidates) {
    queue(forwardEditor, candidate);
  }
  for (auto candidate = candidates.rbegin(); candidate != candidates.rend();
       ++candidate) {
    queue(reverseEditor, *candidate);
  }
  expect(lang::MirPrinter().print(forward) == original &&
             forwardEditor.pendingPatchCount() == candidates.size(),
         "queued patches should not mutate MIR before the apply boundary");
  const lang::MirEditResult forwardEdit = forwardEditor.apply();
  const lang::MirEditResult reverseEdit = reverseEditor.apply();
  expect(forwardEdit.valid() && reverseEdit.valid() && forwardEdit.changed &&
             reverseEdit.changed &&
             lang::MirPrinter().print(forward) ==
                 lang::MirPrinter().print(reverse) &&
             lang::MirPrinter().print(forward) ==
                 lang::MirPrinter().print(o1.mir),
         "patch application should be deterministic regardless of queue "
         "order");

  const lang::HirFunctionInstance *integerHir =
      findHirFunction(frontend, "grouped_integer");
  const lang::MirFunctionInstance *integerBefore =
      integerHir == nullptr ? nullptr
                            : frontend.mir.findFunctionInstance(integerHir->id);
  const lang::MirFunctionInstance *integerAfter =
      integerHir == nullptr ? nullptr
                            : forward.findFunctionInstance(integerHir->id);
  const auto useCount = [](const lang::MirBody &body) {
    std::size_t count = 0;
    for (const auto &uses : body.valueUses) {
      count += uses.size();
    }
    return count;
  };
  expect(integerBefore != nullptr && integerAfter != nullptr &&
             useCount(integerAfter->body) < useCount(integerBefore->body) &&
             lang::computeMirDominance(integerBefore->body) ==
                 lang::computeMirDominance(integerAfter->body),
         "in-place literal replacement should rebuild removed uses while "
         "preserving CFG dominance");

  lang::MirProgram wrongIdentity = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      wrongIdentity.functionInstances());
  if (integerHir != nullptr) {
    lang::MirBody &body = functions[integerHir->id - 1].body;
    for (lang::MirBlock &block : body.blocks) {
      const auto identity = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.operation == lang::MirOperation::Identity &&
                   !instruction.operands.empty();
          });
      if (identity != block.instructions.end()) {
        identity->operands.front().type = lang::SemanticType::Bool;
        break;
      }
    }
  }
  expect(!lang::verifyMirProgram(wrongIdentity).valid(),
         "MIR verification should reject an identity whose operand and result "
         "types differ");
}

void testMirDominanceAndValueAvailability() {
  const lang::ExpressionInfo intInfo{.type = lang::SemanticType::Int32};
  const auto literalInstruction = [&](lang::MirInstructionId instruction,
                                      lang::MirValueId result) {
    return lang::MirInstruction{.id = instruction,
                                .kind = lang::MirInstructionKind::Compute,
                                .result = result,
                                .operation = lang::MirOperation::Literal,
                                .literal = lang::Literal{std::uint64_t{1}},
                                .info = intInfo};
  };
  const auto identityInstruction = [&](lang::MirInstructionId instruction,
                                       lang::MirValueId result,
                                       lang::MirValueId operand) {
    return lang::MirInstruction{
        .id = instruction,
        .kind = lang::MirInstructionKind::Compute,
        .result = result,
        .operands = {{.kind = lang::MirOperandKind::Value,
                      .value = operand,
                      .type = lang::SemanticType::Int32}},
        .operation = lang::MirOperation::Identity,
        .info = intInfo};
  };
  const auto finish = [](lang::MirBody body) {
    lang::rebuildMirReachability(body);
    (void)lang::rebuildMirValueUses(body);
    return body;
  };

  lang::MirBody branchLocalDefinition = finish(lang::MirBody{
      .kind = lang::MirBodyKind::Function,
      .entry = 1,
      .returnType = lang::SemanticType::Int32,
      .blocks =
          {{.id = 1,
            .terminator = {.kind = lang::MirTerminatorKind::Branch,
                           .value =
                               lang::MirOperand{
                                   .kind = lang::MirOperandKind::Constant,
                                   .literal = lang::Literal{true},
                                   .type = lang::SemanticType::Bool},
                           .target = 2,
                           .elseTarget = 3}},
           {.id = 2,
            .instructions = {literalInstruction(1, 1)},
            .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4}},
           {.id = 3,
            .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4}},
           {.id = 4,
            .instructions = {identityInstruction(2, 2, 1)},
            .terminator =
                {.kind = lang::MirTerminatorKind::Return,
                 .value = lang::MirOperand{.kind = lang::MirOperandKind::Value,
                                           .value = 2,
                                           .type = lang::SemanticType::Int32}}},
           {.id = 5,
            .terminator = {.kind = lang::MirTerminatorKind::Return,
                           .value =
                               lang::MirOperand{
                                   .kind = lang::MirOperandKind::Constant,
                                   .literal = lang::Literal{std::uint64_t{0}},
                                   .type = lang::SemanticType::Int32}}}},
      .values = {{.id = 1,
                  .sourceValue = 1,
                  .info = intInfo,
                  .definitionBlock = 2,
                  .definition = 1},
                 {.id = 2,
                  .sourceValue = 2,
                  .info = intInfo,
                  .definitionBlock = 4,
                  .definition = 2}}});

  const std::optional<lang::MirDominanceInfo> first =
      lang::computeMirDominance(branchLocalDefinition);
  const std::optional<lang::MirDominanceInfo> repeated =
      lang::computeMirDominance(branchLocalDefinition);
  expect(first && repeated && *first == *repeated && first->blockCount() == 5 &&
             first->isReachable(4) && first->immediateDominator(4) == 1 &&
             first->dominates(1, 4) && !first->dominates(2, 4) &&
             !first->dominates(0, 4) && !first->isReachable(5) &&
             first->immediateDominator(5) == 0 && !first->dominates(5, 5),
         "dominance queries should return deterministic GTI block identities "
         "and explicit unreachable state for a diamond CFG");

  const lang::MirVerificationResult nonDominating =
      lang::verifyMirBody(branchLocalDefinition);
  expect(!nonDominating.valid() && !nonDominating.errors.empty() &&
             nonDominating.errors.front().message.find("not dominated") !=
                 std::string::npos,
         "the verifier should reject a branch-local value used at a merge");

  lang::MirBody entryDefinition = branchLocalDefinition;
  entryDefinition.blocks[0].instructions = {literalInstruction(1, 1)};
  entryDefinition.blocks[1].instructions.clear();
  entryDefinition.values[0].definitionBlock = 1;
  lang::rebuildMirReachability(entryDefinition);
  expect(lang::rebuildMirValueUses(entryDefinition) &&
             lang::verifyMirBody(entryDefinition).valid(),
         "a value defined at entry should remain available on both sides of "
         "a diamond and at its merge");

  lang::MirBody placeUse = branchLocalDefinition;
  placeUse.places = {
      {.id = 1,
       .root = lang::MirPlaceRootKind::Value,
       .value = 1,
       .type = lang::SemanticType::Int32,
       .traits = lang::semanticTraits(lang::SemanticType::Int32)}};
  placeUse.blocks[3].instructions = {
      {.id = 2,
       .kind = lang::MirInstructionKind::Load,
       .result = 2,
       .operands = {{.kind = lang::MirOperandKind::Copy,
                     .place = 1,
                     .type = lang::SemanticType::Int32}},
       .info = intInfo}};
  const bool rebuiltPlaceUses = lang::rebuildMirValueUses(placeUse);
  const lang::MirVerificationResult invalidPlaceUse =
      lang::verifyMirBody(placeUse);
  expect(rebuiltPlaceUses && !invalidPlaceUse.valid() &&
             !invalidPlaceUse.errors.empty() &&
             invalidPlaceUse.errors.front().message.find("not dominated") !=
                 std::string::npos,
         "dominance validation should follow value and index dependencies "
         "carried by a place used in another block");

  lang::MirBody useBeforeDefinition = finish(lang::MirBody{
      .kind = lang::MirBodyKind::Function,
      .entry = 1,
      .returnType = lang::SemanticType::Int32,
      .blocks = {{.id = 1,
                  .instructions = {identityInstruction(2, 2, 1),
                                   literalInstruction(1, 1)},
                  .terminator = {.kind = lang::MirTerminatorKind::Return,
                                 .value =
                                     lang::MirOperand{
                                         .kind = lang::MirOperandKind::Value,
                                         .value = 2,
                                         .type = lang::SemanticType::Int32}}}},
      .values = {{.id = 1,
                  .sourceValue = 1,
                  .info = intInfo,
                  .definitionBlock = 1,
                  .definition = 1},
                 {.id = 2,
                  .sourceValue = 2,
                  .info = intInfo,
                  .definitionBlock = 1,
                  .definition = 2}}});
  const lang::MirVerificationResult earlyUse =
      lang::verifyMirBody(useBeforeDefinition);
  expect(!earlyUse.valid() && !earlyUse.errors.empty() &&
             earlyUse.errors.front().message.find("before its definition") !=
                 std::string::npos,
         "the verifier should reject a same-block use before its defining "
         "instruction");

  lang::MirBody malformedCfg = branchLocalDefinition;
  malformedCfg.blocks.front().terminator.target =
      malformedCfg.blocks.size() + 1;
  expect(!lang::computeMirDominance(malformedCfg),
         "dominance analysis should reject an invalid CFG without invoking "
         "LLVM on malformed edges");
}

void testMirEffectClassification() {
  for (std::size_t index = 0; index < lang::mirInstructionKindCount; ++index) {
    const auto kind = static_cast<lang::MirInstructionKind>(index);
    expect(lang::name(kind) != "invalid",
           "every MIR instruction kind should have a stable classification");
    (void)lang::effects(kind);
  }
  for (std::size_t index = 0; index < lang::mirOperationCount; ++index) {
    const auto operation = static_cast<lang::MirOperation>(index);
    expect(lang::name(operation) != "invalid",
           "every MIR operation should have a stable classification");
    (void)lang::effects(operation);
  }
  for (std::size_t index = 0; index < lang::intrinsicKindCount; ++index) {
    const auto intrinsic = static_cast<lang::IntrinsicKind>(index);
    expect(lang::name(intrinsic) != "invalid",
           "every intrinsic should have a stable classification");
    (void)lang::effects(intrinsic);
  }
  for (std::size_t index = 0; index < lang::synchronizationOperationKindCount;
       ++index) {
    const auto kind = static_cast<lang::SynchronizationOperationKind>(index);
    expect(lang::name(kind) != "invalid",
           "every synchronization operation should have a stable "
           "classification");
    const lang::MirEffectTraits effect = lang::effects(kind);
    if (kind != lang::SynchronizationOperationKind::None) {
      expect(effect.maySynchronize && !effect.speculatable &&
                 !effect.removableWhenUnused && !effect.reorderable,
             "every synchronization operation should be a retained optimizer "
             "barrier");
    }
  }
  for (std::size_t index = 0; index < lang::atomicMemoryOrderCount; ++index) {
    const auto order = static_cast<lang::AtomicMemoryOrder>(index);
    expect(lang::name(order) != "invalid",
           "every atomic memory order should have a stable spelling");
  }

  const lang::MirEffectTraits literal = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Compute,
                           .operation = lang::MirOperation::Literal});
  expect(literal.speculatable && literal.removableWhenUnused &&
             literal.reorderable && !literal.mayTrap && !literal.maySynchronize,
         "literal computation should be classified as harmless");

  const lang::MirEffectTraits division = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Compute,
                           .operation = lang::MirOperation::Divide});
  expect(
      division.mayTrap && !division.speculatable &&
          !division.removableWhenUnused,
      "division should remain non-removable until GTI edge semantics prove it");

  const lang::MirEffectTraits ordinaryCall = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Call});
  expect(
      ordinaryCall.readsUnknownMemory && ordinaryCall.writesUnknownMemory &&
          ordinaryCall.invokesUserCode && ordinaryCall.mayTrap &&
          ordinaryCall.maySynchronize && !ordinaryCall.removableWhenUnused,
      "ordinary calls should conservatively remain synchronization barriers");
  expect(lang::effects(lang::MirInstructionKind::Call).maySynchronize,
         "instruction-kind summaries should expose possible synchronization");

  const lang::MirEffectTraits callInput =
      lang::effects(lang::MirInstructionKind::CallInput);
  expect(!callInput.speculatable && !callInput.removableWhenUnused &&
             !callInput.reorderable && !callInput.invokesRuntime &&
             !callInput.invokesUserCode,
         "call-input checkpoints should be non-reorderable schedule barriers "
         "without claiming that input preparation invokes code itself");

  const lang::MirEffectTraits copyValueInput = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::CallInput,
                           .callInputKind = lang::HirCallInputKind::CopyValue});
  expect(copyValueInput.copiesValue && copyValueInput.initializesValue &&
             copyValueInput.invokesUserCode && copyValueInput.mayTrap &&
             copyValueInput.maySynchronize && !copyValueInput.speculatable &&
             !copyValueInput.removableWhenUnused && !copyValueInput.reorderable,
         "a class copy-value checkpoint should conservatively expose the "
         "selected copy construction effects");

  const lang::MirEffectTraits moveValueInput = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::CallInput,
                           .callInputKind = lang::HirCallInputKind::MoveValue});
  expect(moveValueInput.movesValue && moveValueInput.initializesValue &&
             moveValueInput.invokesUserCode && moveValueInput.mayTrap &&
             moveValueInput.maySynchronize && !moveValueInput.speculatable &&
             !moveValueInput.removableWhenUnused && !moveValueInput.reorderable,
         "a class move-value checkpoint should conservatively expose the "
         "selected move construction effects");

  const lang::MirEffectTraits lifecycle =
      lang::effects(lang::MirInstructionKind::Lifecycle);
  expect(lifecycle.movesValue && lifecycle.initializesValue &&
             lifecycle.dropsValue && !lifecycle.speculatable &&
             !lifecycle.removableWhenUnused && !lifecycle.reorderable,
         "lifecycle-only transitions should remain observable ordering "
         "barriers even when they produce no SSA result");

  const lang::MirEffectTraits eventBearing = lang::effects(lang::MirInstruction{
      .kind = lang::MirInstructionKind::Compute,
      .operation = lang::MirOperation::Literal,
      .lifecycle = {
          {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1}}});
  expect(eventBearing.movesValue && eventBearing.initializesValue &&
             eventBearing.dropsValue && !eventBearing.speculatable &&
             !eventBearing.removableWhenUnused && !eventBearing.reorderable,
         "any instruction carrying lifecycle transitions should remain an "
         "observable ordering barrier");

  const lang::MirEffectTraits replacement = lang::effects(lang::MirInstruction{
      .kind = lang::MirInstructionKind::Assign,
      .lifecycle = {{.kind = lang::MirLifecycleEventKind::Replace,
                     .source = 1,
                     .target = 2}}});
  expect(replacement.dropsValue && replacement.invokesUserCode &&
             replacement.mayTrap && replacement.maySynchronize &&
             !replacement.reorderable,
         "replacement lifecycle events should conservatively retain prior-"
         "value cleanup effects");

  const lang::MirEffectTraits allocation = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Call,
                           .intrinsic = lang::IntrinsicKind::AllocateStorage});
  expect(allocation.allocates && allocation.invokesRuntime &&
             allocation.writesUnknownMemory && allocation.mayTrap &&
             allocation.maySynchronize,
         "runtime intrinsics should expose allocation and possible "
         "synchronization effects");
  expect(lang::effects(lang::IntrinsicKind::AllocateStorage).maySynchronize,
         "intrinsic summaries should expose possible runtime synchronization");

  const lang::MirEffectTraits drop = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Drop});
  expect(drop.dropsValue && drop.invokesUserCode && drop.writesPlace &&
             drop.maySynchronize && !drop.reorderable,
         "user cleanup should remain observable and a synchronization barrier");

  const lang::MirEffectTraits spawn = lang::effects(lang::MirInstruction{
      .kind = lang::MirInstructionKind::Call,
      .synchronization = {
          .kind = lang::SynchronizationOperationKind::ThreadSpawn}});
  expect(spawn.allocates && spawn.invokesRuntime && spawn.invokesUserCode &&
             spawn.movesValue && spawn.maySynchronize && !spawn.speculatable &&
             !spawn.removableWhenUnused && !spawn.reorderable,
         "thread spawn should remain an observable task-transfer and "
         "synchronization barrier");

  const lang::MirEffectTraits atomicLoad = lang::effects(lang::MirInstruction{
      .kind = lang::MirInstructionKind::Call,
      .synchronization = {.kind =
                              lang::SynchronizationOperationKind::AtomicLoad,
                          .order = lang::AtomicMemoryOrder::Acquire}});
  expect(atomicLoad.readsUnknownMemory && !atomicLoad.writesUnknownMemory &&
             atomicLoad.targetDependent && atomicLoad.maySynchronize &&
             !atomicLoad.speculatable && !atomicLoad.removableWhenUnused &&
             !atomicLoad.reorderable,
         "an acquire atomic load should remain a non-reorderable read "
         "barrier");

  const lang::MirEffectTraits atomicStore = lang::effects(lang::MirInstruction{
      .kind = lang::MirInstructionKind::Call,
      .synchronization = {.kind =
                              lang::SynchronizationOperationKind::AtomicStore,
                          .order = lang::AtomicMemoryOrder::Release}});
  expect(!atomicStore.readsUnknownMemory && atomicStore.writesUnknownMemory &&
             atomicStore.targetDependent && atomicStore.maySynchronize &&
             !atomicStore.speculatable && !atomicStore.removableWhenUnused &&
             !atomicStore.reorderable,
         "a release atomic store should remain a non-reorderable write "
         "barrier");
}

void testDefinedFailureIdentityAndPropagation() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "defined-failure-metadata.gti", R"(
class Box {
public:
  Box(int value) {}
  int operator()() { return 1; }
};

interface Reader {
  int read();
};

class ReaderBox : public Reader {
public:
  int read() override { return 2; }
};

int relay(int value) { return value; }
int read_value(Reader& reader) { return reader.read(); }

int main() {
  mut int value = 8;
  int sum = value + 1;
  int quotient = value / 2;
  int remainder = value % 3;
  int shifted = value << 1;
  int8_t narrowed = int8_t(value);
  mut int values[2] = {1, 2};
  int indexed = values[1];
  values[1] += value;
  mut uint32_t unsigned_value = uint32_t(8);
  uint32_t unsigned_quotient = unsigned_value / uint32_t(2);
  uint32_t unsigned_shifted = unsigned_value << uint32_t(1);
  expected<int, int> result = 1;
  int unwrapped = result.value();
  Box box = Box(value);
  int invoked = box();
  ReaderBox reader = ReaderBox();
  Reader& reader_view = reader;
  int virtual_value = read_value(reader_view);
  auto callable = [](int input) -> int { return input; };
  int callable_value = callable(3);
  return relay(sum + quotient + remainder + shifted + int(narrowed) +
               indexed + unwrapped + invoked + int(unsigned_quotient) +
               int(unsigned_shifted) + virtual_value + callable_value);
}
)",
      {std::filesystem::path(__FILE__).parent_path().parent_path() /
       "stdlib/prelude.gti"});
  const lang::HirFunctionInstance *hirMain = findHirFunction(frontend, "main");
  const lang::MirFunctionInstance *mirMain =
      hirMain == nullptr ? nullptr
                         : frontend.mir.findFunctionInstance(hirMain->id);
  expect(frontend.canGenerateCode() && hirMain != nullptr &&
             mirMain != nullptr && lang::verifyMirProgram(frontend.mir).valid(),
         "defined failures should reach validated HIR and MIR");
  if (!frontend.canGenerateCode() || hirMain == nullptr || mirMain == nullptr) {
    return;
  }

  const auto hasOutcome = [](const lang::DefinedFailureOperation &operation,
                             lang::DefinedFailureCode code,
                             lang::DefinedFailureDetail detail) {
    return std::any_of(
        operation.localOrigins.begin(), operation.localOrigins.end(),
        [&](const lang::DefinedFailureOrigin &origin) {
          return std::find(origin.outcomes.begin(), origin.outcomes.end(),
                           lang::DefinedFailureOutcome{.code = code,
                                                       .detail = detail}) !=
                 origin.outcomes.end();
        });
  };
  const auto hirHasOutcome = [&](lang::DefinedFailureCode code,
                                 lang::DefinedFailureDetail detail) {
    return std::any_of(hirMain->body.values.begin(), hirMain->body.values.end(),
                       [&](const lang::HirValue &value) {
                         return hasOutcome(value.definedFailure, code, detail);
                       });
  };
  bool sawMultipleLocalOrigins = false;
  bool unsignedDivisionIsExact = false;
  bool unsignedShiftIsExact = false;
  for (const lang::HirValue &value : hirMain->body.values) {
    sawMultipleLocalOrigins =
        sawMultipleLocalOrigins || value.definedFailure.localOrigins.size() > 1;
    if (!value.operation) {
      continue;
    }
    if (*value.operation == lang::TokenKind::SLASH &&
        value.info.type == lang::SemanticType::UInt32) {
      unsignedDivisionIsExact =
          hasOutcome(value.definedFailure,
                     lang::DefinedFailureCode::DivisionByZero,
                     lang::DefinedFailureDetail::IntegerDivision) &&
          !hasOutcome(value.definedFailure,
                      lang::DefinedFailureCode::IntegerOverflow,
                      lang::DefinedFailureDetail::Division);
    }
    if (*value.operation == lang::TokenKind::SHIFT_LEFT &&
        value.info.type == lang::SemanticType::UInt32) {
      unsignedShiftIsExact =
          hasOutcome(value.definedFailure,
                     lang::DefinedFailureCode::ShiftCountOutOfRange,
                     lang::DefinedFailureDetail::LeftShift) &&
          !hasOutcome(value.definedFailure,
                      lang::DefinedFailureCode::NegativeShiftCount,
                      lang::DefinedFailureDetail::LeftShift);
    }
  }
  expect(
      hirHasOutcome(lang::DefinedFailureCode::IntegerOverflow,
                    lang::DefinedFailureDetail::Addition) &&
          hirHasOutcome(lang::DefinedFailureCode::DivisionByZero,
                        lang::DefinedFailureDetail::IntegerDivision) &&
          hirHasOutcome(lang::DefinedFailureCode::ModuloByZero,
                        lang::DefinedFailureDetail::IntegerModulo) &&
          hirHasOutcome(lang::DefinedFailureCode::NegativeShiftCount,
                        lang::DefinedFailureDetail::LeftShift) &&
          hirHasOutcome(lang::DefinedFailureCode::ShiftCountOutOfRange,
                        lang::DefinedFailureDetail::LeftShift) &&
          hirHasOutcome(lang::DefinedFailureCode::NumericConversionOutOfRange,
                        lang::DefinedFailureDetail::NumericCast) &&
          hirHasOutcome(lang::DefinedFailureCode::IndexOutOfBounds,
                        lang::DefinedFailureDetail::FixedArray) &&
          hirHasOutcome(lang::DefinedFailureCode::InvalidExpectedAccess,
                        lang::DefinedFailureDetail::ValueOnError) &&
          sawMultipleLocalOrigins && unsignedDivisionIsExact &&
          unsignedShiftIsExact,
      "HIR should preserve the exact compiler-owned failure vocabulary");

  bool sawIndexedLoad = false;
  bool sawConstructorPropagation = false;
  std::size_t directPropagations = 0;
  std::size_t localOriginCount = 0;
  bool allAnchorsExact = true;
  bool allFailureEffectsTrap = true;
  for (const lang::MirBlock &block : mirMain->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      for (const lang::DefinedFailureOrigin &origin :
           instruction.definedFailure.localOrigins) {
        ++localOriginCount;
        allAnchorsExact = allAnchorsExact && origin.sourceUnit != 0 &&
                          origin.end > origin.start && origin.line > 0 &&
                          !origin.outcomes.empty();
      }
      if (!instruction.definedFailure.empty()) {
        const lang::MirEffectTraits effect = lang::effects(instruction);
        allFailureEffectsTrap =
            allFailureEffectsTrap && effect.mayTrap && !effect.speculatable &&
            !effect.removableWhenUnused && !effect.reorderable;
      }
      sawIndexedLoad = sawIndexedLoad ||
                       (instruction.kind == lang::MirInstructionKind::Load &&
                        hasOutcome(instruction.definedFailure,
                                   lang::DefinedFailureCode::IndexOutOfBounds,
                                   lang::DefinedFailureDetail::FixedArray));
      sawConstructorPropagation = sawConstructorPropagation ||
                                  instruction.definedFailure.propagation ==
                                      lang::FailurePropagationKind::Constructor;
      directPropagations += instruction.definedFailure.propagation ==
                                    lang::FailurePropagationKind::DirectCall
                                ? 1
                                : 0;
    }
  }
  expect(localOriginCount >= 8 && allAnchorsExact && sawIndexedLoad &&
             sawConstructorPropagation && directPropagations >= 2 &&
             allFailureEffectsTrap,
         "MIR should retain exact local origins, constructor/call propagation, "
         "and optimizer barriers");

  bool sawVirtualPropagation = false;
  bool sawCallablePropagation = false;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        sawVirtualPropagation = sawVirtualPropagation ||
                                instruction.definedFailure.propagation ==
                                    lang::FailurePropagationKind::VirtualCall;
        sawCallablePropagation = sawCallablePropagation ||
                                 instruction.definedFailure.propagation ==
                                     lang::FailurePropagationKind::Callable;
      }
    }
  }
  expect(sawVirtualPropagation && sawCallablePropagation,
         "MIR should distinguish virtual and callable failure propagation");

  const std::string dump = lang::MirPrinter().print(mirMain->body);
  expect(dump.starts_with("mir-body-v16\n") &&
             dump.find("integer_overflow:addition") != std::string::npos &&
             dump.find("index_out_of_bounds:fixed_array") !=
                 std::string::npos &&
             dump.find("failure-propagation=direct-call") != std::string::npos,
         "MIR v15 should serialize exact failure identities deterministically");

  const auto firstOriginInstruction = [](lang::MirBody &body) {
    for (lang::MirBlock &block : body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return !instruction.definedFailure.localOrigins.empty();
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };

  lang::MirBody invalidPair = mirMain->body;
  lang::MirInstruction *invalidPairInstruction =
      firstOriginInstruction(invalidPair);
  if (invalidPairInstruction != nullptr) {
    invalidPairInstruction->definedFailure.localOrigins.front()
        .outcomes.front() = {.code = lang::DefinedFailureCode::IntegerOverflow,
                             .detail = lang::DefinedFailureDetail::StdoutWrite};
  }
  expect(invalidPairInstruction != nullptr &&
             !lang::verifyMirBody(invalidPair).valid(),
         "the MIR verifier should reject an invalid failure code/detail pair");

  lang::MirBody duplicateOutcome = mirMain->body;
  lang::MirInstruction *duplicateInstruction =
      firstOriginInstruction(duplicateOutcome);
  if (duplicateInstruction != nullptr) {
    std::vector<lang::DefinedFailureOutcome> &outcomes =
        duplicateInstruction->definedFailure.localOrigins.front().outcomes;
    outcomes.push_back(outcomes.front());
  }
  expect(duplicateInstruction != nullptr &&
             !lang::verifyMirBody(duplicateOutcome).valid(),
         "the MIR verifier should reject duplicate failure outcomes");

  lang::MirBody duplicateOrigin = mirMain->body;
  lang::MirInstruction *duplicateOriginInstruction =
      firstOriginInstruction(duplicateOrigin);
  if (duplicateOriginInstruction != nullptr) {
    std::vector<lang::DefinedFailureOrigin> &origins =
        duplicateOriginInstruction->definedFailure.localOrigins;
    origins.push_back(origins.front());
  }
  expect(duplicateOriginInstruction != nullptr &&
             !lang::verifyMirBody(duplicateOrigin).valid(),
         "the MIR verifier should reject duplicate failure origins");

  lang::MirBody emptyOriginSpan = mirMain->body;
  lang::MirInstruction *emptySpanInstruction =
      firstOriginInstruction(emptyOriginSpan);
  if (emptySpanInstruction != nullptr) {
    lang::DefinedFailureOrigin &origin =
        emptySpanInstruction->definedFailure.localOrigins.front();
    origin.end = origin.start;
  }
  expect(emptySpanInstruction != nullptr &&
             !lang::verifyMirBody(emptyOriginSpan).valid(),
         "the MIR verifier should reject an empty failure source span");

  lang::MirProgram missingPropagation = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      missingPropagation.functionInstances());
  lang::MirFunctionInstance *mutableMain =
      hirMain->id == 0 || hirMain->id > functions.size()
          ? nullptr
          : &functions[hirMain->id - 1];
  lang::MirInstruction *propagatingCall = nullptr;
  if (mutableMain != nullptr) {
    for (lang::MirBlock &block : mutableMain->body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.definedFailure.propagation ==
                                lang::FailurePropagationKind::DirectCall;
                       });
      if (found != block.instructions.end()) {
        propagatingCall = &*found;
        break;
      }
    }
  }
  if (propagatingCall != nullptr) {
    propagatingCall->definedFailure.propagation =
        lang::FailurePropagationKind::None;
  }
  const lang::MirVerificationResult missingPropagationResult =
      lang::verifyMirProgram(missingPropagation);
  expect(propagatingCall != nullptr && !missingPropagationResult.valid() &&
             std::any_of(missingPropagationResult.errors.begin(),
                         missingPropagationResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find(
                                      "call failure propagation") !=
                                  std::string::npos;
                         }),
         "program verification should reject a call that drops its exact "
         "failure propagation channel");
}

void testSynchronizationMirContract() {
  const std::string source = R"(
void work() {}
int main() {
  work();
  return 0;
}
)";

  lang::FrontendOptions concurrentOptions;
  concurrentOptions.target = lang::TargetInfo::host();
  concurrentOptions.target.executionProfile =
      lang::ExecutionProfile::Concurrent;
  const lang::FrontendResult concurrent =
      lang::Frontend(concurrentOptions)
          .analyze("synchronization-mir.gti", source);
  expect(concurrent.canGenerateCode(),
         "the synchronization MIR fixture should reach code generation");
  if (!concurrent.canGenerateCode()) {
    return;
  }

  lang::HirProgram stagedHir = concurrent.hir;
  auto &hirFunctions = const_cast<std::vector<lang::HirFunctionInstance> &>(
      stagedHir.functionInstances());
  const auto hirMain =
      std::find_if(hirFunctions.begin(), hirFunctions.end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == "main";
                   });
  lang::HirValue *spawnValue = nullptr;
  if (hirMain != hirFunctions.end()) {
    const auto call =
        std::find_if(hirMain->body.values.begin(), hirMain->body.values.end(),
                     [](const lang::HirValue &value) {
                       return value.kind == lang::HirValueKind::Call &&
                              value.functionTarget.has_value();
                     });
    if (call != hirMain->body.values.end()) {
      spawnValue = &*call;
      spawnValue->synchronization.kind =
          lang::SynchronizationOperationKind::ThreadSpawn;
    }
  }

  const lang::MirLoweringResult lowered =
      lang::MirLowerer().lower(stagedHir, concurrent.failureMetadata);
  const lang::MirFunctionInstance *loweredMain =
      hirMain == hirFunctions.end()
          ? nullptr
          : lowered.program.findFunctionInstance(hirMain->id);
  const lang::MirInstruction *spawnInstruction = nullptr;
  if (loweredMain != nullptr && spawnValue != nullptr) {
    for (const lang::MirBlock &block : loweredMain->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.hirValue == spawnValue->id &&
                   instruction.kind == lang::MirInstructionKind::Call;
          });
      if (found != block.instructions.end()) {
        spawnInstruction = &*found;
        break;
      }
    }
  }
  expect(spawnInstruction != nullptr && lowered.valid() &&
             lowered.program.executionProfile() ==
                 lang::ExecutionProfile::Concurrent &&
             spawnInstruction->synchronization.kind ==
                 lang::SynchronizationOperationKind::ThreadSpawn &&
             lang::verifyMirProgram(lowered.program).valid(),
         "HIR synchronization identity should lower exactly into verified "
         "concurrent MIR");
  if (loweredMain == nullptr || spawnInstruction == nullptr) {
    return;
  }

  const auto mutableInstruction =
      [](lang::MirBody &body,
         lang::MirInstructionId id) -> lang::MirInstruction * {
    for (lang::MirBlock &block : body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == id;
                       });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return nullptr;
  };

  const std::vector<lang::SynchronizationOperation> validOperations = {
      {.kind = lang::SynchronizationOperationKind::ThreadSpawn},
      {.kind = lang::SynchronizationOperationKind::ThreadJoin},
      {.kind = lang::SynchronizationOperationKind::AtomicLoad,
       .order = lang::AtomicMemoryOrder::Acquire},
      {.kind = lang::SynchronizationOperationKind::AtomicStore,
       .order = lang::AtomicMemoryOrder::Release},
      {.kind = lang::SynchronizationOperationKind::AtomicReadModifyWrite,
       .order = lang::AtomicMemoryOrder::AcquireRelease},
      {.kind = lang::SynchronizationOperationKind::AtomicCompareExchange,
       .order = lang::AtomicMemoryOrder::SequentiallyConsistent,
       .failureOrder = lang::AtomicMemoryOrder::Acquire},
      {.kind = lang::SynchronizationOperationKind::MutexLock},
      {.kind = lang::SynchronizationOperationKind::MutexUnlock},
  };
  for (const lang::SynchronizationOperation &operation : validOperations) {
    lang::MirBody candidate = loweredMain->body;
    lang::MirInstruction *instruction =
        mutableInstruction(candidate, spawnInstruction->id);
    if (instruction != nullptr) {
      instruction->synchronization = operation;
    }
    expect(instruction != nullptr && lang::verifyMirBody(candidate).valid(),
           "every well-formed synchronization operation should pass MIR "
           "verification");
  }

  lang::MirBody invalidLoad = loweredMain->body;
  lang::MirInstruction *load =
      mutableInstruction(invalidLoad, spawnInstruction->id);
  if (load != nullptr) {
    load->synchronization = {.kind =
                                 lang::SynchronizationOperationKind::AtomicLoad,
                             .order = lang::AtomicMemoryOrder::Release};
  }
  expect(load != nullptr && !lang::verifyMirBody(invalidLoad).valid(),
         "MIR should reject a release order on an atomic load");

  lang::MirBody invalidCompareExchange = loweredMain->body;
  lang::MirInstruction *compareExchange =
      mutableInstruction(invalidCompareExchange, spawnInstruction->id);
  if (compareExchange != nullptr) {
    compareExchange->synchronization = {
        .kind = lang::SynchronizationOperationKind::AtomicCompareExchange,
        .order = lang::AtomicMemoryOrder::Acquire,
        .failureOrder = lang::AtomicMemoryOrder::SequentiallyConsistent};
  }
  expect(compareExchange != nullptr &&
             !lang::verifyMirBody(invalidCompareExchange).valid(),
         "MIR should reject a compare-exchange failure order stronger than "
         "its success order");

  lang::MirBody misplaced = loweredMain->body;
  lang::MirInstruction *literal = nullptr;
  for (lang::MirBlock &block : misplaced.blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Compute &&
                 instruction.operation == lang::MirOperation::Literal;
        });
    if (found != block.instructions.end()) {
      literal = &*found;
      break;
    }
  }
  if (literal != nullptr) {
    literal->synchronization.kind =
        lang::SynchronizationOperationKind::MutexLock;
  }
  expect(literal != nullptr && !lang::verifyMirBody(misplaced).valid(),
         "MIR should reject synchronization metadata on a non-call "
         "instruction");

  const lang::FrontendResult single = lang::Frontend().analyze(
      "single-threaded-synchronization-mir.gti", source);
  lang::MirProgram forgedSingle = single.mir;
  auto &singleFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forgedSingle.functionInstances());
  lang::MirInstruction *singleCall = nullptr;
  const lang::HirFunctionInstance *singleMain = findHirFunction(single, "main");
  if (singleMain != nullptr && singleMain->id <= singleFunctions.size()) {
    for (lang::MirBlock &block :
         singleFunctions[singleMain->id - 1].body.blocks) {
      const auto call = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget.has_value();
          });
      if (call != block.instructions.end()) {
        singleCall = &*call;
        break;
      }
    }
  }
  if (singleCall != nullptr) {
    singleCall->synchronization.kind =
        lang::SynchronizationOperationKind::ThreadSpawn;
  }
  const lang::MirVerificationResult singleResult =
      lang::verifyMirProgram(forgedSingle);
  expect(singleCall != nullptr && !singleResult.valid() &&
             std::any_of(singleResult.errors.begin(), singleResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("single-threaded") !=
                                  std::string::npos;
                         }),
         "the MIR program verifier should reject synchronization operations "
         "in the single-threaded profile");
}

void testExclusiveReborrowMirFlow() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("exclusive-reborrow-mir.gti", R"(
class Pair {
public:
  mut int left = 0;
  mut int right = 0;

  mut int& get_left() mut { return this.left; }
};

class PairOwner {
public:
  mut Pair inner = Pair();
  mut int other = 0;

  mut Pair& get_inner() mut { return this.inner; }
};

int nested(mut Pair& parent) {
  mut int& field = parent.left;
  mut int& leaf = field;
  leaf += 1;
  field += 2;
  parent.right += 4;
  return parent.left + parent.right;
}

int siblings(mut Pair& parent) {
  mut int& left = parent.left;
  mut int& right = parent.right;
  left += 1;
  right += 2;
  return left + right;
}

int disjoint_parent_access(mut Pair& parent) {
  mut int& left = parent.left;
  parent.right += 1;
  left += 2;
  return parent.left + parent.right;
}

int readonly_child_disjoint_parent_read(mut Pair& parent) {
  int& left = parent.left;
  int right = parent.right;
  return left + right;
}

int retained_call_child(mut Pair& parent) {
  mut int& child = parent.get_left();
  child += 1;
  parent.right += 1;
  return parent.left + parent.right;
}

int retained_call_chain(mut PairOwner& parent) {
  mut int& child = parent.get_inner().get_left();
  child += 1;
  parent.other += 1;
  return parent.inner.left + parent.other;
}

int nonretained_call_member(mut PairOwner& parent) {
  int result = parent.get_inner().left;
  return result;
}

int conditional_reactivation(mut int& parent, bool use_child) {
  mut int& child = parent;
  if (use_child) {
    child += 1;
  } else {
    parent += 2;
  }
  parent += 4;
  return parent;
}

int switch_reactivation(mut int& parent) {
  mut int& child = parent;
  switch (child) {
  case 0:
    parent += 1;
    break;
  default:
    parent += 2;
    break;
  }
  return parent;
}
)");
  expect(frontend.canGenerateCode(),
         "exclusive reborrow fixtures should lower to valid frontend IR");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "exclusive reborrow MIR should satisfy the reusable verifier");

  const lang::MirBody *nested = findFunction(frontend, "nested");
  const lang::MirBody *siblings = findFunction(frontend, "siblings");
  const lang::MirBody *disjointParent =
      findFunction(frontend, "disjoint_parent_access");
  const lang::MirBody *readonlyDisjointParent =
      findFunction(frontend, "readonly_child_disjoint_parent_read");
  const lang::MirBody *retainedCallChild =
      findFunction(frontend, "retained_call_child");
  const lang::MirBody *retainedCallChain =
      findFunction(frontend, "retained_call_chain");
  const lang::MirBody *nonretainedCallMember =
      findFunction(frontend, "nonretained_call_member");
  const lang::MirBody *conditional =
      findFunction(frontend, "conditional_reactivation");
  const lang::MirBody *switchReactivation =
      findFunction(frontend, "switch_reactivation");
  expect(nested != nullptr && siblings != nullptr &&
             disjointParent != nullptr && readonlyDisjointParent != nullptr &&
             retainedCallChild != nullptr && retainedCallChain != nullptr &&
             nonretainedCallMember != nullptr && conditional != nullptr &&
             switchReactivation != nullptr,
         "exclusive reborrow fixtures should expose all MIR bodies");
  if (nested == nullptr || siblings == nullptr || disjointParent == nullptr ||
      readonlyDisjointParent == nullptr || retainedCallChild == nullptr ||
      conditional == nullptr || retainedCallChain == nullptr ||
      nonretainedCallMember == nullptr || switchReactivation == nullptr) {
    return;
  }

  const auto producer = [](const lang::MirBody &body, lang::MirLoanId loan) {
    for (const lang::MirBlock &block : body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.loan && *instruction.loan == loan &&
                   instruction.kind != lang::MirInstructionKind::EndBorrow;
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const auto hasError = [](const lang::MirVerificationResult &result,
                           std::string_view text) {
    return !result.valid() &&
           std::any_of(result.errors.begin(), result.errors.end(),
                       [&](const lang::MirVerificationError &error) {
                         return error.message.find(text) != std::string::npos;
                       });
  };

  const lang::MirLoan *entry = nullptr;
  const lang::MirLoan *field = nullptr;
  const lang::MirLoan *leaf = nullptr;
  for (const lang::MirLoan &loan : nested->loans) {
    if (loan.entry) {
      entry = &loan;
    } else if (loan.parent != 0) {
      const lang::MirLoan *parent = nested->findLoan(loan.parent);
      if (parent != nullptr && parent->entry) {
        field = &loan;
      } else {
        leaf = &loan;
      }
    }
  }
  const lang::MirInstruction *fieldProducer =
      field == nullptr ? nullptr : producer(*nested, field->id);
  const lang::MirInstruction *leafProducer =
      leaf == nullptr ? nullptr : producer(*nested, leaf->id);
  expect(
      entry != nullptr && field != nullptr && leaf != nullptr &&
          field->parent == entry->id && leaf->parent == field->id &&
          fieldProducer != nullptr && leafProducer != nullptr &&
          fieldProducer->operands.size() == 1 &&
          fieldProducer->operands.front().kind == lang::MirOperandKind::Loan &&
          fieldProducer->operands.front().loan == entry->id &&
          leafProducer->operands.size() == 1 &&
          leafProducer->operands.front().kind == lang::MirOperandKind::Loan &&
          leafProducer->operands.front().loan == field->id,
      "nested child loans should retain exact parent IDs and Loan(parent) "
      "borrow operands");

  bool referenceAssignmentUsesReferent = false;
  for (const lang::MirBlock &block : nested->blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Assign ||
          !instruction.destination) {
        continue;
      }
      const lang::MirPlace *destination =
          nested->findPlace(*instruction.destination);
      referenceAssignmentUsesReferent |=
          destination != nullptr &&
          std::any_of(
              destination->projections.begin(), destination->projections.end(),
              [](const lang::MirPlaceProjection &projection) {
                return projection.kind == lang::MirProjectionKind::Dereference;
              });
    }
  }
  expect(referenceAssignmentUsesReferent,
         "assignment through a reference binding should target a dereferenced "
         "referent place in MIR");

  std::vector<const lang::MirLoan *> siblingChildren;
  for (const lang::MirLoan &loan : siblings->loans) {
    if (loan.parent != 0) {
      siblingChildren.push_back(&loan);
    }
  }
  expect(siblingChildren.size() == 2 &&
             siblingChildren[0]->parent == siblingChildren[1]->parent &&
             siblingChildren[0]->source != siblingChildren[1]->source &&
             lang::verifyMirBody(*siblings).valid(),
         "disjoint field reborrows should coexist as children of one suspended "
         "mutable parent");

  if (siblingChildren.size() == 2) {
    lang::MirBody overlappingSibling = *siblings;
    overlappingSibling.loans[siblingChildren[1]->id - 1].source =
        siblingChildren[0]->source;
    expect(hasError(lang::verifyMirBody(overlappingSibling),
                    "active sibling loan"),
           "the verifier should reject overlapping mutable sibling reborrows");

    lang::MirBody overlappingReadonlyLocals = *siblings;
    overlappingReadonlyLocals.loans[siblingChildren[0]->id - 1].access =
        lang::AccessMode::ReadOnly;
    overlappingReadonlyLocals.loans[siblingChildren[1]->id - 1].access =
        lang::AccessMode::ReadOnly;
    overlappingReadonlyLocals.loans[siblingChildren[1]->id - 1].source =
        siblingChildren[0]->source;
    expect(hasError(lang::verifyMirBody(overlappingReadonlyLocals),
                    "active sibling loan"),
           "distinct overlapping read-only Local child loans should be "
           "rejected because shared aliases must reuse one loan identity");
  }

  const auto disjointChild =
      std::find_if(disjointParent->loans.begin(), disjointParent->loans.end(),
                   [](const lang::MirLoan &loan) { return loan.parent != 0; });
  const lang::MirPlace *disjointChildSource =
      disjointChild == disjointParent->loans.end()
          ? nullptr
          : disjointParent->findPlace(disjointChild->source);
  lang::SymbolId childField = 0;
  if (disjointChildSource != nullptr) {
    for (const lang::MirPlaceProjection &projection :
         disjointChildSource->projections) {
      if (projection.kind == lang::MirProjectionKind::Field) {
        childField = projection.field;
      }
    }
  }
  expect(disjointChild != disjointParent->loans.end() && childField != 0 &&
             lang::verifyMirBody(*disjointParent).valid(),
         "a suspended parent should authorize access to a known disjoint "
         "sibling field while its child remains live");
  if (childField != 0) {
    lang::MirBody overlappingParentUse = *disjointParent;
    bool rewired = false;
    for (const lang::MirBlock &block : disjointParent->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if ((instruction.kind != lang::MirInstructionKind::Assign &&
             instruction.kind != lang::MirInstructionKind::Modify) ||
            !instruction.destination) {
          continue;
        }
        lang::MirPlace *destination =
            &overlappingParentUse.places[*instruction.destination - 1];
        for (lang::MirPlaceProjection &projection : destination->projections) {
          if (projection.kind == lang::MirProjectionKind::Field &&
              projection.field != childField) {
            projection.field = childField;
            rewired = true;
            break;
          }
        }
        if (rewired) {
          break;
        }
      }
      if (rewired) {
        break;
      }
    }
    expect(rewired && hasError(lang::verifyMirBody(overlappingParentUse),
                               "active child reborrow"),
           "a suspended parent should not authorize access that overlaps its "
           "active child place");
  }

  const auto readonlyChild = std::find_if(
      readonlyDisjointParent->loans.begin(),
      readonlyDisjointParent->loans.end(), [](const lang::MirLoan &loan) {
        return loan.parent != 0 && loan.access == lang::AccessMode::ReadOnly;
      });
  const lang::MirPlace *readonlyChildSource =
      readonlyChild == readonlyDisjointParent->loans.end()
          ? nullptr
          : readonlyDisjointParent->findPlace(readonlyChild->source);
  lang::SymbolId readonlyChildField = 0;
  if (readonlyChildSource != nullptr) {
    for (const lang::MirPlaceProjection &projection :
         readonlyChildSource->projections) {
      if (projection.kind == lang::MirProjectionKind::Field) {
        readonlyChildField = projection.field;
      }
    }
  }
  expect(readonlyChild != readonlyDisjointParent->loans.end() &&
             readonlyChildField != 0 &&
             lang::verifyMirBody(*readonlyDisjointParent).valid(),
         "a suspended mutable parent should permit a read of a known disjoint "
         "field while a read-only child is active");
  const auto overlapParentRead = [&](lang::MirBody &body,
                                     lang::MirOperandKind replacement) {
    bool childActive = false;
    for (lang::MirBlock &block : body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.loan == readonlyChild->id &&
            instruction.kind == lang::MirInstructionKind::Borrow) {
          childActive = true;
          continue;
        }
        if (instruction.loan == readonlyChild->id &&
            instruction.kind == lang::MirInstructionKind::EndBorrow) {
          childActive = false;
          continue;
        }
        if (!childActive) {
          continue;
        }
        for (lang::MirOperand &operand : instruction.operands) {
          if (operand.kind != lang::MirOperandKind::Copy ||
              operand.place == 0) {
            continue;
          }
          lang::MirPlace *place = &body.places[operand.place - 1];
          for (lang::MirPlaceProjection &projection : place->projections) {
            if (projection.kind == lang::MirProjectionKind::Field &&
                projection.field != readonlyChildField) {
              projection.field = readonlyChildField;
              operand.kind = replacement;
              return true;
            }
          }
        }
      }
    }
    return false;
  };
  if (readonlyChildField != 0) {
    lang::MirBody overlappingCopy = *readonlyDisjointParent;
    expect(overlapParentRead(overlappingCopy, lang::MirOperandKind::Copy) &&
               hasError(lang::verifyMirBody(overlappingCopy),
                        "active child reborrow"),
           "an overlapping Copy through a suspended mutable parent should be "
           "rejected even when its active child is read-only");

    lang::MirBody overlappingBorrowRead = *readonlyDisjointParent;
    bool insertedBorrowRead = false;
    lang::MirInstructionId nextInstruction = 1;
    for (const lang::MirBlock &block : overlappingBorrowRead.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        nextInstruction = std::max(nextInstruction, instruction.id + 1);
      }
    }
    bool readonlyChildActive = false;
    for (lang::MirBlock &block : overlappingBorrowRead.blocks) {
      for (auto instruction = block.instructions.begin();
           instruction != block.instructions.end(); ++instruction) {
        if (instruction->loan == readonlyChild->id &&
            instruction->kind == lang::MirInstructionKind::Borrow) {
          readonlyChildActive = true;
          continue;
        }
        if (instruction->loan == readonlyChild->id &&
            instruction->kind == lang::MirInstructionKind::EndBorrow) {
          readonlyChildActive = false;
          continue;
        }
        if (!readonlyChildActive) {
          continue;
        }
        const auto disjointRead = std::find_if(
            instruction->operands.begin(), instruction->operands.end(),
            [&](const lang::MirOperand &operand) {
              if (operand.kind != lang::MirOperandKind::Copy ||
                  operand.place == 0) {
                return false;
              }
              const lang::MirPlace *place =
                  overlappingBorrowRead.findPlace(operand.place);
              return place != nullptr &&
                     std::any_of(
                         place->projections.begin(), place->projections.end(),
                         [&](const lang::MirPlaceProjection &projection) {
                           return projection.kind ==
                                      lang::MirProjectionKind::Field &&
                                  projection.field != readonlyChildField;
                         });
            });
        if (disjointRead == instruction->operands.end()) {
          continue;
        }
        lang::MirPlace overlapping =
            overlappingBorrowRead.places[disjointRead->place - 1];
        overlapping.id = overlappingBorrowRead.places.size() + 1;
        for (lang::MirPlaceProjection &projection : overlapping.projections) {
          if (projection.kind == lang::MirProjectionKind::Field &&
              projection.field != readonlyChildField) {
            projection.field = readonlyChildField;
          }
        }
        const lang::MirPlaceId overlappingPlace = overlapping.id;
        const lang::SemanticType borrowedType = overlapping.type;
        overlappingBorrowRead.places.push_back(std::move(overlapping));
        block.instructions.insert(
            instruction,
            {.id = nextInstruction,
             .kind = lang::MirInstructionKind::Call,
             .operands = {{.kind = lang::MirOperandKind::BorrowRead,
                           .place = overlappingPlace,
                           .type = lang::SemanticType::referenceTo(
                               borrowedType, lang::AccessMode::ReadOnly)}},
             .info = lang::ExpressionInfo{.type = lang::SemanticType::Void}});
        insertedBorrowRead = true;
        break;
      }
      if (insertedBorrowRead) {
        break;
      }
    }
    expect(insertedBorrowRead &&
               lang::rebuildMirValueUses(overlappingBorrowRead) &&
               hasError(lang::verifyMirBody(overlappingBorrowRead),
                        "active child reborrow"),
           "an overlapping BorrowRead through a suspended mutable parent "
           "should be rejected even when its active child is read-only");
  }

  const lang::MirLoan *callEntry = nullptr;
  const lang::MirLoan *callTransient = nullptr;
  const lang::MirLoan *callChild = nullptr;
  for (const lang::MirLoan &loan : retainedCallChild->loans) {
    if (loan.entry) {
      callEntry = &loan;
    } else if (loan.kind == lang::MirLoanKind::CallResult) {
      callTransient = &loan;
    } else if (loan.parent != 0) {
      callChild = &loan;
    }
  }
  std::size_t instructionOrdinal = 0;
  std::size_t callOrdinal = 0;
  std::size_t transientEndOrdinal = 0;
  std::size_t childBorrowOrdinal = 0;
  for (const lang::MirBlock &block : retainedCallChild->blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      ++instructionOrdinal;
      if (callTransient != nullptr && instruction.loan == callTransient->id) {
        if (instruction.kind == lang::MirInstructionKind::Call) {
          callOrdinal = instructionOrdinal;
        } else if (instruction.kind == lang::MirInstructionKind::EndBorrow) {
          transientEndOrdinal = instructionOrdinal;
        }
      }
      if (callChild != nullptr && instruction.loan == callChild->id &&
          instruction.kind == lang::MirInstructionKind::Borrow) {
        childBorrowOrdinal = instructionOrdinal;
      }
    }
  }
  expect(callEntry != nullptr && callTransient != nullptr &&
             callChild != nullptr && callChild->parent == callEntry->id &&
             callTransient->carriers.empty() && !callTransient->escapes &&
             callOrdinal != 0 && callOrdinal < transientEndOrdinal &&
             transientEndOrdinal < childBorrowOrdinal &&
             lang::verifyMirBody(*retainedCallChild).valid(),
         "retaining a receiver-tied mutable call result should end its "
         "unretained transient before producing the semantic child reborrow");

  const lang::MirLoan *chainEntry = nullptr;
  const lang::MirLoan *chainChild = nullptr;
  std::vector<const lang::MirLoan *> chainTransients;
  for (const lang::MirLoan &loan : retainedCallChain->loans) {
    if (loan.entry) {
      chainEntry = &loan;
    } else if (loan.kind == lang::MirLoanKind::CallResult) {
      chainTransients.push_back(&loan);
    } else if (loan.parent != 0) {
      chainChild = &loan;
    }
  }
  std::sort(chainTransients.begin(), chainTransients.end(),
            [](const lang::MirLoan *left, const lang::MirLoan *right) {
              return left->id < right->id;
            });
  std::vector<std::pair<lang::MirInstructionKind, lang::MirLoanId>>
      chainLoanEvents;
  for (const lang::MirBlock &block : retainedCallChain->blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.loan &&
          (instruction.kind == lang::MirInstructionKind::Call ||
           instruction.kind == lang::MirInstructionKind::EndBorrow ||
           instruction.kind == lang::MirInstructionKind::Borrow)) {
        chainLoanEvents.emplace_back(instruction.kind, *instruction.loan);
      }
    }
  }
  const bool orderedChainRetirement =
      chainEntry != nullptr && chainChild != nullptr &&
      chainTransients.size() == 2 && chainLoanEvents.size() >= 5 &&
      chainLoanEvents[0] ==
          std::pair{lang::MirInstructionKind::Call, chainTransients[0]->id} &&
      chainLoanEvents[1] ==
          std::pair{lang::MirInstructionKind::Call, chainTransients[1]->id} &&
      chainLoanEvents[2] == std::pair{lang::MirInstructionKind::EndBorrow,
                                      chainTransients[1]->id} &&
      chainLoanEvents[3] == std::pair{lang::MirInstructionKind::EndBorrow,
                                      chainTransients[0]->id} &&
      chainLoanEvents[4] ==
          std::pair{lang::MirInstructionKind::Borrow, chainChild->id};
  expect(orderedChainRetirement &&
             chainTransients[0]->parent == chainEntry->id &&
             chainTransients[1]->parent == chainTransients[0]->id &&
             chainChild->parent == chainEntry->id &&
             lang::verifyMirBody(*retainedCallChain).valid() &&
             lang::verifyMirBody(*nonretainedCallMember).valid(),
         "receiver-tied call-result chains should retain exact transient "
         "ancestry, retire leaf-to-root when retained, and authorize "
         "non-retained member access through the derived result");

  if (chainEntry != nullptr && chainChild != nullptr &&
      chainTransients.size() == 2) {
    lang::MirBody wrongCallParent = *retainedCallChain;
    wrongCallParent.loans[chainTransients[1]->id - 1].parent = chainEntry->id;
    expect(hasError(lang::verifyMirBody(wrongCallParent),
                    "must be produced through its declared parent"),
           "a derived call result should reject a parent that is not its "
           "actual receiver loan");

    lang::MirBody siblingCallParent = *retainedCallChain;
    siblingCallParent.loans[chainTransients[1]->id - 1].parent = chainChild->id;
    expect(!lang::verifyMirBody(siblingCallParent).valid(),
           "a derived call result should reject an unrelated sibling as its "
           "loan parent");
  }

  if (entry != nullptr && field != nullptr && leaf != nullptr &&
      leafProducer != nullptr) {
    lang::MirBody widenedChild = *nested;
    widenedChild.loans[leaf->id - 1].source = entry->source;
    expect(hasError(lang::verifyMirBody(widenedChild),
                    "not contained within storage"),
           "the verifier should reject a child source widened beyond its "
           "declared parent place");

    lang::MirBody skippedParent = *nested;
    skippedParent.loans[leaf->id - 1].parent = entry->id;
    bool rewired = false;
    for (lang::MirBlock &block : skippedParent.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.loan && *instruction.loan == leaf->id &&
            instruction.kind == lang::MirInstructionKind::Borrow) {
          instruction.operands.front().loan = entry->id;
          rewired = true;
        }
      }
    }
    expect(
        rewired &&
            hasError(lang::verifyMirBody(skippedParent), "active sibling loan"),
        "the verifier should reject a child rewired around its active direct "
        "parent");
  }

  const lang::SemanticType mutableReference = lang::SemanticType::referenceTo(
      lang::SemanticType::Int32, lang::AccessMode::Mutable);
  const lang::ExpressionInfo referenceInfo{
      .type = mutableReference,
      .category = lang::ValueCategory::Place,
      .access = lang::AccessMode::Mutable,
      .traits = lang::semanticTraits(mutableReference)};
  lang::MirBody branchState{
      .kind = lang::MirBodyKind::Function,
      .entry = 1,
      .returnType = lang::SemanticType::Void,
      .blocks =
          {{.id = 1,
            .instructions = {{.id = 1,
                              .kind = lang::MirInstructionKind::Borrow,
                              .operands = {{.kind = lang::MirOperandKind::Loan,
                                            .loan = 1,
                                            .type = mutableReference}},
                              .loan = 2,
                              .info = referenceInfo}},
            .terminator = {.kind = lang::MirTerminatorKind::Branch,
                           .value =
                               lang::MirOperand{
                                   .kind = lang::MirOperandKind::Constant,
                                   .literal = lang::Literal{true},
                                   .type = lang::SemanticType::Bool},
                           .target = 2,
                           .elseTarget = 3},
            .reachable = true},
           {.id = 2,
            .instructions = {{.id = 2,
                              .kind = lang::MirInstructionKind::EndBorrow,
                              .loan = 2}},
            .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4},
            .reachable = true},
           {.id = 3,
            .instructions = {{.id = 3,
                              .kind = lang::MirInstructionKind::EndBorrow,
                              .loan = 2}},
            .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4},
            .reachable = true},
           {.id = 4,
            .instructions = {{.id = 4,
                              .kind = lang::MirInstructionKind::EndBorrow,
                              .loan = 1}},
            .terminator = {.kind = lang::MirTerminatorKind::Return},
            .reachable = true}},
      .places = {{.id = 1,
                  .root = lang::MirPlaceRootKind::Binding,
                  .binding = 1,
                  .type = lang::SemanticType::Int32,
                  .access = lang::AccessMode::Mutable,
                  .traits = lang::semanticTraits(lang::SemanticType::Int32)}},
      .loans = {{.id = 1,
                 .kind = lang::MirLoanKind::Parameter,
                 .source = 1,
                 .access = lang::AccessMode::Mutable,
                 .carriers = {1},
                 .entry = true},
                {.id = 2,
                 .parent = 1,
                 .kind = lang::MirLoanKind::Local,
                 .source = 1,
                 .access = lang::AccessMode::Mutable,
                 .carriers = {2}}}};
  expect(lang::rebuildMirValueUses(branchState) &&
             lang::verifyMirBody(branchState).valid() &&
             lang::verifyMirBody(*conditional).valid() &&
             lang::verifyMirBody(*switchReactivation).valid(),
         "ending a conditional child on every path should reactivate its "
         "parent and give if/switch joins one exact Active state");

  lang::MirBody escapingChild = branchState;
  escapingChild.loans[1].escapes = true;
  expect(hasError(lang::verifyMirBody(escapingChild),
                  "non-escaping Local or CallResult"),
         "a parent-linked child loan must not escape its parent topology");

  lang::MirBody storedChild = branchState;
  storedChild.loans[1].kind = lang::MirLoanKind::Stored;
  expect(hasError(lang::verifyMirBody(storedChild),
                  "non-escaping Local or CallResult"),
         "Stored, Return, and Parameter loans must not be parent-linked "
         "children");

  lang::MirBody sharedLocalChild = branchState;
  sharedLocalChild.loans[0].access = lang::AccessMode::ReadOnly;
  sharedLocalChild.loans[1].access = lang::AccessMode::ReadOnly;
  expect(hasError(lang::verifyMirBody(sharedLocalChild),
                  "read-only derived call result"),
         "a read-only Local child must reuse its parent's identity instead of "
         "forming persistent read-only ancestry");

  lang::MirBody overlappingCallResults{
      .kind = lang::MirBodyKind::Function,
      .entry = 1,
      .returnType = lang::SemanticType::Void,
      .blocks =
          {{.id = 1,
            .instructions =
                {{.id = 1,
                  .kind = lang::MirInstructionKind::Call,
                  .receiver =
                      lang::MirOperand{.kind =
                                           lang::MirOperandKind::BorrowWrite,
                                       .place = 2,
                                       .type = mutableReference},
                  .loan = 2,
                  .borrowOrigin = lang::BorrowOriginKind::Receiver,
                  .borrowAccess = lang::AccessMode::Mutable,
                  .info =
                      lang::ExpressionInfo{.type = lang::SemanticType::Void}},
                 {.id = 2,
                  .kind = lang::MirInstructionKind::Call,
                  .receiver =
                      lang::MirOperand{.kind =
                                           lang::MirOperandKind::BorrowWrite,
                                       .place = 2,
                                       .type = mutableReference},
                  .loan = 3,
                  .borrowOrigin = lang::BorrowOriginKind::Receiver,
                  .borrowAccess = lang::AccessMode::Mutable,
                  .info =
                      lang::ExpressionInfo{.type = lang::SemanticType::Void}},
                 {.id = 3,
                  .kind = lang::MirInstructionKind::EndBorrow,
                  .loan = 3},
                 {.id = 4,
                  .kind = lang::MirInstructionKind::EndBorrow,
                  .loan = 2},
                 {.id = 5,
                  .kind = lang::MirInstructionKind::EndBorrow,
                  .loan = 1}},
            .terminator = {.kind = lang::MirTerminatorKind::Return},
            .reachable = true}},
      .places = {{.id = 1,
                  .root = lang::MirPlaceRootKind::Binding,
                  .binding = 1,
                  .type = lang::SemanticType::Int32,
                  .access = lang::AccessMode::Mutable,
                  .traits = lang::semanticTraits(lang::SemanticType::Int32)},
                 {.id = 2,
                  .root = lang::MirPlaceRootKind::Binding,
                  .binding = 1,
                  .projections = {{.kind =
                                       lang::MirProjectionKind::Dereference}},
                  .type = lang::SemanticType::Int32,
                  .access = lang::AccessMode::Mutable,
                  .traits = lang::semanticTraits(lang::SemanticType::Int32)}},
      .loans = {{.id = 1,
                 .kind = lang::MirLoanKind::Parameter,
                 .source = 1,
                 .access = lang::AccessMode::Mutable,
                 .carriers = {1},
                 .entry = true},
                {.id = 2,
                 .parent = 1,
                 .kind = lang::MirLoanKind::CallResult,
                 .source = 1,
                 .access = lang::AccessMode::Mutable,
                 .producedBy = 1},
                {.id = 3,
                 .parent = 1,
                 .kind = lang::MirLoanKind::CallResult,
                 .source = 1,
                 .access = lang::AccessMode::Mutable,
                 .producedBy = 2}}};
  expect(lang::rebuildMirValueUses(overlappingCallResults) &&
             hasError(lang::verifyMirBody(overlappingCallResults),
                      "active child reborrow"),
         "overlapping mutable call-result loans should not bypass parent loan "
         "protection merely because they are unretained ephemerals");

  lang::MirBody sharedCallResults = overlappingCallResults;
  for (lang::MirInstruction &instruction :
       sharedCallResults.blocks.front().instructions) {
    if (instruction.kind == lang::MirInstructionKind::Call) {
      instruction.receiver->kind = lang::MirOperandKind::BorrowRead;
      instruction.borrowAccess = lang::AccessMode::ReadOnly;
    }
  }
  sharedCallResults.loans[1].access = lang::AccessMode::ReadOnly;
  sharedCallResults.loans[2].access = lang::AccessMode::ReadOnly;
  expect(lang::rebuildMirValueUses(sharedCallResults) &&
             lang::verifyMirBody(sharedCallResults).valid(),
         "overlapping read-only call-result loans should coexist as siblings "
         "under one mutable parent");

  lang::MirBody storedDependencyAndCallResult = sharedCallResults;
  storedDependencyAndCallResult.loans[1].parent = 0;
  storedDependencyAndCallResult.loans[1].kind = lang::MirLoanKind::Stored;
  expect(lang::rebuildMirValueUses(storedDependencyAndCallResult) &&
             lang::verifyMirBody(storedDependencyAndCallResult).valid(),
         "a derived read-only CallResult should coexist with an active "
         "parentless read-only Stored dependency over the same source");

  const auto replaceCallWithLocalBorrow = [&](lang::MirBody &body,
                                              std::size_t instructionIndex,
                                              lang::MirLoanId loan) {
    const lang::MirInstructionId id =
        body.blocks.front().instructions[instructionIndex].id;
    body.blocks.front().instructions[instructionIndex] = {
        .id = id,
        .kind = lang::MirInstructionKind::Borrow,
        .operands = {{.kind = lang::MirOperandKind::Loan,
                      .loan = 1,
                      .type = mutableReference}},
        .loan = loan,
        .info = referenceInfo};
    body.loans[loan - 1].kind = lang::MirLoanKind::Local;
  };

  lang::MirBody ephemeralThenLocal = sharedCallResults;
  replaceCallWithLocalBorrow(ephemeralThenLocal, 1, 3);
  expect(lang::rebuildMirValueUses(ephemeralThenLocal) &&
             hasError(lang::verifyMirBody(ephemeralThenLocal),
                      "active sibling loan"),
         "a read-only Local child should not be created over an active "
         "overlapping read-only CallResult sibling");

  lang::MirBody localThenEphemeral = sharedCallResults;
  replaceCallWithLocalBorrow(localThenEphemeral, 0, 2);
  expect(lang::rebuildMirValueUses(localThenEphemeral) &&
             hasError(lang::verifyMirBody(localThenEphemeral),
                      "active child reborrow"),
         "a read-only CallResult should not bypass an active overlapping "
         "read-only Local child through their suspended parent");

  lang::MirBody inconsistentJoin = branchState;
  inconsistentJoin.blocks[2].instructions.clear();
  expect(hasError(lang::verifyMirBody(inconsistentJoin),
                  "inconsistent active/suspended state"),
         "CFG joins should reject Active versus Suspended loan states");

  lang::MirBody parentBeforeChild = branchState;
  parentBeforeChild.blocks.front().instructions.push_back(
      {.id = 5, .kind = lang::MirInstructionKind::EndBorrow, .loan = 1});
  expect(hasError(lang::verifyMirBody(parentBeforeChild),
                  "ended before its active child"),
         "explicit endpoints should end a child before its suspended parent");

  lang::MirBody suspendedUse = branchState;
  suspendedUse.blocks.front().instructions.push_back(
      {.id = 5,
       .kind = lang::MirInstructionKind::Call,
       .receiver = lang::MirOperand{.kind = lang::MirOperandKind::Loan,
                                    .loan = 1,
                                    .type = mutableReference},
       .info = lang::ExpressionInfo{.type = lang::SemanticType::Void}});
  expect(hasError(lang::verifyMirBody(suspendedUse),
                  "exclusive child reborrow is active"),
         "a suspended parent loan should not authorize reads or writes until "
         "its last child ends");

  lang::MirBody overlappingLoanCall = branchState;
  overlappingLoanCall.blocks.front().instructions.push_back(
      {.id = 5,
       .kind = lang::MirInstructionKind::Call,
       .operands = {{.kind = lang::MirOperandKind::Loan,
                     .loan = 2,
                     .type = mutableReference},
                    {.kind = lang::MirOperandKind::Loan,
                     .loan = 2,
                     .type = mutableReference}},
       .info = lang::ExpressionInfo{.type = lang::SemanticType::Void}});
  expect(hasError(lang::verifyMirBody(overlappingLoanCall),
                  "overlapping call-duration reference operands"),
         "call-boundary verification should include retained Loan operands in "
         "its pairwise alias check");

  lang::MirBody directReadDuringChild = branchState;
  directReadDuringChild.blocks.front().instructions.push_back(
      {.id = 5,
       .kind = lang::MirInstructionKind::Call,
       .operands = {{.kind = lang::MirOperandKind::BorrowRead,
                     .place = 1,
                     .type = mutableReference}},
       .info = lang::ExpressionInfo{.type = lang::SemanticType::Void}});
  expect(hasError(lang::verifyMirBody(directReadDuringChild),
                  "read of a place conflicts with active mutable loan"),
         "canonical place verification should reject a direct owner read that "
         "overlaps an active mutable child");

  lang::MirBody directWriteDuringChild = branchState;
  directWriteDuringChild.blocks.front().instructions.push_back(
      {.id = 5,
       .kind = lang::MirInstructionKind::Drop,
       .destination = 1,
       .info = lang::ExpressionInfo{
           .type = lang::SemanticType::Int32,
           .category = lang::ValueCategory::Place,
           .access = lang::AccessMode::Mutable,
           .traits = lang::semanticTraits(lang::SemanticType::Int32)}});
  expect(hasError(lang::verifyMirBody(directWriteDuringChild),
                  "write of a place conflicts with active mutable loan"),
         "canonical place verification should reject a direct owner write that "
         "overlaps an active mutable child");

  lang::MirBody carrierDrop = branchState;
  carrierDrop.blocks.resize(1);
  carrierDrop.blocks.front().instructions = {
      {.id = 1,
       .kind = lang::MirInstructionKind::Drop,
       .destination = 1,
       .info = lang::ExpressionInfo{.type = lang::SemanticType::Int32,
                                    .category = lang::ValueCategory::Place,
                                    .access = lang::AccessMode::Mutable}},
      {.id = 2, .kind = lang::MirInstructionKind::EndBorrow, .loan = 1}};
  carrierDrop.blocks.front().terminator = {.kind =
                                               lang::MirTerminatorKind::Return};
  carrierDrop.loans.resize(1);
  carrierDrop.places.front().traits.containsBorrowedState = true;
  expect(lang::rebuildMirValueUses(carrierDrop) &&
             lang::verifyMirBody(carrierDrop).valid(),
         "dropping an exact borrowed-state carrier should clean up the handle "
         "without being mistaken for a write to its referent");

  lang::MirBody ownerDrop = carrierDrop;
  ownerDrop.places.front().traits.containsBorrowedState = false;
  expect(hasError(lang::verifyMirBody(ownerDrop),
                  "write of a place conflicts with active mutable loan"),
         "the carrier Drop exemption should not authorize dropping ordinary "
         "owner storage protected by a live loan");

  lang::MirBody callAliases{
      .kind = lang::MirBodyKind::Function,
      .entry = 1,
      .returnType = lang::SemanticType::Void,
      .blocks = {{.id = 1,
                  .instructions =
                      {{.id = 1,
                        .kind = lang::MirInstructionKind::Call,
                        .operands = {{.kind = lang::MirOperandKind::BorrowWrite,
                                      .place = 1,
                                      .type = mutableReference},
                                     {.kind = lang::MirOperandKind::BorrowRead,
                                      .place = 1,
                                      .type = mutableReference}},
                        .info =
                            lang::ExpressionInfo{
                                .type = lang::SemanticType::Void}}},
                  .terminator = {.kind = lang::MirTerminatorKind::Return},
                  .reachable = true}},
      .places = {{.id = 1,
                  .root = lang::MirPlaceRootKind::Binding,
                  .binding = 1,
                  .projections = {{.kind = lang::MirProjectionKind::Field,
                                   .field = 101}},
                  .type = lang::SemanticType::Int32,
                  .access = lang::AccessMode::Mutable,
                  .traits = lang::semanticTraits(lang::SemanticType::Int32)},
                 {.id = 2,
                  .root = lang::MirPlaceRootKind::Binding,
                  .binding = 1,
                  .projections = {{.kind = lang::MirProjectionKind::Field,
                                   .field = 102}},
                  .type = lang::SemanticType::Int32,
                  .access = lang::AccessMode::Mutable,
                  .traits = lang::semanticTraits(lang::SemanticType::Int32)}}};
  expect(lang::rebuildMirValueUses(callAliases) &&
             hasError(lang::verifyMirBody(callAliases),
                      "overlapping call-duration reference operands"),
         "call-boundary verification should reject overlapping mutable and "
         "read-only temporary borrows");

  lang::MirBody disjointCallBorrows = callAliases;
  disjointCallBorrows.blocks.front().instructions.front().operands[1].place = 2;
  disjointCallBorrows.blocks.front().instructions.front().operands[1].kind =
      lang::MirOperandKind::BorrowWrite;
  expect(lang::rebuildMirValueUses(disjointCallBorrows) &&
             lang::verifyMirBody(disjointCallBorrows).valid(),
         "call-boundary verification should allow mutable borrows of disjoint "
         "sibling fields");

  lang::MirBody sharedCallBorrows = callAliases;
  sharedCallBorrows.blocks.front().instructions.front().operands[0].kind =
      lang::MirOperandKind::BorrowRead;
  expect(lang::rebuildMirValueUses(sharedCallBorrows) &&
             lang::verifyMirBody(sharedCallBorrows).valid(),
         "call-boundary verification should allow overlapping read-only "
         "temporary borrows");
}

void testTransientBorrowNormalization() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("transient-borrow-normalization.gti", R"(
class Leaf {
public:
  mut int value = 1;

  int& read() { return this.value; }
  mut int& get() mut { return this.value; }
};

class Middle {
public:
  mut Leaf leaf = Leaf();

  mut Leaf& next() mut { return this.leaf; }
};

class Root {
public:
  mut Middle middle = Middle();

  mut Middle& next() mut { return this.middle; }
  mut int& chained() mut { return this.next().next().get(); }
};

class Split {
public:
  mut Leaf left = Leaf();
  mut Leaf right = Leaf();
};

class View {
  int& value;

public:
  View(int& source) : value(source) {}
  int& get() { return this.value; }
};

class Owner {
public:
  mut int value = 1;

  int& read() { return this.value; }
  void bump() mut { this.value += 1; }
};

int& observe(int& source) { return source; }
int consume(int& source) { return source; }
int sum(mut int& left, mut int& right) { return left + right; }
int inspect(View value) { return value.get(); }

int retained_depth_three(mut Root& root) {
  mut int& value = root.next().next().get();
  value += 1;
  return value;
}

int readonly_depth_three(int& source) {
  int& value = observe(observe(observe(source)));
  return value;
}

int receiver_return(mut Root& root) {
  mut int& value = root.chained();
  value += 1;
  return value;
}

int disjoint_projected_receivers(mut Split& parent) {
  return sum(parent.left.get(), parent.right.get());
}

int retained_projected_receiver(mut Split& parent) {
  mut int& child = parent.left.get();
  parent.right.value += 1;
  child += 1;
  return child + parent.right.value;
}

int retained_projected_helper(mut Split& parent) {
  int& child = observe(parent.left.value);
  int observed = child;
  parent.right.value += 1;
  return observed + parent.right.value;
}

int comma_reference(mut Leaf& parent) {
  return consume((0, parent.read()));
}

int comma_two_reads(mut Leaf& parent) {
  return consume((parent.read(), parent.read()));
}

int stored_temporary_cleanup(mut Owner& parent) {
  int observed = inspect(View(parent.read()));
  parent.bump();
  return observed;
}

int main() {
  mut Root root = Root();
  mut Split split = Split();
  mut Owner owner = Owner();
  return retained_depth_three(root) + readonly_depth_three(root.middle.leaf.value) +
         receiver_return(root) + disjoint_projected_receivers(split) +
         retained_projected_receiver(split) +
         retained_projected_helper(split) + comma_reference(split.left) +
         comma_two_reads(split.right) +
         stored_temporary_cleanup(owner) - 9;
}
)");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "deep receiver/argument chains, projected receivers, comma-wrapped "
         "references, and stored temporaries should produce valid MIR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirBody *retained =
      findFunction(frontend, "retained_depth_three");
  const lang::MirBody *receiverReturn = findFunction(frontend, "chained");
  const lang::MirBody *projected =
      findFunction(frontend, "disjoint_projected_receivers");
  const lang::MirBody *retainedProjectedReceiver =
      findFunction(frontend, "retained_projected_receiver");
  const lang::MirBody *retainedProjectedHelper =
      findFunction(frontend, "retained_projected_helper");
  const lang::MirBody *comma = findFunction(frontend, "comma_reference");
  const lang::MirBody *commaReads = findFunction(frontend, "comma_two_reads");
  const lang::MirBody *stored =
      findFunction(frontend, "stored_temporary_cleanup");
  expect(retained != nullptr && receiverReturn != nullptr &&
             projected != nullptr && retainedProjectedReceiver != nullptr &&
             retainedProjectedHelper != nullptr && comma != nullptr &&
             commaReads != nullptr && stored != nullptr,
         "transient normalization fixtures should expose each focused MIR "
         "body");
  if (retained == nullptr || receiverReturn == nullptr ||
      projected == nullptr || retainedProjectedReceiver == nullptr ||
      retainedProjectedHelper == nullptr || comma == nullptr ||
      commaReads == nullptr || stored == nullptr) {
    return;
  }

  const auto callResults = [](const lang::MirBody &body) {
    return static_cast<std::size_t>(std::count_if(
        body.loans.begin(), body.loans.end(), [](const lang::MirLoan &loan) {
          return loan.kind == lang::MirLoanKind::CallResult;
        }));
  };
  const auto instructionForLoan = [](const lang::MirBody &body,
                                     lang::MirInstructionKind kind,
                                     lang::MirLoanId loan) {
    return std::any_of(
        body.blocks.begin(), body.blocks.end(),
        [&](const lang::MirBlock &block) {
          return std::any_of(
              block.instructions.begin(), block.instructions.end(),
              [&](const lang::MirInstruction &instruction) {
                return instruction.kind == kind && instruction.loan == loan;
              });
        });
  };

  expect(callResults(*retained) == 3 && lang::verifyMirBody(*retained).valid(),
         "retaining a three-deep receiver chain should preserve and retire an "
         "arbitrary transient ancestry suffix");

  const auto returned = std::find_if(
      receiverReturn->loans.begin(), receiverReturn->loans.end(),
      [](const lang::MirLoan &loan) {
        return loan.kind == lang::MirLoanKind::Return && loan.escapes;
      });
  expect(callResults(*receiverReturn) == 3 &&
             returned != receiverReturn->loans.end() && returned->parent == 0 &&
             lang::verifyMirBody(*receiverReturn).valid(),
         "a receiver-tied return chain should retire transient ancestors and "
         "escape one normalized return loan");

  std::vector<const lang::MirLoan *> projectedResults;
  for (const lang::MirLoan &loan : projected->loans) {
    if (loan.kind == lang::MirLoanKind::CallResult) {
      projectedResults.push_back(&loan);
    }
  }
  expect(projectedResults.size() == 2 &&
             projectedResults[0]->source != projectedResults[1]->source &&
             lang::verifyMirBody(*projected).valid(),
         "selected receiver projections should survive canonicalization so "
         "disjoint sibling calls remain disjoint");

  expect(lang::verifyMirBody(*retainedProjectedReceiver).valid() &&
             lang::verifyMirBody(*retainedProjectedHelper).valid() &&
             callResults(*retainedProjectedReceiver) == 1 &&
             callResults(*retainedProjectedHelper) == 1,
         "retaining receiver- and argument-origin call results should retire "
         "their exact projected transient before creating the semantic child");

  const auto allValuesDefined = [](const lang::MirBody &body) {
    return std::all_of(body.values.begin(), body.values.end(),
                       [](const lang::MirValue &value) {
                         return value.definitionBlock != 0 &&
                                value.definition != 0;
                       });
  };
  expect(allValuesDefined(*comma) && allValuesDefined(*commaReads) &&
             lang::verifyMirBody(*comma).valid() &&
             lang::verifyMirBody(*commaReads).valid(),
         "comma reference expressions should evaluate earlier operands while "
         "forwarding the right-hand place and loan identity");

  const auto temporaryStored =
      std::find_if(stored->loans.begin(), stored->loans.end(),
                   [](const lang::MirLoan &loan) {
                     return loan.kind == lang::MirLoanKind::Stored &&
                            loan.carriers.empty() && !loan.escapes;
                   });
  expect(temporaryStored != stored->loans.end() &&
             instructionForLoan(*stored, lang::MirInstructionKind::EndBorrow,
                                temporaryStored->id) &&
             lang::verifyMirBody(*stored).valid(),
         "a carrier-free temporary Stored loan should end at its full "
         "expression before a following owner mutation");
}

void testReturnEdgeLoanIdentity() {
  const lang::SemanticType reference = lang::SemanticType::referenceTo(
      lang::SemanticType::Int32, lang::AccessMode::ReadOnly);
  lang::MirBody body{
      .kind = lang::MirBodyKind::Function,
      .entry = 1,
      .returnType = reference,
      .blocks = {{.id = 1,
                  .terminator = {.kind = lang::MirTerminatorKind::Branch,
                                 .value =
                                     lang::MirOperand{
                                         .kind = lang::MirOperandKind::Constant,
                                         .literal = lang::Literal{true},
                                         .type = lang::SemanticType::Bool},
                                 .target = 2,
                                 .elseTarget = 3},
                  .reachable = true},
                 {.id = 2,
                  .instructions = {{.id = 1,
                                    .kind = lang::MirInstructionKind::EndBorrow,
                                    .loan = 2}},
                  .terminator =
                      {.kind = lang::MirTerminatorKind::Return,
                       .value =
                           lang::MirOperand{.kind = lang::MirOperandKind::Loan,
                                            .loan = 1,
                                            .type = reference},
                       .returnLoan = 1},
                  .reachable = true},
                 {.id = 3,
                  .instructions = {{.id = 2,
                                    .kind = lang::MirInstructionKind::EndBorrow,
                                    .loan = 1}},
                  .terminator =
                      {.kind = lang::MirTerminatorKind::Return,
                       .value =
                           lang::MirOperand{.kind = lang::MirOperandKind::Loan,
                                            .loan = 2,
                                            .type = reference},
                       .returnLoan = 2},
                  .reachable = true}},
      .places = {{.id = 1,
                  .root = lang::MirPlaceRootKind::Binding,
                  .binding = 101,
                  .type = lang::SemanticType::Int32,
                  .traits = lang::semanticTraits(lang::SemanticType::Int32)},
                 {.id = 2,
                  .root = lang::MirPlaceRootKind::Binding,
                  .binding = 102,
                  .type = lang::SemanticType::Int32,
                  .traits = lang::semanticTraits(lang::SemanticType::Int32)}},
      .loans = {{.id = 1,
                 .kind = lang::MirLoanKind::Return,
                 .source = 1,
                 .carriers = {101},
                 .entry = true,
                 .escapes = true},
                {.id = 2,
                 .kind = lang::MirLoanKind::Return,
                 .source = 2,
                 .carriers = {102},
                 .entry = true,
                 .escapes = true}}};

  expect(lang::verifyMirBody(body).valid(),
         "two return paths should each end the other active entry loan before "
         "returning their selected dependency");

  lang::MirBody wrongReturnPath = body;
  wrongReturnPath.blocks[1].instructions.clear();
  const lang::MirVerificationResult invalid =
      lang::verifyMirBody(wrongReturnPath);
  expect(!invalid.valid() &&
             std::any_of(invalid.errors.begin(), invalid.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find(
                                      "return edge that does not return it") !=
                                  std::string::npos;
                         }),
         "an escaping flag set by another return path must not hide an active "
         "loan that this return edge does not return");
}

void testCallableBoundaryMirMetadata() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("callable-boundary-mir.gti", R"(
void apply<T, First, Second>(mut T& value, First first, Second second) {
  first(value);
  second(value);
}

int main() {
  mut int value = 0;
  auto increment = [](mut int& target) -> void { target++; };
  apply(value, increment, increment);
  return value - 2;
}
)");
  expect(frontend.canGenerateCode(),
         "the callable-boundary MIR fixture should reach lowering");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirFunctionInstance *apply = findHirFunction(frontend, "apply");
  const lang::HirFunctionInstance *main = findHirFunction(frontend, "main");
  const lang::MirFunctionInstance *mirApply =
      apply == nullptr ? nullptr : frontend.mir.findFunctionInstance(apply->id);
  const lang::MirFunctionInstance *mirMain =
      main == nullptr ? nullptr : frontend.mir.findFunctionInstance(main->id);
  expect(apply != nullptr && main != nullptr && mirApply != nullptr &&
             mirMain != nullptr &&
             lang::verifyMirBody(mirApply->body).valid() &&
             lang::verifyMirBody(mirMain->body).valid(),
         "frontend-produced callable boundary MIR should verify");
  if (apply == nullptr || main == nullptr || mirApply == nullptr ||
      mirMain == nullptr) {
    return;
  }

  const auto outerCall = [&](lang::MirBody &body) -> lang::MirInstruction * {
    for (lang::MirBlock &block : body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == apply->id;
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return nullptr;
  };
  const auto confinedInvocation =
      [](lang::MirBody &body) -> lang::MirInstruction * {
    for (lang::MirBlock &block : body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.callableBoundary ==
                       lang::CallableBoundary::Confined;
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return nullptr;
  };

  lang::MirBody validMain = mirMain->body;
  lang::MirBody validApply = mirApply->body;
  lang::MirInstruction *validOuter = outerCall(validMain);
  lang::MirInstruction *validInvocation = confinedInvocation(validApply);
  expect(validOuter != nullptr && validOuter->callableArguments.size() == 2 &&
             validOuter->callableArguments[0].parameterIndex == 1 &&
             validOuter->callableArguments[1].parameterIndex == 2 &&
             validInvocation != nullptr &&
             validInvocation->callableInvocation ==
                 lang::CallableInvocationCapability::Read,
         "MIR should retain ordered confined descriptors at the outer call "
         "and invocation sites");
  if (validOuter == nullptr || validOuter->callableArguments.size() != 2 ||
      validInvocation == nullptr) {
    return;
  }

  const auto mutableFunction =
      [](lang::MirProgram &program,
         lang::HirFunctionInstanceId id) -> lang::MirFunctionInstance * {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  };

  lang::MirBody outOfRange = mirMain->body;
  lang::MirInstruction *outOfRangeCall = outerCall(outOfRange);
  outOfRangeCall->callableArguments.back().parameterIndex =
      outOfRangeCall->operands.size();
  expect(!lang::verifyMirBody(outOfRange).valid(),
         "the MIR verifier should reject an out-of-range callable argument");

  lang::MirBody duplicate = mirMain->body;
  lang::MirInstruction *duplicateCall = outerCall(duplicate);
  duplicateCall->callableArguments.back() =
      duplicateCall->callableArguments.front();
  expect(!lang::verifyMirBody(duplicate).valid(),
         "the MIR verifier should reject duplicate callable descriptors");

  lang::MirBody unsorted = mirMain->body;
  lang::MirInstruction *unsortedCall = outerCall(unsorted);
  std::reverse(unsortedCall->callableArguments.begin(),
               unsortedCall->callableArguments.end());
  expect(!lang::verifyMirBody(unsorted).valid(),
         "the MIR verifier should reject unsorted callable descriptors");

  lang::MirProgram mismatchedOwnedArgument = frontend.mir;
  lang::MirFunctionInstance *ownedArgumentMain =
      mutableFunction(mismatchedOwnedArgument, main->id);
  lang::MirInstruction *ownedArgumentCall =
      ownedArgumentMain == nullptr ? nullptr
                                   : outerCall(ownedArgumentMain->body);
  if (ownedArgumentCall != nullptr) {
    ownedArgumentCall->callableArguments.front().boundary =
        lang::CallableBoundary::Owned;
  }
  expect(ownedArgumentCall != nullptr &&
             lang::verifyMirBody(ownedArgumentMain->body).valid() &&
             !lang::verifyMirProgram(mismatchedOwnedArgument).valid(),
         "an owned callable argument descriptor should be locally shaped "
         "but must match an exact owned target contract");

  lang::MirBody prematureOwnedInvocation = mirApply->body;
  lang::MirInstruction *ownedInvocation =
      confinedInvocation(prematureOwnedInvocation);
  ownedInvocation->callableBoundary = lang::CallableBoundary::Owned;
  expect(!lang::verifyMirBody(prematureOwnedInvocation).valid(),
         "the MIR verifier should reject owned callable invocation metadata "
         "until its lifecycle contract lands");

  lang::MirBody missingCapability = mirApply->body;
  lang::MirInstruction *missingCapabilityInvocation =
      confinedInvocation(missingCapability);
  missingCapabilityInvocation->callableInvocation.reset();
  expect(!lang::verifyMirBody(missingCapability).valid(),
         "a confined MIR invocation must retain its callable capability");

  lang::MirBody prematureOnce = mirApply->body;
  lang::MirInstruction *onceInvocation = confinedInvocation(prematureOnce);
  onceInvocation->callableInvocation = lang::CallableInvocationCapability::Once;
  expect(!lang::verifyMirBody(prematureOnce).valid(),
         "the MIR verifier should reject once-callable invocation without an "
         "exact consuming receiver move");

  lang::MirBody strayNonCallBoundary = mirMain->body;
  lang::MirInstruction *nonCallBoundary = nullptr;
  for (lang::MirBlock &block : strayNonCallBoundary.blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind != lang::MirInstructionKind::Call;
        });
    if (found != block.instructions.end()) {
      nonCallBoundary = &*found;
      break;
    }
  }
  if (nonCallBoundary != nullptr) {
    nonCallBoundary->callableBoundary = lang::CallableBoundary::Confined;
  }
  expect(nonCallBoundary != nullptr &&
             !lang::verifyMirBody(strayNonCallBoundary).valid(),
         "only call instructions may carry a callable boundary");

  lang::MirBody strayNonCallArguments = mirMain->body;
  lang::MirInstruction *nonCallArguments = nullptr;
  for (lang::MirBlock &block : strayNonCallArguments.blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind != lang::MirInstructionKind::Call;
        });
    if (found != block.instructions.end()) {
      nonCallArguments = &*found;
      break;
    }
  }
  if (nonCallArguments != nullptr) {
    nonCallArguments->callableArguments.push_back(
        {.parameterIndex = 0, .boundary = lang::CallableBoundary::Confined});
  }
  expect(nonCallArguments != nullptr &&
             !lang::verifyMirBody(strayNonCallArguments).valid(),
         "only call instructions may carry callable argument descriptors");

  lang::MirProgram missingTargetCapability = frontend.mir;
  lang::MirFunctionInstance *missingCapabilityApply =
      mutableFunction(missingTargetCapability, apply->id);
  lang::MirInstruction *missingTargetInvocation =
      missingCapabilityApply == nullptr
          ? nullptr
          : confinedInvocation(missingCapabilityApply->body);
  if (missingTargetInvocation != nullptr) {
    missingTargetInvocation->callableBoundary.reset();
    missingTargetInvocation->callableInvocation.reset();
  }
  expect(missingTargetInvocation != nullptr &&
             !lang::verifyMirProgram(missingTargetCapability).valid(),
         "a concrete lambda target must retain its selected invocation "
         "capability even if its confined marker is forged away");

  lang::MirProgram missingInvocationBoundary = frontend.mir;
  lang::MirFunctionInstance *missingBoundaryApply =
      mutableFunction(missingInvocationBoundary, apply->id);
  lang::MirInstruction *missingBoundaryInvocation =
      missingBoundaryApply == nullptr
          ? nullptr
          : confinedInvocation(missingBoundaryApply->body);
  if (missingBoundaryInvocation != nullptr) {
    missingBoundaryInvocation->callableBoundary.reset();
  }
  expect(missingBoundaryInvocation != nullptr &&
             lang::verifyMirBody(missingBoundaryApply->body).valid() &&
             !lang::verifyMirProgram(missingInvocationBoundary).valid(),
         "an invocation of an enclosing callable parameter must retain its "
         "confined boundary independently from target capability");

  lang::MirProgram mismatchedRequiredCapability = frontend.mir;
  lang::MirFunctionInstance *requiredApply =
      mutableFunction(mismatchedRequiredCapability, apply->id);
  if (requiredApply != nullptr && !requiredApply->callableParameters.empty() &&
      !requiredApply->callableParameters.front().signatures.empty()) {
    requiredApply->callableParameters.front()
        .signatures.front()
        .requiredCapability = lang::CallableInvocationCapability::Mutable;
  }
  expect(requiredApply != nullptr &&
             !lang::verifyMirProgram(mismatchedRequiredCapability).valid(),
         "a callable signature's required capability must match its formal "
         "parameter access");

  lang::MirProgram invalidLambdaTarget = frontend.mir;
  lang::MirFunctionInstance *lambdaTargetApply =
      mutableFunction(invalidLambdaTarget, apply->id);
  if (lambdaTargetApply != nullptr &&
      !lambdaTargetApply->callableParameters.empty() &&
      !lambdaTargetApply->callableParameters.front().signatures.empty()) {
    lambdaTargetApply->callableParameters.front()
        .signatures.front()
        .lambdaTarget = invalidLambdaTarget.lambdaInstances().size() + 1;
  }
  expect(lambdaTargetApply != nullptr &&
             !lang::verifyMirProgram(invalidLambdaTarget).valid(),
         "a callable signature must name an existing exact lambda target");

  lang::MirProgram mismatchedLambdaResult = frontend.mir;
  auto &mismatchedResultLambdas =
      const_cast<std::vector<lang::MirLambdaInstance> &>(
          mismatchedLambdaResult.lambdaInstances());
  if (!mismatchedResultLambdas.empty()) {
    mismatchedResultLambdas.front().returnType = lang::SemanticType::Int32;
  }
  expect(!mismatchedResultLambdas.empty() &&
             !lang::verifyMirProgram(mismatchedLambdaResult).valid(),
         "a callable signature must match its exact lambda result type");

  lang::MirProgram missingTargetDescriptors = frontend.mir;
  lang::MirFunctionInstance *missingDescriptorsMain =
      mutableFunction(missingTargetDescriptors, main->id);
  lang::MirInstruction *missingDescriptorsCall =
      missingDescriptorsMain == nullptr
          ? nullptr
          : outerCall(missingDescriptorsMain->body);
  if (missingDescriptorsCall != nullptr) {
    missingDescriptorsCall->callableArguments.clear();
  }
  expect(missingDescriptorsCall != nullptr &&
             !lang::verifyMirProgram(missingTargetDescriptors).valid(),
         "a call must carry the exact callable argument descriptors required "
         "by its concrete target");

  const lang::FrontendResult stateful =
      lang::Frontend().analyze("callable-capability-mir.gti", R"(
void run_stateful<Operation>(mut Operation operation) {
  operation();
}

class Stateful {
public:
  void operator()() mut {}
};

int main() {
  Stateful stateful = Stateful();
  run_stateful(stateful);
  return 0;
}
)");
  const lang::HirFunctionInstance *runStateful =
      findHirFunction(stateful, "run_stateful");
  const lang::MirFunctionInstance *mirRunStateful =
      runStateful == nullptr
          ? nullptr
          : stateful.mir.findFunctionInstance(runStateful->id);
  expect(stateful.canGenerateCode() && mirRunStateful != nullptr &&
             lang::verifyMirProgram(stateful.mir).valid(),
         "the mutable callable MIR fixture should retain a valid exact "
         "operator() target");
  if (runStateful != nullptr && mirRunStateful != nullptr) {
    lang::MirProgram forgedSelectedCapability = stateful.mir;
    lang::MirFunctionInstance *forgedRun =
        mutableFunction(forgedSelectedCapability, runStateful->id);
    lang::MirInstruction *forgedInvocation =
        forgedRun == nullptr ? nullptr : confinedInvocation(forgedRun->body);
    if (forgedInvocation != nullptr) {
      forgedInvocation->callableInvocation =
          lang::CallableInvocationCapability::Read;
    }
    expect(forgedInvocation != nullptr &&
               !lang::verifyMirBody(forgedRun->body).valid() &&
               !lang::verifyMirProgram(forgedSelectedCapability).valid(),
           "MIR verification must bind a call site's selected capability to "
           "its ordered receiver and concrete mutable operator() target");

    lang::MirProgram missingOperatorIdentity = stateful.mir;
    lang::MirFunctionInstance *identityRun =
        mutableFunction(missingOperatorIdentity, runStateful->id);
    lang::MirFunctionInstance *operatorTarget = nullptr;
    if (identityRun != nullptr && !identityRun->callableParameters.empty() &&
        !identityRun->callableParameters.front().signatures.empty()) {
      const auto target = identityRun->callableParameters.front()
                              .signatures.front()
                              .functionTarget;
      operatorTarget =
          target ? mutableFunction(missingOperatorIdentity, *target) : nullptr;
    }
    if (operatorTarget != nullptr) {
      operatorTarget->overloadedOperator.reset();
    }
    expect(operatorTarget != nullptr &&
               !lang::verifyMirProgram(missingOperatorIdentity).valid(),
           "a function-object capability must remain attached to an exact "
           "operator() target identity");
  }

  const lang::FrontendResult forwarding =
      lang::Frontend().analyze("callable-forwarding-mir.gti", R"(
void outer<Operation>(Operation operation) { inner(operation); }
void inner<Operation>(Operation operation) { operation(); }

int main() {
  auto operation = []() -> void {};
  outer(operation);
  return 0;
}
)");
  const lang::HirFunctionInstance *outer = findHirFunction(forwarding, "outer");
  const lang::MirFunctionInstance *mirOuter =
      outer == nullptr ? nullptr
                       : forwarding.mir.findFunctionInstance(outer->id);
  expect(forwarding.canGenerateCode() && mirOuter != nullptr &&
             !mirOuter->callableParameters.empty() &&
             mirOuter->callableParameters.front().forwardings.size() == 1 &&
             lang::verifyMirProgram(forwarding.mir).valid(),
         "the forwarding MIR fixture should retain one exact confined edge");
  if (outer != nullptr && mirOuter != nullptr &&
      !mirOuter->callableParameters.empty() &&
      !mirOuter->callableParameters.front().forwardings.empty()) {
    lang::MirProgram invalidForwardingTarget = forwarding.mir;
    lang::MirFunctionInstance *invalidTargetOuter =
        mutableFunction(invalidForwardingTarget, outer->id);
    invalidTargetOuter->callableParameters.front()
        .forwardings.front()
        .functionTarget.reset();
    expect(!lang::verifyMirProgram(invalidForwardingTarget).valid(),
           "a forwarding contract must retain its exact target identity");

    lang::MirProgram invalidForwardingParameter = forwarding.mir;
    lang::MirFunctionInstance *invalidParameterOuter =
        mutableFunction(invalidForwardingParameter, outer->id);
    invalidParameterOuter->callableParameters.front()
        .forwardings.front()
        .parameterIndex = std::numeric_limits<std::size_t>::max();
    expect(!lang::verifyMirProgram(invalidForwardingParameter).valid(),
           "a forwarding contract must name an in-range confined target "
           "parameter");

    lang::MirProgram duplicateForwarding = forwarding.mir;
    lang::MirFunctionInstance *duplicateOuter =
        mutableFunction(duplicateForwarding, outer->id);
    duplicateOuter->callableParameters.front().forwardings.push_back(
        duplicateOuter->callableParameters.front().forwardings.front());
    expect(!lang::verifyMirProgram(duplicateForwarding).valid(),
           "a forwarding contract must not duplicate one concrete edge");

    lang::MirProgram missingForwardingCallDescriptor = forwarding.mir;
    lang::MirFunctionInstance *missingEdgeOuter =
        mutableFunction(missingForwardingCallDescriptor, outer->id);
    bool clearedDescriptor = false;
    for (lang::MirBlock &block : missingEdgeOuter->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (!instruction.callableArguments.empty()) {
          instruction.callableArguments.clear();
          clearedDescriptor = true;
          break;
        }
      }
      if (clearedDescriptor) {
        break;
      }
    }
    expect(clearedDescriptor &&
               !lang::verifyMirProgram(missingForwardingCallDescriptor).valid(),
           "a forwarding record and its concrete call descriptor must remain "
           "consistent");

    lang::MirProgram wrongForwardingOperand = forwarding.mir;
    lang::MirFunctionInstance *wrongOperandOuter =
        mutableFunction(wrongForwardingOperand, outer->id);
    lang::MirInstruction *forwardingCall = nullptr;
    if (wrongOperandOuter != nullptr) {
      for (lang::MirBlock &block : wrongOperandOuter->body.blocks) {
        const auto found = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::Call &&
                     !instruction.callableArguments.empty();
            });
        if (found != block.instructions.end()) {
          forwardingCall = &*found;
          break;
        }
      }
    }
    if (forwardingCall != nullptr && !forwardingCall->operands.empty()) {
      const lang::SemanticType type = forwardingCall->operands.front().type;
      const lang::MirPlaceId forgedPlace =
          wrongOperandOuter->body.places.size() + 1;
      wrongOperandOuter->body.places.push_back(
          {.id = forgedPlace,
           .root = lang::MirPlaceRootKind::Binding,
           .binding = wrongOperandOuter->parameterBindings.front() + 1000,
           .type = type,
           .traits = lang::semanticTraits(type),
           .initiallyAvailable = true});
      forwardingCall->operands.front() = {.kind = lang::MirOperandKind::Copy,
                                          .place = forgedPlace,
                                          .type = type};
      (void)lang::rebuildMirValueUses(wrongOperandOuter->body);
    }
    expect(forwardingCall != nullptr &&
               lang::verifyMirBody(wrongOperandOuter->body).valid() &&
               !lang::verifyMirProgram(wrongForwardingOperand).valid(),
           "a forwarding summary must prove that the descriptor operand is "
           "the source callable parameter binding");
  }

  const lang::FrontendResult onceForwarding = lang::Frontend().analyze(
      "once-callable-forwarding-mir.gti", R"(
class Once {
public:
  Once() {}
  int operator()() && { return 1; }
};

int invoke_once<Operation>(Operation operation) {
  return std::move(operation)();
}

int relay_once<Operation>(Operation operation) {
  return invoke_once(std::move(operation));
}

int outer_once<Operation>(Operation operation) {
  return relay_once(std::move(operation));
}

int main() { return outer_once(Once()) - 1; }
)",
      {std::filesystem::path(__FILE__).parent_path().parent_path() /
       "stdlib/prelude.gti"});
  const lang::HirFunctionInstance *outerOnce =
      findHirFunction(onceForwarding, "outer_once");
  const lang::MirFunctionInstance *mirOuterOnce =
      outerOnce == nullptr
          ? nullptr
          : onceForwarding.mir.findFunctionInstance(outerOnce->id);
  expect(onceForwarding.canGenerateCode() && mirOuterOnce != nullptr &&
             !mirOuterOnce->callableParameters.empty() &&
             !mirOuterOnce->callableParameters.front().forwardings.empty() &&
             lang::verifyMirProgram(onceForwarding.mir).valid(),
         "the transitive once-forwarding MIR fixture should retain a valid "
         "ownership move into its confined target");

  lang::MirProgram copiedOnceForwarding = onceForwarding.mir;
  lang::MirFunctionInstance *copiedOuterOnce =
      outerOnce == nullptr
          ? nullptr
          : mutableFunction(copiedOnceForwarding, outerOnce->id);
  bool replacedOnceMoveWithLoad = false;
  if (copiedOuterOnce != nullptr) {
    const auto definitionFor = [&](lang::MirValueId valueId) {
      const lang::MirValue *value = copiedOuterOnce->body.findValue(valueId);
      if (value == nullptr) {
        return static_cast<lang::MirInstruction *>(nullptr);
      }
      for (lang::MirBlock &block : copiedOuterOnce->body.blocks) {
        if (block.id != value->definitionBlock) {
          continue;
        }
        const auto found =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const lang::MirInstruction &instruction) {
                           return instruction.id == value->definition;
                         });
        return found == block.instructions.end() ? nullptr : &*found;
      }
      return static_cast<lang::MirInstruction *>(nullptr);
    };
    for (lang::MirBlock &block : copiedOuterOnce->body.blocks) {
      for (lang::MirInstruction &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            call.callableArguments.empty() || call.operands.empty() ||
            call.operands.front().kind != lang::MirOperandKind::Value) {
          continue;
        }
        lang::MirValueId forwarded = call.operands.front().value;
        lang::MirInstruction *prepared = definitionFor(forwarded);
        if (prepared != nullptr &&
            prepared->kind == lang::MirInstructionKind::CallInput &&
            prepared->operands.size() == 1 &&
            prepared->operands.front().kind == lang::MirOperandKind::Value) {
          forwarded = prepared->operands.front().value;
        }
        for (lang::MirBlock &definitionBlock : copiedOuterOnce->body.blocks) {
          for (lang::MirInstruction &definition :
               definitionBlock.instructions) {
            if (definition.result != forwarded ||
                definition.kind != lang::MirInstructionKind::Move ||
                definition.operands.size() != 1) {
              continue;
            }
            definition.kind = lang::MirInstructionKind::Load;
            definition.operands.front().kind = lang::MirOperandKind::Copy;
            definition.intrinsic = lang::IntrinsicKind::None;
            definition.ownership.reset();
            definition.lifecycle.clear();
            replacedOnceMoveWithLoad = true;
          }
        }
      }
    }
    (void)lang::rebuildMirValueUses(copiedOuterOnce->body);
  }
  const lang::MirVerificationResult copiedOnceForwardingResult =
      lang::verifyMirProgram(copiedOnceForwarding);
  expect(replacedOnceMoveWithLoad && copiedOuterOnce != nullptr &&
             !lang::verifyMirBody(copiedOuterOnce->body).valid() &&
             !copiedOnceForwardingResult.valid(),
         "class call-input verification must reject a copied callable before "
         "it can forge a transitive once-confined forwarding edge");

  const lang::FrontendResult linearOnce = lang::Frontend().analyze(
      "linear-once-callable-mir.gti", R"(
class Once {
public:
  Once() {}
  void operator()() && {}
};

void invoke_once<Operation>(Operation operation) {
  std::move(operation)();
}

int main() {
  invoke_once(Once());
  return 0;
}
)",
      {std::filesystem::path(__FILE__).parent_path().parent_path() /
       "stdlib/prelude.gti"});
  const lang::HirFunctionInstance *linearInvoke =
      findHirFunction(linearOnce, "invoke_once");
  const lang::MirFunctionInstance *mirLinearInvoke =
      linearInvoke == nullptr
          ? nullptr
          : linearOnce.mir.findFunctionInstance(linearInvoke->id);
  expect(linearOnce.canGenerateCode() && mirLinearInvoke != nullptr &&
             lang::verifyMirProgram(linearOnce.mir).valid(),
         "a once-callable receiver should have one linear use of its exact "
         "ownership move");

  const auto onceReceiverInput = [](lang::MirBody &body) {
    for (lang::MirBlock &block : body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            instruction.callableInvocation !=
                lang::CallableInvocationCapability::Once ||
            !instruction.receiver ||
            instruction.receiver->kind != lang::MirOperandKind::Value) {
          continue;
        }
        const lang::MirValue *receiver =
            body.findValue(instruction.receiver->value);
        if (receiver == nullptr) {
          continue;
        }
        lang::MirBlock *definitionBlock = nullptr;
        for (lang::MirBlock &candidate : body.blocks) {
          if (candidate.id == receiver->definitionBlock) {
            definitionBlock = &candidate;
            break;
          }
        }
        if (definitionBlock == nullptr) {
          continue;
        }
        const auto found =
            std::find_if(definitionBlock->instructions.begin(),
                         definitionBlock->instructions.end(),
                         [&](const lang::MirInstruction &candidate) {
                           return candidate.id == receiver->definition;
                         });
        if (found != definitionBlock->instructions.end() &&
            found->kind == lang::MirInstructionKind::CallInput &&
            found->callInputRole == lang::MirCallInputRole::Receiver) {
          return &*found;
        }
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };

  lang::MirBody orderedOnce =
      mirLinearInvoke == nullptr ? lang::MirBody{} : mirLinearInvoke->body;
  lang::MirInstruction *orderedOnceInput = onceReceiverInput(orderedOnce);
  expect(orderedOnceInput != nullptr &&
             orderedOnceInput->callInputKind ==
                 lang::HirCallInputKind::MoveValue &&
             lang::verifyMirBody(orderedOnce).valid(),
         "a concrete consuming operator should use an ordered move-value "
         "receiver checkpoint");

  lang::MirBody forgedOnce = orderedOnce;
  lang::MirInstruction *forgedOnceInput = onceReceiverInput(forgedOnce);
  if (forgedOnceInput != nullptr) {
    forgedOnceInput->callInputKind = lang::HirCallInputKind::Value;
    forgedOnceInput->lifecycle.clear();
  }
  expect(forgedOnceInput != nullptr && !lang::verifyMirBody(forgedOnce).valid(),
         "the MIR verifier should reject a consuming operator receiver "
         "relabeled as a non-consuming value");

  lang::MirProgram duplicatedOnceInvocation = linearOnce.mir;
  lang::MirFunctionInstance *duplicatedLinearInvoke =
      linearInvoke == nullptr
          ? nullptr
          : mutableFunction(duplicatedOnceInvocation, linearInvoke->id);
  bool clonedOnceCall = false;
  if (duplicatedLinearInvoke != nullptr) {
    lang::MirInstructionId nextInstruction = 1;
    for (const lang::MirBlock &block : duplicatedLinearInvoke->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        nextInstruction = std::max(nextInstruction, instruction.id + 1);
      }
    }
    for (lang::MirBlock &block : duplicatedLinearInvoke->body.blocks) {
      const auto call = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.callableInvocation ==
                       lang::CallableInvocationCapability::Once;
          });
      if (call == block.instructions.end()) {
        continue;
      }
      lang::MirInstruction duplicate = *call;
      duplicate.id = nextInstruction;
      block.instructions.insert(std::next(call), std::move(duplicate));
      clonedOnceCall = true;
      break;
    }
    (void)lang::rebuildMirValueUses(duplicatedLinearInvoke->body);
  }
  const lang::MirVerificationResult duplicatedOnceResult =
      lang::verifyMirProgram(duplicatedOnceInvocation);
  expect(
      clonedOnceCall && duplicatedLinearInvoke != nullptr &&
          !lang::verifyMirBody(duplicatedLinearInvoke->body).valid() &&
          !duplicatedOnceResult.valid() &&
          std::any_of(duplicatedOnceResult.errors.begin(),
                      duplicatedOnceResult.errors.end(),
                      [](const lang::MirVerificationError &error) {
                        return error.message.find(
                                   "once-callable invocation is not rooted in "
                                   "an exact ownership move") !=
                               std::string::npos;
                      }),
      "MIR verification must reject two consuming calls that reuse one moved "
      "receiver value");
}

void testOwnedCallableTransportMirMetadata() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "owned-callable-transport-mir.gti", R"(
class CallableOwner<T> {
  T value;

public:
  CallableOwner(T value) : value(std::move(value)) {}
};

T relay<T>(T value) { return std::move(value); }

CallableOwner<T> own<T>(T value) {
  return CallableOwner<T>(std::move(value));
}

int main() {
  int captured = 7;
  auto operation = [captured]() -> int { return captured; };
  auto returned = relay(std::move(operation));
  auto owner = own(std::move(returned));
  return 0;
}
)",
      {std::filesystem::path(__FILE__).parent_path().parent_path() /
       "stdlib/prelude.gti"});
  const lang::HirFunctionInstance *relay = findHirFunction(frontend, "relay");
  const lang::HirFunctionInstance *own = findHirFunction(frontend, "own");
  const lang::HirFunctionInstance *main = findHirFunction(frontend, "main");
  const lang::MirFunctionInstance *mirRelay =
      relay == nullptr ? nullptr : frontend.mir.findFunctionInstance(relay->id);
  const lang::MirFunctionInstance *mirOwn =
      own == nullptr ? nullptr : frontend.mir.findFunctionInstance(own->id);
  const lang::MirFunctionInstance *mirMain =
      main == nullptr ? nullptr : frontend.mir.findFunctionInstance(main->id);
  const lang::MirConstructorInstance *ownedConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (std::any_of(constructor.initializers.begin(),
                    constructor.initializers.end(),
                    [](const lang::MirConstructorInitializer &initializer) {
                      return initializer.ownedParameter.has_value();
                    })) {
      ownedConstructor = &constructor;
      break;
    }
  }
  expect(frontend.canGenerateCode() && relay != nullptr && own != nullptr &&
             main != nullptr && mirRelay != nullptr && mirOwn != nullptr &&
             mirMain != nullptr && ownedConstructor != nullptr &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "the owned callable transport MIR fixture should retain exact return "
         "and field proofs");
  if (!frontend.canGenerateCode() || relay == nullptr || own == nullptr ||
      main == nullptr || mirRelay == nullptr || mirOwn == nullptr ||
      mirMain == nullptr || ownedConstructor == nullptr) {
    return;
  }

  const auto mutableFunction =
      [](lang::MirProgram &program,
         lang::HirFunctionInstanceId id) -> lang::MirFunctionInstance * {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  };
  const auto mutableConstructor =
      [](lang::MirProgram &program,
         lang::HirConstructorInstanceId id) -> lang::MirConstructorInstance * {
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            program.constructorInstances());
    return id == 0 || id > constructors.size() ? nullptr
                                               : &constructors[id - 1];
  };
  const auto callTo =
      [](lang::MirBody &body,
         lang::HirFunctionInstanceId target) -> lang::MirInstruction * {
    for (lang::MirBlock &block : body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == target;
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return nullptr;
  };
  const auto replaceMoveWithCopy = [](lang::MirBody &body,
                                      lang::MirValueId value) {
    for (lang::MirBlock &block : body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.result != value ||
            instruction.kind != lang::MirInstructionKind::Move ||
            instruction.operands.size() != 1) {
          continue;
        }
        instruction.kind = lang::MirInstructionKind::Load;
        instruction.operands.front().kind = lang::MirOperandKind::Copy;
        instruction.intrinsic = lang::IntrinsicKind::None;
        instruction.ownership.reset();
        instruction.lifecycle.clear();
        (void)lang::rebuildMirValueUses(body);
        return true;
      }
    }
    return false;
  };

  lang::MirProgram copiedCall = frontend.mir;
  lang::MirFunctionInstance *copiedMain = mutableFunction(copiedCall, main->id);
  lang::MirInstruction *copiedRelayCall =
      copiedMain == nullptr ? nullptr : callTo(copiedMain->body, relay->id);
  const bool copiedArgument =
      copiedRelayCall != nullptr && !copiedRelayCall->operands.empty() &&
      copiedRelayCall->operands.front().kind == lang::MirOperandKind::Value &&
      replaceMoveWithCopy(copiedMain->body,
                          copiedRelayCall->operands.front().value);
  const lang::MirVerificationResult copiedCallResult =
      lang::verifyMirProgram(copiedCall);
  expect(copiedArgument && !copiedCallResult.valid() &&
             std::any_of(
                 copiedCallResult.errors.begin(), copiedCallResult.errors.end(),
                 [](const lang::MirVerificationError &error) {
                   return error.message.find(
                              "owned callable argument is not rooted in its "
                              "exact ownership move") != std::string::npos;
                 }),
         "program verification should reject a copied callable at an owned "
         "call boundary");

  lang::MirProgram copiedReturn = frontend.mir;
  lang::MirFunctionInstance *copiedRelay =
      mutableFunction(copiedReturn, relay->id);
  bool copiedReturnedValue = false;
  if (copiedRelay != nullptr) {
    for (const lang::MirBlock &block : copiedRelay->body.blocks) {
      if (block.reachable &&
          block.terminator.kind == lang::MirTerminatorKind::Return &&
          block.terminator.value &&
          block.terminator.value->kind == lang::MirOperandKind::Value) {
        copiedReturnedValue = replaceMoveWithCopy(
            copiedRelay->body, block.terminator.value->value);
        break;
      }
    }
  }
  const lang::MirVerificationResult copiedReturnResult =
      lang::verifyMirProgram(copiedReturn);
  expect(copiedReturnedValue && !copiedReturnResult.valid() &&
             std::any_of(
                 copiedReturnResult.errors.begin(),
                 copiedReturnResult.errors.end(),
                 [](const lang::MirVerificationError &error) {
                   return error.message.find(
                              "exact-return transport is not rooted in the "
                              "parameter move") != std::string::npos;
                 }),
         "an owned exact-return contract should reject a copied result even "
         "when the callable type is copyable");

  lang::MirProgram duplicatedReturnUse = frontend.mir;
  lang::MirFunctionInstance *duplicatedRelay =
      mutableFunction(duplicatedReturnUse, relay->id);
  bool duplicatedReturnedValue = false;
  if (duplicatedRelay != nullptr) {
    lang::MirInstructionId nextInstruction = 1;
    for (const lang::MirBlock &block : duplicatedRelay->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        nextInstruction = std::max(nextInstruction, instruction.id + 1);
      }
    }
    for (lang::MirBlock &block : duplicatedRelay->body.blocks) {
      if (!block.reachable ||
          block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.value ||
          block.terminator.value->kind != lang::MirOperandKind::Value) {
        continue;
      }
      block.instructions.push_back(
          {.id = nextInstruction,
           .kind = lang::MirInstructionKind::Call,
           .operands = {*block.terminator.value},
           .info = {.type = lang::SemanticType::Void,
                    .traits = lang::semanticTraits(lang::SemanticType::Void)}});
      duplicatedReturnedValue = true;
      break;
    }
    (void)lang::rebuildMirValueUses(duplicatedRelay->body);
  }
  const lang::MirVerificationResult duplicatedReturnResult =
      lang::verifyMirProgram(duplicatedReturnUse);
  expect(duplicatedReturnedValue && !duplicatedReturnResult.valid() &&
             std::any_of(
                 duplicatedReturnResult.errors.begin(),
                 duplicatedReturnResult.errors.end(),
                 [](const lang::MirVerificationError &error) {
                   return error.message.find(
                              "exact-return transport is not rooted in the "
                              "parameter move") != std::string::npos;
                 }),
         "an owned exact-return value should have one executable use at its "
         "return terminator");

  lang::MirProgram wrongField = frontend.mir;
  lang::MirFunctionInstance *wrongFieldOwn =
      mutableFunction(wrongField, own->id);
  if (wrongFieldOwn != nullptr && !wrongFieldOwn->callableParameters.empty() &&
      wrongFieldOwn->callableParameters.front().ownedTransport) {
    wrongFieldOwn->callableParameters.front().ownedTransport->field += 100000;
  }
  expect(wrongFieldOwn != nullptr &&
             !lang::verifyMirProgram(wrongField).valid(),
         "an owned exact-field contract should retain the exact destination "
         "field symbol");

  lang::MirProgram missingConstructorProof = frontend.mir;
  lang::MirConstructorInstance *missingProof =
      mutableConstructor(missingConstructorProof, ownedConstructor->id);
  if (missingProof != nullptr) {
    for (lang::MirConstructorInitializer &initializer :
         missingProof->initializers) {
      initializer.ownedParameter.reset();
    }
  }
  expect(missingProof != nullptr &&
             !lang::verifyMirProgram(missingConstructorProof).valid(),
         "an owned exact-field contract should require constructor "
         "parameter-to-field move evidence");
}

} // namespace

void testCrossAnalysisDeterminism() {
  // Two independent analyses allocate at different addresses; identical
  // printed MIR proves no observable output depends on iteration order of
  // address-keyed containers. Generic instances stress instance ordering.
  const std::string source = R"(
class Box<T> {
  T value;

public:
  Box(T value) : value(value) {}

  T& get() {
    return this.value;
  }
};

T relay<T>(T value) {
  return value;
}

int main() {
  Box<int32_t> first{1};
  Box<uint8_t> second{uint8_t(2)};
  int32_t left = relay(first.get());
  uint8_t right = relay(second.get());
  return left - 1 + int32_t(right) - 2;
}
)";
  const lang::FrontendResult first =
      lang::Frontend().analyze("determinism.gti", source);
  const lang::FrontendResult second =
      lang::Frontend().analyze("determinism.gti", source);
  expect(first.canGenerateCode() && second.canGenerateCode(),
         "the determinism fixture should reach code generation");
  expect(lang::MirPrinter().print(first.mir) ==
             lang::MirPrinter().print(second.mir),
         "independent analyses of one source must print identical MIR");
}

int main() {
  lang::installCrashHandlers("gti_optimizer_tests");
  testCrossAnalysisDeterminism();
  testCheckedIntegerContract();
  testMirIntegrityAndIdentityPipeline();
  testOrderedCallInputVerification();
  testOrderedClassValueCallInputVerification();
  testMirLiteralIdentityFoldAndEditor();
  testMirDominanceAndValueAvailability();
  testMirEffectClassification();
  testDefinedFailureIdentityAndPropagation();
  testSynchronizationMirContract();
  testExclusiveReborrowMirFlow();
  testTransientBorrowNormalization();
  testReturnEdgeLoanIdentity();
  testCallableBoundaryMirMetadata();
  testOwnedCallableTransportMirMetadata();

  if (failures != 0) {
    std::cerr << failures << " optimizer test(s) failed\n";
    return 1;
  }
  std::cout << "All optimizer tests passed\n";
  return 0;
}
