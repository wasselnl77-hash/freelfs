/* flsh -- the FreeLFS shell: startup, the interactive loop, scripts.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static void usage(FILE *f)
{
	fprintf(f, "usage: flsh [-ilsc] [-euxfnvCa] [-o OPTION] [SCRIPT [ARG...] | -c COMMAND [NAME [ARG...]]]\n\n"
	           "  -c COMMAND   run COMMAND and exit\n"
	           "  -s           read commands from standard input\n"
	           "  -i           interactive, even without a terminal\n"
	           "  -l, --login  a login shell: read /etc/profile and ~/.profile\n"
	           "  --version    print the version\n\n"
	           "Interactive shells read /etc/flshrc and ~/.config/flsh/flshrc. Type help in flsh for\n"
	           "the builtins and keys.\n");
}

/* Run the commands in text, one complete command at a time. */
static int run_text(const char *text, size_t len)
{
	struct parser p;
	parser_init(&p, text, len, 1);
	struct arena *a = p.arena;
	cur_arena = a;
	for (;;) {
		struct jmp j;
		j.prev = NULL;
		jmp_top = &j;
		if (setjmp(j.buf)) {
			jmp_top = NULL;
			if (!sh.interactive) {
				if (p.incomplete)
					sh_warn("syntax error: unexpected end of file");
				sh_exit(sh.status ? sh.status : 2);
			}
			break; /* the rest of the line is skipped */
		}
		size_t start = p.pos;
		struct node *n = parse_next(&p);
		if (!n)
			break;
		if (sh.o_verbose)
			write_all(2, p.src + start, p.pos - start);
		/* the last command of a non-interactive shell can replace it */
		int flags = 0;
		if (!sh.interactive && sh.exiting_after) {
			size_t k = p.pos;
			while (k < p.len && (p.src[k] == ' ' || p.src[k] == '\t' || p.src[k] == '\n'))
				k++;
			if (k == p.len)
				flags = EV_EXIT;
		}
		exec_node(n, flags);
	}
	jmp_top = NULL;
	cur_arena = NULL;
	arena_unref(a);
	return sh.status;
}

static void source_quiet(const char *path)
{
	if (access(path, R_OK) < 0)
		return;
	struct jmp j;
	j.prev = NULL;
	jmp_top = &j;
	if (setjmp(j.buf) == 0)
		source_file(path, false);
	jmp_top = NULL;
}

static void run_profile(void)
{
	source_quiet("/etc/profile");
	const char *home = var_get("HOME");
	if (home) {
		char *p = xasprintf("%s/.profile", home);
		source_quiet(p);
		free(p);
	}
}

static void run_rc(void)
{
	source_quiet("/etc/flshrc");
	const char *xdg = var_get("XDG_CONFIG_HOME");
	const char *home = var_get("HOME");
	char *p = NULL;
	if (xdg && *xdg)
		p = xasprintf("%s/flsh/flshrc", xdg);
	else if (home)
		p = xasprintf("%s/.config/flsh/flshrc", home);
	if (p)
		source_quiet(p);
	free(p);
}

static bool blank(const char *s)
{
	for (; *s; s++)
		if (*s != ' ' && *s != '\t' && *s != '\n')
			return false;
	return true;
}

static void repl(void)
{
	bool eof_warned = false;
	for (;;) {
		job_notify(false);
		int_pending = 0;
		char *prompt = make_prompt();
		char *line = edit_line(prompt, false);
		free(prompt);
		if (!line) {
			if (sh.o_ignoreeof) {
				fprintf(stderr, "Use exit to leave the shell.\n");
				continue;
			}
			if (job_count(true) && !eof_warned) {
				eof_warned = true;
				fprintf(stderr, "There are stopped jobs. Press Ctrl-D again to end them and leave.\n");
				continue;
			}
			fprintf(stderr, "exit\n");
			break;
		}
		if (edit_interrupted) {
			free(line);
			sh.status = 130;
			prompt_command_finished(130);
			continue;
		}
		struct strbuf cmd = {0};
		sb_addstr(&cmd, line);
		free(line);
		bool aborted = false;
		while (!parse_is_complete(cmd.buf)) {
			char *more = edit_line(ps2_prompt(), true);
			if (!more || edit_interrupted) {
				if (!more)
					sh_warn("syntax error: unexpected end of file");
				aborted = true;
				free(more);
				break;
			}
			sb_addc(&cmd, '\n');
			sb_addstr(&cmd, more);
			free(more);
		}
		if (aborted || blank(cmd.buf)) {
			if (aborted) {
				sh.status = edit_interrupted ? 130 : 2;
				prompt_command_finished(sh.status);
			}
			sb_free(&cmd);
			continue;
		}
		hist_add(cmd.buf);
		prompt_command_started();
		run_text(cmd.buf, cmd.len);
		prompt_command_finished(sh.status);
		sb_free(&cmd);
	}
}

static void read_stdin(void)
{
	struct strbuf acc = {0};
	for (;;) {
		char *line = read_line_plain();
		if (!line) {
			if (acc.len)
				run_text(acc.buf, acc.len);
			break;
		}
		sb_addstr(&acc, line);
		sb_addc(&acc, '\n');
		free(line);
		if (!parse_is_complete(acc.buf))
			continue;
		run_text(acc.buf, acc.len);
		acc.len = 0;
		acc.buf[0] = '\0';
	}
	sb_free(&acc);
}

static const char default_rules[] = "complete cd -d\n"
                                    "complete pushd -d\n"
                                    "complete rmdir -d\n"
                                    "complete type -C\n"
                                    "complete command -C\n"
                                    "complete which -C\n"
                                    "complete man -C\n";

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	sh.pid = getpid();
	signals_init();
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = '\0';
		self_exe = xstrdup(exe);
	}
	vars_init(environ);
	bool opt_c = false, opt_s = false, opt_i = false, opt_l = argv[0][0] == '-';
	int i = 1;
	for (; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--version")) {
			printf("flsh %s\n", FLSH_VERSION);
			return 0;
		}
		if (!strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		}
		if (!strcmp(a, "--login")) {
			opt_l = true;
			continue;
		}
		if (!strcmp(a, "--")) {
			i++;
			break;
		}
		if ((a[0] != '-' && a[0] != '+') || !a[1])
			break;
		bool on = a[0] == '-';
		for (const char *p = a + 1; *p; p++) {
			switch (*p) {
			case 'c': opt_c = true; break;
			case 's': opt_s = true; break;
			case 'i': opt_i = true; break;
			case 'l': opt_l = true; break;
			case 'o':
				if (i + 1 >= argc || set_option_name(argv[i + 1], on) < 0) {
					fprintf(stderr, "flsh: -o %s: unknown option\n", i + 1 < argc ? argv[i + 1] : "");
					return 2;
				}
				i++;
				break;
			default:
				if (set_option_letter(*p, on) < 0) {
					fprintf(stderr, "flsh: -%c: unknown option\n", *p);
					usage(stderr);
					return 2;
				}
			}
		}
	}
	sh.login = opt_l;

	if (opt_c) {
		if (i >= argc) {
			fprintf(stderr, "flsh: -c needs a command\n");
			return 2;
		}
		const char *cmd = argv[i++];
		arg0 = xstrdup(i < argc ? argv[i++] : argv[0]);
		params_set(argv + i, argc - i);
		sh.interactive = opt_i;
		if (opt_l)
			run_profile();
		sh.exiting_after = true;
		run_text(cmd, strlen(cmd));
		sh_exit(sh.status);
	}
	if (!opt_s && i < argc) {
		const char *script = argv[i];
		size_t len;
		char *text = read_file(script, &len);
		if (!text) {
			fprintf(stderr, "flsh: %s: %s\n", script, strerror(errno));
			return errno == ENOENT ? 127 : 126;
		}
		arg0 = xstrdup(script);
		sh.script = xstrdup(script);
		params_set(argv + i + 1, argc - i - 1);
		if (opt_l)
			run_profile();
		sh.exiting_after = true;
		run_text(text, len);
		free(text);
		sh_exit(sh.status);
	}
	arg0 = xstrdup(argv[0]);
	params_set(argv + i, argc - i);
	sh.interactive = opt_i || (isatty(0) && isatty(2));
	sh.hints = sh.interactive;
	if (sh.interactive) {
		const char *term = var_get("TERM");
		use_color = !getenv("NO_COLOR") && isatty(2) && !(term && !strcmp(term, "dumb"));
		interactive_signals();
		if (isatty(0))
			jobs_init();
		edit_init();
		hist_init();
		eval_string(default_rules, NULL);
		if (!var_get("PWD")) {
			char cwd[4096];
			if (getcwd(cwd, sizeof(cwd)))
				var_set("PWD", cwd, V_EXPORT);
		}
	}
	if (opt_l)
		run_profile();
	if (sh.interactive) {
		run_rc();
		repl();
	} else
		read_stdin();
	sh_exit(sh.status);
}
