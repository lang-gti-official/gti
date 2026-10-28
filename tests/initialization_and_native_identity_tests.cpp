#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
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
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source,
                           bool withStandardLibrary = false) {
    if (withStandardLibrary)
      return lang::Frontend().analyze(
          root / "tests" / "initialization-native-identity.gti", source,
          {root / "stdlib" / "prelude.gti"}, {}, {root / "stdlib"});
    return lang::Frontend().analyze("initialization-native-identity.gti",
                                    source);
  };
  const auto rejected = [&](const std::string &source, const char *code,
                            bool withStandardLibrary = false) {
    auto result = analyze(source, withStandardLibrary);
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
public:
  Empty() = default;
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
class Empty { public: Empty(nullptr_t value) {}  public: Empty() = default; };
void take(Empty value) {}
int main() { take(nullptr); return 0; }
)gti",
           "GTI-S2003");
  rejected(R"gti(
class Empty { Empty(nullptr_t value) {}  public: Empty() = default; };
class Parent { Empty field; public: Parent() : field(nullptr) {} };
int main() { return 0; }
)gti",
           "GTI-S2007");

  // A declaration of an unrelated nominal class constructs it from an
  // initializer of another type through an implicit constructor, exactly as
  // its direct initializer would: a local, a namespace binding, a field
  // default and a field constructor initializer, by value or read-only
  // reference. `implicit` stays an ordinary name everywhere else.
  auto declared = analyze(R"gti(
class Celsius {
  int degrees;
public:
  implicit Celsius(int value) : degrees(value) {}
  int read() { return this.degrees; }
};
struct Pair {
  int first = 0;
  int second = 0;
  Pair(int left, int right) : first(left), second(right) {}
public:
  Pair() = default;
};
struct Sum {
  int total;
  implicit Sum(Pair& pair, int bias = 1)
      : total(pair.first + pair.second + bias) {}
};
class Reading {
  Celsius current = 20;
  Celsius minimum;
public:
  Reading(int low) : minimum(low) {}
  int span() { return this.current.read() - this.minimum.read(); }
};
Celsius freezing = 0;
int main() {
  Celsius warm = 21;
  Pair pair{3, 4};
  Sum sum = pair;
  Reading reading{5};
  int implicit = 44;
  return warm.read() + freezing.read() + reading.span() + sum.total - implicit;
}
)gti");
  expect(declared.canGenerateCode(),
         "a declaration must construct its class from one exact argument");
  if (!declared.canGenerateCode())
    for (const auto &d : declared.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  if (declared.canGenerateCode())
    expect(!gti_test::emitCpp(declared).contents.empty(),
           "declaration construction must retain executable authority");
  const std::string celsius = R"gti(
class Celsius {
  int degrees;
public:
  implicit Celsius(int value) : degrees(value) {}
  int read() { return this.degrees; }
};
)gti";
  // Calls and assignment keep exact types.
  rejected(celsius + "void take(Celsius value) {}\n"
                     "int main() { take(21); return 0; }\n",
           "GTI-S2003");
  expect(analyze(celsius + "Celsius make() { return 21; }\n"
                           "int main() { return make().read(); }\n")
             .canGenerateCode(),
         "a declared value return uses one implicit constructor");
  rejected(celsius + "int main() { mut Celsius value{1}; value = 5; "
                     "return value.read(); }\n",
           "GTI-S2003");
  // Return initialization shares exact selection, access and ownership with
  // declarations; the destination is fresh even when its source is a place.
  const auto returned = analyze(R"gti(
#include <std/meta>
struct Score {
  int value;
  implicit Score(int initial, int offset = 2) : value(initial + offset) {}
  Score(Score& other) = delete;
  Score(Score&& other) = delete;
};
Score score(int value) { return value; }
T construct<T>(int value) { return value; }
std::meta::callable_return_type<F> reflected<F>(F& shape, int value) {
  return value;
}
struct GenericBox<T> {
  T value;
  implicit GenericBox(T initial) : value(std::move(initial)) {}
};
GenericBox<T> boxed<T>(int value) { return value; }
struct Factory { Score make(int value) { return value; }  public: Factory() = default; };
int main() {
  auto produce = [](int value) -> Score { return value; };
  Score first = score(1);
  Score second = construct<Score>(2);
  Factory factory{};
  Score third = factory.make(3);
  Score fourth = produce(4);
  auto signature = score;
  Score fifth = reflected(signature, 5);
  GenericBox<int> sixth = boxed<int>(6);
  return first.value + second.value + third.value + fourth.value +
         fifth.value + sixth.value - 31;
}
)gti",
                                true);
  expect(
      returned.canGenerateCode(),
      "declared function, generic, method and lambda returns construct once");
  if (!returned.canGenerateCode())
    for (const auto &d : returned.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  if (returned.canGenerateCode())
    expect(!gti_test::emitCpp(returned).contents.empty(),
           "implicit return construction must use executable MIR");
  if (returned.canGenerateCode()) {
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto forged = returned.hir;
      bool changed = false;
      for (auto &function :
           const_cast<std::vector<lang::HirFunctionInstance> &>(
               forged.functionInstances())) {
        if (function.body.contextualConstructions.empty())
          continue;
        auto &entry = function.body.contextualConstructions.front();
        if (mutation == 0)
          entry.constructionProof.reset();
        else if (mutation == 1)
          entry.source = nullptr;
        else {
          auto &value = *const_cast<lang::HirValue *>(
              function.body.findValue(entry.value));
          value.parameterTypes.front() = lang::SemanticType::Bool;
        }
        changed = true;
        break;
      }
      expect(
          changed &&
              !lang::verifyHirProgramPlans(returned.semantics, forged).valid(),
          "HIR rejects lost, substituted or mismatched contextual selection");
    }
  }
  expect(analyze(R"gti(
struct Empty { implicit Empty(nullptr_t value) {}  public: Empty() = default; };
Empty empty() { return nullptr; }
int main() { Empty value = empty(); return 0; }
)gti")
             .canGenerateCode(),
         "an implicit nullptr constructor also initializes a return");
  expect(analyze(R"gti(
struct Constant {
  int value;
  implicit constexpr Constant(int input) : value(input) {}
};
constexpr Constant constant() { return 7; }
static_assert(constant().value == 7);
int main() { return 0; }
)gti")
             .canGenerateCode(),
         "eligible implicit returns use the staged MIR constant profile");
  rejected(R"gti(
struct Explicit { Explicit(int value) {}  public: Explicit() = default; };
T construct<T>(int value) { return value; }
int main() { Explicit value = construct<Explicit>(1); return 0; }
)gti",
           "GTI-S2003");
  rejected(R"gti(
#include <std/meta>
struct Explicit { Explicit(int value) {}  public: Explicit() = default; };
Explicit exact(int value) { return Explicit(value); }
std::meta::callable_return_type<F> reflected<F>(F& shape, int value) {
  return value;
}
int main() {
  auto shape = exact;
  Explicit result = reflected(shape, 1);
  return 0;
}
)gti",
           "GTI-S2003", true);
  rejected(R"gti(
#include <std/meta>
struct Middle { implicit Middle(int value) {}  public: Middle() = default; };
struct End { implicit End(Middle value) {}  public: End() = default; };
End exact(Middle value) { return End(value); }
std::meta::callable_return_type<F> reflected<F>(F& shape, int value) {
  return value;
}
int main() {
  auto shape = exact;
  End result = reflected(shape, 1);
  return 0;
}
)gti",
           "GTI-S2003", true);
  rejected(R"gti(
struct GenericBox<T> {
  T value;
  implicit GenericBox(T initial) : value(std::move(initial)) {}
};
GenericBox<T> boxed<T>(int value) { return value; }
int main() { auto result = boxed<bool>(1); return 0; }
)gti",
           "GTI-S2003", true);
  // Explicit constructors and the declaration-only nullptr exception do not
  // become implicit at returns.
  rejected("struct Plain { Plain(int value) {}  public: Plain() = default; };\n"
           "Plain make() { return 1; }\nint main() { return 0; }\n",
           "GTI-S2003");
  rejected("struct Plain { Plain(nullptr_t value) {}  public: Plain() = default; };\n"
           "Plain make() { return nullptr; }\nint main() { return 0; }\n",
           "GTI-S2003");
  rejected("class Hidden { implicit Hidden(int value) {}  public: Hidden() = default; };\n"
           "Hidden make() { return 1; }\nint main() { return 0; }\n",
           "GTI-S2007");
  rejected(R"gti(
struct Middle { implicit Middle(int value) {}  public: Middle() = default; };
struct End { implicit End(Middle value) {}  public: End() = default; };
End make() { return 1; }
int main() { return 0; }
)gti",
           "GTI-S2003");
  rejected(R"gti(
struct Twice {
  implicit Twice(int value) {}
  implicit Twice(int value, int extra = 2) {}
public:
  Twice() = default;
};
Twice make() { return 1; }
int main() { return 0; }
)gti",
           "GTI-S2013");
  rejected(R"gti(
struct Owner {
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
};
struct Holder {
  Owner value;
  implicit Holder(Owner owner) : value(std::move(owner)) {}
};
Holder wrap() { Owner owner{}; return owner; }
int main() { return 0; }
)gti",
           "GTI-S2003");
  rejected(R"gti(
struct Borrow {
  int& value;
  implicit Borrow(int& source) : value(source) {}
};
Borrow leak() { int local = 1; return local; }
int main() { return 0; }
)gti",
           "GTI-S2003");
  rejected(celsius + "Celsius& leak() { return 1; }\n"
                     "int main() { return 0; }\n",
           "GTI-S2017");
  // No constructor accepts the initializer: the exact-type error remains.
  rejected(celsius + "int main() { Celsius hot = \"hot\"; return 0; }\n",
           "GTI-S2003");
  // A constant binding keeps exact types.
  rejected(R"gti(
class Celsius {
  int degrees;
public:
  implicit constexpr Celsius(int value) : degrees(value) {}
};
constexpr Celsius boiling = 100;
int main() { return 0; }
)gti",
           "GTI-S2003");
  // Selection is the direct initializer's: ambiguity and a named move-only
  // argument are diagnosed by constructor selection itself.
  rejected(R"gti(
struct Twice {
  int value;
  implicit Twice(int first) : value(first) {}
  implicit Twice(int first, int second = 2) : value(first + second) {}
};
int main() { Twice twice = 1; return twice.value; }
)gti",
           "GTI-S2013");
  rejected(R"gti(
struct Owner {
  int value = 0;
  Owner(int initial) : value(initial) {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
public:
  Owner() = default;
};
struct Holder {
  Owner owner;
  implicit Holder(Owner value) : owner(std::move(value)) {}
};
int main() { Owner named{1}; Holder holder = named; return 0; }
)gti",
           "GTI-S2018");
  // A destination that carries a borrow keeps its explicit construction.
  rejected(R"gti(
class Borrow {
  int& value;
public:
  implicit Borrow(int& source) : value(source) {}
};
int main() { int seven = 7; Borrow borrow = seven; return 0; }
)gti",
           "GTI-S2003");
  // Without `implicit` a constructor is called only explicitly; the
  // exact-type error says how to opt in or construct explicitly.
  {
    auto plain = analyze(R"gti(
struct Buffer {
  int capacity;
  Buffer(int size) : capacity(size) {}
};
int main() { Buffer buffer = 64; return buffer.capacity; }
)gti");
    const auto found =
        std::find_if(plain.diagnostics.begin(), plain.diagnostics.end(),
                     [](const auto &d) { return d.code == "GTI-S2003"; });
    expect(!plain.canGenerateCode() && found != plain.diagnostics.end() &&
               std::any_of(found->hints.begin(), found->hints.end(),
                           [](const std::string &hint) {
                             return hint.find("'implicit'") !=
                                        std::string::npos &&
                                    hint.find("Buffer buffer{...}") !=
                                        std::string::npos;
                           }),
           "a constructor without 'implicit' must not construct a "
           "declaration, and the error must say how to construct");
  }
  // `implicit` marks only an ordinary constructor callable with zero or one
  // explicit argument, with a source body and no unsafe obligation.
  rejected("struct Pair { implicit Pair(int a, int b) {}  public: Pair() = default; };\n"
           "int main() { return 0; }\n",
           "GTI-S2121");
  expect(analyze("struct Empty { implicit Empty() {} };\n"
                 "Empty finish() { return; }\n"
                 "int main() { auto value = finish(); return 0; }\n")
             .canGenerateCode(),
         "implicit empty construction opts a source class into bare return");
  rejected("struct Same { int value = 0; Same() {} implicit Same(Same& other) "
           "{} };\n"
           "int main() { return 0; }\n",
           "GTI-S2121");
  for (const char *spelling :
       {"struct Bad { implicit unsafe Bad(int value) {} };",
        "struct Bad { unsafe implicit Bad(int value) {} };",
        "struct Bad { constexpr implicit Bad(int value) {} };",
        "struct Bad { Bad() {} implicit Bad(Bad&& other) = default; };"})
    rejected(std::string(spelling) + "\nint main() { return 0; }\n",
             "GTI-P0001");

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
