"""Bounded workers with ordered observation and an exact, nonempty case census."""

from collections import deque
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
import re
import time


def positive_jobs(value):
    if not re.fullmatch(r"[1-9][0-9]*", str(value)):
        raise ValueError("case jobs must be a positive integer")
    return int(value)


def configured_jobs():
    # Standalone scripts remain serial unless explicitly opted in. CTest owns
    # this override and reserves the same number of PROCESSORS for the suite.
    return positive_jobs(os.environ.get("GTI_TEST_CASE_JOBS", "1"))


def run_cases(cases, work, jobs=1, observe=None):
    """Run every unique case; observe results on the caller in original order.

    Only work() is concurrent. observe() is serialized even when compilation
    completes out of order, preserving stateful native observations. Workers
    cannot silently lose/duplicate cases; all ordinary failures are collected
    and fail the suite. KeyboardInterrupt/SystemExit are never treated as passes.
    """
    cases = list(cases)
    jobs = positive_jobs(jobs)
    keys = [key for key, _ in cases]
    if not keys or any(not isinstance(key, str) or not key for key in keys):
        raise ValueError("case plan must have nonempty string identities")
    if len(set(keys)) != len(keys):
        raise ValueError("case plan contains duplicate identities")
    failures = []
    results = []
    started = time.monotonic()
    def complete(key, obtain):
        try:
            value = obtain()
            if observe is not None:
                observe(value)
            results.append((key, value))
        except Exception as error:
            failures.append(f"{key}: {type(error).__name__}: {error}")
            print(f"FAIL {key}", flush=True)
        else:
            print(f"PASS {key}", flush=True)

    if jobs == 1:
        for key, value in cases:
            complete(key, lambda: work(value))
    elif observe is None:
        with ThreadPoolExecutor(max_workers=min(jobs, len(cases))) as executor:
            futures = {executor.submit(work, value): key for key, value in cases}
            for future in as_completed(futures):
                complete(futures[future], future.result)
        by_key = dict(results)
        results = [(key, by_key[key]) for key in keys if key in by_key]
    else:
        # Do not queue the next compile until its predecessor is observed.
        # When observing a completed compile, at most jobs-1 compiles remain
        # in flight: native execution never adds an unaccounted extra worker.
        with ThreadPoolExecutor(max_workers=min(jobs, len(cases))) as executor:
            remaining = iter(cases)
            pending = deque()

            def submit_next():
                case = next(remaining, None)
                if case is not None:
                    key, value = case
                    pending.append((key, executor.submit(work, value)))

            for _ in range(min(jobs, len(cases))):
                submit_next()
            while pending:
                key, future = pending.popleft()
                complete(key, future.result)
                submit_next()
    if failures:
        raise RuntimeError("case failures:\n" + "\n".join(failures))
    if [key for key, _ in results] != keys:
        raise RuntimeError("completed case census differs from the plan")
    print(f"Completed {len(keys)} cases with {min(jobs, len(cases))} workers "
          f"in {time.monotonic() - started:.2f}s", flush=True)
    return results
