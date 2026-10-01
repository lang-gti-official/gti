#include "../src/compiler/cpp_mir_body_text_writer.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

void testDischargedCompoundOperatorVocabulary() {
  expect(
      lang::cppMirCompoundOperatorSpelling(lang::MirOperation::AddAssign) ==
              "+=" &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::SubtractAssign) == "-=" &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::MultiplyAssign) == "*=" &&
          lang::cppMirCompoundOperatorSpelling(lang::MirOperation::DivideAssign)
              .empty() &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::RemainderAssign)
              .empty() &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::ShiftLeftAssign)
              .empty() &&
          lang::cppMirCompoundOperatorSpelling(
              lang::MirOperation::ShiftRightAssign)
              .empty(),
      "native compound emission should remain limited to operations whose "
      "proved successful path exactly matches C++ semantics");
}

void testExhaustiveEnumClassification() {
  for (std::size_t value = 0;
       value <= static_cast<std::size_t>(lang::MirInstructionKind::Count);
       ++value) {
    const auto encoding = lang::classifyCppMirInstructionKind(
        static_cast<lang::MirInstructionKind>(value));
    expect((value == static_cast<std::size_t>(lang::MirInstructionKind::Count))
               ? encoding == lang::CppMirEmissionEncoding::Invalid
               : encoding != lang::CppMirEmissionEncoding::Invalid,
           "every named MIR instruction kind should have an exact class");
  }
  for (std::size_t value = 0;
       value <= static_cast<std::size_t>(lang::MirOperation::Count); ++value) {
    const auto encoding =
        lang::classifyCppMirOperation(static_cast<lang::MirOperation>(value));
    expect((value == static_cast<std::size_t>(lang::MirOperation::Count))
               ? encoding == lang::CppMirEmissionEncoding::Invalid
               : encoding != lang::CppMirEmissionEncoding::Invalid,
           "every named MIR operation should have an exact class");
  }

  constexpr std::array operandKinds{
      lang::MirOperandKind::Value,       lang::MirOperandKind::Constant,
      lang::MirOperandKind::Address,     lang::MirOperandKind::Copy,
      lang::MirOperandKind::Move,        lang::MirOperandKind::BorrowRead,
      lang::MirOperandKind::BorrowWrite, lang::MirOperandKind::Loan};
  for (const lang::MirOperandKind kind : operandKinds) {
    expect(lang::classifyCppMirOperandKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR operand kind should have an exact class");
  }
  constexpr std::array rootKinds{
      lang::MirPlaceRootKind::Binding, lang::MirPlaceRootKind::Symbol,
      lang::MirPlaceRootKind::This,    lang::MirPlaceRootKind::Temporary,
      lang::MirPlaceRootKind::Value,   lang::MirPlaceRootKind::Loan};
  for (const lang::MirPlaceRootKind kind : rootKinds) {
    expect(lang::classifyCppMirPlaceRootKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR place root should have an exact class");
  }
  constexpr std::array projections{
      lang::MirProjectionKind::Field, lang::MirProjectionKind::Index,
      lang::MirProjectionKind::Dereference, lang::MirProjectionKind::RawIndex,
      lang::MirProjectionKind::RawDereference};
  for (const lang::MirProjectionKind kind : projections) {
    expect(lang::classifyCppMirProjectionKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR projection should have an exact class");
  }
  constexpr std::array terminators{lang::MirTerminatorKind::None,
                                   lang::MirTerminatorKind::Goto,
                                   lang::MirTerminatorKind::Branch,
                                   lang::MirTerminatorKind::Switch,
                                   lang::MirTerminatorKind::Invoke,
                                   lang::MirTerminatorKind::Return,
                                   lang::MirTerminatorKind::PropagateFailure,
                                   lang::MirTerminatorKind::Unreachable,
                                   lang::MirTerminatorKind::Exit};
  for (const lang::MirTerminatorKind kind : terminators) {
    expect((kind == lang::MirTerminatorKind::None)
               ? lang::classifyCppMirTerminatorKind(kind) ==
                     lang::CppMirEmissionEncoding::Invalid
               : lang::classifyCppMirTerminatorKind(kind) !=
                     lang::CppMirEmissionEncoding::Invalid,
           "every executable MIR terminator should have an exact class");
  }
  constexpr std::array bodyKinds{lang::MirBodyKind::Module,
                                 lang::MirBodyKind::FieldInitializers,
                                 lang::MirBodyKind::StaticFieldInitializers,
                                 lang::MirBodyKind::Function,
                                 lang::MirBodyKind::Constructor,
                                 lang::MirBodyKind::Destructor,
                                 lang::MirBodyKind::Lambda,
                                 lang::MirBodyKind::HostedStartup};
  for (const lang::MirBodyKind kind : bodyKinds) {
    expect(lang::classifyCppMirBodyKind(kind) !=
               lang::CppMirEmissionEncoding::Invalid,
           "every named MIR body kind should have an exact class");
  }

  expect(
      lang::classifyCppMirInstructionKind(lang::MirInstructionKind::Modify) ==
              lang::CppMirEmissionEncoding::MissingMirAuthority &&
          lang::classifyCppMirOperation(lang::MirOperation::Index) ==
              lang::CppMirEmissionEncoding::NeedsCopiedRepresentation,
      "known schedule gaps and safe bounds should remain distinct");
  expect(
      lang::classifyCppMirOperandKind(static_cast<lang::MirOperandKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirPlaceRootKind(static_cast<lang::MirPlaceRootKind>(
              999)) == lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirProjectionKind(
              static_cast<lang::MirProjectionKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirTerminatorKind(
              static_cast<lang::MirTerminatorKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid &&
          lang::classifyCppMirBodyKind(static_cast<lang::MirBodyKind>(999)) ==
              lang::CppMirEmissionEncoding::Invalid,
      "unknown future enum values should fail closed");
}

void testReadyBodyAndRepresentationFailures() {
  const lang::FrontendResult frontend = analyze("cpp-mir-body-ready.gti", R"(
int32_t identity(int32_t value) { return value; }
)");
  expect(frontend.canGenerateCode(),
         "the generic ready-body fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  expect(function != nullptr, "the ready fixture should lower one function");
  if (function == nullptr) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};

  lang::CppMirBodyEmissionMapRows rows = completeRows(frontend.mir);
  const lang::CppMirBodyEmissionAnalysis ready =
      lang::CppMirBodyEmitter(frontend.mir, lang::CppMirBodyEmissionMap(rows))
          .analyze(address);
  expect(ready.ready(),
         "a verified scalar identity body and complete copied map should be "
         "ready, including legitimately untyped lifecycle records");

  const lang::CppMirBodyEmissionAnalysis missing =
      lang::CppMirBodyEmitter(frontend.mir, lang::CppMirBodyEmissionMap{})
          .analyze(address);
  expect(missing.readiness ==
                 lang::CppMirBodyEmissionReadiness::MissingRepresentation &&
             hasIssue(
                 missing,
                 lang::CppMirBodyEmissionIssueKind::MissingTypeRepresentation),
         "an absent copied map should be classified as representation debt");

  rows.types.push_back(rows.types.front());
  const lang::CppMirBodyEmissionAnalysis duplicate =
      lang::CppMirBodyEmitter(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(rows)))
          .analyze(address);
  expect(
      duplicate.readiness == lang::CppMirBodyEmissionReadiness::Incoherent &&
          hasIssue(
              duplicate,
              lang::CppMirBodyEmissionIssueKind::DuplicateTypeRepresentation),
      "duplicate copied type rows should fail closed as incoherent");
}

void testOwningPayloadEnumRepresentationAdmission() {
  const lang::FrontendResult frontend = analyze("owning-payload-map.gti", R"(
mut int32_t drops = 0;
class leaf { public: leaf() {} ~leaf() { drops = drops + 1; } mut int32_t value = 7; };
enum class holder { one(leaf value), none };
int main() {
  holder held = holder::one(leaf());
  switch (held) { case holder::one: break; case holder::none: break; }
  return drops;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning payload-enum map fixture should pass the frontend");
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  if (!frontend.canGenerateCode() || function == nullptr) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  lang::CppMirBodyEmissionMapRows rows = buildRows(frontend, frontend.mir);
  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirBodyEmissionAnalysis exact = emitter.analyze(address);
  if (!exact.ready()) {
    for (const lang::CppMirBodyEmissionIssue &issue : exact.issues) {
      std::cerr << "owning payload admission: " << issue.detail << '\n';
    }
  }
  expect(exact.ready(),
         "an exact owning payload-enum lifecycle row should admit verified "
         "MIR body analysis");

  const auto enumeration = std::find_if(
      rows.enums.begin(), rows.enums.end(),
      [](const lang::CppMirEnumRepresentation &row) {
        return row.requiresActiveCleanup && !row.payloadVariants.empty();
      });
  expect(enumeration != rows.enums.end(),
         "the sealed representation map should carry the owning enum row");
  if (enumeration == rows.enums.end() ||
      enumeration->payloadVariants.front().fieldRequiresActiveCleanup.empty()) {
    return;
  }
  enumeration->payloadVariants.front().fieldRequiresActiveCleanup.front() =
      false;
  const lang::CppMirBodyEmissionAnalysis forged =
      lang::CppMirBodyEmitter(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(rows)))
          .analyze(address);
  expect(
      forged.readiness == lang::CppMirBodyEmissionReadiness::Incoherent &&
          hasIssue(forged,
                   lang::CppMirBodyEmissionIssueKind::InvalidRepresentationRow),
      "a forged payload cleanup row should fail closed against verified "
      "MIR");
}

lang::MirInstructionId immutableClassMoveAssignment(const lang::MirBody &body) {
  lang::MirInstructionId found = 0;
  for (const lang::MirBlock &block : body.blocks) {
    for (std::size_t index = 0; index + 1 < block.instructions.size();
         ++index) {
      const lang::MirInstruction &move = block.instructions[index];
      const lang::MirBlock *commitBlock = &block;
      std::size_t commitIndex = index + 1;
      const lang::MirInstruction *prepare = nullptr;
      if (block.instructions[commitIndex].kind ==
          lang::MirInstructionKind::CleanupBeforeAssign) {
        prepare = &block.instructions[commitIndex];
        if (!prepare->replacementCleanup || !prepare->lifecycle.empty())
          continue;
        if (prepare->definedFailure.propagation ==
            lang::FailurePropagationKind::Destructor) {
          if (commitIndex + 1 != block.instructions.size() ||
              block.terminator.kind != lang::MirTerminatorKind::Invoke ||
              block.terminator.invokeInstruction != prepare->id)
            continue;
          commitBlock = body.findBlock(block.terminator.target);
          commitIndex = 0;
        } else {
          ++commitIndex;
        }
      }
      if (commitBlock == nullptr ||
          commitIndex + 1 >= commitBlock->instructions.size())
        continue;
      const lang::MirInstruction &assign =
          commitBlock->instructions[commitIndex];
      const lang::MirInstruction &destroy =
          commitBlock->instructions[commitIndex + 1];
      if (prepare != nullptr && (prepare->replacementPair != assign.id ||
                                 assign.replacementPair != prepare->id ||
                                 prepare->destination != assign.destination))
        continue;
      const lang::MirPlace *source =
          move.operands.size() == 1
              ? body.findPlace(move.operands.front().place)
              : nullptr;
      if (move.kind != lang::MirInstructionKind::Move || !move.result ||
          move.info.type.kind != lang::SemanticType::Class ||
          move.lifecycle.size() != 1 || source == nullptr ||
          source->access != lang::AccessMode::ReadOnly ||
          assign.kind != lang::MirInstructionKind::Assign ||
          assign.operands.size() != 1 || assign.lifecycle.size() != 1 ||
          assign.operands.front().kind != lang::MirOperandKind::Value ||
          assign.operands.front().value != *move.result ||
          destroy.kind != lang::MirInstructionKind::Drop ||
          destroy.lifecycle.size() != 1) {
        continue;
      }
      if (found != 0) {
        return 0;
      }
      found = move.id;
    }
  }
  return found;
}

void testOwningPayloadEnumFieldContainmentAdmission() {
  const lang::FrontendResult frontend =
      analyze("owning-payload-field-containment.gti", R"(
class resource {
public:
  resource(int32_t value) : value(value) {}
  ~resource() {}
private:
  int32_t value;
};
enum class holder { one(resource value), none };
struct wrapper<T> { holder value = holder::one(resource(7)); };
int main() { wrapper<int32_t> value = wrapper<int32_t>(); return 0; }
)");
  expect(frontend.canGenerateCode(),
         "an ordinary direct owning payload-enum field should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirClassInstance *wrapper = nullptr;
  for (const lang::MirClassInstance &candidate :
       frontend.mir.classInstances()) {
    if (candidate.kind == lang::ClassKind::Struct &&
        candidate.declaredFields.size() == 1 &&
        candidate.declaredFields.front().type.kind ==
            lang::SemanticType::Enum &&
        candidate.declaredFields.front().requiresActiveCleanup) {
      wrapper = &candidate;
    }
  }
  expect(wrapper != nullptr,
         "the containing struct should retain its active field lifecycle");
  if (wrapper == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{
      .kind = lang::MirBodyKind::FieldInitializers, .owner = wrapper->id};
  const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(address);
  if (!analysis.ready()) {
    for (const lang::CppMirBodyEmissionIssue &issue : analysis.issues) {
      std::cerr << "owning payload field admission: " << issue.detail << '\n';
    }
  }
  const lang::CppMirInitializerScheduleText schedule =
      emitter.initializerSchedule(address);
  expect(analysis.ready() && schedule.supported && schedule.fields.size() == 1,
         "the exact active field-initializer schedule should admit one native "
         "field spelling");

  lang::MirProgram forged = frontend.mir;
  lang::MirBody *forgedBody = lang::findMirBody(forged, address);
  lang::MirInstruction *publication = nullptr;
  if (forgedBody != nullptr) {
    for (lang::MirBlock &block : forgedBody->blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind ==
                lang::MirLifecycleEventKind::Reparent) {
          publication = &instruction;
        }
      }
    }
  }
  expect(publication != nullptr,
         "the field fixture should contain one explicit reparent publication");
  if (publication != nullptr) {
    publication->lifecycle.front().target = 0;
    const lang::CppMirBodyEmissionAnalysis forgedAnalysis =
        lang::CppMirBodyEmitter(forged, map).analyze(address);
    expect(!forgedAnalysis.ready() &&
               hasIssue(forgedAnalysis,
                        lang::CppMirBodyEmissionIssueKind::InvalidMirProgram),
           "a forged owning-field reparent edge should fail closed before C++ "
           "text emission");
  }

  const lang::FrontendResult immutableClass = analyzeWithStandardLibrary(
      "immutable-generated-class-move-assignment.gti", R"(
mut int32_t drops = 0;
class resource {
public:
  resource(int32_t identity) : identity(identity) {}
  resource(resource&& other) = default;
  ~resource() { drops = drops + 1; }
private:
  int32_t identity;
};
int main() {
  resource source = resource(1);
  mut resource destination = resource(2);
  destination = std::move(source);
  return 0;
}
)");
  expect(immutableClass.canGenerateCode(),
         "an immutable cleanup-owning class binding should remain a valid "
         "explicit move source");
  const lang::MirFunctionInstance *immutableClassEntry = nullptr;
  for (const lang::MirFunctionInstance &candidate :
       immutableClass.mir.functionInstances()) {
    if (candidate.entryKind != lang::ProgramEntryKind::None) {
      immutableClassEntry = &candidate;
    }
  }
  expect(immutableClassEntry != nullptr,
         "the immutable class move fixture should retain its entry identity");
  if (immutableClass.canGenerateCode() && immutableClassEntry != nullptr) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = immutableClassEntry->id};
    const bool hasPreparation = std::any_of(
        immutableClassEntry->body.blocks.begin(),
        immutableClassEntry->body.blocks.end(), [](const auto &block) {
          return std::any_of(
              block.instructions.begin(), block.instructions.end(),
              [](const auto &instruction) {
                return instruction.kind ==
                       lang::MirInstructionKind::CleanupBeforeAssign;
              });
        });
    expect(hasPreparation &&
               immutableClassMoveAssignment(immutableClassEntry->body) != 0,
           "the unrelated class fixture should retain one immutable-source "
           "Move/cleanup/commit/Drop schedule without payload-enum state");
    for (const lang::CppStandard standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const lang::CppMirBodyEmissionMap map(
          buildRows(immutableClass, immutableClass.mir, standard));
      const lang::CppMirBodyEmitter immutableMoveEmitter(immutableClass.mir,
                                                         map);
      expect(immutableMoveEmitter.analyze(address).ready() &&
                 immutableMoveEmitter.supportsFailureBodyText(address),
             "verified immutable generated-class movement should retain its "
             "C++20/C++23 failure-body representation");
    }
  }

  const lang::FrontendResult nestedEntry =
      analyzeWithStandardLibrary("owning-payload-field-nested-entry.gti", R"(
mut int32_t drops = 0;
class resource {
public:
  resource(int32_t identity) : identity(identity) {}
  resource(resource&& other) = default;
  ~resource() { drops = drops + 1; }
private:
  int32_t identity;
};
enum class holder { occupied(resource value), empty };
class package {
public:
  package(holder content) : content(std::move(content)) {}
  void replace(holder replacement) mut {
    this.content = std::move(replacement);
  }
private:
  mut holder content;
};
struct default_package {
  holder content = holder::occupied(resource(4));
};
int main() {
  {
    package first = package(holder::occupied(resource(1)));
    package second = std::move(first);
    mut package destination = package(holder::occupied(resource(2)));
    destination = std::move(second);
    destination.replace(holder::occupied(resource(3)));
    default_package defaulted = default_package();
  }
  return drops == 4 ? 0 : 1;
}
)");
  expect(nestedEntry.canGenerateCode(),
         "nested entry aggregate payload-enum containment should pass the "
         "frontend");
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &candidate :
       nestedEntry.mir.functionInstances()) {
    if (candidate.entryKind != lang::ProgramEntryKind::None) {
      entry = &candidate;
    }
  }
  expect(entry != nullptr,
         "the nested aggregate payload-enum fixture should retain its entry "
         "body identity");
  if (nestedEntry.canGenerateCode() && entry != nullptr) {
    const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                            .owner = entry->id};
    const lang::MirInstructionId immutableAggregateMove =
        immutableClassMoveAssignment(entry->body);
    expect(immutableAggregateMove != 0,
           "the entry fixture should retain an immutable aggregate move, "
           "adjacent replacement, and moved-shell drop");

    for (const lang::CppStandard standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const lang::CppMirBodyEmissionMap entryMap(
          buildRows(nestedEntry, nestedEntry.mir, standard));
      const lang::CppMirBodyEmissionAnalysis entryAnalysis =
          lang::CppMirBodyEmitter(nestedEntry.mir, entryMap)
              .analyze(entryAddress);
      if (!entryAnalysis.ready()) {
        for (const lang::CppMirBodyEmissionIssue &issue :
             entryAnalysis.issues) {
          std::cerr << "nested owning payload entry admission: " << issue.detail
                    << '\n';
        }
      }
      expect(entryAnalysis.ready() &&
                 lang::CppMirBodyEmitter(nestedEntry.mir, entryMap)
                     .supportsFailureBodyText(entryAddress),
             "an entry body should admit and spell the exact nested aggregate "
             "payload-enum lifecycle schedule under both C++ standards");

      if (immutableAggregateMove != 0) {
        lang::MirProgram forged = nestedEntry.mir;
        lang::MirBody *forgedEntry = lang::findMirBody(forged, entryAddress);
        lang::MirInstruction *forgedMove = nullptr;
        if (forgedEntry != nullptr) {
          for (lang::MirBlock &block : forgedEntry->blocks) {
            for (lang::MirInstruction &instruction : block.instructions) {
              if (instruction.id == immutableAggregateMove) {
                forgedMove = &instruction;
              }
            }
          }
        }
        expect(forgedMove != nullptr && forgedMove->lifecycle.size() == 1,
               "the aggregate move should retain its unique transfer event");
        if (forgedMove != nullptr && forgedMove->lifecycle.size() == 1) {
          forgedMove->lifecycle.front().target = 0;
          const lang::CppMirBodyEmitter forgedEmitter(forged, entryMap);
          const lang::CppMirBodyEmissionAnalysis forgedAnalysis =
              forgedEmitter.analyze(entryAddress);
          expect(
              !forgedAnalysis.ready() &&
                  hasIssue(
                      forgedAnalysis,
                      lang::CppMirBodyEmissionIssueKind::InvalidMirProgram) &&
                  !forgedEmitter.supportsFailureBodyText(entryAddress),
              "a forged immutable aggregate move without its staging "
              "obligation must fail closed before C++ text emission");
        }
      }
    }
  }
}

void testOwningPayloadEnumTransportAdmissionFailsClosed() {
  testOwningPayloadEnumFieldContainmentAdmission();

  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("owning-payload-transport-preflight.gti", R"(
class resource {
public:
  resource() {}
  resource(resource&& other) = default;
  ~resource() {}
};

enum class holder { one(resource value), none };

class relay<T> {
public:
  relay(holder value) {}
};

T transfer<T>(T value, int32_t divisor) {
  int32_t checked = int32_t(1) / divisor;
  return std::move(value);
}

holder named(holder value) {
  return transfer(std::move(value), 1);
}

holder fresh() {
  return transfer(holder::one(resource()), 1);
}

holder direct_fresh_return() {
  return holder::one(resource());
}

holder constructor_direct(holder value) {
  relay<int32_t> service = relay<int32_t>(std::move(value));
  return holder::none;
}

holder exact_lambda(holder value) {
  auto invoke = [](holder inner, int32_t divisor) -> holder {
    int32_t checked = int32_t(1) / divisor;
    return std::move(inner);
  };
  return invoke(std::move(value), 1);
}

holder fresh_exact_lambda() {
  auto invoke = [](holder inner, int32_t divisor) -> holder {
    int32_t checked = int32_t(1) / divisor;
    return std::move(inner);
  };
  return invoke(holder::one(resource()), 1);
}

holder later_argument_failure(int32_t divisor) {
  return transfer(holder::one(resource()), int32_t(1) / divisor);
}

holder later_argument_move_failure(holder value, int32_t divisor) {
  return transfer(std::move(value), int32_t(1) / divisor);
}

int main() { return 0; }
)");
  expect(frontend.canGenerateCode(),
         "the owning payload-enum transport preflight fixture should pass "
         "the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto findInstruction = [](const lang::MirBody &body,
                                  lang::MirInstructionId id) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const auto findMutableInstruction = [](lang::MirBody &body,
                                         lang::MirInstructionId id) {
    for (lang::MirBlock &block : body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto owningPayload = [&](const lang::SemanticType &type) {
    const lang::MirPayloadEnum *enumeration =
        type.kind == lang::SemanticType::Enum
            ? frontend.mir.findPayloadEnum(type.enumId)
            : nullptr;
    return enumeration != nullptr && enumeration->requiresActiveCleanup;
  };

  struct StagedShape {
    lang::HirFunctionInstanceId function = 0;
    lang::MirInstructionId producer = 0;
    lang::MirInstructionId stage = 0;
    lang::MirInstructionId consumer = 0;

    [[nodiscard]] explicit operator bool() const {
      return function != 0 && producer != 0 && stage != 0 && consumer != 0;
    }
  };
  struct DirectShape {
    lang::HirFunctionInstanceId function = 0;
    lang::MirInstructionId move = 0;
    lang::MirInstructionId consumer = 0;

    [[nodiscard]] explicit operator bool() const {
      return function != 0 && move != 0 && consumer != 0;
    }
  };
  struct DirectFreshShape {
    lang::HirFunctionInstanceId function = 0;
    lang::MirInstructionId producer = 0;
    lang::MirInstructionId consumer = 0;

    [[nodiscard]] explicit operator bool() const {
      return function != 0 && producer != 0 && consumer != 0;
    }
  };
  struct DirectFreshReturnShape {
    lang::HirFunctionInstanceId function = 0;
    lang::MirInstructionId producer = 0;
    lang::MirValueId value = 0;

    [[nodiscard]] explicit operator bool() const {
      return function != 0 && producer != 0 && value != 0;
    }
  };

  StagedShape namedShape;
  StagedShape freshShape;
  StagedShape directConstructorShape;
  DirectShape directShape;
  DirectFreshShape directFreshShape;
  DirectFreshReturnShape directFreshReturnShape;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.definitionKind != lang::MirDefinitionKind::Source) {
      continue;
    }
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::PayloadConstruct &&
            instruction.result && !instruction.operands.empty() &&
            owningPayload(instruction.info.type) &&
            block.terminator.kind == lang::MirTerminatorKind::Return &&
            block.terminator.value &&
            block.terminator.value->kind == lang::MirOperandKind::Value &&
            block.terminator.value->value == *instruction.result) {
          directFreshReturnShape = {.function = function.id,
                                    .producer = instruction.id,
                                    .value = *instruction.result};
        }
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.result && instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Value &&
            owningPayload(instruction.operands.front().type)) {
          const lang::MirValue *source =
              function.body.findValue(instruction.operands.front().value);
          const lang::MirInstruction *producer =
              source == nullptr
                  ? nullptr
                  : findInstruction(function.body, source->definition);
          const std::vector<lang::MirValueUse> &uses =
              function.body.usesOf(*instruction.result);
          const lang::MirInstruction *consumer =
              uses.size() == 1 && uses.front().kind ==
                                      lang::MirValueUseKind::InstructionOperand
                  ? findInstruction(function.body, uses.front().instruction)
                  : nullptr;
          if (producer != nullptr && consumer != nullptr &&
              (consumer->kind == lang::MirInstructionKind::Call ||
               consumer->kind == lang::MirInstructionKind::Construct)) {
            StagedShape shape{.function = function.id,
                              .producer = producer->id,
                              .stage = instruction.id,
                              .consumer = consumer->id};
            if (producer->kind == lang::MirInstructionKind::Move) {
              if (consumer->kind == lang::MirInstructionKind::Construct &&
                  instruction.destination &&
                  instruction.preparedParameterDrop) {
                directConstructorShape = shape;
              } else {
                namedShape = shape;
              }
            } else if (producer->kind == lang::MirInstructionKind::Compute &&
                       producer->operation ==
                           lang::MirOperation::PayloadConstruct) {
              freshShape = shape;
            }
          }
        }
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.callableInvocation || !instruction.lambdaTarget ||
            !instruction.result || !owningPayload(instruction.info.type)) {
          continue;
        }
        for (const lang::MirOperand &operand : instruction.operands) {
          const lang::MirValue *value =
              operand.kind == lang::MirOperandKind::Value
                  ? function.body.findValue(operand.value)
                  : nullptr;
          const lang::MirInstruction *producer =
              value == nullptr
                  ? nullptr
                  : findInstruction(function.body, value->definition);
          if (producer != nullptr &&
              producer->kind == lang::MirInstructionKind::Move &&
              owningPayload(operand.type)) {
            directShape = {.function = function.id,
                           .move = producer->id,
                           .consumer = instruction.id};
          } else if (producer != nullptr &&
                     producer->kind == lang::MirInstructionKind::Compute &&
                     producer->operation ==
                         lang::MirOperation::PayloadConstruct &&
                     owningPayload(operand.type)) {
            directFreshShape = {.function = function.id,
                                .producer = producer->id,
                                .consumer = instruction.id};
          }
        }
      }
    }
  }
  expect(namedShape && freshShape && directConstructorShape && directShape &&
             directFreshShape && directFreshReturnShape,
         "the transport fixture should retain prepared named and constructor, "
         "staged-fresh, both direct-lambda, and direct fresh-return owning "
         "payload-enum schedules");
  if (!namedShape || !freshShape || !directConstructorShape || !directShape ||
      !directFreshShape || !directFreshReturnShape) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const auto address = [](lang::HirFunctionInstanceId function) {
    return lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                .owner = function};
  };
  const lang::CppMirBodyEmitter canonical(frontend.mir, map);
  const auto admits = [&](lang::HirFunctionInstanceId function,
                          std::string_view label) {
    const bool supported = canonical.supportsFailureBodyText(address(function));
    if (!supported) {
      std::cerr << "owning payload transport rejected canonical " << label
                << '\n';
    }
    return supported;
  };
  expect(admits(namedShape.function, "named call") &&
             admits(freshShape.function, "fresh call") &&
             admits(directConstructorShape.function, "direct constructor") &&
             admits(directShape.function, "named exact lambda") &&
             admits(directFreshShape.function, "fresh exact lambda") &&
             admits(directFreshReturnShape.function, "direct fresh return"),
         "all canonical owning payload-enum transport bodies should pass "
         "failure-form admission");

  const auto forge = [&](lang::HirFunctionInstanceId functionId,
                         const auto &mutation) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    const auto found =
        std::find_if(functions.begin(), functions.end(),
                     [&](const lang::MirFunctionInstance &function) {
                       return function.id == functionId;
                     });
    expect(found != functions.end(),
           "the forged transport body should retain its function instance");
    if (found != functions.end()) {
      mutation(found->body);
      expect(lang::rebuildMirValueUses(found->body),
             "the forged transport body should retain a rebuildable use "
             "index");
    }
    return forged;
  };
  const auto expectRejected = [&](const lang::MirProgram &forged,
                                  lang::HirFunctionInstanceId function,
                                  std::string_view message) {
    expect(!lang::CppMirBodyEmitter(forged, map)
                .supportsFailureBodyText(address(function)),
           message);
  };

  const lang::MirProgram wrongParameterEntryDrop =
      forge(namedShape.function, [&](lang::MirBody &body) {
        const auto place = std::find_if(
            body.places.begin(), body.places.end(),
            [&](const lang::MirPlace &candidate) {
              return candidate.root == lang::MirPlaceRootKind::Binding &&
                     candidate.projections.empty() &&
                     candidate.initiallyAvailable &&
                     owningPayload(candidate.type);
            });
        expect(place != body.places.end(),
               "the named transport body should retain its active payload "
               "parameter place");
        if (place == body.places.end()) {
          return;
        }
        const auto drop = std::find_if(
            body.dropObligations.begin(), body.dropObligations.end(),
            [&](const lang::MirDropObligation &candidate) {
              return candidate.place == place->id &&
                     candidate.kind == lang::MirDropObligationKind::Binding;
            });
        expect(drop != body.dropObligations.end() && drop->initiallyActive,
               "the owning payload parameter should enter with one active "
               "binding drop");
        if (drop != body.dropObligations.end()) {
          drop->initiallyActive = false;
        }
      });
  expectRejected(wrongParameterEntryDrop, namedShape.function,
                 "an owning payload-enum parameter whose entry drop is not "
                 "active must fail admission");

  const lang::MirProgram wrongMoveKind =
      forge(namedShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *move =
            findMutableInstruction(body, namedShape.producer);
        expect(move != nullptr && move->lifecycle.size() == 1,
               "the named transport move should retain one lifecycle edge");
        if (move != nullptr && move->lifecycle.size() == 1) {
          move->lifecycle.front().kind =
              lang::MirLifecycleEventKind::Initialize;
        }
      });
  expectRejected(wrongMoveKind, namedShape.function,
                 "an owning payload-enum move with the wrong lifecycle kind "
                 "must fail admission");

  const lang::MirProgram wrongStageTarget =
      forge(namedShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *stage =
            findMutableInstruction(body, namedShape.stage);
        expect(stage != nullptr && stage->lifecycle.size() == 1,
               "the named transport stage should retain one lifecycle edge");
        if (stage != nullptr && stage->lifecycle.size() == 1) {
          stage->lifecycle.front().target = stage->lifecycle.front().source;
        }
      });
  expectRejected(wrongStageTarget, namedShape.function,
                 "an owning payload-enum stage with the wrong lifecycle "
                 "target must fail admission");

  const lang::MirProgram wrongCallSite =
      forge(namedShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *stage =
            findMutableInstruction(body, namedShape.stage);
        if (stage != nullptr) {
          ++stage->callSite;
        }
      });
  expectRejected(wrongCallSite, namedShape.function,
                 "a payload-enum stage paired to a different call site must "
                 "fail admission");

  lang::MirProgram wrongConsumerKind = frontend.mir;
  {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongConsumerKind.functionInstances());
    const auto caller =
        std::find_if(functions.begin(), functions.end(),
                     [&](const lang::MirFunctionInstance &function) {
                       return function.id == namedShape.function;
                     });
    lang::MirInstruction *consumer =
        caller == functions.end()
            ? nullptr
            : findMutableInstruction(caller->body, namedShape.consumer);
    const auto target =
        consumer != nullptr && consumer->functionTarget
            ? std::find_if(functions.begin(), functions.end(),
                           [&](const lang::MirFunctionInstance &function) {
                             return function.id == *consumer->functionTarget;
                           })
            : functions.end();
    expect(target != functions.end(),
           "the named payload transport should retain its exact source "
           "function target");
    if (target != functions.end()) {
      target->linkage = lang::LanguageLinkage::C;
    }
  }
  expectRejected(wrongConsumerKind, namedShape.function,
                 "a payload-enum CallInput targeting a native callable "
                 "instance must fail admission");

  const lang::MirProgram wrongInputIndex =
      forge(namedShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *stage =
            findMutableInstruction(body, namedShape.stage);
        lang::MirInstruction *consumer =
            findMutableInstruction(body, namedShape.consumer);
        if (stage != nullptr && consumer != nullptr) {
          stage->callInputIndex = consumer->operands.size();
        }
      });
  expectRejected(wrongInputIndex, namedShape.function,
                 "a payload-enum stage with an out-of-range argument index "
                 "must fail admission");

  const lang::MirProgram duplicateStagedUse =
      forge(namedShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *stage =
            findMutableInstruction(body, namedShape.stage);
        lang::MirInstruction *consumer =
            findMutableInstruction(body, namedShape.consumer);
        if (stage != nullptr && stage->result && consumer != nullptr) {
          consumer->operands.push_back({.kind = lang::MirOperandKind::Value,
                                        .value = *stage->result,
                                        .type = stage->info.type});
        }
      });
  expectRejected(duplicateStagedUse, namedShape.function,
                 "a payload-enum stage with multiple consuming uses must "
                 "fail admission");

  const lang::MirProgram wrongFreshInitialization =
      forge(freshShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *producer =
            findMutableInstruction(body, freshShape.producer);
        if (producer == nullptr) {
          return;
        }
        const auto event = std::find_if(
            producer->lifecycle.begin(), producer->lifecycle.end(),
            [](const lang::MirLifecycleEvent &candidate) {
              return candidate.kind == lang::MirLifecycleEventKind::Initialize;
            });
        expect(event != producer->lifecycle.end(),
               "the fresh payload construction should arm one result drop");
        if (event != producer->lifecycle.end()) {
          event->target = 0;
        }
      });
  expectRejected(wrongFreshInitialization, freshShape.function,
                 "a fresh payload-enum stage with no exact construction "
                 "activation must fail admission");

  const lang::MirProgram wrongPreparedPlace =
      forge(freshShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *stage =
            findMutableInstruction(body, freshShape.stage);
        expect(stage != nullptr && stage->preparedParameterDrop &&
                   stage->destination,
               "the fresh free-function argument should retain its prepared "
               "parameter place");
        if (stage == nullptr || !stage->preparedParameterDrop ||
            !stage->destination) {
          return;
        }
        const auto drop = std::find_if(
            body.dropObligations.begin(), body.dropObligations.end(),
            [&](const lang::MirDropObligation &candidate) {
              return candidate.id == *stage->preparedParameterDrop;
            });
        if (drop != body.dropObligations.end()) {
          drop->place = 0;
        }
      });
  expectRejected(wrongPreparedPlace, freshShape.function,
                 "a prepared payload-enum drop detached from its destination "
                 "place must fail admission");

  const lang::MirProgram conditionalPreparedCleanup =
      forge(freshShape.function, [&](lang::MirBody &body) {
        const auto *stage = findMutableInstruction(body, freshShape.stage);
        bool changed = false;
        if (stage != nullptr && stage->preparedParameterDrop) {
          for (auto &block : body.blocks) {
            for (auto &instruction : block.instructions) {
              if (instruction.kind == lang::MirInstructionKind::Drop &&
                  instruction.lifecycle.size() == 1 &&
                  instruction.lifecycle.front().source ==
                      *stage->preparedParameterDrop) {
                instruction.lifecycle.front().conditional = true;
                changed = true;
              }
            }
          }
        }
        expect(changed,
               "later argument failure must retain prepared enum cleanup");
      });
  expectRejected(conditionalPreparedCleanup, freshShape.function,
                 "conditional prepared cleanup requires an independent "
                 "engagement state and cannot alias the backing shell");

  const lang::MirProgram wrongResultPlace =
      forge(namedShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *call =
            findMutableInstruction(body, namedShape.consumer);
        expect(call != nullptr && call->successResultDrop,
               "the owning payload-enum call should retain a result drop");
        if (call == nullptr || !call->successResultDrop) {
          return;
        }
        const auto drop = std::find_if(
            body.dropObligations.begin(), body.dropObligations.end(),
            [&](const lang::MirDropObligation &candidate) {
              return candidate.id == *call->successResultDrop;
            });
        if (drop != body.dropObligations.end()) {
          drop->place = 0;
        }
      });
  expectRejected(wrongResultPlace, namedShape.function,
                 "an owning payload-enum result drop without its exact value "
                 "slot must fail admission");

  const lang::MirProgram wrongSuccessPairing =
      forge(namedShape.function, [&](lang::MirBody &body) {
        const auto block =
            std::find_if(body.blocks.begin(), body.blocks.end(),
                         [&](const lang::MirBlock &candidate) {
                           return candidate.terminator.kind ==
                                      lang::MirTerminatorKind::Invoke &&
                                  candidate.terminator.invokeInstruction ==
                                      namedShape.consumer;
                         });
        expect(block != body.blocks.end() &&
                   block->terminator.successLifecycle.size() == 1,
               "the payload-enum result should retain one Invoke-success "
               "activation");
        if (block != body.blocks.end() &&
            block->terminator.successLifecycle.size() == 1) {
          block->terminator.successLifecycle.front().target = 0;
        }
      });
  expectRejected(wrongSuccessPairing, namedShape.function,
                 "a payload-enum result whose Invoke success edge activates "
                 "the wrong drop must fail admission");

  const lang::MirProgram wrongDirectConstructorTransfer =
      forge(directConstructorShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *stage =
            findMutableInstruction(body, directConstructorShape.stage);
        expect(stage != nullptr && stage->lifecycle.size() == 1 &&
                   stage->destination && stage->preparedParameterDrop &&
                   stage->lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Reparent &&
                   stage->lifecycle.front().target ==
                       *stage->preparedParameterDrop,
               "the constructor argument should reparent into its exact "
               "prepared CallInput slot before invocation");
        if (stage != nullptr && stage->lifecycle.size() == 1) {
          stage->lifecycle.front().kind =
              lang::MirLifecycleEventKind::TransferOut;
          stage->lifecycle.front().target = 0;
        }
      });
  expectRejected(
      wrongDirectConstructorTransfer, directConstructorShape.function,
      "a constructor argument transferred away before its prepared CallInput "
      "slot owns it must fail admission");

  const lang::MirProgram wrongConstructorInvocationTransfer =
      forge(directConstructorShape.function, [&](lang::MirBody &body) {
        const lang::MirInstruction *stage =
            findMutableInstruction(body, directConstructorShape.stage);
        lang::MirInstruction *consumer =
            findMutableInstruction(body, directConstructorShape.consumer);
        expect(
            stage != nullptr && stage->preparedParameterDrop &&
                consumer != nullptr,
            "constructor transport should retain preparation and invocation");
        if (stage == nullptr || !stage->preparedParameterDrop ||
            consumer == nullptr) {
          return;
        }
        const auto transfer = std::find_if(
            consumer->lifecycle.begin(), consumer->lifecycle.end(),
            [&](const lang::MirLifecycleEvent &event) {
              return event.kind == lang::MirLifecycleEventKind::TransferOut &&
                     event.source == *stage->preparedParameterDrop;
            });
        expect(transfer != consumer->lifecycle.end(),
               "constructor invocation should consume the prepared slot");
        if (transfer != consumer->lifecycle.end()) {
          transfer->kind = lang::MirLifecycleEventKind::Initialize;
        }
      });
  expectRejected(wrongConstructorInvocationTransfer,
                 directConstructorShape.function,
                 "a constructor invocation without its prepared-argument "
                 "TransferOut must fail admission");

  lang::MirProgram wrongConstructorOwner = frontend.mir;
  {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongConstructorOwner.functionInstances());
    const auto caller =
        std::find_if(functions.begin(), functions.end(),
                     [&](const lang::MirFunctionInstance &function) {
                       return function.id == directConstructorShape.function;
                     });
    lang::MirInstruction *consumer =
        caller == functions.end()
            ? nullptr
            : findMutableInstruction(caller->body,
                                     directConstructorShape.consumer);
    const lang::MirConstructorInstance *constructor =
        consumer != nullptr && consumer->constructorTarget
            ? wrongConstructorOwner.findConstructorInstance(
                  *consumer->constructorTarget)
            : nullptr;
    auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
        wrongConstructorOwner.classInstances());
    const auto owner =
        constructor == nullptr
            ? classes.end()
            : std::find_if(classes.begin(), classes.end(),
                           [&](const lang::MirClassInstance &candidate) {
                             return candidate.id == constructor->owner;
                           });
    expect(owner != classes.end(),
           "the direct constructor transport should retain its exact owner "
           "instance");
    if (owner != classes.end()) {
      owner->polymorphic = true;
    }
  }
  expectRejected(wrongConstructorOwner, directConstructorShape.function,
                 "a payload-enum CallInput targeting a polymorphic-owner "
                 "constructor must fail admission");

  const lang::MirProgram wrongDirectTransfer =
      forge(directShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *move =
            findMutableInstruction(body, directShape.move);
        lang::MirInstruction *call =
            findMutableInstruction(body, directShape.consumer);
        expect(move != nullptr && move->lifecycle.size() == 1 &&
                   call != nullptr,
               "the exact lambda argument should retain its move and call "
               "lifecycle records");
        if (move == nullptr || move->lifecycle.size() != 1 || call == nullptr) {
          return;
        }
        const lang::MirDropObligationId movedDrop =
            move->lifecycle.front().target;
        const auto transfer = std::find_if(
            call->lifecycle.begin(), call->lifecycle.end(),
            [&](const lang::MirLifecycleEvent &candidate) {
              return candidate.kind ==
                         lang::MirLifecycleEventKind::TransferOut &&
                     candidate.source == movedDrop;
            });
        expect(transfer != call->lifecycle.end(),
               "the exact lambda call should transfer the moved argument");
        if (transfer != call->lifecycle.end()) {
          transfer->kind = lang::MirLifecycleEventKind::Initialize;
        }
      });
  expectRejected(wrongDirectTransfer, directShape.function,
                 "an exact lambda argument without its unique TransferOut "
                 "edge must fail admission");

  const lang::MirProgram wrongDirectMoveLifecycle =
      forge(directShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *move =
            findMutableInstruction(body, directShape.move);
        expect(move != nullptr && move->lifecycle.size() == 1,
               "the direct exact-lambda move should retain one lifecycle "
               "edge");
        if (move != nullptr && move->lifecycle.size() == 1) {
          move->lifecycle.front().kind =
              lang::MirLifecycleEventKind::Initialize;
        }
      });
  expectRejected(wrongDirectMoveLifecycle, directShape.function,
                 "an exact lambda argument whose Move producer has the "
                 "wrong lifecycle kind must fail admission");

  const lang::MirProgram wrongDirectFreshTransfer =
      forge(directFreshShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *producer =
            findMutableInstruction(body, directFreshShape.producer);
        lang::MirInstruction *call =
            findMutableInstruction(body, directFreshShape.consumer);
        expect(producer != nullptr && call != nullptr,
               "the direct fresh lambda argument should retain its producer "
               "and call");
        if (producer == nullptr || call == nullptr) {
          return;
        }
        const auto initialization =
            std::find_if(producer->lifecycle.begin(), producer->lifecycle.end(),
                         [](const lang::MirLifecycleEvent &candidate) {
                           return candidate.kind ==
                                      lang::MirLifecycleEventKind::Initialize &&
                                  candidate.target != 0;
                         });
        expect(initialization != producer->lifecycle.end(),
               "the direct fresh lambda argument should arm one value drop");
        if (initialization == producer->lifecycle.end()) {
          return;
        }
        const auto transfer = std::find_if(
            call->lifecycle.begin(), call->lifecycle.end(),
            [&](const lang::MirLifecycleEvent &candidate) {
              return candidate.kind ==
                         lang::MirLifecycleEventKind::TransferOut &&
                     candidate.source == initialization->target;
            });
        expect(transfer != call->lifecycle.end(),
               "the direct fresh lambda call should consume that value drop");
        if (transfer != call->lifecycle.end()) {
          transfer->kind = lang::MirLifecycleEventKind::Initialize;
        }
      });
  expectRejected(wrongDirectFreshTransfer, directFreshShape.function,
                 "a direct fresh lambda argument without its producer-to-call "
                 "TransferOut pairing must fail admission");

  const lang::MirProgram wrongDirectFreshProducer =
      forge(directFreshShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *producer =
            findMutableInstruction(body, directFreshShape.producer);
        expect(producer != nullptr &&
                   producer->operation == lang::MirOperation::PayloadConstruct,
               "the direct fresh lambda argument should retain its exact "
               "PayloadConstruct producer");
        if (producer != nullptr) {
          producer->operation = lang::MirOperation::Identity;
        }
      });
  expectRejected(wrongDirectFreshProducer, directFreshShape.function,
                 "a direct fresh lambda argument produced by a different MIR "
                 "operation must fail admission");

  const lang::MirProgram wrongFreshReturnPlace =
      forge(directFreshReturnShape.function, [&](lang::MirBody &body) {
        const auto place = std::find_if(
            body.places.begin(), body.places.end(),
            [&](const lang::MirPlace &candidate) {
              return candidate.root == lang::MirPlaceRootKind::Value &&
                     candidate.value == directFreshReturnShape.value;
            });
        expect(place != body.places.end(),
               "the direct fresh return should retain its value-rooted "
               "result place");
        if (place != body.places.end()) {
          place->root = lang::MirPlaceRootKind::Temporary;
          place->value = 0;
        }
      });
  expectRejected(wrongFreshReturnPlace, directFreshReturnShape.function,
                 "a direct fresh return detached from its exact value-rooted "
                 "result place must fail admission");

  const lang::MirProgram wrongFreshReturnDrop =
      forge(directFreshReturnShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *producer =
            findMutableInstruction(body, directFreshReturnShape.producer);
        if (producer == nullptr) {
          return;
        }
        const auto initialization =
            std::find_if(producer->lifecycle.begin(), producer->lifecycle.end(),
                         [](const lang::MirLifecycleEvent &candidate) {
                           return candidate.kind ==
                                      lang::MirLifecycleEventKind::Initialize &&
                                  candidate.target != 0;
                         });
        expect(initialization != producer->lifecycle.end(),
               "the direct fresh return should arm one result drop");
        if (initialization == producer->lifecycle.end()) {
          return;
        }
        const auto drop = std::find_if(
            body.dropObligations.begin(), body.dropObligations.end(),
            [&](const lang::MirDropObligation &candidate) {
              return candidate.id == initialization->target;
            });
        expect(drop != body.dropObligations.end(),
               "the direct fresh return should retain its result drop row");
        if (drop != body.dropObligations.end()) {
          drop->dropType.requiresActiveCleanup = false;
        }
      });
  expectRejected(wrongFreshReturnDrop, directFreshReturnShape.function,
                 "a direct fresh return whose result drop loses active "
                 "cleanup must fail admission");

  const lang::MirProgram wrongFreshReturnTransfer =
      forge(directFreshReturnShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *producer =
            findMutableInstruction(body, directFreshReturnShape.producer);
        if (producer == nullptr) {
          return;
        }
        const auto initialization =
            std::find_if(producer->lifecycle.begin(), producer->lifecycle.end(),
                         [](const lang::MirLifecycleEvent &candidate) {
                           return candidate.kind ==
                                      lang::MirLifecycleEventKind::Initialize &&
                                  candidate.target != 0;
                         });
        if (initialization == producer->lifecycle.end()) {
          return;
        }
        lang::MirLifecycleEvent *transfer = nullptr;
        for (lang::MirBlock &block : body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            const auto found = std::find_if(
                instruction.lifecycle.begin(), instruction.lifecycle.end(),
                [&](const lang::MirLifecycleEvent &candidate) {
                  return candidate.kind ==
                             lang::MirLifecycleEventKind::TransferOut &&
                         candidate.source == initialization->target;
                });
            if (found != instruction.lifecycle.end()) {
              transfer = &*found;
            }
          }
        }
        expect(transfer != nullptr,
               "the direct fresh return should transfer its result drop");
        if (transfer != nullptr) {
          transfer->target = transfer->source;
        }
      });
  expectRejected(wrongFreshReturnTransfer, directFreshReturnShape.function,
                 "a direct fresh return without its exact out-result "
                 "TransferOut pairing must fail admission");

  const lang::MirProgram wrongFreshReturnUse =
      forge(directFreshReturnShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *producer =
            findMutableInstruction(body, directFreshReturnShape.producer);
        if (producer == nullptr || producer->operands.empty()) {
          return;
        }
        const auto returnBlock =
            std::find_if(body.blocks.begin(), body.blocks.end(),
                         [&](const lang::MirBlock &candidate) {
                           return candidate.terminator.kind ==
                                      lang::MirTerminatorKind::Return &&
                                  candidate.terminator.value &&
                                  candidate.terminator.value->kind ==
                                      lang::MirOperandKind::Value &&
                                  candidate.terminator.value->value ==
                                      directFreshReturnShape.value;
                         });
        expect(returnBlock != body.blocks.end(),
               "the direct fresh result should retain its exact Return use");
        if (returnBlock != body.blocks.end()) {
          returnBlock->terminator.value = producer->operands.front();
        }
      });
  expectRejected(wrongFreshReturnUse, directFreshReturnShape.function,
                 "a direct fresh payload construction returned through a "
                 "different value must fail admission");
}

void testProgramPlanValidationIsOperationScoped() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-operation-scoped-validation.gti", R"(
class Base {
  int32_t value = 3;
public:
  Base() {}
};
class Derived : public Base {
  bool enabled = true;
public:
  Derived() : Base() {}
};
struct Other { int32_t value = 7; };
int32_t identity(int32_t value) { return value; }
int main() {
  Derived derived{};
  Other other{};
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "operation-scoped validation should have a valid multi-owner fixture");
  if (!frontend.canGenerateCode()) {
    return;
  }
  lang::MirProgram program = frontend.mir;
  const lang::CppMirBodyEmissionMapRows rows = buildRows(frontend, program);
  lang::CppMirBodyEmissionMap map(rows);
  lang::CppMirBodyEmissionMetrics metrics;
  const lang::CppMirBodyEmitter emitter(program, map, &metrics);
  std::vector<lang::MirBodyAddress> fields;
  for (const lang::MirClassInstance &owner : program.classInstances()) {
    if (!owner.declaredFields.empty()) {
      fields.push_back(
          {.kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id});
    }
  }
  expect(fields.size() >= 3,
         "the regression should exercise several independent field schedules");
  if (fields.size() < 3) {
    return;
  }
  bool nativeBase = false;
  for (const lang::MirConstructorInstance &constructor :
       program.constructorInstances()) {
    const lang::MirClassInstance *owner =
        program.findClassInstance(constructor.owner);
    if (owner != nullptr && !owner->bases.empty()) {
      nativeBase |=
          emitter.supportsNativeContainedBaseConstruction(constructor.id);
    }
  }
  expect(nativeBase,
         "the fixture should exercise the nested native-base vocabulary proof");

  metrics.programVerifications = 0;
  const lang::CppMirProgramEmissionPlan plan = emitter.buildProgramPlan();
  expect(plan.ready() && metrics.programVerifications == 1,
         "one complete program plan should verify MIR exactly once, including "
         "all initializer schedules and nested base vocabulary probes");
  for (const lang::MirBodyAddress address : fields) {
    const lang::CppMirBodyEmissionPlan *body = plan.find(address);
    expect(body != nullptr && body->ready(),
           "every field schedule should have an admitted child plan");
    const std::size_t before = metrics.programVerifications;
    expect(emitter.initializerSchedule(address).supported &&
               metrics.programVerifications == before + 1,
           "each standalone initializer request should validate MIR afresh");
    expect(emitter.analyze(address).ready() &&
               metrics.programVerifications == before + 2,
           "each standalone analysis should validate MIR afresh");
  }

  const auto expectRejected = [&](std::string_view detail) {
    const std::size_t before = metrics.programVerifications;
    const lang::CppMirProgramEmissionPlan rejected = emitter.buildProgramPlan();
    expect(!rejected.ready() &&
               std::none_of(rejected.bodies().begin(), rejected.bodies().end(),
                            [](const lang::CppMirBodyEmissionPlan &body) {
                              return body.ready();
                            }) &&
               metrics.programVerifications == before + 1,
           detail);
    expect(!emitter.initializerSchedule(fields.front()).supported &&
               metrics.programVerifications == before + 2,
           "a standalone initializer cannot reuse prior successful validation");
    expect(!emitter.analyze(fields.front()).ready() &&
               metrics.programVerifications == before + 3,
           "standalone analysis cannot reuse prior successful validation");
  };

  const lang::MirFunctionInstance *function = firstSourceFunction(program);
  expect(function != nullptr, "the fixture should retain a source function");
  if (function == nullptr) {
    return;
  }
  lang::MirBody *body = lang::findMirBody(
      program, {.kind = lang::MirBodyKind::Function, .owner = function->id});
  expect(body != nullptr && !body->blocks.empty(),
         "the unrelated function should provide an independent MIR mutation");
  if (body == nullptr || body->blocks.empty()) {
    return;
  }
  body->blocks.front().terminator.kind = lang::MirTerminatorKind::None;
  expectRejected("invalid global MIR after successful admission must reject "
                 "the whole plan without any independently Ready child");

  program = frontend.mir;
  const std::size_t beforeRestore = metrics.programVerifications;
  expect(emitter.buildProgramPlan().ready() &&
             metrics.programVerifications == beforeRestore + 1,
         "restoring MIR should permit fresh admission on the same emitter");
  lang::CppMirBodyEmissionMapRows duplicate = rows;
  expect(!duplicate.types.empty(),
         "the fixture should supply a type representation to duplicate");
  if (duplicate.types.empty()) {
    return;
  }
  duplicate.types.push_back(duplicate.types.front());
  map = lang::CppMirBodyEmissionMap(std::move(duplicate));
  expectRejected("a changed incoherent representation map must not reuse "
                 "successful admission or leave any Ready child plan");
}

void testImmutableBodyPlanOutlivesAdmissionInputs() {
  const lang::CppMirBodyEmissionPlan plan = [] {
    const lang::FrontendResult frontend =
        analyze("cpp-mir-body-immutable-plan.gti",
                "int32_t identity(int32_t value) { return value; }");
    expect(frontend.canGenerateCode(),
           "the immutable-plan fixture should pass the frontend");
    const lang::MirFunctionInstance *function =
        firstSourceFunction(frontend.mir);
    expect(function != nullptr,
           "the immutable-plan fixture should lower one source function");
    if (function == nullptr) {
      return lang::CppMirBodyEmissionPlan{};
    }
    const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
    return lang::CppMirBodyEmitter(frontend.mir, map)
        .buildPlan(
            {.kind = lang::MirBodyKind::Function, .owner = function->id});
  }();

  expect(plan.ready() && plan.supportsPlainText(),
         "a complete scalar body should produce an admitted immutable plan");
  const lang::CppMirBodyEmissionText first = lang::CppMirBodyTextWriter::write(
      plan, lang::CppMirBodyTextForm::Plain, "immutable-plan-v1", 1);
  const lang::CppMirBodyEmissionText second = lang::CppMirBodyTextWriter::write(
      plan, lang::CppMirBodyTextForm::Plain, "immutable-plan-v1", 1);
  expect(first.emitted() && first.text == second.text &&
             first.text.find("immutable-plan-v1") != std::string::npos &&
             first.text.find(lang::cppMirBodyEmissionFamilyPlaceholder) ==
                 std::string::npos,
         "the formatting-only writer should remain deterministic after the "
         "MIR program and representation map have been destroyed");
}

void testHostedStartupOwnsRemainingAuthorityGap() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-entry.gti", "int main() { return 0; }");
  expect(frontend.canGenerateCode(),
         "the hosted-entry fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the entry fixture should retain hosted-entry metadata");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
  const lang::CppMirBodyEmissionAnalysis entryAnalysis =
      lang::CppMirBodyEmitter(frontend.mir, map)
          .analyze({.kind = lang::MirBodyKind::Function, .owner = entry->id});
  const lang::CppMirBodyEmissionAnalysis startupAnalysis =
      lang::CppMirBodyEmitter(frontend.mir, map)
          .analyze(
              {.kind = lang::MirBodyKind::HostedStartup, .owner = entry->id});
  expect(
      entryAnalysis.ready() &&
          !hasIssue(entryAnalysis,
                    lang::CppMirBodyEmissionIssueKind::MissingHostedStartupMir),
      "the source entry body should no longer claim ownership of generated "
      "hosted startup");
  expect(
      startupAnalysis.ready() &&
          !hasIssue(startupAnalysis,
                    lang::CppMirBodyEmissionIssueKind::MissingHostedStartupMir),
      "the generated failure-free startup body carries its complete "
      "verified two-operation schedule authority");

  const lang::FrontendResult owned =
      analyzeWithStandardLibrary("cpp-mir-body-owned-entry.gti", R"(
#include <std/string>
#include <std/vector>
int main(int argc, std::vector<std::string> argv) { return argc; }
)");
  expect(owned.canGenerateCode(),
         "the owned hosted-entry fixture should pass the frontend");
  if (!owned.canGenerateCode()) {
    return;
  }
  const auto ownedEntry = std::find_if(
      owned.mir.functionInstances().begin(),
      owned.mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.entryKind == lang::ProgramEntryKind::OwnedArguments;
      });
  expect(ownedEntry != owned.mir.functionInstances().end(),
         "the owned entry fixture should retain its generated startup plan");
  if (ownedEntry == owned.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionAnalysis ownedStartup =
      lang::CppMirBodyEmitter(
          owned.mir, lang::CppMirBodyEmissionMap(completeRows(owned.mir)))
          .analyze({.kind = lang::MirBodyKind::HostedStartup,
                    .owner = ownedEntry->id});
  expect(ownedStartup.ready() &&
             !hasIssue(
                 ownedStartup,
                 lang::CppMirBodyEmissionIssueKind::MissingFailureCleanupMir) &&
             !hasIssue(ownedStartup, lang::CppMirBodyEmissionIssueKind::
                                         MissingPartialConstructionRollbackMir),
         "the exact generated owned startup schedule is accepted: the verified "
         "marshaling plan carries its own failure-cleanup envelope and the "
         "emitted argc/argv main is its complete emission");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(owned.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(owned, owned.mir, owned.mir, optimizations);
  const auto marker = [&](std::string_view kind, std::size_t owner) {
    return artifact.contents.find(std::string(kind) + " " +
                                  std::to_string(owner) + "\n") !=
           std::string::npos;
  };
  const std::optional<lang::MirHostedStartupPlan> &ownedPlan =
      owned.mir.hostedStartupPlan();
  expect(ownedPlan && marker("function-instance", ownedPlan->entry) &&
             marker("function-instance", ownedPlan->appendFunction),
         "owned startup should publish its entry and class-valued append "
         "targets through verified MIR bodies");
  if (ownedPlan) {
    expect(artifact.contents.find("mir_failure_constructor_tag_v1<" +
                                  std::to_string(ownedPlan->vectorConstructor) +
                                  ">{") != std::string::npos &&
               artifact.contents.find(
                   "mir_failure_constructor_tag_v1<" +
                   std::to_string(ownedPlan->stringConstructor) + ">{") !=
                   std::string::npos &&
               artifact.contents.find(
                   "push_back__gti_mir_failure(std::move(__gti_argument)") !=
                   std::string::npos &&
               artifact.contents.find(
                   "::__gti_program::__gti_entry__gti_mir_failure(") !=
                   std::string::npos,
           "the native adapter should invoke the exact transformed vector, "
           "string, append, and entry surfaces selected by hosted MIR");
  }
}

void testVerifiedProgramInitializationPlanClassification() {
  const lang::FrontendResult dataOnly =
      analyze("cpp-mir-body-data-only-module.gti", R"(
constexpr int32_t seed = 4;
mut int32_t zeroed;
)");
  expect(dataOnly.canGenerateCode(),
         "the data-only Module classifier fixture should pass the frontend");
  if (!dataOnly.canGenerateCode()) {
    return;
  }
  lang::CppMirBodyEmissionMapRows dataRows = completeRows(dataOnly.mir);
  dataRows.bodies.erase(
      std::remove_if(dataRows.bodies.begin(), dataRows.bodies.end(),
                     [](const lang::CppMirBodyNameRepresentation &row) {
                       return row.address ==
                              lang::MirBodyAddress{
                                  .kind = lang::MirBodyKind::Module};
                     }),
      dataRows.bodies.end());
  dataRows.capabilities.erase(
      std::remove_if(
          dataRows.capabilities.begin(), dataRows.capabilities.end(),
          [](const lang::CppMirEmissionCapabilityRepresentation &row) {
            return row.kind ==
                   lang::CppMirEmissionCapabilityKind::ProgramInitialization;
          }),
      dataRows.capabilities.end());
  const lang::CppMirBodyEmissionAnalysis dataAnalysis =
      lang::CppMirBodyEmitter(dataOnly.mir,
                              lang::CppMirBodyEmissionMap(std::move(dataRows)))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(
      dataAnalysis.ready() && !dataOnly.mir.module().places.empty() &&
          !hasIssue(
              dataAnalysis,
              lang::CppMirBodyEmissionIssueKind::MissingBodyRepresentation) &&
          !hasIssue(dataAnalysis, lang::CppMirBodyEmissionIssueKind::
                                      MissingCapabilityRepresentation) &&
          !hasIssue(dataAnalysis, lang::CppMirBodyEmissionIssueKind::
                                      MissingProgramInitializationMir),
      "data-only tagged storage stages should require neither an "
      "executable Module name nor a runtime initialization adapter");

  const lang::FrontendResult dynamic =
      analyze("cpp-mir-body-dynamic-module.gti", R"(
int32_t initial_value() { return 5; }
class Registry {
public:
  static mut int32_t value = initial_value();
};
)");
  expect(dynamic.canGenerateCode(),
         "the dynamic Module classifier fixture should pass the frontend");
  if (!dynamic.canGenerateCode()) {
    return;
  }
  const auto staticStep = std::find_if(
      dynamic.mir.programInitializationPlan().steps.begin(),
      dynamic.mir.programInitializationPlan().steps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.storageKind == lang::ProgramStorageKind::StaticField &&
               step.role == lang::ProgramInitializationStepRole::Initializer;
      });
  expect(staticStep != dynamic.mir.programInitializationPlan().steps.end(),
         "the dynamic classifier fixture should retain one planned static "
         "initializer");

  lang::CppMirBodyEmissionMapRows dynamicRows = completeRows(dynamic.mir);
  const bool exactStaticStorage =
      staticStep != dynamic.mir.programInitializationPlan().steps.end() &&
      std::any_of(dynamicRows.symbols.begin(), dynamicRows.symbols.end(),
                  [&](const lang::CppMirSymbolRepresentation &row) {
                    return row.kind ==
                               lang::CppMirSymbolRepresentationKind::Storage &&
                           row.owner == staticStep->ownerClass &&
                           row.symbol == staticStep->symbol;
                  });
  const lang::CppMirBodyEmissionAnalysis dynamicAnalysis =
      lang::CppMirBodyEmitter(dynamic.mir,
                              lang::CppMirBodyEmissionMap(dynamicRows))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(
      exactStaticStorage &&
          !hasIssue(dynamicAnalysis, lang::CppMirBodyEmissionIssueKind::
                                         MissingProgramInitializationMir) &&
          !hasIssue(
              dynamicAnalysis,
              lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation),
      "the verified Module plan should supply exact program-step and "
      "static-storage owner authority to the private classifier");

  dynamicRows.capabilities.erase(
      std::remove_if(
          dynamicRows.capabilities.begin(), dynamicRows.capabilities.end(),
          [](const lang::CppMirEmissionCapabilityRepresentation &row) {
            return row.kind ==
                   lang::CppMirEmissionCapabilityKind::ProgramInitialization;
          }),
      dynamicRows.capabilities.end());
  const lang::CppMirBodyEmissionAnalysis missingAdapter =
      lang::CppMirBodyEmitter(
          dynamic.mir, lang::CppMirBodyEmissionMap(std::move(dynamicRows)))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(missingAdapter.readiness ==
                 lang::CppMirBodyEmissionReadiness::MissingRepresentation &&
             hasIssue(missingAdapter, lang::CppMirBodyEmissionIssueKind::
                                          MissingCapabilityRepresentation) &&
             !hasIssue(missingAdapter, lang::CppMirBodyEmissionIssueKind::
                                           MissingProgramInitializationMir),
         "a verified dynamic plan should expose only the missing copied "
         "runtime adapter, not a fictitious MIR authority gap");

  lang::MirProgram stalePlan = dataOnly.mir;
  auto &steps = const_cast<lang::MirProgramInitializationPlan &>(
                    stalePlan.programInitializationPlan())
                    .steps;
  if (!steps.empty()) {
    steps.front().storageInitialization = 0;
  }
  const lang::CppMirBodyEmissionAnalysis incoherent =
      lang::CppMirBodyEmitter(
          stalePlan, lang::CppMirBodyEmissionMap(completeRows(stalePlan)))
          .analyze({.kind = lang::MirBodyKind::Module});
  expect(incoherent.readiness ==
                 lang::CppMirBodyEmissionReadiness::Incoherent &&
             hasIssue(incoherent,
                      lang::CppMirBodyEmissionIssueKind::InvalidMirProgram),
         "a stale program plan must fail before classifier readiness");
}

void testSafeIndexUsesBoundsNotRawMemory() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-safe-index.gti", R"(
int32_t at(int32_t values[2], uint64_t index) { return values[index]; }
)");
  expect(frontend.canGenerateCode(),
         "the safe fixed-array index fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  expect(function != nullptr, "the index fixture should lower one function");
  if (function == nullptr) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::CppMirBodyEmissionAnalysis noBounds =
      lang::CppMirBodyEmitter(
          frontend.mir,
          lang::CppMirBodyEmissionMap(completeRows(
              frontend.mir, lang::CppMirEmissionCapabilityKind::Bounds)))
          .analyze(address);
  const lang::CppMirBodyEmissionAnalysis noRawMemory =
      lang::CppMirBodyEmitter(
          frontend.mir,
          lang::CppMirBodyEmissionMap(completeRows(
              frontend.mir, lang::CppMirEmissionCapabilityKind::RawMemory)))
          .analyze(address);
  expect(
      hasIssue(
          noBounds,
          lang::CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation) &&
          !hasIssue(noRawMemory, lang::CppMirBodyEmissionIssueKind::
                                     MissingCapabilityRepresentation),
      "safe Index should require a bounds/failure adapter, not raw-memory "
      "authority");
}

void testUnsafeRawPointerVocabulary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-raw-pointer.gti", R"(
int64_t raw_distance() {
  mut int32_t value = 1;
  mut int32_t* pointer = nullptr;
  mut int64_t distance = 0;
  unsafe {
    pointer = &value;
    pointer[0] = 4;
    *(pointer + 0) = 5;
    distance = (pointer + 0) - pointer;
  }
  return distance;
}
)");
  expect(frontend.canGenerateCode(),
         "the raw-pointer text fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto functionIt = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &candidate) {
        return std::any_of(
            candidate.body.blocks.begin(), candidate.body.blocks.end(),
            [](const lang::MirBlock &block) {
              return std::any_of(block.instructions.begin(),
                                 block.instructions.end(),
                                 [](const lang::MirInstruction &instruction) {
                                   return instruction.operation ==
                                          lang::MirOperation::AddressOf;
                                 });
            });
      });
  const lang::MirFunctionInstance *function =
      functionIt == frontend.mir.functionInstances().end() ? nullptr
                                                           : &*functionIt;
  expect(function != nullptr,
         "the raw-pointer fixture should lower one source function");
  if (function == nullptr) {
    return;
  }
  expect(!function->mayRaiseDefinedFailure,
         "verified raw-pointer operations should not invent a GTI "
         "defined-failure effect");
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::CppMirBodyEmissionAnalysis noRawMemory =
      lang::CppMirBodyEmitter(
          frontend.mir,
          lang::CppMirBodyEmissionMap(completeRows(
              frontend.mir, lang::CppMirEmissionCapabilityKind::RawMemory)))
          .analyze(address);
  expect(
      hasIssue(
          noRawMemory,
          lang::CppMirBodyEmissionIssueKind::MissingCapabilityRepresentation),
      "unsafe raw-pointer MIR should require its sealed representation "
      "capability");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyze(address).ready() && emitter.supportsBodyText(address),
         "the production snapshot should admit the exact raw-pointer MIR "
         "vocabulary");
  if (!emitter.supportsBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitBodyText(address, "raw-pointer-test-v0", 1);
  expect(text.text.find(" = (&__gti_mir_p_") != std::string::npos &&
             text.text.find("[__gti_mir_v_") != std::string::npos &&
             text.text.find("(*__gti_mir_v_") != std::string::npos &&
             text.text.find(" + ") != std::string::npos &&
             text.text.find(" - ") != std::string::npos,
         "raw-pointer MIR text should spell address formation, unchecked "
         "index/dereference lvalues, and pointer arithmetic directly");
}

void testConcreteGenericFieldOwner() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-generic-field.gti", R"(
class Box<T> {
  T value;
public:
  Box(T initial) : value(initial) {}
  T read() { return this.value; }
};

int32_t use_int() {
  Box<int32_t> value = Box<int32_t>(1);
  return value.read();
}

uint8_t use_byte() {
  Box<uint8_t> value = Box<uint8_t>(uint8_t(2));
  return value.read();
}
)");
  expect(frontend.canGenerateCode(),
         "the two-instantiation field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  std::vector<const lang::MirClassInstance *> boxes;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.declaredFields.size() == 1) {
      boxes.push_back(&instance);
    }
  }
  expect(boxes.size() == 2 &&
             boxes.front()->declaration == boxes.back()->declaration &&
             boxes.front()->declaredFields.front().symbol ==
                 boxes.back()->declaredFields.front().symbol,
         "the fixture should retain two concrete owners for one source field");
  if (boxes.size() != 2) {
    return;
  }

  const auto methodFor = [&](lang::HirClassInstanceId owner) {
    return std::find_if(
        frontend.mir.functionInstances().begin(),
        frontend.mir.functionInstances().end(),
        [&](const lang::MirFunctionInstance &function) {
          return function.owner == owner &&
                 std::any_of(
                     function.body.places.begin(), function.body.places.end(),
                     [](const lang::MirPlace &place) {
                       return std::any_of(
                           place.projections.begin(), place.projections.end(),
                           [](const lang::MirPlaceProjection &projection) {
                             return projection.kind ==
                                    lang::MirProjectionKind::Field;
                           });
                     });
        });
  };
  const auto firstMethod = methodFor(boxes.front()->id);
  const auto secondMethod = methodFor(boxes.back()->id);
  expect(firstMethod != frontend.mir.functionInstances().end() &&
             secondMethod != frontend.mir.functionInstances().end(),
         "both concrete class instances should have field-reading MIR bodies");
  if (firstMethod == frontend.mir.functionInstances().end() ||
      secondMethod == frontend.mir.functionInstances().end()) {
    return;
  }

  lang::CppMirBodyEmissionMapRows complete = completeRows(frontend.mir);
  const lang::CppMirBodyEmissionMap completeMap(complete);
  const lang::CppMirBodyEmissionAnalysis first =
      lang::CppMirBodyEmitter(frontend.mir, completeMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = firstMethod->id});
  const lang::CppMirBodyEmissionAnalysis second =
      lang::CppMirBodyEmitter(frontend.mir, completeMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = secondMethod->id});
  expect(
      !hasIssue(
          first,
          lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation) &&
          !hasIssue(
              second,
              lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation),
      "field lookup should use each projected place's concrete owner");

  const lang::HirClassInstanceId removedOwner = boxes.front()->id;
  const auto erased = std::remove_if(
      complete.symbols.begin(), complete.symbols.end(),
      [&](const lang::CppMirSymbolRepresentation &row) {
        return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
               row.owner == removedOwner;
      });
  complete.symbols.erase(erased, complete.symbols.end());
  const lang::CppMirBodyEmissionMap missingOwnerMap(std::move(complete));
  const lang::CppMirBodyEmissionAnalysis missingOwner =
      lang::CppMirBodyEmitter(frontend.mir, missingOwnerMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = firstMethod->id});
  const lang::CppMirBodyEmissionAnalysis retainedOwner =
      lang::CppMirBodyEmitter(frontend.mir, missingOwnerMap)
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = secondMethod->id});
  expect(
      hasIssue(
          missingOwner,
          lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation) &&
          !hasIssue(
              retainedOwner,
              lang::CppMirBodyEmissionIssueKind::MissingSymbolRepresentation),
      "removing one generic field owner must not alias the same source "
      "SymbolId from another instantiation");
}

void testAmbiguousStaticStorageFailsClosed() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-storage-owner.gti", R"(
mut int32_t state = 1;
int32_t read_state() { return state; }
)");
  expect(frontend.canGenerateCode(),
         "the global-storage owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *function = firstSourceFunction(frontend.mir);
  expect(function != nullptr, "the storage fixture should lower one function");
  if (function == nullptr) {
    return;
  }
  const auto place =
      std::find_if(function->body.places.begin(), function->body.places.end(),
                   [](const lang::MirPlace &candidate) {
                     return candidate.root == lang::MirPlaceRootKind::Symbol &&
                            candidate.capture == 0;
                   });
  expect(place != function->body.places.end(),
         "the storage fixture should retain a symbol-root place");
  if (place == function->body.places.end()) {
    return;
  }

  lang::CppMirBodyEmissionMapRows rows = completeRows(frontend.mir);
  rows.symbols.erase(
      std::remove_if(
          rows.symbols.begin(), rows.symbols.end(),
          [&](const lang::CppMirSymbolRepresentation &row) {
            return row.kind == lang::CppMirSymbolRepresentationKind::Storage &&
                   row.symbol == place->symbol;
          }),
      rows.symbols.end());
  rows.symbols.push_back({.kind = lang::CppMirSymbolRepresentationKind::Storage,
                          .owner = 101,
                          .symbol = place->symbol,
                          .type = place->type,
                          .spelling = "first_static_instance"});
  rows.symbols.push_back({.kind = lang::CppMirSymbolRepresentationKind::Storage,
                          .owner = 202,
                          .symbol = place->symbol,
                          .type = place->type,
                          .spelling = "second_static_instance"});
  const lang::CppMirBodyEmissionAnalysis analysis =
      lang::CppMirBodyEmitter(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(rows)))
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = function->id});
  expect(
      hasIssue(
          analysis,
          lang::CppMirBodyEmissionIssueKind::MissingProgramInitializationMir),
      "same-Symbol static rows from multiple owners must stay ambiguous "
      "without concrete initialization/storage authority");
}

void testGlobalClassStorageMemberProjection() {
  const auto passive = analyze("cpp-mir-body-passive-class-storage.gti", R"(
class thing {
public:
  thing() {}
  mut int32_t value = 0;
};
mut thing g = thing();
int main() { g.value = 37; return g.value - 37; }
)");
  expect(passive.canGenerateCode(), "the passive class global remains valid");
  if (passive.canGenerateCode()) {
    const auto passiveRows = buildRows(passive, passive.mir);
    const lang::CppMirBodyEmissionMap passiveMap(passiveRows);
    const lang::CppMirBodyEmitter passiveEmitter(passive.mir, passiveMap);
    expect(std::any_of(
               passiveRows.symbols.begin(), passiveRows.symbols.end(),
               [](const auto &row) {
                 return row.kind ==
                            lang::CppMirSymbolRepresentationKind::Storage &&
                        row.type.kind == lang::SemanticType::Class &&
                        !row.runtimeLifetimeSlot;
               }) &&
               passiveEmitter
                   .initializerSchedule(
                       {.kind = lang::MirBodyKind::Module, .owner = 0})
                   .supported &&
               !gti_test::emitCppText(passive).empty(),
           "a passive class global should use its complete initializer "
           "schedule without an invented runtime slot");
  }
  // Keep this initializer genuinely fallible: an empty constructor can now
  // use the passive schedule and correctly needs no runtime lifetime slot.
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-global-class-storage.gti", R"(
class thing {
public:
  thing(int32_t divisor) : value(int32_t(0) / divisor) {}
  mut int32_t value = 0;
};

mut thing g = thing(int32_t(1));

int32_t read_global() { return g.value; }
void write_global(int32_t value) { g.value = value; }

int main() {
  write_global(int32_t(37));
  return read_global() == 37 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the global class-storage fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::CppMirBodyEmissionMapRows rows = buildRows(frontend, frontend.mir);
  const auto slot = std::find_if(
      rows.symbols.begin(), rows.symbols.end(),
      [](const lang::CppMirSymbolRepresentation &row) {
        return row.kind == lang::CppMirSymbolRepresentationKind::Storage &&
               row.type.kind == lang::SemanticType::Class &&
               row.runtimeLifetimeSlot;
      });
  expect(slot != rows.symbols.end(),
         "runtime-initialized class program storage should carry one exact "
         "lifetime-slot representation row");
  if (slot == rows.symbols.end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress module{.kind = lang::MirBodyKind::Module,
                                    .owner = 0};
  expect(emitter.analyze(module).ready() &&
             emitter.supportsFailureBodyText(module),
         "Module/0 should admit the class global's runtime construction "
         "through its copied lifetime-slot row");

  std::size_t projectedBodies = 0;
  bool projectedBodiesSupported = true;
  bool projectedBodiesUseSlotValue = true;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const bool projected = std::any_of(
        function.body.places.begin(), function.body.places.end(),
        [](const lang::MirPlace &place) {
          return place.root == lang::MirPlaceRootKind::Symbol &&
                 !place.projections.empty() &&
                 std::all_of(place.projections.begin(), place.projections.end(),
                             [](const lang::MirPlaceProjection &projection) {
                               return projection.kind ==
                                      lang::MirProjectionKind::Field;
                             });
        });
    if (!projected) {
      continue;
    }
    ++projectedBodies;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const bool supported = function.mayRaiseDefinedFailure
                               ? emitter.supportsFailureBodyText(address)
                               : emitter.supportsBodyText(address);
    projectedBodiesSupported = projectedBodiesSupported && supported;
    if (!supported) {
      continue;
    }
    const lang::CppMirBodyEmissionText text =
        function.mayRaiseDefinedFailure
            ? emitter.emitFailureBodyText(
                  address, "global-class-storage-regression-v1", 1)
            : emitter.emitBodyText(address,
                                   "global-class-storage-regression-v1", 1);
    projectedBodiesUseSlotValue =
        projectedBodiesUseSlotValue && text.emitted() &&
        text.text.find(".get().value") != std::string::npos;
  }
  expect(projectedBodies == 2 && projectedBodiesSupported &&
             projectedBodiesUseSlotValue,
         "global class field reads and writes should both emit through the "
         "engaged lifetime-slot value");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find(
             "mir_lifetime_slot<::__gti_program::thing, true> g") !=
                 std::string::npos &&
             artifact.contents.find("auto &__gti_mir_p_1 = "
                                    "::__gti_program::g;") !=
                 std::string::npos &&
             artifact.contents.find("::__gti_program::g.get().value") !=
                 std::string::npos,
         "the complete backend should declare, construct, and project the "
         "same class-global lifetime-slot representation");

  slot->runtimeLifetimeSlot = false;
  const lang::CppMirBodyEmissionMap missingSlotMap(std::move(rows));
  expect(!lang::CppMirBodyEmitter(frontend.mir, missingSlotMap)
              .supportsFailureBodyText(module),
         "removing the copied lifetime-slot fact must fail Module/0 text "
         "admission instead of emitting an undeclared slot carrier");
}

void testConcreteClassStaticStorage() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-class-static-storage.gti", R"(
class Counter {
public:
  static mut int32_t value = 3;
  static int32_t read() { return value; }
};

int32_t read_counter() { return Counter::value; }
)");
  expect(frontend.canGenerateCode(),
         "the concrete class-static storage fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t supported = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const bool readsStatic =
        std::any_of(function.body.places.begin(), function.body.places.end(),
                    [](const lang::MirPlace &place) {
                      return place.root == lang::MirPlaceRootKind::Symbol &&
                             place.capture == 0;
                    });
    if (!readsStatic) {
      continue;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const bool spellable =
        emitter.analyze(address).ready() && emitter.supportsBodyText(address);
    expect(spellable,
           "a unique class-static storage row should be spellable from its "
           "member and an unrelated concrete function");
    if (!spellable) {
      continue;
    }
    const lang::CppMirBodyEmissionText text =
        emitter.emitBodyText(address, "class-static-storage-test-v0", 1);
    expect(text.emitted() &&
               text.text.find("Counter::value") != std::string::npos,
           "class-static storage text should use its owner-qualified row");
    ++supported;
  }
  expect(supported == 2,
         "the class-static fixture should admit both concrete reader bodies");
}

void testDischargedStorageReadAnalysis() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-discharged-read.gti", R"(
#include <std/vector>

int main() {
  mut std::vector<int> values = std::vector<int>();
  values.push_back(7);
  return values.front();
}
)");
  expect(frontend.canGenerateCode(),
         "the discharged-read fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      pipeline.run({.hir = frontend.hir,
                    .mir = frontend.mir,
                    .level = lang::OptimizationLevel::O3,
                    .compatibility = &compatibility});
  expect(optimized.valid(),
         "the discharged-read fixture should produce coherent O3 MIR");
  if (!optimized.valid()) {
    return;
  }
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
      optimized.report.transformations);
  const lang::CppMirBodyEmissionMap map(
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(optimized.mir, map);
  std::size_t dischargedInvokes = 0;
  bool exactUncheckedEmission = true;
  for (const lang::MirFunctionInstance &instance :
       optimized.mir.functionInstances()) {
    for (const lang::MirBlock &block : instance.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call ||
            (instruction.intrinsic != lang::IntrinsicKind::PrefixStorageRead &&
             instruction.intrinsic !=
                 lang::IntrinsicKind::PrefixStorageReadMut) ||
            !lang::allMirLocalFailuresDischarged(instruction) ||
            block.terminator.kind != lang::MirTerminatorKind::Invoke ||
            block.terminator.invokeInstruction != instruction.id) {
          continue;
        }
        ++dischargedInvokes;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = instance.id};
        const lang::CppMirBodyEmissionText text =
            instance.mayRaiseDefinedFailure
                ? emitter.emitFailureBodyText(
                      address, "discharged-storage-read-regression-v1", 1)
                : emitter.emitBodyText(
                      address, "discharged-storage-read-regression-v1", 1);
        const std::string status =
            "__gti_mir_failure_status_" + std::to_string(instruction.id);
        const std::string checkedHelper =
            instruction.intrinsic == lang::IntrinsicKind::PrefixStorageRead
                ? "::gti_internal::backend::mir_prefix_read_v1"
                : "::gti_internal::backend::mir_prefix_read_mut_v1";
        const std::string uncheckedHelper =
            instruction.intrinsic == lang::IntrinsicKind::PrefixStorageRead
                ? "::gti_internal::backend::prefix_storage_read_unchecked"
                : "::gti_internal::backend::prefix_storage_read_mut_unchecked";
        const bool exact =
            text.emitted() &&
            text.text.find(uncheckedHelper) != std::string::npos &&
            text.text.find(checkedHelper) == std::string::npos &&
            text.text.find(status) == std::string::npos;
        if (!exact) {
          std::cerr << "discharged checked prefix read " << instruction.id
                    << " did not emit solely through its unchecked helper\n";
        }
        exactUncheckedEmission = exactUncheckedEmission && exact;
      }
    }
  }
  expect(dischargedInvokes > 0,
         "the vector fixture should retain at least one checked prefix read "
         "after its failure outcomes are safety-discharged");
  expect(exactUncheckedEmission,
         "every safety-discharged prefix read should use the unchecked helper "
         "without a dead failure-status carrier");
}

void testPrefixStorageCStringAdmissionFailsClosed() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-prefix-storage-c-string.gti", R"(
#include <std/string>

int main() {
  mut std::string text = std::string("abc");
  c_string unchecked = text.c_string();
  c_string checked = text.try_c_string();
  return unchecked == nullptr || checked == nullptr ? 1 : 0;
}
)");
  expect(frontend.canGenerateCode() && frontend.mirValid &&
             frontend.mir.valid(),
         "the C-string observer fixture should produce verified MIR");
  if (!frontend.canGenerateCode() || !frontend.mir.valid()) {
    return;
  }

  struct Observer {
    lang::HirFunctionInstanceId function = 0;
    lang::MirInstructionId instruction = 0;
    lang::IntrinsicKind intrinsic = lang::IntrinsicKind::None;
  };
  const auto findMutableObserver = [](lang::MirBody &body,
                                      lang::MirInstructionId id) {
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
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  std::vector<Observer> observers;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic ==
                lang::IntrinsicKind::PrefixStorageCString ||
            instruction.intrinsic ==
                lang::IntrinsicKind::PrefixStorageTryCString) {
          observers.push_back(
              {function.id, instruction.id, instruction.intrinsic});
        }
      }
    }
  }
  expect(observers.size() == 2,
         "the string methods should lower to exactly the checked and "
         "unchecked C-string observer intrinsics");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  for (const Observer &observer : observers) {
    const lang::MirFunctionInstance *function =
        frontend.mir.findFunctionInstance(observer.function);
    expect(function != nullptr,
           "each C-string observer should retain its source function body");
    if (function == nullptr) {
      continue;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = observer.function};
    const lang::CppMirBodyEmitter exactEmitter(frontend.mir, map);
    const bool ready = function->mayRaiseDefinedFailure
                           ? exactEmitter.supportsFailureBodyText(address)
                           : exactEmitter.supportsBodyText(address);
    expect(ready,
           "a verified C-string storage observer should be admitted by the "
           "complete C++ text vocabulary");
    if (ready) {
      const lang::CppMirBodyEmissionText emitted =
          function->mayRaiseDefinedFailure
              ? exactEmitter.emitFailureBodyText(
                    address, "prefix-storage-c-string-regression-v0", 1)
              : exactEmitter.emitBodyText(
                    address, "prefix-storage-c-string-regression-v0", 1);
      const std::string_view helper =
          observer.intrinsic == lang::IntrinsicKind::PrefixStorageCString
              ? "prefix_storage_c_string("
              : "prefix_storage_try_c_string(";
      expect(emitted.emitted() &&
                 emitted.text.find(helper) != std::string::npos,
             "C-string observer text should name its exact shipped helper");
    }

    lang::MirProgram wrongResult = frontend.mir;
    auto &wrongFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        wrongResult.functionInstances());
    lang::MirInstruction *wrongResultInstruction = nullptr;
    for (lang::MirFunctionInstance &candidate : wrongFunctions) {
      if (candidate.id == observer.function) {
        wrongResultInstruction =
            findMutableObserver(candidate.body, observer.instruction);
        break;
      }
    }
    if (wrongResultInstruction != nullptr) {
      wrongResultInstruction->info.type = lang::SemanticType::Bool;
    }
    expect(wrongResultInstruction != nullptr &&
               !lang::verifyMirProgram(wrongResult).valid() &&
               !(function->mayRaiseDefinedFailure
                     ? lang::CppMirBodyEmitter(wrongResult, map)
                           .supportsFailureBodyText(address)
                     : lang::CppMirBodyEmitter(wrongResult, map)
                           .supportsBodyText(address)),
           "a C-string observer with a forged result type should fail both "
           "MIR verification and backend admission");

    lang::MirProgram missingStorage = frontend.mir;
    auto &missingFunctions =
        const_cast<std::vector<lang::MirFunctionInstance> &>(
            missingStorage.functionInstances());
    lang::MirInstruction *missingStorageInstruction = nullptr;
    for (lang::MirFunctionInstance &candidate : missingFunctions) {
      if (candidate.id == observer.function) {
        missingStorageInstruction =
            findMutableObserver(candidate.body, observer.instruction);
        break;
      }
    }
    if (missingStorageInstruction != nullptr) {
      missingStorageInstruction->operands.clear();
    }
    expect(missingStorageInstruction != nullptr &&
               !lang::verifyMirProgram(missingStorage).valid() &&
               !(function->mayRaiseDefinedFailure
                     ? lang::CppMirBodyEmitter(missingStorage, map)
                           .supportsFailureBodyText(address)
                     : lang::CppMirBodyEmitter(missingStorage, map)
                           .supportsBodyText(address)),
           "a C-string observer without its exact storage operand should "
           "fail both MIR verification and backend admission");
  }
}

// The inline closure chain: lambda-typed places and values never declare
// (C++ closure types are unnameable), so the Closure compute fuses into
// its consuming invocations, which spell the full literal with capture
// names from the Capture rows and the recursively emitted verified body.
// Under the transformed parent convention each private generated literal
// also carries an out-result and failure-record channel, so its checked
// arithmetic preserves the enclosing frame's cleanup edge.

void testExpectedExtractionStateAdmissionFailsClosed() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-extraction-state.gti", R"(
expected<int32_t, int32_t> first_state = int32_t(7);
expected<int32_t, int32_t> second_state = int32_t(8);

expected<int32_t, int32_t>
forward(expected<int32_t, int32_t> result) {
  return ?result;
}

expected<int32_t, int32_t> forward_globals() {
  bool ignored = second_state.has_value();
  return ?first_state;
}

int32_t main() {
  expected<int32_t, int32_t> result = int32_t(7);
  expected<int32_t, int32_t> forwarded = forward(result);
  expected<int32_t, int32_t> global_forwarded = forward_globals();
  return forwarded.has_value() && global_forwarded.has_value()
             ? forwarded.value() + global_forwarded.value() - int32_t(14)
             : int32_t(1);
}
)");
  expect(frontend.canGenerateCode(),
         "the Expected extraction-state fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto forwardHir =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &candidate) {
                     return candidate.source != nullptr &&
                            candidate.source->name().lexeme == "forward";
                   });
  const lang::MirFunctionInstance *forward =
      forwardHir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(forwardHir->id);
  const auto globalForwardHir = std::find_if(
      frontend.hir.functionInstances().begin(),
      frontend.hir.functionInstances().end(),
      [](const lang::HirFunctionInstance &candidate) {
        return candidate.source != nullptr &&
               candidate.source->name().lexeme == "forward_globals";
      });
  const lang::MirFunctionInstance *globalForward =
      globalForwardHir == frontend.hir.functionInstances().end()
          ? nullptr
          : frontend.mir.findFunctionInstance(globalForwardHir->id);
  const lang::MirBlock *stateBranch = nullptr;
  const auto findInstruction = [](const lang::MirBody &body,
                                  lang::MirInstructionId id) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const auto findMutableInstruction = [](lang::MirBody &body,
                                         lang::MirInstructionId id) {
    for (lang::MirBlock &block : body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  if (forward != nullptr) {
    for (const lang::MirBlock &block : forward->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Branch ||
          !block.terminator.value ||
          block.terminator.value->kind != lang::MirOperandKind::Value) {
        continue;
      }
      const lang::MirValue *condition =
          forward->body.findValue(block.terminator.value->value);
      const lang::MirInstruction *definition =
          condition == nullptr
              ? nullptr
              : findInstruction(forward->body, condition->definition);
      if (definition != nullptr &&
          definition->kind == lang::MirInstructionKind::Compute &&
          definition->operation == lang::MirOperation::ExpectedHasValue) {
        stateBranch = &block;
        break;
      }
    }
  }
  bool zeroSiteExtractions = false;
  if (forward != nullptr && stateBranch != nullptr) {
    for (const lang::MirBlockId target :
         {stateBranch->terminator.target, stateBranch->terminator.elseTarget}) {
      const lang::MirBlock *extraction = forward->body.findBlock(target);
      if (extraction == nullptr) {
        continue;
      }
      zeroSiteExtractions =
          zeroSiteExtractions ||
          std::any_of(
              extraction->instructions.begin(), extraction->instructions.end(),
              [](const lang::MirInstruction &instruction) {
                return instruction.kind == lang::MirInstructionKind::Call &&
                       (instruction.intrinsic ==
                            lang::IntrinsicKind::ExpectedValue ||
                        instruction.intrinsic ==
                            lang::IntrinsicKind::ExpectedError) &&
                       instruction.localFailureSites.empty();
              });
    }
  }
  expect(forward != nullptr && stateBranch != nullptr && zeroSiteExtractions,
         "propagation should lower zero-site Expected extractions behind one "
         "matching state branch");
  if (forward == nullptr || stateBranch == nullptr || !zeroSiteExtractions) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = forward->id};
  const lang::CppMirBodyEmitter canonical(frontend.mir, map);
  expect(canonical.supportsFailureBodyText(address),
         "the canonical state-proven propagation body should remain admitted");

  lang::MirProgram extraPredecessor = frontend.mir;
  auto &extraFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      extraPredecessor.functionInstances());
  lang::MirFunctionInstance &extraForward = extraFunctions.at(forward->id - 1);
  extraForward.body.blocks.push_back(
      {.id = static_cast<lang::MirBlockId>(extraForward.body.blocks.size() + 1),
       .terminator = {.kind = lang::MirTerminatorKind::Goto,
                      .target = stateBranch->terminator.target},
       .reachable = true});
  const lang::CppMirBodyEmitter extraPredecessorEmitter(extraPredecessor, map);
  expect(!extraPredecessorEmitter.supportsFailureBodyText(address),
         "a zero-site Expected extraction with an unproven incoming edge "
         "must fail backend admission");

  lang::MirProgram reversedState = frontend.mir;
  auto &reversedFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          reversedState.functionInstances());
  lang::MirFunctionInstance &reversedForward =
      reversedFunctions.at(forward->id - 1);
  lang::MirBlock *reversedBranch =
      reversedForward.body.findBlock(stateBranch->id);
  expect(reversedBranch != nullptr,
         "the propagation state branch should survive the forged copy");
  if (reversedBranch != nullptr) {
    std::swap(reversedBranch->terminator.target,
              reversedBranch->terminator.elseTarget);
    const lang::CppMirBodyEmitter reversedStateEmitter(reversedState, map);
    expect(!reversedStateEmitter.supportsFailureBodyText(address),
           "a zero-site Expected extraction without its matching state edge "
           "must fail backend admission");
  }

  const lang::MirValue *stateConditionValue =
      stateBranch->terminator.value
          ? forward->body.findValue(stateBranch->terminator.value->value)
          : nullptr;
  const lang::MirInstruction *stateCondition =
      stateConditionValue == nullptr
          ? nullptr
          : findInstruction(forward->body, stateConditionValue->definition);
  const lang::MirValue *carrierValue =
      stateCondition != nullptr && stateCondition->operands.size() == 1 &&
              stateCondition->operands.front().kind ==
                  lang::MirOperandKind::Value
          ? forward->body.findValue(stateCondition->operands.front().value)
          : nullptr;
  const lang::MirInstruction *carrierLoad =
      carrierValue != nullptr
          ? findInstruction(forward->body, carrierValue->definition)
          : nullptr;
  const lang::MirBlock *successStateBlock =
      forward->body.findBlock(stateBranch->terminator.target);
  const lang::MirBlock *errorStateBlock =
      forward->body.findBlock(stateBranch->terminator.elseTarget);
  const auto findExtraction = [](const lang::MirBlock *block,
                                 lang::IntrinsicKind intrinsic) {
    if (block == nullptr) {
      return static_cast<const lang::MirInstruction *>(nullptr);
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const lang::MirInstruction &instruction) {
                       return instruction.intrinsic == intrinsic &&
                              instruction.localFailureSites.empty();
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };
  const lang::MirInstruction *successStateExtraction =
      findExtraction(successStateBlock, lang::IntrinsicKind::ExpectedValue);
  const lang::MirInstruction *errorStateExtraction =
      findExtraction(errorStateBlock, lang::IntrinsicKind::ExpectedError);
  const lang::MirPlace *carrierSource =
      carrierLoad != nullptr && carrierLoad->operands.size() == 1
          ? forward->body.findPlace(carrierLoad->operands.front().place)
          : nullptr;
  expect(carrierLoad != nullptr && carrierSource != nullptr &&
             successStateExtraction != nullptr &&
             successStateExtraction->receiver &&
             errorStateExtraction != nullptr && errorStateExtraction->receiver,
         "the named propagation fixture should expose its exact checked and "
         "extracted carrier schedule");
  if (carrierLoad != nullptr && carrierSource != nullptr &&
      successStateExtraction != nullptr && successStateExtraction->receiver &&
      errorStateExtraction != nullptr && errorStateExtraction->receiver) {
    const auto forgeLoanCarriers =
        [&](std::optional<lang::IntrinsicKind> mismatchedExtraction) {
          lang::MirProgram forged = frontend.mir;
          auto &functions =
              const_cast<std::vector<lang::MirFunctionInstance> &>(
                  forged.functionInstances());
          lang::MirBody &body = functions.at(forward->id - 1).body;
          lang::MirInstruction *load =
              findMutableInstruction(body, carrierLoad->id);
          lang::MirInstruction *success =
              findMutableInstruction(body, successStateExtraction->id);
          lang::MirInstruction *error =
              findMutableInstruction(body, errorStateExtraction->id);

          const lang::MirLoanId firstLoan =
              body.loans.empty() ? 1 : body.loans.back().id + 1;
          const lang::MirLoanId secondLoan = firstLoan + 1;
          body.loans.push_back({.id = firstLoan,
                                .kind = lang::MirLoanKind::Local,
                                .source = carrierSource->id,
                                .access = lang::AccessMode::ReadOnly});
          body.loans.push_back({.id = secondLoan,
                                .kind = lang::MirLoanKind::Local,
                                .source = carrierSource->id,
                                .access = lang::AccessMode::ReadOnly});

          const lang::MirPlaceId firstPlace =
              body.places.empty() ? 1 : body.places.back().id + 1;
          const lang::MirPlaceId secondPlace = firstPlace + 1;
          const auto loanPlace = [&](lang::MirPlaceId id,
                                     lang::MirLoanId loan) {
            lang::MirPlace place = *carrierSource;
            place.id = id;
            place.root = lang::MirPlaceRootKind::Loan;
            place.binding = 0;
            place.symbol = 0;
            place.capture = 0;
            place.temporary = 0;
            place.value = 0;
            place.loan = loan;
            place.projections.clear();
            place.key.reset();
            return place;
          };
          body.places.push_back(loanPlace(firstPlace, firstLoan));
          body.places.push_back(loanPlace(secondPlace, secondLoan));

          load->operands.front().place = firstPlace;
          success->receiver->place =
              mismatchedExtraction == lang::IntrinsicKind::ExpectedValue
                  ? secondPlace
                  : firstPlace;
          error->receiver->place =
              mismatchedExtraction == lang::IntrinsicKind::ExpectedError
                  ? secondPlace
                  : firstPlace;
          return forged;
        };

    const lang::MirProgram matchingLoanCarrier =
        forgeLoanCarriers(std::nullopt);
    expect(lang::CppMirBodyEmitter(matchingLoanCarrier, map)
               .supportsFailureBodyText(address),
           "matching loan-root carriers should preserve propagation "
           "admission");
    const lang::MirProgram distinctValueLoan =
        forgeLoanCarriers(lang::IntrinsicKind::ExpectedValue);
    expect(!lang::CppMirBodyEmitter(distinctValueLoan, map)
                .supportsFailureBodyText(address),
           "a state proof for one loan must not admit ExpectedValue from a "
           "different loan root");
    const lang::MirProgram distinctErrorLoan =
        forgeLoanCarriers(lang::IntrinsicKind::ExpectedError);
    expect(!lang::CppMirBodyEmitter(distinctErrorLoan, map)
                .supportsFailureBodyText(address),
           "a state proof for one loan must not admit ExpectedError from a "
           "different loan root");
  }

  expect(globalForward != nullptr,
         "the symbol-carrier propagation fixture should lower to MIR");
  if (globalForward == nullptr) {
    return;
  }
  const lang::MirBodyAddress globalAddress{.kind = lang::MirBodyKind::Function,
                                           .owner = globalForward->id};
  const lang::CppMirBodyEmitter globalCanonical(frontend.mir, map);
  expect(globalCanonical.supportsFailureBodyText(globalAddress),
         "canonical propagation from a global Expected should be admitted");

  lang::MirProgram distinctSymbols = frontend.mir;
  auto &symbolFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      distinctSymbols.functionInstances());
  lang::MirBody &symbolBody = symbolFunctions.at(globalForward->id - 1).body;
  lang::MirInstruction *successExtraction = nullptr;
  lang::MirInstruction *errorExtraction = nullptr;
  for (lang::MirBlock &block : symbolBody.blocks) {
    if (block.terminator.kind != lang::MirTerminatorKind::Branch) {
      continue;
    }
    lang::MirBlock *success = symbolBody.findBlock(block.terminator.target);
    lang::MirBlock *error = symbolBody.findBlock(block.terminator.elseTarget);
    if (success == nullptr || error == nullptr) {
      continue;
    }
    const auto findExtraction = [](lang::MirBlock &candidate,
                                   lang::IntrinsicKind intrinsic) {
      const auto found = std::find_if(
          candidate.instructions.begin(), candidate.instructions.end(),
          [&](const lang::MirInstruction &instruction) {
            return instruction.intrinsic == intrinsic &&
                   instruction.localFailureSites.empty();
          });
      return found == candidate.instructions.end() ? nullptr : &*found;
    };
    successExtraction =
        findExtraction(*success, lang::IntrinsicKind::ExpectedValue);
    errorExtraction =
        findExtraction(*error, lang::IntrinsicKind::ExpectedError);
    if (successExtraction != nullptr && errorExtraction != nullptr) {
      break;
    }
  }
  const lang::MirPlace *provenPlace =
      successExtraction != nullptr && successExtraction->receiver
          ? symbolBody.findPlace(successExtraction->receiver->place)
          : nullptr;
  const auto alternate =
      provenPlace == nullptr
          ? symbolBody.places.end()
          : std::find_if(symbolBody.places.begin(), symbolBody.places.end(),
                         [&](const lang::MirPlace &place) {
                           return place.root ==
                                      lang::MirPlaceRootKind::Symbol &&
                                  place.type == provenPlace->type &&
                                  place.symbol != provenPlace->symbol;
                         });
  expect(successExtraction != nullptr && errorExtraction != nullptr &&
             provenPlace != nullptr && alternate != symbolBody.places.end(),
         "the global propagation fixture should retain two distinct symbol "
         "carriers and both zero-site extractions");
  if (successExtraction != nullptr && errorExtraction != nullptr &&
      provenPlace != nullptr && alternate != symbolBody.places.end()) {
    successExtraction->receiver->place = alternate->id;
    const lang::CppMirBodyEmitter distinctSymbolEmitter(distinctSymbols, map);
    expect(!distinctSymbolEmitter.supportsFailureBodyText(globalAddress),
           "a has-value proof for one symbol must not admit extraction from "
           "a distinct symbol-root Expected carrier");

    lang::MirProgram distinctErrorSymbols = frontend.mir;
    auto &errorFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        distinctErrorSymbols.functionInstances());
    lang::MirBody &errorBody = errorFunctions.at(globalForward->id - 1).body;
    lang::MirInstruction *forgedError =
        findMutableInstruction(errorBody, errorExtraction->id);
    expect(forgedError != nullptr && forgedError->receiver,
           "the copied global propagation body should retain ExpectedError");
    if (forgedError != nullptr && forgedError->receiver) {
      forgedError->receiver->place = alternate->id;
      const lang::CppMirBodyEmitter distinctErrorEmitter(distinctErrorSymbols,
                                                         map);
      expect(!distinctErrorEmitter.supportsFailureBodyText(globalAddress),
             "a has-value proof for one symbol must not admit ExpectedError "
             "from a distinct symbol-root carrier");
    }
  }
}

void testDeclarationOnlyCallBoundary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-declaration-call.gti", R"(
int32_t native_like(int32_t value);

int main() {
  return native_like(7);
}
)");
  expect(frontend.canGenerateCode(),
         "an ordinary declaration-only call should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *declaration = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    } else if (function.definitionKind ==
               lang::MirFunctionInstance::DefinitionKind::Declaration) {
      declaration = &function;
    }
  }
  expect(entry != nullptr && declaration != nullptr &&
             declaration->mayRaiseDefinedFailure,
         "MIR should retain the conservative declaration-only failure "
         "summary");
  if (entry == nullptr || declaration == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a declaration-only plain boundary should remain MIR-emittable");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "declaration-call-test-v0", 1);
  const auto targetRow =
      std::find_if(map.bodies().begin(), map.bodies().end(),
                   [&](const lang::CppMirBodyNameRepresentation &row) {
                     return row.address.kind == lang::MirBodyKind::Function &&
                            row.address.owner == declaration->id;
                   });
  const std::string target =
      targetRow == map.bodies().end() ? std::string() : targetRow->spelling;
  expect(!target.empty() && text.text.find(target + "(") != std::string::npos &&
             text.text.find("__gti_mir_call_success_") == std::string::npos,
         "the declaration-only boundary should use its plain symbol and "
         "treat a native return as success");
}

void testDeclarationOnlyMemberCallBoundary() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-declaration-member-call.gti", R"(
class ExternalBox<T> {
public:
  ExternalBox() {}
  bool ready();
};

int main() {
  mut ExternalBox<int32_t> box = ExternalBox<int32_t>();
  return box.ready() ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "an ordinary declaration-only member call should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirFunctionInstance *declaration = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    } else if (function.owner &&
               function.definitionKind ==
                   lang::MirFunctionInstance::DefinitionKind::Declaration) {
      declaration = &function;
    }
  }
  expect(entry != nullptr && declaration != nullptr &&
             declaration->mayRaiseDefinedFailure,
         "MIR should retain the conservative bodyless-member failure "
         "summary");
  if (entry == nullptr || declaration == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a bodyless member boundary should remain MIR-emittable");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
      address, "member-declaration-call-test-v0", 1);
  const auto targetRow =
      std::find_if(map.bodies().begin(), map.bodies().end(),
                   [&](const lang::CppMirBodyNameRepresentation &row) {
                     return row.address.kind == lang::MirBodyKind::Function &&
                            row.address.owner == declaration->id;
                   });
  const std::string target =
      targetRow == map.bodies().end() ? std::string() : targetRow->spelling;
  expect(!target.empty() && text.text.find(target + "(") != std::string::npos &&
             text.text.find("__gti_mir_call_success_") == std::string::npos,
         "the bodyless member boundary should use its plain member symbol "
         "and treat a native return as success");

  try {
    const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
    expect(!artifact.contents.empty(),
           "production C++ emission should accept a bodyless member call");
  } catch (const std::logic_error &) {
    expect(false, "production C++ emission should not reject a bodyless member "
                  "call");
  }
}

void testPassiveReturnSignedMinimumAndRepresentationSlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-passive-return-and-slot.gti", R"(
#include <std/numeric>

constexpr int8_t wrapped_boundary =
    std::wrapping_add(int8_t(127), int8_t(1));

union NumberBits {
  mut uint32_t integer;
  mut float real;
};

enum class Command {
  stop,
  translate(int32_t x, int32_t y),
  exit_code(int32_t code),
};

int32_t read_integer_bits() {
  NumberBits bits{uint32_t(42)};
  unsafe {
    return int32_t(bits.integer);
  }
}

Command next_command(mut int32_t& calls) {
  calls++;
  return Command::translate(20, 22);
}

int main() {
  Command stopped = Command::stop;
  switch (stopped) {
  case Command::stop:
    break;
  case Command::translate(x, y):
    return x + y;
  case Command::exit_code(code):
    return code;
  }

  mut int32_t calls = 0;
  switch (next_command(calls)) {
  case Command::stop:
    return 1;
  case Command::translate(x, y):
    if (wrapped_boundary != int8_t(-128)) {
      return 2;
    }
    return read_integer_bits() - (x + y);
  case Command::exit_code(code):
    return code;
  }
}
)");
  expect(frontend.canGenerateCode(),
         "the passive-return and representation-slot fixture should pass "
         "the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *signedMinimum = nullptr;
  const lang::MirFunctionInstance *representationSlot = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    bool hasSignedMinimum = false;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        hasSignedMinimum =
            hasSignedMinimum ||
            (instruction.kind == lang::MirInstructionKind::Compute &&
             instruction.operation == lang::MirOperation::Negate &&
             instruction.programConstantSubstitution);
      }
    }
    if (hasSignedMinimum) {
      signedMinimum = &function;
    }
    if (function.returnType == lang::SemanticType::Int32 &&
        function.parameterTypes.empty() &&
        function.body.dropObligations.empty() &&
        std::any_of(function.body.places.begin(), function.body.places.end(),
                    [](const lang::MirPlace &place) {
                      return place.root == lang::MirPlaceRootKind::Binding &&
                             place.projections.empty() &&
                             place.type.kind == lang::SemanticType::Class &&
                             place.traits.drop == lang::DropKind::Trivial;
                    })) {
      representationSlot = &function;
    }
  }
  const bool constantReturn =
      entry != nullptr &&
      std::any_of(entry->body.blocks.begin(), entry->body.blocks.end(),
                  [](const lang::MirBlock &block) {
                    return block.terminator.kind ==
                               lang::MirTerminatorKind::Return &&
                           block.terminator.value &&
                           block.terminator.value->kind ==
                               lang::MirOperandKind::Constant;
                  });
  expect(signedMinimum != nullptr && representationSlot != nullptr &&
             entry != nullptr && constantReturn,
         "the fixture should lower a fused signed minimum, a trivial class "
         "slot, and a passive constant return");
  if (signedMinimum == nullptr || representationSlot == nullptr ||
      entry == nullptr || !constantReturn) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const auto failureText = [&](const lang::MirFunctionInstance &function,
                               std::string_view label) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the passive leaf fixture should enter transformed failure-body "
           "emission");
    return emitter.supportsFailureBodyText(address)
               ? emitter.emitFailureBodyText(address, label, 1).text
               : std::string{};
  };

  const std::string signedText =
      failureText(*signedMinimum, "signed-minimum-test-v0");
  expect(signedText.find("signed-minimum magnitude spells at its negation") !=
                 std::string::npos &&
             signedText.find("static_cast<std::int8_t>(-127 - 1)") !=
                 std::string::npos,
         "the signed minimum should fuse its otherwise unrepresentable "
         "positive magnitude into the typed negation");

  const std::string slotText =
      failureText(*representationSlot, "representation-slot-test-v0");
  expect(slotText.find("mir_lifetime_slot<") != std::string::npos &&
             slotText.find(", true>") != std::string::npos,
         "a trivial GTI value without a Drop obligation should select the "
         "representation-only lifetime-slot policy");

  const std::string entryText = failureText(*entry, "passive-return-test-v0");
  expect(
      entryText.find("*__gti_mir_out_result = static_cast<std::int32_t>(0);") !=
          std::string::npos,
      "a passive constant Return operand should publish directly through "
      "the transformed result pointer");
}

void testSingleUseClassSsaRepresentationSlot() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-class-ssa-slot.gti", R"(
#include <std/algorithm>
#include <std/vector>

class Positive {
  mut bool observed = false;

public:
  bool operator()(int32_t& value) mut {
    this.observed = true;
    return value > 0;
  }
};

int main() {
  mut std::vector<int32_t> values{};
  values.push_back(7);
  return int32_t(std::count_if(values.begin(), values.end(), Positive())) - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the single-use class SSA fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  lang::MirValueId temporary = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &producer : block.instructions) {
        if (producer.kind != lang::MirInstructionKind::Call ||
            !producer.result || producer.functionTarget ||
            producer.constructorTarget || producer.receiver ||
            !producer.operands.empty() ||
            producer.info.type.kind != lang::SemanticType::Class) {
          continue;
        }
        bool directCallArgument = false;
        for (const lang::MirBlock &candidate : function.body.blocks) {
          for (const lang::MirInstruction &consumer : candidate.instructions) {
            directCallArgument =
                directCallArgument ||
                (consumer.kind == lang::MirInstructionKind::Call &&
                 std::any_of(consumer.operands.begin(), consumer.operands.end(),
                             [&](const lang::MirOperand &operand) {
                               return operand.kind ==
                                          lang::MirOperandKind::Value &&
                                      operand.value == *producer.result;
                             }));
          }
        }
        if (directCallArgument) {
          selected = &function;
          temporary = *producer.result;
        }
      }
    }
  }
  expect(selected != nullptr,
         "the fixture should lower a default-constructed class SSA value used "
         "by one call");
  if (selected == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the single-use class SSA fixture should enter transformed emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-ssa-slot-test-v0", 1).text;
  const std::string name = "__gti_mir_v_" + std::to_string(temporary);
  expect(text.find(", true> " + name + ";") != std::string::npos &&
             text.find(name + ".construct();") != std::string::npos &&
             text.find("std::move(" + name + ".get())") != std::string::npos &&
             text.find(name + ".destroy();") != std::string::npos,
         "the class temporary should construct, transfer, and retire through "
         "one representation-only lifetime slot");
}

void testRepresentationOnlyPublicationRearmsBeforeConstruction() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-representation-slot-rearm.gti", R"(
[[c_abi]] struct Pair {
  mut float x;
  mut float y;
};

extern "C" {
  Pair make_pair_c(float x, float y);
}

int main() {
  for (mut int32_t iteration = 0; iteration < 3; iteration++) {
    Pair pair = make_pair_c(1.0, 2.0);
    if (pair.x < 0.0) {
      return 1;
    }
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the repeated passive-record publication fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the repeated passive-record fixture should lower its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const bool failureBody = emitter.supportsFailureBodyText(address);
  expect(emitter.analyze(address).ready() &&
             (failureBody || emitter.supportsBodyText(address)),
         "the repeated passive-record publication should enter transformed "
         "emission");
  if (!failureBody && !emitter.supportsBodyText(address)) {
    return;
  }

  const std::string text =
      failureBody
          ? emitter.emitFailureBodyText(address, "slot-rearm-test-v0", 1).text
          : emitter.emitBodyText(address, "slot-rearm-test-v0", 1).text;
  const std::string constructMarker =
      ".construct_from([&]() { return make_pair_c(";
  const std::size_t construction = text.find(constructMarker);
  const std::size_t slotStart = construction == std::string::npos
                                    ? std::string::npos
                                    : text.rfind("__gti_mir_p_", construction);
  const std::string slot =
      slotStart == std::string::npos
          ? std::string{}
          : text.substr(slotStart, construction - slotStart);
  const std::string rearmMarker = slot + ".destroy_if_engaged();";
  const std::string publicationMarker = slot + constructMarker;
  const std::size_t rearm =
      slot.empty() ? std::string::npos : text.find(rearmMarker);
  const std::size_t publication =
      slot.empty() ? std::string::npos : text.find(publicationMarker);
  const std::string separator =
      rearm == std::string::npos || publication == std::string::npos ||
              rearm + rearmMarker.size() > publication
          ? std::string{"not-adjacent"}
          : text.substr(rearm + rearmMarker.size(),
                        publication - (rearm + rearmMarker.size()));
  expect(!slot.empty() &&
             text.find(", true> " + slot + ";") != std::string::npos &&
             rearm != std::string::npos && publication != std::string::npos &&
             rearm < publication &&
             separator.find_first_not_of(" \t\r\n") == std::string::npos,
         "an exact repeated Initialize should rearm its representation-only "
         "slot immediately before construct_from");
}

void testCopyParameterFieldPublicationAdmission() {
  // A constructor member initializer may copy a passive generated-copy class
  // directly from its exact by-value parameter. MIR retains both the parameter
  // Load and the explicit field Initialize; native member-list emission must
  // prove and consume that exact publication rather than inventing it.
  lang::FrontendResult copyForm =
      analyzeWithStandardLibrary("cpp-mir-copy-parameter-field.gti", R"(
class Payload {
public:
  int32_t value;
  Payload(int32_t value) : value(value) {}
};

class Holder {
public:
  Payload inner;
  Holder(Payload payload) : inner(payload) {}
};

int main() {
  Holder holder = Holder(Payload(3));
  return holder.inner.value - 3;
}
)");
  expect(copyForm.canGenerateCode(),
         "the copy-parameter field fixture should pass the frontend");
  if (!copyForm.canGenerateCode()) {
    return;
  }
  const auto findHolderConstructor =
      [](lang::FrontendResult &frontend) -> lang::MirConstructorInstance * {
    lang::MirConstructorInstance *selected = nullptr;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            frontend.mir.constructorInstances());
    for (lang::MirConstructorInstance &constructor : constructors) {
      if (constructor.parameterTypes.size() == 1 &&
          constructor.parameterTypes.front().kind ==
              lang::SemanticType::Class &&
          !constructor.initializers.empty()) {
        selected = &constructor;
      }
    }
    return selected;
  };
  lang::MirConstructorInstance *copyConstructor =
      findHolderConstructor(copyForm);
  expect(copyConstructor != nullptr,
         "the copy fixture should lower the class-parameter constructor");
  if (copyConstructor == nullptr) {
    return;
  }
  {
    const lang::CppMirBodyEmissionMap map(
        buildRows(copyForm, copyForm.mir, lang::CppStandard::Cpp23));
    const lang::CppMirBodyEmitter emitter(copyForm.mir, map);
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                       .owner = copyConstructor->id};
    const auto bindings =
        lang::cppMirCopyParameterFieldBindings(copyForm.mir, *copyConstructor);
    expect(bindings && bindings->size() == 1 &&
               bindings->front().initializeInstruction != 0,
           "the copy-parameter field publication should prove the exact "
           "explicit passive class Initialize");
    expect(emitter.supportsBodyText(address),
           "the exact passive class copy must enter the ordinary constructor "
           "form");
    expect(emitter.supportsFailureBodyText(address),
           "the exact passive class copy must enter the failure constructor "
           "form");
    if (emitter.supportsBodyText(address)) {
      const std::string text =
          emitter.emitBodyText(address, "copy-parameter-field-v0", 1).text;
      expect(text.find("parameter copy publishes in the member initializer "
                       "list") != std::string::npos,
             "the class parameter Load should be consumed by the exact "
             "member-initializer publication");
    }
  }

  lang::MirInstruction *copyLoad = nullptr;
  for (lang::MirBlock &block : copyConstructor->body.blocks) {
    for (lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Load &&
          instruction.result && instruction.ownership) {
        copyLoad = &instruction;
      }
    }
  }
  expect(copyLoad != nullptr,
         "the copy fixture should retain its parameter Load proof input");
  if (copyLoad != nullptr) {
    copyLoad->ownership->reachable = false;
    const auto malformedBindings =
        lang::cppMirCopyParameterFieldBindings(copyForm.mir, *copyConstructor);
    expect(malformedBindings && malformedBindings->empty(),
           "unreachable forged ownership must invalidate the passive class "
           "copy binding");
    const lang::CppMirBodyEmissionMap malformedMap(
        buildRows(copyForm, copyForm.mir, lang::CppStandard::Cpp23));
    const lang::CppMirBodyEmitter malformedEmitter(copyForm.mir, malformedMap);
    const lang::MirBodyAddress malformedAddress{
        .kind = lang::MirBodyKind::Constructor, .owner = copyConstructor->id};
    expect(!malformedEmitter.supportsBodyText(malformedAddress) &&
               !malformedEmitter.supportsFailureBodyText(malformedAddress),
           "the passive class copy route must fail closed when its exact "
           "ownership event drifts");
  }

  // A scalar parameter published through a normal MIR Initialize remains a
  // body-local place when the constructor reads it again after the member
  // initializer. Only the exact single-use class copy above may elide that
  // carrier in favor of the native parameter.
  lang::FrontendResult scalarReuse =
      analyzeWithStandardLibrary("cpp-mir-scalar-parameter-reuse.gti", R"(
class ScalarReuse {
public:
  int32_t stored;
  mut int32_t observed = 0;

  ScalarReuse(int32_t value) : stored(value) {
    observed = value;
  }
};

int main() {
  ScalarReuse value = ScalarReuse(3);
  return value.observed - 3;
}
)");
  expect(scalarReuse.canGenerateCode(),
         "the scalar parameter reuse fixture should pass the frontend");
  if (scalarReuse.canGenerateCode()) {
    const lang::MirConstructorInstance *reuseConstructor = nullptr;
    for (const lang::MirConstructorInstance &constructor :
         scalarReuse.mir.constructorInstances()) {
      const lang::MirClassInstance *owner =
          scalarReuse.mir.findClassInstance(constructor.owner);
      const lang::ClassTypeInfo *info =
          owner == nullptr
              ? nullptr
              : scalarReuse.semantics.findClassType(owner->declaration);
      if (info != nullptr && info->declaration != nullptr &&
          info->declaration->name().lexeme == "ScalarReuse") {
        reuseConstructor = &constructor;
        break;
      }
    }
    expect(reuseConstructor != nullptr,
           "the scalar fixture should lower its constructor instance");
    if (reuseConstructor != nullptr) {
      const auto bindings = lang::cppMirCopyParameterFieldBindings(
          scalarReuse.mir, *reuseConstructor);
      expect(bindings && bindings->size() == 1 &&
                 bindings->front().initializeInstruction != 0,
             "the scalar field copy should retain its ordinary MIR "
             "Initialize publication");
      const lang::CppMirBodyEmissionMap map(
          buildRows(scalarReuse, scalarReuse.mir, lang::CppStandard::Cpp23));
      const lang::CppMirBodyEmitter emitter(scalarReuse.mir, map);
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                         .owner = reuseConstructor->id};
      expect(emitter.supportsFailureBodyText(address),
             "the scalar parameter reuse should enter failure-body "
             "emission");
      if (bindings && bindings->size() == 1 &&
          emitter.supportsFailureBodyText(address)) {
        const std::string text =
            emitter.emitFailureBodyText(address, "scalar-parameter-reuse-v0", 1)
                .text;
        expect(text.find("std::int32_t __gti_mir_p_" +
                         std::to_string(bindings->front().sourcePlace) +
                         " = __gti_mir_arg_0;") != std::string::npos,
               "a scalar constructor parameter reused in the body must keep "
               "its MIR place declaration");
      }
    }
  }

  // The moved flavor of the same constructor keeps its admitted
  // member-initializer publication.
  lang::FrontendResult moveForm =
      analyzeWithStandardLibrary("cpp-mir-move-parameter-field.gti", R"(
class Payload {
public:
  int32_t value;
  Payload(int32_t value) : value(value) {}
};

class Holder {
public:
  Payload inner;
  Holder(Payload payload) : inner(std::move(payload)) {}
};

int main() {
  Holder holder = Holder(Payload(3));
  return holder.inner.value - 3;
}
)");
  expect(moveForm.canGenerateCode(),
         "the move-parameter field fixture should pass the frontend");
  if (!moveForm.canGenerateCode()) {
    return;
  }
  lang::MirConstructorInstance *moveConstructor =
      findHolderConstructor(moveForm);
  expect(moveConstructor != nullptr,
         "the move fixture should lower the class-parameter constructor");
  if (moveConstructor == nullptr) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(moveForm, moveForm.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(moveForm.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = moveConstructor->id};
  expect(emitter.supportsFailureBodyText(address),
         "the moved-parameter field publication must keep its admitted "
         "failure form");
}

void testFailureFormSubscriptCompatibilityBoundary() {
  lang::FrontendResult frontend = analyze("subscript-boundary.gti", R"(
class Indexed {
  mut int elements[1] = {1};

public:
  int& operator[](uint64_t index) { return this.elements[index]; }
  mut int& operator[](uint64_t index) mut { return this.elements[index]; }
};

int mutate(mut Indexed& indexed) {
  indexed[uint64_t(0)] *= 2;
  return indexed[uint64_t(0)];
}

int main() {
  mut Indexed indexed = Indexed();
  indexed[uint64_t(0)] += 1;
  return mutate(indexed) - 4;
}
)");
  expect(frontend.canGenerateCode(),
         "the reduced failure-capable subscript fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  std::size_t selectedBoundaries = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.overloadedOperator &&
        *function.overloadedOperator == lang::OverloadedOperator::Subscript &&
        lang::cppMirNeedsTerminalSubscriptBoundaryWrapper(frontend.mir, map,
                                                          function.id)) {
      ++selectedBoundaries;
    }
  }
  expect(selectedBoundaries == 2,
         "the compatibility places should select the exact read-only and "
         "mutable subscript boundary instances");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  std::size_t normalDefinitions = 0;
  bool checkedReferenceCompound = false;
  std::istringstream lines(artifact.contents);
  for (std::string line; std::getline(lines, line);) {
    normalDefinitions +=
        line.find("Indexed::__gti_fn_") != std::string::npos &&
        line.find("___gti_operator_subscript(") != std::string::npos &&
        line.find(" {") != std::string::npos;
    checkedReferenceCompound =
        checkedReferenceCompound ||
        (line.find("mir_checked_compound_multiply_v1(") != std::string::npos &&
         line.find("___gti_operator_subscript(") != std::string::npos);
  }
  expect(normalDefinitions == 2,
         "the C++ artifact should define both ordinary subscript boundaries "
         "instead of leaving compatibility calls unresolved");
  expect(checkedReferenceCompound,
         "a reference-projected class subscript should preserve the checked "
         "compound operation instead of degrading it to assignment");
}

constexpr NamedTest kTests[] = {
    {"testOwningPayloadEnumRepresentationAdmission",
     testOwningPayloadEnumRepresentationAdmission},
    {"testOwningPayloadEnumTransportAdmissionFailsClosed",
     testOwningPayloadEnumTransportAdmissionFailsClosed},
    {"testDischargedCompoundOperatorVocabulary",
     testDischargedCompoundOperatorVocabulary},
    {"testExhaustiveEnumClassification", testExhaustiveEnumClassification},
    {"testReadyBodyAndRepresentationFailures",
     testReadyBodyAndRepresentationFailures},
    {"testProgramPlanValidationIsOperationScoped",
     testProgramPlanValidationIsOperationScoped},
    {"testImmutableBodyPlanOutlivesAdmissionInputs",
     testImmutableBodyPlanOutlivesAdmissionInputs},
    {"testHostedStartupOwnsRemainingAuthorityGap",
     testHostedStartupOwnsRemainingAuthorityGap},
    {"testVerifiedProgramInitializationPlanClassification",
     testVerifiedProgramInitializationPlanClassification},
    {"testSafeIndexUsesBoundsNotRawMemory",
     testSafeIndexUsesBoundsNotRawMemory},
    {"testUnsafeRawPointerVocabulary", testUnsafeRawPointerVocabulary},
    {"testConcreteGenericFieldOwner", testConcreteGenericFieldOwner},
    {"testAmbiguousStaticStorageFailsClosed",
     testAmbiguousStaticStorageFailsClosed},
    {"testGlobalClassStorageMemberProjection",
     testGlobalClassStorageMemberProjection},
    {"testConcreteClassStaticStorage", testConcreteClassStaticStorage},
    {"testDischargedStorageReadAnalysis", testDischargedStorageReadAnalysis},
    {"testPrefixStorageCStringAdmissionFailsClosed",
     testPrefixStorageCStringAdmissionFailsClosed},
    {"testExpectedExtractionStateAdmissionFailsClosed",
     testExpectedExtractionStateAdmissionFailsClosed},
    {"testDeclarationOnlyCallBoundary", testDeclarationOnlyCallBoundary},
    {"testDeclarationOnlyMemberCallBoundary",
     testDeclarationOnlyMemberCallBoundary},
    {"testPassiveReturnSignedMinimumAndRepresentationSlot",
     testPassiveReturnSignedMinimumAndRepresentationSlot},
    {"testSingleUseClassSsaRepresentationSlot",
     testSingleUseClassSsaRepresentationSlot},
    {"testRepresentationOnlyPublicationRearmsBeforeConstruction",
     testRepresentationOnlyPublicationRearmsBeforeConstruction},
    {"testCopyParameterFieldPublicationAdmission",
     testCopyParameterFieldPublicationAdmission},
    {"testFailureFormSubscriptCompatibilityBoundary",
     testFailureFormSubscriptCompatibilityBoundary},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "preflight");
}
