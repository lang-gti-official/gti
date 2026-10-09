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
  expect(frontend.canGenerateCode() && frontend.mir.valid(),
         "source prefix owning/reference/text/raw constructor inputs verify");
  if (!frontend.canGenerateCode())
    return;
  const auto map =
      lang::CppMirBodyEmissionMap(buildRows(frontend, frontend.mir));
  std::size_t tested = 0;
  std::size_t capacityGuards = 0;
  std::size_t qualifiedPointers = 0;
  std::size_t nullPointers = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.intrinsic == lang::IntrinsicKind::RequireRelocationCapacity) {
          ++capacityGuards;
          const auto effects = lang::effects(call);
          expect(
              !effects.writesPlace && !effects.invokesUserCode &&
                  !effects.movesValue && call.lifecycle.empty(),
              "source capacity guard cannot consume or construct an element");
        }
        if (call.kind == lang::MirInstructionKind::CallInput && call.result &&
            call.operands.size() == 1 &&
            call.info.type.kind == lang::SemanticType::RawPointer &&
            call.info.type.pointerAccess == lang::AccessMode::ReadOnly) {
          qualifiedPointers += call.operands.front().type.kind ==
                                   lang::SemanticType::RawPointer &&
                               call.operands.front().type.pointerAccess ==
                                   lang::AccessMode::Mutable;
          nullPointers +=
              call.operands.front().type == lang::SemanticType::NullPtr;
        }
        if (call.intrinsic != lang::IntrinsicKind::ObjectConstruct ||
            !call.constructorTarget || call.parameterTypes.size() != 6 ||
            call.parameterTypes[1].kind != lang::SemanticType::Class)
          continue;
        ++tested;
        const auto &body = function.body;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        expect(call.unsafeOperation ==
                   lang::UnsafeOperationKind::ObjectLifetime,
               "prefix policy uses general construction without private "
               "preflight");
        for (std::size_t index = 0; index < call.operands.size(); ++index) {
          const auto *value = body.findValue(call.operands[index].value);
          const auto *stage =
              value ? instruction(body, value->definition) : nullptr;
          expect(stage && stage->kind == lang::MirInstructionKind::CallInput &&
                     stage->callSite == call.callSite &&
                     stage->callInputRole == lang::MirCallInputRole::Argument &&
                     stage->callInputIndex == index &&
                     stage->info.type == call.parameterTypes[index],
                 "construction retains exact ordered typed input stages");
        }
        const auto emitted =
            lang::CppMirBodyEmitter(frontend.mir, map)
                .emitFailureBodyText(address, "source-prefix", 1);
        expect(emitted.emitted() &&
                   emitted.text.find("native_pointer_value(") !=
                       std::string::npos &&
                   emitted.text.find("&__gti_mir_construct_success_") !=
                       std::string::npos,
               "general MIR emits checked final-address construction");
        const auto reject = [&](const auto &mutate, std::string_view message) {
          auto forged = frontend.mir;
          auto *changedBody = lang::findMirBody(forged, address);
          auto *changedCall = instruction(*changedBody, call.id);
          mutate(*changedBody, *changedCall);
          expect(lang::rebuildMirValueUses(*changedBody),
                 "mutation keeps exact use index");
          expect(!lang::verifyMirProgram(forged).valid(), message);
          expect(!lang::CppMirBodyEmitter(forged, map)
                      .supportsFailureBodyText(address),
                 "backend independently rejects malformed construction");
        };
        reject([](auto &, auto &value) { value.callSite = 0; },
               "construction cannot erase ordered call identity");
        reject([](auto &, auto &value) { value.objectAllocation = value.id; },
               "construction cannot invent unrelated allocation authority");
        reject([](auto &, auto &value) { value.constructorTarget.reset(); },
               "construction cannot lose its selected constructor");
        reject(
            [](auto &, auto &value) {
              value.parameterTypes[3] = lang::SemanticType::Int32;
            },
            "reference parameter cannot become a value parameter");
        reject([](auto &, auto &value) { value.lifecycle.clear(); },
               "prepared owners must transfer exactly once");
        reject(
            [](auto &, auto &value) {
              value.unsafeOperation = lang::UnsafeOperationKind::None;
            },
            "construction cannot erase lifetime obligations");
        reject(
            [](auto &, auto &value) {
              value.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
            },
            "construction must preserve selected-constructor failure");
        reject(
            [](auto &changedBody, auto &value) {
              for (auto &changedBlock : changedBody.blocks)
                if (changedBlock.terminator.invokeInstruction == value.id)
                  changedBlock.terminator.kind = lang::MirTerminatorKind::Goto;
            },
            "constructor failure cannot bypass caller cleanup");
      }
    }
  }
  expect(tested != 0 && capacityGuards != 0,
         "fixture reaches source capacity checks and general construction");
  expect(qualifiedPointers != 0 && nullPointers != 0,
         "concrete packs preserve pointer qualification and null conversion");
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
    for (const auto &block : function.body.blocks) {
      for (const auto &append : block.instructions) {
        if (append.intrinsic != lang::IntrinsicKind::ObjectConstruct ||
            append.operands.size() != 2 ||
            append.parameterTypes[1].kind != lang::SemanticType::Class ||
            append.lifecycle.empty())
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
  expect(checked >= 2,
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
        if (call.intrinsic != lang::IntrinsicKind::ObjectConstruct ||
            !call.constructorTarget || call.lifecycle.empty() ||
            call.definedFailure.propagation !=
                lang::FailurePropagationKind::Constructor)
          continue;
        ++checked;
        const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                           .owner = function.id};
        expect(call.unsafeOperation ==
                       lang::UnsafeOperationKind::ObjectLifetime &&
                   !call.parameterTypes.empty() &&
                   call.parameterTypes.front().kind ==
                       lang::SemanticType::RawPointer,
               "source sparse policy uses general typed construction without "
               "private allocation authority");
        const auto emitted = lang::CppMirBodyEmitter(frontend.mir, map)
                                 .emitFailureBodyText(address, "sparse", 1);
        expect(emitted.emitted() &&
                   emitted.text.find("mir_storage_preflight_v1(") ==
                       std::string::npos &&
                   emitted.text.find("mir_storage_construct_with_v1(") ==
                       std::string::npos,
               "sparse construction emits general lifetime operations without "
               "retired private helpers");
        const auto reject = [&](const auto &mutate, std::string_view message) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          auto *changed = instruction(*body, call.id);
          mutate(*changed);
          expect(lang::rebuildMirValueUses(*body),
                 "sparse mutation rebuilds use index");
          expect(!lang::verifyMirProgram(forged).valid(), message);
          expect(!lang::CppMirBodyEmitter(forged, map)
                      .supportsFailureBodyText(address),
                 "sparse backend admission independently rejects the forged "
                 "contract");
        };
        reject([](auto &call) { call.objectAllocation = call.id; },
               "raw construction cannot invent allocation authority");
        reject(
            [](auto &call) {
              call.unsafeOperation = lang::UnsafeOperationKind::None;
            },
            "raw construction cannot erase its explicit unsafe authority");
        reject([](auto &call) { call.constructorTarget.reset(); },
               "sparse constructor cannot erase its source target");
        reject([](auto &call) { call.lifecycle.clear(); },
               "sparse constructor must transfer prepared owners exactly once");
        reject(
            [](auto &call) {
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
