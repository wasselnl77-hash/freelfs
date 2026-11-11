/* Transactions: install, upgrade and remove packages as one atomic step.
 * SPDX-License-Identifier: MIT */
#ifndef FLPKG_TXN_H
#define FLPKG_TXN_H

#include "flpkg.h"
#include "db.h"

struct txn_opts {
	bool nodeps;              /* skip dependency and conflict checks */
	struct strlist overwrite; /* globs of untracked files that may be replaced */
};

struct txn;
/* Locks the database (and finishes or undoes an interrupted transaction). */
struct txn *txn_new(const char *root, const struct txn_opts *opts);
struct db *txn_db(struct txn *t);
/* Queue a package file. REASON is "explicit" or "dep"; an upgrade keeps
 * "explicit" if the installed version had it. */
void txn_add_file(struct txn *t, const char *path, const char *reason);
void txn_remove(struct txn *t, const char *name);
bool txn_empty(const struct txn *t);
void txn_print_plan(const struct txn *t, FILE *out);
/* Check, stage, commit, record, run triggers. Dies (after rolling back)
 * on any error. */
void txn_commit(struct txn *t);
void txn_free(struct txn *t);

/* Finish or undo an interrupted transaction under ROOT (caller holds the
 * lock); the _pending variant takes the lock itself and only warns if it
 * cannot. */
void txn_recover(const char *root);
void txn_recover_pending(const char *root);

/* snapshot.c */
void snapshot_before(const char *summary);

/* triggers.c */
void run_triggers(const char *root, const struct strlist *touched);

/* sysusers.c */
void ensure_sysgroups(const char *root, const struct strlist *groups);
void ensure_sysusers(const char *root, const struct strlist *users);

#endif
