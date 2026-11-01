unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}
fn tree(depth: i32, x: u64) -> u64 {
    if depth == 0 {
        return x % 251;
    }
    tree(depth - 1, (x * 17 + 3) % 1000003) + tree(depth - 1, (x * 13 + 7) % 1000003)
}
fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() };
    unsafe { bench_begin() };
    let mut digest: u64 = 0;
    for r in 0..rounds * 16 {
        digest += tree(16 + ((seed + r) % 3) as i32, seed + r);
    }
    unsafe { bench_end(digest) };
}
