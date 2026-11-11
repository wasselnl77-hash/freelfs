/* syntax.h -- line-by-line syntax colouring for fledit
 * SPDX-License-Identifier: MIT
 *
 * One function colours one line: it gets the state the previous line ended in
 * and the line's bytes, writes one class per byte and returns the state after
 * the line. fledit keeps the state at the start of every line (two bytes a
 * line) and colours only the lines in view, so the cost does not depend on
 * the size of the file. No allocation per line, no regex, no libraries.
 */
#ifndef SYNTAX_H
#define SYNTAX_H

#include <stddef.h>
#include <stdint.h>

typedef enum { L_NONE, L_C, L_SH, L_PY, L_JS, L_HTML, L_CSS, L_DSSL, L_MD, L_CONF, L_COUNT } sx_lang;

/* what a byte is; fledit maps these to FreeLFS palette colours */
enum {
	K_TEXT, K_KEYWORD, K_TYPE, K_STRING, K_NUMBER, K_COMMENT, K_FUNC, K_PREPROC, K_PUNCT, K_PROP,
	K_TAG, K_HEAD, K_EMPH, K_LINK, K_SECTION, K_COUNT
};

typedef uint16_t sx_state; /* 0 is the state at the start of a file */

#define SX_HD_MAX 200 /* here-document words one file can use */
#define SX_HD_LEN 40

/* Per-document state that does not fit in sx_state (shell here-document words).
 * Zero it when the document is loaded. */
struct sx_ctx {
	char hd[SX_HD_MAX][SX_HD_LEN]; /* [0] is '-' when the tabs may be stripped, then the word */
	int nhd;
};

/* by file name, then by the first line (#!) */
sx_lang sx_detect(const char *path, const char *first_line, size_t n);
const char *sx_name(sx_lang l);
/* Colour one line (without its newline). cls may be NULL to only compute the state;
 * otherwise it has room for n bytes. Returns the state at the start of the next line. */
sx_state sx_line(struct sx_ctx *cx, sx_lang l, sx_state in, const char *s, size_t n, uint8_t *cls);
#endif
