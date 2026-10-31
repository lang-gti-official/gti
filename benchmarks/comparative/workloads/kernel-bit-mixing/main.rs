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
    let mut x = seed;
    for _ in 0..rounds * 200000 {
        x ^= (x << 7) & 1073741823;
        x ^= x >> 9;
        x ^= (x << 8) & 1073741823;
    }
    digest = x;
    unsafe { bench_end(digest) };
}
