unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}
fn digit(code: i32) -> Result<i32, ()> {
    if !(48..=57).contains(&code) {
        Err(())
    } else {
        Ok(code - 48)
    }
}
fn two_digits(high: i32, low: i32) -> Result<i32, ()> {
    let first = digit(high)?;
    let second = digit(low)?;
    Ok(first * 10 + second)
}
fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() } % 65536;
    let mut digest: u64 = 0;
    unsafe { bench_begin() };
    for round in 0..rounds {
        for i in 0..1000000u64 {
            let input = ((i + seed + round) % 1000003) as i32;
            digest += match two_digits(48 + input % 11, 48 + (input / 11) % 10) {
                Ok(value) => value as u64,
                Err(_) => 1,
            };
        }
    }
    unsafe { bench_end(digest) };
}
