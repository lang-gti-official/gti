# JSON

Status: Implemented source contract and native C++ execution.

`<std/json>` provides a strict, owned JSON document model, recoverable parsing
and conversion, deterministic serialization, and explicit codecs for mapping
application types. It is ordinary GTI source-defined policy: no public JSON
type or operation has compiler recognition.

The complete surface and codec fixtures pass semantic analysis, verified MIR,
and native execution. Owning `expected<T, E>` error alternatives retain their
cleanup and move-only behavior through both the C++20 and C++23 representations;
the owned error contract does not require copying or borrowing error strings.

## Model

`std::json::value` is a move-only owner with one `std::json::kind`:

- `null_value`
- `boolean`
- `number`
- `string`
- `array`
- `object`

Parsed values own all descendant strings, keys, number lexemes, arrays, and
objects. Nothing in the document borrows the source text. `clone()` performs
an explicit deep copy.

Objects preserve source or insertion order. `set` replaces an existing key in
place and does not reorder it. The parser rejects duplicate keys, so a parsed
object never loses an earlier member silently. Missing keys and JSON `null`
are distinct: `contains` observes membership while `is_null` observes a stored
value.

`operator[]` is deliberately absent. It cannot insert a missing object member
or convert a value implicitly.

## Parsing

```gti
#include <std/json>
#include <std/string>

mut auto parsed = std::json::parse(response_body.view());
if (!parsed) {
  std::println(parsed.error().description());
  return 1;
}

mut std::json::value document = std::move(parsed).value();
auto login = document.get_string("login");
auto id = document.get_uint64("id");
```

Parsing accepts the JSON grammar only. In particular it:

- requires valid UTF-8 in unescaped string contents;
- decodes JSON escapes and UTF-16 surrogate pairs into UTF-8;
- rejects unescaped control characters, invalid escapes, malformed UTF-8,
  lone surrogates, and out-of-range Unicode scalar values;
- rejects comments, trailing commas, leading-zero numbers, duplicate object
  keys, and characters after the root value; and
- preserves the exact source lexeme for every number.

`parse_options` bounds document bytes, decoded string bytes, node count, and
nesting depth. Defaults are 64 MiB per document, 16 MiB per decoded string,
one million values, and 128 nested values. A limit failure is recoverable and
never returns a partial document.

## Errors

`std::json::error` owns its JSON Pointer path and may outlive both the input
text and document. Syntax errors additionally expose a zero-based byte offset
and one-based line and byte column. `has_source_location()` distinguishes
syntax locations from conversion and member-access errors.

`description()` returns stable explanatory text. Programs should branch on
`code()` rather than parse that text.

The principal error families are malformed input, configured resource limits,
missing keys, kind mismatches, and numeric conversion failures. A successful
parse followed by a failed typed getter leaves the document unchanged.

## Access

The recoverable scalar operations are:

```gti
value.as_boolean();
value.as_string();
value.as_int64();
value.as_uint64();
value.as_double();

object.get("owner");
object.get_string("title");
object.get_boolean("private");
object.get_int64("count");
object.get_uint64("id");
object.get_double("ratio");
```

`get` returns a deep owned clone because GTI does not currently admit
`expected<T&, E>`. After a program has validated a shape, `at(key)` and
`at(index)` return borrowed values without cloning. Their missing-key,
wrong-kind, and out-of-range cases use GTI's defined checked runtime failure;
they never insert.

Arrays expose `elements()` for ordinary range iteration. Objects expose
`object_size()`, `key_at(index)`, and `value_at(index)`, which retain insertion
order without exposing separate mutable key and value sequences that could
break the object invariant.

## Numbers

JSON numbers are stored as exact decimal lexemes rather than binary floating
point. `as_int64` and `as_uint64` accept only integer-form lexemes and perform
checked range conversion, including the full signed and unsigned 64-bit
ranges. `as_double` performs a checked decimal conversion and reports
`number_out_of_range` instead of producing infinity or NaN.

Builders provide exact signed and unsigned integer factories plus
`number_text`, which validates a caller-supplied JSON number lexeme. Floating
construction should use a validated decimal lexeme until a shortest-round-trip
binary32/binary64 formatter exists; the library does not silently narrow
`double` or route JSON through the fixed-display `std::to_string(float)` API.

## Building And Serializing

```gti
mut std::json::value body = std::json::value::object();

auto named = body.set("name", "demo");
if (!named) {
  return 1;
}

auto private_value = body.set("private", true);
if (!private_value) {
  return 2;
}

mut std::json::value labels = std::json::value::array();
auto first = labels.push("bug");
auto second = labels.push("compiler");
if (!first || !second) {
  return 3;
}

auto stored = body.set("labels", std::move(labels));
if (!stored) {
  return 4;
}

mut std::string compact = std::json::stringify(body);
mut std::string readable = std::json::stringify_pretty(body, uint8_t(2));
```

Builders validate UTF-8 for keys and strings. `set` and `push` consume an
owned `value`; overloads construct strings, booleans, and 64-bit integers.

Serialization is deterministic and locale-independent. Compact output uses no
unnecessary whitespace. Pretty output changes whitespace only. Object order
and exact number lexemes are preserved, while strings use the shortest normal
JSON escape for quotes, reverse solidus, control characters, and common
control escapes. Non-ASCII UTF-8 is emitted directly.

## Explicit Codecs

The library performs no reflection or implicit field mapping. An application
may define one exact `std::json::codec<Model>` specialization for a nominal
type it owns:

```gti
class std::json::codec<GiteaUser> {
public:
  expected<GiteaUser, std::json::error>
  decode(std::json::value& input) {
    mut auto id = input.get_uint64("id");
    if (!id) {
      return unexpected(std::move(id).error());
    }
    mut auto login = input.get_string("login");
    if (!login) {
      return unexpected(std::move(login).error());
    }
    mut std::string owned_login = std::move(login).value();
    return GiteaUser(id.value(), std::move(owned_login));
  }

  expected<std::json::value, std::json::error>
  encode(GiteaUser& input) {
    mut std::json::value output = std::json::value::object();
    mut auto id = output.set("id", input.id());
    if (!id) {
      return unexpected(std::move(id).error());
    }
    mut auto login = output.set("login", input.login().view());
    if (!login) {
      return unexpected(std::move(login).error());
    }
    return std::move(output);
  }
};
```

`decode<T>(text)`, `encode<T>(value)`, and `decode_array<T>(text)` invoke that
exact specialization. `decode_array` prefixes a failed element's JSON Pointer
with its array index; codecs may use `error::prepend_path` when propagating
failures through other nested structures. The empty primary codec supplies no
fallback behavior. This keeps key choice, required versus optional fields,
defaulting, and schema evolution in application code. A future reflection or
derive facility may generate the same specialization without changing this
API.
