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
        for i in 0..65536u64 {
            text.push(alphabet[((i * 7 + seed + round) % 26) as usize] as char);
        }
        let mut offset = 0;
        while let Some(relative) = text[offset..].find('q') {
            let position = offset + relative;
            digest += position as u64;
            offset = position + 1;
        }
        digest += text.len() as u64;
    }
    unsafe { bench_end(digest) };
}
