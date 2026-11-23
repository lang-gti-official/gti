# HTTP And HTTPS

Status: current bounded standard-library contract

`import std.http;` provides a synchronous reusable HTTP client implemented
as ordinary GTI policy over a versioned private runtime ABI. `send` buffers a
complete response; `send_stream` returns after the final response headers and
exposes a bounded pull-based body through the general `std::io::reader` protocol.
Neither API invokes the `curl` executable.

```gti
import std.http;
import std.string;

int main() {
  mut auto created = std::http::client::create();
  if (!created) {
    return 1;
  }
  mut auto client = std::move(created).value();

  mut std::http::request request = std::http::request(
      std::http::method::get,
      std::http::url::checked("https://gitea.example/api/v1/version"));
  request.set_header("Authorization", "token secret");

  mut auto received = client.send(request);
  if (!received) {
    return 2;
  }
  mut auto response = std::move(received).value();
  std::string body = response.body();
  return response.status() == uint32_t(200) ? 0 : 4;
}
```

Applications name `std::string` or `std::vector` directly only after including
their defining modules; `std.http` does not re-export its implementation
dependencies.

## Public Surface

The request methods are `get`, `post`, `put`, `patch`, `delete_`, and `head`.
`delete_` carries the suffix because `delete` is reserved syntax.

### URLs

`std::http::url` is an owned, move-only absolute HTTP(S) URL. It can be stored
as a class field; an API wrapper normally stores one parsed base URL, clones it
for each operation, appends endpoint components, and reuses one
`std::http::client` for its connection pool.

A URL owns one buffer holding its serialization, plus the offsets of its
components. `scheme()`, `host()`, `path()`, `query()` and `view()` are views
into that buffer, so observing a component never allocates. A retained view
still protects the whole URL against mutation or consumption, and cannot
escape a temporary URL. Builders edit the buffer in place. Internal
construction bridges are private; public factories and `clone()` retain the
parsing and ownership contract.

A URL known when the program is written is best spelled as a checked
literal. The literal is validated during compilation, so the factory cannot
fail:

```gti
std::http::url base = std::http::url::checked("https://gitea.example/api/v1");
mut auto endpoint = base / "repos";
endpoint.add_query_parameter("limit", "20");

mut auto received = client.get(endpoint);
```

A URL that arrives at run time is parsed and its error handled:

```gti
mut auto parsed = std::http::url::parse(configured_base);
if (!parsed) {
  return 1;
}
mut std::http::url endpoint = std::move(parsed).value();
endpoint.push_path_segment("repos");
```

The construction and observation surface is:

```gti
static expected<url, url_errc> parse(string_view absolute);
static url checked(url_literal absolute);
static expected<url, url_errc> https(string_view authority);
url clone();
string_view scheme();
string_view host();
bool has_explicit_port();
uint16_t port();
string_view path();
bool has_query();
string_view query();
void push_path_segment(string_view value) mut;
void operator/=(string_view value) mut;
url operator/(string_view value);
url operator/(string_view value) &&;
void add_query_parameter(string_view name, string_view value) mut;
void set_query_parameter(string_view name, string_view value) mut;
void clear_query() mut;
string_view view();
string string();
```

`parse` accepts only absolute `http://` and `https://` URLs. Scheme matching is
ASCII case-insensitive and stored in lower case. Host, path, query, explicit
port spelling, and valid existing `%HH` escapes otherwise retain their input
spelling. `https(authority)` is the convenient secure factory for an ASCII
host, bracketed IPv6 host, or either form followed by a port; it does not
accept a path or query. Credentials and fragments are deliberately outside
this abstraction.

`url::checked` takes a `std::http::url_literal`, a passive value whose
`implicit consteval` constructor runs the same parser during compilation. A
string constant argument converts to it; an invalid URL stops compilation at
that argument with a message naming the problem (for example, that the port is
not a decimal number from 0 to 65535). A runtime string is rejected with
`GTI-S2057`; use `parse` for it. A `url_literal` can also be stored or passed
through wrappers to keep that proof.

`port()` returns an explicit port when present and otherwise the scheme
default, 80 or 443. `has_explicit_port()` distinguishes those cases. `path()`
is the encoded path, beginning with `/`, or empty when the URL names only an
authority. `query()` is the encoded text after `?`; `has_query()` reports
whether a `?` is present at all, so `https://h/p?` has an empty but present
query. `view()` is the whole serialization and `string()` makes an
independent owned copy. `clone()` makes an independently mutable URL, copying
one buffer.

`base / "api" / "v1"` preserves a named `base`: the first `/` clones it,
then consuming `/` overloads reuse the owned intermediate. A fresh URL or
`std::move(base) / "api"` selects the consuming overload immediately. Moving
`base` makes it unavailable afterward and is rejected while a live view borrows
it. `base /= "api"` appends in place and requires a mutable receiver.

These operators append one raw path segment, exactly like
`push_path_segment`; they do not resolve relative URLs or combine authorities.
Pass a `string_view`, not another `url`. Query text and the authority survive
unchanged. Consuming chains avoid whole-URL clones. Appending to a URL with no
query writes the encoded segment directly onto the end of its buffer; with a
query present, the segment is inserted before the `?`. Either may grow the
buffer.

Builder inputs are raw components. Every byte outside ASCII unreserved text is
written as an uppercase percent escape: a space becomes `%20`, `+` becomes
`%2B`, `/` inside one path segment becomes `%2F`, and `%` becomes `%25`.
Builders never use form-style `+` for spaces. `add_query_parameter` preserves
existing fields and appends another. `set_query_parameter` compares existing
names after `%HH` decoding, replaces the first match, removes later duplicate
matches, and appends when absent; a literal `+` remains a plus. `clear_query`
removes both the query text and its `?` delimiter.

URL construction failures use `url_errc`: `invalid_scheme`, `missing_host`,
`invalid_host`, `invalid_port`, `invalid_percent_encoding`,
`credentials_not_allowed`, and `fragment_not_allowed`. They are separate from
transport `errc` because no request has started.

### Requests, responses, and clients

`std::http::request(method, string_view)` copies its URL text. The
`request(method, url&)` overload copies the rendered owned URL, and `set_url`
has the same two forms. A request is move-only and provides:

- `add_header(name, value)` to preserve another field, including duplicates;
- `set_header(name, value)` to replace every case-insensitive match with one
  field;
- `set_body(string)` and `set_body(string_view)`;
- `set_connect_timeout_ms` and `set_timeout_ms`, whose defaults are 10 and 30
  seconds respectively;
- `set_max_response_bytes`, whose default is 16 MiB;
- `set_redirect_policy` and `set_redirect_limit`, described below;
- `set_scheme_policy(scheme_policy)` to restrict initial and redirected URLs; and
- `set_ca_bundle(path)` for a PEM trust bundle used by this request; and
- `rejection()`, the first builder input the request refused, if any.

Builders return nothing. Header names must be non-empty RFC token text, and
NUL, carriage return, and line feed are refused in values, so these APIs
cannot be used for request smuggling by line injection. A CA path containing
NUL is refused. A refused input is not applied; the request records the first
refusal, and `send`, `send_stream` and every client verb return it
(`errc::invalid_header` for a header, `errc::request_failed` for a CA path)
before contacting anything. One check of the send result therefore covers both
invalid input and transport failure, and `rejection()` reports the same refusal
earlier for a caller who wants it.

`std::http::client::create()` returns `expected<client, errc>`. A client is
move-only, is neither transferable nor shareable, and admits one request at a
time through a mutable borrow. Reusing it retains the backend connection pool.
Its operations are:

```gti
expected<response, errc> send(request& value) mut;
expected<streaming_response, errc> send_stream(request& value) mut;
expected<response, errc> get(string_view url) mut;
expected<response, errc> get(url& target) mut;
expected<response, errc> post(string_view url, string_view body) mut;
expected<response, errc> post(url& target, string_view body) mut;
expected<response, errc> put(string_view url, string_view body) mut;
expected<response, errc> put(url& target, string_view body) mut;
expected<response, errc> patch(string_view url, string_view body) mut;
expected<response, errc> patch(url& target, string_view body) mut;
expected<response, errc> delete_(string_view url) mut;
expected<response, errc> delete_(url& target) mut;
expected<response, errc> head(string_view url) mut;
expected<response, errc> head(url& target) mut;
int32_t last_native_code();
string last_error_message();
```

`last_native_code` and `last_error_message` describe the most recent failed
`send` for logging. Portable control flow branches on `errc`, never on a
libcurl code or diagnostic string. Client creation can fail before a client
exists and therefore reports only its `errc`.

A response is move-only and owns the complete buffered native response. It
exposes `status()`, an owned `body()` copy, and an owned `headers()` vector.
Header order, original spelling, and duplicates are preserved. Returning
copies prevents a view from escaping the response owner and lets the private
runtime representation change without changing public layout.

## Outcomes And Limits

The recoverable categories are `invalid_url`, `invalid_header`,
`unsupported_scheme`, `name_resolution_failed`, `connection_failed`,
`tls_failed`, `timeout`, `request_failed`, `response_too_large`,
`backend_unavailable`, `client_creation_failed`, `cancelled`,
`too_many_redirects`, and `redirect_rejected`.

HTTP status codes are not transport failures. A 404 or 500 is a successful
`response` whose `status()` carries that value. Only URL construction,
resolution, connection, TLS, timeout, response-size, backend, and native
request failures use the unexpected alternative.

Only `http://` and `https://` URLs are accepted. GET and HEAD reject a nonempty
body. Redirect following is disabled by default, so a 3xx response is returned
to the caller. Explicit redirect policy applies to both buffered and streaming
sends. The configured response limit applies to the decoded body for buffered
responses and the content-encoded body bytes for streaming responses. Response
headers also have an independent 1 MiB hard limit. Bodies preserve binary bytes,
including NUL.

## Streaming Responses And General I/O

`send_stream(request)` returns `expected<streaming_response, errc>` after the
final response headers have arrived. It does not wait for the complete body.
Construction, connection, TLS, and redirect failures before those headers are
returned as errors. A body error observed in the same transport step as the
headers is retained on the response and reported by its reader, so the status
and headers remain available for diagnosis.

A streaming response is a move-only owner of an independent transfer. It may
outlive its originating request and client; multiple responses may remain alive
at once. It is neither transferable between threads nor shareable. Status and
header inspection does not advance the body. `headers()` reports the final
response header block; later trailer fields do not mutate it. The public operations are:

```gti
uint32_t status();
vector<header> headers();
string effective_url();
expected<size_t, std::io_errc> read_some(mut mutable_span<uint8_t>& output) mut;
void cancel() mut;
uint64_t bytes_read();
optional<errc> last_error();
int32_t last_native_code();
```

`read_some` writes at most the caller's span size. A positive count is a prefix
of body bytes; zero with a nonempty output means complete body EOF. A zero-size
output is a successful no-op and does not establish EOF. EOF remains EOF on
later reads. Truncated content-length bodies and malformed/incomplete chunked
framing are errors rather than successful EOF. On failure, `last_error()` keeps
the specific HTTP error and `last_native_code()` provides native diagnostic
information when available. Generic readers observe `timed_out` for timeout and
`read_failed` for other transfer errors, including size-limit and cancellation
failures. Errors remain observable on subsequent reads.

`bytes_read()` counts bytes delivered to caller buffers, not bytes prefetched
by the transport. It enables caller-defined progress reporting without a stored
callback or a special download abstraction. `cancel()` terminates an unfinished
transfer and is idempotent; subsequent reads report cancellation. This is
synchronous cancellation between reads, not cross-thread interruption of a
currently blocking call. Request timeouts bound blocking transport work. The
total timeout spans redirect hops and caller pauses between reads; it is not
reset by each successful read.

The streaming body satisfies `std::io::reader`, so it composes with
`read_exact`, `getline`, `buffered_reader`, and `copy`. The destination can be a
file, socket, memory writer, or an unrelated user-defined writer. `copy` uses
caller-provided scratch storage, reports the committed destination prefix on
failure, and does not flush or close the destination:

```gti
mut auto response = ?client.send_stream(request);
// HTTP status policy belongs to the caller; 404 is still a response.
if (response.status() != uint32_t(200)) { return unexpected(my_error); }
mut std::byte_buffer bytes{std::size_t(65536)};
mut auto scratch = bytes.as_mutable_span();
auto copied = std::io::copy(response, destination, scratch);
```

The example is schematic because the enclosing error type and destination are
application policy. See [the runnable HTTP example](../../examples/093-http-json-client.gti)
for a complete streaming-to-file mode. Atomic publication, hashing, retry
policy, file permissions and durability remain separate composable operations;
HTTP does not create or rename application files.

The native streaming profile bounds its owned body staging to 64 KiB rather
than the response size and pauses transport when that storage is full. It
currently uses HTTP/1.1 to avoid HTTP/2 flow-control buffering outside that bound.
It requests identity content encoding and does not decompress streaming bodies.
If a server sends a content encoding, or the caller explicitly requests one,
`read_some` returns those encoded bytes and `Content-Encoding` identifies their
representation. HTTP transfer framing, including chunked encoding, is removed.
This differs intentionally from buffered `send`, which retains its decoded-body
contract. A caller may compose a decoder as another reader without requiring
whole-body buffering.

Destroying an unfinished response cancels its transfer without draining the
remaining body. Its connection is not reused as a successfully consumed
response. Fully completed transfers may return a connection to the originating
client's bounded idle pool. No caller buffer or view is retained after a read.

## Redirect Policy

A request has `redirects_policy()` and `redirect_limit()` accessors, and setters
`set_redirect_policy(redirect_policy)` and `set_redirect_limit(uint32_t)`. The
default policy is `none`; its configured hop limit defaults to five and becomes
relevant only when following is enabled.

- `none` returns the redirect response without following it.
- `same_origin` follows only within the same scheme, host and effective port.
- `secure` allows cross-origin redirects but rejects HTTPS-to-HTTP downgrades.

Both following policies resolve relative locations, limit the number of hops,
and reject unsupported schemes and credential-bearing redirect URLs. Crossing
an origin under `secure` removes **all explicitly configured request headers**,
not only `Authorization`, before the next request. This prevents an application
header with an unfamiliar name from leaking a credential. Callers needing a
different trust policy can keep `none` and perform explicit requests.

A 303 changes the next method to GET unless it was HEAD; 301 and 302 change POST
to GET. These conversions drop the request body. 307 and 308 preserve method
and body, including across origins when the caller opts into `secure`. Choosing
that cross-origin policy therefore authorizes body replay: stripping headers
does not remove credentials or private data embedded in the body. Use
`same_origin` or explicit requests when such replay must be prohibited. A rejected destination reports `redirect_rejected`; exceeding the
hop limit reports `too_many_redirects`. The total timeout continues across all
hops. `streaming_response::effective_url()` reports the final URL as an owned
copy, so it remains valid independently of the response.

## HTTPS Policy

Requests default to `scheme_policy::http_and_https`. Opt into HTTPS-only
transport independently of redirect following:

```gti
request.set_scheme_policy(std::http::scheme_policy::https_only);
request.set_redirect_policy(std::http::redirect_policy::secure);
```

`schemes_policy()` observes the setting. Both `send` and `send_stream` reject
an initial HTTP URL with `errc::unsupported_scheme` before sending any request,
even if that server would redirect to HTTPS. Malformed URLs report
`errc::invalid_url`. Scheme matching is case-insensitive. Every followed
redirect preserves this policy; an HTTP destination reports
`errc::redirect_rejected` before contacting it. Changing the URL does not reset
the policy. Select `http_and_https` explicitly for HTTP test servers.
This policy restricts origin URLs, not the scheme of a transport proxy.


HTTPS always enables peer-certificate and host-name verification. There is no
public insecure switch. By default libcurl selects its configured platform
trust store. `set_ca_bundle` replaces that trust input for private or test
certificate authorities while retaining host-name verification.

The implementation never invokes a shell or the `curl` executable. The
runtime dynamically loads the platform libcurl ABI and returns
`backend_unavailable` when no compatible shared library is present. Linux and
macOS release builders require libcurl development headers to compile the
runtime; installed programs need the platform libcurl shared library at run
time. libcurl types, pointers, handles, option constants, and callbacks never
cross the public GTI API.

## Runtime Boundary

The versioned `gti_rt_http_*_v1` C ABI owns buffered response storage and bounded
streaming transfer sessions. Streaming responses own their easy/multi transport
state independently of their originating client. GTI passes URL, body, CA path, and an
encoded header sequence as counted non-retained views. Fixed 4096-byte private
buffers copy response data into public `std::string` and `std::vector` values.
Native handle identities and runtime declarations remain private to trusted
source, whose accesses use explicit `unsafe` blocks. Applications can use GTI's
general raw-pointer facilities; that does not expose the HTTP runtime's private
handles or confer access to their storage.

The runtime maps libcurl outcomes to stable error ordinals, leaves redirect
selection to ordinary GTI source, enables strict TLS verification, bounds
response accumulation and streaming staging, and cleans handles deterministically.
It does not recognize public GTI class names.

Asynchronous client execution, cancellation of an in-flight blocking client call, cookie jars,
automatic authentication schemes, multipart builders, proxy configuration,
and HTTP/2-specific controls are later layers. They must compose over this
transport boundary rather than expose libcurl as the standard-library API.

## Serving HTTP/1.1

A server package accepts its own transport and keeps one move-only
`std::http::server_codec` per connection. Construct it with
`server_codec::open(server_limits{})`. The codec performs no I/O. A package
can drive many codecs on one thread, or transfer a blocking duplex stream
into `open_server_connection(std::move(stream), limits)`. The function infers
the stream type and requires both `std::io::reader<S>` and `std::io::writer<S>`.

### Driving a connection

1. Feed available bytes to `parse_head(input)`. Remove exactly the reported
   `consumed` prefix. An empty `head` requests more input. A present
   `request_head` owns the method, raw target, effective authority and ordered
   fields. Their text accessors borrow the head. Use `find_next(name, start)`
   on its headers to locate repeated fields.
2. If `expects_continue()` is true, choose between `begin_continue()` and a
   final reply. Finish sending the interim response before reading the body.
   Feed `parse_body(input, dst)` a nonempty decoded-byte destination. Consume
   its input count and deliver its written byte count, including on `body_end`.
   Keep any pipelined suffix for the next request.
3. Build a `response_head` with `create(status)` and `add_header(name, value)`.
   Start output with `begin_response(head, representation_length, policy)`.
   `next_output()` borrows the current step. For `framing`, write from
   `bytes()`. For `body`, write up to `remaining` bytes from the package's
   payload. End the borrow and call `advance_output(n)` with the actual
   positive write count. When the transport would block, keep the same step
   and resume it on readiness. Completion-based transports must keep submitted
   storage alive until the write or cancellation completes.
4. At `flush`, flush the transport and call `finish_flush()` after success.
   `continue_sent` resumes body input, `reusable` admits the next head, and
   `close` requires dropping or closing the transport. Process buffered input
   before waiting. Once it is exhausted, report peer EOF with `end_input()`.
   A half-close after a complete request permits a final reply.

The blocking adapter offers `read_head()`, `read_body(dst)`, `send_continue()`,
`send(head, body, policy)` and `send_error(status)`. It keeps 4096 bytes of
read-ahead. An empty optional from `read_head()` means orderly EOF between
requests. A successful zero from `read_body()` means the body is complete.
The adapter keeps unread bytes across reusable exchanges. The owner closes
the stream on drop without draining or implicitly flushing it.

### Wire rules and errors

The decoder requires CRLF, one valid Host field and unambiguous body framing.
Methods retain their spelling. Targets retain their encoding. Absolute-form
uses the target authority. Fixed-length and chunked bodies stream into caller
storage. Extensions and trailers count toward the framing limits. Permitted
trailers are validated and discarded. A peer close request or an early final
response to an unfinished body forces closure.

The encoder owns Content-Length, Transfer-Encoding, Connection, Trailer and
Upgrade. HEAD sends the supplied representation length and suppresses the
body. Statuses 204 and 304 omit Content-Length, and 205 sends zero length.
These three statuses require a zero-length representation. Successful CONNECT
and protocol upgrades are outside the supported profile.

Limits default to 8192 request-line bytes, 32768 bytes and 100 fields per
request or response head, 1 MiB decoded request body, 1024 bytes per chunk
line, 8192 bytes and 32 fields of trailers, and 65536 bytes of aggregate chunk
framing. Output payloads remain in package-owned storage. A
`server_error` carries its category and phase. The blocking adapter preserves
the underlying I/O error via `transport_error()`. A local preflight error
allows a corrected reply. When `can_send_error` is true the driver can send
one minimal closing 4xx/5xx response with `begin_error` or `send_error`.
Fatal transport errors and cancellation require dropping the connection.

The [two-connection package example](../../tests/project/http_server_connections/web/src/session.gti)
shows the nonblocking driver and routing for a monitoring dashboard.
