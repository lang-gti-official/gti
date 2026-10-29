"""Matrix 17: threads with results and mutex-protected state x payload.

Workers return class, text, polymorphic-owner and payload-enum results through
`std::thread_result`, update `std::mutex`-protected state shared through
`std::shared_ptr` or borrowed by `std::parallel_invoke`, and are kept in
vectors and joined in loops.
"""
import itertools

PRELUDE = """import std.thread;
import std.mutex;
import std.memory;
import std.vector;
import std.string;
import std.functional;
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
  int64_t score();
};
[[unsafe_transfer]] class Square : public Shape {
  Tracked mark;
public:
  Square(int32_t side) : mark(Tracked(side)) {}
  int64_t score() override { return int64_t(this.mark.value()) * int64_t(this.mark.value()); }
};
[[unsafe_transfer]] class Record {
public:
  std::string name;
  Tracked mark;
  Record(int32_t i) : name(std::string("a record name long enough")), mark(Tracked(i)) {}
};
enum Outcome { done(Record record), skipped(int32_t reason) };
"""

# Payload kinds a worker produces from id i: (type, construct, digest of v,
# expected digest).
PAYLOADS = {
    "record": ("Record", "Record(i)",
               "int64_t(v.name.size()) * 100 + int64_t(v.mark.value())",
               lambda i: len("a record name long enough") * 100 + i),
    "text": ("std::string", 'std::string("worker text long enough")',
             "int64_t(v.size())", lambda i: len("worker text long enough")),
    "owner": ("std::unique_ptr<Shape>",
              "std::upcast_unique<Shape, Square>(std::make_unique<Square>(i))",
              "v->score()", lambda i: i * i),
    "outcome": ("Outcome", "Outcome::done(Record(i))",
                "digest_outcome(v)",
                lambda i: len("a record name long enough") * 100 + i),
}

DIGEST_OUTCOME = """int64_t digest_outcome(Outcome& outcome) {
  switch (outcome) {
  case Outcome::done(auto& record): return int64_t(record.name.size()) * 100 + int64_t(record.mark.value());
  case Outcome::skipped(auto& reason): return -int64_t(reason);
  }
}
"""

PATTERNS = {
    # One worker returns a value built from captured input.
    "result": ("""  mut std::thread_result<P> worker{};
  int32_t i = 3;
  if (!worker.start([i]() -> P { return MAKE; })) return 6;
  auto joined = worker.join();
  if (!joined) return 7;
  P v = std::move(joined).value();
  int64_t got = DIGEST;""", lambda D: D(3)),
    # A worker owns a moved input and reports on it.
    "consume": ("""  int32_t i = 4;
  mut P input = MAKE;
  mut std::thread_result<int64_t> worker{};
  if (!worker.start([held = std::move(input)]() -> int64_t {
        P& v = held;
        return DIGEST;
      })) return 6;
  auto joined = worker.join();
  if (!joined) return 7;
  int64_t got = std::move(joined).value();""", lambda D: D(4)),
    # Three workers run at once and are joined in order.
    "trio": ("""  mut std::thread_result<P> first{};
  mut std::thread_result<P> second{};
  mut std::thread_result<P> third{};
  int32_t a = 1;
  int32_t b = 2;
  int32_t c = 3;
  if (!first.start([a]() -> P { int32_t i = a; return MAKE; })) return 6;
  if (!second.start([b]() -> P { int32_t i = b; return MAKE; })) return 6;
  if (!third.start([c]() -> P { int32_t i = c; return MAKE; })) return 6;
  mut int64_t got = 0;
  {
    auto joined = first.join();
    if (!joined) return 7;
    P v = std::move(joined).value();
    got = got * 1000 + DIGEST;
  }
  {
    auto joined = second.join();
    if (!joined) return 7;
    P v = std::move(joined).value();
    got = got * 1000 + DIGEST;
  }
  {
    auto joined = third.join();
    if (!joined) return 7;
    P v = std::move(joined).value();
    got = got * 1000 + DIGEST;
  }""", lambda D: (D(1) * 1000 + D(2)) * 1000 + D(3)),
    # Workers store results into mutex-protected shared state.
    "shared_mutex": ("""  auto state = std::make_shared<std::mutex<std::vector<int64_t>>>(std::vector<int64_t>());
  auto spawn = [](std::shared_ptr<std::mutex<std::vector<int64_t>>> shared, int32_t i) -> expected<std::thread, std::thread_errc> {
    return std::thread::start([held = std::move(shared), i]() -> void {
      P v = MAKE;
      int64_t value = DIGEST;
      mut auto guard = held->lock();
      guard.get().push_back(value);
    });
  };
  auto first_started = spawn(state.clone(), 1);
  auto second_started = spawn(state.clone(), 2);
  if (!first_started || !second_started) return 6;
  mut std::thread first = std::move(first_started).value();
  mut std::thread second = std::move(second_started).value();
  if (!first.join() || !second.join()) return 7;
  mut int64_t got = 0;
  {
    auto guard = state->lock();
    for (auto& value : guard.get()) got += value;
  }""", lambda D: D(1) + D(2)),
    # Scoped tasks update disjoint mutex-protected slots borrowed by reference.
    "parallel_invoke": ("""class Fill {
  std::mutex<int64_t>& slot;
  int32_t i;
public:
  Fill(std::mutex<int64_t>& slot, int32_t i) : slot(slot), i(i) {}
  void operator()() {
    int32_t i = this.i;
    P v = MAKE;
    mut auto guard = this.slot.lock();
    guard.get() = DIGEST;
  }
};
SPLIT  std::mutex<int64_t> first{int64_t(0)};
  std::mutex<int64_t> second{int64_t(0)};
  if (!std::parallel_invoke(Fill(first, 1), Fill(second, 2))) return 6;
  mut int64_t got = 0;
  {
    auto a = first.lock();
    auto b = second.lock();
    got = a.get() * 1000 + b.get();
  }""", lambda D: D(1) * 1000 + D(2)),
}


def program(payload, pattern):
    type_name, make, digest, expected = PAYLOADS[payload]
    body, want = PATTERNS[pattern]
    helpers = ""
    if "SPLIT" in body:
        helpers, body = body.split("SPLIT")
    body = body.replace("MAKE", make).replace("DIGEST", digest)
    helpers = helpers.replace("MAKE", make).replace("DIGEST", digest)
    return (PRELUDE + DIGEST_OUTCOME + f"using P = {type_name};\n" + helpers + f"""int32_t run() {{
{body}
  if (got != {want(expected)}) {{
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
""")


def cells():
    for payload, pattern in itertools.product(PAYLOADS, PATTERNS):
        # An owning payload enum cannot be a by-value lambda result yet
        # (GTI-S2067); it still crosses threads inside a worker.
        if payload == "outcome" and pattern in {"result", "trio"}:
            continue
        yield f"{payload}__{pattern}", program(payload, pattern)
