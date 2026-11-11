#pragma once

#include "gti/c_abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Private, versioned stream-socket boundary for <std/tcp>. Public GTI programs
// use std::tcp owners and never observe these entries. The runtime owns only
// what differs per host: native address layout, socket constants, errno
// classification, descriptor flags and SIGPIPE suppression. Address parsing,
// formatting, buffering, retry and ownership policy stay in GTI source.
//
// Descriptors are nonnegative int32 values owned by the caller from the moment
// an entry reports one until gti_rt_tcp_close_v1 consumes it. Every entry is
// call-bounded: no buffer, view or address pointer is retained after return.
// Interrupted system calls are retried inside the runtime.

typedef uint32_t gti_tcp_status_v1;
enum {
  GTI_TCP_OK_V1 = 0,
  GTI_TCP_WOULD_BLOCK_V1 = 1,
  GTI_TCP_TIMED_OUT_V1 = 2,
  GTI_TCP_CONNECTION_REFUSED_V1 = 3,
  GTI_TCP_CONNECTION_RESET_V1 = 4,
  GTI_TCP_CONNECTION_ABORTED_V1 = 5,
  GTI_TCP_NOT_CONNECTED_V1 = 6,
  GTI_TCP_BROKEN_PIPE_V1 = 7,
  GTI_TCP_ADDRESS_IN_USE_V1 = 8,
  GTI_TCP_ADDRESS_NOT_AVAILABLE_V1 = 9,
  GTI_TCP_NETWORK_UNREACHABLE_V1 = 10,
  GTI_TCP_HOST_UNREACHABLE_V1 = 11,
  GTI_TCP_PERMISSION_DENIED_V1 = 12,
  GTI_TCP_INVALID_ARGUMENT_V1 = 13,
  GTI_TCP_NAME_NOT_FOUND_V1 = 14,
  GTI_TCP_NAME_RESOLUTION_FAILED_V1 = 15,
  GTI_TCP_RESOURCE_EXHAUSTED_V1 = 16,
  GTI_TCP_UNSUPPORTED_V1 = 17,
  GTI_TCP_FAILED_V1 = 18,
};

// An encoded socket address is exactly GTI_TCP_ADDRESS_SIZE_V1 bytes:
//   [0]      family tag, 4 (IPv4) or 6 (IPv6)
//   [1]      reserved, zero
//   [2..3]   port, big-endian
//   [4..7]   IPv6 scope id, big-endian (zero for IPv4)
//   [8..23]  address bytes in network order; IPv4 uses [8..11], rest zero
// The record is a byte sequence, not a native sockaddr, so GTI source never
// depends on a host's sockaddr layout.
enum { GTI_TCP_ADDRESS_SIZE_V1 = 24 };

typedef uint32_t gti_tcp_option_v1;
enum {
  // value: 0 or 1. Applies to reads, writes and accept on this descriptor.
  GTI_TCP_OPTION_NONBLOCKING_V1 = 0,
  // value: 0 or 1 (TCP_NODELAY).
  GTI_TCP_OPTION_NODELAY_V1 = 1,
  // value: milliseconds; 0 removes the timeout. A blocking read or write
  // that expires reports GTI_TCP_TIMED_OUT_V1.
  GTI_TCP_OPTION_READ_TIMEOUT_MS_V1 = 2,
  GTI_TCP_OPTION_WRITE_TIMEOUT_MS_V1 = 3,
  // value: 0 or 1 (SO_KEEPALIVE).
  GTI_TCP_OPTION_KEEPALIVE_V1 = 4,
};

typedef uint32_t gti_tcp_listen_flags_v1;
enum {
  GTI_TCP_LISTEN_REUSE_ADDRESS_V1 = 1,
  GTI_TCP_LISTEN_IPV6_ONLY_V1 = 2,
};

typedef uint32_t gti_tcp_interest_v1;
enum {
  GTI_TCP_INTEREST_READ_V1 = 1,
  GTI_TCP_INTEREST_WRITE_V1 = 2,
};

// Creates a stream socket and connects it to address. A negative timeout_ms
// waits as long as the host allows; otherwise the connection attempt fails
// with GTI_TCP_TIMED_OUT_V1 after that many milliseconds. On success the
// descriptor is blocking, close-on-exec and never raises SIGPIPE.
gti_tcp_status_v1 gti_rt_tcp_connect_v1(const uint8_t *address,
                                        int64_t timeout_ms,
                                        int32_t *output_descriptor);

// Creates, binds and listens on address. backlog <= 0 selects the host
// default.
gti_tcp_status_v1 gti_rt_tcp_listen_v1(const uint8_t *address, int32_t backlog,
                                       gti_tcp_listen_flags_v1 flags,
                                       int32_t *output_descriptor);

// Accepts one pending connection. output_peer receives an encoded address.
gti_tcp_status_v1 gti_rt_tcp_accept_v1(int32_t listener, uint8_t *output_peer,
                                       int32_t *output_descriptor);

// Receives at most capacity bytes. A zero count with GTI_TCP_OK_V1 is the
// peer's orderly end of stream (or an empty request).
gti_tcp_status_v1 gti_rt_tcp_read_v1(int32_t descriptor, uint8_t *output,
                                     uint64_t capacity, uint64_t *output_count);

// Sends at most length bytes and reports the committed prefix.
gti_tcp_status_v1 gti_rt_tcp_write_v1(int32_t descriptor, const uint8_t *input,
                                      uint64_t length, uint64_t *output_count);

// how: 0 read side, 1 write side, 2 both.
gti_tcp_status_v1 gti_rt_tcp_shutdown_v1(int32_t descriptor, uint32_t how);

// Consumes descriptor whatever the status: a failed close is not retryable.
gti_tcp_status_v1 gti_rt_tcp_close_v1(int32_t descriptor);

gti_tcp_status_v1 gti_rt_tcp_local_address_v1(int32_t descriptor,
                                              uint8_t *output);
gti_tcp_status_v1 gti_rt_tcp_peer_address_v1(int32_t descriptor,
                                             uint8_t *output);

gti_tcp_status_v1 gti_rt_tcp_set_option_v1(int32_t descriptor,
                                           gti_tcp_option_v1 option,
                                           int64_t value);

// Waits until the descriptor is ready for one of the interest bits. A negative
// timeout waits indefinitely; expiry reports GTI_TCP_TIMED_OUT_V1. On success
// output_ready holds the ready interest bits; hang-up and error conditions
// report every requested bit so the next operation observes them.
gti_tcp_status_v1 gti_rt_tcp_wait_v1(int32_t descriptor,
                                     gti_tcp_interest_v1 interest,
                                     int64_t timeout_ms,
                                     uint32_t *output_ready);

// Resolves host for stream sockets. family is 0 (any), 4 or 6. Up to
// capacity encoded addresses, each carrying port, are written to output in
// resolver order; output_count receives how many were written.
gti_tcp_status_v1 gti_rt_tcp_resolve_v1(gti_c_string_view host, uint32_t port,
                                        uint32_t family, uint8_t *output,
                                        uint64_t capacity,
                                        uint64_t *output_count);

#ifdef __cplusplus
}
#endif
