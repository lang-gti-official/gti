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
        let alphabet = b"abcdefghijklmnopqrstuvwxyz";
        let mut text = String::new();
        for i in 0..4096u64 {
            text.push(alphabet[((i + seed + round) % 26) as usize] as char);
        }
        for i in 0..2048 {
            let piece = text[i..i + 32].to_owned();
            for (j, ch) in piece.bytes().enumerate() {
                if ch == b'q' {
                    digest += j as u64 + 1;
                }
            }
        }
    }
    unsafe { bench_end(digest) };
}
