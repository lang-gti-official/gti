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
    let mut b = [0u64; 4096];
    for i in 0..4096 {
        a[i] = (i as u64 + seed) % 251;
    }
    for _ in 0..rounds * 32 {
        for i in 1..4095 {
            b[i] = (a[i - 1] + a[i] + a[i + 1]) / 3;
        }
        for i in 1..4095 {
            a[i] = b[i];
        }
    }
    for i in 0..4096 {
        digest += a[i];
    }
    unsafe { bench_end(digest) };
}
