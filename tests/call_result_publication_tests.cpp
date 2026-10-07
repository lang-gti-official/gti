#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
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
          if (instruction.intrinsic != lang::IntrinsicKind::ExpectedValue ||
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
                  candidate.localFailureSites.clear();
                  candidate.definedFailure.localOrigins.clear();
                } else
                  candidate.successResultDrop = 999;
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "payload return retains type, wrong-state check, and drop "
                   "identity");
            expect(
                !lang::CppMirBodyEmitter(malformed, rows)
                     .supportsFailureBodyText(
                         {lang::MirBodyKind::Function, function.id}),
                "payload return admission rejects forged checked publication");
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
