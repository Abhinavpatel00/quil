// Quill sample: Rust syntax via Tree-sitter.
use std::collections::HashMap;

#[derive(Debug, Clone)]
struct Point { x: f64, y: f64 }

impl Point {
    fn length(&self) -> f64 { (self.x * self.x + self.y * self.y).sqrt() }
}

fn main() {
    let mut map: HashMap<&str, i32> = HashMap::new();
    map.insert("quill", 42);
    let p = Point { x: 3.0, y: 4.0 };
    for (k, v) in &map {
        println!("{k} = {v}, length = {}", p.length());
    }
}
