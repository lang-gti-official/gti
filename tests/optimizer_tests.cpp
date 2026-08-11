#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimization/effects.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
             aliasLoan->carriers.size() == 1 &&
             std::find(aliasLoan->carriers.begin(), aliasLoan->carriers.end(),
                       aliasHir->parameterBindings.front()) !=
                 aliasLoan->carriers.end() &&
             aliasReturnLoan->id != aliasLoan->id &&
             aliasReturnLoan->source == aliasLoan->source &&
             aliasReturnLoan->carriers.size() == 1 &&
             lang::verifyMirBody(aliasMir->body).valid(),
         "a direct local reference alias should receive a distinct child loan "
         "rooted at the formal entry dependency");
  expect(branchAliasHir != nullptr && branchAliasMir != nullptr &&
             branchAliasLoan != nullptr && branchAliasReturnLoan != nullptr &&
             branchAliasReturnLoan->id != branchAliasLoan->id &&
             branchAliasReturnLoan->source == branchAliasLoan->source &&
             branchAliasReturnLoan->carriers.size() >= 2 &&
             std::find(branchAliasLoan->carriers.begin(),
                       branchAliasLoan->carriers.end(),
                       branchAliasHir->parameterBindings[1]) !=
                 branchAliasLoan->carriers.end() &&
             lang::verifyMirBody(branchAliasMir->body).valid(),
         "aliases of one local reference across a branch should share its "
         "child loan while retaining the formal entry source");

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
  expect(independentMir != nullptr && independentChildren.size() == 2 &&
             independentChildren[0]->semanticLoan != 0 &&
             independentChildren[1]->semanticLoan != 0 &&
             independentChildren[0]->semanticLoan !=
                 independentChildren[1]->semanticLoan &&
             independentChildren[0]->source == independentChildren[1]->source &&
             lang::verifyMirBody(independentMir->body).valid(),
         "independent aliases of one formal parameter should keep distinct "
         "semantic loans and lexical endpoints over the same owner source");

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
  expect(relayCall != nullptr && relayCall->loan &&
             relayCall->borrowOrigin == lang::BorrowOriginKind::Argument &&
             relayCall->borrowArgument == 0 &&
             relayCall->borrowAccess == lang::AccessMode::ReadOnly &&
             !relayCall->operands.empty() &&
             relayCall->operands.front().place != 0 && mainMir != nullptr &&
             mainMir->body.findLoan(*relayCall->loan) != nullptr &&
             mainMir->body.findLoan(*relayCall->loan)->source ==
                 relayCall->operands.front().place,
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
             optimized.report.passes.empty(),
         "the initial MIR pipeline should verify an identity snapshot without "
         "claiming transformations");
  expect(before == lang::MirPrinter().print(optimized.mir),
         "identity optimization should preserve the complete MIR snapshot");

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

} // namespace

int main() {
  testCheckedIntegerContract();
  testMirIntegrityAndIdentityPipeline();
  testMirEffectClassification();
  testReturnEdgeLoanIdentity();

  if (failures != 0) {
    std::cerr << failures << " optimizer test(s) failed\n";
    return 1;
  }
  std::cout << "All optimizer tests passed\n";
  return 0;
}
