# HTTP server connection costs

Run this from the GTI checkout with a Release compiler and its matching runtime.
Choose a scratch directory for binaries, compiler logs and `measurements.json`.

```sh
GTI_STDLIB_PATH="$PWD/stdlib" python3 benchmarks/probes/http-server-connections/run.py \
  build-release/gti /tmp/gti-http-probe
```

For each HTTP/1.1 workload, driver 0 feeds the public codec directly from a
byte stream. Driver 1 sends the same bytes through `open_server_connection`.
Both validate the same messages, consume every decoded byte and check a digest
of the application input and encoded output. Both use 4096-byte windows.
This comparison measures the cost of the blocking driver. Socket scheduling
is exercised separately by the two-client package test.

The script builds at LLVM `-O2`. It times seven alternating runs and records
each run's throughput inputs, per-request p99, transport calls and wire
digest. The workloads include persistent GET, a short JSON POST, a head
delivered one octet per read, uploads of 4096 and 1048576 octets, and a
rejected request on a fresh connection. A second build adds a distinct duplex
type with 128-byte reads and 16-byte writes to measure the change in optimized
text size and compile time. Both transport instantiations run.

On macOS, a separate instrumented executable counts successful malloc,
calloc, realloc and posix_memalign calls, and peak live allocator block bytes
inside the measured interval. A C control verifies the interposer. The peak
includes the codec, transport and application scratch. System-wide heap
metadata and the size of the preallocated timing-sample array are outside
this measure. Mach task counters record process system calls during each
timed batch, including counter observation. The scripted transport uses no
socket syscalls. On other hosts the timing and transport checks run and the
Mach counters have no meaning.

The resident probe parks up to 1024 codecs idle or one byte into sending a
64 MiB response, then services another connection and drops all owners.
The persistent probe compares peak heap use across 1, 100 and 20000 requests.

## macOS arm64 measurement

Measured on 2026-11-23 using a Release GTI compiler built from `f2c84c8c9`.
Rates are requests per second. Tail latency is the median p99 in nanoseconds
across the seven timed runs. Heap figures come from the separate diagnostic
build. Both drivers made the same number of read, write and flush calls and
produced the same digest for each workload.

| Workload | Direct req/s | Blocking req/s | Direct p99 | Blocking p99 | Blocking allocs/request | Blocking peak bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| GET | 2,081,526 | 2,035,710 | 584 | 584 | 11.00 | 8,656 |
| JSON POST | 1,544,667 | 1,497,714 | 792 | 833 | 14.00 | 9,008 |
| GET, one byte per read | 1,539,409 | 1,377,363 | 750 | 833 | 11.00 | 8,656 |
| 4 KiB upload | 151,289 | 151,201 | 7,084 | 6,958 | 14.02 | 9,008 |
| 1 MiB upload | 655 | 654 | 1,583,667 | 1,588,292 | 14.32 | 9,008 |
| Fresh connection, rejected request | 99,706 | 187,880 | 10,459 | 5,625 | 18.00 | 8,368 |

The GET used one read, one write and one flush per request. Fragmentation
raised reads to 35. The two uploads used 2 and 257 reads respectively.
The timed in-memory batches issued no Unix syscalls. The blocking GET held
8,656 live bytes at its peak for 1, 100 and 20,000 requests. With a second
connection servicing a GET, 1,024 idle codecs peaked at 418,176 bytes, and
1,024 codecs with stalled responses peaked at 565,632 bytes. All measured
workload allocations were retired by the end of each batch.

One concrete stream type produced 85,064 bytes of native text in 8.97 s.
Two produced 98,048 bytes in 10.41 s. These build times include the compiler
and native linker on this host.

The external-package loopback test also ran with a diagnostic build of
`socket_counts_macos.c` injected into the server alone. Its counters in one
successful 64 MiB run were

```text
accepts=3 sends=1406 receives=7 polls=280 would_block_writes=278 would_block_reads=2 sent_bytes=67109089 received_bytes=342
```

Socket buffering and scheduling determine the individual call counts. The
fixture checks exact wire bytes and independent progress.
