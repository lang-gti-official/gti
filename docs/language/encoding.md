# Text and binary encoding

`<std/encoding>` provides portable source-defined conversion between counted
bytes and textual Base64 or hexadecimal representations, plus strict UTF-8
scalar conversion and traversal. The binary-to-text operations use
`std::string_view` as a byte view and return owning `std::string` values, so
embedded zeroes and every other byte value are preserved.

## API

```gti
#include <std/encoding>
#include <std/string>

std::string encoded = std::encoding::base64::encode("hello");
mut auto decoded = std::encoding::base64::decode(encoded.view());
if (!decoded) {
  return 1;
}

std::string digest_text = std::encoding::hex::encode(decoded.value().view());
```

The public operations are:

```gti
std::string std::encoding::base64::encode(std::string_view bytes);
expected<std::string, std::encoding::decode_errc>
std::encoding::base64::decode(std::string_view text);

std::string std::encoding::hex::encode(std::string_view bytes);
expected<std::string, std::encoding::decode_errc>
std::encoding::hex::decode(std::string_view text);
```

`<std/encoding>` includes its implementation dependency on `<std/string>`, but
optional standard units do not re-export declarations. A source file that
names `std::string` directly therefore also includes `<std/string>`.

## UTF-8 contract

`std::encoding::utf8` is implemented entirely in ordinary GTI. It does not
change `char`, string indexing, or string length: those remain byte-oriented.

| Operation | Result |
| --- | --- |
| `decode(view, offset = 0)` | `expected<scalar, error>` for one scalar at a byte offset |
| `validate(view)` | `expected<void, error>` for the entire counted byte view |
| `append(output, point)` | `expected<void, error>` appending one `uint32_t` scalar to a mutable string |
| `encode(point)` | `expected<std::string, error>` containing one scalar |
| `decoder{view}` | Incremental traversal borrowing a named `std::string_view` |

`scalar.value` is a `uint32_t` Unicode scalar value; `scalar.width` is its
encoded byte count (`std::size_t`, 1 through 4). `error.code` is `utf8::errc`
and `error.offset` is a byte offset into the input:

- `invalid_offset`: the requested offset is greater than the input size;
- `unexpected_end`: the requested scalar is missing or truncated, at input size;
- `invalid_lead`: a continuation byte, C0/C1, or F5 through FF starts a scalar;
- `invalid_continuation`: an expected continuation is not 80 through BF, at
  that byte;
- `overlong`, `surrogate`, `out_of_range`: a sequence would respectively encode
  a non-shortest form, U+D800 through U+DFFF, or a value above U+10FFFF, at its
  leading byte.

Malformed sequences are never replaced or skipped. Errors are reported as soon
as they are established; for example E0 80 is already overlong even without a
third byte. `decode` does not scan the preceding bytes to establish that an
arbitrary offset is a boundary. Empty input validates successfully, but decoding
at its end reports `unexpected_end`. Embedded NUL, BOM (not stripped), and
Unicode noncharacters are valid scalars. Append/encode reject surrogates and
values above U+10FFFF with offset zero; invalid scalar input never modifies the
destination. Allocation failure follows ordinary string allocation policy,
not the recoverable UTF-8 error channel.

```gti
#include <std/encoding>
#include <std/string>

auto text = std::encoding::utf8::encode(uint32_t(0x20ac)).expect("valid scalar");
auto view = text.view();
mut std::encoding::utf8::decoder scalars{view};
while (!scalars.done()) {
  auto next = scalars.next();
  if (!next) { return 1; }
  std::println(next.value().value);
}
```

The decoder borrows both the named view and its backing storage; neither may
be invalidated while it is used. `position()` returns the next byte offset.
`next()` advances only on success, and `done()` means that position equals the
input size. A caller must handle or stop after an error rather than retrying
forever. This is a fallible cursor, not a range-for iterator or chunked-stream
decoder. Decode/append take constant work per scalar; validation and complete
traversal take linear time and constant auxiliary storage. Decode, validate and
cursor traversal do not allocate. Encode allocates its returned string;
append uses the destination's ordinary capacity policy.

This first phase does not provide UTF-16 conversion, normalization, case folding,
grapheme segmentation, or changes to JSON's existing UTF-8 implementation.
The runnable example is [example 94](../../examples/094-binary-text-encoding.gti).

## Base64 contract

Base64 uses the standard RFC 4648 alphabet
`A-Z a-z 0-9 + /`. Encoding always emits canonical `=` padding and produces an
empty string for empty input.

Decoding is deliberately strict:

- the input length is a multiple of four;
- padding appears only in the final four-character quantum and has the exact
  one- or two-character shape required by the decoded byte count;
- non-alphabet characters, including ASCII whitespace, are rejected; and
- unused low bits in the final quantum are zero, so alternate spellings of the
  same bytes are rejected as non-canonical.

This API does not accept unpadded Base64, MIME line wrapping, or the URL-safe
`-`/`_` alphabet. Those policies require distinct operations rather than flags
that weaken the canonical decoder.

## Hexadecimal contract

Hex encoding emits two lowercase digits per byte. Decoding requires an even
number of digits and accepts both uppercase and lowercase ASCII letters. It
does not accept `0x` prefixes, separators, or whitespace.

## Errors and security boundary

Both decoders return `expected<std::string,
std::encoding::decode_errc>`. The error values are:

- `invalid_length` for an impossible encoded length;
- `invalid_character` for a character outside the selected alphabet;
- `invalid_padding` for structurally misplaced Base64 padding; and
- `non_canonical_input` for nonzero unused Base64 bits.

Base64/hex encoding is not encryption, authentication, validation of decoded
content, or a Unicode transformation. Applications handling secrets still need an
appropriate cryptographic API and should not assume these ordinary
source-defined loops provide constant-time behavior.
