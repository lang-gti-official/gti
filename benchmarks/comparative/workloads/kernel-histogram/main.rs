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
    let mut bins = [0u64; 256];
    let mut x = seed;
    for _ in 0..rounds * 100000 {
        x = (x * 48271) % 2147483647;
        bins[(x % 256) as usize] += 1;
    }
    for i in 0..256 {
        digest += bins[i] * (i as u64 + 1);
    }
    unsafe { bench_end(digest) };
}
