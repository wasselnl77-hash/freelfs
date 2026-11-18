---
tags: [source]
---
# src/flcomp/xdg.c

#source

Component: [[docs/graph/components/flcomp|flcomp]] -- 1642 lines

## Includes

- [[docs/graph/files/flcomp/comp.h|src/flcomp/comp.h]]
- [[docs/graph/files/flcomp/xdg-decoration-unstable-v1-protocol.h|src/flcomp/xdg-decoration-unstable-v1-protocol.h]]
- [[docs/graph/files/flcomp/xdg-shell-protocol.h|src/flcomp/xdg-shell-protocol.h]]
- 3 system or external headers (not linked)

## Functions (best effort)

- `winlog_on`, `win_name`, `work_area`, `geom_w`, `geom_h`, `window_rect`, `window_configure`, `window_by_id`, `window_raise`, `update_kb_focus`, `window_focus`, `top_window`, `window_minimize`, `window_activate`, `window_close`, `window_refit`, `window_set_state`, `window_snap_to`, `window_unsnap`, `window_set_above`, `window_action`, `window_geom_h_pub`, `window_action_on`, `windows_fit_output`, `work_area_changed`, `window_visible`, `window_on_ws`, `ws_count`, `ws_switch`, `ws_step`, `ws_put`, `ws_move_window`, `ws_move_focused`, `ws_configure`, `grab_active`, `grab_start_move`, `grab_start_resize`, `grab_motion`, `grab_end`, `cycling`, `cycle_windows`, `cycle_step`, `cycle_next`, `cycle_prev`, `cycle_end`, `window_destroy`, `popup_destroy`, `window_destroy_for_surface`, `window_place`, `window_commit`, `window_commit_xw`, `window_request_state`, `window_move_begin`, `window_resize_begin`, `popup_send_configure`, `xdg_surface_commit`, `shell_surface_commit`, `menu_dismiss`, `flyout_dismiss`, `popups_dismiss`, `pos_destroy_req`, `pos_set_size`, `pos_set_anchor_rect`, `pos_set_anchor`, `pos_set_gravity`, `pos_set_adjust`, `pos_set_offset`, `pos_nop_reactive`, `pos_nop_size`, `pos_nop_serial`, `positioner_free`, `popup_compute`, `popup_destroy_req`, `popup_grab`, `popup_reposition`, `popup_res_destroy`, `tl_destroy_req`, `tl_set_parent`, `tl_set_title`, `tl_set_app_id`, `tl_show_window_menu`, `tl_move`, `tl_resize`, `tl_set_max_size`, `tl_set_min_size`, `tl_set_maximized`, `tl_unset_maximized`, `tl_set_fullscreen`, `tl_unset_fullscreen`, `tl_set_minimized`, `deco_set_ssd`, `deco_apply`, `deco_destroy_req`, `deco_set_mode`, `deco_unset_mode`, `deco_res_destroy`, `dm_destroy_req`, `dm_get_toplevel_decoration`, `dm_bind`, `tl_res_destroy`, `xs_destroy_req`, `xs_get_toplevel`, `xs_get_popup`, `xs_set_geometry`, `xs_ack_configure`, `xs_res_destroy`, `wm_destroy_req`, `wm_create_positioner`, `wm_get_xdg_surface`, `wm_pong`, `wm_bind`, `xdg_init`

See also: [flcomp component](../../components/flcomp.md) · [src/flcomp/xdg.c](../../../../src/flcomp/xdg.c)
