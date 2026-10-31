unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}
fn step(x: u64) -> u64 {
    (x * 31 + 7) % 1000000007
}
fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() };
    unsafe { bench_begin() };
    let mut digest: u64 = 0;
    let mut x = seed;
    for i in 0..rounds * 100000 {
        x = step(step(x) + i % 17);
    }
    digest = x;
    unsafe { bench_end(digest) };
}
