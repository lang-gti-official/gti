"""Matrix 4: hierarchy class holding owned fields x operation, self-checking.

Base initializers receive only scalars; owned data lives in derived fields.
An owned base-initializer argument is covered by the transfer matrix (LLVM
refuses it as UnstagedBaseInitializer).
"""
import itertools

from .transfer import PRELUDE, TEXT, text_sum, TEXT_CHECK

COMMON = """#include <std/polymorphic>
#include <std/utility>
int64_t check_text(std::string& body) {
""" + TEXT_CHECK.replace("BODY", "body") + """
}
[[transfer]] interface Labeled {
  int64_t check();
};
[[transfer]] interface Tagged {
  int32_t tag();
};
"""

LABEL = TEXT


def shapes(shape):
    """Returns (source, type name, constructor expression maker, interface)."""
    if shape == "iface":
        return ("""[[unsafe_transfer]] class Item : public Labeled {
  std::string label;
  Tracked mark;
  int32_t scale;
public:
  Item(int32_t scale) : label(std::string("LABEL")), mark(Tracked(scale)), scale(scale) {}
  int64_t check() override { return check_text(this.label) + int64_t(this.mark.value()) * int64_t(this.scale); }
};
""", "Item")
    if shape == "dual":
        return ("""[[unsafe_transfer]] class Item : public Tagged, public Labeled {
  std::string label;
  Tracked mark;
  int32_t scale;
public:
  Item(int32_t scale) : label(std::string("LABEL")), mark(Tracked(scale)), scale(scale) {}
  int32_t tag() override { return 5; }
  int64_t check() override { return check_text(this.label) + int64_t(this.mark.value()) * int64_t(this.scale) + int64_t(this.tag()) - 5; }
};
""", "Item")
    if shape == "concrete":
        return ("""[[unsafe_transfer]] class Root : public Labeled {
  int32_t base;
public:
  Root(int32_t base) : base(base) {}
  virtual int32_t extra() { return 100; }
  int32_t base_value() { return this.base; }
  int64_t check() override { return int64_t(this.base_value()) + int64_t(this.extra()); }
};
[[unsafe_transfer]] class Item : public Root {
  std::string label;
  Tracked mark;
  int32_t scale;
public:
  Item(int32_t scale) : Root(3), label(std::string("LABEL")), mark(Tracked(scale)), scale(scale) {}
  int32_t extra() override { return 0; }
  int64_t check() override { return check_text(this.label) + int64_t(this.mark.value()) * int64_t(this.scale) + int64_t(this.base_value()) - 3 + int64_t(this.extra()); }
};
""", "Item")
    if shape == "generic":
        return ("""[[unsafe_transfer]] class Generic<T> : public Labeled {
  T label;
  Tracked mark;
  int32_t scale;
public:
  Generic(T label, int32_t scale) : label(std::move(label)), mark(Tracked(scale)), scale(scale) {}
  int64_t check() override { return check_text(this.label) + int64_t(this.mark.value()) * int64_t(this.scale); }
};
using Item = Generic<std::string>;
""", "Item")
    raise ValueError(shape)


def make(shape, scale):
    if shape == "generic":
        return f'Item(std::string("{LABEL}"), {scale})'
    return f"Item({scale})"


def expected(scale):
    return text_sum(LABEL) + scale * scale


def operation(op, shape):
    m = lambda s: make(shape, s)  # noqa: E731
    if op == "byvalue":
        return ("int64_t take(Item item) { Labeled& view = item; return view.check(); }\n",
                f"""  mut Item local = {m(2)};
  int64_t got = take(std::move(local)) * 3 + take({m(3)});""", expected(2) * 3 + expected(3))
    if op == "move":
        return ("", f"""  mut Item first = {m(2)};
  mut Item second = std::move(first);
  Labeled& view = second;
  int64_t got = view.check();""", expected(2))
    if op == "return":
        return (f"""Item build(int32_t scale) {{
  mut Item local = {m(4)};
  if (scale == 4) return local;
  return {m(5)};
}}
""", """  mut Item a = build(4);
  mut Item b = build(5);
  int64_t got = a.check() * 3 + b.check();""", expected(4) * 3 + expected(5))
    if op == "unique":
        return ("", f"""  std::unique_ptr<Labeled> owner = std::upcast_unique<Labeled, Item>(std::make_unique<Item>({m(2)}));
  std::unique_ptr<Labeled> moved = std::move(owner);
  int64_t got = moved->check();""", expected(2))
    if op == "vector_unique":
        return ("", f"""  mut std::vector<std::unique_ptr<Labeled>> items{{}};
  for (mut int32_t i = 1; i < 4; i += 1) items.push_back(std::upcast_unique<Labeled, Item>(std::make_unique<Item>({make(shape, "i")})));
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < items.size(); k += std::size_t(1)) got = got * 3 + items[k]->check();""",
                (expected(1) * 3 + expected(2)) * 3 + expected(3))
    if op == "slot":
        return ("", f"""  mut std::inline_polymorphic<Labeled, 128, 8> slot{{}};
  slot.replace({m(2)});
  int64_t first = slot.get().check();
  slot.replace({m(3)});
  mut std::inline_polymorphic<Labeled, 128, 8> moved = std::move(slot);
  int64_t got = first * 3 + moved.get().check();""", expected(2) * 3 + expected(3))
    if op == "field":
        return ("""class Outer {
public:
  Item item;
  int32_t extra;
  Outer(Item item) : item(std::move(item)), extra(1) {}
};
""", f"""  mut Outer outer = Outer({m(2)});
  mut Outer moved = std::move(outer);
  Labeled& view = moved.item;
  int64_t got = view.check() + int64_t(moved.extra) - 1;""", expected(2))
    if op == "lambda":
        return ("", f"""  mut Item local = {m(2)};
  auto reader = [held = std::move(local)]() -> int64_t {{
    mut int64_t total = 0;
    total += held.check();
    return total;
  }};
  int64_t got = reader();
  if (reader() != got) return 5;""", expected(2))
    if op == "thread":
        return ("", f"""  mut Item local = {m(2)};
  mut std::thread_result<int64_t> worker{{}};
  if (!worker.start([held = std::move(local)]() -> int64_t {{
        return held.check();
      }})) return 6;
  auto joined = worker.join();
  if (!joined) return 7;
  int64_t got = std::move(joined).value();""", expected(2))
    if op == "array":
        return ("", f"""  mut Item[3] items = {{{m(1)}, {m(2)}, {m(3)}}};
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < std::size_t(3); k += std::size_t(1)) {{
    Labeled& view = items[k];
    got = got * 3 + view.check();
  }}""", (expected(1) * 3 + expected(2)) * 3 + expected(3))
    if op == "shared":
        return ("", f"""  std::shared_ptr<Item> owner = std::make_shared<Item>({m(2)});
  std::shared_ptr<Item> second = owner.clone();
  int64_t got = owner->check() * 3 + second->check();""", expected(2) * 3 + expected(2))
    raise ValueError(op)


OPS = ["byvalue", "move", "return", "unique", "vector_unique", "slot", "field", "lambda", "thread", "array", "shared"]


def program(shape, op):
    source = PRELUDE + COMMON
    shape_source, _ = shapes(shape)
    source += shape_source.replace("LABEL", LABEL) + "\n"
    helpers, body, want = operation(op, shape)
    source += helpers + "\n"
    source += f"""int32_t run() {{
{body}
  if (got != {want}) {{
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
    return source


def cells():
    for shape, op in itertools.product(["iface", "dual", "concrete"], OPS):
        yield f"{shape}__{op}", program(shape, op)
