/* What the signed index may claim and which keys count: freshness (serial,
 * valid-until), the list of trusted keys, and key rotation.
 *
 * Freshness. The signed trusted comment of index.zst.sig carries
 *     serial:N  valid-until:2026-12-01T00:00:00Z
 * (the minisign global signature covers it, so it cannot be edited or moved
 * to another index). 'flpkg sync' refuses an index that is expired or whose
 * serial is lower than the last one this machine accepted (kept in
 * <root>/var/lib/flpkg/index-serial.REPO). Expiry stops a mirror or
 * man-in-the-middle from freezing a machine on an old index, which would
 * hide security updates; the serial stops it from replaying an old, still
 * unexpired one.
 *
 * Keys. A machine trusts the *.pub files of the keys directory (flpkg.conf
 * "keys", /etc/flpkg/keys) plus the keys it learned from a rotation, kept in
 * <root>/var/lib/flpkg/keys/. A rotation statement is the file rotate.pub
 * (the new public key) with rotate.pub.sig, signed by a key that is trusted
 * now ('flpkg key introduce'); its signed comment names the old key
 * (rotate-from), the new key (rotate-to) and the day the old key stops being
 * trusted (retire-old). From the first sync that sees it, both keys are
 * trusted; after retire-old only the new one is. Retirements are kept in
 * keys/retire.list ("KEYID EPOCH").
 * SPDX-License-Identifier: MIT */
#include "db.h"
#include "sign.h"

#include <errno.h>
#include <getopt.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- time ----------------------------------------------------------------- */

bool iso_parse(const char *s, time_t *out)
{
	struct tm tm = {0};
	const char *end = strptime(s, "%Y-%m-%dT%H:%M:%SZ", &tm);
	if (!end || *end)
		return false;
	*out = timegm(&tm);
	return true;
}

void iso_format(time_t t, char out[32])
{
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

char *trusted_field(const char *trusted, const char *name)
{
	char *copy = xstrdup(trusted), *save = NULL, *found = NULL;
	size_t nl = strlen(name);
	for (char *f = strtok_r(copy, "\t", &save); f && !found; f = strtok_r(NULL, "\t", &save))
		if (!strncmp(f, name, nl) && f[nl] == ':')
			found = xstrdup(f + nl + 1);
	free(copy);
	return found;
}

/* ---- state -------------------------------------------------------------------- */

static char *scratch; /* learned keys during check-updates, which must not write the system */

void trust_set_scratch(const char *dir)
{
	free(scratch);
	scratch = dir ? xstrdup(dir) : NULL;
}

/* msg() goes to stdout; check-updates keeps stdout for its machine-readable list */
#define note(...) (scratch ? (void)(fputs(":: ", stderr), fprintf(stderr, __VA_ARGS__), fputc('\n', stderr)) : msg(__VA_ARGS__))

static char *state_path(const char *sub)
{
	char *d = db_dir(cfg.root), *p = path_join(d, sub);
	free(d);
	return p;
}

/* A repo name as a file name part. */
static char *safe_name(const char *s)
{
	char *r = xstrdup(s);
	for (char *p = r; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		      *p == '.' || *p == '-' || *p == '_'))
			*p = '_';
	return r;
}

static int write_atomic(const char *path, const char *text)
{
	char *tmp = xasprintf("%s.new", path);
	int rc = write_file(tmp, text, strlen(text), 0644);
	if (!rc)
		rc = rename(tmp, path);
	if (rc)
		unlink(tmp);
	free(tmp);
	return rc;
}

/* ---- index freshness ---------------------------------------------------------- */

static long long serial_parse(const char *s)
{
	char *end;
	errno = 0;
	long long v = strtoll(s, &end, 10);
	return errno || end == s || *end || v < 0 ? -1 : v;
}

static long long serial_last(const char *repo, char **path)
{
	char *n = safe_name(repo), *sub = xasprintf("index-serial.%s", n);
	*path = state_path(sub);
	free(n);
	free(sub);
	char *text = read_file(*path, NULL);
	long long v = text ? serial_parse(trim(text)) : -1;
	free(text);
	return v;
}

long long index_check(const char *repo, const char *trusted, char **err)
{
	char *s_serial = trusted_field(trusted, "serial");
	char *s_until = trusted_field(trusted, "valid-until");
	long long serial = s_serial ? serial_parse(s_serial) : -1;
	time_t until = 0, now = time(NULL);
	long long rc = -1;
	char *spath = NULL;
	if (serial < 0 || !s_until || !iso_parse(s_until, &until)) {
		*err = xasprintf("repo %s: the signed index has no valid serial and valid-until, so "
		                 "its freshness cannot be checked. The repository was made by an "
		                 "older flpkg: ask its owner to republish it with the current "
		                 "'flpkg repo add' (build/70-repo.sh).", repo);
	} else if (until <= now) {
		char u[32], n[32];
		iso_format(until, u);
		iso_format(now, n);
		*err = xasprintf("repo %s: the signed index EXPIRED on %s (now %s). Nothing was "
		                 "changed. The repository owner must re-sign and republish it "
		                 "(build/70-repo.sh); until then updates cannot be trusted, because "
		                 "an old index could hide security fixes. If the repository is "
		                 "current and only this machine's clock is wrong, set the date and "
		                 "run 'flpkg sync' again.", repo, u, n);
	} else {
		long long last = serial_last(repo, &spath);
		if (last >= 0 && serial < last) {
			*err = xasprintf("repo %s: ROLLBACK refused: the index has serial %lld but this "
			                 "machine already accepted serial %lld. The server (or something "
			                 "between you and it) is offering an older repository state. Do "
			                 "not install from it; try again later or from another mirror. If "
			                 "the owner really restored an old repository, they must publish "
			                 "a serial above %lld; as a last resort, delete %s after "
			                 "checking with them.", repo, serial, last, last, spath);
		} else {
			rc = serial;
		}
	}
	free(spath);
	free(s_serial);
	free(s_until);
	return rc;
}

void index_commit(const char *repo, long long serial)
{
	char *spath, *dir = db_dir(cfg.root);
	long long last = serial_last(repo, &spath);
	if (last != serial) {
		char *text = xasprintf("%lld\n", serial);
		if (mkdirs(dir, 0755) < 0 || write_atomic(spath, text) < 0)
			die("cannot remember the index serial in %s: %s", spath, strerror(errno));
		free(text);
	}
	free(spath);
	free(dir);
}

void index_warn_cached(const char *repo, const char *sigpath)
{
	char *text = read_file(sigpath, NULL);
	if (!text)
		return;
	char *save = NULL, *line = strtok_r(text, "\n", &save);
	for (int i = 0; line && i < 2; i++)
		line = strtok_r(NULL, "\n", &save);
	if (line && has_prefix(line, "trusted comment: ")) {
		char *u = trusted_field(line + strlen("trusted comment: "), "valid-until");
		time_t until;
		if (u && iso_parse(u, &until) && until <= time(NULL))
			warn("repo %s: the index you have EXPIRED on %s; run 'flpkg sync' before trusting "
			     "what it says is the newest version", repo, u);
		free(u);
	}
	free(text);
}

long long index_next_serial(const char *dir)
{
	long long next = (long long)time(NULL);
	char *sig = path_join(dir, "index.zst.sig"), *text = read_file(sig, NULL);
	free(sig);
	if (text) {
		char *save = NULL, *line = strtok_r(text, "\n", &save);
		for (int i = 0; line && i < 2; i++)
			line = strtok_r(NULL, "\n", &save);
		if (line && has_prefix(line, "trusted comment: ")) {
			char *s = trusted_field(line + strlen("trusted comment: "), "serial");
			long long prev = s ? serial_parse(s) : -1;
			if (prev >= next)
				next = prev + 1;
			free(s);
		}
		free(text);
	}
	return next;
}

/* ---- the key list ---------------------------------------------------------------- */

struct retire {
	char id[17];
	time_t when;
};

struct retirelist {
	struct retire *v;
	size_t n;
};

static void retire_load(const char *file, struct retirelist *rl)
{
	char *text = read_file(file, NULL), *save = NULL;
	if (!text)
		return;
	for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char id[32];
		long long when;
		if (sscanf(l, "%31s %lld", id, &when) != 2 || strlen(id) != 16)
			continue;
		rl->v = xrealloc(rl->v, (rl->n + 1) * sizeof(*rl->v));
		memcpy(rl->v[rl->n].id, id, 17);
		rl->v[rl->n++].when = (time_t)when;
	}
	free(text);
}

static const struct retire *retire_find(const struct retirelist *rl, const char *id)
{
	const struct retire *best = NULL;
	for (size_t i = 0; i < rl->n; i++)
		if (!strcmp(rl->v[i].id, id) && (!best || rl->v[i].when < best->when))
			best = &rl->v[i];
	return best;
}

static void retire_all(struct retirelist *rl)
{
	char *p = state_path("keys/retire.list");
	retire_load(p, rl);
	free(p);
	if (scratch) {
		p = path_join(scratch, "keys/retire.list");
		retire_load(p, rl);
		free(p);
	}
}

static void keys_load_all(struct pubkey **keys, size_t *n)
{
	char *dirs[3] = {xstrdup(cfg.keydir), state_path("keys"), scratch ? path_join(scratch, "keys") : NULL};
	*keys = NULL;
	*n = 0;
	for (int d = 0; d < 3; d++) {
		struct pubkey *v;
		size_t m;
		if (dirs[d])
			pubkeys_load(dirs[d], &v, &m);
		else
			continue;
		for (size_t i = 0; i < m; i++) {
			bool dup = false;
			for (size_t j = 0; j < *n && !dup; j++)
				dup = !memcmp((*keys)[j].keynum, v[i].keynum, 8) &&
				      !memcmp((*keys)[j].pk, v[i].pk, 32);
			if (dup) {
				free(v[i].path);
				continue;
			}
			*keys = xrealloc(*keys, (*n + 1) * sizeof(**keys));
			(*keys)[(*n)++] = v[i];
		}
		free(v);
		free(dirs[d]);
	}
}

void keys_effective(struct pubkey **keys, size_t *n)
{
	struct retirelist rl = {0};
	retire_all(&rl);
	keys_load_all(keys, n);
	time_t now = time(NULL);
	size_t out = 0;
	for (size_t i = 0; i < *n; i++) {
		char id[17];
		keyid_hex((*keys)[i].keynum, id);
		const struct retire *r = retire_find(&rl, id);
		if (r && r->when <= now) {
			free((*keys)[i].path);
			continue;
		}
		(*keys)[out++] = (*keys)[i];
	}
	*n = out;
	free(rl.v);
}

char *keys_retired_hint(void)
{
	struct retirelist rl = {0};
	retire_all(&rl);
	time_t now = time(NULL);
	char *hint = xstrdup("");
	for (size_t i = 0; i < rl.n; i++) {
		if (rl.v[i].when > now)
			continue;
		char when[32];
		iso_format(rl.v[i].when, when);
		char *h = xasprintf("%s%s key %s was retired on %s by a key rotation; if the repo is "
		                    "signed by a key you do not have, install the newest flpkg package "
		                    "or add the new public key to the keys directory.",
		                    hint, *hint ? "" : "\n", rl.v[i].id, when);
		free(hint);
		hint = h;
	}
	free(rl.v);
	return hint;
}

int cmd_key_list(void)
{
	struct retirelist rl = {0};
	struct pubkey *keys;
	size_t n;
	retire_all(&rl);
	keys_load_all(&keys, &n);
	time_t now = time(NULL);
	for (size_t i = 0; i < n; i++) {
		char id[17], when[32];
		keyid_hex(keys[i].keynum, id);
		const struct retire *r = retire_find(&rl, id);
		if (!r) {
			printf("%s  trusted            %s\n", id, keys[i].path);
		} else {
			iso_format(r->when, when);
			printf("%s  %s %s  %s\n", id, r->when <= now ? "RETIRED since   " : "trusted until   ",
			       when, keys[i].path);
		}
	}
	if (!n)
		fprintf(stderr, "%s: no keys in %s\n", progname, cfg.keydir);
	pubkeys_free(keys, n);
	free(rl.v);
	return 0;
}

/* ---- rotation ---------------------------------------------------------------------- */

static bool has_key(const struct pubkey *keys, size_t n, const unsigned char keynum[8])
{
	for (size_t i = 0; i < n; i++)
		if (!memcmp(keys[i].keynum, keynum, 8))
			return true;
	return false;
}

void rotate_apply(const char *repo, const char *pubfile, const char *sigfile)
{
	struct pubkey *keys, nk;
	size_t n;
	char *err = NULL, *trusted = NULL, *from = NULL, *to = NULL, *ret = NULL;
	unsigned char signer[8];
	keys_effective(&keys, &n);
	if (pubkey_load_file(pubfile, &nk, &err) < 0) {
		warn("repo %s: ignoring its key rotation file: %s", repo, err);
		goto out;
	}
	bool have = has_key(keys, n, nk.keynum);
	if (verify_file_ex(keys, n, pubfile, sigfile, "rotate.pub", &err, &trusted, signer) < 0) {
		/* a statement signed by a key that has been retired since is stale, not an attack */
		if (!have || verbose)
			warn("repo %s offers a new signing key, but it is not signed by a key you trust, "
			     "so it is ignored: %s", repo, err);
		goto out;
	}
	char idnew[17], idold[17];
	keyid_hex(nk.keynum, idnew);
	keyid_hex(signer, idold);
	from = trusted_field(trusted, "rotate-from");
	to = trusted_field(trusted, "rotate-to");
	ret = trusted_field(trusted, "retire-old");
	time_t when;
	if (!from || !to || !ret || strcmp(from, idold) || strcmp(to, idnew) || !iso_parse(ret, &when) ||
	    !strcmp(idnew, idold)) {
		warn("repo %s: the key rotation statement is malformed, ignoring it", repo);
		goto out;
	}

	char *dir = scratch ? path_join(scratch, "keys") : state_path("keys");
	if (mkdirs(dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));
	if (!have) {
		char *text = read_file(pubfile, NULL);
		char *name = xasprintf("%s.pub", idnew), *dest = path_join(dir, name);
		if (!text || write_atomic(dest, text) < 0)
			die("cannot save the new key in %s: %s", dest, strerror(errno));
		note("repo %s: now also trusting the new signing key %s (introduced by %s); %s stops "
		    "being trusted on %s", repo, idnew, idold, idold, ret);
		free(text);
		free(name);
		free(dest);
	}
	char *rpath = path_join(dir, "retire.list");
	struct retirelist rl = {0};
	retire_load(rpath, &rl);
	const struct retire *have_r = retire_find(&rl, idold);
	if (!have_r || have_r->when > when) {
		struct strbuf sb = {0};
		for (size_t i = 0; i < rl.n; i++)
			if (strcmp(rl.v[i].id, idold)) {
				char *l = xasprintf("%s %lld\n", rl.v[i].id, (long long)rl.v[i].when);
				sb_addstr(&sb, l);
				free(l);
			}
		char *l = xasprintf("%s %lld\n", idold, (long long)when);
		sb_addstr(&sb, l);
		free(l);
		if (write_atomic(rpath, sb.buf) < 0)
			die("cannot write %s: %s", rpath, strerror(errno));
		sb_free(&sb);
		if (have)
			note("repo %s: the old key %s stops being trusted on %s", repo, idold, ret);
	}
	free(rl.v);
	free(rpath);
	free(dir);
out:
	free(err);
	free(trusted);
	free(from);
	free(to);
	free(ret);
	pubkeys_free(keys, n);
}

/* flpkg key introduce --key OLD.key --new NEW.pub [--retire-days N] DIR
 * writes DIR/rotate.pub and DIR/rotate.pub.sig. Put them next to index.zst. */
int cmd_key_introduce(int argc, char **argv)
{
	static const struct option longopts[] = {{"key", required_argument, NULL, 'k'},
	                                         {"new", required_argument, NULL, 'n'},
	                                         {"retire-days", required_argument, NULL, 'd'},
	                                         {0}};
	const char *oldkey = cfg.signkey, *newpub = NULL;
	long days = 60;
	int c;
	optind = 2;
	while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (c) {
		case 'k': oldkey = optarg; break;
		case 'n': newpub = optarg; break;
		case 'd': days = strtol(optarg, NULL, 10); break;
		default: return 2;
		}
	}
	if (!oldkey || !newpub || optind + 1 != argc)
		die("usage: flpkg key introduce --key OLD.key --new NEW.pub [--retire-days N] DIR\n"
		    "  DIR/rotate.pub and rotate.pub.sig are signed by the OLD (currently trusted) key;\n"
		    "  clients trust both keys from their next sync until the old key retires\n"
		    "  (default 60 days; a negative number retires it at once: tests only).");
	struct seckey old;
	struct pubkey np;
	char *err = NULL;
	if (seckey_load(oldkey, &old, &err) < 0 || pubkey_load_file(newpub, &np, &err) < 0)
		die("%s", err);
	if (!memcmp(old.keynum, np.keynum, 8))
		die("the new key is the old key");
	if (mkdirs(argv[optind], 0755) < 0)
		die("cannot create %s: %s", argv[optind], strerror(errno));
	char *text = read_file(newpub, NULL), *dest = path_join(argv[optind], "rotate.pub");
	if (!text || write_file(dest, text, strlen(text), 0644) < 0)
		die("cannot write %s: %s", dest, strerror(errno));
	char idold[17], idnew[17], when[32];
	keyid_hex(old.keynum, idold);
	keyid_hex(np.keynum, idnew);
	time_t retire = time(NULL) + (time_t)days * 86400;
	iso_format(retire, when);
	char *extra = xasprintf("rotate-from:%s\trotate-to:%s\tretire-old:%s", idold, idnew, when);
	if (sign_file_ex(&old, dest, extra, &err) < 0)
		die("%s", err);
	OPENSSL_cleanse(&old, sizeof(old));
	msg("%s: key %s introduces %s; clients trust both until %s, then only %s", dest, idold,
	    idnew, when, idnew);
	free(extra);
	free(dest);
	free(text);
	return 0;
}
