#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    printf("Hello from Windows on SynthOS!\n");
    printf("argc=%d argv[0]=%s\n", argc, argv[0]);
    for (int i = 1; i < argc; i++)
        printf("  arg %d: %s\n", i, argv[i]);
    printf("pi=%.5f e=%g big=%e int64=%lld hex=%#x\n", 3.14159265, 2.718281828, 12345.678, 1234567890123LL, 255);
    const char *user = getenv("USERNAME");
    printf("user=%s windir=%s\n", user ? user : "?", getenv("windir"));
    char *buf = malloc(64);
    strcpy(buf, "heap ok");
    puts(buf);
    free(buf);
    return argc > 1 ? atoi(argv[1]) : 0;
}
