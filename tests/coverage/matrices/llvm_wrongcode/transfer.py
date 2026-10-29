"""Matrix 1: owned value kind x transfer x class shape, self-checking.

Each program computes a checksum of its owned value after the transfer and
compares it with a constant derived here from GTI semantics (the literal
inputs), returning a distinct non-zero code on any mismatch. Destructor
accounting checks that every tracked object is destroyed exactly once.
"""
import itertools

TEXT = "wrong-code hunt payload, long enough to live on the heap"
VEC = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5]
CELL = 41
TRACK = 17


def text_sum(s):
    total = 0
    for i, c in enumerate(s):
        if c == "o":
            total += i + 1
        if c == "e":
            total += (i + 1) * 2
    return len(s) * 10000 + total


TEXT_CHECK = """  mut int64_t sum = 0;
  for (mut std::size_t i = std::size_t(0); i < BODY.size(); i += std::size_t(1)) {
    if (BODY[i] == 'o') sum += int64_t(i) + 1;
    if (BODY[i] == 'e') sum += (int64_t(i) + 1) * 2;
  }
  return int64_t(BODY.size()) * 10000 + sum;"""


VALUES = {
    "string": dict(
        type="std::string",
        make=f'std::string("{TEXT}")',
        check="int64_t check_v(std::string& v) {\n" + TEXT_CHECK.replace("BODY", "v") + "\n}",
        expected=text_sum(TEXT)),
    "vector": dict(
        type="std::vector<int32_t>",
        make="make_vector()",
        helper="std::vector<int32_t> make_vector() {\n  mut std::vector<int32_t> v{};\n" +
               "".join(f"  v.push_back({x});\n" for x in VEC) + "  return v;\n}",
        check="""int64_t check_v(std::vector<int32_t>& v) {
  mut int64_t sum = 0;
  for (mut std::size_t i = std::size_t(0); i < v.size(); i += std::size_t(1)) sum += int64_t(v[i]) * int64_t(i + std::size_t(1));
  return int64_t(v.size()) * 1000 + sum;
}""",
        expected=len(VEC) * 1000 + sum(x * (i + 1) for i, x in enumerate(VEC))),
    "unique": dict(
        type="std::unique_ptr<Cell>",
        make=f"std::make_unique<Cell>({CELL})",
        check="""int64_t check_v(std::unique_ptr<Cell>& v) {
  if (v == nullptr) return -1;
  return int64_t(v->value) * 3;
}""",
        expected=CELL * 3),
    "tracked": dict(
        type="Tracked",
        make=f"Tracked({TRACK})",
        check="""int64_t check_v(Tracked& v) { return int64_t(v.value()) * 7; }""",
        expected=TRACK * 7),
    "payload": dict(
        type="Packet",
        make=f'Packet::text(std::string("{TEXT}"))',
        check="int64_t check_text(std::string& body) {\n" + TEXT_CHECK.replace("BODY", "body") + "\n}\n" + """int64_t check_v(Packet& v) {
  switch (v) {
  case Packet::text(auto& body): return check_text(body);
  case Packet::none: return -1;
  }
}""",
        expected=text_sum(TEXT)),
}

PRELUDE = """import std.string;
import std.vector;
import std.memory;
import std.thread;
import std.atomic;
import std.format;

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

class Cell {
public:
  int32_t value;
  Cell(int32_t value) : value(value) {}
};

enum Packet { text(std::string body), none };
"""

TAG = 7


def shape_source(shape, vtype):
    if shape == "plain":
        return f"""class Holder {{
public:
  {vtype} v;
  int32_t tag;
  Holder({vtype} v, int32_t tag) : v(std::move(v)), tag(tag) {{}}
}};
int64_t check_holder(Holder& h) {{ return check_v(h.v) + int64_t(h.tag); }}
Holder make_holder() {{ return Holder(make_value(), {TAG}); }}
"""
    if shape == "derived":
        return f"""class HolderBase {{
public:
  {vtype} v;
  HolderBase({vtype} v) : v(std::move(v)) {{}}
}};
class Holder : public HolderBase {{
public:
  int32_t tag;
  Holder({vtype} v, int32_t tag) : HolderBase(std::move(v)), tag(tag) {{}}
}};
int64_t check_holder(Holder& h) {{ return check_v(h.v) + int64_t(h.tag); }}
Holder make_holder() {{ return Holder(make_value(), {TAG}); }}
"""
    if shape == "polymorphic":
        return f"""[[transfer]] interface Checker {{
  int64_t check();
}};
class Holder : public Checker {{
public:
  {vtype} v;
  int32_t tag;
  Holder({vtype} v, int32_t tag) : v(std::move(v)), tag(tag) {{}}
  int64_t check() override {{ return check_v(this.v) + int64_t(this.tag); }}
}};
int64_t check_holder(Holder& h) {{
  Checker& view = h;
  int64_t direct = check_v(h.v) + int64_t(h.tag);
  return view.check() == direct ? direct : int64_t(-2);
}}
Holder make_holder() {{ return Holder(make_value(), {TAG}); }}
"""
    if shape == "generic":
        return f"""class Box<T> {{
public:
  T v;
  int32_t tag;
  Box(T v, int32_t tag) : v(std::move(v)), tag(tag) {{}}
}};
using Holder = Box<{vtype}>;
int64_t check_holder(Holder& h) {{ return check_v(h.v) + int64_t(h.tag); }}
Holder make_holder() {{ return Holder(make_value(), {TAG}); }}
"""
    raise ValueError(shape)


def transfer_source(transfer, shape):
    # Returns (helpers, body) where body computes `got` from a Holder.
    if transfer == "byvalue":
        return ("int64_t take(Holder h) { return check_holder(h); }\n",
                """  mut Holder local = make_holder();
  int64_t got = take(std::move(local));
  int64_t fresh = take(make_holder());
  if (fresh != got) return 3;""")
    if transfer == "move":
        return ("", """  mut Holder first = make_holder();
  mut Holder second = std::move(first);
  mut Holder third = std::move(second);
  int64_t got = check_holder(third);""")
    if transfer == "return":
        return ("""Holder relay() {
  mut Holder local = make_holder();
  return local;
}
Holder relay_twice() { return relay(); }
""", """  mut Holder held = relay_twice();
  int64_t got = check_holder(held);""")
    if transfer == "field":
        return ("""class Outer {
public:
  Holder inner;
  int32_t extra;
  Outer(Holder inner) : inner(std::move(inner)), extra(1) {}
};
""", """  mut Outer outer = Outer(make_holder());
  int64_t got = check_holder(outer.inner);
  if (outer.extra != 1) return 4;""")
    if transfer == "base":
        base = "Box<VTYPE>" if shape == "generic" else "Holder"
        return (f"""class Wrapper : public {base} {{
public:
  int32_t extra;
  Wrapper(VTYPE v) : {base}(std::move(v), {TAG}), extra(1) {{}}
}};
""", """  mut Wrapper wrapped = Wrapper(make_value());
  int64_t got = check_holder(wrapped);
  if (wrapped.extra != 1) return 4;""")
    if transfer == "base_fresh":
        # The owned argument is created in the base initializer itself.
        base = "Box<VTYPE>" if shape == "generic" else "Holder"
        return (f"""class Wrapper : public {base} {{
public:
  int32_t extra;
  Wrapper(int32_t extra) : {base}(make_value(), {TAG}), extra(extra) {{}}
}};
""", """  mut Wrapper wrapped = Wrapper(1);
  int64_t got = check_holder(wrapped);
  if (wrapped.extra != 1) return 4;""")
    if transfer == "lambda":
        return ("", """  mut Holder local = make_holder();
  auto reader = [held = std::move(local)]() -> int64_t {
    mut int64_t total = 0;
    total += check_holder(held);
    return total;
  };
  int64_t got = reader();
  if (reader() != got) return 5;""")
    if transfer == "thread":
        return ("", """  mut Holder local = make_holder();
  mut std::thread_result<int64_t> worker{};
  if (!worker.start([held = std::move(local)]() -> int64_t {
        return check_holder(held);
      })) return 6;
  auto joined = worker.join();
  if (!joined) return 7;
  int64_t got = std::move(joined).value();""")
    raise ValueError(transfer)


def program(value, transfer, shape):
    spec = VALUES[value]
    source = PRELUDE
    if "helper" in spec:
        source += spec["helper"] + "\n"
    source += spec["check"] + "\n"
    source += f"{spec['type']} make_value() {{ return {spec['make']}; }}\n\n"
    source += shape_source(shape, spec["type"]) + "\n"
    helpers, body = transfer_source(transfer, shape)
    source += helpers.replace("VTYPE", spec["type"]) + "\n"
    expected = spec["expected"] + TAG
    source += f"""int32_t run() {{
{body}
  if (got != {expected}) {{
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


TRANSFERS = ["byvalue", "move", "return", "field", "base", "base_fresh",
             "lambda", "thread"]
SHAPES = ["plain", "derived", "polymorphic", "generic"]


def cells():
    for value, transfer, shape in itertools.product(VALUES, TRANSFERS, SHAPES):
        yield f"{value}__{transfer}__{shape}", program(value, transfer, shape)
