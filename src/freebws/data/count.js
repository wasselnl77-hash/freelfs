/* FreeBWS: an estimate of how many requests the ad blocker stopped on the page. WebKit's content
 * blockers do not report what they block, so this counts third-party sub-resources that failed
 * to load without ever reaching the network (no resource timing entry). Runs in every frame. */
(function () {
	'use strict';
	if (window.__fbCnt) return;
	window.__fbCnt = 1;
	var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.fbcount;
	if (!h) return;
	var n = 0, timer = 0, seen = Object.create(null);
	function flush() { timer = 0; var k = n; n = 0; if (k) try { h.postMessage(String(k)); } catch (e) {} }
	function hit(u) {
		if (!u || seen[u] || !/^https?:/.test(u)) return;
		try { if (performance.getEntriesByName(u).length) return; } catch (e) {}
		seen[u] = 1;
		(window.__fbCntList = window.__fbCntList || []).push(u);
		n++;
		if (!timer) timer = setTimeout(flush, 400);
	}
	window.addEventListener('error', function (e) {
		var t = e.target;
		if (!t || t === window || !t.tagName) return;
		if (/^(SCRIPT|IMG|IFRAME|LINK|VIDEO|AUDIO|SOURCE|EMBED|OBJECT)$/.test(t.tagName)) hit(t.currentSrc || t.src || t.href);
	}, true);
	var of = window.fetch;
	if (of) window.fetch = function (inp) {
		var u = typeof inp === 'string' ? inp : (inp && inp.url) || '';
		var p = of.apply(this, arguments);
		p.catch(function (err) { if (err && err.name === 'TypeError' && navigator.onLine) try { hit(new URL(u, location.href).href); } catch (e) {} });
		return p;
	};
})();
