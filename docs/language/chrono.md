# Clocks And Durations

Status: Implemented signed duration units and nanosecond clock readings.

`std.chrono` provides signed durations, checked unit conversions, a process-local
monotonic clock, and a wall clock for Unix timestamps. Clock families remain
nominally distinct: a wall-clock timestamp is not a monotonic reading.

## Duration units

The six named durations are aliases of `duration<Unit>`. Each stores one
`int64_t` count in its own unit, with no runtime unit tag or allocation.
Default construction yields zero; construction from an `int64_t` count and
`count()` are constexpr. Negative durations are supported.

| Duration | Unit tag | Nanoseconds per tick | Format suffix |
| --- | --- | ---: | --- |
| `nanoseconds` | `nanosecond_unit` | 1 | ns |
| `microseconds` | `microsecond_unit` | 1000 | us |
| `milliseconds` | `millisecond_unit` | 1000000 | ms |
| `seconds` | `second_unit` | 1000000000 | s |
| `minutes` | `minute_unit` | 60000000000 | min |
| `hours` | `hour_unit` | 3600000000000 | h |

`duration_unit<U>` admits exactly these six unit tags. `duration_type<T>` admits
exactly their duration types. The descriptor implementation is private; arbitrary
user-defined or rational units are not part of this contract. `tick_nanoseconds()`
and `unit_symbol()` observe the type's scale and suffix, not stored state.

The former `nanoseconds` class is now an alias of
`duration<nanosecond_unit>`. Its construction, signed range and `count()` remain
source-compatible, and clocks continue to return that type. Reflection and
diagnostics may display the generic type identity.

## try_cast and cast

`value.try_cast<To>()` returns `expected<To, duration_errc>`.
`value.cast<To>()` returns `To`; it delegates to the same conversion and panics
with `Duration cast out of range` on overflow, preserving normal cleanup and
tracking the caller. There are no unchecked or implicit cross-unit conversions.

```gti
import std.chrono;

expected<std::chrono::seconds, std::chrono::duration_errc> timeout() {
  std::chrono::milliseconds input{1500i64};
  return input.try_cast<std::chrono::seconds>();
}

int main() {
  std::chrono::milliseconds input{-1500i64};
  auto seconds = input.cast<std::chrono::seconds>();
  return seconds.count() == -1i64 ? 0 : 1;
}
```

Both operations truncate toward zero: 1500 ms becomes 1 s, and -1500 ms becomes
-1 s. Discarding fractional ticks is successful. `duration_errc::out_of_range`
means the destination count cannot fit in `int64_t`. The source is unchanged.
Identity conversion preserves every count, including signed minimum/maximum.
Conversion reduces the scale before arithmetic; it never first converts to
nanoseconds. Thus a large hours count may convert to minutes even when its
nanosecond equivalent would overflow.

Both operations work in constant evaluation. A failed `try_cast` remains an
ordinary error value that constexpr code can inspect. An overflowing `cast` in
a required constant expression is a compile-time diagnostic. Runtime and
constexpr conversions use the same source implementation and staged MIR rules.

An application can register `std::from<AppError, duration_errc>` to propagate a
failed `try_cast` into its own error domain with `?`. `std::from` is not used to
make duration conversions implicit or infallible.

## Comparison, arithmetic and formatting

The six comparison operators compare counts of the same exact unit.
`checked_add(other)` and `checked_sub(other)` accept the same duration type and
return `expected<duration<Unit>, duration_errc>`. Overflow is reported; no count
wraps. These operations are constexpr. Convert explicitly before comparing or
combining different units. Arithmetic operators and implicit common-unit
selection are not provided.

All six types implement `std::formatting` through one generic conformance:

```gti
std::chrono::milliseconds timeout{1500i64};
std::println("Timeout: {}", timeout); // Timeout: 1500ms
```

Formatting applies the existing signed-integer format specification to the
count and then appends the suffix. Width/alignment therefore apply to the count,
not the combined count and suffix. Formatting failures use `format_errc`.

Floating representations, arbitrary ratios, duration literal suffixes, parsing,
floor/ceil/round conversions, and calendar units remain outside this slice.

## Constant clock observations

Passive time-point constructors and pure observations are constexpr; actual
clock reads and recoverable `elapsed_since` remain runtime operations.
`system_time_point::unix_seconds()` retains floor-based epoch decomposition;
it deliberately differs from duration casting's truncation toward zero.

## Monotonic Time

`std::chrono::steady_clock::now()` returns
`expected<steady_time_point, clock_errc>`. Successful readings are
nondecreasing within one process and are not adjusted when the host wall clock
changes. Their epoch is intentionally unspecified.

`steady_time_point::elapsed_since(earlier)` computes `this - earlier` with
checked signed arithmetic. The result may be negative when the receiver is the
earlier point. `before`, `after`, and `equals` compare points from this clock.
`time_since_origin()` exposes the opaque numeric reading for storage or
diagnostics but does not give the origin portable meaning.

## Wall Time

`std::chrono::system_clock::now()` returns
`expected<system_time_point, clock_errc>`. A system point is signed nanoseconds
since `1970-01-01T00:00:00Z`, excluding leap seconds. The representation covers
approximately `1677-09-21` through `2262-04-11`.

`nanoseconds_since_unix_epoch()` returns the exact count. `unix_seconds()` uses
floor division rather than truncation, and `subsecond_nanoseconds()` is always
in `[0, 1,000,000,000)`. Consequently a point one nanosecond before the epoch
has Unix seconds `-1` and subsecond nanoseconds `999,999,999`.

System points provide the same checked `elapsed_since`, `before`, `after`, and
`equals` operations as steady points. No operation accepts the other point
family.

## Errors And Host Boundary

Clock reads and elapsed subtraction use these recoverable categories:

| Value | Meaning |
| --- | --- |
| `unavailable` | the target runtime cannot obtain the requested clock |
| `out_of_range` | the reading or elapsed result cannot be represented as signed nanoseconds |

The private scalar entries `gti_rt_chrono_steady_now_v1` and
`gti_rt_chrono_system_now_v1` write an `int64_t` only on success. Public GTI
types and clock policy do not cross the ABI, and neither compiler nor backend
recognizes their qualified names.

This slice has no sleep API, calendar fields, civil-time formatting or parsing,
time zones, leap-second database, UTC/TAI clock families, generic clocks, or
user-selected duration units. Existing C-style thread sleep remains a separate
threading facility rather than being treated as clock observation.
