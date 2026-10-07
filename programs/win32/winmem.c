/* Allocates memory in 64 MB blocks until the process runs out, then checks every block. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    enum { BLOCK = 64 << 20 };
    static unsigned char *blocks[64];
    int n = 0;
    while (n < 64 && (blocks[n] = malloc(BLOCK)) != NULL) {
        memset(blocks[n], n + 1, BLOCK);
        n++;
    }
    int bad = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < BLOCK; k += 4096)
            bad += blocks[i][k] != (unsigned char)(i + 1);
    printf("allocated %d MB in %d blocks, %s\n", n * 64, n, bad ? "corrupted" : "verified");
    for (int i = 0; i < n; i++)
        free(blocks[i]);
    return 0;
}
