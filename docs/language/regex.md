# Regular Expressions

`import std.regex;` provides ECMAScript regular expressions with the names and
pattern grammar of C++ `std::regex`. Compilation, matching and replacement are
ordinary GTI in `stdlib/std/regex.gti`; no regex type or operation has
compiler recognition. [Example 194](../../examples/194-regular-expressions.gti)
shows the common operations.

```gti
std::regex date = std::regex("(?<year>\\d{4})-(?<month>\\d\\d)");
std::string text{"released 2024-03"};
std::match_results m = std::regex_search(text.view(), date);
if (m) {
  std::println(m.str("year"));     // 2024
  std::println(m.format("$2/$1")); // 03/2024
}
```

## Where it differs from C++

The API follows C++ where its design holds up and departs where
`std::regex` drew lasting criticism:

| C++ `std::regex` | GTI `std::regex` |
| --- | --- |
| An invalid pattern throws `regex_error`. | `regex::compile` returns `expected<regex, regex_error>`; the error carries a `regex_errc` and the byte offset. The `regex(pattern)` constructor, for patterns known to be valid such as literals, panics instead. |
| Common implementations backtrack exponentially (`(a*)*b`) and recurse on the native stack, so long subjects can crash. | Patterns without backreferences or lookaheads match in time linear in the subject. The matcher and the compiler use explicit heap stacks, so neither subject length nor nesting depth can overflow the native stack. |
| `match_results` holds iterators into the subject; matching a temporary `std::string` leaves them dangling (only partly prevented by deleted overloads). | A `match_results` borrows its subject. The borrow checker rejects changing or destroying the subject while a result is used, and returning a result past its subject. |
| Results come back through an out-parameter plus a `bool`. | `regex_match` and `regex_search` return the `match_results`; it tests false when nothing matched. |
| No named groups. | `(?<name>...)`, `\k<name>`, `$<name>`, and name-based accessors. |
| Six grammars, locale-dependent `regex_traits`, collation. | One grammar (ECMAScript). Classes are the C locale's; matching is by byte. |
| `regex_replace` takes only a format string. | `regex_replace_with` also takes a callable that computes each replacement. |
| `syntax_option_type` and `match_flag_type` bitmasks. | `regex_options` and `regex_format_options` structs with named fields: `std::regex_options icase{.icase = true};`. |

## Patterns

The grammar is ECMAScript's, as in C++ `std::regex_constants::ECMAScript`:

- literals; `.`; alternation `|`; groups `(...)`, `(?:...)` and named
  `(?<name>...)`; lookaheads `(?=...)` and `(?!...)`;
- quantifiers `*`, `+`, `?`, `{n}`, `{n,}`, `{n,m}`, each made lazy by a
  trailing `?`; counts are at most 10000;
- classes `[...]` and `[^...]` with ranges, the escapes below, and
  `[:alpha:]`, `[:digit:]`, `[:alnum:]`, `[:space:]`, `[:upper:]`,
  `[:lower:]`, `[:punct:]`, `[:xdigit:]`, `[:blank:]`, `[:cntrl:]`,
  `[:graph:]`, `[:print:]`; `[=c=]` and `[.c.]` name single bytes; `[]`
  matches nothing and `[^]` matches any byte;
- anchors `^` and `$`, and word boundaries `\b` and `\B`;
- escapes `\d \D \w \W \s \S`, `\n \r \t \f \v \0`, `\cX`, `\xHH`, `\uHHHH`
  (a code point above `\u007F` stands for its UTF-8 bytes outside classes),
  and identity escapes of non-alphanumeric bytes; inside a class `\b` is a
  backspace;
- backreferences `\1` through `\99` and `\k<name>`. A backreference to a group
  that has not matched matches the empty string.

Matching is leftmost-first: alternatives are tried left to right and
quantifiers prefer more (or, when lazy, fewer) repetitions, as in ECMAScript.
An iteration of a quantified group that matches the empty string ends the
repetition. Matching is by byte: `.` and a negated class match one byte,
`\w` and the POSIX classes are ASCII, and `icase` folds ASCII letters only.

`regex_options` fields select:

- `icase`: letters match either case;
- `nosubs`: groups do not capture, `mark_count()` is 0 and a backreference
  is an error;
- `multiline`: `^` and `$` also match after and before `\n` or `\r`;
- `dotall`: `.` also matches `\n` and `\r` (ECMAScript's `s` flag).

## Errors

`regex::compile` reports the first problem as a `regex_error` with `code`,
`position` (a byte offset in the pattern) and `description()`. The
`regex_errc` enumerators keep C++'s names without the `error_` prefix:
`paren` (unmatched `(` or `)`), `brack` (unterminated class), `brace` (an
unterminated `{` count), `badbrace` (an invalid count or `n > m`), `badrepeat`
(a quantifier with nothing to repeat, after an assertion, or doubled),
`escape`, `range` (`[b-a]` or a class escape as an endpoint), `backref` (a
group that does not exist), `ctype` (an unknown `[:name:]`), `collate`
(`[.ab.]`), `complexity` (the compiled program would exceed 200000
instructions, as `(a{1000}){1000}` would), `stack` (groups nested more than
1000 deep, or lookaheads nested more than 32 deep), and `space`.

## Matching

- `regex_match(subject, re)` / `re.match(subject)`: the whole subject.
- `regex_search(subject, re)` / `re.search(subject)`: the leftmost match.
- `regex_search(subject, re, from)` / `re.search(subject, from)`: the leftmost
  match starting at or after byte offset `from`. Anchors and `\b` still see
  the text before `from`, like C++ `match_prev_avail`.
- `re.matches(subject)` and `re.contains(subject)`: the same tests without a
  result; `re.count(subject)` counts non-overlapping matches.

A `match_results` reports `size()` groups (0 when nothing matched, otherwise
`mark_count() + 1`), and for a group number or name: `matched`, `position`
(`std::string::npos` when unmatched), `length`, `str` (an owned copy),
`equals(group, text)` (a comparison without copying), and `operator[]`, a
`sub_match` with `matched`, `first`, `second` and `length()`. `prefix()` and
`suffix()` copy the text around the match, and `format(replacement)` expands a
replacement string against it.

To visit every match, search from `next_position()`, which steps past an empty
match so the loop always advances:

```gti
mut std::size_t from = 0;
while (true) {
  std::match_results m = std::regex_search(text.view(), re, from);
  if (!m) { break; }
  use(m.str());
  from = m.next_position();
}
```

A result borrows its subject, so it cannot yet be passed to a lambda or
stored in a container. For those uses, `re.find_all(subject)` returns a
`std::vector<std::match_groups>`: owned copies of each match's groups with
the same accessors (`str` returns a view of the copy).

## Replacing and splitting

`regex_replace(subject, re, replacement)` and `re.replace` substitute every
match. The replacement string expands `$&` (the match), `` $` `` and `$'` (the
text before and after it), `$1` through `$99`, `$<name>` and `$$`; any other
`$` is literal, and a group that did not take part expands to nothing. A
`regex_format_options` argument with `.first_only` set replaces only the first
match, and one with `.no_copy` set omits the text between matches:

```gti
std::regex_format_options first{.first_only = true};
std::string once = std::regex_replace(text.view(), re, "[$&]", first);
```

`regex_replace_with(subject, re, f)` and `re.replace_with(subject, f)` call
`f(match_groups&)` for each match and insert the `std::string` it returns.

`re.split(subject)` returns the owned pieces between matches, like a C++
`regex_token_iterator` over submatch -1. An empty match at either end of the
subject does not split, so `std::regex("").split("abc")` is `a`, `b`, `c`.

## Performance

Compilation parses the pattern into a tree and emits a small instruction
program, both without native recursion. Matching runs a backtracking machine
with its own stack of choice points. For patterns without backreferences or
lookaheads, the machine records each (choice point, position) pair it has
already tried and fails at once on a repeat, so a match costs at most the
program's choice points times the subject length, however the pattern
nests. The record grows only as far as a search reaches, and a loop of
searches clears only what the previous search set. A search also skips start
positions whose byte cannot begin a match, and a pattern starting with `^`
(without `multiline`) is tried only at offset 0.

Patterns with backreferences or lookaheads keep full ECMAScript semantics and
can take exponential time on adversarial input, as in other backtracking
engines. Lookaheads recurse on the native stack once per nesting level, which
the 32-level limit bounds.

On a 10 MB generated log, counting `\d+` matches, a failing search for
`[a-z]+@[a-z]+\.com` and replacing `\b[a-z]+-[a-z]+\b` took about 60, 160 and
120 ms at `-O3` (Apple M-series, 2026-11-15), against 790, 2460 and 1020 ms
for libc++ `std::regex` on the same text.
