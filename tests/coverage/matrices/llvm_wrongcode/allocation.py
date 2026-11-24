"""Matrix: allocator-backed owners, slot lifetimes and resource leases.

Contract (docs/language/ownership-and-lifetimes.md "Public Typed
Allocation", "Bounded Arena And Pool Ownership", "Programmable Resource
Leases", "Standard-Library Storage"; execution.md 4.10): `std::storage<T>`
slots are each uninitialized or hold one live `T`; `construct` and
`emplace` begin a slot lifetime, `destroy` ends one, and `clear`, `release`
and owner destruction end every live slot in reverse index order. Moving the
owner does not move its elements. Allocation requests report
`invalid_alignment` and `size_overflow` as recoverable errors, and zero
elements succeeds. Out-of-range, duplicate and uninitialized slot use are
defined failures that clean every other live owner. An arena inserts at its
monotonic position and `reset` destroys in reverse; a pool reuses its lowest
free slot and `clear` destroys in reverse slot order; both report `full`.
A resource-backed owner destroys its elements in reverse, then returns its
regions to the provider, and the provider outlives every owner.

Every Mark prints its code when destroyed and the provider prints 900 per
released region and 901 when it ends, so the expected stdout is the trace
these rules give, then `ok` (or status 70 and the report).

Axes: owner (storage, arena, pool, resource vector) x operation (construct
and destroy, emplace, early release, owner move, clear and reuse, capacity
errors, checked failures, growth through a resource).

Excluded shapes: allocation exhaustion of infallible owners (aborts without
cleanup); `relocate_to` of non-relocatable elements (static rejection);
runtime-polymorphic resources and resource owners other than
`resource_vector`/`resource_storage` (not implemented, standard-library.md
gaps list).
"""

PRELUDE = """import std.format;
import std.memory;
import std.resource;
import std.resource_vector;
import std.utility;
import std.atomic;
std::atomic<int32_t> unit{1};
std::atomic<int32_t> allocated{0};
std::atomic<int32_t> released{0};
int32_t one() { return unit.load(); }
void note(int32_t code) { std::println("{}", code); }
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { note(this.code); }
  int32_t value() { return this.code; }
};
class Pinned {
  int32_t code;
public:
  Pinned(int32_t a, int32_t b) : code(a * 10 + b) {}
  Pinned(Pinned& other) = delete;
  Pinned(Pinned&& other) = delete;
  ~Pinned() { note(this.code); }
  int32_t value() { return this.code; }
};
class Provider {
  std::process_resource process{};
public:
  unsafe expected<std::raw_allocation, std::alloc_errc> allocate(std::layout request) {
    [[discard]] allocated.fetch_add(1);
    unsafe { return this.process.allocate(request); }
  }
  unsafe void release(std::raw_allocation allocation) {
    [[discard]] released.fetch_add(1);
    unsafe { this.process.release(allocation); }
  }
  ~Provider() { note(900 + allocated.load() - released.load()); }
public:
  Provider() = default;
};
std::storage<Mark> slots(std::size_t count) {
  std::allocator allocator = std::allocator();
  auto attempt = allocator.try_storage<Mark>(count);
  if (!attempt) std::panic("allocation failed");
  return std::move(attempt).value();
}
"""

CELLS = {
    "storage_construct_destroy": ("""  {
    mut std::storage<Mark> values = slots(std::size_t(4));
    values.construct(std::size_t(3), Mark(3));
    values.construct(std::size_t(0), Mark(10));
    values.construct(std::size_t(2), Mark(2));
    values.destroy(std::size_t(0));
    note(50);
    if (values.get(std::size_t(2)).value() + values.get(std::size_t(3)).value() != 5) return 1;
    if (values.initialized(std::size_t(0)) || !values.initialized(std::size_t(3))) return 2;
    values.construct(std::size_t(0), Mark(11));
  }""", [10, 50, 3, 2, 11]),
    "storage_emplace_immovable": ("""  {
    std::allocator allocator = std::allocator();
    auto attempt = allocator.try_storage<Pinned>(std::size_t(3), std::alignment(std::size_t(64)));
    if (!attempt) return 1;
    mut std::storage<Pinned> values = std::move(attempt).value();
    values.emplace(std::size_t(1), 1, 2);
    values.emplace(std::size_t(2), 3, 4);
    note(50);
    if (values.get(std::size_t(1)).value() != 12) return 2;
  }""", [50, 34, 12]),
    "storage_release_early": ("""  {
    mut std::storage<Mark> values = slots(std::size_t(3));
    values.construct(std::size_t(0), Mark(1));
    values.construct(std::size_t(1), Mark(2));
    values.release();
    note(50);
    if (values.capacity() != std::size_t(0)) return 1;
  }""", [2, 1, 50]),
    "storage_owner_move": ("""  {
    mut std::storage<Mark> values = slots(std::size_t(2));
    values.construct(std::size_t(0), Mark(1));
    values.construct(std::size_t(1), Mark(2));
    {
      std::storage<Mark> moved = std::move(values);
      note(50);
      if (moved.get(std::size_t(1)).value() != 2) return 1;
    }
    note(51);
  }""", [50, 2, 1, 51]),
    "storage_clear_reuse": ("""  {
    mut std::storage<Mark> values = slots(std::size_t(3));
    for (mut int32_t i = 0; i < 3; i += 1) values.construct(std::size_t(i), Mark(i + 1));
    values.clear();
    note(50);
    values.construct(std::size_t(1), Mark(7));
    mut Mark& slot = values.get_mut(std::size_t(1));
    slot = Mark(8);
    note(51);
  }""", [3, 2, 1, 50, 7, 51, 8]),
    "storage_request_errors": ("""  std::allocator allocator = std::allocator();
  auto zero = allocator.try_storage<Mark>(std::size_t(0));
  if (!zero || zero.value().capacity() != std::size_t(0)) return 1;
  auto bad_alignment = allocator.try_storage<Mark>(std::size_t(2), std::alignment(std::size_t(3)));
  if (bad_alignment || bad_alignment.error() != std::alloc_errc::invalid_alignment) return 2;
  auto huge = allocator.try_storage<Mark>(std::size_t(18446744073709551615));
  if (huge || huge.error() != std::alloc_errc::size_overflow) return 3;""", []),
    "storage_out_of_bounds": ("""  mut std::storage<Mark> values = slots(std::size_t(2));
  values.construct(std::size_t(0), Mark(1));
  Mark guard = Mark(5);
  values.construct(std::size_t(one() + 1), Mark(2));
  note(60);""", [2, 5, 1], "runtime error[GTI-R0007]: index_out_of_bounds in private_storage"),
    "storage_duplicate": ("""  mut std::storage<Mark> values = slots(std::size_t(2));
  values.construct(std::size_t(0), Mark(1));
  Mark guard = Mark(5);
  values.construct(std::size_t(one() - 1), Mark(2));
  note(60);""", [2, 5, 1], "runtime error[GTI-R0010]: invalid_storage_state in duplicate_construction"),
    "storage_uninitialized": ("""  mut std::storage<Mark> values = slots(std::size_t(2));
  values.construct(std::size_t(0), Mark(1));
  Mark guard = Mark(5);
  note(values.get(std::size_t(one())).value());""", [5, 1], "runtime error[GTI-R0010]: invalid_storage_state in uninitialized_access"),
    "arena_reset_full": ("""  {
    auto created = std::try_make_arena<Mark>(std::size_t(3));
    if (!created) return 1;
    mut std::arena<Mark> marks = std::move(created).value();
    for (mut int32_t i = 1; i <= 3; i += 1) {
      auto at = marks.try_insert(Mark(i));
      if (!at || at.value() != std::size_t(i - 1)) return 2;
    }
    auto overflow = marks.try_insert(Mark(9));
    if (overflow || overflow.error() != std::arena_errc::full) return 3;
    note(50);
    marks.reset();
    note(51);
    auto again = marks.try_insert(Mark(4));
    if (!again || again.value() != std::size_t(0) || marks.get(std::size_t(0)).value() != 4) return 4;
  }""", [9, 50, 3, 2, 1, 51, 4]),
    "pool_reuse_clear": ("""  {
    auto created = std::try_make_pool<Mark>(std::size_t(3));
    if (!created) return 1;
    mut std::pool<Mark> marks = std::move(created).value();
    for (mut int32_t i = 1; i <= 3; i += 1) {
      if (!marks.try_insert(Mark(i))) return 2;
    }
    marks.erase(std::size_t(1));
    note(50);
    auto reused = marks.try_insert(Mark(4));
    if (!reused || reused.value() != std::size_t(1)) return 3;
    auto overflow = marks.try_insert(Mark(9));
    if (overflow || overflow.error() != std::pool_errc::full) return 4;
    if (!marks.contains(std::size_t(1)) || marks.size() != std::size_t(3)) return 5;
    marks.clear();
    note(51);
    if (marks.size() != std::size_t(0)) return 6;
  }""", [2, 50, 9, 3, 4, 1, 51]),
    "resource_vector_growth": ("""  {
    Provider provider{};
    unsafe {
      auto resource = std::bind_resource(provider);
      mut std::resource_vector<Mark, Provider> values{resource};
      for (mut int32_t i = 1; i <= 3; i += 1) values.emplace_back(i);
      if (values.size() != std::size_t(3) || values[2].value() != 3) return 1;
      note(50);
    }
    note(51);
  }
  if (allocated.load() < 1 || allocated.load() != released.load()) return 2;""", [50, 3, 2, 1, 51, 900]),
    "resource_vector_moved_owner": ("""  {
    Provider provider{};
    unsafe {
      auto resource = std::bind_resource(provider);
      mut std::resource_vector<Mark, Provider> values{resource};
      values.emplace_back(1);
      values.emplace_back(2);
      {
        auto first = std::move(values);
        auto second = std::move(first);
        note(50);
      }
      note(51);
    }
  }
  if (allocated.load() != released.load()) return 1;""", [50, 2, 1, 51, 900]),
}

MAIN = """int32_t run() {
BODY
  note(90);
  return 0;
}
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
    for name, cell in CELLS.items():
        body, trace = cell[0], cell[1]
        text = PRELUDE.replace("import std.atomic;", "import std.atomic;\nimport std.panic;") \
            + MAIN.replace("BODY", body)
        if len(cell) > 2:
            yield name, text, {"stdout": "".join(f"{c}\n" for c in trace),
                               "returncode": 70, "stderr": cell[2]}
        else:
            yield name, text, {"stdout": "".join(f"{c}\n" for c in trace) + "90\nok\n"}


# A deliberately broken variant the oracle must flag (the wrong slot is freed, so reuse lands elsewhere):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('pool_reuse_clear',
                    'marks.erase(std::size_t(1));',
                    'marks.erase(std::size_t(2));')
