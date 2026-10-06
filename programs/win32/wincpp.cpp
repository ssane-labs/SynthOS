#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct Shape {
    virtual ~Shape() {}
    virtual double area() const = 0;
    virtual const char *name() const = 0;
};
struct Square : Shape {
    double s;
    Square(double v) : s(v) {}
    double area() const override { return s * s; }
    const char *name() const override { return "square"; }
};
struct Circle : Shape {
    double r;
    Circle(double v) : r(v) {}
    double area() const override { return 3.14159265 * r * r; }
    const char *name() const override { return "circle"; }
};

static int global_counter = 0;
struct Init {
    Init() { global_counter = 7; }
} init_instance;

int main()
{
    std::vector<std::string> words = {"pear", "apple", "fig", "banana"};
    std::sort(words.begin(), words.end());
    std::map<std::string, size_t> lengths;
    for (const auto &w : words)
        lengths[w] = w.size();
    for (const auto &p : lengths)
        std::printf("%s=%zu ", p.first.c_str(), p.second);
    std::printf("\n");
    std::vector<Shape *> shapes = {new Square(3), new Circle(1)};
    for (auto *s : shapes) {
        std::printf("%s area %.2f\n", s->name(), s->area());
        delete s;
    }
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        std::printf("caught: %s\n", e.what());
    }
    std::printf("static init: %d\n", global_counter);
    return 0;
}
