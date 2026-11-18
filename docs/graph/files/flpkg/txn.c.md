---
tags: [source]
---
# src/flpkg/txn.c

#source

Component: [[docs/graph/components/flpkg|flpkg]] -- 1078 lines

## Includes

- [[docs/graph/files/flpkg/txn.h|src/flpkg/txn.h]]
- 10 system or external headers (not linked)

## Functions (best effort)

- `abspath`, `exists`, `put_field`, `write_journal`, `mark_committed`, `undo_op`, `finish_op`, `journal_field`, `txn_recover`, `txn_recover_pending`, `txn_new`, `txn_db`, `txn_add_file`, `txn_remove`, `txn_empty`, `txn_print_plan`, `is_removed`, `find_add`, `final_set`, `satisfied`, `check_deps`, `overwrite_ok`, `is_dir_or_link_to_dir`, `merge_dir`, `migrate_to_link`, `dir_in_transaction`, `check_files`, `add_op`, `hidden_name`, `cmp_strp`, `in_newpaths`, `plan_remove`, `plan`, `note_owner`, `set_owner`, `stage_mkdir`, `stage_file`, `stage_package`, `save_config`, `rollback`, `rollback_active`, `apply_ops`, `crash_point`, `cmp_desc`, `remove_empty_dirs`, `record`, `txn_commit`, `txn_free`

See also: [flpkg component](../../components/flpkg.md) · [src/flpkg/txn.c](../../../../src/flpkg/txn.c)
