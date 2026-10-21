#include "cpp_backend_test_support.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace lang {
struct SemanticModelTestAccess {
  static void record(SemanticModel &model, const Expr &expression,
                     std::vector<SemanticLoanDependency> dependencies) {
    model.recordBorrowDependencies(expression, std::move(dependencies));
  }
  static void complete(SemanticModel &model, const Expr &expression,
                       SemanticBorrowDependencySet dependencies) {
    model.recordBorrowDependencySet(expression, std::move(dependencies));
  }
  static void delta(SemanticModel &model, const SemanticModel &base) {
    model.beginInstanceDelta(base);
  }
  static void rebase(SemanticModel &model, const SemanticModel &base) {
    model.rebase(&base);
  }
  static void clear(SemanticModel &model) { model.clear(); }
};
struct MirProgramTestAccess {
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
};
} // namespace lang

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
  {
    lang::LiteralExpr expression(std::uint64_t{0});
    lang::SemanticModel base;
    expect(base.findBorrowDependencySet(expression) == nullptr,
           "unproved dependencies must be absent, not complete-empty");
    lang::SemanticModelTestAccess::record(base, expression,
                                          {{.place = {.root = 1}}});
    lang::SemanticModel overlay;
    lang::SemanticModelTestAccess::delta(overlay, base);
    expect(
        overlay.findBorrowDependencySet(expression) != nullptr &&
            overlay.findBorrowDependencySet(expression)->dependencies.size() ==
                1,
        "an instance inherits the exact complete dependency set");
    const lang::SemanticBorrowDependencySet emptyAlternative{
        .possibleAlternatives = {
            {.conditions = {{.enumeration = lang::SemanticType::enumType(1),
                             .variant = 2}}}}};
    lang::SemanticModelTestAccess::complete(overlay, expression,
                                            emptyAlternative);
    expect(
        overlay.findBorrowDependencySet(expression) != nullptr &&
            overlay.findBorrowDependencySet(expression)->dependencies.empty(),
        "complete-empty overrides an inherited nonempty set");
    lang::SemanticModel relocated;
    lang::SemanticModelTestAccess::record(relocated, expression,
                                          {{.place = {.root = 2}}});
    lang::SemanticModelTestAccess::rebase(overlay, relocated);
    expect(overlay.findBorrowDependencySet(expression) != nullptr &&
               *overlay.findBorrowDependencySet(expression) == emptyAlternative,
           "rebase preserves explicitly empty instance evidence");
    lang::SemanticModelTestAccess::clear(overlay);
    expect(overlay.findBorrowDependencySet(expression) == nullptr,
           "clearing semantic evidence removes complete-set presence");
  }
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
  // Exactly one `code` diagnostic spans `token`, written right after
  // `before`. A block's syntax that its operations restate must not report
  // again for each copy.
  const auto reportedOnce = [&](const std::string &source, const char *code,
                                const std::string &before,
                                const std::string &token) {
    const auto result = analyze(source);
    const std::size_t found = source.find(before + token);
    expect(found != std::string::npos, "no '" + before + token + "' in source");
    const std::size_t start = found + before.size();
    const auto count = std::count_if(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &d) {
          return d.code == code &&
                 d.primary.source.ends_with("protocol-concepts.gti") &&
                 d.primary.start == start &&
                 d.primary.end == start + token.size();
        });
    if (count != 1)
      dump(result);
    expect(!result.canGenerateCode(),
           "invalid source must not reach codegen: " + before + token);
    expect(count == 1, std::string(code) + " at '" + before + token +
                           "' reported " + std::to_string(count) +
                           " times; expected once");
  };

  const std::string conditionalContract = R"gti(
concept selecting<T> {
  typename element;
  [[borrowed_from(source)]] element choose(element source, element other);
};
struct Marker<T> { int tag; };
for <T> concept selecting<Marker<T>> {
  using element = T;
  T choose(T source, T other) { return source; }
};
)gti";
  accepted(conditionalContract + R"gti(
int main() { return selecting<Marker<int>>::choose(7, 3) - 7; }
)gti",
           "dependent owned protocol results need no borrowed_from annotation");
  rejected(conditionalContract + R"gti(
int main() {
  int first = 7;
  int second = 3;
  return selecting<Marker<int&>>::choose(first, second) - 7;
}
)gti",
           "GTI-S2123", "concrete reference result");
  rejected(conditionalContract + R"gti(
int evaluate<T>(T marker) requires selecting<T> { return 0; }
int main() {
  Marker<int&> marker{0};
  return evaluate(marker);
}
)gti",
           "GTI-S2029", "borrowed_from parameter contract");
  std::string substitutedContract = conditionalContract;
  substitutedContract.replace(substitutedContract.find("  T choose(T source"),
                              std::string("  T choose").size(),
                              "  [[borrowed_from(other)]] T choose");
  rejected(substitutedContract + R"gti(
int main() {
  int first = 7;
  int second = 3;
  return selecting<Marker<int&>>::choose(first, second) - 7;
}
)gti",
           "GTI-S2123", "concrete reference result");

  const std::string borrowed = R"gti(
concept observed<T> {
  typename element;
  [[borrowed_from(source)]] element get(T source);
};
class Reading {
public:
  int& external;
  Reading(int& owner) : external(owner) {}
};
for concept observed<Reading> {
  using element = int&;
  [[borrowed_from(value)]] int& get(Reading value) {
    return value.external;
  }
};
)gti";
  accepted(borrowed + R"gti(
int main() {
  int owner = 7;
  Reading view{owner};
  int& result = observed<Reading>::get(view);
  return result - 7;
}
)gti",
           "protocol result origins relay a by-value carrier's external owner");
  const std::string borrowedRelay = borrowed + R"gti(
[[borrowed_from(value)]]
observed<T>::element relay<T>(T value) requires observed<T> {
  return observed<T>::get(value);
}
int main() {
  int owner = 7;
  Reading view{owner};
  int& result = relay(view);
  return result - 7;
}
)gti";
  accepted(borrowedRelay,
           "generic protocol calls preserve the declared result origin");
  rejected(borrowed + R"gti(
[[borrowed_from(owner)]] int& escape(int& owner) {
  int local = 3;
  Reading view{local};
  return observed<Reading>::get(view);
}
int main() { return 0; }
)gti",
           "GTI-S2017", "Returned reference");
  const auto relayProof = analyze(borrowedRelay);
  if (relayProof.canGenerateCode()) {
    for (int corruption = 0; corruption != 5; ++corruption) {
      auto malformed = relayProof.mir;
      bool changed = false;
      for (auto &function : lang::MirProgramTestAccess::functions(malformed)) {
        if (function.returnType.kind != lang::SemanticType::Reference ||
            function.returnBorrowOrigin != lang::BorrowOriginKind::Argument)
          continue;
        for (auto &block : function.body.blocks)
          for (auto &instruction : block.instructions) {
            if (changed || instruction.kind != lang::MirInstructionKind::Call ||
                !instruction.loan)
              continue;
            auto &loan = function.body.loans[*instruction.loan - 1];
            if (loan.kind != lang::MirLoanKind::CallResult)
              continue;
            if (corruption == 0)
              loan.parent = 0;
            if (corruption == 1)
              loan.source = 0;
            if (corruption == 2)
              loan.access = lang::AccessMode::Mutable;
            if (corruption == 3)
              loan.producedBy = 0;
            if (corruption == 4)
              instruction.borrowOrigin = lang::BorrowOriginKind::None;
            changed = true;
          }
      }
      expect(changed, "forwarding mutation must select the exact call chain");
      expect(!changed || !lang::verifyMirProgram(malformed).valid(),
             "MIR must reject substituted origin, permission or producer in "
             "a returned-reference forwarding chain");
    }
  }
  const std::string mutableAssociated = R"gti(
concept writable<T> { typename element; };
for concept writable<int> { using element = mut int&; };
int main() { return 0; }
)gti";
  accepted(mutableAssociated,
           "associated types preserve explicit mutable reference access");
  accepted(R"gti(
concept editable<T> {
  typename element;
  [[borrowed_from(value)]] element get(T value);
};
class Editing {
public:
  mut int& value;
  Editing(mut int& owner) : value(owner) {}
};
for concept editable<Editing> {
  using element = mut int&;
  [[borrowed_from(value)]] mut int& get(mut Editing value) {
    return value.value;
  }
};
[[borrowed_from(value)]] editable<T>::element relay<T>(mut T value)
    requires editable<T> {
  return editable<T>::get(std::move(value));
}
int main() {
  mut int owner = 4;
  Editing view{owner};
  mut int& value = relay(std::move(view));
  value = 8;
  return value - 8;
}
)gti",
           "generic protocol return projections preserve exclusive access");
  const auto formattedMutable = lang::Formatter().format(mutableAssociated);
  accepted(formattedMutable,
           "formatting preserves mutable reference associated types");
  expect(lang::Formatter().format(formattedMutable) == formattedMutable,
         "mutable associated type formatting is idempotent");
  rejected(R"gti(
concept writable<T> { typename element; };
for concept writable<int> { using element = mut int; };
int main() { return 0; }
)gti",
           "GTI-S2030", "requires a checked reference");
  rejected(R"gti(
concept observed<T> {
  [[borrowed_from(missing)]] int& get(T& source);
};
int main() { return 0; }
)gti",
           "GTI-S2123", "must name one of its parameters");
  rejected(R"gti(
concept observed<T> {
  [[borrowed_from(source)]] int& get(T& source, T& other);
};
struct Reading { int value; };
for concept observed<Reading> {
  [[borrowed_from(other)]] int& get(Reading& source, Reading& other) {
    return other.value;
  }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "must preserve the concept's borrowed_from");
  rejected(R"gti(
class Reading {
public:
  int& external;
  int owned;
  Reading(int& owner) : external(owner), owned(3) {}
};
[[borrowed_from(value)]] int& invalid(Reading value) {
  return value.owned;
}
int main() { return 0; }
)gti",
           "GTI-S2085", "by-value carrier shell");
  rejected(R"gti(
class Reading {
public:
  int& external;
  Reading(int& owner) : external(owner) {}
};
[[borrowed_from(first)]] int& invalid(Reading first, Reading second) {
  return second.external;
}
int main() { return 0; }
)gti",
           "GTI-S2085", "exact external owner dependency");
  rejected(borrowed + R"gti(
int& invalid() {
  int local = 7;
  return observed<Reading>::get(Reading(local));
}
int main() { return 0; }
)gti",
           "GTI-S2017", "return");

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

  // A block owns its header, including when there are no generated operations.
  for (int operations = 0; operations <= 2; ++operations) {
    const auto members = [&](const std::string &type) {
      std::string text;
      if (operations > 0)
        text += "int measure(" + type + "& item) { return 1; }\n";
      if (operations > 1)
        text += "bool is_empty(" + type + "& item) { return false; }\n";
      return text;
    };
    const auto oneHeaderError = [&](const std::string &header,
                                    const std::string &type,
                                    const std::string &token,
                                    const std::string &code) {
      const std::string source = measurable + header + " {\n" +
                                 "using unit_type = int;\n" + members(type) +
                                 "};\nint main() { return 0; }\n";
      const auto result = analyze(source);
      const std::size_t start = source.find(token, measurable.size());
      const auto count =
          std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                        [&](const auto &diagnostic) {
                          return diagnostic.primary.start == start &&
                                 diagnostic.primary.end == start + token.size();
                        });
      expect(!result.canGenerateCode() && count == 1,
             "one header diagnostic regardless of operation count: " + header);
      expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                         [&](const auto &diagnostic) {
                           return diagnostic.primary.start == start &&
                                  diagnostic.code == code;
                         }),
             "the header retains its owning diagnostic: " + header);
      if (count != 1)
        dump(result);
    };
    oneHeaderError("for concept missing<Rope>", "Rope", "missing", "GTI-S2123");
    oneHeaderError("for concept measurable<Rope, Rope>", "Rope", "measurable",
                   "GTI-S2123");
    oneHeaderError("for <unknown_constraint T> concept measurable<Crate<T>>",
                   "Crate<T>", "unknown_constraint", "GTI-S2029");
    oneHeaderError("for concept gti_internal::missing<Rope>", "Rope",
                   "gti_internal", "GTI-S2058");

    const std::string source = "#include \"middle.gti\"\nstruct Rope {};\n"
                               "for concept measurable<Rope> {\n" +
                               members("Rope") +
                               "};\nint main() { return 0; }\n";
    const auto entry = root / "tests" / "protocol-concepts.gti";
    const auto conceptPath = root / "tests" / "measure.gti";
    const auto invisible = lang::Frontend().analyze(
        entry, source, {root / "stdlib" / "prelude.gti"},
        {{(root / "tests" / "middle.gti").string(),
          "#include \"measure.gti\"\n"},
         {conceptPath.string(), "concept measurable<T> { int measure(T& item); "
                                "bool is_empty(T& item); };\n"}},
        {root / "stdlib"});
    const auto diagnostic =
        std::find_if(invisible.diagnostics.begin(), invisible.diagnostics.end(),
                     [](const auto &item) { return item.code == "GTI-S2024"; });
    expect(!invisible.canGenerateCode() &&
               std::count_if(invisible.diagnostics.begin(),
                             invisible.diagnostics.end(),
                             [](const auto &item) {
                               return item.code == "GTI-S2024" ||
                                      item.code == "GTI-S2123" ||
                                      item.code == "GTI-S2049";
                             }) == 1,
           "an invisible concept reports once from its block");
    expect(diagnostic != invisible.diagnostics.end() &&
               diagnostic->primary.start == source.find("measurable") &&
               diagnostic->missingIncludeName == "measurable" &&
               std::any_of(diagnostic->related.begin(),
                           diagnostic->related.end(),
                           [&](const auto &related) {
                             return related.span.source == conceptPath.string();
                           }) &&
               std::any_of(diagnostic->hints.begin(), diagnostic->hints.end(),
                           [](const auto &hint) {
                             return hint.find("#include \"measure.gti\"") !=
                                    std::string::npos;
                           }),
           "an invisible concept retains declaration and include help even "
           "without operations");
  }
  rejected(measurable + R"gti(
for <T> concept measurable<Crate<T>> {
  using unit_type = int;
  int T(Crate<T>& item) { return 1; }
};
)gti",
           "GTI-S2000", "Generic parameter cannot have the same name");

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

  // A structural family conformance proves its open applications without
  // asking each caller to repeat an already established requirement.
  accepted(measurable + blocks + R"gti(
measurable<Crate<T>>::unit_type family_count<T>(Crate<T>& item) {
  return measurable<Crate<T>>::measure(item);
}
int main() {
  Crate<int> item{1, 42};
  return family_count(item) - 42;
}
)gti",
           "open family heads should establish projections and operations");
  rejected(measurable + R"gti(
for concept measurable<Crate<int>> {
  using unit_type = int;
  int measure(Crate<int>& item) { return item.count; }
  bool is_empty(Crate<int>& item) { return item.count == 0; }
};
measurable<Crate<T>>::unit_type unsupported<T>(Crate<T>& item) {
  return measurable<Crate<T>>::measure(item);
}
)gti",
           "GTI-S2123", "requires 'requires measurable<Crate<T>>'");

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

  // Several parameters: each block argument is a closed type or one class
  // template over block parameters; a parameter may repeat, and every one
  // of its occurrences must bind the same type.
  const std::string adds = R"gti(
concept adds_to<Left, Right> {
  typename sum_type;
  sum_type add(Left& left, Right& right);
};
struct Meters {
  int value;
  Meters(int amount) : value(amount) {}
};
struct Centimeters {
  int value;
  Centimeters(int amount) : value(amount) {}
};
class Tally<T> {
public:
  int count;
  Tally(int amount) : count(amount) {}
};
class Pair<A, B> {
public:
  A first;
  B second;
  Pair(A one, B two) : first(one), second(two) {}
};
for concept adds_to<Meters, Centimeters> {
  using sum_type = Centimeters;
  Centimeters add(Meters& left, Centimeters& right) {
    return Centimeters(left.value * 100 + right.value);
  }
};
for <T> concept adds_to<Tally<T>, Tally<T>> {
  using sum_type = Tally<T>;
  Tally<T> add(Tally<T>& left, Tally<T>& right) {
    adds_to<Tally<T>, Tally<T>>::sum_type sum = Tally<T>(left.count);
    return Tally<T>(sum.count + right.count);
  }
};
for <T> concept adds_to<Tally<T>, int> {
  using sum_type = Tally<T>;
  Tally<T> add(Tally<T>& left, int& right) {
    return Tally<T>(left.count + right);
  }
};
for <V, std::numeric K> concept adds_to<Pair<K, V>, Pair<K, K>> {
  using sum_type = K;
  K add(Pair<K, V>& left, Pair<K, K>& right) {
    return left.first + right.first + right.second;
  }
};
)gti";
  accepted(adds + R"gti(
adds_to<L, R>::sum_type total<L, R>(L& left, R& right)
  requires adds_to<L, R> {
  return adds_to<L, R>::add(left, right);
}
struct Apples {};
int main() {
  Meters length{2};
  Centimeters extra{35};
  Centimeters sum = total(length, extra);
  Tally<Apples> morning{3};
  Tally<Apples> evening{4};
  Tally<Apples> day = total(morning, evening);
  int more = 5;
  adds_to<Tally<Apples>, int>::sum_type later =
      adds_to<Tally<Apples>, int>::add(day, more);
  Pair<int, bool> left{1, true};
  Pair<int, int> right{2, 3};
  int keys = total(left, right);
  return sum.value + later.count + keys - 253;
}
)gti",
           "concepts with several parameters should select blocks by every "
           "argument, including repeated parameters");
  // The same kind on both sides is required by the repeated parameter.
  rejected(adds + R"gti(
struct Apples {};
struct Pears {};
int main() {
  Tally<Apples> apples{1};
  Tally<Pears> pears{2};
  adds_to<Tally<Apples>, Tally<Pears>>::add(apples, pears);
  return 0;
}
)gti",
           "GTI-S2123",
           "No conformance block 'for concept adds_to<Tally<Apples>, "
           "Tally<Pears>>'");
  rejected(adds + R"gti(
int main() {
  adds_to<Meters>::sum_type sum = Centimeters(1);
  return 0;
}
)gti",
           "GTI-S2123", "takes 2 type arguments");
  rejected(adds + R"gti(
for concept adds_to<Centimeters> {
  using sum_type = int;
};
int main() { return 0; }
)gti",
           "GTI-S2123", "to 2 types");
  rejected(adds + R"gti(
for <T> concept adds_to<T, Meters> {
  using sum_type = Meters;
  Meters add(T& left, Meters& right) { return Meters(right.value); }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "Each argument of a conformance block");
  rejected(adds + R"gti(
for concept adds_to<Tally<bool>, Tally<bool>> {
  using sum_type = int;
  int add(Tally<bool>& left, Tally<bool>& right) { return 0; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "overlaps another");
  rejected(adds + R"gti(
for <A, B> concept adds_to<Pair<A, B>, Pair<B, B>> {
  using sum_type = int;
  int add(Pair<A, B>& left, Pair<B, B>& right) { return 0; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "overlaps another");
  // Unifying the repeated parameter with two different closed types fails,
  // so these blocks are disjoint.
  accepted(adds + R"gti(
for concept adds_to<Pair<int, bool>, Pair<int, int64_t>> {
  using sum_type = int;
  int add(Pair<int, bool>& left, Pair<int, int64_t>& right) { return 1; }
};
int main() { return 0; }
)gti",
           "blocks whose repeated parameter cannot unify should not overlap");
  rejected(adds + R"gti(
int unconstrained<L, R>(L& left, R& right) requires adds_to<R, L> {
  adds_to<L, R>::add(left, right);
  return 0;
}
int main() { return 0; }
)gti",
           "GTI-S2123", "requires 'requires adds_to<L, R>'");
  // Generic checks spell an associated type through its concept.
  rejected(R"gti(
concept scaled<From, To> {
  typename factor_type;
  To scale(From& value, factor_type factor);
};
int apply<A, B>(A& value) requires scaled<A, B> {
  scaled<A, B>::scale(value, 5);
  return 0;
}
int main() { return 0; }
)gti",
           "GTI-S2005", "expects 'scaled<A, B>::factor_type'");
  // An enum template is a pattern like a class template: its instances
  // select the block, and a concrete instance overlaps it.
  const std::string slot = R"gti(
concept sized<T> {
  typename count_type;
  count_type count(T& value);
};
enum class Slot<T> {
  empty,
  full(T value),
};
for <T> concept sized<Slot<T>> {
  using count_type = int;
  int count(Slot<T>& value) {
    switch (value) {
    case Slot<T>::full:
      return 1;
    case Slot<T>::empty:
      return 0;
    }
  }
};
)gti";
  accepted(slot + R"gti(
sized<Slot<int>>::count_type total(Slot<int>& value) {
  return sized<Slot<int>>::count(value);
}
sized<T>::count_type counted<T>(T& value) requires sized<T> {
  return sized<T>::count(value);
}
int main() {
  Slot<int> full = Slot<int>::full(3);
  Slot<bool> empty = Slot<bool>::empty;
  return total(full) + counted(empty) - 1;
}
)gti",
           "an enum-template block should serve its instances");
  rejected(slot + R"gti(
for concept sized<Slot<int>> {
  using count_type = int;
  int count(Slot<int>& value) { return 2; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "overlaps another");

  // A source `expected`, a raw pointer and a fixed-length array are patterns
  // like a class template. A pointer pattern keeps its access, and an array
  // pattern its length, so `T*`, `const T*`, `T[3]` and `T[4]` are four
  // patterns.
  const std::string builtin = R"gti(
concept sized<T> {
  typename count_type;
  count_type count(T& value);
};
concept counted<T> {
  int count(T value);
};
for <T, E> concept sized<expected<T, E>> {
  using count_type = bool;
  bool count(expected<T, E>& value) { return value.has_value(); }
};
for <T> concept counted<T*> {
  int count(T* value) { return value == nullptr ? 0 : 1; }
};
for <T> concept counted<const T*> {
  int count(const T* value) { return 2; }
};
for <T> concept counted<T[3]> {
  int count(T value[3]) { return 3; }
};
for <T> concept counted<T[4]> {
  int count(T value[4]) { return 4; }
};
)gti";
  accepted(builtin + R"gti(
sized<T>::count_type measured<T>(T& value) requires sized<T> {
  return sized<T>::count(value);
}
int total<T>(T value) requires counted<T> {
  return counted<T>::count(std::move(value));
}
int main() {
  expected<int, bool> held = 3;
  sized<expected<int, bool>>::count_type present = measured(held);
  mut int number = 4;
  int fixed = 5;
  int three[3] = {1, 2, 3};
  int four[4] = {1, 2, 3, 4};
  return (present ? 0 : 1) + total(&number) + total(&fixed) +
         counted<int[3]>::count(three) + total(four) - 10;
}
)gti",
           "blocks over expected, pointers and arrays should serve their "
           "instances");
  rejected(builtin + R"gti(
for concept counted<int*> {
  int count(int* value) { return 9; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "overlaps another");
  rejected(builtin + R"gti(
int main() {
  int five[5] = {1, 2, 3, 4, 5};
  return counted<int[5]>::count(five);
}
)gti",
           "GTI-S2123", "No conformance block 'for concept counted<");
  rejected(R"gti(
concept counted<T> {
  int count(T value);
};
for <T> concept counted<T**> {
  int count(T** value) { return 0; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "'T*' or 'T[3]'");
  // No package owns a built-in type, so only the concept's package declares
  // a block over one.
  rejected(R"gti(
for <T> concept std::tryable<T*> {
  using value_type = T*;
  using residual_type = T*;
  bool has_value(T*& value) { return value != nullptr; }
  T* take_value(T* value) { return value; }
  T* take_residual(T* value) { return value; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "owns neither the concept nor the type");
  // The prelude's package owns the source `expected` class and
  // `std::tryable`, so an application block over it is equally foreign.
  rejected(R"gti(
for <T, E> concept std::tryable<expected<T, E>> {
  using value_type = T;
  using residual_type = expected<T, E>;
  bool has_value(expected<T, E>& value) { return value.has_value(); }
  T take_value(expected<T, E> value) { return std::move(value).value(); }
  expected<T, E> take_residual(expected<T, E> value) { return value; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "owns neither the concept nor the type");

  // A block function has the concept operation's interface: it declares no
  // defaults, so concrete and generic calls accept the same arguments.
  const std::string probe = R"gti(
concept probe<T> {
  int read(T& value, int scale);
};
class Cell {
public:
  int value;
  Cell(int amount) : value(amount) {}
};
)gti";
  rejected(probe + R"gti(
for concept probe<Cell> {
  int read(Cell& value, int scale = 2) { return value.value * scale; }
};
int main() {
  Cell cell{1};
  return probe<Cell>::read(cell) - 2;
}
)gti",
           "GTI-S2123", "cannot declare a default argument");
  reportedOnce(probe + R"gti(
for concept probe<Cell> {
  int read(Cell& value, int scale = 2) { return value.value * scale; }
};
int main() { return 0; }
)gti",
               "GTI-S2123", "Cell& value, int ", "scale");
  // `C<X>::name` selects the block as `requires C<X>` does, constraints
  // included.
  const std::string constrained = R"gti(
concept collection<T> {
  typename element_type;
  int size(T& value);
};
class Box<T> {
public:
  T item;
  Box(T value) : item(value) {}
};
for <std::integral T> concept collection<Box<T>> {
  using element_type = T;
  int size(Box<T>& value) { return 1; }
};
)gti";
  accepted(constrained + R"gti(
int main() {
  collection<Box<int>>::element_type value = 3;
  return value - 3;
}
)gti",
           "an associated type of a satisfied block should resolve");
  rejected(constrained + R"gti(
int main() {
  collection<Box<double>>::element_type value = 3.0;
  return 0;
}
)gti",
           "GTI-S2029", "does not satisfy");
  rejected(constrained + R"gti(
int main() {
  double half = 1.5;
  Box<double> box{half};
  return collection<Box<double>>::size(box);
}
)gti",
           "GTI-S2029", "does not satisfy");

  // A constraint a block's pattern fails names the block's own parameter.
  // The block validates its pattern; each operation restates the block's
  // application as its requirement without validating it again, so the
  // pattern reports once however many operations the block has. An
  // operation's own parameter type is a separate location.
  const std::string unprovenPattern = R"gti(
concept wrapped<T> {
  typename value_type;
  value_type unwrap(T value);
};
class holder<std::copy_constructible T> {
public:
  T item;
  holder(T value) : item(value) {}
};
for <T> concept wrapped<holder<T>> {
  using value_type = T;
  T unwrap(holder<T> value) { return value.item; }
};
int main() { return 0; }
)gti";
  rejected(unprovenPattern, "GTI-S2029", "Type 'T' does not satisfy");
  reportedOnce(unprovenPattern, "GTI-S2029", "concept wrapped<holder<", "T");
  reportedOnce(unprovenPattern, "GTI-S2029", "unwrap(holder<", "T");
  const std::string unprovenPatternTwice = R"gti(
concept wrapped<T> {
  typename value_type;
  value_type unwrap(T value);
  bool is_empty(T value);
};
class holder<std::copy_constructible T> {
public:
  T item;
  holder(T value) : item(value) {}
};
for <T> concept wrapped<holder<T>> {
  using value_type = T;
  T unwrap(holder<T> value) { return value.item; }
  bool is_empty(holder<T> value) { return false; }
};
int main() { return 0; }
)gti";
  reportedOnce(unprovenPatternTwice, "GTI-S2029", "concept wrapped<holder<",
               "T");
  reportedOnce(unprovenPatternTwice, "GTI-S2029", "unwrap(holder<", "T");
  reportedOnce(unprovenPatternTwice, "GTI-S2029", "is_empty(holder<", "T");
  const std::string unknownPattern = measurable + R"gti(
for concept measurable<Missing> {
  using unit_type = int;
  int measure(Missing& item) { return 0; }
  bool is_empty(Missing& item) { return true; }
};
int main() { return 0; }
)gti";
  reportedOnce(unknownPattern, "GTI-S2000", "concept measurable<", "Missing");
  reportedOnce(unknownPattern, "GTI-S2000", "measure(", "Missing");

  // A block's types are validated after every block is registered and each
  // class's members and lifecycle are known, so a concrete type's
  // constraints and a projection through a later block hold as they do in
  // any other declaration.
  const std::string comparedBox = R"gti(
concept wrapped<T> {
  typename value_type;
  value_type unwrap(T value);
};
class box<std::equality_comparable T> {
public:
  T item;
  box(T value) : item(value) {}
};
class Coin {
public:
  int id;
  Coin(int value) : id(value) {}
)gti";
  const std::string coinOperators = R"gti(
  bool operator==(Coin& other) { return id == other.id; }
  bool operator!=(Coin& other) { return id != other.id; }
)gti";
  accepted(comparedBox + coinOperators + R"gti(};
for concept wrapped<box<Coin>> {
  using value_type = box<Coin>;
  box<Coin> unwrap(box<Coin> value) { return value; }
};
int main() {
  box<Coin> boxed{Coin(42)};
  wrapped<box<Coin>>::value_type same = wrapped<box<Coin>>::unwrap(boxed);
  return same.item.id - 42;
}
)gti",
           "a concrete pattern and associated type should satisfy a "
           "constraint met by the class's members");
  const std::string uncomparedBox = comparedBox + R"gti(};
for concept wrapped<box<Coin>> {
  using value_type = int;
  int unwrap(box<Coin> value) { return value.item.id; }
};
int main() { return 0; }
)gti";
  rejected(uncomparedBox, "GTI-S2029", "Type 'Coin' does not satisfy");
  reportedOnce(uncomparedBox, "GTI-S2029", "concept wrapped<box<", "Coin");
  reportedOnce(uncomparedBox, "GTI-S2029", "unwrap(box<", "Coin");
  // A block without operations validates its pattern too.
  const std::string uncopiedPattern = R"gti(
concept labeled<T> {
  typename label_type;
};
class holder<std::copy_constructible T> {
public:
  T item;
  holder(T value) : item(value) {}
};
class Ticket {
public:
  int id;
  Ticket(int value) : id(value) {}
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
};
for concept labeled<holder<Ticket>> {
  using label_type = int;
};
int main() { return 0; }
)gti";
  rejected(uncopiedPattern, "GTI-S2029", "Type 'Ticket' does not satisfy");
  reportedOnce(uncopiedPattern, "GTI-S2029", "concept labeled<holder<",
               "Ticket");
  accepted(measurable + R"gti(
concept labeled<T> {
  typename label_type;
};
for concept labeled<Meters> {
  using label_type = measurable<Rope>::unit_type;
};
for concept measurable<Rope> {
  using unit_type = int;
  int measure(Rope& item) { return item.length; }
  bool is_empty(Rope& item) { return item.length == 0; }
};
int main() {
  labeled<Meters>::label_type length = 42;
  return length - 42;
}
)gti",
           "an associated type may project through a block declared later");

  // Declarations.
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
           "GTI-S2123", "Block parameter 'U' does not appear");
  rejected(measurable + R"gti(
for <T> concept measurable<Crate<Crate<T>>> {
  using unit_type = int;
  int measure(Crate<Crate<T>>& item) { return 0; }
  bool is_empty(Crate<Crate<T>>& item) { return true; }
};
int main() { return 0; }
)gti",
           "GTI-S2123", "Each argument of a conformance block");
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

  // Completion inside a block's pattern offers the types in scope, whether
  // or not the block has operations.
  for (const std::string members :
       {"  using value_type = Item;\n"
        "  Item unwrap(holder<Item> value) { return value.item; }\n",
        "  using value_type = Item;\n"}) {
    const std::string source = R"gti(concept wrapped<T> {
  typename value_type;
  value_type unwrap(T value);
};
class holder<T> {
public:
  T item;
  holder(T value) : item(value) {}
};
for <Item> concept wrapped<hol> {
)gti" + members + "};\n";
    const auto completion = lang::LanguageQueries().complete(
        {.entryPath = root / "tests" / "protocol-concepts.gti",
         .source = source,
         .byteOffset = source.find("<hol>") + 4,
         .preludePaths = {root / "stdlib" / "prelude.gti"},
         .standardLibraryRoots = {root / "stdlib"}});
    expect(std::any_of(completion.candidates.begin(),
                       completion.candidates.end(),
                       [](const auto &candidate) {
                         return candidate.label == "holder";
                       }),
           "completion in a block's pattern should offer class templates");
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
  const std::string formattedPair = R"gti(concept adds_to<Left, Right> {
  typename sum_type;
  sum_type add(Left& left, Right& right);
};

for <T> concept adds_to<Tally<T>, Tally<T>> {
  using sum_type = Tally<T>;
  Tally<T> add(Tally<T>& left, Tally<T>& right) {
    return Tally<T>(left.count + right.count);
  }
};
)gti";
  expect(lang::Formatter().format(formattedPair) == formattedPair,
         "formatted concepts with several parameters should be stable");
  const std::string formattedBuiltin =
      R"gti(for <T, E> concept sized<expected<T, E>> {
  using count_type = bool;
  bool count(expected<T, E>& value) {
    return value.has_value();
  }
};

for <T> concept counted<const T*> {
  int count(const T* value) {
    return 2;
  }
};

for <T> concept counted<T[3]> {
  int count(T value[3]) {
    return 3;
  }
};
)gti";
  expect(lang::Formatter().format(formattedBuiltin) == formattedBuiltin,
         "formatted blocks over built-in types should be stable");
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
