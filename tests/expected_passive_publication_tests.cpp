#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

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
      expect(function.returnType.kind == lang::SemanticType::Expected,
             "call payload is wrapped by an Expected return");
      for (const auto &block : function.body.blocks) {
        for (const auto &call : block.instructions) {
          if (call.kind != lang::MirInstructionKind::Call ||
              !call.functionTarget || !call.result ||
              call.info.type.kind != lang::SemanticType::Class)
            continue;
          const auto uses = function.body.usesOf(*call.result);
          if (uses.size() != 1 ||
              uses.front().kind != lang::MirValueUseKind::Terminator)
            continue;
          ++wrappedCalls;
          const auto slot = "__gti_mir_v_" + std::to_string(*call.result);
          expect(!call.successResultDrop && !call.successResultDestination,
                 "passive call does not invent a semantic result obligation");
          expect(text.text.find(slot + ".get()") != std::string::npos,
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
                  candidateBlock.terminator.value->value == *call.result) {
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
              const lang::CppMirBodyEmitter rejected(malformed, rows);
              expect(!(function.mayRaiseDefinedFailure
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
        if ((call.intrinsic != lang::IntrinsicKind::ExpectedValue &&
             call.intrinsic != lang::IntrinsicKind::ExpectedError) ||
            call.info.type.kind != lang::SemanticType::Class || !call.result ||
            !call.receiver || !call.successResultDestination)
          continue;
        if (call.receiver->kind == lang::MirOperandKind::Value) {
          ++moves;
          expect(text.text.find("mir_expected_value_move_v1") !=
                     std::string::npos,
                 "move-only payload retains the consuming checked helper");
          continue;
        }
        ++copies;
        expect(call.receiver->kind == lang::MirOperandKind::BorrowRead,
               "payload copy leaves the Expected receiver borrowed");
        const auto helper = call.intrinsic == lang::IntrinsicKind::ExpectedValue
                                ? "mir_expected_value_copy_v1"
                                : "mir_expected_error_copy_v1";
        expect(text.text.find(helper) != std::string::npos,
               "payload copy uses a checked placement helper");
        const auto *success = function.body.findBlock(block.terminator.target);
        expect(success != nullptr && !success->instructions.empty(),
               "copy has a success publication block");
        if (success == nullptr || success->instructions.empty())
          continue;
        const auto &initialize = success->instructions.front();
        expect(initialize.kind == lang::MirInstructionKind::Initialize &&
                   initialize.lifecycle.size() == 1,
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
              if (candidate.id == call.id && mutation == 0) {
                candidate.successResultDestination.reset();
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
          if (mutation == 0 || mutation == 1 || mutation == 2 || mutation == 5)
            expect(!rejected.supportsFailureBodyText(address),
                   "copy representation independently rejects missing or "
                   "mismatched publication");
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

} // namespace

int main() {
  testPublication();
  if (failures != 0)
    return 1;
  std::cout << "passive expected publication mutations passed\n";
  return 0;
}
