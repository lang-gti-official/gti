#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/native_header.h"
#include "native_symbol_spelling.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

// Frozen C++-backend coverage moved from native_c_scalar_tests.cpp: the C++
// spelling of native C scalar declarations.

int main() {
  using C = lang::NativeCScalarKind;
  using T = lang::SemanticType;
  int failures = 0;
  const auto expect = [&](bool value, const char *message) {
    if (!value) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  for (const char *triple :
       {"arm64-apple-macos", "x86_64-apple-macos", "arm64-unknown-linux-gnu",
        "x86_64-unknown-linux-gnu", "arm64-pc-windows-msvc",
        "x86_64-pc-windows-msvc"}) {
    lang::FrontendOptions options;
    options.target = *lang::parseTargetTriple(triple);
    const auto &target = options.target;
    const auto longType = T::nativeCScalarType(C::Long, target);
    const auto longLongType = T::nativeCScalarType(C::LongLong, target);
    expect(longType && longLongType && *longType != *longLongType,
           "equal widths must not collapse long and long long");
    expect(T::nativeCScalarType(C::Int, target) == T::Int32,
           "c_int and int32_t must canonicalize when they are true native "
           "aliases");
    expect((T::nativeCScalarType(C::Size, target) ==
            T::nativeCScalarType(target.os == "windows" ? C::UnsignedLongLong
                                                        : C::UnsignedLong,
                                 target)),
           "pointer-sized C typedefs must canonicalize by native identity");
    const auto result = lang::Frontend(options).analyze(
        "native-scalars.gti", R"(
using Word = c_long;
[[c_abi]] struct Packet { mut c_long word; mut c_char code; mut c_bool ready; };
using NativeScalars = (c_long, c_char, c_bool) -> c_longlong;
extern "C" {
  NativeScalars scalar_callback();
  Word take_word(Word value);
  c_size_t take_size(c_size_t value);
  c_bool take_bool(c_bool value);
  c_char take_char(c_char value);
  c_longlong take_longlong(c_longlong value);
}
int family(c_long value) { return 1; }
int family(c_longlong value) { return 2; }
struct Box<T> { mut T value; };
int main() {
  Box<c_long> native{.value = c_long(3)};
  Box<int64_t> fixed{.value = int64_t(4)};
  Word copied = take_word(native.value);
  c_size_t length = take_size(c_size_t(7));
  c_bool flag = take_bool(true);
  c_char code = take_char(c_char(65));
  c_longlong left = take_longlong(c_long(2) + c_longlong(3));
  c_longlong right = take_longlong(c_longlong(3) + c_long(2));
  unsafe {
    NativeScalars callback = scalar_callback();
    c_longlong answer = callback(c_long(4), c_char(65), true);
    if (answer != c_longlong(70)) { return 3; }
  }
  if (family(c_long(1)) != 1 || family(c_longlong(1)) != 2 || left != right) { return 2; }
  if (copied != c_long(3) || length != c_size_t(7) || !flag || int32_t(code) != 65) { return 1; }
  return 0;
}
)",
        {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    expect(result.canGenerateCode(),
           "native C scalar aliases must reach verified MIR");
    if (!result.canGenerateCode()) {
      for (const auto &diagnostic : result.diagnostics) {
        std::cerr << triple << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
      }
      continue;
    }
    const std::string wordSpelling =
        longType->nativeCScalar == C::None ? "int64_t" : "long";
    const auto artifact =
        gti_test::emitCpp(result, lang::CppStandard::Cpp20, target);
    expect(artifact.contents.find(
               wordSpelling + " " +
               gti_test::nativeSpelling(artifact.contents, "take_word")) !=
               std::string::npos,
           "native C declarations must preserve long rather than substitute an "
           "equal-width type");
    expect(artifact.contents.find(
               "char " +
               gti_test::nativeSpelling(artifact.contents, "take_char")) !=
               std::string::npos,
           "native plain char must not become signed char or a GTI code unit");
  }
  return failures ? 1 : 0;
}
