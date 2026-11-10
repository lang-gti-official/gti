// RFC 0004 erasure-gate normalizer: it erases loan bookkeeping (semantic loan
// numbers and EndBorrow placement, with the instruction-ID shifts EndBorrow
// placement causes) and nothing else, and it fails closed.
#include "../src/compiler/mir_loan_normalization.h"
#include "compiler_test_fixture.h"

#include <functional>
#include <limits>
#include <optional>

namespace {

constexpr std::string_view source = R"(
struct Counter {
public:
  Counter() = default;
  ~Counter() {}
  mut int value = 0;
  void bump() mut { this.value += 1; }
};

int read(Counter& counter) { return counter.value; }
void increment(mut Counter& counter) { counter.bump(); }
Counter& same(Counter& counter) { return counter; }

int main() {
  mut Counter first = Counter();
  Counter second = Counter();
  increment(first);
  Counter& view = second;
  Counter& again = same(view);
  return read(again) + read(first) + 40;
}
)";

lang::MirProgram lowered() {
  const std::filesystem::path entry =
      std::filesystem::temp_directory_path() / "gti-mir-loan-normalization.gti";
  const std::string text(source);
  auto result = lang::Frontend().analyze(
      entry, text, {standardLibraryPrelude()},
      {{std::filesystem::weakly_canonical(entry).string(), text}},
      {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
  }
  expect(result.canGenerateCode(), "the normalization fixture lowers to MIR");
  return std::move(result.mir);
}

std::string print(const lang::MirProgram &program) {
  return lang::MirPrinter().print(program);
}

std::string normalized(const lang::MirProgram &program) {
  return print(lang::normalizeLoanBookkeeping(program));
}

std::string dependencyNormalized(const lang::MirProgram &program) {
  return print(lang::normalizeBorrowDependencies(
      lang::normalizeLoanBookkeeping(program)));
}

bool isEndBorrow(const lang::MirInstruction &instruction) {
  return instruction.kind == lang::MirInstructionKind::EndBorrow &&
         !instruction.tombstone;
}

// Visits every body until the visitor returns true; reports whether one did.
bool anyBody(lang::MirProgram &program,
             const std::function<bool(lang::MirBody &)> &visit) {
  for (const auto address : lang::enumerateMirBodyAddresses(program))
    if (auto *body = lang::findMirBody(program, address); body && visit(*body))
      return true;
  return false;
}

lang::MirInstructionId largestId(const lang::MirBody &body) {
  lang::MirInstructionId largest = 0;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      largest = std::max(largest, instruction.id);
  return largest;
}

bool throwsNormalizationError(const lang::MirProgram &program) {
  try {
    (void)lang::normalizeLoanBookkeeping(program);
  } catch (const lang::MirLoanNormalizationError &) {
    return true;
  }
  return false;
}

// (i) Programs that differ only in EndBorrow position (and the instruction
// IDs a relocated EndBorrow takes) and in semantic loan numbers normalize to
// the same text.
void loanBookkeepingNormalizesAway() {
  const lang::MirProgram reference = lowered();
  lang::MirProgram candidate = reference;
  std::size_t semanticLoans = 0;
  const bool moved = anyBody(candidate, [&](lang::MirBody &body) {
    for (auto &block : body.blocks) {
      const auto end = std::find_if(block.instructions.begin(),
                                    block.instructions.end(), isEndBorrow);
      if (end == block.instructions.end() || block.instructions.size() < 3)
        continue;
      // Relocate the loan end to the other end of its block under a fresh
      // ID, as a liveness-derived end inserted after lowering would be.
      lang::MirInstruction relocated = *end;
      relocated.id = largestId(body) + 1;
      const bool atFront = end == block.instructions.begin();
      block.instructions.erase(end);
      block.instructions.insert(atFront ? block.instructions.end()
                                        : block.instructions.begin(),
                                std::move(relocated));
      return true;
    }
    return false;
  });
  expect(moved, "the fixture has an EndBorrow to relocate");
  anyBody(candidate, [&](lang::MirBody &body) {
    for (auto &loan : body.loans)
      loan.semanticLoan += 1000 + ++semanticLoans;
    return false;
  });
  expect(semanticLoans != 0, "the fixture has loans");
  expect(print(reference) != print(candidate),
         "the relocated EndBorrow and renumbered loans differ before "
         "normalization");
  expect(normalized(reference) == normalized(candidate),
         "EndBorrow placement and semantic loan numbers normalize away");
  const std::string text = normalized(reference);
  expect(text.find("end-borrow") == std::string::npos,
         "normalized MIR has no EndBorrow");
  expect(text.find(" semantic=0 ") != std::string::npos &&
             text.find(" semantic=1") == std::string::npos,
         "normalized loan rows carry semantic loan zero");
}

// (ii) A Drop moved across another non-loan instruction stays a difference.
void movedDropStillDiffers() {
  const lang::MirProgram reference = lowered();
  lang::MirProgram candidate = reference;
  const bool moved = anyBody(candidate, [](lang::MirBody &body) {
    for (auto &block : body.blocks) {
      auto &instructions = block.instructions;
      for (std::size_t index = 0; index < instructions.size(); ++index) {
        if (instructions[index].kind != lang::MirInstructionKind::Drop)
          continue;
        // The nearest earlier instruction that is not a loan end.
        for (std::size_t earlier = index; earlier-- > 0;) {
          if (isEndBorrow(instructions[earlier]))
            continue;
          std::swap(instructions[earlier], instructions[index]);
          return true;
        }
      }
    }
    return false;
  });
  expect(moved, "the fixture has a Drop after another non-loan instruction");
  expect(normalized(reference) != normalized(candidate),
         "a moved Drop survives loan normalization");
}

// (iii) A changed value or call stays a difference.
void changedNonLoanFieldStillDiffers() {
  const lang::MirProgram reference = lowered();
  lang::MirProgram literal = reference;
  const bool changedLiteral = anyBody(literal, [](lang::MirBody &body) {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.literal &&
            *instruction.literal == lang::Literal(std::uint64_t{40})) {
          *instruction.literal = lang::Literal(std::uint64_t{41});
          return true;
        }
    return false;
  });
  expect(changedLiteral, "the fixture has the integer literal 40");
  expect(normalized(reference) != normalized(literal),
         "a changed literal survives loan normalization");

  lang::MirProgram call = reference;
  const bool changedCall = anyBody(call, [](lang::MirBody &body) {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget) {
          *instruction.functionTarget += 1;
          return true;
        }
    return false;
  });
  expect(changedCall, "the fixture has a direct call");
  expect(normalized(reference) != normalized(call),
         "a changed call target survives loan normalization");
}

const lang::MirInstruction *findInstruction(const lang::MirBody &body,
                                            lang::MirBlockId blockId,
                                            lang::MirInstructionId id) {
  const auto *block = body.findBlock(blockId);
  if (block == nullptr)
    return nullptr;
  for (const auto &instruction : block->instructions)
    if (instruction.id == id)
      return &instruction;
  return nullptr;
}

// (iv) Every instruction-ID reference names the same instruction after
// renumbering, including references into a relocated-EndBorrow body.
void instructionReferencesRemapConsistently() {
  lang::MirProgram program = lowered();
  // Shift IDs: move each body's first EndBorrow to the front of its block.
  anyBody(program, [](lang::MirBody &body) {
    for (auto &block : body.blocks) {
      const auto end = std::find_if(block.instructions.begin(),
                                    block.instructions.end(), isEndBorrow);
      if (end == block.instructions.end() || end == block.instructions.begin())
        continue;
      std::rotate(block.instructions.begin(), end, std::next(end));
      return false;
    }
    return false;
  });
  lang::MirProgram result = lang::normalizeLoanBookkeeping(program);
  std::size_t definitions = 0;
  std::size_t uses = 0;
  std::size_t invokes = 0;
  for (const auto address : lang::enumerateMirBodyAddresses(result)) {
    const auto *body = lang::findMirBody(result, address);
    lang::MirInstructionId expected = 0;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        expect(!isEndBorrow(instruction), "no EndBorrow survives");
        expect(instruction.id == ++expected,
               "instruction IDs are dense in block order");
      }
    for (const auto &value : body->values) {
      if (value.definition == 0)
        continue;
      const auto *producer =
          findInstruction(*body, value.definitionBlock, value.definition);
      expect(producer && producer->result == value.id,
             "a value definition names its producer");
      ++definitions;
    }
    for (std::size_t index = 0; index < body->valueUses.size(); ++index)
      for (const auto &use : body->valueUses[index]) {
        if (use.kind != lang::MirValueUseKind::InstructionOperand &&
            use.kind != lang::MirValueUseKind::InstructionReceiver)
          continue;
        const auto *user = findInstruction(*body, use.block, use.instruction);
        const bool names =
            user &&
            (use.kind == lang::MirValueUseKind::InstructionReceiver
                 ? user->receiver && user->receiver->value == index + 1
                 : use.operandIndex < user->operands.size() &&
                       user->operands[use.operandIndex].value == index + 1);
        expect(names, "a value use names its consuming instruction");
        ++uses;
      }
    for (const auto &block : body->blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Invoke)
        continue;
      const auto *invoked =
          findInstruction(*body, block.id, block.terminator.invokeInstruction);
      expect(invoked != nullptr && &*invoked == &block.instructions.back(),
             "an Invoke names its block's final instruction");
      ++invokes;
    }
    for (const auto &record : body->failureRecords) {
      if (record.kind != lang::MirFailureRecordKind::Producer)
        continue;
      const auto *producerBlock = body->findBlock(record.producerBlock);
      expect(producerBlock && producerBlock->terminator.invokeInstruction ==
                                  record.producerInstruction,
             "a failure record names its Invoke producer");
    }
  }
  expect(definitions != 0 && uses != 0 && invokes != 0,
         "the fixture exercises definitions, uses and invokes");
  if (const auto &plan = result.hostedStartupPlan(); plan) {
    const auto *body =
        lang::findMirBody(result, {.kind = lang::MirBodyKind::HostedStartup,
                                   .owner = plan->entry});
    for (const auto &operation : plan->operations)
      expect(operation.instruction == 0 ||
                 findInstruction(*body, operation.block,
                                 operation.instruction) != nullptr,
             "a hosted startup operation names an instruction of its block");
  }
}

// The normalizer refuses rather than hide a non-loan fact.
void normalizationFailsClosed() {
  const lang::MirProgram reference = lowered();
  expect(!throwsNormalizationError(reference),
         "ordinary lowered MIR normalizes");

  // An EndBorrow carrying a lifecycle event is not only a loan end.
  lang::MirProgram lifecycle = reference;
  expect(anyBody(lifecycle,
                 [](lang::MirBody &body) {
                   for (auto &block : body.blocks)
                     for (auto &instruction : block.instructions)
                       if (isEndBorrow(instruction)) {
                         instruction.lifecycle.push_back(
                             {.kind = lang::MirLifecycleEventKind::Drop,
                              .source = 1});
                         return true;
                       }
                   return false;
                 }),
         "the fixture has an EndBorrow");
  expect(throwsNormalizationError(lifecycle),
         "an EndBorrow with a lifecycle event is refused");

  // A non-loan field referring to an EndBorrow would leak its position.
  lang::MirProgram reference_to_end = reference;
  expect(anyBody(reference_to_end,
                 [](lang::MirBody &body) {
                   for (auto &block : body.blocks)
                     for (auto &instruction : block.instructions)
                       if (isEndBorrow(instruction) && !body.values.empty()) {
                         body.values.front().definition = instruction.id;
                         return true;
                       }
                   return false;
                 }),
         "the fixture has an EndBorrow beside a value");
  expect(throwsNormalizationError(reference_to_end),
         "a reference to a removed EndBorrow is refused");

  // An ID that names no instruction is refused, not passed through.
  lang::MirProgram unknown = reference;
  expect(anyBody(unknown,
                 [](lang::MirBody &body) {
                   if (body.values.empty())
                     return false;
                   body.values.front().definition = largestId(body) + 100;
                   return true;
                 }),
         "the fixture has a value");
  expect(throwsNormalizationError(unknown),
         "an instruction ID outside its body is refused");
}

// The dependency mode erases borrow-dependency metadata and nothing else.
void borrowDependenciesNormalizeAway() {
  const lang::MirProgram reference = lowered();
  lang::MirProgram candidate = reference;
  std::size_t calls = 0;
  std::size_t parents = 0;
  std::size_t borrows = 0;
  anyBody(candidate, [&](lang::MirBody &body) {
    for (auto &loan : body.loans) {
      if (loan.parent != 0)
        ++parents;
      loan.parent = loan.parent == 0 ? 1 : 0;
      loan.storedDependency += 1;
      loan.additionalDependencies.push_back({.source = 1});
      if (loan.kind == lang::MirLoanKind::CallResult)
        loan.source += 1;
    }
    for (auto &value : body.values)
      if (value.borrowDependencies)
        value.borrowDependencies->dependencies.push_back({.source = 1});
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Borrow)
          for (auto &operand : instruction.operands)
            if (operand.kind == lang::MirOperandKind::Loan &&
                body.loans.size() > 1) {
              operand.loan = operand.loan == 1 ? 2 : 1;
              ++borrows;
            }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.borrowOrigin != lang::BorrowOriginKind::None) {
          instruction.borrowArguments.push_back(instruction.borrowArgument);
          instruction.borrowFields.push_back(
              {.origin = lang::BorrowOriginKind::Receiver});
          instruction.borrowAccess = lang::AccessMode::Mutable;
          ++calls;
        }
      }
    return false;
  });
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           candidate.functionInstances()))
    if (function.returnBorrowOrigin != lang::BorrowOriginKind::None) {
      function.returnBorrowParameters.push_back(0);
      function.explicitReturnBorrowOrigin =
          !function.explicitReturnBorrowOrigin;
    }
  expect(calls != 0 && parents != 0 && borrows != 0,
         "the fixture has contract calls, parent loans and reborrows");
  expect(normalized(reference) != normalized(candidate),
         "dependency metadata differs after loan normalization alone");
  expect(dependencyNormalized(reference) == dependencyNormalized(candidate),
         "borrow-dependency metadata normalizes away");
}

// A call target, a moved Drop and a Loan operand's kind stay differences.
void dependencyModeKeepsOtherFacts() {
  const lang::MirProgram reference = lowered();
  lang::MirProgram call = reference;
  expect(anyBody(call,
                 [](lang::MirBody &body) {
                   for (auto &block : body.blocks)
                     for (auto &instruction : block.instructions)
                       if (instruction.kind == lang::MirInstructionKind::Call &&
                           instruction.borrowOrigin !=
                               lang::BorrowOriginKind::None &&
                           instruction.functionTarget) {
                         *instruction.functionTarget += 1;
                         return true;
                       }
                   return false;
                 }),
         "the fixture has a contract call");
  expect(dependencyNormalized(reference) != dependencyNormalized(call),
         "a changed call target survives dependency normalization");

  lang::MirProgram drop = reference;
  expect(anyBody(drop,
                 [](lang::MirBody &body) {
                   for (auto &block : body.blocks) {
                     auto &instructions = block.instructions;
                     for (std::size_t index = 1; index < instructions.size();
                          ++index)
                       if (instructions[index].kind ==
                               lang::MirInstructionKind::Drop &&
                           !isEndBorrow(instructions[index - 1])) {
                         std::swap(instructions[index - 1],
                                   instructions[index]);
                         return true;
                       }
                   }
                   return false;
                 }),
         "the fixture has a Drop to move");
  expect(dependencyNormalized(reference) != dependencyNormalized(drop),
         "a moved Drop survives dependency normalization");

  lang::MirProgram operand = reference;
  expect(anyBody(operand,
                 [](lang::MirBody &body) {
                   for (auto &block : body.blocks)
                     for (auto &instruction : block.instructions)
                       if (instruction.kind == lang::MirInstructionKind::Borrow)
                         for (auto &input : instruction.operands)
                           if (input.kind == lang::MirOperandKind::Loan) {
                             input.kind = lang::MirOperandKind::Address;
                             return true;
                           }
                   return false;
                 }),
         "the fixture has a reborrow operand");
  expect(dependencyNormalized(reference) != dependencyNormalized(operand),
         "a reborrow operand's kind survives dependency normalization");

  lang::MirProgram missing = reference;
  expect(anyBody(missing,
                 [](lang::MirBody &body) {
                   for (auto &block : body.blocks)
                     for (auto &instruction : block.instructions)
                       if (instruction.kind == lang::MirInstructionKind::Borrow)
                         for (auto &input : instruction.operands)
                           if (input.kind == lang::MirOperandKind::Loan) {
                             input.loan = body.loans.size() + 5;
                             return true;
                           }
                   return false;
                 }),
         "the fixture has a reborrow operand to break");
  bool refused = false;
  try {
    (void)lang::normalizeBorrowDependencies(missing);
  } catch (const lang::MirLoanNormalizationError &) {
    refused = true;
  }
  expect(refused, "a reborrow of an unknown loan is refused");
}

} // namespace

int main(int argc, char **argv) {
  static constexpr NamedTest tests[] = {
      {"loan_bookkeeping_normalizes_away", loanBookkeepingNormalizesAway},
      {"moved_drop_still_differs", movedDropStillDiffers},
      {"changed_non_loan_field_still_differs", changedNonLoanFieldStillDiffers},
      {"instruction_references_remap_consistently",
       instructionReferencesRemapConsistently},
      {"normalization_fails_closed", normalizationFailsClosed},
      {"borrow_dependencies_normalize_away", borrowDependenciesNormalizeAway},
      {"dependency_mode_keeps_other_facts", dependencyModeKeepsOtherFacts},
  };
  return runNamedTests(argc, argv, tests, "MIR loan normalization");
}
