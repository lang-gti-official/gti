"""Matrix 10: standard-library containers holding owned values.

Ordered and hashed maps, sets, optionals and deques of strings, vectors and
tracked objects, the shapes ordinary programs use. Each program checks its
result against a value computed here from the literal inputs, and counts
tracked destructions.
"""

PRELUDE = """import std.map;
import std.unordered_map;
import std.set;
import std.optional;
import std.deque;
import std.vector;
import std.string;
import std.format;
import std.atomic;
import std.utility;
std::atomic<int32_t> constructed{0};
std::atomic<int32_t> destroyed{0};
class Tracked {
  int32_t id;
public:
  Tracked(int32_t id) : id(id) { [[discard]] constructed.fetch_add(1); }
  Tracked(Tracked& other) = delete;
  Tracked(Tracked&& other) = default;
  ~Tracked() { [[discard]] destroyed.fetch_add(1); }
  int32_t value() { return this.id; }
};
int64_t text_code(std::string& text) {
  mut int64_t sum = 0;
  for (mut std::size_t i = std::size_t(0); i < text.size(); i += std::size_t(1))
    sum = (sum * 31 + int64_t(uint8_t(text[i]))) % 1000000007;
  return sum;
}
"""


def text_code(text):
    total = 0
    for ch in text:
        total = (total * 31 + ord(ch)) % 1000000007
    return total


WORDS = ["delta", "alpha", "charlie", "bravo", "alpha", "delta", "delta"]


def map_index_expected():
    index = {}
    for i, word in enumerate(WORDS):
        index.setdefault(word, []).append(i)

    def digest(table):
        total = 0
        for key in sorted(table):
            inner = 0
            for value in table[key]:
                inner = inner * 10 + value
            total = (total * 1000 + len(key) * 100 + inner) % 1000000007
        return total

    first = digest(index)
    del index["charlie"]
    index["bravo"] = [99]
    return first, digest(index)


def map_index():
    first, second = map_index_expected()
    words = ", ".join(f'std::string("{w}")' for w in WORDS)
    return f"""int64_t digest(std::map<std::string, std::vector<int32_t>>& index) {{
  mut int64_t total = 0;
  for (auto& entry : index) {{
    mut int64_t inner = 0;
    for (mut std::size_t k = std::size_t(0); k < entry.second.size(); k += std::size_t(1))
      inner = inner * 10 + int64_t(entry.second[k]);
    total = (total * 1000 + int64_t(entry.first.size()) * 100 + inner) % 1000000007;
  }}
  return total;
}}
int32_t run() {{
  std::string[{len(WORDS)}] words = {{{words}}};
  mut std::map<std::string, std::vector<int32_t>> index{{}};
  for (mut int32_t i = 0; i < {len(WORDS)}; i += 1) {{
    std::string& word = words[std::size_t(i)];
    if (index.contains(word)) {{
      mut auto cursor = index.find_mut(word);
      cursor->mapped().push_back(i);
    }} else {{
      mut std::vector<int32_t> fresh{{}};
      fresh.push_back(i);
      [[discard]] index.insert(word.clone(), std::move(fresh));
    }}
  }}
  if (digest(index) != {first}) return 2;
  std::string gone = std::string("charlie");
  if (index.erase(gone) != std::size_t(1)) return 3;
  mut std::vector<int32_t> replacement{{}};
  replacement.push_back(99);
  [[discard]] index.insert_or_assign(std::string("bravo"), std::move(replacement));
  if (index.size() != std::size_t(3) || digest(index) != {second}) return 4;
  return 0;
}}
"""


def hashed_tracked():
    keys = range(1, 25)
    odd = [k for k in keys if k % 2 == 1]
    total = sum(k * 7 for k in odd) - 3 * 7 + 1000
    return f"""int32_t run() {{
  {{
    mut std::unordered_map<int32_t, Tracked> table{{}};
    for (mut int32_t k = 1; k < 25; k += 1) [[discard]] table.insert(k, Tracked(k * 7));
    for (mut int32_t k = 2; k < 25; k += 2) {{
      if (table.erase(k) != std::size_t(1)) return 2;
    }}
    [[discard]] table.insert_or_assign(3, Tracked(1000));
    mut int64_t total = 0;
    for (mut int32_t k = 1; k < 25; k += 2) total += int64_t(table.at(k).value());
    if (table.size() != std::size_t({len(odd)}) || total != {total}) return 3;
    mut int64_t walked = 0;
    for (auto& entry : table) walked += int64_t(entry.second.value());
    if (walked != {total}) return 4;
  }}
  if (constructed.load() != 25 || destroyed.load() != 25) return 5;
  return 0;
}}
"""


def optional_owned():
    long_text = "an optional string long enough for the heap"
    want = sum(text_code(long_text) for i in range(6) if i % 2 == 0) + 7 * 3
    return f"""std::optional<std::string> pick(int32_t i) {{
  if (i % 2 == 0) return std::string("{long_text}");
  return std::nullopt;
}}
std::optional<Tracked> track(int32_t i) {{
  if (i % 3 == 0) return Tracked(7);
  return std::nullopt;
}}
int32_t run() {{
  {{
    mut int64_t total = 0;
    mut std::vector<std::optional<std::string>> kept{{}};
    for (mut int32_t i = 0; i < 6; i += 1) {{
      mut std::optional<std::string> chosen = pick(i);
      if (chosen.has_value()) total += text_code(chosen.value());
      kept.push_back(std::move(chosen));
    }}
    mut std::optional<Tracked> slot{{}};
    for (mut int32_t i = 0; i < 9; i += 1) {{
      mut std::optional<Tracked> next = track(i);
      if (next) total += int64_t(next.value().value());
      slot = std::move(next);
    }}
    slot.reset();
    if (total != {want} || kept.size() != std::size_t(6) || kept[std::size_t(1)].has_value()) return 2;
  }}
  if (constructed.load() != 3 || destroyed.load() != 3) return 3;
  return 0;
}}
"""


def deque_strings():
    items = []
    texts = [f"deque entry number {i} with padding" for i in range(8)]
    for i, text in enumerate(texts):
        if i % 2 == 0:
            items.append(text)
        else:
            items.insert(0, text)
    items.pop(0)
    items.pop()
    del items[1]
    items.insert(1, "inserted entry, long enough to allocate")
    total = 0
    for text in items:
        total = (total * 7 + text_code(text)) % 1000000007
    pushes = "\n".join(
        f'    q.push_{"back" if i % 2 == 0 else "front"}(std::string("{t}"));'
        for i, t in enumerate(texts))
    return f"""int32_t run() {{
  mut std::deque<std::string> q{{}};
{pushes}
  q.pop_front();
  q.pop_back();
  q.erase(std::size_t(1));
  q.insert(std::size_t(1), std::string("inserted entry, long enough to allocate"));
  mut int64_t total = 0;
  for (auto& text : q) total = (total * 7 + text_code(text)) % 1000000007;
  if (q.size() != std::size_t({len(items)}) || total != {total}) return 2;
  return 0;
}}
"""


def set_strings():
    words = ["pear", "apple", "fig", "apple", "kiwi", "pear", "banana"]
    ordered = sorted(set(words))
    total = 0
    for text in ordered:
        total = (total * 11 + text_code(text)) % 1000000007
    inserts = "\n".join(f'  [[discard]] names.insert(std::string("{w}"));' for w in words)
    return f"""int32_t run() {{
  mut std::set<std::string> names{{}};
{inserts}
  mut int64_t total = 0;
  for (auto& text : names) total = (total * 11 + text_code(text)) % 1000000007;
  std::string probe = std::string("fig");
  if (names.size() != std::size_t({len(ordered)}) || total != {total} || !names.contains(probe)) return 2;
  return 0;
}}
"""


def string_building():
    parts = [f"segment-{i}" for i in range(12)]
    joined = "/".join(parts)
    formatted = f"{42}:{joined}:{-7}"
    return f"""int32_t run() {{
  mut std::string joined{{}};
  for (mut int32_t i = 0; i < 12; i += 1) {{
    if (i > 0) joined.push_back('/');
    std::string part = std::format("segment-{{}}", i);
    joined.append(part.view());
  }}
  if (joined.size() != std::size_t({len(joined)}) || text_code(joined) != {text_code(joined)}) return 2;
  if (!joined.starts_with("segment-0/") || !joined.ends_with("/segment-11")) return 3;
  if (joined.find("segment-7") != std::size_t({joined.find("segment-7")})) return 4;
  int32_t answer = 42;
  int32_t negative = -7;
  std::string formatted = std::format("{{}}:{{}}:{{}}", answer, joined.view(), negative);
  if (text_code(formatted) != {text_code(formatted)} || formatted.size() != std::size_t({len(formatted)})) return 5;
  return 0;
}}
"""


PROGRAMS = {
    "map_index": map_index,
    "hashed_tracked": hashed_tracked,
    "optional_owned": optional_owned,
    "deque_strings": deque_strings,
    "set_strings": set_strings,
    "string_building": string_building,
}


def program(name):
    return PRELUDE + PROGRAMS[name]() + """
int main() {
  int32_t status = run();
  if (status != 0) {
    std::println("status {}", status);
    return status;
  }
  std::println("ok");
  return 0;
}
"""


def cells():
    for name in PROGRAMS:
        yield name, program(name)
