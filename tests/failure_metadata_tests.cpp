#include "gti/failure_metadata.h"
#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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
  int32_t values[2] = {left, right};
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
      snapshot.starts_with("mir-v95 ") &&
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

expected<int, int> produce_expected(mut int value) {
  int checked = value + 1;
  return checked;
}

expected<bool, int> produce_condition(mut int value) {
  int checked = value + 1;
  return checked > 0;
}

int nested_expected_condition(mut int value) {
  if (!produce_condition(value)) { return 1; }
  return 0;
}

int direct_result(mut int value) {
  expected<int, int> result = produce_expected(value);
  expected<int, int> spare = produce_expected(value);
  return value;
}

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
  expect(deferredOwningPropagation != nullptr && nestedOwningCall != nullptr &&
             !nestedOwningCall->lifecycle.empty() && nestedOwningCallHasRecord,
         "a nested call that stages its own owning parameters now carries its "
         "own record and failure edge under position-independent routing");

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

  const lang::MirBody *directResult = functionBody(frontend, "direct_result");
  bool exactDirectResultEdge = false;
  const lang::MirInstruction *directCall = nullptr;
  const lang::MirFailureRecord *directRecord = nullptr;
  if (directResult != nullptr) {
    for (const lang::MirBlock &block : directResult->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.successResultDestination) {
          directCall = &instruction;
        }
      }
    }
    if (directCall != nullptr) {
      const auto found =
          std::find_if(directResult->failureRecords.begin(),
                       directResult->failureRecords.end(),
                       [&](const lang::MirFailureRecord &record) {
                         return record.producerInstruction == directCall->id;
                       });
      if (found != directResult->failureRecords.end()) {
        directRecord = &*found;
      }
    }
  }
  if (directResult != nullptr && directCall != nullptr &&
      directRecord != nullptr) {
    const lang::MirFailureRecord &record = *directRecord;
    const lang::MirBlock *producer =
        directResult->findBlock(record.producerBlock);
    const lang::MirBlock *success =
        producer == nullptr
            ? nullptr
            : directResult->findBlock(producer->terminator.target);
    const lang::MirInstruction *call = directCall;
    const lang::MirInstruction *initialize =
        success == nullptr ? nullptr : [&]() -> const lang::MirInstruction * {
      const auto found = std::find_if(
          success->instructions.begin(), success->instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Initialize;
          });
      return found == success->instructions.end() ? nullptr : &*found;
    }();
    exactDirectResultEdge =
        call != nullptr && call->result && call->successResultDestination &&
        !call->successResultDrop &&
        producer->terminator.kind == lang::MirTerminatorKind::Invoke &&
        producer->terminator.successLifecycle.empty() &&
        initialize != nullptr &&
        initialize->destination == call->successResultDestination &&
        initialize->operands.size() == 1 &&
        initialize->operands.front().kind == lang::MirOperandKind::Value &&
        initialize->operands.front().value == *call->result;
  }
  expect(exactDirectResultEdge,
         "a direct-initialization call result should publish only into its "
         "exact binding on the invoke success edge");

  const lang::MirBody *nestedExpectedCondition =
      functionBody(frontend, "nested_expected_condition");
  const lang::MirInstruction *nestedExpectedCall = nullptr;
  const lang::MirFailureRecord *nestedExpectedRecord = nullptr;
  const lang::MirInstruction *nestedExpectedHasValue = nullptr;
  if (nestedExpectedCondition != nullptr) {
    for (const lang::MirBlock &block : nestedExpectedCondition->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.info.type.kind == lang::SemanticType::Expected &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::DirectCall) {
          nestedExpectedCall = &instruction;
        }
      }
    }
    if (nestedExpectedCall != nullptr && nestedExpectedCall->result) {
      const auto record = std::find_if(
          nestedExpectedCondition->failureRecords.begin(),
          nestedExpectedCondition->failureRecords.end(),
          [&](const lang::MirFailureRecord &candidate) {
            return candidate.producerInstruction == nestedExpectedCall->id;
          });
      if (record != nestedExpectedCondition->failureRecords.end()) {
        nestedExpectedRecord = &*record;
        const lang::MirBlock *producer =
            nestedExpectedCondition->findBlock(record->producerBlock);
        const lang::MirBlock *normal = producer == nullptr
                                           ? nullptr
                                           : nestedExpectedCondition->findBlock(
                                                 producer->terminator.target);
        if (normal != nullptr) {
          const auto conversion = std::find_if(
              normal->instructions.begin(), normal->instructions.end(),
              [&](const lang::MirInstruction &instruction) {
                return instruction.kind == lang::MirInstructionKind::Compute &&
                       instruction.operation ==
                           lang::MirOperation::ExpectedHasValue &&
                       instruction.operands.size() == 1 &&
                       instruction.operands.front().kind ==
                           lang::MirOperandKind::Value &&
                       instruction.operands.front().value ==
                           *nestedExpectedCall->result;
              });
          if (conversion != normal->instructions.end()) {
            nestedExpectedHasValue = &*conversion;
          }
        }
      }
    }
  }
  const lang::MirBlock *nestedExpectedProducer =
      nestedExpectedCondition == nullptr || nestedExpectedRecord == nullptr
          ? nullptr
          : nestedExpectedCondition->findBlock(
                nestedExpectedRecord->producerBlock);
  const lang::MirBlock *nestedExpectedFailure =
      nestedExpectedCondition == nullptr || nestedExpectedRecord == nullptr
          ? nullptr
          : nestedExpectedCondition->findBlock(
                nestedExpectedRecord->parameterBlock);
  expect(nestedExpectedCall != nullptr && nestedExpectedRecord != nullptr &&
             nestedExpectedProducer != nullptr &&
             nestedExpectedFailure != nullptr &&
             nestedExpectedHasValue != nullptr &&
             nestedExpectedCall->lifecycle.empty() &&
             !nestedExpectedCall->successResultDrop &&
             nestedExpectedProducer->terminator.kind ==
                 lang::MirTerminatorKind::Invoke &&
             nestedExpectedProducer->terminator.invokeInstruction ==
                 nestedExpectedCall->id &&
             nestedExpectedProducer->terminator.failureRecord ==
                 nestedExpectedRecord->id &&
             nestedExpectedFailure->terminator.kind ==
                 lang::MirTerminatorKind::PropagateFailure &&
             nestedExpectedFailure->terminator.failureRecord ==
                 nestedExpectedRecord->id,
         "a fallible Expected<scalar, E> call nested in a condition should "
         "route failure before its normal-path contextual conversion");

  const std::optional<lang::HirFunctionInstanceId> directResultId =
      functionInstance(frontend, "direct_result");
  lang::MirProgram mismatchedDirectDestination = frontend.mir;
  lang::MirBody *mismatchedDirectResult =
      directResultId
          ? functionBody(mismatchedDirectDestination, *directResultId)
          : nullptr;
  lang::MirInstruction *firstDirectCall = nullptr;
  std::optional<lang::MirPlaceId> alternateDirectDestination;
  if (mismatchedDirectResult != nullptr) {
    for (lang::MirBlock &block : mismatchedDirectResult->blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (!instruction.successResultDestination) {
          continue;
        }
        if (firstDirectCall == nullptr) {
          firstDirectCall = &instruction;
        } else if (instruction.successResultDestination !=
                   firstDirectCall->successResultDestination) {
          alternateDirectDestination = instruction.successResultDestination;
        }
      }
    }
  }
  if (firstDirectCall != nullptr && alternateDirectDestination) {
    firstDirectCall->successResultDestination = alternateDirectDestination;
  }
  const lang::MirVerificationResult mismatchedDirectDestinationResult =
      lang::verifyMirProgram(mismatchedDirectDestination);
  expect(firstDirectCall != nullptr && alternateDirectDestination &&
             !mismatchedDirectDestinationResult.valid() &&
             hasVerificationError(mismatchedDirectDestinationResult,
                                  "direct destination"),
         "MIR verification should reject a direct call result redirected to "
         "another valid binding");

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

  const std::optional<lang::HirFunctionInstanceId> nestedExpectedConditionId =
      functionInstance(frontend, "nested_expected_condition");
  lang::MirProgram missingNestedExpectedInvoke = frontend.mir;
  lang::MirBody *missingNestedExpectedInvokeBody =
      nestedExpectedConditionId ? functionBody(missingNestedExpectedInvoke,
                                               *nestedExpectedConditionId)
                                : nullptr;
  bool removedNestedExpectedInvoke = false;
  if (missingNestedExpectedInvokeBody != nullptr) {
    for (const lang::MirFailureRecord &record :
         missingNestedExpectedInvokeBody->failureRecords) {
      lang::MirBlock &producer =
          missingNestedExpectedInvokeBody->blocks[record.producerBlock - 1];
      if (producer.instructions.empty() ||
          producer.instructions.back().kind != lang::MirInstructionKind::Call ||
          producer.instructions.back().info.type.kind !=
              lang::SemanticType::Expected) {
        continue;
      }
      producer.terminator = {.kind = lang::MirTerminatorKind::Goto,
                             .target = producer.terminator.target};
      removedNestedExpectedInvoke = true;
      break;
    }
    lang::rebuildMirReachability(*missingNestedExpectedInvokeBody);
    (void)lang::rebuildMirValueUses(*missingNestedExpectedInvokeBody);
  }
  const lang::MirVerificationResult missingNestedExpectedInvokeResult =
      lang::verifyMirProgram(missingNestedExpectedInvoke);
  expect(
      removedNestedExpectedInvoke &&
          !missingNestedExpectedInvokeResult.valid() &&
          hasVerificationError(missingNestedExpectedInvokeResult, "one invoke"),
      "MIR verification should reject a nested Expected condition whose "
      "fallible call loses its invoke edge");

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
  expect(rewiredArgument && forgedArgumentRelationResult.valid(),
         "a nested invoke stays valid on its own shape when its consuming "
         "argument is rewired; eligibility is position-independent");

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
  expect(detachedNestedCallResult && forgedNestedCallRelationResult.valid(),
         "nested call propagation stays valid on its own shape when its "
         "outer argument input is rewired; eligibility is "
         "position-independent");
}

void testContainerContractChecks() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-container-contract/main.gti",
      "#include <std/functional>\n"
      "void check(bool valid) { std::key(valid); std::argument(valid); }\n",
      {standardLibraryPrelude()},
      {{(standardLibraryRoot() / "std/functional.gti").string(),
        "namespace std {\n"
        "void key(bool valid) { gti_internal::require_key(valid); }\n"
        "void argument(bool valid) { "
        "gti_internal::require_container_argument(valid); }\n}\n"}},
      {standardLibraryRoot()});
  if (!frontend.canGenerateCode())
    printDiagnostics(frontend);
  expect(frontend.canGenerateCode(),
         "private bool contract checks must lower to verified MIR");
  if (!frontend.canGenerateCode())
    return;
  for (const auto intrinsic : {lang::IntrinsicKind::RequireKey,
                               lang::IntrinsicKind::RequireContainerArgument}) {
    bool found = false;
    forEachInstruction(
        frontend.mir, [&](const lang::MirInstruction &instruction) {
          if (instruction.intrinsic != intrinsic)
            return;
          found = true;
          expect(instruction.localFailureSites.size() == 1 &&
                     instruction.definedFailure.localOrigins.size() == 1,
                 "each contract check must own exactly one failure site");
        });
    expect(found,
           "both declaration-bound contract checks must survive MIR lowering");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto forged = frontend.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      for (auto &function : functions)
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (instruction.intrinsic != intrinsic)
              continue;
            if (mutation == 0)
              instruction.operands.front().type = lang::SemanticType::Int32;
            if (mutation == 1)
              instruction.parameterTypes.clear();
            if (mutation == 2)
              instruction.definedFailure.localOrigins.clear();
            if (mutation == 3)
              instruction.intrinsic =
                  intrinsic == lang::IntrinsicKind::RequireKey
                      ? lang::IntrinsicKind::RequireContainerArgument
                      : lang::IntrinsicKind::RequireKey;
          }
      expect(!lang::verifyMirProgram(forged).valid(),
             "contract check operand, parameter, origin and category mutations "
             "must fail closed");
    }
  }
  expect(
      !lang::validDefinedFailureCode(static_cast<lang::DefinedFailureCode>(14)),
      "emergency code14 must remain excluded from ordinary failure records");
  for (const std::string call : {"require_key(1)", "require_key()",
                                 "require_container_argument(true, false)"}) {
    const auto invalid = lang::Frontend().analyze(
        "/tmp/gti-container-contract/invalid.gti",
        "#include <std/functional>\nvoid invoke() { std::invalid(); }\n",
        {standardLibraryPrelude()},
        {{(standardLibraryRoot() / "std/functional.gti").string(),
          "namespace std { void invalid() { gti_internal::" + call + "; } }"}},
        {standardLibraryRoot()});
    expect(!invalid.canGenerateCode() &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const lang::Diagnostic &diagnostic) {
                             return diagnostic.code == "GTI-S2019";
                           }),
           "trusted contract check calls must still require one exact bool");
  }
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

void testCallerLocationAssignments() {
  const std::string source = "int target(int value) { return value + 1; }\n"
                             "int main() { return target(3); }\n";
  const auto frontend =
      lang::Frontend().analyze("/tmp/gti-caller-sites/main.gti", source);
  expect(frontend.canGenerateCode(),
         "caller metadata source fixture reaches verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  const auto mainId = functionInstance(frontend, "main");
  expect(mainId.has_value(), "caller metadata fixture contains main");
  if (!mainId) {
    return;
  }
  auto hir = frontend.hir;
  auto *main = const_cast<lang::HirFunctionInstance *>(
      hir.findFunctionInstance(*mainId));
  auto call = std::find_if(main->body.values.begin(), main->body.values.end(),
                           [](const lang::HirValue &value) {
                             return value.kind == lang::HirValueKind::Call &&
                                    value.functionTarget;
                           });
  expect(call != main->body.values.end(),
         "caller metadata fixture contains a direct call");
  if (call == main->body.values.end()) {
    return;
  }
  const std::size_t start = source.find("target(3)");
  const lang::CallerLocationOrigin origin{.sourceUnit = main->sourceUnit,
                                          .start = start,
                                          .end = start + 9,
                                          .line = 2};
  // Exercise the metadata consumer independently of the frontend's attribute
  // transport. This call remains propagation-only, with no local detector.
  call->callerLocation = origin;
  const auto originalFailure = call->definedFailure;
  const auto built = rebuildFailureMetadata(frontend, hir);
  expect(built.valid() && lang::verifyFailureMetadata(built.metadata).valid(),
         "caller attribution should build a verified reporting site");
  if (!built.valid()) {
    return;
  }
  const auto site = built.metadata.callerSiteFor(origin);
  expect(site && built.metadata.callerAssignments().size() == 1 &&
             built.metadata.assignments() ==
                 frontend.failureMetadata.assignments() &&
             call->definedFailure == originalFailure &&
             originalFailure.localOrigins.empty(),
         "caller attribution must not manufacture detector assignments or "
         "local failures");
  const lang::DefinedFailureOrigin forgedDetector{
      .outcomes = {{lang::DefinedFailureCode::Panic,
                    lang::DefinedFailureDetail::ExplicitPanic}},
      .sourceUnit = origin.sourceUnit,
      .start = origin.start,
      .end = origin.end,
      .line = origin.line};
  expect(!built.metadata.siteFor(forgedDetector),
         "report admission alone must not resolve as a detector origin");
  const auto *descriptor = site ? built.metadata.findSite(*site) : nullptr;
  expect(descriptor && descriptor->outcomes == forgedDetector.outcomes &&
             descriptor->logicalSource == "main.gti" && descriptor->line == 2,
         "caller site reports the exact caller anchor with bounded panic "
         "admission");
  auto wrongLine = origin;
  ++wrongLine.line;
  expect(!built.metadata.callerSiteFor(wrongLine),
         "caller site lookup must reject inconsistent source lines");
  call->callerLocation = wrongLine;
  expect(!rebuildFailureMetadata(frontend, hir).valid(),
         "caller source lines must match their exact input bytes");
  call->callerLocation = origin;
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto malformed = built.metadata;
    auto &assignments =
        const_cast<std::vector<lang::CallerLocationAssignment> &>(
            malformed.callerAssignments());
    switch (mutation) {
    case 0:
      assignments.clear();
      break;
    case 1:
      assignments.push_back(assignments.front());
      break;
    case 2:
      assignments.front().site = 0;
      break;
    case 3:
      ++assignments.front().origin.line;
      break;
    case 4:
      ++assignments.front().origin.start;
      break;
    }
    expect(
        !lang::verifyFailureMetadata(malformed).valid(),
        "independent metadata verification rejects forged caller assignment " +
            std::to_string(mutation));
  }

  // Shared physical spans can admit both real detector and caller reporting
  // roles. Their union must never expand the local detector's own vocabulary.
  const auto &local = frontend.failureMetadata.assignments().front();
  const lang::CallerLocationOrigin shared{.sourceUnit = local.sourceUnit,
                                          .start = local.start,
                                          .end = local.end,
                                          .line = local.line};
  call->callerLocation = shared;
  const auto combined = rebuildFailureMetadata(frontend, hir);
  expect(combined.valid() &&
             combined.metadata.sites().size() ==
                 frontend.failureMetadata.sites().size() &&
             combined.metadata.assignments().front().outcomes ==
                 local.outcomes &&
             combined.metadata.sites().front().outcomes.size() ==
                 local.outcomes.size() + 1,
         "a shared reporting span unions admissions without changing detector "
         "authority");
  if (combined.valid()) {
    auto missingCaller = combined.metadata;
    const_cast<std::vector<lang::CallerLocationAssignment> &>(
        missingCaller.callerAssignments())
        .clear();
    expect(!lang::verifyFailureMetadata(missingCaller).valid(),
           "a still-referenced detector site cannot retain unaccounted panic "
           "admission");
  }
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
int32_t unchecked_positive(int32_t value) { return +value; }
int8_t contextual_negative_literal() { return -1; }
int32_t signed_minimum_literal() { return -2147483648; }
bool contextual_int8_minimum(int8_t value) { return value == -128; }
bool contextual_int32_minimum(int32_t value) {
  return value == -2147483648;
}
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
      {"unchecked_positive", lang::MirOperation::Positive, {}},
      {"contextual_negative_literal",
       lang::MirOperation::Negate,
       {{.code = lang::DefinedFailureCode::IntegerOverflow,
         .detail = lang::DefinedFailureDetail::Negation}}},
      // The source literal is checked in its exact contextual domain. The
      // positive boundary magnitude produces the signed minimum without a
      // possible runtime overflow, just like the contextual comparisons.
      {"signed_minimum_literal", lang::MirOperation::Negate, {}},
      {"contextual_int8_minimum", lang::MirOperation::Negate, {}},
      {"contextual_int32_minimum", lang::MirOperation::Negate, {}},
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

  lang::MirProgram outOfRangeSignedMinimum = frontend.mir;
  bool changedSignedMinimum = false;
  if (const auto function =
          functionInstance(frontend, "signed_minimum_literal"))
    if (auto *body = functionBody(outOfRangeSignedMinimum, *function))
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.operation == lang::MirOperation::Literal) {
            instruction.literal = std::uint64_t{2147483649};
            changedSignedMinimum = true;
          }
  expect(changedSignedMinimum,
         "the signed minimum mutation changes its boundary magnitude");
  expectContractRejection(outOfRangeSignedMinimum,
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

void testUniqueOwnerBorrowFailureEdges() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-owner-failure/main.gti", R"(
#include <std/memory>
struct Record { mut int32_t value = 1; };
int32_t read(std::unique_ptr<Record>& owner) {
  return (*owner).value + owner->value;
}
int32_t write(mut std::unique_ptr<Record>& owner) {
  (*owner).value = 2;
  owner->value = 3;
  return 0;
}
int main() {
  mut auto owner = std::make_unique<Record>();
  return read(owner) + write(owner);
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "all four unique-owner access forms need verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  std::size_t detectors = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::UniqueOwnerBorrow &&
            instruction.intrinsic !=
                lang::IntrinsicKind::UniqueOwnerBorrowMut) {
          continue;
        }
        ++detectors;
        const auto *failure =
            function.body.findBlock(block.terminator.elseTarget);
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id &&
                   failure != nullptr &&
                   failure->terminator.kind ==
                       lang::MirTerminatorKind::PropagateFailure &&
                   instruction.localFailureSites.size() == 1 &&
                   function.mayRaiseDefinedFailure,
               "owner access must check before publishing a reference and "
               "forward one original failure record");
        for (int mutation = 0; mutation < 7; ++mutation) {
          lang::MirProgram forged = frontend.mir;
          lang::MirBody *body = functionBody(forged, function.id);
          auto &changedBlock = body->blocks[block.id - 1];
          auto &changed = *std::find_if(
              changedBlock.instructions.begin(),
              changedBlock.instructions.end(),
              [&](const auto &value) { return value.id == instruction.id; });
          switch (mutation) {
          case 0:
            changedBlock.terminator = {.kind = lang::MirTerminatorKind::Goto,
                                       .target = block.terminator.target};
            break;
          case 1:
            changed.definedFailure.localOrigins.clear();
            changed.localFailureSites.clear();
            break;
          case 2:
            changed.definedFailure.localOrigins.front().outcomes.front().code =
                lang::DefinedFailureCode::AllocationFailure;
            break;
          case 3:
            changed.definedFailure.localOrigins.front()
                .outcomes.front()
                .detail = lang::DefinedFailureDetail::PrivateStorage;
            break;
          case 4:
            changed.borrowOrigin = lang::BorrowOriginKind::None;
            break;
          case 5:
            body->loans[*changed.loan - 1].source = 0;
            break;
          case 6:
            body->loans[*changed.loan - 1].access =
                changed.borrowAccess == lang::AccessMode::Mutable
                    ? lang::AccessMode::ReadOnly
                    : lang::AccessMode::Mutable;
            break;
          }
          const auto result = lang::verifyMirProgram(forged);
          expect(!result.valid(), "forged owner detector, failure edge, or "
                                  "loan must fail MIR verification");
          if (mutation != 0) {
            expect(hasVerificationError(result, "unique-owner borrow"),
                   "owner failures need an independent operation-specific "
                   "verifier contract");
          }
        }
      }
    }
  }
  expect(
      detectors == 4,
      "read/mutable dereference and arrow each need an exact owner detector");
}

void testScalarUniqueAllocationContracts() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-scalar-allocation/main.gti", R"(
#include <std/memory>
int main() {
  auto value = std::make_unique<int32_t>(7);
  auto zero = std::make_unique<int32_t>();
  return *value + *zero;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "scalar unique allocation needs verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  std::size_t allocations = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::AllocateUniqueOwner) {
          continue;
        }
        ++allocations;
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id &&
                   instruction.successResultDrop.has_value(),
               "allocation must publish an owner only after its success edge");
        for (int mutation = 0; mutation < 6; ++mutation) {
          auto forged = frontend.mir;
          auto *body = functionBody(forged, function.id);
          auto &changedBlock = body->blocks[block.id - 1];
          auto &changed = *std::find_if(changedBlock.instructions.begin(),
                                        changedBlock.instructions.end(),
                                        [&](const auto &candidate) {
                                          return candidate.id == instruction.id;
                                        });
          switch (mutation) {
          case 0:
            changedBlock.terminator = {.kind = lang::MirTerminatorKind::Goto,
                                       .target = block.terminator.target};
            break;
          case 1:
            changed.definedFailure.localOrigins.clear();
            changed.localFailureSites.clear();
            break;
          case 2:
            changed.definedFailure.localOrigins.front()
                .outcomes.front()
                .detail = lang::DefinedFailureDetail::PrivateStorage;
            break;
          case 3:
            changed.info.type =
                lang::SemanticType::uniqueOwnerOf(lang::SemanticType::Void);
            break;
          case 4:
            changed.parameterTypes.push_back(lang::SemanticType::Bool);
            break;
          case 5:
            changed.constructorTarget = 1;
            break;
          }
          const auto verified = lang::verifyMirProgram(forged);
          expect(!verified.valid(),
                 "forged scalar allocation must fail verification");
          if (mutation != 0) {
            expect(hasVerificationError(verified, "scalar unique allocation"),
                   "scalar allocation must have an independent exact contract");
          }
        }
      }
    }
  }
  expect(allocations == 2,
         "default and exact-value scalar allocation are distinct instances");

  const auto callback = lang::Frontend().analyze(
      "/tmp/gti-scalar-callback/main.gti", R"(
#include <std/memory>
using callback = (int32_t) -> int32_t;
int main() {
  callback empty = nullptr;
  auto owner = std::make_unique<callback>(empty);
  return *owner == nullptr ? 0 : 1;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(callback.canGenerateCode(),
         "callback values need scalar owner allocation");
  if (!callback.canGenerateCode()) {
    printDiagnostics(callback);
  }

  for (const auto &[expression, fragment] :
       std::vector<std::pair<std::string, std::string>>{
           {"std::make_unique<uint32_t>(7)", "exact pointee type"},
           {"std::make_unique<bool>(1)", "exact pointee type"},
           {"std::make_unique<int32_t>(1, 2)", "exact pointee type"},
           {"std::make_unique<choice>()", "no default initialization"},
           {"std::make_unique<nullptr_t>()", "no default initialization"},
           {"std::make_unique<callback>()", "no default initialization"}}) {
    const std::string path = "/tmp/gti-invalid-scalar/main.gti";
    const auto rejected = lang::Frontend().analyze(
        path,
        "#include <std/memory>\nenum class choice { one };\n"
        "using callback = (int32_t) -> int32_t;\n"
        "int main() { auto owner = " +
            expression + "; return 0; }\n",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(!rejected.canGenerateCode(),
           "inexact scalar allocation must be rejected");
    const auto diagnostic =
        std::find_if(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                     [&](const auto &value) {
                       return value.code == "GTI-S2018" &&
                              value.message.find(fragment) != std::string::npos;
                     });
    expect(diagnostic != rejected.diagnostics.end(),
           "scalar allocation needs its owning semantic diagnostic");
    if (diagnostic != rejected.diagnostics.end()) {
      const auto *source = rejected.sources.find(diagnostic->primary.source);
      expect(diagnostic->phase == lang::DiagnosticPhase::Semantics &&
                 diagnostic->severity == lang::DiagnosticSeverity::Error &&
                 diagnostic->fixes.empty() && source != nullptr &&
                 std::filesystem::path(diagnostic->primary.source).filename() ==
                     "memory.gti" &&
                 source->substr(diagnostic->primary.start,
                                diagnostic->primary.end -
                                    diagnostic->primary.start) == ")" &&
                 std::any_of(diagnostic->related.begin(),
                             diagnostic->related.end(),
                             [&](const auto &related) {
                               return std::filesystem::weakly_canonical(
                                          related.span.source) ==
                                      std::filesystem::weakly_canonical(path);
                             }),
             "invalid generic allocation must retain exact stdlib span and "
             "application instantiation");
    }
  }
}

void testClassUniqueAllocationContracts() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-class-allocation/main.gti", R"(
#include <std/memory>
class Value {
  int32_t result;
public:
  Value(int32_t divisor) : result(9 / divisor) {}
};
int main() { auto owner = std::make_unique<Value>(0); return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "class unique allocation needs verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  std::size_t allocations = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &allocate : block.instructions) {
        if (allocate.kind != lang::MirInstructionKind::AllocateObject)
          continue;
        ++allocations;
        const auto *raw =
            function.body.findDropObligation(allocate.objectAllocation);
        const auto &construction =
            function.body.blocks[block.terminator.target - 1];
        const auto &call = construction.instructions.front();
        expect(
            raw && raw->kind == lang::MirDropObligationKind::ObjectAllocation &&
                !allocate.result && allocate.successResultDrop == raw->id &&
                call.objectAllocation == raw->id &&
                call.intrinsic == lang::IntrinsicKind::AllocateUniqueOwner &&
                call.definedFailure.localOrigins.empty() &&
                call.definedFailure.propagation ==
                    lang::FailurePropagationKind::Constructor &&
                construction.terminator.kind == lang::MirTerminatorKind::Invoke,
            "raw allocation and exact constructor require separate failure "
            "edges");
        for (int mutation = 0; mutation < 14; ++mutation) {
          auto forged = frontend.mir;
          auto *body = functionBody(forged, function.id);
          auto &allocationBlock = body->blocks[block.id - 1];
          auto &changed = *std::find_if(
              allocationBlock.instructions.begin(),
              allocationBlock.instructions.end(),
              [&](const auto &value) { return value.id == allocate.id; });
          auto &constructorBlock = body->blocks[construction.id - 1];
          auto &constructor = constructorBlock.instructions.front();
          auto &retirement = body->blocks[construction.terminator.target - 1]
                                 .instructions.front();
          switch (mutation) {
          case 0:
            changed.objectAllocation = 0;
            break;
          case 1:
            changed.definedFailure.localOrigins.front()
                .outcomes.front()
                .detail = lang::DefinedFailureDetail::PrivateStorage;
            break;
          case 2:
            changed.successResultDrop.reset();
            break;
          case 3:
            constructor.objectAllocation = 0;
            break;
          case 4:
            constructor.constructorTarget.reset();
            break;
          case 5:
            constructor.definedFailure.propagation =
                lang::FailurePropagationKind::None;
            break;
          case 6:
            retirement.objectAllocation = 0;
            break;
          case 7:
            retirement.lifecycle.front().conditional = true;
            break;
          case 8:
            allocationBlock.terminator.target = construction.terminator.target;
            break;
          case 9:
            body->places[*changed.destination - 1].initiallyAvailable = true;
            break;
          case 10:
            body->dropObligations[changed.objectAllocation - 1]
                .initiallyActive = true;
            break;
          case 11:
            for (const auto &target : frontend.mir.constructorInstances()) {
              if (target.id != constructor.constructorTarget) {
                constructor.constructorTarget = target.id;
                break;
              }
            }
            break;
          case 12:
            allocationBlock.instructions.front().objectAllocation = raw->id;
            break;
          case 13: {
            auto alias = body->places[raw->place - 1];
            alias.id = body->places.size() + 1;
            body->places.push_back(std::move(alias));
            break;
          }
          }
          expect(!lang::verifyMirProgram(forged).valid(),
                 "forged class allocation protocol must fail independent "
                 "verification");
        }
      }
    }
  }
  expect(allocations == 1,
         "one class allocation requires one raw-storage resource");
}

void testUniquePointeeCleanupContracts() {
  const auto quiet = lang::Frontend().analyze(
      "/tmp/gti-quiet-cleanup/main.gti", R"(
#include <std/memory>
class Quiet { public: Quiet() {} ~Quiet() {} };
int main() { auto owner = std::make_unique<Quiet>(); return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(quiet.canGenerateCode(), "quiet unique cleanup must lower");
  bool sawQuietCleanup = false;
  for (const auto address : lang::enumerateMirBodyAddresses(quiet.mir)) {
    const auto *body = lang::findMirBody(quiet.mir, address);
    if (body == nullptr)
      continue;
    for (const auto &block : body->blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1)
          continue;
        const auto *drop =
            body->findDropObligation(instruction.lifecycle.front().source);
        if (drop == nullptr ||
            lang::mirDropDestructorTargets(drop->dropType).empty())
          continue;
        sawQuietCleanup = true;
        expect(lang::effects(quiet.mir, instruction).invokesRuntime,
               "allocation release remains a runtime effect when the source "
               "destructor is empty");
      }
    }
  }
  expect(sawQuietCleanup, "quiet fixture must retain an owned-pointee drop");
  for (const auto fixture :
       {"aggregate_cleanup_failure.gti", "unique_pointee_cleanup_failure.gti",
        "inherited_multi_owner_cleanup_failure.gti",
        "source_destructor_cleanup_failure.gti"}) {
    const auto path =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / fixture;
    std::ifstream input(path);
    expect(input.good(), "unique-pointee cleanup fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = lang::Frontend().analyze(path.string(), source,
                                                   {standardLibraryPrelude()},
                                                   {}, {standardLibraryRoot()});
    expect(frontend.canGenerateCode(),
           "unique-pointee cleanup MIR must verify");
    if (!frontend.canGenerateCode()) {
      printDiagnostics(frontend);
      return;
    }
    std::size_t cleanupInvokes = 0;
    std::size_t multiTargetSchedules = 0;
    for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
      const auto *body = lang::findMirBody(frontend.mir, address);
      if (body == nullptr)
        continue;
      for (const auto &block : body->blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              instruction.lifecycle.size() != 1 ||
              instruction.definedFailure.propagation !=
                  lang::FailurePropagationKind::Destructor)
            continue;
          const auto *drop =
              body->findDropObligation(instruction.lifecycle.front().source);
          if (drop == nullptr ||
              lang::mirUniqueCleanupType(frontend.mir, drop->dropType.type) !=
                  drop->dropType)
            continue;
          ++cleanupInvokes;
          const auto targets = lang::mirDropDestructorTargets(drop->dropType);
          multiTargetSchedules += targets.size() >= 2;
          expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                     block.terminator.invokeInstruction == instruction.id,
                 "fallible pointee cleanup must own an explicit MIR invoke");
          for (int mutation = 0; mutation < 7; ++mutation) {
            auto forged = frontend.mir;
            auto *changed = lang::findMirBody(forged, address);
            auto &type = changed->dropObligations[drop->id - 1].dropType;
            auto *leaf = &type;
            while (!leaf->aggregate.empty())
              leaf = &leaf->aggregate.front();
            if (mutation == 0) {
              // The shared recipe now also represents direct source-class
              // leaves. Remove the identity actually present, so this remains
              // a malformed descriptor rather than a no-op mutation.
              if (leaf->uniquePointeeClass)
                leaf->uniquePointeeClass.reset();
              else
                leaf->classInstance.reset();
            } else if (mutation == 1) {
              // The first leaf can be passive allocation bookkeeping. Remove
              // an actual source destructor, not an already-empty optional.
              const auto removeDestructor =
                  [&](const auto &self, lang::MirDropType &step) -> bool {
                if (step.destructor) {
                  step.destructor.reset();
                  return true;
                }
                if (step.polymorphicCleanup) {
                  step.polymorphicCleanup = false;
                  return true;
                }
                for (auto &child : step.aggregate)
                  if (self(self, child))
                    return true;
                return false;
              };
              expect(
                  removeDestructor(removeDestructor, type),
                  "fallible cleanup mutation must remove an actual destructor");
            } else if (mutation == 2)
              leaf->uniquePointeeClass =
                  frontend.mir.classInstances().size() + 1;
            else if (mutation == 3)
              type.uniqueOwnerField += 1000000;
            else if (mutation == 4)
              type.uniqueOwnerBase = frontend.mir.classInstances().size() + 1;
            else if (mutation == 5)
              changed->blocks[block.id - 1].terminator.kind =
                  lang::MirTerminatorKind::Goto;
            else {
              if (targets.size() < 2)
                continue;
              leaf->destructor = targets.front() == leaf->destructor
                                     ? targets.back()
                                     : targets.front();
            }
            expect(!lang::verifyMirProgram(forged).valid(),
                   "forged unique-pointee cleanup identity must be rejected: " +
                       std::string(fixture) + " drop " +
                       std::to_string(drop->id) + " mutation " +
                       std::to_string(mutation));
          }
          const auto mutateSchedules =
              [&](const auto &self, const std::vector<std::size_t> &path,
                  const lang::MirDropType &step) -> void {
            if (!step.aggregate.empty()) {
              if (step.destructor) {
                for (int mutation = 0; mutation < 3; ++mutation) {
                  auto forged = frontend.mir;
                  auto *changed = lang::findMirBody(forged, address);
                  auto *selected =
                      &changed->dropObligations[drop->id - 1].dropType;
                  for (const auto index : path)
                    selected = &selected->aggregate[index];
                  if (mutation == 0)
                    selected->destructor.reset();
                  else if (mutation == 1)
                    selected->destructor =
                        selected->aggregate.front().destructor;
                  else
                    const_cast<lang::MirClassInstance *>(
                        forged.findClassInstance(*selected->classInstance))
                        ->requiresActiveDropState = false;
                  expect(!lang::verifyMirProgram(forged).valid(),
                         "aggregate cleanup must retain its exact guarded "
                         "source destructor before fields");
                }
              }
              for (int mutation = 0; mutation < 3; ++mutation) {
                auto forged = frontend.mir;
                auto *changed = lang::findMirBody(forged, address);
                auto *selected =
                    &changed->dropObligations[drop->id - 1].dropType;
                for (const auto index : path)
                  selected = &selected->aggregate[index];
                if (mutation == 0)
                  selected->aggregate.pop_back();
                else if (mutation == 1)
                  selected->aggregate.push_back(selected->aggregate.front());
                else {
                  if (selected->aggregate.size() < 2)
                    continue;
                  std::swap(selected->aggregate.front(),
                            selected->aggregate.back());
                }
                expect(!lang::verifyMirProgram(forged).valid(),
                       "missing, duplicate, or reordered aggregate cleanup "
                       "must fail");
              }
            }
            for (std::size_t index = 0; index < step.aggregate.size();
                 ++index) {
              auto childPath = path;
              childPath.push_back(index);
              self(self, childPath, step.aggregate[index]);
            }
          };
          mutateSchedules(mutateSchedules, {}, drop->dropType);
        }
      }
    }
    expect(cleanupInvokes >= 2,
           "fixture must exercise normal and failure-path pointee cleanup");
    if (std::string_view(fixture) == "aggregate_cleanup_failure.gti")
      expect(multiTargetSchedules != 0,
             "aggregate effects must retain both distinct destructor targets");
  }
}

void testCheckedOwnerReplacement() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/checked_owner_replacement.gti";
  std::ifstream input(path);
  expect(input.good(), "checked replacement fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = lang::Frontend().analyze(path.string(), source,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "checked replacement MIR must verify");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  std::size_t checked = 0;
  bool boundsOnly = false;
  bool boundsAndCleanup = false;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto &body = *lang::findMirBody(frontend.mir, address);
    for (const auto &block : body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CleanupBeforeAssign ||
            instruction.definedFailure.localOrigins.empty())
          continue;
        ++checked;
        boundsOnly |= instruction.definedFailure.propagation ==
                      lang::FailurePropagationKind::None;
        boundsAndCleanup |= instruction.definedFailure.propagation ==
                            lang::FailurePropagationKind::Destructor;
        const auto effects = lang::effects(frontend.mir, instruction);
        expect(
            effects.mayTrap && effects.writesPlace && effects.dropsValue &&
                !effects.removableWhenUnused && !effects.reorderable &&
                instruction.lifecycle.empty() && !instruction.ownership,
            "checked preparation must retain failure/cleanup effects without "
            "consuming RHS ownership");
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "bounds-only and fallible cleanup preparation must both invoke");
        for (int mutation = 0; mutation < 6; ++mutation) {
          auto forged = frontend.mir;
          auto &changed = *lang::findMirBody(forged, address);
          auto &producerBlock = changed.blocks[block.id - 1];
          auto &prepare = *std::find_if(producerBlock.instructions.begin(),
                                        producerBlock.instructions.end(),
                                        [&](const auto &candidate) {
                                          return candidate.id == instruction.id;
                                        });
          auto &commit =
              changed.blocks[block.terminator.target - 1].instructions.front();
          if (mutation == 0) {
            prepare.definedFailure.localOrigins.clear();
            prepare.localFailureSites.clear();
          } else if (mutation == 1) {
            commit.definedFailure.localOrigins =
                prepare.definedFailure.localOrigins;
            commit.localFailureSites = prepare.localFailureSites;
            prepare.definedFailure.localOrigins.clear();
            prepare.localFailureSites.clear();
          } else if (mutation == 2) {
            prepare.definedFailure.localOrigins.front()
                .outcomes.front()
                .detail = lang::DefinedFailureDetail::IntegerDivision;
          } else if (mutation == 3) {
            producerBlock.terminator.target =
                producerBlock.terminator.elseTarget;
          } else if (mutation == 4) {
            prepare.lifecycle = commit.lifecycle;
            commit.lifecycle.clear();
          } else {
            commit.replacementPair = 0;
          }
          expect(
              !lang::verifyMirProgram(forged).valid(),
              "forged bounds/cleanup/commit ordering must fail verification " +
                  std::to_string(mutation));
        }
      }
    }
  }
  expect(checked >= 5 && boundsOnly && boundsAndCleanup,
         "fixture must cover fresh/moved/class, chained, and bounds-only "
         "preparation");
}

void testNestedOwnerBorrowProjection() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/recursive_owner_projection_gap.gti";
  std::ifstream input(path);
  expect(input.good(), "nested-owner projection fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = lang::Frontend().analyze(path.string(), source,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "nested owner borrows must verify");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  const auto id = functionInstance(frontend, "main");
  const auto *body = functionBody(frontend, "main");
  expect(id && body != nullptr, "projection fixture must retain main");
  if (!id || body == nullptr)
    return;
  bool found = false;
  for (const auto &loan : body->loans) {
    const auto *place = body->findPlace(loan.source);
    if (loan.kind != lang::MirLoanKind::CallResult || place == nullptr ||
        place->root != lang::MirPlaceRootKind::Loan ||
        place->projections.empty())
      continue;
    found = true;
    const auto *parent = body->findLoan(place->loan);
    expect(parent != nullptr && parent->kind == lang::MirLoanKind::CallResult &&
               loan.parent == parent->id && place->projections.size() == 1 &&
               place->projections.front().kind ==
                   lang::MirProjectionKind::Field,
           "nested arrow must project from its live parent call result");
    if (parent == nullptr)
      continue;
    auto forged = frontend.mir;
    auto &changed = *functionBody(forged, *id);
    auto flattened = *changed.findPlace(parent->source);
    flattened.id = changed.places.size() + 1;
    flattened.type = place->type;
    flattened.traits = place->traits;
    flattened.access = place->access;
    flattened.sourceValue = place->sourceValue;
    flattened.projections.insert(flattened.projections.end(),
                                 place->projections.begin(),
                                 place->projections.end());
    changed.places.push_back(flattened);
    changed.loans[loan.id - 1].source = flattened.id;
    expect(
        hasVerificationError(
            lang::verifyMirProgram(forged),
            "projected call-result borrow must preserve the selected referent"),
        "flattening a projected call result into its owner must fail "
        "independent verification even when dependency keys compare equal");
  }
  expect(found, "fixture must exercise a projected call-result reborrow");
}

void testRecursiveCleanupContracts() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/recursive_cleanup_failure.gti";
  std::ifstream input(path);
  expect(input.good(), "recursive cleanup fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = lang::Frontend().analyze(path.string(), source,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "recursive cleanup MIR must verify");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  bool checkedReplacement = false;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto &body = *lang::findMirBody(frontend.mir, address);
    for (const auto &block : body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CleanupBeforeAssign ||
            instruction.definedFailure.propagation !=
                lang::FailurePropagationKind::Destructor)
          continue;
        checkedReplacement = true;
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id &&
                   instruction.lifecycle.empty(),
               "replacement cleanup must precede ownership publication through "
               "Invoke");
        const auto effect = lang::effects(frontend.mir, instruction);
        expect(effect.writesPlace && effect.dropsValue &&
                   !effect.removableWhenUnused && !effect.reorderable,
               "replacement cleanup must retain its non-removable cleanup "
               "effects");
        for (int mutation = 0; mutation < 6; ++mutation) {
          auto forged = frontend.mir;
          auto &changed = *lang::findMirBody(forged, address);
          auto &producerBlock = changed.blocks[block.id - 1];
          auto &prepare = *std::find_if(producerBlock.instructions.begin(),
                                        producerBlock.instructions.end(),
                                        [&](const auto &candidate) {
                                          return candidate.id == instruction.id;
                                        });
          auto &commit =
              changed.blocks[block.terminator.target - 1].instructions.front();
          if (mutation == 0)
            prepare.replacementCleanup.reset();
          else if (mutation == 1)
            prepare.replacementPair = 0;
          else if (mutation == 2)
            commit.replacementPair = 0;
          else if (mutation == 3)
            prepare.definedFailure.propagation =
                lang::FailurePropagationKind::None;
          else if (mutation == 4)
            prepare.replacementCleanup->recursiveCleanup = true;
          else
            producerBlock.terminator.target =
                producerBlock.terminator.elseTarget;
          expect(!lang::verifyMirProgram(forged).valid(),
                 "forged replacement preparation or success-only commit must "
                 "fail closed");
        }
        break;
      }
      if (checkedReplacement)
        break;
    }
    if (checkedReplacement)
      break;
  }
  expect(checkedReplacement,
         "recursive fixture must retain fallible replacement preparation");
  // Compare recipe effects against a separate graph walk over class metadata.
  // In particular, a mutually recursive pair must retain both bodies no
  // matter which class is selected as the root.
  bool sawMutualTargets = false;
  for (const auto &instance : frontend.mir.classInstances()) {
    const auto recipe = lang::mirUniqueCleanupType(frontend.mir, instance.type);
    if (!recipe)
      continue;
    std::vector<lang::HirClassInstanceId> visited;
    std::vector<lang::HirDestructorInstanceId> expected;
    const auto walk = [&](const auto &self,
                          const lang::SemanticType &type) -> void {
      if ((type.kind == lang::SemanticType::UniqueOwner ||
           type.kind == lang::SemanticType::PrefixStorage) &&
          type.arguments.size() == 1) {
        self(self, type.arguments.front());
        return;
      }
      for (const auto &selected : frontend.mir.classInstances()) {
        if (selected.type != type || std::find(visited.begin(), visited.end(),
                                               selected.id) != visited.end())
          continue;
        visited.push_back(selected.id);
        if (selected.destructor)
          expected.push_back(*selected.destructor);
        for (const auto &field : selected.declaredFields)
          if (field.requiresActiveCleanup)
            self(self, field.type);
        for (const auto &base : selected.bases)
          self(self, frontend.mir.findClassInstance(base.instance)->type);
      }
    };
    walk(walk, instance.type);
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()),
                   expected.end());
    expect(
        lang::mirDropDestructorTargets(*recipe) == expected,
        "finite recipe effects must cover the complete reachable class graph");
    sawMutualTargets |= expected.size() >= 2;
  }
  expect(sawMutualTargets,
         "recursive fixture must exercise distinct cleanup targets");
  bool checked = false;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto &body = *lang::findMirBody(frontend.mir, address);
    for (const auto &drop : body.dropObligations) {
      std::vector<std::size_t> selectedPath;
      const auto findReference = [&](const auto &self,
                                     const lang::MirDropType &step) -> bool {
        if (step.recursiveCleanup)
          return true;
        for (std::size_t index = 0; index < step.aggregate.size(); ++index) {
          selectedPath.push_back(index);
          if (self(self, step.aggregate[index]))
            return true;
          selectedPath.pop_back();
        }
        return false;
      };
      if (!findReference(findReference, drop.dropType))
        continue;
      checked = true;
      expect(!selectedPath.empty() &&
                 lang::mirUniqueCleanupType(frontend.mir, drop.dropType.type) ==
                     drop.dropType,
             "recursive cleanup must retain a complete independently derived "
             "root");
      expect(
          !lang::mirDropDestructorTargets(drop.dropType).empty() &&
              lang::mirDropMayRaiseDefinedFailure(frontend.mir, drop.dropType),
          "back-edges must not hide reachable source destructor effects");
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto forged = frontend.mir;
        auto *changed = lang::findMirBody(forged, address);
        auto &root = changed->dropObligations[drop.id - 1].dropType;
        auto *reference = &root;
        for (const auto index : selectedPath)
          reference = &reference->aggregate[index];
        if (mutation == 0)
          reference->recursiveCleanup = false;
        else if (mutation == 1)
          reference->classInstance = frontend.mir.classInstances().size() + 1;
        else if (mutation == 2)
          reference->destructor =
              lang::mirDropDestructorTargets(drop.dropType).front();
        else if (mutation == 3)
          reference->aggregate.push_back(*reference);
        else if (mutation == 4)
          root.recursiveCleanup = true;
        else
          reference->requiresActiveCleanup = false;
        expect(!lang::verifyMirProgram(forged).valid(),
               "forged recursive cleanup references must fail closed");
      }
      // One exact root supplies all reference mutations; the runtime matrix
      // separately covers the operation and failure-path inventory.
      break;
    }
    if (checked)
      break;
  }
  expect(checked, "recursive owner fixture must retain a finite MIR back-edge");
  expect(lang::MirPrinter().print(frontend.mir).find(";recursive=1") !=
             std::string::npos,
         "serialized cleanup must expose its finite back-edge");
}

void testCollectionCleanupContracts() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/collection_cleanup_failure.gti";
  std::ifstream input(path);
  expect(input.good(), "collection cleanup fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = lang::Frontend().analyze(path.string(), source,
                                                 {standardLibraryPrelude()}, {},
                                                 {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "collection cleanup MIR must verify");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  bool checkedObjectCleanup = false;
  bool checkedPrefix = false;
  bool checkedDestroy = false;
  bool checkedPop = false;
  bool checkedErase = false;
  bool checkedRelocation = false;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    const auto &body = *lang::findMirBody(frontend.mir, address);
    for (std::size_t b = 0; b < body.blocks.size(); ++b) {
      for (std::size_t i = 0; i < body.blocks[b].instructions.size(); ++i) {
        const auto &instruction = body.blocks[b].instructions[i];
        if (!instruction.elementCleanup ||
            !lang::mirDropMayRaiseDefinedFailure(frontend.mir,
                                                 *instruction.elementCleanup))
          continue;
        const bool pop =
            instruction.intrinsic == lang::IntrinsicKind::PrefixStoragePop;
        const bool destroy =
            instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy;
        const bool erase =
            instruction.intrinsic == lang::IntrinsicKind::PrefixStorageErase;
        if (erase && instruction.elementCleanup->classInstance &&
            !checkedRelocation &&
            frontend.mir
                .findClassInstance(*instruction.elementCleanup->classInstance)
                ->requiresActiveDropState) {
          const auto type = instruction.elementCleanup->type;
          const auto id = *instruction.elementCleanup->classInstance;
          expect(
              lang::mirTypeRelocationIsDefinedFailureFree(frontend.mir, type),
              "a guarded source destructor does not make generated relocation "
              "fail");
          for (int mutation = 0; mutation < 2; ++mutation) {
            auto forged = frontend.mir;
            auto *owner = const_cast<lang::MirClassInstance *>(
                forged.findClassInstance(id));
            if (mutation == 0)
              owner->moveConstructor = lang::SpecialMemberStatus::Declared;
            else
              owner->requiresActiveDropState = false;
            expect(
                !lang::mirTypeRelocationIsDefinedFailureFree(forged, type),
                "relocation cannot invent a generated move or lifecycle guard");
            expect(!lang::verifyMirProgram(forged).valid(),
                   "erase with a forged relocation contract must fail "
                   "verification");
          }
          checkedRelocation = true;
        }
        if ((pop && checkedPop) || (destroy && checkedDestroy) ||
            (erase && checkedErase))
          continue;
        expect(pop || destroy || erase,
               "element recipe belongs only to explicit removal");
        checkedPop |= pop;
        checkedDestroy |= destroy;
        checkedErase |= erase;
        if (destroy) {
          const auto &pointer = instruction.operands.front().type;
          expect(pointer.kind == lang::SemanticType::RawPointer &&
                     pointer.arguments.size() == 1 &&
                     instruction.elementCleanup ==
                         lang::mirUniqueCleanupType(
                             frontend.mir, pointer.arguments.front()) &&
                     instruction.unsafeOperation ==
                         lang::UnsafeOperationKind::ObjectLifetime &&
                     instruction.localFailureSites.empty() &&
                     instruction.definedFailure.localOrigins.empty(),
                 "source sparse cleanup uses the exact general payload recipe "
                 "and propagates the original destructor failure");
          const auto &producer = body.blocks[b];
          const auto *record =
              body.findFailureRecord(producer.terminator.failureRecord);
          expect(
              producer.terminator.kind == lang::MirTerminatorKind::Invoke &&
                  producer.terminator.invokeInstruction == instruction.id &&
                  record != nullptr &&
                  record->producerInstruction == instruction.id,
              "source destruction retains its failure record and cleanup edge");
          checkedObjectCleanup = true;
        }
        expect(instruction.definedFailure.propagation ==
                   lang::FailurePropagationKind::Destructor,
               "removal retains source-destructor propagation independently of "
               "local bounds");
        for (int mutation = 0; mutation < 6; ++mutation) {
          auto forged = frontend.mir;
          auto &changed =
              lang::findMirBody(forged, address)->blocks[b].instructions[i];
          if (mutation == 0)
            changed.elementCleanup.reset();
          else if (mutation == 1)
            changed.elementCleanup->type = lang::SemanticType::Int32;
          else if (mutation == 2)
            changed.elementCleanup->uniqueOwnerField = 1;
          else if (mutation == 3)
            changed.elementCleanup->requiresActiveCleanup = false;
          else if (mutation == 4)
            changed.definedFailure.propagation =
                lang::FailurePropagationKind::None;
          else
            changed.intrinsic = lang::IntrinsicKind::ObjectRelocate;
          expect(!lang::verifyMirProgram(forged).valid(),
                 "missing, forged, inactive or misplaced removal "
                 "recipes/effects must fail verification");
        }
      }
    }
    for (const auto &drop : body.dropObligations) {
      const auto inspect = [&](const auto &self, const lang::MirDropType &step,
                               std::vector<std::size_t> path) -> void {
        const bool prefix = step.type.kind == lang::SemanticType::PrefixStorage;
        if (prefix && !checkedPrefix &&
            lang::mirDropMayRaiseDefinedFailure(frontend.mir, step)) {
          expect(
              step.aggregate.size() == 1 &&
                  step.aggregate.front().type == step.type.arguments.front() &&
                  !step.aggregate.front().uniqueOwnerBase &&
                  step.aggregate.front().uniqueOwnerField == 0,
              "a live-element loop must retain one exact unprojected recipe");
          checkedPrefix = true;
          for (int mutation = 0; mutation < 5; ++mutation) {
            auto forged = frontend.mir;
            auto *changed = &lang::findMirBody(forged, address)
                                 ->dropObligations[drop.id - 1]
                                 .dropType;
            for (const auto index : path)
              changed = &changed->aggregate[index];
            if (mutation == 0)
              changed->aggregate.clear();
            else if (mutation == 1)
              changed->aggregate.push_back(changed->aggregate.front());
            else if (mutation == 2)
              changed->aggregate.front().type = lang::SemanticType::Int32;
            else if (mutation == 3)
              changed->aggregate.front().uniqueOwnerField = 1;
            else
              changed->aggregate.front().requiresActiveCleanup = false;
            expect(!lang::verifyMirProgram(forged).valid(),
                   "missing, duplicated, mistyped, projected, or inactive "
                   "collection element recipes must fail verification");
          }
        }
        for (std::size_t index = 0; index < step.aggregate.size(); ++index) {
          auto child = path;
          child.push_back(index);
          self(self, step.aggregate[index], std::move(child));
        }
      };
      inspect(inspect, drop.dropType, {});
    }
  }
  expect(
      checkedObjectCleanup && checkedPrefix,
      "both source-defined sparse destruction and live-prefix cleanup require "
      "verifier coverage");
  expect(checkedDestroy && checkedPop && checkedErase && checkedRelocation,
         "slot destruction, prefix pop/erase and generated relocation require "
         "verifier coverage");
}

void testConstructorPreparedParameterTransfers() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-constructor-prepared-parameter/main.gti", R"(
#include <std/string>
mut int32_t divisor = 1;
void consume(std::string value) { int32_t checked = 3 / divisor; }
std::string relay(std::string value) { return std::move(value); }
class Fields {
  std::string first = relay(std::string("first field"));
  std::string second = relay(std::string("second field"));
};
class Caller {
public:
  Caller() {
    consume(std::string("first"));
    consume(std::string("second"));
    int32_t checked = 7 / divisor;
  }
};
int main() { auto fields = Fields(); auto caller = Caller(); return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "constructor calls must transfer prepared parameters to their callee "
         "without being mistaken for unarmed subobjects");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  std::size_t transfers = 0;
  std::size_t explicitConstructorTransfers = 0;
  std::size_t generatedConstructorTransfers = 0;
  std::size_t fieldInitializerTransfers = 0;
  for (const auto address : lang::enumerateMirBodyAddresses(frontend.mir)) {
    if (address.kind != lang::MirBodyKind::Constructor &&
        address.kind != lang::MirBodyKind::FieldInitializers)
      continue;
    const auto &body = *lang::findMirBody(frontend.mir, address);
    for (const auto &block : body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call)
          continue;
        for (std::size_t eventIndex = 0;
             eventIndex < instruction.lifecycle.size(); ++eventIndex) {
          const auto &event = instruction.lifecycle[eventIndex];
          const auto *source = body.findDropObligation(event.source);
          if (event.kind != lang::MirLifecycleEventKind::TransferOut ||
              source == nullptr ||
              source->kind != lang::MirDropObligationKind::PreparedParameter)
            continue;
          ++transfers;
          if (address.kind == lang::MirBodyKind::FieldInitializers) {
            ++fieldInitializerTransfers;
          } else if (frontend.mir.findConstructorInstance(address.owner)
                         ->generatedDefault) {
            ++generatedConstructorTransfers;
          } else {
            ++explicitConstructorTransfers;
          }
          expect(
              lang::mirBodyRoutesFailureEdges(body) &&
                  !body.failureRecords.empty() &&
                  (instruction.definedFailure.empty() ||
                   (block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                    block.terminator.invokeInstruction == instruction.id)),
              "an owned call in a construction body retains failure routing");
          for (int mutation = 0; mutation < 4; ++mutation) {
            auto forged = frontend.mir;
            auto *changedBody = lang::findMirBody(forged, address);
            auto &changedBlock = changedBody->blocks[block.id - 1];
            auto &changed = *std::find_if(
                changedBlock.instructions.begin(),
                changedBlock.instructions.end(), [&](const auto &candidate) {
                  return candidate.id == instruction.id;
                });
            if (mutation == 0)
              changed.lifecycle.erase(changed.lifecycle.begin() + eventIndex);
            else if (mutation == 1)
              changed.lifecycle.push_back(changed.lifecycle[eventIndex]);
            else if (mutation == 2)
              ++changed.callSite;
            else {
              // A staged owner belonging to the other consume call must not
              // gain admission merely because it is a PreparedParameter.
              const auto other = std::find_if(
                  changedBody->dropObligations.begin(),
                  changedBody->dropObligations.end(), [&](const auto &drop) {
                    return drop.kind ==
                               lang::MirDropObligationKind::PreparedParameter &&
                           drop.id != event.source;
                  });
              expect(other != changedBody->dropObligations.end(),
                     "fixture needs a second prepared call owner");
              if (other == changedBody->dropObligations.end())
                continue;
              changed.lifecycle[eventIndex].source = other->id;
            }
            expect(!lang::verifyMirProgram(forged).valid(),
                   "missing, duplicate, misidentified, or cross-call prepared "
                   "transfers must fail verification (mutation " +
                       std::to_string(mutation) + ")");
          }
        }
      }
    }
  }
  expect(transfers == 6 && explicitConstructorTransfers == 2 &&
             generatedConstructorTransfers == 2 &&
             fieldInitializerTransfers == 2,
         "fixture must hand off exactly two string owners in each explicit "
         "constructor, generated constructor and field-initializer schedule");
}

void testBaseInitializationContracts() {
  const auto frontend = lang::Frontend().analyze(
      "/tmp/gti-base-initialization/main.gti", R"(
#include <std/memory>
mut int32_t divisor = 1;
class Base {
  std::unique_ptr<int32_t> value;
  int32_t checked;
public:
  Base() : value(std::make_unique<int32_t>(42)), checked(7 / divisor) {}
  virtual int32_t read() = 0;
};
class Derived : public Base {
  int32_t result;
public:
  Derived() : Base(), result(9 / divisor) {}
  int32_t read() override { return this.result; }
};
int main() { auto owner = std::make_unique<Derived>(); return owner->read(); }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "owning base initialization needs verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }
  std::size_t stages = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *entry = constructor.body.findBlock(constructor.body.entry);
    if (!entry || entry->instructions.empty() ||
        entry->instructions.front().kind !=
            lang::MirInstructionKind::InitializeBase)
      continue;
    ++stages;
    const auto &stage = entry->instructions.front();
    expect(
        constructor.effectSummary.traits.allocates &&
            constructor.effectSummary.traits.readsPlace,
        "base construction must contribute the selected constructor's effects");
    const auto *drop =
        constructor.body.findDropObligation(*stage.successResultDrop);
    const auto *base = constructor.body.findPlace(*stage.destination);
    const auto &failure =
        constructor.body.blocks[entry->terminator.elseTarget - 1];
    expect(
        drop && base && !drop->initiallyActive &&
            drop->kind == lang::MirDropObligationKind::ConstructionRollback &&
            base->root == lang::MirPlaceRootKind::This &&
            base->projections.front().kind == lang::MirProjectionKind::Base &&
            entry->terminator.kind == lang::MirTerminatorKind::Invoke &&
            entry->terminator.successLifecycle.size() == 1 &&
            entry->terminator.successLifecycle.front().target == drop->id &&
            failure.instructions.empty(),
        "base failure must not arm or drain the unfinished base");
    for (int mutation = 0; mutation < 17; ++mutation) {
      auto forged = frontend.mir;
      auto &constructors =
          const_cast<std::vector<lang::MirConstructorInstance> &>(
              forged.constructorInstances());
      auto &changed = *std::find_if(constructors.begin(), constructors.end(),
                                    [&](const auto &candidate) {
                                      return candidate.id == constructor.id;
                                    });
      auto &body = changed.body;
      auto &start = body.blocks[body.entry - 1];
      auto &operation = start.instructions.front();
      auto &place = body.places[base->id - 1];
      auto &rollback = body.dropObligations[drop->id - 1];
      switch (mutation) {
      case 0:
        operation.constructorInitializer = 0;
        break;
      case 1:
        operation.constructorTarget = constructor.id;
        break;
      case 2:
        place.projections.front().selection = constructor.owner;
        break;
      case 3:
        rollback.initiallyActive = true;
        break;
      case 4:
        operation.successResultDrop.reset();
        break;
      case 5:
        start.terminator.successLifecycle.clear();
        break;
      case 6:
        start.terminator.elseTarget = start.terminator.target;
        break;
      case 7: {
        auto alias = place;
        alias.id = body.places.size() + 1;
        body.places.push_back(alias);
        break;
      }
      case 8:
        place.initiallyAvailable = true;
        break;
      case 9:
        operation.definedFailure.propagation =
            lang::FailurePropagationKind::None;
        break;
      case 10:
        rollback.dropType.classInstance = constructor.owner;
        break;
      case 11:
      case 12:
        for (auto &block : body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.lifecycle.size() == 1 &&
                instruction.lifecycle.front().source == drop->id &&
                instruction.lifecycle.front().kind ==
                    (mutation == 11 ? lang::MirLifecycleEventKind::Drop
                                    : lang::MirLifecycleEventKind::TransferOut))
              instruction.lifecycle.clear();
          }
        }
        break;
      case 13:
        operation.kind = lang::MirInstructionKind::Initialize;
        break;
      case 14:
        changed.initializers[stage.constructorInitializer - 1]
            .generatedDefault = true;
        break;
      case 15:
        place.access = lang::AccessMode::ReadOnly;
        break;
      case 16:
        changed.effectSummary.traits.allocates = false;
        break;
      }
      expect(!lang::verifyMirProgram(forged).valid(),
             "forged base initialization must fail independent verification "
             "(mutation " +
                 std::to_string(mutation) + ")");
    }
  }
  expect(stages == 1,
         "one owning base must have exactly one MIR initialization stage");
}

} // namespace

int main() {
  testCanonicalMetadataAndMirSites();
  testContainerContractChecks();
  testInvokeEdgesAndFailureCleanup();
  testEmptyDescriptorContract();
  testHostedProgramEntryMetadata();
  testExternalSourceRouteIdentity();
  testMetadataAndSiteVerifierMutations();
  testCallerLocationAssignments();
  testCheckedIntegerOperationVerifierContracts();
  testUniqueOwnerBorrowFailureEdges();
  testScalarUniqueAllocationContracts();
  testClassUniqueAllocationContracts();
  testBaseInitializationContracts();
  testConstructorPreparedParameterTransfers();
  testCollectionCleanupContracts();
  testCheckedOwnerReplacement();
  testNestedOwnerBorrowProjection();
  testRecursiveCleanupContracts();
  testUniquePointeeCleanupContracts();
  return passed ? 0 : 1;
}
