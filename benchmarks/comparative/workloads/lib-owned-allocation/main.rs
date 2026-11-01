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
        let mut values: Vec<Box<u64>> = Vec::with_capacity(8192);
        for i in 0..8192u64 {
            values.push(Box::new((i + seed + round) % 65521));
        }
        for value in &values {
            digest += **value;
        }
    }
    unsafe { bench_end(digest) };
}
