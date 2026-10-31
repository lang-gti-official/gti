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
    let mut a = [0u64; 4096];
    for i in 0..4096 {
        a[i] = i as u64 * 3 + seed % 3;
    }
    let mut x = seed;
    for _ in 0..rounds * 10000 {
        x = (x * 48271) % 2147483647;
        let key = x % 12288;
        let mut lo = 0usize;
        let mut hi = 4096usize;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if a[mid] < key {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        digest += lo as u64;
    }
    unsafe { bench_end(digest) };
}
