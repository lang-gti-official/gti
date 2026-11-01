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
    let mut x = seed;
    let mut state = 0i32;
    for _ in 0..rounds * 100000 {
        x = (x * 48271) % 2147483647;
        match state {
            0 => state = (x % 3) as i32,
            1 => {
                state = (x % 4) as i32;
                digest += 1;
            }
            2 => {
                state = (x % 2) as i32;
                digest += 2;
            }
            _ => {
                state = 0;
                digest += 3;
            }
        }
    }
    digest += state as u64;
    unsafe { bench_end(digest) };
}
