#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static int alive;

struct Guard {
    std::string name;
    explicit Guard(const char *n) : name(n) { alive++; }
    ~Guard()
    {
        alive--;
        std::printf("~%s ", name.c_str());
    }
};

static void level3(int x)
{
    Guard g("g3");
    if (x > 2)
        throw std::runtime_error("deep " + std::to_string(x));
}

static void level2(int x)
{
    Guard g("g2");
    std::vector<Guard> v;
    v.emplace_back("v1");
    level3(x);
}

static int level1(int x)
{
    Guard g("g1");
    try {
        level2(x);
    } catch (const std::logic_error &) {
        return -1;
    }
    return 0;
}

int main()
{
    try {
        level1(5);
    } catch (const std::exception &e) {
        std::printf("| caught '%s' alive=%d\n", e.what(), alive);
    }
    try {
        try {
            throw 42;
        } catch (int v) {
            std::printf("inner %d, ", v);
            throw;
        }
    } catch (int v) {
        std::printf("rethrown %d\n", v);
    }
    int caught = 0;
    for (int i = 0; i < 500; i++) {
        try {
            throw std::to_string(i);
        } catch (const std::string &s) {
            caught += s == std::to_string(i);
        }
    }
    auto words = std::make_unique<std::map<std::string, int>>();
    for (const char *w : {"pear", "fig", "apple", "fig"})
        (*words)[w]++;
    std::printf("caught %d, map:", caught);
    for (const auto &kv : *words)
        std::printf(" %s=%d", kv.first.c_str(), kv.second);
    std::printf("\n");
    try {
        std::vector<int> v(3);
        v.at(10) = 1;
    } catch (const std::out_of_range &) {
        std::printf("out_of_range ok\n");
    }
    return 0;
}
