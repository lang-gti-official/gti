#include "gti/failure_metadata.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

bool passed = true;

void expect(bool condition, const std::string &message) {
  if (condition) {
    return;
  }
  passed = false;
  std::cerr << "FAIL: " << message << '\n';
}

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "stdlib/prelude.gti";
}

std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

lang::FailureMetadataBuildResult
rebuildFailureMetadata(const lang::FrontendResult &frontend,
                       const lang::HirProgram &hir) {
  const lang::SourceUnit *entry =
      frontend.sourceGraph.findUnit(frontend.sourceGraph.entryUnit());
  return lang::FailureMetadataBuilder().build(
      frontend.sourceGraph, frontend.sources, hir,
      entry == nullptr ? std::filesystem::path{} : entry->path);
}

template <typename Callback>
void forEachInstruction(const lang::MirProgram &program, Callback callback) {
  const auto body = [&](const lang::MirBody &value) {
    for (const lang::MirBlock &block : value.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        callback(instruction);
      }
    }
  };
  body(program.module());
  for (const lang::MirClassInstance &instance : program.classInstances()) {
    body(instance.fieldInitializers);
    body(instance.staticFieldInitializers);
  }
  for (const lang::MirFunctionInstance &instance :
       program.functionInstances()) {
    body(instance.body);
  }
  for (const lang::MirConstructorInstance &instance :
       program.constructorInstances()) {
    body(instance.body);
  }
  for (const lang::MirDestructorInstance &instance :
       program.destructorInstances()) {
    body(instance.body);
  }
  for (const lang::MirLambdaInstance &instance : program.lambdaInstances()) {
    body(instance.body);
  }
}

lang::MirInstruction *firstFailureInstruction(lang::MirProgram &program) {
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      program.functionInstances());
  for (lang::MirFunctionInstance &function : functions) {
    for (lang::MirBlock &block : function.body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return !instruction.definedFailure.localOrigins.empty();
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
  }
  return nullptr;
}

std::optional<lang::HirFunctionInstanceId>
functionInstance(const lang::FrontendResult &frontend, std::string_view name) {
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr && function.source->name().lexeme == name) {
      return function.id;
    }
  }
  return std::nullopt;
}

lang::MirBody *functionBody(lang::MirProgram &program,
                            lang::HirFunctionInstanceId id) {
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      program.functionInstances());
  const auto found =
      std::find_if(functions.begin(), functions.end(),
                   [&](const lang::MirFunctionInstance &function) {
                     return function.id == id;
                   });
  return found == functions.end() ? nullptr : &found->body;
}

const lang::MirBody *functionBody(const lang::FrontendResult &frontend,
                                  std::string_view name) {
  const std::optional<lang::HirFunctionInstanceId> id =
      functionInstance(frontend, name);
  const lang::MirFunctionInstance *function =
      id ? frontend.mir.findFunctionInstance(*id) : nullptr;
  return function == nullptr ? nullptr : &function->body;
}

bool hasVerificationError(const lang::MirVerificationResult &result,
                          std::string_view message) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(message) != std::string::npos;
                     });
}

lang::MirInstruction *functionInstruction(lang::MirProgram &program,
                                          lang::HirFunctionInstanceId function,
                                          lang::MirOperation operation) {
  lang::MirBody *body = functionBody(program, function);
  if (body == nullptr) {
    return nullptr;
  }
  for (lang::MirBlock &block : body->blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Compute &&
                 instruction.operation == operation;
        });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

void testCanonicalMetadataAndMirSites() {
  const std::string source = R"(
int32_t increment<T>(T marker, int32_t value) {
  return value + 1;
}

int main() {
  int32_t left = increment<int32_t>(1, 1);
  int32_t right = increment<int64_t>(int64_t(2), 2);
  mut int32_t values[2] = {left, right};
  return values[0] / right;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("/tmp/gti-failure-a/main.gti", source);
  const lang::FrontendResult relocated =
      lang::Frontend().analyze("/tmp/gti-failure-b/main.gti", source);

  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the failure metadata fixture should reach verified MIR");
  expect(relocated.canGenerateCode() && relocated.diagnostics.empty(),
         "the relocated failure metadata fixture should compile");
  if (!frontend.canGenerateCode() || !relocated.canGenerateCode()) {
    printDiagnostics(frontend);
    printDiagnostics(relocated);
    return;
  }

  const lang::FailureMetadata &metadata = frontend.failureMetadata;
  expect(lang::verifyFailureMetadata(metadata).valid(),
         "the frontend should publish structurally valid failure metadata");
  expect(!metadata.sites().empty() && !metadata.artifactIdentity().isZero() &&
             metadata.artifactIdentity().hex().size() == 64,
         "a checked artifact should have one-based sites and a SHA-256 ID");
  expect(metadata.descriptorBytes() ==
                 relocated.failureMetadata.descriptorBytes() &&
             metadata.artifactIdentity() ==
                 relocated.failureMetadata.artifactIdentity() &&
             metadata.sites() == relocated.failureMetadata.sites(),
         "relocating identical direct compilation inputs must not change the "
         "failure descriptor");
  const bool directNames =
      std::all_of(metadata.sourceUnits().begin(), metadata.sourceUnits().end(),
                  [](const lang::FailureSourceDescriptor &unit) {
                    return unit.logicalName == "main.gti" &&
                           unit.logicalName.find("/tmp/") == std::string::npos;
                  });
  if (!directNames) {
    for (const lang::FailureSourceDescriptor &unit : metadata.sourceUnits()) {
      std::cerr << "  failure source " << unit.sourceUnit << ": "
                << unit.logicalName << '\n';
    }
  }
  expect(directNames,
         "direct compilation metadata should retain only entry-relative "
         "logical names");

  std::size_t localOrigins = 0;
  std::size_t propagationOperations = 0;
  bool exactSites = true;
  forEachInstruction(
      frontend.mir, [&](const lang::MirInstruction &instruction) {
        localOrigins += instruction.definedFailure.localOrigins.size();
        propagationOperations += instruction.definedFailure.propagation !=
                                 lang::FailurePropagationKind::None;
        exactSites =
            exactSites && instruction.localFailureSites.size() ==
                              instruction.definedFailure.localOrigins.size();
        for (std::size_t index = 0;
             index < instruction.definedFailure.localOrigins.size(); ++index) {
          const std::optional<lang::FailureSiteId> expected =
              metadata.siteFor(instruction.definedFailure.localOrigins[index]);
          exactSites = exactSites && expected &&
                       instruction.localFailureSites[index] == *expected;
        }
        if (instruction.definedFailure.localOrigins.empty()) {
          exactSites = exactSites && instruction.localFailureSites.empty();
        }
      });
  if (localOrigins <= metadata.sites().size()) {
    std::cerr << "  local origins: " << localOrigins
              << ", canonical sites: " << metadata.sites().size() << '\n';
  }
  expect(localOrigins > metadata.sites().size(),
         "generic instances should coalesce at their shared definition site");
  expect(propagationOperations != 0 && exactSites &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "MIR detectors should retain exact sites while propagation remains "
         "un-sited");

  const std::string snapshot = lang::MirPrinter().print(frontend.mir);
  const bool validSnapshot =
      snapshot.starts_with("mir-v23 ") &&
      snapshot.find("failure-metadata artifact=" +
                    metadata.artifactIdentity().hex()) != std::string::npos &&
      snapshot.find("failure-site @1 source=8:main.gti") != std::string::npos &&
      snapshot.find(" failure-sites=[") != std::string::npos &&
      snapshot.find("/tmp/gti-failure") == std::string::npos;
  if (!validSnapshot) {
    std::cerr << snapshot;
  }
  expect(validSnapshot,
         "MIR snapshots should expose deterministic artifact sites without "
         "absolute paths");
}

void testInvokeEdgesAndFailureCleanup() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("/tmp/gti-failure-control/main.gti", R"(
class Token {
public:
  int value;
  Token(int input) : value(input) {}
  ~Token() {}
};

int leaf(mut int value) {
  Token outer = Token(1);
  {
    Token inner = Token(2);
    return value + 1;
  }
}

int caller(mut int value) {
  Token local = Token(3);
  return leaf(value);
}

int consume(Token first, Token second, int value) { return value; }

int deferred_nested_argument(mut int value) {
  return consume(Token(4), Token(5), value + 1);
}

int checked_increment(mut int value) { return value + 1; }

int deferred_nested_call(mut int value) {
  return consume(Token(6), Token(7), checked_increment(value));
}

int checked_owned_increment(Token token, mut int value) { return value + 1; }

int deferred_nested_owning_call(mut int value) {
  return consume(Token(8), Token(9), checked_owned_increment(Token(10), value));
}

Token produce(mut int value) {
  int checked = value + 1;
  return Token(checked);
}

Token owning_result(mut int value) { return produce(value); }

int main() { return caller(1); }
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the failure cleanup fixture should reach verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }

  const lang::MirBody *leaf = functionBody(frontend, "leaf");
  const lang::MirBody *caller = functionBody(frontend, "caller");
  const auto inspect = [](const lang::MirBody *body,
                          std::size_t expectedFailureDrops,
                          lang::FailurePropagationKind propagation) {
    if (body == nullptr || body->failureRecords.size() != 1) {
      return false;
    }
    const lang::MirFailureRecord &record = body->failureRecords.front();
    const lang::MirBlock *producer = body->findBlock(record.producerBlock);
    const lang::MirBlock *cleanup = body->findBlock(record.parameterBlock);
    if (producer == nullptr || cleanup == nullptr ||
        producer->terminator.kind != lang::MirTerminatorKind::Invoke ||
        producer->terminator.failureRecord != record.id ||
        cleanup->failureParameter != record.id ||
        cleanup->terminator.kind != lang::MirTerminatorKind::PropagateFailure ||
        cleanup->terminator.failureRecord != record.id ||
        producer->instructions.empty() ||
        producer->instructions.back().id != record.producerInstruction ||
        producer->instructions.back().definedFailure.propagation !=
            propagation) {
      return false;
    }
    std::vector<std::size_t> constructionOrder;
    for (const lang::MirInstruction &instruction : cleanup->instructions) {
      if (instruction.kind != lang::MirInstructionKind::Drop) {
        continue;
      }
      if (instruction.lifecycle.size() != 1 ||
          !instruction.lifecycle.front().failureCleanup) {
        return false;
      }
      const lang::MirDropObligation *obligation =
          body->findDropObligation(instruction.lifecycle.front().source);
      if (obligation == nullptr) {
        return false;
      }
      constructionOrder.push_back(obligation->constructionOrder);
    }
    return constructionOrder.size() == expectedFailureDrops &&
           std::is_sorted(constructionOrder.begin(), constructionOrder.end(),
                          std::greater<>());
  };
  expect(inspect(leaf, 2, lang::FailurePropagationKind::None),
         "a local scalar failure should receive an exact record and clean "
         "nested locals in reverse construction order");
  expect(inspect(caller, 1, lang::FailurePropagationKind::DirectCall),
         "a propagating scalar call should preserve its record while cleaning "
         "the caller's local state");

  const lang::MirBody *deferred =
      functionBody(frontend, "deferred_nested_argument");
  std::vector<lang::MirDropObligationId> deferredPreparedParameters;
  const lang::MirInstruction *nestedDetector = nullptr;
  const lang::MirInstruction *deferredCall = nullptr;
  const lang::MirFailureRecord *nestedRecord = nullptr;
  const lang::MirFailureRecord *deferredCallRecord = nullptr;
  if (deferred != nullptr) {
    for (const lang::MirBlock &block : deferred->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.preparedParameterDrop) {
          deferredPreparedParameters.push_back(
              *instruction.preparedParameterDrop);
        }
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            !instruction.definedFailure.localOrigins.empty() &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::None) {
          nestedDetector = &instruction;
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.operands.size() == 3) {
          deferredCall = &instruction;
        }
      }
    }
    for (const lang::MirFailureRecord &record : deferred->failureRecords) {
      const lang::MirBlock *producer =
          deferred->findBlock(record.producerBlock);
      const lang::MirInstruction *instruction =
          producer == nullptr || producer->instructions.empty()
              ? nullptr
              : &producer->instructions.back();
      if (instruction == nestedDetector) {
        nestedRecord = &record;
      }
      if (instruction == deferredCall) {
        deferredCallRecord = &record;
      }
    }
  }
  std::vector<lang::MirDropObligationId> nestedFailureDrops;
  if (deferred != nullptr && nestedRecord != nullptr) {
    const lang::MirBlock *cleanup =
        deferred->findBlock(nestedRecord->parameterBlock);
    if (cleanup != nullptr) {
      for (const lang::MirInstruction &instruction : cleanup->instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1 ||
            !instruction.lifecycle.front().failureCleanup) {
          continue;
        }
        const lang::MirDropObligation *obligation =
            deferred->findDropObligation(instruction.lifecycle.front().source);
        if (obligation != nullptr &&
            obligation->kind ==
                lang::MirDropObligationKind::PreparedParameter) {
          nestedFailureDrops.push_back(obligation->id);
        }
      }
    }
  }
  const bool reversePreparedCleanup =
      nestedFailureDrops.size() == deferredPreparedParameters.size() &&
      std::equal(nestedFailureDrops.begin(), nestedFailureDrops.end(),
                 deferredPreparedParameters.rbegin());
  const bool allPreparedTransferAtCall =
      deferredCall != nullptr &&
      std::all_of(
          deferredPreparedParameters.begin(), deferredPreparedParameters.end(),
          [&](lang::MirDropObligationId prepared) {
            return std::count_if(
                       deferredCall->lifecycle.begin(),
                       deferredCall->lifecycle.end(),
                       [&](const lang::MirLifecycleEvent &event) {
                         return event.kind ==
                                    lang::MirLifecycleEventKind::TransferOut &&
                                event.source == prepared;
                       }) == 1;
          });
  bool nestedFeedsFinalArgument = false;
  if (deferred != nullptr && nestedDetector != nullptr &&
      nestedDetector->result && deferredCall != nullptr &&
      deferredCall->operands.size() == 3) {
    const lang::MirValue *argumentValue =
        deferred->findValue(deferredCall->operands[2].value);
    const lang::MirBlock *argumentBlock =
        argumentValue == nullptr
            ? nullptr
            : deferred->findBlock(argumentValue->definitionBlock);
    const auto argumentInput =
        argumentBlock == nullptr
            ? std::vector<lang::MirInstruction>::const_iterator{}
            : std::find_if(argumentBlock->instructions.begin(),
                           argumentBlock->instructions.end(),
                           [&](const lang::MirInstruction &instruction) {
                             return instruction.id == argumentValue->definition;
                           });
    nestedFeedsFinalArgument =
        argumentBlock != nullptr &&
        argumentInput != argumentBlock->instructions.end() &&
        argumentInput->kind == lang::MirInstructionKind::CallInput &&
        argumentInput->callInputIndex == 2 &&
        argumentInput->operands.size() == 1 &&
        argumentInput->operands.front().kind == lang::MirOperandKind::Value &&
        argumentInput->operands.front().value == *nestedDetector->result;
  }
  expect(deferred != nullptr && deferred->failureRecords.size() == 1 &&
             deferredPreparedParameters.size() == 2 &&
             nestedDetector != nullptr && nestedRecord != nullptr &&
             deferredCall != nullptr && deferredCallRecord == nullptr &&
             deferredCall->definedFailure.empty() && nestedFeedsFinalArgument &&
             reversePreparedCleanup && allPreparedTransferAtCall,
         "a later scalar argument failure should branch before the callee, "
         "drop earlier prepared owners in reverse order, reserve transfer for "
         "the normal call path, and leave no stale record on the proved-"
         "failure-free outer call");

  const lang::MirBody *deferredPropagation =
      functionBody(frontend, "deferred_nested_call");
  std::vector<lang::MirDropObligationId> propagationPreparedParameters;
  const lang::MirInstruction *nestedCall = nullptr;
  const lang::MirInstruction *propagationOuterCall = nullptr;
  const lang::MirFailureRecord *nestedCallRecord = nullptr;
  const lang::MirFailureRecord *propagationOuterRecord = nullptr;
  if (deferredPropagation != nullptr) {
    for (const lang::MirBlock &block : deferredPropagation->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.preparedParameterDrop) {
          propagationPreparedParameters.push_back(
              *instruction.preparedParameterDrop);
        }
        if (instruction.kind != lang::MirInstructionKind::Call) {
          continue;
        }
        if (instruction.operands.size() == 1 &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::DirectCall) {
          nestedCall = &instruction;
        } else if (instruction.operands.size() == 3) {
          propagationOuterCall = &instruction;
        }
      }
    }
    for (const lang::MirFailureRecord &record :
         deferredPropagation->failureRecords) {
      if (nestedCall != nullptr &&
          record.producerInstruction == nestedCall->id) {
        nestedCallRecord = &record;
      }
      if (propagationOuterCall != nullptr &&
          record.producerInstruction == propagationOuterCall->id) {
        propagationOuterRecord = &record;
      }
    }
  }
  std::vector<lang::MirDropObligationId> nestedCallFailureDrops;
  bool exactPropagatedRecord = false;
  if (deferredPropagation != nullptr && nestedCall != nullptr &&
      nestedCallRecord != nullptr) {
    const lang::MirBlock *producer =
        deferredPropagation->findBlock(nestedCallRecord->producerBlock);
    const lang::MirBlock *cleanup =
        deferredPropagation->findBlock(nestedCallRecord->parameterBlock);
    exactPropagatedRecord =
        producer != nullptr && cleanup != nullptr &&
        nestedCall->definedFailure.localOrigins.empty() &&
        nestedCall->localFailureSites.empty() &&
        nestedCall->lifecycle.empty() && !nestedCall->successResultDrop &&
        producer->terminator.kind == lang::MirTerminatorKind::Invoke &&
        producer->terminator.failureRecord == nestedCallRecord->id &&
        cleanup->failureParameter == nestedCallRecord->id &&
        cleanup->terminator.kind == lang::MirTerminatorKind::PropagateFailure &&
        cleanup->terminator.failureRecord == nestedCallRecord->id;
    if (cleanup != nullptr) {
      for (const lang::MirInstruction &instruction : cleanup->instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1 ||
            !instruction.lifecycle.front().failureCleanup) {
          continue;
        }
        const lang::MirDropObligation *obligation =
            deferredPropagation->findDropObligation(
                instruction.lifecycle.front().source);
        if (obligation != nullptr &&
            obligation->kind ==
                lang::MirDropObligationKind::PreparedParameter) {
          nestedCallFailureDrops.push_back(obligation->id);
        }
      }
    }
  }
  bool nestedCallFeedsOuterArgument = false;
  if (deferredPropagation != nullptr && nestedCall != nullptr &&
      nestedCall->result && propagationOuterCall != nullptr &&
      propagationOuterCall->operands.size() == 3 &&
      propagationOuterCall->operands[2].kind == lang::MirOperandKind::Value) {
    const lang::MirValue *argumentValue =
        deferredPropagation->findValue(propagationOuterCall->operands[2].value);
    const lang::MirBlock *argumentBlock =
        argumentValue == nullptr
            ? nullptr
            : deferredPropagation->findBlock(argumentValue->definitionBlock);
    const auto argumentInput =
        argumentBlock == nullptr
            ? std::vector<lang::MirInstruction>::const_iterator{}
            : std::find_if(argumentBlock->instructions.begin(),
                           argumentBlock->instructions.end(),
                           [&](const lang::MirInstruction &instruction) {
                             return instruction.id == argumentValue->definition;
                           });
    nestedCallFeedsOuterArgument =
        argumentBlock != nullptr &&
        argumentInput != argumentBlock->instructions.end() &&
        argumentInput->kind == lang::MirInstructionKind::CallInput &&
        argumentInput->callInputIndex == 2 &&
        argumentInput->operands.size() == 1 &&
        argumentInput->operands.front().kind == lang::MirOperandKind::Value &&
        argumentInput->operands.front().value == *nestedCall->result;
  }
  const bool reversePropagationCleanup =
      nestedCallFailureDrops.size() == propagationPreparedParameters.size() &&
      std::equal(nestedCallFailureDrops.begin(), nestedCallFailureDrops.end(),
                 propagationPreparedParameters.rbegin());
  const bool propagationTransfersOnlyAtOuterCall =
      propagationOuterCall != nullptr && nestedCall != nullptr &&
      nestedCall->lifecycle.empty() &&
      std::all_of(
          propagationPreparedParameters.begin(),
          propagationPreparedParameters.end(),
          [&](lang::MirDropObligationId prepared) {
            return std::count_if(
                       propagationOuterCall->lifecycle.begin(),
                       propagationOuterCall->lifecycle.end(),
                       [&](const lang::MirLifecycleEvent &event) {
                         return event.kind ==
                                    lang::MirLifecycleEventKind::TransferOut &&
                                event.source == prepared;
                       }) == 1;
          });
  expect(deferredPropagation != nullptr &&
             deferredPropagation->failureRecords.size() == 1 &&
             propagationPreparedParameters.size() == 2 &&
             nestedCall != nullptr && nestedCallRecord != nullptr &&
             propagationOuterCall != nullptr &&
             propagationOuterRecord == nullptr &&
             propagationOuterCall->definedFailure.empty() &&
             exactPropagatedRecord && nestedCallFeedsOuterArgument &&
             reversePropagationCleanup && propagationTransfersOnlyAtOuterCall,
         "a nested direct scalar call should propagate its exact record, "
         "clean earlier prepared owners, feed one normal outer argument, and "
         "leave no stale record on the proved-failure-free outer call");

  const lang::MirBody *deferredOwningPropagation =
      functionBody(frontend, "deferred_nested_owning_call");
  const lang::MirInstruction *nestedOwningCall = nullptr;
  bool nestedOwningCallHasRecord = false;
  if (deferredOwningPropagation != nullptr) {
    for (const lang::MirBlock &block : deferredOwningPropagation->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.operands.size() == 2 &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::DirectCall) {
          nestedOwningCall = &instruction;
        }
      }
    }
    nestedOwningCallHasRecord =
        nestedOwningCall != nullptr &&
        std::any_of(deferredOwningPropagation->failureRecords.begin(),
                    deferredOwningPropagation->failureRecords.end(),
                    [&](const lang::MirFailureRecord &record) {
                      return record.producerInstruction == nestedOwningCall->id;
                    });
  }
  expect(
      deferredOwningPropagation != nullptr && nestedOwningCall != nullptr &&
          !nestedOwningCall->lifecycle.empty() && !nestedOwningCallHasRecord,
      "the bounded nested-call slice should not claim calls that stage their "
      "own owning parameters");

  const lang::MirBody *owningResult = functionBody(frontend, "owning_result");
  bool exactOwningResultEdge = false;
  if (owningResult != nullptr && owningResult->failureRecords.size() == 1) {
    const lang::MirFailureRecord &record = owningResult->failureRecords.front();
    const lang::MirBlock *producer =
        owningResult->findBlock(record.producerBlock);
    const lang::MirBlock *cleanup =
        owningResult->findBlock(record.parameterBlock);
    const lang::MirInstruction *call =
        producer == nullptr || producer->instructions.empty()
            ? nullptr
            : &producer->instructions.back();
    const lang::MirDropObligation *result =
        call == nullptr || !call->successResultDrop
            ? nullptr
            : owningResult->findDropObligation(*call->successResultDrop);
    const bool failureDropsResult =
        cleanup != nullptr && result != nullptr &&
        std::any_of(cleanup->instructions.begin(), cleanup->instructions.end(),
                    [&](const lang::MirInstruction &instruction) {
                      return instruction.kind ==
                                 lang::MirInstructionKind::Drop &&
                             instruction.lifecycle.size() == 1 &&
                             instruction.lifecycle.front().source == result->id;
                    });
    exactOwningResultEdge =
        producer != nullptr && cleanup != nullptr && call != nullptr &&
        result != nullptr &&
        producer->terminator.kind == lang::MirTerminatorKind::Invoke &&
        producer->terminator.successLifecycle.size() == 1 &&
        producer->terminator.successLifecycle.front().kind ==
            lang::MirLifecycleEventKind::Initialize &&
        producer->terminator.successLifecycle.front().target == result->id &&
        result->kind == lang::MirDropObligationKind::Value &&
        result->dropType.requiresActiveCleanup && !failureDropsResult;
  }
  expect(exactOwningResultEdge,
         "a cleanup-owning call result should initialize only on the invoke "
         "success edge and remain absent from failure cleanup");

  const std::string snapshot = lang::MirPrinter().print(frontend.mir);
  expect(snapshot.find("failure-records 1") != std::string::npos &&
             snapshot.find("failure-parameter=fail1") != std::string::npos &&
             snapshot.find("failure-cleanup=1") != std::string::npos,
         "MIR snapshots should expose fixed records and failure cleanup");

  const std::optional<lang::HirFunctionInstanceId> leafId =
      functionInstance(frontend, "leaf");
  if (!leafId) {
    expect(false, "the failure cleanup fixture should retain leaf identity");
    return;
  }

  lang::MirProgram missingInvoke = frontend.mir;
  lang::MirBody *missingInvokeLeaf = functionBody(missingInvoke, *leafId);
  if (missingInvokeLeaf != nullptr &&
      !missingInvokeLeaf->failureRecords.empty()) {
    const lang::MirFailureRecord &record =
        missingInvokeLeaf->failureRecords.front();
    lang::MirBlock &producer =
        missingInvokeLeaf->blocks[record.producerBlock - 1];
    producer.terminator = {.kind = lang::MirTerminatorKind::Goto,
                           .target = producer.terminator.target};
    lang::rebuildMirReachability(*missingInvokeLeaf);
    (void)lang::rebuildMirValueUses(*missingInvokeLeaf);
  }
  const lang::MirVerificationResult missingInvokeResult =
      lang::verifyMirProgram(missingInvoke);
  expect(!missingInvokeResult.valid() &&
             hasVerificationError(missingInvokeResult, "one invoke"),
         "MIR verification should reject a removed failure invoke");

  lang::MirProgram rewrittenRecord = frontend.mir;
  lang::MirBody *rewrittenRecordLeaf = functionBody(rewrittenRecord, *leafId);
  if (rewrittenRecordLeaf != nullptr &&
      !rewrittenRecordLeaf->failureRecords.empty()) {
    const lang::MirFailureRecord &record =
        rewrittenRecordLeaf->failureRecords.front();
    rewrittenRecordLeaf->blocks[record.parameterBlock - 1]
        .terminator.failureRecord = 0;
  }
  const lang::MirVerificationResult rewrittenRecordResult =
      lang::verifyMirProgram(rewrittenRecord);
  expect(!rewrittenRecordResult.valid() &&
             hasVerificationError(rewrittenRecordResult,
                                  "preserve its exact fixed record"),
         "MIR verification should reject a rewritten propagated record");

  lang::MirProgram reorderedCleanup = frontend.mir;
  lang::MirBody *reorderedCleanupLeaf = functionBody(reorderedCleanup, *leafId);
  if (reorderedCleanupLeaf != nullptr &&
      !reorderedCleanupLeaf->failureRecords.empty()) {
    const lang::MirFailureRecord &record =
        reorderedCleanupLeaf->failureRecords.front();
    std::vector<lang::MirInstruction> &instructions =
        reorderedCleanupLeaf->blocks[record.parameterBlock - 1].instructions;
    std::vector<std::size_t> drops;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].kind == lang::MirInstructionKind::Drop) {
        drops.push_back(index);
      }
    }
    if (drops.size() >= 2) {
      std::swap(instructions[drops[0]], instructions[drops[1]]);
    }
    (void)lang::rebuildMirValueUses(*reorderedCleanupLeaf);
  }
  const lang::MirVerificationResult reorderedCleanupResult =
      lang::verifyMirProgram(reorderedCleanup);
  expect(!reorderedCleanupResult.valid() &&
             hasVerificationError(reorderedCleanupResult, "cleanup sequence"),
         "MIR verification should reject reordered failure cleanup");

  const std::optional<lang::HirFunctionInstanceId> owningResultId =
      functionInstance(frontend, "owning_result");
  lang::MirProgram missingSuccessInitialization = frontend.mir;
  lang::MirBody *missingSuccessInitializationBody =
      owningResultId
          ? functionBody(missingSuccessInitialization, *owningResultId)
          : nullptr;
  if (missingSuccessInitializationBody != nullptr &&
      !missingSuccessInitializationBody->failureRecords.empty()) {
    const lang::MirFailureRecord &record =
        missingSuccessInitializationBody->failureRecords.front();
    missingSuccessInitializationBody->blocks[record.producerBlock - 1]
        .terminator.successLifecycle.clear();
  }
  const lang::MirVerificationResult missingSuccessInitializationResult =
      lang::verifyMirProgram(missingSuccessInitialization);
  expect(!missingSuccessInitializationResult.valid() &&
             hasVerificationError(missingSuccessInitializationResult,
                                  "success edge"),
         "MIR verification should reject an owning call result omitted from "
         "the invoke success edge");

  const std::optional<lang::HirFunctionInstanceId> deferredId =
      functionInstance(frontend, "deferred_nested_argument");
  lang::MirProgram forgedArgumentRelation = frontend.mir;
  lang::MirBody *forgedArgumentBody =
      deferredId ? functionBody(forgedArgumentRelation, *deferredId) : nullptr;
  bool rewiredArgument = false;
  if (forgedArgumentBody != nullptr) {
    lang::MirInstruction *detector = nullptr;
    for (lang::MirBlock &block : forgedArgumentBody->blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            !instruction.definedFailure.localOrigins.empty() &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::None) {
          detector = &instruction;
          break;
        }
      }
      if (detector != nullptr) {
        break;
      }
    }
    if (detector != nullptr && detector->result) {
      const auto replacement =
          std::find_if(detector->operands.begin(), detector->operands.end(),
                       [](const lang::MirOperand &operand) {
                         return operand.kind == lang::MirOperandKind::Value;
                       });
      for (lang::MirBlock &block : forgedArgumentBody->blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (replacement != detector->operands.end() &&
              instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputIndex == 2 &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().kind ==
                  lang::MirOperandKind::Value &&
              instruction.operands.front().value == *detector->result) {
            instruction.operands.front() = *replacement;
            rewiredArgument = true;
            break;
          }
        }
        if (rewiredArgument) {
          break;
        }
      }
    }
    (void)lang::rebuildMirValueUses(*forgedArgumentBody);
  }
  const lang::MirVerificationResult forgedArgumentRelationResult =
      lang::verifyMirProgram(forgedArgumentRelation);
  expect(rewiredArgument && !forgedArgumentRelationResult.valid() &&
             hasVerificationError(forgedArgumentRelationResult,
                                  "eligible invoke edge"),
         "MIR verification should reject a nested invoke detached from its "
         "exact indexed call argument");

  lang::MirProgram missingPreparedCleanup = frontend.mir;
  lang::MirBody *missingPreparedCleanupBody =
      deferredId ? functionBody(missingPreparedCleanup, *deferredId) : nullptr;
  bool removedPreparedCleanup = false;
  if (missingPreparedCleanupBody != nullptr) {
    lang::MirBlock *cleanup = nullptr;
    for (const lang::MirFailureRecord &record :
         missingPreparedCleanupBody->failureRecords) {
      const lang::MirBlock *producer =
          missingPreparedCleanupBody->findBlock(record.producerBlock);
      const lang::MirInstruction *instruction =
          producer == nullptr || producer->instructions.empty()
              ? nullptr
              : &producer->instructions.back();
      if (instruction != nullptr &&
          instruction->kind == lang::MirInstructionKind::Compute &&
          !instruction->definedFailure.localOrigins.empty()) {
        cleanup =
            &missingPreparedCleanupBody->blocks[record.parameterBlock - 1];
        break;
      }
    }
    if (cleanup != nullptr) {
      const auto drop = std::find_if(
          cleanup->instructions.begin(), cleanup->instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            if (instruction.kind != lang::MirInstructionKind::Drop ||
                instruction.lifecycle.size() != 1) {
              return false;
            }
            const lang::MirDropObligation *obligation =
                missingPreparedCleanupBody->findDropObligation(
                    instruction.lifecycle.front().source);
            return obligation != nullptr &&
                   obligation->kind ==
                       lang::MirDropObligationKind::PreparedParameter;
          });
      if (drop != cleanup->instructions.end()) {
        const lang::MirDropObligationId removed =
            drop->lifecycle.front().source;
        cleanup->instructions.erase(drop);
        for (lang::MirCleanupBoundary &boundary :
             missingPreparedCleanupBody->cleanupBoundaries) {
          if (boundary.kind == lang::MirCleanupBoundaryKind::Failure) {
            std::erase(boundary.obligations, removed);
          }
        }
        removedPreparedCleanup = true;
      }
    }
    (void)lang::rebuildMirValueUses(*missingPreparedCleanupBody);
  }
  const lang::MirVerificationResult missingPreparedCleanupResult =
      lang::verifyMirProgram(missingPreparedCleanup);
  expect(removedPreparedCleanup && !missingPreparedCleanupResult.valid() &&
             hasVerificationError(missingPreparedCleanupResult,
                                  "nested call-argument failure cleanup"),
         "MIR verification should reject a nested argument failure that omits "
         "an earlier prepared owner from cleanup");

  const std::optional<lang::HirFunctionInstanceId> deferredPropagationId =
      functionInstance(frontend, "deferred_nested_call");
  lang::MirProgram forgedNestedCallRelation = frontend.mir;
  lang::MirBody *forgedNestedCallBody =
      deferredPropagationId
          ? functionBody(forgedNestedCallRelation, *deferredPropagationId)
          : nullptr;
  bool detachedNestedCallResult = false;
  if (forgedNestedCallBody != nullptr) {
    lang::MirInstruction *propagatingCall = nullptr;
    for (lang::MirBlock &block : forgedNestedCallBody->blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.operands.size() == 1 &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::DirectCall) {
          propagatingCall = &instruction;
          break;
        }
      }
      if (propagatingCall != nullptr) {
        break;
      }
    }
    const lang::MirValue *nestedInputValue =
        propagatingCall == nullptr || propagatingCall->operands.empty() ||
                propagatingCall->operands.front().kind !=
                    lang::MirOperandKind::Value
            ? nullptr
            : forgedNestedCallBody->findValue(
                  propagatingCall->operands.front().value);
    lang::MirInstruction *nestedInput = nullptr;
    if (nestedInputValue != nullptr) {
      lang::MirBlock *definitionBlock =
          &forgedNestedCallBody->blocks[nestedInputValue->definitionBlock - 1];
      const auto definition =
          std::find_if(definitionBlock->instructions.begin(),
                       definitionBlock->instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == nestedInputValue->definition;
                       });
      if (definition != definitionBlock->instructions.end() &&
          definition->kind == lang::MirInstructionKind::CallInput) {
        nestedInput = &*definition;
      }
    }
    if (propagatingCall != nullptr && propagatingCall->result &&
        nestedInput != nullptr && nestedInput->operands.size() == 1 &&
        nestedInput->operands.front().kind == lang::MirOperandKind::Value) {
      for (lang::MirBlock &block : forgedNestedCallBody->blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputIndex == 2 &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().kind ==
                  lang::MirOperandKind::Value &&
              instruction.operands.front().value == *propagatingCall->result) {
            instruction.operands.front() = nestedInput->operands.front();
            detachedNestedCallResult = true;
            break;
          }
        }
        if (detachedNestedCallResult) {
          break;
        }
      }
    }
    (void)lang::rebuildMirValueUses(*forgedNestedCallBody);
  }
  const lang::MirVerificationResult forgedNestedCallRelationResult =
      lang::verifyMirProgram(forgedNestedCallRelation);
  expect(detachedNestedCallResult && !forgedNestedCallRelationResult.valid() &&
             hasVerificationError(forgedNestedCallRelationResult,
                                  "eligible invoke edge"),
         "MIR verification should reject nested call propagation detached "
         "from its exact outer argument input");
}

void testEmptyDescriptorContract() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-empty/main.gti", "int main() { return 0; }");
  expect(frontend.canGenerateCode() && frontend.failureMetadata.sites().empty(),
         "a failure-free artifact should still publish an empty descriptor");
  expect(frontend.failureMetadata.artifactIdentity().hex() ==
             "4ca6c6a620c8410ce1f804123e28329ae1e10a2fca4c6d975e502246a213a721",
         "the empty descriptor must retain the normative prefix digest");
}

void testHostedProgramEntryMetadata() {
  const lang::FrontendResult noEntry = lang::Frontend().analyze(
      "/tmp/gti-hosted-metadata/no-entry.gti", "int helper() { return 0; }\n");
  const lang::FrontendResult noArguments =
      lang::Frontend().analyze("/tmp/gti-hosted-metadata/no-arguments.gti",
                               "int main() { return 0; }\n");
  expect(noEntry.canGenerateCode() && !noEntry.hir.hostedProgramEntryPlan() &&
             noEntry.failureMetadata.sites().empty() &&
             noEntry.failureMetadata.assignments().empty(),
         "a program without an entry point should add no hosted failure "
         "metadata");
  expect(noArguments.canGenerateCode() &&
             noArguments.hir.hostedProgramEntryPlan() &&
             noArguments.hir.hostedProgramEntryPlan()->kind ==
                 lang::ProgramEntryKind::NoArguments &&
             noArguments.failureMetadata.sites().empty() &&
             noArguments.failureMetadata.assignments().empty(),
         "a no-argument entry point should add no hosted failure metadata");

  const std::string source = R"(
#include <std/string>
#include <std/vector>
using arguments = std::vector<std::string>;
int main(mut int count, mut arguments values) { return count; }
)";
  const auto analyzeAt = [&](const std::filesystem::path &entry) {
    return lang::Frontend().analyze(entry, source, {standardLibraryPrelude()},
                                    {}, {standardLibraryRoot()});
  };
  const lang::FrontendResult owned =
      analyzeAt("/tmp/gti-hosted-metadata-a/owned-main.gti");
  const lang::FrontendResult relocated =
      analyzeAt("/tmp/gti-hosted-metadata-b/owned-main.gti");
  expect(owned.canGenerateCode() && owned.diagnostics.empty(),
         "the owned hosted-metadata fixture should reach verified MIR");
  expect(relocated.canGenerateCode() && relocated.diagnostics.empty(),
         "the relocated owned hosted-metadata fixture should compile");
  if (!owned.canGenerateCode() || !relocated.canGenerateCode()) {
    printDiagnostics(owned);
    printDiagnostics(relocated);
    return;
  }

  const std::optional<lang::HirHostedProgramEntryPlan> &hosted =
      owned.hir.hostedProgramEntryPlan();
  const std::vector<lang::DefinedFailureOutcome> expectedOutcomes{
      {.code = lang::DefinedFailureCode::NumericConversionOutOfRange,
       .detail = lang::DefinedFailureDetail::HostedArgumentCount},
      {.code = lang::DefinedFailureCode::HostedRuntimeContractFailure,
       .detail = lang::DefinedFailureDetail::NegativeArgumentCount}};
  const lang::DefinedFailureOrigin *validate =
      hosted && hosted->validateCount.localOrigins.size() == 1
          ? &hosted->validateCount.localOrigins.front()
          : nullptr;
  const lang::DefinedFailureOrigin *convert =
      hosted && hosted->convertCount.localOrigins.size() == 1
          ? &hosted->convertCount.localOrigins.front()
          : nullptr;
  const std::optional<lang::FailureSiteId> validateSite =
      validate == nullptr ? std::nullopt
                          : owned.failureMetadata.siteFor(*validate);
  const std::optional<lang::FailureSiteId> convertSite =
      convert == nullptr ? std::nullopt
                         : owned.failureMetadata.siteFor(*convert);
  const lang::FailureSiteDescriptor *site =
      validateSite ? owned.failureMetadata.findSite(*validateSite) : nullptr;
  const auto assignment =
      !hosted
          ? owned.failureMetadata.assignments().end()
          : std::find_if(owned.failureMetadata.assignments().begin(),
                         owned.failureMetadata.assignments().end(),
                         [&](const lang::FailureOriginAssignment &candidate) {
                           return candidate.sourceUnit == hosted->sourceUnit &&
                                  candidate.start == hosted->mainAnchor.start &&
                                  candidate.end == hosted->mainAnchor.end;
                         });
  const bool noHostedArgumentAllocation = std::none_of(
      owned.failureMetadata.sites().begin(),
      owned.failureMetadata.sites().end(),
      [](const lang::FailureSiteDescriptor &candidate) {
        return std::any_of(candidate.outcomes.begin(), candidate.outcomes.end(),
                           [](lang::DefinedFailureOutcome outcome) {
                             return outcome.detail ==
                                    lang::DefinedFailureDetail::HostedArguments;
                           });
      });
  expect(hosted && hosted->kind == lang::ProgramEntryKind::OwnedArguments &&
             validate != nullptr && convert != nullptr && validateSite &&
             convertSite && *validateSite != 0 &&
             *validateSite == *convertSite && site != nullptr &&
             site->line == hosted->mainAnchor.line &&
             site->start == hosted->mainAnchor.start &&
             site->end == hosted->mainAnchor.end &&
             site->outcomes == expectedOutcomes &&
             assignment != owned.failureMetadata.assignments().end() &&
             assignment->site == *validateSite &&
             assignment->outcomes == expectedOutcomes &&
             noHostedArgumentAllocation,
         "owned startup should intern its two exact main-anchored detectors "
         "into one site without manufacturing a hosted-arguments allocation "
         "origin");
  expect(owned.failureMetadata.descriptorBytes() ==
                 relocated.failureMetadata.descriptorBytes() &&
             owned.failureMetadata.artifactIdentity() ==
                 relocated.failureMetadata.artifactIdentity() &&
             owned.failureMetadata.sites() == relocated.failureMetadata.sites(),
         "owned startup metadata and its digest should be deterministic "
         "across relocation");
  expect(static_cast<std::uint16_t>(
             lang::DefinedFailureDetail::HostedArguments) == 28 &&
             lang::validDefinedFailureOutcome(
                 {.code = lang::DefinedFailureCode::AllocationFailure,
                  .detail = lang::DefinedFailureDetail::HostedArguments}),
         "hosted_arguments should retain its stable accepted vocabulary "
         "ordinal while remaining unproduced by current startup");

  const auto expectHostedBuilderRejection =
      [&](std::string_view label,
          const std::function<void(lang::HirHostedProgramEntryPlan &)>
              &mutate) {
        lang::HirProgram corrupted = owned.hir;
        auto &plan =
            const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
                corrupted.hostedProgramEntryPlan());
        if (plan) {
          mutate(*plan);
        }
        const lang::FailureMetadataBuildResult result =
            rebuildFailureMetadata(owned, corrupted);
        expect(plan && !result.valid(),
               "hosted failure metadata should reject " + std::string(label));
      };
  expectHostedBuilderRejection("a missing count-validation detector",
                               [](auto &plan) { plan.validateCount = {}; });
  expectHostedBuilderRejection("a missing source unit", [](auto &plan) {
    plan.sourceUnit = 0;
    plan.validateCount.localOrigins.front().sourceUnit = 0;
    plan.convertCount.localOrigins.front().sourceUnit = 0;
  });
  expectHostedBuilderRejection("an unknown source unit", [](auto &plan) {
    constexpr lang::SourceUnitId unknownUnit = 999999;
    plan.sourceUnit = unknownUnit;
    plan.validateCount.localOrigins.front().sourceUnit = unknownUnit;
    plan.convertCount.localOrigins.front().sourceUnit = unknownUnit;
  });
  expectHostedBuilderRejection("a drifted source span", [](auto &plan) {
    ++plan.validateCount.localOrigins.front().start;
  });
  expectHostedBuilderRejection("a missing source span", [](auto &plan) {
    plan.validateCount.localOrigins.front().start = 0;
    plan.validateCount.localOrigins.front().end = 0;
  });
  expectHostedBuilderRejection("a drifted source line", [](auto &plan) {
    ++plan.validateCount.localOrigins.front().line;
  });
  expectHostedBuilderRejection("a missing source line", [](auto &plan) {
    plan.validateCount.localOrigins.front().line = 0;
  });
  expectHostedBuilderRejection("a wrong detector outcome", [](auto &plan) {
    plan.convertCount.localOrigins.front().outcomes.front().detail =
        lang::DefinedFailureDetail::NumericCast;
  });
  expectHostedBuilderRejection("a missing detector outcome", [](auto &plan) {
    plan.convertCount.localOrigins.front().outcomes.clear();
  });
  expectHostedBuilderRejection("a duplicate detector outcome", [](auto &plan) {
    auto &outcomes = plan.validateCount.localOrigins.front().outcomes;
    outcomes.push_back(outcomes.front());
  });
  expectHostedBuilderRejection("a duplicate detector origin", [](auto &plan) {
    plan.validateCount.localOrigins.push_back(
        plan.validateCount.localOrigins.front());
  });
  expectHostedBuilderRejection("a third detector origin", [](auto &plan) {
    plan.validateCount.localOrigins.push_back(
        plan.convertCount.localOrigins.front());
  });
  expectHostedBuilderRejection(
      "a manufactured hosted-arguments allocation origin", [](auto &plan) {
        plan.convertCount.localOrigins.front().outcomes = {
            {.code = lang::DefinedFailureCode::AllocationFailure,
             .detail = lang::DefinedFailureDetail::HostedArguments}};
      });
  expectHostedBuilderRejection("a propagating hosted detector", [](auto &plan) {
    plan.validateCount.propagation = lang::FailurePropagationKind::DirectCall;
  });

  lang::HirProgram forgedNoArguments = noArguments.hir;
  auto &forgedNoArgumentsPlan =
      const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
          forgedNoArguments.hostedProgramEntryPlan());
  if (forgedNoArgumentsPlan && hosted) {
    forgedNoArgumentsPlan->validateCount = hosted->validateCount;
  }
  expect(forgedNoArgumentsPlan &&
             !rebuildFailureMetadata(noArguments, forgedNoArguments).valid(),
         "a no-argument hosted entry should reject an injected detector");

  lang::HirProgram coordinatedSpanDrift = owned.hir;
  auto &coordinatedPlan =
      const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
          coordinatedSpanDrift.hostedProgramEntryPlan());
  if (coordinatedPlan) {
    ++coordinatedPlan->mainAnchor.start;
    ++coordinatedPlan->validateCount.localOrigins.front().start;
    ++coordinatedPlan->convertCount.localOrigins.front().start;
  }
  expect(coordinatedPlan &&
             !lang::verifyHirProgramPlans(owned.semantics, coordinatedSpanDrift)
                  .valid(),
         "HIR plan verification should reject coordinated main-anchor drift "
         "that remains internally self-consistent");
}

void testExternalSourceRouteIdentity() {
  const std::filesystem::path fixtureRoot =
      std::filesystem::weakly_canonical(
          std::filesystem::temp_directory_path()) /
      "gti-route";
  const std::string entrySource = R"(
#include "../shared.gti"
int main() { return checked(1); }
)";
  const std::string externalSource =
      "int checked(int value) { return value + 1; }";
  const auto analyzeAt = [&](const std::filesystem::path &root) {
    const std::filesystem::path entry = root / "app/main.gti";
    const std::filesystem::path external = root / "shared.gti";
    return lang::Frontend().analyze(entry, entrySource, {},
                                    {{external.string(), externalSource}});
  };
  const lang::FrontendResult frontend = analyzeAt(fixtureRoot);
  const lang::FrontendResult relocated =
      analyzeAt(fixtureRoot.parent_path() / "gti-route-relocated");

  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the external source route fixture should compile");
  expect(relocated.canGenerateCode() && relocated.diagnostics.empty(),
         "the relocated external source route fixture should compile");
  if (!frontend.canGenerateCode() || !relocated.canGenerateCode()) {
    printDiagnostics(frontend);
    printDiagnostics(relocated);
    return;
  }
  const auto externalUnit =
      std::find_if(frontend.failureMetadata.sourceUnits().begin(),
                   frontend.failureMetadata.sourceUnits().end(),
                   [](const lang::FailureSourceDescriptor &unit) {
                     return unit.logicalName.starts_with("<external>/");
                   });
  const auto edge = std::find_if(frontend.sourceGraph.dependencyEdges().begin(),
                                 frontend.sourceGraph.dependencyEdges().end(),
                                 [](const lang::SourceDependency &dependency) {
                                   return dependency.kind ==
                                          lang::SourceDependencyKind::Include;
                                 });
  expect(externalUnit != frontend.failureMetadata.sourceUnits().end() &&
             externalUnit->logicalName.ends_with("/shared.gti") &&
             externalUnit->logicalName.find("/tmp/") == std::string::npos,
         "an out-of-root unit should use a content-and-route external name");
  expect(frontend.failureMetadata.descriptorBytes() ==
                 relocated.failureMetadata.descriptorBytes() &&
             frontend.failureMetadata.artifactIdentity() ==
                 relocated.failureMetadata.artifactIdentity(),
         "relocating an external include graph must preserve its descriptor "
         "and artifact identity");
  expect(edge != frontend.sourceGraph.dependencyEdges().end() &&
             edge->includeSpelling == "\"../shared.gti\"" &&
             edge->includeOccurrence == 0,
         "the source graph should retain exact non-path include-route facts");
}

void testMetadataAndSiteVerifierMutations() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-forged/main.gti", "int checked(int value) { return value + 1; }"
                                  "int main() { return checked(1); }");
  expect(frontend.canGenerateCode(),
         "the verifier mutation fixture should compile");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::MirProgram wrongSite = frontend.mir;
  lang::MirInstruction *instruction = firstFailureInstruction(wrongSite);
  if (instruction != nullptr && !instruction->localFailureSites.empty()) {
    instruction->localFailureSites.front() =
        instruction->localFailureSites.front() == 1 ? 2 : 1;
  }
  const lang::MirVerificationResult wrongSiteResult =
      lang::verifyMirProgram(wrongSite);
  expect(instruction != nullptr && !wrongSiteResult.valid() &&
             std::any_of(wrongSiteResult.errors.begin(),
                         wrongSiteResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("artifact-local site") !=
                                  std::string::npos;
                         }),
         "MIR verification should reject a forged detector site");

  lang::FailureMetadata malformed = frontend.failureMetadata;
  auto &bytes =
      const_cast<std::vector<std::uint8_t> &>(malformed.descriptorBytes());
  if (!bytes.empty()) {
    bytes.front() ^= 0xFFU;
  }
  expect(!lang::verifyFailureMetadata(malformed).valid(),
         "failure metadata verification should reject descriptor drift");

  lang::FailureMetadata wrongAssignment = frontend.failureMetadata;
  auto &assignments = const_cast<std::vector<lang::FailureOriginAssignment> &>(
      wrongAssignment.assignments());
  if (!assignments.empty()) {
    assignments.front().site = 0;
  }
  expect(!lang::verifyFailureMetadata(wrongAssignment).valid(),
         "failure metadata verification should reject a forged assignment");
}

void testCheckedIntegerOperationVerifierContracts() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("/tmp/gti-checked-integer-contract/main.gti", R"(
int32_t checked_add(int32_t left, int32_t right) { return left + right; }
int32_t checked_subtract(int32_t left, int32_t right) {
  return left - right;
}
uint32_t checked_multiply(uint32_t left, uint32_t right) {
  return left * right;
}
int32_t checked_signed_divide(int32_t left, int32_t right) {
  return left / right;
}
uint32_t checked_unsigned_divide(uint32_t left, uint32_t right) {
  return left / right;
}
int32_t checked_remainder(int32_t left, int32_t right) {
  return left % right;
}
int32_t checked_left_shift(int32_t left, int32_t count) {
  return left << count;
}
uint32_t checked_right_shift(uint32_t left, uint32_t count) {
  return left >> count;
}
int32_t checked_negate(int32_t value) { return -value; }
int8_t contextual_negative_literal() { return -1; }
int32_t signed_minimum_literal() { return -2147483648; }
int8_t checked_convert(int32_t value) { return int8_t(value); }
int32_t safe_convert(int8_t value) { return int32_t(value); }
int main() { return checked_add(1, 2); }
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the checked-integer verifier fixture should reach verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }

  struct ExpectedContract {
    std::string_view function;
    lang::MirOperation operation;
    std::vector<lang::DefinedFailureOutcome> outcomes;
  };
  const std::vector<ExpectedContract> contracts = {
      {"checked_add",
       lang::MirOperation::Add,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Addition}}},
      {"checked_subtract",
       lang::MirOperation::Subtract,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Subtraction}}},
      {"checked_multiply",
       lang::MirOperation::Multiply,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Multiplication}}},
      {"checked_signed_divide",
       lang::MirOperation::Divide,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Division},
        {.code = lang::DefinedFailureCode::DivisionByZero,
         .detail = lang::DefinedFailureDetail::IntegerDivision}}},
      {"checked_unsigned_divide",
       lang::MirOperation::Divide,
       {{.code = lang::DefinedFailureCode::DivisionByZero,
         .detail = lang::DefinedFailureDetail::IntegerDivision}}},
      {"checked_remainder",
       lang::MirOperation::Remainder,
       {{.code = lang::DefinedFailureCode::ModuloByZero,
         .detail = lang::DefinedFailureDetail::IntegerModulo}}},
      {"checked_left_shift",
       lang::MirOperation::ShiftLeft,
       {{.code = lang::DefinedFailureCode::NegativeShiftCount,
         .detail = lang::DefinedFailureDetail::LeftShift},
        {.code = lang::DefinedFailureCode::ShiftCountOutOfRange,
         .detail = lang::DefinedFailureDetail::LeftShift}}},
      {"checked_right_shift",
       lang::MirOperation::ShiftRight,
       {{.code = lang::DefinedFailureCode::ShiftCountOutOfRange,
         .detail = lang::DefinedFailureDetail::RightShift}}},
      {"checked_negate",
       lang::MirOperation::Negate,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Negation}}},
      {"contextual_negative_literal",
       lang::MirOperation::Negate,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Negation}}},
      {"signed_minimum_literal",
       lang::MirOperation::Negate,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Negation}}},
      {"checked_convert",
       lang::MirOperation::Convert,
       {{.code = lang::DefinedFailureCode::NumericConversionOutOfRange,
         .detail = lang::DefinedFailureDetail::NumericCast}}},
      {"safe_convert", lang::MirOperation::Convert, {}},
  };

  bool exactBaseline = lang::verifyMirProgram(frontend.mir).valid();
  for (const ExpectedContract &contract : contracts) {
    const std::optional<lang::HirFunctionInstanceId> function =
        functionInstance(frontend, contract.function);
    lang::MirProgram copy = frontend.mir;
    const lang::MirInstruction *instruction =
        function ? functionInstruction(copy, *function, contract.operation)
                 : nullptr;
    exactBaseline =
        exactBaseline && instruction != nullptr &&
        (contract.outcomes.empty()
             ? instruction->definedFailure.empty() &&
                   instruction->localFailureSites.empty()
             : instruction->definedFailure.propagation ==
                       lang::FailurePropagationKind::None &&
                   instruction->definedFailure.localOrigins.size() == 1 &&
                   instruction->definedFailure.localOrigins.front().outcomes ==
                       contract.outcomes &&
                   instruction->localFailureSites.size() == 1 &&
                   instruction->localFailureSites.front() != 0);
  }
  expect(exactBaseline,
         "checked integer MIR should derive the exact canonical outcome set "
         "from each operation and signed/count/conversion domain");

  const auto mutateInstruction =
      [&](lang::MirProgram &program, std::string_view name,
          lang::MirOperation operation) -> lang::MirInstruction * {
    const std::optional<lang::HirFunctionInstanceId> function =
        functionInstance(frontend, name);
    return function ? functionInstruction(program, *function, operation)
                    : nullptr;
  };
  const auto expectContractRejection = [&](const lang::MirProgram &program,
                                           std::string_view message) {
    const lang::MirVerificationResult result = lang::verifyMirProgram(program);
    const bool rejectedAtContract =
        !result.valid() && hasVerificationError(result, message);
    if (!rejectedAtContract) {
      std::cerr << "Expected checked-operation verifier error containing '"
                << message << "', got:";
      for (const lang::MirVerificationError &error : result.errors) {
        std::cerr << "\n  " << error.message;
      }
      std::cerr << '\n';
    }
    expect(rejectedAtContract,
           "checked integer mutation should fail at the exact operation "
           "contract: " +
               std::string(message));
  };
  const auto relabelValueOperand = [&](lang::MirProgram &program,
                                       std::string_view name,
                                       lang::MirOperation operation,
                                       std::size_t operandIndex,
                                       const lang::SemanticType &type) {
    const std::optional<lang::HirFunctionInstanceId> function =
        functionInstance(frontend, name);
    lang::MirBody *body = function ? functionBody(program, *function) : nullptr;
    lang::MirInstruction *instruction =
        function ? functionInstruction(program, *function, operation) : nullptr;
    if (body == nullptr || instruction == nullptr ||
        operandIndex >= instruction->operands.size() ||
        instruction->operands[operandIndex].kind !=
            lang::MirOperandKind::Value) {
      return false;
    }
    lang::MirOperand &operand = instruction->operands[operandIndex];
    lang::MirValue *value =
        operand.value == 0 || operand.value > body->values.size()
            ? nullptr
            : &body->values[operand.value - 1];
    if (value == nullptr) {
      return false;
    }
    lang::MirBlock *definitionBlock =
        value->definitionBlock == 0 ||
                value->definitionBlock > body->blocks.size()
            ? nullptr
            : &body->blocks[value->definitionBlock - 1];
    const auto definition =
        definitionBlock == nullptr
            ? std::vector<lang::MirInstruction>::iterator{}
            : std::find_if(definitionBlock->instructions.begin(),
                           definitionBlock->instructions.end(),
                           [&](const lang::MirInstruction &candidate) {
                             return candidate.id == value->definition;
                           });
    if (definitionBlock == nullptr ||
        definition == definitionBlock->instructions.end()) {
      return false;
    }
    operand.type = type;
    value->info.type = type;
    definition->info.type = type;
    if (definition->operands.size() == 1) {
      definition->operands.front().type = type;
    }
    return true;
  };

  lang::MirProgram mismatchedArithmeticDomain = frontend.mir;
  expect(relabelValueOperand(mismatchedArithmeticDomain, "checked_add",
                             lang::MirOperation::Add, 0,
                             lang::SemanticType::Int64),
         "the arithmetic-domain mutation should locate its first operand");
  expectContractRejection(mismatchedArithmeticDomain,
                          "fixed-width operand and result domains");

  lang::MirProgram mismatchedShiftValueDomain = frontend.mir;
  expect(relabelValueOperand(mismatchedShiftValueDomain, "checked_left_shift",
                             lang::MirOperation::ShiftLeft, 0,
                             lang::SemanticType::Int64),
         "the shift-domain mutation should locate its shifted operand");
  expectContractRejection(mismatchedShiftValueDomain,
                          "fixed-width operand and result domains");

  lang::MirProgram nonIntegerShiftCount = frontend.mir;
  expect(relabelValueOperand(nonIntegerShiftCount, "checked_left_shift",
                             lang::MirOperation::ShiftLeft, 1,
                             lang::SemanticType::Bool),
         "the shift-count mutation should locate its count operand");
  expectContractRejection(nonIntegerShiftCount,
                          "fixed-width operand and result domains");

  lang::MirProgram mismatchedNegationDomain = frontend.mir;
  expect(relabelValueOperand(mismatchedNegationDomain, "checked_negate",
                             lang::MirOperation::Negate, 0,
                             lang::SemanticType::Int64),
         "the negation-domain mutation should locate its operand");
  expectContractRejection(mismatchedNegationDomain,
                          "fixed-width operand and result domains");

  lang::MirProgram outOfRangeContextualLiteral = frontend.mir;
  lang::MirInstruction *contextualNegation = mutateInstruction(
      outOfRangeContextualLiteral, "contextual_negative_literal",
      lang::MirOperation::Negate);
  lang::MirBody *contextualBody = nullptr;
  const std::optional<lang::HirFunctionInstanceId> contextualFunction =
      functionInstance(frontend, "contextual_negative_literal");
  if (contextualFunction) {
    contextualBody =
        functionBody(outOfRangeContextualLiteral, *contextualFunction);
  }
  lang::MirInstruction *contextualLiteral = nullptr;
  if (contextualBody != nullptr && contextualNegation != nullptr &&
      contextualNegation->operands.size() == 1 &&
      contextualNegation->operands.front().kind ==
          lang::MirOperandKind::Value) {
    const lang::MirValue *operand =
        contextualBody->findValue(contextualNegation->operands.front().value);
    lang::MirBlock *definitionBlock =
        operand == nullptr || operand->definitionBlock == 0 ||
                operand->definitionBlock > contextualBody->blocks.size()
            ? nullptr
            : &contextualBody->blocks[operand->definitionBlock - 1];
    if (definitionBlock != nullptr) {
      const auto definition =
          std::find_if(definitionBlock->instructions.begin(),
                       definitionBlock->instructions.end(),
                       [&](const lang::MirInstruction &candidate) {
                         return candidate.id == operand->definition;
                       });
      if (definition != definitionBlock->instructions.end()) {
        contextualLiteral = &*definition;
        contextualLiteral->literal = std::uint64_t{2147483649};
      }
    }
  }
  expect(contextualLiteral != nullptr,
         "the contextual literal mutation should locate its exact source "
         "literal");
  expectContractRejection(outOfRangeContextualLiteral,
                          "fixed-width operand and result domains");

  lang::MirProgram driftedConversionSource = frontend.mir;
  expect(relabelValueOperand(driftedConversionSource, "checked_convert",
                             lang::MirOperation::Convert, 0,
                             lang::SemanticType::Int8),
         "the conversion-domain mutation should locate its source operand");
  expectContractRejection(driftedConversionSource,
                          "canonical local failure outcomes");

  lang::MirProgram missingOutcome = frontend.mir;
  if (lang::MirInstruction *instruction =
          mutateInstruction(missingOutcome, "checked_signed_divide",
                            lang::MirOperation::Divide)) {
    instruction->definedFailure.localOrigins.front().outcomes.pop_back();
  }
  expectContractRejection(missingOutcome, "canonical local failure outcomes");

  lang::MirProgram extraOutcome = frontend.mir;
  if (lang::MirInstruction *instruction =
          mutateInstruction(extraOutcome, "checked_unsigned_divide",
                            lang::MirOperation::Divide)) {
    instruction->definedFailure.localOrigins.front().outcomes.insert(
        instruction->definedFailure.localOrigins.front().outcomes.begin(),
        {.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Division});
  }
  expectContractRejection(extraOutcome, "canonical local failure outcomes");

  lang::MirProgram reorderedOutcomes = frontend.mir;
  if (lang::MirInstruction *instruction =
          mutateInstruction(reorderedOutcomes, "checked_signed_divide",
                            lang::MirOperation::Divide)) {
    std::reverse(
        instruction->definedFailure.localOrigins.front().outcomes.begin(),
        instruction->definedFailure.localOrigins.front().outcomes.end());
  }
  expectContractRejection(reorderedOutcomes,
                          "canonical local failure outcomes");

  lang::MirProgram missingMetadata = frontend.mir;
  if (lang::MirInstruction *instruction = mutateInstruction(
          missingMetadata, "checked_subtract", lang::MirOperation::Subtract)) {
    instruction->definedFailure.localOrigins.clear();
    instruction->localFailureSites.clear();
  }
  expectContractRejection(missingMetadata, "canonical local failure outcomes");

  lang::MirProgram mismatchedOperation = frontend.mir;
  if (lang::MirInstruction *instruction = mutateInstruction(
          mismatchedOperation, "checked_add", lang::MirOperation::Add)) {
    instruction->operation = lang::MirOperation::Multiply;
  }
  expectContractRejection(mismatchedOperation,
                          "canonical local failure outcomes");

  lang::MirProgram duplicateOrigin = frontend.mir;
  if (lang::MirInstruction *instruction = mutateInstruction(
          duplicateOrigin, "checked_multiply", lang::MirOperation::Multiply)) {
    instruction->definedFailure.localOrigins.push_back(
        instruction->definedFailure.localOrigins.front());
    instruction->localFailureSites.push_back(
        instruction->localFailureSites.front());
  }
  expectContractRejection(duplicateOrigin, "canonical local failure outcomes");

  lang::MirProgram staleOrigin = frontend.mir;
  if (lang::MirInstruction *instruction = mutateInstruction(
          staleOrigin, "checked_remainder", lang::MirOperation::Remainder)) {
    ++instruction->definedFailure.localOrigins.front().line;
  }
  expectContractRejection(staleOrigin, "origin and site identity");

  lang::MirProgram signedShiftMissingOutcome = frontend.mir;
  if (lang::MirInstruction *instruction =
          mutateInstruction(signedShiftMissingOutcome, "checked_left_shift",
                            lang::MirOperation::ShiftLeft)) {
    instruction->definedFailure.localOrigins.front().outcomes.erase(
        instruction->definedFailure.localOrigins.front().outcomes.begin());
  }
  expectContractRejection(signedShiftMissingOutcome,
                          "canonical local failure outcomes");

  lang::MirProgram unsignedShiftExtraOutcome = frontend.mir;
  if (lang::MirInstruction *instruction =
          mutateInstruction(unsignedShiftExtraOutcome, "checked_right_shift",
                            lang::MirOperation::ShiftRight)) {
    instruction->definedFailure.localOrigins.front().outcomes.insert(
        instruction->definedFailure.localOrigins.front().outcomes.begin(),
        {.code = lang::DefinedFailureCode::NegativeShiftCount,
         .detail = lang::DefinedFailureDetail::RightShift});
  }
  expectContractRejection(unsignedShiftExtraOutcome,
                          "canonical local failure outcomes");

  lang::MirProgram mismatchedNegation = frontend.mir;
  if (lang::MirInstruction *instruction = mutateInstruction(
          mismatchedNegation, "checked_negate", lang::MirOperation::Negate)) {
    instruction->definedFailure.localOrigins.front().outcomes.front().detail =
        lang::DefinedFailureDetail::Subtraction;
  }
  expectContractRejection(mismatchedNegation,
                          "canonical local failure outcomes");

  lang::MirProgram missingConversionOutcome = frontend.mir;
  if (lang::MirInstruction *instruction =
          mutateInstruction(missingConversionOutcome, "checked_convert",
                            lang::MirOperation::Convert)) {
    instruction->definedFailure.localOrigins.clear();
    instruction->localFailureSites.clear();
  }
  expectContractRejection(missingConversionOutcome,
                          "canonical local failure outcomes");

  lang::MirProgram forgedWideningFailure = frontend.mir;
  lang::MirInstruction *checked = mutateInstruction(
      forgedWideningFailure, "checked_convert", lang::MirOperation::Convert);
  lang::MirInstruction *widening = mutateInstruction(
      forgedWideningFailure, "safe_convert", lang::MirOperation::Convert);
  if (checked != nullptr && widening != nullptr) {
    widening->definedFailure = checked->definedFailure;
    widening->localFailureSites = checked->localFailureSites;
  }
  expectContractRejection(forgedWideningFailure,
                          "canonical local failure outcomes");

  lang::MirProgram staleRecord = frontend.mir;
  const std::optional<lang::HirFunctionInstanceId> addFunction =
      functionInstance(frontend, "checked_add");
  lang::MirBody *staleRecordBody =
      addFunction ? functionBody(staleRecord, *addFunction) : nullptr;
  if (staleRecordBody != nullptr && !staleRecordBody->failureRecords.empty()) {
    ++staleRecordBody->failureRecords.front().producerInstruction;
  }
  expectContractRejection(staleRecord, "exactly one fixed failure record");

  lang::MirProgram duplicateRecord = frontend.mir;
  lang::MirBody *duplicateRecordBody =
      addFunction ? functionBody(duplicateRecord, *addFunction) : nullptr;
  if (duplicateRecordBody != nullptr &&
      !duplicateRecordBody->failureRecords.empty()) {
    lang::MirFailureRecord duplicate =
        duplicateRecordBody->failureRecords.front();
    duplicate.id = duplicateRecordBody->failureRecords.size() + 1;
    duplicateRecordBody->failureRecords.push_back(duplicate);
  }
  expectContractRejection(duplicateRecord, "exactly one fixed failure record");
}

} // namespace

int main() {
  testCanonicalMetadataAndMirSites();
  testInvokeEdgesAndFailureCleanup();
  testEmptyDescriptorContract();
  testHostedProgramEntryMetadata();
  testExternalSourceRouteIdentity();
  testMetadataAndSiteVerifierMutations();
  testCheckedIntegerOperationVerifierContracts();
  return passed ? 0 : 1;
}
