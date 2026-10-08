#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <algorithm>
#include <iostream>
#include <set>
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
  rejected("extern \"C\" { [[link_name(\"bad\\\"label\")]] int32_t call(); } "
           "int main() { return 0; }",
           "GTI-S2054");
  rejected("extern \"C\" { [[link_name(\"__gti_c_fn_1\")]] int32_t call(); } "
           "int main() { return 0; }",
           "GTI-S2054");
  rejected(R"gti(
using Native = (int32_t) -> int32_t;
extern "C" { int32_t native(int32_t); }
int main() { Native callback = native; return 0; }
)gti",
           "GTI-S2076");

  auto unnamed = analyze(R"gti(
interface I { int32_t inspect(int32_t, uint32_t); };
int32_t discard(int32_t, uint32_t) { return 0; }
int main() { return discard(1, uint32_t(2)); }
)gti");
  expect(unnamed.canGenerateCode(),
         "unnamed bodyless and executable formals need physical identities");
  for (const std::string shape :
       {"int32_t", "int32_t&", "mut int32_t&", "int32_t*"}) {
    for (const std::string &declaration :
         {"int32_t f(" + shape + ");",
          "class C { public: int32_t f(" + shape + "); };",
          "interface I { int32_t f(" + shape + "); };"}) {
      auto cell = analyze(declaration + " int main() { return 0; }");
      expect(cell.canGenerateCode(),
             "each bodyless unnamed formal shape must retain canonical MIR");
      if (cell.canGenerateCode())
        expect(!gti_test::emitCpp(cell).contents.empty(),
               "each bodyless unnamed shape must reach production emission");
    }
    if (shape == "int32_t" || shape == "int32_t*") {
      auto cell = analyze("extern \"C\" { int32_t f(" + shape +
                          "); } int main() { return 0; }");
      expect(cell.canGenerateCode(),
             "unnamed valid C ABI shape must retain canonical MIR");
      if (cell.canGenerateCode())
        expect(!gti_test::emitCpp(cell).contents.empty(),
               "unnamed C ABI shape must reach production emission");
    }
  }
  if (unnamed.canGenerateCode()) {
    std::set<lang::SymbolId> ids;
    std::set<std::pair<std::size_t, std::size_t>> spans;
    for (const auto &symbol : unnamed.semantics.database().symbols()) {
      if (symbol.kind != lang::SymbolKind::Parameter)
        continue;
      expect(symbol.generated, "unnamed formal must remain tooling-hidden");
      expect(symbol.id != 0 && !symbol.nameSpan.source.empty() &&
                 symbol.nameSpan.end > symbol.nameSpan.start,
             "unnamed formal needs a real type source anchor");
      expect(
          unnamed.semantics.database().occurrencesForSymbol(symbol.id).empty(),
          "synthetic formal must not become a source name occurrence");
      ids.insert(symbol.id);
      spans.emplace(symbol.nameSpan.start, symbol.nameSpan.end);
    }
    expect(ids.size() == 4 && spans.size() == 4,
           "each owner and formal index must retain distinct identities");
    const auto lowered =
        gti_test::lowerProgram(unnamed, unnamed.mir, unnamed.mir);
    int emptyNames = 0;
    for (const auto &declaration : lowered.declarations()) {
      const auto *function =
          std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
      if (function == nullptr)
        continue;
      for (const auto &parameter : function->parameters) {
        expect(parameter.name.empty(),
               "Lowered formal must not acquire a manufactured public name");
        expect(parameter.symbol != 0,
               "Lowered formal must retain exact physical symbol");
        ++emptyNames;
      }
    }
    expect(emptyNames == 4, "both unnamed signatures must reach publication");
    expect(!gti_test::emitCpp(unnamed).contents.empty(),
           "unnamed formals must reach verified executable emission");
  }
  auto constexprUnnamed =
      analyze("constexpr int32_t reads(int32_t*) { return 0; }\n"
              "int main() { return 0; }");
  expect(!constexprUnnamed.canGenerateCode(),
         "raw pointer constexpr restriction remains intact");
  const auto constDiagnostic = std::find_if(
      constexprUnnamed.diagnostics.begin(), constexprUnnamed.diagnostics.end(),
      [](const auto &d) { return d.code == "GTI-S2057"; });
  expect(constDiagnostic != constexprUnnamed.diagnostics.end() &&
             constDiagnostic->primary.start == 24 &&
             constDiagnostic->primary.end == 31,
         "unnamed constexpr diagnostic must anchor the actual type token");

  auto wrongOrigin = analyze(R"gti(
class Holder {
  int32_t& value;
public:
  Holder(int32_t& source) : value(source) {}
};
Holder build(int32_t&) {
  int32_t local = 2;
  return Holder(local);
}
int main() { return 0; }
)gti");
  const auto originDiagnostic = std::find_if(
      wrongOrigin.diagnostics.begin(), wrongOrigin.diagnostics.end(),
      [](const auto &d) { return d.code == "GTI-S2045"; });
  expect(!wrongOrigin.canGenerateCode() &&
             originDiagnostic != wrongOrigin.diagnostics.end() &&
             originDiagnostic->message.find("unnamed read-only parameter 1") !=
                 std::string::npos &&
             originDiagnostic->related.size() == 1 &&
             !originDiagnostic->related.front().span.source.empty() &&
             originDiagnostic->related.front().span.end >
                 originDiagnostic->related.front().span.start,
         "borrow origin diagnostic needs the unnamed formal's real type span");
  return failures ? 1 : 0;
}
