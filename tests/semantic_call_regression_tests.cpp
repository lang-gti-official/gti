#include "compiler_test_fixture.h"

namespace {
lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("semantic-call-regression.gti",
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}
void accepted(std::string source) {
  const auto result = analyze(std::move(source));
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(),
         "valid generic call composition reaches MIR");
  if (result.canGenerateCode())
    expect(lang::verifyMirProgram(result.mir).valid(),
           "call composition verifies");
}
void rejected(std::string source, std::string_view code,
              std::string_view message, std::string_view anchor,
              std::string_view hint = {}) {
  const auto result = analyze(source);
  std::vector<const lang::Diagnostic *> errors;
  for (const auto &diagnostic : result.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      errors.push_back(&diagnostic);
  if (errors.size() != 1 || errors.front()->code != code)
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(errors.size() == 1, "one causal error without follow-on diagnostics");
  if (errors.size() != 1)
    return;
  const auto &diagnostic = *errors.front();
  expect(diagnostic.code == code &&
             diagnostic.message.find(message) != std::string::npos,
         "stable specific diagnostic code and cause");
  if (diagnostic.primary.start != source.rfind(anchor))
    std::cerr << "Span for " << code << ": expected " << source.rfind(anchor)
              << " at '" << anchor << "', found " << diagnostic.primary.start
              << " near '" << source.substr(diagnostic.primary.start, 35)
              << "'\n";
  expect(diagnostic.primary.source.ends_with("semantic-call-regression.gti") &&
             diagnostic.primary.start == source.rfind(anchor) &&
             diagnostic.primary.end > diagnostic.primary.start,
         "diagnostic identifies the caller expression");
  if (!hint.empty())
    expect(std::any_of(diagnostic.hints.begin(), diagnostic.hints.end(),
                       [&](const auto &text) {
                         return text.find(hint) != std::string::npos;
                       }),
           "diagnostic explains the actionable failure");
}
} // namespace
int main() {
  accepted(R"GTI(concept steps<I> = requires(mut I& p) { { ++p } -> void; };
struct Counter { mut int at = 0; public: Counter() = default; void operator++() mut { this.at++; } };
void step<steps I>(mut I& it) { ++it; }
I skip<steps I>(mut I first) {
  step(first);
  return std::move(first);
}
int main() {
  Counter c{};
  Counter d = skip(c);
  return d.at == 1 ? 0 : 1;
}
)GTI");
  accepted(R"GTI(import std.functional;
class table<K, H> {
  H hasher;
public:
  table(H h) requires std::read_callable<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires std::read_callable<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  accepted(
      R"GTI(concept maps<F, A, R> = requires(F& f, A& a) { { f(a) } -> R; };
class table<K, H> {
  H hasher;
public:
  table(H h) requires maps<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires maps<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  accepted(R"GTI(import std.functional;
class table<K, H> {
  H hasher;
public:
  table(H h) requires std::read_callable<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires std::read_callable<H, K, uint64_t> {
    uint64_t hash = this.hasher(key); return hash % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  accepted(R"GTI(class Parcel {
public:
  int value;
  Parcel(int v) : value(v) {}
  Parcel(Parcel&) = delete;
  Parcel(Parcel&&) = default;
};
void consume<Args...>(int tag, Args... args) {}
void consume(char tag) {}
int main() { Parcel p{42}; consume(1, std::move(p)); return 0; }
)GTI");
  accepted(R"GTI(constexpr int source() { return 7; }
consteval int check(int value) { return value; }
int main() { return check(source()) - 7; }
)GTI");
  accepted(
      R"GTI(concept stepped<T> = requires(T& value) { { value + value } -> T; };
void sink<stepped Args...>(Args... args) {}
void forward<stepped Args...>(Args... args) { sink(args...); }
int main() { forward(1, 2); return 0; }
)GTI");
  rejected(R"GTI(class Parcel {
public:
  int value;
  Parcel(int v) : value(v) {}
  Parcel(Parcel&) = delete;
  Parcel(Parcel&&) = default;
};
void consume<Args...>(int tag, Args... args) {}
void consume(char tag) {}
int main() { Parcel p{42}; consume(1, p); return 0; }
)GTI",
           "GTI-S2018", "deleted copy constructor", "p);", "std::move");
  rejected(R"GTI(import std.string;
import std.format;

int main() {
  mut std::string s = "hello";
  std::println("{}", s);
  return 0;
}
)GTI",
           "GTI-S2018", "deleted copy constructor", "s);", "std::move");
  rejected(R"GTI(int dynamic_value() { return 7; }
consteval int check(int value) { return value; }
int main() { return check(dynamic_value()); }
)GTI",
           "GTI-S2057", "Consteval invocation", "); }",
           "Argument 1 to immediate function 'check'");
  rejected(R"GTI(consteval int check(int value) { return value; }
int run(int value) { return check(value); }
int main() { return run(1); }
)GTI",
           "GTI-S2057", "Consteval invocation", "); }\nint main",
           "Argument 1 to immediate function 'check'");
  accepted(R"GTI(class Parcel {
public:
  int value;
  Parcel(int v) : value(v) {}
  Parcel(Parcel&) = delete;
  Parcel(Parcel&&) = default;
};
void consume<Args...>(int tag, Args... args) {}
void consume<T>(int tag, T& value) {}
int main() { Parcel p{42}; consume(1, p); return 0; }
)GTI");
  accepted(R"GTI(void retain<F>(F callback) {}
F relay<F>(F callback) { return std::move(callback); }
int main() {
  int value = 42;
  auto callback = [&value]() -> int { return value; };
  auto result = relay(std::move(callback));
  return result();
}
)GTI");
  accepted(
      R"GTI(concept maps<F, A, R> = requires(F& f, A& a) { { f(a) } -> R; } || requires(F& f, A a) { { f(std::move(a)) } -> R; };
class table<K, H> {
  H hasher;
public:
  table(H h) requires maps<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires maps<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  accepted(R"GTI(class Writer<T> {
public:
  Writer() = default;
  int write(T& value) requires std::integral<T> { return int(value); }
};
concept Writable<T> = requires(Writer<T>& writer, T& value) {
  { writer.write(value) } -> int;
};
void sink<Writable First, Writable Args...>(First first, Args... args) {}
void forward<Writable First, Writable Args...>(First first, Args... args) {
  sink(std::move(first), args...);
}
int main() { forward(1, 2, 3); return 0; }
)GTI");
  rejected(R"GTI(class Parcel {
public:
  int value;
  Parcel(int v) : value(v) {}
  Parcel(Parcel&) = delete;
  Parcel(Parcel&&) = default;
};
void consume<Args...>(int tag, Args... args) {}
void consume(char tag) {}
int main() { Parcel p{42}; consume(true, p); return 0; }
)GTI",
           "GTI-S2012", "No overload", "consume(true, p)", "");
  rejected(R"GTI(concept steps<I> = requires(mut I& p) { { ++p } -> void; };
struct Counter { mut int at = 0; public: Counter() = default; void operator++() mut { this.at++; } };
void step<steps I>(mut I& it) { ++it; }
I skip<steps I>(I first) {
  step(first);
  return std::move(first);
}
int main() {
  Counter c{};
  Counter d = skip(c);
  return d.at == 1 ? 0 : 1;
}
)GTI",
           "GTI-S2017", "requires a mutable value", "first);\n  return", "");
  rejected(R"GTI(import std.string;
F relay<F>(F callback) { return std::move(callback); }
int main() {
  std::string_view text = "borrowed";
  auto callback = [text]() -> int { return 1; };
  auto result = relay(std::move(callback));
  return result();
}
)GTI",
           "GTI-S2046", "borrowed or raw-pointer capture state",
           "));\n  return result", "");
  rejected(
      R"GTI(concept maps<F, A, R> = requires(F& f, A& a) { { f(a) } -> R; } || requires(F& f, A a) { { f(std::move(a)) } -> bool; };
class table<K, H> {
  H hasher;
public:
  table(H h) requires maps<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires maps<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI",
      "GTI-S2004", "requires integer operands", "% n;", "");
  rejected(
      R"GTI(concept maps<F, A, R> = requires(F& f, A& a) { { f(a) } -> R; } || requires(F& f, bool a) { { f(a) } -> R; };
class table<K, H> {
  H hasher;
public:
  table(H h) requires maps<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires maps<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI",
      "GTI-S2004", "requires integer operands", "% n;", "");
  rejected(R"GTI(import std.functional;
class Payload {
public:
  int value;
  Payload(int value) : value(value) {}
  Payload(Payload&) = delete;
  Payload(Payload&&) = default;
};
struct Consumer {
  Consumer() = default;
  uint64_t operator()(Payload value) { return uint64_t(value.value); }
};
class Holder<F> {
  F callback;
public:
  Holder(F callback) : callback(std::move(callback)) {}
  uint64_t call(Payload& value) requires std::read_callable<F, Payload, uint64_t> {
    return this.callback(value) % uint64_t(4);
  }
};
int main() { Holder<Consumer> holder{Consumer()}; Payload p{9}; return int(holder.call(p)); }
)GTI",
           "GTI-S2022", "No exact overload", ") % uint64_t",
           "ownership requirements");
  rejected(R"GTI(import std.string;
consteval std::size_t validate(std::string_view pattern) { return pattern.size(); }
#macro checked(std::string_view pattern, int value) {
  std::size_t count = validate(pattern);
}
int main() {
  std::string text{"dyn {}"};
  int value = 3;
  if (value == 4) { checked(text.view(), value); }
  return 0;
}
)GTI",
           "GTI-S2057", "Consteval invocation", "checked(text.view",
           "Argument 1 to immediate function");
  rejected(R"GTI(class Parcel {
public:
  int value;
  Parcel(int v) : value(v) {}
  Parcel(Parcel&) = delete;
  Parcel(Parcel&&) = default;
};
void consume<Args...>(int tag, Args... args) {}
void consume(char tag) {}
int main() { Parcel p{42}; consume(1, p, p); return 0; }
)GTI",
           "GTI-S2018", "deleted copy constructor", "p, p);", "std::move");
  rejected(R"GTI(consteval int divide(int value) { return 1 / value; }
int main() { return divide(0); }
)GTI",
           "GTI-S2057", "Consteval invocation", "); }", "zero divisor");
  accepted(R"GTI(class Writer<T> {
public:
  Writer() = default;
  int write(T& value) requires std::integral<T> { return int(value); }
};
concept Writable<T> = requires(Writer<T>& writer, T& value) {
  { writer.write(value) } -> int;
};
void sink<Writable First, Writable Args...>(First first, Args... args) {}
concept Alias<T> = Writable<T>;
concept More<T> = Writable<T>;
void forward<Writable First, More Args...>(First first, Args... args) {
  sink(std::move(first), args...);
}
int main() { forward(1, 2, 3); return 0; }
)GTI");
  accepted(R"GTI(class Writer<T> {
public:
  Writer() = default;
  int write(T& value) requires std::integral<T> { return int(value); }
};
concept Writable<T> = requires(Writer<T>& writer, T& value) {
  { writer.write(value) } -> int;
};
void sink<Writable First, Writable Args...>(First first, Args... args) {}
concept Alias<T> = Writable<T>;
concept More<T> = Writable<T> && std::copyable<T>;
void forward<Writable First, More Args...>(First first, Args... args) {
  sink(std::move(first), args...);
}
int main() { forward(1, 2, 3); return 0; }
)GTI");
  accepted(R"GTI(class Writer<T> {
public:
  Writer() = default;
  int write(T& value) requires std::integral<T> { return int(value); }
};
concept Writable<T> = requires(Writer<T>& writer, T& value) {
  { writer.write(value) } -> int;
};
void sink<Writable First, Writable Args...>(First first, Args... args) {}
concept Alias<T> = Writable<T>;
concept More<T> = Writable<T> || Alias<T>;
void forward<Writable First, More Args...>(First first, Args... args) {
  sink(std::move(first), args...);
}
int main() { forward(1, 2, 3); return 0; }
)GTI");
  rejected(R"GTI(class Writer<T> {
public:
  Writer() = default;
  int write(T& value) requires std::integral<T> { return int(value); }
};
concept Writable<T> = requires(Writer<T>& writer, T& value) {
  { writer.write(value) } -> int;
};
void sink<Writable First, Writable Args...>(First first, Args... args) {}
concept Alias<T> = Writable<T>;
concept More<T> = Writable<T> || std::copyable<T>;
void forward<Writable First, More Args...>(First first, Args... args) {
  sink(std::move(first), args...);
}
int main() { forward(1, 2, 3); return 0; }
)GTI",
           "GTI-S2029", "concept 'Writable'", ");\n}");
  accepted(
      R"GTI(concept maps<F, A, R> = requires(F& f, A& a) { { (f((a))) } -> R; };
class table<K, H> {
  H hasher;
public:
  table(H h) requires maps<H, K, uint64_t> : hasher(std::move(h)) {}
  uint64_t slot(K key, uint64_t n) requires maps<H, K, uint64_t> {
    return this.hasher(key) % n;
  }
};
struct id_hash {
  id_hash() = default;
  uint64_t operator()(int& v) { return uint64_t(v); }
};
int main() {
  table<int, id_hash> t{id_hash()};
  return t.slot(9, uint64_t(4)) == uint64_t(1) ? 0 : 1;
}
)GTI");
  // An immutable text alias retains the exact return dependency, including
  // through a helper call and lexical blocks. Its body still proves the owner.
  const std::string textOwner = R"GTI(import std.memory;
class ByteOwner {
  uint8_t& byte;
public:
  ByteOwner(uint8_t& byte) : byte(byte) {}
  uint8_t& get() { return this.byte; }
  std::string_view text() {
    unsafe {
      const char* first = std::typed_address<char>(std::erase_address(&this.byte));
      auto alias = std::assume_string_view(first, std::size_t(1), this);
      return alias;
    }
  }
  c_string ctext() {
    unsafe {
      const char* first = std::typed_address<char>(std::erase_address(&this.byte));
      auto alias = std::assume_c_string(first, this);
      return alias;
    }
  }
};
std::string_view view(ByteOwner& owner) {
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&owner.get()));
    auto text = std::assume_string_view(first, std::size_t(1), owner);
    auto second = text;
    return second;
  }
}
c_string ctext(ByteOwner& owner) {
  unsafe {
    const char* first = std::typed_address<char>(std::erase_address(&owner.get()));
    auto text = std::assume_c_string(first, owner);
    return text;
  }
}
)GTI";
  accepted(textOwner + R"GTI(std::string_view relay(ByteOwner& owner) {
  auto text = view(owner);
  { auto text = "ownerless"; }
  return text;
}
int main() {
  uint8_t bytes[2] = {uint8_t(104), uint8_t(0)};
  uint8_t& byte = bytes[0]; ByteOwner owner{byte};
  return int(relay(owner).size()) - 1;
}
)GTI");
  for (
      const std::string invalid :
      {// Different return paths cannot select a single caller owner.
       R"GTI(std::string_view bad(ByteOwner& left, ByteOwner& right, bool branch) {
  if (branch) { auto text = view(left); return text; }
  auto text = view(right); return text;
})GTI",
       // A same-named local must shadow the parameter, never impersonate it.
       R"GTI(std::string_view bad(ByteOwner& owner) {
  uint8_t bytes[1] = {uint8_t(104)}; uint8_t& byte = bytes[0];
  { ByteOwner owner{byte}; auto text = view(owner); return text; }
})GTI",
       // Mutable aliases require a path-sensitive assignment summary.
       R"GTI(std::string_view bad(ByteOwner& left, ByteOwner& right) {
  mut auto text = view(left); text = view(right); return text;
})GTI"}) {
    const auto result =
        analyze(textOwner + invalid + "\nint main() { return 0; }\n");
    expect(!result.canGenerateCode() &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2045";
                           }),
           "text alias return cannot escape an unproved or local owner");
  }
  return failures == 0 ? 0 : 1;
}
