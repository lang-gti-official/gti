#include "gti/frontend.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
  const bool mir = argc > 1 && std::string_view(argv[1]) == "--mir";
  int failures = 0;
  const auto check = [&](std::string_view name, std::string_view source,
                         bool accepted, std::string_view code = {},
                         bool withTextPrelude = false) {
    const auto frontend = lang::Frontend(
        {.stopAfter = mir ? lang::FrontendPhase::Mir : lang::FrontendPhase::Semantics});
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
    const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
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
    const auto result = withTextPrelude
        ? frontend.analyze(std::string(name) + ".gti", std::string(source),
                           {prelude}, {{prelude.string(), textPrelude}}, {root})
        : frontend.analyze(std::string(name) + ".gti", std::string(source));
    const bool passed = mir ? result.canGenerateCode() : result.semanticValid;
    const bool codePresent = code.empty() ||
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const auto &diagnostic) { return diagnostic.code == code; });
    const bool internal = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                                     [](const auto &diagnostic) {
                                       return diagnostic.code == "GTI-B0001";
                                     });
    if (passed != accepted || !codePresent || internal) {
      ++failures;
      std::cerr << name << ": expected " << (accepted ? "acceptance" : "rejection")
                << (code.empty() ? "" : std::string(" with ") + std::string(code)) << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
      if (mir && internal && argc > 2 &&
          (name == "stored-external-reference-keeps-its-declared-region" ||
           name == "borrowed-value-parameter-publishes-stored-suppliers")) {
        const std::size_t owner =
            name == "borrowed-value-parameter-publishes-stored-suppliers" ? 1 : 2;
        const auto directory = std::filesystem::path(argv[2]);
        std::filesystem::create_directories(directory);
        const auto path = directory / (std::string(name) + ".mir.txt");
        std::ofstream evidence(path);
        if (const auto *function = result.mir.findFunctionInstance(owner))
          evidence << lang::MirPrinter{}.print(function->body);
        if (name == "stored-external-reference-keeps-its-declared-region")
          if (const auto *getter = result.mir.findFunctionInstance(3)) {
            evidence << "GETTER owner=3\n";
            evidence << lang::MirPrinter{}.print(getter->body);
          }
        if (const auto *function = result.hir.findFunctionInstance(owner)) {
          evidence << "HIR owner=" << owner << " declaration=" << function->declaration << '\n';
          for (const auto &loan : function->body.loans) {
            evidence << "HIR loan=" << loan.semanticLoan << " kind=" << static_cast<int>(loan.kind)
                     << " parent=" << loan.parent << " stored=" << loan.storedDependency
                     << " entry=" << loan.entry << " suppliers=" << loan.additionalDependencies.size()
                     << " fields=" << loan.designatedFields.size() << '\n';
          }
          for (const auto &value : function->body.values)
            if (value.borrowOrigin != lang::BorrowOriginKind::None || value.borrowDependencies)
              evidence << "HIR value=" << value.id << " kind=" << static_cast<int>(value.kind)
                       << " origin=" << static_cast<int>(value.borrowOrigin)
                       << " parameter=" << value.borrowArgument << " suppliers=" << value.borrowFields.size()
                       << " complete=" << value.borrowDependencies.has_value() << '\n';
        }
        std::cerr << "  failing owner " << owner << " evidence: " << path << '\n';
      }
      if (mir && internal) {
        const auto suppliers = [](std::string_view label, const auto &fields) {
          std::cerr << "  " << label << " suppliers=" << fields.size();
          for (const auto &field : fields) {
            std::cerr << " [origin=" << static_cast<int>(field.origin)
                      << " argument=" << field.parameter << " source=";
            for (const auto part : field.sourceFields) std::cerr << part << '/';
            std::cerr << " conditions=" << field.sourceConditions.size() << ']';
          }
          std::cerr << '\n';
        };
        for (const auto &function : result.mir.functionInstances())
          for (const auto &block : function.body.blocks)
            for (const auto &instruction : block.instructions)
              if (instruction.functionTarget) {
                const auto *target = result.mir.findFunctionInstance(*instruction.functionTarget);
                if (target && instruction.borrowFields != target->returnBorrowFields) {
                  std::cerr << "  supplier mismatch target=" << target->id << '\n';
                  suppliers("call", instruction.borrowFields);
                  suppliers("target", target->returnBorrowFields);
                  if (const auto *declaration = result.semantics.findFunction(target->declaration)) {
                    std::cerr << "  declaration lifetime-origin="
                              << static_cast<int>(declaration->lifetimeContract.origin)
                              << " valid=" << declaration->lifetimeContract.valid()
                              << " explicit-origin=" << declaration->explicitReturnBorrowOrigin
                              << " complete=" << declaration->completeReturnBorrow.has_value() << '\n';
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
)gti", true, {}, true);
  check("text-equality-does-not-admit-other-scalar-shapes", R"gti(
bool wrong(std::string_view text, int number) { return text == number; }
)gti", false, "GTI-S2000", true);
  check("explicit-identity", R"gti(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int main() { int value = 7; int& result = identity(value); return result - 7; }
)gti", true);
  check("elided-identity", R"gti(
int& identity(int& value) { return value; }
int main() { int value = 7; int& result = identity(value); return result - 7; }
)gti", true);
  check("choose-two-origins", R"gti(
int&<a> choose<lifetime a>(int&<a> first, int&<a> second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
int main() { int first = 3; int second = 8; return choose(first, second, false) - 8; }
)gti", true);
  check("independent-regions", R"gti(
int&<a> first<lifetime a, lifetime b>(int&<a> left, int&<b> right) { return left; }
int main() { int left = 3; int right = 8; return first(left, right) - 3; }
)gti", true);
  check("wrong-owner-region", R"gti(
int&<a> wrong<lifetime a, lifetime b>(int&<a> left, int&<b> right) { return right; }
int main() { int left = 3; int right = 8; return wrong(left, right); }
)gti", false);
  check("subobject-preserves-owner-region", R"gti(
struct Number { int value = 7; Number() = default; };
int&<a> field<lifetime a>(Number&<a> number) { return number.value; }
int main() { Number number{}; return field(number) - 7; }
)gti", true);
  check("common-region-cannot-escape-as-first-input", R"gti(
int&<a> choose<lifetime a>(int&<a> first, int&<a> second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
int&<a> wrong<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  return choose(first, second, true);
}
)gti", false, "GTI-S2127");
  check("declaration-retains-unused-supplier", R"gti(
int&<a> first<lifetime a>(int&<a> left, int&<a> right) { return left; }
int main() {
  int left = 3;
  mut int right = 8;
  int& result = first(left, right);
  right = 9;
  return result;
}
)gti", false);
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
)gti", false);
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
)gti", true);
  check("ambiguous-elision", R"gti(
int& choose(int& first, int& second, bool pick_first) {
  if (pick_first) { return first; }
  return second;
}
)gti", false, "GTI-S2127");
  check("declared-outlives", R"gti(
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input)
  requires outlives<longer, shorter> { return input; }
)gti", true);
  check("undeclared-outlives", R"gti(
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input) {
  return input;
}
)gti", false);
  check("named-outlives-contract", R"gti(
concept Survives<lifetime longer, lifetime shorter> = outlives<longer, shorter>;
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> input)
  requires Survives<longer, shorter> { return input; }
)gti", true);
  check("disjunctive-requirement-inference", R"gti(
int&<a> identity<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
int main() { int value = 7; return identity(value) - 7; }
)gti", true);
  check("disjunction-is-not-two-assumptions", R"gti(
int&<b> bad<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
)gti", false);
  check("disjunctive-contract-forwarding", R"gti(
int&<a> inner<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> { return input; }
int&<a> outer<lifetime a, lifetime b>(int&<a> input)
  requires outlives<a, b> || outlives<b, a> {
  return inner<a, b>(input);
}
int main() { int value = 7; return outer(value) - 7; }
)gti", true);
  check("distributed-outlives-return", R"gti(
int&<c> shorten<lifetime a, lifetime b, lifetime c, lifetime d>(int&<a> input)
  requires (outlives<a, b> && outlives<b, c>) ||
           (outlives<a, d> && outlives<d, c>) { return input; }
int main() { int value = 7; return shorten(value) - 7; }
)gti", true);
  check("disjunctive-type-bound-forwarding", R"gti(
void inner<T, lifetime a, lifetime b>(T input)
  requires outlives<T, a> || outlives<T, b> {}
void outer<T, lifetime a, lifetime b>(T input)
  requires outlives<T, a> || outlives<T, b> { inner<T, a, b>(input); }
int main() { outer<int>(7); return 0; }
)gti", true);
  check("phantom-region-is-not-borrowed-storage", R"gti(
struct Tag<lifetime a> { int value = 7; Tag() = default; };
int read<lifetime a>(Tag<a> tag) requires outlives<Tag<a>, static> { return tag.value; }
int main() { Tag tag{}; return read(tag) - 7; }
)gti", true);
  check("borrowed-record-construction", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
int main() { int number = 7; View view{number}; return view.value - 7; }
)gti", true);
  check("borrowed-record-escape", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
View<a> bad<lifetime a>(int&<a> outside) {
  int local = 7;
  return View<a>(local);
}
)gti", false);
  check("anonymous-constructor-declaration-contract", R"gti(
struct Pair {
  int& first;
  int& second;
  Pair(int& left, int& right) : first(left), second(right) {}
};
int main() { int a = 3; int b = 8; Pair pair{a, b}; return pair.second - 8; }
)gti", true);
  check("explicit-owner-region-is-not-body-inferred", R"gti(
struct View<lifetime owner> {
  int&<owner> value;
  View(int& input) : value(input) {}
};
)gti", false, "GTI-S2127");
  check("anonymous-constructor-local-escape", R"gti(
struct View {
  int& value;
  View(int& input) : value(input) {}
};
View bad(int& outside) { int local = 3; return View(local); }
)gti", false);
  check("ordinary-input-is-not-static", R"gti(
int&<static> forever(int& input) { return input; }
)gti", false);
  check("thread-storage-is-not-static", R"gti(
thread_local int value = 7;
int&<static> forever() { return value; }
)gti", false, "GTI-S2127");
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
)gti", true);
  check("whole-receiver-contract-keeps-compact-call-summary", R"gti(
struct Number {
  int value = 7;
  Number() = default;
  int& get() { return this.value; }
};
int main() { Number number{}; int& alias = number.get(); return alias - 7; }
)gti", true);
  check("owned-generic-value-has-local-validity", R"gti(
int observe<T, lifetime a>(T&<a> value) requires outlives<T, a> { return 7; }
int inspect<T>(T value) { return observe(value); }
int main() { return inspect(42) - 7; }
)gti", true);
  check("owned-generic-receiver-has-local-validity", R"gti(
class Box<T> {
  T value;
public:
  Box(T input) : value(input) {}
  int inspect() { return 7; }
};
int inspect<T>(Box<T> value) { return value.inspect(); }
int main() { Box<int> box{42}; return inspect(box) - 7; }
)gti", true);
  check("local-generic-validity-does-not-imply-static", R"gti(
int require_static<T>(T& value) requires outlives<T, static> { return 7; }
int inspect<T>(T value) { return require_static(value); }
)gti", false, "GTI-S2127");
  check("local-generic-validity-does-not-escape", R"gti(
int observe<T, lifetime a>(T&<a> value) requires outlives<T, a> { return 7; }
T&<a> bad<T, lifetime a>(T value, T&<a> outside) {
  observe(value);
  return value;
}
)gti", false);
  check("generic-payload-reference-alias", R"gti(
enum State<T> { left(T item), right(T item) };
T&<a> get<T, lifetime a>(State<T>&<a> state) {
  switch (state) {
  case State<T>::left(auto& payload): { T& alias = payload; return alias; }
  case State<T>::right(auto& payload): return payload;
  }
}
int main() { State<int> state = State<int>::right(7); return get(state) - 7; }
)gti", true);
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
)gti", true);
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
)gti", true);
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
)gti", true);
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
)gti", false);
  check("stored-reference-cannot-change-declared-owner-region", R"gti(
struct Split<lifetime a, lifetime b> {
  int&<a> first; int&<b> second;
  Split(int&<a> left, int&<b> right) : first(left), second(right) {}
  int&<a> wrong() { return this.second; }
};
)gti", false, "GTI-S2127");
  check("physical-payload-borrow-keeps-caller-region", R"gti(
enum State { value(int item) };
int&<a> select<lifetime a>(State&<a> state) {
  switch (state) { case State::value(auto& payload): return payload; }
}
int main() { State state = State::value(7); return select(state) - 7; }
)gti", true);
  check("local-physical-payload-borrow-has-storage-region", R"gti(
enum State { value(int item) };
int main() {
  State state = State::value(7);
  switch (state) { case State::value(auto& payload): return payload - 7; }
}
)gti", true);
  check("stored-reference-reborrow-keeps-external-region", R"gti(
enum Link<lifetime a> { value(int&<a> item) };
int&<a> select<lifetime a>(Link<a>& link) {
  switch (link) { case Link<a>::value(auto& payload): return payload; }
}
int main() { int value = 7; auto link = Link<_>::value(value); return select(link) - 7; }
)gti", true);
  check("payload-reference-cannot-be-extended-to-static", R"gti(
enum Link<lifetime a> { value(int&<a> item) };
int&<static> select<lifetime a>(Link<a>& link) {
  switch (link) { case Link<a>::value(auto& payload): return payload; }
}
)gti", false, "GTI-S2127");
  check("local-payload-is-not-caller-storage", R"gti(
enum State { value(int item) };
int&<a> bad<lifetime a>(int&<a> caller) {
  State local = State::value(7);
  switch (local) { case State::value(auto& payload): return payload; }
}
)gti", false);
  check("no-local-escape", R"gti(
int&<a> escape<lifetime a>(int&<a> input) { int local = 9; return local; }
)gti", false);
  check("unknown-lifetime", R"gti(
int&<missing> bad(int& input) { return input; }
)gti", false, "GTI-S2126");
  check("lifetime-is-not-runtime-value", R"gti(
int bad<lifetime a>() { return a; }
)gti", false, "GTI-S2126");
  return failures == 0 ? 0 : 1;
}
