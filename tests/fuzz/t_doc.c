/* PDF and the office formats (DOCX, XLSX, PPTX, OpenDocument) as Reader reads them into its page model */
#include <synth/error.h>
#include <synth/fmt.h>
#include <synth/heap.h>
#include <synth/inflate.h>
#include <synth/picture.h>
#include <synth/string.h>
#include <synth/vfs.h>
#include <synth/zip.h>

#pragma GCC diagnostic ignored "-Wunused-function"

#include "../../kernel/gui/optional/doc/model.h"

static int doc_text_width(int style, int size, const char *s, int n)
{
    (void)s;
    return n * size * (style == ST_BOLD ? 7 : 6) / 13;
}

#include "../../kernel/gui/optional/doc/office.h"
#include "../../kernel/gui/optional/doc/pdf.h"
#include "targets.h"

void fuzz_doc(const uint8_t *d, size_t n)
{
    struct doc doc;
    memset(&doc, 0, sizeof(doc));
    if (n > 4 && !memcmp(d, "%PDF", 4))
        pdf_read(&doc, d, n);
    else
        office_read(&doc, d, n);
    doc_free(&doc);
}
