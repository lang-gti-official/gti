# Clocks, Calendars And Time Zones

`import std.chrono;` provides fixed durations, monotonic and wall clocks,
validated Gregorian calendar values, Unix timestamps, named time zones and
temporal formatting. All public types live in `std::chrono`.

## Choosing The Right Kind Of Time

| Need | Type |
| --- | --- |
| Measure elapsed time or a timeout | `steady_time_point` and fixed durations |
| Store an instant on the Unix timeline | `timestamp` |
| Store a calendar date | `date` |
| Store a clock reading without a date | `time_of_day` |
| Store an appointment's local date and clock reading | `local_datetime` |
| Interpret an instant using named rules or a fixed offset | `time_zone` and `zoned_time` |
| Advance the calendar | `calendar_days`, `months`, `years` |

A fixed `days(1i64)` is exactly 86,400 seconds. Advancing a local reading by
`calendar_days(1i64)` preserves its clock reading; resolving it in a zone may
produce an elapsed interval other than 24 hours. Months and years are calendar
counts, not durations convertible to seconds without a starting date and zone.

## Fallible And Direct Operations

New fallible operations have a `try_` form returning `expected` and a direct
form returning the value or using a cleanup-preserving panic on failure. For
example, `date::try_create(...)` reports an invalid date while
`date::create(...)` panics. Use `try_` forms for user input, IO and other failures
that the application should recover from.

Operations that cannot fail under their type invariants return values directly:
component accessors, comparisons, explicit `clone()` operations, `time_zone::utc()`
and conversion from a `system_time_point` into the wider `timestamp` need no
`try_` counterpart. Allocation failure retains the ordinary library allocation
policy; these APIs do not promise allocation-failure recovery.

The existing `steady_clock::now()`, `system_clock::now()` and their time-point
`elapsed_since()` methods retain their expected-returning contracts for
compatibility. New `timestamp::now()` is the direct form;
`timestamp::try_now()` is recoverable.

## Fixed Durations And Conversion

`duration<Unit>` stores one signed `int64_t` count in its own unit. The supported
aliases are `nanoseconds`, `microseconds`, `milliseconds`, `seconds`, `minutes`,
`hours` and `days`. Constructors and `count()` are constexpr.

```gti
std::chrono::milliseconds input{1500i64};
auto whole = input.cast<std::chrono::seconds>(); // 1 second
auto checked = input.try_cast<std::chrono::seconds>();
```

Conversions truncate toward zero: -1500 milliseconds becomes -1 second.
`try_cast<To>()` reports `duration_errc::out_of_range`; `cast<To>()` panics on
that same failure. Conversion checks the destination without first converting
the entire input into nanoseconds. Units do not convert implicitly.

Same-unit comparisons are direct. `try_add`/`add` and `try_sub`/`sub` check
signed overflow. The earlier `checked_add`/`checked_sub` spellings remain
recoverable aliases. Generic formatting prints a count and unit suffix.
Calendar counts are excluded from `duration_type`; there is no average-month
or average-year cast to seconds.

## Validated Dates And Clock Readings

`date::try_create(year, month, day)` validates a proleptic Gregorian date in
years 1 through 9999. `date::create` is its direct counterpart. Default
construction produces 1970-01-01. Valid dates have private fields and cannot
represent February 30.

`year()`, `month()` and `day()` return components. `weekday()` numbers Sunday
as 0 through Saturday as 6; `day_of_year()` is one-based. `days_since_epoch()`
counts calendar days from 1970-01-01; `try_from_days`/`from_days` perform the
inverse with range checking.

`time_of_day::try_create(hour, minute, second, nanosecond = 0)` accepts hours
0–23, minutes/seconds 0–59 and nanoseconds 0–999,999,999. Default construction
produces midnight. Accessors are `hour()`, `minute()`, `second()`,
`subsecond_nanoseconds()` and `seconds_since_midnight()`. Neither 24:00 nor a
leap-second reading with second 60 is accepted.

`local_datetime(date, time_of_day)` composes already validated values directly.
`try_create`/`create` validate all components together. `calendar_date()` and
`time()` return the parts; component accessors are also available directly.
`try_from_unix_seconds`/`from_unix_seconds` and `unix_seconds()` encode/decode
calendar fields relative to the epoch **without assigning a time zone**. This
numeric local reading is not an instant until a zone or explicit offset is
applied.

Calendar constructors, observations, comparisons and arithmetic are constexpr
source operations. Clock reads and zone IO remain runtime operations.

## Calendar Arithmetic

`date` and `local_datetime` accept `calendar_days`, `months` and `years` through
`try_add`/`add` and `try_sub`/`sub`. Calendar count types themselves support
checked addition/subtraction and direct comparisons.

```gti
namespace c = std::chrono;
auto january = c::date::create(2027, 1, 31);
auto invalid = january.try_add(c::months(1i64));
auto february = january.add(c::months(1i64), c::month_roll::clamp);
```

The default `month_roll::reject` reports an invalid resulting day. Explicit
`month_roll::clamp` selects the last valid day of the target month. Clamping
January 31 to February loses the original day intent; repeated additions are
not necessarily associative or reversible. Every operation checks integer and
calendar-range overflow. On `local_datetime`, calendar arithmetic preserves
the time-of-day and fractional nanoseconds.

## Unix Timestamps And Clock Bridges

`timestamp` stores signed 64-bit Unix seconds plus normalized fractional
nanoseconds, excluding leap seconds. `timestamp(seconds)` is a direct,
infallible whole-second constructor. `try_create(seconds, nanos)`/`create`
normalize fractional input with checked arithmetic. Half a second before the
epoch is represented as seconds -1 and nanoseconds 500,000,000.

`unix_seconds()` and `subsecond_nanoseconds()` expose that representation.
`try_add(duration)`/`add`, `try_sub(duration)`/`sub`, and the corresponding
`*_seconds(int64_t)` methods operate on the timeline. No timestamp-plus-timestamp
operation is defined.

`try_elapsed_since(earlier)`/`elapsed_since` return nanoseconds.
`try_elapsed_since_as<To>(earlier)`/`elapsed_since_as<To>` select a fixed unit
and truncate toward zero. A coarser result can represent an interval that
would overflow an `int64_t` nanosecond count. Comparisons and
`before`/`after`/`equals` are direct.

`from_system_time_point(point)` is exact and infallible.
`try_to_system_time_point()`/`to_system_time_point()` check the narrower
nanosecond range. Although stored timestamps can represent a wider interval,
civil conversion is limited to years 1–9999. `try_now()` obtains the existing
system clock and therefore retains that clock acquisition range.

## Time-Zone Databases And Ownership

`tzdb::try_load_default()`/`load_default()` load the pinned IANA 2026f snapshot
embedded in the standard library at compilation. A deployed executable using
this default does not need a runtime path back to the toolchain installation.
`try_load(path)`/`load(path)` accept an explicit `GTITZDB1` bundle for
application-controlled updates. `try_from_bytes`/`from_bytes` consume an owned
byte vector in the same format. The bundle generator is
[`scripts/build_chrono_tzdb.py`](../../scripts/build_chrono_tzdb.py); provenance
and license information accompany the checked-in resource.

`version()` returns the IANA version, `size()` the entry count, and `names()`
returns owned identifiers. `try_locate(name)`/`locate(name)` parse and retain a
zone's rules. Retain the resulting handle for repeated conversions rather
than repeatedly locating the same name.

Database and zone handles are move-only with explicit `clone()`. They retain
immutable owned data; replacing the original file or dropping the database
does not change an existing zone or `zoned_time`. Metadata views borrow from
the object exposing them; clone or copy text when it must outlive that owner.
There is no automatic network update or silent fallback to UTC.

`time_zone::utc()` needs no database. `try_fixed(offset_seconds)`/`fixed`
construct a fixed offset with second precision. `try_from_tzif(bytes, name,
version)`/`from_tzif` accept an individual TZif byte span. The supported profile
uses POSIX-time TZif v2/v3/v4 including future-rule footers; leap-aware data and
unsupported coverage produce errors rather than guessed conversions.

Bundle input is limited to 64 MiB and 4096 entries; individual zone files are
limited to 4 MiB, 200,000 transitions, 256 local types and 65,536 designation
bytes. Malformed sizes, duplicate names and invalid indices are rejected.

## Zoned Instants And Daylight-Saving Resolution

`zone.try_at(timestamp)`/`at(timestamp)` convert an instant into `zoned_time`.
The result retains its zone and exposes `instant()`, `local()`,
`offset_seconds()`, `is_daylight_saving()`, `abbreviation()`, `zone_name()` and
`database_version()`. Historical offsets can include seconds, and offset
changes need not be one hour.

A local reading can have zero, one or two corresponding instants.
`try_lookup(local)`/`lookup(local)` return a `local_mapping` payload enum:

- `unique(instant)` for one interpretation.
- `ambiguous(earlier, later)` when a clock reading repeats.
- `nonexistent(gap)` when a forward change skips the reading. `gap.begin` and
  `gap.end` describe the missing half-open local interval; `gap.next` is the
  first following instant.

Unsupported mappings with more than two candidates return `unsupported_data`.
`try_resolve(local, policy)`/`resolve` select a zoned result. Default policy
rejects both folds and gaps. A `resolve_policy` accepts `fold_policy::earlier`
or `later`, and `gap_policy::next_valid`. Next-valid selects the transition
boundary; it does not preserve the original minutes by adding the gap width.

For an appointment tomorrow, advance `meeting.local()` by `calendar_days(1i64)`
and resolve again with the intended policy. For an elapsed deadline 24 hours
later, add `hours(24i64)` to `meeting.instant()` and convert using `at`.
Store civil intent, zone ID and resolution policy when future rule updates
should be allowed to change a scheduled instant.

`try_current_zone_id()`/`current_zone_id()` discover an explicit `TZ` identifier
or a `/etc/localtime` link into a `zoneinfo` directory on supported hosts. They
do not change `TZ`, parse arbitrary POSIX rule strings, or infer an identity
from a display abbreviation. Failure to discover an identifier is explicit;
lookup in the chosen database is a separate operation.

## Formatting

Default `std::format("{}", value)` supports ISO-like dates, local readings,
UTC timestamps and zoned times. It preserves nonzero fractional nanoseconds
and offset seconds. Local readings never acquire an implicit zone. Not every
representation is RFC 3339, particularly when historical offsets include
seconds.

For custom layouts use the chrono-specific pattern API:

```gti
namespace c = std::chrono;
auto db = c::tzdb::load_default();
auto perth = db.locate("Australia/Perth");
auto now = c::timestamp::now();
auto local = perth.at(now);
auto text = c::format(local, "%I:%M:%S %P on %d %b %Y");
```

`try_format(value, pattern)`/`format` use a checked `time_pattern<T>` literal.
`try_runtime_format(value, string_view)`/`runtime_format` validate dynamic
patterns. `try_format_to(output, value, pattern)`/`format_to` and
`try_runtime_format_to`/`runtime_format_to` append to a mutable string; a
recoverable failure leaves the original output unchanged.

`try_prepare_time_pattern<T>(pattern)`/`prepare_time_pattern<T>` create an
owning `prepared_time_pattern<T>` that parses once. It offers `try_format`/
`format`, `try_format_to`/`format_to`, and mutable `try_assign`/`assign` to
replace its pattern. Failed assignment preserves the previous valid pattern.
Patterns are limited to 64 KiB and each rendered result to 1 MiB. Prepared
patterns avoid repeated parsing, but output construction can still allocate.

| Directive | Output |
| --- | --- |
| `%Y`, `%m`, `%d` | Four-digit year, two-digit month/day |
| `%H`, `%M`, `%S` | Two-digit 24-hour clock components |
| `%f` | Exactly nine fractional-nanosecond digits |
| `%I`, `%p`, `%P` | 12-hour hour, AM/PM, am/pm |
| `%a`, `%A` | English abbreviated/full weekday |
| `%b`, `%B` | English abbreviated/full month |
| `%:z`, `%::z` | Signed HH:MM / HH:MM:SS offset |
| `%Z`, `%{zone}` | Abbreviation / zone identity |
| `%%` | Literal percent sign |

`%:z` reports an error when nonzero offset seconds would be lost. A date cannot
supply clock fields, and a local reading cannot supply zone fields. Literal
patterns reject such mismatches during checking; runtime failures return
`format_error { code, position }`, where `position` is a byte offset in the
pattern. English names are deterministic and do not consult the ambient locale.
The general `std::format` parser does not accept these temporal directives in
its own replacement-field specification; use `std::chrono::format` for them.

## Existing Monotonic And System Clocks

`steady_clock::now()` returns `expected<steady_time_point, clock_errc>`.
Successful readings are nondecreasing within one process, are not adjusted
with the wall clock, and have an unspecified epoch. `time_since_origin()` is
useful numerically but gives the epoch no portable meaning.

`system_clock::now()` returns `expected<system_time_point, clock_errc>`.
System points are signed nanoseconds since 1970-01-01T00:00:00Z, excluding leap
seconds, covering approximately 1677-09-21 through 2262-04-11.
`unix_seconds()` uses floor division and `subsecond_nanoseconds()` lies in
[0, 1,000,000,000). One nanosecond before the epoch therefore decomposes to
seconds -1 and nanoseconds 999,999,999.

Both point families provide recoverable `elapsed_since`, direct comparisons
and `before`/`after`/`equals`. They cannot be mixed. Existing `clock_errc` values
are `unavailable` and `out_of_range`. The two private
`gti_rt_chrono_*_now_v1` entries retain scalar status/output ABI contracts; no
public calendar or zone type crosses that boundary.

## Errors And Scope

New calendar, timestamp and zone failures use `chrono::errc`: `invalid_date`,
`invalid_time`, `out_of_range`, `unavailable`, `unknown_zone`, `invalid_data`,
`unsupported_data`, `unsupported_coverage`, `ambiguous_time`, `nonexistent_time`,
`invalid_pattern` and `resource_limit`. Formatting adds the pattern position.
Duration errors remain `duration_errc`; existing clock errors remain
`clock_errc`. The payload enum `chrono::error` preserves any of these causes
(including formatting byte positions) when prefix `?` propagates into
`expected<T, chrono::error>`, through explicit `std::from` conformances.

The API does not add leap-second-aware UTC/TAI clocks, non-Gregorian calendars,
BCE/year-zero dates, general date-string parsing, full localization or a
recurring-schedule engine. Time-zone accuracy reflects the pinned data's
historical coverage and rules known at that release; future government changes
require an explicit data update or a newly built application.
