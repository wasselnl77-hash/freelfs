/* FreeBWS scriptlets -- a small, own implementation of the scriptlets the filter lists call
 * (the +js(name, args...) rules). FreeBWS passes this function the calls of the site being
 * loaded: [["set","a.b","undefined"], ["json-prune","adSlots"], ...]; it runs at document
 * start in the page's world, before the page's own scripts. Unknown names are ignored.
 * Written for FreeLFS (MIT); the names and argument forms follow the filter lists' syntax,
 * the code is not taken from any other project.
 * What exists: set, aopr, aopw, acs, aost, nowoif, nostif, nosiif, nano-stb, nano-sib, aeld,
 * rmnt, rpnt, ra, rc, noeval, noeval-if, no-fetch-if, no-xhr-if, json-prune,
 * json-prune-fetch-response, json-prune-xhr-response, trusted-replace-xhr-response,
 * trusted-replace-fetch-response (and the long names and trusted- aliases of those). */
function (calls) {
	'use strict';
	var W = window, D = document;
	var defProp = Object.defineProperty, gopd = Object.getOwnPropertyDescriptor;
	var _setTimeout = W.setTimeout, _setInterval = W.setInterval;
	var ALIAS = {
		'set-constant': 'set', 'trusted-set': 'set',
		'abort-on-property-read': 'aopr', 'abort-on-property-write': 'aopw',
		'abort-current-script': 'acs', 'abort-current-inline-script': 'acs', 'acis': 'acs',
		'abort-on-stack-trace': 'aost', 'addEventListener-defuser': 'aeld',
		'no-window-open-if': 'nowoif', 'window.open-defuser': 'nowoif',
		'no-setTimeout-if': 'nostif', 'setTimeout-defuser': 'nostif', 'nostif': 'nostif',
		'no-setInterval-if': 'nosiif', 'setInterval-defuser': 'nosiif',
		'nano-setTimeout-booster': 'nano-stb', 'nano-setInterval-booster': 'nano-sib',
		'remove-node-text': 'rmnt', 'replace-node-text': 'rpnt', 'trusted-replace-node-text': 'rpnt',
		'trusted-rpnt': 'rpnt', 'remove-attr': 'ra', 'remove-class': 'rc',
		'noeval': 'noeval', 'no-eval': 'noeval', 'noeval-if': 'noeval-if', 'no-eval-if': 'noeval-if',
		'trusted-replace-xhr-response': 'rpxr', 'trusted-replace-fetch-response': 'rpfr',
		'trusted-rpfr': 'rpfr', 'json-prune-fetch-response': 'jpfr', 'json-prune-xhr-response': 'jpxr'
	};

	/* ---- helpers ---- */
	function unq(s) {
		s = String(s === undefined ? '' : s);
		var c = s.charAt(0);
		if (s.length > 1 && (c === "'" || c === '"' || c === '`') && s.charAt(s.length - 1) === c) return s.slice(1, -1);
		return s;
	}
	function rx(s, flags) {	/* "/re/flags" or a literal string -> RegExp; "" -> null */
		s = unq(s);
		if (!s) return null;
		var m = /^\/(.+)\/([a-z]*)$/.exec(s);
		try {
			if (m) return new RegExp(m[1], m[2]);
			return new RegExp(s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), flags || '');
		} catch (e) { return null; }
	}
	function isObj(o) { return o !== null && (typeof o === 'object' || typeof o === 'function'); }
	function token() { return Math.random().toString(36).slice(2, 10); }
	function noop() {}
	function parseConst(v) {
		v = unq(v);
		switch (v) {
		case 'undefined': return undefined;
		case 'false': return false;
		case 'true': return true;
		case 'null': return null;
		case 'noopFunc': return function () {};
		case 'trueFunc': return function () { return true; };
		case 'falseFunc': return function () { return false; };
		case 'throwFunc': return function () { throw new Error(); };
		case 'noopPromiseResolve': return function () { return Promise.resolve(new Response('')); };
		case 'noopPromiseReject': return function () { return Promise.reject(); };
		case 'emptyArr': return [];
		case 'emptyObj': return {};
		case 'emptyStr': return '';
		case 'yes': return 'yes';
		case 'no': return 'no';
		case 'on': return 'on';
		case 'off': return 'off';
		}
		if (/^-?\d+(\.\d+)?$/.test(v)) return +v;
		return v;
	}
	/* walk a.b.c: install an accessor on the first link that is missing so that a later assignment
	 * continues the walk; at the end call leaf(owner, prop, existingDescriptor) */
	function chain(root, props, i, leaf) {
		var prop = props[i], last = i === props.length - 1;
		var d = gopd(root, prop);
		if (d && d.configurable === false) return;
		if (last) { leaf(root, prop, d); return; }
		var v = d && 'value' in d ? d.value : (d && d.get ? d.get.call(root) : undefined);
		if (isObj(v)) chain(v, props, i + 1, leaf);
		defProp(root, prop, {
			configurable: true, enumerable: d ? d.enumerable : true,
			get: function () { return v; },
			set: function (nv) { v = nv; if (isObj(nv)) chain(nv, props, i + 1, leaf); }
		});
	}
	function onChain(path, leaf) {
		var props = String(path).split('.');
		var root = W;
		if (props[0] === 'window') props.shift();
		if (props.length && props[0] === 'document') { props.shift(); root = D; }
		if (!props.length) return;
		chain(root, props, 0, leaf);
	}
	function currentText() {
		var cs = D.currentScript;
		return cs ? (cs.textContent || '') + '\n' + (cs.src || '') : null;
	}

	/* ---- property traps ---- */
	function set(path, val) {
		var c = parseConst(val);
		if (c === undefined && unq(val) !== 'undefined') return;
		onChain(path, function (o, p, d) {
			defProp(o, p, { configurable: true, enumerable: d ? d.enumerable : true,
				get: function () { return c; }, set: function () {} });
		});
	}
	function aopr(path) {
		var t = token();
		onChain(path, function (o, p, d) {
			var v = d && 'value' in d ? d.value : undefined;
			defProp(o, p, { configurable: true, get: function () { throw new ReferenceError(t); }, set: function (n) { v = n; } });
		});
	}
	function aopw(path) {
		var t = token();
		onChain(path, function (o, p, d) {
			var v = d && 'value' in d ? d.value : undefined;
			defProp(o, p, { configurable: true, get: function () { return v; }, set: function () { throw new ReferenceError(t); } });
		});
	}
	function acs(path, needle) {
		var re = rx(needle), t = token();
		function bad() {
			var tx = currentText();
			if (tx === null) return false;
			return re ? re.test(tx) : true;
		}
		onChain(path, function (o, p, d) {
			var v = d && 'value' in d ? d.value : undefined;
			defProp(o, p, { configurable: true,
				get: function () { if (bad()) throw new ReferenceError(t); return v; },
				set: function (n) { if (bad()) throw new ReferenceError(t); v = n; } });
		});
	}
	function aost(path, needle) {
		var re = rx(needle), t = token();
		function bad() { var s = new Error().stack || ''; return re ? re.test(s) : false; }
		onChain(path, function (o, p, d) {
			var v = d && 'value' in d ? d.value : undefined;
			defProp(o, p, { configurable: true,
				get: function () { if (bad()) throw new ReferenceError(t); return v; },
				set: function (n) { if (bad()) throw new ReferenceError(t); v = n; } });
		});
	}

	/* ---- timers, events, window.open ---- */
	function nowoif(pat) {
		var re = rx(pat);
		var orig = W.open;
		W.open = function (u) {
			if (!re || re.test(String(u))) return null;
			return orig.apply(this, arguments);
		};
	}
	function timerBlocker(which, needle, delay) {
		var neg = false, n = unq(needle);
		if (n.charAt(0) === '!') { neg = true; n = n.slice(1); }
		var re = rx(n), d = delay === undefined || delay === '' ? -1 : +unq(delay);
		var orig = W[which];
		W[which] = function (cb, ms) {
			var s = typeof cb === 'function' ? Function.prototype.toString.call(cb) : String(cb);
			var hit = (!re || re.test(s)) && (d < 0 || ms === d);
			if (neg) hit = !hit;
			if (hit) return 0;
			return orig.apply(this, arguments);
		};
	}
	function nostif(a, b) { timerBlocker('setTimeout', a, b); }
	function nosiif(a, b) { timerBlocker('setInterval', a, b); }
	function booster(which, needle, delay, boost) {
		var re = rx(needle), d = delay === undefined || delay === '' ? 1000 : +unq(delay);
		var b = boost === undefined || boost === '' ? 0.05 : +unq(boost);
		var orig = W[which];
		W[which] = function (cb, ms) {
			var s = typeof cb === 'function' ? Function.prototype.toString.call(cb) : String(cb);
			if ((!re || re.test(s)) && (ms === d || d === 0)) {
				arguments[1] = ms * b;
			}
			return orig.apply(this, arguments);
		};
	}
	function nanoStb(a, b, c) { booster('setTimeout', a, b, c); }
	function nanoSib(a, b, c) { booster('setInterval', a, b, c); }
	function aeld(type, pat) {
		var tre = rx(type), pre = rx(pat);
		var orig = EventTarget.prototype.addEventListener;
		EventTarget.prototype.addEventListener = function (t, h) {
			try {
				if ((!tre || tre.test(String(t))) && (!pre || pre.test(typeof h === 'function' ? Function.prototype.toString.call(h) :
					(h && typeof h.handleEvent === 'function' ? Function.prototype.toString.call(h.handleEvent) : String(h)))))
					return;
			} catch (e) {}
			return orig.apply(this, arguments);
		};
	}
	function noeval() { W.eval = noop; }
	function noevalIf(needle) {
		var re = rx(needle), orig = W.eval;
		W.eval = function (s) { if (re && re.test(String(s))) return; return orig.apply(this, arguments); };
	}

	/* ---- the DOM ---- */
	function onNodes(fn, selector) {
		function scan(root) {
			var l = root.querySelectorAll ? root.querySelectorAll(selector) : [];
			for (var i = 0; i < l.length; i++) fn(l[i]);
		}
		function go() { scan(D); }
		if (D.readyState === 'loading') D.addEventListener('DOMContentLoaded', go); else go();
		return scan;
	}
	function textNodes(nodeName, test, edit) {
		var nn = nodeName.toUpperCase();
		function handle(n) {
			if (n.nodeName !== nn && !(nn === '#TEXT' && n.nodeType === 3)) return;
			var txt = n.textContent;
			if (!txt || !test(txt)) return;
			var out = edit(txt);
			if (out !== txt) n.textContent = out;
		}
		new MutationObserver(function (ms) {
			for (var i = 0; i < ms.length; i++) {
				var a = ms[i].addedNodes;
				for (var j = 0; j < a.length; j++) handle(a[j]);
			}
		}).observe(D, { childList: true, subtree: true });
		var ex = D.querySelectorAll(nn.charAt(0) === '#' ? 'body' : nodeName);
		for (var k = 0; k < ex.length; k++) handle(ex[k]);
	}
	function rmnt(node, inc) {
		var re = rx(inc);
		textNodes(unq(node), function (t) { return !re || re.test(t); }, function () { return ''; });
	}
	function rpnt(node, pattern, repl) {
		var re = rx(pattern, 'g'), r = unq(repl);
		textNodes(unq(node), function (t) { return !re || (re.lastIndex = 0, re.test(t)); },
			function (t) { return re ? t.replace(re, r) : t; });
	}
	function stayArgs(args) { return args.indexOf('stay') >= 0; }
	function ra(attr, sel) {
		var a = unq(attr).split('|'), s = unq(sel) || '[' + a[0] + ']';
		var stay = stayArgs([].slice.call(arguments, 2));
		function run(root) {
			var l = (root || D).querySelectorAll(s);
			for (var i = 0; i < l.length; i++) for (var j = 0; j < a.length; j++) l[i].removeAttribute(a[j]);
		}
		if (D.readyState === 'loading') D.addEventListener('DOMContentLoaded', function () { run(); }); else run();
		if (stay) new MutationObserver(function () { run(); }).observe(D, { childList: true, subtree: true, attributes: true });
	}
	function rc(cls, sel) {
		var c = unq(cls).split('|'), s = unq(sel) || '.' + c[0];
		var stay = stayArgs([].slice.call(arguments, 2));
		function run() {
			var l = D.querySelectorAll(s);
			for (var i = 0; i < l.length; i++) for (var j = 0; j < c.length; j++) l[i].classList.remove(c[j]);
		}
		if (D.readyState === 'loading') D.addEventListener('DOMContentLoaded', run); else run();
		if (stay) new MutationObserver(run).observe(D, { childList: true, subtree: true, attributes: true });
	}

	/* ---- JSON and network ---- */
	function prunePath(o, parts, i) {
		if (!isObj(o) || i >= parts.length) return;
		var k = parts[i], last = i === parts.length - 1;
		if (k === '*' || k === '[]' || k === '[-]') {
			var keys = Array.isArray(o) ? o.map(function (_, n) { return n; }) : Object.keys(o);
			for (var j = 0; j < keys.length; j++) {
				if (last) { try { delete o[keys[j]]; } catch (e) {} } else prunePath(o[keys[j]], parts, i + 1);
			}
			return;
		}
		if (last) { try { delete o[k]; } catch (e) {} return; }
		if (Array.isArray(o) && !(k in o)) {	/* a.b.c over an array: apply to the elements */
			for (var m = 0; m < o.length; m++) prunePath(o[m], parts, i);
			return;
		}
		prunePath(o[k], parts, i + 1);
	}
	function hasPath(o, parts, i) {
		if (!isObj(o)) return false;
		if (i >= parts.length) return true;
		var k = parts[i];
		if (k === '*' || k === '[]' || k === '[-]') {
			var ks = Object.keys(o);
			for (var j = 0; j < ks.length; j++) if (hasPath(o[ks[j]], parts, i + 1)) return true;
			return false;
		}
		return k in o && hasPath(o[k], parts, i + 1);
	}
	function pruner(paths, needles) {
		var pp = unq(paths).split(/\s+/).filter(Boolean).map(function (p) { return p.split('.'); });
		var np = unq(needles).split(/\s+/).filter(Boolean).map(function (p) { return p.split('.'); });
		return function (o) {
			if (!isObj(o) || !pp.length) return o;
			if (np.length) for (var i = 0; i < np.length; i++) if (!hasPath(o, np[i], 0)) return o;
			for (var j = 0; j < pp.length; j++) prunePath(o, pp[j], 0);
			return o;
		};
	}
	function jsonPrune(paths, needles) {
		var prune = pruner(paths, needles);
		var oparse = JSON.parse;
		JSON.parse = function () { return prune(oparse.apply(this, arguments)); };
		var ojson = W.Response && Response.prototype.json;
		if (ojson) Response.prototype.json = function () { return ojson.apply(this, arguments).then(prune); };
	}
	function urlMatcher(args) {
		/* [..., 'propsToMatch', spec] or just spec: "url:pattern method:GET" or a bare pattern */
		var i = args.indexOf('propsToMatch'), spec = i >= 0 ? args[i + 1] : '';
		spec = unq(spec);
		if (!spec) return function () { return true; };
		var m = /^(?:url:)?(.*)$/.exec(spec.split(/\s+(?=method:)/)[0]);
		var re = rx(m ? m[1] : spec);
		return function (u) { return !re || re.test(String(u)); };
	}
	function jpfr(paths, needles) {
		var args = [].slice.call(arguments, 2), prune = pruner(paths, needles), match = urlMatcher(args);
		var of = W.fetch;
		W.fetch = function (inp) {
			var u = typeof inp === 'string' ? inp : (inp && inp.url) || '';
			var p = of.apply(this, arguments);
			if (!match(u)) return p;
			return p.then(function (r) {
				return r.clone().text().then(function (t) {
					var o; try { o = JSON.parse(t); } catch (e) { return r; }
					try { o = prune(o); } catch (e) { return r; }
					return new Response(JSON.stringify(o), { status: r.status, statusText: r.statusText, headers: r.headers });
				}).catch(function () { return r; });
			});
		};
	}
	function xhrEdit(match, edit) {
		var oo = XMLHttpRequest.prototype.open;
		XMLHttpRequest.prototype.open = function (m, u) {
			this.__fbUrl = String(u);
			if (match(this.__fbUrl)) {
				var self = this, cache;
				function done() {
					if (cache !== undefined) return cache;
					var t = self.__fbNativeText();
					try { cache = edit(t); } catch (e) { cache = t; }
					return cache;
				}
				var dr = gopd(XMLHttpRequest.prototype, 'response'), dt = gopd(XMLHttpRequest.prototype, 'responseText');
				this.__fbNativeText = function () { return dt.get.call(self); };
				defProp(this, 'responseText', { configurable: true, get: function () { return self.readyState === 4 ? done() : dt.get.call(self); } });
				defProp(this, 'response', { configurable: true, get: function () {
					if (self.readyState !== 4 || (self.responseType && self.responseType !== 'text' && self.responseType !== 'json')) return dr.get.call(self);
					var v = done();
					if (self.responseType === 'json') { try { return JSON.parse(v); } catch (e) { return null; } }
					return v;
				} });
			}
			return oo.apply(this, arguments);
		};
	}
	function jpxr(paths, needles) {
		var args = [].slice.call(arguments, 2), prune = pruner(paths, needles);
		xhrEdit(urlMatcher(args), function (t) {
			var o = JSON.parse(t);
			return JSON.stringify(prune(o));
		});
	}
	function rpxr(pattern, repl) {
		var args = [].slice.call(arguments, 2), re = rx(pattern, 'g'), r = unq(repl);
		if (!re) return;
		xhrEdit(urlMatcher(args), function (t) { re.lastIndex = 0; return t.replace(re, r); });
	}
	function rpfr(pattern, repl) {
		var args = [].slice.call(arguments, 2), re = rx(pattern, 'g'), r = unq(repl), match = urlMatcher(args);
		if (!re) return;
		var of = W.fetch;
		W.fetch = function (inp) {
			var u = typeof inp === 'string' ? inp : (inp && inp.url) || '';
			var p = of.apply(this, arguments);
			if (!match(u)) return p;
			return p.then(function (resp) {
				return resp.clone().text().then(function (t) {
					re.lastIndex = 0;
					return new Response(t.replace(re, r), { status: resp.status, statusText: resp.statusText, headers: resp.headers });
				}).catch(function () { return resp; });
			});
		};
	}
	function pairs(spec) {
		var o = {}, parts = unq(spec).split(/\s+/);
		for (var i = 0; i < parts.length; i++) {
			var m = /^([a-z]+):(.*)$/.exec(parts[i]);
			if (m) o[m[1]] = m[2]; else if (parts[i]) o.url = parts[i];
		}
		return o;
	}
	function noFetchIf(spec) {
		var p = pairs(spec), re = rx(p.url || ''), of = W.fetch;
		W.fetch = function (inp, init) {
			var u = typeof inp === 'string' ? inp : (inp && inp.url) || '';
			var meth = (init && init.method) || (inp && inp.method) || 'GET';
			if ((!re || re.test(u)) && (!p.method || p.method.toUpperCase() === meth.toUpperCase()))
				return Promise.resolve(new Response('', { status: 200 }));
			return of.apply(this, arguments);
		};
	}
	function noXhrIf(spec) {
		var p = pairs(spec), re = rx(p.url || '');
		var oo = XMLHttpRequest.prototype.open, os = XMLHttpRequest.prototype.send;
		XMLHttpRequest.prototype.open = function (m, u) {
			this.__fbBlock = (!re || re.test(String(u))) && (!p.method || p.method.toUpperCase() === String(m).toUpperCase());
			return oo.apply(this, arguments);
		};
		XMLHttpRequest.prototype.send = function () {
			if (this.__fbBlock) {
				var x = this;
				defProp(x, 'readyState', { value: 4 });
				defProp(x, 'status', { value: 200 });
				defProp(x, 'responseText', { value: '' });
				defProp(x, 'response', { value: '' });
				_setTimeout(function () {
					try { x.dispatchEvent(new Event('readystatechange')); x.dispatchEvent(new Event('load')); x.dispatchEvent(new Event('loadend')); } catch (e) {}
				}, 1);
				return;
			}
			return os.apply(this, arguments);
		};
	}

	var LIB = {
		'set': set, 'aopr': aopr, 'aopw': aopw, 'acs': acs, 'aost': aost, 'nowoif': nowoif,
		'nostif': nostif, 'nosiif': nosiif, 'nano-stb': nanoStb, 'nano-sib': nanoSib, 'aeld': aeld,
		'rmnt': rmnt, 'rpnt': rpnt, 'ra': ra, 'rc': rc, 'noeval': noeval, 'noeval-if': noevalIf,
		'no-fetch-if': noFetchIf, 'no-xhr-if': noXhrIf, 'json-prune': jsonPrune,
		'jpfr': jpfr, 'jpxr': jpxr, 'rpxr': rpxr, 'rpfr': rpfr
	};
	for (var i = 0; i < calls.length; i++) {
		var c = calls[i], name = ALIAS[c[0]] || c[0], f = LIB[name];
		if (!f) continue;
		try { f.apply(null, c.slice(1)); } catch (e) {}
	}
}
