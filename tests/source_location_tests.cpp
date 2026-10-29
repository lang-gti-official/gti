#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string source) {
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(
      "source-location-contract.gti", std::move(source),
      {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

struct InstructionAddress {
  lang::HirFunctionInstanceId function = 0;
  lang::MirBlockId block = 0;
  lang::MirInstructionId instruction = 0;
};

std::optional<InstructionAddress>
findSourceLocationInstruction(const lang::MirProgram &program) {
  for (const lang::MirFunctionInstance &function :
       program.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::SourceLine) {
          return InstructionAddress{.function = function.id,
                                    .block = block.id,
                                    .instruction = instruction.id};
        }
      }
    }
  }
  return std::nullopt;
}

lang::MirInstruction *findInstruction(lang::MirProgram &program,
                                      InstructionAddress address) {
  auto *function = const_cast<lang::MirFunctionInstance *>(
      program.findFunctionInstance(address.function));
  if (function == nullptr) {
    return nullptr;
  }
  lang::MirBlock *block = function->body.findBlock(address.block);
  if (block == nullptr) {
    return nullptr;
  }
  for (lang::MirInstruction &instruction : block->instructions) {
    if (instruction.id == address.instruction) {
      return &instruction;
    }
  }
  return nullptr;
}

void testSourceLocationMirContract() {
  const lang::FrontendResult frontend = analyze(R"gti(
import std.source_location;
int main() {
  std::source_location location = std::source_location::current();
  return int32_t(location.line());
}
)gti");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "source-location calls should lower to verified MIR");
  const auto address = findSourceLocationInstruction(frontend.mir);
  expect(address.has_value(),
         "source-location fixture should retain a source-line intrinsic");
  if (!address) {
    return;
  }

  for (int mutation = 0; mutation != 6; ++mutation) {
    lang::MirProgram malformed = frontend.mir;
    lang::MirInstruction *instruction = findInstruction(malformed, *address);
    expect(instruction != nullptr,
           "source-location mutation should find the selected instruction");
    if (instruction == nullptr) {
      continue;
    }
    switch (mutation) {
    case 0:
      instruction->sourceLocation.reset();
      break;
    case 1:
      instruction->kind = lang::MirInstructionKind::Compute;
      break;
    case 2:
      instruction->info.type = lang::SemanticType::Bool;
      break;
    case 3:
      instruction->intrinsic = lang::IntrinsicKind::None;
      break;
    case 4:
      instruction->operands.push_back(
          {.kind = lang::MirOperandKind::Constant,
           .literal = lang::Literal{std::uint64_t{1}},
           .type = lang::SemanticType::UInt32});
      instruction->parameterTypes.push_back(lang::SemanticType::UInt32);
      break;
    case 5:
      instruction->synchronization.kind =
          lang::SynchronizationOperationKind::ThreadSpawn;
      break;
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "MIR should reject missing metadata, wrong instruction/result "
           "shapes, and forged source-location effects");
  }
}

void testConstructorCallerContexts() {
  const lang::FrontendResult frontend =
      analyze(R"gti(import std.source_location;
class Base {
  uint32_t line_value;
public:
  Base(std::source_location where = std::source_location::current())
      : line_value(where.line()) {}
};
class Explicit : public Base {
public:
  Explicit() : Base() {}
};
class Implicit : public Base {
public:
  Implicit() {}
};
int main() { Explicit first{}; Implicit second{}; return 0; }
)gti");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "explicit and implicit base defaults should lower to verified MIR");
  bool explicitBase = false;
  bool implicitBase = false;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    for (const lang::MirBlock &block : constructor.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::SourceLine ||
            !instruction.sourceLocation) {
          continue;
        }
        const lang::SourceLocationLiteral &location =
            *instruction.sourceLocation;
        explicitBase =
            explicitBase ||
            (location.line == 10 && location.function == "Explicit::Explicit");
        implicitBase =
            implicitBase ||
            (location.line == 14 && location.function == "Implicit::Implicit");
      }
    }
  }
  expect(explicitBase,
         "an explicit base default should capture its initializer call site");
  expect(implicitBase,
         "an implicit base default should capture the derived constructor "
         "site");
}

void testFieldInitializerCallerContext() {
  const lang::FrontendResult frontend =
      analyze(R"gti(import std.source_location;
class Record {
public:
  Record(std::source_location where = std::source_location::current()) {}
};
class FieldHolder {
  Record where;
public:
  FieldHolder() : where(Record()) {}
};
int main() { FieldHolder value{}; return 0; }
)gti");
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "a constructor call in a field initializer should reach verified "
         "MIR even when its owning native layout is not yet backend-admitted");
  bool found = false;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    for (const lang::MirBlock &block : constructor.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        found = found ||
                (instruction.intrinsic == lang::IntrinsicKind::SourceLine &&
                 instruction.sourceLocation &&
                 instruction.sourceLocation->line == 9 &&
                 instruction.sourceLocation->function ==
                     "FieldHolder::FieldHolder");
      }
    }
  }
  expect(found,
         "a nested field construction default should retain its written call "
         "site");
}

} // namespace

int main() {
  testSourceLocationMirContract();
  testConstructorCallerContexts();
  testFieldInitializerCallerContext();
  return failures == 0 ? 0 : 1;
}
