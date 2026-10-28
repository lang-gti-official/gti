#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const std::string &message) {
  if (!ok) {
    std::cerr << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend().analyze("protected.gti", source,
                                  {root / "stdlib/prelude.gti"}, {},
                                  {root / "stdlib"});
}
void accept(const std::string &source) {
  const auto result = analyze(source);
  if (!result.canGenerateCode()) {
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    std::cerr << source << '\n';
  }
  check(result.canGenerateCode(),
        "protected program must pass through verified MIR");
}
void reject(const std::string &source, const std::string &code = "GTI-S2007") {
  const auto result = analyze(source);
  check(result.syntaxValid && !result.semanticValid &&
            !result.canGenerateCode(),
        "invalid access must fail in semantics: " + source);
  auto found =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const auto &d) { return d.code == code; });
  check(found != result.diagnostics.end(), "expected " + code + ": " + source);
  if (found != result.diagnostics.end() && code == "GTI-S2007") {
    check(found->message.find("protected") != std::string::npos,
          "access error names protected visibility");
    check(std::filesystem::path(found->primary.source).filename() ==
                  "protected.gti" &&
              found->primary.end > found->primary.start &&
              !found->related.empty() &&
              std::filesystem::path(found->related.front().span.source)
                      .filename() == "protected.gti",
          "access error retains use and declaration spans");
  }
}
const std::string base = R"(
class Base {
protected:
  mut int value = 1;
  int method() { return value; }
  static int secret() { return 3; }
  static mut int shared = 2;
  using Number = int;
  struct Detail { int code = 4;  public: Detail() = default; };
public:
  Base() {}
  int inspect(Base& peer) { return peer.value; }
  virtual int api() { return value; }
};
struct Sibling : public Base { public: Sibling() = default; };
)";
} // namespace
int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  std::ifstream stream(root / "examples/150-protected-events.gti");
  const std::string example((std::istreambuf_iterator<char>(stream)), {});
  accept(example);
  accept(base + R"(
struct Derived : public Base {
  int read(Derived& peer) { return peer.value + peer.method(); }
  void update() mut { value = 9; }
  int types() { Base::Number x = 2; Base::Detail d{}; return x + d.code; }
  static int helper() { return Base::secret() + Base::shared; }
  int api() override { return method(); }
public:
  Derived() = default;
};
struct Leaf : public Derived { int read_leaf(Leaf& peer) { return peer.value; }  public: Leaf() = default; };
int main() { mut Derived d{}; d.update(); return d.api() - 9; }
)");
  accept(R"(class Base { protected: Base() {} }; struct D : public Base { public: D() = default; };
int main() { D value{}; return 0; })");
  accept(base + R"(struct Derived : public Base { int read();  public: Derived() = default; };
int Derived::read() { return value; }
int main() { Derived d{}; return d.read() - 1; })");
  accept(base + R"(struct Derived : public Base {
struct Nested { static int read(Derived& value) { return value.value; }  public: Nested() = default; };
public:
  Derived() = default;
}; int main() { Derived d{}; return Derived::Nested::read(d) - 1; })");
  for (const std::string expression :
       {"item.value", "item.method()", "Base::secret()", "Base::shared"})
    reject(base + "int test(Base& item) { return " + expression + "; }");
  for (const std::string receiver : {"Base", "Sibling"}) {
    for (const std::string expression : {"peer.value", "peer.method()"})
      reject(base + "struct Derived : public Base { int test(" + receiver +
             "& peer) { return " + expression + "; } };");
  }
  reject(base + "int test() { Base::shared = 4; return 0; }");
  reject(base + "using Hidden = Base::Number;");
  reject(base + "Base::Detail make() { return Base::Detail(); }");
  reject(R"(class Base { protected: Base(int x) {}  public: Base() = default; };
struct Derived : public Base { Derived() : Base(1) {} Base make() { return Base(2); } };)");
  reject(
      R"(class Base { protected: Base() {} }; int main() { Base value{}; return 0; })");
  accept(R"(class Converted { protected: implicit Converted(int value) {}
public: static int make() { Converted value = 1; return 0; }  public: Converted() = default; };
int main() { return Converted::make(); })");
  reject(R"(class Converted { protected: implicit Converted(int value) {}  public: Converted() = default; };
int main() { Converted value = 1; return 0; })");
  reject(R"(class Converted { protected: implicit Converted(int value) {}  public: Converted() = default; };
struct Derived : public Converted { Derived() : Converted(1) {}
int read() { Converted value = 1; return 0; } };)");
  reject(R"(struct Base { public: Base() = default; }; struct Derived : protected Base { public: Derived() = default; };)", "GTI-S2040");
  reject(R"(class Bad { protected: ~Bad() {}  public: Bad() = default; };)", "GTI-S2021");
  reject(R"(union Bad { protected: int value; };)", "GTI-S2066");
  reject(R"([[c_abi]] struct Bad { protected: int value; };)", "GTI-S2064");
  reject(base +
             "struct Derived : public Base { void bad() { this.value = 2; }  public: Derived() = default; };",
         "GTI-S2000");
  reject(base + "interface Bad { protected: int read(); };", "GTI-S2041");
  reject(
      R"(class Base<T> { protected: T value; public: Base(T x) : value(x) {} };
struct D<T> : public Base<T> { D(T x) : Base<T>(x) {} T bad(Base<T>& b) { return b.value; } };
int main() { D<int> d{1}; Base<int> b{2}; return d.bad(b); })");
  reject(R"(class Base<T> { protected: static int secret() { return 1; }  public: Base() = default; };
struct Derived : public Base<int> { int bad() { return Base<bool>::secret(); }  public: Derived() = default; };
int main() { Derived d{}; return d.bad(); })",
         "GTI-S2039");
  reject(R"(class Base<T> { protected: static int secret = 1;  public: Base() = default; };
struct Derived : public Base<int> { int bad() { return Base<bool>::secret; }  public: Derived() = default; };
int main() { Derived d{}; return d.bad(); })",
         "GTI-S2039");
  reject(R"(class Base<T> { protected: int value = 1;  public: Base() = default; };
struct D<T> : public Base<T> { int bad(D<bool>& peer) { return peer.value; }  public: D() = default; };
int main() { D<int> d{}; D<bool> peer{}; return d.bad(peer); })");
  accept(base + R"(struct Derived : public Base {
unsafe int raw(Derived* peer) { unsafe { return peer->value; } }
public:
  Derived() = default;
};)");
  reject(base + R"(struct Derived : public Base {
unsafe int raw(Base* peer) { unsafe { return peer->value; } }
public:
  Derived() = default;
};)");
  reject(R"(class Base<T> { protected: using Hidden = T;  public: Base() = default; };
struct Derived : public Base<int> { int bad() { Base<bool>::Hidden value = true; return 0; }  public: Derived() = default; };)");
  accept(R"(class Base<T> { protected: using Hidden = T;  public: Base() = default; };
struct Derived : public Base<int> { int good() { Base<int>::Hidden value = 1; return value; }  public: Derived() = default; };)");
  accept(R"(class Base { public: virtual int read() { return 1; }  public: Base() = default; };
class Derived : public Base { protected: int read() override { return 2; }  public: Derived() = default; };
int main() { Derived d{}; Base& b = d; return b.read() - 2; })");
  reject(R"(class Base { public: virtual int read() { return 1; }  public: Base() = default; };
class Derived : public Base { protected: int read() override { return 2; }  public: Derived() = default; };
int main() { Derived d{}; return d.read(); })");
  accept(
      R"(class Base<T, uint64_t N> { protected: T value; public: Base(T x) : value(x) {} };
struct Derived<T, uint64_t N> : public Base<T, N> {
  Derived(T x) : Base<T, N>(x) {}
  unsafe T read_raw(Derived<T, N>* peer) { unsafe { return peer->value; } }
};
int use(Derived<int, 7>* peer) { Derived<int, 7> d{1}; unsafe { return d.read_raw(peer); } })");
  reject(
      R"(class Base<T, uint64_t N> { protected: T value; public: Base(T x) : value(x) {} };
struct Derived<T, uint64_t N> : public Base<T, N> { Derived(T x) : Base<T, N>(x) {} };
int main() { Derived<int, 7> d{}; return 0; })",
      "GTI-S2012");
  const lang::Formatter formatter;
  const auto formatted = formatter.format(example);
  check(formatter.format(formatted) == formatted &&
            formatted.find("\nprotected:\n") != std::string::npos,
        "protected formatting is outdented and idempotent");
  accept(formatted);
  return failures ? 1 : 0;
}
