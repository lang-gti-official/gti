unsafe extern "C" {
    fn bench_rounds() -> u64;
    fn bench_seed() -> u64;
    fn bench_begin();
    fn bench_end(digest: u64);
}
trait Shape {
    fn area(&self) -> i32;
}
struct Square {
    side: i32,
}
impl Shape for Square {
    fn area(&self) -> i32 {
        self.side * self.side
    }
}
struct Circle {
    radius: i32,
}
impl Shape for Circle {
    fn area(&self) -> i32 {
        3 * self.radius * self.radius
    }
}
fn sum_areas(item: &dyn Shape, count: i32) -> u64 {
    let mut total = 0;
    for i in 0..count {
        total += (item.area() + i % 7) as u64;
    }
    total
}
fn main() {
    let rounds = unsafe { bench_rounds() };
    let seed = unsafe { bench_seed() } % 65536;
    let mut digest: u64 = 0;
    unsafe { bench_begin() };
    for round in 0..rounds {
        let s = Square {
            side: ((seed + round) % 16) as i32 + 1,
        };
        let c = Circle {
            radius: ((seed + round) % 7) as i32 + 1,
        };
        digest += sum_areas(&s, 500000);
        digest += sum_areas(&c, 500000);
    }
    unsafe { bench_end(digest) };
}
