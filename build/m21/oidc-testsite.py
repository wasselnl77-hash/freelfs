#!/usr/bin/env python3
"""A pretend "Sign in with Google" for FreeBWS tests (round 21), Python standard library only, no account of any kind.

    build/m21/oidc-testsite.py [--port 8080] [--log FILE]

One server answers for several host names (put them in the guest's /etc/hosts, all pointing at the host: with
QEMU user networking 10.0.2.2):

    client.test   the "client website": a page with a cross-origin iframe (like Google's One Tap), a Sign in button
                  that opens a popup (window.open), a canvas and a WebGL canvas that keep the GPU busy, a heartbeat
    idp.test      the identity provider: /authorize, account chooser, consent, /popup-done (postMessage to
                  window.opener, then window.close()), /token (CORS), the iframe
    sso.test      the second domain of a login (accounts.youtube-style cookie sync): /sync and /ping

The flow, like Google's: popup -> idp /authorize -> 302 sso /sync (sets a cookie) -> 302 idp /signin (chooser,
picks the account by itself after 600 ms and POSTs /consent) -> 303 /popup-done (postMessage code, window.close)
-> the opener fetches /token and then navigates to client /session -> 302 idp /check -> 302 sso /ping -> 302
client /welcome (heavy page, heartbeat goes on). Every stage the page reaches is reported by a /beacon request,
and the heartbeat (requestAnimationFrame counter, once a second) says whether the page still paints; /status?run=N
tells a test what happened. The log (--log) has one line per request with the time.
"""
import argparse, json, sys, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8080)
ap.add_argument("--log", default=None)
args = ap.parse_args()
P = args.port
CLIENT, IDP, SSO = "client.test", "idp.test", "sso.test"
lock = threading.Lock()
stages = {}  # run id -> list of (time, stage, detail)
logf = open(args.log, "a", buffering=1) if args.log else sys.stderr


def u(host, path):
    return "http://%s:%d%s" % (host, P, path)


HEAD = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
BEACON = """
<script>
var RUN = new URLSearchParams(location.search).get('run') || (document.cookie.match(/run=([^;]+)/)||[])[1] || '0';
function beacon(stage, detail){ try { new Image().src = '%(client)s/beacon?run=' + RUN + '&stage=' + encodeURIComponent(stage) +
  '&d=' + encodeURIComponent(detail||'') + '&t=' + Date.now(); } catch(e){} }
var frames = 0; (function f(){ frames++; requestAnimationFrame(f); })();
setInterval(function(){ beacon('heartbeat', location.host + ' frames=' + frames + ' vis=' + document.visibilityState); }, 1000);
</script>""" % {"client": u(CLIENT, "")}

CLIENT_HOME = HEAD + "<title>Test client</title>" + BEACON + """
<style>
body{font:16px sans-serif;margin:0;background:#f4f6fa;color:#223}
header{padding:16px 32px;background:#fff;box-shadow:0 2px 12px #0002;display:flex;gap:24px;align-items:center}
.card{margin:32px;padding:32px;background:#fff;border-radius:16px;box-shadow:0 8px 40px #0003;max-width:720px}
button{font-size:18px;padding:14px 28px;border:0;border-radius:8px;background:#0b57d0;color:#fff;cursor:pointer}
.spin{width:48px;height:48px;border-radius:50%%;border:6px solid #0b57d0;border-top-color:transparent;animation:s 1s linear infinite}
@keyframes s{to{transform:rotate(360deg)}}
.blur{backdrop-filter:blur(8px);background:#ffffff80;padding:12px;border-radius:12px}
iframe{border:0;width:360px;height:120px;position:absolute;right:24px;top:90px;border-radius:12px;box-shadow:0 4px 24px #0004}
</style>
<header><b>Test client</b><div class=spin></div><span id=state>signed out</span></header>
<iframe src="%(idp)s/onetap?run=RUNID"></iframe>
<div class=card><h2>Sign in</h2><p class=blur>Sign in with the test identity provider.</p>
<button id=signin>Sign in with IdP</button>
<canvas id=c2d width=600 height=200></canvas><canvas id=gl width=400 height=200></canvas></div>
<script>
document.querySelectorAll('iframe')[0].src = document.querySelectorAll('iframe')[0].src.replace('RUNID', RUN);
beacon('client-loaded', location.href);
var c = document.getElementById('c2d').getContext('2d');
(function d(t){ for (var i=0;i<30;i++){ c.fillStyle='hsl('+((t/10+i*12)%%360)+',70%%,50%%)'; c.fillRect(i*20,(Math.sin(t/300+i)+1)*80,18,40);} c.clearRect(0,0,0,0); requestAnimationFrame(d); })(0);
try { var g = document.getElementById('gl').getContext('webgl'); (function r(t){ g.clearColor((Math.sin(t/500)+1)/2,0.3,0.6,1); g.clear(g.COLOR_BUFFER_BIT); requestAnimationFrame(r); })(0); beacon('webgl','yes'); } catch(e){ beacon('webgl','no'); }
var popup = null;
document.getElementById('signin').onclick = function(){
  beacon('click-signin','');
  popup = window.open('%(idp)s/authorize?client_id=test&response_type=code&display=popup&run=' + RUN +
    '&state=' + Math.random().toString(36).slice(2) + '&redirect_uri=' + encodeURIComponent('%(client)s'), 'idp-login', 'width=480,height=640');
  beacon('popup-opened', popup ? 'yes' : 'BLOCKED');
};
window.addEventListener('message', function(ev){
  beacon('message', ev.origin + ' ' + JSON.stringify(ev.data).slice(0,80));
  if (ev.origin !== '%(idp)s' || !ev.data || !ev.data.code) return;
  document.getElementById('state').textContent = 'got code';
  fetch('%(idp)s/token?code=' + ev.data.code + '&run=' + RUN, {mode:'cors'}).then(function(r){return r.json();}).then(function(j){
    beacon('token', j.id_token.slice(0,16));
    location.href = '/session?token=' + j.id_token + '&run=' + RUN;
  }).catch(function(e){ beacon('token-error', String(e)); });
});
</script>""" % {"idp": u(IDP, ""), "client": u(CLIENT, "")}

WELCOME = HEAD + "<title>Welcome</title>" + BEACON + """
<style>body{font:16px sans-serif;margin:0;background:#101820;color:#dde}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px;padding:16px}
.t{height:90px;border-radius:8px;animation:p 2s ease-in-out infinite alternate;will-change:transform}
@keyframes p{from{transform:scale(1);filter:hue-rotate(0)}to{transform:scale(1.08);filter:hue-rotate(90deg)}}</style>
<h1 style="padding:16px">Welcome, Test Person <span id=n></span></h1><div class=grid id=g></div>
<script>
beacon('welcome-loaded', location.href);
var g = document.getElementById('g'); for (var i=0;i<400;i++){ var d=document.createElement('div'); d.className='t';
  d.style.background='hsl('+(i*7%360)+',60%,45%)'; d.style.animationDelay=(i%20)/10+'s'; g.appendChild(d); }
setInterval(function(){ document.getElementById('n').textContent = frames; }, 500);
fetch('/api/me?run=' + RUN).then(function(r){return r.json();}).then(function(j){ beacon('api-me', j.name); });
</script>""" 

ONETAP = HEAD + "<title>One tap</title>" + BEACON + """
<style>body{font:14px sans-serif;margin:8px;background:#fff}button{padding:8px 16px;background:#0b57d0;color:#fff;border:0;border-radius:16px}</style>
<div>Continue as Test Person? <button>Continue</button></div>
<script>beacon('onetap-loaded', location.href);
window.parent.postMessage({hello:'from-iframe'}, '*');
window.addEventListener('message', function(e){ beacon('onetap-got', JSON.stringify(e.data).slice(0,40)); });</script>"""

SIGNIN = HEAD + "<title>Choose an account</title>" + BEACON + """
<style>body{font:16px sans-serif;margin:0;background:#fff}.a{margin:24px;padding:16px;border:1px solid #ccc;border-radius:8px}</style>
<div class=a><b>Test Person</b><br>test.person@idp.test</div>
<form id=f method=post action=/consent><input type=hidden name=run id=r><input type=hidden name=state id=s></form>
<script>beacon('chooser-shown', location.href);
document.getElementById('r').value = RUN; document.getElementById('s').value = new URLSearchParams(location.search).get('state')||'';
setTimeout(function(){ beacon('chooser-submit',''); document.getElementById('f').submit(); }, 600);</script>"""

DONE = HEAD + "<title>Done</title>" + BEACON + """
<p>Signed in, closing...</p>
<script>
beacon('popup-done', 'opener=' + (window.opener ? 'yes' : 'NO'));
if (window.opener) { window.opener.postMessage({code: 'CODE-' + RUN, state: 'x'}, '%(client)s'); beacon('popup-posted',''); }
setTimeout(function(){ beacon('popup-closing',''); window.close(); }, 150);
</script>""" % {"client": u(CLIENT, "")}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        logf.write("%.3f %s %s %s\n" % (time.time(), self.headers.get("Host", "?"), self.command, self.path))

    def send(self, code, body=b"", ctype="text/html; charset=utf-8", headers=()):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def redirect(self, url, code=302, headers=()):
        self.send(code, "", headers=[("Location", url)] + list(headers))

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        form = urllib.parse.parse_qs(self.rfile.read(n).decode())
        run = form.get("run", ["0"])[0]
        if self.path.startswith("/consent"):
            return self.redirect(u(IDP, "/popup-done?run=" + run), 303)
        self.send(404)

    def do_GET(self):
        host = self.headers.get("Host", "").split(":")[0]
        pr = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(pr.query)
        run = q.get("run", ["0"])[0]
        path = pr.path
        if path == "/beacon":
            with lock:
                stages.setdefault(run, []).append((time.time(), q.get("stage", [""])[0], q.get("d", [""])[0]))
            return self.send(200, b"GIF89a", "image/gif")
        if path == "/status":
            with lock:
                return self.send(200, json.dumps(stages.get(run, [])), "application/json")
        if path == "/reset":
            with lock:
                stages.clear()
            return self.send(200, "ok", "text/plain")
        if path == "/favicon.ico":
            return self.send(204)
        if host == CLIENT:
            if path == "/":
                return self.send(200, CLIENT_HOME, headers=[("Set-Cookie", "run=%s; Path=/" % run)])
            if path == "/session":
                return self.redirect(u(IDP, "/check?run=%s&token=%s" % (run, q.get("token", [""])[0])),
                                     headers=[("Set-Cookie", "session=1; Path=/")])
            if path == "/autonav":    # navigates itself after 3 s (a test of a page that changes while hidden)
                return self.send(200, HEAD + "<title>Autonav</title>" + BEACON + "<h1>autonav</h1><script>beacon('autonav-loaded','');"
                                 "setTimeout(function(){ location.href='/welcome?run=' + RUN; }, 3000);</script>")
            if path == "/welcome":
                return self.send(200, WELCOME, headers=[("Set-Cookie", "run=%s; Path=/" % run)])
            if path == "/api/me":
                return self.send(200, json.dumps({"name": "Test Person"}), "application/json")
        if host == IDP:
            if path == "/authorize":
                return self.redirect(u(SSO, "/sync?run=%s&state=%s" % (run, q.get("state", [""])[0])))
            if path == "/signin":
                return self.send(200, SIGNIN)
            if path == "/popup-done":
                return self.send(200, DONE, headers=[("Set-Cookie", "run=%s; Path=/" % run)])
            if path == "/token":
                return self.send(200, json.dumps({"id_token": "ID.%s.TOKEN" % run}), "application/json",
                                 [("Access-Control-Allow-Origin", u(CLIENT, ""))])
            if path == "/check":
                return self.redirect(u(SSO, "/ping?run=%s" % run))
            if path == "/onetap":
                return self.send(200, ONETAP, headers=[("Set-Cookie", "run=%s; Path=/" % run)])
        if host == SSO:
            if path == "/sync":
                return self.redirect(u(IDP, "/signin?run=%s&state=%s" % (run, q.get("state", [""])[0])),
                                     headers=[("Set-Cookie", "sso=1; Path=/")])
            if path == "/ping":
                return self.redirect(u(CLIENT, "/welcome?run=" + run))
        self.send(404, "not found: %s %s" % (host, self.path))


class S(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


print("test sites on port %d: %s %s %s" % (P, CLIENT, IDP, SSO), flush=True)
S(("0.0.0.0", P), H).serve_forever()
