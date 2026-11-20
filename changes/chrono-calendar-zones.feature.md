Add validated Gregorian dates and civil times, distinct calendar counts, wide
Unix timestamps, IANA time-zone lookup with explicit daylight-saving resolution,
and checked temporal formatting to `std.chrono`. Fallible operations provide
recoverable `try_` APIs and convenience counterparts; infallible observations
remain direct. Ship a pinned time-zone snapshot for reproducible applications.
