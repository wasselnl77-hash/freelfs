---
tags: [source]
---
# src/flcomp/seat.c

#source

Component: [[docs/graph/components/flcomp|flcomp]] -- 1413 lines

## Includes

- [[docs/graph/files/flcomp/comp.h|src/flcomp/comp.h]]
- 8 system or external headers (not linked)

## Functions (best effort)

- `load_keymap_fd`, `send_keymap`, `send_modifiers_to`, `for_focus_keyboards`, `numlock_on`, `update_leds`, `numlock_set`, `build_keymap`, `seat_set_layout`, `seat_layout`, `read_config`, `cursor_rect`, `cursor_damage`, `cursor_draw`, `ptr_frame`, `seat_set_ptr_focus`, `send_motion`, `seat_refocus`, `motion_common`, `clamp_pointer`, `seat_motion`, `seat_motion_abs`, `press_outside`, `seat_button`, `seat_axis`, `sel_changed`, `seat_selection_watch`, `ds_offer`, `ds_destroy_req`, `ds_set_actions`, `selection_broadcast`, `dsource_destroy`, `offer_accept`, `offer_finish`, `offer_set_actions`, `offer_receive`, `offer_destroy_req`, `offer_destroy`, `send_selection_to`, `fsrc_free`, `selection_cancel`, `seat_set_foreign_selection`, `seat_clear_foreign_selection`, `seat_wayland_selection_mimes`, `seat_wayland_selection_send`, `cliphist_count`, `cliphist_get`, `hist_push`, `cap_end`, `cap_cb`, `cap_timeout`, `capture_selection`, `hs_send`, `hs_cancelled`, `cliphist_paste`, `dd_start_drag`, `dd_set_selection`, `dd_release`, `ddm_create_source`, `ddm_get_device`, `ddm_bind`, `seat_set_kb_focus`, `seat_surface_gone`, `mod_active`, `handle_binding`, `locked_binding`, `forward_key`, `pend_flush`, `pend_cb`, `synth_paste`, `seat_key`, `seat_apply_config`, `ptr_set_cursor`, `ptr_release`, `kb_release`, `seat_get_pointer`, `seat_get_keyboard`, `seat_get_touch`, `seat_release`, `seat_bind`, `seat_init`

See also: [flcomp component](../../components/flcomp.md) · [src/flcomp/seat.c](../../../../src/flcomp/seat.c)
