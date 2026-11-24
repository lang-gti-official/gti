"""Matrix: thread-local owners, first-use initialization and per-thread cleanup.

Contract (docs/language/concurrency.md "thread_local" and execution.md 4.9):
`thread_local` gives each thread its own instance of a namespace variable,
a static data member or a block-local variable. Initialization is lazy at
the first evaluated use on each thread; a block declaration initializes when
execution reaches it. Cleanup-owning values are destroyed on their own
thread in reverse successful-initialization order: a managed worker drains
before its outcome is published (so before `join` returns), and the initial
thread drains at hosted exit, after `main` returns. A block-local instance
outlives its function and may be borrowed after it returns. Recursive
initialization and new initialization during thread-exit cleanup are
`GTI-R0013`; a destructor may still use another initialized slot that is
not yet retired.

Axes: storage form (namespace, static member, block-local) x thread that
uses it (the initial thread, one worker, two workers) x payload (a counter,
an owning Mark, a vector of Marks). Fixed scenarios cover first-use order,
borrowing a block-local instance after return, cross-slot use during
cleanup, and both `GTI-R0013` failures.

Excluded shapes: generic static thread-local members (not supported:
"non-generic static data members"); dynamic initialization on an
unattached native thread (needs raw C threads); TLS cleanup after a worker
failure (the drain order on that path is not specified).
"""

PRELUDE = """import std.format;
import std.vector;
import std.thread;
import std.atomic;
import std.panic;
import std.utility;
std::atomic<int32_t> drops{0};
std::atomic<int32_t> made{0};
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) { [[discard]] made.fetch_add(1); }
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { [[discard]] drops.fetch_add(1); }
  int32_t value() { return this.code; }
};
"""

# Storage: declarations and an expression `SLOT` usable as a mutable place
# of the payload type P, initialized with INIT.
STORAGE = {
    "namespace": ("thread_local mut P slot = INIT;\nmut P& place() { return slot; }\n"),
    "static_member": ("class Registry {\npublic:\n  static thread_local mut P slot = INIT;\n};\n"
                      "mut P& place() { return Registry::slot; }\n"),
    "block_local": ("mut P& place() {\n  thread_local mut P slot = INIT;\n  return slot;\n}\n"),
}

# Payload: (type, initializer, `bump` body adding n and returning the new
# total, whether it owns a Mark per step)
PAYLOADS = {
    "counter": ("int32_t", "0", """int32_t bump(int32_t n) {
  mut int32_t& value = place();
  value += n;
  return value;
}
""", 0),
    "owner": ("Mark", "Mark(0)", """int32_t bump(int32_t n) {
  mut Mark& value = place();
  value = Mark(value.value() + n);
  return value.value();
}
""", 1),
    "vector": ("std::vector<Mark>", "std::vector<Mark>()", """int32_t bump(int32_t n) {
  mut std::vector<Mark>& marks = place();
  marks.push_back(Mark(n));
  mut int32_t total = 0;
  for (auto& mark : marks) total += mark.value();
  return total;
}
""", 2),
}

# Thread use: run() body. Each thread bumps 1, 2, 3 (total 6) on its own
# instance; the initial thread then bumps 10 to prove a worker never touched
# its instance. LIVE(k) is the drop count after k workers have drained.
USES = {
    "initial": """int32_t run() {
  if (bump(1) != 1 || bump(2) != 3 || bump(3) != 6) return 1;
  if (bump(10) != 16) return 2;
  return 0;
}
""",
    "worker": """int32_t work() {
  [[discard]] bump(1);
  [[discard]] bump(2);
  return bump(3);
}
int32_t run() {
  if (bump(100) != 100) return 1;
  mut std::thread_result<int32_t> worker{};
  if (!worker.start([]() -> int32_t { return work(); })) return 2;
  auto result = worker.join();
  if (!result || result.value() != 6) return 3;
  if (drops.load() != LIVE1) return 4;
  if (bump(10) != 110) return 5;
  return 0;
}
""",
    "two_workers": """int32_t work() {
  [[discard]] bump(1);
  [[discard]] bump(2);
  return bump(3);
}
int32_t run() {
  mut std::thread_result<int32_t> first{};
  mut std::thread_result<int32_t> second{};
  if (!first.start([]() -> int32_t { return work(); })) return 1;
  if (!second.start([]() -> int32_t { return work(); })) return 1;
  auto a = first.join();
  auto b = second.join();
  if (!a || !b || a.value() != 6 || b.value() != 6) return 2;
  if (drops.load() != LIVE2) return 3;
  if (bump(10) != 10) return 4;
  return 0;
}
""",
}


def live_after(payload, workers):
    """Drops after `workers` workers drained their instance: an owner keeps
    one Mark per thread (each replaced Mark and moved-from temporary is
    already counted on assignment), a vector keeps three."""
    _, _, _, kind = PAYLOADS[payload]
    if kind == 0:
        return 0
    if kind == 1:
        # Per worker: Mark(0) initial, three assignments each destroying the
        # old value; the final value drains at thread exit.
        return workers * 4
    # Per worker: three Marks pushed (temporaries moved in); all three drain.
    return workers * 3


MAIN = """int main() {
  int32_t status = run();
  if (status != 0) {
    std::println("status {} drops {} made {}", status, drops.load(), made.load());
    return status;
  }
  std::println("ok");
  return 0;
}
"""

SCENARIOS = {
    # First use, not declaration order, fixes initialization; the initial
    # thread drains in reverse initialization order after main returns.
    "first_use_order": ("""class Loud {
  int32_t code;
public:
  Loud(int32_t code) : code(code) { std::println("init {}", code); }
  ~Loud() { std::println("drop {}", this.code); }
  int32_t value() { return this.code; }
};
thread_local Loud first = Loud(1);
thread_local Loud second = Loud(2);
thread_local Loud third = Loud(3);
int main() {
  if (second.value() + first.value() != 3) return 1;
  std::println("ok");
  return 0;
}
""", {"stdout": "init 2\\ninit 1\\nok\\ndrop 1\\ndrop 2\\n"}),
    # A worker drains its instances before join returns, newest first, and
    # never initializes the initial thread's instances.
    "worker_drain_order": ("""std::atomic<int32_t> trail{0};
class Loud {
  int32_t code;
public:
  Loud(int32_t code) : code(code) {}
  ~Loud() { trail.store(trail.load() * 10 + this.code); }
  int32_t value() { return this.code; }
};
thread_local Loud first = Loud(1);
thread_local Loud second = Loud(2);
int32_t touch() {
  thread_local Loud local = Loud(3);
  return local.value();
}
int main() {
  auto started = std::thread::start([]() -> void {
    if (first.value() + touch() + second.value() != 6) std::panic("bad tls values");
  });
  if (!started) return 1;
  mut auto worker = std::move(started).value();
  if (!worker.join()) return 2;
  if (trail.load() != 231) { std::println("trail {}", trail.load()); return 3; }
  std::println("ok");
  return 0;
}
""", {}),
    # A block-local instance outlives its function.
    "borrow_after_return": ("""mut int32_t& counter() {
  thread_local mut int32_t value = 40;
  return value;
}
int main() {
  {
    mut int32_t& held = counter();
    held += 2;
  }
  if (counter() != 42) return 1;
  std::println("ok");
  return 0;
}
""", {}),
    # A destructor may use another initialized slot that is not retired:
    # `late` initializes after `early`, so it is destroyed first.
    "cleanup_reads_live_slot": ("""class Early {
public:
  int32_t code = 7;
  Early() = default;
  ~Early() { std::println("drop early"); }
};
thread_local Early early = Early();
class Late {
public:
  Late() = default;
  ~Late() { std::println("late sees {}", early.code); }
  int32_t touch() { return 1; }
};
thread_local Late late = Late();
int main() {
  if (early.code != 7) return 1;
  if (late.touch() != 1) return 2;
  std::println("ok");
  return 0;
}
""", {"stdout": "ok\\nlate sees 7\\ndrop early\\n"}),
    # Initialization that reaches its own slot again is GTI-R0013.
    "recursive_initialization": ("""int32_t compute();
thread_local int32_t cycle = compute();
int32_t compute() { return cycle + 1; }
int main() {
  std::println("before");
  if (cycle == 0) return 1;
  std::println("after");
  return 0;
}
""", {"stdout": "before\\n", "returncode": 70,
      "stderr": "runtime error[GTI-R0013]: hosted_runtime_contract_failure in recursive_thread_local_initialization"}),
    # First use of a new slot during thread-exit cleanup is GTI-R0013.
    "initialization_during_shutdown": ("""class Fresh {
public:
  int32_t code = 3;
  Fresh() = default;
};
thread_local Fresh fresh = Fresh();
class Reaper {
public:
  Reaper() = default;
  ~Reaper() { std::println("reaper {}", fresh.code); }
  int32_t touch() { return 1; }
};
thread_local Reaper reaper = Reaper();
int main() {
  if (reaper.touch() != 1) return 2;
  std::println("ok");
  return 0;
}
""", {"stdout": "ok\\n", "returncode": 70,
      "stderr": "runtime error[GTI-R0013]: hosted_runtime_contract_failure in thread_local_initialization_during_shutdown"}),
}


def program(storage, payload, use):
    type_, init, bump, _ = PAYLOADS[payload]
    decls = STORAGE[storage].replace("P", type_).replace("INIT", init)
    # In "worker" the initial thread first replaces its own owner once.
    before = 1 if payload == "owner" else 0
    body = USES[use].replace("LIVE1", str(before + live_after(payload, 1))).replace(
        "LIVE2", str(live_after(payload, 2)))
    return PRELUDE + decls + bump + body + MAIN


def cells():
    for storage in STORAGE:
        for payload in PAYLOADS:
            for use in USES:
                yield f"{storage}_{payload}_{use}", program(storage, payload, use)
    for name, (text, expect) in SCENARIOS.items():
        expect = {key: value.replace("\\n", "\n") if isinstance(value, str) else value
                  for key, value in expect.items()}
        if expect:
            yield name, PRELUDE + text, expect
        else:
            yield name, PRELUDE + text


# A deliberately broken variant the oracle must flag (a worker's instance accumulates a wrong total):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('namespace_owner_worker',
                    '  value = Mark(value.value() + n);\n',
                    '  value = Mark(value.value() + n + 1);\n')
