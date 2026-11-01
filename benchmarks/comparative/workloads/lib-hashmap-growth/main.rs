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
        let mut table: std::collections::HashMap<i32, i32> = std::collections::HashMap::new();
        for i in 0..16384i32 {
            table.insert(((i as u64 * 7919 + seed + round) % 32771) as i32, i);
        }
        for key in 0..32768i32 {
            if table.contains_key(&key) {
                digest += table[&key] as u64;
            }
        }
    }
    unsafe { bench_end(digest) };
}
