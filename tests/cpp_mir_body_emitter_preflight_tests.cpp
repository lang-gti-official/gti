#include "../src/compiler/cpp_mir_body_text_writer.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

template <typename Body>
auto *findPreflightInstruction(Body &body, lang::MirInstructionId id) {
  for (auto &block : body.blocks) {
    for (auto &instruction : block.instructions) {
      if (instruction.id == id)
        return &instruction;
    }
  }
  return static_cast<decltype(&body.blocks.front().instructions.front())>(
      nullptr);
}

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

void testConsumedPassivePayloadTagObservation() {
  const auto frontend =
      analyzeWithStandardLibrary("consumed-payload-tag.gti", R"(
enum Packet { empty, number(int value) };
class Envelope {
  mut Packet packet;
public:
  Envelope(int value) : packet(Packet::number(value)) {}
  int take() && {
    switch (std::move(this.packet)) {
    case Packet::empty: return 0;
    case Packet::number(value): return value;
    }
  }
};
int main() { return Envelope(42).take() == 42 ? 0 : 1; }
)");
  expect(frontend.canGenerateCode(), "consumed passive enum should lower");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  constexpr std::string_view closedDomain = "mir_closed_enum_unreachable_v1";
  std::size_t observed = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source || hir->source->name().lexeme != "take")
      continue;
    const auto *entry = function.body.findBlock(function.body.entry);
    if (!entry || entry->terminator.kind != lang::MirTerminatorKind::Switch)
      continue;
    ++observed;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    const auto text =
        emitter.emitBodyText(address, "consumed-payload-domain", 1);
    expect(text.emitted() && text.text.find(closedDomain) != std::string::npos,
           "exact passive entry transfer preserves the initialized tag domain");
    if (entry->instructions.size() != 3) {
      expect(false,
             "consumed enum fixture retains move/init/boundary schedule");
      continue;
    }
    for (int mutation = 0; mutation != 15; ++mutation) {
      auto forged = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          forged.findFunctionInstance(function.id));
      auto &block = changed->body.blocks[entry->id - 1];
      auto &move = block.instructions[0];
      auto &initialize = block.instructions[1];
      auto &source = changed->body.places[move.operands.front().place - 1];
      auto &subject = changed->body.places[block.terminator.value->place - 1];
      switch (mutation) {
      case 0:
        move.ownership->before = lang::OwnershipStateSet::Moved;
        break;
      case 1:
        move.ownership->after = lang::OwnershipStateSet::Available;
        break;
      case 2:
        move.operands.front().kind = lang::MirOperandKind::Copy;
        break;
      case 3:
        initialize.operands.front().value = 0;
        break;
      case 4:
        initialize.destination = source.id;
        break;
      case 5:
        subject.initiallyAvailable = true;
        break;
      case 6:
        source.projections.front().field += 1;
        break;
      case 7:
        source.root = lang::MirPlaceRootKind::Value;
        break;
      case 8:
        block.instructions.back().kind = lang::MirInstructionKind::Drop;
        break;
      case 9:
        block.instructions.back().ownership.reset();
        break;
      case 10:
        changed->receiverMutability = lang::ReceiverMutability::ReadOnly;
        break;
      case 11:
        move.rawMemoryAccess = true;
        break;
      case 12:
        block.terminator.switchTargets.pop_back();
        break;
      case 13:
        move.intrinsic = lang::IntrinsicKind::None;
        break;
      case 14:
        initialize.info.category = lang::ValueCategory::Value;
        break;
      }
      const lang::CppMirBodyEmitter rejected(forged, map);
      const auto result =
          rejected.emitBodyText(address, "forged-transfer-domain", 1);
      expect(!result.emitted() ||
                 result.text.find(closedDomain) == std::string::npos,
             "altered producer, transfer, availability, effects or domain "
             "cannot justify unreachable");
    }
  }
  expect(observed == 1, "fixture exercises one consumed passive enum match");
}

void testPassivePayloadTagObservation() {
  const auto frontend = analyze("passive-payload-tag.gti", R"(
enum State<T> { empty, value(T value) };
bool present(State<int>& state) {
  switch (state) {
  case State<int>::empty: return false;
  case State<int>::value: return true;
  }
}
class Holder {
  State<int> state;
public:
  Holder() : state(State<int>::value(7)) {}
  bool present() {
    switch (this.state) {
    case State<int>::empty: return false;
    case State<int>::value: return true;
    }
  }
};
int main() {
  auto state = State<int>::value(7);
  Holder holder{};
  return present(state) && holder.present() ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(), "passive tag observer should lower");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  constexpr std::string_view closedDomain = "mir_closed_enum_unreachable_v1";
  std::size_t observed = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      const auto &switching = block.terminator;
      if (switching.kind != lang::MirTerminatorKind::Switch ||
          !switching.value ||
          !frontend.mir.findPayloadEnum(switching.value->type))
        continue;
      ++observed;
      expect(switching.value->kind == lang::MirOperandKind::BorrowRead &&
                 switching.value->place != 0,
             "tag-only matching must observe the enum place, not copy it");
      for (const auto &candidate : function.body.blocks)
        for (const auto &instruction : candidate.instructions)
          expect(instruction.kind != lang::MirInstructionKind::Load ||
                     instruction.info.type != switching.value->type,
                 "tag-only matching must not stage a whole-enum copy");
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, map);
      const auto text =
          emitter.emitBodyText(address, "closed-payload-domain", 1);
      expect(text.emitted() &&
                 text.text.find(closedDomain) != std::string::npos,
             "exact initialized entry observers expose the closed tag domain");
      for (int mutation = 0; mutation != 14; ++mutation) {
        auto forged = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        auto &entry = changed->body.blocks[block.id - 1];
        auto &term = entry.terminator;
        auto &place = changed->body.places[term.value->place - 1];
        auto &fallback = changed->body.blocks[term.target - 1];
        switch (mutation) {
        case 0:
          term.switchTargets.pop_back();
          break;
        case 1:
          term.switchTargets[1].value = term.switchTargets[0].value;
          break;
        case 2:
          term.switchTargets[0].value->value.magnitude = 99;
          break;
        case 3:
          ++term.switchTargets[0].value->enumOwner;
          break;
        case 4:
          term.switchTargets[0].value->type = lang::SemanticType::Int32;
          break;
        case 5:
          term.value->kind = lang::MirOperandKind::Move;
          break;
        case 6:
          place.type = lang::SemanticType::Int32;
          break;
        case 7:
          fallback.terminator.kind = lang::MirTerminatorKind::Return;
          break;
        case 8:
          fallback.instructions.push_back(entry.instructions.front());
          break;
        case 9:
          entry.instructions.front().kind = lang::MirInstructionKind::Drop;
          break;
        case 10:
          term.switchTargets[0].target = changed->body.entry;
          break;
        case 11:
          if (function.owner)
            changed->receiverMutability = lang::ReceiverMutability::Consuming;
          else
            for (auto &root : changed->body.places)
              root.initiallyAvailable = false;
          break;
        case 12:
          if (function.owner)
            ++place.projections.front().field;
          else
            changed->parameterBindings.front() = 0;
          break;
        case 13:
          if (entry.instructions.front().ownership)
            entry.instructions.front().ownership->before =
                lang::OwnershipStateSet::Moved;
          else
            place.root = lang::MirPlaceRootKind::Value;
          break;
        }
        const lang::CppMirBodyEmitter rejected(forged, map);
        const auto rejectedText =
            rejected.emitBodyText(address, "forged-domain", 1);
        expect(!rejectedText.emitted() ||
                   rejectedText.text.find(closedDomain) == std::string::npos,
               "incomplete, substituted, effectful or unavailable domains "
               "never become native unreachable");
      }
      for (int mutation = 0; mutation != 5; ++mutation) {
        auto rows = buildRows(frontend, frontend.mir);
        const auto row = std::find_if(
            rows.enums.begin(), rows.enums.end(), [&](const auto &candidate) {
              return candidate.type == switching.value->type;
            });
        expect(row != rows.enums.end(), "observer has an exact enum row");
        if (row == rows.enums.end())
          continue;
        switch (mutation) {
        case 0:
          row->payloadVariants.pop_back();
          break;
        case 1:
          row->payloadVariants[1].index = 0;
          break;
        case 2:
          ++row->owner;
          break;
        case 3:
          row->traits.copyable = !row->traits.copyable;
          break;
        case 4:
          rows.enums.push_back(*row);
          break;
        }
        const lang::CppMirBodyEmissionMap forgedRows(std::move(rows));
        const lang::CppMirBodyEmitter rejected(frontend.mir, forgedRows);
        const auto rejectedText =
            rejected.emitBodyText(address, "forged-enum-row", 1);
        expect(!rejectedText.emitted() ||
                   rejectedText.text.find(closedDomain) == std::string::npos,
               "stale, incomplete or ambiguous representation rows cannot "
               "prove a closed domain");
      }
    }
  }
  expect(observed == 2,
         "fixture must exercise reference and receiver-field observers");
  std::size_t constructed = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::PayloadConstruct)
          continue;
        ++constructed;
        const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                           constructor.id};
        const lang::CppMirBodyEmitter emitter(frontend.mir, map);
        const auto text =
            emitter.emitFailureBodyText(address, "passive-field", 1);
        constexpr std::string_view publication =
            "direct passive payload field publication";
        expect(text.emitted() &&
                   text.text.find(publication) != std::string::npos,
               "passive enum fields construct directly at their MIR "
               "publication point");
        for (int mutation = 0; mutation != 13; ++mutation) {
          auto forged = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              forged.findConstructorInstance(constructor.id));
          auto *producer =
              findPreflightInstruction(changed->body, instruction.id);
          auto *payload = const_cast<lang::MirPayloadEnum *>(
              forged.findPayloadEnum(producer->info.type));
          switch (mutation) {
          case 11:
            payload->traits.moveAssignable = false;
            break;
          case 12:
            payload->variants.back().payloadTypes.front() =
                lang::SemanticType::arrayOf(lang::SemanticType::Int32, 2);
            break;
          case 0:
            ++changed->initializers.front().field;
            break;
          case 1:
            changed->initializers.front().targetType =
                lang::SemanticType::Int32;
            break;
          case 2:
            changed->initializers.front().generatedDefault = true;
            break;
          case 3:
            changed->initializers.push_back(changed->initializers.front());
            break;
          case 4:
            producer->localFailureSites.push_back(1);
            break;
          case 5:
            changed->body.blocks[block.id - 1]
                .instructions.back()
                .fullExpressionEnd = 0;
            break;
          case 6:
            producer->info.traits.containsBorrowedState = true;
            break;
          case 7:
          case 8:
          case 9:
          case 10:
            for (auto &item : changed->body.blocks[block.id - 1].instructions) {
              if (item.kind != lang::MirInstructionKind::Initialize)
                continue;
              if (mutation == 7)
                item.constructorInitializer = 0;
              if (mutation == 8)
                item.hirValue = 0;
              if (mutation == 9)
                item.operands.front().value = 0;
              if (mutation == 10)
                item.destination.reset();
            }
            break;
          }
          const lang::CppMirBodyEmitter rejected(forged, map);
          const auto rejectedText =
              rejected.emitFailureBodyText(address, "forged-passive-field", 1);
          expect(!rejectedText.emitted() ||
                     rejectedText.text.find(publication) == std::string::npos,
                 "passive publication rejects wrong fields, roots, effects, or "
                 "borrowed state");
        }
      }
  }
  expect(constructed == 1,
         "fixture exercises one direct passive field construction");
}

void testPayloadObserverRequiresCompleteReceiver() {
  // The backend's entry-domain proof relies on this source/MIR call boundary:
  // a read-only method cannot receive an object with a moved enum field.
  const auto invalid =
      analyzeWithStandardLibrary("partial-payload-observer.gti", R"gti(
#include <std/string>
enum State { empty, value(std::string text) };
class Holder {
  mut State state;
public:
  Holder() : state(State::value(std::string("owned"))) {}
  bool present() {
    switch (this.state) {
    case State::empty: return false;
    case State::value: return true;
    }
  }
  bool consume_then_observe() mut {
    auto moved = std::move(this.state);
    return this.present();
  }
};
int main() { mut Holder holder{}; return holder.consume_then_observe() ? 1 : 0; }
)gti");
  expect(!invalid.canGenerateCode() &&
             std::any_of(
                 invalid.diagnostics.begin(), invalid.diagnostics.end(),
                 [](const auto &diagnostic) {
                   return diagnostic.code == "GTI-S2018" &&
                          diagnostic.message.find(
                              "Place 'this.state' has already been moved") !=
                              std::string::npos;
                 }),
         "read-only entry observers reject a caller with a moved enum field");
}

void testBoundedSecondaryFailureRecordStorage() {
  std::string source = R"gti(
class Guard {
  int divisor;
public:
  Guard(int divisor) : divisor(divisor) {}
  ~Guard() { int checked = 1 / this.divisor; }
};
int many(int divisor) {
)gti";
  for (int index = 0; index != 32; ++index)
    source += "  Guard guard" + std::to_string(index) + "{divisor};\n";
  source += "  return 0;\n}\nint main() { return many(1); }\n";
  const auto frontend = analyze("bounded-secondary-records.gti", source);
  expect(frontend.canGenerateCode(), "many fallible guards must lower");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t checkedBodies = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.owner || function.parameterTypes.size() != 1)
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const auto text =
        emitter.emitFailureBodyText(address, "bounded-records", 1);
    expect(text.emitted(), "many guards must emit through verified MIR");
    const auto occurrences = [&](std::string_view needle) {
      std::size_t count = 0;
      for (std::size_t position = text.text.find(needle);
           position != std::string::npos;
           position = text.text.find(needle, position + needle.size()))
        ++count;
      return count;
    };
    std::size_t secondaryDrops = 0;
    std::size_t terminalEdges = 0;
    std::size_t primaryEntries = 0;
    std::size_t fallibleDrops = 0;
    for (const auto &block : function.body.blocks) {
      // A shared cleanup continuation runs under the context its entering
      // producer already activated.
      const auto *entered =
          function.body.findFailureRecord(block.failureParameter);
      primaryEntries += block.activeFailure != 0 &&
                        block.failureParameter == block.activeFailure &&
                        entered != nullptr &&
                        entered->kind == lang::MirFailureRecordKind::Producer;
      terminalEdges += block.terminator.kind ==
                       lang::MirTerminatorKind::TerminateCleanupFailure;
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
          ++fallibleDrops;
          secondaryDrops += block.activeFailure != 0;
          expect(text.text.find("bool __gti_mir_drop_success_" +
                                std::to_string(instruction.id) + ";") !=
                     std::string::npos,
                 "each paired Drop status is defined at its producer, not "
                 "initialized at entry for native phi construction");
        }
    }
    // Every failure edge reaches one shared reverse chain, so each guard has
    // exactly one fallible cleanup drop however many failures keep it live.
    expect(secondaryDrops >= 31 && secondaryDrops <= 32 &&
               fallibleDrops > secondaryDrops &&
               terminalEdges == secondaryDrops,
           "the fixture shares one cleanup-failure alternative per guard");
    expect(
        occurrences("::gti_failure_record_v2 "
                    "__gti_mir_cleanup_failure_record{};") == 1 &&
            occurrences("mir_failure_cleanup_scope_v1 "
                        "__gti_mir_cleanup_context;") == 1 &&
            occurrences("__gti_mir_cleanup_context.activate(") ==
                primaryEntries &&
            occurrences("std::memset(&__gti_mir_cleanup_failure_record,") ==
                secondaryDrops &&
            occurrences("mir_terminate_cleanup_failure_v2(") == terminalEdges &&
            text.text.find("__gti_mir_cleanup_failure_record_") ==
                std::string::npos &&
            text.text.find("::gti_failure_emergency_v2") == std::string::npos,
        "all alternatives share one fresh secondary record and one scope "
        "activated only at primary entries; terminal envelopes belong to the "
        "out-of-line helper frame");
    ++checkedBodies;
  }
  expect(checkedBodies == 1,
         "the many-guard body must be checked exactly once");
}

void testTypedCleanupAdapterInterning() {
  const auto frontend =
      analyzeWithStandardLibrary("typed-cleanup-adapters.gti", R"gti(
class LeftError {
  int divisor;
public:
  LeftError(int value) : divisor(value) {}
  ~LeftError() { int left_failure = 1 / this.divisor; }
};
class RightError {
  int divisor;
public:
  RightError(int value) : divisor(value) {}
  ~RightError() { int right_failure = 2 / this.divisor; }
};
enum LeftResult { value(int payload), error(LeftError failure) };
enum OtherLeftResult { value(bool payload), error(LeftError failure) };
enum RightResult { value(int payload), error(RightError failure) };
int many(int divisor) {
  LeftResult first = LeftResult::error(LeftError(divisor));
  LeftResult second = LeftResult::error(LeftError(divisor));
  OtherLeftResult different_type = OtherLeftResult::error(LeftError(divisor));
  RightResult different_recipe = RightResult::error(RightError(divisor));
  return 3 / divisor;
}
int main() { return many(1); }
)gti");
  expect(frontend.canGenerateCode(),
         "unrelated source error types must lower with repeated typed cleanup");
  if (!frontend.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    std::size_t checkedBodies = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      const auto *hir = frontend.hir.findFunctionInstance(function.id);
      if (!hir || !hir->source || hir->source->name().lexeme != "many")
        continue;
      std::vector<lang::MirDropType> recipes;
      std::size_t drops = 0;
      std::size_t secondaryFailures = 0;
      for (const auto &block : function.body.blocks) {
        secondaryFailures += block.terminator.kind ==
                             lang::MirTerminatorKind::TerminateCleanupFailure;
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              instruction.lifecycle.size() != 1)
            continue;
          const auto *drop = function.body.findDropObligation(
              instruction.lifecycle.front().source);
          if (!drop || (drop->dropType.type.kind != lang::SemanticType::Class &&
                        drop->dropType.type.kind != lang::SemanticType::Enum))
            continue;
          ++drops;
          if (std::find(recipes.begin(), recipes.end(), drop->dropType) ==
              recipes.end())
            recipes.push_back(drop->dropType);
        }
      }
      if (drops == 0)
        continue;
      const auto original = function;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const auto plan = emitter.buildPlan(address);
      const auto text = emitter.emitFailureBodyText(plan, "typed-adapters", 1);
      expect(text.emitted(), "typed cleanup adapter body must be admitted");
      const auto occurrences = [&](std::string_view needle) {
        std::size_t count = 0;
        for (std::size_t position = text.text.find(needle);
             position != std::string::npos;
             position = text.text.find(needle, position + needle.size()))
          ++count;
        return count;
      };
      expect(recipes.size() >= 3 && drops > recipes.size(),
             "fixture distinguishes equal recipes, equal cleanup expressions "
             "with different native types, and different source error recipes");
      expect(occurrences("const auto __gti_mir_cleanup_adapter_") ==
                     recipes.size() &&
                 occurrences(".destroy_cleanup_with_failure(") == drops &&
                 occurrences("[](auto &__gti_cleanup_value,") == recipes.size(),
             "each exact typed recipe owns one captureless adapter but every "
             "verified Drop still invokes it separately");
      expect(text.text.find("const auto __gti_mir_cleanup_adapter_") <
                     text.text.find("goto __gti_mir_bb_") &&
                 occurrences("mir_terminate_cleanup_failure_v2(") ==
                     secondaryFailures,
             "adapter initialization precedes every label and preserves all "
             "secondary-failure termination edges");
      for (const auto &block : function.body.blocks)
        expect(occurrences("__gti_mir_bb_" + std::to_string(block.id) +
                           ": {") == 1,
               "adapter sharing cannot coalesce or invent MIR blocks");
      expect(emitter.emitFailureBodyText(address, "typed-adapters", 1).text ==
                     text.text &&
                 emitter.emitFailureBodyText(plan, "typed-adapters", 1).text ==
                     text.text &&
                 function == original,
             "per-body inventories are deterministic, sealed in the detached "
             "plan, and cannot mutate canonical MIR");
      ++checkedBodies;
    }
    expect(checkedBodies == 1,
           "the repeated cleanup body is checked once per native standard");
  }
}

void testPropagationOnlyFailureContext() {
  const auto frontend = analyze("propagation-only-context.gti", R"gti(
int divide(int value) { return 12 / value; }
int forward(int value) { return divide(value); }
class Accessor {
  int value;
public:
  Accessor(int value) : value(value) {}
  int read() { return forward(this.value); }
};
class Resource {
  int value;
public:
  Resource(int value) : value(value) {}
  ~Resource() { int checked = forward(this.value); }
};
int guarded(int value) {
  Resource resource{value};
  return forward(value);
}
int main() { Accessor accessor{2}; return guarded(accessor.read()); }
)gti");
  expect(frontend.canGenerateCode(),
         "general accessor and resource must lower");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t erased = 0;
  std::size_t retained = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.mayRaiseDefinedFailure)
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const auto text = emitter.emitFailureBodyText(address, "context", 1);
    expect(text.emitted(), "failure context must emit from verified MIR");
    bool hasCleanup = false;
    const lang::MirBlock *entry = nullptr;
    for (const auto &block : function.body.blocks) {
      if (block.activeFailure == 0)
        continue;
      if (block.failureParameter == block.activeFailure)
        entry = &block;
      for (const auto &instruction : block.instructions)
        hasCleanup |= instruction.kind == lang::MirInstructionKind::Drop;
    }
    expect(entry != nullptr, "fixture must retain a primary failure entry");
    const bool hasContext =
        text.text.find("__gti_mir_cleanup_context") != std::string::npos;
    expect(hasContext == hasCleanup,
           "only executable cleanup requires a local primary context");
    if (hasCleanup) {
      ++retained;
    } else {
      ++erased;
      // A detector's cold publication helper runs the same check after it
      // writes the record; other failure edges call the check directly.
      expect(text.text.find("mir_check_incoming_cleanup_failure_v1(") !=
                     std::string::npos ||
                 text.text.find("mir_publish_failure_v1(") != std::string::npos,
             "propagation-only functions still detect an ancestor's primary "
             "failure when they fail inside cleanup");
    }
    if (entry != nullptr) {
      auto forged = frontend.mir;
      auto *body = lang::findMirBody(forged, address);
      body->findBlock(entry->id)->activeFailure = 0;
      expect(!lang::verifyMirProgram(forged).valid(),
             "removing the active record must not conceal cleanup effects");
    }
  }
  expect(erased >= 3 && retained >= 1,
         "leaf, forwarding and user accessor elision plus resource retention "
         "must all be exercised");
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

void testLargeScalarAggregateAdmission() {
  // A ROM-sized aggregate must not be mistaken for an owning Expected
  // extraction candidate at each scalar element. Keep this structural test
  // threshold-free; paired benchmark runs measure the admission cost.
  // Mix unrelated scalar values with real class temporary receivers and a
  // borrowed argument. Necessary-type exclusions must not suppress an
  // applicable materialization or its exact cleanup schedule.
  std::string source = R"(
class Token {
  mut int32_t value;
public:
  Token(int32_t initial) : value(initial) {}
  ~Token() {}
  int32_t read() { return this.value; }
  int32_t update() mut { this.value++; return this.value; }
};
int32_t observe(Token& token) { return token.read(); }
int32_t main() { int32_t rom[1024] = {
)";
  for (int index = 0; index < 1024; ++index) {
    if (index != 0)
      source += ",";
    source += std::to_string(index);
  }
  source += R"(
};
  int32_t answer = Token(7).read() + Token(8).update() + observe(Token(9));
  return rom[1023] - 1023 + answer - 25;
}
)";
  const auto frontend = analyze("large-scalar-aggregate.gti", source);
  expect(frontend.canGenerateCode(), "ROM-sized scalar aggregate must verify");
  if (!frontend.canGenerateCode())
    return;
  expect(
      !gti_test::emitCpp(frontend, frontend.mir, frontend.mir).contents.empty(),
      "ROM-sized scalar aggregate must pass complete production admission");

  auto malformed = frontend.mir;
  bool changed = false;
  for (const auto &function : malformed.functionInstances()) {
    auto *body = const_cast<lang::MirFunctionInstance *>(
        malformed.findFunctionInstance(function.id));
    for (auto &block : body->body.blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::Aggregate &&
            instruction.operands.size() == 1024) {
          instruction.operands.pop_back();
          changed = true;
        }
      }
    }
  }
  expect(changed && !lang::verifyMirProgram(malformed).valid(),
         "fast rejection of unrelated ownership patterns must not admit an "
         "incomplete aggregate");
  bool rejected = false;
  try {
    static_cast<void>(gti_test::emitCpp(frontend, malformed, malformed));
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(rejected, "production admission must reject the incomplete aggregate");
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

void testConstructionOnlyPayloadEnumFieldAdmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/construction_only_payload_enum.gti";
  std::ifstream input(path);
  expect(input.good(), "construction-only enum fixture must exist");
  std::string source{std::istreambuf_iterator<char>(input), {}};
  // A mutex payload can move but cannot be assigned. Its owning enum now has
  // whole-value replacement, but initializing a containing field still needs
  // its exact construction/publication proof, not an assignment shortcut.
  const std::string original = "std::optional<std::string>";
  const std::string replacement = "std::mutex<std::string>";
  for (std::size_t offset = 0;
       (offset = source.find(original, offset)) != std::string::npos;
       offset += replacement.size())
    source.replace(offset, original.size(), replacement);
  source = "#include <std/mutex>\n" + source;
  // The mutex's address stability is separate from construction-only field
  // publication; it is not a relocatable vector element.
  const auto vectorStart = source.find("  mut std::vector<Event> events{};");
  const auto vectorEnd = source.find("  mut std::optional<Event> selected");
  expect(vectorStart != std::string::npos && vectorEnd > vectorStart,
         "construction-only control identifies its unrelated vector section");
  if (vectorStart != std::string::npos && vectorEnd != std::string::npos &&
      vectorEnd > vectorStart)
    source.erase(vectorStart, vectorEnd - vectorStart);
  const auto optionalStart = source.find("  mut std::optional<Event> selected");
  const auto optionalEnd = source.find("\n}", optionalStart);
  expect(optionalStart != std::string::npos && optionalEnd > optionalStart,
         "construction-only control identifies its unrelated optional section");
  if (optionalStart != std::string::npos && optionalEnd != std::string::npos &&
      optionalEnd > optionalStart)
    source.erase(optionalStart, optionalEnd - optionalStart);
  for (const std::string name : {"cleanup_divisor", "helper_divisor"}) {
    const auto offset = source.find("mut int32_t " + name);
    expect(offset != std::string::npos,
           "construction-only control retains its named divisor");
    if (offset != std::string::npos)
      source.erase(offset, 4);
  }
  auto target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = true;
  const auto standardLibrary =
      path.parent_path().parent_path().parent_path() / "stdlib";
  const auto frontend =
      lang::Frontend({.target = target})
          .analyze(path.string(), source, {standardLibrary / "prelude.gti"}, {},
                   {standardLibrary});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "construction-only enum fields must pass semantic and MIR checks");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23, target));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "fallible owners of construction-only enums need a complete backend "
         "representation, not an assignment capability");
  bool foundFresh = false;
  bool foundMove = false;
  bool foundCall = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *field =
            instruction.destination
                ? constructor.body.findPlace(*instruction.destination)
                : nullptr;
        const auto *enumeration =
            field && field->type.kind == lang::SemanticType::Enum
                ? frontend.mir.findPayloadEnum(field->type.enumId)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !field || field->root != lang::MirPlaceRootKind::This ||
            !enumeration || !enumeration->requiresActiveCleanup ||
            !enumeration->traits.movable ||
            !enumeration->traits.moveAssignable ||
            instruction.lifecycle.size() != 1 ||
            instruction.lifecycle.front().kind !=
                lang::MirLifecycleEventKind::Reparent)
          continue;
        const auto *value =
            constructor.body.findValue(instruction.operands.front().value);
        const auto *producer =
            value
                ? findPreflightInstruction(constructor.body, value->definition)
                : nullptr;
        const bool moved = producer != nullptr &&
                           producer->kind == lang::MirInstructionKind::Move;
        const bool called = producer != nullptr &&
                            producer->kind == lang::MirInstructionKind::Call;
        foundMove = foundMove || moved;
        foundCall = foundCall || called;
        foundFresh = foundFresh || (producer != nullptr &&
                                    producer->operation ==
                                        lang::MirOperation::PayloadConstruct);
        const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                           constructor.id};
        expect(emitter.supportsFailureBodyText(address),
               "construction-only enum publication must retain its failure "
               "and rollback schedule");
        if (emitter.supportsFailureBodyText(address)) {
          const auto text =
              emitter.emitFailureBodyText(address, "construction-only-enum", 1);
          const auto fieldSpelling = "__gti_mir_p_" + std::to_string(field->id);
          const auto *sourceDrop = constructor.body.findDropObligation(
              instruction.lifecycle.front().source);
          const auto sourceSpelling =
              "__gti_mir_p_" + std::to_string(sourceDrop->place);
          const auto retirement = text.text.find(
              "std::destroy_at(std::addressof(" + fieldSpelling + "));");
          const auto publication = text.text.find(
              "::gti_internal::backend::construct_representation_at(std::"
              "addressof(" +
              fieldSpelling + "), std::move(" + sourceSpelling + ".get()));");
          const auto sourceRetirement = text.text.find(
              sourceSpelling + ".destroy(); // end transferred "
                               "payload-enum field representation");
          if (moved) {
            // An exact owned-parameter transfer now constructs the field in
            // the native initializer list. Its MIR publication is erased,
            // but the field's rollback obligation remains authoritative.
            const auto bindings = lang::cppMirOwnedParameterFieldBindings(
                frontend.mir, constructor);
            const auto *binding = bindings && bindings->size() == 1
                                      ? &bindings->front()
                                      : nullptr;
            expect(
                binding != nullptr &&
                    binding->moveInstruction == producer->id &&
                    binding->movedValue == instruction.operands.front().value &&
                    binding->initializeInstruction == instruction.id &&
                    binding->valueDrop ==
                        instruction.lifecycle.front().source &&
                    binding->fieldDrop ==
                        instruction.lifecycle.front().target &&
                    field->projections.size() == 1 &&
                    binding->field == field->projections.front().field,
                "native enum publication must prove the exact parameter move, "
                "field initialization, and rollback transfer");
            expect(text.emitted() &&
                       text.text.find(
                           "// move " + std::to_string(*producer->result) +
                           " publishes in the member initializer list") !=
                           std::string::npos &&
                       text.text.find(
                           "// field initialization publishes in the "
                           "member initializer list") != std::string::npos &&
                       text.text.find("}(" + fieldSpelling +
                                      "); // GTI MIR field rollback") !=
                           std::string::npos &&
                       retirement == std::string::npos &&
                       publication == std::string::npos &&
                       sourceRetirement == std::string::npos &&
                       text.text.find(sourceSpelling + ".construct(") ==
                           std::string::npos,
                   "native enum publication must retain field rollback without "
                   "constructing or retiring a duplicate value representation");
          } else if (!called) {
            const auto direct =
                text.text.find("::new (static_cast<void *>(std::addressof(" +
                               fieldSpelling + "))) ");
            expect(text.emitted() && retirement != std::string::npos &&
                       direct != std::string::npos && retirement < direct &&
                       text.text.find("// GTI MIR in-place payload-enum field "
                                      "reparent into p" +
                                      std::to_string(field->id)) !=
                           std::string::npos &&
                       sourceRetirement == std::string::npos &&
                       text.text.find(sourceSpelling + ".construct(") ==
                           std::string::npos &&
                       text.text.find(sourceSpelling + ".construct_from(") ==
                           std::string::npos,
                   "a fresh adjacent alternative must construct directly in "
                   "its empty field while retaining the MIR reparent");
          } else {
            expect(text.emitted() && retirement != std::string::npos &&
                       publication != std::string::npos &&
                       retirement < publication &&
                       sourceRetirement != std::string::npos &&
                       publication < sourceRetirement,
                   "publication must replace the private empty enum shell by "
                   "construction and retire its transferred source "
                   "representation");
          }
        }
        for (int mutation = 0; mutation != (called ? 6 : 3); ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          auto *publication = findPreflightInstruction(*body, instruction.id);
          if (mutation == 0)
            publication->lifecycle.front().target = 0;
          else if (mutation == 1)
            body->dropObligations[publication->lifecycle.front().target - 1]
                .initiallyActive = true;
          else if (mutation == 2 && !called)
            findPreflightInstruction(*body, producer->id)->lifecycle.clear();
          else if (mutation == 3)
            findPreflightInstruction(*body, producer->id)->successResultDrop =
                publication->lifecycle.front().target;
          else if (mutation == 5)
            findPreflightInstruction(*body, producer->id)->functionTarget = 0;
          else {
            for (auto &producerBlock : body->blocks) {
              if (producerBlock.terminator.kind !=
                      lang::MirTerminatorKind::Invoke ||
                  producerBlock.terminator.invokeInstruction != producer->id)
                continue;
              if (mutation == 2)
                producerBlock.terminator.successLifecycle.clear();
              else
                producerBlock.terminator.successLifecycle.front().target =
                    publication->lifecycle.front().target;
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid() &&
                     !lang::CppMirBodyEmitter(malformed, map)
                          .supportsFailureBodyText(address),
                 "construction-only enum publication must reject a lost "
                 "reparent, prematurely active field, erased/forged success "
                 "activation or unavailable source target");
        }
      }
    }
  }
  expect(foundFresh && foundMove && foundCall,
         "fixture must exercise constructed, moved and helper-returned "
         "enum fields with nonassignable payloads");
}

void testPassiveEnumTemporaryBorrowAdmission() {
  const auto frontend = analyze("passive-enum-temporary-borrow.gti", R"gti(
enum Token { item(int value), empty };
int inspect(Token& token) {
  switch (token) {
    case Token::item(value): return value;
    case Token::empty: return 0;
  }
}
Token make(int value) { return Token::item(value); }
int main() { return inspect(Token::item(7)) + inspect(make(5)) - 12; }
)gti");
  expect(frontend.canGenerateCode(),
         "an ordinary observer may borrow a passive enum temporary");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "a passive enum temporary must have a complete representation");
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const auto text = emitter.supportsFailureBodyText(address)
                          ? emitter.emitFailureBodyText(address, "enum-view", 1)
                          : emitter.emitBodyText(address, "enum-view", 1);
    expect(text.emitted(), "the checked enum read view must emit");
    for (const auto &loan : function.body.loans) {
      const auto *place = function.body.findPlace(loan.source);
      if (loan.kind != lang::MirLoanKind::CallArgument || !place ||
          place->type.kind != lang::SemanticType::Enum)
        continue;
      ++checked;
      expect(text.text.find("__gti_mir_v_" + std::to_string(place->value)) !=
                     std::string::npos &&
                 text.text.find("__gti_mir_p_" + std::to_string(place->id) +
                                ";") == std::string::npos,
             "a temporary read view reuses the passive SSA object");
      for (int mutation = 0; mutation != 11; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        auto &changedLoan = body->loans[loan.id - 1];
        auto &changedPlace = body->places[place->id - 1];
        lang::MirInstruction *stage = nullptr;
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.loan == loan.id)
              stage = &instruction;
        auto drop = std::find_if(body->dropObligations.begin(),
                                 body->dropObligations.end(),
                                 [&](const auto &candidate) {
                                   return candidate.place == place->id;
                                 });
        expect(stage && drop != body->dropObligations.end(),
               "the proof mutation must retain its exact stage and drop");
        if (!stage || drop == body->dropObligations.end())
          continue;
        switch (mutation) {
        case 0:
          changedPlace.sourceValue = 0;
          break;
        case 1:
          changedLoan.producedBy = 0;
          break;
        case 2:
          changedLoan.escapes = true;
          break;
        case 3:
          changedLoan.access = lang::AccessMode::Mutable;
          break;
        case 4:
          stage->operands.front().place = 0;
          break;
        case 5:
          stage->loan.reset();
          break;
        case 6:
          drop->fullExpression = 0;
          break;
        case 7:
          drop->value = 0;
          break;
        case 8:
          drop->dropType.requiresActiveCleanup = true;
          break;
        case 9: {
          const auto *value = body->findValue(place->value);
          findPreflightInstruction(*body, value->definition)->lifecycle.clear();
          break;
        }
        case 10:
          changedPlace.initiallyAvailable = true;
          break;
        }
        const lang::CppMirBodyEmitter malformedEmitter(malformed, map);
        expect(!lang::verifyMirProgram(malformed).valid() &&
                   !malformedEmitter.supportsBodyText(address) &&
                   !malformedEmitter.supportsFailureBodyText(address),
               "temporary admission rejects missing or forged producer, "
               "materialization, full-expression and call-loan proof " +
                   std::to_string(mutation));
      }
    }
  }
  expect(checked == 2,
         "ordinary observers must test constructed and returned temporaries");
}

void testPassiveEnumTemporaryObservationThenTransfer() {
  const auto frontend =
      analyzeWithStandardLibrary("enum-observe-transfer.gti", R"gti(
struct Rejected { int code; Rejected(int value) : code(value) {} };
struct Status {
  int code;
  Status(int value) : code(value) {}
  implicit Status(Rejected value) : code(value.code) {}
};
enum Parse { ok(int value), failed(int code) };
for concept std::tryable<Parse> {
  using value_type = int;
  using residual_type = Rejected;
  bool has_value(Parse& value) {
    switch (value) {
      case Parse::ok: return true;
      case Parse::failed: return false;
    }
  }
  int take_value(Parse value) {
    switch (value) {
      case Parse::ok(item): return item;
      case Parse::failed: return 0;
    }
  }
  Rejected take_residual(Parse value) {
    switch (value) {
      case Parse::ok: return Rejected(0);
      case Parse::failed(code): return Rejected(code);
    }
  }
};
Status parse() { int value = ?Parse::ok(7); return Status(value); }
int main() { return parse().code - 7; }
)gti");
  expect(frontend.canGenerateCode(),
         "a passive enum temporary may be observed then transferred by value");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "observation and later passive transfer must share one SSA object");
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.body.loans.empty())
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    auto withPassiveRead = frontend.mir;
    auto *body = lang::findMirBody(withPassiveRead, address);
    lang::MirInstructionId nextInstruction = 1;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        nextInstruction = std::max(nextInstruction, instruction.id + 1);
    bool inserted = false;
    lang::MirValueId readResult = 0;
    lang::MirValueId readSource = 0;
    for (auto &block : body->blocks) {
      for (auto at = block.instructions.begin(); at != block.instructions.end();
           ++at) {
        if (at->kind != lang::MirInstructionKind::Move ||
            at->info.type.kind != lang::SemanticType::Enum)
          continue;
        // Ordinary enum temporary transport also represents an unused
        // passive read; it is not limited to reads feeding value stages.
        auto extra = *at;
        extra.id = nextInstruction;
        extra.kind = lang::MirInstructionKind::Load;
        extra.operands.front().kind = lang::MirOperandKind::Copy;
        extra.result = body->values.size() + 1;
        readResult = *extra.result;
        const auto *source = body->findPlace(extra.operands.front().place);
        readSource = source ? source->value : 0;
        body->values.push_back({.id = *extra.result,
                                .sourceValue = extra.hirValue,
                                .info = extra.info,
                                .definitionBlock = block.id,
                                .definition = extra.id});
        block.instructions.insert(at, std::move(extra));
        inserted = true;
        break;
      }
      if (inserted)
        break;
    }
    if (!inserted)
      continue;
    ++checked;
    expect(lang::rebuildMirValueUses(*body),
           "the passive read must have an exact SSA use inventory");
    const auto verification = lang::verifyMirProgram(withPassiveRead);
    for (const auto &error : verification.errors)
      std::cerr << "passive read fixture: " << error.message << '\n';
    expect(verification.valid(),
           "an unused passive read is valid MIR, not a missing lifetime proof");
    const lang::CppMirBodyEmitter readEmitter(withPassiveRead, map);
    const auto text =
        readEmitter.supportsFailureBodyText(address)
            ? readEmitter.emitFailureBodyText(address, "enum-read", 1)
            : readEmitter.emitBodyText(address, "enum-read", 1);
    expect(text.emitted() && readResult != 0 && readSource != 0 &&
               text.text.find("__gti_mir_v_" + std::to_string(readResult) +
                              " = __gti_mir_v_" + std::to_string(readSource) +
                              ";") != std::string::npos,
           "ordinary temporary transport emits the exact passive SSA copy");
  }
  expect(checked == 1,
         "one observed temporary must exercise ordinary passive transport");
}

void testFreshClassPayloadOperandStaging() {
  const auto native = analyze("native-payload-call-staging.gti", R"gti(
[[c_abi]] struct NativeValue { int stamp; };
extern "C" { NativeValue produce(int stamp); }
enum NativeResult { pair(NativeValue first, NativeValue second), empty };
int main() {
  unsafe {
    auto result = NativeResult::pair(produce(1), produce(2));
    switch (result) {
      case NativeResult::pair(first, second):
        return first.stamp + second.stamp - 3;
      case NativeResult::empty: return 1;
    }
  }
}
)gti");
  expect(native.canGenerateCode(), "direct native payload calls must lower");
  if (native.canGenerateCode()) {
    const lang::CppMirBodyEmissionMap rows(buildRows(native, native.mir));
    const lang::CppMirBodyEmitter backend(native.mir, rows);
    expect(backend.analyzeProgram().ready(),
           "native payload calls must reuse exact passive SSA staging");
    for (const auto &function : native.mir.functionInstances()) {
      if (function.entryKind == lang::ProgramEntryKind::None)
        continue;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = native.mir;
        auto *body = lang::findMirBody(malformed, address);
        bool changed = false;
        for (auto &block : body->blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.operation != lang::MirOperation::PayloadConstruct)
              continue;
            if (mutation == 0)
              instruction.localFailureSites.push_back(1);
            else if (mutation == 1)
              instruction.operands.front().type = lang::SemanticType::Int32;
            else if (mutation == 2)
              instruction.enumVariant = 1;
            else
              instruction.operands.back() = instruction.operands.front();
            changed = true;
          }
        }
        const lang::CppMirBodyEmitter forged(malformed, rows);
        expect(changed && !forged.supportsBodyText(address) &&
                   !forged.supportsFailureBodyText(address),
               "native payload staging rejects failure, wrong type/tag and "
               "duplicated result use");
      }
    }
  }
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/payload_fresh_class_operands.gti";
  std::ifstream input(path);
  expect(input.good(), "fresh class payload operand fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "fresh class payload operands must pass semantic and MIR checks");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "fresh class payload operands need a complete backend "
         "representation");
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &candidate :
       frontend.mir.functionInstances()) {
    if (candidate.entryKind != lang::ProgramEntryKind::None)
      entry = &candidate;
  }
  expect(entry != nullptr, "the fixture must retain its entry body");
  if (entry == nullptr)
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const bool failureForm = emitter.supportsFailureBodyText(address);
  expect(failureForm || emitter.supportsBodyText(address),
         "the entry body staging fresh payload operands must be emittable");
  const auto text =
      failureForm ? emitter.emitFailureBodyText(address, "payload-fresh", 1)
                  : emitter.emitBodyText(address, "payload-fresh", 1);

  // Each staged operand is constructed at its own MIR position, moved into
  // the alternative, and only then has its moved-from shell destroyed.
  const std::string retirement =
      ".destroy(); // end payload operand representation";
  std::vector<lang::MirValueId> staged;
  bool ordered = true;
  for (std::size_t at = text.text.find(retirement); at != std::string::npos;
       at = text.text.find(retirement, at + 1)) {
    const std::size_t begin = text.text.rfind("__gti_mir_v_", at);
    const std::string name = text.text.substr(begin, at - begin);
    // Failure-free constructors publish with construct(); failure-capable
    // ones construct in place at construction_address() before marking it.
    const std::size_t construct =
        std::min(text.text.find(name + ".construct("),
                 text.text.find(name + ".construction_address()"));
    const std::size_t moved = text.text.find("std::move(" + name + ".get())");
    ordered = ordered && construct != std::string::npos &&
              moved != std::string::npos && construct < moved && moved < at;
    staged.push_back(
        std::stoul(name.substr(std::string("__gti_mir_v_").size())));
  }
  expect(text.emitted() && staged.size() >= 4 && ordered,
         "non-adjacent fresh class payload operands must be constructed, "
         "moved into the alternative, and then retired");

  // Forge the consuming PayloadConstruct of each staged operand: a local
  // failure site or a drifted operand type removes the slot's proof, and the
  // emitter must decline rather than spell an unproven transfer.
  std::size_t rejected = 0;
  std::size_t forged = 0;
  for (const lang::MirValueId value : staged) {
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      lang::MirInstruction *consumer = nullptr;
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.operation == lang::MirOperation::PayloadConstruct)
            for (const auto &operand : instruction.operands)
              if (operand.kind == lang::MirOperandKind::Value &&
                  operand.value == value)
                consumer = &instruction;
      if (consumer == nullptr)
        continue;
      ++forged;
      if (mutation == 0) {
        consumer->localFailureSites.push_back(1);
      } else {
        for (auto &operand : consumer->operands)
          if (operand.kind == lang::MirOperandKind::Value &&
              operand.value == value)
            operand.type = lang::SemanticType::Int32;
      }
      const lang::CppMirBodyEmitter forgedEmitter(malformed, map);
      rejected += !(failureForm ? forgedEmitter.supportsFailureBodyText(address)
                                : forgedEmitter.supportsBodyText(address));
    }
  }
  expect(forged == staged.size() * 2 && rejected == forged,
         "a staged payload operand whose consumer gains a failure site or "
         "loses its exact operand type must fail closed");
}

void testOwningPayloadEnumTransportAdmissionFailsClosed() {
  testOwningPayloadEnumFieldContainmentAdmission();
  testConstructionOnlyPayloadEnumFieldAdmission();

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

holder direct_fresh_return(int32_t divisor) {
  int32_t checked = int32_t(1) / divisor;
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
            ? frontend.mir.findPayloadEnum(type)
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
  // Exact typed lambda invocations use the same ordered CallInput schedule.
  StagedShape lambdaShape;
  StagedShape lambdaFreshShape;
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
            if (consumer->lambdaTarget &&
                producer->kind == lang::MirInstructionKind::Move) {
              lambdaShape = shape;
            } else if (consumer->lambdaTarget &&
                       producer->kind == lang::MirInstructionKind::Compute &&
                       producer->operation ==
                           lang::MirOperation::PayloadConstruct) {
              lambdaFreshShape = shape;
            } else if (producer->kind == lang::MirInstructionKind::Move) {
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
      }
    }
  }
  expect(namedShape && freshShape && directConstructorShape && lambdaShape &&
             lambdaFreshShape && directFreshReturnShape,
         "the transport fixture should retain prepared named and constructor, "
         "staged-fresh, both staged exact-lambda, and direct fresh-return "
         "owning payload-enum schedules");
  if (!namedShape || !freshShape || !directConstructorShape || !lambdaShape ||
      !lambdaFreshShape || !directFreshReturnShape) {
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
             admits(lambdaShape.function, "named exact lambda") &&
             admits(lambdaFreshShape.function, "fresh exact lambda") &&
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

  // The staged exact-lambda schedule closes the same lifecycle proof as an
  // ordinary call: the prepared parameter drop leaves only at the invocation.
  const auto forgeLambdaTransfer = [&](const StagedShape &shape) {
    return forge(shape.function, [&](lang::MirBody &body) {
      lang::MirInstruction *stage = findMutableInstruction(body, shape.stage);
      lang::MirInstruction *call = findMutableInstruction(body, shape.consumer);
      expect(stage != nullptr && stage->preparedParameterDrop &&
                 call != nullptr,
             "the exact lambda argument should retain its prepared stage and "
             "call");
      if (stage == nullptr || !stage->preparedParameterDrop ||
          call == nullptr) {
        return;
      }
      const auto transfer = std::find_if(
          call->lifecycle.begin(), call->lifecycle.end(),
          [&](const lang::MirLifecycleEvent &candidate) {
            return candidate.kind == lang::MirLifecycleEventKind::TransferOut &&
                   candidate.source == *stage->preparedParameterDrop;
          });
      expect(transfer != call->lifecycle.end(),
             "the exact lambda call should transfer the prepared argument");
      if (transfer != call->lifecycle.end()) {
        transfer->kind = lang::MirLifecycleEventKind::Initialize;
      }
    });
  };
  expectRejected(forgeLambdaTransfer(lambdaShape), lambdaShape.function,
                 "a staged exact-lambda argument without its unique "
                 "TransferOut edge must fail admission");
  expectRejected(forgeLambdaTransfer(lambdaFreshShape),
                 lambdaFreshShape.function,
                 "a staged fresh exact-lambda argument without its unique "
                 "TransferOut edge must fail admission");

  const lang::MirProgram wrongLambdaMoveLifecycle =
      forge(lambdaShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *move =
            findMutableInstruction(body, lambdaShape.producer);
        expect(move != nullptr && move->lifecycle.size() == 1,
               "the exact-lambda argument move should retain one lifecycle "
               "edge");
        if (move != nullptr && move->lifecycle.size() == 1) {
          move->lifecycle.front().kind =
              lang::MirLifecycleEventKind::Initialize;
        }
      });
  expectRejected(wrongLambdaMoveLifecycle, lambdaShape.function,
                 "an exact lambda argument whose Move producer has the "
                 "wrong lifecycle kind must fail admission");

  const lang::MirProgram wrongLambdaFreshProducer =
      forge(lambdaFreshShape.function, [&](lang::MirBody &body) {
        lang::MirInstruction *producer =
            findMutableInstruction(body, lambdaFreshShape.producer);
        expect(producer != nullptr &&
                   producer->operation == lang::MirOperation::PayloadConstruct,
               "the fresh exact-lambda argument should retain its exact "
               "PayloadConstruct producer");
        if (producer != nullptr) {
          producer->operation = lang::MirOperation::Identity;
        }
      });
  expectRejected(wrongLambdaFreshProducer, lambdaFreshShape.function,
                 "a fresh exact-lambda argument produced by a different MIR "
                 "operation must fail admission");

  // The lambda transport boundary is the exact captureless closure that the
  // invocation names; a capture lifecycle could observe the argument.
  lang::MirProgram capturingLambda = frontend.mir;
  {
    const auto &functions = capturingLambda.functionInstances();
    const auto caller =
        std::find_if(functions.begin(), functions.end(),
                     [&](const lang::MirFunctionInstance &function) {
                       return function.id == lambdaShape.function;
                     });
    const lang::MirInstruction *consumer =
        caller == functions.end()
            ? nullptr
            : findInstruction(caller->body, lambdaShape.consumer);
    auto &lambdas = const_cast<std::vector<lang::MirLambdaInstance> &>(
        capturingLambda.lambdaInstances());
    const auto target =
        consumer != nullptr && consumer->lambdaTarget
            ? std::find_if(lambdas.begin(), lambdas.end(),
                           [&](const lang::MirLambdaInstance &lambda) {
                             return lambda.id == *consumer->lambdaTarget;
                           })
            : lambdas.end();
    expect(target != lambdas.end() && target->captureTypes.empty(),
           "the named exact-lambda transport should retain its captureless "
           "lambda target");
    if (target != lambdas.end()) {
      target->captureTypes.push_back(lang::SemanticType::Int32);
    }
  }
  expectRejected(capturingLambda, lambdaShape.function,
                 "a payload-enum CallInput consumed by a capturing lambda "
                 "invocation must fail admission");

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

// A returned owning payload-enum call result constructs in the caller's
// result storage, from an ordinary function or an exact lambda, including
// across a fallible drop that keeps it an unpublished obligation. An exact
// lambda's prepared class argument stays real storage when a later argument
// can fail. The resource has a field and a printing destructor, so each
// shape carries active, fallible cleanup.
void testLambdaOwningTransportAdmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/lambda_owning_transport.gti";
  std::ifstream input(path);
  expect(input.good(), "the lambda owning transport fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "the lambda owning transport fixture must pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "the lambda owning transport fixture needs a complete backend "
         "representation");

  const auto owningPayload = [&](const lang::SemanticType &type) {
    const lang::MirPayloadEnum *enumeration =
        type.kind == lang::SemanticType::Enum
            ? frontend.mir.findPayloadEnum(type)
            : nullptr;
    return enumeration != nullptr && enumeration->requiresActiveCleanup;
  };
  struct Shape {
    lang::HirFunctionInstanceId function = 0;
    lang::MirInstructionId instruction = 0;

    [[nodiscard]] explicit operator bool() const {
      return function != 0 && instruction != 0;
    }
  };
  Shape functionReturn;
  Shape lambdaReturn;
  Shape pendingLambdaReturn;
  Shape preparedArgument;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.definitionKind != lang::MirDefinitionKind::Source)
      continue;
    const lang::MirBody &body = function.body;
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        const auto returned = [&](lang::MirValueId value) {
          return std::any_of(body.blocks.begin(), body.blocks.end(),
                             [&](const lang::MirBlock &candidate) {
                               return candidate.terminator.kind ==
                                          lang::MirTerminatorKind::Return &&
                                      candidate.terminator.value &&
                                      candidate.terminator.value->kind ==
                                          lang::MirOperandKind::Value &&
                                      candidate.terminator.value->value ==
                                          value;
                             });
        };
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.result && owningPayload(instruction.info.type) &&
            returned(*instruction.result)) {
          const bool pending = std::any_of(
              body.dropObligations.begin(), body.dropObligations.end(),
              [&](const lang::MirDropObligation &drop) {
                const lang::MirPlace *place = body.findPlace(drop.place);
                return drop.kind == lang::MirDropObligationKind::ReturnResult &&
                       place != nullptr &&
                       place->root == lang::MirPlaceRootKind::Value &&
                       place->value == *instruction.result;
              });
          Shape &shape = !instruction.lambdaTarget ? functionReturn
                         : pending                 ? pendingLambdaReturn
                                                   : lambdaReturn;
          shape = {.function = function.id, .instruction = instruction.id};
        }
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputKind == lang::HirCallInputKind::MoveValue &&
            instruction.result && instruction.destination &&
            instruction.preparedParameterDrop &&
            instruction.info.type.kind == lang::SemanticType::Class) {
          const auto &uses = body.usesOf(*instruction.result);
          const lang::MirInstruction *consumer =
              uses.size() == 1
                  ? findPreflightInstruction(body, uses.front().instruction)
                  : nullptr;
          const bool failureCleanup = std::any_of(
              body.blocks.begin(), body.blocks.end(),
              [&](const lang::MirBlock &candidate) {
                return std::any_of(
                    candidate.instructions.begin(),
                    candidate.instructions.end(),
                    [&](const lang::MirInstruction &member) {
                      return member.kind == lang::MirInstructionKind::Drop &&
                             member.lifecycle.size() == 1 &&
                             member.lifecycle.front().failureCleanup &&
                             member.lifecycle.front().source ==
                                 *instruction.preparedParameterDrop;
                    });
              });
          if (consumer != nullptr && consumer->lambdaTarget && failureCleanup) {
            preparedArgument = {.function = function.id,
                                .instruction = instruction.id};
          }
        }
      }
    }
  }
  expect(functionReturn && lambdaReturn && pendingLambdaReturn &&
             preparedArgument,
         "the fixture should retain returned function, exact-lambda, and "
         "pending exact-lambda payload-enum results and a prepared lambda "
         "class argument with failure cleanup");
  if (!functionReturn || !lambdaReturn || !pendingLambdaReturn ||
      !preparedArgument) {
    return;
  }

  const auto address = [](lang::HirFunctionInstanceId function) {
    return lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                .owner = function};
  };
  const lang::CppMirBodyEmitter canonical(frontend.mir, map);
  for (const Shape &shape :
       {functionReturn, lambdaReturn, pendingLambdaReturn, preparedArgument}) {
    expect(canonical.supportsFailureBodyText(address(shape.function)),
           "each canonical owning lambda transport body should pass "
           "failure-form admission");
  }

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
    if (found != functions.end()) {
      mutation(found->body);
      expect(lang::rebuildMirValueUses(found->body),
             "the forged owning lambda transport body should retain a "
             "rebuildable use index");
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

  // Without the exact TransferOut the call cannot publish into the caller's
  // storage, and assigning the value would overwrite a nonexistent object.
  for (const Shape &shape : {functionReturn, lambdaReturn}) {
    const lang::MirProgram missingTransfer =
        forge(shape.function, [&](lang::MirBody &body) {
          const lang::MirInstruction *call =
              findPreflightInstruction(body, shape.instruction);
          const lang::MirDropObligationId result =
              call != nullptr && call->successResultDrop
                  ? *call->successResultDrop
                  : 0;
          std::size_t erased = 0;
          for (lang::MirBlock &block : body.blocks) {
            for (lang::MirInstruction &member : block.instructions) {
              if (member.kind != lang::MirInstructionKind::Lifecycle)
                continue;
              erased += std::erase_if(
                  member.lifecycle, [&](const lang::MirLifecycleEvent &event) {
                    return event.kind ==
                               lang::MirLifecycleEventKind::TransferOut &&
                           event.source == result;
                  });
            }
          }
          expect(result != 0 && erased == 1,
                 "the returned payload-enum call result should transfer its "
                 "drop out exactly once");
        });
    expectRejected(missingTransfer, shape.function,
                   "a returned payload-enum call result without its exact "
                   "TransferOut must fail admission");
  }

  // The pending result must be reparented exactly once before the fallible
  // drop, or its rollback has no proven caller-storage object.
  const lang::MirProgram missingReparent =
      forge(pendingLambdaReturn.function, [&](lang::MirBody &body) {
        std::size_t changed = 0;
        for (lang::MirBlock &block : body.blocks) {
          for (lang::MirInstruction &member : block.instructions) {
            for (lang::MirLifecycleEvent &event : member.lifecycle) {
              const lang::MirDropObligation *target =
                  body.findDropObligation(event.target);
              if (event.kind == lang::MirLifecycleEventKind::Reparent &&
                  target != nullptr &&
                  target->kind == lang::MirDropObligationKind::ReturnResult) {
                event.target = event.source;
                ++changed;
              }
            }
          }
        }
        expect(changed == 1, "the pending lambda result should be "
                             "reparented into its return obligation once");
      });
  expectRejected(missingReparent, pendingLambdaReturn.function,
                 "a pending payload-enum lambda result without its exact "
                 "return-obligation reparent must fail admission");

  // The prepared slot proof binds the exact lambda parameter type.
  const lang::MirProgram driftedParameter =
      forge(preparedArgument.function, [&](lang::MirBody &body) {
        const lang::MirInstruction *stage =
            findPreflightInstruction(body, preparedArgument.instruction);
        const auto &uses = body.usesOf(*stage->result);
        lang::MirInstruction *consumer =
            findPreflightInstruction(body, uses.front().instruction);
        consumer->parameterTypes[stage->callInputIndex] =
            lang::SemanticType::Int32;
      });
  expectRejected(driftedParameter, preparedArgument.function,
                 "a prepared lambda class argument whose parameter type "
                 "drifts must fail admission");
}

// A borrowed receiver stays representation-erased across a conditional
// fallible argument after an owning class argument for the same call was
// prepared: freshly constructed by a constructor that cannot fail, or moved
// from another local. Lambda and method receivers share the proof.
void testConditionalOwningArgumentReceiverAdmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/conditional_owning_argument_receivers.gti";
  std::ifstream input(path);
  expect(input.good(), "the conditional owning argument fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "the conditional owning argument fixture must pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "the conditional owning argument fixture needs a complete backend "
         "representation");
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &candidate :
       frontend.mir.functionInstances()) {
    if (candidate.entryKind != lang::ProgramEntryKind::None)
      entry = &candidate;
  }
  expect(entry != nullptr, "the fixture must retain its entry body");
  if (entry == nullptr)
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(address),
         "fresh and moved owning arguments before a conditional fallible "
         "argument should keep lambda and method receivers admissible");

  // Each owning argument is prepared before its call's receiver is consumed:
  // two fresh constructions and two moves of other locals.
  std::vector<lang::MirInstructionId> freshProducers;
  std::vector<lang::MirInstructionId> movedStages;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput ||
          instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
          !instruction.destination || !instruction.preparedParameterDrop ||
          instruction.info.type.kind != lang::SemanticType::Class ||
          instruction.operands.size() != 1)
        continue;
      const lang::MirValue *source =
          entry->body.findValue(instruction.operands.front().value);
      const lang::MirInstruction *producer =
          source == nullptr
              ? nullptr
              : findPreflightInstruction(entry->body, source->definition);
      if (producer != nullptr &&
          producer->kind == lang::MirInstructionKind::Construct &&
          !producer->successResultDrop && producer->lifecycle.size() == 1)
        freshProducers.push_back(producer->id);
      if (producer != nullptr &&
          producer->kind == lang::MirInstructionKind::Move)
        movedStages.push_back(instruction.id);
    }
  }
  expect(freshProducers.size() == 2 && movedStages.size() == 2,
         "the fixture should prepare two fresh and two moved owning "
         "arguments");

  const auto forge = [&](const auto &mutation) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    for (lang::MirFunctionInstance &function : functions) {
      if (function.id != entry->id)
        continue;
      mutation(function.body);
      expect(lang::rebuildMirValueUses(function.body),
             "the forged conditional owning argument body should retain a "
             "rebuildable use index");
    }
    return !lang::CppMirBodyEmitter(forged, map)
                .supportsFailureBodyText(address);
  };
  for (const lang::MirInstructionId id : freshProducers) {
    expect(forge([&](lang::MirBody &body) {
             findPreflightInstruction(body, id)->lifecycle.front().conditional =
                 true;
           }),
           "a fresh owning argument without an exact unconditional "
           "activation must fail admission");
  }
  for (const lang::MirInstructionId id : movedStages) {
    expect(forge([&](lang::MirBody &body) {
             ++findPreflightInstruction(body, id)->callSite;
           }),
           "a moved owning argument staged for a different call must fail "
           "admission");
  }
}

// A borrowed argument stays representation-erased across a later conditional
// argument whose test, or one arm, is a nested call that can fail. The nested
// call's own borrows may name other locals, or read the borrowed local
// itself, but never mutably borrow the borrowed place's root.
void testBorrowedArgumentFallibleConditionAdmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrowed_argument_fallible_condition.gti";
  std::ifstream input(path);
  expect(input.good(), "the fallible condition fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "the fallible condition fixture must pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "the fallible condition fixture needs a complete backend "
         "representation");
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &candidate :
       frontend.mir.functionInstances()) {
    if (candidate.entryKind != lang::ProgramEntryKind::None)
      entry = &candidate;
  }
  expect(entry != nullptr, "the fixture must retain its entry body");
  if (entry == nullptr)
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(address),
         "borrowed arguments before a conditional with a fallible nested "
         "call should stay admissible");

  // The first argument of a nested two-argument test, a read borrow of a
  // local: `sees(first, argv)` borrows the borrowed `first` itself and then
  // `argv`, while `has(argv, "free")` borrows another local and takes a text
  // view.
  const auto nestedReadBorrows = [&](lang::SemanticType::Kind second) {
    std::vector<lang::MirInstructionId> found;
    for (const lang::MirBlock &block : entry->body.blocks) {
      for (const lang::MirInstruction &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            call.info.type.kind != lang::SemanticType::Bool ||
            call.operands.size() != 2 ||
            call.operands.back().type.kind != second)
          continue;
        const lang::MirValue *value =
            call.operands.front().kind == lang::MirOperandKind::Value
                ? entry->body.findValue(call.operands.front().value)
                : nullptr;
        const lang::MirInstruction *input =
            value == nullptr
                ? nullptr
                : findPreflightInstruction(entry->body, value->definition);
        if (input != nullptr &&
            input->kind == lang::MirInstructionKind::CallInput &&
            input->callInputKind == lang::HirCallInputKind::ReadBorrow &&
            input->operands.size() == 1 &&
            input->operands.front().kind == lang::MirOperandKind::BorrowRead)
          found.push_back(input->id);
      }
    }
    return found;
  };
  const auto sameRoot = nestedReadBorrows(lang::SemanticType::Reference);
  const auto otherRoot = nestedReadBorrows(lang::SemanticType::StringView);
  expect(sameRoot.size() == 1,
         "the fixture should read the borrowed local in one nested test");
  expect(!otherRoot.empty(),
         "the fixture should read another local in nested tests");
  if (sameRoot.size() != 1 || otherRoot.empty())
    return;

  const auto admitted = [&](lang::MirInstructionId id) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    for (lang::MirFunctionInstance &function : functions) {
      if (function.id != entry->id)
        continue;
      lang::MirInstruction *input = findPreflightInstruction(function.body, id);
      input->callInputKind = lang::HirCallInputKind::MutableBorrow;
      input->operands.front().kind = lang::MirOperandKind::BorrowWrite;
      expect(lang::rebuildMirValueUses(function.body),
             "the forged fallible condition body should retain a rebuildable "
             "use index");
    }
    return lang::CppMirBodyEmitter(forged, map)
        .supportsFailureBodyText(address);
  };
  expect(admitted(otherRoot.front()),
         "a nested test may mutably borrow a local other than the borrowed "
         "argument's root");
  expect(!admitted(sameRoot.front()),
         "a nested test that mutably borrows the borrowed argument's root "
         "must fail admission");
}

// A borrowed argument stays representation-erased across a later conditional
// argument whose nested call takes an owning class argument by value. Each
// prepared argument hands off by one TransferOut, and a failure between its
// preparation and the nested call destroys it.
void testBorrowedArgumentNestedOwningArgumentAdmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/borrowed_argument_nested_owning_argument.gti";
  std::ifstream input(path);
  expect(input.good(), "the nested owning argument fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(),
         "the nested owning argument fixture must pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "the nested owning argument fixture needs a complete backend "
         "representation");
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &candidate :
       frontend.mir.functionInstances()) {
    if (candidate.entryKind != lang::ProgramEntryKind::None)
      entry = &candidate;
  }
  expect(entry != nullptr, "the fixture must retain its entry body");
  if (entry == nullptr)
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(address),
         "borrowed arguments before a nested call taking an owning argument "
         "should stay admissible");

  // Calls consuming prepared arguments, and the failure-edge drops of
  // prepared arguments that a later argument's failure abandons.
  std::vector<lang::MirInstructionId> handoffs;
  std::vector<lang::MirInstructionId> cleanups;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      const auto prepared = [&](lang::MirDropObligationId id) {
        const lang::MirDropObligation *drop =
            entry->body.findDropObligation(id);
        return drop != nullptr &&
               drop->kind == lang::MirDropObligationKind::PreparedParameter;
      };
      if (instruction.kind == lang::MirInstructionKind::Call &&
          !instruction.lifecycle.empty() &&
          std::all_of(
              instruction.lifecycle.begin(), instruction.lifecycle.end(),
              [&](const lang::MirLifecycleEvent &event) {
                return event.kind == lang::MirLifecycleEventKind::TransferOut &&
                       prepared(event.source);
              }))
        handoffs.push_back(instruction.id);
      if (instruction.kind == lang::MirInstructionKind::Drop &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().failureCleanup &&
          prepared(instruction.lifecycle.front().source))
        cleanups.push_back(instruction.id);
    }
  }
  expect(handoffs.size() == 7,
         "the fixture should hand seven prepared arguments to their calls");
  expect(!cleanups.empty(),
         "the fixture should destroy a prepared argument on a failure edge");

  const auto admitted = [&](lang::MirInstructionId id, bool duplicate) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    for (lang::MirFunctionInstance &function : functions) {
      if (function.id != entry->id)
        continue;
      lang::MirInstruction *instruction =
          findPreflightInstruction(function.body, id);
      if (duplicate)
        instruction->lifecycle.push_back(instruction->lifecycle.front());
      else
        instruction->tombstone = true;
      expect(lang::rebuildMirValueUses(function.body),
             "the forged nested owning argument body should retain a "
             "rebuildable use index");
    }
    return lang::CppMirBodyEmitter(forged, map)
        .supportsFailureBodyText(address);
  };
  for (const lang::MirInstructionId id : handoffs)
    expect(!admitted(id, true),
           "a call that hands off a prepared argument twice must fail "
           "admission");
  for (const lang::MirInstructionId id : cleanups)
    expect(!admitted(id, false),
           "a failure edge that abandons a prepared argument must fail "
           "admission");
}

void testConditionalExpectedArgumentReceiverAdmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/conditional_expected_argument_receiver.gti";
  std::ifstream input(path);
  expect(input.good(), "the expected argument fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "expected argument frontend must succeed");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "owning expected extraction before conditional member arguments must "
         "emit");
  const lang::MirFunctionInstance *entry = nullptr;
  for (const auto &function : frontend.mir.functionInstances())
    if (function.entryKind != lang::ProgramEntryKind::None)
      entry = &function;
  expect(entry != nullptr, "expected fixture must have an entry body");
  if (!entry)
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  std::vector<lang::MirInstructionId> stages;
  for (const auto &block : entry->body.blocks) {
    for (const auto &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput ||
          instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
          !instruction.preparedParameterDrop ||
          instruction.operands.size() != 1)
        continue;
      const auto *value =
          entry->body.findValue(instruction.operands.front().value);
      const auto *producer =
          value ? findPreflightInstruction(entry->body, value->definition)
                : nullptr;
      const auto *target =
          producer && producer->functionTarget
              ? frontend.hir.findFunctionInstance(*producer->functionTarget)
              : nullptr;
      if (producer && producer->kind == lang::MirInstructionKind::Call &&
          target && target->source && target->source->name().lexeme == "value")
        stages.push_back(instruction.id);
    }
  }
  expect(stages.size() == 2,
         "unique-owner and user class payloads must both be covered");
  const auto forge = [&](const auto &mutation) {
    auto forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    for (auto &function : functions) {
      if (function.id != entry->id)
        continue;
      mutation(function.body);
      expect(lang::rebuildMirValueUses(function.body),
             "mutation must rebuild its use index");
    }
    return !lang::CppMirBodyEmitter(forged, map)
                .supportsFailureBodyText(address);
  };
  for (auto id : stages) {
    expect(forge([&](lang::MirBody &body) {
             ++findPreflightInstruction(body, id)->callSite;
           }),
           "an extracted owner prepared for another call must be rejected");
    expect(forge([&](lang::MirBody &body) {
             findPreflightInstruction(body, id)->lifecycle.front().conditional =
                 true;
           }),
           "conditional ownership transfer must not erase receiver staging");
    expect(forge([&](lang::MirBody &body) {
             const auto *stage = findPreflightInstruction(body, id);
             const auto *value = body.findValue(stage->operands.front().value);
             findPreflightInstruction(body, value->definition)
                 ->successResultDrop.reset();
           }),
           "missing extraction activation must not erase receiver staging");
    expect(forge([&](lang::MirBody &body) {
             const auto *stage = findPreflightInstruction(body, id);
             for (auto &block : body.blocks)
               for (auto &instruction : block.instructions)
                 if (instruction.kind == lang::MirInstructionKind::Drop &&
                     instruction.lifecycle.size() == 1 &&
                     instruction.lifecycle.front().source ==
                         *stage->preparedParameterDrop &&
                     instruction.lifecycle.front().failureCleanup)
                   instruction.lifecycle.front().conditional = true;
           }),
           "a conditional failure cleanup cannot protect a prepared owner");
  }
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
  const lang::CppMirProgramEmissionAnalysis structural =
      emitter.analyzeProgramStructure();
  const auto addresses = lang::enumerateMirBodyAddresses(program);
  expect(structural.ready() && structural.issues.empty() &&
             structural.bodies.size() == addresses.size() &&
             metrics.programVerifications == 1,
         "one structural batch must validate the exact immutable MIR/map once "
         "and retain the complete body inventory");
  for (std::size_t index = 0; index < structural.bodies.size(); ++index) {
    const auto &batch = structural.bodies[index];
    expect(index < addresses.size() && batch.body == addresses[index],
           "structural batching must not drop, substitute or duplicate a body");
    const lang::CppMirBodyEmissionAnalysis standalone =
        emitter.analyze(batch.body);
    expect(batch.body == standalone.body &&
               batch.readiness == standalone.readiness &&
               batch.issues.size() == standalone.issues.size() &&
               std::equal(batch.issues.begin(), batch.issues.end(),
                          standalone.issues.begin(),
                          [](const lang::CppMirBodyEmissionIssue &left,
                             const lang::CppMirBodyEmissionIssue &right) {
                            return left.kind == right.kind &&
                                   left.body == right.body &&
                                   left.block == right.block &&
                                   left.instruction == right.instruction &&
                                   left.detail == right.detail;
                          }),
           "batch and standalone structural analysis must match every body and "
           "exact issue identity/detail");
  }
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
    const lang::CppMirProgramEmissionAnalysis rejectedStructure =
        emitter.analyzeProgramStructure();
    expect(!rejectedStructure.ready() && !rejectedStructure.issues.empty() &&
               rejectedStructure.bodies.empty() &&
               metrics.programVerifications == before + 4,
           "a globally invalid MIR/map must fail fresh structural validation "
           "without any independently Ready child");
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

  program = frontend.mir;
  lang::CppMirBodyEmissionMapRows missing = rows;
  const lang::MirBodyAddress missingAddress{
      .kind = lang::MirBodyKind::Function,
      .owner = firstSourceFunction(frontend.mir)->id};
  const std::size_t beforeMissing = missing.bodies.size();
  std::erase_if(missing.bodies,
                [&](const auto &row) { return row.address == missingAddress; });
  expect(missing.bodies.size() + 1 == beforeMissing,
         "the negative must remove exactly one real body representation");
  map = lang::CppMirBodyEmissionMap(std::move(missing));
  metrics.programVerifications = 0;
  const auto incomplete = emitter.analyzeProgramStructure();
  expect(incomplete.issues.empty() &&
             incomplete.bodies.size() == addresses.size() &&
             metrics.programVerifications == 1,
         "a locally missing representation must retain the complete structural "
         "inventory rather than be confused with global invalidity");
  bool rejectedMissing = false;
  for (const auto &batch : incomplete.bodies) {
    const auto standalone = emitter.analyze(batch.body);
    expect(batch.readiness == standalone.readiness &&
               batch.issues.size() == standalone.issues.size() &&
               std::equal(batch.issues.begin(), batch.issues.end(),
                          standalone.issues.begin(),
                          [](const lang::CppMirBodyEmissionIssue &left,
                             const lang::CppMirBodyEmissionIssue &right) {
                            return left.kind == right.kind &&
                                   left.body == right.body &&
                                   left.block == right.block &&
                                   left.instruction == right.instruction &&
                                   left.detail == right.detail;
                          }),
           "batching must preserve exact body-local rejection issues");
    if (batch.body == missingAddress)
      rejectedMissing = !batch.ready();
  }
  expect(rejectedMissing,
         "structural batching must not independently admit the missing body");
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
  const std::string nestedFamily =
      "family<" + std::string(lang::cppMirBodyEmissionIndentationPlaceholder) +
      ">" + std::string(lang::cppMirBodyEmissionFamilyPlaceholder);
  const auto nested = lang::CppMirBodyTextWriter::write(
      plan, lang::CppMirBodyTextForm::Plain, nestedFamily, 2);
  expect(nested.emitted() &&
             nested.text.find(
                 "family<    >" +
                 std::string(lang::cppMirBodyEmissionFamilyPlaceholder)) !=
                 std::string::npos,
         "linear rendering must preserve family-then-indentation order and "
         "must not recursively replace markers inserted by the family");
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
  expect(text.text.find(
             " = ::gti_internal::backend::raw_address_of(&__gti_mir_p_") !=
                 std::string::npos &&
             text.text.find("[__gti_mir_v_") != std::string::npos &&
             text.text.find("(*__gti_mir_v_") != std::string::npos &&
             text.text.find(" + ") != std::string::npos &&
             text.text.find(" - ") != std::string::npos,
         "raw-pointer MIR text should preserve checked MIR authority through "
         "carrier-backed address formation, index/dereference lvalues, and "
         "pointer arithmetic");
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

void testByteFieldRepresentationFailsClosed() {
  const auto frontend = analyze("cpp-mir-byte-field-mutation.gti", R"(
struct Packet { int value = 7; };
int main() { Packet packet{}; return packet.value; }
)");
  expect(frontend.canGenerateCode(), "byte-field mutation fixture is valid");
  const auto *function = firstSourceFunction(frontend.mir);
  if (!frontend.canGenerateCode() || function == nullptr)
    return;
  auto rows = buildRows(frontend, frontend.mir);
  const auto field = std::find_if(
      rows.symbols.begin(), rows.symbols.end(), [](const auto &row) {
        return row.kind == lang::CppMirSymbolRepresentationKind::Field;
      });
  expect(field != rows.symbols.end(), "byte-field mutation has a field row");
  if (field == rows.symbols.end())
    return;
  field->bytePreservingFieldCell = true;
  const auto forged =
      lang::CppMirBodyEmitter(frontend.mir,
                              lang::CppMirBodyEmissionMap(std::move(rows)))
          .analyze(
              {.kind = lang::MirBodyKind::Function, .owner = function->id});
  expect(hasIssue(forged,
                  lang::CppMirBodyEmissionIssueKind::InvalidRepresentationRow),
         "scalar field cannot forge a native byte-cell accessor");

  const auto nativeFrontend =
      analyze("cpp-mir-concrete-byte-field-mutation.gti", R"(
[[c_abi]] struct Native { const int* pointer; int value; };
struct Packet { Native native; };
int read(Packet value) { return value.native.value; }
int main() { return 0; }
)");
  expect(nativeFrontend.canGenerateCode(),
         "concrete native-field mutation fixture is valid");
  const auto *nativeFunction = firstSourceFunction(nativeFrontend.mir);
  if (!nativeFrontend.canGenerateCode() || nativeFunction == nullptr)
    return;
  auto nativeRows = buildRows(nativeFrontend, nativeFrontend.mir);
  const auto nativeField = std::find_if(
      nativeRows.symbols.begin(), nativeRows.symbols.end(),
      [](const auto &row) {
        return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
               row.spelling == "native";
      });
  expect(nativeField != nativeRows.symbols.end(),
         "concrete native-field row exists");
  if (nativeField == nativeRows.symbols.end())
    return;
  nativeField->bytePreservingFieldCell = true;
  const auto concreteForged =
      lang::CppMirBodyEmitter(nativeFrontend.mir, lang::CppMirBodyEmissionMap(
                                                      std::move(nativeRows)))
          .analyze({.kind = lang::MirBodyKind::Function,
                    .owner = nativeFunction->id});
  expect(
      hasIssue(concreteForged,
               lang::CppMirBodyEmissionIssueKind::InvalidRepresentationRow),
      "ordinary concrete native field cannot forge a dependent cell accessor");
}

void testPlainExpectedPayloadConstruction() {
  const auto frontend =
      analyzeWithStandardLibrary("plain-expected-payload.gti", R"(
class Ticket {
  int first;
  int second;
public:
  Ticket(int first, int second) : first(first), second(second) {}
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
  ~Ticket() {}
};
int exercise(int first, int second) {
  expected<Ticket, int> value = Ticket(first, second);
  return 0;
}
int main() { return exercise(1, 2); }
)");
  expect(frontend.canGenerateCode(), "plain Expected payload must lower");
  if (!frontend.canGenerateCode())
    return;
  const auto function = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [&](const auto &candidate) {
        const auto *hir = frontend.hir.findFunctionInstance(candidate.id);
        return hir && hir->source && hir->source->name().lexeme == "exercise";
      });
  expect(function != frontend.mir.functionInstances().end(),
         "plain Expected payload fixture has its source function");
  if (function == frontend.mir.functionInstances().end())
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::MirInstruction *construct = nullptr;
  const lang::MirInstruction *initialize = nullptr;
  const lang::MirInstruction *stage = nullptr;
  for (const auto &block : function->body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          instruction.operands.size() == 2)
        construct = &instruction;
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.info.type.kind == lang::SemanticType::Class)
        initialize = &instruction;
      if (instruction.kind == lang::MirInstructionKind::CallInput)
        stage = &instruction;
    }
  expect(
      construct && initialize && stage && construct->definedFailure.empty() &&
          construct->operands.size() == 2 && construct->lifecycle.size() == 1 &&
          initialize->lifecycle.size() == 1 &&
          stage->preparedParameterDrop.has_value(),
      "nonraising constructor retains prepared arguments and two ownership "
      "stages");
  if (!construct || !initialize || !stage)
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    const auto text =
        function->mayRaiseDefinedFailure
            ? emitter.emitFailureBodyText(address, "plain-payload", 1)
            : emitter.emitBodyText(address, "plain-payload", 1);
    expect(text.emitted() && text.text.find(".construct(") != std::string::npos,
           "nonraising payload executes at Construct in its explicit lifetime "
           "slot");
    expect(
        text.text.find(".take())") != std::string::npos &&
            text.text.find(".destroy();") != std::string::npos,
        "Expected publication consumes the payload and retires its old shell");
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto forged = frontend.mir;
      auto *body = &const_cast<lang::MirFunctionInstance *>(
                        forged.findFunctionInstance(function->id))
                        ->body;
      auto *producer = findPreflightInstruction(*body, construct->id);
      auto *publication = findPreflightInstruction(*body, initialize->id);
      auto *input = findPreflightInstruction(*body, stage->id);
      if (mutation == 0)
        producer->lifecycle.clear();
      else if (mutation == 1)
        publication->lifecycle.front().source = 0;
      else if (mutation == 2)
        publication->lifecycle.back().target = 0;
      else if (mutation == 3)
        input->callInputIndex = 99;
      else
        producer->parameterTypes.front() = lang::SemanticType::Bool;
      const lang::CppMirBodyEmitter rejected(forged, map);
      expect(
          !lang::verifyMirProgram(forged).valid() ||
              (!rejected.supportsBodyText(address) &&
               !rejected.supportsFailureBodyText(address)),
          "forged Expected payload construction/publication must fail closed");
    }
  }
}

void testExpectedReferenceHasOnlyLoanStorage() {
  const auto frontend =
      analyzeWithStandardLibrary("expected-reference-storage.gti", R"(
#include <std/memory>
class Ticket {
  int number;
public:
  Ticket(int number) : number(number) {}
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
  ~Ticket() {}
  int id() { return this.number; }
};
expected<Ticket, int>& relay(expected<Ticket, int>& value) { return value; }
expected<Ticket, int>& raw_relay(expected<Ticket, int>& value) {
  unsafe { return std::assume_ref(&value, value); }
}
int main() {
  expected<Ticket, int> value = Ticket(7);
  expected<Ticket, int>& first = relay(value);
  expected<Ticket, int>& second = raw_relay(first);
  return second.value().id() == 7 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(), "Expected reference relay must lower");
  if (!frontend.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    std::size_t identities = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions) {
          if (!instruction.result || !instruction.loan ||
              instruction.kind != lang::MirInstructionKind::Call ||
              instruction.info.type.kind != lang::SemanticType::Class ||
              instruction.info.category != lang::ValueCategory::Place)
            continue;
          const auto text =
              function.mayRaiseDefinedFailure
                  ? emitter.emitFailureBodyText(address, "expected-reference",
                                                1)
                  : emitter.emitBodyText(address, "expected-reference", 1);
          expect(text.emitted(), "Expected reference body must emit");
          expect(text.text.find(" __gti_mir_v_" +
                                std::to_string(*instruction.result) + "{};") ==
                     std::string::npos,
                 "a checked reference must not default-construct a phantom "
                 "Expected");
          expect(text.text.find("__gti_mir_loan_" +
                                std::to_string(*instruction.loan)) !=
                     std::string::npos,
                 "the actual checked loan remains the referent carrier");
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          auto *loan =
              const_cast<lang::MirLoan *>(body->findLoan(*instruction.loan));
          loan->producedBy = 0;
          const lang::CppMirBodyEmitter malformed(forged, map);
          expect(
              !malformed.supportsBodyText(address) &&
                  !malformed.supportsFailureBodyText(address),
              "reference storage erasure cannot hide a detached loan producer");
          ++identities;
        }
    }
    expect(identities >= 4,
           "ordinary and raw-backed Expected relays are covered");
  }
}

void testExpectedFieldHasOnlyPayloadStorage() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/expected_field_no_phantom.gti";
  std::ifstream input(path);
  expect(input.good(), "Expected projected field fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "Expected projected fields must lower");
  if (!frontend.canGenerateCode())
    return;
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind != lang::ProgramEntryKind::None;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "Expected projected field fixture has an entry body");
  if (entry == frontend.mir.functionInstances().end())
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    const auto text =
        emitter.emitFailureBodyText(address, "expected-fields", 1);
    expect(text.emitted(), "Expected projected field body must emit");
    std::size_t fields = 0;
    bool nested = false;
    for (const auto &place : entry->body.places) {
      if (place.root != lang::MirPlaceRootKind::Loan ||
          place.projections.empty())
        continue;
      const auto *loan = entry->body.findLoan(place.loan);
      const lang::MirInstruction *producer = nullptr;
      for (const auto &block : entry->body.blocks)
        for (const auto &instruction : block.instructions)
          if (loan && instruction.hirValue == loan->producedBy &&
              instruction.loan == place.loan &&
              instruction.kind == lang::MirInstructionKind::Call)
            producer = &instruction;
      if (!producer || producer->kind != lang::MirInstructionKind::Call ||
          !producer->functionTarget || !producer->loan ||
          producer->info.category != lang::ValueCategory::Place)
        continue;
      ++fields;
      nested |= place.projections.size() == 2;
      expect(text.text.find("__gti_mir_p_" + std::to_string(place.id)) ==
                 std::string::npos,
             "a payload field must not declare or access independent storage");
      expect(text.text.find("(*__gti_mir_loan_" + std::to_string(place.loan) +
                            ").") != std::string::npos,
             "a payload field must access its checked extraction address");
      for (int mutation = 0; mutation != 2; ++mutation) {
        auto forged = frontend.mir;
        auto *body = lang::findMirBody(forged, address);
        auto *field = const_cast<lang::MirPlace *>(body->findPlace(place.id));
        if (mutation == 0)
          field->projections.back().field = 0;
        else
          field->type = lang::SemanticType::Bool;
        const lang::CppMirBodyEmitter malformed(forged, map);
        expect(!malformed.supportsFailureBodyText(address),
               "field storage elision cannot hide a forged projection or type");
      }
    }
    expect(fields >= 4 && nested,
           "direct and nested Expected field projections must be exercised");
  }
}

void testNativeExpectedCopyPublication() {
  auto frontend =
      analyzeWithStandardLibrary("cpp-mir-native-expected-copy.gti", R"(
[[c_abi]] struct Native { const int* pointer; int value; };
extern "C" { Native empty_native(); }
expected<Native, int> success(Native value) { return value; }
expected<T, int> generic_success<T>(T value) { return value; }
expected<T, int> relay<T>(expected<T, int> value) { return value; }
int main() {
  unsafe {
  Native native = empty_native();
  mut auto original = success(native);
  auto copied = original;
  original = copied;
  auto relayed = relay<Native>(generic_success<Native>(native));
  return 0;
  }
}
)");
  expect(frontend.canGenerateCode(), "native Expected copy fixture is valid");
  if (!frontend.canGenerateCode())
    return;
  const auto &functions = frontend.mir.functionInstances();
  const auto entry = std::find_if(
      functions.begin(), functions.end(), [](const auto &function) {
        return function.entryKind != lang::ProgramEntryKind::None;
      });
  expect(entry != functions.end(), "native Expected copy has an entry body");
  if (entry == functions.end())
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.supportsFailureBodyText(address),
         "native Expected copy retains lexical cleanup admission");
  if (!emitter.supportsFailureBodyText(address))
    return;
  const auto text =
      emitter.emitFailureBodyText(address, "native-expected-copy", 1);
  expect(text.text.find("class copy load") != std::string::npos &&
             text.text.find("publishes at its consuming initialize") !=
                 std::string::npos,
         "source result copy constructs one final class binding slot");
  auto mutated = frontend.mir;
  auto *function = const_cast<lang::MirFunctionInstance *>(
      mutated.findFunctionInstance(entry->id));
  bool changed = false;
  for (auto &block : function->body.blocks) {
    for (std::size_t index = 1; index < block.instructions.size(); ++index) {
      auto &instruction = block.instructions[index];
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.info.type.kind == lang::SemanticType::Class &&
          block.instructions[index - 1].kind ==
              lang::MirInstructionKind::Load &&
          instruction.lifecycle.size() == 1) {
        instruction.lifecycle.front().target = 0;
        changed = true;
      }
    }
  }
  expect(changed, "native Expected copy mutation finds binding activation");
  expect(!lang::verifyMirProgram(mutated).valid(),
         "native Expected copy cannot lose its destination lexical drop");
  auto cleanupMutation = frontend.mir;
  auto *cleanupFunction = const_cast<lang::MirFunctionInstance *>(
      cleanupMutation.findFunctionInstance(entry->id));
  bool changedCleanup = false;
  for (auto &block : cleanupFunction->body.blocks) {
    for (auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Assign &&
          instruction.info.type.kind == lang::SemanticType::Class &&
          instruction.destination) {
        const auto drop = std::find_if(
            cleanupFunction->body.dropObligations.begin(),
            cleanupFunction->body.dropObligations.end(), [&](const auto &item) {
              return item.place == *instruction.destination;
            });
        if (drop == cleanupFunction->body.dropObligations.end())
          continue;
        expect(!instruction.replacementCleanup &&
                   !drop->dropType.requiresActiveCleanup,
               "passive source result assignment has no active cleanup");
        instruction.replacementCleanup = drop->dropType;
        instruction.replacementCleanup->requiresActiveCleanup = true;
        changedCleanup = true;
      }
    }
  }
  expect(changedCleanup,
         "native Expected assignment has a passive cleanup recipe");
  expect(!lang::verifyMirProgram(cleanupMutation).valid(),
         "native Expected assignment cannot fuse across active cleanup");
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

void testDischargedArrayReadAnalysis() {
  // Prefix reads are now ordinary source guards followed by a general raw
  // borrow. Preserve discharged-read text admission with a bounded primitive
  // array instead of asserting retired private container proof machinery.
  const lang::FrontendResult frontend =
      analyze("cpp-mir-discharged-array-read.gti", R"(
int main() {
  int values[2] = {7, 9};
  mut int index = 0;
  return values[index];
}
)");
  expect(frontend.canGenerateCode(),
         "the discharged-array-read fixture should pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const auto hasBounds = [](const lang::MirInstruction &instruction) {
    return std::any_of(
        instruction.definedFailure.localOrigins.begin(),
        instruction.definedFailure.localOrigins.end(), [](const auto &origin) {
          return std::any_of(
              origin.outcomes.begin(), origin.outcomes.end(),
              [](const auto &outcome) {
                return outcome.code ==
                       lang::DefinedFailureCode::IndexOutOfBounds;
              });
        });
  };
  const lang::CppMirBodyEmissionMap originalMap(
      buildRows(frontend, frontend.mir));
  std::size_t originalReads = 0;
  for (const auto &instance : frontend.mir.functionInstances()) {
    for (const auto &block : instance.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!hasBounds(instruction))
          continue;
        ++originalReads;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = instance.id};
        const auto text =
            lang::CppMirBodyEmitter(frontend.mir, originalMap)
                .emitFailureBodyText(address, "checked-array-read-v1", 1);
        const std::string status =
            "__gti_mir_failure_status_" + std::to_string(instruction.id);
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id &&
                   text.emitted() &&
                   text.text.find("mir_checked_array_read_v1(") !=
                       std::string::npos &&
                   text.text.find("if (" + status) != std::string::npos,
               "O0 must retain the exact checked read and its failure Invoke");
      }
    }
  }
  expect(originalReads != 0,
         "the array fixture must start with a genuine checked read");
  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      pipeline.run({.hir = frontend.hir,
                    .mir = frontend.mir,
                    .level = lang::OptimizationLevel::O3,
                    .compatibility = &compatibility});
  expect(optimized.valid(),
         "the discharged-array-read fixture should produce coherent O3 MIR");
  if (!optimized.valid())
    return;
  const lang::LoweredProgram lowered = gti_test::lowerProgram(
      frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
      optimized.report.transformations);
  const lang::CppMirBodyEmissionMap map(
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(optimized.mir, map);
  std::size_t dischargedReads = 0;
  for (const auto &instance : optimized.mir.functionInstances()) {
    for (const auto &block : instance.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!hasBounds(instruction) || instruction.safetyDischarges.empty() ||
            !lang::allMirLocalFailuresDischarged(instruction))
          continue;
        ++dischargedReads;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = instance.id};
        const auto text =
            instance.mayRaiseDefinedFailure
                ? emitter.emitFailureBodyText(address,
                                              "discharged-array-read-v1", 1)
                : emitter.emitBodyText(address, "discharged-array-read-v1", 1);
        const std::string status =
            "__gti_mir_failure_status_" + std::to_string(instruction.id);
        // General array emission may leave one unused, success-initialized
        // native status declaration. It is neither a detector nor a status
        // use. The retired private prefix emitter's no-declaration assertion
        // is not claimed as source-owner cross-call optimization parity.
        const auto declaration = text.text.find(status);
        const bool noStatusUse =
            declaration == std::string::npos ||
            (text.text.find(status, declaration + status.size()) ==
                 std::string::npos &&
             text.text.find(
                 status +
                 " = ::gti_internal::backend::mir_failure_success_v1;") !=
                 std::string::npos);
        expect(text.emitted() &&
                   text.text.find("mir_checked_array_read_v1(") ==
                       std::string::npos &&
                   text.text.find("array_at(") == std::string::npos &&
                   noStatusUse && text.text.find('[') != std::string::npos &&
                   block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id &&
                   text.text.find("goto __gti_mir_bb_" +
                                  std::to_string(block.terminator.target)) !=
                       std::string::npos,
               "proved array bounds use direct indexing and the Invoke's "
               "success edge without a guard or status use");
      }
    }
  }
  expect(dischargedReads != 0, "the array fixture must retain a checked read "
                               "with independently discharged bounds");
}

void testDefaultPrefixConstructorStatusAdmission() {
  const auto source = [](std::string_view body) {
    return std::string(R"(
#include <std/optional>
#include <std/string>
#include <std/vector>
class Envelope {
  std::optional<std::string> payload{};
public:
  Envelope() { )") +
           std::string(body) + R"( }
};
int main() {
  std::vector<Envelope> values{std::size_t(2)};
  return 0;
}
)";
  };
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-default-prefix-constructor.gti", source(""));
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "construction-only default prefix elements should have verified MIR");
  if (!frontend.canGenerateCode() || !frontend.mir.valid())
    return;
  const auto findAppend = [](const lang::MirProgram &program) {
    std::pair<lang::HirFunctionInstanceId, lang::MirInstructionId> result{};
    for (const auto &function : program.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.intrinsic == lang::IntrinsicKind::ObjectConstruct &&
              instruction.constructorTarget && instruction.operands.size() == 1)
            result = {function.id, instruction.id};
        }
      }
    }
    return result;
  };
  const auto [owner, appendId] = findAppend(frontend.mir);
  expect(owner != 0 && appendId != 0,
         "sized vector must preserve its selected default element constructor");
  if (owner == 0 || appendId == 0)
    return;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = owner};
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const auto *ownerFunction = frontend.mir.findFunctionInstance(owner);
  const bool failureForm = ownerFunction->mayRaiseDefinedFailure;
  expect(failureForm ? emitter.supportsFailureBodyText(address)
                     : emitter.supportsBodyText(address),
         "source default construction admits its exact concrete body form");
  const auto emitted =
      failureForm
          ? emitter.emitFailureBodyText(address, "source-default-construction",
                                        1)
          : emitter.emitBodyText(address, "source-default-construction", 1);
  expect(emitted.emitted() &&
             emitted.text.find("::new (static_cast<void *") !=
                 std::string::npos &&
             emitted.text.find("native_pointer_value(") != std::string::npos &&
             emitted.text.find("&__gti_mir_construct_success_" +
                               std::to_string(appendId)) != std::string::npos,
         "owning-field placement retains its exact final-address constructor "
         "status even when the selected body cannot fail");

  lang::MirProgram forged = frontend.mir;
  auto &constructors = const_cast<std::vector<lang::MirConstructorInstance> &>(
      forged.constructorInstances());
  lang::HirConstructorInstanceId target = 0;
  for (const auto &block :
       frontend.mir.findFunctionInstance(owner)->body.blocks) {
    for (const auto &instruction : block.instructions) {
      if (instruction.id == appendId)
        target = instruction.constructorTarget.value_or(0);
    }
  }
  expect(target != 0 && lang::cppMirConstructorStatusCannotFail(forged, target),
         "status append requires independent exact-target no-failure proof");
  if (target != 0) {
    lang::MirProgram wrongActivation = frontend.mir;
    auto &activationConstructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            wrongActivation.constructorInstances());
    bool changedActivation = false;
    for (auto &block : activationConstructors[target - 1].body.blocks) {
      if (!block.terminator.successLifecycle.empty()) {
        block.terminator.successLifecycle.front().target = 0;
        changedActivation = true;
        break;
      }
    }
    expect(
        changedActivation &&
            !lang::cppMirConstructorStatusCannotFail(wrongActivation, target) &&
            !(failureForm ? lang::CppMirBodyEmitter(wrongActivation, map)
                                .supportsFailureBodyText(address)
                          : lang::CppMirBodyEmitter(wrongActivation, map)
                                .supportsBodyText(address)),
        "a forged success-result drop cannot enter the no-failure proof");

    // Preserve the selected target/signature but forge an unknown propagation
    // channel on its initializer. The no-failure proof must fail closed.
    auto &body = constructors[target - 1].body;
    expect(!body.blocks.empty() && !body.blocks.front().instructions.empty(),
           "the selected constructor should initialize its owning field");
    if (!body.blocks.empty() && !body.blocks.front().instructions.empty()) {
      body.blocks.front().instructions.front().definedFailure.propagation =
          lang::FailurePropagationKind::DirectCall;
      expect(!lang::cppMirConstructorStatusCannotFail(forged, target) &&
                 !(failureForm ? lang::CppMirBodyEmitter(forged, map)
                                     .supportsFailureBodyText(address)
                               : lang::CppMirBodyEmitter(forged, map)
                                     .supportsBodyText(address)),
             "unproved constructor failure cannot bypass final-address "
             "publication");
    }
  }

  const auto fallible = analyzeWithStandardLibrary(
      "cpp-mir-fallible-default-prefix-constructor.gti",
      source("int zero = 0; int result = 1 / zero;"));
  expect(fallible.canGenerateCode() && fallible.mir.valid(),
         "fallible default construction should retain a valid source schedule");
  if (fallible.canGenerateCode() && fallible.mir.valid()) {
    const auto [fallibleOwner, fallibleAppend] = findAppend(fallible.mir);
    expect(fallibleOwner != 0 && fallibleAppend != 0,
           "fallible default construction must retain its exact append target");
    if (fallibleOwner != 0) {
      const lang::CppMirBodyEmissionMap fallibleMap(
          buildRows(fallible, fallible.mir, lang::CppStandard::Cpp23));
      expect(
          lang::CppMirBodyEmitter(fallible.mir, fallibleMap)
              .supportsFailureBodyText({.kind = lang::MirBodyKind::Function,
                                        .owner = fallibleOwner}),
          "fallible default prefix construction must use the exact propagated "
          "constructor status without publishing a failed slot");
      auto missingEffect = fallible.mir;
      auto &instances = const_cast<std::vector<lang::MirFunctionInstance> &>(
          missingEffect.functionInstances());
      for (auto &block : instances[fallibleOwner - 1].body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.id == fallibleAppend)
            instruction.definedFailure.propagation =
                lang::FailurePropagationKind::None;
        }
      }
      expect(!lang::verifyMirProgram(missingEffect).valid(),
             "a zero-argument final-address call cannot erase its exact ctor "
             "failure effect");
      auto forgedOwnership = fallible.mir;
      auto &ownershipInstances =
          const_cast<std::vector<lang::MirFunctionInstance> &>(
              forgedOwnership.functionInstances());
      for (auto &block : ownershipInstances[fallibleOwner - 1].body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.id == fallibleAppend)
            instruction.lifecycle.push_back(
                {.kind = lang::MirLifecycleEventKind::TransferOut,
                 .source = 1});
        }
      }
      expect(!lang::verifyMirProgram(forgedOwnership).valid(),
             "zero-argument final-address construction cannot invent an owned "
             "input handoff");
      const auto rejectMutation = [&](const auto &mutate,
                                      std::string_view message) {
        auto changed = fallible.mir;
        auto &changedConstructors =
            const_cast<std::vector<lang::MirFunctionInstance> &>(
                changed.functionInstances());
        auto &body = changedConstructors[fallibleOwner - 1].body;
        auto *append = findPreflightInstruction(body, fallibleAppend);
        expect(append != nullptr, "prefix mutation must find its exact append");
        if (append == nullptr)
          return;
        mutate(changed, body, *append);
        expect(!lang::verifyMirProgram(changed).valid(), message);
        expect(
            !lang::CppMirBodyEmitter(changed, fallibleMap)
                 .supportsFailureBodyText({.kind = lang::MirBodyKind::Function,
                                           .owner = fallibleOwner}),
            message);
      };
      rejectMutation(
          [](auto &, auto &, auto &append) {
            append.constructorTarget.reset();
          },
          "default construction cannot erase its exact selected constructor");
      rejectMutation(
          [](auto &, auto &, auto &append) {
            append.constructorTarget.reset();
            append.definedFailure.propagation =
                lang::FailurePropagationKind::None;
          },
          "erasing target and propagation cannot recover native T() fallback");
      rejectMutation(
          [&](auto &, auto &, auto &append) {
            append.constructorTarget = fallibleOwner;
          },
          "default construction cannot select a different element constructor");
      rejectMutation(
          [](auto &, auto &body, auto &append) {
            for (auto &block : body.blocks) {
              if (block.terminator.invokeInstruction == append.id)
                block.terminator.kind = lang::MirTerminatorKind::Goto;
            }
          },
          "default construction cannot erase its checked Invoke edge");
      rejectMutation(
          [](auto &, auto &body, auto &append) {
            for (auto &block : body.blocks) {
              if (block.terminator.invokeInstruction == append.id)
                block.terminator.elseTarget = block.terminator.target;
            }
          },
          "default construction cannot route failure through successful "
          "publication");
    }
  }
}

void testScalarPrefixConstructorStatusAdmission() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/vector_scalar_constructor_failure.gti";
  std::ifstream stream(path);
  const std::string source{std::istreambuf_iterator<char>(stream), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "scalar-input final-slot construction must retain verified MIR");
  if (!frontend.canGenerateCode() || !frontend.mir.valid())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t tested = 0;
  std::size_t normalized = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &append : block.instructions) {
        if (append.intrinsic != lang::IntrinsicKind::ObjectConstruct ||
            !append.constructorTarget || append.operands.size() != 3)
          continue;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        const auto *target =
            frontend.mir.findConstructorInstance(*append.constructorTarget);
        if (target != nullptr && !target->mayRaiseDefinedFailure) {
          ++normalized;
          expect(append.definedFailure.propagation ==
                     lang::FailurePropagationKind::None,
                 "concrete HIR packs must normalize an exact failure-free "
                 "constructor effect");
          // A caller with no other failure source is itself failure-free and
          // emits only its plain body; either form constructs in place.
          const lang::CppMirBodyEmitter emitter(frontend.mir, map);
          const auto emitted =
              function.mayRaiseDefinedFailure
                  ? emitter.emitFailureBodyText(address, "passive-prefix", 1)
                  : emitter.emitBodyText(address, "passive-prefix", 1);
          expect(emitted.emitted() &&
                     emitted.text.find("construct_representation_at(") !=
                         std::string::npos &&
                     emitted.text.find("&__gti_mir_construct_success_") ==
                         std::string::npos,
                 "proved-failure-free scalar construction must retain ordinary "
                 "in-place construction");
          continue;
        }
        ++tested;
        const auto emitted =
            lang::CppMirBodyEmitter(frontend.mir, map)
                .emitFailureBodyText(address, "scalar-prefix", 1);
        expect(emitted.emitted() &&
                   emitted.text.find("::new (static_cast<void *") !=
                       std::string::npos &&
                   emitted.text.find("&__gti_mir_construct_success_" +
                                     std::to_string(append.id) +
                                     ", __gti_mir_failure_record);") !=
                       std::string::npos,
               "scalar append must call the exact status ctor with its staged "
               "inputs");
        const auto summary = lang::exactMirInvocationSummary(
            frontend.mir, function.body, append);
        expect(summary && target &&
                   summary->argumentsEscape ==
                       target->effectSummary.argumentsEscape &&
                   summary->receiverEscapes ==
                       target->effectSummary.receiverEscapes &&
                   summary->traits.writesUnknownMemory &&
                   summary->traits.invokesUserCode && summary->traits.mayTrap,
               "general construction retains the selected constructor's exact "
               "argument/receiver escape and raw-memory/user/failure effects");
        const auto reject = [&](const auto &mutate, std::string_view reason) {
          auto forged = frontend.mir;
          auto &functions =
              const_cast<std::vector<lang::MirFunctionInstance> &>(
                  forged.functionInstances());
          auto &body = functions[function.id - 1].body;
          auto *changed = findPreflightInstruction(body, append.id);
          mutate(forged, body, *changed);
          expect(!lang::verifyMirProgram(forged).valid(), reason);
          expect(!lang::CppMirBodyEmitter(forged, map)
                      .supportsFailureBodyText(address),
                 reason);
        };
        reject(
            [](auto &, auto &, auto &call) { call.constructorTarget.reset(); },
            "scalar append cannot lose its selected target");
        reject(
            [](auto &, auto &, auto &call) {
              call.constructorTarget.reset();
              call.parameterTypes.clear();
            },
            "missing target and parameter roles must fail closed without "
            "invalid tail iteration");
        reject(
            [](auto &, auto &, auto &call) {
              call.constructorTarget.reset();
              call.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
            },
            "erasing target and propagation cannot recover native constructor "
            "fallback");
        reject(
            [](auto &, auto &, auto &call) {
              call.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
            },
            "scalar append cannot erase constructor propagation");
        reject(
            [](auto &program, auto &, auto &call) {
              for (const auto &constructor : program.constructorInstances()) {
                if (constructor.id != *call.constructorTarget) {
                  call.constructorTarget = constructor.id;
                  break;
                }
              }
            },
            "scalar append cannot select a different constructor target");
        reject(
            [](auto &, auto &, auto &call) { call.parameterTypes.pop_back(); },
            "scalar append must retain exact constructor arity");
        reject(
            [](auto &, auto &, auto &call) {
              call.parameterTypes[1] = lang::SemanticType::Bool;
            },
            "scalar append must retain exact constructor parameter types");
        reject(
            [](auto &, auto &, auto &call) {
              call.lifecycle.push_back(
                  {.kind = lang::MirLifecycleEventKind::TransferOut,
                   .source = 1});
            },
            "passive inputs cannot invent an ownership handoff");
        reject(
            [](auto &, auto &body, auto &call) {
              for (auto &candidate : body.blocks)
                if (candidate.terminator.invokeInstruction == call.id)
                  candidate.terminator.kind = lang::MirTerminatorKind::Goto;
            },
            "scalar construction cannot remove its checked Invoke");
        reject(
            [](auto &, auto &body, auto &call) {
              for (auto &candidate : body.blocks)
                if (candidate.terminator.invokeInstruction == call.id)
                  candidate.terminator.elseTarget = candidate.terminator.target;
            },
            "failed scalar construction cannot publish a successful element");
        for (const auto &type :
             {lang::SemanticType::referenceTo(lang::SemanticType::Int32),
              lang::SemanticType::rawPointerTo(lang::SemanticType::Int32),
              lang::SemanticType(lang::SemanticType::StringView)}) {
          reject([&](auto &, auto &,
                     auto &call) { call.parameterTypes[1] = type; },
                 "reference/raw/text inputs cannot enter the passive status "
                 "route");
        }
      }
    }
  }
  expect(
      tested == 1,
      "the fixture must exercise exactly one concrete scalar emplacement body");
  expect(normalized == 1, "the fixture must also exercise one "
                          "proved-failure-free scalar constructor");
}

void testOwningConstructorAcrossArgumentDiamond() {
  const std::string sources[] = {R"(
#include <std/string>
class Reading {
public:
  int& value;
  mut std::string text;
  Reading(int& source, std::string input)
      : value(source), text(std::move(input)) {}
};
int inspect(int& owner, bool choose) {
  Reading result{owner, std::string(choose ? "present" : "")};
  return result.value;
}
int main() {
  int owner = 42;
  return inspect(owner, true) + inspect(owner, false) - 84;
}
)",
                                 R"(
#include <std/string>
class Parcel {
  std::string text;
public:
  Parcel(int value) : text(std::string("parcel")) {
    int checked = 1 / value;
  }
  ~Parcel() { std::println(this.text); }
};
struct Sink {
  void accept(Parcel parcel) mut { std::println("accepted"); }
};
int exercise(bool choose) {
  mut Sink sink{};
  sink.accept(Parcel(choose ? 1 : 2));
  return 0;
}
int main() { return exercise(true); }
)",
                                 R"(
#include <std/string>
class Reading {
public:
  int& value;
  mut std::string text;
  Reading(int& source, std::string input)
      : value(source), text(std::move(input)) {}
};
int inspect(int& owner, bool choose) {
  Reading result{owner, (std::string(choose ? "present" : ""))};
  return result.value;
}
int main() {
  int owner = 42;
  return inspect(owner, true) + inspect(owner, false) - 84;
}
)"};
  for (const auto &source : sources) {
    const auto frontend = analyzeWithStandardLibrary(
        "cpp-mir-owning-constructor-argument-diamond.gti", source);
    expect(frontend.canGenerateCode() && frontend.mir.valid(),
           "owning constructor after passive argument diamond must verify");
    if (!frontend.canGenerateCode() || !frontend.mir.valid())
      return;
    lang::HirFunctionInstanceId owner = 0;
    lang::MirInstructionId producerId = 0;
    lang::MirInstructionId stageId = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::CallInput ||
              instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
              instruction.operands.size() != 1 ||
              !instruction.preparedParameterDrop ||
              instruction.operands.front().kind != lang::MirOperandKind::Value)
            continue;
          const auto *value =
              function.body.findValue(instruction.operands.front().value);
          const auto *producer =
              value ? findPreflightInstruction(function.body, value->definition)
                    : nullptr;
          if (producer &&
              producer->kind == lang::MirInstructionKind::Construct &&
              producer->successResultDrop) {
            owner = function.id;
            producerId = producer->id;
            stageId = instruction.id;
          }
        }
      }
    }
    expect(owner && producerId && stageId,
           "diamond must retain exact owning constructor result staging");
    if (!owner)
      return;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = owner};
    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
    expect(
        lang::CppMirBodyEmitter(frontend.mir, map)
            .supportsFailureBodyText(address),
        "owning constructor staging must preserve the earlier stable receiver");
    const auto rejectMutation = [&](const auto &mutate,
                                    std::string_view message) {
      auto changed = frontend.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          changed.functionInstances());
      auto &body = functions[owner - 1].body;
      auto *producer = findPreflightInstruction(body, producerId);
      auto *stage = findPreflightInstruction(body, stageId);
      mutate(body, *producer, *stage);
      expect(!lang::verifyMirProgram(changed).valid(), message);
      expect(!lang::CppMirBodyEmitter(changed, map)
                  .supportsFailureBodyText(address),
             message);
    };
    rejectMutation(
        [](auto &, auto &, auto &stage) { stage.lifecycle.front().source = 0; },
        "argument diamond cannot erase the owning result Reparent source");
    rejectMutation(
        [](auto &, auto &, auto &stage) {
          stage.lifecycle.front().target = stage.lifecycle.front().source;
        },
        "argument diamond cannot reparent to the wrong prepared slot");
    rejectMutation(
        [](auto &body, auto &producer, auto &) {
          for (auto &block : body.blocks) {
            if (block.terminator.invokeInstruction == producer.id)
              block.terminator.successLifecycle.clear();
          }
        },
        "argument diamond cannot erase successful result activation");
  }
}

void testMutableReferencePrefixBeforeOwnedField() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-mutable-reference-owned-prefix.gti", R"(
#include <std/string>
class Editing {
public:
  mut int& value;
  mut std::string error;
  Editing(mut int& source, std::string failure)
      : value(source), error(std::move(failure)) {}
};
int edit(mut int& owner, bool fail) {
  mut Editing result{owner, std::string(fail ? "failed" : "")};
  result.value += 1;
  return result.value;
}
int main() {
  mut int owner = 40;
  int first = edit(owner, false);
  return edit(owner, true) - first - 1;
}
)");
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "exclusive reference preceding an owned field must verify");
  if (!frontend.canGenerateCode() || !frontend.mir.valid())
    return;
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const auto &candidate : frontend.mir.constructorInstances()) {
    if (candidate.parameterTypes.size() == 2 &&
        candidate.parameterTypes.front().kind ==
            lang::SemanticType::Reference &&
        candidate.parameterTypes.front().referenceAccess ==
            lang::AccessMode::Mutable &&
        candidate.initializers.size() == 2 &&
        candidate.initializers.front().storesReference &&
        candidate.initializers.back().ownedParameter == 1)
      constructor = &candidate;
  }
  expect(constructor != nullptr, "fixture must retain the exact constructor");
  if (!constructor)
    return;
  const auto references =
      lang::cppMirStoredReferenceBindings(frontend.mir, *constructor);
  const auto owned =
      lang::cppMirOwnedParameterFieldBindings(frontend.mir, *constructor);
  expect(references && references->size() == 1 && owned && owned->size() == 1,
         "exact exclusive transfer prefix permits the later owned-field move");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = constructor->id};
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(address),
         "mutable reference and owned field retain native constructor support");
  const auto rejectMutation = [&](const auto &mutate,
                                  std::string_view message) {
    auto changed = frontend.mir;
    auto *candidate = const_cast<lang::MirConstructorInstance *>(
        changed.findConstructorInstance(constructor->id));
    mutate(*candidate);
    const auto bindings =
        lang::cppMirOwnedParameterFieldBindings(changed, *candidate);
    expect(!bindings, message);
    expect(
        !lang::CppMirBodyEmitter(changed, map).supportsFailureBodyText(address),
        message);
  };
  const auto changeStored = [](auto &candidate, const auto &mutate) {
    for (auto &loan : candidate.body.loans) {
      if (loan.storedField == candidate.initializers.front().field) {
        mutate(loan);
        return;
      }
    }
  };
  rejectMutation(
      [&](auto &candidate) {
        changeStored(candidate, [&](auto &loan) {
          loan.storedField = candidate.initializers.back().field;
        });
      },
      "exclusive prefix rejects a substituted reference field");
  rejectMutation(
      [&](auto &candidate) {
        changeStored(candidate, [](auto &loan) { loan.source = 0; });
      },
      "exclusive prefix rejects a missing exact formal origin");
  rejectMutation(
      [&](auto &candidate) {
        changeStored(candidate, [](auto &loan) {
          loan.access = lang::AccessMode::ReadOnly;
        });
      },
      "exclusive prefix rejects substituted shared access");
  rejectMutation(
      [&](auto &candidate) {
        changeStored(candidate, [](auto &loan) { loan.entry = false; });
      },
      "exclusive prefix rejects a non-entry transfer");
  rejectMutation(
      [](auto &candidate) {
        candidate.body.blocks.front()
            .instructions.front()
            .operands.front()
            .place = 0;
      },
      "exclusive prefix rejects a substituted initializer referent");
  rejectMutation(
      [](auto &candidate) {
        candidate.body.blocks.front().instructions[1].fullExpressionEnd = 0;
      },
      "exclusive prefix rejects a missing initializer boundary");
}

void testOptionalPayloadCleanupComposition() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-optional-payload-cleanup-composition.gti", R"(
#include <std/optional>
#include <std/string>
#include <std/vector>
class Message {
  std::optional<std::string> text{};
public:
  Message() {}
  ~Message() { int zero = 0; int result = 1 / zero; }
};
class Quiet {
  int marker;
public:
  Quiet(int value) : marker(value) {}
  ~Quiet() { std::println(marker); }
};
class EmptyHolder {
  std::optional<Quiet> first{};
  std::optional<Quiet> second{};
public:
  EmptyHolder(int value) {}
};
int main() {
  EmptyHolder empty{0};
  mut std::vector<Message> messages{};
  messages.push_back(Message());
  messages.erase(std::size_t(0));
  return 0;
}
)");
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "optional payload cleanup composition must produce verified MIR");
  if (!frontend.canGenerateCode() || !frontend.mir.valid())
    return;
  lang::MirBodyAddress address{};
  lang::MirInstructionId eraseId = 0;
  const auto findPayloadCleanup = [&](const auto &self,
                                      auto &recipe) -> decltype(&recipe) {
    if (recipe.type.kind == lang::SemanticType::Enum &&
        frontend.mir.findPayloadEnum(recipe.type))
      return &recipe;
    for (auto &child : recipe.aggregate)
      if (auto *found = self(self, child))
        return found;
    return nullptr;
  };
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy &&
            instruction.elementCleanup &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor &&
            findPayloadCleanup(findPayloadCleanup,
                               *instruction.elementCleanup)) {
          address = {.kind = lang::MirBodyKind::Function, .owner = function.id};
          eraseId = instruction.id;
        }
      }
    }
  }
  expect(eraseId != 0,
         "source prefix removal must retain exact general object destruction");
  if (!eraseId)
    return;
  const auto *body = lang::findMirBody(frontend.mir, address);
  const auto *erase = findPreflightInstruction(*body, eraseId);
  expect(erase->elementCleanup &&
             erase->definedFailure.propagation ==
                 lang::FailurePropagationKind::Destructor &&
             findPayloadCleanup(findPayloadCleanup, *erase->elementCleanup),
         "nested source optional enum must preserve exact payload cleanup and "
         "enclosing destructor failure propagation");
  if (!erase->elementCleanup)
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  expect(lang::CppMirBodyEmitter(frontend.mir, map)
             .supportsFailureBodyText(address),
         "nested enum cleanup must remain an admitted MIR operation");
  const auto *enumCleanup =
      findPayloadCleanup(findPayloadCleanup, *erase->elementCleanup);
  const auto *enumeration =
      enumCleanup ? frontend.mir.findPayloadEnum(enumCleanup->type) : nullptr;
  expect(enumeration && enumeration->variants.size() == 2 &&
             enumeration->variants.front().payloadTypes.empty() &&
             enumeration->variants.back().payloadTypes.size() == 1 &&
             enumCleanup->aggregate.size() == 1 &&
             enumCleanup->aggregate.front().payloadVariant ==
                 enumeration->variants.back().variantIndex &&
             enumCleanup->aggregate.front().payloadField == 0 &&
             enumCleanup->aggregate.front().type ==
                 enumeration->variants.back().payloadTypes.front(),
         "source optional cleanup must select its exact engaged enum field, "
         "with no cleanup for the empty alternative");
  bool sawDormantCleanup = false;
  for (const auto &owner : frontend.mir.classInstances()) {
    for (const auto &block : owner.fieldInitializers.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Drop ||
            instruction.definedFailure.propagation !=
                lang::FailurePropagationKind::Destructor ||
            instruction.lifecycle.size() != 1)
          continue;
        const auto *drop = owner.fieldInitializers.findDropObligation(
            instruction.lifecycle.front().source);
        if (!drop || !findPayloadCleanup(findPayloadCleanup, drop->dropType))
          continue;
        const auto targets = lang::mirDropDestructorTargets(drop->dropType);
        if (targets.empty())
          continue;
        sawDormantCleanup = true;
        expect(lang::CppMirBodyEmitter(frontend.mir, map)
                   .initializerSchedule(
                       {.kind = lang::MirBodyKind::FieldInitializers,
                        .owner = owner.id})
                   .supported,
               "contained declaration initialization must admit its exact "
               "dormant optional rollback independently of payload effects");
      }
    }
  }
  expect(sawDormantCleanup,
         "fixture must retain a dormant optional cleanup rollback edge");
  bool sawInfallibleStatusOnly = false;
  bool sawRequiredContainedWrapper = false;
  for (const auto &row : map.containedConstructors()) {
    const auto *constructor =
        frontend.mir.findConstructorInstance(row.constructor);
    if (!constructor ||
        !lang::cppMirConstructorStatusCannotFail(frontend.mir,
                                                 constructor->id) ||
        !lang::cppMirConstructorRequiresFieldPlacement(frontend.mir,
                                                       *constructor))
      continue;
    sawInfallibleStatusOnly = true;
    const lang::MirBodyAddress constructorAddress{
        .kind = lang::MirBodyKind::Constructor, .owner = constructor->id};
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(!emitter.supportsBodyText(constructorAddress) &&
               emitter.supportsFailureBodyText(constructorAddress),
           "infallible field placement must retain its status-only body");
    std::optional<lang::MirBodyAddress> requiredConstructor;
    for (const auto &consumer : frontend.mir.constructorInstances()) {
      bool consumesTarget = false;
      for (const auto &block : consumer.body.blocks)
        for (const auto &instruction : block.instructions)
          consumesTarget |=
              instruction.kind == lang::MirInstructionKind::Construct &&
              instruction.constructorTarget == row.constructor;
      if (!consumesTarget)
        continue;
      const auto schedule = emitter.initializerSchedule(
          {.kind = lang::MirBodyKind::FieldInitializers,
           .owner = consumer.owner});
      expect(schedule.supported && !schedule.fields.empty() &&
                 std::all_of(
                     schedule.fields.begin(), schedule.fields.end(),
                     [](const auto &field) { return field.spelling.empty(); }),
             "constructor-owned defaults must not also execute as native "
             "field initializers");
      requiredConstructor = {.kind = lang::MirBodyKind::Constructor,
                             .owner = consumer.id};
      expect(emitter.supportsFailureBodyText(*requiredConstructor),
             "the exact constructor executes its status-only field target");
      break;
    }
    if (!requiredConstructor)
      continue;
    sawRequiredContainedWrapper = true;
    auto missingRows =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
    std::erase_if(missingRows.types, [&](const auto &candidate) {
      return candidate.type == row.ownerType;
    });
    const lang::CppMirBodyEmissionMap missingMap(std::move(missingRows));
    expect(!lang::CppMirBodyEmitter(frontend.mir, missingMap)
                .supportsFailureBodyText(*requiredConstructor),
           "removing the selected field target's type representation must "
           "fail closed even when its exact constructor cannot fail");
  }
  expect(sawInfallibleStatusOnly,
         "fixture must retain an infallible status-only initializer wrapper");
  expect(sawRequiredContainedWrapper,
         "a concrete constructor schedule must execute the status-only field "
         "construction");
  const auto rejectMutation = [&](const auto &mutate,
                                  std::string_view message) {
    auto changed = frontend.mir;
    auto *changedBody = lang::findMirBody(changed, address);
    auto *changedErase = findPreflightInstruction(*changedBody, eraseId);
    mutate(*changedErase);
    expect(!lang::verifyMirProgram(changed).valid() &&
               !lang::CppMirBodyEmitter(changed, map).analyzeProgram().ready(),
           message);
  };
  rejectMutation([](auto &erase) { erase.elementCleanup.reset(); },
                 "nested enum cleanup cannot erase its removal recipe");
  rejectMutation(
      [&](auto &erase) {
        auto *payload =
            findPayloadCleanup(findPayloadCleanup, *erase.elementCleanup);
        if (payload)
          payload->aggregate.clear();
      },
      "optional cleanup cannot omit its engaged payload recipe");
  rejectMutation(
      [&](auto &erase) {
        auto *payload =
            findPayloadCleanup(findPayloadCleanup, *erase.elementCleanup);
        if (payload && !payload->aggregate.empty())
          payload->aggregate.front().type = lang::SemanticType::Int32;
      },
      "optional cleanup cannot replace its exact payload type");
  rejectMutation(
      [&](auto &erase) {
        auto *payload =
            findPayloadCleanup(findPayloadCleanup, *erase.elementCleanup);
        if (payload && !payload->aggregate.empty())
          payload->aggregate.front().payloadVariant = 0;
      },
      "optional cleanup cannot route its payload destructor through the "
      "empty alternative");
  rejectMutation(
      [&](auto &erase) {
        auto *payload =
            findPayloadCleanup(findPayloadCleanup, *erase.elementCleanup);
        if (payload && !payload->aggregate.empty())
          payload->aggregate.front().payloadField = 1;
      },
      "optional cleanup cannot substitute an absent payload field");
  rejectMutation(
      [&](auto &erase) {
        auto *payload =
            findPayloadCleanup(findPayloadCleanup, *erase.elementCleanup);
        if (payload && !payload->aggregate.empty())
          payload->aggregate.front().payloadVariant.reset();
      },
      "optional cleanup cannot discard its active-alternative guard");
  rejectMutation(
      [&](auto &erase) {
        auto *payload =
            findPayloadCleanup(findPayloadCleanup, *erase.elementCleanup);
        if (payload)
          for (const auto &candidate : frontend.mir.payloadEnums())
            if (candidate.type != payload->type) {
              payload->type = candidate.type;
              break;
            }
      },
      "optional cleanup cannot substitute a distinct concrete enum identity");
  rejectMutation(
      [](auto &erase) {
        erase.definedFailure.propagation = lang::FailurePropagationKind::None;
      },
      "nested cleanup cannot erase its propagated failure");
}

void testSourceStringCStringAdmissionFailsClosed() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-source-string-c-string.gti", R"(
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
        if (instruction.intrinsic == lang::IntrinsicKind::CStringFromAddress) {
          observers.push_back(
              {function.id, instruction.id, instruction.intrinsic});
        }
      }
    }
  }
  expect(observers.size() == 1,
         "both source string policies should share one concrete general "
         "C-string owner-witness bridge, not separate private observers");

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
           "a verified C-string owner-witness bridge should be admitted by the "
           "complete C++ text vocabulary");
    if (ready) {
      const lang::CppMirBodyEmissionText emitted =
          function->mayRaiseDefinedFailure
              ? exactEmitter.emitFailureBodyText(
                    address, "source-string-c-string-regression-v1", 1)
              : exactEmitter.emitBodyText(
                    address, "source-string-c-string-regression-v1", 1);
      expect(emitted.emitted() &&
                 emitted.text.find(".native_address()") != std::string::npos &&
                 emitted.text.find("(void)(") != std::string::npos,
             "C-string representation consumes the verified address and "
             "nominated owner rather than private container policy");
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
           "a C-string bridge without its exact address/witness inputs should "
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
  // Source-defined propagation invokes ordinary checked protocol operations.
  // The former builtin's zero-site extraction/state-branch shortcut no longer
  // exists: wrong-state access retains the callee's defined-failure edge.
  const auto frontend =
      analyzeWithStandardLibrary("source-result-propagation-proof.gti", R"(
expected<int, int> forward(expected<int, int> result, int divisor) {
  int checked = 1 / divisor;
  return ?result;
}
int main() {
  auto success = forward(expected<int, int>(7), 1);
  auto failure = forward(expected<int, int>(unexpected(9)), 1);
  return success.value() + failure.error() - 16;
}
)");
  expect(
      frontend.canGenerateCode(),
      "source result propagation must lower through ordinary protocol calls");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "source result protocol bodies must have complete native admission");
  unsigned protocolCalls = 0;
  unsigned trackedFailures = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    const bool propagationBody =
        hir && hir->source && hir->source->name().lexeme == "forward";
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        expect(
            instruction.intrinsic != lang::IntrinsicKind::ExpectedValue &&
                instruction.intrinsic != lang::IntrinsicKind::ExpectedError &&
                instruction.operation != lang::MirOperation::ExpectedHasValue,
            "source propagation must not recover builtin extraction authority");
        if (instruction.intrinsic == lang::IntrinsicKind::FailInactiveValue ||
            instruction.intrinsic == lang::IntrinsicKind::FailInactiveError) {
          ++trackedFailures;
          expect(instruction.neverReturns && instruction.tracksCaller &&
                     instruction.forwardsCallerLocation &&
                     instruction.localFailureSites.size() == 1,
                 "source observers retain tracked terminal checked-access "
                 "failure");
          for (unsigned mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *body = lang::findMirBody(
                malformed, {lang::MirBodyKind::Function, function.id});
            auto *changed = findPreflightInstruction(*body, instruction.id);
            if (mutation == 0)
              changed->localFailureSites.clear();
            if (mutation == 1)
              changed->neverReturns = false;
            if (mutation == 2)
              changed->callerLocationSite = 0;
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "checked source failure cannot lose its site, terminal or "
                   "caller proof");
          }
        }
        if (!propagationBody ||
            instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.functionTarget)
          continue;
        const auto *target =
            frontend.hir.findFunctionInstance(*instruction.functionTarget);
        if (!target || !target->source)
          continue;
        const auto name = target->source->name().lexeme;
        if (name != "has_value" && name != "take_value" &&
            name != "take_residual")
          continue;
        ++protocolCalls;
        for (unsigned mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto *changed = findPreflightInstruction(*body, instruction.id);
          if (mutation == 0)
            changed->functionTarget.reset();
          if (mutation == 1) {
            expect(!changed->parameterTypes.empty(),
                   "result protocol operation has its checked carrier input");
            if (changed->parameterTypes.empty())
              continue;
            changed->parameterTypes.front() = lang::SemanticType::Bool;
          }
          if (mutation == 2)
            changed->info.type = lang::SemanticType::Void;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "ordinary propagation cannot substitute its target, input or "
                 "output contract");
        }
      }
    }
  }
  expect(protocolCalls == 3 && trackedFailures >= 2,
         "success/residual propagation and both checked observer failures are "
         "covered");
  if (protocolCalls != 3 || trackedFailures < 2)
    std::cerr << "source propagation coverage: operations=" << protocolCalls
              << " checked failures=" << trackedFailures << '\n';
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
  lang::MirPlaceId temporary = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &producer : block.instructions) {
        if ((producer.kind != lang::MirInstructionKind::Call &&
             producer.kind != lang::MirInstructionKind::Construct) ||
            !producer.result || producer.functionTarget || producer.receiver ||
            !producer.operands.empty() ||
            producer.info.type.kind != lang::SemanticType::Class) {
          continue;
        }
        if (producer.constructorTarget) {
          const auto *constructor =
              frontend.mir.findConstructorInstance(*producer.constructorTarget);
          const auto *owner =
              constructor == nullptr
                  ? nullptr
                  : frontend.mir.findClassInstance(constructor->owner);
          expect(owner != nullptr && owner->type == producer.info.type &&
                     constructor->parameterTypes.empty(),
                 "selected default construction retains its exact owner and "
                 "empty parameter signature");
        }
        for (const lang::MirBlock &candidate : function.body.blocks) {
          for (const lang::MirInstruction &consumer : candidate.instructions) {
            if (consumer.kind == lang::MirInstructionKind::CallInput &&
                consumer.callInputKind == lang::HirCallInputKind::MoveValue &&
                consumer.preparedParameterDrop && consumer.destination &&
                consumer.operands.size() == 1 &&
                consumer.operands.front().kind == lang::MirOperandKind::Value &&
                consumer.operands.front().value == *producer.result) {
              selected = &function;
              temporary = *consumer.destination;
            }
          }
        }
      }
    }
  }
  expect(selected != nullptr,
         "the fixture should stage a default-constructed class SSA value into "
         "one prepared call parameter");
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
  const std::string name = "__gti_mir_p_" + std::to_string(temporary);
  expect(text.find("> " + name + ";") != std::string::npos &&
             text.find(name + ".construct();") != std::string::npos &&
             text.find("std::move(" + name + ".get())") != std::string::npos &&
             text.find(name + ".destroy();") != std::string::npos,
         "the class temporary should construct, transfer, and retire through "
         "one verified prepared-parameter lifetime slot");
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
  // C-linkage callees use their private native identity, not the C name.
  const std::string constructMarker =
      ".construct_from([&]() { return __gti_c_fn_";
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

  auto nativeForm =
      analyzeWithStandardLibrary("cpp-mir-native-parameter-field.gti", R"(
[[c_abi]] struct Native { const int* pointer; int value; };
class Holder {
public:
  Native inner;
  Holder(Native payload) : inner(std::move(payload)) {}
};
int read(Native value) { Holder holder{value}; return holder.inner.value; }
int main() { return 0; }
)");
  expect(nativeForm.canGenerateCode(),
         "native parameter field fixture is valid");
  auto *nativeConstructor = findHolderConstructor(nativeForm);
  if (!nativeForm.canGenerateCode() || nativeConstructor == nullptr)
    return;
  const auto nativeBindings = lang::cppMirOwnedParameterFieldBindings(
      nativeForm.mir, *nativeConstructor);
  expect(nativeBindings && nativeBindings->size() == 1,
         "trivial native field move retains exact parameter publication proof");
  const lang::CppMirBodyEmissionMap nativeMap(
      buildRows(nativeForm, nativeForm.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter nativeEmitter(nativeForm.mir, nativeMap);
  const lang::MirBodyAddress nativeAddress{
      .kind = lang::MirBodyKind::Constructor, .owner = nativeConstructor->id};
  expect(nativeEmitter.supportsFailureBodyText(nativeAddress),
         "native field movement admits its byte-preserving failure form");
  if (nativeEmitter.supportsFailureBodyText(nativeAddress)) {
    const auto text =
        nativeEmitter.emitFailureBodyText(nativeAddress, "native-field-v1", 1);
    expect(text.text.find("::gti_internal::backend::assign_representation(") !=
               std::string::npos,
           "native field publication must not perform native pointer copying");
    expect(text.text.find(", __gti_mir_arg_0);") != std::string::npos,
           "native moved field publication reads its exact source parameter");
  }
  auto *nativeOwner = const_cast<lang::MirClassInstance *>(
      nativeForm.mir.findClassInstance(nativeConstructor->owner));
  if (nativeOwner != nullptr && !nativeOwner->declaredFields.empty()) {
    nativeOwner->declaredFields.front().requiresActiveCleanup = true;
    expect(!lang::cppMirOwnedParameterFieldBindings(nativeForm.mir,
                                                    *nativeConstructor),
           "native no-drop publication rejects forged active cleanup");
  }
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

int observe(Indexed& indexed) { return indexed[uint64_t(0)]; }

int main() {
  mut Indexed indexed = Indexed();
  indexed[uint64_t(0)] += 1;
  return mutate(indexed) - observe(indexed);
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
  expect(
      !lang::cppMirNeedsTerminalSubscriptBoundaryWrapper(frontend.mir, map,
                                                         0) &&
          !lang::cppMirNeedsTerminalSubscriptBoundaryWrapper(
              frontend.mir, map, frontend.mir.functionInstances().size() + 1),
      "unresolved targets cannot require a subscript boundary");
  std::size_t selectedBoundaries = 0;
  std::size_t readOnlyInstances = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (!function.overloadedOperator ||
        *function.overloadedOperator != lang::OverloadedOperator::Subscript) {
      expect(!lang::cppMirNeedsTerminalSubscriptBoundaryWrapper(
                 frontend.mir, map, function.id),
             "ordinary functions cannot select a subscript boundary even "
             "when other bodies use class-indexed places");
      continue;
    }
    readOnlyInstances +=
        function.receiverMutability == lang::ReceiverMutability::ReadOnly;
    if (lang::cppMirNeedsTerminalSubscriptBoundaryWrapper(frontend.mir, map,
                                                          function.id)) {
      ++selectedBoundaries;
      expect(function.receiverMutability == lang::ReceiverMutability::Mutable,
             "closed compound places need only their selected mutable accessor "
             "boundary");
    }
  }
  expect(selectedBoundaries == 0 && readOnlyInstances == 1,
         "staged accessor results need no implicit-index terminal wrapper; "
         "the explicit read-only overload remains selected");

  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t accessorCalls = 0;
  std::size_t checkedReferences = 0;
  std::size_t checkedSchedules = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    expect(emitter.supportsFailureBodyText(
               {.kind = lang::MirBodyKind::Function, .owner = function.id}),
           "the backend admits the exact staged accessor and primitive update "
           "schedule");
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *target =
            instruction.functionTarget
                ? frontend.mir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (instruction.kind == lang::MirInstructionKind::Call && target &&
            target->overloadedOperator == lang::OverloadedOperator::Subscript)
          ++accessorCalls;
        if (instruction.kind == lang::MirInstructionKind::Borrow &&
            !instruction.localFailureSites.empty()) {
          ++checkedReferences;
          expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                     block.terminator.invokeInstruction == instruction.id,
                 "bounds checks precede reference publication on the exact "
                 "Borrow/Invoke edge");
        }
        if (instruction.kind != lang::MirInstructionKind::Compute ||
            instruction.operation != lang::MirOperation::Multiply ||
            instruction.operands.size() != 2 || !instruction.result)
          continue;
        const auto *prior =
            instruction.operands.front().kind == lang::MirOperandKind::Value
                ? function.body.findValue(instruction.operands.front().value)
                : nullptr;
        const auto *load =
            prior ? findPreflightInstruction(function.body, prior->definition)
                  : nullptr;
        const auto *place =
            load && load->operands.size() == 1
                ? function.body.findPlace(load->operands.front().place)
                : nullptr;
        const auto *loan = place && place->root == lang::MirPlaceRootKind::Loan
                               ? function.body.findLoan(place->loan)
                               : nullptr;
        expect(load && load->kind == lang::MirInstructionKind::Load && loan &&
                   loan->kind == lang::MirLoanKind::CallResult &&
                   loan->access == lang::AccessMode::Mutable &&
                   load->id < instruction.id &&
                   instruction.localFailureSites.size() == 1 &&
                   instruction.definedFailure.localOrigins.size() == 1 &&
                   block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "multiply reads one staged mutable referent and owns its "
               "overflow failure before committing");
        std::size_t commits = 0;
        for (const auto &success : function.body.blocks)
          for (const auto &assign : success.instructions)
            if (assign.kind == lang::MirInstructionKind::Assign &&
                assign.operation == lang::MirOperation::Assign &&
                assign.operands.size() == 1 &&
                assign.operands.front().kind == lang::MirOperandKind::Value &&
                assign.operands.front().value == *instruction.result) {
              ++commits;
              expect(place && assign.destination == place->id &&
                         assign.id > instruction.id,
                     "the only committing write retains the exact loaded "
                     "referent after successful arithmetic");
            }
        expect(commits == 1, "the staged multiplication commits exactly once");
        ++checkedSchedules;
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto *producerBlock = changed->body.findBlock(block.id);
        producerBlock->terminator = {.kind = lang::MirTerminatorKind::Goto,
                                     .target =
                                         producerBlock->terminator.target};
        lang::rebuildMirReachability(changed->body);
        (void)lang::rebuildMirValueUses(changed->body);
        expect(!lang::verifyMirProgram(malformed).valid(),
               "staged multiplication cannot erase its checked failure edge");
      }
    }
  }
  expect(accessorCalls == 4 && checkedReferences == 2 && checkedSchedules == 1,
         "each source subscript evaluates once, both overloads check their "
         "reference, and the multiplication has one primitive schedule");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "the verified accessor schedules produce the detached artifact");
}

constexpr NamedTest kTests[] = {
    {"testPropagationOnlyFailureContext", testPropagationOnlyFailureContext},
    {"testBoundedSecondaryFailureRecordStorage",
     testBoundedSecondaryFailureRecordStorage},
    {"testTypedCleanupAdapterInterning", testTypedCleanupAdapterInterning},
    {"testOwningPayloadEnumRepresentationAdmission",
     testOwningPayloadEnumRepresentationAdmission},
    {"testOwningPayloadEnumTransportAdmissionFailsClosed",
     testOwningPayloadEnumTransportAdmissionFailsClosed},
    {"testLambdaOwningTransportAdmission", testLambdaOwningTransportAdmission},
    {"testConditionalOwningArgumentReceiverAdmission",
     testConditionalOwningArgumentReceiverAdmission},
    {"testBorrowedArgumentFallibleConditionAdmission",
     testBorrowedArgumentFallibleConditionAdmission},
    {"testBorrowedArgumentNestedOwningArgumentAdmission",
     testBorrowedArgumentNestedOwningArgumentAdmission},
    {"testConditionalExpectedArgumentReceiverAdmission",
     testConditionalExpectedArgumentReceiverAdmission},
    {"testFreshClassPayloadOperandStaging",
     testFreshClassPayloadOperandStaging},
    {"testPassiveEnumTemporaryBorrowAdmission",
     testPassiveEnumTemporaryBorrowAdmission},
    {"testPassiveEnumTemporaryObservationThenTransfer",
     testPassiveEnumTemporaryObservationThenTransfer},
    {"testDischargedCompoundOperatorVocabulary",
     testDischargedCompoundOperatorVocabulary},
    {"testPassivePayloadTagObservation", testPassivePayloadTagObservation},
    {"testConsumedPassivePayloadTagObservation",
     testConsumedPassivePayloadTagObservation},
    {"testPayloadObserverRequiresCompleteReceiver",
     testPayloadObserverRequiresCompleteReceiver},
    {"testExhaustiveEnumClassification", testExhaustiveEnumClassification},
    {"testReadyBodyAndRepresentationFailures",
     testReadyBodyAndRepresentationFailures},
    {"testLargeScalarAggregateAdmission", testLargeScalarAggregateAdmission},
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
    {"testByteFieldRepresentationFailsClosed",
     testByteFieldRepresentationFailsClosed},
    {"testNativeExpectedCopyPublication", testNativeExpectedCopyPublication},
    {"testPlainExpectedPayloadConstruction",
     testPlainExpectedPayloadConstruction},
    {"testExpectedReferenceHasOnlyLoanStorage",
     testExpectedReferenceHasOnlyLoanStorage},
    {"testExpectedFieldHasOnlyPayloadStorage",
     testExpectedFieldHasOnlyPayloadStorage},
    {"testConcreteClassStaticStorage", testConcreteClassStaticStorage},
    {"testDischargedArrayReadAnalysis", testDischargedArrayReadAnalysis},
    {"testDefaultPrefixConstructorStatusAdmission",
     testDefaultPrefixConstructorStatusAdmission},
    {"testScalarPrefixConstructorStatusAdmission",
     testScalarPrefixConstructorStatusAdmission},
    {"testOwningConstructorAcrossArgumentDiamond",
     testOwningConstructorAcrossArgumentDiamond},
    {"testMutableReferencePrefixBeforeOwnedField",
     testMutableReferencePrefixBeforeOwnedField},
    {"testOptionalPayloadCleanupComposition",
     testOptionalPayloadCleanupComposition},
    {"testSourceStringCStringAdmissionFailsClosed",
     testSourceStringCStringAdmissionFailsClosed},
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
