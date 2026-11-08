/* Quill sample: C syntax via Tree-sitter. */
#include <stdio.h>
#include <stdint.h>

typedef struct Point { float x, y; } Point;

static float length(Point p)
{
    return sqrtf(p.x * p.x + p.y * p.y);
}

int main(void)
{
    Point a = { 3.0f, 4.0f };
    const char* msg = "hello, quill";
    for (int i = 0; i < 10; ++i) {
        printf("%s %d: %.2f\n", msg, i, length(a));
    }
    return 0;
}
