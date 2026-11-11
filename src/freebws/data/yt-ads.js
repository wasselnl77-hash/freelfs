/* FreeBWS: YouTube ad removal. A user script (document start, every frame; it does nothing on
 * other sites). Written for FreeLFS (MIT). YouTube changes this often, so it is kept small:
 *  1. ad fields are removed from the player data before the page reads them
 *     (ytInitialPlayerResponse, ytInitialData, JSON.parse, fetch and XHR of /youtubei/v1/...)
 *  2. what still slips through is skipped: the ad is wound to its end, "Skip" is pressed
 *  3. ad slots on the pages are hidden, the "ad blockers are not allowed" box is closed
 * The filter lists' own YouTube rules (scriptlets, network rules) work next to this. */
(function () {
	'use strict';
	var h = location.hostname;
	if (!/(^|\.)(youtube\.com|youtube-nocookie\.com|youtubekids\.com)$/.test(h)) return;
	if (window.__fbYtAds) return;
	window.__fbYtAds = 1;
	var W = window, D = document;

	var DROP = { adPlacements: 1, playerAds: 1, adSlots: 1, adBreakHeartbeatParams: 1, adsEngagementPanelRenderer: 1 };
	var AD_ITEM = ['adSlotRenderer', 'promotedSparklesWebRenderer', 'promotedVideoRenderer', 'compactPromotedVideoRenderer',
		'displayAdRenderer', 'inFeedAdLayoutRenderer', 'searchPyvRenderer', 'bannerPromoRenderer', 'statementBannerRenderer',
		'adBreakServiceRenderer', 'playerLegacyDesktopWatchAdsRenderer', 'carouselAdRenderer', 'actionCompanionAdRenderer'];

	function clean(o, depth) {
		if (!o || typeof o !== 'object' || depth > 40) return o;
		if (Array.isArray(o)) {
			for (var i = o.length - 1; i >= 0; i--) {
				var it = o[i];
				if (it && typeof it === 'object') {
					var drop = false;
					for (var k = 0; k < AD_ITEM.length; k++) if (AD_ITEM[k] in it) { drop = true; break; }
					if (!drop && it.content && typeof it.content === 'object')
						for (var k2 = 0; k2 < AD_ITEM.length; k2++) if (AD_ITEM[k2] in it.content) { drop = true; break; }
					if (!drop && it.richItemRenderer && it.richItemRenderer.content)
						for (var k3 = 0; k3 < AD_ITEM.length; k3++) if (AD_ITEM[k3] in it.richItemRenderer.content) { drop = true; break; }
					if (drop) { o.splice(i, 1); continue; }
				}
				clean(it, depth + 1);
			}
			return o;
		}
		for (var key in o) {
			if (DROP[key]) { try { delete o[key]; } catch (e) {} continue; }
			var v = o[key];
			if (v && typeof v === 'object') clean(v, depth + 1);
		}
		if (o.playerConfig && o.playerConfig.adConfig) try { delete o.playerConfig.adConfig; } catch (e) {}
		return o;
	}

	/* 1a. the data the page's own script assigns */
	['ytInitialPlayerResponse', 'ytInitialData'].forEach(function (name) {
		var v;
		try {
			Object.defineProperty(W, name, { configurable: true, enumerable: true,
				get: function () { return v; },
				set: function (n) { v = clean(n, 0); } });
		} catch (e) {}
	});

	/* 1b. JSON.parse (the player and the browse responses go through it) */
	var oparse = JSON.parse;
	JSON.parse = function () {
		var r = oparse.apply(this, arguments);
		if (r && typeof r === 'object' && ('adPlacements' in r || 'playerAds' in r || 'adSlots' in r || 'playerResponse' in r ||
			'responseContext' in r || 'contents' in r || 'onResponseReceivedActions' in r))
			try { clean(r, 0); } catch (e) {}
		return r;
	};

	/* 1c. fetch of the internal API: the Response's json() and text() */
	var ofetch = W.fetch;
	if (ofetch) W.fetch = function (inp) {
		var p = ofetch.apply(this, arguments);
		var u = typeof inp === 'string' ? inp : (inp && inp.url) || '';
		if (u.indexOf('/youtubei/v1/') < 0) return p;
		return p.then(function (r) {
			return r.clone().text().then(function (t) {
				if (t.charAt(0) !== '{') return r;
				try {
					var o = clean(oparse(t), 0);
					return new Response(JSON.stringify(o), { status: r.status, statusText: r.statusText, headers: r.headers });
				} catch (e) { return r; }
			}).catch(function () { return r; });
		});
	};

	/* 1d. XHR of the internal API */
	var xo = XMLHttpRequest.prototype.open;
	XMLHttpRequest.prototype.open = function (m, u) {
		if (String(u).indexOf('/youtubei/v1/') >= 0) {
			var self = this, dt = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, 'responseText'),
				dr = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, 'response'), cache;
			var text = function () {
				if (cache === undefined) {
					var t = dt.get.call(self);
					try { cache = t && t.charAt(0) === '{' ? JSON.stringify(clean(oparse(t), 0)) : t; } catch (e) { cache = t; }
				}
				return cache;
			};
			Object.defineProperty(this, 'responseText', { configurable: true, get: function () { return self.readyState === 4 ? text() : dt.get.call(self); } });
			Object.defineProperty(this, 'response', { configurable: true, get: function () {
				if (self.readyState !== 4 || (self.responseType && self.responseType !== 'text' && self.responseType !== 'json')) return dr.get.call(self);
				return self.responseType === 'json' ? oparse(text()) : text();
			} });
		}
		return xo.apply(this, arguments);
	};

	/* 3. cosmetic: ad slots and banners out of the way */
	function css() {
		var par = D.head || D.documentElement;
		if (!par) { D.addEventListener('readystatechange', css, { once: true }); return; }
		var s = D.createElement('style');
		s.textContent = [
			'ytd-ad-slot-renderer', 'ytd-promoted-sparkles-web-renderer', 'ytd-promoted-video-renderer',
			'ytd-display-ad-renderer', 'ytd-in-feed-ad-layout-renderer', 'ytd-banner-promo-renderer',
			'ytd-statement-banner-renderer', 'ytd-companion-slot-renderer', 'ytd-action-companion-ad-renderer',
			'ytd-player-legacy-desktop-watch-ads-renderer', 'ytd-merch-shelf-renderer', '#masthead-ad', '#player-ads',
			'.ytp-ad-overlay-container', '.ytp-ad-image-overlay', '.ytp-ad-text-overlay', '.ytp-paid-content-overlay',
			'.ytd-companion-slot-renderer', 'ytm-promoted-sparkles-web-renderer', 'ytm-companion-ad-renderer',
			'.ad-container', '.ad-div', 'ytd-reel-video-renderer:has(ytd-ad-slot-renderer)',
			'ytd-rich-item-renderer:has(> #content > ytd-ad-slot-renderer)', 'ytd-ad-slot-renderer ~ *:empty',
			'.ytp-ad-persistent-progress-bar-container', 'tp-yt-paper-dialog:has(ytd-enforcement-message-view-model)',
			'ytd-enforcement-message-view-model', 'tp-yt-iron-overlay-backdrop.opened:has(~ tp-yt-paper-dialog ytd-enforcement-message-view-model)'
		].join(',') + '{display:none!important}';
		par.appendChild(s);
	}

	/* 2. what slipped through: skip it */
	function tick() {
		try {
			var p = D.querySelector('.html5-video-player');
			if (p && p.classList.contains('ad-showing')) {
				var v = p.querySelector('video.html5-main-video') || D.querySelector('video');
				if (v) {
					v.muted = true;
					if (isFinite(v.duration) && v.duration > 0 && v.currentTime < v.duration - 0.25) v.currentTime = v.duration - 0.1;
					v.playbackRate = 16;
					if (v.paused) { var pr = v.play(); if (pr && pr.catch) pr.catch(function () {}); }
				}
				var b = D.querySelector('.ytp-skip-ad-button, .ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-ad-skip-button-container button');
				if (b) b.click();
			}
			var c = D.querySelector('.ytp-ad-overlay-close-button');
			if (c) c.click();
			var e = D.querySelector('ytd-enforcement-message-view-model');
			if (e) {
				var dlg = e.closest('tp-yt-paper-dialog') || e;
				var btn = dlg.querySelector('#dismiss-button button, #dismiss-button, button[aria-label]');
				if (btn) btn.click();
				dlg.remove();
				var bd = D.querySelector('tp-yt-iron-overlay-backdrop.opened'); if (bd) bd.remove();
				D.body && (D.body.style.overflow = '');
				var vv = D.querySelector('video'); if (vv && vv.paused) { var q = vv.play(); if (q && q.catch) q.catch(function () {}); }
			}
		} catch (er) {}
	}
	css();
	setInterval(tick, 400);
	var pending = 0;
	new MutationObserver(function () {
		if (pending) return;
		pending = 1;
		setTimeout(function () { pending = 0; tick(); }, 150);
	}).observe(D, { childList: true, subtree: true });
})();
