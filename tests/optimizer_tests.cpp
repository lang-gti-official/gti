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
class Counter {
  int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  ~Counter() {}
  int32_t read() { return this.value; }
  int32_t& borrow() { return this.value; }
};

int32_t choose(bool condition) {
  if (condition) {
    return 1;
  }
  return 2;
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
  return choose(counter.read() == 1);
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
  const lang::MirBody *loopBorrow =
      findFunction(frontend, "borrow_in_loop_condition");
  const lang::MirBody *twoBorrows = findFunction(frontend, "two_borrows");
  const lang::MirBody *aliasedBorrow = findFunction(frontend, "aliased_borrow");
  expect(borrowed != nullptr && branched != nullptr && loopBorrow != nullptr &&
             twoBorrows != nullptr && aliasedBorrow != nullptr &&
             lang::verifyMirBody(*borrowed).valid() &&
             lang::verifyMirBody(*branched).valid() &&
             lang::verifyMirBody(*loopBorrow).valid() &&
             lang::verifyMirBody(*twoBorrows).valid() &&
             lang::verifyMirBody(*aliasedBorrow).valid(),
         "frontend MIR should balance lexical loans through straight-line and "
         "branching control flow and loop backedges");
  if (borrowed == nullptr || branched == nullptr || loopBorrow == nullptr ||
      twoBorrows == nullptr || aliasedBorrow == nullptr ||
      borrowed->loans.empty() || branched->loans.empty() ||
      loopBorrow->loans.empty() || twoBorrows->loans.size() < 2 ||
      aliasedBorrow->loans.empty()) {
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

} // namespace

int main() {
  testCheckedIntegerContract();
  testMirIntegrityAndIdentityPipeline();
  testMirEffectClassification();

  if (failures != 0) {
    std::cerr << failures << " optimizer test(s) failed\n";
    return 1;
  }
  std::cout << "All optimizer tests passed\n";
  return 0;
}
