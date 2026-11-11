/* FreeBWS internal pages: the bridge to the browser (see pages.c) and small helpers. */
var fb = (function () {
	var n = 0, pend = {};
	document.addEventListener('fb-reply', function (e) {
		var d = JSON.parse(e.detail), p = pend[d.id];
		if (!p) return;
		delete pend[d.id];
		var r = null;
		try { r = JSON.parse(d.reply); } catch (x) {}
		p(r);
	});
	return function (cmd, args) {
		return new Promise(function (res) {
			var id = ++n, msg = { cmd: cmd };
			for (var k in args || {}) msg[k] = args[k];
			pend[id] = res;
			document.dispatchEvent(new CustomEvent('fb-call', { detail: JSON.stringify({ id: id, msg: msg }) }));
		});
	};
})();
function $(s, r) { return (r || document).querySelector(s); }
function el(tag, attrs, kids) {
	var e = document.createElement(tag);
	for (var k in attrs || {}) {
		if (k === 'text') e.textContent = attrs[k];
		else if (k.slice(0, 2) === 'on') e.addEventListener(k.slice(2), attrs[k]);
		else if (k === 'class') e.className = attrs[k];
		else e.setAttribute(k, attrs[k]);
	}
	(kids || []).forEach(function (c) { if (c) e.appendChild(c); });
	return e;
}
function hostOf(u) { try { return new URL(u).hostname; } catch (x) { return u; } }
function fmtBytes(n) {
	if (!n) return '';
	var u = ['B', 'kB', 'MB', 'GB'], i = 0;
	while (n >= 1000 && i < 3) { n /= 1000; i++; }
	return (i ? n.toFixed(n < 10 ? 1 : 0) : n) + ' ' + u[i];
}
function fmtDate(t) {
	if (!t) return 'never';
	var d = new Date(t * 1000);
	return d.getFullYear() + '-' + ('0' + (d.getMonth() + 1)).slice(-2) + '-' + ('0' + d.getDate()).slice(-2);
}
function sw(on, cb) {
	var b = el('button', { class: 'sw' + (on ? ' on' : ''), type: 'button', role: 'switch', 'aria-checked': on ? 'true' : 'false' });
	b.addEventListener('click', function () {
		on = !on;
		b.className = 'sw' + (on ? ' on' : '');
		b.setAttribute('aria-checked', on ? 'true' : 'false');
		cb(on);
	});
	return b;
}
