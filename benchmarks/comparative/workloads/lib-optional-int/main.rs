unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}
fn lookup(index: i32) -> Option<i32> {
    if index % 4 == 0 {
        None
    } else {
        Some(index)
    }
}
fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() } % 65536;
    let mut digest: u64 = 0;
    unsafe { bench_begin() };
    for round in 0..rounds {
        for i in 0..1000000u64 {
            if let Some(value) = lookup(((i + seed + round) % 1000003) as i32) {
                digest += value as u64;
            }
        }
    }
    unsafe { bench_end(digest) };
}
