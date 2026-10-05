# Einfacher FTP-Server für die automatischen Tests (pyftpdlib).
# Aufruf: python tools/ci-ftpd.py <Wurzelverzeichnis> [Port]
# Konten: anonym (nur lesen) und "test" / "geheim" (lesen und schreiben).
import sys
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer

root = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 2121
auth = DummyAuthorizer()
auth.add_user("test", "geheim", root, perm="elradfmwMT")
auth.add_anonymous(root)
handler = FTPHandler
handler.authorizer = auth
handler.passive_ports = range(30000, 30100)
FTPServer(("127.0.0.1", port), handler).serve_forever()
