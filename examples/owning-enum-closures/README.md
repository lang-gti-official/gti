# Owning enum closures

A callback moves a generic payload enum containing a string into its closure,
then transfers the closure into `std::move_only_function<int() &&>`. The stored
callback owns the active payload until consuming invocation or destruction.

Run with `gti main.gti --backend llvm -O3 -o example && ./example`.
The `lambda_owning_transport_runtime` test runs this example at O0 and O3.
Captures remain immutable; moving a captured field out of a closure and owning
enum types with conditional borrowed payloads are outside this example's support.
