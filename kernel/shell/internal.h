#ifndef SYNTH_SHELL_INTERNAL_H
#define SYNTH_SHELL_INTERNAL_H

#include <synth/shell.h>

enum tok_type {
    TOK_WORD,
    TOK_PIPE,
    TOK_AND,
    TOK_OR,
    TOK_OUT,
    TOK_APPEND,
    TOK_IN,
    TOK_ERR,
    TOK_ERR_APPEND,
    TOK_ERR_TO_OUT,
    TOK_COMMENT,
    TOK_SEMI,
};

struct token {
    enum tok_type type;
    char *text;
    size_t offset;
    size_t length;
};

struct token_list {
    struct token *items;
    size_t count;
    size_t cap;
};

struct strvec {
    char **items;
    size_t count;
    size_t cap;
};

enum keyword {
    KW_NONE,
    KW_IF,
    KW_ELIF,
    KW_ELSE,
    KW_END,
    KW_WHILE,
    KW_FOR,
    KW_FUNC,
    KW_BREAK,
    KW_CONTINUE,
    KW_RETURN,
};

int lex(const char *line, struct token_list *out, bool keep_comments, char *err, size_t errcap);
void tokens_free(struct token_list *t);

void strvec_push(struct strvec *v, const char *s);
void strvec_push_owned(struct strvec *v, char *s);
void strvec_free(struct strvec *v);
void strvec_sort(struct strvec *v);

int expand_word(struct shell *sh, const char *raw, struct strvec *out);
int expand_word_ex(struct shell *sh, const char *raw, struct strvec *out, bool glob);
char *expand_single(struct shell *sh, const char *raw);

size_t split_statements(const char *text, struct strvec *out);
enum keyword statement_keyword(const char *stmt, const char **rest);
int block_balance(const char *text);

int run_statement(struct shell *sh, const char *stmt);
int run_argv(struct shell *sh, int argc, char **argv);
int script_run_text(struct shell *sh, const char *text);

bool sh_is_command(struct shell *sh, const char *name);

#endif
