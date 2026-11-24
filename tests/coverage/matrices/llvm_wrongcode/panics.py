"""Matrix: defined runtime failure, its cleanup and the exit status.

Contract (docs/language/execution.md 4.10 and 4.2.3): a defined failure (an
explicit `std::panic` or a checked operation) is not resumable. Cleanup
follows compiler-generated failure edges and destroys every live temporary,
local, owned parameter and initialized subobject in the same reverse order as
an ordinary exit, callee locals before parameters; a partially constructed
object cleans only its completed fields and never runs its own destructor.
A worker failure cleans the task's captures and re-raises on join. The hosted
boundary flushes stdout written so far, prints one report line
`runtime error[GTI-Rnnnn]: ...` on stderr and exits with status 70. A second
failure during failure cleanup takes the `GTI-R0014` path, which skips the
remaining cleanup.

Each cell prints a destruction trace to stdout. Its expectation is the exact
trace, status 70 and the report's code and detail on stderr, all derived
from the rules above, never from a run.

Axes: failure source (explicit panic, integer overflow, division by zero,
fixed-array and vector index, empty owner, inactive expected) x the cleanup
context it interrupts (nested scopes, a callee with owned parameters, a loop
iteration, a full-expression temporary, a field initializer of a partly
built object, a vector of owners, a shared owner, a closure's captures).
Fixed scenarios cover a failing destructor on ordinary exit, the double
failure path and a failing worker thread.

Excluded shapes: allocation exhaustion (`GTI-R0011` aborts without cleanup
and cannot be provoked deterministically); TLS shutdown failures (covered by
thread_local_owners); embedding and observer boundaries (not exposed to
source programs).
"""

PRELUDE = """import std.format;
import std.vector;
import std.memory;
import std.panic;
import std.thread;
import std.utility;
import std.atomic;
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { std::println("{}", this.code); }
  int32_t value() { return this.code; }
};
std::atomic<int32_t> unit{1};
int32_t one() { return unit.load(); }
"""

# source: (statement or expression that fails, stderr substring)
FAILURES = {
    "panic": ("std::panic(\"stop here\")", 'runtime error[GTI-R0017]: panic: "stop here"'),
    "overflow": ("unit.store(int32_t(2147483647) + one())",
                 "runtime error[GTI-R0001]: integer_overflow in addition"),
    "divide": ("unit.store(int32_t(10) / (one() - 1))",
               "runtime error[GTI-R0002]: division_by_zero in integer_division"),
    "array_index": ("[[discard]] fixed_at(int32_t(3) + one())",
                    "runtime error[GTI-R0007]: index_out_of_bounds in fixed_array"),
    "vector_index": ("[[discard]] vector_at(std::size_t(4))",
                     "runtime error[GTI-R0007]: index_out_of_bounds in vector"),
    "empty_owner": ("[[discard]] empty_owner()",
                    "runtime error[GTI-R0008]: empty_owner_access in"),
    "expected": ("[[discard]] expected_value()",
                 "runtime error[GTI-R0009]: invalid_expected_access in value_on_error"),
}

HELPERS = """int32_t fixed_at(int32_t index) {
  int32_t values[3] = {1, 2, 3};
  return values[index];
}
int32_t vector_at(std::size_t index) {
  mut std::vector<int32_t> values{};
  values.push_back(7);
  values.push_back(8);
  return values[index];
}
int32_t empty_owner() {
  std::unique_ptr<int32_t> owner{};
  if (one() > 1) { return 0; }
  return *owner;
}
expected<int32_t, int32_t> attempt() {
  if (one() == 1) return unexpected(int32_t(4));
  return int32_t(5);
}
int32_t expected_value() { return attempt().value(); }
"""

# context: (code with FAIL as the failing statement, expected stdout)
CONTEXTS = {
    "nested_scopes": ("""void run() {
  Mark a = Mark(1);
  {
    Mark b = Mark(2);
    if (one() == 1) {
      Mark c = Mark(3);
      FAIL;
    }
    Mark d = Mark(4);
  }
}
""", [3, 2, 1]),
    "callee_parameters": ("""void inner(Mark first, Mark second) {
  Mark local = Mark(5);
  FAIL;
}
void run() {
  Mark outer = Mark(1);
  std::println("{}", 0);
  inner(Mark(2), Mark(3));
}
""", [0, 5, 3, 2, 1]),
    "loop_iteration": ("""void run() {
  Mark outer = Mark(9);
  for (mut int32_t i = 1; i <= 3; i += 1) {
    Mark step = Mark(i);
    if (i == 3) { FAIL; }
  }
}
""", [1, 2, 3, 9]),
    "temporary": ("""int32_t fail_value() {
  FAIL;
  return 0;
}
void run() {
  Mark outer = Mark(1);
  int32_t total = Mark(7).value() + Mark(8).value() + fail_value();
  std::println("{}", total);
}
""", [8, 7, 1]),
    "field_initializer": ("""Mark make_failing(int32_t code) {
  Mark temp = Mark(code);
  FAIL;
  return temp;
}
class Pair {
  Mark first;
  Mark second;
  Mark third;
public:
  Pair() : first(Mark(1)), second(Mark(2)), third(make_failing(6)) {}
  ~Pair() { std::println("{}", 99); }
};
void run() {
  Mark outer = Mark(5);
  Pair pair = Pair();
}
""", [6, 2, 1, 5]),
    "vector_owners": ("""void run() {
  mut std::vector<std::unique_ptr<Mark>> owners{};
  owners.push_back(std::make_unique<Mark>(4));
  Mark after = Mark(6);
  FAIL;
}
""", [6, 4]),
    "shared_owner": ("""void use(std::shared_ptr<Mark> held) {
  Mark local = Mark(3);
  FAIL;
}
void run() {
  auto owner = std::make_shared<Mark>(4);
  auto alias = owner.clone();
  Mark middle = Mark(1);
  use(std::move(alias));
}
""", [3, 1, 4]),
    "closure_captures": ("""void run() {
  Mark outer = Mark(1);
  auto first = Mark(2);
  auto second = Mark(3);
  auto action = [a = std::move(first), b = std::move(second)]() -> int32_t {
    Mark inside = Mark(4);
    FAIL;
    return a.value() + b.value();
  };
  [[discard]] action();
}
""", [4, 3, 2, 1]),
}

SCENARIOS = {
    # A destructor that fails on ordinary scope exit starts failure cleanup
    # of the remaining locals (execution.md 4.10).
    "failing_destructor": ("""class Bomb {
public:
  Bomb() = default;
  ~Bomb() {
    std::println("{}", 50);
    std::panic("from destructor");
  }
};
void run() {
  Mark a = Mark(1);
  Bomb b = Bomb();
  Mark c = Mark(3);
}
""", [3, 50, 1], 'runtime error[GTI-R0017]: panic: "from destructor"'),
    # A second failure during failure cleanup skips the remaining cleanup.
    "double_failure": ("""class Bomb {
public:
  Bomb() = default;
  ~Bomb() {
    std::println("{}", 50);
    std::panic("second");
  }
};
void run() {
  Mark a = Mark(1);
  Bomb b = Bomb();
  Mark c = Mark(3);
  std::panic("first");
}
""", [3, 50], "runtime error[GTI-R0014]: failure_during_cleanup; primary [GTI-R0017]"),
    # The worker's capture is cleaned at its task boundary; join re-raises
    # the record on the joining thread, which then cleans its own locals.
    "worker_thread": ("""[[unsafe_transfer, unsafe_share]] class Token {
  int32_t code;
public:
  Token(int32_t code) : code(code) {}
  Token(Token& other) = delete;
  Token(Token&& other) = default;
  ~Token() { std::println("{}", this.code); }
  int32_t value() { return this.code; }
};
void run() {
  Mark outer = Mark(1);
  auto token = Token(5);
  auto started = std::thread::start([token = std::move(token)]() -> void {
    if (token.value() == 5) std::panic("worker stop");
  });
  if (!started) return;
  mut auto worker = std::move(started).value();
  [[discard]] worker.join();
  std::println("{}", 99);
}
""", [5, 1], 'runtime error[GTI-R0017]: panic: "worker stop"'),
}

MAIN = """int main() {
  run();
  std::println("{}", 100);
  return 0;
}
"""


def expectation(trace, report):
    return {"stdout": "".join(f"{code}\n" for code in trace),
            "returncode": 70, "stderr": report}


def cells():
    for failure, (statement, report) in FAILURES.items():
        for context, (code, trace) in CONTEXTS.items():
            text = PRELUDE + HELPERS + code.replace("FAIL", statement) + MAIN
            yield f"{failure}_{context}", text, expectation(trace, report)
    for name, (code, trace, report) in SCENARIOS.items():
        yield name, PRELUDE + code + MAIN, expectation(trace, report)


# A deliberately broken variant the oracle must flag (the callee local is never created):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('panic_callee_parameters',
                    '  Mark local = Mark(5);\n  std::panic',
                    '  std::panic')
