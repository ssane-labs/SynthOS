#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    double values[16];
    int n = 0;
    printf("Enter numbers (0 to finish):\n");
    while (n < 16 && scanf("%lf", &values[n]) == 1 && values[n] != 0)
        n++;
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += values[i];
    qsort(values, n, sizeof(double), cmp);
    printf("count=%d sum=%.2f mean=%.3f\n", n, sum, n ? sum / n : 0);
    if (n)
        printf("min=%g max=%g\n", values[0], values[n - 1]);
    printf("sqrt(2)=%.6f sin(pi/6)=%.4f pow(2,10)=%.0f exp(1)=%.5f log10(1000)=%.1f\n", sqrt(2.0), sin(M_PI / 6),
           pow(2, 10), exp(1.0), log10(1000.0));
    char word[32];
    int a, b;
    if (sscanf("x=12,y=30 name", "x=%d,y=%d %31s", &a, &b, word) == 3)
        printf("sscanf: %d %d %s\n", a, b, word);
    printf("strtod: %.3f atof: %.2e\n", strtod("  -1.5e3xyz", NULL), atof("6.02e23"));
    return 0;
}
