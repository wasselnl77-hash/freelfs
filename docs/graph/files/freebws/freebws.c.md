---
tags: [source]
---
# src/freebws/freebws.c

#source

Component: [[docs/graph/components/freebws|freebws]] -- 3342 lines

## Includes

- [[docs/graph/files/flcomp/linux-dmabuf-unstable-v1-client-protocol.h|src/flcomp/linux-dmabuf-unstable-v1-client-protocol.h]]
- [[docs/graph/files/flui/flui.h|src/flui/flui.h]]
- [[docs/graph/files/freebws/adblock.h|src/freebws/adblock.h]]
- [[docs/graph/files/freebws/bookmarks.h|src/freebws/bookmarks.h]]
- [[docs/graph/files/freebws/config.h|src/freebws/config.h]]
- [[docs/graph/files/freebws/downloads.h|src/freebws/downloads.h]]
- [[docs/graph/files/freebws/pages.h|src/freebws/pages.h]]
- [[docs/graph/files/freebws/popup.h|src/freebws/popup.h]]
- [[docs/graph/files/freebws/tabstrip.h|src/freebws/tabstrip.h]]
- [[docs/graph/files/libdssl/dssl.h|src/libdssl/dssl.h]]
- 22 system or external headers (not linked)

## Functions (best effort)

- `now_ms`, `flog`, `tab_vb`, `cur_vb`, `sb_release`, `sbuf_get`, `show_pixels`, `export_shm`, `dmb_buffer_release`, `export_dmabuf`, `exp_destroy`, `wpe_mods`, `focus_page`, `p_enter`, `p_leave`, `p_motion`, `p_button`, `p_axis`, `p_frame`, `p_axis_source`, `p_axis_stop`, `p_axis_discrete`, `tab_index`, `tab_label`, `title_update`, `spin_cb`, `spin_start`, `sync_ui`, `tab_realize`, `tab_unrealize`, `snap_store`, `tab_free`, `tab_activate`, `make_room`, `tab_create`, `tab_close`, `tab_discard`, `tab_select_rel`, `idle_activate`, `idle_close`, `focus_url`, `new_tab_focus_url`, `idle_newtab`, `on_key`, `on_focus`, `url_from_input`, `reload_icon`, `nav_button`, `host_in_list`, `uri_host`, `toast_clear`, `toast`, `dark_navigate`, `navigate_hook`, `load_uri`, `dark_refresh_all`, `dark_here`, `pop_close`, `pop_close_hook`, `bookmark_toggle`, `open_in_tab`, `do_action`, `idle_action`, `act`, `pop_row`, `folder_rows`, `folder_list`, `text_dim`, `draw_menu`, `fmt_num`, `draw_shield`, `fmt_size`, `draw_downloads`, `draw_folder`, `pop_draw`, `pop_size`, `pop_open`, `pop_toggle`, `star_icon`, `shield_icon`, `download_icon`, `tool_button`, `draw_bookmarks_bar`, `draw`, `url_show`, `on_uri`, `on_title`, `on_progress`, `eval_done`, `eval_later`, `on_load`, `on_load_failed`, `on_tls_failed`, `popup_show_late`, `on_ready`, `on_create`, `on_close_req`, `on_policy`, `on_terminated`, `on_count`, `tab_signals`, `mem_total_mb`, `sandbox_probe`, `sandbox_setup`, `sandbox_paths`, `user_agent_setup`, `cookies_setup`, `tune_webkit`, `settings_apply`, `mem_available_mb`, `discard_log`, `has_popup`, `mem_tick`, `dssl_scheme`, `jsc_str`, `on_dssl_message`, `dssl_setup_view`, `dssl_init`, `on_close`, `dl_flash_end`, `on_dl_changed`, `fb_open_url`, `fb_tab_retry`, `fb_tab_tls_proceed`, `fb_tab_blocked_proceed`, `fb_setting_changed`, `fb_adb_changed`, `src_prepare`, `src_check`, `src_dispatch`, `dmabuf_fallback`, `dmabuf_watchdog`, `hang_watchdog`, `flcomp_takes_planes`, `fdo_egl_start`, `dmb_format`, `dmb_modifier`, `on_global`, `main`

See also: [freebws component](../../components/freebws.md) · [src/freebws/freebws.c](../../../../src/freebws/freebws.c)
