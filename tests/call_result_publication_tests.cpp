#include "cpp_mir_body_emitter_test_fixture.h"

namespace {
// The fixture's own bodies; prelude result members also consume receivers.
bool entryFunction(const lang::FrontendResult &frontend,
                   lang::HirFunctionInstanceId id) {
  const lang::HirFunctionInstance *function =
      frontend.hir.findFunctionInstance(id);
  return function != nullptr &&
         function->sourceUnit == frontend.sourceGraph.entryUnit();
}

// value() of the prelude's ordinary source class expected<T, E>.
bool expectedValueCall(const lang::FrontendResult &frontend,
                       const lang::MirBody &body,
                       const lang::MirInstruction &instruction) {
  const lang::HirFunctionInstance *target =
      instruction.kind == lang::MirInstructionKind::Call &&
              instruction.functionTarget
          ? frontend.hir.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  const lang::MirValue *receiver =
      instruction.receiver ? body.findValue(instruction.receiver->value)
                           : nullptr;
  const auto *owner =
      receiver != nullptr &&
              receiver->info.type.kind == lang::SemanticType::Class
          ? frontend.semantics.findClassType(receiver->info.type.classId)
          : nullptr;
  return target != nullptr && target->source != nullptr &&
         target->source->name().lexeme == "value" && owner != nullptr &&
         owner->declaration != nullptr &&
         owner->declaration->name().lexeme == "expected";
}
} // namespace

int main() {
  {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures/consuming_call_assignment.gti";
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(),
           "consuming result assignment has verified MIR");
    if (!frontend.canGenerateCode())
      return 1;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (!entryFunction(frontend, function.id))
        continue;
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (!instruction.functionTarget ||
              instruction.callableInvocation !=
                  lang::CallableInvocationCapability::Once ||
              instruction.info.type.kind != lang::SemanticType::Class)
            continue;
          ++checked;
          expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                     .supportsFailureBodyText(
                         {lang::MirBodyKind::Function, function.id}),
                 "source consuming receivers share the class result "
                 "publication proof");
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &candidate : changedBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (mutation == 0)
                  candidate.callableInvocation =
                      lang::CallableInvocationCapability::Mutable;
                else if (mutation == 1)
                  candidate.receiver->type = lang::SemanticType::Bool;
                else
                  candidate.successResultDrop = 999;
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "consuming receiver retains access, exact owner, and result "
                   "activation");
            expect(!lang::CppMirBodyEmitter(malformed, rows)
                        .supportsFailureBodyText(
                            {lang::MirBodyKind::Function, function.id}),
                   "consuming result admission rejects forged "
                   "receiver/publication facts");
          }
        }
      }
    }
    expect(checked == 2, "fixture reaches named and fresh consuming receivers");
  }
  for (const std::string source : {
           "struct Value { int number; Value(int n) : number(n) {} };\n"
           "expected<Value, int> attempt() { return Value(7); }\n"
           "Value extract() { mut auto source = attempt(); return "
           "std::move(source).value(); }\n"
           "int main() { return extract().number - 7; }\n",
           "class Value { int number; public: Value(int n) : number(n) {} "
           "Value(Value& other) = delete; Value(Value&& other) = default; "
           "~Value() {} };\n"
           "expected<Value, int> attempt() { return Value(7); }\n"
           "Value extract() { return attempt().value(); }\n"
           "int main() { Value value = extract(); return 0; }\n",
       }) {
    const auto frontend =
        analyzeWithStandardLibrary("checked-return.gti", source);
    expect(frontend.canGenerateCode(), "checked class return has verified MIR");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    try {
      expect(!gti_test::emitCppText(frontend).empty(),
             "checked payload return publishes without an extra object");
    } catch (const std::exception &error) {
      expect(false, error.what());
    }
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (!expectedValueCall(frontend, function.body, instruction) ||
              instruction.info.type.kind != lang::SemanticType::Class)
            continue;
          ++checked;
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &candidate : changedBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (mutation == 0)
                  candidate.info.type = lang::SemanticType::Int32;
                else if (mutation == 1) {
                  // The wrong-state check runs in the source accessor; its
                  // caller must keep propagating that failure.
                  candidate.localFailureSites.clear();
                  candidate.definedFailure.localOrigins.clear();
                  candidate.definedFailure.propagation =
                      lang::FailurePropagationKind::None;
                } else
                  candidate.successResultDrop = 999;
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "payload return retains type, wrong-state check, and drop "
                   "identity");
            // value() is an ordinary source call: the emitter's admission is
            // its vocabulary probe plus a Ready analysis, which re-verifies
            // the call's exact result and failure contracts.
            const lang::CppMirBodyEmitter emitter(malformed, rows);
            const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                               function.id};
            expect(!emitter.supportsFailureBodyText(address) ||
                       !emitter.analyze(address).ready(),
                   "payload return admission rejects forged checked "
                   "publication");
          }
        }
      }
    }
    expect(checked == 1,
           "each payload reproducer reaches its checked extraction");
  }
  for (const std::string source : {
           "[[c_abi]] struct Point { mut int32_t x; mut int32_t y; };\n"
           "extern \"C\" { Point transform(Point value); }\n"
           "Point forward(Point value) { return transform(value); }\n"
           "int main() { return 0; }\n",
           "[[c_abi]] struct Point { mut int32_t x; mut int32_t y; };\n"
           "extern \"C\" { Point make(); }\n"
           "int main() { [[discard]] make(); return 0; }\n",
       }) {
    const auto frontend =
        analyzeWithStandardLibrary("passive-native-result.gti", source);
    expect(frontend.canGenerateCode(),
           "passive call results have verified MIR");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    try {
      expect(!gti_test::emitCppText(frontend).empty(),
             "original native result return/discard must emit C++");
    } catch (const std::exception &error) {
      expect(false, error.what());
    }
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Call ||
              !instruction.functionTarget ||
              instruction.info.type.kind != lang::SemanticType::Class)
            continue;
          const auto *target =
              frontend.mir.findFunctionInstance(*instruction.functionTarget);
          if (!target || target->linkage != lang::LanguageLinkage::C)
            continue;
          ++checked;
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &candidate : changedBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (mutation == 0)
                  candidate.info.type = lang::SemanticType::Int32;
                else if (mutation == 1)
                  candidate.parameterTypes.push_back(lang::SemanticType::Bool);
                else
                  candidate.successResultDrop = 999;
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "passive result must retain type/signature/drop identity");
            expect(!lang::CppMirBodyEmitter(malformed, rows)
                        .supportsFailureBodyText(
                            {lang::MirBodyKind::Function, function.id}),
                   "native result admission rejects forged transport facts");
          }
        }
      }
    }
    expect(checked == 1, "each original native reproducer reaches its call");
  }
  if (failures)
    return 1;
  std::cout << "call-result publication proofs passed\n";
}
