#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
  const bool mir = argc > 1 && std::string_view(argv[1]) == "--mir";
  int failures = 0;
  const auto check = [&](std::string_view name, std::string_view source,
                         bool accepted, std::string_view code = {},
                         bool withTextPrelude = false) {
    const auto frontend =
        lang::Frontend({.stopAfter = mir ? lang::FrontendPhase::Mir
                                         : lang::FrontendPhase::Semantics});
    const auto root =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
    const auto prelude =
        std::filesystem::weakly_canonical(root / "prelude.gti");
    const std::string textPrelude = R"gti(
namespace gti_internal {
  class text_view<lifetime storage> {};
  class c_string<lifetime storage> {};
}
namespace std {
  using string_view<lifetime storage> = gti_internal::text_view<storage>;
  using c_string<lifetime storage> = gti_internal::c_string<storage>;
}
)gti";
    const auto result =
        withTextPrelude
            ? frontend.analyze(std::string(name) + ".gti", std::string(source),
                               {prelude}, {{prelude.string(), textPrelude}},
                               {root})
            : frontend.analyze(std::string(name) + ".gti", std::string(source));
    const bool passed = mir ? result.canGenerateCode() : result.semanticValid;
    if (mir && accepted && passed &&
        (name.starts_with("conformance-") ||
         name.starts_with("patterned-primary-"))) {
      const auto lowered = lang::LoweredProgramBuilder{}.build(
          result.program, result.semantics, result.hir, result.mir, result.mir,
          {}, lang::TargetInfo::host());
      if (!lowered.valid()) {
        ++failures;
        std::cerr << name
                  << ": accepted generic declaration must retain valid lowered "
                     "metadata\n";
        for (const auto &issue : lowered.issues)
          std::cerr << "  " << issue.detail << '\n';
      }
    }
    const bool codePresent =
        code.empty() ||
        std::any_of(
            result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto &diagnostic) { return diagnostic.code == code; });
    const bool internal = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto &diagnostic) { return diagnostic.code == "GTI-B0001"; });
    if (passed != accepted || !codePresent || internal) {
      ++failures;
      std::cerr << name << ": expected "
                << (accepted ? "acceptance" : "rejection")
                << (code.empty() ? ""
                                 : std::string(" with ") + std::string(code))
                << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << "  " << diagnostic.code << " at line "
                  << diagnostic.primary.line << ": " << diagnostic.message
                  << '\n';
      if (mir && internal && argc > 2) {
        const auto directory = std::filesystem::path(argv[2]);
        std::filesystem::create_directories(directory);
        std::ofstream evidence(directory /
                               (std::string(name) + ".program.mir.txt"));
        lang::MirPrinter{}.print(result.mir, evidence);
      }
      if (mir && internal && argc > 2 &&
          (name == "stored-external-reference-keeps-its-declared-region" ||
           name == "borrowed-value-parameter-publishes-stored-suppliers" ||
           name == "stored-reference-reborrow-keeps-external-region")) {
        const std::size_t owner =
            name == "stored-external-reference-keeps-its-declared-region" ? 2
                                                                          : 1;
        const auto directory = std::filesystem::path(argv[2]);
        std::filesystem::create_directories(directory);
        const auto path = directory / (std::string(name) + ".mir.txt");
        std::ofstream evidence(path);
        if (const auto *function = result.mir.findFunctionInstance(owner))
          evidence << lang::MirPrinter{}.print(function->body);
        if (name == "stored-reference-reborrow-keeps-external-region")
          if (const auto *selected = result.mir.findFunctionInstance(2)) {
            evidence << "SELECT owner=2\n";
            evidence << lang::MirPrinter{}.print(selected->body);
          }
        if (name == "stored-external-reference-keeps-its-declared-region")
          if (const auto *getter = result.mir.findFunctionInstance(3)) {
            evidence << "GETTER owner=3\n";
            evidence << lang::MirPrinter{}.print(getter->body);
          }
        if (const auto *function = result.hir.findFunctionInstance(owner)) {
          evidence << "HIR owner=" << owner
                   << " declaration=" << function->declaration << '\n';
          for (const auto &loan : function->body.loans) {
            evidence << "HIR loan=" << loan.semanticLoan
                     << " kind=" << static_cast<int>(loan.kind)
                     << " parent=" << loan.parent
                     << " stored=" << loan.storedDependency
                     << " entry=" << loan.entry
                     << " suppliers=" << loan.additionalDependencies.size()
                     << " fields=" << loan.designatedFields.size() << '\n';
          }
          for (const auto &value : function->body.values)
            if (value.borrowOrigin != lang::BorrowOriginKind::None ||
                value.borrowDependencies)
              evidence << "HIR value=" << value.id
                       << " kind=" << static_cast<int>(value.kind)
                       << " origin=" << static_cast<int>(value.borrowOrigin)
                       << " parameter=" << value.borrowArgument
                       << " suppliers=" << value.borrowFields.size()
                       << " complete=" << value.borrowDependencies.has_value()
                       << '\n';
        }
        std::cerr << "  failing owner " << owner << " evidence: " << path
                  << '\n';
      }
      if (mir && internal) {
        const auto suppliers = [](std::string_view label, const auto &fields) {
          std::cerr << "  " << label << " suppliers=" << fields.size();
          for (const auto &field : fields) {
            std::cerr << " [origin=" << static_cast<int>(field.origin)
                      << " argument=" << field.parameter << " source=";
            for (const auto &part : field.sourceFields)
              std::cerr << part << '/';
            std::cerr << " conditions=" << field.sourceConditions.size() << ']';
          }
          std::cerr << '\n';
        };
        for (const auto &function : result.mir.functionInstances())
          for (const auto &block : function.body.blocks)
            for (const auto &instruction : block.instructions)
              if (instruction.functionTarget) {
                const auto *target = result.mir.findFunctionInstance(
                    *instruction.functionTarget);
                if (target &&
                    instruction.borrowFields != target->returnBorrowFields) {
                  std::cerr << "  supplier mismatch target=" << target->id
                            << '\n';
                  suppliers("call", instruction.borrowFields);
                  suppliers("target", target->returnBorrowFields);
                  if (const auto *declaration =
                          result.semantics.findFunction(target->declaration)) {
                    std::cerr
                        << "  declaration origin="
                        << static_cast<int>(declaration->returnBorrowOrigin)
                        << " explicit-origin="
                        << declaration->explicitReturnBorrowOrigin
                        << " complete="
                        << declaration->completeReturnBorrow.has_value()
                        << '\n';
                    suppliers("declaration", declaration->returnBorrowFields);
                  }
                }
              }
      }
    }
  };
  check("text-equality-with-independent-storage-regions", R"gti(
bool same<lifetime a, lifetime b>(std::string_view<a> left, std::string_view<b> right) {
  return left == right;
}
bool different(std::string_view left, std::string_view right) { return left != right; }
bool same_c<lifetime a, lifetime b>(std::c_string<a> left, std::c_string<b> right) {
  return left == right;
}
int main() { std::string_view text = "hello"; return same(text, "hello") ? 0 : 1; }
)gti",
        true, {}, true);
  check("text-equality-does-not-admit-other-scalar-shapes", R"gti(
bool wrong(std::string_view text, int number) { return text == number; }
)gti",
        false, "GTI-S2000", true);
  check("operator-calls-use-the-declared-lifetime-application", R"gti(
struct Reader<lifetime a> {
  int&<a> value;
  Reader(int&<a> input) : value(input) {}
  int operator()(int&<a> other) { return value + other; }
  int&<a> operator*() { return value; }
  operator bool() { return value != 0; }
};
int read<lifetime a>(int&<a> first, int&<a> second) {
  Reader<a> reader{first};
  if (reader) { return reader(second) + *reader; }
  return 0;
}
int main() { int first = 3; int second = 4; return read(first, second) - 10; }
)gti",
        true);
  check("operator-calls-prove-independent-input-bounds", R"gti(
struct Reader<lifetime a> {
  int&<a> value;
  Reader(int&<a> input) : value(input) {}
  int operator()(int&<a> other) { return value + other; }
};
int read<lifetime a, lifetime b>(int&<a> first, int&<b> second)
    requires outlives<b, a> {
  Reader<a> reader{first}; return reader(second);
}
int main() { int first = 3; int second = 4; return read(first, second) - 7; }
)gti",
        true);
  check("operator-calls-cannot-invent-independent-input-bounds", R"gti(
struct Reader<lifetime a> {
  int&<a> value;
  Reader(int&<a> input) : value(input) {}
  int operator()(int&<a> other) { return value + other; }
};
int invalid<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  Reader<a> reader{first}; return reader(second);
}
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("operator-lifetime-application-preserves-literal-adoption", R"gti(
struct Number {
  int value;
  Number(int input) : value(input) {}
  int operator+(uint8_t other) { return value + int(other); }
};
int main() { Number number{5}; return number + 2 - 7; }
)gti",
        true);
  check("explicit-identity", R"gti(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int main() { int value = 7; int& result = identity(value); return result - 7; }
)gti",
        true);
  check("elided-identity", R"gti(
int& identity(int& value) { return value; }
int main() { int value = 7; int& result = identity(value); return result - 7; }
)gti",
        true);
  check("choose-two-origins", R"gti(
int&<a> choose<lifetime a>(int&<a> first, int&<a> second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
int main() { int first = 3; int second = 8; return choose(first, second, false) - 8; }
)gti",
        true);
  check("independent-regions", R"gti(
int&<a> first<lifetime a, lifetime b>(int&<a> left, int&<b> right) { return left; }
int main() { int left = 3; int right = 8; return first(left, right) - 3; }
)gti",
        true);
  check("wrong-owner-region", R"gti(
int&<a> wrong<lifetime a, lifetime b>(int&<a> left, int&<b> right) { return right; }
int main() { int left = 3; int right = 8; return wrong(left, right); }
)gti",
        false);
  check("subobject-preserves-owner-region", R"gti(
struct Number { int value = 7; Number() = default; };
int&<a> field<lifetime a>(Number&<a> number) { return number.value; }
int main() { Number number{}; return field(number) - 7; }
)gti",
        true);
  check("common-region-cannot-escape-as-first-input", R"gti(
int&<a> choose<lifetime a>(int&<a> first, int&<a> second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
int&<a> wrong<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  return choose(first, second, true);
}
)gti",
        false, "GTI-S2127");
  check("declaration-retains-unused-supplier", R"gti(
int&<a> first<lifetime a>(int&<a> left, int&<a> right) { return left; }
int main() {
  int left = 3;
  mut int right = 8;
  int& result = first(left, right);
  right = 9;
  return result;
}
)gti",
        false);
  check("declared-suppliers-survive-alias", R"gti(
int&<a> first<lifetime a>(int&<a> left, int&<a> right) { return left; }
int main() {
  int left = 3;
  mut int right = 8;
  int& result = first(left, right);
  int& alias = result;
  right = 9;
  return alias;
}
)gti",
        false);
  check("declared-suppliers-end-at-last-use", R"gti(
int&<a> first<lifetime a>(int&<a> left, int&<a> right) { return left; }
int main() {
  int left = 3;
  mut int right = 8;
  int& result = first(left, right);
  int saved = result;
  right = 9;
  return saved - 3;
}
)gti",
        true);
  // Elision with two eligible inputs is the ordinary ambiguous-dependency
  // diagnostic (Rust: missing lifetime specifier), not a relationship error.
  check("ambiguous-elision", R"gti(
int& choose(int& first, int& second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
)gti",
        false, "GTI-S2017");
  check("declared-outlives", R"gti(
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input)
  requires outlives<longer, shorter> { return input; }
)gti",
        true);
  check("undeclared-outlives", R"gti(
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input) {
  return input;
}
)gti",
        false);
  check("class-alias-keeps-target-lifetime-inference-at-use", R"gti(
struct View<T, lifetime a> { T&<a> value; View(T&<a> input) : value(input) {} };
class Collection<T> { public: using view = View<T>; };
int read<lifetime a>(int&<a> input) {
  Collection<int>::view value{input};
  return value.value;
}
int main() { int value = 7; return read(value) - 7; }
)gti",
        true);
  check("alias-target-still-proves-stronger-static-bound-at-use", R"gti(
struct Item<lifetime a> { int&<a> value; Item(int&<a> input) : value(input) {} };
struct View<T, lifetime a> { T&<a> value; View(T&<a> input) : value(input) {} };
class Collection<T> { public: using view = View<T, static>; };
void invalid<lifetime a>(Item<a>& input) { Collection<Item<a>>::view value{input}; }
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("patterned-primary-separates-nominal-environment", R"gti(
struct Callback<R(Args...), lifetime environment> {
  int&<environment> value;
  Callback(int&<environment> input) : value(input) {}
  int read() { return this.value; }
};
int read<lifetime a>(int&<a> input) {
  Callback<int(int), a> explicitView{input};
  Callback<int(int)> inferredView{input};
  return explicitView.read() + inferredView.read();
}
int main() { int value = 7; return read(value) - 14; }
)gti",
        true);
  check("patterned-primary-fixed-region-is-not-projected", R"gti(
struct Callback<R&<environment>(Args...), lifetime environment> {
  R&<environment> value;
  Callback(R&<environment> input) : value(input) {}
};
int read<lifetime a>(int&<a> input) {
  Callback<int&<a>(), a> value{input};
  return value.value;
}
int main() { int value = 7; return read(value) - 7; }
)gti",
        true);
  check("patterned-primary-fixed-region-repeated-in-signature", R"gti(
struct Callback<R&<environment>(R&<environment>), lifetime environment> {
  R&<environment> value;
  Callback(R&<environment> input) : value(input) {}
};
int read<lifetime a>(int&<a> input) {
  Callback<int&<a>(int&<a>), a> value{input};
  return value.value;
}
int main() { int value = 7; return read(value) - 7; }
)gti",
        true);
  check("patterned-primary-fixed-region-rejects-unrelated-signature-input",
        R"gti(
struct Callback<R&<environment>(R&<environment>), lifetime environment> {
  R&<environment> value;
  Callback(R&<environment> input) : value(input) {}
};
void invalid<lifetime a, lifetime b>(Callback<int&<a>(int&<b>), a> input) {}
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("patterned-primary-fixed-environment-keeps-invocation-binder", R"gti(
struct Callback<R&<environment>(Args...), lifetime environment> {
  R&<environment> value;
  Callback(R&<environment> input) : value(input) {}
};
int read<lifetime a>(int&<a> input) {
  Callback<int&<a>(int&), a> value{input};
  return value.value;
}
int main() { int value = 7; return read(value) - 7; }
)gti",
        true);
  check("patterned-primary-invocation-binder-cannot-supply-environment", R"gti(
struct Callback<R&<environment>(Args...), lifetime environment> {
  R&<environment> value;
  Callback(R&<environment> input) : value(input) {}
};
void invalid<lifetime a>(Callback<int&(int&), a> input) {}
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("patterned-primary-fixed-static-region-is-not-projected", R"gti(
struct StaticView<R&<static>()> {
  R&<static> value;
  StaticView(R&<static> input) : value(input) {}
};
int stored = 7;
int main() { StaticView<int&<static>()> value{stored}; return value.value - 7; }
)gti",
        true);
  check("raw-pointer-storage-outlives-without-pointee-obligation", R"gti(
void needs_static<T>(T* pointer) requires outlives<T*, static> {}
void relay<T>(T* pointer) { needs_static(pointer); }
struct Borrowed<lifetime a> { int&<a> value; Borrowed(int&<a> source) : value(source) {} };
void exercise<lifetime a>(int&<a> value) {
  Borrowed<a>* pointer = nullptr;
  relay(pointer);
}
int main() { int value = 7; exercise(value); return 0; }
)gti",
        true);
  check("owned-raw-pointer-field-allows-generic-receiver-borrow", R"gti(
struct Inner<T> { T* pointer = nullptr; int size() { return 0; } };
struct Outer<T> { Inner<T> data; int size() { return this.data.size(); } };
int main() { return 0; }
)gti",
        true);
  check("raw-pointer-premise-does-not-promote-pointee-to-static", R"gti(
void needs_static<T>(T* pointer) requires outlives<T, static> {}
void relay<T>(T* pointer) requires outlives<T*, static> { needs_static(pointer); }
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("checked-reference-storage-still-requires-its-region", R"gti(
void needs_static<lifetime a>(int&<a> value) requires outlives<int&<a>, static> {}
void relay<lifetime a>(int&<a> value) { needs_static(value); }
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("stored-opaque-field-allows-generic-receiver-borrow", R"gti(
struct Cell<T> { T value; int size() { return 1; } };
struct Outer<T> { Cell<T> cell; int size() { return this.cell.size(); } };
int main() { return 0; }
)gti",
        true);
  check("stored-opaque-field-bound-follows-checked-container", R"gti(
struct Cell<T> { T value; };
void need<T>() requires outlives<T, static> {}
void relay<T>(Cell<T>&<static> input) { need<T>(); }
int main() { return 0; }
)gti",
        true);
  check("stored-base-field-bound-follows-checked-container", R"gti(
struct Cell<T> { T value; };
struct Derived<T> : public Cell<T> {};
void need<T>() requires outlives<T, static> {}
void relay<T>(Derived<T>&<static> input) { need<T>(); }
int main() { return 0; }
)gti",
        true);
  check("stored-enum-payload-bound-follows-checked-container", R"gti(
enum Cell<T> { empty, value(T item) };
void need<T>() requires outlives<T, static> {}
void relay<T>(Cell<T>&<static> input) { need<T>(); }
int main() { return 0; }
)gti",
        true);
  check("stored-field-bound-cannot-outlive-container", R"gti(
struct Cell<T> { T value; };
void need<T>() requires outlives<T, static> {}
void relay<T, lifetime a>(Cell<T>&<a> input) { need<T>(); }
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("stored-field-bound-does-not-reach-phantom-argument", R"gti(
struct Cell<T> { int value; };
void need<T>() requires outlives<T, static> {}
void relay<T>(Cell<T>&<static> input) { need<T>(); }
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("stored-field-bound-does-not-reach-raw-pointee", R"gti(
struct Cell<T> { T* value = nullptr; };
void need<T>() requires outlives<T, static> {}
void relay<T>(Cell<T>&<static> input) { need<T>(); }
int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("owned-field-owned-field-reference", R"gti(
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> input) : external(input) {} int& read() { return this.local; } }; int main() { int value = 3; View view{value}; return view.read() - 7; }
)gti",
        true);
  check("owned-field-mutable-owned-field-reference", R"gti(
struct View<lifetime a> {
 int&<a> external; mut int local = 7;
 View(int&<a> input) : external(input) {}
 int& read() { return this.local; }
 mut int& edit() mut { return this.local; }
};
int main() { int value = 3; mut View view{value}; view.edit() = 11; return view.read() - 11; }
)gti",
        true);
  check("owned-field-formal-owned-field-reference", R"gti(
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> input) : external(input) {} };
int&<owner> read<lifetime a, lifetime owner>(View<a>&<owner> view) { return view.local; }
int main() { int value = 3; View view{value}; return read(view) - 7; }
)gti",
        true);
  check("owned-field-reject-static-owner", R"gti(
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> input) : external(input) {} int&<static> read() { return this.local; } }; int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("owned-field-reject-local-owner", R"gti(
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> input) : external(input) {} }; int stored = 3; int&<static> invalid() { View view{stored}; return view.local; } int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("owned-field-reject-value-owner", R"gti(
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> input) : external(input) {} }; int&<static> invalid(View<static> view) { return view.local; } int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("owner-witness-generic-owner-read", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct Box<T> { T extra; int local = 7; int& read() { unsafe { return borrow(&this.local, this); } } }; int main() { return 0; }
)gti",
        true);
  check("owner-witness-borrowed-owner-read", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> source) : external(source) {} int& read() { unsafe { return borrow(&this.local, this); } } }; int main() { int value = 3; View view{value}; return view.read() - 7; }
)gti",
        true);
  check("owner-witness-nested-generic-owner-read", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct Box<T> { T extra; int local = 7; int& read() { unsafe { return borrow(&this.local, this); } } }; struct Outer<T> { Box<T> inner; int& read() { return this.inner.read(); } }; int main() { return 0; }
)gti",
        true);
  check("owner-witness-local-witness-cannot-escape", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct Box { int value = 7; Box() = default; }; int&<static> invalid() { Box local{}; unsafe { return borrow(&local.value, local); } } int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("owner-witness-by-value-witness-cannot-escape", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct Box { int value = 7; Box() = default; }; int&<static> invalid(Box local) { unsafe { return borrow(&local.value, local); } } int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("owner-witness-caller-witness-cannot-invent-static", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct Box { int value = 7; Box() = default; }; int&<static> invalid(Box& source) { unsafe { return borrow(&source.value, source); } } int main() { return 0; }
)gti",
        false, "GTI-S2127");
  check("owner-witness-stored-owner-reference-read", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct View<lifetime a> { int&<a> external; int local = 7; View(int&<a> source) : external(source) {} int& read() { unsafe { return borrow(&this.local, this); } } }; struct Wrapper<lifetime a, lifetime owner> { View<a>&<owner> value; Wrapper(View<a>&<owner> input) : value(input) {} int& read() { return this.value.read(); } }; int main() { int value = 3; View view{value}; Wrapper wrapper{view}; return wrapper.read() - 7; }
)gti",
        true);
  check("owner-witness-mutable-physical-input", R"gti(
[[borrowed_from(owner)]] unsafe mut T& borrow_mut<T, Owner>(T* address, mut Owner& owner) { unsafe { return *address; } }
struct View<lifetime a> {
  int&<a> external; mut int local = 7;
  View(int&<a> source) : external(source) {}
  mut int& edit() mut { unsafe { return borrow_mut(&this.local, this); } }
  [[borrowed_from(this)]] int& read() { unsafe { return *(&this.local); } }
};
int main() {
  int external = 3; mut View view{external};
  view.edit() = 11;
  return view.read() - 11;
}
)gti",
        true);
  check("owner-witness-consuming-shell-cannot-escape", R"gti(
struct View {
  int local = 7;
  [[borrowed_from(this)]] int& take() && { unsafe { return *(&this.local); } }
};
int main() { return 0; }
)gti",
        false);
  check("constructor-reports-unproved-lifetime-for-matching-type", R"gti(
struct StaticInput { StaticInput(int&<static> input) {} };
int main() { int value = 7; StaticInput invalid{value}; return 0; }
)gti",
        false, "GTI-S2127");
  check("constructor-runtime-type-mismatch-is-not-a-lifetime-error", R"gti(
struct StaticInput { StaticInput(int&<static> input) {} };
int main() { bool value = true; StaticInput invalid{value}; return 0; }
)gti",
        false, "GTI-S2012");
  check("constructor-access-mismatch-is-not-a-lifetime-error", R"gti(
struct StaticInput { StaticInput(mut int&<static> input) {} };
int main() { int value = 7; StaticInput invalid{value}; return 0; }
)gti",
        false, "GTI-S2012");
  check("constructor-discarded-lifetime-failure-keeps-valid-overload", R"gti(
struct Input {
  Input(int&<static> input) {}
  Input(int input, bool ignored = true) {}
};
int main() { int value = 7; Input valid{value}; return 0; }
)gti",
        true);
  check("patterned-primary-nominal-environment-cannot-invent-static", R"gti(
struct Callback<R(Args...), lifetime environment> {
  int&<environment> value;
  Callback(int&<environment> input) : value(input) {}
};
int main() { int value = 7; Callback<int(int), static> invalid{value}; return 0; }
)gti",
        false, "GTI-S2127");
  check("patterned-primary-type-lifetime-name-collision", R"gti(
struct Callback<R(Args...), lifetime R> {};
)gti",
        false, "GTI-S2128");
  check("conformance-head-elides-borrowed-nominal-region", R"gti(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
concept Inspect<T> { int inspect(T& input); };
for concept Inspect<View> { int inspect(View& input) { return input.value; } };
int main() { int value = 7; View view{value}; return Inspect<View>::inspect(view) - 7; }
)gti",
        true);
  check("conformance-head-keeps-repeated-storage-and-independent-shells", R"gti(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
concept ReadPair<T> { int read(T& first, T& second); };
for concept ReadPair<View> {
  int read(View& first, View& second) { return first.value + second.value; }
};
int main() { int value = 7; View view{value}; return ReadPair<View>::read(view, view) - 14; }
)gti",
        true);
  check("conformance-associated-reference-uses-operation-elision", R"gti(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
concept Take<T> { typename Item; [[borrowed_from(value)]] Item take(T value); };
for concept Take<View> {
  using Item = int&;
  [[borrowed_from(value)]] int& take(View value) { return value.value; }
};
int main() { int value = 7; View view{value}; return Take<View>::take(view) - 7; }
)gti",
        true);
  check("conformance-head-keeps-closed-type-arguments-distinct", R"gti(
struct View<T, lifetime a> { T&<a> value; View(T&<a> input) : value(input) {} };
concept Inspect<T> { int inspect(T& value); };
for concept Inspect<View<int>> { int inspect(View<int>& value) { return value.value; } };
for concept Inspect<View<bool>> { int inspect(View<bool>& value) { if (value.value) { return 1; } return 0; } };
int main() { int value = 7; View<int> view{value}; return Inspect<View<int>>::inspect(view) - 7; }
)gti",
        true);
  check("conformance-head-keeps-generic-types-and-hidden-regions", R"gti(
struct View<T, lifetime a> { T&<a> value; View(T&<a> input) : value(input) {} };
concept Inspect<T> { bool inspect(T& value); };
for<T> concept Inspect<View<T>> { bool inspect(View<T>& value) { return true; } };
int main() { int value = 7; View<int> view{value}; if (Inspect<View<int>>::inspect(view)) { return 0; } return 1; }
)gti",
        true);
  check("conformance-head-well-formedness-bounds-associated-type", R"gti(
struct View<T, lifetime a> { T&<a> value; View(T&<a> input) : value(input) {} };
concept Wrapped<T> { typename Item; };
for<T, lifetime a> concept Wrapped<View<T, a>> { using Item = View<T, a>; };
int main() { return 0; }
)gti",
        true);
  check("conformance-head-does-not-invent-stronger-associated-bound", R"gti(
struct View<T, lifetime a> { T&<a> value; View(T&<a> input) : value(input) {} };
concept Wrapped<T> { typename Item; };
for<T, lifetime a> concept Wrapped<View<T, a>> { using Item = View<T, static>; };
)gti",
        false, "GTI-S2127");
  check("conformance-explicit-static-cannot-replace-head-region", R"gti(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
concept Inspect<T> { int inspect(T& value); };
for concept Inspect<View> { int inspect(View<static>& value) { return value.value; } };
)gti",
        false, "GTI-S2123");
  check("conformance-preserves-declared-operation-lifetime-requirements", R"gti(
concept Pair<T, lifetime a, lifetime b> {
  void use(int&<a> first, int&<b> second) requires outlives<a, b>;
};
for<lifetime a, lifetime b> concept Pair<int, a, b> {
  void use(int&<a> first, int&<b> second) requires outlives<a, b> {}
};
int main() { return 0; }
)gti",
        true);
  check("conformance-head-keeps-independent-stored-regions", R"gti(
struct PairView {
  int& first; int& second;
  PairView(int& left, int& right) : first(left), second(right) {}
};
concept Inspect<T> { int inspect(T& value); };
for concept Inspect<PairView> {
  int inspect(PairView& value) { return value.first + value.second; }
};
int main() { int left = 3; int right = 8; PairView value{left, right}; return Inspect<PairView>::inspect(value) - 11; }
)gti",
        true);
  check("conformance-cannot-strengthen-operation-lifetime-requirements", R"gti(
concept Pair<T, lifetime a, lifetime b> { void use(int&<a> first, int&<b> second); };
for<lifetime a, lifetime b> concept Pair<int, a, b> {
  void use(int&<a> first, int&<b> second) requires outlives<a, b> {}
};
)gti",
        false, "GTI-S2123");
  check("conformance-unused-head-region-does-not-become-static", R"gti(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
concept Marker<T> { int value(); };
for<lifetime a> concept Marker<View<a>> {
  int value() requires outlives<a, static> { return 1; }
};
)gti",
        false, "GTI-S2123");
  check("conformance-implicit-head-overlaps-explicit-lifetime-head", R"gti(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
concept Marker<T> { int value(); };
for concept Marker<View> { int value() { return 1; } };
for<lifetime a> concept Marker<View<a>> { int value() { return 2; } };
)gti",
        false, "GTI-S2123");
  check("named-outlives-contract", R"gti(
concept Survives<lifetime longer, lifetime shorter> = outlives<longer, shorter>;
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input)
  requires Survives<longer, shorter> { return input; }
)gti",
        true);
  check("disjunctive-requirement-inference", R"gti(
int&<a> identity<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
int main() { int value = 7; return identity(value) - 7; }
)gti",
        true);
  check("disjunction-is-not-two-assumptions", R"gti(
int&<b> bad<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
)gti",
        false);
  check("disjunctive-contract-forwarding", R"gti(
int&<a> inner<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
int&<a> outer<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> {
  return inner<a, b>(input);
}
int main() { int value = 7; return outer(value) - 7; }
)gti",
        true);
  check("distributed-outlives-return", R"gti(
int&<c> shorten<lifetime a, lifetime b, lifetime c, lifetime d>(int&<a> input)
  requires (outlives<a, b> && outlives<b, c>) ||
           (outlives<a, d> && outlives<d, c>) { return input; }
int main() { int value = 7; return shorten(value) - 7; }
)gti",
        true);
  check("disjunctive-type-bound-forwarding", R"gti(
void inner<T, lifetime a, lifetime b>(T input)
  requires outlives<T, a> || outlives<T, b> {}
void outer<T, lifetime a, lifetime b>(T input)
  requires outlives<T, a> || outlives<T, b> { inner<T, a, b>(input); }
int main() { outer<int>(7); return 0; }
)gti",
        true);
  check("phantom-region-is-not-borrowed-storage", R"gti(
struct Tag<lifetime a> { int value = 7; Tag() = default; };
int read<lifetime a>(Tag<a> tag) requires outlives<Tag<a>, static> { return tag.value; }
int main() { Tag tag{}; return read(tag) - 7; }
)gti",
        true);
  check("borrowed-record-construction", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
int main() { int number = 7; View view{number}; return view.value - 7; }
)gti",
        true);
  check("borrowed-record-escape", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
View<a> bad<lifetime a>(int&<a> outside) {
  int local = 7;
  return View<a>(local);
}
)gti",
        false);
  check("anonymous-constructor-declaration-contract", R"gti(
struct Pair {
  int& first;
  int& second;
  Pair(int& left, int& right) : first(left), second(right) {}
};
int main() { int a = 3; int b = 8; Pair pair{a, b}; return pair.second - 8; }
)gti",
        true);
  check("explicit-owner-region-is-not-body-inferred", R"gti(
struct View<lifetime owner> {
  int&<owner> value;
  View(int& input) : value(input) {}
};
)gti",
        false, "GTI-S2127");
  check("anonymous-constructor-local-escape", R"gti(
struct View {
  int& value;
  View(int& input) : value(input) {}
};
View bad(int& outside) { int local = 3; return View(local); }
)gti",
        false);
  check("ordinary-input-is-not-static", R"gti(
int&<static> forever(int& input) { return input; }
)gti",
        false);
  check("thread-storage-is-not-static", R"gti(
thread_local int value = 7;
int&<static> forever() { return value; }
)gti",
        false, "GTI-S2127");
  check("generic-owned-payload-receiver-borrow", R"gti(
enum State<T> { value(T payload) };
class Holder<T> {
  State<T> state;
public:
  Holder(T input) : state(State<T>::value(input)) {}
  T& get() {
    switch (this.state) {
    case State<T>::value(auto& payload): return payload;
    }
  }
};
int main() { Holder<int> holder{7}; return holder.get() - 7; }
)gti",
        true);
  check("whole-receiver-contract-keeps-compact-call-summary", R"gti(
struct Number {
  int value = 7;
  Number() = default;
  int& get() { return this.value; }
};
int main() { Number number{}; int& alias = number.get(); return alias - 7; }
)gti",
        true);
  check("owned-generic-value-has-local-validity", R"gti(
int observe<T, lifetime a>(T&<a> value) requires outlives<T, a> { return 7; }
int inspect<T>(T value) { return observe(value); }
int main() { return inspect(42) - 7; }
)gti",
        true);
  check("owned-generic-receiver-has-local-validity", R"gti(
class Box<T> {
  T value;
public:
  Box(T input) : value(input) {}
  int inspect() { return 7; }
};
int inspect<T>(Box<T> value) { return value.inspect(); }
int main() { Box<int> box{42}; return inspect(box) - 7; }
)gti",
        true);
  check("local-generic-validity-does-not-imply-static", R"gti(
int require_static<T>(T& value) requires outlives<T, static> { return 7; }
int inspect<T>(T value) { return require_static(value); }
)gti",
        false, "GTI-S2127");
  check("local-generic-validity-does-not-escape", R"gti(
int observe<T, lifetime a>(T&<a> value) requires outlives<T, a> { return 7; }
T&<a> bad<T, lifetime a>(T value, T&<a> outside) {
  observe(value);
  return value;
}
)gti",
        false);
  check("generic-payload-reference-alias", R"gti(
enum State<T> { left(T item), right(T item) };
T&<a> get<T, lifetime a>(State<T>&<a> state) {
  switch (state) {
  case State<T>::left(auto& payload): { T& alias = payload; return alias; }
  case State<T>::right(auto& payload): return payload;
  }
}
int main() { State<int> state = State<int>::right(7); return get(state) - 7; }
)gti",
        true);
  check("stored-external-reference-keeps-its-declared-region", R"gti(
class View<lifetime a> {
  int&<a> value;
public:
  View(int&<a> input) : value(input) {}
  int&<a> get() { return this.value; }
};
int&<a> use<lifetime a>(int&<a> input) {
  View<a> view{input};
  return view.get();
}
int main() { int value = 7; return use(value) - 7; }
)gti",
        true);
  check("method-receiver-instantiates-owner-regions", R"gti(
class View<lifetime a> {
  int&<a> value;
public:
  View(int&<a> input) : value(input) {}
  int read() { return this.value; }
};
int inspect<lifetime a>(int&<a> input) {
  View<a> view{input};
  return view.read();
}
int main() { int value = 7; return inspect(value) - 7; }
)gti",
        true);
  check("borrowed-value-parameter-publishes-stored-suppliers", R"gti(
struct View<lifetime a> {
  int&<a> first; int&<a> second;
  View(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int&<a> choose<lifetime a>(View<a> view, bool first) {
  if (first) { return view.first; }
  return view.second;
}
int main() {
  int left = 3; int right = 7;
  View view{left, right};
  return choose(view, false) - 7;
}
)gti",
        true);
  check("stored-supplier-contract-retains-unused-owner", R"gti(
struct View<lifetime a> {
  int&<a> first; int&<a> second;
  View(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int&<a> first<lifetime a>(View<a> view) { return view.first; }
int main() {
  int left = 3; mut int right = 7;
  View view{left, right};
  int& result = first(view);
  right = 9;
  return result;
}
)gti",
        false);
  check("stored-reference-cannot-change-declared-owner-region", R"gti(
struct Split<lifetime a, lifetime b> {
  int&<a> first; int&<b> second;
  Split(int&<a> left, int&<b> right) : first(left), second(right) {}
  int&<a> wrong() { return this.second; }
};
)gti",
        false, "GTI-S2127");
  check("projected-only-class-instantiation", R"gti(
struct Fixed<int&<source>> {
  int&<source> value;
  Fixed(int&<source> input) : value(input) {}
  int&<source> get() { return this.value; }
};
int&<a> read<lifetime a>(int&<a> input) {
  Fixed<int&<a>> holder{input};
  return holder.get();
}
int main() { int value = 7; return read(value) - 7; }
)gti",
        true);
  check("inherited-inline-fields-remain-disjoint", R"gti(
struct Base {
  mut int left = 1;
  Base() = default;
};
struct Derived : public Base {
  mut int right = 2;
  Derived() = default;
};
int bump(mut int& first, mut int& second) {
  first = first + 1;
  second = second + 1;
  return first + second;
}
int main() { mut Derived pair{}; return bump(pair.left, pair.right) - 5; }
)gti",
        true);
  check("same-inherited-inline-field-remains-exclusive", R"gti(
struct Base {
  mut int left = 1;
  Base() = default;
};
struct Derived : public Base {
  mut int right = 2;
  Derived() = default;
};
int bump(mut int& first, mut int& second) {
  first = first + 1;
  second = second + 1;
  return first + second;
}
int main() { mut Derived pair{}; return bump(pair.left, pair.left) - 5; }
)gti",
        false);
  check("inherited-reference-keeps-declaring-base-path", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
  int&<a> get() { return value; }
};
int&<a> read<lifetime a>(Derived<a>& view) { return view.value; }
int main() { int value = 7; Derived view{value}; return read(view) + view.get() - 14; }
)gti",
        true);
  check("nested-inherited-reference-retains-every-base-owner", R"gti(
struct Root<T, lifetime a> {
  T&<a> value;
  Root(T&<a> input) : value(input) {}
};
struct Middle<T, lifetime a> : public Root<T, a> {
  Middle(T&<a> input) : Root<T, a>(input) {}
};
struct Leaf<lifetime a> : public Middle<int, a> {
  Leaf(int&<a> input) : Middle<int, a>(input) {}
  int&<a> get() { return value; }
};
int&<a> read<lifetime a>(Leaf<a>& view) { return view.value; }
int main() { int value = 7; Leaf view{value}; return read(view) + view.get() - 14; }
)gti",
        true);
  check("inherited-reference-after-interface-base", R"gti(
interface Marker { int mark(); };
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Marker, public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
  int mark() override { return 0; }
};
int&<a> read<lifetime a>(Derived<a>& view) { return view.value; }
int main() { int value = 7; Derived view{value}; return read(view) - 7; }
)gti",
        true);
  check("inherited-and-direct-borrows-retain-distinct-origins", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.value;
}
int main() { int value = 7; return external(value) - 7; }
)gti",
        true);
  check("inherited-field-read-through-reference-parameter", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
};
int&<a> through<lifetime a>(Derived<a>& view) { return view.value; }
int&<a> direct<lifetime a>(Base<a>& view) { return view.value; }
int main() {
  int value = 7;
  Derived view{value};
  return through(view) + direct(view) - 14;
}
)gti",
        true);
  check("swapped-base-suppliers-keep-each-field-origin", R"gti(
struct Root<lifetime a, lifetime b> {
  int&<a> first;
  int&<b> second;
  Root(int&<a> first, int&<b> second) : first(first), second(second) {}
};
struct Swapped<lifetime a, lifetime b> : public Root<b, a> {
  Swapped(int&<a> first, int&<b> second) : Root<b, a>(second, first) {}
};
int&<a> pick<lifetime a>(int&<a> input) {
  int local = 9;
  Swapped view{local, input};
  return view.first;
}
int main() { int value = 7; return pick(value) - 7; }
)gti",
        true);
  check("inherited-local-supplier-cannot-escape", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
};
int&<a> leak<lifetime a>(int&<a> input) {
  int local = 3;
  Derived view{local};
  return view.value;
}
int main() { int value = 7; return leak(value); }
)gti",
        false, "GTI-S2017");
  check("inherited-referent-stays-borrowed", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
};
int main() { mut int value = 7; Derived view{value}; value = 8; return view.value; }
)gti",
        false, "GTI-S2017");
  check("inherited-and-direct-borrows-keep-the-local-field-local", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{local, input};
  return pair.value;
}
int main() { int value = 7; return external(value) - 9; }
)gti",
        false, "GTI-S2017");
  check("inherited-and-direct-fields-cannot-duplicate-exclusive-input", R"gti(
struct Base<lifetime a> {
  mut int&<a> value;
  Base(mut int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Base<a> {
  mut int&<a> other;
  Derived(mut int&<a> input) : Base<a>(input), other(input) {}
};
int main() { return 0; }
)gti",
        false, "GTI-S2045");
  check("inherited-external-owner-cannot-escape", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
};
Derived<static> invalid() {
  int value = 7;
  Derived<static> escaped{value};
  return escaped;
}
)gti",
        false, "GTI-S2127");
  check("default-field-keeps-constructor-lifetime-universe", R"gti(
enum Reading<lifetime a> { empty, value(int&<a> item) };
struct Default<int&<source>> {
  Reading<source> reading = Reading<source>::empty;
  Default(int count) {}
};
int main() { Default<int&<static>> holder{0}; return 0; }
)gti",
        true);
  check("explicit-field-initializer-replaces-ownerless-default", R"gti(
enum Reading<lifetime a> { empty, value(int&<a> item) };
struct Default<int&<source>> {
  Reading<source> reading = Reading<source>::empty;
  Default(int count) {}
  Default(int&<source> item, int count) : reading(Reading<source>::value(item)) {}
};
int main() {
  int value = 7;
  Default<int&> active{value, 0};
  switch (active.reading) {
    case Reading<_>::empty: return 1;
    case Reading<_>::value(auto& item): return item - 7;
  }
}
)gti",
        true);
  check("overridden-ownerless-default-cannot-hide-an-escaping-reference", R"gti(
enum Reading<lifetime a> { empty, value(int&<a> item) };
struct Default<int&<source>> {
  Reading<source> reading = Reading<source>::empty;
  Default(int&<source> item) : reading(Reading<source>::value(item)) {}
};
Default<int&<static>> invalid() {
  int value = 7;
  Default<int&<static>> escaped{value};
  return escaped;
}
)gti",
        false, "GTI-S2127");
  check("field-default-keeps-declaration-name-lookup", R"gti(
int initial_score() { return 7; }
struct Fixed<int&<source>> {
  int&<source> value;
  int score = initial_score();
  Fixed(int&<source> input, int initial_score) : value(input) {}
};
int main() { int value = 2; Fixed<int&> holder{value, 99}; return holder.score - 7; }
)gti",
        true);
  check("physical-payload-borrow-keeps-caller-region", R"gti(
enum State { value(int item) };
int&<a> select<lifetime a>(State&<a> state) {
  switch (state) { case State::value(auto& payload): return payload; }
}
int main() { State state = State::value(7); return select(state) - 7; }
)gti",
        true);
  check("local-physical-payload-borrow-has-storage-region", R"gti(
enum State { value(int item) };
int main() {
  State state = State::value(7);
  switch (state) { case State::value(auto& payload): return payload - 7; }
}
)gti",
        true);
  check("stored-reference-reborrow-keeps-external-region", R"gti(
enum Link<lifetime a> { value(int&<a> item) };
int&<a> select<lifetime a>(Link<a>& link) {
  switch (link) { case Link<a>::value(auto& payload): return payload; }
}
int main() { int value = 7; auto link = Link<_>::value(value); return select(link) - 7; }
)gti",
        true);
  check("payload-reference-cannot-be-extended-to-static", R"gti(
enum Link<lifetime a> { value(int&<a> item) };
int&<static> select<lifetime a>(Link<a>& link) {
  switch (link) { case Link<a>::value(auto& payload): return payload; }
}
)gti",
        false, "GTI-S2127");
  check("local-payload-is-not-caller-storage", R"gti(
enum State { value(int item) };
int&<a> bad<lifetime a>(int&<a> caller) {
  State local = State::value(7);
  switch (local) { case State::value(auto& payload): return payload; }
}
)gti",
        false);
  check("no-local-escape", R"gti(
int&<a> escape<lifetime a>(int&<a> input) { int local = 9; return local; }
)gti",
        false);
  check("unknown-lifetime", R"gti(
int&<missing> bad(int& input) { return input; }
)gti",
        false, "GTI-S2128");
  check("lifetime-is-not-runtime-value", R"gti(
int bad<lifetime a>() { return a; }
)gti",
        false, "GTI-S2128");
  return failures == 0 ? 0 : 1;
}
