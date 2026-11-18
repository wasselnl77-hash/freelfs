---
tags: [source]
---
# src/flsh/parse.c

#source

Component: [[docs/graph/components/flsh|flsh]] -- 1116 lines

## Includes

- [[docs/graph/files/flsh/flsh.h|src/flsh/flsh.h]]
- 2 system or external headers (not linked)

## Functions (best effort)

- `arena_new`, `arena_alloc`, `arena_strndup`, `arena_ref`, `arena_unref`, `is_name`, `is_reserved`, `is_meta`, `scan_squote`, `scan_bquote`, `scan_dquote`, `scan_dollar`, `scan_word`, `parser_init`, `dequote_plain`, `read_heredocs`, `alias_active`, `splice_alias`, `lex`, `peek_tok`, `peek_cmd`, `consume`, `is_kw`, `tok_desc`, `expect_kw`, `skip_newlines`, `new_node`, `is_terminator`, `parse_list`, `parse_pipeline`, `parse_and_or`, `parse_redir`, `parse_trailing_redirs`, `add_redir`, `assign_word`, `parse_simple`, `parse_if`, `parse_for`, `parse_case`, `parse_function_body`, `parse_command`, `parse_next`, `parse_is_complete`

See also: [flsh component](../../components/flsh.md) · [src/flsh/parse.c](../../../../src/flsh/parse.c)
