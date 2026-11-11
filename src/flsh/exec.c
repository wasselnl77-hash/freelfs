/* Running the tree: simple commands, pipelines, compound commands,
 * functions, redirections, traps and the command lookup.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct sh sh;
struct arena *cur_arena;
char *self_exe;
volatile int int_pending;
volatile int last_signal;

/* ---- signals and traps ------------------------------------------------------------------ */
static char *traps[NSIG];          /* command, "" = ignore, NULL = none */
static volatile sig_atomic_t pending[NSIG];
static volatile sig_atomic_t any_pending;
static bool ignored_at_start[NSIG];
static char *exit_trap;

static const struct {
	int sig;
	const char *name;
} signames[] = {
	{SIGHUP, "HUP"}, {SIGINT, "INT"}, {SIGQUIT, "QUIT"}, {SIGILL, "ILL"}, {SIGTRAP, "TRAP"},
	{SIGABRT, "ABRT"}, {SIGBUS, "BUS"}, {SIGFPE, "FPE"}, {SIGKILL, "KILL"}, {SIGUSR1, "USR1"},
	{SIGSEGV, "SEGV"}, {SIGUSR2, "USR2"}, {SIGPIPE, "PIPE"}, {SIGALRM, "ALRM"},
	{SIGTERM, "TERM"}, {SIGCHLD, "CHLD"}, {SIGCONT, "CONT"}, {SIGSTOP, "STOP"},
	{SIGTSTP, "TSTP"}, {SIGTTIN, "TTIN"}, {SIGTTOU, "TTOU"}, {SIGURG, "URG"},
	{SIGXCPU, "XCPU"}, {SIGXFSZ, "XFSZ"}, {SIGVTALRM, "VTALRM"}, {SIGPROF, "PROF"},
	{SIGWINCH, "WINCH"}, {SIGIO, "IO"}, {SIGPWR, "PWR"}, {SIGSYS, "SYS"},
};

const char *strsig(int sig)
{
	for (size_t i = 0; i < sizeof(signames) / sizeof(signames[0]); i++)
		if (signames[i].sig == sig)
			return signames[i].name;
	return NULL;
}

int sig_number(const char *name)
{
	long n;
	if (str_to_int(name, &n) == 0)
		return n >= 0 && n < NSIG ? (int)n : -1;
	if (!strcasecmp(name, "EXIT"))
		return 0;
	if (!strncasecmp(name, "SIG", 3))
		name += 3;
	for (size_t i = 0; i < sizeof(signames) / sizeof(signames[0]); i++)
		if (!strcasecmp(signames[i].name, name))
			return signames[i].sig;
	return -1;
}

static void on_signal(int sig)
{
	last_signal = sig;
	pending[sig] = 1;
	any_pending = 1;
}

static void on_sigint(int sig)
{
	int_pending = 1;
	if (traps[SIGINT])
		on_signal(sig);
}

static void set_handler(int sig, void (*fn)(int))
{
	struct sigaction sa = {0};
	sa.sa_handler = fn;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
}

/* What a signal does in this shell when no trap is set */
static void default_disposition(int sig)
{
	if (ignored_at_start[sig] && !sh.interactive) {
		set_handler(sig, SIG_IGN);
		return;
	}
	if (sh.interactive && !sh.subshell) {
		if (sig == SIGINT) {
			set_handler(sig, on_sigint);
			return;
		}
		if (sig == SIGQUIT || sig == SIGTERM ||
		    (sh.o_monitor && (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU))) {
			set_handler(sig, SIG_IGN);
			return;
		}
	}
	set_handler(sig, SIG_DFL);
}

void signals_init(void)
{
	for (int sig = 1; sig < NSIG; sig++) {
		struct sigaction sa;
		if (sigaction(sig, NULL, &sa) == 0 && sa.sa_handler == SIG_IGN)
			ignored_at_start[sig] = true;
	}
}

void interactive_signals(void)
{
	int sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU};
	for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		if (!traps[sigs[i]])
			default_disposition(sigs[i]);
}

int set_trap(int sig, const char *cmd)
{
	if (sig == 0) {
		free(exit_trap);
		exit_trap = cmd ? xstrdup(cmd) : NULL;
		return 0;
	}
	if (sig == SIGKILL || sig == SIGSTOP)
		return -1;
	if (ignored_at_start[sig] && !sh.interactive)
		return 0; /* POSIX: may not be trapped or reset */
	free(traps[sig]);
	traps[sig] = cmd ? xstrdup(cmd) : NULL;
	if (!cmd)
		default_disposition(sig);
	else if (!*cmd)
		set_handler(sig, SIG_IGN);
	else
		set_handler(sig, sig == SIGINT ? on_sigint : on_signal);
	return 0;
}

static void print_trap(const char *cmd, const char *name)
{
	struct strbuf sb = {0};
	sb_addstr(&sb, "trap -- '");
	for (const char *p = cmd; *p; p++)
		if (*p == '\'')
			sb_addstr(&sb, "'\\''");
		else
			sb_addc(&sb, *p);
	sb_addf(&sb, "' %s\n", name);
	fputs(sb.buf, stdout);
	sb_free(&sb);
}

void list_traps(void)
{
	if (exit_trap)
		print_trap(exit_trap, "EXIT");
	for (int sig = 1; sig < NSIG; sig++)
		if (traps[sig] && strsig(sig))
			print_trap(traps[sig], strsig(sig));
}

void check_traps(void)
{
	if (int_pending && sh.interactive && !sh.subshell && !traps[SIGINT]) {
		int_pending = 0;
		sh_raise(130);
	}
	if (!any_pending)
		return;
	any_pending = 0;
	for (int sig = 1; sig < NSIG; sig++) {
		if (!pending[sig])
			continue;
		pending[sig] = 0;
		if (sig == SIGINT)
			int_pending = 0;
		if (traps[sig] && *traps[sig]) {
			int saved = sh.status;
			char *cmd = xstrdup(traps[sig]);
			eval_string(cmd, NULL);
			free(cmd);
			sh.status = saved;
		}
	}
}

void run_exit_trap(void)
{
	if (!exit_trap)
		return;
	char *cmd = exit_trap;
	exit_trap = NULL;
	struct jmp j;
	j.prev = NULL;
	jmp_top = &j;
	if (setjmp(j.buf) == 0)
		eval_string(cmd, NULL);
	jmp_top = NULL;
	free(cmd);
}

/* In a new child process: plain signal dispositions, traps reset. */
void child_reset_signals(void)
{
	free(exit_trap);
	exit_trap = NULL;
	for (int sig = 1; sig < NSIG; sig++) {
		pending[sig] = 0;
		if (traps[sig] && !*traps[sig])
			continue; /* ignored stays ignored */
		free(traps[sig]);
		traps[sig] = NULL;
		if (ignored_at_start[sig])
			continue;
		struct sigaction sa;
		if (sigaction(sig, NULL, &sa) == 0 && sa.sa_handler != SIG_DFL && sig != SIGKILL &&
		    sig != SIGSTOP)
			set_handler(sig, SIG_DFL);
	}
	any_pending = 0;
	int_pending = 0;
}

void become_child(void)
{
	jmp_top = NULL;
	sh.subshell = true;
	sh.interactive = false;
	sh.o_monitor = false;
	jobs_forget();
	child_reset_signals();
}

/* ---- exit ---------------------------------------------------------------------------- */
void sh_exit(int status)
{
	sh.status = status;
	if (!sh.exiting) {
		sh.exiting = true;
		run_exit_trap();
	}
	fflush(stdout);
	fflush(stderr);
	if (sh.interactive && !sh.subshell) {
		jobs_hangup();
		edit_reset_term();
	}
	exit(status & 0xff);
}

/* ---- the command hash ----------------------------------------------------------------- */
struct hashent {
	struct hashent *next;
	char *name, *path;
	int hits;
};
static struct hashent *cmd_hash;

void hash_clear(void)
{
	for (struct hashent *h = cmd_hash, *next; h; h = next) {
		next = h->next;
		free(h->name);
		free(h->path);
		free(h);
	}
	cmd_hash = NULL;
}

bool hash_forget(const char *name)
{
	for (struct hashent **pp = &cmd_hash; *pp; pp = &(*pp)->next)
		if (!strcmp((*pp)->name, name)) {
			struct hashent *h = *pp;
			*pp = h->next;
			free(h->name);
			free(h->path);
			free(h);
			return true;
		}
	return false;
}

void hash_list(void)
{
	if (!cmd_hash)
		puts("hash: no commands remembered");
	else
		puts("hits\tcommand");
	for (struct hashent *h = cmd_hash; h; h = h->next)
		printf("%4d\t%s\n", h->hits, h->path);
}

static bool is_exec_file(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

char *find_command(const char *name, bool use_hash)
{
	if (strchr(name, '/'))
		return xstrdup(name);
	if (use_hash)
		for (struct hashent *h = cmd_hash; h; h = h->next)
			if (!strcmp(h->name, name)) {
				if (is_exec_file(h->path)) {
					h->hits++;
					return xstrdup(h->path);
				}
				hash_forget(name);
				break;
			}
	const char *path = var_get("PATH");
	if (!path)
		path = "";
	const char *p = path;
	for (;;) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		char *full = len ? xasprintf("%.*s/%s", (int)len, p, name) : xasprintf("./%s", name);
		if (is_exec_file(full)) {
			if (use_hash) {
				struct hashent *h = xcalloc(1, sizeof(*h));
				h->name = xstrdup(name);
				h->path = xstrdup(full);
				h->hits = 1;
				h->next = cmd_hash;
				cmd_hash = h;
			}
			return full;
		}
		free(full);
		if (!colon)
			break;
		p = colon + 1;
	}
	return NULL;
}

/* ---- functions ------------------------------------------------------------------------- */
struct func {
	struct func *next;
	char *name;
	struct node *body;
	struct arena *arena;
};
static struct func *funcs;

void define_function(const char *name, struct node *body, struct arena *a)
{
	for (struct func *f = funcs; f; f = f->next)
		if (!strcmp(f->name, name)) {
			arena_ref(a);
			arena_unref(f->arena);
			f->body = body;
			f->arena = a;
			return;
		}
	struct func *f = xcalloc(1, sizeof(*f));
	f->name = xstrdup(name);
	f->body = body;
	f->arena = a;
	arena_ref(a);
	f->next = funcs;
	funcs = f;
}

static struct func *lookup_func(const char *name)
{
	for (struct func *f = funcs; f; f = f->next)
		if (!strcmp(f->name, name))
			return f;
	return NULL;
}

struct node *find_function(const char *name)
{
	struct func *f = lookup_func(name);
	return f ? f->body : NULL;
}

bool unset_function(const char *name)
{
	for (struct func **pp = &funcs; *pp; pp = &(*pp)->next)
		if (!strcmp((*pp)->name, name)) {
			struct func *f = *pp;
			*pp = f->next;
			free(f->name);
			arena_unref(f->arena);
			free(f);
			return true;
		}
	return false;
}

void function_names(struct strlist *out)
{
	for (struct func *f = funcs; f; f = f->next)
		sl_add(out, f->name);
}

static int cmp_func(const void *a, const void *b)
{
	return strcmp((*(struct func *const *)a)->name, (*(struct func *const *)b)->name);
}

void list_functions(bool bodies)
{
	size_t n = 0;
	for (struct func *f = funcs; f; f = f->next)
		n++;
	struct func **v = xmalloc((n + 1) * sizeof(*v));
	n = 0;
	for (struct func *f = funcs; f; f = f->next)
		v[n++] = f;
	qsort(v, n, sizeof(*v), cmp_func);
	for (size_t i = 0; i < n; i++) {
		if (!bodies) {
			puts(v[i]->name);
			continue;
		}
		struct strbuf sb = {0};
		sb_addf(&sb, "%s() ", v[i]->name);
		print_node(&sb, v[i]->body);
		puts(sb.buf);
		sb_free(&sb);
	}
	free(v);
}

/* ---- redirections ---------------------------------------------------------------------------- */
static void save_fd(struct redir_save *save, int fd)
{
	if (!save)
		return;
	for (size_t i = 0; i < save->n; i++)
		if (save->v[i].fd == fd)
			return;
	if (save->n == save->cap) {
		save->cap = save->cap ? save->cap * 2 : 4;
		save->v = xrealloc(save->v, save->cap * sizeof(*save->v));
	}
	int saved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	save->v[save->n].fd = fd;
	save->v[save->n].saved = saved; /* -1: it was closed */
	save->n++;
}

static int heredoc_fd(struct redir *r)
{
	char *body = r->quoted_doc ? xstrdup(r->word) : expand_heredoc(r->word);
	int fd = memfd_create("flsh-heredoc", MFD_CLOEXEC);
	if (fd < 0) {
		free(body);
		return -1;
	}
	write_all(fd, body, strlen(body));
	lseek(fd, 0, SEEK_SET);
	free(body);
	return fd;
}

int apply_redirs(struct redir *r, struct redir_save *save)
{
	fflush(stdout);
	fflush(stderr);
	for (; r; r = r->next) {
		int newfd = -1;
		char *target = NULL;
		if (r->op == R_HEREDOC) {
			newfd = heredoc_fd(r);
			if (newfd < 0) {
				sh_warn("here-document: %s", strerror(errno));
				return -1;
			}
		} else {
			target = expand_word(r->word, 0);
			if (r->op == R_DUPIN || r->op == R_DUPOUT) {
				if (!strcmp(target, "-")) {
					save_fd(save, r->fd);
					close(r->fd);
					free(target);
					continue;
				}
				long n;
				if (str_to_int(target, &n) < 0 || n < 0 || fcntl((int)n, F_GETFD) < 0) {
					sh_warn("%s: bad file descriptor", target);
					free(target);
					return -1;
				}
				free(target);
				if (n == r->fd)
					continue;
				save_fd(save, r->fd);
				if (dup2((int)n, r->fd) < 0) {
					sh_warn("%ld: %s", n, strerror(errno));
					return -1;
				}
				continue;
			}
			int flags = 0;
			switch (r->op) {
			case R_IN: flags = O_RDONLY; break;
			case R_OUT:
			case R_CLOBBER: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
			case R_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
			case R_RDWR: flags = O_RDWR | O_CREAT; break;
			default: break;
			}
			if (r->op == R_OUT && sh.o_noclobber) {
				struct stat st;
				if (stat(target, &st) == 0 && S_ISREG(st.st_mode)) {
					sh_warn("%s: file exists (noclobber is set)", target);
					free(target);
					return -1;
				}
			}
			newfd = open(target, flags | O_CLOEXEC, 0666);
			if (newfd < 0) {
				sh_warn("%s: %s", target, strerror(errno));
				free(target);
				return -1;
			}
			free(target);
		}
		if (newfd == r->fd) {
			fcntl(newfd, F_SETFD, 0);
			continue;
		}
		save_fd(save, r->fd);
		if (dup2(newfd, r->fd) < 0) {
			sh_warn("%d: %s", r->fd, strerror(errno));
			close(newfd);
			return -1;
		}
		close(newfd);
	}
	return 0;
}

void restore_redirs(struct redir_save *save)
{
	fflush(stdout);
	fflush(stderr);
	for (size_t i = save->n; i-- > 0;) {
		struct saved_fd *s = &save->v[i];
		if (s->saved >= 0) {
			dup2(s->saved, s->fd);
			close(s->saved);
		} else
			close(s->fd);
	}
	free(save->v);
	save->v = NULL;
	save->n = save->cap = 0;
}

/* ---- printing commands (jobs, type, set -x) -------------------------------------------------- */
static void print_redirs(struct strbuf *sb, const struct redir *r)
{
	static const char *const ops[] = {"<", ">", ">>", ">|", "<>", "<&", ">&", "<<"};
	for (; r; r = r->next) {
		bool deffd = r->fd == (r->op == R_IN || r->op == R_RDWR || r->op == R_DUPIN ||
		                               r->op == R_HEREDOC ? 0 : 1);
		sb_addc(sb, ' ');
		if (!deffd)
			sb_addf(sb, "%d", r->fd);
		sb_addstr(sb, ops[r->op]);
		if (r->op == R_HEREDOC)
			sb_addf(sb, "%s%s", r->strip_tabs ? "-" : "", r->delim ? r->delim : "EOF");
		else
			sb_addstr(sb, r->word);
	}
}

static void print_list(struct strbuf *sb, const struct node *n, int indent);

static void nl_indent(struct strbuf *sb, int indent)
{
	sb_addc(sb, '\n');
	for (int i = 0; i < indent; i++)
		sb_addstr(sb, "    ");
}

static void print_rec(struct strbuf *sb, const struct node *n, int indent)
{
	if (!n)
		return;
	switch (n->type) {
	case N_SIMPLE:
		for (size_t i = 0; i < n->simple.nassigns; i++)
			sb_addf(sb, "%s%s", i ? " " : "", n->simple.assigns[i]);
		for (size_t i = 0; i < n->simple.nwords; i++)
			sb_addf(sb, "%s%s", i || n->simple.nassigns ? " " : "", n->simple.words[i]);
		print_redirs(sb, n->redirs);
		return;
	case N_PIPE:
		if (n->pipe.bang)
			sb_addstr(sb, "! ");
		for (size_t i = 0; i < n->pipe.n; i++) {
			if (i)
				sb_addstr(sb, " | ");
			print_rec(sb, n->pipe.cmds[i], indent);
		}
		return;
	case N_AND:
	case N_OR:
		print_rec(sb, n->bin.left, indent);
		sb_addstr(sb, n->type == N_AND ? " && " : " || ");
		print_rec(sb, n->bin.right, indent);
		return;
	case N_NOT:
		sb_addstr(sb, "! ");
		print_rec(sb, n->body, indent);
		return;
	case N_SEQ:
		print_rec(sb, n->bin.left, indent);
		if (n->bin.left->type != N_BG)
			sb_addc(sb, ';');
		if (indent)
			nl_indent(sb, indent);
		else
			sb_addc(sb, ' ');
		print_rec(sb, n->bin.right, indent);
		return;
	case N_BG:
		print_rec(sb, n->body, indent);
		sb_addstr(sb, " &");
		return;
	case N_SUBSHELL:
		sb_addc(sb, '(');
		print_rec(sb, n->body, 0);
		sb_addc(sb, ')');
		break;
	case N_BRACE:
		sb_addc(sb, '{');
		print_list(sb, n->body, indent + 1);
		nl_indent(sb, indent);
		sb_addc(sb, '}');
		break;
	case N_IF:
		sb_addstr(sb, "if ");
		print_rec(sb, n->ifn.cond, 0);
		sb_addstr(sb, "; then");
		print_list(sb, n->ifn.then, indent + 1);
		for (const struct node *e = n->ifn.els; e;) {
			nl_indent(sb, indent);
			if (e->type == N_IF && !e->redirs) {
				sb_addstr(sb, "elif ");
				print_rec(sb, e->ifn.cond, 0);
				sb_addstr(sb, "; then");
				print_list(sb, e->ifn.then, indent + 1);
				e = e->ifn.els;
			} else {
				sb_addstr(sb, "else");
				print_list(sb, e, indent + 1);
				break;
			}
		}
		nl_indent(sb, indent);
		sb_addstr(sb, "fi");
		break;
	case N_WHILE:
	case N_UNTIL:
		sb_addstr(sb, n->type == N_WHILE ? "while " : "until ");
		print_rec(sb, n->loop.cond, 0);
		sb_addstr(sb, "; do");
		print_list(sb, n->loop.body, indent + 1);
		nl_indent(sb, indent);
		sb_addstr(sb, "done");
		break;
	case N_FOR:
		sb_addf(sb, "for %s", n->forn.var);
		if (n->forn.has_in) {
			sb_addstr(sb, " in");
			for (size_t i = 0; i < n->forn.nwords; i++)
				sb_addf(sb, " %s", n->forn.words[i]);
		}
		sb_addstr(sb, "; do");
		print_list(sb, n->forn.body, indent + 1);
		nl_indent(sb, indent);
		sb_addstr(sb, "done");
		break;
	case N_CASE:
		sb_addf(sb, "case %s in", n->casen.word);
		for (const struct caseitem *it = n->casen.items; it; it = it->next) {
			nl_indent(sb, indent + 1);
			for (size_t i = 0; i < it->npats; i++)
				sb_addf(sb, "%s%s", i ? " | " : "", it->pats[i]);
			sb_addc(sb, ')');
			if (it->body)
				print_list(sb, it->body, indent + 2);
			nl_indent(sb, indent + 2);
			sb_addstr(sb, it->fallthrough ? ";&" : ";;");
		}
		nl_indent(sb, indent);
		sb_addstr(sb, "esac");
		break;
	case N_FUNC:
		sb_addf(sb, "%s() ", n->func.name);
		print_rec(sb, n->func.body, indent);
		return;
	}
	print_redirs(sb, n->redirs);
}

static void print_list(struct strbuf *sb, const struct node *n, int indent)
{
	nl_indent(sb, indent);
	print_rec(sb, n, indent);
}

void print_node(struct strbuf *sb, const struct node *n)
{
	print_rec(sb, n, 0);
}

static char *node_text(const struct node *n)
{
	struct strbuf sb = {0};
	print_rec(&sb, n, 0);
	/* one line for job lists */
	for (size_t i = 0; i < sb.len; i++)
		if (sb.buf[i] == '\n')
			sb.buf[i] = ' ';
	return sb_steal(&sb);
}

/* ---- processes -------------------------------------------------------------------------------- */
/* Fork for job j (NULL: a plain child that is waited for at once). In the
 * child, returns 0 with the shell turned into a subshell. */
static pid_t fork_for(struct job *j, bool fg)
{
	fflush(stdout);
	fflush(stderr);
	bool jc = sh.o_monitor && !sh.subshell;
	pid_t pid = fork();
	if (pid < 0)
		sh_error("cannot fork: %s", strerror(errno));
	if (pid == 0) {
		if (jc) {
			pid_t pg = j && j->pgid ? j->pgid : getpid();
			setpgid(0, pg);
			if (fg)
				set_tty_pgrp(pg);
		}
		become_child();
		return 0;
	}
	if (jc && j) {
		if (!j->pgid)
			j->pgid = pid;
		setpgid(pid, j->pgid);
	}
	return pid;
}

static void xtrace(struct strlist *assigns, struct strlist *args)
{
	if (!sh.o_xtrace)
		return;
	char *ps4 = expand_string(var_get("PS4") ? var_get("PS4") : "+ ");
	struct strbuf sb = {0};
	sb_addstr(&sb, ps4);
	free(ps4);
	bool first = true;
	struct strlist *lists[] = {assigns, args};
	for (int l = 0; l < 2; l++)
		for (size_t i = 0; lists[l] && i < lists[l]->n; i++) {
			const char *w = lists[l]->v[i];
			if (!first)
				sb_addc(&sb, ' ');
			first = false;
			bool plain = *w != '\0';
			for (const char *p = w; *p; p++)
				if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./=:,+%@^", *p))
					plain = false;
			if (plain)
				sb_addstr(&sb, w);
			else {
				sb_addc(&sb, '\'');
				for (const char *p = w; *p; p++)
					if (*p == '\'')
						sb_addstr(&sb, "'\\''");
					else
						sb_addc(&sb, *p);
				sb_addc(&sb, '\'');
			}
		}
	sb_addc(&sb, '\n');
	fflush(stdout);
	write_all(2, sb.buf, sb.len);
	sb_free(&sb);
}

static int status_of_exec_error(int err)
{
	return err == ENOENT || err == ENOTDIR ? 127 : 126;
}

/* Replace this process with the command: never returns. */
__attribute__((noreturn)) static void exec_external(const char *path, char **argv)
{
	char **env = var_environ();
	execve(path, argv, env);
	int err = errno;
	if (err == ENOEXEC) {
		/* a script without #!: run it with this shell */
		size_t n = 0;
		while (argv[n])
			n++;
		char **nargv = xcalloc(n + 2, sizeof(char *));
		nargv[0] = (char *)"flsh";
		nargv[1] = (char *)path;
		for (size_t i = 1; i < n; i++)
			nargv[i + 1] = argv[i];
		execve(self_exe ? self_exe : "/usr/bin/flsh", nargv, env);
		err = errno;
	}
	sh_warn("%s: %s", argv[0], strerror(err));
	_exit(status_of_exec_error(err));
}

static void not_found(const char *name)
{
	const char *hint = sh.hints && !strchr(name, '/') ? did_you_mean(name) : NULL;
	if (hint)
		sh_warn("%s: no such command. Did you mean %s?", name, hint);
	else if (strchr(name, '/'))
		sh_warn("%s: no such file", name);
	else
		sh_warn("%s: no such command", name);
}

static int call_function(struct func *f, struct strlist *args, struct strlist *assigns)
{
	struct params saved = params;
	params.argc = (int)args->n - 1;
	params.argv = xcalloc(args->n, sizeof(char *));
	for (size_t i = 1; i < args->n; i++)
		params.argv[i - 1] = xstrdup(args->v[i]);
	vars_push_scope();
	for (size_t i = 0; i < assigns->n; i++) {
		char *eq = strchr(assigns->v[i], '=');
		char *name = xstrndup(assigns->v[i], (size_t)(eq - assigns->v[i]));
		var_make_local(name);
		var_set(name, eq + 1, V_EXPORT);
		free(name);
	}
	struct arena *saved_arena = cur_arena;
	struct arena *a = f->arena;
	arena_ref(a);
	cur_arena = a;
	sh.funcdepth++;
	struct jmp j;
	j.prev = jmp_top;
	jmp_top = &j;
	int st;
	if (setjmp(j.buf) == 0)
		st = exec_node(f->body, 0);
	else {
		/* an error inside: clean up, then pass it on */
		jmp_top = j.prev;
		sh.funcdepth--;
		cur_arena = saved_arena;
		vars_pop_scope();
		params_free(&params);
		params = saved;
		arena_unref(a);
		sh_raise(sh.status);
	}
	jmp_top = j.prev;
	sh.funcdepth--;
	if (sh.returning) {
		sh.returning = false;
		st = sh.status;
	}
	cur_arena = saved_arena;
	vars_pop_scope();
	params_free(&params);
	params = saved;
	arena_unref(a);
	return st;
}

static bool is_decl_builtin(const char *w)
{
	return !strcmp(w, "export") || !strcmp(w, "readonly") || !strcmp(w, "local");
}

static int exec_simple(struct node *n, int flags)
{
	sh.lineno = n->lineno;
	long substs = sh.cmdsubsts;
	struct strlist args = {0}, assigns = {0};
	int st = 0;

	/* the words: export/readonly/local take assignments unsplit */
	if (n->simple.nwords && is_decl_builtin(n->simple.words[0])) {
		sl_add(&args, n->simple.words[0]);
		for (size_t i = 1; i < n->simple.nwords; i++) {
			const char *w = n->simple.words[i];
			const char *eq = strchr(w, '=');
			if (eq && is_name(w, (size_t)(eq - w))) {
				char *v = expand_word(eq + 1, X_ASSIGN);
				sl_push(&args, xasprintf("%.*s=%s", (int)(eq - w), w, v));
				free(v);
			} else
				expand_words(&n->simple.words[i], 1, &args);
		}
	} else
		expand_words(n->simple.words, n->simple.nwords, &args);
	for (size_t i = 0; i < n->simple.nassigns; i++) {
		const char *a = n->simple.assigns[i];
		const char *eq = strchr(a, '=');
		char *v = expand_word(eq + 1, X_ASSIGN);
		sl_push(&assigns, xasprintf("%.*s=%s", (int)(eq - a), a, v));
		free(v);
	}
	xtrace(&assigns, &args);

	if (!args.n) {
		/* only assignments and redirections */
		for (size_t i = 0; i < assigns.n; i++)
			var_assign(assigns.v[i], 0);
		struct redir_save save = {0};
		if (apply_redirs(n->redirs, &save) < 0)
			st = 1;
		else
			st = sh.cmdsubsts != substs ? sh.status : 0;
		restore_redirs(&save);
		goto out;
	}

	const char *name = args.v[0];
	bool skip_funcs = false;
	/* command [-p] NAME: no functions, no special builtins */
	while (!strcmp(name, "command") && args.n > 1 && args.v[1][0] != '-') {
		free(args.v[0]);
		memmove(args.v, args.v + 1, args.n * sizeof(char *));
		args.n--;
		name = args.v[0];
		skip_funcs = true;
	}
	const struct builtin *b = find_builtin(name);
	struct func *f = skip_funcs || strchr(name, '/') ? NULL : lookup_func(name);

	if (b && b->special && !skip_funcs) {
		/* special builtins: assignments stay, errors are fatal for scripts */
		for (size_t i = 0; i < assigns.n; i++)
			var_assign(assigns.v[i], 0);
		struct redir_save save = {0};
		bool permanent = !strcmp(name, "exec") && args.n == 1;
		if (apply_redirs(n->redirs, permanent ? NULL : &save) < 0) {
			restore_redirs(&save);
			if (!sh.interactive)
				sh_exit(1);
			st = 1;
			goto out;
		}
		st = run_builtin(b, (int)args.n, args.v);
		restore_redirs(&save);
		goto out;
	}
	if (f || b) {
		struct redir_save save = {0};
		if (apply_redirs(n->redirs, &save) < 0) {
			restore_redirs(&save);
			st = 1;
			goto out;
		}
		if (f)
			st = call_function(f, &args, &assigns);
		else {
			if (assigns.n) {
				vars_push_scope();
				for (size_t i = 0; i < assigns.n; i++) {
					char *eq = strchr(assigns.v[i], '=');
					char *nm = xstrndup(assigns.v[i], (size_t)(eq - assigns.v[i]));
					var_make_local(nm);
					var_set(nm, eq + 1, V_EXPORT);
					free(nm);
				}
			}
			st = run_builtin(b, (int)args.n, args.v);
			if (assigns.n)
				vars_pop_scope();
		}
		restore_redirs(&save);
		goto out;
	}

	char *path = find_command(name, true);
	if (!path) {
		/* still apply redirections, for their side effects and errors */
		struct redir_save save = {0};
		if (apply_redirs(n->redirs, &save) == 0)
			not_found(name);
		restore_redirs(&save);
		st = 127;
		goto out;
	}
	if (sh.interactive && !guard_rm_allows(path, sl_nullterm(&args))) {
		free(path);
		st = 1;
		goto out;
	}
	bool inplace = (flags & EV_EXIT) && !exit_trap_set();
	pid_t pid = 0;
	struct job *j = NULL;
	if (!inplace) {
		j = job_new(0, false, NULL);
		pid = fork_for(j, true);
	}
	if (pid == 0) {
		if (inplace)
			child_reset_signals();
		if (apply_redirs(n->redirs, NULL) < 0)
			_exit(1);
		for (size_t i = 0; i < assigns.n; i++)
			var_assign(assigns.v[i], V_EXPORT);
		exec_external(path, sl_nullterm(&args));
	}
	free(path);
	j->cmd = node_text(n);
	job_add_proc(j, pid, j->cmd);
	st = job_wait_fg(j);
out:
	sl_free(&args);
	sl_free(&assigns);
	return st;
}

bool exit_trap_set(void)
{
	return exit_trap != NULL;
}

static int exec_tested(struct node *n, int flags);

static int exec_pipeline(struct node *n, int flags)
{
	if (n->pipe.n == 1) {
		int st = exec_tested(n->pipe.cmds[0], flags);
		return n->pipe.bang ? !st : st;
	}
	struct job *j = job_new(0, false, NULL);
	j->cmd = node_text(n);
	int in = -1;
	for (size_t i = 0; i < n->pipe.n; i++) {
		int fds[2] = {-1, -1};
		bool last = i + 1 == n->pipe.n;
		if (!last && pipe2(fds, O_CLOEXEC) < 0) {
			if (in >= 0)
				close(in);
			sh_error("cannot make a pipe: %s", strerror(errno));
		}
		pid_t pid = fork_for(j, true);
		if (pid == 0) {
			if (in >= 0) {
				dup2(in, 0);
				close(in);
			}
			if (!last) {
				close(fds[0]);
				dup2(fds[1], 1);
				close(fds[1]);
			}
			sh_exit(exec_node(n->pipe.cmds[i], EV_EXIT));
		}
		if (in >= 0)
			close(in);
		if (!last) {
			close(fds[1]);
			in = fds[0];
		}
		char *t = node_text(n->pipe.cmds[i]);
		job_add_proc(j, pid, t);
		free(t);
	}
	int st = job_wait_fg(j);
	return n->pipe.bang ? !st : st;
}

static int exec_background(struct node *n)
{
	struct job *j = job_new(0, true, NULL);
	j->cmd = node_text(n->body);
	bool jc = sh.o_monitor && !sh.subshell;
	pid_t pid = fork_for(j, false);
	if (pid == 0) {
		if (!jc) {
			/* without job control, background jobs ignore ^C and read
			 * nothing from the terminal */
			signal(SIGINT, SIG_IGN);
			signal(SIGQUIT, SIG_IGN);
			int fd = open("/dev/null", O_RDONLY);
			if (fd >= 0 && fd != 0) {
				dup2(fd, 0);
				close(fd);
			}
		}
		sh_exit(exec_node(n->body, EV_EXIT));
	}
	job_add_proc(j, pid, j->cmd);
	sh.lastbg = pid;
	if (sh.interactive && !sh.subshell)
		fprintf(stderr, "[%d] %ld\n", j->id, (long)pid);
	return 0;
}

static int exec_subshell(struct node *n, int flags)
{
	if ((flags & EV_EXIT) && !exit_trap_set()) {
		struct redir_save save = {0};
		if (apply_redirs(n->redirs, &save) < 0)
			return 1;
		return exec_node(n->body, EV_EXIT);
	}
	struct job *j = job_new(0, false, NULL);
	j->cmd = node_text(n);
	pid_t pid = fork_for(j, true);
	if (pid == 0) {
		if (apply_redirs(n->redirs, NULL) < 0)
			_exit(1);
		sh_exit(exec_node(n->body, EV_EXIT));
	}
	job_add_proc(j, pid, j->cmd);
	return job_wait_fg(j);
}

static bool control_pending(void)
{
	return sh.breaks || sh.returning || sh.exiting;
}

/* After a loop body: handle break/continue. Returns true to leave the loop. */
static bool loop_control(void)
{
	if (sh.returning)
		return true;
	if (!sh.breaks)
		return false;
	if (sh.cont && sh.breaks == 1) {
		sh.breaks = 0;
		sh.cont = false;
		return false;
	}
	sh.breaks--;
	if (!sh.breaks)
		sh.cont = false;
	return true;
}

static int exec_loop(struct node *n, int flags)
{
	int st = 0;
	sh.loopdepth++;
	for (;;) {
		int c = exec_tested(n->loop.cond, flags);
		if (sh.breaks || sh.returning) {
			if (loop_control())
				break;
			continue;
		}
		if ((c == 0) != (n->type == N_WHILE))
			break;
		st = exec_node(n->loop.body, flags & ~EV_EXIT);
		if (loop_control())
			break;
	}
	sh.loopdepth--;
	return st;
}

static int exec_for(struct node *n, int flags)
{
	struct strlist items = {0};
	if (n->forn.has_in)
		expand_words(n->forn.words, n->forn.nwords, &items);
	else
		for (int i = 0; i < params.argc; i++)
			sl_add(&items, params.argv[i]);
	int st = 0;
	sh.loopdepth++;
	for (size_t i = 0; i < items.n; i++) {
		var_set(n->forn.var, items.v[i], 0);
		st = exec_node(n->forn.body, flags & ~EV_EXIT);
		if (loop_control())
			break;
	}
	sh.loopdepth--;
	sl_free(&items);
	return st;
}

static int exec_case(struct node *n, int flags)
{
	char *word = expand_word(n->casen.word, 0);
	int st = 0;
	bool through = false;
	for (struct caseitem *it = n->casen.items; it; it = it->next) {
		bool match = through;
		for (size_t i = 0; !match && i < it->npats; i++) {
			char *pat = expand_pattern(it->pats[i]);
			match = pattern_match(pat, word);
			free(pat);
		}
		if (!match)
			continue;
		st = it->body ? exec_node(it->body, flags) : 0;
		if (!it->fallthrough || control_pending())
			break;
		through = true;
	}
	free(word);
	return st;
}

/* A command whose status is tested: set -e does not apply inside it */
static int exec_tested(struct node *n, int flags)
{
	sh.noerrexit++;
	int st = exec_node(n, (flags & ~EV_EXIT) | EV_TESTED);
	sh.noerrexit--;
	return st;
}

int exec_node(struct node *n, int flags)
{
	if (!n)
		return 0;
	check_traps();
	if (sh.o_noexec && !sh.interactive)
		return 0;
	int st = 0;
	struct redir_save save = {0};
	bool redirs = n->redirs && n->type != N_SIMPLE && n->type != N_SUBSHELL;
	if (redirs && apply_redirs(n->redirs, &save) < 0) {
		restore_redirs(&save);
		return sh.status = 1;
	}
	switch (n->type) {
	case N_SIMPLE:
		st = exec_simple(n, flags);
		break;
	case N_PIPE:
		st = exec_pipeline(n, flags);
		break;
	case N_AND:
	case N_OR:
		st = exec_tested(n->bin.left, flags);
		if (control_pending())
			break;
		if ((st == 0) == (n->type == N_AND))
			st = exec_node(n->bin.right, flags);
		break;
	case N_NOT:
		st = !exec_tested(n->body, flags);
		break;
	case N_SEQ:
		exec_node(n->bin.left, flags & ~EV_EXIT);
		if (control_pending()) {
			st = sh.status;
			break;
		}
		st = exec_node(n->bin.right, flags);
		break;
	case N_BG:
		st = exec_background(n);
		break;
	case N_SUBSHELL:
		st = exec_subshell(n, flags);
		break;
	case N_BRACE:
		st = exec_node(n->body, redirs ? flags & ~EV_EXIT : flags);
		break;
	case N_IF:
		st = exec_tested(n->ifn.cond, flags);
		if (control_pending())
			break;
		if (st == 0)
			st = exec_node(n->ifn.then, redirs ? flags & ~EV_EXIT : flags);
		else
			st = n->ifn.els ? exec_node(n->ifn.els, redirs ? flags & ~EV_EXIT : flags) : 0;
		break;
	case N_WHILE:
	case N_UNTIL:
		st = exec_loop(n, flags);
		break;
	case N_FOR:
		st = exec_for(n, flags);
		break;
	case N_CASE:
		st = exec_case(n, redirs ? flags & ~EV_EXIT : flags);
		break;
	case N_FUNC:
		define_function(n->func.name, n->func.body, cur_arena);
		st = 0;
		break;
	}
	if (redirs)
		restore_redirs(&save);
	sh.status = st;
	if (sh.o_errexit && st != 0 && !(flags & EV_TESTED) && !sh.noerrexit && !control_pending() &&
	    (n->type == N_SIMPLE || n->type == N_SUBSHELL || (n->type == N_PIPE && !n->pipe.bang)))
		sh_exit(st);
	return st;
}

/* ---- running text ---------------------------------------------------------------------------- */
int eval_string(const char *s, const char *name)
{
	struct parser p;
	parser_init(&p, s, strlen(s), sh.lineno ? sh.lineno : 1);
	struct arena *saved_arena = cur_arena;
	cur_arena = p.arena;
	struct jmp j;
	j.prev = jmp_top;
	jmp_top = &j;
	int st = 0;
	if (setjmp(j.buf)) {
		jmp_top = j.prev;
		cur_arena = saved_arena;
		arena_unref(p.arena);
		if (p.incomplete)
			sh_warn("syntax error: unexpected end of input");
		sh_raise(sh.status ? sh.status : 2);
	}
	struct node *n;
	while ((n = parse_next(&p))) {
		st = exec_node(n, 0);
		if (control_pending())
			break;
	}
	jmp_top = j.prev;
	cur_arena = saved_arena;
	arena_unref(p.arena);
	(void)name;
	return st;
}

int source_file(const char *path, bool must_exist)
{
	size_t len;
	char *text = read_file(path, &len);
	if (!text) {
		if (must_exist)
			sh_warn("%s: %s", path, strerror(errno));
		return must_exist ? 1 : 0;
	}
	if (memchr(text, '\0', len)) {
		free(text);
		sh_warn("%s: cannot run a binary file", path);
		return 126;
	}
	char *saved_script = sh.script;
	int saved_line = sh.lineno;
	sh.script = xstrdup(path);
	sh.lineno = 1;
	sh.sourcedepth++;
	struct jmp j;
	j.prev = jmp_top;
	jmp_top = &j;
	int st;
	if (setjmp(j.buf) == 0)
		st = eval_string(text, path);
	else {
		jmp_top = j.prev;
		sh.sourcedepth--;
		free(sh.script);
		sh.script = saved_script;
		sh.lineno = saved_line;
		free(text);
		sh_raise(sh.status);
	}
	jmp_top = j.prev;
	sh.sourcedepth--;
	if (sh.returning) { /* return in the dot script itself */
		sh.returning = false;
		st = sh.status;
	}
	free(sh.script);
	sh.script = saved_script;
	sh.lineno = saved_line;
	free(text);
	return st;
}
