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
        "generic_closure_field_copy": root / "fixtures/generic_closure_field_copy.gti",
        "generic_closure_field_construction": root / "fixtures/generic_closure_field_construction.gti",
        "generic_closure_field_composition": root / "fixtures/generic_closure_field_composition.gti",
        "generic_closure_local_copy": root / "fixtures/generic_closure_local_copy.gti",
        "nested_callable_results": root / "fixtures/nested_callable_results.gti",
        "loop_closure_reconstruction": root / "fixtures/loop_closure_reconstruction.gti",
        "nested_callable_reproducer": root / "coverage/reproducers/f14-nested-callable-invocation.gti",
        "higher_order_lambdas": root / "coverage/c52_higher_order_lambdas.gti",
    }
    cases = {
        "generic_closure_field_copy": [
            ((), (0, "10\n13\n14\n99\n8\n7\n16\n8\n13\n14\n8\n9\n", ""))],
        "generic_closure_field_construction": [
            ((), (0, "2\n4\n5\n5\n6\n7\n8\n6\n13\n", ""))],
        "generic_closure_field_composition": [
            ((), (0, "11\n13\n14\n15\n16\n43\n44\n53\n54\n23\n24\n"
                     "named\n1\nnamed\n3\nnamed\n4\nlabeled\n3\nlabeled\n4\n"
                     "first\nsecond\n3\nfirst\nsecond\n4\nboth\n36\nboth\n37\n",
                  ""))],
        "generic_closure_local_copy": [
            ((), (0, "2\n6\n8\n6\n8\n3\n4\n9\n12\n5\n3\n", ""))],
        "nested_callable_reproducer": [((), (0, "12\n", ""))],
        "higher_order_lambdas": [((), (0, "5 12 5 42 33\n", ""))],
    }
    main(names=tuple(sources), fixture_paths=sources, cases=cases)
