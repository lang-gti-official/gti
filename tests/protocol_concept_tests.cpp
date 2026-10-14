#include "cpp_backend_test_support.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

// Protocol concepts: a concept with a body declares associated types and
// operations, and a type satisfies it only through a `for concept` block.
// Every rule is exercised through unrelated user types.
int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const std::string &message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source) {
    return lang::Frontend().analyze(root / "tests" / "protocol-concepts.gti",
                                    source, {root / "stdlib" / "prelude.gti"},
                                    {}, {root / "stdlib"});
  };
  const auto dump = [](const lang::FrontendResult &result) {
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  };
  // Accepted sources must reach codegen; `fragments` must appear in the C++.
  const auto accepted = [&](const std::string &source,
                            const std::string &message,
                            std::initializer_list<std::string> fragments = {}) {
    const auto result = analyze(source);
    expect(result.canGenerateCode(), message);
    if (!result.canGenerateCode()) {
      dump(result);
      return;
    }
    const std::string cpp = gti_test::emitCpp(result).contents;
    for (const std::string &fragment : fragments) {
      expect(cpp.find(fragment) != std::string::npos,
             message + " (emitted C++ lacks '" + fragment + "')");
    }
  };
  // The located owning diagnostic, its code and a fragment of its wording.
  const auto rejected = [&](const std::string &source, const char *code,
                            const std::string &fragment) {
    const auto result = analyze(source);
    const bool located = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &d) {
          const bool hinted = std::any_of(
              d.hints.begin(), d.hints.end(), [&](const std::string &hint) {
                return hint.find(fragment) != std::string::npos;
              });
          return d.code == code && !d.primary.source.empty() &&
                 d.primary.end > d.primary.start &&
                 (d.message.find(fragment) != std::string::npos || hinted);
        });
    if (!located)
      dump(result);
    expect(!result.canGenerateCode(),
           "invalid source must not reach codegen: " + fragment);
    expect(located,
           std::string("missing ") + code + " with '" + fragment + "'");
  };

  const std::string measurable = R"gti(
concept measurable<T> {
  typename unit_type;
  unit_type measure(T& item);
  bool is_empty(T& item);
};
struct Meters {
  int value;
  Meters(int amount) : value(amount) {}
};
class Rope {
public:
  int length;
  Rope(int amount) : length(amount) {}
};
class Crate<T> {
public:
  T item;
  int count;
  Crate(T stored, int amount) : item(stored), count(amount) {}
};
)gti";
  const std::string blocks = R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure(Rope& item) { return Meters(item.length); }
  bool is_empty(Rope& item) { return item.length == 0; }
};
for <T> concept measurable<Crate<T>> {
  using unit_type = int;
  unit_type measure(Crate<T>& item) { return item.count; }
  bool is_empty(Crate<T>& item) {
    measurable<Crate<T>>::unit_type count = measure(item);
    return count == 0;
  }
};
)gti";

  // Concrete use: projections reduce to the block's types and operations
  // call the block's functions.
  accepted(measurable + blocks + R"gti(
int main() {
  Rope rope{12};
  measurable<Rope>::unit_type length = measurable<Rope>::measure(rope);
  Crate<bool> crate{true, 30};
  int count = measurable<Crate<bool>>::measure(crate);
  return length.value + count - 42;
}
)gti",
           "concrete and generic blocks should serve concrete projections "
           "and operation calls",
           {"__gti_conformance_"});

  // Generic code: a projection in the signature, calls through the
  // requirement, and forwarding the requirement to another generic.
  accepted(measurable + blocks + R"gti(
measurable<T>::unit_type total<T>(T& item) requires measurable<T> {
  if (measurable<T>::is_empty(item)) {
    return measurable<T>::measure(item);
  }
  measurable<T>::unit_type result = measurable<T>::measure(item);
  return result;
}
measurable<T>::unit_type twice<T>(T& item) requires measurable<T> {
  return total(item);
}
int main() {
  Rope rope{12};
  Crate<bool> crate{true, 30};
  Meters length = twice(rope);
  int count = total(crate);
  return length.value + count - 42;
}
)gti",
           "a generic function should use associated types and operations "
           "through its requires clause");

  // A requirement inside a conjunction still proves the application.
  accepted(measurable + blocks + R"gti(
measurable<T>::unit_type first<T>(T& item)
  requires std::copyable<T> && measurable<T> {
  return measurable<T>::measure(item);
}
int main() {
  Crate<int> crate{1, 7};
  return first(crate) - 7;
}
)gti",
           "a conjunct of the requires clause should prove the application");

  // An associated type may be a block parameter, and patterns may bind
  // several parameters in any order.
  accepted(R"gti(
concept keyed<T> {
  typename key_type;
  typename value_type;
  key_type key(T& entry);
  value_type value(T& entry);
};
class Entry<A, B> {
public:
  A first;
  B second;
  Entry(A one, B two) : first(one), second(two) {}
};
for <V, K> concept keyed<Entry<K, V>> {
  using key_type = K;
  using value_type = V;
  K key(Entry<K, V>& entry) { return entry.first; }
  V value(Entry<K, V>& entry) { return entry.second; }
};
keyed<T>::value_type lookup<T>(T& entry) requires keyed<T> {
  return keyed<T>::value(entry);
}
int main() {
  Entry<int, bool> entry{4, true};
  keyed<Entry<int, bool>>::key_type key = keyed<Entry<int, bool>>::key(entry);
  bool value = lookup(entry);
  return value ? key - 4 : 1;
}
)gti",
           "parameters bound in any order should reach associated types");

  // Concepts and blocks in namespaces, and a block in another namespace of
  // the same package.
  accepted(R"gti(
namespace shapes {
concept sized<T> {
  typename size_type;
  size_type size(T& value);
};
class Square {
public:
  int side;
  Square(int length) : side(length) {}
};
}
namespace adapters {
for concept shapes::sized<shapes::Square> {
  using size_type = int;
  int size(shapes::Square& value) { return value.side * value.side; }
};
}
int main() {
  shapes::Square square{3};
  shapes::sized<shapes::Square>::size_type area =
      shapes::sized<shapes::Square>::size(square);
  return area - 9;
}
)gti",
           "qualified concepts and blocks in other namespaces should work");

  // A protocol concept composes into an ordinary concept for viability.
  accepted(measurable + blocks + R"gti(
concept measured_copy<T> = measurable<T> && std::copyable<T>;
int ok<T>(T& item) requires measured_copy<T> { return 1; }
int main() {
  Crate<int> crate{1, 2};
  return ok(crate) - 1;
}
)gti",
           "a composed concept should check the block for a concrete type");

  // Declarations.
  rejected(R"gti(
concept pairish<A, B> {
  typename first;
};
int main() { return 0; }
)gti",
           "GTI-S2123", "exactly one type parameter");
  rejected(R"gti(
concept dup<T> {
  typename item;
  int item(T& value);
};
int main() { return 0; }
)gti",
           "GTI-S2123", "Duplicate member name 'item'");
  rejected(R"gti(
concept modifiers<T> {
  static int count(T& value);
};
int main() { return 0; }
)gti",
           "GTI-S2123", "takes no modifiers");
  rejected(R"gti(
concept generic_operation<T> {
  int convert<U>(T& value);
};
int main() { return 0; }
)gti",
           "GTI-S2123", "cannot declare generic parameters");
  rejected(R"gti(
concept defaults<T> {
  int pick(T& value, int fallback = 1);
};
int main() { return 0; }
)gti",
           "GTI-S2123", "cannot declare a parameter pack or default");

  // Blocks.
  rejected(measurable + R"gti(
for concept missing<Rope> {
};
int main() { return 0; }
)gti",
           "GTI-S2123", "Unknown or invisible concept 'missing'");
  rejected(measurable + R"gti(
concept plain<T> = requires(T& value) { { value.size() } -> int; };
for concept plain<Rope> {
};
int main() { return 0; }
)gti",
           "GTI-S2123", "has no body");
  rejected(measurable + R"gti(
for <T, U> concept measurable<Crate<T>> {
  using unit_type = int;
  int measure(Crate<T>& item) { return 0; }
  bool is_empty(Crate<T>& item) { return true; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "pattern applies one class template");
  rejected(measurable + R"gti(
for <T> concept measurable<Crate<Crate<T>>> {
  using unit_type = int;
  int measure(Crate<Crate<T>>& item) { return 0; }
  bool is_empty(Crate<Crate<T>>& item) { return true; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "pattern applies one class template");
  rejected(measurable + blocks + R"gti(
for concept measurable<Crate<int>> {
  using unit_type = int;
  int measure(Crate<int>& item) { return 1; }
  bool is_empty(Crate<int>& item) { return false; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "overlaps another");
  rejected(measurable + blocks + R"gti(
for concept measurable<Rope> {
  using unit_type = int;
  int measure(Rope& item) { return 1; }
  bool is_empty(Rope& item) { return false; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "overlaps another");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  Meters measure(Rope& item) { return Meters(item.length); }
  bool is_empty(Rope& item) { return false; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "does not define associated type 'unit_type'");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  using extra = int;
  Meters measure(Rope& item) { return Meters(item.length); }
  bool is_empty(Rope& item) { return false; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "'extra' is not an associated type");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure(Rope& item) { return Meters(item.length); }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "does not define operation 'is_empty'");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure(Rope& item) { return Meters(item.length); }
  bool is_empty(Rope& item) { return false; }
  void polish(Rope& item) {}
};
int main() { return 0; }
)gti",
           "GTI-S2123", "'polish' is not an operation");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure(Rope& item) { return Meters(item.length); }
  int is_empty(Rope& item) { return 0; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "must have the signature 'bool is_empty(Rope&)'");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure(Rope item) { return Meters(item.length); }
  bool is_empty(Rope& item) { return false; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "must have the signature");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure(Rope& item) requires std::copyable<Rope> {
    return Meters(item.length);
  }
  bool is_empty(Rope& item) { return false; }
};
int main() { return 0; }
)gti",
           "GTI-P0001", "cannot declare a requires clause");
  rejected(measurable + R"gti(
for concept measurable<Rope> {
  using unit_type = Meters;
  Meters measure<U>(Rope& item) { return Meters(item.length); }
  bool is_empty(Rope& item) { return false; }
};
int main() { return 0; }
)gti",
           "GTI-P0001", "cannot declare its own");
  rejected(measurable + R"gti(
class Holder {
  for concept measurable<Rope> {
  };
};
int main() { return 0; }
)gti",
           "GTI-P0001", "limited to namespace scope");

  // Uses.
  rejected(measurable + blocks + R"gti(
measurable<T>::unit_type unconstrained<T>(T& item) { return 0; }
int main() { return 0; }
)gti",
           "GTI-S2123", "requires 'requires measurable<T>'");
  rejected(measurable + blocks + R"gti(
int call<T>(T& item) {
  measurable<T>::is_empty(item);
  return 0;
}
int main() { return 0; }
)gti",
           "GTI-S2123", "Calling 'measurable<T>::is_empty' requires");
  rejected(measurable + blocks + R"gti(
int sized<T>(T& item) requires measurable<T> { return 0; }
class Plank {};
int main() {
  Plank plank{};
  return sized(plank);
}
)gti",
           "GTI-S2029", "has a body, so a type satisfies it only");
  rejected(measurable + R"gti(
int main() {
  Rope rope{1};
  return measurable<Rope>::measure(rope).value;
}
)gti",
           "GTI-S2123", "No conformance block 'for concept measurable<Rope>'");
  rejected(measurable + R"gti(
int main() {
  measurable<Rope>::unit_type length = Meters(1);
  return 0;
}
)gti",
           "GTI-S2123", "No conformance block");
  rejected(measurable + blocks + R"gti(
int main() {
  measurable<Rope>::weight_type weight = 1;
  return 0;
}
)gti",
           "GTI-S2123", "has no associated type 'weight_type'");
  rejected(measurable + blocks + R"gti(
int main() {
  Rope rope{1};
  measurable<Rope>::polish(rope);
  return 0;
}
)gti",
           "GTI-S2123", "has no operation 'polish'");
  rejected(measurable + blocks + R"gti(
int main() {
  Rope rope{1};
  measurable<Rope>::measure<int>(rope);
  return 0;
}
)gti",
           "GTI-S2123", "takes no explicit generic arguments");
  rejected(measurable + blocks + R"gti(
int inline_form<measurable T>(T& item) { return 0; }
int main() { return 0; }
)gti",
           "GTI-S2029", "trailing requires clause");
  rejected(measurable + blocks + R"gti(
int main() {
  auto operation = measurable<Rope>::measure;
  return 0;
}
)gti",
           "GTI-S2123", "can only be called");
  rejected(measurable + blocks + R"gti(
int main() {
  Rope rope{1};
  int wrong = measurable<Rope>::measure(rope);
  return 0;
}
)gti",
           "GTI-S2003", "Cannot initialize 'wrong'");

  // The outline shows each block, with its members, in place of the
  // generated namespace that holds its functions.
  {
    const auto result =
        analyze(measurable + blocks + "int main() { return 0; }\n");
    const auto symbols = lang::LanguageQueries().documentSymbols(
        result, result.sourceGraph.entryUnit());
    const auto named = [&](const std::string &name) {
      return std::find_if(
          symbols.begin(), symbols.end(),
          [&](const auto &symbol) { return symbol.name == name; });
    };
    const auto rope = named("for concept measurable<Rope>");
    const auto crate = named("for concept measurable<Crate<T>>");
    expect(rope != symbols.end() && crate != symbols.end() &&
               rope->children.size() == 3 &&
               rope->children[0].name == "unit_type" &&
               rope->children[1].name == "measure" &&
               rope->children[2].name == "is_empty",
           "document symbols should outline conformance blocks and their "
           "members");
    expect(std::none_of(symbols.begin(), symbols.end(),
                        [](const auto &symbol) {
                          return symbol.name.find("__gti_") !=
                                 std::string::npos;
                        }),
           "the generated namespace must not appear in the outline");
  }

  // Formatting keeps `for <T> concept` and treats block parameters and
  // associated names as types.
  const std::string formatted = R"gti(concept measurable<T> {
  typename unit_type;
  unit_type measure(T& item);
  bool is_empty(T& item);
};

class Crate<T> {
public:
  int count;
  Crate(int amount) : count(amount) {}
};

for <T> concept measurable<Crate<T>> {
  using unit_type = int;
  unit_type measure(Crate<T>& item) {
    return item.count;
  }
  bool is_empty(Crate<T>& item) {
    measurable<Crate<T>>::unit_type count = measure(item);
    return count == 0;
  }
};
)gti";
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted protocol concepts and blocks should be stable");
  expect(lang::Formatter().format(
             "class Crate<T>{public:int count;};\n"
             "for<T>concept measurable<Crate<T>>{using unit_type=int;"
             "unit_type measure(Crate<T>&item){return item.count;}};\n"
             "concept   measurable<T>{typename   unit_type;"
             "unit_type measure(T&item);};\n") ==
             "class Crate<T> {\npublic:\n  int count;\n};\n"
             "for <T> concept measurable<Crate<T>> {\n"
             "  using unit_type = int;\n"
             "  unit_type measure(Crate<T>& item) {\n"
             "    return item.count;\n  }\n};\n"
             "concept measurable<T> {\n  typename unit_type;\n"
             "  unit_type measure(T& item);\n};\n",
         "protocol concepts and blocks should normalize to the canonical "
         "layout");

  if (failures != 0) {
    std::cerr << failures << " protocol concept checks failed\n";
    return 1;
  }
  std::cout << "protocol concept tests passed\n";
  return 0;
}
