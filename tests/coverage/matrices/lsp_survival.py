#!/usr/bin/env python3
"""Open each file in gti_lsp and check the server survives and answers.

An LSP crash is worse than a compiler crash: it takes the user's editor
session with it, and the user did not ask for a build.
"""
import json, os, pathlib, subprocess, sys, time

SERVER = sys.argv[1]
FILES = [pathlib.Path(p) for p in sys.argv[2:]]

def frame(obj):
    body = json.dumps(obj).encode()
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body

def read_message(proc, timeout=25.0):
    # Headers then body; returns None on EOF/timeout.
    deadline = time.time() + timeout
    header = b""
    while b"\r\n\r\n" not in header:
        if time.time() > deadline:
            return None
        ch = proc.stdout.read(1)
        if not ch:
            return None
        header += ch
    length = 0
    for line in header.decode(errors="replace").split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    body = proc.stdout.read(length)
    if not body:
        return None
    return json.loads(body)

def probe(path):
    proc = subprocess.Popen([SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    try:
        proc.stdin.write(frame({"jsonrpc":"2.0","id":1,"method":"initialize",
                                "params":{"processId":None,"rootUri":None,"capabilities":{}}}))
        proc.stdin.flush()
        if read_message(proc) is None:
            return "no initialize response"
        proc.stdin.write(frame({"jsonrpc":"2.0","method":"initialized","params":{}}))
        text = path.read_text(errors="replace")
        uri = "file://" + str(path.resolve())
        proc.stdin.write(frame({"jsonrpc":"2.0","method":"textDocument/didOpen",
                                "params":{"textDocument":{"uri":uri,"languageId":"gti",
                                                          "version":1,"text":text}}}))
        proc.stdin.flush()
        # Ask for something that forces analysis, then require an answer.
        proc.stdin.write(frame({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol",
                                "params":{"textDocument":{"uri":uri}}}))
        proc.stdin.flush()
        deadline = time.time() + 40
        while time.time() < deadline:
            msg = read_message(proc, timeout=max(1.0, deadline - time.time()))
            if msg is None:
                break
            if msg.get("id") == 2:
                return "ok"
        rc = proc.poll()
        if rc is not None:
            return f"SERVER DIED (exit {rc})"
        return "NO RESPONSE (hang)"
    finally:
        try:
            proc.kill(); proc.wait(timeout=5)
        except Exception:
            pass

bad = 0
for f in FILES:
    result = probe(f)
    if result != "ok":
        bad += 1
        print(f"  {f.name:38s} {result}", flush=True)
print(f"checked={len(FILES)} problems={bad}")
