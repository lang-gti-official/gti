#include "compiler_test_fixture.h"

int main() {
  const auto application = [](const std::string &name,
                              const std::string &source,
                              lang::FrontendOptions options = {}) {
    const auto entry =
        std::filesystem::temp_directory_path() / ("gti-app-" + name);
    return lang::Frontend(std::move(options))
        .analyze(entry, source, {standardLibraryPrelude()},
                 {{std::filesystem::weakly_canonical(entry).string(), source}},
                 {standardLibraryRoot()});
  };
  const auto positive = [&](const std::string &name,
                            const std::string &source) {
    auto result = application(name + ".gti", source);
    if (!result.canGenerateCode())
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << name << ": " << diagnostic.code << " ("
                  << diagnostic.primary.source << ":" << diagnostic.primary.line
                  << "): " << diagnostic.message << '\n';
    expect(result.canGenerateCode(), name);
    lang::FrontendOptions reachableOptions;
    reachableOptions.toolingOccurrences = false;
    reachableOptions.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
    reachableOptions.lowerReachableOnly = true;
    reachableOptions.lowerLibraryReachableOnly = true;
    const auto reachable = application(name + "-reachable.gti", source,
                                       std::move(reachableOptions));
    if (!reachable.canGenerateCode())
      for (const auto &diagnostic : reachable.diagnostics)
        std::cerr << name << " (reachable): " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    expect(reachable.canGenerateCode(), name + " with artifact reachability");
    return result;
  };
  const auto negative = [&](const std::string &name, const std::string &source,
                            const std::string &code) {
    const auto result = application(name + ".gti", source);
    if (result.canGenerateCode() ||
        !hasDiagnosticCode(result.diagnostics, code))
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << name << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    expect(!result.canGenerateCode() &&
               hasDiagnosticCode(result.diagnostics, code) &&
               !hasDiagnosticCode(result.diagnostics, "GTI-B0001"),
           name + " is rejected by semantics, not the backend");
  };
  positive("shared-capture", R"(
int main() {
  int value = 7;
  auto read = [&value]() -> int { return value; };
  return read() - 7;
}
)");
  positive("exclusive-capture", R"(
int main() {
  mut int value = 7;
  mut auto increment = [mut &value]() -> int { value += 1; return value; };
  int first = increment();
  int second = increment();
  return first + second - 17;
}
)");
  positive("disjoint-captures", R"(
struct State { mut int left = 2; mut int right = 3; };
int main() {
  mut State state{};
  mut auto& left = state.left;
  mut auto& right = state.right;
  mut auto update = [mut &left, mut &right]() -> int {
    left += 1;
    right += 2;
    return left + right;
  };
  return update() - 8;
}
)");
  positive("confined-generic-capture", R"(
int invoke<F>(mut F& callback) { return callback(); }
int main() {
  mut int value = 3;
  mut auto update = [mut &value]() -> int { value += 2; return value; };
  return invoke(update) - 5;
}
)");
  positive("stored-generic-capture", R"(
class Handler<F> {
  mut F callback;
public:
  Handler(F callback) : callback(std::move(callback)) {}
  int run() mut { return this.callback(); }
};
Handler<F> store<F>(F callback) {
  return Handler<F>(std::move(callback));
}
int main() {
  mut int value = 3;
  mut auto update = [mut &value]() -> int { value += 2; return value; };
  mut auto handler = store(std::move(update));
  return handler.run() - 5;
}
)");
  positive("nested-shared-capture", R"(
int main() {
  int value = 3;
  auto read = [&value]() -> int { return value; };
  auto outer = [read]() -> int { return read(); };
  return outer() - 3;
}
)");
  positive("capture-last-use", R"(
int main() {
  mut int value = 3;
  auto read = [&value]() -> int { return value; };
  int previous = read();
  value = 4;
  return previous + value - 7;
}
)");
  positive("shared-reentrant-arguments", R"(
int invoke<F>(F& callback, int& value) {
  int result = callback();
  return result + value;
}
int main() {
  int value = 3;
  auto read = [&value]() -> int { return value; };
  return invoke(read, value) - 6;
}
)");
  positive("ordinary-reference-block-last-use", R"(
int main() {
  mut int owner = 3;
  mut int& reference = owner;
  { reference += 2; }
  owner += 1;
  return owner - 6;
}
)");
  negative("ordinary-reference-block-future-use", R"(
int main() {
  mut int owner = 3;
  mut int& reference = owner;
  { reference += 2; }
  owner += 1;
  return reference;
}
)",
           "GTI-S2017");
  negative("ordinary-reference-block-loop-backedge", R"(
int main() {
  mut int owner = 3;
  mut int& reference = owner;
  for (mut int i = 0; i < 2; i += 1) {
    { reference += 2; }
    owner += 1;
  }
  return 0;
}
)",
           "GTI-S2017");
  negative("exclusive-reentrant-arguments", R"(
int invoke<F>(mut F& callback, mut int& value) { value += 1; return callback(); }
int main() {
  mut int value = 3;
  mut auto update = [mut &value]() -> int { value += 1; return value; };
  return invoke(update, value);
}
)",
           "GTI-S2017");
  negative("borrow-then-move-capture", R"(
int main() {
  mut int value = 3;
  auto read = [&value, copied = std::move(value)]() -> int {
    return value + copied;
  };
  return read();
}
)",
           "GTI-S2017");
  negative("shared-capture-mutation", R"(
int main() {
  mut int value = 0;
  auto callback = [&value]() -> int { value += 1; return value; };
  return callback();
}
)",
           "GTI-S2002");
  positive("erased-reference-round-trip", R"(
#include <std/memory>
int main() {
  mut int owner = 3;
  mut std::erased_object& erased = std::erase_mut(owner);
  unsafe {
    mut int& restored = std::restore_mut<int>(erased);
    restored += 2;
  }
  return owner - 5;
}
)");
  positive("erased-reference-user-wrapper", R"(
#include <std/memory>
class Inspection {
  std::erased_object& object;
public:
  Inspection(int& object) : object(std::erase_ref(object)) {}
  int read() {
    unsafe { int& value = std::restore_ref<int>(this.object); return value; }
  }
};
int main() { int owner = 7; Inspection view{owner}; return view.read() - 7; }
)");
  positive("gti-invocation-pointer", R"(
#include <std/functional>
int increment(int value) { return value + 1; }
int main() {
  std::function_ptr<int(int)> first{increment};
  std::function_ptr<int(int)> second{[](int value) -> int { return value + 2; }};
  return first(3) + second(4) - 10;
}
)");
  positive("restore-through-ordinary-helper-chain", R"(
#include <std/memory>
struct Reader {
  int& value;
  Reader(int& source) : value(source) {}
  int read() { return this.value; }
};
int main() {
  int value = 9;
  Reader reader{value};
  std::erased_object& erased = std::erase_ref(reader);
  unsafe {
    Reader& restored = forward<Reader>(erased);
    return restored.read() - 9;
  }
}
unsafe T& forward<T>(std::erased_object& object) {
  unsafe {
    T& restored = restore<T>(object);
    return restored;
  }
}
unsafe T& restore<T>(std::erased_object& object) {
  unsafe { return std::restore_ref<T>(object); }
}
)");
  positive("read-function-ref", R"(
#include <std/functional>
int apply(std::function_ref<int(int)>& operation) { return operation(4); }
int main() {
  int offset = 3;
  auto callback = [&offset](int value) -> int { return offset + value; };
  std::function_ref<int(int)> view{callback};
  return apply(view) - 7;
}
)");
  positive("mutable-restoration-helper-alias", R"(
#include <std/memory>
struct Writer {
  mut int& value;
  Writer(mut int& source) : value(source) {}
  void increment() mut { this.value += 1; }
};
unsafe mut Writer& restore_writer(mut std::erased_object& object) {
  unsafe {
    mut Writer& restored = std::restore_mut<Writer>(object);
    return restored;
  }
}
int main() {
  mut int value = 4;
  mut Writer writer{value};
  mut std::erased_object& object = std::erase_mut(writer);
  unsafe {
    mut Writer& restored = restore_writer(object);
    restored.increment();
  }
  return value - 5;
}
)");
  negative("restoration-alias-cannot-return-local-owner", R"(
#include <std/memory>
struct Reader {
  int& value;
  Reader(int& source) : value(source) {}
};
Reader& bad(int& source) {
  Reader local{source};
  std::erased_object& object = std::erase_ref(local);
  unsafe {
    Reader& restored = std::restore_ref<Reader>(object);
    return restored;
  }
}
int main() { return 0; }
)",
           "GTI-S2017");
  negative("restoration-alias-cannot-upgrade-return-access", R"(
#include <std/memory>
struct Reader {
  int& value;
  Reader(int& source) : value(source) {}
};
unsafe mut Reader& bad(std::erased_object& object) {
  unsafe {
    Reader& restored = std::restore_ref<Reader>(object);
    return restored;
  }
}
int main() { return 0; }
)",
           "GTI-S2017");
  positive("mutable-function-ref", R"(
#include <std/functional>
int main() {
  mut int value = 3;
  mut auto callback = [mut &value](int amount) -> int { value += amount; return value; };
  mut std::function_ref<int(int) mut> view{callback};
  int first = view(2);
  int second = view(3);
  return first + second - 13;
}
)");
  positive("function-ref-preserves-independent-capture-origins", R"(
#include <std/functional>
int main() {
  mut int left = 2;
  mut int right = 3;
  int limit = 8;
  mut auto callback = [mut &left, mut &right, &limit]() -> int {
    left += 1;
    right += 2;
    return left + right + limit;
  };
  mut std::function_ref<int() mut> view{callback};
  int result = view();
  return result + left + right - 24;
}
)");
  negative("restore-requires-unsafe", R"(
#include <std/memory>
int main() {
  int owner = 3;
  std::erased_object& erased = std::erase_ref(owner);
  int& restored = std::restore_ref<int>(erased);
  return restored;
}
)",
           "GTI-S2055");
  negative("erased-referent-cannot-be-constructed", R"(
#include <std/memory>
int main() { std::erased_object forged{}; return 0; }
)",
           "GTI-S2019");
  negative("erased-private-factory-remains-private", R"(
#include <std/memory>
int main() {
  int owner = 3;
  auto& erased = gti_internal::erase_reference(owner);
  return 0;
}
)",
           "GTI-S2058");
  negative("erased-referent-cannot-be-a-base", R"(
#include <std/memory>
class Derived : public std::erased_object {};
int main() { Derived forged{}; return 0; }
)",
           "GTI-S2040");
  negative("invocation-pointer-has-no-default", R"(
#include <std/functional>
int main() { std::function_ptr<int()> pointer{}; return pointer(); }
)",
           "GTI-S2012");
  negative("erased-reference-owner-conflict", R"(
#include <std/memory>
int main() {
  mut int owner = 3;
  std::erased_object& erased = std::erase_ref(owner);
  owner = 5;
  unsafe { int& restored = std::restore_ref<int>(erased); return restored; }
}
)",
           "GTI-S2017");
  negative("erased-reference-retains-capture-origin", R"(
#include <std/memory>
int main() {
  mut int owner = 3;
  auto callback = [&owner]() -> int { return owner; };
  std::erased_object& erased = std::erase_ref(callback);
  owner = 5;
  unsafe { int& wrong = std::restore_ref<int>(erased); return wrong; }
}
)",
           "GTI-S2017");
  positive("erased-reference-local-shell-in-scope", R"(
#include <std/memory>
struct Reader {
  int& value;
  Reader(int& value) : value(value) {}
};
int read(int& value) {
  Reader local{value};
  std::erased_object& erased = std::erase_ref(local);
  unsafe { Reader& restored = std::restore_ref<Reader>(erased); return restored.value; }
}
int main() { int value = 3; return read(value) - 3; }
)");
  positive("shared-reference-value-carrier-return", R"(
struct Reader {
  int& value;
  Reader(int& value) : value(value) {}
};
Reader borrow(int& value) { Reader local{value}; return local; }
int main() { int value = 3; auto reader = borrow(value); return reader.value - 3; }
)");
  negative("erased-reference-cannot-escape-local-shell", R"(
#include <std/memory>
struct Reader {
  int& value;
  Reader(int& value) : value(value) {}
};
std::erased_object& bad(int& value) {
  Reader local{value};
  std::erased_object& erased = std::erase_ref(local);
  return erased;
}
int main() { return 0; }
)",
           "GTI-S2017");
  negative("erased-reference-is-not-layout-storage", R"(
#include <std/memory>
void require_layout<T>() requires std::layout_queryable<T> {}
int main() { require_layout<std::erased_object>(); return 0; }
)",
           "GTI-S2029");
  negative("function-ref-cannot-outlive-local-callable", R"(
#include <std/functional>
std::function_ref<int()> bad() {
  auto callback = []() -> int { return 1; };
  return std::function_ref<int()>(callback);
}
int main() { return 0; }
)",
           "GTI-S2045");
  negative("function-ref-retains-shared-capture-origin", R"(
#include <std/functional>
int main() {
  mut int owner = 3;
  auto callback = [&owner]() -> int { return owner; };
  std::function_ref<int()> view{callback};
  owner = 5;
  return view();
}
)",
           "GTI-S2017");
  negative("invocation-pointer-capturing-target", R"(
#include <std/functional>
int main() {
  int value = 3;
  auto callback = [value]() -> int { return value; };
  std::function_ptr<int()> pointer{callback};
  return pointer();
}
)",
           "GTI-S2093");
  negative("invocation-pointer-signature-mismatch", R"(
#include <std/functional>
int main() {
  std::function_ptr<int(int)> pointer{[](bool value) -> int { return value ? 1 : 0; }};
  return pointer(3);
}
)",
           "GTI-S2093");
  negative("invocation-pointer-borrowed-result", R"(
#include <std/functional>
int& identity(int& value) { return value; }
int main() {
  std::function_ptr<int&(int&)> pointer{identity};
  return 0;
}
)",
           "GTI-S2093");
  negative("invocation-pointer-owning-result", R"(
#include <std/functional>
#include <std/string>
std::string label(int value) { return std::string("label"); }
int main() {
  std::function_ptr<std::string(int)> pointer{label};
  return 0;
}
)",
           "GTI-S2093");
  negative("erased-reference-cannot-invent-mutable-access", R"(
#include <std/memory>
int main() {
  int owner = 3;
  std::erased_object& erased = std::erase_ref(owner);
  unsafe {
    mut int& restored = std::restore_mut<int>(erased);
    restored = 5;
  }
  return owner;
}
)",
           "GTI-S2017");
  negative("borrowed-callable-outlives-owner", R"(
#include <std/functional>
std::function_ref<int()> bad() {
  int owner = 3;
  auto callback = [&owner]() -> int { return owner; };
  return std::function_ref<int()>(callback);
}
int main() { auto escaped = bad(); return escaped(); }
)",
           "GTI-S2045");
  negative("exclusive-function-ref-conflicting-call", R"(
#include <std/functional>
int invoke(mut std::function_ref<int() mut>& callback, mut int& owner) {
  owner += 1;
  return callback();
}
int main() {
  mut int owner = 3;
  mut auto callback = [mut &owner]() -> int { owner += 1; return owner; };
  mut std::function_ref<int() mut> view{callback};
  return invoke(view, owner);
}
)",
           "GTI-S2017");
  negative("mutable-function-ref-copy", R"(
#include <std/functional>
int main() {
  mut int value = 3;
  mut auto callback = [mut &value]() -> int { value += 1; return value; };
  mut std::function_ref<int() mut> view{callback};
  mut auto copy = view;
  return copy();
}
)",
           "GTI-S2003");
  negative("exclusive-capture-readonly-source", R"(
int main() {
  int value = 0;
  mut auto callback = [mut &value]() -> int { return value; };
  return callback();
}
)",
           "GTI-S2017");
  negative("exclusive-capture-readonly-call", R"(
int main() {
  mut int value = 0;
  auto callback = [mut &value]() -> int { value += 1; return value; };
  return callback();
}
)",
           "GTI-S2046");
  negative("capture-owner-conflict", R"(
int main() {
  mut int value = 0;
  auto callback = [&value]() -> int { return value; };
  value = 4;
  return callback();
}
)",
           "GTI-S2017");
  negative("exclusive-capture-alias", R"(
int main() {
  mut int value = 0;
  mut auto& first = value;
  mut auto& second = value;
  mut auto callback = [mut &first, mut &second]() -> int { return first + second; };
  return callback();
}
)",
           "GTI-S2017");
  negative("exclusive-capture-copy", R"(
int main() {
  mut int value = 0;
  mut auto callback = [mut &value]() -> int { value += 1; return value; };
  mut auto copy = callback;
  return copy();
}
)",
           "GTI-S2003");
  return failures == 0 ? 0 : 1;
}
