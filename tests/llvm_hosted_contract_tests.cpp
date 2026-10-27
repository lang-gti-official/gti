#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/hosted.h"
#include "../src/compiler/llvm/runtime_abi.h"
#include "../src/compiler/mir_verification_queries.h"
#include "cpp_backend_test_support.h"
#include "gti/driver/artifact.h"
#include "gti/driver/native_toolchain.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lang {
struct MirProgramTestAccess {
  static auto &hostedPlan(MirProgram &program) {
    return program.hostedStartupPlan_;
  }
  static auto &functions(MirProgram &program) { return program.functions; }
};
} // namespace lang

namespace {
using namespace lang;
using namespace lang::native_llvm;

struct Case {
  std::string_view fixture;
  int trace;
  std::string_view failure;
  unsigned line;
};

constexpr std::array cases{
    Case{"llvm_hosted_scalar_return.gti", 321, "", 0},
    Case{"llvm_hosted_scalar_return_failure.gti", 321, "scalar return failure",
         13},
    Case{"llvm_hosted_union_first_field.gti", 12356, "", 0},
    Case{"llvm_hosted_union_first_field_aggregate.gti", 47, "", 0},
    Case{"llvm_hosted_union_first_field_failure.gti", 321,
         "union first-field initialization failure", 15},
    Case{"llvm_hosted_generated_default.gti", 33333, "", 0},
    Case{"llvm_hosted_generated_default_failure.gti", 321,
         "generated default argument failure", 17},
    Case{"llvm_hosted_success.gti", 12354, "", 0},
    Case{"llvm_hosted_startup_failure.gti", 321, "hosted startup failure", 16},
    Case{"llvm_hosted_entry_failure.gti", 321, "hosted entry failure", 16},
    Case{"llvm_hosted_reference_input_failure.gti", 321,
         "borrowed argument failure", 18},
    Case{"llvm_hosted_text_return_failure.gti", 321, "counted text failure",
         14},
    Case{"llvm_hosted_reference_return_failure.gti", 321,
         "reference return failure", 13},
};

void require(bool value, std::string_view message) {
  if (!value)
    throw std::runtime_error(std::string(message));
}

void unsafeReferenceLoadContract(const std::filesystem::path &repository,
                                 bool callInput = false) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      repository /
          (callInput ? "tests/fixtures/llvm_hosted_unsafe_reference_input.gti"
                     : "tests/fixtures/llvm_hosted_unsafe_reference_load.gti"),
      std::nullopt, {repository / "stdlib/prelude.gti"}, {},
      {repository / "stdlib"});
  require(frontend.canGenerateCode(), "unrelated unsafe helper is valid GTI");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program && planLlvmAdmission(*program).admitted(),
          "checked unsafe result materialization has structural admission");
  std::size_t owner = 0;
  MirBlockId blockId = 0;
  MirInstructionId loadId = 0;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == (callInput ? MirInstructionKind::CallInput
                                           : MirInstructionKind::Load) &&
            instruction.unsafeOperation ==
                UnsafeOperationKind::DeclaredUnsafeCall &&
            (!callInput ||
             (instruction.callInputKind == HirCallInputKind::ReadBorrow &&
              instruction.info.type.kind == SemanticType::Reference &&
              instruction.info.type.arguments.size() == 1 &&
              instruction.info.type.arguments.front() ==
                  SemanticType::Int32))) {
          require(owner == 0, "regression isolates exactly one selected result "
                              "materialization");
          owner = function.id;
          blockId = block.id;
          loadId = instruction.id;
        }
  require(owner != 0,
          "real producer retains its unsafe materialization marker");
  using Mutation =
      std::function<void(MirProgram &, MirBlock &, MirInstruction &,
                         MirInstruction &, MirPlace &, MirLoan &)>;
  std::vector<Mutation> mutations{
      [](auto &, auto &, auto &, auto &, auto &place, auto &) {
        place.loan = 0;
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &loan) {
        load.operands.front().place = loan.source;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &loan) {
        loan.kind = MirLoanKind::Local;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &loan) {
        loan.entry = true;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &loan) {
        ++loan.producedBy;
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &) {
        load.hirValue = 0;
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &) {
        load.rawMemoryAccess = true;
      },
      [](auto &, auto &, auto &, auto &, auto &place, auto &) {
        place.projections.push_back(
            {.kind = MirProjectionKind::RawDereference});
      },
      [](auto &, auto &, auto &load, auto &, auto &, auto &) {
        load.info.type = SemanticType::Int64;
      },
      [](auto &, auto &, auto &, auto &, auto &place, auto &loan) {
        place.access = loan.access = AccessMode::Mutable;
      },
      [](auto &mir, auto &, auto &, auto &call, auto &, auto &) {
        MirProgramTestAccess::functions(mir)[*call.functionTarget - 1]
            .requiresUnsafe = false;
      },
      [](auto &mir, auto &, auto &, auto &call, auto &, auto &) {
        MirProgramTestAccess::functions(mir)[*call.functionTarget - 1].linkage =
            LanguageLinkage::C;
      },
      [](auto &, auto &, auto &, auto &call, auto &, auto &) {
        call.intrinsic = IntrinsicKind::SourceLine;
      },
      [](auto &, auto &, auto &, auto &call, auto &, auto &) {
        call.dispatch = CallDispatch::Virtual;
      },
      [](auto &, auto &, auto &load, auto &call, auto &, auto &) {
        std::swap(load, call);
      },
      [](auto &, auto &block, auto &load, auto &, auto &, auto &loan) {
        const auto ended =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const auto &stage) {
                           return stage.kind == MirInstructionKind::EndBorrow &&
                                  stage.loan == loan.id;
                         });
        require(ended != block.instructions.end(),
                "result loan has an endpoint");
        std::swap(load, *ended);
      },
  };
  if (callInput) {
    mutations.push_back([](auto &, auto &, auto &input, auto &, auto &,
                           auto &) { input.operands.front().loan = 0; });
    mutations.push_back(
        [](auto &, auto &, auto &input, auto &, auto &, auto &) {
          input.callInputRole = MirCallInputRole::Receiver;
        });
    mutations.push_back(
        [](auto &, auto &, auto &input, auto &, auto &, auto &) {
          input.callInputKind = HirCallInputKind::CopyValue;
        });
    mutations.push_back([](auto &, auto &, auto &input, auto &, auto &,
                           auto &) { input.callSite = 0; });
    mutations.push_back([](auto &, auto &, auto &input, auto &, auto &,
                           auto &) { input.defaultArgument = true; });
    mutations.push_back(
        [](auto &, auto &, auto &input, auto &, auto &, auto &) {
          input.info.type.referenceAccess = AccessMode::Mutable;
        });
    mutations.push_back(
        [](auto &, auto &, auto &input, auto &, auto &, auto &) {
          input.operands.front().type = SemanticType::Int64;
        });
    mutations.push_back([](auto &, auto &, auto &input, auto &, auto &,
                           auto &) { input.result.reset(); });
    mutations.push_back([](auto &, auto &, auto &, auto &call, auto &, auto &) {
      call.tombstone = true;
    });
    mutations.push_back([](auto &, auto &, auto &, auto &call, auto &, auto &) {
      call.unsafeOperation = UnsafeOperationKind::RawDereference;
    });
  }
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    auto invalid = frontend.mir;
    auto &body = MirProgramTestAccess::functions(invalid)[owner - 1].body;
    auto &block = body.blocks[blockId - 1];
    auto &load =
        *std::find_if(block.instructions.begin(), block.instructions.end(),
                      [&](const auto &stage) { return stage.id == loadId; });
    auto &place = body.places[load.operands.front().place - 1];
    auto &loan = body.loans[place.loan - 1];
    auto &call =
        *std::find_if(block.instructions.begin(), block.instructions.end(),
                      [&](const auto &stage) {
                        return stage.kind == MirInstructionKind::Call &&
                               stage.loan == loan.id;
                      });
    mutations[index](invalid, block, load, call, place, loan);
    auto built = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, invalid, invalid,
        {}, TargetInfo::host());
    bool rejected = !built.valid();
    if (built.valid()) {
      auto verified = verifiedLoweredProgram(std::move(*built.program), issues);
      rejected = !verified || !planLlvmAdmission(*verified).admitted();
    }
    require(rejected,
            "forged unsafe call-result relationship cannot be admitted");
  }
}

void unsafeReferenceReturnContract(
    const std::filesystem::path &repository, bool countedText = false,
    bool rawText = false,
    const SemanticType &borrowedType = SemanticType::StringView,
    const char *fixture = nullptr, bool fallibleReference = false) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      repository /
          (fixture ? fixture
           : countedText
               ? (borrowedType == SemanticType::CString
                      ? "tests/fixtures/llvm_hosted_unsafe_cstring_return.gti"
                      : "tests/fixtures/llvm_hosted_unsafe_text_return.gti")
               : "tests/fixtures/llvm_hosted_unsafe_reference_return.gti"),
      std::nullopt, {repository / "stdlib/prelude.gti"}, {},
      {repository / "stdlib"});
  require(frontend.canGenerateCode(), "unsafe reference relay is valid GTI");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program && planLlvmAdmission(*program).admitted(),
          "unsafe reference relay preserves its exact checked runtime loan");
  std::size_t owner = 0;
  MirBlockId blockId = 0;
  MirInstructionId transferId = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (countedText && !rawText && function.requiresUnsafe)
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions)
        if (stage.kind == MirInstructionKind::Borrow &&
            stage.unsafeOperation ==
                (rawText ? UnsafeOperationKind::RawDereference
                         : UnsafeOperationKind::DeclaredUnsafeCall) &&
            (!countedText || stage.info.type == borrowedType)) {
          require(owner == 0, "relay isolates one repeated unsafe marker");
          owner = function.id;
          blockId = block.id;
          transferId = stage.id;
        }
  }
  require(owner != 0, "relay has a genuine checked return transfer");
  using Mutation =
      std::function<void(MirProgram &, MirBody &, MirBlock &, MirInstruction &,
                         MirInstruction &, MirLoan &, MirLoan &)>;
  std::vector<Mutation> mutations{
      [](auto &, auto &, auto &, auto &, auto &, auto &, auto &to) {
        to.source = 0;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &from, auto &to) {
        to.additionalDependencies.push_back(
            {.source = from.source, .access = AccessMode::Mutable});
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &, auto &to) {
        to.escapes = false;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &from, auto &to) {
        to.parent = from.id;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &from, auto &to) {
        to.access = from.access == AccessMode::ReadOnly ? AccessMode::Mutable
                                                        : AccessMode::ReadOnly;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &, auto &to) {
        to.producedBy = 0;
      },
      [](auto &, auto &, auto &, auto &, auto &, auto &, auto &to) {
        to.designatedFields = {1};
      },
      [](auto &, auto &, auto &, auto &stage, auto &, auto &from, auto &) {
        stage.operands.front().place = from.source;
      },
      [](auto &, auto &, auto &, auto &stage, auto &, auto &, auto &) {
        stage.operands.front().loan = 0;
      },
      [](auto &, auto &, auto &block, auto &, auto &, auto &from, auto &) {
        block.terminator.returnLoan = from.id;
      },
      [](auto &, auto &, auto &block, auto &, auto &, auto &from, auto &) {
        block.terminator.value->loan = from.id;
      },
      [rawText](auto &, auto &, auto &, auto &, auto &call, auto &, auto &) {
        if (rawText)
          call.intrinsic = IntrinsicKind::None;
        else
          call.functionTarget.reset();
      },
      [](auto &, auto &, auto &, auto &stage, auto &, auto &, auto &) {
        stage.rawMemoryAccess = true;
      },
      [rawText](auto &, auto &, auto &, auto &stage, auto &, auto &, auto &) {
        stage.unsafeOperation = rawText
                                    ? UnsafeOperationKind::DeclaredUnsafeCall
                                    : UnsafeOperationKind::RawDereference;
      },
      [](auto &, auto &, auto &, auto &stage, auto &, auto &, auto &) {
        stage.info.type = SemanticType::Char;
      },
      [](auto &, auto &, auto &, auto &, auto &call, auto &, auto &) {
        ++call.hirValue;
      },
      [](auto &, auto &, auto &, auto &stage, auto &call, auto &, auto &) {
        std::swap(stage, call);
      },
      [](auto &, auto &, auto &block, auto &stage, auto &, auto &from, auto &) {
        const auto ended = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &candidate) {
              return candidate.kind == MirInstructionKind::EndBorrow &&
                     candidate.loan == from.id;
            });
        require(ended != block.instructions.end(),
                "relay has a source endpoint");
        std::swap(stage, *ended);
      },
  };
  if (fallibleReference ||
      (countedText && !rawText && borrowedType == SemanticType::StringView)) {
    mutations.push_back(
        [](auto &, auto &body, auto &, auto &, auto &call, auto &, auto &) {
          auto &previous =
              body.blocks[body.findValue(*call.result)->definitionBlock - 1];
          previous.terminator.target = previous.terminator.elseTarget;
        });
    mutations.push_back(
        [](auto &, auto &body, auto &, auto &, auto &call, auto &, auto &) {
          auto &previous =
              body.blocks[body.findValue(*call.result)->definitionBlock - 1];
          std::swap(previous.terminator.target, previous.terminator.elseTarget);
        });
    mutations.push_back([](auto &, auto &body, auto &block, auto &, auto &call,
                           auto &, auto &) {
      body.values[*call.result - 1].definitionBlock = block.id;
    });
    mutations.push_back([](auto &, auto &, auto &, auto &, auto &call, auto &,
                           auto &) { call.tombstone = true; });
  }
  if (countedText) {
    mutations.push_back([](auto &, auto &, auto &, auto &, auto &, auto &from,
                           auto &) { from.kind = MirLoanKind::CallResult; });
    mutations.push_back([](auto &, auto &, auto &, auto &, auto &, auto &from,
                           auto &) { from.escapes = true; });
    mutations.push_back([](auto &, auto &, auto &, auto &, auto &, auto &from,
                           auto &to) { from.parent = to.id; });
    mutations.push_back([](auto &, auto &, auto &, auto &, auto &, auto &from,
                           auto &) { from.lifecycleBound = true; });
    mutations.push_back([](auto &, auto &, auto &, auto &, auto &, auto &from,
                           auto &) { from.carriers.push_back(1); });
    mutations.push_back(
        [](auto &, auto &, auto &, auto &, auto &call, auto &, auto &) {
          call.info.category = ValueCategory::Place;
        });
    mutations.push_back([](auto &, auto &, auto &block, auto &, auto &, auto &,
                           auto &) { block.terminator.value->value = 0; });
    mutations.push_back([rawText](auto &mir, auto &, auto &, auto &, auto &call,
                                  auto &, auto &) {
      if (rawText)
        call.functionTarget = 1;
      else
        MirProgramTestAccess::functions(mir)[*call.functionTarget - 1]
            .requiresUnsafe = false;
    });
    mutations.push_back([rawText](auto &mir, auto &, auto &, auto &, auto &call,
                                  auto &, auto &) {
      if (rawText)
        call.unsafeOperation = UnsafeOperationKind::DeclaredUnsafeCall;
      else
        MirProgramTestAccess::functions(mir)[*call.functionTarget - 1].linkage =
            LanguageLinkage::C;
    });
    mutations.push_back([](auto &, auto &, auto &, auto &, auto &, auto &,
                           auto &to) { to.carriers.push_back(1); });
  }
  for (const auto &mutate : mutations) {
    auto invalid = frontend.mir;
    auto &body = MirProgramTestAccess::functions(invalid)[owner - 1].body;
    auto &block = body.blocks[blockId - 1];
    auto &transfer = *std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const auto &stage) { return stage.id == transferId; });
    auto &from = body.loans[transfer.operands.front().loan - 1];
    auto &to = body.loans[*transfer.loan - 1];
    MirInstruction *producer = nullptr;
    for (auto &candidate : body.blocks)
      for (auto &stage : candidate.instructions)
        if (stage.kind == MirInstructionKind::Call && stage.loan == from.id) {
          require(!producer, "return witness has one exact call producer");
          producer = &stage;
        }
    require(producer, "return witness retains its call producer");
    auto &call = *producer;
    mutate(invalid, body, block, transfer, call, from, to);
    auto built = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, invalid, invalid,
        {}, TargetInfo::host());
    bool rejected = !built.valid();
    if (built.valid()) {
      auto verified = verifiedLoweredProgram(std::move(*built.program), issues);
      rejected = !verified || !planLlvmAdmission(*verified).admitted();
    }
    require(rejected, "forged unsafe return relationship cannot be admitted");
  }
}

void classOwnerRawReferenceContract(const std::filesystem::path &repository,
                                    std::string_view fixture) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto analyze = [&](const std::filesystem::path &path,
                           std::optional<std::string> source = {}) {
    return Frontend(options).analyze(path, std::move(source),
                                     {repository / "stdlib/prelude.gti"}, {},
                                     {repository / "stdlib"});
  };
  const auto frontend = analyze(repository / "tests/fixtures" / fixture);
  require(frontend.canGenerateCode(), "owning class witness is valid GTI");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program && planLlvmAdmission(*program).admitted(),
          "native owner witness retains the ordinary class admission boundary");
  std::array<bool, 2> seen{};
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.intrinsic != IntrinsicKind::RawBorrow &&
            stage.intrinsic != IntrinsicKind::RawBorrowMut)
          continue;
        const auto *loan =
            mir_detail::rawBorrowResultLoan(function.body, stage);
        require(loan != nullptr,
                "raw reference has the centralized loan proof");
        const auto &formal = stage.parameterTypes[1];
        require(formal.kind == SemanticType::Reference &&
                    formal.arguments.size() == 1 &&
                    formal.arguments.front().kind == SemanticType::Class,
                "raw reference is tied to an ordinary class witness");
        seen[stage.intrinsic == IntrinsicKind::RawBorrowMut] = true;
        for (unsigned mutation = 0; mutation != 9; ++mutation) {
          auto invalidBody = function.body;
          auto invalid = stage;
          auto &invalidLoan = invalidBody.loans[loan->id - 1];
          switch (mutation) {
          case 0:
            invalidLoan.parent = 0;
            break;
          case 1:
            invalidLoan.source = 0;
            break;
          case 2:
            invalidLoan.access = loan->access == AccessMode::Mutable
                                     ? AccessMode::ReadOnly
                                     : AccessMode::Mutable;
            break;
          case 3:
            invalid.parameterTypes[1].referenceAccess =
                formal.referenceAccess == AccessMode::Mutable
                    ? AccessMode::ReadOnly
                    : AccessMode::Mutable;
            break;
          case 4:
            invalid.operands[0].value = 0;
            break;
          case 5:
            invalid.info.type = stage.info.type.kind == SemanticType::Class
                                    ? SemanticType::Int32
                                    : SemanticType::Char;
            break;
          case 6:
            invalid.operands[0].type = SemanticType::rawPointerTo(
                SemanticType::Void, stage.operands[0].type.pointerAccess);
            break;
          case 7:
            invalid.borrowAccess = loan->access == AccessMode::Mutable
                                       ? AccessMode::ReadOnly
                                       : AccessMode::Mutable;
            break;
          case 8:
            invalid.parameterTypes[0] =
                SemanticType::rawPointerTo(SemanticType::Char);
            break;
          }
          require(!mir_detail::rawBorrowResultLoan(invalidBody, invalid),
                  "forged parent/source/access/formal/address loses its proof");
        }
        const auto transfer = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &candidate) {
              return candidate.kind == MirInstructionKind::Borrow &&
                     candidate.operands.size() == 1 &&
                     candidate.operands.front().kind == MirOperandKind::Loan &&
                     candidate.operands.front().loan == loan->id;
            });
        require(transfer != block.instructions.end(),
                "raw assertion has a real checked return publication");
        for (unsigned mutation = 0; mutation != 12; ++mutation) {
          auto invalidMir = frontend.mir;
          auto &invalidBody =
              MirProgramTestAccess::functions(invalidMir)[function.id - 1].body;
          auto &invalidBlock = invalidBody.blocks[block.id - 1];
          auto &producer = *std::find_if(
              invalidBlock.instructions.begin(),
              invalidBlock.instructions.end(),
              [&](const auto &candidate) { return candidate.id == stage.id; });
          auto &publication = *std::find_if(
              invalidBlock.instructions.begin(),
              invalidBlock.instructions.end(), [&](const auto &candidate) {
                return candidate.id == transfer->id;
              });
          auto &returned = invalidBody.loans[*publication.loan - 1];
          switch (mutation) {
          case 0:
            publication.unsafeOperation =
                UnsafeOperationKind::DeclaredUnsafeCall;
            break;
          case 1:
            producer.unsafeOperation = UnsafeOperationKind::DeclaredUnsafeCall;
            break;
          case 2:
            producer.intrinsic = IntrinsicKind::RestoreReference;
            break;
          case 3:
            publication.operands.front().loan = 0;
            break;
          case 4: {
            const auto ended = std::find_if(
                invalidBlock.instructions.begin(),
                invalidBlock.instructions.end(), [&](const auto &candidate) {
                  return candidate.kind == MirInstructionKind::EndBorrow &&
                         candidate.loan == loan->id;
                });
            require(ended != invalidBlock.instructions.end(),
                    "raw return source has an explicit endpoint");
            std::swap(publication, *ended);
            break;
          }
          case 5:
            std::swap(producer, publication);
            break;
          case 6:
            returned.source = 0;
            break;
          case 7:
            returned.access = loan->access == AccessMode::Mutable
                                  ? AccessMode::ReadOnly
                                  : AccessMode::Mutable;
            break;
          case 8:
            returned.additionalDependencies.push_back(
                {.source = loan->source, .access = loan->access});
            break;
          case 9:
            invalidBlock.terminator.returnLoan = loan->id;
            break;
          case 10:
            ++producer.hirValue;
            break;
          case 11:
            producer.loan = returned.id;
            break;
          }
          auto built = LoweredProgramBuilder().build(
              frontend.program, frontend.semantics, frontend.hir, invalidMir,
              invalidMir, {}, TargetInfo::host());
          bool rejected = !built.valid();
          if (built.valid()) {
            auto verified =
                verifiedLoweredProgram(std::move(*built.program), issues);
            rejected = !verified || !planLlvmAdmission(*verified).admitted();
          }
          require(
              rejected,
              "raw/source authority substitution or forged return is rejected");
        }
      }
  require(seen[0] && seen[1], "both class-witness access modes are exercised");

  const std::string owner = R"(
#include <std/memory>
struct Owner {
  mut int value;
  std::unique_ptr<int> token;
  Owner() : value(7), token(std::make_unique<int>(5)) {}
};
)";
  const std::array badBodies{
      R"(int& escape() {
           mut Owner owner{};
           unsafe { return std::assume_ref(&owner.value, owner); }
         }
         int main() { return 0; })",
      R"(void consume(Owner value) {}
         int main() {
           mut Owner owner{};
           int& read = unsafe { std::assume_ref(&owner.value, owner) };
           consume(std::move(owner));
           return read;
         })",
      R"(int main() {
           mut Owner owner{};
           mut int& first = unsafe { std::assume_mut(&owner.value, owner) };
           mut int& second = unsafe { std::assume_mut(&owner.value, owner) };
           first = 1;
           second = 2;
           return 0;
         })",
  };
  for (const auto body : badBodies) {
    const auto invalid = analyze("invalid-owner-loan.gti", owner + body);
    require(
        !invalid.canGenerateCode() &&
            std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.phase == DiagnosticPhase::Semantics;
                        }),
        "owner death, movement and overlapping mutation remain rejected");
  }
  const std::string nestedOwner = R"(
#include <std/memory>
struct Referent {
  mut int value = 7;
  std::unique_ptr<int> token;
  Referent() : token(std::make_unique<int>(5)) {}
};
struct Owner {
  mut Referent item;
  Owner() : item(Referent()) {}
};
)";
  const std::array badClassBodies{
      R"(Referent& escape() {
           mut Owner owner{};
           unsafe { return std::assume_ref(&owner.item, owner); }
         }
         int main() { return 0; })",
      R"(void consume(Owner value) {}
         int main() {
           mut Owner owner{};
           Referent& read = unsafe { std::assume_ref(&owner.item, owner) };
           consume(std::move(owner));
           return read.value;
         })",
      R"(int main() {
           mut Owner owner{};
           mut Referent& first = unsafe { std::assume_mut(&owner.item, owner) };
           mut Referent& second = unsafe { std::assume_mut(&owner.item, owner) };
           first.value = 1;
           second.value = 2;
           return 0;
         })",
  };
  for (const auto body : badClassBodies) {
    const auto invalid =
        analyze("invalid-class-referent-loan.gti", nestedOwner + body);
    require(
        !invalid.canGenerateCode() &&
            std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.phase == DiagnosticPhase::Semantics;
                        }),
        "class referent cannot outlive, move or overlap its owner loan");
  }
  const auto unsupported = analyze("unsupported-owner-layout.gti", R"(
#include <std/memory>
struct Owner {
  int value = 7;
  virtual int observe() { return this.value; }
  ~Owner() {}
};
int main() {
  mut Owner owner{};
  int& read = unsafe { std::assume_ref(&owner.value, owner) };
  return read == 7 ? 0 : 1;
}
)");
  require(unsupported.canGenerateCode(), "unsupported layout is valid source");
  const auto pending = verifiedLoweredProgram(
      gti_test::lowerProgram(unsupported, unsupported.mir, unsupported.mir),
      issues);
  require(pending && !planLlvmAdmission(*pending).admitted(),
          "primitive pointee does not admit an unsupported owner layout");
  const auto unsupportedReferent =
      analyze("unsupported-referent-layout.gti", R"(
#include <std/memory>
struct Referent {
  int value = 7;
  virtual int observe() { return this.value; }
  ~Referent() {}
};
struct Owner { int marker = 11; };
int main() {
  Referent referent{};
  Owner owner{};
  Referent& read = unsafe { std::assume_ref(&referent, owner) };
  return read.value == 7 ? 0 : 1;
}
)");
  require(unsupportedReferent.canGenerateCode(),
          "unsupported referent layout is valid source");
  const auto pendingReferent = verifiedLoweredProgram(
      gti_test::lowerProgram(unsupportedReferent, unsupportedReferent.mir,
                             unsupportedReferent.mir),
      issues);
  require(pendingReferent && !planLlvmAdmission(*pendingReferent).admitted(),
          "ordinary witness does not admit an unsupported referent layout");
}

void classOwnerTextContract(const std::filesystem::path &repository) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto analyze = [&](const std::filesystem::path &path,
                           std::optional<std::string> source = {}) {
    return Frontend(options).analyze(path, std::move(source),
                                     {repository / "stdlib/prelude.gti"}, {},
                                     {repository / "stdlib"});
  };
  const auto frontend = analyze(
      repository / "tests/fixtures/llvm_hosted_class_owner_text_return.gti");
  require(frontend.canGenerateCode(), "class text witness is valid GTI");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program && planLlvmAdmission(*program).admitted(),
          "text assertions share the ordinary native class witness boundary");
  std::array<bool, 2> seen{};
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        const bool counted =
            stage.intrinsic == IntrinsicKind::TextViewFromAddress;
        if (!counted && stage.intrinsic != IntrinsicKind::CStringFromAddress)
          continue;
        const std::size_t witness = counted ? 2 : 1;
        seen[counted] = true;
        const auto *loan =
            mir_detail::textAddressResultLoan(function.body, stage);
        require(loan &&
                    stage.parameterTypes[witness].kind ==
                        SemanticType::Reference &&
                    stage.parameterTypes[witness].arguments.size() == 1 &&
                    stage.parameterTypes[witness].arguments.front().kind ==
                        SemanticType::Class,
                "raw text has an exact readonly ordinary-class witness");
        for (unsigned mutation = 0; mutation != 8; ++mutation) {
          auto invalidBody = function.body;
          auto invalid = stage;
          auto &invalidLoan = invalidBody.loans[loan->id - 1];
          switch (mutation) {
          case 0:
            invalidLoan.source = 0;
            break;
          case 1:
            invalidLoan.producedBy = 0;
            break;
          case 2:
            invalid.info.access = AccessMode::Mutable;
            break;
          case 3:
            invalid.borrowArgument = 0;
            break;
          case 4:
            invalid.parameterTypes[witness].referenceAccess =
                AccessMode::Mutable;
            break;
          case 5:
            invalid.operands[witness].value = 0;
            break;
          case 6:
            invalid.unsafeOperation = UnsafeOperationKind::DeclaredUnsafeCall;
            break;
          case 7:
            invalid.rawMemoryAccess = true;
            break;
          }
          require(
              !mir_detail::textAddressResultLoan(invalidBody, invalid),
              "forged owner identity, access, input or raw authority rejects");
        }
      }
  require(seen[0] && seen[1], "both fundamental text construction proofs run");
  const std::string owner = R"(
#include <std/memory>
#include <std/string_view>
struct Owner {
  mut int tag = 22;
  std::unique_ptr<int> token;
  Owner() : token(std::make_unique<int>(5)) {}
};
)";
  for (const bool counted : {false, true}) {
    const std::string type = counted ? "std::string_view" : "c_string";
    const std::string construct =
        counted ? "std::assume_string_view(&bytes[std::size_t(0)], "
                  "std::size_t(3), owner)"
                : "std::assume_c_string(&bytes[std::size_t(0)], owner)";
    const std::string use =
        counted ? "int(result.size())" : "(result == nullptr ? 0 : 1)";
    const std::array badBodies{
        type +
            " escape() { Owner owner{}; char bytes[3] = {'a', '\\0', 'b'}; "
            "unsafe { return " +
            construct + "; } } int main() { return 0; }",
        "int main() { mut Owner owner{}; char bytes[3] = {'a', '\\0', 'b'}; " +
            type + " result = unsafe { " + construct +
            " }; owner.tag = 23; return " + use + "; }",
        "void consume(Owner value) {} int main() { mut Owner owner{}; char "
        "bytes[3] = {'a', '\\0', 'b'}; " +
            type + " result = unsafe { " + construct +
            " }; consume(std::move(owner)); return " + use + "; }",
    };
    for (std::size_t index = 0; index < badBodies.size(); ++index) {
      const auto invalid =
          analyze("invalid-text-owner-loan.gti", owner + badBodies[index]);
      require(!invalid.canGenerateCode() &&
                  std::any_of(
                      invalid.diagnostics.begin(), invalid.diagnostics.end(),
                      [index](const auto &diagnostic) {
                        return diagnostic.phase == DiagnosticPhase::Semantics &&
                               diagnostic.code ==
                                   (index == 0 ? "GTI-S2045" : "GTI-S2017");
                      }),
              "text cannot escape, mutate or move its ordinary class witness");
    }
    const auto unsupported =
        analyze("unsupported-text-owner-layout.gti", std::string(R"(
#include <std/memory>
#include <std/string_view>
struct Owner {
  int tag = 22;
  virtual int observe() { return this.tag; }
  ~Owner() {}
};
int main() {
  Owner owner{};
  char bytes[3] = {'a', '\0', 'b'};
)") + type + " result = unsafe { " + construct + " }; return " +
                                                         use + "; }");
    require(unsupported.canGenerateCode(),
            "unsupported text witness layout is valid source");
    const auto pending = verifiedLoweredProgram(
        gti_test::lowerProgram(unsupported, unsupported.mir, unsupported.mir),
        issues);
    require(
        pending && !planLlvmAdmission(*pending).admitted(),
        "text construction does not admit unsupported class witness layouts");
  }
}

void scalarReturnContracts() {
  using Kind = mir_detail::MirScalarReturnConversion;
  struct Positive {
    const char *name;
    const char *source;
    Kind expected;
  };
  const std::array positives{
      Positive{"float-literal", "double source() { return 1.25; }",
               Kind::Initialization},
      Positive{"float-value", "double source(float value) { return value; }",
               Kind::Initialization},
      Positive{"float-reference-value",
               "double source(float& value) { return value; }",
               Kind::Initialization},
      Positive{"explicit-double", "double source() { return double(1.25); }",
               Kind::Exact},
      Positive{"signed-widen",
               "int64_t source(int32_t value) { return value; }",
               Kind::Initialization},
      Positive{"unsigned-widen",
               "int64_t source(uint32_t value) { return value; }",
               Kind::Initialization},
      Positive{"small-widen", "int16_t source(uint8_t value) { return value; }",
               Kind::Initialization},
      Positive{"integer-double",
               "double source(int64_t value) { return value; }",
               Kind::Initialization},
      Positive{"integer-float", "float source(int32_t value) { return value; }",
               Kind::Initialization},
      Positive{"literal-narrow", "int8_t source() { return 7; }", Kind::Exact},
      Positive{"bool", "bool source(bool value) { return value; }",
               Kind::Exact},
      Positive{"char", "char source(char value) { return value; }",
               Kind::Exact},
      Positive{"reference",
               "[[borrowed_from(value)]] int32_t& source(int32_t& value) { "
               "return value; }",
               Kind::NotApplicable},
      Positive{"aggregate",
               "struct Record { int32_t value; }; Record source(Record value) "
               "{ return value; }",
               Kind::NotApplicable},
      Positive{"enum",
               "enum class Flag { one, two }; Flag source(Flag value) { return "
               "value; }",
               Kind::NotApplicable},
      Positive{"raw-null", "int32_t* source() { return nullptr; }",
               Kind::NotApplicable},
  };
  for (const auto &test : positives) {
    const auto frontend = Frontend().analyze(test.name, test.source);
    require(
        frontend.canGenerateCode() && verifyMirProgram(frontend.mir).valid(),
        std::string("legal scalar/non-scalar return verifies: ") + test.name);
    bool seen = false;
    for (const auto &hir : frontend.hir.functionInstances()) {
      if (!hir.source || hir.source->name().lexeme != "source")
        continue;
      const auto *function = frontend.mir.findFunctionInstance(hir.id);
      require(function != nullptr, "return test retains its exact function");
      for (const auto &block : function->body.blocks) {
        if (block.terminator.kind != MirTerminatorKind::Return ||
            !block.terminator.value)
          continue;
        seen = true;
        require(mir_detail::scalarReturnConversion(
                    function->body, *block.terminator.value) == test.expected,
                std::string("typed return boundary agrees with semantics: ") +
                    test.name);
      }
    }
    require(seen, "return test has an executable return");
  }
  const std::array negatives{
      "float source(double value) { return value; }",
      "int32_t source(int64_t value) { return value; }",
      "uint64_t source(int32_t value) { return value; }",
      "int64_t source(uint64_t value) { return value; }",
      "int32_t source(bool value) { return value; }",
      "int32_t source(char value) { return value; }",
      "enum class Flag { one }; int32_t source(Flag value) { return value; }",
      "int8_t source() { return int64_t(7); }",
      "int8_t source() { constexpr int64_t value = 7; return value; }",
  };
  for (const auto source : negatives) {
    const auto frontend =
        Frontend().analyze("forbidden-scalar-return.gti", source);
    require(!frontend.canGenerateCode() &&
                std::any_of(frontend.diagnostics.begin(),
                            frontend.diagnostics.end(),
                            [](const auto &error) {
                              return error.code == "GTI-S2003";
                            }),
            "forbidden return conversions remain semantic errors");
  }
  const auto frontend = Frontend().analyze("forged-scalar-return.gti",
                                           "int8_t source() { return 7; }");
  require(frontend.canGenerateCode(),
          "literal narrowing was selected before MIR");
  auto invalid = frontend.mir;
  bool changed = false;
  for (auto &function : MirProgramTestAccess::functions(invalid))
    for (auto &block : function.body.blocks) {
      if (block.terminator.kind != MirTerminatorKind::Return ||
          !block.terminator.value)
        continue;
      auto &operand = *block.terminator.value;
      if (operand.kind == MirOperandKind::Value) {
        auto *value =
            const_cast<MirValue *>(function.body.findValue(operand.value));
        require(value != nullptr,
                "contextual literal has its exact SSA result");
        value->info.type = SemanticType::Int64;
        bool producerChanged = false;
        for (auto &producerBlock : function.body.blocks)
          for (auto &producer : producerBlock.instructions)
            if (producer.result == operand.value) {
              producer.info.type = SemanticType::Int64;
              producerChanged = true;
            }
        require(producerChanged,
                "forged literal retains a coherent typed producer");
      } else {
        require(operand.kind == MirOperandKind::Constant,
                "contextual literal has a value carrier");
      }
      operand.type = SemanticType::Int64;
      require(
          mir_detail::scalarReturnConversion(function.body, operand) ==
              Kind::Invalid,
          "MIR cannot invent literal-dependent narrowing even when bits fit");
      const auto verification = verifyMirBody(function.body, function.id);
      require(!verification.valid() &&
                  std::any_of(
                      verification.errors.begin(), verification.errors.end(),
                      [](const auto &error) {
                        return error.message.find("scalar return lacks") !=
                               std::string::npos;
                      }),
              "the owning return invariant rejects the forged constant");
      changed = true;
    }
  require(changed && !verifyMirProgram(invalid).valid(),
          "forged scalar return fails whole-program verification");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, invalid, invalid, {},
      TargetInfo::host());
  std::vector<LoweredProgramIssue> issues;
  require(
      !lowered.valid() ||
          !verifiedLoweredProgram(std::move(*lowered.program), issues),
      "forged scalar conversion cannot cross the sealed publication boundary");
  MirBody scalar;
  scalar.returnType = SemanticType::Int32;
  for (const auto kind : {MirOperandKind::Address, MirOperandKind::BorrowRead,
                          MirOperandKind::BorrowWrite, MirOperandKind::Loan})
    require(mir_detail::scalarReturnConversion(
                scalar, {.kind = kind, .type = SemanticType::Int32}) ==
                Kind::Invalid,
            "a scalar result cannot smuggle an address/loan carrier");
}

void scalarReturnPlaceContracts() {
  using Kind = mir_detail::MirScalarReturnConversion;
  struct Source {
    const char *name;
    const char *text;
    SemanticType placeType;
    bool projected;
  };
  const std::array sources{
      Source{"float", "double source(float value) { return value; }",
             SemanticType::Float, false},
      Source{"int32", "double source(int32_t value) { return value; }",
             SemanticType::Int32, false},
      Source{"int64", "double source(int64_t value) { return value; }",
             SemanticType::Int64, false},
      Source{"double", "double source(double value) { return value; }",
             SemanticType::Double, false},
      Source{"field",
             "struct Record { float value; }; double source(Record value) { "
             "return value.value; }",
             SemanticType::Float, true},
  };
  for (const auto &test : sources) {
    const auto frontend = Frontend().analyze(test.name, test.text);
    require(frontend.canGenerateCode() &&
                verifyMirProgram(frontend.mir).valid(),
            "place-return source has verified parameter/field storage");
    HirFunctionInstanceId id = 0;
    for (const auto &hir : frontend.hir.functionInstances())
      if (hir.source && hir.source->name().lexeme == "source")
        id = hir.id;
    require(id != 0, "place-return test has its exact function identity");
    for (const auto carrier : {MirOperandKind::Copy, MirOperandKind::Move}) {
      // A field may have additional independent move/ownership restrictions.
      // Its matching copy proves selected projection typing without changing
      // them.
      if (test.projected && carrier == MirOperandKind::Move)
        continue;
      for (const SemanticType claim :
           {SemanticType::Float, SemanticType::Int32, SemanticType::Int64,
            SemanticType::Double}) {
        auto program = frontend.mir;
        auto *function =
            const_cast<MirFunctionInstance *>(program.findFunctionInstance(id));
        require(function != nullptr, "place-return body remains available");
        const MirPlace *selected = nullptr;
        for (const auto &place : function->body.places) {
          const bool field =
              std::any_of(place.projections.begin(), place.projections.end(),
                          [](const auto &projection) {
                            return projection.kind == MirProjectionKind::Field;
                          });
          if (place.type == test.placeType && field == test.projected &&
              (test.projected ||
               (place.root == MirPlaceRootKind::Binding &&
                place.binding == function->parameterBindings.front() &&
                place.projections.empty()))) {
            selected = &place;
            break;
          }
        }
        require(selected != nullptr,
                "place-return uses the exact typed selected storage");
        const auto selectedId = selected->id;
        MirOperand *returned = nullptr;
        for (auto &block : function->body.blocks)
          if (block.terminator.kind == MirTerminatorKind::Return &&
              block.terminator.value) {
            block.terminator.value =
                MirOperand{.kind = carrier, .place = selectedId, .type = claim};
            returned = &*block.terminator.value;
          }
        require(returned != nullptr && rebuildMirValueUses(function->body),
                "place-return control retains a coherent operand-use index");
        const bool matching = claim == test.placeType;
        const auto expected = !matching ? Kind::Invalid
                              : claim == SemanticType::Double
                                  ? Kind::Exact
                                  : Kind::Initialization;
        require(mir_detail::scalarReturnConversion(function->body, *returned) ==
                    expected,
                "copy/move return proof binds the claimed type before "
                "exact/conversion selection");
        const auto bodyVerification = verifyMirBody(function->body, id);
        const auto programVerification = verifyMirProgram(program);
        if (matching) {
          require(
              bodyVerification.valid() && programVerification.valid(),
              "a genuine matching parameter/projected copy/move stays valid");
        } else {
          require(!bodyVerification.valid() && !programVerification.valid() &&
                      std::any_of(bodyVerification.errors.begin(),
                                  bodyVerification.errors.end(),
                                  [](const auto &error) {
                                    return error.message.find(
                                               "scalar return lacks") !=
                                           std::string::npos;
                                  }),
                  "wider, narrower and reinterpreted storage claims fail the "
                  "owning return invariant");
        }
        auto lowered = LoweredProgramBuilder().build(
            frontend.program, frontend.semantics, frontend.hir, program,
            program, {}, TargetInfo::host());
        std::vector<LoweredProgramIssue> issues;
        const bool sealed =
            lowered.valid() &&
            verifiedLoweredProgram(std::move(*lowered.program), issues)
                .has_value();
        require(sealed == matching,
                "matching place returns cross sealing and mismatched claims "
                "reject before publication");
      }
    }
  }
}

void hostedArgumentsContract(const std::filesystem::path &repository) {
  for (const auto triple :
       {"arm64-apple-macos", "x86_64-apple-macos", "x86_64-unknown-linux-gnu",
        "aarch64-unknown-linux-gnu"}) {
    const auto target = parseTargetTriple(triple);
    require(target.has_value(), "hosted ABI test names an accepted target");
    const auto parameters = hostedNativeParameters(*target);
    require(parameters && parameters->front() == SemanticType::Int32 &&
                parameters->back().kind == SemanticType::RawPointer &&
                parameters->back().arguments.size() == 1 &&
                parameters->back().arguments.front().kind ==
                    SemanticType::RawPointer,
            "hosted argc and argv derive from exact target C declarations");
  }
  auto unsupported = TargetInfo::host();
  unsupported.arch = "unsupported";
  require(!hostedNativeParameters(unsupported),
          "unsupported target cannot guess a hosted native ABI");
  llvm::LLVMContext context;
  llvm::IRBuilder<> ir(context);
  const auto low = std::int64_t(std::numeric_limits<std::int32_t>::min());
  const auto high = std::int64_t(std::numeric_limits<std::int32_t>::max());
  for (const auto count :
       {std::numeric_limits<std::int64_t>::min(), low - 1, low,
        std::int64_t(-1), std::int64_t(0), high, high + 1,
        std::numeric_limits<std::int64_t>::max()}) {
    const auto *outside = llvm::dyn_cast<llvm::ConstantInt>(
        hostedCountOutOfRange(ir, ir.getInt64(count)));
    require(outside && outside->isOne() == (count < low || count > high),
            "count narrowing checks the entire sealed Int64 domain");
  }
  bool wrongWidth = false;
  try {
    static_cast<void>(hostedCountOutOfRange(ir, ir.getInt32(0)));
  } catch (const std::logic_error &) {
    wrongWidth = true;
  }
  require(wrongWidth,
          "a native-width count cannot impersonate the stabilized count");

  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      repository / "tests/fixtures/llvm_hosted_arguments.gti", std::nullopt,
      {repository / "stdlib/prelude.gti"}, {}, {repository / "stdlib"});
  require(frontend.canGenerateCode() && verifyMirProgram(frontend.mir).valid(),
          "hosted arguments have a real verified source startup plan");
  const auto &body = *frontend.mir.hostedStartup();
  const auto &startup = *frontend.mir.hostedStartupPlan();
  const auto nextIndex = std::find_if(
      startup.operations.begin(), startup.operations.end(), [](const auto &op) {
        return op.kind == MirHostedStartupOperationKind::ComputeNextArgumentIndex;
      });
  const auto advance = std::find_if(
      startup.operations.begin(), startup.operations.end(), [](const auto &op) {
        return op.kind == MirHostedStartupOperationKind::AdvanceArgumentIndex;
      });
  require(nextIndex != startup.operations.end() && advance != startup.operations.end(),
          "hosted index arithmetic and commit have separate sealed identities");
  for (unsigned mutation = 0; mutation != 4; ++mutation) {
    auto invalid = frontend.mir;
    auto &invalidBody = *const_cast<MirBody *>(invalid.hostedStartup());
    auto &block = invalidBody.blocks[nextIndex->block - 1];
    auto &compute = *std::find_if(
        block.instructions.begin(), block.instructions.end(), [&](const auto &op) {
          return op.id == nextIndex->instruction;
        });
    auto &commit = *std::find_if(
        block.instructions.begin(), block.instructions.end(), [&](const auto &op) {
          return op.id == advance->instruction;
        });
    if (mutation == 0)
      compute.operands[1].literal = Literal{std::uint64_t{2}};
    else if (mutation == 1)
      compute.operation = MirOperation::Subtract;
    else if (mutation == 2)
      commit.operands[0].value = compute.operands[0].value;
    else {
      commit.kind = MirInstructionKind::Modify;
      commit.operands.clear();
      commit.operation = MirOperation::PreIncrement;
    }
    require(!verifyMirProgram(invalid).valid(),
            "hosted index cannot change step, direction, commit or use legacy mutation");
  }
  std::array<bool, 3> seen{};
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions) {
      const auto *operation =
          hostedArgumentOperation(frontend.mir, body, block, stage);
      if (!operation)
        continue;
      const auto index =
          operation->kind ==
                  MirHostedStartupOperationKind::ValidateArgumentCount
              ? 0
          : operation->kind ==
                  MirHostedStartupOperationKind::ConvertArgumentCount
              ? 1
              : 2;
      seen[index] = true;
      auto detached = stage;
      bool rejected = false;
      try {
        static_cast<void>(
            hostedArgumentOperation(frontend.mir, body, block, detached));
      } catch (const std::logic_error &) {
        rejected = true;
      }
      require(
          rejected,
          "copied stage identities cannot impersonate the sealed instruction");
      for (unsigned mutation = 0; mutation != 6; ++mutation) {
        auto invalid = frontend.mir;
        auto &plan = *MirProgramTestAccess::hostedPlan(invalid);
        auto &invalidBody = *const_cast<MirBody *>(invalid.hostedStartup());
        auto &invalidBlock = invalidBody.blocks[block.id - 1];
        auto &invalidStage = *std::find_if(
            invalidBlock.instructions.begin(), invalidBlock.instructions.end(),
            [&](const auto &candidate) { return candidate.id == stage.id; });
        auto &invalidOperation = plan.operations[operation->id - 1];
        switch (mutation) {
        case 0:
          ++invalidOperation.block;
          break;
        case 1:
          ++invalidOperation.instruction;
          break;
        case 2:
          invalidOperation.value = 0;
          break;
        case 3:
          if (invalidStage.operands.empty())
            invalidStage.info.type = SemanticType::Int32;
          else
            invalidStage.operands.front().value = 0;
          break;
        case 4:
          if (!invalidStage.localFailureSites.empty())
            invalidStage.localFailureSites.front() = 0;
          else
            invalidOperation.failureBehavior =
                MirHostedStartupFailureBehavior::Detect;
          break;
        case 5:
          if (invalidBlock.terminator.kind == MirTerminatorKind::Invoke)
            invalidBlock.terminator.elseTarget = invalidBlock.terminator.target;
          else
            invalidBlock.terminator.target = 0;
          break;
        }
        require(
            !verifyMirProgram(invalid).valid(),
            "malformed hosted identity, site or edge cannot cross MIR sealing");
      }
    }
  require(seen[0] && seen[1] && seen[2],
          "count validation/conversion and argument view have real producer "
          "contracts");
}

void hostedArgumentsBoundary(const std::filesystem::path &repository,
                             const std::filesystem::path &scratch,
                             LlvmCodegenOptimization optimization) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      repository / "tests/fixtures/llvm_hosted_arguments.gti", std::nullopt,
      {repository / "stdlib/prelude.gti"}, {}, {repository / "stdlib"});
  require(frontend.canGenerateCode(), "owned arguments have valid source MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program.has_value(), "owned arguments cross the sealed boundary");
  const LlvmBackendOptions backendOptions{.optimization = optimization};
  const auto admission = planLlvmAdmission(*program, backendOptions);
  if (!admission.admitted())
    std::cerr << admission.limitation() << '\n';
  require(admission.admitted(), "entire owned-argument closure is admitted");
  Session session(*program, backendOptions);
  lowerScalarProgram(session, admission);
  auto *entry = session.module().getFunction("main");
  require(entry && entry->arg_size() == 2 &&
              entry->getArg(0)->getType()->isIntegerTy(32) &&
              entry->getArg(1)->getType()->isPointerTy(),
          "hosted entry exposes the exact C argc/argv boundary");
  // Rename only the external entry so a C caller can exercise zero/negative
  // argc. All source bodies, guards, cleanup and runtime calls are unchanged.
  entry->setName("gti_hosted_entry");
  const auto directory = scratch / "arguments-boundary" /
      (optimization == LlvmCodegenOptimization::None ? "O0" : "O3");
  std::filesystem::create_directories(directory);
  const auto object = directory / "program.o";
  require(driver::writeArtifact(object, session.emitObject().contents) ==
              driver::ArtifactWriteStatus::Success,
          "owned-argument object publishes through normal emission");
  const auto shim = directory / "caller.c";
  require(driver::writeArtifact(shim,
      "#include <stddef.h>\n"
      "extern int gti_hosted_entry(int, char **);\n"
      "int main(int argc, char **argv) {\n"
      "  (void)argv;\n"
      "  return gti_hosted_entry(argc == 1 ? 0 : -1, NULL);\n"
      "}\n") == driver::ArtifactWriteStatus::Success,
      "write hosted C boundary caller");
  const auto layout = driver::discoverToolchainLayout(nullptr);
  driver::NativeToolchain toolchain;
  const auto callerObject = directory / "caller.o";
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), shim, callerObject, driver::CStandard::C17,
      OptimizationLevel::O2, {}, {}));
  require(compiled.succeeded(), "hosted boundary caller compiles as C17");
  driver::NativeInputs inputs;
  inputs.finalLibraryFiles = {callerObject, layout.runtimeLibrary};
  const auto executable = directory / "program";
  const auto linked = toolchain.invoke(driver::NativeLinkRequest(
      driver::discoverNativeCompiler(), object, executable, inputs));
  if (!linked.succeeded())
    std::cerr << linked.output;
  require(linked.succeeded(), "real owned startup links to its runtime");
  const auto zero = driver::invokeProcess({executable.string()});
  require(zero.succeeded() && zero.output.empty(),
          "zero count constructs and drops an empty owner without reading null argv");
  const auto negative = driver::invokeProcess({executable.string(), "negative"});
  if (negative.exitCode != 70)
    std::cerr << negative.output;
  require(negative.exitCode == 70 && !negative.signal && !negative.timedOut &&
              negative.output.find("hosted_runtime_contract_failure") != std::string::npos &&
              negative.output.find("negative_argument_count") != std::string::npos &&
              negative.output.find("llvm_hosted_arguments.gti:4:") != std::string::npos,
          "negative argc terminates with the original hosted failure and main anchor");
}

void sourceMetadataContract(const std::filesystem::path &repository) {
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      repository / "tests/fixtures/stdlib_surface_source_location.gti",
      std::nullopt, {repository / "stdlib/prelude.gti"}, {},
      {repository / "stdlib"});
  require(frontend.canGenerateCode(),
          "source metadata has verified source MIR");
  std::array<bool, 4> seen{};
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto literal = hostedSourceLiteral(instruction);
        if (!literal)
          continue;
        const auto &location = *instruction.sourceLocation;
        require(location.file.ends_with("stdlib_surface_source_location.gti") &&
                    location.function == "main" &&
                    (location.line == 9 || location.line == 15) &&
                    location.column == 0,
                "caller metadata is not rediscovered from the library body");
        if (instruction.intrinsic == IntrinsicKind::SourceFileName) {
          seen[0] = true;
          require(std::get<std::string>(*literal) == location.file,
                  "file metadata retains exact counted bytes");
        } else if (instruction.intrinsic == IntrinsicKind::SourceFunctionName) {
          seen[1] = true;
          require(std::get<std::string>(*literal) == location.function,
                  "function metadata retains exact counted bytes");
        } else if (instruction.intrinsic == IntrinsicKind::SourceLine) {
          seen[2] = true;
          require(std::get<std::uint64_t>(*literal) == location.line,
                  "line metadata retains its uint32 value");
        } else {
          seen[3] = true;
          require(std::get<std::uint64_t>(*literal) == location.column,
                  "unavailable column remains zero");
        }
        const auto reject = [&](MirInstruction invalid) {
          bool rejected = false;
          try {
            static_cast<void>(hostedSourceLiteral(invalid));
          } catch (const std::logic_error &) {
            rejected = true;
          }
          require(rejected, "malformed source metadata cannot reach lowering");
        };
        auto invalid = instruction;
        invalid.sourceLocation.reset();
        reject(invalid);
        invalid = instruction;
        invalid.info.type = SemanticType::Int64;
        reject(invalid);
        invalid = instruction;
        invalid.kind = MirInstructionKind::Load;
        reject(invalid);
      }
  require(
      std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
      "all four source metadata capabilities have real producer evidence");
  auto invalid = frontend.mir;
  bool removed = false;
  for (auto &function : MirProgramTestAccess::functions(invalid))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!removed && instruction.sourceLocation) {
          instruction.sourceLocation.reset();
          removed = true;
        }
  require(removed && !verifyMirProgram(invalid).valid(),
          "missing source metadata is rejected by the owning MIR verifier");
}

void run(const std::filesystem::path &repository,
         const std::filesystem::path &scratch, const Case &test,
         LlvmCodegenOptimization optimization) {
  const auto source = repository / "tests/fixtures" / test.fixture;
  FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto frontend = Frontend(options).analyze(
      source, std::nullopt, {repository / "stdlib/prelude.gti"}, {},
      {repository / "stdlib"});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(frontend.canGenerateCode(), "hosted source produces valid MIR");
  require(verifyMirProgram(frontend.mir).valid(), "hosted MIR verifies");
  if (test.fixture == "llvm_hosted_union_first_field.gti") {
    const auto rejectsUnion = [&](auto mutation) {
      auto invalid = frontend.mir;
      bool changed = false;
      for (auto &function : MirProgramTestAccess::functions(invalid))
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed || !mir_detail::passiveUnionFieldConstruction(
                               invalid, instruction))
              continue;
            mutation(invalid, instruction);
            require(!mir_detail::passiveUnionFieldConstruction(invalid,
                                                               instruction),
                    "malformed union cannot retain the first-field proof");
            changed = true;
          }
      require(changed && !verifyMirProgram(invalid).valid(),
              "malformed union proof fails before artifact publication");
      auto lowered = LoweredProgramBuilder().build(
          frontend.program, frontend.semantics, frontend.hir, invalid, invalid,
          {}, TargetInfo::host());
      std::vector<LoweredProgramIssue> rejectedIssues;
      require(!lowered.valid() ||
                  !verifiedLoweredProgram(std::move(*lowered.program),
                                          rejectedIssues),
              "malformed union construction cannot cross the sealed boundary");
    };
    rejectsUnion([](auto &, auto &instruction) {
      const auto wrong =
          instruction.parameterTypes.front() == SemanticType::UInt8
              ? SemanticType::UInt64
              : SemanticType::UInt8;
      instruction.parameterTypes.front() = wrong;
    });
    rejectsUnion(
        [](auto &, auto &instruction) { instruction.enumVariant = 0; });
    rejectsUnion([](auto &, auto &instruction) {
      instruction.definedFailure.propagation =
          FailurePropagationKind::Constructor;
    });
  }
  std::vector<LoweredProgramIssue> issues;
  const auto program = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  require(program.has_value(), "hosted source crosses the sealed boundary");
  const auto &startup = program->program().mir().hostedStartupPlan();
  require(startup && startup->kind == ProgramEntryKind::NoArguments,
          "fixture exercises the real no-argument hosted startup plan");
  {
    auto invalid = frontend.mir;
    MirProgramTestAccess::hostedPlan(invalid)->kind =
        ProgramEntryKind::OwnedArguments;
    require(!verifyMirProgram(invalid).valid(),
            "an unsupported argument shape cannot forge the no-argument plan");
    invalid = frontend.mir;
    auto &operations = MirProgramTestAccess::hostedPlan(invalid)->operations;
    const auto drain = std::find_if(
        operations.begin(), operations.end(), [](const auto &operation) {
          return operation.kind ==
                 MirHostedStartupOperationKind::DrainThreadLocals;
        });
    require(drain != operations.end(),
            "startup has an explicit drain operation");
    operations.erase(drain);
    require(!verifyMirProgram(invalid).valid(),
            "missing hosted teardown authority fails MIR verification");
  }
  const LlvmBackendOptions backendOptions{.optimization = optimization};
  const auto admission = planLlvmAdmission(*program, backendOptions);
  if (!admission.admitted())
    std::cerr << admission.limitation() << '\n';
  require(admission.admitted(), "hosted fixture has native admission");

  SymbolId trace = 0;
  unsigned matches = 0;
  for (const auto &symbol : program->program().symbols()) {
    if (symbol.qualifiedName != "trace")
      continue;
    ++matches;
    require(symbol.kind == SymbolKind::GlobalVariable &&
                symbol.type == SemanticType::Int32 && symbol.mutableBinding,
            "trace is an exact sealed mutable scalar global");
    trace = symbol.id;
  }
  require(matches == 1, "trace has one sealed identity");

  Session session(*program, backendOptions);
  const auto returned = std::find_if(
      startup->operations.begin(), startup->operations.end(),
      [](const auto &operation) {
        return operation.kind == MirHostedStartupOperationKind::ReturnEntry;
      });
  require(returned != startup->operations.end(),
          "startup has one exact return operation");
  llvm::IRBuilder<> invalidBuilder(session.context());
  const auto rejectReturn = [&](MirHostedStartupOperation operation,
                                llvm::Value *value) {
    bool rejected = false;
    try {
      static_cast<void>(hostedReturnStatus(invalidBuilder, operation, value));
    } catch (const std::logic_error &) {
      rejected = true;
    }
    require(rejected, "malformed hosted status lowering rejects its input");
  };
  rejectReturn(*returned, invalidBuilder.getInt64(256));
  rejectReturn(*returned, nullptr);
  auto wrongOperation = *returned;
  wrongOperation.kind = MirHostedStartupOperationKind::CallEntry;
  rejectReturn(wrongOperation, invalidBuilder.getInt32(256));
  lowerScalarProgram(session, admission);
  auto &module = session.module();
  if (test.fixture == "llvm_hosted_generated_default.gti") {
    unsigned factories = 0;
    for (const auto &instance : frontend.hir.functionInstances()) {
      if (!instance.source)
        continue;
      const auto name = instance.source->name().lexeme;
      if (name != "make_policy" && name != "make_defaulted" &&
          name != "make_generic")
        continue;
      const auto *function = session.abi().declareFunction(instance.id);
      require(!function->isDeclaration(),
              "generated factory has an emitted body");
      for (const auto &block : *function)
        for (const auto &instruction : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
            require(call->getCalledFunction() &&
                        call->getCalledFunction()->isIntrinsic(),
                    "bodyless factory adds no allocator, dispatch or "
                    "source-constructor call");
      ++factories;
    }
    require(factories == 3,
            "three unrelated bodyless factories retain their cost contract");
  }
  if (test.fixture == "llvm_hosted_union_first_field.gti" ||
      test.fixture == "llvm_hosted_union_first_field_aggregate.gti") {
    unsigned factories = 0;
    for (const auto &instance : frontend.hir.functionInstances()) {
      if (!instance.source)
        continue;
      const auto name = instance.source->name().lexeme;
      if (name != "make_byte" && name != "make_nested_union")
        continue;
      const auto *function = session.abi().declareFunction(instance.id);
      bool fieldStore = false;
      for (const auto &block : *function)
        for (const auto &instruction : block) {
          require(!llvm::isa<llvm::MemSetInst>(instruction),
                  "first-field construction adds no inactive union zeroing");
          if (name == "make_byte") {
            if (const auto *store =
                    llvm::dyn_cast<llvm::StoreInst>(&instruction))
              fieldStore |= store->getValueOperand()->getType()->isIntegerTy(8);
          } else if (const auto *copy =
                         llvm::dyn_cast<llvm::MemCpyInst>(&instruction)) {
            const auto *size =
                llvm::dyn_cast<llvm::ConstantInt>(copy->getLength());
            require(size && size->getZExtValue() == 8,
                    "passive first-field copy uses its field width, not union "
                    "width");
            fieldStore = true;
          }
        }
      require(fieldStore,
              "union factory initializes its exact typed first field");
      ++factories;
    }
    require(factories == 1,
            "the exact scalar or passive aggregate union factory is emitted");
  }
  if (test.fixture == "llvm_hosted_scalar_return.gti") {
    unsigned checkedReturns = 0;
    for (const auto &instance : frontend.hir.functionInstances()) {
      if (!instance.source)
        continue;
      const auto name = instance.source->name().lexeme;
      if (name != "widen_float" && name != "outslot_float")
        continue;
      const auto *function = session.abi().declareFunction(instance.id);
      bool extended = false, published = false;
      for (const auto &block : *function)
        for (const auto &instruction : block) {
          extended |= instruction.getOpcode() == llvm::Instruction::FPExt &&
                      instruction.getType()->isDoubleTy();
          if (name == "widen_float") {
            if (const auto *returned =
                    llvm::dyn_cast<llvm::ReturnInst>(&instruction))
              published |= returned->getReturnValue() &&
                           returned->getReturnValue()->getType()->isDoubleTy();
            require(!llvm::isa<llvm::CallBase>(instruction),
                    "pure scalar return adds no runtime or allocation call");
          } else if (const auto *store =
                         llvm::dyn_cast<llvm::StoreInst>(&instruction))
            published |= store->getValueOperand()->getType()->isDoubleTy();
        }
      require(extended && published, "numeric return converts before direct or "
                                     "full-width outslot publication");
      require(name == "widen_float" ? function->getReturnType()->isDoubleTy()
                                    : function->getReturnType()->isIntegerTy(1),
              "direct and may-fail scalar returns retain their distinct ABI "
              "contracts");
      ++checkedReturns;
    }
    require(checkedReturns == 2,
            "both scalar ABI publication paths are checked");
  }
  auto *global = module.getGlobalVariable(
      "__gti_native_v1_g" + std::to_string(trace), true);
  require(global && global->getValueType()->isIntegerTy(32) &&
              !global->isConstant() && !global->isThreadLocal(),
          "observer sees the exact native trace storage");
  // The source program has no foreign calls. Only this test boundary exposes
  // its trace and observes the documented C failure observer slot.
  llvm::GlobalAlias::create(global->getValueType(), global->getAddressSpace(),
                            llvm::GlobalValue::ExternalLinkage,
                            "gti_hosted_trace", global, &module);
  auto *main = module.getFunction("main");
  require(main && main->arg_empty(), "real hosted entry has no arguments");
  main->setName("gti_hosted_entry");
  auto *pointer = session.layouts().pointer();
  auto *observer = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "gti_hosted_observer", module);
  auto *terminate =
      declareRuntimeService(session, RuntimeService::TerminateFailure);
  const auto expectedObservations = static_cast<unsigned>(std::count_if(
      startup->operations.begin(), startup->operations.end(),
      [](const auto &operation) {
        return operation.kind == MirHostedStartupOperationKind::ContainFailure;
      }));
  unsigned observations = 0;
  for (auto &block : *main)
    for (auto &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      auto *target = call ? call->getCalledFunction() : nullptr;
      if (target != terminate)
        continue;
      require(
          call->getFunctionType() == terminate->getFunctionType() &&
              call->arg_size() == 4 &&
              llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(2)) &&
              llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(3)),
          "containment retains the existing v2 observer contract");
      call->setArgOperand(2, observer);
      ++observations;
    }
  require(observations != 0 && observations == expectedObservations,
          "only the exact planned hosted containment calls are observed");

  const auto directory =
      scratch / std::string(test.fixture) /
      (optimization == LlvmCodegenOptimization::None ? "O0" : "O3");
  std::filesystem::create_directories(directory);
  const auto object = directory / "program.o";
  require(driver::writeArtifact(object, session.emitObject().contents) ==
              driver::ArtifactWriteStatus::Success,
          "verified hosted object is published");
  const auto shim = directory / "observer.c";
  const std::string consumer =
      "#include <gti/runtime_failure.h>\n"
      "#include <gti/runtime_execution.h>\n"
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "extern int32_t gti_hosted_trace;\n"
      "extern int gti_hosted_entry(void);\n"
      "void gti_hosted_observer(const gti_failure_record_v2 *record, void "
      "*context) {\n"
      "  if (!record || context || gti_hosted_trace != " +
      std::to_string(test.trace) +
      ") abort();\n"
      "  fputs(\"hosted observer: cleanup verified\\n\", stderr);\n"
      "  fflush(stderr);\n"
      "}\n"
      "gti_failure_observer_v2 checked_observer = gti_hosted_observer;\n"
      "int main(void) {\n"
      "  if (gti_rt_thread_locals_attached_v1()) return 91;\n"
      "  int result = gti_hosted_entry();\n"
      "  if (gti_hosted_trace != " +
      std::to_string(test.trace) +
      " || gti_rt_thread_locals_attached_v1()) return 92;\n"
      "  return result;\n"
      "}\n";
  require(driver::writeArtifact(shim, consumer) ==
              driver::ArtifactWriteStatus::Success,
          "write independent C hosted observer");
  const auto layout = driver::discoverToolchainLayout(nullptr);
  driver::NativeToolchain toolchain;
  const auto observerObject = directory / "observer.o";
  const auto compiled = toolchain.invoke(driver::NativeCCompileRequest(
      driver::discoverCCompiler(), shim, observerObject, driver::CStandard::C17,
      OptimizationLevel::O2, {layout.runtimeInclude}, {}));
  if (!compiled.succeeded())
    std::cerr << compiled.output;
  require(compiled.succeeded(),
          "C observer matches the actual runtime headers");
  driver::NativeInputs inputs;
  inputs.finalLibraryFiles = {observerObject, layout.runtimeLibrary};
  const auto executable = directory / "program";
  const auto linked = toolchain.invoke(driver::NativeLinkRequest(
      driver::discoverNativeCompiler(), object, executable, inputs));
  if (!linked.succeeded())
    std::cerr << linked.output;
  require(linked.succeeded(), "real hosted entry links to the runtime");
  const auto executionStart = std::chrono::steady_clock::now();
  const auto executed = driver::invokeProcess({executable.string()});
  const auto executionMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - executionStart)
          .count();
  if (test.failure.empty()) {
    require(executed.succeeded() && executed.output.empty(),
            "normal startup and teardown preserve their exact order");
  } else {
    const auto location =
        std::string(test.fixture) + ":" + std::to_string(test.line) + ":";
    if (executed.exitCode != 70)
      std::cerr << executed.output;
    require(executed.exitCode == 70 && !executed.signal && !executed.timedOut &&
                !executed.driverDiagnostic &&
                executed.output.find("hosted observer: cleanup verified") !=
                    std::string::npos &&
                executed.output.find("GTI-R0017") != std::string::npos &&
                executed.output.find(location) != std::string::npos &&
                executed.output.find(test.failure) != std::string::npos,
            "panic containment observes cleanup and original source location");
  }
  if (test.fixture == "llvm_hosted_scalar_return.gti")
    std::cout << "scalar return footprint: object="
              << std::filesystem::file_size(object)
              << " executable=" << std::filesystem::file_size(executable)
              << " bytes; process-wall-us=" << executionMicros
              << "; pure widening adds one FP extension and no calls\n";
  if (test.fixture == "llvm_hosted_union_first_field.gti" ||
      test.fixture == "llvm_hosted_union_first_field_aggregate.gti")
    std::cout << "union first-field footprint: object="
              << std::filesystem::file_size(object)
              << " executable=" << std::filesystem::file_size(executable)
              << " bytes; typed field stores add no inactive-tail zeroing\n";
  if (test.fixture == "llvm_hosted_generated_default.gti")
    std::cout << "generated construction footprint: object="
              << std::filesystem::file_size(object)
              << " executable=" << std::filesystem::file_size(executable)
              << " bytes; three factories have no runtime/source calls\n";
  std::cout << test.fixture << ' '
            << (optimization == LlvmCodegenOptimization::None ? "O0" : "O3")
            << ": native hosted lifecycle verified\n";
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto scratch =
      std::filesystem::temp_directory_path() /
      ("gti-llvm-hosted-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{scratch};
  try {
    unsafeReferenceLoadContract(std::filesystem::canonical(argv[1]));
    unsafeReferenceLoadContract(std::filesystem::canonical(argv[1]), true);
    unsafeReferenceReturnContract(std::filesystem::canonical(argv[1]));
    unsafeReferenceReturnContract(std::filesystem::canonical(argv[1]), true);
    unsafeReferenceReturnContract(std::filesystem::canonical(argv[1]), true,
                                  true);
    unsafeReferenceReturnContract(std::filesystem::canonical(argv[1]), true,
                                  false, SemanticType::CString);
    unsafeReferenceReturnContract(std::filesystem::canonical(argv[1]), true,
                                  true, SemanticType::CString);
    for (const bool raw : {false, true}) {
      unsafeReferenceReturnContract(
          std::filesystem::canonical(argv[1]), true, raw,
          SemanticType::StringView,
          "tests/fixtures/llvm_hosted_class_owner_text_return.gti");
      unsafeReferenceReturnContract(
          std::filesystem::canonical(argv[1]), true, raw, SemanticType::CString,
          "tests/fixtures/llvm_hosted_class_owner_text_return.gti");
    }
    for (const auto fixture :
         {"tests/fixtures/llvm_hosted_unsafe_reference_invoke_readonly.gti",
          "tests/fixtures/llvm_hosted_unsafe_reference_invoke_mutable.gti"})
      unsafeReferenceReturnContract(std::filesystem::canonical(argv[1]), false,
                                    false, SemanticType::StringView, fixture,
                                    true);
    classOwnerTextContract(std::filesystem::canonical(argv[1]));
    classOwnerRawReferenceContract(std::filesystem::canonical(argv[1]),
                                   "llvm_hosted_class_owner_reference.gti");
    classOwnerRawReferenceContract(std::filesystem::canonical(argv[1]),
                                   "llvm_hosted_class_pointee_reference.gti");
    sourceMetadataContract(std::filesystem::canonical(argv[1]));
    scalarReturnContracts();
    scalarReturnPlaceContracts();
    hostedArgumentsContract(std::filesystem::canonical(argv[1]));
    for (auto optimization :
         {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive})
      hostedArgumentsBoundary(std::filesystem::canonical(argv[1]), scratch,
                              optimization);
    for (const auto &test : cases)
      for (auto optimization :
           {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Aggressive})
        run(std::filesystem::canonical(argv[1]), scratch, test, optimization);
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
