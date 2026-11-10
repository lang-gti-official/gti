// Frozen C++-backend coverage moved from layout_query_tests.cpp: the C++
// emitter's construction contract, the deprecated backend's rejection of
// native layout constants, and the C++ text of layout-query bodies.
#include "gti/cpp_emitter.h"

#include "cpp_backend_test_support.h"
#include "mir_test_fixture.h"

#include <type_traits>

using namespace gti_test::mir_fixture;

namespace {

static_assert(!std::is_default_constructible_v<lang::CppEmitter>);
static_assert(
    !std::is_constructible_v<lang::CppEmitter, const lang::SemanticModel &,
                             const lang::HirProgram &>);
static_assert(!std::is_constructible_v<lang::CppEmitter, lang::SemanticModel &&,
                                       const lang::HirProgram &>);
static_assert(
    !std::is_constructible_v<lang::CppEmitter, const lang::SemanticModel &,
                             lang::HirProgram &&>);
static_assert(!std::is_constructible_v<lang::CppEmitter, lang::SemanticModel &&,
                                       lang::HirProgram &&>);

void printDiagnostics(const lang::FrontendResult &) {}

std::string validLayoutSource() {
  return R"(
using Word = uint16_t;
using Matrix = int16_t[3][4];
enum class State : uint8_t { Ready };
union WordBits { uint32_t integer; float real; };
constexpr uint64_t stride = sizeof(uint32_t);

int main() {
  uint64_t bool_size = sizeof(bool);
  uint64_t char_alignment = alignof(char);
  uint64_t default_int_size = sizeof(int);
  uint64_t default_uint_alignment = alignof(uint);
  uint64_t i8_size = sizeof(int8_t);
  uint64_t i16_alignment = alignof(int16_t);
  uint64_t i32_size = sizeof(int32_t);
  uint64_t i64_alignment = alignof(int64_t);
  uint64_t u8_size = sizeof(uint8_t);
  uint64_t u16_alignment = alignof(uint16_t);
  uint64_t u32_size = sizeof(uint32_t);
  uint64_t u64_alignment = alignof(uint64_t);
  uint64_t float_size = sizeof(float);
  uint64_t double_size = sizeof(double);
  uint64_t double_alignment = alignof(double);
  uint64_t pointer_alignment = alignof(const int32_t*);
  uint64_t void_pointer_size = sizeof(void*);
  uint64_t alias_size = sizeof(Word);
  uint64_t enum_size = sizeof(State);
  uint64_t union_size = sizeof(WordBits);
  uint64_t union_alignment = alignof(WordBits);
  uint64_t matrix_size = sizeof(Matrix);
  uint64_t matrix_alignment = alignof(Matrix);
  uint64_t indirect_array_size = sizeof(int16_t[stride]);
  int32_t values[stride] = {};
  return values[0];
}
)";
}

void testNativeLayoutAdmission() {
  // A use that disappears during constant evaluation still selects the native
  // representation; the deprecated backend must not silently emit its value.
  for (const auto source :
       {"struct Box { int value; }; static_assert(sizeof(Box) > uint64_t(0)); "
        "int main() { return 0; }",
        "struct Box { int value; }; constexpr uint64_t bytes<T>() { return "
        "sizeof(T); } static_assert(bytes<Box>() > uint64_t(0)); int main() { "
        "return 0; }",
        "struct Box { int value; }; constexpr uint64_t n = sizeof(Box); int "
        "main() { uint8_t buffer[n] = {}; return 0; }",
        "struct Box { int value; }; constexpr bool fits<T>() { return "
        "sizeof(T) <= uint64_t(16); } "
        "concept Small<T> = fits<T>(); int select<T>() requires Small<T> { "
        "return 0; } "
        "int main() { return select<Box>(); }"}) {
    const auto result = analyze("native-layout-admission.gti", source);
    if (!result.canGenerateCode())
      printDiagnostics(result);
    expect(result.canGenerateCode() &&
               result.semantics.requiresNativeObjectLayout() &&
               result.hir.requiresNativeObjectLayout() &&
               result.mir.requiresNativeObjectLayout(),
           "erased native layout uses retain the backend admission fact");
    if (!result.canGenerateCode())
      continue;
    bool rejected = false;
    try {
      (void)gti_test::emitCpp(result);
    } catch (const lang::BackendUnsupportedError &error) {
      rejected =
          std::string_view(error.what()).find("general sizeof/alignof") !=
          std::string_view::npos;
    }
    expect(rejected, "C++ rejects native layout constants after erasure");
  }
  const auto scalar = analyze("scalar-generic-layout.gti", R"(
constexpr uint64_t bytes<T>() { return sizeof(T); }
static_assert(bytes<uint32_t>() == uint64_t(4));
int main() { return 0; }
)");
  expect(scalar.canGenerateCode() && !scalar.mir.requiresNativeObjectLayout(),
         "a scalar generic query does not require native aggregate layout");
  if (scalar.canGenerateCode())
    (void)gti_test::emitCpp(scalar);
}

void testBackendText() {
  const lang::FrontendResult result =
      analyze("layout-query-lowering.gti", validLayoutSource());
  expect(result.canGenerateCode(), "valid layout queries lower");
  if (!result.canGenerateCode())
    return;
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(result.hir, lang::OptimizationLevel::O1);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(result, result.mir, result.mir, optimizations);
  // The body emits from verified MIR: layout results stage as the
  // frontend-computed literals, and the emitted body region carries no
  // native layout query at all (the sealed helpers above it may).
  const std::size_t layoutBodyStart =
      artifact.contents.find("function-instance");
  const std::string_view layoutBody =
      layoutBodyStart == std::string::npos
          ? std::string_view()
          : std::string_view(artifact.contents).substr(layoutBodyStart);
  const auto stagedLiteral = [&](std::uint64_t expected) {
    return layoutBody.find(">(" + std::to_string(expected) + ")") !=
           std::string_view::npos;
  };
  expect(!layoutBody.empty() && stagedLiteral(1) && stagedLiteral(24) &&
             stagedLiteral(8) &&
             layoutBody.find("sizeof") == std::string_view::npos &&
             layoutBody.find("alignof") == std::string_view::npos,
         "the backend should emit frontend-computed numeric constants, not "
         "native C++ layout queries for source expressions");

  expect(!std::is_constructible_v<lang::CppEmitter, const lang::SemanticModel &,
                                  const lang::HirProgram &>,
         "only CppBackend should construct the emitter, with verified MIR and "
         "its copied representation rows");
}
} // namespace

int main() {
  testNativeLayoutAdmission();
  testBackendText();
  return failures == 0 ? 0 : 1;
}
