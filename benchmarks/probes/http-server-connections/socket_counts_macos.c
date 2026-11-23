// Count actual transport calls in the two-client package server during a
// diagnostic run.
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
static uint64_t accepts, sends, receives, waits, blocked_writes, blocked_reads,
    sent_bytes, received_bytes;
static int counted_accept(int fd, struct sockaddr *addr, socklen_t *size) {
  ++accepts;
  return accept(fd, addr, size);
}
static int counted_poll(struct pollfd *fds, nfds_t count, int timeout) {
  ++waits;
  return poll(fds, count, timeout);
}
static ssize_t counted_send(int fd, const void *data, size_t size, int flags) {
  ++sends;
  ssize_t n = send(fd, data, size, flags);
  if (n > 0)
    sent_bytes += (uint64_t)n;
  else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    ++blocked_writes;
  return n;
}
static ssize_t counted_recv(int fd, void *data, size_t size, int flags) {
  ++receives;
  ssize_t n = recv(fd, data, size, flags);
  if (n > 0)
    received_bytes += (uint64_t)n;
  else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    ++blocked_reads;
  return n;
}
#define INTERPOSE(fn, original)                                                \
  __attribute__((used, section("__DATA,__interpose"))) static const struct {   \
    const void *replacement, *symbol;                                          \
  } entry_##original = {fn, original}
INTERPOSE(counted_send, send);
INTERPOSE(counted_recv, recv);
INTERPOSE(counted_accept, accept);
INTERPOSE(counted_poll, poll);
__attribute__((destructor)) static void report(void) {
  fprintf(stderr,
          "accepts=%" PRIu64 " sends=%" PRIu64 " receives=%" PRIu64
          " polls=%" PRIu64 " would_block_writes=%" PRIu64
          " would_block_reads=%" PRIu64 " sent_bytes=%" PRIu64
          " received_bytes=%" PRIu64 "\n",
          accepts, sends, receives, waits, blocked_writes, blocked_reads,
          sent_bytes, received_bytes);
}
