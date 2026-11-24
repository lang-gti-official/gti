"""Matrix: shared and weak ownership, expiry, cycles and cleanup order.

Contract (docs/language/ownership-and-lifetimes.md "Shared Ownership"):
`std::shared_ptr` adds strong owners only through `clone()`; the last strong
release destroys the payload exactly once; `std::weak_ptr` observers report
zero `use_count()` and `expired()` once the payload is gone, and `lock()`
returns an empty handle after expiry and a strong owner before it, which keeps
the payload alive. `upcast_shared` keeps one control block and one count.
Weak observation breaks ownership cycles; strong cycles are not collected
until a program breaks them itself. Each Item records its destruction, and
every cell compares the count and order with these rules.

Axes: holder of the first strong owner (a local, a vector element, a class
field, a moved-to local, a function result, an optional) x operation on it
(expire, lock extends, clone counts, upcast, weak clone, swap, thread
release). Fixed scenarios cover weak-broken cycles, strong cycles broken by
hand, destruction order along a shared chain, and observers stored in
containers.

Excluded shapes: weak dereference (the type has none); mutation through a
strong handle (dereference is read-only, so cycles are built through
`std::mutex` fields); `make_shared` allocation failure (an aborting report,
not a checked outcome).
"""

PRELUDE = """import std.memory;
import std.vector;
import std.optional;
import std.format;
import std.atomic;
import std.mutex;
import std.thread;
import std.utility;
import std.panic;
std::atomic<int32_t> drops{0};
std::atomic<int32_t> trail{0};
interface View { int32_t read(); };
class Item : public View {
  int32_t id;
public:
  Item(int32_t id) : id(id) {}
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  int32_t read() override { return this.id; }
  int32_t value() { return this.id; }
  ~Item() {
    [[discard]] drops.fetch_add(1);
    trail.store(trail.load() * 10 + this.id);
  }
};
class Holder {
public:
  mut std::shared_ptr<Item> owner;
  Holder(std::shared_ptr<Item> owner) : owner(std::move(owner)) {}
};
std::shared_ptr<Item> build(int32_t id) {
  auto spare = std::make_shared<Item>(9);
  auto made = std::make_shared<Item>(id);
  return made;
}
"""

# name: (setup, access, release, drops after setup)
HOLDERS = {
    "local": ("  mut auto owner = std::make_shared<Item>(VALUE);\n",
              "owner", "owner.reset();", 0),
    "vector": ("  mut std::vector<std::shared_ptr<Item>> owners{};\n"
               "  owners.push_back(std::make_shared<Item>(VALUE));\n",
               "owners[0]", "owners.clear();", 0),
    "field": ("  mut Holder holder = Holder(std::make_shared<Item>(VALUE));\n",
              "holder.owner", "holder.owner.reset();", 0),
    "moved": ("  mut auto first = std::make_shared<Item>(VALUE);\n"
              "  mut auto owner = std::move(first);\n",
              "owner", "owner = nullptr;", 0),
    "result": ("  mut auto owner = build(VALUE);\n", "owner", "owner.reset();", 1),
    "optional": ("  mut std::optional<std::shared_ptr<Item>> slot ="
                 " std::optional<std::shared_ptr<Item>>(std::make_shared<Item>(VALUE));\n",
                 "slot.value()", "slot = std::optional<std::shared_ptr<Item>>();", 0),
}

# Each operation uses A (the strong owner), R (its release) and B (drops
# before the operation), and returns a distinct status on each mismatch.
OPERATIONS = {
    "expire": """  mut auto weak = A.downgrade();
  if (weak.use_count() != std::size_t(1) || weak.expired()) return 1;
  {
    auto peek = weak.lock();
    if (!peek || peek->value() != VALUE || weak.use_count() != std::size_t(2)) return 2;
  }
  R
  if (drops.load() != B + 1 || !weak.expired() || weak.use_count() != std::size_t(0)) return 3;
  if (weak.lock()) return 4;
""",
    "lock_extends": """  mut auto weak = A.downgrade();
  {
    auto held = weak.lock();
    R
    if (drops.load() != B || held->read() != VALUE || weak.use_count() != std::size_t(1)) return 1;
  }
  if (drops.load() != B + 1 || !weak.expired()) return 2;
""",
    "clone_counts": """  mut auto c1 = A.clone();
  mut auto c2 = c1.clone();
  if (c2.use_count() != std::size_t(3) || A.use_count() != std::size_t(3)) return 1;
  R
  c1.reset();
  if (drops.load() != B || c2.use_count() != std::size_t(1) || c2->value() != VALUE) return 2;
  c2 = nullptr;
  if (drops.load() != B + 1 || c2) return 3;
""",
    "upcast": """  mut auto base = std::upcast_shared<View, Item>(A.clone());
  mut auto observer = base.downgrade();
  R
  if (drops.load() != B || base->read() != VALUE || base.use_count() != std::size_t(1)) return 1;
  auto promoted = observer.lock();
  if (!promoted || promoted->read() != VALUE || observer.use_count() != std::size_t(2)) return 2;
  base.reset();
  if (drops.load() != B || promoted.use_count() != std::size_t(1)) return 3;
""",
    "weak_clone": """  mut auto w1 = A.downgrade();
  mut auto w2 = w1.clone();
  mut std::weak_ptr<Item> w3{A};
  R
  w1.reset();
  if (drops.load() != B + 1 || !w2.expired() || !w3.expired() || w2.lock()) return 1;
  w2.swap(w3);
  w3 = nullptr;
  if (w2.use_count() != std::size_t(0) || drops.load() != B + 1) return 2;
""",
    "swap": """  mut auto other = std::make_shared<Item>(VALUE + 1);
  A.swap(other);
  if (A->value() != VALUE + 1 || other->value() != VALUE) return 1;
  R
  if (drops.load() != B + 1 || trail.load() % 10 != (VALUE + 1) % 10) return 2;
  other.reset();
  if (drops.load() != B + 2 || trail.load() % 10 != VALUE % 10) return 3;
""",
}

SCENARIOS = {
    # Strong and weak handles as payloads of an owning enum array (a
    # non-polymorphic payload: moving a polymorphic class into an enum is the
    # declared O-LLVM-10 limitation).
    "enum_payloads": ("""class Token {
  int32_t id;
public:
  Token(int32_t id) : id(id) {}
  Token(Token& other) = delete;
  Token(Token&& other) = default;
  int32_t value() { return this.id; }
  ~Token() {
    [[discard]] drops.fetch_add(1);
    trail.store(trail.load() * 10 + this.id);
  }
};
enum Msg { none, owned(Token m), strong(std::shared_ptr<Token> s), observer(std::weak_ptr<Token> w) };
int32_t code_of(Msg& msg) {
  switch (msg) {
  case Msg::owned(auto& m): return m.value();
  case Msg::strong(auto& s): return s ? s->value() + 100 : -1;
  case Msg::observer(auto& w): {
    auto held = w.lock();
    return held ? held->value() + 200 : -2;
  }
  case Msg::none: return 0;
  }
}
int32_t run() {
  {
    mut auto owner = std::make_shared<Token>(5);
    mut Msg items[3] = {Msg::strong(owner.clone()), Msg::observer(owner.downgrade()), Msg::owned(Token(7))};
    if (code_of(items[0]) != 105 || code_of(items[1]) != 205 || code_of(items[2]) != 7) return 1;
    owner.reset();
    if (drops.load() != 0) return 2;
    items[0] = Msg::none;
    if (drops.load() != 1 || trail.load() != 5 || code_of(items[1]) != -2) return 3;
    auto taken = std::move(items[2]);
    if (drops.load() != 1 || code_of(taken) != 7) return 4;
  }
  if (drops.load() != 2 || trail.load() != 57) return 5;
  return 0;
}
"""),
    "thread_release": """[[unsafe_transfer, unsafe_share]] class Plain {
  int32_t id;
public:
  Plain(int32_t id) : id(id) {}
  int32_t value() { return this.id; }
  ~Plain() { [[discard]] drops.fetch_add(1); }
};
int32_t run() {
  mut auto owner = std::make_shared<Plain>(5);
  mut auto weak = owner.downgrade();
  {
    auto moved = owner.clone();
    owner.reset();
    auto started = std::thread::start([moved = std::move(moved)]() -> void {
      if (moved->value() != 5) std::panic("wrong payload in worker");
    });
    if (!started) return 1;
    mut auto worker = std::move(started).value();
    if (!worker.join()) return 2;
  }
  if (drops.load() != 1 || !weak.expired() || weak.lock()) return 3;
  return 0;
}
""",
    "cycle_weak": """class Node {
  int32_t id;
public:
  std::mutex<std::weak_ptr<Node>> back;
  std::mutex<std::shared_ptr<Node>> next;
  Node(int32_t id) : id(id),
      back(std::mutex<std::weak_ptr<Node>>(std::weak_ptr<Node>())),
      next(std::mutex<std::shared_ptr<Node>>(std::shared_ptr<Node>())) {}
  ~Node() { [[discard]] drops.fetch_add(1); trail.store(trail.load() * 10 + this.id); }
};
int32_t run() {
  {
    auto a = std::make_shared<Node>(1);
    auto b = std::make_shared<Node>(2);
    {
      mut auto guard = a->next.lock();
      guard.get() = b.clone();
    }
    {
      mut auto guard = b->back.lock();
      guard.get() = a.downgrade();
    }
    if (a.use_count() != std::size_t(1) || b.use_count() != std::size_t(2)) return 1;
  }
  if (drops.load() != 2 || trail.load() != 12) return 2;
  return 0;
}
""",
    "cycle_strong_broken": """class Node {
  int32_t id;
public:
  std::mutex<std::shared_ptr<Node>> next;
  Node(int32_t id) : id(id),
      next(std::mutex<std::shared_ptr<Node>>(std::shared_ptr<Node>())) {}
  ~Node() { [[discard]] drops.fetch_add(1); trail.store(trail.load() * 10 + this.id); }
};
int32_t run() {
  mut std::weak_ptr<Node> watch_a{};
  mut std::weak_ptr<Node> watch_b{};
  {
    auto a = std::make_shared<Node>(1);
    auto b = std::make_shared<Node>(2);
    { mut auto guard = a->next.lock(); guard.get() = b.clone(); }
    { mut auto guard = b->next.lock(); guard.get() = a.clone(); }
    watch_a = a.downgrade();
    watch_b = b.downgrade();
  }
  if (drops.load() != 0 || watch_a.use_count() != std::size_t(1) || watch_b.use_count() != std::size_t(1)) return 1;
  {
    auto a = watch_a.lock();
    mut std::shared_ptr<Node> taken{};
    { mut auto guard = a->next.lock(); taken.swap(guard.get()); }
    if (drops.load() != 0) return 2;
  }
  if (drops.load() != 2 || !watch_a.expired() || !watch_b.expired()) return 3;
  return 0;
}
""",
    "chain_order": """class Link {
  int32_t id;
public:
  std::shared_ptr<Link> next;
  Link(int32_t id, std::shared_ptr<Link> next) : id(id), next(std::move(next)) {}
  ~Link() { [[discard]] drops.fetch_add(1); trail.store(trail.load() * 10 + this.id); }
};
int32_t run() {
  mut std::weak_ptr<Link> tail_watch{};
  {
    auto tail = std::make_shared<Link>(3, std::shared_ptr<Link>());
    tail_watch = tail.downgrade();
    auto middle = std::make_shared<Link>(2, tail.clone());
    auto head = std::make_shared<Link>(1, std::move(middle));
    if (tail.use_count() != std::size_t(2)) return 1;
  }
  // Locals end newest first: head (and middle through it), then tail.
  if (drops.load() != 3 || trail.load() != 123 || !tail_watch.expired()) return 2;
  return 0;
}
""",
    "observer_vector": """int32_t run() {
  mut std::vector<std::weak_ptr<Item>> observers{};
  mut std::vector<std::shared_ptr<Item>> owners{};
  for (mut int32_t i = 1; i <= 4; i += 1) {
    owners.push_back(std::make_shared<Item>(i));
    observers.push_back(owners[owners.size() - std::size_t(1)].downgrade());
  }
  owners.erase(std::size_t(1));
  if (drops.load() != 1 || trail.load() != 2) return 1;
  mut int32_t live = 0;
  mut int32_t sum = 0;
  for (auto& observer : observers) {
    auto held = observer.lock();
    if (held) { live += 1; sum += held->value(); }
  }
  if (live != 3 || sum != 8) return 2;
  owners.clear();
  if (drops.load() != 4 || trail.load() / 1000 != 2) return 3;
  for (auto& observer : observers) {
    if (!observer.expired()) return 4;
  }
  return 0;
}
""",
}

MAIN = """int main() {
  int32_t status = run();
  if (status != 0) {
    std::println("status {} drops {} trail {}", status, drops.load(), trail.load());
    return status;
  }
  std::println("ok");
  return 0;
}
"""


def program(holder, operation, value=5):
    setup, access, release, before = HOLDERS[holder]
    body = OPERATIONS[operation].replace("A.", access + ".").replace(
        "A->", access + "->").replace("{A}", "{" + access + "}").replace(
        "R\n", release + "\n").replace("B", str(before))
    text = "int32_t run() {\n" + setup + body + "  return 0;\n}\n"
    return PRELUDE + text.replace("VALUE", str(value)) + MAIN


def cells():
    for holder in HOLDERS:
        for operation in OPERATIONS:
            yield f"{holder}_{operation}", program(holder, operation)
    for name, text in SCENARIOS.items():
        if isinstance(text, tuple):
            text = text[0]
        yield name, PRELUDE + text + MAIN


# A deliberately broken variant the oracle must flag (expects a second drop at expiry):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('local_expire',
                    'if (drops.load() != 0 + 1 ||',
                    'if (drops.load() != 0 + 2 ||')
