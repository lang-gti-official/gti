unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}

fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() } % 65536;
    let mut digest: u64 = 0;
    unsafe { bench_begin() };
    for round in 0..rounds {
        let value = std::sync::atomic::AtomicU64::new(seed + round);
        for _ in 0..1000000 {
            digest += value.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        }
        digest += value.load(std::sync::atomic::Ordering::Relaxed);
    }
    unsafe { bench_end(digest) };
}
