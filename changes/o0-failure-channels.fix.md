Programs built at `-O0` use much smaller stack frames: calls whose failures can
never be in flight at the same time share failure storage, so a function with
many fallible calls no longer reserves tens of kilobytes per frame and ordinary
recursion no longer overflows the stack.
