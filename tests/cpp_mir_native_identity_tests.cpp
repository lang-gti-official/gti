// C++ representation of native function identity: private `__gti_c_fn_*`
// declarations and the per-target C++ link-label spelling. The
// backend-neutral MIR facts live in
// initialization_and_native_identity_tests.cpp.

#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <iostream>
#include <string>

int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const char *message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const std::string nativeSource = R"gti(
extern "C" {
  uint64_t strlen(const uint8_t* text);
  [[link_name("unrelated_native_length")]] uint64_t length(const uint8_t* text);
}
int probe(const uint8_t* text) {
  unsafe { return strlen(text) == length(text) ? 0 : 1; }
}
int main() { return 0; }
)gti";
  // The selected target fixes the object-format link label; the lowered
  // program must not defer that choice to host preprocessor branches.
  for (const char *triple : {"aarch64-apple-darwin", "x86_64-unknown-linux-gnu",
                             "x86_64-pc-windows-msvc"}) {
    lang::FrontendOptions options;
    options.target = *lang::parseTargetTriple(triple);
    const auto native = lang::Frontend(options).analyze(
        "initialization-native-identity.gti", nativeSource);
    expect(native.canGenerateCode(),
           "native functions keep their semantic ABI");
    if (!native.canGenerateCode()) {
      for (const auto &d : native.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
      continue;
    }
    const auto artifact =
        gti_test::emitCpp(native, lang::CppStandard::Cpp23, options.target);
    const std::size_t programStart =
        artifact.contents.find("namespace __gti_program {");
    expect(programStart != std::string::npos &&
               artifact.contents.find("#if", programStart) == std::string::npos,
           "native link labels must follow the selected target, not host "
           "preprocessor branches");
    int checked = 0;
    for (const auto &function : native.mir.functionInstances()) {
      if (function.linkage != lang::LanguageLinkage::C)
        continue;
      ++checked;
      expect(function.returnType == lang::SemanticType::UInt64,
             "private declaration isolation must not remap uint64_t");
      const auto name = "__gti_c_fn_" + std::to_string(function.declaration);
      expect(artifact.contents.find(name + "(") != std::string::npos,
             "native declaration and call must share private identity");
      const std::string &os = options.target.os;
      const std::string label =
          os == "windows"
              ? "/alternatename:" + name + "=" + function.externalSymbol + "\""
              : "__asm__(\"" + std::string(os == "macos" ? "_" : "") +
                    function.externalSymbol + "\")";
      expect(artifact.contents.find(label) != std::string::npos,
             "the target's link label must retain the exact selected symbol");
    }
    expect(checked == 2, "both native declarations must retain identity");
  }
  return failures == 0 ? 0 : 1;
}
