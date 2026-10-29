#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const std::string source = R"gti(
import std.thread;
[[unsafe_transfer]] class task_cleanup_failure {
public:
  ~task_cleanup_failure() { int32_t zero = 0; int32_t value = 1 / zero; }
public:
  task_cleanup_failure() = default;
};
int main() {
  task_cleanup_failure guard = task_cleanup_failure();
  auto task = [owned = std::move(guard)]() -> void {};
  auto started = std::thread::start(std::move(task));
  if (!started.has_value()) { return 1; }
  mut std::thread worker = std::move(started).value();
  auto joined = worker.join();
  return 0;
}
)gti";
  auto target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = true;
  lang::FrontendOptions options;
  options.target = target;
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend(options).analyze(
      "expected-payload-pending-cleanup.gti", source, {library / "prelude.gti"},
      {}, {library});
  expect(frontend.canGenerateCode(), "fallible task cleanup retains valid MIR");
  if (!frontend.canGenerateCode())
    return 1;
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir, target);
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::Class ||
        function.callableParameters.empty())
      continue;
    const auto pending = std::find_if(
        function.body.dropObligations.begin(),
        function.body.dropObligations.end(), [](const auto &drop) {
          return drop.kind == lang::MirDropObligationKind::ReturnResult;
        });
    if (pending == function.body.dropObligations.end())
      continue;
    ++checked;
    auto rawRows =
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23, target);
    const auto *declaration =
        lowered.findFunctionDeclaration(function.declaration);
    expect(declaration != nullptr &&
               lang::cppMirApplyCallableTemplateTypeOverlays(
                   rawRows, lowered, lang::CppStandard::Cpp23, *declaration,
                   function)
                       .value_or(0) != 0,
           "generic task body has exact callable representation overlay");
    const lang::CppMirBodyEmissionMap rows(std::move(rawRows));
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    expect(lang::CppMirBodyEmitter(frontend.mir, rows)
               .supportsFailureBodyText(address),
           "source result supports cleanup-only Invoke success chain");
    const auto text = lang::CppMirBodyEmitter(frontend.mir, rows)
                          .emitFailureBodyText(address, "test", 0);
    expect(text.emitted(), "pending payload emits complete status body");
    const auto *place = function.body.findPlace(pending->place);
    // expected<T, E> is an ordinary class result constructed in place in the
    // caller's output, so its pending rollback destroys that constructed
    // output object rather than a separate local payload slot.
    expect(text.text.find("std::destroy_at(__gti_mir_out_result); // GTI MIR "
                          "unpublished return cleanup") != std::string::npos,
           "pending rollback destroys local payload, not uninitialized "
           "Expected output");
    expect(text.text.find("__gti_mir_out_result->value()") == std::string::npos,
           "pending payload is not read from uninitialized output");
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      for (auto &drop : changed->body.dropObligations)
        if (drop.id == pending->id && mutation == 4)
          drop.initiallyActive = true;
      for (auto &block : changed->body.blocks) {
        for (auto &member : block.instructions) {
          for (auto &event : member.lifecycle) {
            if (event.kind == lang::MirLifecycleEventKind::Reparent &&
                event.target == pending->id) {
              if (mutation == 0)
                event.conditional = true;
              if (mutation == 1)
                event.target = event.source;
            }
            if (mutation == 2 &&
                event.kind == lang::MirLifecycleEventKind::TransferOut &&
                event.source == pending->id)
              event.failureCleanup = true;
          }
        }
        if (mutation == 3 &&
            block.terminator.kind == lang::MirTerminatorKind::Return &&
            block.terminator.value &&
            block.terminator.value->value == place->value)
          block.instructions.insert(
              block.instructions.begin(),
              lang::MirInstruction{.id = 999999,
                                   .kind = lang::MirInstructionKind::Compute});
      }
      // The result is an ordinary class constructed in the caller's output:
      // MIR verification and the backend's body analysis own its lifecycle
      // authority, not a result-specific emission route.
      const lang::CppMirBodyEmitter rejected(malformed, rows);
      expect(!lang::verifyMirProgram(malformed).valid() &&
                 (!rejected.analyze(address).ready() ||
                  !rejected.supportsFailureBodyText(address)),
             "pending payload rejects altered ownership, initial state, or "
             "effectful success chain: " +
                 std::to_string(mutation));
    }
  }
  expect(checked == 1,
         "fixture reaches one pending generic expected payload return");
  return failures ? 1 : 0;
}
