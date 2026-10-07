#ifndef SYNTH_HTML_H
#define SYNTH_HTML_H

#include <synth/http.h>
#include <synth/types.h>

enum html_kind { HR_TEXT, HR_BREAK, HR_BLOCK, HR_RULE, HR_FIELD, HR_GAP };
enum html_font { HF_NORMAL, HF_BOLD, HF_SMALL, HF_H1, HF_H2, HF_H3, HF_MONO };
enum { HS_PRE = 1, HS_BUTTON = 2, HS_MUTED = 4, HS_ITALIC = 8 };
enum html_field_type { HT_TEXT, HT_PASSWORD, HT_HIDDEN, HT_SUBMIT, HT_CHECK, HT_RADIO, HT_TEXTAREA, HT_SELECT };

#define HTML_FIELD_LINK(i) (-2 - (i))
#define HTML_LINK_FIELD(l) (-2 - (l))

struct html_run {
    uint32_t off, len;
    uint8_t kind, font, flags, indent;
    int32_t link;
};

struct html_field {
    int form;
    uint8_t type;
    bool checked;
    uint32_t name, value, hint;
};

struct html_form {
    uint32_t action;
    bool post;
};

struct html_doc {
    char *text;
    size_t text_len, text_cap;
    struct html_run *runs;
    size_t run_cap;
    int run_count;
    char *pool;
    size_t pool_len, pool_cap;
    uint32_t *links;
    size_t link_cap;
    int link_count;
    struct html_field *fields;
    size_t field_cap;
    int field_count;
    struct html_form *forms;
    size_t form_cap;
    int form_count;
    char title[160];
    char base[HTTP_URL_MAX];
};

int html_parse(struct html_doc *d, const char *src, size_t len, const char *base_url);
int html_plain(struct html_doc *d, const char *src, size_t len, const char *base_url);
void html_free(struct html_doc *d);
const char *html_str(const struct html_doc *d, uint32_t off);
uint32_t html_intern(struct html_doc *d, const char *s);
const char *html_link(const struct html_doc *d, int link);
char *html_to_utf8(const char *src, size_t len, const char *content_type, size_t *out_len);
int html_submit(const struct html_doc *d, int field, char *url, size_t url_cap, char **body, size_t *body_len);

#endif
