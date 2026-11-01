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
    for r in 0..rounds {
        let mut a = [0i32; 32768];
        let n = 30000 + ((seed + r) % 2000) as usize;
        let mut p = 2;
        while p * p < n {
            if a[p] == 0 {
                let mut j = p * p;
                while j < n {
                    a[j] = 1;
                    j += p;
                }
            }
            p += 1;
        }
        for i in 2..n {
            if a[i] == 0 {
                digest += i as u64;
            }
        }
    }
    unsafe { bench_end(digest) };
}
