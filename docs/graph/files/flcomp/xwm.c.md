---
tags: [source]
---
# src/flcomp/xwm.c

#source

Component: [[docs/graph/components/flcomp|flcomp]] -- 1634 lines

## Includes

- [[docs/graph/files/flcomp/xwayland-shell-protocol.h|src/flcomp/xwayland-shell-protocol.h]]
- [[docs/graph/files/flcomp/xwm-int.h|src/flcomp/xwm-int.h]]
- 12 system or external headers (not linked)

## Functions (best effort)

- `xwm_running`, `xwm_display`, `display_free`, `listen_bind`, `listen_arm`, `listen_cb`, `xsurf_destroy_req`, `xsurf_set_serial`, `xsurf_res_destroy`, `xshell_destroy_req`, `xshell_get_surface`, `xshell_bind`, `shell_filter`, `xwin_by_id`, `xw_link`, `match_window`, `match_surface`, `xwm_surface_origin`, `xwm_surface_gone`, `preq_add`, `prop_request`, `props_request_all`, `dupn`, `latin1_to_utf8`, `names_update`, `write_wm_state`, `write_extents`, `prop_apply`, `do_map`, `preq_done`, `pending_replies`, `pq_clear`, `xwm_flush`, `xwin_free`, `xwin_new`, `xw_make_window`, `xw_surface_ready`, `xwm_surface_commit`, `xwm_window_gone`, `window_skip_panel`, `send_client_message`, `xwm_window_close`, `xwm_window_configure`, `xwm_window_changed`, `xwm_focus_sync`, `xwm_sync`, `xwm_or_list`, `xwm_or_visible`, `ev_configure_request`, `ev_configure_notify`, `ev_create`, `ev_destroy`, `ev_map_request`, `ev_map_notify`, `ev_unmap_notify`, `ev_property`, `ev_client_message`, `event_dispatch`, `idle_cb`, `idle_check`, `xclient_gone`, `x_lost`, `x_cb`, `atoms_intern`, `xwm_intern`, `xwm_atom_name`, `xwm_setup`, `xwl_teardown_windows`, `xwl_stop`, `xwm_child_exited`, `disp_cb`, `xwl_start`, `find_bin`, `xwm_init`, `xwm_fini`

See also: [flcomp component](../../components/flcomp.md) · [src/flcomp/xwm.c](../../../../src/flcomp/xwm.c)
