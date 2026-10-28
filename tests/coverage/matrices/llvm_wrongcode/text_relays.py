"""Matrix 14: text views and spans relayed through methods, virtuals,
interfaces, generics, operators and nested members x consumer.

Each relay returns a `std::string_view` or `std::span<int32_t>` borrowed from
an owning object; the consumer digests it while the owner is alive.
"""
import itertools

PRELUDE = """#include <std/string>
#include <std/vector>
#include <std/span>
#include <std/memory>
#include <std/format>
#include <std/utility>
int64_t digest(std::string_view v) {
  mut int64_t e = 0;
  for (mut std::size_t i = std::size_t(0); i < v.size(); i += std::size_t(1)) {
    if (v[i] == 'e') e += 1;
  }
  return int64_t(v.size()) * 100 + e;
}
int64_t sum(std::span<int32_t> s) {
  mut int64_t total = 0;
  for (mut std::size_t i = std::size_t(0); i < s.size(); i += std::size_t(1)) total = total * 10 + int64_t(s[i]);
  return total;
}
std::vector<int32_t> numbers(int32_t i) {
  mut std::vector<int32_t> values{};
  values.push_back(i);
  values.push_back(i + 1);
  values.push_back(i + 2);
  return values;
}
"""

TEXT = "seventeen letters"
PART = "the second part here"


def text_digest(text):
    return len(text) * 100 + text.count("e")


def span_sum(i):
    return (i * 10 + i + 1) * 10 + i + 2


# Each relay: (declarations, expression yielding the relayed value from an
# owner named `owner` built with id 3, consumer kind, expected digest).
RELAYS = {
    "method": ("""class Owner {
public:
  std::string text;
  Owner(int32_t i) : text(std::string("TEXT")) {}
  std::string_view name() { return this.text.view(); }
};
""", "owner.name()", "text", text_digest(TEXT)),
    "virtual": ("""class Labeled {
public:
  std::string base_text;
  Labeled() : base_text(std::string("base")) {}
  virtual std::string_view label() { return this.base_text.view(); }
};
class Owner : public Labeled {
public:
  std::string text;
  Owner(int32_t i) : Labeled(), text(std::string("TEXT")) {}
  std::string_view label() override { return this.text.view(); }
};
std::string_view relay(Labeled& view) { return view.label(); }
""", "relay(owner)", "text", text_digest(TEXT)),
    "interface": ("""interface Labeled {
  std::string_view label();
};
class Owner : public Labeled {
public:
  std::string text;
  Owner(int32_t i) : text(std::string("TEXT")) {}
  std::string_view label() override { return this.text.view(); }
};
std::string_view relay(Labeled& view) { return view.label(); }
""", "relay(owner)", "text", text_digest(TEXT)),
    "base_field": ("""class Base {
public:
  std::string text;
  Base(int32_t i) : text(std::string("TEXT")) {}
};
class Owner : public Base {
public:
  Owner(int32_t i) : Base(i) {}
  std::string_view name() { return this.text.view(); }
};
""", "owner.name()", "text", text_digest(TEXT)),
    "free_function": ("""class Owner {
public:
  std::string text;
  Owner(int32_t i) : text(std::string("TEXT")) {}
  std::string_view name() { return this.text.view(); }
};
std::string_view relay(Owner& holder) { return holder.name(); }
""", "relay(owner)", "text", text_digest(TEXT)),
    "operator": ("""class Owner {
public:
  std::vector<std::string> parts;
  Owner(int32_t i) : parts() {}
  std::string_view operator[](std::size_t index) { return this.parts[index].view(); }
};
""", "owner[std::size_t(1)]", "text", text_digest(PART)),
    "nested": ("""class Inner {
public:
  std::string text;
  Inner() : text(std::string("TEXT")) {}
  std::string_view name() { return this.text.view(); }
};
class Owner {
public:
  Inner inner;
  Owner(int32_t i) : inner(Inner()) {}
};
std::string_view relay(Owner& outer) { return outer.inner.name(); }
""", "relay(owner)", "text", text_digest(TEXT)),
    "span_method": ("""class Owner {
public:
  std::vector<int32_t> values;
  Owner(int32_t i) : values(numbers(i)) {}
  std::span<int32_t> view() { return this.values.as_span(); }
};
""", "owner.view()", "span", span_sum(3)),
    "span_virtual": ("""class Source {
public:
  std::vector<int32_t> base_values;
  Source() : base_values(numbers(0)) {}
  virtual std::span<int32_t> view() { return this.base_values.as_span(); }
};
class Owner : public Source {
public:
  std::vector<int32_t> values;
  Owner(int32_t i) : Source(), values(numbers(i)) {}
  std::span<int32_t> view() override { return this.values.as_span(); }
};
std::span<int32_t> relay(Source& source) { return source.view(); }
""", "relay(owner)", "span", span_sum(3)),
    "span_free": ("""class Owner {
public:
  std::vector<int32_t> values;
  Owner(int32_t i) : values(numbers(i)) {}
  std::span<int32_t> view() { return this.values.as_span(); }
};
std::span<int32_t> relay(Owner& holder) { return holder.view(); }
""", "relay(owner)", "span", span_sum(3)),
    "virtual_ref": ("""class Slots {
public:
  int32_t base_value;
  Slots() : base_value(0) {}
  virtual int32_t& slot() { return this.base_value; }
};
class Owner : public Slots {
public:
  int32_t value;
  Owner(int32_t i) : Slots(), value(i * 11) {}
  int32_t& slot() override { return this.value; }
};
int32_t& relay(Slots& slots) { return slots.slot(); }
""", "relay(owner)", "int", 33),
    "interface_ref": ("""interface Slots {
  int32_t& slot();
};
class Owner : public Slots {
public:
  int32_t value;
  Owner(int32_t i) : value(i * 11) {}
  int32_t& slot() override { return this.value; }
};
int32_t& relay(Slots& slots) { return slots.slot(); }
""", "relay(owner)", "int", 33),
    "virtual_record_ref": ("""class Record {
public:
  std::string name;
  Record() : name(std::string("TEXT")) {}
};
class Records {
public:
  Record base_record;
  Records() : base_record(Record()) {}
  virtual Record& record() { return this.base_record; }
};
class Owner : public Records {
public:
  Record own;
  Owner(int32_t i) : Records(), own(Record()) {}
  Record& record() override { return this.own; }
};
Record& relay(Records& records) { return records.record(); }
int64_t digest_record(Record& record) { return digest(record.name.view()); }
""", "relay(owner)", "record", text_digest(TEXT)),
}

# How the owner exists while the relayed value is consumed.
OWNERS = {
    "local": ("  mut Owner owner = Owner(3);\nFILL  int64_t got = CONSUME(EXPR);",),
    "unique": ("  mut std::unique_ptr<Owner> holder = std::make_unique<Owner>(Owner(3));\nFILL  int64_t got = CONSUME(EXPR);",),
    "vector": ("  mut std::vector<Owner> owners{};\n  owners.push_back(Owner(3));\n  mut Owner& owner = owners[std::size_t(0)];\nFILL  int64_t got = CONSUME(EXPR);",),
    "loop": ("  mut Owner owner = Owner(3);\nFILL  mut int64_t got = 0;\n  for (mut int32_t k = 0; k < 2; k += 1) got = CONSUME(EXPR);",),
}


def program(relay, owner):
    declarations, expression, kind, expected = RELAYS[relay]
    declarations = declarations.replace("TEXT", TEXT)
    fill = ""
    if relay == "operator":
        fill = (f'  owner.parts.push_back(std::string("first"));\n'
                f'  owner.parts.push_back(std::string("{PART}"));\n')
        declarations = declarations.replace("  std::vector<std::string> parts;", "  mut std::vector<std::string> parts;").replace(
            "Owner(int32_t i) : parts() {}", "Owner(int32_t i) {}")
    consume = {"text": "digest", "span": "sum", "int": "int64_t",
               "record": "digest_record"}[kind]
    if owner == "unique":
        # Reach the owner through its unique owner rather than a local.
        expression = (expression.replace("relay(owner)", "relay(*holder)")
                      .replace("owner[", "(*holder)[").replace("owner.", "holder->"))
        fill = fill.replace("owner.", "holder->")
    body = OWNERS[owner][0].replace("FILL", fill).replace("CONSUME", consume).replace("EXPR", expression)
    return PRELUDE + declarations + f"""int32_t run() {{
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
  std::println("ok");
  return 0;
}}
"""


def cells():
    for relay, owner in itertools.product(RELAYS, OWNERS):
        yield f"{relay}__{owner}", program(relay, owner)
