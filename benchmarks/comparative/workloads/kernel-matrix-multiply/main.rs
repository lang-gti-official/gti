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
    let mut a = [0.0f64; 2304];
    let mut b = [0.0f64; 2304];
    let mut c = [0.0f64; 2304];
    for i in 0..2304 {
        a[i] = ((i as u64 + seed) % 17) as f64 * 0.125;
        b[i] = (i % 13) as f64 * 0.25;
    }
    for _ in 0..rounds {
        for i in 0..48 {
            for k in 0..48 {
                let v = a[i * 48 + k];
                for j in 0..48 {
                    c[i * 48 + j] += v * b[k * 48 + j];
                }
            }
        }
    }
    let mut total = 0.0;
    for i in 0..2304 {
        total += c[i];
    }
    digest = total as u64;
    unsafe { bench_end(digest) };
}
