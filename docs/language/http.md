# Buffered HTTP And HTTPS

Status: current bounded standard-library contract

`import std.http;` provides a synchronous reusable HTTP client implemented
as ordinary GTI policy over a versioned private runtime ABI. The first surface
is deliberately buffered: each request completes before `send` returns, and a
successful response owns its complete headers and body.

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
      std::http::method::get, "https://gitea.example/api/v1/version");
  mut auto token = request.set_header("Authorization", "token secret");
  if (!token) {
    return 2;
  }

  mut auto received = client.send(request);
  if (!received) {
    return 3;
  }
  mut auto response = std::move(received).value();
  mut std::string body = response.body();
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

Component and rendered views borrow the URL's existing owning string fields;
there are no duplicate view backing buffers. A retained view still protects
the whole URL against mutation or consumption, and cannot escape a temporary
URL. Internal construction bridges are private; public factories and `clone()`
retain the parsing and ownership contract.

```gti
mut auto parsed = std::http::url::https("gitea.example");
if (!parsed) {
  return 1;
}
mut std::http::url endpoint = std::move(parsed).value();
endpoint.push_path_segment("api");
endpoint.push_path_segment("v1");
endpoint.push_path_segment("repos");
endpoint.add_query_parameter("limit", "20");

mut auto received = client.get(endpoint);
```

The construction and observation surface is:

```gti
static expected<url, url_errc> parse(string_view absolute);
static expected<url, url_errc> https(string_view authority);
url clone();
string_view scheme();
string_view host();
bool has_explicit_port();
uint16_t port();
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

`port()` returns an explicit port when present and otherwise the scheme
default, 80 or 443. `has_explicit_port()` distinguishes those cases. `view()`
is a receiver-tied view and `string()` makes an independent owned copy.
`clone()` makes an independently mutable URL.

`base / "api" / "v1"` preserves a named `base`: the first `/` clones it,
then consuming `/` overloads reuse the owned intermediate. A fresh URL or
`std::move(base) / "api"` selects the consuming overload immediately. Moving
`base` makes it unavailable afterward and is rejected while a live view borrows
it. `base /= "api"` appends in place and requires a mutable receiver.

These operators append one raw path segment, exactly like
`push_path_segment`; they do not resolve relative URLs or combine authorities.
Pass a `string_view`, not another `url`. Query text and the authority survive
unchanged. Consuming chains avoid whole-URL clones, but each append still
rebuilds the rendered URL and may allocate.

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
- `set_max_response_bytes`, whose default is 16 MiB; and
- `set_ca_bundle(path)` for a PEM trust bundle used by this request.

Header mutators and `set_ca_bundle` return `expected<void, errc>`. Header names
must be non-empty RFC token text. NUL, carriage return, and line feed are
rejected in values, so these APIs cannot be used for request smuggling by line
injection. A CA path containing NUL is rejected.

`std::http::client::create()` returns `expected<client, errc>`. A client is
move-only, is neither transferable nor shareable, and admits one request at a
time through a mutable borrow. Reusing it retains the backend connection pool.
Its operations are:

```gti
expected<response, errc> send(request& value) mut;
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
`backend_unavailable`, and `client_creation_failed`.

HTTP status codes are not transport failures. A 404 or 500 is a successful
`response` whose `status()` carries that value. Only URL construction,
resolution, connection, TLS, timeout, response-size, backend, and native
request failures use the unexpected alternative.

Only `http://` and `https://` URLs are accepted. GET and HEAD reject a nonempty
body. Redirect following is disabled, so a 3xx response is returned to the
caller and credentials are never silently forwarded to another origin. The
configured response limit applies to the decoded body; response headers also
have an independent 1 MiB hard limit. Body and header strings preserve their
exact bytes, including NUL.

## HTTPS Policy

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

The versioned `gti_rt_http_*_v1` C ABI owns one easy handle per client and one
fully buffered result per response. GTI passes URL, body, CA path, and an
encoded header sequence as counted non-retained views. Fixed 4096-byte private
buffers copy response data into public `std::string` and `std::vector` values.
Native handle identities and runtime declarations remain private to trusted
source, whose accesses use explicit `unsafe` blocks. Applications can use GTI's
general raw-pointer facilities; that does not expose the HTTP runtime's private
handles or confer access to their storage.

The runtime maps libcurl outcomes to the stable `errc` ordinals, disables
redirects, enables strict TLS verification, bounds response accumulation, and
cleans both handles deterministically. It does not own public request policy or
recognize public GTI class names.

Streaming bodies, cancellation, asynchronous execution, cookie jars,
automatic authentication schemes, multipart builders, proxy configuration,
and HTTP/2-specific controls are later layers. They must compose over this
transport boundary rather than expose libcurl as the standard-library API.
