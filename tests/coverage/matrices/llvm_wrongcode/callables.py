"""Matrix 7: callable holders with owned captures x operation."""
import itertools

from .transfer import PRELUDE, TEXT, text_sum, TEXT_CHECK

COMMON = """#include <std/functional>
#include <std/utility>
int64_t check_text(std::string& body) {
""" + TEXT_CHECK.replace("BODY", "body") + """
}
"""

# make(i) returns a callable C whose call yields value(i); type alias C.
KINDS = {
    "fn_string": ("""using C = std::function<int64_t()>;
C make(int32_t i) {
  std::string text = std::string("TEXT");
  return C([text, i]() -> int64_t { mut std::string copy = text; return check_text(copy) * 3 + int64_t(i); });
}
""", lambda i: text_sum(TEXT) * 3 + i),
    "transfer_tracked": ("""using C = std::transfer_function<int64_t()>;
class Probe {
  Tracked item;
  std::string text;
public:
  Probe(int32_t i) : item(Tracked(i)), text(std::string("TEXT")) {}
  int64_t operator()() { mut int64_t total = 0; total += int64_t(this.item.value()) * 5; return total + int64_t(this.text.size()); }
};
C make(int32_t i) { return C(Probe(i)); }
""", lambda i: i * 5 + len(TEXT)),
    "transfer_unique": ("""using C = std::transfer_function<int64_t() mut>;
class Probe {
  std::unique_ptr<Cell> cell;
  mut int32_t calls = 0;
public:
  Probe(int32_t i) : cell(std::make_unique<Cell>(i * 3)) {}
  int64_t operator()() mut { this.calls += 1; return int64_t(this.cell->value) * 7 + int64_t(this.calls) - 1; }
};
C make(int32_t i) { return C(Probe(i)); }
""", lambda i: i * 3 * 7),
    "once_tracked": ("""using C = std::transfer_function<int64_t() &&>;
class Probe {
  Tracked item;
public:
  Probe(int32_t i) : item(Tracked(i)) {}
  int64_t operator()() && { return int64_t(this.item.value()) * 11; }
};
C make(int32_t i) { return C(Probe(i)); }
""", lambda i: i * 11),
}

ONCE_CALL = {"once_tracked": "std::move({v})()"}


def call(kind, var):
    return ONCE_CALL.get(kind, "{v}()").format(v=var)


def ops(kind):
    c = lambda v: call(kind, v)  # noqa: E731
    once = kind == "once_tracked"
    mut_prefix = "mut "
    return {
        "call_move": ("", f"""  {mut_prefix}C a = make(2);
  {mut_prefix}C b = std::move(a);
  int64_t got = {c("b")};""", lambda X: X(2)),
        "byvalue": (f"int64_t take(C fn) {{ {mut_prefix}C local = std::move(fn); return {c('local')}; }}\n", """  mut C a = make(2);
  int64_t got = take(std::move(a)) * 3 + take(make(3));""", lambda X: X(2) * 3 + X(3)),
        "return": (f"""C relay(int32_t i) {{
  {mut_prefix}C local = make(i);
  return local;
}}
""", f"""  {mut_prefix}C a = relay(4);
  int64_t got = {c("a")};""", lambda X: X(4)),
        "vector": ("", f"""  mut std::vector<C> items{{}};
  for (mut int32_t i = 1; i < 4; i += 1) items.push_back(make(i));
  {mut_prefix}C taken = std::exchange(items[std::size_t(1)], make(5));
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < items.size(); k += std::size_t(1)) {{
    {mut_prefix}C fn = std::exchange(items[k], make(9));
    got = got * 3 + {c("fn")};
  }}
  got = got * 3 + {c("taken")};""", lambda X: ((X(1) * 3 + X(5)) * 3 + X(3)) * 3 + X(2)),
        "field": (f"""class Holder {{
public:
  mut C fn;
  int32_t id;
  Holder(C fn, int32_t id) : fn(std::move(fn)), id(id) {{}}
}};
""", f"""  mut Holder h = Holder(make(2), 1);
  mut Holder moved = std::move(h);
  {mut_prefix}C fn = std::exchange(moved.fn, make(9));
  int64_t got = {c("fn")} + int64_t(moved.id) - 1;""", lambda X: X(2)),
        "replace": ("", f"""  mut C a = make(1);
  a = make(4);
  a = make(5);
  int64_t got = {c("a")};""", lambda X: X(5)),
        "thread": ("", f"""  mut C a = make(2);
  mut std::thread_result<int64_t> worker{{}};
  if (!worker.start(std::move(a))) return 6;
  auto joined = worker.join();
  if (!joined) return 7;
  int64_t got = std::move(joined).value();""", lambda X: X(2)),
    }


def program(kind, op):
    kind_source, X = KINDS[kind]
    helpers, body, want = ops(kind)[op]
    source = PRELUDE + COMMON + kind_source.replace("TEXT", TEXT) + "\n" + helpers + "\n"
    source += f"""int32_t run() {{
{body}
  if (got != {want(X)}) {{
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
    for kind in KINDS:
        for op in ops(kind):
            yield f"{kind}__{op}", program(kind, op)
