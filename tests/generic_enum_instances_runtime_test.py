#!/usr/bin/env python3
"""General generic enum instances retain exact identity, loans and cleanup."""

import pathlib
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as native_matrix


PASSIVE = """
enum Event<T, U = T, uint64_t Channel = 3> {
  idle,
  item(T value),
  pair(T first, U second)
};
"""

OWNING = """
#include <std/vector>
#include <std/optional>

mut int32_t drops = 0;
mut int32_t cleanup_divisor = 1;

class Resource {
  int32_t identity;
public:
  Resource(int32_t identity) : identity(identity) {}
  Resource(Resource& other) = delete;
  Resource(Resource&& other) = default;
  ~Resource() {
    drops = drops + 1;
    std::println(this.identity);
    if (this.identity == 2) {
      int32_t checked = int32_t(1) / cleanup_divisor;
    }
  }
  int32_t id() { return this.identity; }
};

enum Event<T, uint64_t Channel = 3> { idle, item(T value) };

concept identified<T> = requires(T& value) { { value.id() } -> int32_t; };

int32_t id<T, uint64_t Channel>(Event<T, Channel>& event) requires identified<T> {
  switch (event) {
    case Event<T, Channel>::idle: return 0;
    case Event<T, Channel>::item(auto& value): return value.id();
  }
}

T relay<T>(T value) { return std::move(value); }

class Envelope<T> {
  mut T value;
public:
  Envelope(T value) : value(std::move(value)) {}
  T& get() { return this.value; }
  void replace(T replacement) mut { this.value = std::move(replacement); }
  T extract() && { return std::move(this.value); }
};
"""


def runtime_cases():
    cases = {
        "exact-instances": (PASSIVE + """
T identity<T>(T value) { return value; }
int main() {
  Event<int32_t> first = Event<int32_t>::item(7);
  Event<int64_t> second = identity(Event<int64_t>::pair(int64_t(4), int64_t(5)));
  mut int32_t total = 0;
  switch (first) {
    case Event<int32_t>::idle: return 1;
    case Event<int32_t>::item(value): total = value; break;
    case Event<int32_t>::pair(a, b): return 2;
  }
  switch (second) {
    case Event<int64_t>::idle: return 3;
    case Event<int64_t>::item(value): return 4;
    case Event<int64_t>::pair(a, b): return total == 7 && a + b == int64_t(9) ? 0 : 5;
  }
}
""", 0, ""),
        "default-alias-namespace": ("""
namespace messages {
  enum class Packet<T, U = T, uint64_t Channel = 3> {
    absent,
    pair(T first, U second)
  };
}
using Default = messages::Packet<int32_t>;
using Explicit = messages::Packet<int32_t, int32_t, 3>;
using Other = messages::Packet<int32_t, int64_t, 9>;
Default default_packet(Explicit packet) { return packet; }
int main() {
  Default first = default_packet(messages::Packet<int32_t, int32_t, 3>::pair(1, 2));
  Other second = Other::pair(3, int64_t(4));
  switch (first) {
    case Default::absent: return 1;
    case Default::pair(a, b): if (a + b != 3) { return 2; } break;
  }
  switch (second) {
    case messages::Packet<int32_t, int64_t, 9>::absent: return 3;
    case messages::Packet<int32_t, int64_t, 9>::pair(a, b): return a == 3 && b == int64_t(4) ? 0 : 4;
  }
}
""", 0, ""),
        "scalar-pattern-loans": (PASSIVE + """
int main() {
  mut Event<int32_t> event = Event<int32_t>::item(1);
  switch (event) {
    case Event<int32_t>::idle: return 1;
    case Event<int32_t>::item(mut auto& value): value = 9; break;
    case Event<int32_t>::pair(a, b): return 2;
  }
  switch (event) {
    case Event<int32_t>::idle: return 3;
    case Event<int32_t>::item(auto& value): return value == 9 ? 0 : 4;
    case Event<int32_t>::pair(a, b): return 5;
  }
}
""", 0, ""),
        "owning-transport-and-containers": (OWNING + """
void exercise() {
  Event<Resource> source = relay(Event<Resource>::item(Resource(1)));
  mut Envelope<Event<Resource>> packet = Envelope<Event<Resource>>(std::move(source));
  packet.replace(Event<Resource>::item(Resource(2)));
  if (id(packet.get()) != 2) { std::println(99); }
  Event<Resource> extracted = std::move(packet).extract();
  mut std::vector<Event<Resource>> events{};
  events.push_back(std::move(extracted));
  events.push_back(Event<Resource>::idle);
  events.push_back(Event<Resource>::item(Resource(3)));
  events.reserve(std::size_t(12));
  mut int32_t total = 0;
  for (auto& event : events) { total = total + id(event); }
  if (total != 5) { std::println(99); }
  mut std::optional<Event<Resource, 8>> selected =
      std::optional<Event<Resource, 8>>(Event<Resource, 8>::item(Resource(4)));
  if (id(selected.value()) != 4) { std::println(99); }
  selected.reset();
}
int main() { exercise(); return drops == 4 ? 0 : 1; }
""", 0, "1\n4\n3\n2\n"),
        "generic-class-payload": (OWNING + """
class Payload<identified T> {
  T resource;
public:
  Payload(T value) : resource(std::move(value)) {}
  Payload(Payload<T>&& other) = default;
  int32_t id() { return this.resource.id(); }
};
int main() {
  Event<Payload<Resource>> first = Event<Payload<Resource>>::item(Payload<Resource>(Resource(5)));
  Event<Resource, 7> second = Event<Resource, 7>::item(Resource(6));
  return id(first) == 5 && id(second) == 6 ? 0 : 1;
}
""", 0, "6\n5\n"),
        "receiver-pattern-loans": ("""
#include <std/vector>
enum Buffer<std::move_constructible T> { data(std::vector<T> values) };
class Inbox<std::move_constructible T> {
  mut Buffer<T> message = Buffer<T>::data(std::vector<T>());
public:
  Inbox() {}
  std::vector<T>& values() {
    switch (this.message) {
      case Buffer<T>::data(auto& values): return values;
    }
  }
  mut std::vector<T>& values() mut {
    switch (this.message) {
      case Buffer<T>::data(mut auto& values): return values;
    }
  }
};
int main() {
  mut Inbox<int32_t> inbox{};
  inbox.values().push_back(9);
  return inbox.values()[std::size_t(0)] == 9 ? 0 : 1;
}
""", 0, ""),
        "finite-recursive-generic-sum": (OWNING + """
enum Tree<T> { empty, leaf(T value), branch(std::vector<Node<T>> children) };
class Node<T> {
  Tree<T> data;
public:
  Node(Tree<T> value) : data(std::move(value)) {}
  Node(Node<T>&& other) = default;
};
Node<T> branch<T>(T first, T second) {
  mut std::vector<Node<T>> children{};
  children.push_back(Node<T>(Tree<T>::leaf(std::move(first))));
  children.push_back(Node<T>(Tree<T>::leaf(std::move(second))));
  return Node<T>(Tree<T>::branch(std::move(children)));
}
int main() {
  { Node<Resource> tree = branch(Resource(1), Resource(3)); }
  return drops == 2 ? 0 : 1;
}
""", 0, "3\n1\n"),
        "passive-pointer-and-enum-payloads": ("""
enum Color : uint8_t { blue = 4, green = 5 };
enum Value<T> { empty, item(T value) };
int main() {
  int32_t owner = 7;
  Value<const int32_t*> pointer = Value<const int32_t*>::item(&owner);
  Value<Color> color = Value<Color>::item(Color::blue);
  switch (pointer) {
    case Value<const int32_t*>::empty: return 1;
    case Value<const int32_t*>::item(address):
      unsafe { if (*address != 7) { return 2; } }
      break;
  }
  switch (color) {
    case Value<Color>::empty: return 3;
    case Value<Color>::item(value): return value == Color::blue ? 0 : 4;
  }
}
""", 0, ""),
        "value-argument-payload-substitution": ("""
class Tagged<T, uint64_t Tag> {
  T value;
public:
  Tagged(T value) : value(value) {}
  T get() { return this.value; }
  uint64_t tag() { return Tag; }
};
enum TaggedEvent<T, uint64_t Tag> { item(Tagged<T, Tag> value) };
int main() {
  TaggedEvent<int32_t, 3> first =
      TaggedEvent<int32_t, 3>::item(Tagged<int32_t, 3>(7));
  TaggedEvent<int32_t, 9> second =
      TaggedEvent<int32_t, 9>::item(Tagged<int32_t, 9>(11));
  switch (first) {
    case TaggedEvent<int32_t, 3>::item(auto& value):
      if (value.get() != 7 || value.tag() != uint64_t(3)) { return 1; }
      break;
  }
  switch (second) {
    case TaggedEvent<int32_t, 9>::item(auto& value):
      return value.get() == 11 && value.tag() == uint64_t(9) ? 0 : 2;
  }
}
""", 0, ""),
        "constrained-parameter": ("""
concept numeric<T> = std::signed_numeric<T>;
enum NumericEvent<numeric T> { empty, item(T value) };
int main() {
  NumericEvent<int32_t> event = NumericEvent<int32_t>::item(8);
  switch (event) {
    case NumericEvent<int32_t>::empty: return 1;
    case NumericEvent<int32_t>::item(value): return value == 8 ? 0 : 2;
  }
}
""", 0, ""),
        "pack-identity-and-inference": ("""
enum Packet<std::copyable T, Rest...> { item(T value) };
T read<std::copyable T, Rest...>(Packet<T, Rest...>& packet) {
  switch (packet) {
    case Packet<T, Rest...>::item(value): return value;
  }
}
int main() {
  Packet<int32_t, bool, char> first = Packet<int32_t, bool, char>::item(7);
  Packet<int32_t> second = Packet<int32_t>::item(9);
  return read(first) == 7 && read(second) == 9 ? 0 : 1;
}
""", 0, ""),
        "all-default-arguments": ("""
enum DefaultEvent<T = int32_t, uint64_t Channel = 3> { idle, item(T value) };
using Selected = DefaultEvent<>;
int main() {
  Selected first = DefaultEvent<>::item(7);
  DefaultEvent<int32_t, 3> second = first;
  switch (second) {
    case Selected::idle: return 1;
    case Selected::item(value): return value == 7 ? 0 : 2;
  }
}
""", 0, ""),
        "two-field-partial-construction": (OWNING + """
enum Pair<T, U> { values(T first, U second) };
int main() {
  Resource caller = Resource(9);
  mut int32_t zero = 0;
  Pair<Resource, Resource> pair = Pair<Resource, Resource>::values(Resource(1), Resource(int32_t(1) / zero));
  return 1;
}
""", 70, "1\n9\n"),
        "later-call-argument-failure": (OWNING + """
void accept<T>(T first, int32_t second) {}
int main() {
  Resource caller = Resource(9);
  mut int32_t zero = 0;
  accept(Event<Resource>::item(Resource(7)), int32_t(1) / zero);
  return 1;
}
""", 70, "7\n9\n"),
        "nested-result-later-argument-success": (OWNING + """
void accept<T>(T first, int32_t second) {
  if (id(first) != 7 || second != 3) { std::println(99); }
}
int main() {
  accept(relay(Event<Resource>::item(Resource(7))), 3);
  return drops == 1 ? 0 : 1;
}
""", 0, "7\n"),
        "nested-result-later-argument-failure": (OWNING + """
void accept<T>(T first, int32_t second) {}
int main() {
  Resource caller = Resource(9);
  mut int32_t zero = 0;
  accept(relay(Event<Resource>::item(Resource(7))), int32_t(1) / zero);
  return 1;
}
""", 70, "7\n9\n"),
        "later-call-argument-after-move": (OWNING + """
void accept<T>(T first, int32_t second) {}
int main() {
  Resource caller = Resource(9);
  Event<Resource> source = Event<Resource>::item(Resource(7));
  mut int32_t zero = 0;
  accept(std::move(source), int32_t(1) / zero);
  return 1;
}
""", 70, "7\n9\n"),
        "callee-failure": (OWNING + """
void accept<T>(T first, int32_t divisor) {
  Resource local = Resource(6);
  int32_t checked = int32_t(1) / divisor;
}
int main() {
  Resource caller = Resource(9);
  accept(Event<Resource>::item(Resource(7)), 0);
  return 1;
}
""", 70, "6\n7\n9\n"),
        "containing-field-rollback": (OWNING + """
class Partial<T> {
  T completed;
  int32_t checked;
public:
  Partial(T value, int32_t divisor)
      : completed(std::move(value)), checked(int32_t(1) / divisor) {}
};
int main() {
  Resource caller = Resource(9);
  Partial<Event<Resource>> value = Partial<Event<Resource>>(Event<Resource>::item(Resource(7)), 0);
  return 1;
}
""", 70, "7\n9\n"),
        "container-cleanup-failure": (OWNING + """
int main() {
  Resource caller = Resource(9);
  cleanup_divisor = 0;
  mut std::vector<Event<Resource>> events{};
  events.push_back(Event<Resource>::item(Resource(1)));
  events.push_back(Event<Resource>::item(Resource(2)));
  events.push_back(Event<Resource>::item(Resource(3)));
  return 0;
}
""", 70, "3\n2\n1\n9\n"),
        "alternative-sibling-cleanup-failure": (OWNING + """
enum Pair<T, U> { values(T first, U second) };
int main() {
  Resource caller = Resource(9);
  cleanup_divisor = 0;
  Pair<Resource, Resource> pair = Pair<Resource, Resource>::values(Resource(1), Resource(2));
  return 0;
}
""", 70, "2\n1\n9\n"),
    }
    example = pathlib.Path(__file__).resolve().parents[1] / "examples/129-generic-payload-enums.gti"
    cases["application-example"] = (example.read_text(), 0, "hello\nidle\n0\n")
    return cases


def rejection_cases():
    return {
        "mismatched-construction": (PASSIVE + """
int main() { Event<int32_t> event = Event<int64_t>::item(int64_t(1)); return 0; }
""", "GTI-S2003", "type"),
        "mismatched-value-argument": (PASSIVE + """
int main() { Event<int32_t, int32_t, 3> event = Event<int32_t, int32_t, 7>::item(1); return 0; }
""", "GTI-S2003", "type"),
        "mismatched-pack-argument": ("""
enum Packet<T, Rest...> { item(T value) };
int main() { Packet<int32_t, bool> packet = Packet<int32_t, char>::item(1); return 0; }
""", "GTI-S2003", "type"),
        "mismatched-pattern": (PASSIVE + """
int main() {
  Event<int32_t> event = Event<int32_t>::item(1);
  switch (event) {
    case Event<int64_t>::item(value): return 0;
    default: return 1;
  }
}
""", "GTI-S2067", "variant"),
        "nonexhaustive": (PASSIVE + """
int main() {
  Event<int32_t> event = Event<int32_t>::item(1);
  switch (event) { case Event<int32_t>::item(value): return value; }
  return 0;
}
""", "GTI-S2067", "exhaustive"),
        "invalid-void-substitution": (PASSIVE + """
int main() { auto event = Event<void>::idle; return 0; }
""", "GTI-S2067", "payload"),
        "invalid-reference-substitution": (PASSIVE + """
int main() { auto event = Event<int32_t&>::idle; return 0; }
""", "GTI-S2067", "payload"),
        "invalid-array-substitution": (PASSIVE + """
int main() { auto event = Event<int32_t[2]>::idle; return 0; }
""", "GTI-S2067", "payload"),
        "invalid-signature-substitution": (PASSIVE + """
int main() { auto event = Event<int32_t(int32_t)>::idle; return 0; }
""", "GTI-S2067", "payload"),
        "implicit-owning-copy": (OWNING + """
int main() {
  Event<Resource> source = Event<Resource>::item(Resource(1));
  Event<Resource> copy = source;
  return 0;
}
""", "GTI-S2003", "copy"),
        "use-after-owning-move": (OWNING + """
int main() {
  Event<Resource> source = Event<Resource>::item(Resource(1));
  Event<Resource> destination = std::move(source);
  return id(source);
}
""", "GTI-S2018", "moved"),
        "move-during-live-pattern-loan": (OWNING + """
int main() {
  Event<Resource> source = Event<Resource>::item(Resource(1));
  switch (source) {
    case Event<Resource>::idle: return 1;
    case Event<Resource>::item(auto& value):
      Event<Resource> destination = std::move(source);
      return value.id();
  }
}
""", "GTI-S2017", "borrow"),
        "immutable-mut-pattern": (PASSIVE + """
int main() {
  Event<int32_t> event = Event<int32_t>::item(1);
  switch (event) {
    case Event<int32_t>::item(mut auto& value): value = 3; return value;
    default: return 1;
  }
}
""", "GTI-S2067", "mutable"),
        "temporary-pattern-loan": (PASSIVE + """
int main() {
  switch (Event<int32_t>::item(1)) {
    case Event<int32_t>::item(auto& value): return value;
    default: return 1;
  }
}
""", "GTI-S2067", "stable"),
        "escaping-local-payload-loan": (OWNING + """
Resource& escape() {
  Event<Resource> event = Event<Resource>::item(Resource(1));
  switch (event) {
    case Event<Resource>::idle: return escape();
    case Event<Resource>::item(auto& value): return value;
  }
}
int main() { return 0; }
""", "GTI-S2017", "return"),
        "sibling-exclusive-pattern-loans": ("""
enum Pair<T> { values(T first, T second) };
int main() {
  mut Pair<int32_t> pair = Pair<int32_t>::values(1, 2);
  switch (pair) {
    case Pair<int32_t>::values(mut auto& first, mut auto& second):
      first = 3;
      second = 4;
      return first + second;
  }
}
""", "GTI-S2017", "borrow"),
        "recursive-by-value-layout": ("""
enum Link<T> { node(T value) };
struct Node { Link<Node> next; };
int main() { return 0; }
""", "GTI-S2082", "recursive"),
        "bare-enum-unqualified-case": ("""
enum State { idle, item(int32_t value) };
int main() { State state = idle; return 0; }
""", "GTI-S2001", "idle"),
        "bare-enum-integer-conversion": ("""
enum State : uint8_t { idle, ready = 4 };
int main() { int32_t value = State::ready; return value; }
""", "GTI-S2003", "type"),
    }


def check_rejections(compiler, root):
    rejected_cases = rejection_cases()
    for name, (source, code, message) in rejected_cases.items():
        path = root / f"reject-{name}.gti"
        path.write_text(source)
        result = subprocess.run([str(compiler), str(path), "--emit-cpp", "-o",
                                 str(root / f"reject-{name}.cpp")],
                                text=True, capture_output=True, timeout=120)
        if (result.returncode == 0 or code not in result.stderr or
                message not in result.stderr.lower() or "GTI-B000" in result.stderr):
            raise AssertionError((name, "expected source diagnostic", code,
                                  message, result.stdout, result.stderr))
    print(f"Generic enum instances: {len(rejected_cases)} source rejections passed")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: generic_enum_instances_runtime_test.py <gti> [matrix filters]")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-generic-enum-instances-") as temporary:
        root = pathlib.Path(temporary)
        check_rejections(compiler, root)
        fixtures = {}
        observations = {}
        for name, (source, status, output) in runtime_cases().items():
            path = root / f"generic-enum-{name}.gti"
            path.write_text(source)
            fixtures[name] = path
            stderr = ""
            if status == 70:
                needle = ("int32_t checked = int32_t(1) / cleanup_divisor;"
                          if "cleanup-failure" in name else
                          "checked(int32_t(1) / divisor)"
                          if name == "containing-field-rollback" else
                          "checked = int32_t(1) / divisor"
                          if name == "callee-failure" else
                          "int32_t(1) / zero")
                matches = [index for index, line in enumerate(source.splitlines(), 1)
                           if needle in line]
                if len(matches) != 1:
                    raise AssertionError((name, "failure source must have one location", matches))
                stderr = (f"{path.name}:{matches[0]}: runtime error[GTI-R0002]: "
                          "division_by_zero in integer_division\n")
            observations[name] = [((), (status, output, stderr))]
        native_matrix(names=tuple(fixtures), cases=observations, fixture_paths=fixtures)


if __name__ == "__main__":
    main()
