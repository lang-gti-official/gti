#include "../src/compiler/mir_verification_support.h"
#include "cpp_mir_body_emitter_test_fixture.h"
#include "gti/mir_printer.h"

namespace {
template <typename Body>
auto *instruction(Body &body, lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &value : block.instructions)
      if (value.id == id)
        return &value;
  return static_cast<decltype(&body.blocks.front().instructions.front())>(
      nullptr);
}

void testPreparedPrefixConstructor() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/vector_prepared_constructor_failure.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto stdlib = path.parent_path().parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend().analyze(
      path, source,
      {stdlib / "prelude.gti",
       path.parent_path() / "prefix_constructor_test.gti"},
      {}, {stdlib});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &issue : lang::verifyMirProgram(frontend.mir).errors)
      std::cerr << "MIR owner " << issue.owner << " block " << issue.block
                << " instruction " << issue.instruction << ": " << issue.message
                << '\n';
  }
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "owning/reference/owner-backed-text/raw constructor inputs verify");
  if (!frontend.canGenerateCode() || !frontend.mir.valid())
    return;
  const auto map =
      lang::CppMirBodyEmissionMap(buildRows(frontend, frontend.mir));
  std::size_t tested = 0;
  std::size_t qualifiedPointers = 0;
  std::size_t nullPointers = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &append : block.instructions) {
        if (append.storagePreflight != 0 && append.parameterTypes.size() == 2 &&
            append.parameterTypes[1].kind == lang::SemanticType::RawPointer) {
          const auto *value = function.body.findValue(append.operands[1].value);
          const auto *stage =
              value == nullptr ? nullptr
                               : instruction(function.body, value->definition);
          const auto *target = append.constructorTarget
                                   ? frontend.mir.findConstructorInstance(
                                         *append.constructorTarget)
                                   : nullptr;
          expect(
              stage && stage->kind == lang::MirInstructionKind::CallInput &&
                  stage->callInputKind == lang::HirCallInputKind::Value &&
                  stage->info.type == append.parameterTypes[1] && target &&
                  target->parameterTypes ==
                      std::vector<lang::SemanticType>{stage->info.type},
              "forwarded pointer input retains the exact selected parameter");
          if (stage && stage->operands.size() == 1) {
            const auto &sourceType = stage->operands.front().type;
            qualifiedPointers +=
                sourceType.kind == lang::SemanticType::RawPointer &&
                sourceType.pointerAccess == lang::AccessMode::Mutable &&
                stage->info.type.pointerAccess == lang::AccessMode::ReadOnly;
            nullPointers += sourceType == lang::SemanticType::NullPtr;
          }
        }
        if (append.storagePreflight == 0 || append.parameterTypes.size() != 6 ||
            append.parameterTypes[1].kind != lang::SemanticType::Class)
          continue;
        ++tested;
        const auto &body = function.body;
        const auto *check = instruction(body, append.storagePreflight);
        expect(check && lang::mirStorageConstructForPreflight(
                            body, check->id) == &append,
               "append retains exact non-consuming capacity pair");
        expect(check && lang::effects(*check).readsPlace &&
                   !lang::effects(*check).writesPlace &&
                   !lang::effects(*check).invokesUserCode,
               "capacity check has no write/user-code/ownership authority");
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        const auto emitted =
            lang::CppMirBodyEmitter(frontend.mir, map)
                .emitFailureBodyText(address, "prepared-prefix", 1);
        expect(emitted.emitted() &&
                   emitted.text.find("mir_prefix_preflight_v1(") !=
                       std::string::npos &&
                   emitted.text.find("mir_prefix_construct_v1(") !=
                       std::string::npos,
               "MIR emits separate capacity and exact final-slot constructor "
               "status");
        if (emitted.emitted()) {
          const auto preflightPosition =
              emitted.text.find("mir_prefix_preflight_v1(");
          const auto invocationPosition =
              emitted.text.find("mir_prefix_construct_v1(");
          expect(preflightPosition < invocationPosition,
                 "capacity is checked before final-slot construction");
          for (const auto &operand : append.operands) {
            const auto *value = body.findValue(operand.value);
            const auto *stage =
                value ? instruction(body, value->definition) : nullptr;
            if (!stage || !stage->preparedParameterDrop || !stage->destination)
              continue;
            const auto slot =
                "__gti_mir_p_" + std::to_string(*stage->destination);
            expect(emitted.text.find(slot + ".construct(") < preflightPosition,
                   "owning parameter is materialized before capacity check");
            expect(invocationPosition != std::string::npos &&
                       emitted.text.find(slot + ".destroy();",
                                         invocationPosition) !=
                           std::string::npos,
                   "consumed prepared owner shell is retired after invocation");
          }
        }
        const auto reject = [&](const auto &mutate, std::string_view message) {
          auto forged = frontend.mir;
          auto *changedBody = lang::findMirBody(forged, address);
          auto *call = instruction(*changedBody, append.id);
          auto *preflight = instruction(*changedBody, append.storagePreflight);
          mutate(*changedBody, *call, *preflight);
          expect(lang::rebuildMirValueUses(*changedBody),
                 "mutation keeps exact use index");
          expect(!lang::verifyMirProgram(forged).valid(), message);
        };
        reject([](auto &, auto &call, auto &) { call.storagePreflight = 0; },
               "cannot erase preflight pairing");
        reject([](auto &, auto &, auto &check) { ++check.callSite; },
               "capacity check must belong to the exact call");
        reject([](auto &, auto &call,
                  auto &check) { check.operands[0] = call.operands[1]; },
               "capacity check must protect the exact storage input");
        reject(
            [](auto &, auto &call, auto &) { call.constructorTarget.reset(); },
            "non-scalar append cannot lose its exact selected constructor");
        reject(
            [](auto &, auto &call, auto &) {
              call.parameterTypes[3] = lang::SemanticType::Int32;
            },
            "selected reference parameter role cannot become a value role");
        reject([](auto &, auto &call, auto &) { call.lifecycle.clear(); },
               "owning prepared parameters must transfer exactly once");
        reject(
            [](auto &, auto &call, auto &check) {
              check.lifecycle = call.lifecycle;
              call.lifecycle.clear();
            },
            "capacity check cannot consume parameters before it succeeds");
        reject(
            [](auto &body, auto &, auto &check) {
              for (auto &block : body.blocks)
                if (block.terminator.invokeInstruction == check.id)
                  block.terminator.kind = lang::MirTerminatorKind::Goto;
            },
            "capacity failure cannot bypass caller parameter cleanup");
        reject(
            [](auto &body, auto &, auto &check) {
              for (auto &block : body.blocks)
                if (block.terminator.invokeInstruction == check.id)
                  block.terminator.elseTarget = block.terminator.target;
            },
            "capacity failure cannot reach the constructor");
      }
    }
  }
  expect(tested >= 2,
         "test must reach both direct capability and generic pack handoff");
  expect(qualifiedPointers == 1 && nullPointers == 1,
         "concrete pack inputs preserve pointer qualification and null "
         "conversion");
}

void testPrefixPointerInputRejection() {
  const auto mutablePointer =
      lang::SemanticType::rawPointerTo(lang::SemanticType::Int32);
  const auto readPointer = lang::SemanticType::rawPointerTo(
      lang::SemanticType::Int32, lang::AccessMode::ReadOnly);
  expect(!lang::mir_detail::validCallInputType(readPointer, mutablePointer,
                                               lang::HirCallInputKind::Value),
         "call-input proof never removes pointee constness");
  expect(!lang::mir_detail::validCallInputType(
             lang::SemanticType::rawPointerTo(lang::SemanticType::Float),
             readPointer, lang::HirCallInputKind::Value),
         "call-input proof never changes the pointee type");
  expect(!lang::mir_detail::validCallInputType(lang::SemanticType::Int32,
                                               readPointer,
                                               lang::HirCallInputKind::Value),
         "call-input proof never turns an integer into a pointer");
  const auto stdlib =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  for (const auto &[parameter, argument] :
       {std::pair{"int*", "const int*"}, std::pair{"const int*", "float*"}}) {
    const std::string source =
        "#include <std/vector>\nclass Entry { public: Entry(" +
        std::string(parameter) + " value) {} };\nint main() { " + argument +
        " value = nullptr; mut std::vector<Entry> values{}; "
        "[[discard]] values.emplace_back(value); return 0; }\n";
    const auto frontend =
        lang::Frontend().analyze("invalid-prefix-pointer.gti", source,
                                 {stdlib / "prelude.gti"}, {}, {stdlib});
    expect(!frontend.canGenerateCode() && !frontend.diagnostics.empty() &&
               std::none_of(
                   frontend.diagnostics.begin(), frontend.diagnostics.end(),
                   [](const auto &item) { return item.code == "GTI-B0001"; }),
           "invalid forwarded pointer conversions are rejected before backend");
  }
}
void testGeneratedPrefixTransport() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/prefix_generated_transport.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto stdlib = path.parent_path().parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend().analyze(
      path, source,
      {stdlib / "prelude.gti",
       path.parent_path() / "prefix_constructor_test.gti"},
      {}, {stdlib});
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "direct and pack-generated prefix transport have verified MIR");
  if (!frontend.canGenerateCode())
    return;
  unsigned checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (hir == nullptr || hir->source == nullptr ||
        (hir->source->name().lexeme != "append_same_type" &&
         hir->source->name().lexeme != "append_same_type_pack"))
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &append : block.instructions) {
        if (append.intrinsic != lang::IntrinsicKind::PrefixStorageAppend)
          continue;
        ++checked;
        expect(append.callSite != 0 && !append.constructorTarget &&
                   append.constructorKind == lang::ConstructorKind::Move &&
                   append.operands.size() == 2,
               "generated prefix move uses ordered inputs without an invented "
               "source constructor");
        if (append.operands.size() != 2)
          continue;
        const auto *value = function.body.findValue(append.operands[1].value);
        const auto *stage = value == nullptr
                                ? nullptr
                                : instruction(function.body, value->definition);
        expect(stage && stage->kind == lang::MirInstructionKind::CallInput &&
                   stage->callInputKind == lang::HirCallInputKind::MoveValue &&
                   stage->callSite == append.callSite &&
                   stage->preparedParameterDrop.has_value(),
               "generated movement retains the exact prepared owner instead "
               "of an untracked native temporary");
        auto forged = frontend.mir;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        auto *changed = lang::findMirBody(forged, address);
        instruction(*changed, append.id)->callSite = 0;
        expect(
            !lang::verifyMirProgram(forged).valid(),
            "generated prefix transport cannot erase its staged call identity");
      }
    }
  }
  expect(checked == 2,
         "direct and pack-generated transport both exercise the rule");
}
void testSparseFinalAddressConstructor() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/storage_final_address.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto stdlib = path.parent_path().parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend().analyze(
      path, source, {stdlib / "prelude.gti"}, {}, {stdlib});
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "sparse final-address construction verifies");
  if (!frontend.canGenerateCode())
    return;
  const auto map =
      lang::CppMirBodyEmissionMap(buildRows(frontend, frontend.mir));
  unsigned checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.intrinsic != lang::IntrinsicKind::StorageConstruct ||
            call.storagePreflight == 0)
          continue;
        ++checked;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        const auto *check = instruction(function.body, call.storagePreflight);
        expect(check && check->operands.size() == 2 &&
                   check->operands[1] == call.operands[1] &&
                   lang::mirStorageConstructForPreflight(function.body,
                                                         check->id) == &call,
               "sparse preflight protects the exact allocation and index");
        const auto emitted = lang::CppMirBodyEmitter(frontend.mir, map)
                                 .emitFailureBodyText(address, "sparse", 1);
        expect(emitted.emitted() &&
                   emitted.text.find("mir_storage_preflight_v1(") !=
                       std::string::npos &&
                   emitted.text.find("mir_storage_construct_with_v1(") !=
                       std::string::npos,
               "sparse construction uses MIR preflight and source failure "
               "propagation");
        const auto reject = [&](const auto &mutate, std::string_view message) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          auto *changed = instruction(*body, call.id);
          auto *preflight = instruction(*body, call.storagePreflight);
          mutate(*changed, *preflight);
          expect(lang::rebuildMirValueUses(*body),
                 "sparse mutation rebuilds use index");
          expect(!lang::verifyMirProgram(forged).valid(), message);
          expect(!lang::CppMirBodyEmitter(forged, map)
                      .supportsFailureBodyText(address),
                 "sparse backend admission independently rejects the forged "
                 "contract");
        };
        reject([](auto &call, auto &) { call.storagePreflight = 0; },
               "sparse constructor cannot erase its preflight");
        reject([](auto &call,
                  auto &check) { check.operands[1] = call.operands.back(); },
               "sparse constructor cannot check another slot index");
        reject([](auto &call, auto &) { call.constructorTarget.reset(); },
               "sparse constructor cannot erase its source target");
        reject([](auto &call, auto &) { call.lifecycle.clear(); },
               "sparse constructor must transfer prepared owners exactly once");
        reject(
            [](auto &call, auto &) {
              call.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
            },
            "sparse constructor must preserve source failure propagation");
      }
    }
  }
  expect(checked != 0, "fixture exercises sparse final-address construction");
  for (const std::string &source : {std::string(R"(
#include <std/memory>
int main() {
  std::allocator allocator{};
  mut auto slots = allocator.try_storage<int>(std::size_t(1)).expect("allocation");
  mut int& value = slots.emplace(std::size_t(0), 7);
  slots.destroy(std::size_t(0));
  return value;
})"),
                                    std::string(R"(
#include <std/memory>
int& escape() {
  std::allocator allocator{};
  mut auto slots = allocator.try_storage<int>(std::size_t(1)).expect("allocation");
  return slots.emplace(std::size_t(0), 7);
}
int main() { return 0; }
)")}) {
    const auto invalid =
        lang::Frontend().analyze("invalid-storage-borrow.gti", source,
                                 {stdlib / "prelude.gti"}, {}, {stdlib});
    expect(
        !invalid.canGenerateCode() &&
            std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto &d) { return d.code == "GTI-S2017"; }) &&
            std::none_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-B0001"; }),
        "emplaced references cannot escape or survive owner mutation");
  }
}
} // namespace

int main() {
  testPreparedPrefixConstructor();
  testPrefixPointerInputRejection();
  testGeneratedPrefixTransport();
  testSparseFinalAddressConstructor();
  return failures == 0 ? 0 : 1;
}
