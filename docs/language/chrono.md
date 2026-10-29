# Clocks And Durations

Status: Implemented bounded nanosecond clock slice.

`std.chrono` provides exact signed nanosecond values, a process-local
monotonic clock for elapsed-time measurement, and a wall clock for Unix
timestamps. It deliberately keeps the two time-point families nominally
distinct so a timestamp cannot accidentally participate in monotonic timing.

## Nanoseconds

`std::chrono::nanoseconds` stores one signed `int64_t` count and exposes it
through `count()`. This first slice does not implement C++ ratio-based duration
templates, implicit unit conversions, floating durations, or duration
arithmetic operators.

Both nanosecond constructors and `count()` are `constexpr`. Construction and
pure observation of steady/system time points, including their comparisons,
Unix seconds and fractional decomposition, are also eligible for the existing
passive-record constant evaluator. Semantics stages and executes canonical MIR;
native C++ constant evaluation does not decide validity. The shared step/depth
limits still apply. When observing a duration returned from
`time_since_origin()` in a constant function, bind it to a local before calling
`count()`; direct temporary-receiver composition remains outside the current
staging profile. Clock reads and recoverable `elapsed_since` operations remain
runtime-only. See [the frame-budget example](../../examples/102-constexpr-frame-budget.gti).

The representable interval is exactly the `int64_t` nanosecond range. Operations
that could exceed it return `clock_errc::out_of_range`; they do not wrap and do
not use host C++ overflow behavior.

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
