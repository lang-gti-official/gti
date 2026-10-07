#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <algorithm>
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
  const auto analyze = [](const std::string &source) {
    return lang::Frontend().analyze("initialization-native-identity.gti",
                                    source);
  };
  const auto rejected = [&](const std::string &source, const char *code) {
    auto result = analyze(source);
    const bool located =
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const auto &d) {
                      return d.code == code && !d.primary.source.empty() &&
                             d.primary.end > d.primary.start;
                    });
    if (!located)
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    expect(!result.canGenerateCode(), "invalid source must not reach codegen");
    expect(located, "invalid source needs its located owning diagnostic");
  };
  auto nullable = analyze(R"gti(
class Empty {
  int marker = 7;
public:
  Empty(nullptr_t value) {}
  int read() { return this.marker; }
};
class Parent {
  Empty first;
  Empty second = nullptr;
public:
  Parent() : first(nullptr) {}
  int read() { return this.first.read() + this.second.read(); }
};
int main() { Parent parent{}; return parent.read() - 14; }
)gti");
  expect(nullable.canGenerateCode(),
         "unrelated nominal nullptr constructor must initialize a field");
  if (nullable.canGenerateCode())
    expect(!gti_test::emitCpp(nullable).contents.empty(),
           "nullable field construction must retain executable authority");
  rejected(R"gti(
class Empty { public: Empty(nullptr_t value) {} };
void take(Empty value) {}
int main() { take(nullptr); return 0; }
)gti",
           "GTI-S2003");
  rejected(R"gti(
class Empty { Empty(nullptr_t value) {} };
class Parent { Empty field; public: Parent() : field(nullptr) {} };
int main() { return 0; }
)gti",
           "GTI-S2007");

  auto native = analyze(R"gti(
extern "C" {
  uint64_t strlen(const uint8_t* text);
  [[link_name("unrelated_native_length")]] uint64_t length(const uint8_t* text);
}
int probe(const uint8_t* text) {
  unsafe { return strlen(text) == length(text) ? 0 : 1; }
}
int main() { return 0; }
)gti");
  expect(native.canGenerateCode(), "native functions keep their semantic ABI");
  if (!native.canGenerateCode())
    for (const auto &d : native.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  if (native.canGenerateCode()) {
    const auto artifact = gti_test::emitCpp(native);
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
      expect(artifact.contents.find("__asm__(\"_" + function.externalSymbol +
                                    "\")") != std::string::npos,
             "Darwin link label must retain exact selected symbol");
      expect(artifact.contents.find("__asm__(\"" + function.externalSymbol +
                                    "\")") != std::string::npos,
             "non-Darwin link label must retain exact selected symbol");
    }
    expect(checked == 2, "both native declarations must retain identity");
  }
  rejected("extern \"C\" { [[link_name(\"bad\\\"label\")]] int32_t call(); } "
           "int main() { return 0; }",
           "GTI-S2054");
  rejected("extern \"C\" { [[link_name(\"__gti_c_fn_1\")]] int32_t call(); } "
           "int main() { return 0; }",
           "GTI-S2054");
  return failures ? 1 : 0;
}
