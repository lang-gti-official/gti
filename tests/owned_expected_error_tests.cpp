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

bool calls(const lang::FrontendResult &frontend,
           const lang::MirInstruction &instruction, std::string_view name) {
  const lang::HirFunctionInstance *target =
      instruction.kind == lang::MirInstructionKind::Call &&
              instruction.functionTarget
          ? frontend.hir.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  return target != nullptr && target->source != nullptr &&
         target->source->name().lexeme == name;
}

const lang::MirInstruction *definition(const lang::MirBody &body,
                                       lang::MirValueId value) {
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.result == value)
        return &instruction;
  return nullptr;
}

// Backend admission is body analysis plus the text vocabulary. The source
// expected class publishes through the generic class vocabulary, which leaves
// lifecycle forgeries of its owned results to body analysis.
bool admitted(const lang::MirProgram &program,
              const lang::CppMirBodyEmissionMap &rows,
              const lang::MirFunctionInstance &function) {
  const lang::CppMirBodyEmitter emitter(program, rows);
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, function.id};
  return emitter.analyze(address).ready() &&
         (function.mayRaiseDefinedFailure
              ? emitter.supportsFailureBodyText(address)
              : emitter.supportsBodyText(address));
}
} // namespace

int main() {
  {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures/expected_void_owned_propagation.gti";
    std::ifstream input(path);
    expect(input.good(), "owning void propagation fixture exists");
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode() &&
               lang::verifyMirProgram(frontend.mir).valid(),
           "void propagation with a lexical owning error has verified MIR");
    if (frontend.canGenerateCode()) {
      const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
      std::size_t checked = 0;
      for (const auto &function : frontend.mir.functionInstances()) {
        const auto *hir = frontend.hir.findFunctionInstance(function.id);
        if (!hir || !hir->source || hir->source->name().lexeme != "pipeline")
          continue;
        expect(
            admitted(frontend.mir, rows, function),
            "void Expected success and error observers share an emitted slot");
        for (const auto &block : function.body.blocks) {
          for (const auto &instruction : block.instructions) {
            // `?` observes a void success through the tryable protocol.
            if (!calls(frontend, instruction, "take_value") ||
                instruction.info.type != lang::SemanticType::Void ||
                instruction.operands.size() != 1 ||
                !isExpected(frontend, instruction.operands.front().type))
              continue;
            ++checked;
            expect(!instruction.result,
                   "void success remains an explicit result-less observation");
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.id == instruction.id)
                  candidate.info.type = lang::SemanticType::Int32;
            expect(!lang::verifyMirProgram(malformed).valid() &&
                       !admitted(malformed, rows, *changed),
                   "admission still rejects a forged void-success result type");
          }
        }
      }
      expect(checked == 1, "one void-success observation in the loop");
    }
  }
  const auto passive = analyzeWithStandardLibrary("passive_error.gti", R"(
class Error {
public:
  int code = 0;
  Error(int code) : code(code) {}
  Error(Error& other) = delete;
  Error(Error&& other) = default;
};
expected<int, Error> make(bool fail, int divisor) {
  int checked = 1 / divisor;
  if (fail) { return unexpected(Error(29)); }
  return checked;
}
expected<int, Error> forward(bool fail) {
  int value = ?make(fail, 1);
  return value;
}
expected<void, Error> complete(bool fail) {
  int value = ?make(fail, 1);
  return;
}
)");
  expect(passive.canGenerateCode(), "cleanup-free move-only error is valid");
  if (passive.canGenerateCode()) {
    const lang::CppMirBodyEmissionMap passiveRows(
        buildRows(passive, passive.mir));
    std::size_t checked = 0;
    for (const auto &function : passive.mir.functionInstances()) {
      const auto *hir = passive.hir.findFunctionInstance(function.id);
      if (hir == nullptr ||
          hir->sourceUnit != passive.sourceGraph.entryUnit() ||
          !isExpected(passive, function.returnType))
        continue;
      ++checked;
      expect(!lang::cppMirExpectedClassPlacementResult(passive.mir, passiveRows,
                                                       function.returnType),
             "cleanup-free error keeps the ordinary Expected result ABI");
      expect(admitted(passive.mir, passiveRows, function),
             "cleanup-free error propagation remains emit-ready");
    }
    expect(checked == 3, "scalar and void cleanup-free error results checked");
  }
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owned_expected_error.gti";
  std::ifstream input(path);
  expect(input.good(), "owning error fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "owning error construction and forwarded Expected results have valid "
         "MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t wrappers = 0;
  std::size_t publications = 0;
  std::size_t bodies = 0;
  std::size_t passiveBindings = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source)
      continue;
    const std::string &name = hir->source->name().lexeme;
    if (name != "success" && name != "direct_error" &&
        name != "factory_error" && name != "moved_error" &&
        name != "forwarded_error" && name != "cloned_text" &&
        name != "text_error" && name != "checked_result" && name != "main" &&
        name != "void_success" && name != "void_error" &&
        name != "forwarded_text")
      continue;
    ++bodies;
    expect(admitted(frontend.mir, rows, function),
           "owning-error Expected body is admitted: " + name);
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        // The implicit expected(T) constructor converts the passive success
        // value; its owned class result is reparented into the binding.
        const auto *converted =
            instruction.kind == lang::MirInstructionKind::Initialize &&
                    instruction.operands.size() == 1
                ? definition(function.body, instruction.operands.front().value)
                : nullptr;
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            destination && isExpected(frontend, destination->type) &&
            destination->type.arguments.size() == 2 &&
            destination->type.arguments.front() == lang::SemanticType::Int32 &&
            converted != nullptr &&
            converted->kind == lang::MirInstructionKind::Construct &&
            converted->operands.size() == 1 &&
            converted->operands.front().type == lang::SemanticType::Int32) {
          ++passiveBindings;
          expect(instruction.lifecycle.size() == 1 &&
                     instruction.lifecycle.front().kind ==
                         lang::MirLifecycleEventKind::Reparent &&
                     converted->successResultDrop &&
                     instruction.lifecycle.front().source ==
                         *converted->successResultDrop,
                 "passive success activates its owning-alternative binding");
          if (instruction.lifecycle.size() != 1)
            continue;
          const auto dropId = instruction.lifecycle.front().target;
          for (int mutation = 0; mutation != 7; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.id == instruction.id) {
                  if (mutation == 0)
                    candidate.lifecycle.clear();
                  if (mutation == 1)
                    candidate.lifecycle.front().target = 0;
                  if (mutation == 2)
                    candidate.lifecycle.front().conditional = true;
                  if (mutation == 3)
                    candidate.lifecycle.front().source = dropId;
                }
            for (auto &drop : changed->body.dropObligations)
              if (drop.id == dropId) {
                if (mutation == 4)
                  drop.dropType.type = lang::SemanticType::Int32;
                if (mutation == 5)
                  drop.initiallyActive = true;
                if (mutation == 6)
                  drop.place = 0;
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "passive Expected success requires exact binding activation "
                   "and drop metadata");
            expect(!admitted(malformed, rows, function),
                   "emission rejects malformed passive Expected binding "
                   "publication: " +
                       std::to_string(mutation));
          }
        }
        // unexpected(e) is the prelude's source factory: it consumes its
        // prepared payload and activates its wrapper on the success edge.
        if (!calls(frontend, instruction, "unexpected"))
          continue;
        ++wrappers;
        expect(
            instruction.lifecycle.size() == 1 &&
                instruction.lifecycle.front().kind ==
                    lang::MirLifecycleEventKind::TransferOut &&
                instruction.successResultDrop &&
                block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                block.terminator.invokeInstruction == instruction.id,
            "Unexpected transfers one exact payload and activates its wrapper");
        if (instruction.lifecycle.size() != 1 || !instruction.successResultDrop)
          continue;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &candidateBlock : changed->body.blocks)
            for (auto &candidate : candidateBlock.instructions)
              if (candidate.id == instruction.id) {
                if (mutation == 0)
                  candidate.lifecycle.front().source =
                      *candidate.successResultDrop;
                if (mutation == 1)
                  candidate.lifecycle.erase(candidate.lifecycle.begin());
                if (mutation == 2)
                  candidate.successResultDrop =
                      candidate.lifecycle.front().source;
                if (mutation == 3)
                  candidate.successResultDrop.reset();
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "MIR rejects missing or wrong Unexpected source/wrapper "
                 "ownership");
          expect(!admitted(malformed, rows, function),
                 "emission fails closed on invalid Unexpected ownership");
        }
      }
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          name == "main" || name == "success" || name == "void_success")
        continue;
      bool foundPublication = false;
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Lifecycle)
          continue;
        for (std::size_t index = 0; index < instruction.lifecycle.size();
             ++index) {
          const auto &event = instruction.lifecycle[index];
          if (event.kind != lang::MirLifecycleEventKind::TransferOut)
            continue;
          foundPublication = true;
          for (int mutation = 0; mutation != 2; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.id == instruction.id) {
                  if (mutation == 0)
                    candidate.lifecycle[index].conditional = true;
                  else
                    candidate.lifecycle.erase(
                        candidate.lifecycle.begin() +
                        static_cast<std::ptrdiff_t>(index));
                }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "Expected result publication requires its unconditional "
                   "handoff");
            expect(!admitted(malformed, rows, function),
                   "emitter cannot publish an owning result without exact "
                   "handoff: " +
                       name + " mutation " + std::to_string(mutation));
          }
        }
      }
      expect(foundPublication,
             "owning return exposes its publication event: " + name);
      publications += foundPublication;
    }
  }
  expect(bodies == 12 && wrappers == 6 && publications == 10,
         "fixture covers literal success, direct/factory/moved errors, clone "
         "and forwarding");
  expect(passiveBindings == 1,
         "fixture retains one direct passive-success owning Expected binding");
  return failures ? 1 : 0;
}
