"""Drive one single-threaded, poller-driven server with concurrent loopback clients."""
import contextlib
import pathlib
import select
import socket
import subprocess
import time


def line(server):
    if not select.select([server.stdout], [], [], 8)[0]:
        raise TimeoutError("server did not reach its next synchronization point")
    value = server.stdout.readline()
    if not value:
        raise RuntimeError(f"server exited during exchange: {server.poll()}")
    return value.strip().decode()


def expect(server, wanted):
    """Wait for one marker; markers from other connections may interleave."""
    seen = []
    while True:
        value = line(server)
        if value == wanted:
            return seen
        seen.append(value)
        if len(seen) > 8:
            raise AssertionError(f"expected {wanted!r}, saw {seen!r}")


def header(stream, expected):
    data = bytearray()
    while not data.endswith(b"\r\n\r\n"):
        byte = stream.recv(1)
        if not byte or len(data) > 4096:
            raise AssertionError(f"incomplete response: {data!r}")
        data.extend(byte)
    assert bytes(data) == expected, data


def exercise(server):
    port = int(line(server))
    address = ("127.0.0.1", port)
    slow = socket.create_connection(address, timeout=5)
    fast = socket.create_connection(address, timeout=5)
    with slow, fast:
        # The slow client stalls in the middle of its request head.
        slow.sendall(b"GET /bulk HTTP/1.1\r\nHo")
        expect(server, "partial head 1")
        # The fast client completes a request while the slow one is stalled.
        fast.sendall(b"GET /monitors HTTP/1.1\r\nHost: local\r\n\r\n")
        expect(server, "complete GET /monitors")
        header(fast, b"HTTP/1.1 200 \r\nContent-Length: 0\r\n\r\n")
        # The slow client finishes its head and then reads nothing: the
        # server's 64 MiB response fills the socket and waits for capacity.
        slow.sendall(b"st: local\r\n\r\n")
        expect(server, "backpressured GET /bulk")
        # The backpressured writer does not block the other connection.
        fast.sendall(b"POST /monitors HTTP/1.1\r\nHost: local\r\nContent-Length: 2\r\n\r\n{}"
                     b"POST /missing HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\n"
                     b"Content-Length: 100\r\n\r\n")
        expect(server, "complete POST /monitors")
        expect(server, "complete POST /missing")
        expect(server, "closed 2")
        header(fast, b"HTTP/1.1 201 \r\nContent-Length: 0\r\n\r\n")
        header(fast, b"HTTP/1.1 404 \r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        assert fast.recv(1) == b""
        # Draining the slow client resumes the writer on writable readiness.
        header(slow, b"HTTP/1.1 200 \r\nContent-Length: 67108864\r\n\r\n")
        remaining = 64 * 1024 * 1024
        deadline = time.monotonic() + 20
        while remaining:
            if time.monotonic() > deadline:
                raise TimeoutError("response failed to resume")
            data = slow.recv(min(65536, remaining))
            assert data and data.count(0) == len(data)
            remaining -= len(data)
        expect(server, "complete GET /bulk")
    expect(server, "closed 1")
    assert line(server) == "served"
    assert server.wait(timeout=5) == 0


def main():
    exe, = pathlib.Path("build/gti/dev").glob("*/dashboard")
    with subprocess.Popen([exe], stdout=subprocess.PIPE, bufsize=0) as server:
        try:
            exercise(server)
        finally:
            if server.poll() is None:
                with contextlib.suppress(ProcessLookupError):
                    server.kill()
    print("one poller served concurrent HTTP connections")


if __name__ == "__main__":
    main()
