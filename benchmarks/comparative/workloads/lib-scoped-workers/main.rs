unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}
fn work(input: u64) -> u64 {
    let mut state = input % 65521;
    for i in 0..250000u64 {
        state = (state * 17 + i) % 65521;
    }
    state
}
fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() } % 65536;
    let mut digest: u64 = 0;
    unsafe { bench_begin() };
    for round in 0..rounds {
        digest += std::thread::scope(|scope| {
            let a = scope.spawn(|| work(seed + round));
            let b = scope.spawn(|| work(seed + round + 1));
            let c = scope.spawn(|| work(seed + round + 2));
            let d = scope.spawn(|| work(seed + round + 3));
            a.join().unwrap() + b.join().unwrap() + c.join().unwrap() + d.join().unwrap()
        });
    }
    unsafe { bench_end(digest) };
}
