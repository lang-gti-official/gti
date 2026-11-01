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
    let mut a = [0u64; 4096];
    for i in 0..4096 {
        a[i] = (i as u64 + seed) % 251;
    }
    for r in 0..rounds * 32 {
        for i in 0..4096 {
            a[i] = (a[i] * 17 + r % 251) % 251;
        }
    }
    for i in 0..4096 {
        digest += a[i];
    }
    unsafe { bench_end(digest) };
}
