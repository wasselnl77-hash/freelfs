/* cmd.h -- helper programs for the Settings pages, run without blocking the window
 * SPDX-License-Identifier: MIT
 */
#ifndef CMD_H
#define CMD_H
#include <stddef.h>
#include <flui.h>

#define CMD_ARGS 8          /* program and arguments of one step */
#define CMD_ARG_LEN 200     /* printer URIs */
#define CMD_MAX_STEPS 8
#define CMD_OUT_MAX (64 * 1024)

struct cmd_line {
	int n;                  /* words in arg[]; arg[0] is the program, looked up on PATH */
	int text_fail;          /* also a failure when the output says "Failed" or "failed" */
	char arg[CMD_ARGS][CMD_ARG_LEN];
};

struct cmd_result {
	int code;               /* exit status; -1 when killed by a signal or timed out */
	int timed_out;
	int spawn_errno;        /* not 0: the program could not be started (ENOENT: not installed) */
	const char *out;        /* stdout and stderr, NUL terminated, ANSI colour codes removed; owned by the seq */
	const char *err;
};

/* failed: the index of the step that failed, or -1 when every step went through. r is the last step run. */
typedef void (*seq_done_cb)(void *ud, int failed, const struct cmd_result *r);

struct seq;

/* Runs the steps one after another. Each step is a child with stdout and stderr on pipes that the event
 * loop reads; nothing waits. The callback runs once, from the loop, and is never called inside seq_start.
 * A step that cannot be started, or that outlives timeout_ms (0: 15 s), is a failure. */
struct seq *seq_start(fl_app *app, const struct cmd_line *lines, int n, int timeout_ms, seq_done_cb cb, void *ud);
/* Stops a running sequence and drops its callback. The pointer is dead afterwards: forget it. */
void seq_cancel(struct seq *s);
/* The first non-empty line of stderr, else of stdout (or why it could not start), in buf. */
void cmd_message(const struct cmd_result *r, char *buf, size_t n);

#endif
