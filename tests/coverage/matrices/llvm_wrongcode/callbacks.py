"""Matrix 12: erased callbacks capturing objects x where they live.

Lambdas and functions held in `std::function`, `std::transfer_function` and
`std::function_ptr`, capturing scalars, text and polymorphic owners, stored
locally, in fields of classes invoked through an interface, in vectors and
maps, returned from factories and owned through a polymorphic owner.
make(i) returns a callback whose call with x yields VALUE(i, x).
"""
import itertools

PRELUDE = """import std.functional;
import std.memory;
import std.vector;
import std.map;
import std.string;
import std.atomic;
import std.format;
import std.utility;
std::atomic<int32_t> constructed{0};
std::atomic<int32_t> destroyed{0};
[[unsafe_transfer]] class Tracked {
  int32_t id;
public:
  Tracked(int32_t id) : id(id) { [[discard]] constructed.fetch_add(1); }
  Tracked(Tracked& other) = delete;
  Tracked(Tracked&& other) = default;
  ~Tracked() { [[discard]] destroyed.fetch_add(1); }
  int32_t value() { return this.id; }
};
[[transfer]] interface Shape {
  int64_t weight();
};
[[unsafe_transfer]] class Box : public Shape {
  Tracked mark;
public:
  Box(int32_t id) : mark(Tracked(id)) {}
  int64_t weight() override { return int64_t(this.mark.value()) * 7; }
};
interface Action {
  int64_t fire(int64_t x);
};
"""

TEXT = "callback text"

KINDS = {
    "function_scalar": ("""using C = std::function<int64_t(int64_t)>;
C make(int32_t i) {
  int64_t base = int64_t(i);
  return C([base](int64_t x) -> int64_t { return base * 10 + x; });
}
""", lambda i, x: i * 10 + x),
    "move_only_text": (f"""using C = std::move_only_function<int64_t(int64_t)>;
C make(int32_t i) {{
  std::string text = std::string("{TEXT}");
  int64_t base = int64_t(i);
  return C([held = std::move(text), base](int64_t x) -> int64_t {{ return int64_t(held.size()) * 100 + base * 10 + x; }});
}}
""", lambda i, x: len(TEXT) * 100 + i * 10 + x),
    "move_only_owner": ("""using C = std::move_only_function<int64_t(int64_t)>;
C make(int32_t i) {
  std::unique_ptr<Shape> owner = std::upcast_unique<Shape, Box>(std::make_unique<Box>(i));
  return C([held = std::move(owner)](int64_t x) -> int64_t { return held->weight() + x; });
}
""", lambda i, x: i * 7 + x),
    "transfer_owner": ("""using C = std::transfer_function<int64_t(int64_t)>;
C make(int32_t i) {
  std::unique_ptr<Shape> owner = std::upcast_unique<Shape, Box>(std::make_unique<Box>(i));
  return C([held = std::move(owner)](int64_t x) -> int64_t { return held->weight() + x; });
}
""", lambda i, x: i * 7 + x),
    "move_only_shared": ("""using C = std::move_only_function<int64_t(int64_t)>;
C make(int32_t i) {
  std::shared_ptr<Shape> owner = std::upcast_shared<Shape, Box>(std::make_shared<Box>(i));
  std::shared_ptr<Shape> other = owner.clone();
  return C([first = std::move(owner), second = std::move(other)](int64_t x) -> int64_t {
    return first->weight() + second->weight() + x;
  });
}
""", lambda i, x: i * 14 + x),
    "function_ptr": ("""using C = std::function_ptr<int64_t(int64_t)>;
int64_t triple(int64_t x) { return x * 3; }
int64_t square(int64_t x) { return x * x; }
C make(int32_t i) {
  if (i % 2 == 0) return C(triple);
  return C(square);
}
""", lambda i, x: x * 3 if i % 2 == 0 else x * x),
}

HOLDERS = {
    "local": ("", """  C first = make(2);
  C second = make(3);
  int64_t got = first(int64_t(1)) * 1000 + second(int64_t(4));""", lambda V: V(2, 1) * 1000 + V(3, 4)),
    "field_interface": ("""class Button : public Action {
  C on_fire;
public:
  Button(C on_fire) : on_fire(std::move(on_fire)) {}
  int64_t fire(int64_t x) override { return this.on_fire(x); }
};
int64_t press(Action& action, int64_t x) { return action.fire(x); }
""", """  Button button = Button(make(2));
  Action& view = button;
  int64_t got = press(view, int64_t(5)) * 1000 + view.fire(int64_t(6));""", lambda V: V(2, 5) * 1000 + V(2, 6)),
    "vector": ("", """  mut std::vector<C> handlers{};
  for (mut int32_t i = 1; i <= 3; i += 1) handlers.push_back(make(i));
  mut int64_t got = 0;
  for (auto& handler : handlers) got = got * 1000 + handler(int64_t(handlers.size()));""",
               lambda V: (V(1, 3) * 1000 + V(2, 3)) * 1000 + V(3, 3)),
    "map": ("", """  mut std::map<int32_t, C> registry{};
  for (mut int32_t i = 1; i <= 3; i += 1) [[discard]] registry.insert(i, make(i));
  int32_t key = 2;
  int64_t got = registry.at(key)(int64_t(7)) * 1000 + int64_t(registry.size());""",
            lambda V: V(2, 7) * 1000 + 3),
    "returned": ("""class Holder {
public:
  C held;
  Holder(C held) : held(std::move(held)) {}
};
Holder build(int32_t i) {
  mut C local = make(i);
  return Holder(std::move(local));
}
""", """  mut Holder holder = build(3);
  mut Holder moved = std::move(holder);
  int64_t got = moved.held(int64_t(2));""", lambda V: V(3, 2)),
    "owned_interface": ("""class Button : public Action {
  C on_fire;
public:
  Button(C on_fire) : on_fire(std::move(on_fire)) {}
  int64_t fire(int64_t x) override { return this.on_fire(x); }
};
""", """  std::unique_ptr<Action> owner = std::upcast_unique<Action, Button>(std::make_unique<Button>(Button(make(4))));
  std::unique_ptr<Action> moved = std::move(owner);
  int64_t got = moved->fire(int64_t(1)) * 1000 + moved->fire(int64_t(2));""", lambda V: V(4, 1) * 1000 + V(4, 2)),
}


def program(kind, holder):
    kind_source, value = KINDS[kind]
    helpers, body, want = HOLDERS[holder]
    return PRELUDE + kind_source + helpers + f"""int32_t run() {{
{body}
  if (got != {want(value)}) {{
    std::println("mismatch {{}}", got);
    return 2;
  }}
  return 0;
}}

int main() {{
  int32_t status = run();
  if (status != 0) return status;
  if (constructed.load() != destroyed.load()) {{
    std::println("constructed {{}} destroyed {{}}", constructed.load(), destroyed.load());
    return 9;
  }}
  std::println("ok");
  return 0;
}}
"""


def cells():
    for kind, holder in itertools.product(KINDS, HOLDERS):
        yield f"{kind}__{holder}", program(kind, holder)
