"""Exact copy/move lifecycle and lambda return publication composition."""
from pathlib import Path

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    sources = {
        "relay_copyable": root / "coverage/reproducers/a16-relay-copyable-lambda.gti",
        "vector_callable": root / "coverage/reproducers/a32-vector-push-back-move-only-function.gti",
        "optional_callable": root / "coverage/reproducers/a32-optional-emplace-move-only-function.gti",
        "generic_callable_transport": root / "fixtures/generic_callable_transport.gti",
        "nested_callable_results": root / "fixtures/nested_callable_results.gti",
        "loop_closure_reconstruction": root / "fixtures/loop_closure_reconstruction.gti",
        "nested_callable_reproducer": root / "coverage/reproducers/f14-nested-callable-invocation.gti",
        "higher_order_lambdas": root / "coverage/c52_higher_order_lambdas.gti",
    }
    cases = {
        "nested_callable_reproducer": [((), (0, "12\n", ""))],
        "higher_order_lambdas": [((), (0, "5 12 5 42 33\n", ""))],
    }
    main(names=tuple(sources), fixture_paths=sources, cases=cases)
