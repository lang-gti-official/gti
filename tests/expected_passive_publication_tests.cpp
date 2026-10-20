#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

// expected<T, E> is the prelude's ordinary source class.
bool isExpected(const lang::FrontendResult &frontend,
                const lang::SemanticType &type) {
  const auto *info = type.kind == lang::SemanticType::Class
                         ? frontend.semantics.findClassType(type.classId)
                         : nullptr;
  return info != nullptr && info->declaration != nullptr &&
         info->declaration->name().lexeme == "expected";
}

const lang::HirFunctionInstance *
callee(const lang::FrontendResult &frontend,
       const lang::MirInstruction &instruction) {
  return instruction.kind == lang::MirInstructionKind::Call &&
                 instruction.functionTarget
             ? frontend.hir.findFunctionInstance(*instruction.functionTarget)
             : nullptr;
}

// The value()/error() accessor of expected that this call selects, if any.
std::string_view expectedAccessor(const lang::FrontendResult &frontend,
                                  const lang::MirBody &body,
                                  const lang::MirInstruction &instruction) {
  const auto *target = callee(frontend, instruction);
  const lang::MirValue *receiver =
      instruction.receiver ? body.findValue(instruction.receiver->value)
                           : nullptr;
  if (target == nullptr || target->source == nullptr || receiver == nullptr ||
      !isExpected(frontend, receiver->info.type))
    return {};
  const std::string_view name = target->source->name().lexeme;
  return name == "value" || name == "error" ? name : std::string_view{};
}

// The single instruction that consumes a value, if it has exactly one use.
const lang::MirInstruction *soleConsumer(const lang::MirBody &body,
                                         lang::MirValueId value) {
  const auto uses = body.usesOf(value);
  if (uses.size() != 1 ||
      uses.front().kind != lang::MirValueUseKind::InstructionOperand)
    return nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.id == uses.front().instruction)
        return &instruction;
  return nullptr;
}

// The CallInput that stages a call's receiver.
const lang::MirInstruction *receiverInput(const lang::MirBody &body,
                                          const lang::MirInstruction &call) {
  if (!call.receiver || call.receiver->kind != lang::MirOperandKind::Value)
    return nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::CallInput &&
          instruction.result == call.receiver->value &&
          instruction.operands.size() == 1)
        return &instruction;
  return nullptr;
}

void testPublication() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/expected_passive_publication.gti";
  std::ifstream input(path);
  expect(input.good(), "passive publication fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "passive expected publication has valid frontend MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  std::size_t copies = 0;
  std::size_t moves = 0;
  std::size_t wrappers = 0;
  std::size_t wrappedCalls = 0;
  std::size_t preparedHandoffs = 0;
  std::size_t passiveDrops = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (hir == nullptr || hir->source == nullptr)
      continue;
    const auto &name = hir->source->name().lexeme;
    if (name != "main" && name != "wrap_plain" && name != "wrap_checked" &&
        name != "wrapped" && name != "wrap_report")
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    const auto text = function.mayRaiseDefinedFailure
                          ? emitter.emitFailureBodyText(address, "passive", 1)
                          : emitter.emitBodyText(address, "passive", 1);
    expect(text.emitted(), "passive publication body emits: " + name);
    if (name != "main") {
      ++wrappers;
      expect(isExpected(frontend, function.returnType),
             "call payload is wrapped by an Expected return");
      for (const auto &block : function.body.blocks) {
        for (const auto &call : block.instructions) {
          if (call.kind != lang::MirInstructionKind::Call ||
              !call.functionTarget || !call.result ||
              call.info.type.kind != lang::SemanticType::Class)
            continue;
          // The payload reaches the returned expected as the prepared
          // argument of its implicit converting constructor.
          const auto *stage = soleConsumer(function.body, *call.result);
          const auto *wrap =
              stage != nullptr &&
                      stage->kind == lang::MirInstructionKind::CallInput &&
                      stage->result && stage->destination
                  ? soleConsumer(function.body, *stage->result)
                  : nullptr;
          if (wrap == nullptr ||
              wrap->kind != lang::MirInstructionKind::Construct ||
              !wrap->result || !isExpected(frontend, wrap->info.type))
            continue;
          const auto uses = function.body.usesOf(*wrap->result);
          if (uses.size() != 1 ||
              uses.front().kind != lang::MirValueUseKind::Terminator)
            continue;
          ++wrappedCalls;
          const auto slot =
              "__gti_mir_p_" + std::to_string(*stage->destination);
          expect(!call.successResultDrop && !call.successResultDestination,
                 "passive call does not invent a semantic result obligation");
          expect(text.text.find("std::move(" + slot + ".take())") !=
                     std::string::npos,
                 "Expected return reads its completed payload slot");
          for (int mutation = 0; mutation != 2; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = lang::findMirBody(malformed, address);
            bool hit = false;
            for (auto &candidateBlock : changed->blocks) {
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.id == call.id && mutation == 0) {
                  candidate.operands.clear();
                  hit = true;
                }
              if (mutation == 1 &&
                  candidateBlock.terminator.kind ==
                      lang::MirTerminatorKind::Return &&
                  candidateBlock.terminator.value &&
                  candidateBlock.terminator.value->value == *wrap->result) {
                candidateBlock.terminator.value->type =
                    lang::SemanticType::Int32;
                hit = true;
              }
            }
            expect(hit, "call publication mutation hits the intended record");
            expect(
                !lang::verifyMirProgram(malformed).valid(),
                "MIR rejects mismatched call staging or payload return type");
            const lang::CppMirBodyEmitter rejected(malformed, rows);
            expect(
                !rejected.analyze(address).ready() ||
                    !(function.mayRaiseDefinedFailure
                          ? rejected.supportsFailureBodyText(address)
                          : rejected.supportsBodyText(address)),
                "passive call publication fails closed on altered authority: " +
                    name + " mutation " + std::to_string(mutation));
          }
          if (!call.lifecycle.empty()) {
            ++preparedHandoffs;
            for (int mutation = 0; mutation != 4; ++mutation) {
              auto malformed = frontend.mir;
              auto *changed = lang::findMirBody(malformed, address);
              for (auto &candidateBlock : changed->blocks)
                for (auto &candidate : candidateBlock.instructions)
                  if (candidate.id == call.id) {
                    if (mutation == 0)
                      candidate.lifecycle.clear();
                    if (mutation == 1)
                      candidate.lifecycle.front().conditional = true;
                    if (mutation == 2)
                      candidate.lifecycle.front().source = 999999;
                    if (mutation == 3)
                      candidate.lifecycle.push_back(
                          candidate.lifecycle.front());
                  }
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "MIR rejects missing, conditional, mismatched or "
                     "duplicate prepared input handoff");
              const lang::CppMirBodyEmitter rejected(malformed, rows);
              expect(!(function.mayRaiseDefinedFailure
                           ? rejected.supportsFailureBodyText(address)
                           : rejected.supportsBodyText(address)),
                     "payload result representation requires exact prepared "
                     "input transfers");
            }
          }
          const auto *returned = function.body.findBlock(uses.front().block);
          for (const auto &member : returned->instructions) {
            if (member.kind != lang::MirInstructionKind::Drop)
              continue;
            ++passiveDrops;
            for (int mutation = 0; mutation != 2; ++mutation) {
              auto malformed = frontend.mir;
              auto *changed = lang::findMirBody(malformed, address);
              for (auto &candidateBlock : changed->blocks)
                for (auto &candidate : candidateBlock.instructions)
                  if (candidate.id == member.id) {
                    if (mutation == 0)
                      candidate.lifecycle.clear();
                    if (mutation == 1)
                      candidate.lifecycle.front().failureCleanup = true;
                  }
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "MIR rejects malformed passive local retirement before "
                     "Return");
              // The retired local is an ordinary class value; its drop
              // lifecycle forgeries are rejected by body analysis.
              const lang::CppMirBodyEmitter rejected(malformed, rows);
              expect(!rejected.analyze(address).ready() ||
                         !(function.mayRaiseDefinedFailure
                               ? rejected.supportsFailureBodyText(address)
                               : rejected.supportsBodyText(address)),
                     "payload return representation rejects altered local "
                     "retirement");
            }
          }
        }
      }
      continue;
    }
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        const std::string_view accessor =
            expectedAccessor(frontend, function.body, call);
        const auto *receiver = receiverInput(function.body, call);
        if (accessor.empty() || receiver == nullptr ||
            call.info.type.kind != lang::SemanticType::Class || !call.result)
          continue;
        // A consuming accessor constructs its payload in the destination.
        if (receiver->operands.front().kind == lang::MirOperandKind::Value) {
          if (!call.successResultDestination)
            continue;
          ++moves;
          expect(text.text.find("_" + std::string(accessor) +
                                "__gti_mir_failure(") != std::string::npos &&
                     text.text.find(
                         "__gti_mir_p_" +
                         std::to_string(*call.successResultDestination) +
                         ".construction_address()") != std::string::npos,
                 "move-only payload retains the consuming checked helper");
          continue;
        }
        // A read-only accessor returns a reference; the copy reads it into
        // the destination on the call's success edge.
        const auto *success = function.body.findBlock(block.terminator.target);
        const lang::MirInstruction *copy = nullptr;
        const lang::MirInstruction *publication = nullptr;
        if (call.loan && success != nullptr)
          for (const auto &candidate : success->instructions) {
            const auto *source =
                candidate.kind == lang::MirInstructionKind::Load &&
                        candidate.operands.size() == 1 &&
                        candidate.operands.front().kind ==
                            lang::MirOperandKind::Copy
                    ? function.body.findPlace(candidate.operands.front().place)
                    : nullptr;
            if (source != nullptr &&
                source->root == lang::MirPlaceRootKind::Loan &&
                source->loan == *call.loan && source->projections.empty())
              copy = &candidate;
            if (copy != nullptr && candidate.result != copy->result &&
                candidate.kind == lang::MirInstructionKind::Initialize &&
                candidate.operands.size() == 1 &&
                candidate.operands.front().value == *copy->result)
              publication = &candidate;
          }
        if (publication == nullptr)
          continue;
        ++copies;
        // A mutable carrier selects the source `mut` accessor overload.
        expect(receiver->operands.front().kind ==
                       lang::MirOperandKind::BorrowRead ||
                   receiver->operands.front().kind ==
                       lang::MirOperandKind::BorrowWrite,
               "payload copy leaves the Expected receiver borrowed");
        expect(text.text.find("_" + std::string(accessor) +
                              "__gti_mir_failure(") != std::string::npos,
               "payload copy uses a checked placement helper");
        const auto &initialize = *publication;
        expect(initialize.lifecycle.size() == 1,
               "exact sole destination is initialized on success");
        if (initialize.lifecycle.size() != 1)
          continue;
        const auto dropId = initialize.lifecycle.front().target;
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = lang::findMirBody(malformed, address);
          bool hit = false;
          for (auto &candidateBlock : changed->blocks) {
            for (auto &candidate : candidateBlock.instructions) {
              // The returned reference is the copy's publication link.
              if (candidate.id == call.id && mutation == 0) {
                candidate.loan.reset();
                hit = true;
              }
              if (candidate.id == call.id && mutation == 5) {
                candidate.receiver->type = lang::SemanticType::Int32;
                hit = true;
              }
              if (candidate.id == initialize.id) {
                if (mutation == 1) {
                  candidate.lifecycle.clear();
                  hit = true;
                }
                if (mutation == 2) {
                  candidate.lifecycle.front().conditional = true;
                  hit = true;
                }
              }
            }
            if (mutation == 4 && candidateBlock.id == block.id) {
              candidateBlock.terminator.elseTarget =
                  candidateBlock.terminator.target;
              hit = true;
            }
          }
          if (mutation == 3)
            for (auto &drop : changed->dropObligations)
              if (drop.id == dropId) {
                drop.initiallyActive = true;
                hit = true;
              }
          expect(hit, "copy mutation hits its intended authority record");
          // The destination is optional MIR metadata: without it, publication
          // needs a different representation. Receiver spelling likewise has
          // its exact type checked by representation admission. Lifecycle and
          // failure-edge corruption must be rejected by MIR verification.
          if (mutation != 0 && mutation != 5)
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "MIR rejects malformed checked payload publication: " +
                       std::to_string(mutation));
          const lang::CppMirBodyEmitter rejected(malformed, rows);
          expect(!rejected.analyze(address).ready() ||
                     !rejected.supportsFailureBodyText(address),
                 "emission admission rejects malformed payload publication: " +
                     std::to_string(mutation));
          // The copy is an ordinary member call's published result, with no
          // result-specific emission route, so the combined admission check
          // above is the representation's whole authority.
        }
      }
    }
  }
  expect(copies == 9 && moves == 1 && wrappers == 4,
         "test reaches all nine copies, one consuming extraction and four "
         "wrappers");
  expect(wrappedCalls == 4 && preparedHandoffs == 1 && passiveDrops == 1,
         "return proof covers exact calls, prepared fallback argument, and "
         "local passive retirement");
}

void testPassiveErrorCleanup() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/passive_error_cleanup.gti";
  std::ifstream input(path);
  expect(input.good(), "passive error cleanup fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "passive errors cross surrounding fallible cleanup in verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  std::size_t wrappers = 0;
  std::size_t cleanupInvokes = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source ||
        (hir->source->name().lexeme != "operation" &&
         hir->source->name().lexeme != "forward"))
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    expect(emitter.supportsFailureBodyText(address),
           "passive constructed/copied error snapshots emit before cleanup");
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        // unexpected(e) is the prelude's source factory function.
        const auto *factory = callee(frontend, instruction);
        const bool wrapper = factory != nullptr && factory->source != nullptr &&
                             factory->source->name().lexeme == "unexpected";
        const bool cleanup =
            instruction.kind == lang::MirInstructionKind::Drop &&
            block.terminator.kind == lang::MirTerminatorKind::Invoke &&
            block.terminator.invokeInstruction == instruction.id;
        if (!wrapper && !cleanup)
          continue;
        wrappers += wrapper;
        cleanupInvokes += cleanup;
        auto malformed = frontend.mir;
        auto *changed = lang::findMirBody(malformed, address);
        for (auto &candidateBlock : changed->blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (candidate.id == instruction.id) {
              if (wrapper)
                candidate.operands.front().type = lang::SemanticType::Int32;
              else
                candidate.lifecycle.clear();
            }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "passive return cannot erase payload identity or surrounding "
               "cleanup");
        const lang::CppMirBodyEmitter rejected(malformed, rows);
        expect(!rejected.analyze(address).ready() ||
                   !rejected.supportsFailureBodyText(address),
               "passive error representation fails closed on corrupted MIR");
      }
    }
  }
  expect(
      wrappers == 2 && cleanupInvokes > 0,
      "passive error tests cover direct/copy producers and fallible cleanup");
}

} // namespace

int main() {
  testPublication();
  testPassiveErrorCleanup();
  if (failures != 0)
    return 1;
  std::cout << "passive expected publication mutations passed\n";
  return 0;
}
