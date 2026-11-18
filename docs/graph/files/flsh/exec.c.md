---
tags: [source]
---
# src/flsh/exec.c

#source

Component: [[docs/graph/components/flsh|flsh]] -- 1397 lines

## Includes

- [[docs/graph/files/flsh/flsh.h|src/flsh/flsh.h]]
- 9 system or external headers (not linked)

## Functions (best effort)

- `strsig`, `sig_number`, `on_signal`, `on_sigint`, `set_handler`, `default_disposition`, `signals_init`, `interactive_signals`, `set_trap`, `print_trap`, `list_traps`, `check_traps`, `run_exit_trap`, `child_reset_signals`, `become_child`, `sh_exit`, `hash_clear`, `hash_forget`, `hash_list`, `is_exec_file`, `find_command`, `define_function`, `lookup_func`, `find_function`, `unset_function`, `function_names`, `cmp_func`, `list_functions`, `save_fd`, `heredoc_fd`, `apply_redirs`, `restore_redirs`, `print_redirs`, `nl_indent`, `print_rec`, `print_list`, `print_node`, `node_text`, `fork_for`, `xtrace`, `status_of_exec_error`, `not_found`, `call_function`, `is_decl_builtin`, `exec_simple`, `exit_trap_set`, `exec_pipeline`, `exec_background`, `exec_subshell`, `control_pending`, `loop_control`, `exec_loop`, `exec_for`, `exec_case`, `exec_tested`, `exec_node`, `eval_string`, `source_file`

See also: [flsh component](../../components/flsh.md) · [src/flsh/exec.c](../../../../src/flsh/exec.c)
