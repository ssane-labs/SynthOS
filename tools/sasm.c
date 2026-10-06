#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <synth/heap.h>
#include <synth/sx.h>

void *kmalloc(size_t size)
{
    return malloc(size ? size : 1);
}

void *kzalloc(size_t size)
{
    return calloc(1, size ? size : 1);
}

void *krealloc(void *ptr, size_t size)
{
    return realloc(ptr, size ? size : 1);
}

void kfree(void *ptr)
{
    free(ptr);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        text = NULL;
    }
    if (text)
        text[n] = 0;
    fclose(f);
    return text;
}

static char base_dir[1024];

static int load(void *ctx, const char *path, char **text)
{
    (void)ctx;
    char full[2048];
    snprintf(full, sizeof(full), "%s%s", path[0] == '/' ? "" : base_dir, path);
    *text = read_file(full);
    return *text ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = "a.sx";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else
            in = argv[i];
    }
    if (!in) {
        fprintf(stderr, "usage: sasm source.sasm [-o program.sx]\n");
        return 1;
    }
    char *text = read_file(in);
    if (!text) {
        perror(in);
        return 1;
    }
    const char *slash = strrchr(in, '/');
    if (slash)
        snprintf(base_dir, sizeof(base_dir), "%.*s/", (int)(slash - in), in);
    struct sasm_options opts = {load, NULL};
    struct sasm_output result;
    if (sasm_assemble(slash ? slash + 1 : in, text, &opts, &result) != 0) {
        fprintf(stderr, "%s\n", result.error);
        return 1;
    }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(result.data, 1, result.size, f) != result.size) {
        perror(out);
        return 1;
    }
    fclose(f);
    free(result.data);
    free(text);
    return 0;
}
