#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const std::string source = R"(
enum class Wire : uint64_t { high = 18446744073709551615 };
enum class Payload { value(int data) };
struct Encoding<E, U> {
  bool matches() requires std::integral_enum<E> && std::enum_underlying<E, U> { return true; }
public:
  Encoding() = default;
};
static_assert(requires(Encoding<Wire, uint64_t>& probe) { { probe.matches() } -> bool; });
static_assert(!requires(Encoding<Wire, int64_t>& probe) { { probe.matches() } -> bool; });
static_assert(!requires(Encoding<Payload, int32_t>& probe) { { probe.matches() } -> bool; });
static_assert(!requires(Encoding<int32_t, int32_t>& probe) { { probe.matches() } -> bool; });
uint64_t decode(Wire value) { return std::to_underlying<uint64_t, Wire>(value); }
int main() { return decode(Wire::high) == uint64_t(18446744073709551615) ? 0 : 1; }
)";
  const auto frontend = analyzeWithStandardLibrary("enum.gti", source);
  expect(frontend.canGenerateCode(), "exact enum extraction analyzes");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  unsigned extractions = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::EnumUnderlyingValue)
          continue;
        ++extractions;
        expect(supportsSelectedBodyText(frontend.mir, rows, function.id),
               "enum extraction has a production verified-MIR emitter");
        for (unsigned mutation = 0; mutation != 4; ++mutation) {
          auto invalid = frontend.mir;
          auto *owner = const_cast<lang::MirFunctionInstance *>(
              invalid.findFunctionInstance(function.id));
          auto *home = owner->body.findBlock(block.id);
          auto changed =
              std::find_if(home->instructions.begin(), home->instructions.end(),
                           [&](const auto &candidate) {
                             return candidate.id == instruction.id;
                           });
          if (mutation == 0)
            changed->info.type = lang::SemanticType::Int64;
          if (mutation == 1)
            changed->parameterTypes.clear();
          if (mutation == 2)
            changed->operands.clear();
          if (mutation == 3) {
            auto *enumeration = const_cast<lang::MirIntegralEnum *>(
                invalid.findIntegralEnum(changed->parameterTypes[0].enumId));
            enumeration->underlyingType = lang::SemanticType::Int64;
          }
          expect(!lang::verifyMirProgram(invalid).valid(),
                 "enum extraction rejects malformed exact-type facts");
          expect(!supportsSelectedBodyText(invalid, rows, function.id),
                 "enum extraction emission fails closed on malformed "
                 "exact-type facts");
        }
      }
    }
  }
  expect(extractions == 1, "source wrapper lowers one exact extraction");
  for (const std::string invalidSource :
       {"enum class E : uint64_t { x }; int main() { return "
        "std::to_underlying<int32_t, E>(E::x); }",
        "concept invalid<T> = std::enum_underlying<Missing, int32_t>; int "
        "main() { return 0; }",
        "enum class E { x(int value) }; int main() { return "
        "std::to_underlying<int32_t, E>(E::x(1)); }",
        "int main() { return std::to_underlying<int32_t, int32_t>(1); }",
        "enum class E { x }; constexpr int value = "
        "std::to_underlying<int32_t,E>(E::x); int main() { return value; }",
        "enum class E { x }; int main() { int value = E::x; return value; }",
        "enum class E { x }; int main() { return int32_t(E::x); }",
        "#include <std/format>\nenum class E { x(int value) }; int main() { "
        "[[discard]] std::format(\"{}\", E::x(1)); return 0; }",
        "#include <std/format>\nenum class E { x }; class std::formatter<E> "
        "{ public: formatter() = default; }; int main() { [[discard]] std::format(\"{}\", E::x); return 0; }",
        "#include <std/format>\nenum class E { x }; class std::formatter<E> { "
        "public: int format(E& value) { return 0; }  public: formatter() = default; }; int main() { "
        "[[discard]] std::format(\"{}\", E::x); return 0; }",
        "enum class E { x }; int main() { return "
        "gti_internal::enum_underlying_value<int32_t,E>(E::x); }"}) {
    const auto rejected =
        analyzeWithStandardLibrary("invalid-enum.gti", invalidSource);
    expect(!rejected.canGenerateCode(),
           "invalid enum facilities fail in semantics");
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code == "GTI-B0001";
                        }),
           "invalid enum source never becomes a backend integrity failure");
  }
  const auto mismatch = analyzeWithStandardLibrary(
      "enum-mismatch.gti", "enum class E : uint64_t { x }; int main() { return "
                           "std::to_underlying<int32_t,E>(E::x); }");
  expect(std::any_of(mismatch.diagnostics.begin(), mismatch.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2029" &&
                              diagnostic.message.find("enum_underlying") !=
                                  std::string::npos;
                     }),
         "wrong extraction type names the exact underlying-type contract");
  return failures == 0 ? 0 : 1;
}
