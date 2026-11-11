/* flsh -- the FreeLFS shell: shared declarations.
 * SPDX-License-Identifier: MIT */
#ifndef FLSH_H
#define FLSH_H

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define FLSH_VERSION "0.1"

/* ---- util.c ---------------------------------------------------------------- */
struct strbuf {
	char *buf;
	size_t len, cap;
};
struct strlist {
	char **v;
	size_t n, cap;
};

extern const char *progname;
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sb_add(struct strbuf *sb, const char *s, size_t n);
void sb_addc(struct strbuf *sb, char c);
void sb_addstr(struct strbuf *sb, const char *s);
void sb_addf(struct strbuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *sb_steal(struct strbuf *sb);
void sb_free(struct strbuf *sb);
void sl_add(struct strlist *sl, const char *s);
void sl_push(struct strlist *sl, char *s); /* takes ownership */
void sl_free(struct strlist *sl);
char **sl_nullterm(struct strlist *sl);
bool has_prefix(const char *s, const char *p);
char *read_fd(int fd, size_t *len);
char *read_file(const char *path, size_t *len);
int write_all(int fd, const void *buf, size_t n);
int move_fd_high(int fd); /* dup to >= 10, close-on-exec */
int str_to_int(const char *s, long *out);
int utf8_len(unsigned char c);
int utf8_width(const char *s, size_t n, size_t *bytes);

/* ---- errors -------------------------------------------------------------- */
/* A shell error (syntax error, bad expansion, failed special builtin)
 * unwinds to the nearest catch point, as in other shells. */
struct jmp {
	jmp_buf buf;
	struct jmp *prev;
};
extern struct jmp *jmp_top;
void sh_error(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void sh_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sh_raise(int status) __attribute__((noreturn));

/* ---- lex.c / parse.c ------------------------------------------------------ */
enum ntype {
	N_SIMPLE,
	N_PIPE,
	N_AND,
	N_OR,
	N_NOT,
	N_SEQ,
	N_BG,
	N_SUBSHELL,
	N_BRACE,
	N_IF,
	N_WHILE,
	N_UNTIL,
	N_FOR,
	N_CASE,
	N_FUNC,
};

enum rop { R_IN, R_OUT, R_APPEND, R_CLOBBER, R_RDWR, R_DUPIN, R_DUPOUT, R_HEREDOC };

struct redir {
	struct redir *next;
	int fd;
	enum rop op;
	char *word;      /* the target (raw word); for here-docs the body */
	bool quoted_doc; /* here-doc with a quoted delimiter: no expansion */
	bool strip_tabs; /* <<- */
	char *delim;     /* here-doc delimiter while its body is being read */
};

struct caseitem {
	struct caseitem *next;
	char **pats;
	size_t npats;
	struct node *body;
	bool fallthrough; /* ;& */
};

struct arena;
struct alias_frame;
struct docref;

struct node {
	enum ntype type;
	int lineno;
	struct redir *redirs;
	union {
		struct {
			char **assigns;
			size_t nassigns;
			char **words;
			size_t nwords;
		} simple;
		struct {
			struct node **cmds;
			size_t n;
			bool bang;
		} pipe;
		struct {
			struct node *left, *right;
		} bin;
		struct node *body; /* N_NOT, N_BG, N_SUBSHELL, N_BRACE */
		struct {
			struct node *cond, *then, *els;
		} ifn;
		struct {
			struct node *cond, *body;
		} loop;
		struct {
			char *var;
			char **words;
			size_t nwords;
			bool has_in;
			struct node *body;
		} forn;
		struct {
			char *word;
			struct caseitem *items;
		} casen;
		struct {
			char *name;
			struct node *body;
			struct arena *arena;
		} func;
	};
};

struct arena *arena_new(void);
void *arena_alloc(struct arena *a, size_t n);
char *arena_strndup(struct arena *a, const char *s, size_t n);
void arena_ref(struct arena *a);
void arena_unref(struct arena *a);

/* A parser over a string. parse_next() returns the next complete command
 * (one line's worth at the top level), NULL at the end; *incomplete is set
 * when the input stopped in the middle of a command (interactive shells
 * then ask for another line). */
struct parser {
	const char *src;
	size_t pos, len;
	int lineno;
	struct arena *arena;
	bool incomplete;
	bool eof;
	/* lexer state */
	struct alias_frame *alias;
	struct docref *pending_docs, **pending_tail;
	size_t tok_start;
	int tok_line;
	bool alias_next;
	size_t alias_next_pos; /* the word starting here or later is checked too */
	int tok;
	char *tokword;
	int tok_io; /* io number for a redirection token */
	bool tok_quoted;
	bool peeked;
	bool check_alias;
	bool check_reserved;
	int depth; /* nesting, for "incomplete" */
};

enum {
	T_EOF,
	T_WORD,
	T_NEWLINE,
	T_SEMI,
	T_AMP,
	T_PIPE,
	T_ANDIF,
	T_ORIF,
	T_DSEMI,
	T_SEMIAND,
	T_LPAREN,
	T_RPAREN,
	T_REDIR, /* tokword is the operator */
};

void parser_init(struct parser *p, const char *src, size_t len, int lineno);
struct node *parse_next(struct parser *p);
bool parse_is_complete(const char *src); /* for the line editor */
extern bool parse_quiet;
/* Scan a word's end: returns the index after the word starting at s[i].
 * Used by the lexer and the highlighter. */
size_t scan_word(const char *s, size_t i, size_t n, bool *unterminated);
size_t scan_dollar(const char *s, size_t i, size_t n, bool *unterminated);
bool is_name(const char *s, size_t n);
bool is_reserved(const char *w);

/* ---- var.c ----------------------------------------------------------------- */
#define V_EXPORT 1
#define V_READONLY 2
#define V_LOCAL 4

struct var {
	struct var *next;
	char *name;
	char *value; /* NULL: declared but unset (export X) */
	unsigned flags;
};

void vars_init(char **envp);
const char *var_get(const char *name);
struct var *var_lookup(const char *name);
void var_set(const char *name, const char *value, unsigned flags);
void var_set_int(const char *name, long v);
int var_unset(const char *name);
void var_export(const char *name);
char **var_environ(void);
void var_assign(const char *assign, unsigned flags); /* NAME=VALUE, already expanded */
void var_list(int (*filter)(struct var *), void (*show)(struct var *));
void var_names(struct strlist *out);
void vars_push_scope(void);
void vars_pop_scope(void);
void var_make_local(const char *name);
bool var_scope_active(void);
long var_changes(void); /* bumped when any variable changes (PATH caching) */
long path_changes(void);

struct params {
	char **argv; /* $1.. */
	int argc;
};
extern struct params params;
extern char *arg0;
void params_set(char **argv, int argc);
void params_free(struct params *p);

/* ---- expand.c ---------------------------------------------------------------- */
#define X_SPLIT 1 /* field splitting and pathname expansion */
#define X_NOGLOB 2
#define X_ASSIGN 4 /* tilde after = and : */
#define X_PATTERN 8 /* case pattern: keep quoting as backslashes */
#define X_HEREDOC 16

void expand_words(char **words, size_t n, struct strlist *out);
char *expand_word(const char *w, int flags);
char *expand_heredoc(const char *body);
char *expand_pattern(const char *w);
char *expand_string(const char *s); /* parameter/command/arith only (prompts) */
char *command_subst(const char *cmd, size_t len);
bool pattern_match(const char *pat, const char *s);
char *dequote(const char *w);

/* ---- arith.c --------------------------------------------------------------------- */
long arith_eval(const char *expr);

/* ---- exec.c ---------------------------------------------------------------------- */
#define EV_EXIT 1    /* the shell exits after this: exec in place */
#define EV_TESTED 2  /* errexit is off (condition of if, &&, !) */
#define EV_NOFORK 4

struct sh {
	int status;    /* $? */
	pid_t pid;     /* $$ */
	pid_t lastbg;  /* $! */
	bool interactive;
	bool hints;    /* started interactive: friendly messages, also in children */
	bool login;
	int sourcedepth;
	int noerrexit; /* > 0: inside a tested command (if, &&, !, ...) */
	long cmdsubsts; /* command substitutions run so far */
	bool subshell; /* in a forked child */
	int loopdepth;
	int breaks;    /* pending break/continue levels */
	bool cont;
	bool returning;
	int funcdepth;
	int lineno;
	char *script;  /* script name for messages */
	bool exiting;
	bool exiting_after; /* -c or a script: the shell ends after the text */
	/* options */
	bool o_errexit, o_nounset, o_xtrace, o_noglob, o_noexec, o_verbose, o_noclobber,
	    o_allexport, o_monitor, o_pipefail, o_notify, o_ignoreeof;
};
extern struct sh sh;

int exec_node(struct node *n, int flags);
extern struct arena *cur_arena;
extern char *self_exe;
extern volatile int int_pending;
extern volatile int last_signal;
int set_trap(int sig, const char *cmd); /* cmd NULL: default, "": ignore */
void list_traps(void);
void run_exit_trap(void);
void signals_init(void);
void interactive_signals(void);
void become_child(void);
bool exit_trap_set(void);
bool guard_rm_allows(const char *cmdpath, char **argv); /* guard.c: the easter egg */
int eval_string(const char *s, const char *name);
int source_file(const char *path, bool must_exist);
char *find_command(const char *name, bool use_hash); /* malloc'd path or NULL */
void hash_clear(void);
void hash_list(void);
bool hash_forget(const char *name);
void sh_exit(int status) __attribute__((noreturn));
void define_function(const char *name, struct node *body, struct arena *a);
struct node *find_function(const char *name);
bool unset_function(const char *name);
void list_functions(bool bodies);
void function_names(struct strlist *out);
void check_traps(void);
void child_reset_signals(void);
const char *strsig(int sig);
int sig_number(const char *name);
void print_node(struct strbuf *sb, const struct node *n);

/* ---- redir --------------------------------------------------------------------- */
struct saved_fd {
	int fd, saved; /* saved < 0: fd was closed */
};
struct redir_save {
	struct saved_fd *v;
	size_t n, cap;
};
int apply_redirs(struct redir *r, struct redir_save *save);
void restore_redirs(struct redir_save *save);

/* ---- builtin.c --------------------------------------------------------------------- */
typedef int (*builtin_fn)(int argc, char **argv);
struct builtin {
	const char *name;
	builtin_fn fn;
	bool special;
	const char *help;
};
const struct builtin *find_builtin(const char *name);
const struct builtin *builtins_table(size_t *n);
int run_builtin(const struct builtin *b, int argc, char **argv);
const char *alias_get(const char *name);
void alias_set(const char *name, const char *value);
struct strlist *alias_names(void);
int bi_test(int argc, char **argv);

/* ---- job.c ---------------------------------------------------------------------- */
struct proc {
	pid_t pid;
	int status;
	bool done, stopped;
	char *cmd;
};
struct job {
	int id;
	pid_t pgid;
	struct proc *procs;
	size_t nprocs;
	char *cmd;
	bool bg;
	bool notified;
	bool changed;
};
extern pid_t shell_pgid;
extern int tty_fd;
void jobs_init(void);
struct job *job_new(pid_t pgid, bool bg, const char *cmd);
void job_add_proc(struct job *j, pid_t pid, const char *cmd);
int job_wait_fg(struct job *j);  /* status of the last process */
void job_notify(bool all);
struct job *job_find(const char *spec, bool quiet);
void job_list(bool long_fmt, bool pids_only);
int job_fg(struct job *j);
int job_bg(struct job *j);
int job_status(struct job *j);
bool job_running(struct job *j);
void job_free_done(void);
void job_reap(bool block);
int job_wait_pid(pid_t pid);
int job_wait_all(void);
size_t job_count(bool stopped_only);
void jobs_forget(void);
void set_tty_pgrp(pid_t pgid);
void jobs_hangup(void);

/* ---- interactive: edit.c, hist.c, complete.c, highlight.c, prompt.c -------------- */
char *edit_line(const char *prompt, bool continuation); /* NULL on EOF */
extern bool edit_interrupted; /* the last line ended with ^C */
char *read_line_plain(void);
void prompt_reset_status(void);
int set_option_letter(char c, bool on);
int set_option_name(const char *name, bool on);
void edit_init(void);
void edit_reset_term(void);
int term_width(void);

void hist_init(void);
void hist_add(const char *line);
size_t hist_count(void);
const char *hist_at(size_t i); /* 0 = oldest */
const char *hist_suggest(const char *prefix);
int bi_history(int argc, char **argv);

struct hl_tok {
	size_t start, end;
	int kind;
	int cmd_tok; /* index of this token's command word, -1 if none */
	int argno;   /* 0 = command word, 1.. arguments */
};
enum {
	HL_PLAIN,
	HL_CMD,
	HL_CMD_BAD,
	HL_CMD_PART, /* still being typed, a prefix of a command */
	HL_RESERVED,
	HL_ARG,
	HL_OPTION,
	HL_STRING,
	HL_VAR,
	HL_OP,
	HL_COMMENT,
	HL_ASSIGN,
	HL_REDIR,
	HL_PATH,
};
size_t hl_lex(const char *s, size_t n, struct hl_tok **out);
void hl_render(const char *s, size_t n, size_t cursor, struct strbuf *out, unsigned char *kinds);
bool command_exists(const char *name);
void commands_names(struct strlist *out); /* builtins, functions, aliases, PATH */
const char *did_you_mean(const char *name);

struct completion {
	struct strlist words; /* full replacements (already quoted) */
	struct strlist shown; /* how the menu shows them */
	size_t start, end;    /* the replaced part of the line */
	bool nospace;
};
void complete_line(const char *line, size_t cursor, struct completion *c);
void completion_free(struct completion *c);
int bi_complete(int argc, char **argv);

char *make_prompt(void);
void prompt_command_started(void);
void prompt_command_finished(int status);
const char *ps2_prompt(void);
bool term_is_linux(void);
extern bool use_color;

#endif
