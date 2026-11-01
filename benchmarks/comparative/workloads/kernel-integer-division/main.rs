unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}

fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() };
    unsafe { bench_begin() };
    let mut digest: u64 = 0;
    let mut x = seed + 1000000007;
    for i in 0..rounds * 100000 {
        let d = 3 + (i + seed) % 251;
        x = (x / d + x % d + 1234567) % 1000000007;
    }
    digest = x;
    unsafe { bench_end(digest) };
}
