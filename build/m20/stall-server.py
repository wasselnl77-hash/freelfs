# A web server that sends the start of an HTML page and then stalls for ever (test of FreeBWS's watchdogs).
import socket, sys, threading, time

def serve(c):
    try:
        c.recv(4096)
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 100000\r\n\r\n<html><body><h1>stalled page</h1>")
        while True:
            time.sleep(60)
    except OSError:
        pass

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(8)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
