#ifndef SYNTH_STRING_H
#define SYNTH_STRING_H

#include <synth/types.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

static inline bool is_digit(int c) { return c >= '0' && c <= '9'; }
static inline bool is_upper(int c) { return c >= 'A' && c <= 'Z'; }
static inline bool is_lower(int c) { return c >= 'a' && c <= 'z'; }
static inline bool is_alpha(int c) { return is_upper(c) || is_lower(c); }
static inline bool is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static inline bool is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline bool is_print(int c) { return (c >= 0x20 && c < 0x7f) || c >= 0x80; }
static inline bool is_xdigit(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline bool is_ident(int c) { return is_alnum(c) || c == '_'; }
static inline int to_upper(int c) { return is_lower(c) ? c - 32 : c; }
static inline int to_lower(int c) { return is_upper(c) ? c + 32 : c; }

size_t str_copy(char *dst, const char *src, size_t cap);
size_t str_append(char *dst, const char *src, size_t cap);
int str_casecmp(const char *a, const char *b);
int str_ncasecmp(const char *a, const char *b, size_t n);
bool str_starts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);
bool str_iends(const char *s, const char *suffix);
char *str_trim(char *s);
void str_upper(char *s);
void str_lower(char *s);
long str_to_long(const char *s, const char **end, int base);
bool str_parse_long(const char *s, long *out);
bool str_in_list(const char *name, const char *list);

#endif
