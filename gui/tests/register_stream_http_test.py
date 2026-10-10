"""Exercise the real curl transport with fragmented SSE and a reconnect."""
import http.server
import json
import subprocess
import sys
import threading
import time
import urllib.parse


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    connections = 0
    queries = []
    reads = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        self.server.reads.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
        body = b'{"revision":"12","values":[{"address":"D100","value":42},{"address":"D101","value":0}]}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        uri = urllib.parse.urlsplit(self.path)
        self.server.queries.append((uri.path, urllib.parse.parse_qs(uri.query)))
        self.server.connections += 1
        connection = self.server.connections
        if connection == 1:
            self.send_response(429)
            body = b'{"error":{"code":"REG_BUSY","message":"too many register watchers"}}'
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        if connection == 2:
            wire = b': connected\n\nevent: change\ndata: {"r":"11","a":"D100","v":41}\n\nevent: change\ndata: {"r":"12","a":"D101","v":0}\n\nevent: dropped\ndata: 3\n\n'
        else:
            wire = b': connected\n\nevent: change\ndata: {"r":"1","a":"D100","v":0}\n\n'
        try:
            for pos in range(0, len(wire), 3):
                self.wfile.write(wire[pos:pos + 3])
                self.wfile.flush()
            if connection > 2:
                for _ in range(20):
                    time.sleep(0.25)
                    self.wfile.write(b': ping\n\n')
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


with Server(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}"], check=True, timeout=20)
        assert server.connections == 3, server.connections
        assert all(path == "/api/v1/registers/events" and query == {
            "start": ["D100"], "count": ["32"]
        } for path, query in server.queries), server.queries
        assert len(server.reads) == 2 and all(read == {"addresses": ["D100", "D101"]} for read in server.reads)
    finally:
        server.shutdown()
        thread.join()
