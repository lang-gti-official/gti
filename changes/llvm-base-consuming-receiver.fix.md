Calling a method on a fresh temporary of a derived class that stores
references, such as `Derived(x, y).right()` returning a borrow through a base
class's stored field, now builds and runs on the LLVM backend instead of
stopping with GTI-B0003.
