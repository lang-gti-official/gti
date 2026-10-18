#include "compiler_test_fixture.h"

namespace {

void testFrontendBackendAndOptimizationPipeline() {
  const std::string source = R"(
int main() {
  bool folded = (1 < 2) and !false;
  int arithmetic = 1 + 2;
  if (arithmetic == 3) {
    return 0;
  }
  return arithmetic;
}
)";
  lang::FrontendResult frontend =
      lang::Frontend().analyze("frontend.gti", source);
  expect(frontend.canGenerateCode(),
         "the shared frontend should produce a checked program");
  expect(frontend.diagnostics.empty(),
         "valid frontend input should not produce diagnostics");
  expect(frontend.semantics.expressionCount() > 0,
         "the frontend should retain expression type information");

  const lang::HirFunctionInstance *mainInstance = nullptr;
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "main") {
      mainInstance = &instance;
      break;
    }
  }
  expect(mainInstance != nullptr && mainInstance->body.roots.size() == 4 &&
             mainInstance->body.statements.size() >= 5,
         "typed HIR should retain an executable function body");
  const lang::HirStatement *conditional =
      mainInstance == nullptr
          ? nullptr
          : mainInstance->body.findStatement(mainInstance->body.roots[2]);
  expect(conditional != nullptr &&
             conditional->kind == lang::HirStatementKind::If &&
             conditional->condition.has_value() &&
             conditional->body.has_value(),
         "typed HIR should retain explicit branch and condition edges");

  const lang::HirValue *logicalValue = nullptr;
  if (mainInstance != nullptr) {
    for (const lang::HirValue &value : mainInstance->body.values) {
      if (value.kind == lang::HirValueKind::Logical) {
        logicalValue = &value;
        break;
      }
    }
  }
  expect(logicalValue != nullptr && logicalValue->operands.size() == 2,
         "typed HIR values should retain stable operand IDs");

  const lang::OptimizationResult unoptimized = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O0);
  expect(unoptimized.foldedExpressionCount() == 0,
         "-O0 should not rewrite expressions");

  const lang::OptimizationResult optimized = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O1);
  expect(optimized.foldedExpressionCount() > 0,
         "-O1 should record safe constant expressions");
  const lang::ConstantValue *foldedLogical =
      logicalValue == nullptr ? nullptr
                              : optimized.replacement(logicalValue->id);
  expect(
      foldedLogical != nullptr && std::get_if<bool>(foldedLogical) != nullptr &&
          *std::get_if<bool>(foldedLogical),
      "constant folding should consume typed HIR and key results by value ID");

  // The backend consumes the verified optimized MIR exactly as the
  // production pipeline supplies it; the fold evidence is the MIR
  // authority's spelling.
  const lang::OptimizedProgram optimizedProgram =
      lang::OptimizationPipeline().run(
          lang::OptimizationRequest{.hir = frontend.hir,
                                    .mir = frontend.mir,
                                    .level = lang::OptimizationLevel::O1,
                                    .compatibility = &optimized});
  expect(optimizedProgram.valid(),
         "the owned MIR pipeline should optimize the fixture");
  std::unique_ptr<lang::Backend> backend = std::make_unique<lang::CppBackend>();
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimizedProgram.sourceMir, optimizedProgram.mir,
      lang::TargetInfo::host(), optimizedProgram.report.transformations);
  const lang::BackendArtifact artifact = backend->generate(lowered);
  expect(backend->name() == "cpp" &&
             artifact.kind == lang::BackendArtifactKind::Source &&
             artifact.extension == ".cpp",
         "the C++ emitter should be available through the backend contract");
  const std::size_t mainBodyStart = artifact.contents.find("function-instance");
  const std::string_view mainOnward =
      mainBodyStart == std::string::npos
          ? std::string_view()
          : std::string_view(artifact.contents).substr(mainBodyStart);
  expect(mainOnward.find("= true;") != std::string_view::npos &&
             mainOnward.find(" < __gti_mir_v_") == std::string_view::npos,
         "the C++ backend should consume optimization results");
  expect(mainOnward.find("= static_cast<std::int32_t>(3);") !=
             std::string_view::npos,
         "proven in-range integer arithmetic should fold to a typed constant");
  expect(!mainOnward.empty() &&
             mainOnward.find("if ((") == std::string_view::npos,
         "contextual binary conditions should not gain warning-producing "
         "parentheses in C++");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-frontend.gti", "int main() { return 0 }");
  expect(!invalid.canGenerateCode() && !invalid.syntaxValid,
         "the frontend should block code generation after syntax errors");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-P0001"),
         "the shared frontend should retain parser diagnostics");

  const std::filesystem::path overlayDirectory =
      std::filesystem::temp_directory_path();
  const std::filesystem::path overlayEntry =
      overlayDirectory / "gti-source-overlay-entry.gti";
  const std::filesystem::path overlayDependency =
      overlayDirectory / "gti-source-overlay-dependency.gti";
  const std::string dependencyKey =
      std::filesystem::weakly_canonical(overlayDependency).string();
  const lang::FrontendResult overlaid = lang::Frontend().analyze(
      overlayEntry,
      "#include \"gti-source-overlay-dependency.gti\"\n"
      "int main() { return dependency_value; }\n",
      {}, {{dependencyKey, "int dependency_value = 0;\n"}});
  expect(overlaid.canGenerateCode() && overlaid.diagnostics.empty(),
         "the frontend should analyze unsaved included-source overlays");
}

void testMirControlFlowAndOwnershipEffects() {
  const std::string source = R"(
class Resource {
  int32_t value;

public:
  Resource(int32_t initial) : value(initial) {}
  ~Resource() {}

  int32_t& read() {
    return this.value;
  }
};

class Bundle {
  Resource first;
  Resource second;

public:
  Bundle() : first(Resource(1)), second(Resource(2)) {}
};

class Gate {
  bool open;

public:
  Gate(bool value) : open(value) {}
  operator bool() { return this.open; }
};

int32_t inspect(int32_t& value) { return value; }

expected<int32_t, int32_t> checked(bool fail) {
  if (fail) {
    return unexpected(1);
  }
  return 2;
}

int32_t flow(bool stop) {
  Resource original = Resource(1);
  Resource moved = std::move(original);
  mut int32_t count = 0;
  while (count < 3) {
    count++;
    if (count == 1) {
      continue;
    }
    if (stop) {
      break;
    }
  }
  switch (count) {
  case 2:
    count += 1;
    break;
  default:
    break;
  }
  int32_t& value_ref = moved.read();
  Gate gate = Gate(stop);
  if (gate and inspect(count) > 0) {
    count += 1;
  }
  expected<int32_t, int32_t> result = checked(stop);
  if (result) {
    count += 1;
  }
  int32_t& count_ref = count;
  return inspect(count_ref) + inspect(value_ref);
}

int main() { [[discard]] flow(true); }
)";

  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "mir-foundation.gti", source, {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected MIR diagnostic: " << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             frontend.mir.valid(),
         "valid typed HIR should lower to a validated MIR program");

  const lang::HirClassInstance *bundle = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Bundle") {
      bundle = &instance;
      break;
    }
  }
  const lang::MirClassInstance *loweredBundle =
      bundle == nullptr ? nullptr : frontend.mir.findClassInstance(bundle->id);
  expect(
      loweredBundle != nullptr && bundle->fields.size() == 2 &&
          loweredBundle->fieldDropOrder.size() == 2 &&
          loweredBundle->fieldDropOrder[0].field == bundle->fields[1].binding &&
          loweredBundle->fieldDropOrder[1].field == bundle->fields[0].binding &&
          loweredBundle->fieldDropOrder[0].symbol != 0 &&
          loweredBundle->fieldDropOrder[1].symbol != 0,
      "MIR class metadata should retain stable field symbols and reverse "
      "lexical drop order");

  const auto findBody = [&](std::string_view name) -> const lang::MirBody * {
    for (const lang::HirFunctionInstance &instance :
         frontend.hir.functionInstances()) {
      if (instance.source == nullptr ||
          instance.source->name().lexeme != name) {
        continue;
      }
      const lang::MirFunctionInstance *lowered =
          frontend.mir.findFunctionInstance(instance.id);
      return lowered == nullptr ? nullptr : &lowered->body;
    }
    return nullptr;
  };
  const lang::MirBody *flow = findBody("flow");
  const lang::MirBody *read = findBody("read");
  const lang::MirBody *mainBody = findBody("main");
  expect(flow != nullptr && read != nullptr && mainBody != nullptr,
         "MIR should preserve stable links to global and member functions");
  if (flow == nullptr || read == nullptr || mainBody == nullptr) {
    return;
  }

  const auto instructionCount = [](const lang::MirBody &body,
                                   lang::MirInstructionKind kind) {
    std::size_t count = 0;
    for (const lang::MirBlock &block : body.blocks) {
      count += static_cast<std::size_t>(
          std::count_if(block.instructions.begin(), block.instructions.end(),
                        [kind](const lang::MirInstruction &instruction) {
                          return instruction.kind == kind;
                        }));
    }
    return count;
  };
  const auto terminatorCount = [](const lang::MirBody &body,
                                  lang::MirTerminatorKind kind) {
    return static_cast<std::size_t>(
        std::count_if(body.blocks.begin(), body.blocks.end(),
                      [kind](const lang::MirBlock &block) {
                        return block.terminator.kind == kind;
                      }));
  };

  const lang::MirBlock *entryBlock = flow->findBlock(flow->entry);
  expect(entryBlock != nullptr &&
             std::all_of(flow->blocks.begin(), flow->blocks.end(),
                         [](const lang::MirBlock &block) {
                           return block.terminator.kind !=
                                  lang::MirTerminatorKind::None;
                         }) &&
             entryBlock->reachable,
         "MIR bodies should form validated CFGs with a reachable entry and "
         "explicit terminators");
  expect(terminatorCount(*flow, lang::MirTerminatorKind::Branch) >= 3 &&
             terminatorCount(*flow, lang::MirTerminatorKind::Switch) == 1 &&
             terminatorCount(*flow, lang::MirTerminatorKind::Goto) >= 3 &&
             terminatorCount(*flow, lang::MirTerminatorKind::Return) == 1,
         "if, loop, break, continue, and switch flow should lower to explicit "
         "MIR edges");
  bool hasImplicitZeroReturn = false;
  for (const lang::MirBlock &block : mainBody->blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Return ||
        !block.terminator.value ||
        block.terminator.value->kind != lang::MirOperandKind::Constant ||
        !block.terminator.value->literal) {
      continue;
    }
    const std::uint64_t *value =
        std::get_if<std::uint64_t>(&*block.terminator.value->literal);
    hasImplicitZeroReturn = value != nullptr && *value == 0;
  }
  expect(hasImplicitZeroReturn,
         "MIR should preserve main's defined implicit zero return");
  expect(instructionCount(*flow, lang::MirInstructionKind::Move) >= 1 &&
             instructionCount(*flow, lang::MirInstructionKind::Borrow) >= 1 &&
             instructionCount(*flow, lang::MirInstructionKind::Call) >= 3 &&
             instructionCount(*flow, lang::MirInstructionKind::EndBorrow) >=
                 2 &&
             instructionCount(*flow, lang::MirInstructionKind::Drop) >= 2,
         "MIR should make moves, borrows, calls, borrow ends, and lexical "
         "drops explicit");

  const auto operationCount = [&](lang::MirInstructionKind kind,
                                  lang::MirOperation operation) {
    std::size_t count = 0;
    for (const lang::MirBlock &block : flow->blocks) {
      count += static_cast<std::size_t>(
          std::count_if(block.instructions.begin(), block.instructions.end(),
                        [&](const lang::MirInstruction &instruction) {
                          return instruction.kind == kind &&
                                 instruction.operation == operation;
                        }));
    }
    return count;
  };
  // A compound update is an ordered read/operate/commit schedule rather than
  // one closed mutation operation, so its arithmetic appears as an ordinary
  // Compute and its commit as an ordinary Assign.
  expect(operationCount(lang::MirInstructionKind::Compute,
                        lang::MirOperation::Add) >= 1 &&
             operationCount(lang::MirInstructionKind::Compute,
                            lang::MirOperation::Less) >= 1 &&
             operationCount(lang::MirInstructionKind::Compute,
                            lang::MirOperation::ExpectedHasValue) >= 1 &&
             operationCount(lang::MirInstructionKind::Assign,
                            lang::MirOperation::Assign) >= 1 &&
             instructionCount(*flow, lang::MirInstructionKind::Load) >= 1,
         "MIR should use backend-neutral scalar and mutation operations");

  const bool hasLogicalTemporary =
      std::any_of(flow->places.begin(), flow->places.end(),
                  [](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Temporary;
                  });
  const bool hasContextualBoolCall = std::any_of(
      flow->blocks.begin(), flow->blocks.end(),
      [](const lang::MirBlock &block) {
        return std::any_of(
            block.instructions.begin(), block.instructions.end(),
            [](const lang::MirInstruction &instruction) {
              return instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.receiver && instruction.result &&
                     instruction.info.type == lang::SemanticType::Bool;
            });
      });
  expect(hasLogicalTemporary && hasContextualBoolCall,
         "short-circuiting should use explicit CFG storage and the selected "
         "contextual bool call");

  bool validUseDef = flow->values.size() == flow->valueUses.size();
  bool hasInstructionUse = false;
  bool hasTerminatorUse = false;
  for (const lang::MirValue &value : flow->values) {
    const lang::MirBlock *definitionBlock =
        flow->findBlock(value.definitionBlock);
    validUseDef = validUseDef && definitionBlock != nullptr &&
                  value.definition != 0 &&
                  std::any_of(definitionBlock->instructions.begin(),
                              definitionBlock->instructions.end(),
                              [&](const lang::MirInstruction &instruction) {
                                return instruction.id == value.definition &&
                                       instruction.result == value.id;
                              });
    for (const lang::MirValueUse &use : flow->usesOf(value.id)) {
      validUseDef = validUseDef && use.value == value.id;
      hasInstructionUse = hasInstructionUse ||
                          use.kind == lang::MirValueUseKind::InstructionOperand;
      hasTerminatorUse =
          hasTerminatorUse || use.kind == lang::MirValueUseKind::Terminator;
    }
  }
  expect(validUseDef && hasInstructionUse && hasTerminatorUse,
         "every MIR value should have one definition and indexed instruction "
         "and terminator uses");

  bool cleanupBeforeReturn = false;
  for (const lang::MirBlock &block : flow->blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Return) {
      continue;
    }
    const bool endsBorrow = std::any_of(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::EndBorrow;
        });
    const bool dropsOwner =
        std::any_of(block.instructions.begin(), block.instructions.end(),
                    [](const lang::MirInstruction &instruction) {
                      return instruction.kind == lang::MirInstructionKind::Drop;
                    });
    cleanupBeforeReturn = endsBorrow && dropsOwner;
  }
  expect(cleanupBeforeReturn,
         "return blocks should end active borrows and drop lexical owners "
         "before transferring control");

  const auto escapingLoan = std::find_if(
      read->loans.begin(), read->loans.end(), [](const lang::MirLoan &loan) {
        return loan.kind == lang::MirLoanKind::Return && loan.escapes;
      });
  const lang::MirPlace *borrowedPlace =
      escapingLoan == read->loans.end() ? nullptr
                                        : read->findPlace(escapingLoan->source);
  expect(borrowedPlace != nullptr &&
             borrowedPlace->root == lang::MirPlaceRootKind::This &&
             std::any_of(borrowedPlace->projections.begin(),
                         borrowedPlace->projections.end(),
                         [](const lang::MirPlaceProjection &projection) {
                           return projection.kind ==
                                  lang::MirProjectionKind::Field;
                         }),
         "receiver-tied reference returns should retain an escaping loan from "
         "a projected 'this' place");
}

void testOrderedOrdinaryCallInputs() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("ordered-call-inputs.gti", R"(
extern "C" {
  void ordered_native_close(void* value);
}

class OrderedReceiver {
  mut void* handle = nullptr;

public:
  OrderedReceiver() {}

  int ordered_member(int scalar, int& observed, mut int& changed) mut {
    changed += scalar;
    return observed + changed;
  }

  void ordered_close() mut {
    unsafe { ordered_native_close(this.handle); }
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
  receiver.ordered_close();
  return first + second;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected ordered-call diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "ordinary scalar/reference calls should lower to verified ordered "
         "call inputs");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto hirFunction =
      [&](std::string_view name) -> const lang::HirFunctionInstance * {
    const auto found =
        std::find_if(frontend.hir.functionInstances().begin(),
                     frontend.hir.functionInstances().end(),
                     [&](const lang::HirFunctionInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->name().lexeme == name;
                     });
    return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
  };
  const lang::HirFunctionInstance *freeTarget = hirFunction("ordered_free");
  const lang::HirFunctionInstance *memberTarget = hirFunction("ordered_member");
  const lang::HirFunctionInstance *nativeTarget =
      hirFunction("ordered_native_close");
  const lang::HirFunctionInstance *closeTarget = hirFunction("ordered_close");
  const lang::HirFunctionInstance *mainTarget = hirFunction("main");
  const lang::MirFunctionInstance *mirMain =
      mainTarget == nullptr ? nullptr
                            : frontend.mir.findFunctionInstance(mainTarget->id);
  expect(freeTarget != nullptr && memberTarget != nullptr &&
             nativeTarget != nullptr && closeTarget != nullptr &&
             mainTarget != nullptr && mirMain != nullptr,
         "the ordered-call fixture should retain exact HIR and MIR targets");
  if (freeTarget == nullptr || memberTarget == nullptr ||
      nativeTarget == nullptr || closeTarget == nullptr ||
      mainTarget == nullptr || mirMain == nullptr) {
    return;
  }

  const auto hirCallTo = [&](lang::HirFunctionInstanceId target) {
    const auto found = std::find_if(
        mainTarget->body.values.begin(), mainTarget->body.values.end(),
        [&](const lang::HirValue &value) {
          return value.kind == lang::HirValueKind::Call &&
                 value.functionTarget == target;
        });
    return found == mainTarget->body.values.end() ? nullptr : &*found;
  };
  const lang::HirValue *freeCall = hirCallTo(freeTarget->id);
  const lang::HirValue *memberCall = hirCallTo(memberTarget->id);
  const auto nativeCall = std::find_if(
      closeTarget->body.values.begin(), closeTarget->body.values.end(),
      [&](const lang::HirValue &value) {
        return value.kind == lang::HirValueKind::Call &&
               value.functionTarget == nativeTarget->id;
      });
  const auto validArguments = [&](const lang::HirValue *call) {
    if (call == nullptr || !call->callPlan ||
        call->callPlan->arguments.size() != 3 || call->operands.size() < 3) {
      return false;
    }
    const std::array kinds{lang::HirCallInputKind::Value,
                           lang::HirCallInputKind::ReadBorrow,
                           lang::HirCallInputKind::MutableBorrow};
    for (std::size_t index = 0; index < kinds.size(); ++index) {
      const lang::HirCallArgument &argument = call->callPlan->arguments[index];
      if (argument.parameterIndex != index || argument.kind != kinds[index] ||
          argument.parameterType != call->parameterTypes[index] ||
          argument.value != call->operands[call->operands.size() - 3 + index]) {
        return false;
      }
    }
    return true;
  };
  const lang::HirValue *memberCallee =
      memberCall == nullptr || memberCall->operands.empty()
          ? nullptr
          : mainTarget->body.findValue(memberCall->operands.front());
  expect(freeCall != nullptr && freeCall->callPlan &&
             !freeCall->callPlan->receiver && validArguments(freeCall) &&
             memberCall != nullptr && memberCall->callPlan &&
             memberCall->callPlan->receiver &&
             memberCall->callPlan->receiver->kind ==
                 lang::HirCallInputKind::MutableBorrow &&
             memberCallee != nullptr && !memberCallee->operands.empty() &&
             memberCall->callPlan->receiver->value ==
                 memberCallee->operands.front() &&
             validArguments(memberCall) &&
             nativeCall != closeTarget->body.values.end() &&
             nativeCall->callPlan && !nativeCall->callPlan->receiver &&
             nativeCall->callPlan->arguments.size() == 1,
         "HIR should retain the exact receiver and source-ordered scalar, "
         "read-borrow, and mutable-borrow inputs without inventing a receiver "
         "for a free native call inside a method");

  const lang::MirFunctionInstance *mirClose =
      frontend.mir.findFunctionInstance(closeTarget->id);
  const lang::MirInstruction *nativeMirCall = nullptr;
  if (mirClose != nullptr) {
    for (const lang::MirBlock &block : mirClose->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget == nativeTarget->id;
          });
      if (found != block.instructions.end()) {
        nativeMirCall = &*found;
        break;
      }
    }
  }
  expect(mirClose != nullptr && nativeMirCall != nullptr &&
             !nativeMirCall->receiver && nativeMirCall->operands.size() == 1 &&
             nativeMirCall->callSite != 0,
         "MIR should materialize a raw-pointer argument conversion without "
         "falling back to the enclosing method receiver");

  const auto instructionForValue =
      [&](const lang::MirOperand &operand) -> const lang::MirInstruction * {
    if (operand.kind != lang::MirOperandKind::Value) {
      return nullptr;
    }
    const lang::MirValue *value = mirMain->body.findValue(operand.value);
    const lang::MirBlock *block =
        value == nullptr ? nullptr
                         : mirMain->body.findBlock(value->definitionBlock);
    if (block == nullptr) {
      return nullptr;
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.id == value->definition;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };
  const auto mirCallTo = [&](lang::HirFunctionInstanceId target) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
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
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const auto orderedMirCall = [&](const lang::HirValue *hirCall,
                                  const lang::MirInstruction *call,
                                  bool hasReceiver) {
    if (hirCall == nullptr || call == nullptr ||
        call->callSite != hirCall->id || call->hirValue != hirCall->id ||
        call->operands.size() != 3 ||
        call->receiver.has_value() != hasReceiver) {
      return false;
    }
    const std::array inputKinds{lang::HirCallInputKind::Value,
                                lang::HirCallInputKind::ReadBorrow,
                                lang::HirCallInputKind::MutableBorrow};
    const std::array operandKinds{lang::MirOperandKind::Value,
                                  lang::MirOperandKind::BorrowRead,
                                  lang::MirOperandKind::BorrowWrite};
    const lang::MirInstruction *previous = nullptr;
    if (call->receiver) {
      previous = instructionForValue(*call->receiver);
      if (previous == nullptr ||
          previous->kind != lang::MirInstructionKind::CallInput ||
          previous->callSite != call->callSite ||
          previous->callInputRole != lang::MirCallInputRole::Receiver ||
          previous->callInputKind != lang::HirCallInputKind::MutableBorrow ||
          previous->operands.size() != 1 ||
          previous->operands.front().kind !=
              lang::MirOperandKind::BorrowWrite) {
        return false;
      }
    }
    for (std::size_t index = 0; index < call->operands.size(); ++index) {
      const lang::MirInstruction *input =
          instructionForValue(call->operands[index]);
      if (input == nullptr ||
          input->kind != lang::MirInstructionKind::CallInput ||
          input->callSite != call->callSite ||
          input->callInputRole != lang::MirCallInputRole::Argument ||
          input->callInputIndex != index ||
          input->callInputKind != inputKinds[index] ||
          input->info.type != call->parameterTypes[index] ||
          input->operands.size() != 1 ||
          input->operands.front().kind != operandKinds[index] ||
          mirMain->body.usesOf(*input->result).size() != 1 ||
          (previous != nullptr && previous->id >= input->id)) {
        return false;
      }
      previous = input;
    }
    return previous != nullptr && previous->id < call->id;
  };
  const lang::MirInstruction *freeMirCall = mirCallTo(freeTarget->id);
  const lang::MirInstruction *memberMirCall = mirCallTo(memberTarget->id);
  expect(orderedMirCall(freeCall, freeMirCall, false) &&
             orderedMirCall(memberCall, memberMirCall, true),
         "MIR should form a one-use receiver, argument-zero, argument-one, "
         "argument-two, invocation chain");
}

void testOrderedRawReceiverInputs() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "ordered-raw-receiver.gti", R"(
#include <std/memory>
struct Item {
  mut std::unique_ptr<int> value;
  Item(int n) : value(std::make_unique<int>(n)) {}
};
struct Receiver {
  void Accept(Item item) mut {}
  void Observe(Item item) {}
};
int main() {
  mut Receiver receiver{};
  mut Receiver* pointer = &receiver;
  expected<Item, int> first = Item(1);
  expected<Item, int> second = Item(2);
  unsafe {
    pointer->Accept(std::move(first).value());
    pointer->Observe(std::move(second).value());
  }
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode() && frontend.mirValid,
         "raw member owning inputs should pass semantic and MIR checks");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  std::size_t plans = 0;
  for (const auto &instance : frontend.hir.functionInstances()) {
    if (!instance.source || instance.source->name().lexeme != "main")
      continue;
    for (const auto &call : instance.body.values) {
      if (call.kind != lang::HirValueKind::Call || call.operands.empty())
        continue;
      const auto *callee = instance.body.findValue(call.operands.front());
      if (!callee ||
          callee->unsafeOperation != lang::UnsafeOperationKind::RawMember)
        continue;
      ++plans;
      const auto *pointer = instance.body.findValue(callee->operands.front());
      expect(call.callPlan && call.callPlan->receiver && pointer &&
                 call.callPlan->receiver->value == callee->id &&
                 call.callPlan->receiver->type ==
                     pointer->info.type.arguments.front() &&
                 call.callPlan->arguments.size() == 1 &&
                 call.callPlan->arguments.front().kind ==
                     lang::HirCallInputKind::MoveValue &&
                 call.callPlan->receiver->kind ==
                     (plans == 1 ? lang::HirCallInputKind::MutableBorrow
                                 : lang::HirCallInputKind::ReadBorrow),
             "raw calls stage the exact semantic pointee receiver before "
             "owning arguments");
    }
    const auto *mir = frontend.mir.findFunctionInstance(instance.id);
    if (!mir) {
      expect(false, "raw receiver MIR body must exist");
      continue;
    }
    std::size_t stagedReceivers = 0;
    for (const auto &block : mir->body.blocks) {
      for (const auto &input : block.instructions) {
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputRole != lang::MirCallInputRole::Receiver ||
            input.operands.size() != 1)
          continue;
        const auto *place = mir->body.findPlace(input.operands.front().place);
        if (!place || place->projections.empty() ||
            place->projections.front().kind !=
                lang::MirProjectionKind::RawDereference)
          continue;
        ++stagedReceivers;
        lang::MirProgram malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(instance.id));
        for (auto &badBlock : changed->body.blocks)
          for (auto &badInput : badBlock.instructions)
            if (badInput.id == input.id)
              badInput.callSite = 0;
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "raw receiver stages cannot lose their exact call-site identity");
        for (int mutation = 0; mutation < 4; ++mutation) {
          lang::MirProgram missingEffect = frontend.mir;
          auto *effectBody = const_cast<lang::MirFunctionInstance *>(
              missingEffect.findFunctionInstance(instance.id));
          for (auto &effectBlock : effectBody->body.blocks)
            for (auto &effect : effectBlock.instructions) {
              const bool selected =
                  mutation < 2
                      ? effect.id == input.id
                      : effect.kind == lang::MirInstructionKind::Call &&
                            effect.callSite == input.callSite;
              if (selected) {
                if (mutation % 2 == 0)
                  effect.rawMemoryAccess = false;
                else
                  effect.unsafeOperation = lang::UnsafeOperationKind::None;
              }
            }
          expect(!lang::verifyMirProgram(missingEffect).valid(),
                 "raw receiver stage and call must both retain unsafe and "
                 "memory effects");
        }
      }
    }
    expect(stagedReceivers == 2,
           "both raw receiver access modes have explicit MIR stages");
  }
  expect(plans == 2, "both raw member calls have ordered HIR plans");
}

void testOrderedClassValueCallInputs() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("ordered-class-value-inputs.gti", R"(
class CopiedValue {
  int value;

public:
  CopiedValue(int input) : value(input) {}
  int read() { return this.value; }
};

class MovedValue {
  int value;

public:
  MovedValue(int input) : value(input) {}
  MovedValue(MovedValue& other) = delete;
  MovedValue(MovedValue&& other) = default;
  ~MovedValue() {}
  int read() { return this.value; }
};

class BorrowedValue {
  int& value;

public:
  BorrowedValue(int& input) : value(input) {}
  int read() { return this.value; }
};

int ordered_values(CopiedValue copied, MovedValue moved,
                   MovedValue temporary) {
  return copied.read() + moved.read() + temporary.read();
}

int observe_borrowed(BorrowedValue view) { return view.read(); }

int main() {
  mut int observed = 4;
  CopiedValue copied = CopiedValue(1);
  MovedValue moved = MovedValue(2);
  int total = ordered_values(copied, std::move(moved), MovedValue(3));
  return total + observe_borrowed(BorrowedValue(observed)) - 10;
}
)",
                               {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected class-value call diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "copyable and explicitly moved class arguments should lower to "
         "verified ordered parameter inputs");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto function = [&](std::string_view name) {
    const auto found =
        std::find_if(frontend.hir.functionInstances().begin(),
                     frontend.hir.functionInstances().end(),
                     [&](const lang::HirFunctionInstance &instance) {
                       return instance.source != nullptr &&
                              instance.source->name().lexeme == name;
                     });
    return found == frontend.hir.functionInstances().end() ? nullptr : &*found;
  };
  const lang::HirFunctionInstance *ordered = function("ordered_values");
  const lang::HirFunctionInstance *observe = function("observe_borrowed");
  const lang::HirFunctionInstance *main = function("main");
  const lang::MirFunctionInstance *mirMain =
      main == nullptr ? nullptr : frontend.mir.findFunctionInstance(main->id);
  expect(ordered != nullptr && observe != nullptr && main != nullptr &&
             mirMain != nullptr,
         "the class-value fixture should retain exact HIR and MIR functions");
  if (ordered == nullptr || observe == nullptr || main == nullptr ||
      mirMain == nullptr) {
    return;
  }

  const auto hirCallTo = [&](lang::HirFunctionInstanceId target) {
    const auto found =
        std::find_if(main->body.values.begin(), main->body.values.end(),
                     [&](const lang::HirValue &value) {
                       return value.kind == lang::HirValueKind::Call &&
                              value.functionTarget == target;
                     });
    return found == main->body.values.end() ? nullptr : &*found;
  };
  const lang::HirValue *orderedCall = hirCallTo(ordered->id);
  const lang::HirValue *borrowedCall = hirCallTo(observe->id);
  const std::array expectedKinds{lang::HirCallInputKind::CopyValue,
                                 lang::HirCallInputKind::MoveValue,
                                 lang::HirCallInputKind::MoveValue};
  bool exactHirPlan =
      orderedCall != nullptr && orderedCall->callPlan &&
      !orderedCall->callPlan->receiver &&
      orderedCall->callPlan->arguments.size() == expectedKinds.size();
  for (std::size_t index = 0; exactHirPlan && index < expectedKinds.size();
       ++index) {
    const lang::HirCallArgument &argument =
        orderedCall->callPlan->arguments[index];
    const lang::HirValue *source = main->body.findValue(argument.value);
    exactHirPlan =
        argument.parameterIndex == index &&
        argument.kind == expectedKinds[index] &&
        argument.parameterType == orderedCall->parameterTypes[index] &&
        source != nullptr &&
        source->info.category == (index == 0 ? lang::ValueCategory::Place
                                             : lang::ValueCategory::Value);
  }
  expect(exactHirPlan && borrowedCall != nullptr && borrowedCall->callPlan &&
             borrowedCall->callPlan->arguments.size() == 1 &&
             borrowedCall->callPlan->arguments.front().kind ==
                 lang::HirCallInputKind::MoveValue,
         "HIR should distinguish class copy and move parameter setup while "
         "using the same exact input plan for constructed borrowed carriers");

  const auto definitionFor = [&](const lang::MirOperand &operand) {
    if (operand.kind != lang::MirOperandKind::Value) {
      return static_cast<const lang::MirInstruction *>(nullptr);
    }
    const lang::MirValue *value = mirMain->body.findValue(operand.value);
    const lang::MirBlock *block =
        value == nullptr ? nullptr
                         : mirMain->body.findBlock(value->definitionBlock);
    if (block == nullptr) {
      return static_cast<const lang::MirInstruction *>(nullptr);
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.id == value->definition;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };
  const auto mirCallTo = [&](lang::HirFunctionInstanceId target) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
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
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const lang::MirInstruction *call = mirCallTo(ordered->id);
  const lang::MirInstruction *borrowedMirCall = mirCallTo(observe->id);
  std::array<const lang::MirInstruction *, 3> inputs{};
  if (call != nullptr && call->operands.size() == inputs.size()) {
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      inputs[index] = definitionFor(call->operands[index]);
    }
  }
  const auto exactPreparedParameter =
      [&](const lang::MirInstruction *input,
          lang::MirLifecycleEventKind expectedLifecycle) {
        if (input == nullptr || !input->preparedParameterDrop ||
            !input->destination || input->lifecycle.size() != 1 ||
            input->lifecycle.front().kind != expectedLifecycle ||
            input->lifecycle.front().target != *input->preparedParameterDrop) {
          return false;
        }
        const lang::MirDropObligation *obligation =
            mirMain->body.findDropObligation(*input->preparedParameterDrop);
        const lang::MirPlace *place =
            obligation == nullptr ? nullptr
                                  : mirMain->body.findPlace(obligation->place);
        const std::size_t transfers =
            call == nullptr
                ? 0
                : static_cast<std::size_t>(std::count_if(
                      call->lifecycle.begin(), call->lifecycle.end(),
                      [&](const lang::MirLifecycleEvent &event) {
                        return event.kind ==
                                   lang::MirLifecycleEventKind::TransferOut &&
                               event.source == *input->preparedParameterDrop;
                      }));
        return obligation != nullptr && place != nullptr &&
               obligation->kind ==
                   lang::MirDropObligationKind::PreparedParameter &&
               obligation->dropType.type == input->info.type &&
               place->root == lang::MirPlaceRootKind::Temporary &&
               *input->destination == place->id && transfers == 1;
      };
  const lang::MirPlace *copySource =
      inputs[0] == nullptr || inputs[0]->operands.empty()
          ? nullptr
          : mirMain->body.findPlace(inputs[0]->operands.front().place);
  const lang::MirInstruction *explicitMoveSource =
      inputs[1] == nullptr || inputs[1]->operands.empty()
          ? nullptr
          : definitionFor(inputs[1]->operands.front());
  const lang::MirInstruction *temporarySource =
      inputs[2] == nullptr || inputs[2]->operands.empty()
          ? nullptr
          : definitionFor(inputs[2]->operands.front());
  expect(call != nullptr && call->callSite != 0 &&
             call->operands.size() == inputs.size() && inputs[0] != nullptr &&
             inputs[1] != nullptr && inputs[2] != nullptr &&
             inputs[0]->callInputKind == lang::HirCallInputKind::CopyValue &&
             inputs[0]->operands.size() == 1 &&
             inputs[0]->operands.front().kind == lang::MirOperandKind::Copy &&
             copySource != nullptr &&
             exactPreparedParameter(inputs[0],
                                    lang::MirLifecycleEventKind::Initialize) &&
             inputs[1]->callInputKind == lang::HirCallInputKind::MoveValue &&
             inputs[2]->callInputKind == lang::HirCallInputKind::MoveValue &&
             exactPreparedParameter(inputs[1],
                                    lang::MirLifecycleEventKind::Reparent) &&
             exactPreparedParameter(inputs[2],
                                    lang::MirLifecycleEventKind::Reparent) &&
             explicitMoveSource != nullptr &&
             explicitMoveSource->kind == lang::MirInstructionKind::Move &&
             temporarySource != nullptr &&
             temporarySource->kind == lang::MirInstructionKind::Construct &&
             inputs[0]->id < inputs[1]->id && inputs[1]->id < inputs[2]->id &&
             inputs[2]->id < call->id && call->lifecycle.size() == 3 &&
             borrowedMirCall != nullptr && borrowedMirCall->callSite != 0 &&
             borrowedMirCall->operands.size() == 1 &&
             definitionFor(borrowedMirCall->operands.front()) != nullptr &&
             definitionFor(borrowedMirCall->operands.front())->kind ==
                 lang::MirInstructionKind::CallInput &&
             definitionFor(borrowedMirCall->operands.front())
                 ->preparedParameterDrop.has_value(),
         "MIR should stage each class-value parameter in caller-owned storage, "
         "and transfer both owning and borrowed-carrier shells exactly once "
         "when the callee begins");

  const std::string dump = lang::MirPrinter().print(mirMain->body);
  expect(dump.starts_with("mir-body-v87\n") &&
             dump.find("call-input-kind=copy-value") != std::string::npos &&
             dump.find("call-input-kind=move-value") != std::string::npos &&
             dump.find("prepared-parameter-drop=") != std::string::npos,
         "MIR v24 should serialize staged class-value parameter ownership");
}

void testOrderedOrdinaryConstructorInputs() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("ordered-constructor-inputs.gti", R"(
class CopiedValue {
  int value;
public:
  CopiedValue(int input) : value(input) {}
};

class MovedValue {
  int value;
public:
  MovedValue(int input) : value(input) {}
  MovedValue(MovedValue& other) = delete;
  MovedValue(MovedValue&& other) = default;
  ~MovedValue() {}
};

class Product {
public:
  Product(int scalar, int& observed, mut int& changed, CopiedValue copied,
          MovedValue moved, MovedValue temporary) {
    changed += scalar;
  }
};

class Empty {
public:
  Empty() {}
};

int main() {
  mut int observed = 2;
  mut int changed = 3;
  CopiedValue copied = CopiedValue(1);
  CopiedValue copied_again = CopiedValue(copied);
  MovedValue moved = MovedValue(2);
  Product product = Product(4, observed, changed, copied_again,
                            std::move(moved), MovedValue(3));
  Empty empty = Empty();
  return changed - 7;
}
)",
                               {standardLibraryPrelude()});
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected ordered-constructor diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "ordinary constructors should lower supported arguments through a "
         "verified ordered parameter schedule");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirConstructorInstance *productTarget = nullptr;
  const lang::HirConstructorInstance *emptyTarget = nullptr;
  for (const lang::HirConstructorInstance &constructor :
       frontend.hir.constructorInstances()) {
    if (constructor.source != nullptr &&
        constructor.source->name().lexeme == "Product") {
      productTarget = &constructor;
    }
    if (constructor.source != nullptr &&
        constructor.source->name().lexeme == "Empty") {
      emptyTarget = &constructor;
    }
  }
  const lang::HirFunctionInstance *mainTarget = nullptr;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "main") {
      mainTarget = &function;
      break;
    }
  }
  const lang::HirValue *productConstruction = nullptr;
  const lang::HirValue *copyConstruction = nullptr;
  const lang::HirValue *emptyConstruction = nullptr;
  if (mainTarget != nullptr && productTarget != nullptr) {
    for (const lang::HirValue &value : mainTarget->body.values) {
      if (value.constructorTarget == productTarget->id) {
        productConstruction = &value;
      }
      if (value.constructorKind == lang::ConstructorKind::Copy) {
        copyConstruction = &value;
      }
      if (emptyTarget != nullptr &&
          value.constructorTarget == emptyTarget->id) {
        emptyConstruction = &value;
      }
    }
  }

  const std::array expectedKinds{
      lang::HirCallInputKind::Value,         lang::HirCallInputKind::ReadBorrow,
      lang::HirCallInputKind::MutableBorrow, lang::HirCallInputKind::CopyValue,
      lang::HirCallInputKind::MoveValue,     lang::HirCallInputKind::MoveValue};
  bool exactHirPlan =
      productConstruction != nullptr && productConstruction->callPlan &&
      !productConstruction->callPlan->receiver &&
      productConstruction->callPlan->arguments.size() == expectedKinds.size();
  for (std::size_t index = 0; exactHirPlan && index < expectedKinds.size();
       ++index) {
    const lang::HirCallArgument &argument =
        productConstruction->callPlan->arguments[index];
    exactHirPlan =
        argument.parameterIndex == index &&
        argument.kind == expectedKinds[index] &&
        argument.parameterType == productConstruction->parameterTypes[index] &&
        mainTarget->body.findValue(argument.value) != nullptr;
  }
  expect(productTarget != nullptr && mainTarget != nullptr && exactHirPlan &&
             copyConstruction != nullptr && !copyConstruction->callPlan &&
             emptyConstruction != nullptr && !emptyConstruction->callPlan,
         "HIR should retain source-ordered ordinary-constructor inputs while "
         "leaving special and zero-argument construction outside this bounded "
         "schedule");

  const lang::MirFunctionInstance *mirMain =
      mainTarget == nullptr ? nullptr
                            : frontend.mir.findFunctionInstance(mainTarget->id);
  const lang::MirInstruction *construct = nullptr;
  if (mirMain != nullptr && productTarget != nullptr) {
    for (const lang::MirBlock &block : mirMain->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.constructorTarget == productTarget->id) {
          construct = &instruction;
          break;
        }
      }
      if (construct != nullptr) {
        break;
      }
    }
  }
  const auto definitionFor =
      [&](const lang::MirOperand &operand) -> const lang::MirInstruction * {
    if (mirMain == nullptr || operand.kind != lang::MirOperandKind::Value) {
      return nullptr;
    }
    const lang::MirValue *value = mirMain->body.findValue(operand.value);
    const lang::MirBlock *block =
        value == nullptr ? nullptr
                         : mirMain->body.findBlock(value->definitionBlock);
    if (block == nullptr) {
      return nullptr;
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.id == value->definition;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };

  std::array<const lang::MirInstruction *, 6> inputs{};
  if (construct != nullptr && construct->operands.size() == inputs.size()) {
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      inputs[index] = definitionFor(construct->operands[index]);
    }
  }
  const auto exactPreparedOwner = [&](const lang::MirInstruction *input,
                                      bool copied) {
    if (input == nullptr || !input->preparedParameterDrop ||
        !input->destination || input->operands.size() != 1 ||
        input->lifecycle.size() != 1) {
      return false;
    }
    const auto *drop =
        mirMain->body.findDropObligation(*input->preparedParameterDrop);
    const auto *place = mirMain->body.findPlace(*input->destination);
    const auto &event = input->lifecycle.front();
    if (drop == nullptr || place == nullptr ||
        drop->kind != lang::MirDropObligationKind::PreparedParameter ||
        drop->initiallyActive || drop->place != place->id ||
        drop->dropType.type != input->info.type ||
        place->root != lang::MirPlaceRootKind::Temporary ||
        place->type != input->info.type || place->initiallyAvailable ||
        !place->projections.empty() || event.target != drop->id ||
        event.conditional || event.failureCleanup) {
      return false;
    }
    if (copied) {
      return input->operands.front().kind == lang::MirOperandKind::Copy &&
             event.kind == lang::MirLifecycleEventKind::Initialize &&
             event.source == 0;
    }
    const auto *source = mirMain->body.findDropObligation(event.source);
    const auto *sourcePlace =
        source == nullptr ? nullptr : mirMain->body.findPlace(source->place);
    return input->operands.front().kind == lang::MirOperandKind::Value &&
           event.kind == lang::MirLifecycleEventKind::Reparent &&
           source != nullptr && source->dropType == drop->dropType &&
           sourcePlace != nullptr &&
           sourcePlace->root == lang::MirPlaceRootKind::Value &&
           sourcePlace->value == input->operands.front().value;
  };
  bool exactMirPlan =
      construct != nullptr && productConstruction != nullptr &&
      construct->callSite == productConstruction->id &&
      construct->parameterTypes == productConstruction->parameterTypes &&
      construct->operands.size() == inputs.size();
  const lang::MirInstruction *previous = nullptr;
  for (std::size_t index = 0; exactMirPlan && index < inputs.size(); ++index) {
    const lang::MirInstruction *input = inputs[index];
    exactMirPlan = input != nullptr &&
                   input->kind == lang::MirInstructionKind::CallInput &&
                   input->callSite == construct->callSite &&
                   input->callInputRole == lang::MirCallInputRole::Argument &&
                   input->callInputIndex == index &&
                   input->callInputKind == expectedKinds[index] &&
                   input->info.type == construct->parameterTypes[index] &&
                   input->operands.size() == 1 && input->result &&
                   mirMain->body.usesOf(*input->result).size() == 1 &&
                   (previous == nullptr || previous->id < input->id);
    previous = input;
  }
  bool exactInvocationTransfers =
      construct != nullptr && construct->lifecycle.size() == 3;
  for (std::size_t index = 3; exactInvocationTransfers && index < inputs.size();
       ++index) {
    const auto *input = inputs[index];
    const auto &event = construct->lifecycle[index - 3];
    exactInvocationTransfers =
        input != nullptr && input->preparedParameterDrop &&
        event.kind == lang::MirLifecycleEventKind::TransferOut &&
        event.source == *input->preparedParameterDrop && event.target == 0 &&
        !event.conditional && !event.failureCleanup;
    for (std::size_t earlier = 3; exactInvocationTransfers && earlier < index;
         ++earlier)
      exactInvocationTransfers = inputs[earlier]->preparedParameterDrop !=
                                 input->preparedParameterDrop;
  }
  expect(exactMirPlan && previous != nullptr && previous->id < construct->id &&
             inputs[0]->operands.front().kind == lang::MirOperandKind::Value &&
             inputs[1]->operands.front().kind ==
                 lang::MirOperandKind::BorrowRead &&
             inputs[2]->operands.front().kind ==
                 lang::MirOperandKind::BorrowWrite &&
             inputs[3]->operands.front().kind == lang::MirOperandKind::Copy &&
             exactPreparedOwner(inputs[3], true) &&
             exactPreparedOwner(inputs[4], false) &&
             exactPreparedOwner(inputs[5], false) && exactInvocationTransfers,
         "MIR should prepare constructor arguments left to right, retain "
         "distinct caller-owned copy/move stages, and transfer them only when "
         "the constructor begins");

  const auto mutableMain = [&](lang::MirProgram &program) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    return mainTarget == nullptr || mainTarget->id == 0 ||
                   mainTarget->id > functions.size()
               ? static_cast<lang::MirFunctionInstance *>(nullptr)
               : &functions[mainTarget->id - 1];
  };
  const auto hasVerificationMessage =
      [](const lang::MirVerificationResult &result, std::string_view text) {
        return std::any_of(result.errors.begin(), result.errors.end(),
                           [&](const lang::MirVerificationError &error) {
                             return error.message.find(text) !=
                                    std::string::npos;
                           });
      };
  const auto mutableProductConstruction =
      [&](lang::MirFunctionInstance &function) -> lang::MirInstruction * {
    for (lang::MirBlock &block : function.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            productTarget != nullptr &&
            instruction.constructorTarget == productTarget->id) {
          return &instruction;
        }
      }
    }
    return nullptr;
  };

  lang::MirProgram forgedEmptySchedule = frontend.mir;
  for (int mutation = 0; mutation != 2; ++mutation) {
    auto malformed = frontend.mir;
    auto *changedMain = mutableMain(malformed);
    auto *changedConstruct = changedMain == nullptr
                                 ? nullptr
                                 : mutableProductConstruction(*changedMain);
    bool changed =
        changedConstruct != nullptr && changedConstruct->lifecycle.size() == 3;
    if (changed) {
      const auto transfer = changedConstruct->lifecycle.back();
      changedConstruct->lifecycle.pop_back();
      if (mutation == 1) {
        changed = false;
        for (auto &block : changedMain->body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.preparedParameterDrop == transfer.source) {
              instruction.lifecycle.push_back(transfer);
              changed = true;
            }
      }
    }
    expect(changed && !lang::verifyMirProgram(malformed).valid(),
           mutation == 0
               ? "MIR verification should reject missing constructor "
                 "invocation ownership transfer"
               : "MIR verification should reject an early argument-stage "
                 "transfer before constructor invocation");
  }
  lang::MirFunctionInstance *forgedEmptyMain = mutableMain(forgedEmptySchedule);
  lang::MirInstruction *forgedEmptyConstruct = nullptr;
  if (forgedEmptyMain != nullptr && emptyTarget != nullptr) {
    for (lang::MirBlock &block : forgedEmptyMain->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.constructorTarget == emptyTarget->id) {
          forgedEmptyConstruct = &instruction;
          break;
        }
      }
      if (forgedEmptyConstruct != nullptr) {
        break;
      }
    }
  }
  if (forgedEmptyConstruct != nullptr) {
    forgedEmptyConstruct->callSite = forgedEmptyConstruct->hirValue;
  }
  const lang::MirVerificationResult forgedEmptyResult =
      lang::verifyMirProgram(forgedEmptySchedule);
  expect(forgedEmptyConstruct != nullptr && !forgedEmptyResult.valid() &&
             hasVerificationMessage(forgedEmptyResult,
                                    "invalid shape or reference"),
         "MIR verification should reject an ordered marker on zero-argument "
         "construction");

  lang::MirProgram bypassed = frontend.mir;
  lang::MirFunctionInstance *bypassedMain = mutableMain(bypassed);
  lang::MirInstruction *bypassedConstruct =
      bypassedMain == nullptr ? nullptr
                              : mutableProductConstruction(*bypassedMain);
  bool bypassedInput = false;
  if (bypassedMain != nullptr && bypassedConstruct != nullptr &&
      !bypassedConstruct->operands.empty()) {
    const lang::MirOperand prepared = bypassedConstruct->operands.front();
    const lang::MirValue *preparedValue =
        prepared.kind == lang::MirOperandKind::Value
            ? bypassedMain->body.findValue(prepared.value)
            : nullptr;
    lang::MirInstruction *input = nullptr;
    if (preparedValue != nullptr) {
      for (lang::MirBlock &block : bypassedMain->body.blocks) {
        const auto found =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const lang::MirInstruction &instruction) {
                           return instruction.id == preparedValue->definition;
                         });
        if (found != block.instructions.end()) {
          input = &*found;
          break;
        }
      }
    }
    if (input != nullptr && !input->operands.empty()) {
      bypassedConstruct->operands.front() = input->operands.front();
      bypassedInput = lang::rebuildMirValueUses(bypassedMain->body);
    }
  }
  const lang::MirVerificationResult bypassedResult =
      lang::verifyMirProgram(bypassed);
  expect(
      bypassedInput && !bypassedResult.valid() &&
          hasVerificationMessage(bypassedResult, "exactly one executable use"),
      "MIR verification should reject a constructor that bypasses its "
      "prepared argument checkpoint");

  lang::MirProgram wrongRole = frontend.mir;
  lang::MirFunctionInstance *wrongRoleMain = mutableMain(wrongRole);
  lang::MirInstruction *wrongRoleConstruct =
      wrongRoleMain == nullptr ? nullptr
                               : mutableProductConstruction(*wrongRoleMain);
  bool changedRole = false;
  if (wrongRoleMain != nullptr && wrongRoleConstruct != nullptr) {
    for (lang::MirBlock &block : wrongRoleMain->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callSite == wrongRoleConstruct->callSite &&
            instruction.callInputIndex == 0) {
          instruction.callInputRole = lang::MirCallInputRole::Receiver;
          changedRole = true;
          break;
        }
      }
      if (changedRole) {
        break;
      }
    }
  }
  const lang::MirVerificationResult wrongRoleResult =
      lang::verifyMirProgram(wrongRole);
  expect(changedRole && !wrongRoleResult.valid() &&
             hasVerificationMessage(wrongRoleResult,
                                    "does not match its ordered invocation"),
         "MIR verification should reject a constructor argument forged into "
         "a receiver checkpoint");

  lang::MirProgram reordered = frontend.mir;
  lang::MirFunctionInstance *reorderedMain = mutableMain(reordered);
  lang::MirInstruction *reorderedConstruct =
      reorderedMain == nullptr ? nullptr
                               : mutableProductConstruction(*reorderedMain);
  bool reorderedInputs = false;
  if (reorderedMain != nullptr && reorderedConstruct != nullptr) {
    for (lang::MirBlock &block : reorderedMain->body.blocks) {
      std::vector<std::size_t> indices;
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callSite == reorderedConstruct->callSite) {
          indices.push_back(index);
        }
      }
      if (indices.size() >= 2) {
        std::iter_swap(block.instructions.begin() +
                           static_cast<std::ptrdiff_t>(indices[0]),
                       block.instructions.begin() +
                           static_cast<std::ptrdiff_t>(indices[1]));
        reorderedInputs = true;
        break;
      }
    }
  }
  const lang::MirVerificationResult reorderedResult =
      lang::verifyMirProgram(reordered);
  expect(reorderedInputs && !reorderedResult.valid(),
         "MIR verification should reject reordered constructor parameter "
         "checkpoints");
}

void testMirTemporaryAndDropObligations() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("mir-lifetimes.gti", R"(
class Token {
public:
  int value;
  Token(int input) : value(input) {}
  ~Token() {}
  operator bool() { return this.value != 0; }
};

class Marker {
public:
  Marker(int input) {}
  ~Marker() {}
};

Token make_token(int value) { return Token(value); }

class Holder {
  Token field = make_token(10);
  Token other = make_token(11);
};

int consume(Token left, Token right) {
  return left.value + right.value;
}

void observe(bool value) {}

int lifetime(bool choose) {
  [[discard]] make_token(0);
  [[discard]] Marker(10);
  [[discard]] consume(make_token(1), make_token(2));
  Token selected = choose ? make_token(3) : make_token(4);
  bool short_circuit = false && make_token(5);
  observe(choose && (make_token(8), make_token(18), true));
  observe(choose ? (make_token(9), true) : false);
  for (mut int index = 0; index < 2; index++) {
    [[discard]] make_token(index);
    if (index == 0) {
      continue;
    }
    break;
  }
  return consume(make_token(6), make_token(7));
}

int main() { return lifetime(true) - 13; }
)");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << "Unexpected MIR lifetime diagnostic: " << diagnostic.message
                << '\n';
    }
  }
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "temporary and lexical drop obligations should verify across calls, "
         "conditionals, short-circuiting, loops, and returns");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::FunctionDecl *lifetimeSource =
      findTopLevelFunction(frontend.program, "lifetime");
  const lang::ForStmt *lifetimeLoop = nullptr;
  const lang::ReturnStmt *lifetimeReturn = nullptr;
  std::size_t semanticStatementFullExpressions = 0;
  if (lifetimeSource != nullptr && lifetimeSource->body() != nullptr) {
    for (const lang::StmtPtr &statement :
         lifetimeSource->body()->statements()) {
      semanticStatementFullExpressions +=
          frontend.semantics.fullExpressionsFor(*statement).size();
      if (lifetimeLoop == nullptr) {
        lifetimeLoop = dynamic_cast<const lang::ForStmt *>(statement.get());
      }
      if (const auto *candidate =
              dynamic_cast<const lang::ReturnStmt *>(statement.get())) {
        lifetimeReturn = candidate;
      }
    }
  }
  expect(lifetimeSource != nullptr && lifetimeLoop != nullptr &&
             lifetimeReturn != nullptr &&
             frontend.semantics.fullExpressionsFor(*lifetimeLoop).size() == 2 &&
             frontend.semantics.fullExpressionsFor(*lifetimeReturn).size() ==
                 1 &&
             semanticStatementFullExpressions != 0,
         "semantics should select AST full-expression roots, including "
         "separate for-condition and increment endpoints");

  const auto hirFunction =
      [&](std::string_view name) -> const lang::HirFunctionInstance * {
    for (const lang::HirFunctionInstance &instance :
         frontend.hir.functionInstances()) {
      if (instance.source != nullptr &&
          instance.source->name().lexeme == name) {
        return &instance;
      }
    }
    return nullptr;
  };
  const lang::HirFunctionInstance *hirLifetime = hirFunction("lifetime");
  const lang::HirFunctionInstance *hirConsume = hirFunction("consume");
  const lang::HirFunctionInstance *hirMake = hirFunction("make_token");
  const lang::MirFunctionInstance *mirLifetime =
      hirLifetime == nullptr
          ? nullptr
          : frontend.mir.findFunctionInstance(hirLifetime->id);
  const lang::MirFunctionInstance *mirConsume =
      hirConsume == nullptr ? nullptr
                            : frontend.mir.findFunctionInstance(hirConsume->id);
  const lang::MirFunctionInstance *mirMake =
      hirMake == nullptr ? nullptr
                         : frontend.mir.findFunctionInstance(hirMake->id);

  const lang::HirClassInstance *token = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Token") {
      token = &instance;
      break;
    }
  }
  const lang::MirClassInstance *mirToken =
      token == nullptr ? nullptr : frontend.mir.findClassInstance(token->id);
  const lang::HirClassInstance *holder = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Holder") {
      holder = &instance;
      break;
    }
  }
  const lang::MirClassInstance *mirHolder =
      holder == nullptr ? nullptr : frontend.mir.findClassInstance(holder->id);
  const lang::HirConstructorInstance *tokenConstructor = nullptr;
  if (token != nullptr) {
    const auto found =
        std::find_if(frontend.hir.constructorInstances().begin(),
                     frontend.hir.constructorInstances().end(),
                     [&](const lang::HirConstructorInstance &instance) {
                       return instance.owner == token->id &&
                              instance.source != nullptr &&
                              !instance.source->initializers().empty();
                     });
    if (found != frontend.hir.constructorInstances().end()) {
      tokenConstructor = &*found;
    }
  }
  bool constructorRootMapping = false;
  if (tokenConstructor != nullptr) {
    const lang::ConstructorInitializer &sourceInitializer =
        tokenConstructor->source->initializers().front();
    const std::vector<lang::SemanticFullExpression> &semanticExpressions =
        frontend.semantics.fullExpressionsFor(sourceInitializer);
    if (semanticExpressions.size() == 1 &&
        tokenConstructor->body.fullExpressions.size() == 1) {
      const lang::HirFullExpression &hirExpression =
          tokenConstructor->body.fullExpressions.front();
      const lang::HirValue *hirRoot =
          hirExpression.roots.size() == 1
              ? tokenConstructor->body.findValue(hirExpression.roots.front())
              : nullptr;
      constructorRootMapping =
          hirExpression.constructorInitializer == 1 && hirRoot != nullptr &&
          hirRoot->source == semanticExpressions.front().roots.front();
    }
  }
  bool exactDropTargets = token != nullptr && mirToken != nullptr &&
                          token->destructor == mirToken->destructor &&
                          token->requiresActiveDropState &&
                          mirToken->requiresActiveDropState;
  const auto inspectDropTargets = [&](const lang::MirBody &body) {
    if (token == nullptr) {
      return;
    }
    for (const lang::MirDropObligation &obligation : body.dropObligations) {
      if (obligation.dropType.type.kind != lang::SemanticType::Class ||
          obligation.dropType.type.classId != token->type.classId) {
        continue;
      }
      exactDropTargets = exactDropTargets &&
                         obligation.dropType.classInstance == token->id &&
                         obligation.dropType.destructor == token->destructor &&
                         obligation.dropType.requiresActiveCleanup;
    }
  };
  if (mirLifetime != nullptr) {
    inspectDropTargets(mirLifetime->body);
  }
  if (mirConsume != nullptr) {
    inspectDropTargets(mirConsume->body);
  }
  if (mirMake != nullptr) {
    inspectDropTargets(mirMake->body);
  }
  bool semanticRootMapping = hirLifetime != nullptr;
  if (hirLifetime != nullptr) {
    std::vector<const lang::SemanticFullExpression *> semanticExpressions;
    for (const lang::SemanticFullExpression &expression :
         frontend.semantics.fullExpressions()) {
      const bool belongsToLifetime =
          expression.statement != nullptr &&
          std::any_of(hirLifetime->body.statements.begin(),
                      hirLifetime->body.statements.end(),
                      [&](const lang::HirStatement &statement) {
                        return statement.source == expression.statement;
                      });
      if (belongsToLifetime) {
        semanticExpressions.push_back(&expression);
      }
    }
    semanticRootMapping =
        semanticExpressions.size() == hirLifetime->body.fullExpressions.size();
    for (std::size_t expressionIndex = 0;
         semanticRootMapping &&
         expressionIndex < hirLifetime->body.fullExpressions.size();
         ++expressionIndex) {
      const lang::HirFullExpression &expression =
          hirLifetime->body.fullExpressions[expressionIndex];
      const lang::SemanticFullExpression &semanticExpression =
          *semanticExpressions[expressionIndex];
      const lang::HirStatement *statement =
          hirLifetime->body.findStatement(expression.statement);
      semanticRootMapping =
          statement != nullptr &&
          statement->source == semanticExpression.statement &&
          semanticExpression.roots.size() == expression.roots.size();
      for (std::size_t rootIndex = 0;
           semanticRootMapping && rootIndex < expression.roots.size();
           ++rootIndex) {
        const lang::HirValue *value =
            hirLifetime->body.findValue(expression.roots[rootIndex]);
        semanticRootMapping =
            value != nullptr &&
            value->source == semanticExpression.roots[rootIndex];
      }
    }
  }
  expect(exactDropTargets && hirLifetime != nullptr &&
             !hirLifetime->body.fullExpressions.empty() &&
             !hirLifetime->body.dropObligations.empty() &&
             constructorRootMapping && semanticRootMapping,
         "HIR should map semantic full-expression roots to concrete IDs while "
         "HIR and MIR retain the exact class destructor and active-drop "
         "descriptor");

  lang::HirStatementId hirLoopStatement = 0;
  if (hirLifetime != nullptr) {
    const auto loop = std::find_if(hirLifetime->body.statements.begin(),
                                   hirLifetime->body.statements.end(),
                                   [&](const lang::HirStatement &item) {
                                     return item.source == lifetimeLoop;
                                   });
    if (loop != hirLifetime->body.statements.end()) {
      hirLoopStatement = loop->id;
    }
  }
  std::size_t loopBoundaries = 0;
  std::vector<std::size_t> lifetimeBoundaryMarkers;
  if (mirLifetime != nullptr) {
    lifetimeBoundaryMarkers.assign(mirLifetime->body.fullExpressions.size(), 0);
    loopBoundaries = static_cast<std::size_t>(
        std::count_if(mirLifetime->body.fullExpressions.begin(),
                      mirLifetime->body.fullExpressions.end(),
                      [&](const lang::MirFullExpression &expression) {
                        return expression.statement == hirLoopStatement;
                      }));
    for (const lang::MirBlock &block : mirLifetime->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.fullExpressionEnd != 0 &&
            instruction.fullExpressionEnd <= lifetimeBoundaryMarkers.size()) {
          ++lifetimeBoundaryMarkers[instruction.fullExpressionEnd - 1];
        }
      }
    }
  }
  const lang::MirConstructorInstance *mirTokenConstructor =
      tokenConstructor == nullptr
          ? nullptr
          : frontend.mir.findConstructorInstance(tokenConstructor->id);
  bool constructorBoundary = false;
  if (mirTokenConstructor != nullptr &&
      mirTokenConstructor->body.fullExpressions.size() == 1) {
    const lang::MirFullExpression &expression =
        mirTokenConstructor->body.fullExpressions.front();
    constructorBoundary =
        expression.constructorInitializer == 1 &&
        std::any_of(mirTokenConstructor->body.blocks.begin(),
                    mirTokenConstructor->body.blocks.end(),
                    [&](const lang::MirBlock &block) {
                      return std::any_of(
                          block.instructions.begin(), block.instructions.end(),
                          [&](const lang::MirInstruction &instruction) {
                            return instruction.fullExpressionEnd ==
                                   expression.id;
                          });
                    });
  }
  expect(hirLoopStatement != 0 && loopBoundaries == 2 &&
             !lifetimeBoundaryMarkers.empty() &&
             std::all_of(lifetimeBoundaryMarkers.begin(),
                         lifetimeBoundaryMarkers.end(),
                         [](std::size_t count) { return count == 1; }) &&
             constructorBoundary,
         "MIR should retain one explicit boundary for each reached HIR "
         "full expression, including loop condition/increment and "
         "constructor-initializer endpoints");

  std::size_t fullExpressionConditionalDrops = 0;
  bool reparent = false;
  bool transfer = false;
  bool directConditionalResultSlot = false;
  if (mirLifetime != nullptr) {
    for (const lang::MirBlock &block : mirLifetime->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.destination && instruction.result &&
            (instruction.kind == lang::MirInstructionKind::Compute ||
             instruction.kind == lang::MirInstructionKind::Move ||
             instruction.kind == lang::MirInstructionKind::Call ||
             instruction.kind == lang::MirInstructionKind::Construct)) {
          const lang::MirPlace *destination =
              mirLifetime->body.findPlace(*instruction.destination);
          directConditionalResultSlot =
              directConditionalResultSlot ||
              (destination != nullptr &&
               destination->root == lang::MirPlaceRootKind::Temporary &&
               std::any_of(
                   instruction.lifecycle.begin(), instruction.lifecycle.end(),
                   [&](const lang::MirLifecycleEvent &event) {
                     const lang::MirDropObligation *obligation =
                         mirLifetime->body.findDropObligation(event.target);
                     return obligation != nullptr &&
                            obligation->place == destination->id &&
                            (event.kind ==
                                 lang::MirLifecycleEventKind::Initialize ||
                             event.kind == lang::MirLifecycleEventKind::Move ||
                             event.kind ==
                                 lang::MirLifecycleEventKind::Reparent);
                   }));
        }
        for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
          if (event.kind == lang::MirLifecycleEventKind::Drop &&
              event.conditional && !event.failureCleanup) {
            const lang::MirDropObligation *obligation =
                mirLifetime->body.findDropObligation(event.source);
            if (obligation != nullptr && token != nullptr &&
                obligation->fullExpression != 0 &&
                obligation->dropType.classInstance == token->id) {
              ++fullExpressionConditionalDrops;
            }
          }
          reparent =
              reparent || event.kind == lang::MirLifecycleEventKind::Reparent;
          transfer = transfer ||
                     event.kind == lang::MirLifecycleEventKind::TransferOut;
        }
      }
    }
  }
  if (mirMake != nullptr) {
    for (const lang::MirBlock &block : mirMake->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        transfer =
            transfer ||
            std::any_of(
                instruction.lifecycle.begin(), instruction.lifecycle.end(),
                [](const lang::MirLifecycleEvent &event) {
                  return event.kind == lang::MirLifecycleEventKind::TransferOut;
                });
      }
    }
  }
  lang::MirDropObligationId persistentFieldSource = 0;
  bool persistentFieldSourceDropped = false;
  if (mirHolder != nullptr) {
    for (const lang::MirBlock &block : mirHolder->fieldInitializers.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
          if (instruction.kind == lang::MirInstructionKind::Initialize &&
              event.kind == lang::MirLifecycleEventKind::Reparent) {
            const lang::MirDropObligation *target =
                mirHolder->fieldInitializers.findDropObligation(event.target);
            const lang::MirDropObligation *obligation =
                mirHolder->fieldInitializers.findDropObligation(event.source);
            if (obligation != nullptr && token != nullptr &&
                target != nullptr &&
                target->kind ==
                    lang::MirDropObligationKind::ConstructionRollback &&
                obligation->dropType.classInstance == token->id) {
              persistentFieldSource = event.source;
            }
          }
          persistentFieldSourceDropped =
              persistentFieldSourceDropped ||
              (persistentFieldSource != 0 &&
               event.kind == lang::MirLifecycleEventKind::Drop &&
               event.source == persistentFieldSource);
        }
      }
    }
  }
  expect(fullExpressionConditionalDrops >= 2 && reparent && transfer &&
             directConditionalResultSlot && persistentFieldSource != 0 &&
             !persistentFieldSourceDropped && mirHolder != nullptr &&
             mirHolder->requiresActiveCleanup &&
             !mirHolder->requiresActiveDropState,
         "MIR should preserve branch-local temporaries through their enclosing "
         "call and drop them conditionally at the full-expression boundary, "
         "directly materialize promoted results in hidden slots, and transfer "
         "field-initializer temporaries into persistent field ownership");

  std::vector<lang::HirBindingId> consumeDropOrder;
  if (mirConsume != nullptr) {
    for (const lang::MirBlock &block : mirConsume->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        for (const lang::MirLifecycleEvent &event : instruction.lifecycle) {
          if (event.kind != lang::MirLifecycleEventKind::Drop) {
            continue;
          }
          const lang::MirDropObligation *obligation =
              mirConsume->body.findDropObligation(event.source);
          if (obligation != nullptr &&
              obligation->kind == lang::MirDropObligationKind::Binding) {
            consumeDropOrder.push_back(obligation->binding);
          }
        }
      }
    }
  }
  expect(hirConsume != nullptr && hirConsume->parameterBindings.size() == 2 &&
             consumeDropOrder.size() >= 2 &&
             consumeDropOrder[0] == hirConsume->parameterBindings[1] &&
             consumeDropOrder[1] == hirConsume->parameterBindings[0],
         "lexical parameter obligations should drop in reverse construction "
         "order on the normal return edge");

  const auto mutableFunction =
      [](lang::MirProgram &program,
         lang::HirFunctionInstanceId id) -> lang::MirFunctionInstance * {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  };
  const auto mutableClass =
      [](lang::MirProgram &program,
         lang::HirClassInstanceId id) -> lang::MirClassInstance * {
    auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
        program.classInstances());
    return id == 0 || id > classes.size() ? nullptr : &classes[id - 1];
  };
  const auto hasVerificationMessage =
      [](const lang::MirVerificationResult &result, std::string_view text) {
        return std::any_of(result.errors.begin(), result.errors.end(),
                           [&](const lang::MirVerificationError &error) {
                             return error.message.find(text) !=
                                    std::string::npos;
                           });
      };

  lang::MirProgram forgedFullExpression = frontend.mir;
  lang::MirFunctionInstance *forgedFullExpressionLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(forgedFullExpression, hirLifetime->id);
  if (forgedFullExpressionLifetime != nullptr &&
      !forgedFullExpressionLifetime->body.fullExpressions.empty()) {
    forgedFullExpressionLifetime->body.fullExpressions.front().hirExpression =
        0;
  }
  const lang::MirVerificationResult forgedFullExpressionResult =
      lang::verifyMirProgram(forgedFullExpression);
  expect(!forgedFullExpressionResult.valid() &&
             hasVerificationMessage(forgedFullExpressionResult,
                                    "full-expression table"),
         "MIR verification should reject a forged HIR full-expression "
         "identity");

  lang::MirProgram missingFullExpressionMarker = frontend.mir;
  lang::MirFunctionInstance *missingMarkerLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(missingFullExpressionMarker, hirLifetime->id);
  bool removedFullExpressionMarker = false;
  if (missingMarkerLifetime != nullptr) {
    for (lang::MirBlock &block : missingMarkerLifetime->body.blocks) {
      const auto marker =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.fullExpressionEnd != 0;
                       });
      if (marker != block.instructions.end()) {
        block.instructions.erase(marker);
        removedFullExpressionMarker = true;
        break;
      }
    }
  }
  const lang::MirVerificationResult missingFullExpressionMarkerResult =
      lang::verifyMirProgram(missingFullExpressionMarker);
  expect(removedFullExpressionMarker &&
             !missingFullExpressionMarkerResult.valid() &&
             hasVerificationMessage(missingFullExpressionMarkerResult,
                                    "exactly one primary boundary marker"),
         "MIR verification should reject a missing full-expression marker");

  lang::MirProgram duplicateFullExpressionMarker = frontend.mir;
  lang::MirFunctionInstance *duplicateMarkerLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(duplicateFullExpressionMarker, hirLifetime->id);
  bool duplicatedFullExpressionMarker = false;
  if (duplicateMarkerLifetime != nullptr) {
    lang::MirInstructionId nextInstruction = 1;
    for (const lang::MirBlock &block : duplicateMarkerLifetime->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        nextInstruction = std::max(nextInstruction, instruction.id + 1);
      }
    }
    for (lang::MirBlock &block : duplicateMarkerLifetime->body.blocks) {
      const auto marker =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.fullExpressionEnd != 0;
                       });
      if (marker != block.instructions.end()) {
        lang::MirInstruction duplicate = *marker;
        duplicate.id = nextInstruction;
        block.instructions.insert(std::next(marker), std::move(duplicate));
        duplicatedFullExpressionMarker = true;
        break;
      }
    }
  }
  const lang::MirVerificationResult duplicateFullExpressionMarkerResult =
      lang::verifyMirProgram(duplicateFullExpressionMarker);
  expect(duplicatedFullExpressionMarker &&
             !duplicateFullExpressionMarkerResult.valid() &&
             hasVerificationMessage(duplicateFullExpressionMarkerResult,
                                    "exactly one primary boundary marker"),
         "MIR verification should reject a duplicate full-expression marker");

  lang::MirProgram wrongFullExpressionDropOrder = frontend.mir;
  lang::MirFunctionInstance *wrongDropOrderLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(wrongFullExpressionDropOrder, hirLifetime->id);
  bool swappedFullExpressionDrops = false;
  if (wrongDropOrderLifetime != nullptr) {
    for (lang::MirBlock &block : wrongDropOrderLifetime->body.blocks) {
      for (std::size_t marker = 2; marker < block.instructions.size();
           ++marker) {
        if (block.instructions[marker].fullExpressionEnd != 0 &&
            block.instructions[marker - 1].kind ==
                lang::MirInstructionKind::Drop &&
            block.instructions[marker - 2].kind ==
                lang::MirInstructionKind::Drop &&
            !block.instructions[marker - 1].lifecycle.empty() &&
            !block.instructions[marker - 2].lifecycle.empty()) {
          const lang::MirDropObligation *last =
              wrongDropOrderLifetime->body.findDropObligation(
                  block.instructions[marker - 1].lifecycle.front().source);
          const lang::MirDropObligation *beforeLast =
              wrongDropOrderLifetime->body.findDropObligation(
                  block.instructions[marker - 2].lifecycle.front().source);
          if (last == nullptr || beforeLast == nullptr ||
              last->fullExpression !=
                  block.instructions[marker].fullExpressionEnd ||
              beforeLast->fullExpression !=
                  block.instructions[marker].fullExpressionEnd) {
            continue;
          }
          std::swap(block.instructions[marker - 1],
                    block.instructions[marker - 2]);
          swappedFullExpressionDrops = true;
          break;
        }
      }
      if (swappedFullExpressionDrops) {
        break;
      }
    }
  }
  const lang::MirVerificationResult wrongFullExpressionDropOrderResult =
      lang::verifyMirProgram(wrongFullExpressionDropOrder);
  expect(swappedFullExpressionDrops &&
             !wrongFullExpressionDropOrderResult.valid() &&
             hasVerificationMessage(wrongFullExpressionDropOrderResult,
                                    "full-expression drops"),
         "MIR verification should enforce reverse full-expression cleanup "
         "order immediately before its marker");

  lang::MirProgram earlyFullExpressionDrop = frontend.mir;
  lang::MirFunctionInstance *earlyDropLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(earlyFullExpressionDrop, hirLifetime->id);
  bool movedDropBeforeRoot = false;
  if (earlyDropLifetime != nullptr) {
    for (lang::MirBlock &block : earlyDropLifetime->body.blocks) {
      for (std::size_t marker = 1; marker < block.instructions.size();
           ++marker) {
        if (block.instructions[marker].fullExpressionEnd == 0 ||
            block.instructions[marker - 1].kind !=
                lang::MirInstructionKind::Drop) {
          continue;
        }
        const lang::MirFullExpression &expression =
            earlyDropLifetime->body
                .fullExpressions[block.instructions[marker].fullExpressionEnd -
                                 1];
        const auto root = std::find_if(
            block.instructions.begin(), block.instructions.begin() + marker - 1,
            [&](const lang::MirInstruction &instruction) {
              return instruction.fullExpressionEnd == 0 &&
                     std::find(expression.roots.begin(), expression.roots.end(),
                               instruction.hirValue) != expression.roots.end();
            });
        if (root == block.instructions.begin() + marker - 1) {
          continue;
        }
        const std::size_t rootIndex =
            static_cast<std::size_t>(root - block.instructions.begin());
        lang::MirInstruction drop = std::move(block.instructions[marker - 1]);
        block.instructions.erase(block.instructions.begin() + marker - 1);
        block.instructions.insert(block.instructions.begin() + rootIndex,
                                  std::move(drop));
        movedDropBeforeRoot = true;
        break;
      }
      if (movedDropBeforeRoot) {
        break;
      }
    }
  }
  const lang::MirVerificationResult earlyFullExpressionDropResult =
      lang::verifyMirProgram(earlyFullExpressionDrop);
  expect(movedDropBeforeRoot && !earlyFullExpressionDropResult.valid() &&
             hasVerificationMessage(earlyFullExpressionDropResult,
                                    "full-expression"),
         "MIR verification should reject full-expression cleanup moved before "
         "its root completion");

  lang::MirProgram missingDrop = frontend.mir;
  lang::MirFunctionInstance *missingDropConsume =
      hirConsume == nullptr ? nullptr
                            : mutableFunction(missingDrop, hirConsume->id);
  if (missingDropConsume != nullptr) {
    for (lang::MirBlock &block : missingDropConsume->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Drop &&
                   !instruction.lifecycle.empty();
          });
      if (found != block.instructions.end()) {
        found->lifecycle.clear();
        break;
      }
    }
  }
  const lang::MirVerificationResult missingDropResult =
      lang::verifyMirProgram(missingDrop);
  expect(!missingDropResult.valid() &&
             hasVerificationMessage(missingDropResult, "cleanup sequence"),
         "MIR verification should reject a forged missing lexical drop");

  lang::MirProgram wrongLexicalDropOrder = frontend.mir;
  lang::MirFunctionInstance *wrongLexicalDropOrderConsume =
      hirConsume == nullptr
          ? nullptr
          : mutableFunction(wrongLexicalDropOrder, hirConsume->id);
  bool swappedLexicalDrops = false;
  if (wrongLexicalDropOrderConsume != nullptr) {
    for (lang::MirBlock &block : wrongLexicalDropOrderConsume->body.blocks) {
      for (std::size_t marker = 2; marker < block.instructions.size();
           ++marker) {
        const std::size_t boundaryId =
            block.instructions[marker].cleanupBoundaryEnd;
        if (boundaryId == 0 ||
            wrongLexicalDropOrderConsume->body.cleanupBoundaries[boundaryId - 1]
                    .kind != lang::MirCleanupBoundaryKind::Normal ||
            block.instructions[marker - 1].kind !=
                lang::MirInstructionKind::Drop ||
            block.instructions[marker - 2].kind !=
                lang::MirInstructionKind::Drop) {
          continue;
        }
        std::swap(block.instructions[marker - 1],
                  block.instructions[marker - 2]);
        swappedLexicalDrops = true;
        break;
      }
    }
  }
  const lang::MirVerificationResult wrongLexicalDropOrderResult =
      lang::verifyMirProgram(wrongLexicalDropOrder);
  expect(swappedLexicalDrops && !wrongLexicalDropOrderResult.valid() &&
             hasVerificationMessage(wrongLexicalDropOrderResult,
                                    "cleanup sequence"),
         "MIR verification should reject adjacent lexical drops swapped out "
         "of reverse construction order");

  lang::MirProgram missingCleanupMember = frontend.mir;
  lang::MirFunctionInstance *missingCleanupMemberConsume =
      hirConsume == nullptr
          ? nullptr
          : mutableFunction(missingCleanupMember, hirConsume->id);
  bool erasedCleanupMember = false;
  if (missingCleanupMemberConsume != nullptr) {
    for (lang::MirCleanupBoundary &boundary :
         missingCleanupMemberConsume->body.cleanupBoundaries) {
      if (boundary.kind == lang::MirCleanupBoundaryKind::Normal &&
          boundary.obligations.size() >= 2) {
        boundary.obligations.erase(boundary.obligations.begin());
        erasedCleanupMember = true;
        break;
      }
    }
  }
  const lang::MirVerificationResult missingCleanupMemberResult =
      lang::verifyMirProgram(missingCleanupMember);
  expect(erasedCleanupMember && !missingCleanupMemberResult.valid() &&
             hasVerificationMessage(missingCleanupMemberResult,
                                    "exactly one lexical cleanup boundary"),
         "MIR verification should reject an omitted binding drop from a "
         "lexical cleanup boundary");

  lang::MirProgram missingCleanupMarker = frontend.mir;
  lang::MirFunctionInstance *missingCleanupMarkerConsume =
      hirConsume == nullptr
          ? nullptr
          : mutableFunction(missingCleanupMarker, hirConsume->id);
  bool removedCleanupMarker = false;
  if (missingCleanupMarkerConsume != nullptr) {
    for (lang::MirBlock &block : missingCleanupMarkerConsume->body.blocks) {
      const auto marker =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.cleanupBoundaryEnd != 0;
                       });
      if (marker != block.instructions.end()) {
        block.instructions.erase(marker);
        removedCleanupMarker = true;
        break;
      }
    }
  }
  const lang::MirVerificationResult missingCleanupMarkerResult =
      lang::verifyMirProgram(missingCleanupMarker);
  expect(removedCleanupMarker && !missingCleanupMarkerResult.valid() &&
             hasVerificationMessage(missingCleanupMarkerResult,
                                    "exactly one boundary marker"),
         "MIR verification should reject a missing lexical cleanup marker");

  lang::MirProgram doubleDrop = frontend.mir;
  lang::MirFunctionInstance *doubleDropConsume =
      hirConsume == nullptr ? nullptr
                            : mutableFunction(doubleDrop, hirConsume->id);
  if (doubleDropConsume != nullptr) {
    lang::MirInstructionId nextInstruction = 1;
    for (const lang::MirBlock &block : doubleDropConsume->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        nextInstruction = std::max(nextInstruction, instruction.id + 1);
      }
    }
    bool inserted = false;
    for (lang::MirBlock &block : doubleDropConsume->body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return instruction.kind == lang::MirInstructionKind::Drop &&
                   !instruction.lifecycle.empty();
          });
      if (found == block.instructions.end()) {
        continue;
      }
      lang::MirInstruction duplicate = *found;
      duplicate.id = nextInstruction;
      block.instructions.insert(std::next(found), std::move(duplicate));
      inserted = true;
      break;
    }
    expect(inserted, "the double-drop verifier mutation needs a drop site");
  }
  const lang::MirVerificationResult doubleDropResult =
      lang::verifyMirProgram(doubleDrop);
  expect(!doubleDropResult.valid() &&
             (hasVerificationMessage(doubleDropResult,
                                     "drop targets an inactive obligation") ||
              hasVerificationMessage(doubleDropResult, "cleanup boundary")),
         "MIR verification should reject a forged double drop");

  lang::MirProgram wrongDestructor = frontend.mir;
  lang::MirFunctionInstance *wrongDestructorLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(wrongDestructor, hirLifetime->id);
  if (wrongDestructorLifetime != nullptr &&
      !wrongDestructorLifetime->body.dropObligations.empty()) {
    wrongDestructorLifetime->body.dropObligations.front()
        .dropType.destructor.reset();
  }
  const lang::MirVerificationResult wrongDestructorResult =
      lang::verifyMirProgram(wrongDestructor);
  // Direct source-class leaves now use the same canonical recipe check as
  // pointee and collection cleanup, before the legacy class-only check.
  expect(!wrongDestructorResult.valid() &&
             hasVerificationMessage(wrongDestructorResult,
                                    "exact pointee cleanup"),
         "MIR verification should reject a forged destructor target");

  lang::MirProgram wrongActiveCleanup = frontend.mir;
  lang::MirFunctionInstance *wrongActiveCleanupLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(wrongActiveCleanup, hirLifetime->id);
  if (wrongActiveCleanupLifetime != nullptr) {
    const auto cleanup =
        std::find_if(wrongActiveCleanupLifetime->body.dropObligations.begin(),
                     wrongActiveCleanupLifetime->body.dropObligations.end(),
                     [](const lang::MirDropObligation &obligation) {
                       return obligation.dropType.classInstance.has_value();
                     });
    if (cleanup != wrongActiveCleanupLifetime->body.dropObligations.end()) {
      cleanup->dropType.requiresActiveCleanup = false;
    }
  }
  const lang::MirVerificationResult wrongActiveCleanupResult =
      lang::verifyMirProgram(wrongActiveCleanup);
  expect(!wrongActiveCleanupResult.valid() &&
             hasVerificationMessage(wrongActiveCleanupResult,
                                    "exact active-cleanup descriptor"),
         "MIR verification should reject forged active-cleanup metadata");

  lang::MirProgram wrongActiveDropState = frontend.mir;
  lang::MirClassInstance *wrongActiveDropToken =
      token == nullptr ? nullptr
                       : mutableClass(wrongActiveDropState, token->id);
  if (wrongActiveDropToken != nullptr) {
    wrongActiveDropToken->requiresActiveDropState = false;
  }
  const lang::MirVerificationResult wrongActiveDropStateResult =
      lang::verifyMirProgram(wrongActiveDropState);
  expect(
      !wrongActiveDropStateResult.valid() &&
          hasVerificationMessage(wrongActiveDropStateResult,
                                 "active-drop state"),
      "MIR verification should reject class active-drop state that disagrees "
      "with its destructor");

  lang::MirProgram wrongStructuralCleanup = frontend.mir;
  lang::MirClassInstance *wrongStructuralHolder =
      holder == nullptr ? nullptr
                        : mutableClass(wrongStructuralCleanup, holder->id);
  if (wrongStructuralHolder != nullptr) {
    wrongStructuralHolder->requiresActiveCleanup = false;
    for (lang::MirFieldDrop &field : wrongStructuralHolder->fieldDropOrder) {
      field.requiresActiveCleanup = false;
    }
  }
  const lang::MirVerificationResult wrongStructuralCleanupResult =
      lang::verifyMirProgram(wrongStructuralCleanup);
  expect(!wrongStructuralCleanupResult.valid() &&
             hasVerificationMessage(wrongStructuralCleanupResult,
                                    "active-cleanup metadata"),
         "MIR verification should derive class active cleanup from exact field "
         "types even when correlated metadata is forged");

  lang::MirProgram missingStructuralField = frontend.mir;
  lang::MirClassInstance *missingStructuralHolder =
      holder == nullptr ? nullptr
                        : mutableClass(missingStructuralField, holder->id);
  if (missingStructuralHolder != nullptr &&
      !missingStructuralHolder->fieldDropOrder.empty()) {
    missingStructuralHolder->fieldDropOrder.pop_back();
  }
  const lang::MirVerificationResult missingStructuralFieldResult =
      lang::verifyMirProgram(missingStructuralField);
  expect(!missingStructuralFieldResult.valid() &&
             hasVerificationMessage(missingStructuralFieldResult,
                                    "exact reverse projection"),
         "MIR verification should reject an omitted cleanup-owning field from "
         "the class drop projection");

  lang::MirProgram wrongBaseMetadata = frontend.mir;
  lang::MirClassInstance *wrongBaseHolder =
      holder == nullptr ? nullptr : mutableClass(wrongBaseMetadata, holder->id);
  if (wrongBaseHolder != nullptr) {
    wrongBaseHolder->bases.push_back(
        {.instance = wrongBaseMetadata.classInstances().size() + 1,
         .type = wrongBaseHolder->type});
  }
  const lang::MirVerificationResult wrongBaseMetadataResult =
      lang::verifyMirProgram(wrongBaseMetadata);
  expect(!wrongBaseMetadataResult.valid() &&
             hasVerificationMessage(wrongBaseMetadataResult, "structural base"),
         "MIR verification should reject forged class base identity metadata");

  lang::MirProgram wrongLifecycleBinding = frontend.mir;
  lang::MirFunctionInstance *wrongLifecycleLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(wrongLifecycleBinding, hirLifetime->id);
  bool changedLifecycleBinding = false;
  if (wrongLifecycleLifetime != nullptr) {
    for (lang::MirBlock &block : wrongLifecycleLifetime->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        const auto initialize = std::find_if(
            instruction.lifecycle.begin(), instruction.lifecycle.end(),
            [](const lang::MirLifecycleEvent &event) {
              return event.kind == lang::MirLifecycleEventKind::Initialize;
            });
        if (initialize == instruction.lifecycle.end()) {
          continue;
        }
        const auto alternate = std::find_if(
            wrongLifecycleLifetime->body.dropObligations.begin(),
            wrongLifecycleLifetime->body.dropObligations.end(),
            [&](const lang::MirDropObligation &obligation) {
              const lang::MirDropObligation *original =
                  wrongLifecycleLifetime->body.findDropObligation(
                      initialize->target);
              return original != nullptr &&
                     obligation.id != initialize->target &&
                     obligation.dropType.type == original->dropType.type;
            });
        if (alternate != wrongLifecycleLifetime->body.dropObligations.end()) {
          initialize->target = alternate->id;
          changedLifecycleBinding = true;
          break;
        }
      }
      if (changedLifecycleBinding) {
        break;
      }
      if (block.terminator.kind != lang::MirTerminatorKind::Invoke ||
          block.terminator.successLifecycle.size() != 1) {
        continue;
      }
      lang::MirLifecycleEvent &initialize =
          block.terminator.successLifecycle.front();
      const lang::MirDropObligation *original =
          wrongLifecycleLifetime->body.findDropObligation(initialize.target);
      const auto alternate = std::find_if(
          wrongLifecycleLifetime->body.dropObligations.begin(),
          wrongLifecycleLifetime->body.dropObligations.end(),
          [&](const lang::MirDropObligation &obligation) {
            return original != nullptr && obligation.id != initialize.target &&
                   obligation.dropType.type == original->dropType.type;
          });
      if (alternate != wrongLifecycleLifetime->body.dropObligations.end()) {
        initialize.target = alternate->id;
        changedLifecycleBinding = true;
        break;
      }
    }
  }
  expect(changedLifecycleBinding,
         "the lifecycle-binding verifier mutation needs an alternate owner");
  const lang::MirVerificationResult wrongLifecycleBindingResult =
      lang::verifyMirProgram(wrongLifecycleBinding);
  expect(!wrongLifecycleBindingResult.valid() &&
             (hasVerificationMessage(wrongLifecycleBindingResult,
                                     "does not match its instruction") ||
              hasVerificationMessage(
                  wrongLifecycleBindingResult,
                  "invoke success edge does not initialize its exact")),
         "MIR verification should bind lifecycle targets to producer outputs");

  lang::MirProgram wrongResultSlot = frontend.mir;
  lang::MirFunctionInstance *wrongResultSlotLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(wrongResultSlot, hirLifetime->id);
  bool removedResultSlot = false;
  if (wrongResultSlotLifetime != nullptr) {
    for (lang::MirBlock &block : wrongResultSlotLifetime->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        const lang::MirPlace *destination =
            instruction.destination ? wrongResultSlotLifetime->body.findPlace(
                                          *instruction.destination)
                                    : nullptr;
        if (destination != nullptr &&
            destination->root == lang::MirPlaceRootKind::Temporary &&
            instruction.result &&
            (instruction.kind == lang::MirInstructionKind::Compute ||
             instruction.kind == lang::MirInstructionKind::Move ||
             instruction.kind == lang::MirInstructionKind::Call ||
             instruction.kind == lang::MirInstructionKind::Construct)) {
          instruction.destination.reset();
          removedResultSlot = true;
          break;
        }
      }
      if (removedResultSlot) {
        break;
      }
    }
  }
  expect(removedResultSlot,
         "the direct-result-slot mutation needs a promoted producer");
  const lang::MirVerificationResult wrongResultSlotResult =
      lang::verifyMirProgram(wrongResultSlot);
  expect(!wrongResultSlotResult.valid() &&
             hasVerificationMessage(wrongResultSlotResult,
                                    "does not match its instruction"),
         "MIR verification should bind promoted conditional storage to its "
         "actual producer destination");

  lang::MirProgram wrongLifecycleType = frontend.mir;
  lang::MirFunctionInstance *wrongLifecycleTypeLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(wrongLifecycleType, hirLifetime->id);
  bool changedLifecycleType = false;
  if (wrongLifecycleTypeLifetime != nullptr) {
    for (lang::MirBlock &block : wrongLifecycleTypeLifetime->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput) {
          continue;
        }
        for (lang::MirLifecycleEvent &event : instruction.lifecycle) {
          if (event.kind != lang::MirLifecycleEventKind::Reparent) {
            continue;
          }
          const lang::MirDropObligation *source =
              wrongLifecycleTypeLifetime->body.findDropObligation(event.source);
          const auto alternate = std::find_if(
              wrongLifecycleTypeLifetime->body.dropObligations.begin(),
              wrongLifecycleTypeLifetime->body.dropObligations.end(),
              [&](const lang::MirDropObligation &obligation) {
                return source != nullptr && obligation.id != event.target &&
                       obligation.dropType.type != source->dropType.type;
              });
          if (alternate !=
              wrongLifecycleTypeLifetime->body.dropObligations.end()) {
            event.target = alternate->id;
            changedLifecycleType = true;
            break;
          }
        }
        if (changedLifecycleType) {
          break;
        }
      }
      if (changedLifecycleType) {
        break;
      }
    }
  }
  expect(changedLifecycleType,
         "the lifecycle-type verifier mutation needs a reparent event");
  const lang::MirVerificationResult wrongLifecycleTypeResult =
      lang::verifyMirProgram(wrongLifecycleType);
  expect(!wrongLifecycleTypeResult.valid() &&
             hasVerificationMessage(wrongLifecycleTypeResult,
                                    "types do not match"),
         "MIR verification should reject cross-type lifecycle reparenting");

  lang::MirProgram wrongFieldTransfer = frontend.mir;
  lang::MirClassInstance *wrongFieldTransferHolder =
      holder == nullptr ? nullptr
                        : mutableClass(wrongFieldTransfer, holder->id);
  bool removedFieldTransfer = false;
  if (wrongFieldTransferHolder != nullptr) {
    for (lang::MirBlock &block :
         wrongFieldTransferHolder->fieldInitializers.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        const auto transfer = std::find_if(
            instruction.lifecycle.begin(), instruction.lifecycle.end(),
            [](const lang::MirLifecycleEvent &event) {
              return event.kind == lang::MirLifecycleEventKind::Reparent;
            });
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            transfer != instruction.lifecycle.end()) {
          instruction.lifecycle.erase(transfer);
          removedFieldTransfer = true;
          break;
        }
      }
      if (removedFieldTransfer) {
        break;
      }
    }
  }
  expect(removedFieldTransfer,
         "the field-transfer verifier mutation needs an Initialize transfer");
  const lang::MirVerificationResult wrongFieldTransferResult =
      lang::verifyMirProgram(wrongFieldTransfer);
  expect(
      !wrongFieldTransferResult.valid() &&
          hasVerificationMessage(wrongFieldTransferResult,
                                 "full-expression boundary retains active"),
      "MIR verification should reject a persistent initializer that does not "
      "transfer its source lifetime");

  lang::MirProgram wrongFieldTransferBinding = frontend.mir;
  lang::MirClassInstance *wrongFieldTransferBindingHolder =
      holder == nullptr ? nullptr
                        : mutableClass(wrongFieldTransferBinding, holder->id);
  bool changedFieldTransferBinding = false;
  if (wrongFieldTransferBindingHolder != nullptr) {
    std::vector<lang::MirLifecycleEvent *> transfers;
    for (lang::MirBlock &block :
         wrongFieldTransferBindingHolder->fieldInitializers.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize) {
          continue;
        }
        for (lang::MirLifecycleEvent &event : instruction.lifecycle) {
          if (event.kind == lang::MirLifecycleEventKind::Reparent) {
            transfers.push_back(&event);
          }
        }
      }
    }
    if (transfers.size() >= 2 && transfers[0]->source != transfers[1]->source) {
      transfers[0]->source = transfers[1]->source;
      changedFieldTransferBinding = true;
    }
  }
  expect(changedFieldTransferBinding,
         "the field-transfer binding mutation needs two owning fields");
  const lang::MirVerificationResult wrongFieldTransferBindingResult =
      lang::verifyMirProgram(wrongFieldTransferBinding);
  expect(!wrongFieldTransferBindingResult.valid() &&
             hasVerificationMessage(wrongFieldTransferBindingResult,
                                    "does not match its instruction"),
         "MIR verification should bind an Initialize field reparent to its "
         "exact source operand");

  lang::MirProgram borrowedTransfer = frontend.mir;
  lang::MirClassInstance *borrowedTransferHolder =
      holder == nullptr ? nullptr : mutableClass(borrowedTransfer, holder->id);
  bool changedTransferOperandToBorrow = false;
  if (borrowedTransferHolder != nullptr) {
    for (lang::MirBlock &block :
         borrowedTransferHolder->fieldInitializers.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        const bool transfers = std::any_of(
            instruction.lifecycle.begin(), instruction.lifecycle.end(),
            [](const lang::MirLifecycleEvent &event) {
              return event.kind == lang::MirLifecycleEventKind::Reparent;
            });
        if (!transfers || instruction.operands.empty() ||
            instruction.operands.front().kind != lang::MirOperandKind::Value) {
          continue;
        }
        const lang::MirLifecycleEvent &event = *std::find_if(
            instruction.lifecycle.begin(), instruction.lifecycle.end(),
            [](const lang::MirLifecycleEvent &candidate) {
              return candidate.kind == lang::MirLifecycleEventKind::Reparent;
            });
        const lang::MirDropObligation *source =
            borrowedTransferHolder->fieldInitializers.findDropObligation(
                event.source);
        if (source == nullptr) {
          continue;
        }
        instruction.operands.front().kind = lang::MirOperandKind::Copy;
        instruction.operands.front().value = 0;
        instruction.operands.front().place = source->place;
        changedTransferOperandToBorrow = true;
        break;
      }
      if (changedTransferOperandToBorrow) {
        break;
      }
    }
  }
  const lang::MirVerificationResult borrowedTransferResult =
      lang::verifyMirProgram(borrowedTransfer);
  expect(changedTransferOperandToBorrow && !borrowedTransferResult.valid() &&
             hasVerificationMessage(borrowedTransferResult,
                                    "does not match its instruction"),
         "MIR verification should reject a field reparent forged onto a Copy "
         "or borrowed operand");

  lang::MirProgram forgedCallParameterRole = frontend.mir;
  lang::MirFunctionInstance *forgedCallParameterLifetime =
      hirLifetime == nullptr
          ? nullptr
          : mutableFunction(forgedCallParameterRole, hirLifetime->id);
  bool changedCallParameterRole = false;
  if (forgedCallParameterLifetime != nullptr) {
    for (lang::MirBlock &block : forgedCallParameterLifetime->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputRole != lang::MirCallInputRole::Argument ||
            instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
            instruction.lifecycle.empty() || instruction.operands.size() != 1) {
          continue;
        }
        for (lang::MirBlock &callBlock :
             forgedCallParameterLifetime->body.blocks) {
          for (lang::MirInstruction &call : callBlock.instructions) {
            if (call.kind != lang::MirInstructionKind::Call ||
                call.callSite != instruction.callSite ||
                instruction.callInputIndex >= call.parameterTypes.size()) {
              continue;
            }
            call.parameterTypes[instruction.callInputIndex] =
                lang::SemanticType::referenceTo(
                    instruction.operands.front().type);
            changedCallParameterRole = true;
            break;
          }
          if (changedCallParameterRole) {
            break;
          }
        }
      }
      if (changedCallParameterRole) {
        break;
      }
    }
  }
  const lang::MirVerificationResult forgedCallParameterRoleResult =
      lang::verifyMirProgram(forgedCallParameterRole);
  expect(changedCallParameterRole && !forgedCallParameterRoleResult.valid() &&
             (hasVerificationMessage(forgedCallParameterRoleResult,
                                     "parameter roles") ||
              hasVerificationMessage(forgedCallParameterRoleResult,
                                     "does not match its instruction") ||
              hasVerificationMessage(forgedCallParameterRoleResult,
                                     "ordered call argument")),
         "MIR verification should bind ownership consumption to the exact "
         "resolved by-value parameter role");

  lang::MirProgram wrongJoin = frontend.mir;
  lang::MirFunctionInstance *wrongJoinLifetime =
      hirLifetime == nullptr ? nullptr
                             : mutableFunction(wrongJoin, hirLifetime->id);
  if (wrongJoinLifetime != nullptr) {
    for (lang::MirBlock &block : wrongJoinLifetime->body.blocks) {
      bool changed = false;
      for (lang::MirInstruction &instruction : block.instructions) {
        for (lang::MirLifecycleEvent &event : instruction.lifecycle) {
          if (event.kind == lang::MirLifecycleEventKind::Drop) {
            event.conditional = true;
            changed = true;
            break;
          }
        }
        if (changed) {
          break;
        }
      }
      if (changed) {
        break;
      }
    }
  }
  const lang::MirVerificationResult wrongJoinResult =
      lang::verifyMirProgram(wrongJoin);
  expect(
      !wrongJoinResult.valid() &&
          hasVerificationMessage(wrongJoinResult, "conditionality disagrees"),
      "MIR verification should reject a forged path-conditional drop");
}

void testProgramArgumentsEntryPoint() {
  const std::string source = R"(
#include <std/string>
#include <std/vector>
using arguments_type = std::vector<std::string>;

int main(mut int argc, mut arguments_type argv) {
  if (argc > 0 and argv.size() == uint64_t(argc) and
      argv[std::size_t(0)].size() > 0) {
    return 0;
  }
  return 1;
}
)";
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "program-arguments.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected program-argument diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  const lang::FunctionDecl *main = findTopLevelFunction(valid.program, "main");
  const lang::FunctionInfo *mainInfo =
      main == nullptr ? nullptr : valid.semantics.findFunction(*main);
  const std::optional<lang::HostedProgramEntryPlan> &semanticHosted =
      valid.semantics.hostedProgramEntryPlan();
  expect(valid.canGenerateCode() && valid.diagnostics.empty() &&
             mainInfo != nullptr &&
             mainInfo->entryKind == lang::ProgramEntryKind::OwnedArguments &&
             mainInfo->entryArgumentAppendFunction != 0 && semanticHosted &&
             semanticHosted->entry == mainInfo->id &&
             semanticHosted->appendFunction ==
                 mainInfo->entryArgumentAppendFunction &&
             semanticHosted->vectorConstructor != 0 &&
             semanticHosted->stringConstructor != 0 &&
             !semanticHosted->validateCount.empty() &&
             !semanticHosted->convertCount.empty(),
         "semantics should accept the resolved owned program-argument entry "
         "contract and retain its exact startup targets and failure origins");

  const lang::HirFunctionInstance *hirMain = nullptr;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source == main) {
      hirMain = &instance;
      break;
    }
  }
  const lang::MirFunctionInstance *mirMain =
      hirMain == nullptr ? nullptr
                         : valid.mir.findFunctionInstance(hirMain->id);
  const lang::HirFunctionInstance *hirAppend =
      hirMain == nullptr || !hirMain->entryArgumentAppendTarget
          ? nullptr
          : valid.hir.findFunctionInstance(*hirMain->entryArgumentAppendTarget);
  const lang::MirFunctionInstance *mirAppend =
      mirMain == nullptr || !mirMain->entryArgumentAppendTarget
          ? nullptr
          : valid.mir.findFunctionInstance(*mirMain->entryArgumentAppendTarget);
  const std::optional<lang::HirHostedProgramEntryPlan> &hirHosted =
      valid.hir.hostedProgramEntryPlan();
  expect(hirMain != nullptr && mirMain != nullptr &&
             hirMain->entryKind == lang::ProgramEntryKind::OwnedArguments &&
             mirMain->entryKind == lang::ProgramEntryKind::OwnedArguments &&
             hirAppend != nullptr && mirAppend != nullptr &&
             hirAppend->declaration == mainInfo->entryArgumentAppendFunction &&
             mirMain->entryArgumentAppendTarget ==
                 hirMain->entryArgumentAppendTarget &&
             mirAppend->returnType == lang::SemanticType::Void &&
             mirAppend->parameterTypes.size() == 1 && hirHosted &&
             hirHosted->entry == hirMain->id &&
             hirHosted->appendFunction == hirAppend->id &&
             hirHosted->vectorConstructor != 0 &&
             hirHosted->stringConstructor != 0 &&
             hirHosted->semanticEntry == semanticHosted->entry &&
             hirHosted->semanticAppendFunction ==
                 semanticHosted->appendFunction &&
             lang::verifyHirProgramPlans(valid.semantics, valid.hir).valid() &&
             lang::verifyMirProgram(valid.mir).valid(),
         "HIR and MIR should retain the concrete checked owned-argument "
         "append target and entry adapter contract");

  lang::HirProgram corruptedHosted = valid.hir;
  auto &corruptedHostedPlan =
      const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
          corruptedHosted.hostedProgramEntryPlan());
  if (corruptedHostedPlan) {
    corruptedHostedPlan->semanticAppendFunction = 0;
  }
  expect(!lang::verifyHirProgramPlans(valid.semantics, corruptedHosted).valid(),
         "HIR plan verification should reject hosted-entry provenance "
         "corrupted after semantic lowering");

  lang::HirProgram wrongHostedAppend = valid.hir;
  auto &wrongHostedAppendPlan =
      const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
          wrongHostedAppend.hostedProgramEntryPlan());
  if (wrongHostedAppendPlan) {
    wrongHostedAppendPlan->appendFunction = wrongHostedAppendPlan->entry;
  }
  lang::HirProgram wrongHostedVectorConstructor = valid.hir;
  auto &wrongHostedVectorPlan =
      const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
          wrongHostedVectorConstructor.hostedProgramEntryPlan());
  if (wrongHostedVectorPlan) {
    wrongHostedVectorPlan->vectorConstructor =
        wrongHostedVectorPlan->stringConstructor;
  }
  lang::HirProgram wrongHostedStringConstructor = valid.hir;
  auto &wrongHostedStringPlan =
      const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
          wrongHostedStringConstructor.hostedProgramEntryPlan());
  if (wrongHostedStringPlan) {
    wrongHostedStringPlan->stringConstructor =
        wrongHostedStringPlan->vectorConstructor;
  }
  expect(!lang::verifyHirProgramPlans(valid.semantics, wrongHostedAppend)
                 .valid() &&
             !lang::verifyHirProgramPlans(valid.semantics,
                                          wrongHostedVectorConstructor)
                  .valid() &&
             !lang::verifyHirProgramPlans(valid.semantics,
                                          wrongHostedStringConstructor)
                  .valid(),
         "hosted HIR verification should reject substituted append, vector "
         "constructor, and string constructor instances");

  lang::HirProgram wrongHostedOwner = valid.hir;
  auto &wrongHostedOwnerPlan =
      const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
          wrongHostedOwner.hostedProgramEntryPlan());
  auto &wrongHostedFunctions =
      const_cast<std::vector<lang::HirFunctionInstance> &>(
          wrongHostedOwner.functionInstances());
  if (wrongHostedOwnerPlan && wrongHostedOwnerPlan->appendFunction != 0 &&
      wrongHostedOwnerPlan->appendFunction <= wrongHostedFunctions.size()) {
    wrongHostedFunctions[wrongHostedOwnerPlan->appendFunction - 1]
        .owner.reset();
  }
  expect(
      !lang::verifyHirProgramPlans(valid.semantics, wrongHostedOwner).valid(),
      "hosted HIR verification should reject an append specialization "
      "whose exact concrete owner identity is lost");

  const auto mutateHostedFailure = [](lang::HirProgram &program, auto mutate) {
    auto &plan = const_cast<std::optional<lang::HirHostedProgramEntryPlan> &>(
        program.hostedProgramEntryPlan());
    if (plan && !plan->validateCount.localOrigins.empty()) {
      mutate(plan->validateCount.localOrigins.front());
    }
  };
  lang::HirProgram wrongHostedFailureCode = valid.hir;
  mutateHostedFailure(wrongHostedFailureCode,
                      [](lang::DefinedFailureOrigin &origin) {
                        if (!origin.outcomes.empty()) {
                          origin.outcomes.front().code =
                              lang::DefinedFailureCode::AllocationFailure;
                        }
                      });
  lang::HirProgram wrongHostedFailureDetail = valid.hir;
  mutateHostedFailure(wrongHostedFailureDetail,
                      [](lang::DefinedFailureOrigin &origin) {
                        if (!origin.outcomes.empty()) {
                          origin.outcomes.front().detail =
                              lang::DefinedFailureDetail::HostedArgumentCount;
                        }
                      });
  lang::HirProgram wrongHostedFailureSite = valid.hir;
  mutateHostedFailure(
      wrongHostedFailureSite,
      [](lang::DefinedFailureOrigin &origin) { ++origin.start; });
  expect(
      !lang::verifyHirProgramPlans(valid.semantics, wrongHostedFailureCode)
              .valid() &&
          !lang::verifyHirProgramPlans(valid.semantics,
                                       wrongHostedFailureDetail)
               .valid() &&
          !lang::verifyHirProgramPlans(valid.semantics, wrongHostedFailureSite)
               .valid(),
      "hosted HIR verification should exact-check adapter failure code, "
      "detail, and main-anchor site");

  const lang::FrontendResult noArguments = lang::Frontend().analyze(
      "no-argument-entry-plan.gti", "int main() { return 0; }\n");
  const auto &semanticNoArguments =
      noArguments.semantics.hostedProgramEntryPlan();
  const auto &hirNoArguments = noArguments.hir.hostedProgramEntryPlan();
  expect(noArguments.canGenerateCode() && semanticNoArguments &&
             semanticNoArguments->kind == lang::ProgramEntryKind::NoArguments &&
             semanticNoArguments->appendFunction == 0 &&
             semanticNoArguments->vectorConstructor == 0 &&
             semanticNoArguments->stringConstructor == 0 &&
             semanticNoArguments->validateCount.empty() &&
             semanticNoArguments->convertCount.empty() && hirNoArguments &&
             hirNoArguments->entry != 0 &&
             hirNoArguments->appendFunction == 0 &&
             hirNoArguments->vectorConstructor == 0 &&
             hirNoArguments->stringConstructor == 0 &&
             lang::verifyHirProgramPlans(noArguments.semantics, noArguments.hir)
                 .valid(),
         "no-argument hosted startup should retain one exact entry and no "
         "owned-argument targets or failure origins");

  lang::MirProgram corruptedAdapter = valid.mir;
  auto &corruptedFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          corruptedAdapter.functionInstances());
  if (mirMain != nullptr) {
    corruptedFunctions[mirMain->id - 1].entryArgumentAppendTarget = mirMain->id;
  }
  const lang::MirVerificationResult corruptedResult =
      lang::verifyMirProgram(corruptedAdapter);
  expect(mirMain != nullptr && !corruptedResult.valid() &&
             std::any_of(
                 corruptedResult.errors.begin(), corruptedResult.errors.end(),
                 [](const lang::MirVerificationError &error) {
                   return error.message.find("owned-argument entry point") !=
                          std::string::npos;
                 }),
         "MIR verification should reject a startup append target corrupted by "
         "a later pass");

  const std::string generated = gti_test::emitCppText(valid);
  const lang::FunctionInfo *append =
      mainInfo == nullptr
          ? nullptr
          : valid.semantics.findFunction(mainInfo->entryArgumentAppendFunction);
  const std::unordered_set<std::string> appendNames =
      concreteBackendSymbols(generated, "__gti_member_", "push_back");
  // Public member aliases can require the native template carrier instead of
  // a concrete owner spelling. Both representations must invoke the exact
  // selected GTI append declaration; neither may rely on native overload
  // lookup.
  const std::string appendName =
      appendNames.size() == 1 ? *appendNames.begin()
      : appendNames.empty() && append != nullptr
          ? "__gti_fn_" + std::to_string(append->id) + "_push_back"
          : std::string{};
  expect(generated.find(
             "int main(int __gti_native_argc, char **__gti_native_argv)") !=
                 std::string::npos &&
             generated.find("std::string_view("
                            "__gti_native_argv[__gti_index])") !=
                 std::string::npos &&
             !appendName.empty() && append != nullptr &&
             generated.find("__gti_arguments." + appendName +
                            "__gti_mir_failure(") != std::string::npos &&
             generated.find("std::move(__gti_arguments)") != std::string::npos,
         "the C++ backend should isolate native argc/argv in an owned GTI "
         "entry adapter");

  const std::string oneParameterSource =
      "int main(int value) { return value; }\n";
  const lang::FrontendResult oneParameter =
      lang::Frontend().analyze("main-one-parameter.gti", oneParameterSource);
  const lang::Diagnostic *oneParameterDiagnostic =
      findDiagnosticByCode(oneParameter.diagnostics, "GTI-S2032");
  const std::size_t parameterOffset = oneParameterSource.find("int value");
  expect(!oneParameter.canGenerateCode() && oneParameterDiagnostic != nullptr &&
             oneParameterDiagnostic->phase ==
                 lang::DiagnosticPhase::Semantics &&
             oneParameterDiagnostic->message.find("std::vector<std::string>") !=
                 std::string::npos &&
             oneParameterDiagnostic->primary.start == parameterOffset &&
             oneParameterDiagnostic->primary.end == parameterOffset + 3 &&
             !oneParameterDiagnostic->hints.empty() &&
             oneParameterDiagnostic->fixes.empty(),
         "GTI-S2032 should diagnose an invalid main parameter list at the "
         "first parameter type with actionable owned-argument guidance");

  const lang::FrontendResult wrongArguments = lang::Frontend().analyze(
      "main-wrong-arguments.gti",
      "#include <std/vector>\n"
      "int main(int count, std::vector<int> values) { return count; }\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::FrontendResult rawArguments = lang::Frontend().analyze(
      "main-raw-arguments.gti",
      "int main(int count, const char* values) { return count; }\n");
  const lang::FrontendResult lookalikeArguments =
      lang::Frontend().analyze("main-lookalike-arguments.gti", R"(
namespace std {
class string {};
class vector<T> {
public:
  void push_back(T value) {}
};
}
int main(int count, std::vector<std::string> values) { return count; }
)");
  expect(!wrongArguments.canGenerateCode() && !rawArguments.canGenerateCode() &&
             !lookalikeArguments.canGenerateCode() &&
             hasDiagnosticCode(wrongArguments.diagnostics, "GTI-S2032") &&
             hasDiagnosticCode(rawArguments.diagnostics, "GTI-S2032") &&
             hasDiagnosticCode(lookalikeArguments.diagnostics, "GTI-S2032"),
         "main should reject arbitrary containers, source lookalikes, and raw "
         "native argument pointers without widening pointer-to-pointer "
         "semantics or granting spelling-based behavior");
}

void testProgramInitializationPlans() {
  lang::FrontendOptions hirOptions;
  hirOptions.stopAfter = lang::FrontendPhase::Hir;
  const lang::FrontendResult ordered =
      lang::Frontend(hirOptions).analyze("program-initialization-plan.gti", R"(
constexpr int32_t negative = -7;
constexpr bool enabled = true;
constexpr char marker = 'G';
constexpr double ratio = 1.5;
mut int32_t zeroed;
mut int32_t negative_copy = negative;
mut bool enabled_copy = enabled;
mut char marker_copy = marker;
mut double ratio_copy = ratio;

class Registry {
public:
  static mut int32_t value = negative_copy;
};

int main() { return Registry::value + zeroed + negative_copy + 7; }
)");
  if (!ordered.semanticValid || !ordered.hirValid) {
    for (const lang::Diagnostic &diagnostic : ordered.diagnostics) {
      std::cerr << "Unexpected program-initialization diagnostic: "
                << diagnostic.message << '\n';
    }
  }
  const lang::ProgramInitializationPlan &semanticPlan =
      ordered.semantics.programInitializationPlan();
  const lang::HirProgramInitializationPlan &hirPlan =
      ordered.hir.programInitializationPlan();
  const std::vector<std::string> expectedNames{
      "negative",      "enabled",      "marker",      "ratio",      "zeroed",
      "negative_copy", "enabled_copy", "marker_copy", "ratio_copy", "value"};
  std::vector<std::string> actualNames;
  for (const lang::ProgramInitializationStep &step : semanticPlan.steps) {
    actualNames.push_back(step.declaration == nullptr
                              ? std::string{}
                              : step.declaration->name().lexeme);
  }
  const bool dataRoles =
      semanticPlan.steps.size() == expectedNames.size() &&
      std::all_of(semanticPlan.steps.begin(), semanticPlan.steps.begin() + 5,
                  [](const lang::ProgramInitializationStep &step) {
                    return step.role ==
                           lang::ProgramInitializationStepRole::DataOnly;
                  }) &&
      std::all_of(semanticPlan.steps.begin() + 5, semanticPlan.steps.end(),
                  [](const lang::ProgramInitializationStep &step) {
                    return step.role ==
                           lang::ProgramInitializationStepRole::Initializer;
                  });
  const std::size_t substitutions = static_cast<std::size_t>(std::count_if(
      ordered.hir.module().values.begin(), ordered.hir.module().values.end(),
      [](const lang::HirValue &value) {
        return value.programConstantSubstitution && value.constant.has_value();
      }));
  expect(
      ordered.semanticValid && ordered.hirValid &&
          ordered.diagnostics.empty() && actualNames == expectedNames &&
          dataRoles &&
          semanticPlan.steps.back().kind ==
              lang::ProgramStorageKind::StaticField &&
          hirPlan.steps.size() == semanticPlan.steps.size() &&
          ordered.hir.module().roots.size() == 5 && substitutions >= 4 &&
          lang::verifyHirProgramPlans(ordered.semantics, ordered.hir).valid(),
      "semantics and HIR should retain one deterministic global/static "
      "plan, explicit data-only roles, and representable constant "
      "substitutions");

  const lang::FrontendResult deferredConstant =
      lang::Frontend().analyze("deferred-program-constant.gti", R"(
class DeferredGlobals {
public:
  static mut int32_t first = DeferredGlobals::later;
  static constexpr int32_t later = -7;
};
int main() { return DeferredGlobals::first + DeferredGlobals::later + 7; }
)");
  const lang::ProgramInitializationStep *deferredFirst =
      deferredConstant.semantics.programInitializationPlan().steps.empty()
          ? nullptr
          : &deferredConstant.semantics.programInitializationPlan().steps[0];
  const lang::Expr *deferredUse =
      deferredFirst == nullptr || deferredFirst->declaration == nullptr
          ? nullptr
          : deferredFirst->declaration->initializer().get();
  const lang::HirValue *deferredHirValue = nullptr;
  if (deferredUse != nullptr) {
    const std::vector<lang::HirValueId> &ids =
        deferredConstant.hir.valueIdsForSource(*deferredUse);
    if (ids.size() == 1) {
      deferredHirValue = deferredConstant.hir.module().findValue(ids.front());
    }
  }
  std::size_t deferredMarkers = 0;
  bool deferredLoad = false;
  if (deferredHirValue != nullptr) {
    for (const lang::MirBlock &block : deferredConstant.mir.module().blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.hirValue != deferredHirValue->id) {
          continue;
        }
        deferredMarkers += instruction.programConstantSubstitution ? 1U : 0U;
        deferredLoad =
            deferredLoad || instruction.kind == lang::MirInstructionKind::Load;
      }
    }
  }
  expect(
      deferredConstant.canGenerateCode() && deferredFirst != nullptr &&
          deferredUse != nullptr &&
          deferredConstant.semantics.isProgramConstantSubstitution(
              *deferredUse) &&
          deferredHirValue != nullptr &&
          deferredHirValue->programConstantSubstitution &&
          deferredConstant.mir.module().programConstantSubstitutions.size() ==
              1 &&
          deferredMarkers == 1 && !deferredLoad &&
          lang::verifyHirProgramPlans(deferredConstant.semantics,
                                      deferredConstant.hir)
              .valid() &&
          lang::verifyMirProgram(deferredConstant.mir).valid(),
      "a later constexpr value should be deferred, substituted as exact "
      "HIR/MIR data, and never loaded from its later storage step");

  const lang::FrontendResult substitutionLocality =
      lang::Frontend().analyze("program-constant-boundary-locality.gti", R"(
class SubstitutionLocality {
public:
  static int32_t inspect(int32_t& value) { return value; }
  static mut int32_t first = SubstitutionLocality::later;
  static constexpr int32_t later = -7;
  static mut int32_t forced =
      SubstitutionLocality::inspect((SubstitutionLocality::later));
};
int main() { return SubstitutionLocality::first +
                    SubstitutionLocality::forced + 7; }
)");
  const lang::VariableDecl *localityFirst = nullptr;
  const lang::VariableDecl *localityForced = nullptr;
  for (const lang::ProgramInitializationStep &step :
       substitutionLocality.semantics.programInitializationPlan().steps) {
    if (step.declaration != nullptr &&
        step.declaration->name().lexeme == "first") {
      localityFirst = step.declaration;
    } else if (step.declaration != nullptr &&
               step.declaration->name().lexeme == "forced") {
      localityForced = step.declaration;
    }
  }
  const auto *forcedCall = localityForced == nullptr
                               ? nullptr
                               : dynamic_cast<const lang::Call *>(
                                     localityForced->initializer().get());
  const lang::Expr *forcedBoundary =
      forcedCall == nullptr || forcedCall->arguments().empty()
          ? nullptr
          : forcedCall->arguments().front().get();
  const auto *forcedGrouping =
      dynamic_cast<const lang::Grouping *>(forcedBoundary);
  const lang::Expr *forcedInner =
      forcedGrouping == nullptr ? nullptr : forcedGrouping->expression().get();
  const lang::Expr *firstUse =
      localityFirst == nullptr ? nullptr : localityFirst->initializer().get();
  const auto exactModuleHirValue =
      [&](const lang::Expr *source) -> const lang::HirValue * {
    if (source == nullptr) {
      return nullptr;
    }
    const std::vector<lang::HirValueId> &ids =
        substitutionLocality.hir.valueIdsForSource(*source);
    return ids.size() == 1
               ? substitutionLocality.hir.module().findValue(ids.front())
               : nullptr;
  };
  const lang::HirValue *localityFirstHir = exactModuleHirValue(firstUse);
  const lang::HirValue *localityForcedBoundaryHir =
      exactModuleHirValue(forcedBoundary);
  const lang::HirValue *localityForcedInnerHir =
      exactModuleHirValue(forcedInner);
  bool forcedBoundaryAccess = false;
  if (localityForcedBoundaryHir != nullptr) {
    for (const lang::MirBlock &block :
         substitutionLocality.mir.module().blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        for (const lang::MirOperand &operand : instruction.operands) {
          const lang::MirPlace *place =
              (operand.kind == lang::MirOperandKind::BorrowRead ||
               operand.kind == lang::MirOperandKind::BorrowWrite)
                  ? substitutionLocality.mir.module().findPlace(operand.place)
                  : nullptr;
          forcedBoundaryAccess =
              forcedBoundaryAccess ||
              (place != nullptr &&
               place->sourceValue == localityForcedBoundaryHir->id);
        }
      }
    }
  }
  expect(substitutionLocality.canGenerateCode() && firstUse != nullptr &&
             forcedBoundary != nullptr && forcedInner != nullptr &&
             substitutionLocality.semantics.isProgramConstantSubstitution(
                 *firstUse) &&
             !substitutionLocality.semantics.isProgramConstantSubstitution(
                 *forcedBoundary) &&
             !substitutionLocality.semantics.isProgramConstantSubstitution(
                 *forcedInner) &&
             localityFirstHir != nullptr &&
             localityFirstHir->programConstantSubstitution &&
             localityForcedBoundaryHir != nullptr &&
             !localityForcedBoundaryHir->programConstantSubstitution &&
             localityForcedInnerHir != nullptr &&
             !localityForcedInnerHir->programConstantSubstitution &&
             forcedBoundaryAccess &&
             lang::verifyMirProgram(substitutionLocality.mir).valid(),
         "forcing one grouped reference boundary should retain storage access "
         "only "
         "at that exact boundary without clearing an earlier substitution "
         "for the same program-storage symbol");

  const lang::HirProgramInitializationStep *zeroed = nullptr;
  const lang::HirProgramInitializationStep *staticValue = nullptr;
  for (const lang::HirProgramInitializationStep &step : hirPlan.steps) {
    if (step.source != nullptr && step.source->name().lexeme == "zeroed") {
      zeroed = &step;
    } else if (step.source != nullptr &&
               step.source->name().lexeme == "value") {
      staticValue = &step;
    }
  }
  const lang::HirClassInstance *staticOwner =
      staticValue == nullptr || staticValue->ownerClass == 0
          ? nullptr
          : &ordered.hir.classInstances()[staticValue->ownerClass - 1];
  expect(zeroed != nullptr &&
             zeroed->role == lang::ProgramInitializationStepRole::DataOnly &&
             !zeroed->initializer && zeroed->statement == 0 &&
             staticValue != nullptr && staticValue->statement != 0 &&
             staticOwner != nullptr &&
             staticOwner->staticFieldInitializers.bindings.empty() &&
             staticOwner->staticFieldInitializers.values.empty() &&
             staticOwner->staticFieldInitializers.statements.empty() &&
             staticOwner->staticFieldInitializers.roots.empty() &&
             staticOwner->staticFieldInitializers.placeDomain.snapshot ==
                 ordered.hir.module().placeDomain.snapshot &&
             staticOwner->staticFieldInitializers.placeDomain.body != 0 &&
             staticOwner->staticFieldInitializers.placeDomain !=
                 ordered.hir.module().placeDomain,
         "zero-initialized mutable storage should be sealed data and a "
         "planned class static should have one module root plus a distinct "
         "canonical empty class-body domain");

  lang::HirProgram corrupted = ordered.hir;
  auto &corruptedPlan = const_cast<lang::HirProgramInitializationPlan &>(
      corrupted.programInitializationPlan());
  if (corruptedPlan.steps.size() > 5) {
    corruptedPlan.steps[5].role = lang::ProgramInitializationStepRole::DataOnly;
  }
  expect(!lang::verifyHirProgramPlans(ordered.semantics, corrupted).valid(),
         "HIR plan verification should reject a role/root mutation after "
         "semantic lowering");

  lang::HirProgram missingInitializer = ordered.hir;
  auto &missingInitializerPlan =
      const_cast<lang::HirProgramInitializationPlan &>(
          missingInitializer.programInitializationPlan());
  if (missingInitializerPlan.steps.size() > 5) {
    const lang::HirStatementId statement =
        missingInitializerPlan.steps[5].statement;
    missingInitializerPlan.steps[5].initializer.reset();
    auto &module = const_cast<lang::HirBody &>(missingInitializer.module());
    const auto root =
        std::find_if(module.statements.begin(), module.statements.end(),
                     [statement](const lang::HirStatement &candidate) {
                       return candidate.id == statement;
                     });
    if (root != module.statements.end()) {
      root->value.reset();
    }
  }
  expect(!lang::verifyHirProgramPlans(ordered.semantics, missingInitializer)
              .valid(),
         "HIR plan verification should reject an executable initializer "
         "whose step and statement both lose the source value");

  lang::HirProgram extraModuleValue = ordered.hir;
  auto &extraModuleBody =
      const_cast<lang::HirBody &>(extraModuleValue.module());
  if (!extraModuleBody.values.empty()) {
    lang::HirValue extra = extraModuleBody.values.back();
    extra.id = static_cast<lang::HirValueId>(
        1 + std::max_element(
                extraModuleBody.values.begin(), extraModuleBody.values.end(),
                [](const lang::HirValue &left, const lang::HirValue &right) {
                  return left.id < right.id;
                })
                ->id);
    extraModuleBody.values.push_back(std::move(extra));
  }
  expect(
      !lang::verifyHirProgramPlans(ordered.semantics, extraModuleValue).valid(),
      "HIR plan verification should reject an unrooted extra module value");

  lang::HirProgram wrongBindingInfo = ordered.hir;
  auto &wrongBindingBody =
      const_cast<lang::HirBody &>(wrongBindingInfo.module());
  if (!wrongBindingBody.bindings.empty()) {
    wrongBindingBody.bindings.front().info.access = lang::AccessMode::Mutable;
  }
  expect(
      !lang::verifyHirProgramPlans(ordered.semantics, wrongBindingInfo).valid(),
      "HIR plan verification should reject ID-preserving binding metadata "
      "changes");

  const auto mutateFirstSubstitution = [&](lang::HirProgram &program,
                                           auto mutate) {
    auto &body = const_cast<lang::HirBody &>(program.module());
    const auto value = std::find_if(body.values.begin(), body.values.end(),
                                    [](const lang::HirValue &item) {
                                      return item.programConstantSubstitution;
                                    });
    if (value != body.values.end()) {
      mutate(*value);
    }
  };
  lang::HirProgram wrongSubstitutionMarker = ordered.hir;
  mutateFirstSubstitution(wrongSubstitutionMarker, [](lang::HirValue &value) {
    value.programConstantSubstitution = false;
  });
  lang::HirProgram wrongSubstitutionConstant = ordered.hir;
  mutateFirstSubstitution(wrongSubstitutionConstant, [](lang::HirValue &value) {
    value.constant = lang::ConstantInteger{
        .magnitude = 99,
        .domain = lang::CheckedIntegerDomain{.width = 32, .signedValue = true}};
  });
  lang::HirProgram wrongSubstitutionInfo = ordered.hir;
  mutateFirstSubstitution(wrongSubstitutionInfo, [](lang::HirValue &value) {
    value.info.type = lang::SemanticType::UInt64;
  });
  expect(
      !lang::verifyHirProgramPlans(ordered.semantics, wrongSubstitutionMarker)
              .valid() &&
          !lang::verifyHirProgramPlans(ordered.semantics,
                                       wrongSubstitutionConstant)
               .valid() &&
          !lang::verifyHirProgramPlans(ordered.semantics, wrongSubstitutionInfo)
               .valid(),
      "HIR plan verification should exact-check each reachable constant "
      "substitution marker, value, and ExpressionInfo");

  lang::HirProgram wrongStaticMetadata = ordered.hir;
  auto &wrongStaticClasses = const_cast<std::vector<lang::HirClassInstance> &>(
      wrongStaticMetadata.classInstances());
  if (staticValue != nullptr && staticValue->ownerClass != 0 &&
      staticValue->ownerClass <= wrongStaticClasses.size() &&
      !wrongStaticClasses[staticValue->ownerClass - 1].staticFields.empty()) {
    wrongStaticClasses[staticValue->ownerClass - 1]
        .staticFields.front()
        .requiresActiveCleanup = true;
  }
  expect(!lang::verifyHirProgramPlans(ordered.semantics, wrongStaticMetadata)
              .valid(),
         "HIR plan verification should reject mutated static-field lifecycle "
         "metadata while retaining the same plan IDs");

  const std::string directSource = R"(
class Globals {
public:
  static mut int32_t first = Globals::later;
  static mut int32_t later = 1;
  static mut int32_t self = Globals::self;
};
int main() { return Globals::first + Globals::later + Globals::self; }
)";
  const lang::FrontendResult direct =
      lang::Frontend().analyze("later-program-storage.gti", directSource);
  const lang::Diagnostic *directDiagnostic =
      findDiagnosticByCode(direct.diagnostics, "GTI-S2068");
  const std::size_t directAccess =
      directSource.find("Globals::later") + std::string("Globals::").size();
  const std::size_t directTarget = directSource.find("later = 1");
  const std::size_t directInitializer = directSource.find("first =");
  expect(!direct.semanticValid &&
             countDiagnosticCode(direct.diagnostics, "GTI-S2068") == 2 &&
             directDiagnostic != nullptr &&
             directDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             directDiagnostic->severity == lang::DiagnosticSeverity::Error &&
             directDiagnostic->primary.source ==
                 std::filesystem::weakly_canonical(
                     std::filesystem::absolute("later-program-storage.gti"))
                     .string() &&
             directDiagnostic->primary.start == directAccess &&
             directDiagnostic->primary.end == directAccess + 5 &&
             directDiagnostic->primary.line == 4 &&
             directDiagnostic->message ==
                 "Initializer for 'Globals::first' may access program-wide "
                 "storage 'Globals::later' before its initialization step "
                 "completes." &&
             directDiagnostic->related.size() == 2 &&
             directDiagnostic->related[0].span.start == directTarget &&
             directDiagnostic->related[0].span.end == directTarget + 5 &&
             directDiagnostic->related[0].span.line == 5 &&
             directDiagnostic->related[0].message ==
                 "This storage is initialized at its own later program step." &&
             directDiagnostic->related[1].span.start == directInitializer &&
             directDiagnostic->related[1].span.end == directInitializer + 5 &&
             directDiagnostic->related[1].span.line == 4 &&
             directDiagnostic->related[1].message ==
                 "The rejected initializer is declared here." &&
             directDiagnostic->hints ==
                 std::vector<std::string>{
                     "Move the dependency earlier in source-unit/declaration "
                     "order or remove the access from this initializer's "
                     "closed GTI call graph."} &&
             directDiagnostic->fixes.empty(),
         "GTI-S2068 should reject direct later and self storage access with "
         "an exact access span, declaration relations, ordering guidance, "
         "and no unsafe fix-it");

  const lang::FrontendResult zeroBeforeStep =
      lang::Frontend().analyze("later-zero-storage.gti", R"(
class Globals {
public:
  static mut int32_t first = Globals::zeroed;
  static mut int32_t zeroed;
};
int main() { return Globals::first; }
)");
  expect(!zeroBeforeStep.semanticValid &&
             countDiagnosticCode(zeroBeforeStep.diagnostics, "GTI-S2068") == 1,
         "a later data-only zero-initialization step must not create an "
         "early-value loophole");

  const lang::FrontendResult transitive =
      lang::Frontend().analyze("transitive-later-program-storage.gti", R"(
class Globals {
public:
  static int32_t read_later() { return Globals::later; }
  static mut int32_t first = Globals::read_later();
  static mut int32_t later = 1;
};
int main() { return Globals::first; }
)");
  expect(!transitive.semanticValid &&
             countDiagnosticCode(transitive.diagnostics, "GTI-S2068") == 1 &&
             hasRelatedDiagnostic(transitive.diagnostics,
                                  "transitive storage access"),
         "later-storage proof should close over exact non-generic GTI calls");

  const lang::FrontendResult referenceBoundary =
      lang::Frontend().analyze("reference-later-program-storage.gti", R"(
class Globals {
public:
  static int32_t inspect(int32_t& value) { return value; }
  static mut int32_t first = Globals::inspect(Globals::later);
  static constexpr int32_t later = -3;
};
int main() { return Globals::first; }
)");
  expect(!referenceBoundary.semanticValid &&
             countDiagnosticCode(referenceBoundary.diagnostics, "GTI-S2068") ==
                 1,
         "a bare constexpr passed through an exact reference parameter must "
         "remain a storage access rather than constant substitution");

  const lang::FrontendResult referenceReturn =
      lang::Frontend().analyze("reference-return-later-storage.gti", R"(
class ReferenceReturnGlobals {
public:
  static int32_t& borrow_later() {
    return ReferenceReturnGlobals::later;
  }
  static mut int32_t first = ReferenceReturnGlobals::borrow_later();
  static constexpr int32_t later = -7;
};
int main() { return ReferenceReturnGlobals::first; }
)");
  expect(!referenceReturn.semanticValid &&
             countDiagnosticCode(referenceReturn.diagnostics, "GTI-S2068") ==
                 1 &&
             hasRelatedDiagnostic(referenceReturn.diagnostics,
                                  "transitive storage access"),
         "a reference return must preserve the later constexpr as a storage "
         "access in the transitive initializer proof");

  const lang::FrontendResult bodylessGti =
      lang::Frontend().analyze("bodyless-gti-later-storage.gti", R"(
int32_t opaque();
class BodylessGlobals {
public:
  static mut int32_t first = opaque();
  static mut int32_t later = 1;
};
int main() { return BodylessGlobals::first; }
)");
  expect(!bodylessGti.semanticValid &&
             countDiagnosticCode(bodylessGti.diagnostics, "GTI-S2068") == 1 &&
             hasDiagnostic(bodylessGti.diagnostics, "no executable body"),
         "an ordinary bodyless GTI declaration should be unknown rather than "
         "silently treated as storage-effect-free");

  const lang::FrontendResult storedReferenceBoundary =
      lang::Frontend().analyze("stored-reference-later-storage.gti", R"(
class RefBox {
  int32_t& value;
public:
  RefBox(int32_t& source) : value(source) {}
};
class Globals {
public:
  static int32_t make_box() {
    RefBox box = RefBox(Globals::later);
    return 1;
  }
  static mut int32_t first = Globals::make_box();
  static constexpr int32_t later = 4;
};
int main() { return Globals::first; }
)");
  expect(!storedReferenceBoundary.semanticValid &&
             countDiagnosticCode(storedReferenceBoundary.diagnostics,
                                 "GTI-S2068") == 1,
         "stored-reference construction must force its later constexpr "
         "argument to remain a storage access");

  const lang::FrontendResult nativeAddressBoundary =
      lang::Frontend().analyze("native-address-later-storage.gti", R"(
extern "C" { void observe(const int32_t* value); }
class Globals {
public:
  static int32_t call_native() {
    unsafe { observe(&Globals::later); }
    return 1;
  }
  static mut int32_t first = Globals::call_native();
  static constexpr int32_t later = 5;
};
int main() { return Globals::first; }
)");
  expect(!nativeAddressBoundary.semanticValid &&
             countDiagnosticCode(nativeAddressBoundary.diagnostics,
                                 "GTI-S2068") == 1,
         "native calls may hide no GTI body edge, but their raw-address "
         "arguments must remain program-storage accesses");

  const lang::FrontendResult constructorAccess =
      lang::Frontend().analyze("constructor-later-storage.gti", R"(
class Probe {
public:
  Probe() { Probe::later = 1; }
  static int32_t make_probe() { Probe probe = Probe(); return 1; }
  static mut int32_t first = Probe::make_probe();
  static mut int32_t later = 0;
};
int main() { return Probe::first; }
)");
  expect(!constructorAccess.semanticValid &&
             countDiagnosticCode(constructorAccess.diagnostics, "GTI-S2068") ==
                 1,
         "construction effects should include the exact selected constructor "
         "body even when no destructor exists");

  const lang::FrontendResult implicitBaseConstructor =
      lang::Frontend().analyze("implicit-base-later-storage.gti", R"(
class ImplicitBase {
public:
  ImplicitBase() { ImplicitBaseProbe::later = 1; }
};
class ImplicitBaseProbe : public ImplicitBase {
public:
  ImplicitBaseProbe() {}
  static int32_t make_probe() {
    ImplicitBaseProbe probe = ImplicitBaseProbe();
    return 1;
  }
  static mut int32_t first = ImplicitBaseProbe::make_probe();
  static mut int32_t later = 0;
};
int main() { return ImplicitBaseProbe::first; }
)");
  expect(!implicitBaseConstructor.semanticValid &&
             countDiagnosticCode(implicitBaseConstructor.diagnostics,
                                 "GTI-S2068") == 1,
         "implicit base construction should include the exact selected base "
         "constructor effects before accepting a program initializer");

  const lang::FrontendResult destructorAccess =
      lang::Frontend().analyze("destructor-later-storage.gti", R"(
class Probe {
public:
  Probe() {}
  ~Probe() { Probe::later = 1; }
  static int32_t make_probe() { Probe probe = Probe(); return 1; }
  static mut int32_t first = Probe::make_probe();
  static mut int32_t later = 0;
};
int main() { return Probe::first; }
)");
  expect(!destructorAccess.semanticValid &&
             countDiagnosticCode(destructorAccess.diagnostics, "GTI-S2068") ==
                 1,
         "lexical and temporary cleanup should close over exact destructor "
         "effects before accepting a program initializer");

  const lang::FrontendResult earlierDestructor =
      lang::Frontend().analyze("earlier-destructor-storage.gti", R"(
class Probe {
public:
  Probe() {}
  ~Probe() { Probe::earlier = 1; }
  static mut int32_t earlier = 0;
  static int32_t make_probe() { Probe probe = Probe(); return 1; }
  static mut int32_t result = Probe::make_probe();
};
int main() { return Probe::result; }
)");
  expect(earlierDestructor.semanticValid &&
             !hasDiagnosticCode(earlierDestructor.diagnostics, "GTI-S2068"),
         "the same closed destructor effect should be accepted when its "
         "program storage step is earlier");

  const lang::FrontendResult genericStatic =
      lang::Frontend().analyze("generic-static-plan-exclusion.gti", R"(
class Registry<T> {
public:
  static mut int32_t value = 0;
};
)");
  expect(!genericStatic.semanticValid &&
             hasDiagnosticCode(genericStatic.diagnostics, "GTI-S2039") &&
             genericStatic.semantics.programInitializationPlan().steps.empty(),
         "generic class statics should remain outside the whole-program plan "
         "until their language-level qualified identity is supported");
}

void testTypedHirGenericInstances() {
  const lang::FrontendResult valid = lang::Frontend().analyze(
      "hir-generics.gti",
      R"(
#include <std/memory>

struct Widget {
public:
  int value = 7;
};

T transfer<T>(T value) {
  return std::move(value);
}

class Holder<T> {
  T value;
public:
  Holder(T value) : value(std::move(value)) {}
};

int main() {
  mut std::unique_ptr<Widget> owner = std::make_unique<Widget>();
  mut std::unique_ptr<Widget> transferred = transfer(std::move(owner));
  Holder<std::unique_ptr<Widget>> holder =
      Holder<std::unique_ptr<Widget>>(std::move(transferred));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!valid.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : valid.diagnostics) {
      std::cerr << "Unexpected HIR diagnostic: " << diagnostic.message << '\n';
    }
  }
  expect(valid.canGenerateCode(),
         "typed HIR should recheck valid concrete ownership transfers");
  expect(valid.hir.valid() && !valid.hir.classInstances().empty() &&
             !valid.hir.functionInstances().empty() &&
             !valid.hir.constructorInstances().empty() &&
             valid.hir.valueCount() > 0,
         "typed HIR should retain concrete class, callable, binding, and "
         "value instances");

  bool foundMoveOnlyTransfer = false;
  bool foundResolvedTransferCall = false;
  lang::HirFunctionInstanceId transferTarget = 0;
  for (const lang::HirFunctionInstance &instance :
       valid.hir.functionInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "transfer" &&
        !instance.parameterTypes.empty() && !instance.body.bindings.empty() &&
        !instance.body.bindings.front().info.traits.copyable) {
      foundMoveOnlyTransfer = true;
    }
    for (const lang::HirValue &value : instance.body.values) {
      if (value.functionTarget) {
        const lang::HirFunctionInstance *target =
            valid.hir.findFunctionInstance(*value.functionTarget);
        if (target != nullptr && target->source != nullptr &&
            target->source->name().lexeme == "transfer") {
          foundResolvedTransferCall = true;
          transferTarget = target->id;
        }
      }
    }
  }
  expect(foundMoveOnlyTransfer && foundResolvedTransferCall,
         "HIR should monomorphize move-only bindings and retain resolved call "
         "edges");

  lang::MirProgram missingCallTarget = valid.mir;
  bool erasedCallTarget = false;
  for (lang::MirFunctionInstance &instance :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           missingCallTarget.functionInstances())) {
    for (lang::MirBlock &block : instance.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (transferTarget != 0 &&
            instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget == transferTarget) {
          instruction.functionTarget.reset();
          erasedCallTarget = true;
          break;
        }
      }
      if (erasedCallTarget) {
        break;
      }
    }
    if (erasedCallTarget) {
      break;
    }
  }
  expect(erasedCallTarget && !lang::verifyMirProgram(missingCallTarget).valid(),
         "MIR verification should reject an ownership-consuming ordinary "
         "call whose exact target identity was erased");

  lang::MirProgram missingConstructorTarget = valid.mir;
  bool erasedConstructorTarget = false;
  for (lang::MirFunctionInstance &instance :
       const_cast<std::vector<lang::MirFunctionInstance> &>(
           missingConstructorTarget.functionInstances())) {
    for (lang::MirBlock &block : instance.body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.constructorKind == lang::ConstructorKind::Ordinary &&
            instruction.constructorTarget && !instruction.operands.empty() &&
            std::any_of(
                instruction.lifecycle.begin(), instruction.lifecycle.end(),
                [](const lang::MirLifecycleEvent &event) {
                  return event.kind == lang::MirLifecycleEventKind::TransferOut;
                })) {
          instruction.constructorTarget.reset();
          erasedConstructorTarget = true;
          break;
        }
      }
      if (erasedConstructorTarget) {
        break;
      }
    }
    if (erasedConstructorTarget) {
      break;
    }
  }
  expect(erasedConstructorTarget &&
             !lang::verifyMirProgram(missingConstructorTarget).valid(),
         "MIR verification should reject ownership-consuming ordinary "
         "construction whose exact target identity was erased");

  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "invalid-hir-generics.gti",
      R"(
#include <std/memory>

struct Widget {};

T accidental_copy<T>(T value) {
  return value;
}

class BadHolder<T> {
  T value;
public:
  BadHolder(T value) : value(value) {}
};

int main() {
  mut std::unique_ptr<Widget> first = std::make_unique<Widget>();
  std::unique_ptr<Widget> copied = accidental_copy(std::move(first));
  mut std::unique_ptr<Widget> second = std::make_unique<Widget>();
  BadHolder<std::unique_ptr<Widget>> holder =
      BadHolder<std::unique_ptr<Widget>>(std::move(second));
  return 0;
}
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!invalid.canGenerateCode() && invalid.semanticValid &&
             !invalid.hirValid,
         "invalid ownership in a concrete generic instance should fail in "
         "HIR lowering");
  expect(hasDiagnosticCode(invalid.diagnostics, "GTI-S2003") &&
             hasDiagnostic(invalid.diagnostics,
                           "Cannot initialize field 'value' by copying "
                           "move-only type") &&
             hasRelatedDiagnostic(invalid.diagnostics,
                                  "Concrete generic instance requested here"),
         "HIR diagnostics should accept the generic exact return while "
         "reporting the invalid field copy and instantiation site");
}

void testCompletePipeline() {
  lang::Lexer lexer;
  const std::string source = R"(
int twice(int value) {
  return value * 2;
}

int main() {
  int result = twice(4);
  if (result == 8) {
    return 0;
  }
  return 1;
}
)";
  auto tokens = lexer.scan(source);
  expect(!lexer.hadError(), "valid source should lex");

  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "valid source should parse");
  expect(program.declarations().size() == 2,
         "program should contain both functions");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program), "valid program should pass semantic checks");

  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find(
                 "_twice__gti_mir_failure(std::int32_t __gti_mir_arg_0") !=
                 std::string::npos &&
             generated.find("mir_checked_multiply_v1<std::int32_t>") !=
                 std::string::npos,
         "emitter should lower function signatures");
  expect(
      generated.find("_twice__gti_mir_failure(__gti_mir_v_") !=
              std::string::npos &&
          generated.find("// GTI verified-MIR body: scalar-cfg-failure-v1") !=
              std::string::npos,
      "emitter should lower immutable source bindings through MIR-owned "
      "places");
  expect(generated.find("#include <iostream>") == std::string::npos,
         "emitter should not include print runtime support");
}

constexpr NamedTest kTests[] = {
    {"testFrontendBackendAndOptimizationPipeline",
     testFrontendBackendAndOptimizationPipeline},
    {"testMirControlFlowAndOwnershipEffects",
     testMirControlFlowAndOwnershipEffects},
    {"testOrderedOrdinaryCallInputs", testOrderedOrdinaryCallInputs},
    {"testOrderedRawReceiverInputs", testOrderedRawReceiverInputs},
    {"testOrderedClassValueCallInputs", testOrderedClassValueCallInputs},
    {"testOrderedOrdinaryConstructorInputs",
     testOrderedOrdinaryConstructorInputs},
    {"testMirTemporaryAndDropObligations", testMirTemporaryAndDropObligations},
    {"testProgramArgumentsEntryPoint", testProgramArgumentsEntryPoint},
    {"testProgramInitializationPlans", testProgramInitializationPlans},
    {"testTypedHirGenericInstances", testTypedHirGenericInstances},
    {"testCompletePipeline", testCompletePipeline},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_e2e_tests");
  return runNamedTests(argc, argv, kTests, "e2e");
}
