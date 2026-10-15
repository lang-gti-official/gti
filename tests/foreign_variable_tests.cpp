#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(std::string source) {
  auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("foreign-storage.gti", std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
}
void diagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
void testStorage() {
  auto result = analyze(R"(
using Callback = (c_int) -> c_int;
[[c_abi]] struct Record { mut c_int value; mut c_int* pointer; };
namespace native {
extern "C" {
  mut c_int counter;
  c_int limit;
  mut Callback callback;
  mut c_int* pointer;
  mut Record record;
  [[link_name("actual_epoch")]] mut c_int epoch;
  void mutate();
}
}
int main() {
  auto address = &native::counter;
  auto readonly_address = &native::limit;
  auto field_address = &native::record.value;
  unsafe {
    native::counter = c_int(7);
    native::counter++;
    auto previous = native::counter;
    native::mutate();
    native::pointer = address;
    native::record.value = native::limit;
    native::epoch = native::callback(previous);
    return int(native::counter + native::record.value + native::epoch);
  }
}
)");
  if (!result.canGenerateCode())
    diagnostics(result);
  expect(result.canGenerateCode(),
         "foreign storage vertical fixture must lower");
  if (!result.canGenerateCode())
    return;
  expect(result.mir.foreignStorageDeclarations().size() == 6,
         "exact external declaration inventory");
  expect(result.mir.programInitializationPlan().steps.empty(),
         "external storage has no GTI initialization");
  expect(result.mir.module().dropObligations.empty(),
         "external storage has no GTI cleanup");
  expect(lang::verifyMirProgram(result.mir).valid(),
         "foreign storage MIR verifies");
  std::size_t accesses = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto foreign = [&](lang::MirPlaceId id) {
          const auto *place = function.body.findPlace(id);
          return place && place->foreignStorage;
        };
        const bool read = std::any_of(
            instruction.operands.begin(), instruction.operands.end(),
            [&](const auto &operand) {
              return operand.kind != lang::MirOperandKind::Address &&
                     foreign(operand.place);
            });
        const bool write =
            instruction.destination && foreign(*instruction.destination);
        if (!read && !write)
          continue;
        ++accesses;
        const auto effects = lang::effects(result.mir, instruction);
        expect(instruction.rawMemoryAccess &&
                   (write ? effects.writesUnknownMemory
                          : effects.readsUnknownMemory),
               "foreign access retains unknown-memory effects");
        auto forged = result.mir;
        auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
            forged.functionInstances());
        for (auto &changedFunction : functions)
          if (changedFunction.id == function.id)
            for (auto &changedBlock : changedFunction.body.blocks)
              for (auto &changed : changedBlock.instructions)
                if (changed.id == instruction.id)
                  changed.rawMemoryAccess = false;
        expect(!lang::verifyMirProgram(forged).valid(),
               "verifier rejects erased external-memory effects");
      }
    }
    for (const auto &place : function.body.places) {
      if (!place.foreignStorage)
        continue;
      auto forged = result.mir;
      auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
          forged.functionInstances());
      for (auto &changedFunction : functions)
        if (changedFunction.id == function.id)
          changedFunction.body.places[place.id - 1].foreignStorage = false;
      expect(!lang::verifyMirProgram(forged).valid(),
             "verifier rejects erased foreign identity");
    }
  }
  expect(accesses >= 6, "fixture exercises foreign loads and writes");
  for (auto level : {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
                     lang::OptimizationLevel::O3}) {
    try {
      auto cpp = gti_test::emitCppText(result, lang::CppStandard::Cpp23,
                                       lang::TargetInfo::host(), level);
      expect(cpp.find("actual_epoch") != std::string::npos,
             "backend preserves actual external symbol");
    } catch (const std::exception &error) {
      expect(false, error.what());
    }
  }
}
void testRejections() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"extern \"C\" { mut c_int counter; } int main() { return int(counter); "
       "}",
       "GTI-S2055"},
      {"extern \"C\" { c_int limit; } int main() { return int(limit); }",
       "GTI-S2055"},
      {"extern \"C\" { mut c_int counter; } int main() { counter = c_int(1); "
       "return 0; }",
       "GTI-S2055"},
      {"extern \"C\" { c_int limit; } int main() { unsafe { limit = c_int(1); "
       "} return 0; }",
       ""},
      {"extern \"C\" { mut c_int counter; } int main() { unsafe { c_int& ref = "
       "counter; return int(ref); } }",
       "GTI-S2054"},
      {"extern \"C\" { mut c_int counter; } c_int read(c_int& value) { return "
       "value; } int main() { unsafe { return int(read(counter)); } }",
       "GTI-S2054"},
      {"extern \"C\" { mut string text; } int main() { return 0; }",
       "GTI-S2054"},
      {"extern \"C\" { mut c_int values[4]; } int main() { return 0; }",
       "GTI-S2054"},
      {"extern \"C\" { [[link_name(\"same\")]] mut c_int first; "
       "[[link_name(\"same\")]] mut c_int second; } int main() { return 0; }",
       "GTI-S2054"},
      {"extern \"C\" { [[link_name(\"same\")]] mut c_int first; void same(); } "
       "int main() { return 0; }",
       "GTI-S2054"},
  };
  for (const auto &[source, code] : cases) {
    auto result = analyze(source);
    expect(!result.canGenerateCode(), "must reject: " + source);
    if (!code.empty())
      expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [&](const auto &diagnostic) {
                           return diagnostic.code == code;
                         }),
             "expected " + code + " for " + source);
  }
}
} // namespace
int main() {
  testStorage();
  testRejections();
  return failures == 0 ? 0 : 1;
}
