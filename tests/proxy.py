"""An HTTP proxy for the tests: wants the password test:pw, tunnels CONNECT, and answers plain requests for
proxy.test itself (which proves the name was never looked up on the client)."""
import base64, select, socket, sys, threading

WANT = "Basic " + base64.b64encode(b"test:pw").decode()


def handle(c):
    try:
        head = b""
        while b"\r\n\r\n" not in head:
            d = c.recv(4096)
            if not d:
                return
            head += d
        text, rest = head.split(b"\r\n\r\n", 1)
        lines = text.decode("latin-1").split("\r\n")
        method, target, _ = lines[0].split(" ", 2)
        headers = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        if headers.get("proxy-authorization") != WANT:
            c.sendall(b"HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"t\"\r\n"
                      b"Content-Length: 0\r\nConnection: close\r\n\r\n")
            return
        if method == "CONNECT":
            host, port = target.rsplit(":", 1)
            up = socket.create_connection(("127.0.0.1" if host == "10.0.2.2" else host, int(port)))
            c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            if rest:
                up.sendall(rest)
            while True:
                r, _, _ = select.select([c, up], [], [], 30)
                if not r:
                    return
                for s in r:
                    d = s.recv(65536)
                    if not d:
                        return
                    (up if s is c else c).sendall(d)
        body = ("served by the proxy for %s\n" % target).encode()
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                  % len(body) + body)
    except OSError:
        pass
    finally:
        c.close()


s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(8)
while True:
    conn, _ = s.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
