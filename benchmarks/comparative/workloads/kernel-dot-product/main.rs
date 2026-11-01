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
    let mut a = [0.0f64; 4096];
    let mut b = [0.0f64; 4096];
    for i in 0..4096 {
        a[i] = ((i as u64 + seed) % 17) as f64 * 0.125;
        b[i] = (i % 13) as f64 * 0.25;
    }
    let mut total = 0.0;
    for r in 0..rounds * 64 {
        a[(r % 4096) as usize] += 0.125;
        for i in 0..4096 {
            total += a[i] * b[i];
        }
    }
    digest = total as u64;
    unsafe { bench_end(digest) };
}
