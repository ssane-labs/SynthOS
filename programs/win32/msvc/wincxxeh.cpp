// C++ exceptions as MSVC builds them (throw/catch through _CxxThrowException and __CxxFrameHandler3): by value, by
// reference to a base class, catch(...), rethrow, and the destructors of the frames in between.
extern "C" int printf(const char *, ...);
extern "C" void exit(int);

void operator delete(void *, unsigned) noexcept {}
void operator delete(void *) noexcept {}

/* the vtable of std::type_info that type descriptors point at (only their names are compared) */
extern "C" const void *const type_info_vftable[2] __asm__("??_7type_info@@6B@") = {0, 0};

struct Err {
    int code;
};
struct Base {
    virtual ~Base() {}
    int v = 1;
};
struct Derived : Base {
    Derived() { v = 2; }
};

static int destroyed;
struct Guard {
    ~Guard() { destroyed++; }
};

__declspec(noinline) static void thrower(int n)
{
    Guard g;
    if (n == 1)
        throw Err{42};
    if (n == 2)
        throw Derived();
    if (n == 3)
        throw 7;
}

static int run()
{
    int r = 0;
    try {
        thrower(1);
    } catch (Err e) {
        r += e.code;
    }
    try {
        thrower(2);
    } catch (Base &b) {
        r += b.v * 100;
    }
    try {
        thrower(3);
    } catch (...) {
        r += 1000;
    }
    try {
        try {
            thrower(1);
        } catch (Err &) {
            throw;
        }
    } catch (Err &e) {
        r += e.code * 1000;
    }
    try {
        thrower(0);
    } catch (...) {
        r = -1;
    }
    return r;
}

extern "C" void start()
{
    int r = run();
    printf("cxx %d destroyed %d\n", r, destroyed);
    exit(0);
}
