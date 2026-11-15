Add owning HTTP response streams that implement `std::io::reader`, with bounded buffering, caller-controlled copying, progress counts, cancellation, total deadlines, and opt-in secure redirect policies shared with buffered requests. Streaming responses expose headers before the body and preserve content-encoded bytes for caller-selected decoding.

Requests can opt into `scheme_policy::https_only`, rejecting initial HTTP URLs and HTTP redirect targets before contacting them.
