---
tags: [source]
---
# src/fledit/fledit.c

#source

Component: [[docs/graph/components/fledit|fledit]] -- 3314 lines

## Includes

- [[docs/graph/files/fledit/syntax.h|src/fledit/syntax.h]]
- [[docs/graph/files/flui/flui.h|src/flui/flui.h]]
- 13 system or external headers (not linked)

## Functions (best effort)

- `msg_clear`, `say`, `u8dec`, `cp_width`, `is_valid_utf8`, `prev_char`, `next_char`, `line_of`, `line_end`, `index_grow`, `reindex_from`, `col_of`, `off_of`, `hs_at`, `line_classes`, `detect_indent`, `doc_detect`, `buf_reserve`, `clear_redo`, `undo_push`, `do_replace`, `modified_doc`, `modified`, `do_undo`, `do_redo`, `free_undo`, `has_sel`, `sel_range`, `insert_text`, `delete_sel`, `move_to`, `is_word`, `word_left`, `word_right`, `move_vert`, `indent_unit`, `is_blank`, `ends_word`, `md_marker`, `newline`, `type_text`, `indent_lines`, `copy_sel`, `paste`, `re_prepare`, `match_at`, `match_in_line`, `match_last_in_line`, `find_from`, `find_step`, `count_matches`, `sb_add`, `expand_repl`, `replace_one`, `replace_all`, `doc_reset`, `load_file`, `write_all`, `write_doc`, `save_to`, `open_path_dialog`, `doc_alloc`, `doc_free`, `pos_note`, `pos_restore`, `session_path`, `session_load`, `session_save`, `sess_fire`, `sess_note`, `tab_select`, `tab_insert`, `tab_new`, `tab_open`, `tab_close`, `tab_first_modified`, `perform`, `request`, `save_now`, `expand_home`, `complete_path`, `path_confirm`, `goto_line`, `dialog_frame`, `draw_overlay`, `digits_of`, `make_layout`, `clamp_scroll`, `reveal_cursor`, `hit_test`, `make_palette`, `flush_run`, `draw_line`, `bracket_match`, `handle_key`, `handle_mouse`, `open_find`, `find_refresh`, `draw_find`, `fmt_size`, `draw_status`, `update_title`, `tab_label`, `draw_dot`, `draw_tabs`, `global_keys`, `draw`, `closed`, `focus_changed`, `main`

See also: [fledit component](../../components/fledit.md) · [src/fledit/fledit.c](../../../../src/fledit/fledit.c)
