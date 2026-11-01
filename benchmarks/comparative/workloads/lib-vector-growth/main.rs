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
        let mut values: Vec<i32> = Vec::new();
        for i in 0..65536u64 {
            values.push(((i * 7919 + seed + round) % 10007) as i32);
        }
        for value in &values {
            digest += *value as u64;
        }
    }
    unsafe { bench_end(digest) };
}
