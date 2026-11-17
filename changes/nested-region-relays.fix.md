A returned region call may now take another region call as its argument
(`return pass(same(box));`), or a local alias of what a parameter's stored
reference holds (`Box& local = holder.ref; return pass(local);`). Both were
rejected with GTI-S2127 even when every supplier outlives the result.
