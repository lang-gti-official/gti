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
        let value = std::sync::Mutex::new(seed + round);
        for _ in 0..100000 {
            let mut guard = value.lock().unwrap();
            *guard += 1;
        }
        digest += *value.lock().unwrap();
    }
    unsafe { bench_end(digest) };
}
