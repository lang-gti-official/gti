#include "gti/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr std::uint64_t kMaximumResponseHeaderBytes = 1024U * 1024U;

using CurlGlobalInit = decltype(&curl_global_init);
using CurlEasyInit = decltype(&curl_easy_init);
using CurlEasyCleanup = decltype(&curl_easy_cleanup);
using CurlEasyReset = decltype(&curl_easy_reset);
using CurlEasySetopt = decltype(&curl_easy_setopt);
using CurlEasyPerform = decltype(&curl_easy_perform);
using CurlEasyGetinfo = decltype(&curl_easy_getinfo);
using CurlEasyStrerror = decltype(&curl_easy_strerror);
using CurlSlistAppend = decltype(&curl_slist_append);
using CurlSlistFreeAll = decltype(&curl_slist_free_all);

void *openDynamicLibrary(const char *name) {
#if defined(_WIN32)
  return reinterpret_cast<void *>(LoadLibraryA(name));
#else
  return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *loadDynamicSymbol(void *library, const char *name) {
  if (library == nullptr) {
    return nullptr;
  }
#if defined(_WIN32)
  return reinterpret_cast<void *>(
      GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
  return dlsym(library, name);
#endif
}

template <typename Function>
bool loadFunction(void *library, const char *name, Function &output) {
  void *symbol = loadDynamicSymbol(library, name);
  if (symbol == nullptr || sizeof(output) != sizeof(symbol)) {
    return false;
  }
  std::memcpy(&output, &symbol, sizeof(output));
  return true;
}

struct CurlApi {
  void *library = nullptr;
  CurlGlobalInit globalInit = nullptr;
  CurlEasyInit easyInit = nullptr;
  CurlEasyCleanup easyCleanup = nullptr;
  CurlEasyReset easyReset = nullptr;
  CurlEasySetopt easySetopt = nullptr;
  CurlEasyPerform easyPerform = nullptr;
  CurlEasyGetinfo easyGetinfo = nullptr;
  CurlEasyStrerror easyStrerror = nullptr;
  CurlSlistAppend slistAppend = nullptr;
  CurlSlistFreeAll slistFreeAll = nullptr;
  decltype(&curl_easy_pause) easyPause = nullptr;
  decltype(&curl_multi_init) multiInit = nullptr;
  decltype(&curl_multi_cleanup) multiCleanup = nullptr;
  decltype(&curl_multi_add_handle) multiAdd = nullptr;
  decltype(&curl_multi_remove_handle) multiRemove = nullptr;
  decltype(&curl_multi_perform) multiPerform = nullptr;
  decltype(&curl_multi_poll) multiPoll = nullptr;
  decltype(&curl_multi_info_read) multiInfo = nullptr;
  bool streamingAvailable = false;
  bool available = false;
};

CurlApi loadCurlApi() {
  CurlApi api;
#if defined(_WIN32)
  constexpr std::array candidates{"libcurl.dll", "curl.dll"};
#elif defined(__APPLE__)
  constexpr std::array candidates{"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib",
                                  "libcurl.dylib"};
#else
  constexpr std::array candidates{"libcurl.so.4", "libcurl.so"};
#endif
  for (const char *candidate : candidates) {
    api.library = openDynamicLibrary(candidate);
    if (api.library != nullptr) {
      break;
    }
  }
  if (api.library == nullptr ||
      !loadFunction(api.library, "curl_global_init", api.globalInit) ||
      !loadFunction(api.library, "curl_easy_init", api.easyInit) ||
      !loadFunction(api.library, "curl_easy_cleanup", api.easyCleanup) ||
      !loadFunction(api.library, "curl_easy_reset", api.easyReset) ||
      !loadFunction(api.library, "curl_easy_setopt", api.easySetopt) ||
      !loadFunction(api.library, "curl_easy_perform", api.easyPerform) ||
      !loadFunction(api.library, "curl_easy_getinfo", api.easyGetinfo) ||
      !loadFunction(api.library, "curl_easy_strerror", api.easyStrerror) ||
      !loadFunction(api.library, "curl_slist_append", api.slistAppend) ||
      !loadFunction(api.library, "curl_slist_free_all", api.slistFreeAll)) {
    return api;
  }
  api.streamingAvailable =
      loadFunction(api.library, "curl_easy_pause", api.easyPause) &&
      loadFunction(api.library, "curl_multi_init", api.multiInit) &&
      loadFunction(api.library, "curl_multi_cleanup", api.multiCleanup) &&
      loadFunction(api.library, "curl_multi_add_handle", api.multiAdd) &&
      loadFunction(api.library, "curl_multi_remove_handle", api.multiRemove) &&
      loadFunction(api.library, "curl_multi_perform", api.multiPerform) &&
      loadFunction(api.library, "curl_multi_poll", api.multiPoll) &&
      loadFunction(api.library, "curl_multi_info_read", api.multiInfo);
  api.available = api.globalInit(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  return api;
}

CurlApi &curlApi() {
  // Intentionally process-lifetime state. Destroying libcurl globals while
  // other runtime clients may still exist is unsafe, and the operating system
  // reclaims the loader handle at process exit.
  static CurlApi api = loadCurlApi();
  return api;
}

struct HttpHeader {
  std::string name;
  std::string value;
};

struct HttpResponse {
  std::uint32_t status = 0;
  std::vector<HttpHeader> headers;
  std::string body;
};

struct StreamPool;

struct HttpClient {
  CURL *easy = nullptr;
  std::string lastError;
  std::shared_ptr<StreamPool> streamPool;
};

struct TransferContext {
  HttpResponse *response = nullptr;
  std::uint64_t maximumBodyBytes = 0;
  std::uint64_t headerBytes = 0;
  bool responseTooLarge = false;
};

bool copyView(gti_c_string_view input, std::string &output) {
  if (input.length > SIZE_MAX || (input.length != 0 && input.data == nullptr)) {
    return false;
  }
  if (input.length == 0) {
    output.clear();
    return true;
  }
  output.assign(input.data, static_cast<std::size_t>(input.length));
  return true;
}

bool containsNull(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

bool isHeaderNameCharacter(unsigned char value) {
  if (std::isalnum(value) != 0) {
    return true;
  }
  constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
  return punctuation.find(static_cast<char>(value)) != std::string_view::npos;
}

bool validHeader(const HttpHeader &header) {
  if (header.name.empty()) {
    return false;
  }
  for (unsigned char value : header.name) {
    if (!isHeaderNameCharacter(value)) {
      return false;
    }
  }
  return !containsNull(header.value) &&
         header.value.find('\r') == std::string::npos &&
         header.value.find('\n') == std::string::npos;
}

gti_http_error_v1 validateUrl(std::string_view url) {
  if (url.empty() || containsNull(url)) {
    return GTI_HTTP_INVALID_URL_V1;
  }
  const std::size_t separator = url.find(':');
  if (separator == std::string_view::npos || separator == 0 ||
      separator + 2 >= url.size() || url[separator + 1] != '/' ||
      url[separator + 2] != '/') {
    return GTI_HTTP_INVALID_URL_V1;
  }
  std::string scheme(url.substr(0, separator));
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return scheme == "http" || scheme == "https" ? GTI_HTTP_OK_V1
                                               : GTI_HTTP_UNSUPPORTED_SCHEME_V1;
}

bool decodeHeaders(std::string_view encoded, std::uint64_t count,
                   std::vector<HttpHeader> &headers) {
  if (count > SIZE_MAX || count > encoded.size() / 2 + 1) {
    return false;
  }
  headers.clear();
  headers.reserve(static_cast<std::size_t>(count));
  std::size_t offset = 0;
  for (std::uint64_t index = 0; index < count; ++index) {
    const std::size_t nameEnd = encoded.find('\0', offset);
    if (nameEnd == std::string_view::npos) {
      return false;
    }
    HttpHeader header;
    header.name.assign(encoded.substr(offset, nameEnd - offset));
    offset = nameEnd + 1;
    const std::size_t valueEnd = encoded.find('\0', offset);
    if (valueEnd == std::string_view::npos) {
      return false;
    }
    header.value.assign(encoded.substr(offset, valueEnd - offset));
    offset = valueEnd + 1;
    if (!validHeader(header)) {
      return false;
    }
    headers.push_back(std::move(header));
  }
  return offset == encoded.size();
}

std::size_t writeBody(char *data, std::size_t elementSize,
                      std::size_t elementCount, void *opaque) {
  auto *context = static_cast<TransferContext *>(opaque);
  if (context == nullptr || context->response == nullptr ||
      (elementSize != 0 && elementCount > SIZE_MAX / elementSize)) {
    return 0;
  }
  const std::size_t length = elementSize * elementCount;
  const std::uint64_t current = context->response->body.size();
  if (length > context->maximumBodyBytes ||
      current > context->maximumBodyBytes - length) {
    context->responseTooLarge = true;
    return 0;
  }
  context->response->body.append(data, length);
  return length;
}

std::string_view trimOptionalWhitespace(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

std::size_t writeHeader(char *data, std::size_t elementSize,
                        std::size_t elementCount, void *opaque) {
  auto *context = static_cast<TransferContext *>(opaque);
  if (context == nullptr || context->response == nullptr ||
      (elementSize != 0 && elementCount > SIZE_MAX / elementSize)) {
    return 0;
  }
  const std::size_t length = elementSize * elementCount;
  std::string_view line(data, length);
  if (line.starts_with("HTTP/")) {
    if (length > kMaximumResponseHeaderBytes) {
      context->responseTooLarge = true;
      return 0;
    }
    context->response->headers.clear();
    context->headerBytes = length;
  } else {
    if (length > kMaximumResponseHeaderBytes ||
        context->headerBytes > kMaximumResponseHeaderBytes - length) {
      context->responseTooLarge = true;
      return 0;
    }
    context->headerBytes += length;
  }
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  const std::size_t separator = line.find(':');
  if (separator != std::string_view::npos) {
    const std::string_view name =
        trimOptionalWhitespace(line.substr(0, separator));
    const std::string_view value =
        trimOptionalWhitespace(line.substr(separator + 1));
    if (!name.empty()) {
      context->response->headers.push_back(
          {.name = std::string(name), .value = std::string(value)});
    }
  }
  return length;
}

gti_http_error_v1 mapCurlError(CURLcode code) {
  if (code == CURLE_URL_MALFORMAT) {
    return GTI_HTTP_INVALID_URL_V1;
  }
  if (code == CURLE_UNSUPPORTED_PROTOCOL) {
    return GTI_HTTP_UNSUPPORTED_SCHEME_V1;
  }
  if (code == CURLE_COULDNT_RESOLVE_HOST ||
      code == CURLE_COULDNT_RESOLVE_PROXY) {
    return GTI_HTTP_NAME_RESOLUTION_FAILED_V1;
  }
  if (code == CURLE_COULDNT_CONNECT) {
    return GTI_HTTP_CONNECTION_FAILED_V1;
  }
  if (code == CURLE_OPERATION_TIMEDOUT) {
    return GTI_HTTP_TIMEOUT_V1;
  }
  if (code == CURLE_SSL_CONNECT_ERROR ||
      code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CERTPROBLEM ||
      code == CURLE_SSL_CIPHER || code == CURLE_SSL_ENGINE_INITFAILED ||
      code == CURLE_SSL_CACERT_BADFILE) {
    return GTI_HTTP_TLS_FAILED_V1;
  }
  return GTI_HTTP_REQUEST_FAILED_V1;
}

bool timeoutFitsLong(std::uint64_t value) {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<long>::max());
}

void setClientError(HttpClient &client, CURLcode code,
                    std::string_view errorBuffer) {
  client.lastError.assign(errorBuffer);
  if (client.lastError.empty()) {
    const char *message = curlApi().easyStrerror(code);
    client.lastError = message == nullptr ? "HTTP request failed" : message;
  }
}

template <typename Value>
bool setOption(HttpClient &client, CURLoption option, Value value) {
  const CURLcode status = curlApi().easySetopt(client.easy, option, value);
  if (status == CURLE_OK) {
    return true;
  }
  setClientError(client, status, {});
  return false;
}

bool configureRequest(HttpClient &client, gti_http_method_v1 method,
                      const std::string &url, const std::string &body,
                      const std::string &caBundlePath,
                      std::uint64_t connectTimeoutMs,
                      std::uint64_t requestTimeoutMs, curl_slist *nativeHeaders,
                      curl_write_callback bodyCallback,
                      curl_write_callback headerCallback, void *context,
                      std::array<char, CURL_ERROR_SIZE> &errorBuffer,
                      bool streaming) {
  bool configured =
      setOption(client, CURLOPT_URL, url.c_str()) &&
      setOption(client, CURLOPT_NOSIGNAL, 1L) &&
      setOption(client, CURLOPT_FOLLOWLOCATION, 0L) &&
      setOption(client, CURLOPT_SSL_VERIFYPEER, 1L) &&
      setOption(client, CURLOPT_SSL_VERIFYHOST, 2L) &&
      setOption(client, CURLOPT_ERRORBUFFER, errorBuffer.data()) &&
      setOption(client, CURLOPT_WRITEFUNCTION, bodyCallback) &&
      setOption(client, CURLOPT_WRITEDATA, context) &&
      setOption(client, CURLOPT_HEADERFUNCTION, headerCallback) &&
      setOption(client, CURLOPT_HEADERDATA, context) &&
      setOption(client, CURLOPT_USERAGENT, "gti-stdlib-http/1") &&
      setOption(client, CURLOPT_ACCEPT_ENCODING, streaming ? "identity" : "") &&
      setOption(client, CURLOPT_HTTPHEADER, nativeHeaders);
  if (configured && connectTimeoutMs != 0) {
    configured = setOption(client, CURLOPT_CONNECTTIMEOUT_MS,
                           static_cast<long>(connectTimeoutMs));
  }
  if (configured && requestTimeoutMs != 0) {
    configured = setOption(client, CURLOPT_TIMEOUT_MS,
                           static_cast<long>(requestTimeoutMs));
  }
  if (configured && !caBundlePath.empty()) {
    configured = setOption(client, CURLOPT_CAINFO, caBundlePath.c_str());
  }
  if (configured) {
    switch (method) {
    case GTI_HTTP_GET_V1:
      configured = setOption(client, CURLOPT_HTTPGET, 1L);
      break;
    case GTI_HTTP_POST_V1:
      configured = setOption(client, CURLOPT_POST, 1L) &&
                   setOption(client, CURLOPT_POSTFIELDS, body.data()) &&
                   setOption(client, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(body.size()));
      break;
    case GTI_HTTP_PUT_V1:
    case GTI_HTTP_PATCH_V1:
    case GTI_HTTP_DELETE_V1: {
      const char *verb = method == GTI_HTTP_PUT_V1     ? "PUT"
                         : method == GTI_HTTP_PATCH_V1 ? "PATCH"
                                                       : "DELETE";
      configured = setOption(client, CURLOPT_CUSTOMREQUEST, verb) &&
                   setOption(client, CURLOPT_POSTFIELDS, body.data()) &&
                   setOption(client, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(body.size()));
      break;
    }
    case GTI_HTTP_HEAD_V1:
      configured = setOption(client, CURLOPT_NOBODY, 1L);
      break;
    default:
      client.lastError = "HTTP request method is invalid";
      configured = false;
      break;
    }
  }
  if (configured && streaming) {
    configured =
        setOption(client, CURLOPT_HTTP_VERSION, long(CURL_HTTP_VERSION_1_1)) &&
        setOption(client, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L) &&
        setOption(client, CURLOPT_HTTP_CONTENT_DECODING, 0L);
  }
  return configured;
}

// A session retains the connection cache only after a complete successful
// response. Each live response has its own session; no caller storage escapes.
struct StreamSession {
  CURL *easy = nullptr;
  CURLM *multi = nullptr;
  bool attached = false;
  StreamSession() : easy(curlApi().easyInit()), multi(curlApi().multiInit()) {}
  void detach() {
    if (attached) {
      curlApi().multiRemove(multi, easy);
      attached = false;
    }
  }
  ~StreamSession() {
    detach();
    if (easy)
      curlApi().easyCleanup(easy);
    if (multi)
      curlApi().multiCleanup(multi);
  }
};

struct StreamPool {
  // One idle session bounds retained pool state regardless of prior
  // concurrency.
  std::unique_ptr<StreamSession> idle;
};

struct HttpStream {
  std::shared_ptr<StreamPool> pool;
  std::unique_ptr<StreamSession> session;
  HttpResponse response;
  std::string url, body, caBundle;
  curl_slist *headers = nullptr;
  std::array<char, CURL_ERROR_SIZE> errorBuffer{};
  std::array<std::uint8_t, 65536> buffer{};
  std::size_t begin = 0, end = 0;
  std::uint64_t maximum = 0, received = 0, delivered = 0, headerBytes = 0;
  std::uint64_t timeout = 0;
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  gti_http_error_v1 error = GTI_HTTP_OK_V1;
  CURLcode nativeCode = CURLE_OK;
  bool headersDone = false, paused = false, finished = false;

  ~HttpStream() {
    if (session && session->easy) {
      session->detach();
      // Reset retained callbacks and request pointers before freeing their
      // owners.
      curlApi().easyReset(session->easy);
      if (finished && error == GTI_HTTP_OK_V1 && begin == end && pool &&
          !pool->idle)
        pool->idle = std::move(session);
    }
    curlApi().slistFreeAll(headers);
  }

  bool expired() {
    if (timeout == 0 || (finished && begin == end))
      return false;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (elapsed >= 0 && static_cast<std::uint64_t>(elapsed) >= timeout) {
      error = GTI_HTTP_TIMEOUT_V1;
      nativeCode = CURLE_OPERATION_TIMEDOUT;
      return true;
    }
    return false;
  }
};

std::size_t streamHeader(char *data, std::size_t size, std::size_t count,
                         void *opaque) {
  auto &stream = *static_cast<HttpStream *>(opaque);
  if (size != 0 && count > SIZE_MAX / size)
    return 0;
  const std::size_t length = size * count;
  if (length > kMaximumResponseHeaderBytes ||
      stream.headerBytes > kMaximumResponseHeaderBytes - length) {
    stream.error = GTI_HTTP_RESPONSE_TOO_LARGE_V1;
    return 0;
  }
  stream.headerBytes += length;
  std::string_view line(data, length);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    line.remove_suffix(1);
  // Trailers are consumed by the transport but do not mutate published headers.
  if (stream.headersDone)
    return length;
  if (line.starts_with("HTTP/")) {
    auto space = line.find(' ');
    unsigned status = 0;
    if (space == std::string_view::npos ||
        std::from_chars(line.data() + space + 1, line.data() + line.size(),
                        status)
                .ec != std::errc{}) {
      stream.error = GTI_HTTP_REQUEST_FAILED_V1;
      return 0;
    }
    stream.response.status = status;
    stream.response.headers.clear();
    if (status == 101) {
      stream.error = GTI_HTTP_REQUEST_FAILED_V1;
      return 0;
    }
  } else if (line.empty() && stream.response.status >= 200) {
    stream.headersDone = true;
  } else {
    auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      auto name = trimOptionalWhitespace(line.substr(0, colon));
      auto value = trimOptionalWhitespace(line.substr(colon + 1));
      if (!name.empty())
        stream.response.headers.push_back(
            {std::string(name), std::string(value)});
    }
  }
  return length;
}

std::size_t streamBody(char *data, std::size_t size, std::size_t count,
                       void *opaque) {
  auto &stream = *static_cast<HttpStream *>(opaque);
  if (size != 0 && count > SIZE_MAX / size)
    return 0;
  const std::size_t length = size * count;
  if (length > stream.maximum || stream.received > stream.maximum - length) {
    stream.error = GTI_HTTP_RESPONSE_TOO_LARGE_V1;
    return 0;
  }
  if (length > stream.buffer.size() - stream.end) {
    stream.paused = true;
    return CURL_WRITEFUNC_PAUSE; // The complete unconsumed chunk is replayed.
  }
  std::memcpy(stream.buffer.data() + stream.end, data, length);
  stream.end += length;
  stream.received += length;
  return length;
}

void pumpStream(HttpStream &stream, bool headersOnly) {
  auto &api = curlApi();
  while (stream.error == GTI_HTTP_OK_V1 && !stream.expired()) {
    if ((headersOnly && stream.headersDone) ||
        (!headersOnly && stream.end != stream.begin) || stream.finished)
      return;
    if (stream.paused) {
      stream.paused = false;
      auto code = api.easyPause(stream.session->easy, CURLPAUSE_CONT);
      if (code != CURLE_OK) {
        stream.nativeCode = code;
        stream.error = mapCurlError(code);
        return;
      }
    }
    int running = 0;
    if (api.multiPerform(stream.session->multi, &running) != CURLM_OK) {
      stream.error = GTI_HTTP_REQUEST_FAILED_V1;
      return;
    }
    int remaining = 0;
    while (auto *message = api.multiInfo(stream.session->multi, &remaining)) {
      if (message->msg == CURLMSG_DONE) {
        stream.finished = true;
        stream.nativeCode = message->data.result;
        if (stream.error == GTI_HTTP_OK_V1 && stream.nativeCode != CURLE_OK)
          stream.error = mapCurlError(stream.nativeCode);
      }
    }
    if (stream.error != GTI_HTTP_OK_V1 || stream.finished ||
        (headersOnly && stream.headersDone) ||
        (!headersOnly && stream.end != stream.begin))
      return;
    if (api.multiPoll(stream.session->multi, nullptr, 0, 100, nullptr) !=
        CURLM_OK) {
      stream.error = GTI_HTTP_REQUEST_FAILED_V1;
      return;
    }
  }
}

std::int64_t encodeHandle(void *handle) {
  return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(handle));
}

template <typename Value> Value *decodeHandle(std::int64_t handle) {
  if (handle <= 0) {
    return nullptr;
  }
  return reinterpret_cast<Value *>(static_cast<std::intptr_t>(handle));
}

std::int64_t readBytes(std::string_view value, std::uint64_t offset,
                       std::uint8_t *output, std::uint64_t capacity) {
  if (capacity == 0 || offset >= value.size()) {
    return 0;
  }
  if (output == nullptr || capacity > SIZE_MAX) {
    return -1;
  }
  const std::uint64_t available = value.size() - offset;
  const std::size_t count =
      static_cast<std::size_t>(std::min(capacity, available));
  std::memcpy(output, value.data() + static_cast<std::size_t>(offset), count);
  return static_cast<std::int64_t>(count);
}

const HttpHeader *responseHeader(HttpResponse *response, std::uint64_t index) {
  if (response == nullptr || index >= response->headers.size()) {
    return nullptr;
  }
  return &response->headers[static_cast<std::size_t>(index)];
}

} // namespace

extern "C" gti_http_error_v1
gti_rt_http_client_create_v1(std::int64_t *outputClient) {
  if (outputClient == nullptr) {
    return GTI_HTTP_CLIENT_CREATION_FAILED_V1;
  }
  *outputClient = 0;
  CurlApi &api = curlApi();
  if (!api.available) {
    return GTI_HTTP_BACKEND_UNAVAILABLE_V1;
  }
  auto client = std::make_unique<HttpClient>();
  client->easy = api.easyInit();
  if (client->easy == nullptr) {
    return GTI_HTTP_CLIENT_CREATION_FAILED_V1;
  }
  *outputClient = encodeHandle(client.release());
  return GTI_HTTP_OK_V1;
}

extern "C" void gti_rt_http_client_destroy_v1(std::int64_t handle) {
  std::unique_ptr<HttpClient> client(decodeHandle<HttpClient>(handle));
  if (client != nullptr && client->easy != nullptr) {
    curlApi().easyCleanup(client->easy);
  }
}

extern "C" gti_http_error_v1 gti_rt_http_client_send_v1(
    std::int64_t clientHandle, gti_http_method_v1 method,
    gti_c_string_view urlView, gti_c_string_view encodedHeadersView,
    std::uint64_t headerCount, gti_c_string_view bodyView,
    std::uint64_t connectTimeoutMs, std::uint64_t requestTimeoutMs,
    std::uint64_t maximumResponseBytes, gti_c_string_view caBundlePathView,
    std::int64_t *outputResponse, std::int32_t *outputNativeCode) {
  HttpClient *client = decodeHandle<HttpClient>(clientHandle);
  if (outputResponse != nullptr) {
    *outputResponse = 0;
  }
  if (outputNativeCode != nullptr) {
    *outputNativeCode = 0;
  }
  if (client == nullptr || client->easy == nullptr ||
      outputResponse == nullptr || outputNativeCode == nullptr) {
    return GTI_HTTP_REQUEST_FAILED_V1;
  }
  client->lastError.clear();

  std::string url;
  std::string encodedHeaders;
  std::string body;
  std::string caBundlePath;
  if (!copyView(urlView, url) ||
      !copyView(encodedHeadersView, encodedHeaders) ||
      !copyView(bodyView, body) || !copyView(caBundlePathView, caBundlePath)) {
    client->lastError = "HTTP request contains an invalid counted string";
    return GTI_HTTP_REQUEST_FAILED_V1;
  }
  const gti_http_error_v1 urlStatus = validateUrl(url);
  if (urlStatus != GTI_HTTP_OK_V1) {
    client->lastError = urlStatus == GTI_HTTP_UNSUPPORTED_SCHEME_V1
                            ? "HTTP URL uses an unsupported scheme"
                            : "HTTP URL is invalid";
    return urlStatus;
  }
  if (containsNull(caBundlePath)) {
    client->lastError = "CA bundle path contains a NUL byte";
    return GTI_HTTP_REQUEST_FAILED_V1;
  }
  std::vector<HttpHeader> headers;
  if (!decodeHeaders(encodedHeaders, headerCount, headers)) {
    client->lastError = "HTTP request contains an invalid header";
    return GTI_HTTP_INVALID_HEADER_V1;
  }
  if (!timeoutFitsLong(connectTimeoutMs) ||
      !timeoutFitsLong(requestTimeoutMs) ||
      body.size() >
          static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max())) {
    client->lastError = "HTTP request limit exceeds the native backend range";
    return GTI_HTTP_REQUEST_FAILED_V1;
  }
  if ((method == GTI_HTTP_GET_V1 || method == GTI_HTTP_HEAD_V1) &&
      !body.empty()) {
    client->lastError = "GET and HEAD requests cannot contain a body";
    return GTI_HTTP_REQUEST_FAILED_V1;
  }

  CurlApi &api = curlApi();
  api.easyReset(client->easy);
  std::array<char, CURL_ERROR_SIZE> errorBuffer{};
  auto response = std::make_unique<HttpResponse>();
  TransferContext transfer{.response = response.get(),
                           .maximumBodyBytes = maximumResponseBytes};
  curl_slist *nativeHeaders = nullptr;
  for (const HttpHeader &header : headers) {
    const std::string line = header.name + ": " + header.value;
    curl_slist *next = api.slistAppend(nativeHeaders, line.c_str());
    if (next == nullptr) {
      api.slistFreeAll(nativeHeaders);
      client->lastError = "HTTP header allocation failed";
      return GTI_HTTP_REQUEST_FAILED_V1;
    }
    nativeHeaders = next;
  }
  const auto releaseHeaders = [&api, &nativeHeaders]() {
    api.slistFreeAll(nativeHeaders);
    nativeHeaders = nullptr;
  };

  bool configured =
      configureRequest(*client, method, url, body, caBundlePath,
                       connectTimeoutMs, requestTimeoutMs, nativeHeaders,
                       &writeBody, &writeHeader, &transfer, errorBuffer, false);

  if (!configured) {
    releaseHeaders();
    return GTI_HTTP_REQUEST_FAILED_V1;
  }

  const CURLcode nativeStatus = api.easyPerform(client->easy);
  releaseHeaders();
  *outputNativeCode = static_cast<std::int32_t>(nativeStatus);
  if (nativeStatus != CURLE_OK) {
    setClientError(*client, nativeStatus, errorBuffer.data());
    if (transfer.responseTooLarge) {
      return GTI_HTTP_RESPONSE_TOO_LARGE_V1;
    }
    return mapCurlError(nativeStatus);
  }

  long status = 0;
  const CURLcode infoStatus =
      api.easyGetinfo(client->easy, CURLINFO_RESPONSE_CODE, &status);
  if (infoStatus != CURLE_OK || status < 0 ||
      static_cast<unsigned long>(status) > UINT32_MAX) {
    *outputNativeCode = static_cast<std::int32_t>(infoStatus);
    setClientError(*client, infoStatus, {});
    return GTI_HTTP_REQUEST_FAILED_V1;
  }
  response->status = static_cast<std::uint32_t>(status);
  *outputResponse = encodeHandle(response.release());
  return GTI_HTTP_OK_V1;
}

extern "C" std::uint64_t gti_rt_http_client_error_size_v1(std::int64_t handle) {
  const HttpClient *client = decodeHandle<HttpClient>(handle);
  return client == nullptr ? 0 : client->lastError.size();
}

extern "C" std::int64_t
gti_rt_http_client_error_read_v1(std::int64_t handle, std::uint64_t offset,
                                 std::uint8_t *output, std::uint64_t capacity) {
  const HttpClient *client = decodeHandle<HttpClient>(handle);
  return client == nullptr
             ? -1
             : readBytes(client->lastError, offset, output, capacity);
}

extern "C" void gti_rt_http_response_destroy_v1(std::int64_t handle) {
  delete decodeHandle<HttpResponse>(handle);
}

extern "C" std::uint32_t gti_rt_http_response_status_v1(std::int64_t handle) {
  const HttpResponse *response = decodeHandle<HttpResponse>(handle);
  return response == nullptr ? 0 : response->status;
}

extern "C" std::uint64_t
gti_rt_http_response_header_count_v1(std::int64_t handle) {
  const HttpResponse *response = decodeHandle<HttpResponse>(handle);
  return response == nullptr ? 0 : response->headers.size();
}

extern "C" std::uint64_t
gti_rt_http_response_header_name_size_v1(std::int64_t handle,
                                         std::uint64_t index) {
  const HttpHeader *header =
      responseHeader(decodeHandle<HttpResponse>(handle), index);
  return header == nullptr ? 0 : header->name.size();
}

extern "C" std::int64_t gti_rt_http_response_header_name_read_v1(
    std::int64_t handle, std::uint64_t index, std::uint64_t offset,
    std::uint8_t *output, std::uint64_t capacity) {
  const HttpHeader *header =
      responseHeader(decodeHandle<HttpResponse>(handle), index);
  return header == nullptr ? -1
                           : readBytes(header->name, offset, output, capacity);
}

extern "C" std::uint64_t
gti_rt_http_response_header_value_size_v1(std::int64_t handle,
                                          std::uint64_t index) {
  const HttpHeader *header =
      responseHeader(decodeHandle<HttpResponse>(handle), index);
  return header == nullptr ? 0 : header->value.size();
}

extern "C" std::int64_t gti_rt_http_response_header_value_read_v1(
    std::int64_t handle, std::uint64_t index, std::uint64_t offset,
    std::uint8_t *output, std::uint64_t capacity) {
  const HttpHeader *header =
      responseHeader(decodeHandle<HttpResponse>(handle), index);
  return header == nullptr ? -1
                           : readBytes(header->value, offset, output, capacity);
}

extern "C" std::uint64_t
gti_rt_http_response_body_size_v1(std::int64_t handle) {
  const HttpResponse *response = decodeHandle<HttpResponse>(handle);
  return response == nullptr ? 0 : response->body.size();
}

extern "C" std::int64_t
gti_rt_http_response_body_read_v1(std::int64_t handle, std::uint64_t offset,
                                  std::uint8_t *output,
                                  std::uint64_t capacity) {
  const HttpResponse *response = decodeHandle<HttpResponse>(handle);
  return response == nullptr
             ? -1
             : readBytes(response->body, offset, output, capacity);
}

extern "C" gti_http_error_v1 gti_rt_http_client_stream_v1(
    std::int64_t clientHandle, gti_http_method_v1 method,
    gti_c_string_view urlView, gti_c_string_view encodedHeadersView,
    std::uint64_t headerCount, gti_c_string_view bodyView,
    std::uint64_t connectTimeoutMs, std::uint64_t requestTimeoutMs,
    std::uint64_t maximumResponseBytes, gti_c_string_view caBundlePathView,
    std::int64_t *outputStream, std::int32_t *outputNativeCode) {
  if (outputStream)
    *outputStream = 0;
  if (outputNativeCode)
    *outputNativeCode = 0;
  auto *client = decodeHandle<HttpClient>(clientHandle);
  if (!client || !outputStream || !outputNativeCode)
    return GTI_HTTP_REQUEST_FAILED_V1;
  client->lastError.clear();
  auto &api = curlApi();
  if (!api.streamingAvailable)
    return GTI_HTTP_BACKEND_UNAVAILABLE_V1;
  auto stream = std::make_unique<HttpStream>();
  std::string encoded;
  std::vector<HttpHeader> fields;
  if (!copyView(urlView, stream->url) || !copyView(bodyView, stream->body) ||
      !copyView(caBundlePathView, stream->caBundle) ||
      !copyView(encodedHeadersView, encoded) ||
      containsNull(stream->caBundle) || !timeoutFitsLong(connectTimeoutMs) ||
      !timeoutFitsLong(requestTimeoutMs) ||
      stream->body.size() >
          static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max()))
    return GTI_HTTP_REQUEST_FAILED_V1;
  auto valid = validateUrl(stream->url);
  if (valid != GTI_HTTP_OK_V1)
    return valid;
  if (!decodeHeaders(encoded, headerCount, fields))
    return GTI_HTTP_INVALID_HEADER_V1;
  if ((method == GTI_HTTP_GET_V1 || method == GTI_HTTP_HEAD_V1) &&
      !stream->body.empty())
    return GTI_HTTP_REQUEST_FAILED_V1;
  if (!client->streamPool)
    client->streamPool = std::make_shared<StreamPool>();
  stream->pool = client->streamPool;
  stream->session = std::move(stream->pool->idle);
  if (!stream->session)
    stream->session = std::make_unique<StreamSession>();
  if (!stream->session->easy || !stream->session->multi)
    return GTI_HTTP_CLIENT_CREATION_FAILED_V1;
  api.easyReset(stream->session->easy);
  for (const auto &field : fields) {
    auto line = field.name + ": " + field.value;
    auto *next = api.slistAppend(stream->headers, line.c_str());
    if (!next)
      return GTI_HTTP_REQUEST_FAILED_V1;
    stream->headers = next;
  }
  stream->maximum = maximumResponseBytes;
  stream->timeout = requestTimeoutMs;
  HttpClient configuring;
  configuring.easy = stream->session->easy;
  if (!configureRequest(configuring, method, stream->url, stream->body,
                        stream->caBundle, connectTimeoutMs, requestTimeoutMs,
                        stream->headers, &streamBody, &streamHeader,
                        stream.get(), stream->errorBuffer, true)) {
    client->lastError = configuring.lastError;
    return GTI_HTTP_REQUEST_FAILED_V1;
  }
  if (api.multiAdd(stream->session->multi, stream->session->easy) != CURLM_OK)
    return GTI_HTTP_REQUEST_FAILED_V1;
  stream->session->attached = true;
  pumpStream(*stream, true);
  *outputNativeCode = static_cast<std::int32_t>(stream->nativeCode);
  if (stream->error != GTI_HTTP_OK_V1 && !stream->headersDone) {
    setClientError(*client, stream->nativeCode, stream->errorBuffer.data());
    return stream->error;
  }
  if (!stream->headersDone)
    return GTI_HTTP_REQUEST_FAILED_V1;
  *outputStream = encodeHandle(stream.release());
  return GTI_HTTP_OK_V1;
}

extern "C" void gti_rt_http_stream_destroy_v1(std::int64_t handle) {
  delete decodeHandle<HttpStream>(handle);
}
extern "C" std::int64_t gti_rt_http_stream_metadata_v1(std::int64_t handle) {
  auto *stream = decodeHandle<HttpStream>(handle);
  return stream ? encodeHandle(&stream->response) : 0;
}
extern "C" gti_http_error_v1 gti_rt_http_stream_read_v1(std::int64_t handle,
                                                        std::uint8_t *output,
                                                        std::uint64_t capacity,
                                                        std::uint64_t *count) {
  if (count)
    *count = 0;
  auto *stream = decodeHandle<HttpStream>(handle);
  if (!stream || !count || capacity > SIZE_MAX || (capacity && !output))
    return GTI_HTTP_REQUEST_FAILED_V1;
  if (capacity == 0)
    return GTI_HTTP_OK_V1;
  if (stream->error != GTI_HTTP_OK_V1 || stream->expired())
    return stream->error;
  pumpStream(*stream, false);
  if (stream->error != GTI_HTTP_OK_V1)
    return stream->error;
  const auto available = stream->end - stream->begin;
  const auto length = std::min(static_cast<std::size_t>(capacity), available);
  if (length)
    std::memcpy(output, stream->buffer.data() + stream->begin, length);
  stream->begin += length;
  stream->delivered += length;
  if (stream->begin == stream->end)
    stream->begin = stream->end = 0;
  *count = length;
  return GTI_HTTP_OK_V1;
}
extern "C" void gti_rt_http_stream_cancel_v1(std::int64_t handle) {
  auto *stream = decodeHandle<HttpStream>(handle);
  if (!stream || stream->error != GTI_HTTP_OK_V1 ||
      (stream->finished && stream->begin == stream->end))
    return;
  stream->error = GTI_HTTP_CANCELLED_V1;
  stream->nativeCode = CURLE_ABORTED_BY_CALLBACK;
  stream->begin = stream->end = 0;
  stream->session->detach();
}
extern "C" std::uint64_t gti_rt_http_stream_bytes_read_v1(std::int64_t handle) {
  auto *stream = decodeHandle<HttpStream>(handle);
  return stream ? stream->delivered : 0;
}
extern "C" gti_http_error_v1 gti_rt_http_stream_error_v1(std::int64_t handle) {
  auto *stream = decodeHandle<HttpStream>(handle);
  return stream ? stream->error : gti_http_error_v1(GTI_HTTP_REQUEST_FAILED_V1);
}
extern "C" std::int32_t gti_rt_http_stream_native_code_v1(std::int64_t handle) {
  auto *stream = decodeHandle<HttpStream>(handle);
  return stream ? static_cast<std::int32_t>(stream->nativeCode) : 0;
}
