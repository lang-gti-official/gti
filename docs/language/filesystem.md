# Filesystem

`std.filesystem` provides owning lexical paths and recoverable host filesystem
operations. Public types, error mapping, iteration policy, and deterministic
cleanup are ordinary GTI. Host operations use the versioned bounded C ABI in
`runtime/include/gti/filesystem.h`; no public filesystem name has compiler
meaning.

## Paths and errors

`std::filesystem::path` keeps its supplied spelling and remains move-only.
`clone()` and `string()` produce independent owners. `view()` borrows its bytes;
the ordinary last-use lifetime rules prevent mutation or destruction of the
owner while that view remains live. Decomposition, joining, comparisons,
modifiers, and `lexically_normal()` perform no filesystem access.

Host functions accept `std::string_view` or `path&`. Operations involving two
paths provide either two views or two path references. Path inputs must be
nonempty and must not contain embedded NUL bytes: invalid input returns
`errc::invalid_argument` before a host operation. POSIX paths preserve native
bytes; Windows uses UTF-8 at the boundary and native wide paths internally.

Operations return `expected<T, std::filesystem::errc>`. Stable error categories
are `invalid_argument`, `not_found`, `permission_denied`, `already_exists`,
`not_directory`, `is_directory`, `directory_not_empty`, `too_many_symlinks`,
`no_space`, `read_only`, `cross_device`, `name_too_long`, `out_of_memory`,
`unsupported`, and fallback `io_error`. Host exceptions never cross the C ABI.
Infallible allocation of ordinary GTI strings/vectors aborts on exhaustion
without cleanup; size-computation overflow remains a contained defined failure.
The Expected channel covers the documented host operations and their native
allocation, not implicit recoverability of all GTI-side allocation.

## Observation and mutation

| Operation | Success value and behavior |
| --- | --- |
| `status`, `symlink_status` | `file_status`, whose `type()` reports `file_type`; only `status` follows links |
| `exists`, `is_regular_file`, `is_directory`, `is_symlink` | `bool`; path queries are fallible, while `file_status` overloads are pure predicates |
| `file_size` | `uint64_t` regular-file byte count |
| `is_empty` | `bool` for a zero-byte file or directory without entries |
| `equivalent` | Whether two existing paths identify the same host file |
| `create_directory`, `create_directories` | Whether a directory was created; the latter also creates missing parents |
| `remove` | Whether one file, link, or empty directory was removed |
| `remove_all` | Count of removed entries, including the supplied root |
| `rename` | `void`; native rename semantics, including replacement where the host permits it |
| `copy_file` | Whether regular-file contents were copied |
| `create_symlink`, `create_directory_symlink`, `create_hard_link` | `void`; first argument is the target, second the new link path |

`file_type` distinguishes `not_found`, `regular`, `directory`, `symlink`,
`block`, `character`, `fifo`, `socket`, and `unknown`. An absent path is a
successful `not_found` status. Thus `exists` returns false for a missing path
or dangling symbolic link. `symlink_status` and `is_symlink` inspect the link
itself. Other host errors, such as denied traversal permission, remain errors.

Creating an already-existing directory succeeds with false. Removing an absent
path succeeds with false, or zero for `remove_all`. `remove` fails on a
nonempty directory. Both removal functions remove symbolic links themselves;
recursive removal does not descend through a symbolic-link entry. Missing
parents are not synthesized by copy, rename, or link creation.

`copy_file(from, to)` fails if the destination exists. Its explicit third
argument may be `copy_options::none`, `skip_existing`, or
`overwrite_existing`; skip returns false, and overwrite authorizes replacement
of existing regular-file contents. File copying follows symbolic links.
Copying a file onto itself fails. Recursive copying and metadata preservation
controls are not provided by this operation.

Filesystem observation and later mutation are separate OS operations. They
provide no security containment or atomic check-then-use guarantee against
concurrent changes. Partial mutation, such as some successful recursive removals
before a later permission error, is not rolled back. Current-directory changes
affect process-wide relative path resolution.

## Host path resolution

`current_path()` returns an owned absolute working-directory path;
`current_path(path)` changes it. `temp_directory_path()` reports the host's
temporary-directory location without creating a private temporary directory.
`absolute(path)` resolves against the current directory without requiring the
path to exist. `canonical(path)` requires existence and resolves symbolic links
and dot components. `weakly_canonical(path)` resolves the existing prefix and
normalizes the remaining suffix. `read_symlink(path)` returns the stored link
target, which may be relative or dangling.

## Directory iteration

`directory_iterator::open(path)` returns a move-only directory stream positioned
before the first entry. `next()` is a mutable, fallible operation: true means
an entry is available, false means end of stream. `entry_path()` returns a new,
independent owning path for the current entry, or `invalid_argument` before the
first successful `next()` and after end/error. Previously returned paths remain
valid when the iterator advances or is destroyed. Iterator destruction closes
its native resources through normal GTI cleanup, including failure unwinding.

```gti
import std.filesystem;
import std.string;

expected<void, std::filesystem::errc> list(std::filesystem::path& directory) {
  mut auto entries = ?std::filesystem::directory_iterator::open(directory);
  while (?entries.next()) {
    mut auto entry = ?entries.entry_path();
    std::println(entry.view());
  }
  return;
}
```

`directory_entries(path)` consumes such a stream and returns
`expected<std::vector<path>, errc>`. The returned vector is an ordinary owning
range, useful for sorting, retaining, or processing entries after enumeration.
It uses memory proportional to all returned paths; the stream retains only the
current path plus the host iterator state. A read error returns an error instead
of publishing a partial vector.

Both forms are nonrecursive, omit `.` and `..`, and use unspecified host order.
Directory symlinks are entries and are not traversed. Concurrent changes may
or may not become visible during enumeration; collecting an owning vector does
not make the host traversal atomic. Streams are neither transferable nor
shareable between threads; independent owned paths can be transferred normally.
Recursive traversal, filesystem watches, permissions mutation, timestamps, and
space queries remain outside this surface.
