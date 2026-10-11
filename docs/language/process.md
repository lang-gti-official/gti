# Processes And Environment

Status: Implemented bounded POSIX process execution and read-only environment access.

`<std/process>` provides synchronous child-process execution through exact
argument vectors. It exists for programs such as command-line clients that
need to invoke an editor, pager, or another tool without importing shell
parsing into GTI.

## Environment Observation

`<std/environment>` (or `import std.environment;`) provides
`std::environment::get(std::string_view name)`, returning
`expected<std::optional<std::string>, std::environment::errc>`:

- A missing variable is a successful empty optional.
- A present variable is an engaged optional holding an independent owned
  string, including when the value is empty.
- An empty name, `=` anywhere in a name, or an embedded NUL returns
  `errc::invalid_name`. No prefix lookup, shell expansion, or assignment occurs.
- Failure to allocate the native name/snapshot returns `errc::out_of_memory`.
  Allocating the ordinary GTI result string follows GTI's normal allocation
  failure contract instead; the temporary snapshot participates in ordinary
  cleanup, with allocation exhaustion retaining its abort policy.

```gti
#include <std/environment>
#include <std/string>

int main() {
  auto lookup = std::environment::get("APP_PROFILE");
  if (!lookup) { return 1; }
  auto profile = std::move(lookup).expect("checked lookup");
  if (profile) { std::println(profile.value().view()); }
  return 0;
}
```

Names are case-sensitive native POSIX bytes, not restricted to shell identifier
syntax. Values preserve native bytes without UTF-8 validation or replacement.
The owned result survives later lookups and subsequent environment changes;
any view made from it retains the ordinary checked string-owner lifetime.
Lookups observe the current environment, not a cached startup copy. Reading
configuration does not make its value trustworthy or suitable for shell use.

Safe GTI can query concurrently: this API never changes the environment.
Unsafe/native code that changes the process-wide environment (`setenv`,
`unsetenv`, `putenv`, or direct `environ` edits) must exclude concurrent
environment access, including foreign readers, and preserve all host validity
requirements. Returning an owned copy does not make a racing native mutation
safe. This slice intentionally provides no setter, enumeration, or child-local
override. Windows imports are rejected until their native encoding/lookup
contract is implemented; the private ABI also reports `unsupported` there.
Unknown targets are rejected by the compiler's target model, with an additional
defensive source-unit guard.

The runtime's versioned `gti_rt_environment_get_v1` copies a present value into
one immutable native snapshot. A source-defined temporary owner releases it
after copying into the public string. An owner-witnessed view covers only that
copy operation; neither libc storage nor the private handle escapes. Snapshot
metadata and bytes share one allocation; the GTI string reserves the exact
required capacity. Missing lookups allocate no snapshot or result string.
`examples/140-environment-configuration.gti` demonstrates absent, empty and
configured values without printing credentials.

## Command Construction

`std::process::command(executable)` owns the executable spelling and every
argument added with `arg(value)`. The executable shall be nonempty, and neither
it nor an argument may contain an embedded zero byte. Empty argument values are
valid and remain distinct argv elements.

`valid()` reports whether construction has retained a valid argv, and
`argument_count()` includes the executable. Once an invalid element is added,
the command remains invalid and `run()` returns
`std::process::errc::invalid_argument` without creating a child.

GTI performs no shell splitting, interpolation, expansion, quoting, globbing,
or redirection. Shell behavior is available only when the program explicitly
runs a shell executable and supplies that shell's arguments.

## Execution And Inheritance

`command::run()` creates one child, waits synchronously for termination, and
returns `expected<std::process::exit_status, std::process::errc>`. A command may
be run more than once. The current slice inherits the caller's:

- environment;
- working directory; and
- stdin, stdout, and stderr handles.

The current slice has no asynchronous child owner, pipeline, stream capture or
redirection, environment mutation, working-directory override, timeout,
cancellation, or process-group policy. Those require explicit ownership and
deadlock contracts rather than additions to this synchronous result.

## Termination Status

A successful `run()` means that GTI obtained a child termination status; it
does not mean that the child exited successfully.

`exit_status::exited()` distinguishes an ordinary exit from signal
termination. For an ordinary exit, `exit_code()` returns the exact host exit
code and `terminating_signal()` returns zero. For signal termination,
`signaled()` is true, `terminating_signal()` returns the host signal number,
and `exit_code()` returns `-1`. `success()` is true only for ordinary exit with
code zero.

The public API does not collapse signal termination into the shell convention
`128 + signal`, because that would lose the distinction between an ordinary
exit code and a signal.

## Recoverable Errors

`std::process::errc` has the following stable categories:

| Value | Meaning |
| --- | --- |
| `invalid_argument` | argv is empty, unterminated internally, has an empty executable, or contains an embedded zero |
| `unsupported` | the selected runtime has no implementation of this process family |
| `spawn_failed` | host resources or process creation failed before execution |
| `execute_failed` | the child could not execute the requested program |
| `wait_failed` | GTI could not obtain a coherent termination status |

An `execute_failed` result is distinct from a child that successfully starts
and exits with code 127. Native `errno`-style values remain private runtime
detail and are not a stable portable public enum.

Infallible allocation needed to own command text aborts on exhaustion without
cleanup rather than returning `process::errc`; size overflow retains the
contained defined-failure contract.

## Runtime Boundary

The source-defined wrapper serializes argv as counted NUL-delimited bytes and
calls `gti_rt_process_run_v1`. The runtime retains no caller storage. On POSIX
it uses an exec-error pipe with close-on-exec so the parent can distinguish
failure to execute from an ordinary child exit status. The older
`gti_rt_run_process_v1` build-program entry remains as a compatibility wrapper
with its historical normalized integer result.

The public class and enum types do not cross the runtime ABI, and the compiler
does not recognize their qualified names. Windows is rejected at import time
until a native implementation with exact UTF-16 argument quoting and handle
inheritance rules is delivered.
