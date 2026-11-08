# Quill sample: Python syntax via Tree-sitter.
from dataclasses import dataclass

@dataclass
class Point:
    x: float
    y: float

    def length(self) -> float:
        return (self.x ** 2 + self.y ** 2) ** 0.5

def main() -> None:
    p = Point(3.0, 4.0)
    items = {"quill": 42, "tree-sitter": 7}
    for key, value in items.items():
        print(f"{key}={value} length={p.length():.2f}")

if __name__ == "__main__":
    main()
