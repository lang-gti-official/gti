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
        let mut values = Vec::with_capacity(32768);
        for i in 0..32768u64 {
            values.push((i * 7919 + seed + round) % 65521);
        }
        values.sort_unstable();
        for (i, value) in values.iter().enumerate() {
            digest += (value * (i as u64 + 1)) % 1000000007;
        }
    }
    unsafe { bench_end(digest) };
}
