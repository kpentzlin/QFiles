# WebDAV-Testserver für die automatischen Tests (CI): Benutzer "test", Kennwort "geheim", Standard- und Digest-Anmeldung.
# Aufruf: python tools/ci-davd.py <Wurzelverzeichnis> <Port> [digest]   (digest = nur Digest-Anmeldung)
import sys

from cheroot import wsgi
from wsgidav.wsgidav_app import WsgiDAVApp

root, port = sys.argv[1], int(sys.argv[2])
digest_only = len(sys.argv) > 3 and sys.argv[3] == "digest"
config = {
    "host": "127.0.0.1",
    "port": port,
    "provider_mapping": {"/": root},
    "http_authenticator": {
        "accept_basic": not digest_only,
        "accept_digest": True,
        "default_to_digest": digest_only,
    },
    "simple_dc": {"user_mapping": {"*": {"test": {"password": "geheim"}}}},
    "verbose": 1,
    "logging": {"enable_loggers": []},
}
server = wsgi.Server((config["host"], port), WsgiDAVApp(config))
try:
    server.start()
except KeyboardInterrupt:
    server.stop()
