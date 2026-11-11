#include "gti/tcp.h"

#include <cstring>
#include <string>

#if defined(_WIN32)

// std.tcp rejects Windows targets at import; these entries keep the runtime
// archive linkable there until a reviewed Winsock adapter exists.
extern "C" {
gti_tcp_status_v1 gti_rt_tcp_connect_v1(const uint8_t *, int64_t, int32_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_listen_v1(const uint8_t *, int32_t,
                                       gti_tcp_listen_flags_v1, int32_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_accept_v1(int32_t, uint8_t *, int32_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_read_v1(int32_t, uint8_t *, uint64_t, uint64_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_write_v1(int32_t, const uint8_t *, uint64_t,
                                      uint64_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_shutdown_v1(int32_t, uint32_t) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_close_v1(int32_t) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_local_address_v1(int32_t, uint8_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_peer_address_v1(int32_t, uint8_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_set_option_v1(int32_t, gti_tcp_option_v1,
                                           int64_t) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_wait_v1(int32_t, gti_tcp_interest_v1, int64_t,
                                     uint32_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
gti_tcp_status_v1 gti_rt_tcp_resolve_v1(gti_c_string_view, uint32_t, uint32_t,
                                        uint8_t *, uint64_t, uint64_t *) {
  return GTI_TCP_UNSUPPORTED_V1;
}
}

#else

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

gti_tcp_status_v1 classify(int error) {
  switch (error) {
  case 0:
    return GTI_TCP_OK_V1;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
  case EWOULDBLOCK:
#endif
  case EAGAIN:
  case EINPROGRESS:
  case EALREADY:
    return GTI_TCP_WOULD_BLOCK_V1;
  case ETIMEDOUT:
    return GTI_TCP_TIMED_OUT_V1;
  case ECONNREFUSED:
    return GTI_TCP_CONNECTION_REFUSED_V1;
  case ECONNRESET:
    return GTI_TCP_CONNECTION_RESET_V1;
  case ECONNABORTED:
    return GTI_TCP_CONNECTION_ABORTED_V1;
  case ENOTCONN:
    return GTI_TCP_NOT_CONNECTED_V1;
  case EPIPE:
    return GTI_TCP_BROKEN_PIPE_V1;
  case EADDRINUSE:
    return GTI_TCP_ADDRESS_IN_USE_V1;
  case EADDRNOTAVAIL:
    return GTI_TCP_ADDRESS_NOT_AVAILABLE_V1;
  case ENETUNREACH:
  case ENETDOWN:
    return GTI_TCP_NETWORK_UNREACHABLE_V1;
  case EHOSTUNREACH:
#ifdef EHOSTDOWN
  case EHOSTDOWN:
#endif
    return GTI_TCP_HOST_UNREACHABLE_V1;
  case EACCES:
  case EPERM:
    return GTI_TCP_PERMISSION_DENIED_V1;
  case EINVAL:
  case EBADF:
  case ENOTSOCK:
  case EAFNOSUPPORT:
  case EDESTADDRREQ:
  case EISCONN:
    return GTI_TCP_INVALID_ARGUMENT_V1;
  case EMFILE:
  case ENFILE:
  case ENOBUFS:
  case ENOMEM:
    return GTI_TCP_RESOURCE_EXHAUSTED_V1;
  case EOPNOTSUPP:
  case EPROTONOSUPPORT:
    return GTI_TCP_UNSUPPORTED_V1;
  default:
    return GTI_TCP_FAILED_V1;
  }
}

uint32_t readBig32(const uint8_t *bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

void writeBig32(uint8_t *bytes, uint32_t value) {
  bytes[0] = uint8_t(value >> 24);
  bytes[1] = uint8_t(value >> 16);
  bytes[2] = uint8_t(value >> 8);
  bytes[3] = uint8_t(value);
}

// Decodes an encoded record into native storage, rejecting malformed tags.
bool decodeAddress(const uint8_t *encoded, sockaddr_storage &storage,
                   socklen_t &length) {
  if (encoded == nullptr || encoded[1] != 0) {
    return false;
  }
  std::memset(&storage, 0, sizeof(storage));
  const uint16_t port = uint16_t((uint16_t(encoded[2]) << 8) | encoded[3]);
  if (encoded[0] == 4) {
    auto *address = reinterpret_cast<sockaddr_in *>(&storage);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    address->sin_len = sizeof(sockaddr_in);
#endif
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    std::memcpy(&address->sin_addr, encoded + 8, 4);
    length = sizeof(sockaddr_in);
    return true;
  }
  if (encoded[0] == 6) {
    auto *address = reinterpret_cast<sockaddr_in6 *>(&storage);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    address->sin6_len = sizeof(sockaddr_in6);
#endif
    address->sin6_family = AF_INET6;
    address->sin6_port = htons(port);
    address->sin6_scope_id = readBig32(encoded + 4);
    std::memcpy(&address->sin6_addr, encoded + 8, 16);
    length = sizeof(sockaddr_in6);
    return true;
  }
  return false;
}

bool encodeAddress(const sockaddr *native, uint8_t *encoded) {
  std::memset(encoded, 0, GTI_TCP_ADDRESS_SIZE_V1);
  if (native->sa_family == AF_INET) {
    const auto *address = reinterpret_cast<const sockaddr_in *>(native);
    const uint16_t port = ntohs(address->sin_port);
    encoded[0] = 4;
    encoded[2] = uint8_t(port >> 8);
    encoded[3] = uint8_t(port);
    std::memcpy(encoded + 8, &address->sin_addr, 4);
    return true;
  }
  if (native->sa_family == AF_INET6) {
    const auto *address = reinterpret_cast<const sockaddr_in6 *>(native);
    const uint16_t port = ntohs(address->sin6_port);
    encoded[0] = 6;
    encoded[2] = uint8_t(port >> 8);
    encoded[3] = uint8_t(port);
    writeBig32(encoded + 4, address->sin6_scope_id);
    std::memcpy(encoded + 8, &address->sin6_addr, 16);
    return true;
  }
  return false;
}

// Applies close-on-exec and SIGPIPE suppression to a new descriptor.
bool prepareDescriptor(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFD);
  if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
    return false;
  }
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                 sizeof(enabled)) != 0) {
    return false;
  }
#endif
  return true;
}

bool setNonblocking(int descriptor, bool enabled) {
  const int flags = fcntl(descriptor, F_GETFL);
  if (flags < 0) {
    return false;
  }
  const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return updated == flags || fcntl(descriptor, F_SETFL, updated) == 0;
}

bool isNonblocking(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFL);
  return flags >= 0 && (flags & O_NONBLOCK) != 0;
}

// A blocking descriptor reports EAGAIN only when SO_RCVTIMEO/SO_SNDTIMEO
// expired; report that as a timeout rather than a nonblocking condition.
gti_tcp_status_v1 classifyTransfer(int descriptor, int error) {
  gti_tcp_status_v1 status = classify(error);
  if (status == GTI_TCP_WOULD_BLOCK_V1 && !isNonblocking(descriptor)) {
    return GTI_TCP_TIMED_OUT_V1;
  }
  return status;
}

int openStreamSocket(int family) { return socket(family, SOCK_STREAM, 0); }

void closeQuietly(int descriptor) {
  const int saved = errno;
  close(descriptor);
  errno = saved;
}

// Waits for one poll event, retrying interrupted waits against the original
// deadline. Returns 1 when ready, 0 on timeout and -1 with errno on failure.
int pollOnce(int descriptor, short events, int64_t timeout_ms, short &revents) {
  timeval start{};
  gettimeofday(&start, nullptr);
  for (;;) {
    int wait = -1;
    if (timeout_ms >= 0) {
      timeval now{};
      gettimeofday(&now, nullptr);
      const int64_t elapsed =
          (int64_t(now.tv_sec) - int64_t(start.tv_sec)) * 1000 +
          (int64_t(now.tv_usec) - int64_t(start.tv_usec)) / 1000;
      const int64_t remaining = timeout_ms - elapsed;
      wait = remaining <= 0
                 ? 0
                 : (remaining > 0x7fffffff ? 0x7fffffff : int(remaining));
    }
    pollfd entry{};
    entry.fd = descriptor;
    entry.events = events;
    const int result = poll(&entry, 1, wait);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    revents = entry.revents;
    return result;
  }
}

} // namespace

extern "C" {

gti_tcp_status_v1 gti_rt_tcp_connect_v1(const uint8_t *address,
                                        int64_t timeout_ms,
                                        int32_t *output_descriptor) {
  sockaddr_storage storage{};
  socklen_t length = 0;
  if (output_descriptor == nullptr ||
      !decodeAddress(address, storage, length)) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  *output_descriptor = -1;
  const int descriptor = openStreamSocket(storage.ss_family);
  if (descriptor < 0) {
    return classify(errno);
  }
  if (!prepareDescriptor(descriptor) || !setNonblocking(descriptor, true)) {
    const int error = errno;
    closeQuietly(descriptor);
    return classify(error);
  }
  int result =
      connect(descriptor, reinterpret_cast<sockaddr *>(&storage), length);
  if (result != 0 && errno != EINPROGRESS && errno != EINTR) {
    const int error = errno;
    closeQuietly(descriptor);
    return classify(error);
  }
  if (result != 0) {
    short revents = 0;
    const int ready = pollOnce(descriptor, POLLOUT, timeout_ms, revents);
    if (ready <= 0) {
      const int error = ready == 0 ? ETIMEDOUT : errno;
      closeQuietly(descriptor);
      return classify(error);
    }
    int pending = 0;
    socklen_t pendingLength = sizeof(pending);
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending,
                   &pendingLength) != 0) {
      pending = errno;
    }
    if (pending != 0) {
      closeQuietly(descriptor);
      return classify(pending);
    }
  }
  if (!setNonblocking(descriptor, false)) {
    const int error = errno;
    closeQuietly(descriptor);
    return classify(error);
  }
  *output_descriptor = descriptor;
  return GTI_TCP_OK_V1;
}

gti_tcp_status_v1 gti_rt_tcp_listen_v1(const uint8_t *address, int32_t backlog,
                                       gti_tcp_listen_flags_v1 flags,
                                       int32_t *output_descriptor) {
  sockaddr_storage storage{};
  socklen_t length = 0;
  if (output_descriptor == nullptr ||
      !decodeAddress(address, storage, length)) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  *output_descriptor = -1;
  const int descriptor = openStreamSocket(storage.ss_family);
  if (descriptor < 0) {
    return classify(errno);
  }
  int enabled = 1;
  bool ok = prepareDescriptor(descriptor);
  if (ok && (flags & GTI_TCP_LISTEN_REUSE_ADDRESS_V1) != 0) {
    ok = setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled,
                    sizeof(enabled)) == 0;
  }
  if (ok && storage.ss_family == AF_INET6) {
    int only = (flags & GTI_TCP_LISTEN_IPV6_ONLY_V1) != 0 ? 1 : 0;
    ok = setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &only,
                    sizeof(only)) == 0;
  }
  if (ok) {
    ok = bind(descriptor, reinterpret_cast<sockaddr *>(&storage), length) == 0;
  }
  if (ok) {
    ok = listen(descriptor, backlog > 0 ? backlog : SOMAXCONN) == 0;
  }
  if (!ok) {
    const int error = errno;
    closeQuietly(descriptor);
    return classify(error);
  }
  *output_descriptor = descriptor;
  return GTI_TCP_OK_V1;
}

gti_tcp_status_v1 gti_rt_tcp_accept_v1(int32_t listener, uint8_t *output_peer,
                                       int32_t *output_descriptor) {
  if (output_peer == nullptr || output_descriptor == nullptr) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  *output_descriptor = -1;
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  int descriptor = -1;
  do {
    length = sizeof(storage);
    descriptor =
        accept(listener, reinterpret_cast<sockaddr *>(&storage), &length);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return classifyTransfer(listener, errno);
  }
  // An accepted socket may inherit O_NONBLOCK from the listener on some
  // hosts; the documented contract is a blocking stream.
  if (!prepareDescriptor(descriptor) || !setNonblocking(descriptor, false) ||
      !encodeAddress(reinterpret_cast<sockaddr *>(&storage), output_peer)) {
    const int error = errno;
    closeQuietly(descriptor);
    return classify(error == 0 ? EINVAL : error);
  }
  *output_descriptor = descriptor;
  return GTI_TCP_OK_V1;
}

gti_tcp_status_v1 gti_rt_tcp_read_v1(int32_t descriptor, uint8_t *output,
                                     uint64_t capacity,
                                     uint64_t *output_count) {
  if (output_count == nullptr || (capacity != 0 && output == nullptr)) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  *output_count = 0;
  if (capacity == 0) {
    return GTI_TCP_OK_V1;
  }
  const size_t request =
      capacity > uint64_t(0x7fffffff) ? size_t(0x7fffffff) : size_t(capacity);
  ssize_t count = -1;
  do {
    count = recv(descriptor, output, request, 0);
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    return classifyTransfer(descriptor, errno);
  }
  *output_count = uint64_t(count);
  return GTI_TCP_OK_V1;
}

gti_tcp_status_v1 gti_rt_tcp_write_v1(int32_t descriptor, const uint8_t *input,
                                      uint64_t length, uint64_t *output_count) {
  if (output_count == nullptr || (length != 0 && input == nullptr)) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  *output_count = 0;
  if (length == 0) {
    return GTI_TCP_OK_V1;
  }
  const size_t request =
      length > uint64_t(0x7fffffff) ? size_t(0x7fffffff) : size_t(length);
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  ssize_t count = -1;
  do {
    count = send(descriptor, input, request, flags);
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    return classifyTransfer(descriptor, errno);
  }
  *output_count = uint64_t(count);
  return GTI_TCP_OK_V1;
}

gti_tcp_status_v1 gti_rt_tcp_shutdown_v1(int32_t descriptor, uint32_t how) {
  int native = 0;
  switch (how) {
  case 0:
    native = SHUT_RD;
    break;
  case 1:
    native = SHUT_WR;
    break;
  case 2:
    native = SHUT_RDWR;
    break;
  default:
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  return shutdown(descriptor, native) == 0 ? gti_tcp_status_v1(GTI_TCP_OK_V1)
                                           : classify(errno);
}

gti_tcp_status_v1 gti_rt_tcp_close_v1(int32_t descriptor) {
  if (descriptor < 0) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  // POSIX leaves the descriptor state unspecified after EINTR; never retry,
  // since another thread may already have reused the number.
  if (close(descriptor) == 0 || errno == EINTR) {
    return GTI_TCP_OK_V1;
  }
  return classify(errno);
}

gti_tcp_status_v1 gti_rt_tcp_local_address_v1(int32_t descriptor,
                                              uint8_t *output) {
  if (output == nullptr) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&storage),
                  &length) != 0) {
    return classify(errno);
  }
  return encodeAddress(reinterpret_cast<sockaddr *>(&storage), output)
             ? gti_tcp_status_v1(GTI_TCP_OK_V1)
             : gti_tcp_status_v1(GTI_TCP_UNSUPPORTED_V1);
}

gti_tcp_status_v1 gti_rt_tcp_peer_address_v1(int32_t descriptor,
                                             uint8_t *output) {
  if (output == nullptr) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  if (getpeername(descriptor, reinterpret_cast<sockaddr *>(&storage),
                  &length) != 0) {
    return classify(errno);
  }
  return encodeAddress(reinterpret_cast<sockaddr *>(&storage), output)
             ? gti_tcp_status_v1(GTI_TCP_OK_V1)
             : gti_tcp_status_v1(GTI_TCP_UNSUPPORTED_V1);
}

gti_tcp_status_v1 gti_rt_tcp_set_option_v1(int32_t descriptor,
                                           gti_tcp_option_v1 option,
                                           int64_t value) {
  switch (option) {
  case GTI_TCP_OPTION_NONBLOCKING_V1:
    if (value != 0 && value != 1) {
      return GTI_TCP_INVALID_ARGUMENT_V1;
    }
    return setNonblocking(descriptor, value == 1)
               ? gti_tcp_status_v1(GTI_TCP_OK_V1)
               : classify(errno);
  case GTI_TCP_OPTION_NODELAY_V1:
  case GTI_TCP_OPTION_KEEPALIVE_V1: {
    if (value != 0 && value != 1) {
      return GTI_TCP_INVALID_ARGUMENT_V1;
    }
    int enabled = int(value);
    const int level =
        option == GTI_TCP_OPTION_NODELAY_V1 ? IPPROTO_TCP : SOL_SOCKET;
    const int name =
        option == GTI_TCP_OPTION_NODELAY_V1 ? TCP_NODELAY : SO_KEEPALIVE;
    return setsockopt(descriptor, level, name, &enabled, sizeof(enabled)) == 0
               ? gti_tcp_status_v1(GTI_TCP_OK_V1)
               : classify(errno);
  }
  case GTI_TCP_OPTION_READ_TIMEOUT_MS_V1:
  case GTI_TCP_OPTION_WRITE_TIMEOUT_MS_V1: {
    if (value < 0) {
      return GTI_TCP_INVALID_ARGUMENT_V1;
    }
    timeval timeout{};
    timeout.tv_sec = time_t(value / 1000);
    timeout.tv_usec = suseconds_t((value % 1000) * 1000);
    const int name =
        option == GTI_TCP_OPTION_READ_TIMEOUT_MS_V1 ? SO_RCVTIMEO : SO_SNDTIMEO;
    return setsockopt(descriptor, SOL_SOCKET, name, &timeout,
                      sizeof(timeout)) == 0
               ? gti_tcp_status_v1(GTI_TCP_OK_V1)
               : classify(errno);
  }
  default:
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
}

gti_tcp_status_v1 gti_rt_tcp_wait_v1(int32_t descriptor,
                                     gti_tcp_interest_v1 interest,
                                     int64_t timeout_ms,
                                     uint32_t *output_ready) {
  if (output_ready == nullptr || interest == 0 ||
      (interest &
       ~uint32_t(GTI_TCP_INTEREST_READ_V1 | GTI_TCP_INTEREST_WRITE_V1)) != 0) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  *output_ready = 0;
  short events = 0;
  if ((interest & GTI_TCP_INTEREST_READ_V1) != 0) {
    events |= POLLIN;
  }
  if ((interest & GTI_TCP_INTEREST_WRITE_V1) != 0) {
    events |= POLLOUT;
  }
  short revents = 0;
  const int result = pollOnce(descriptor, events, timeout_ms, revents);
  if (result < 0) {
    return classify(errno);
  }
  if (result == 0) {
    return GTI_TCP_TIMED_OUT_V1;
  }
  if ((revents & POLLNVAL) != 0) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  uint32_t ready = 0;
  if ((revents & (POLLERR | POLLHUP)) != 0) {
    ready = interest;
  }
  if ((revents & POLLIN) != 0) {
    ready |= GTI_TCP_INTEREST_READ_V1;
  }
  if ((revents & POLLOUT) != 0) {
    ready |= GTI_TCP_INTEREST_WRITE_V1;
  }
  *output_ready = ready & interest;
  return GTI_TCP_OK_V1;
}

gti_tcp_status_v1 gti_rt_tcp_resolve_v1(gti_c_string_view host, uint32_t port,
                                        uint32_t family, uint8_t *output,
                                        uint64_t capacity,
                                        uint64_t *output_count) {
  if (output_count == nullptr || (capacity != 0 && output == nullptr) ||
      port > 65535 || (family != 0 && family != 4 && family != 6) ||
      host.length == 0 || host.data == nullptr) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  *output_count = 0;
  const std::string name(host.data, size_t(host.length));
  if (name.find('\0') != std::string::npos) {
    return GTI_TCP_INVALID_ARGUMENT_V1;
  }
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_family =
      family == 4 ? AF_INET : (family == 6 ? AF_INET6 : AF_UNSPEC);
  hints.ai_flags = AI_ADDRCONFIG;
  addrinfo *results = nullptr;
  const int status = getaddrinfo(name.c_str(), nullptr, &hints, &results);
  if (status != 0) {
    switch (status) {
    case EAI_NONAME:
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
    case EAI_NODATA:
#endif
      return GTI_TCP_NAME_NOT_FOUND_V1;
    case EAI_MEMORY:
      return GTI_TCP_RESOURCE_EXHAUSTED_V1;
    case EAI_FAMILY:
      return GTI_TCP_UNSUPPORTED_V1;
    case EAI_SYSTEM:
      return classify(errno);
    default:
      return GTI_TCP_NAME_RESOLUTION_FAILED_V1;
    }
  }
  uint64_t written = 0;
  for (addrinfo *entry = results; entry != nullptr && written < capacity;
       entry = entry->ai_next) {
    if (entry->ai_addr == nullptr) {
      continue;
    }
    uint8_t *record = output + written * GTI_TCP_ADDRESS_SIZE_V1;
    if (!encodeAddress(entry->ai_addr, record)) {
      continue;
    }
    record[2] = uint8_t(port >> 8);
    record[3] = uint8_t(port);
    ++written;
  }
  freeaddrinfo(results);
  *output_count = written;
  return written == 0 ? gti_tcp_status_v1(GTI_TCP_NAME_NOT_FOUND_V1)
                      : gti_tcp_status_v1(GTI_TCP_OK_V1);
}

} // extern "C"

#endif
