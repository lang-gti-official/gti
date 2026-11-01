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
    let mut x = (seed % 97) as f64 * 0.125;
    let mut total = 0.0f64;
    for _ in 0..rounds * 200000 {
        x = x * 0.9999 + 0.0001;
        total += x;
    }
    digest = (total * 1000.0) as u64;
    unsafe { bench_end(digest) };
}
