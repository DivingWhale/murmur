import http.client
import json
import ssl
from urllib.parse import urlsplit


class API:
    def __init__(self, url, token, ca_file=None, timeout=15):
        u = urlsplit(url)
        if u.scheme != "https" or not u.hostname or u.username or u.query or u.fragment or u.path.strip("/"):
            raise ValueError("api_url must be a plain HTTPS origin")
        self.conn = http.client.HTTPSConnection(u.hostname, u.port, timeout=timeout,
                                                 context=ssl.create_default_context(cafile=ca_file))
        self.headers = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}

    def __call__(self, endpoint, payload):
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
        # A TLS keep-alive may expire during a normal daemon sleep. CAS and
        # client-generated creation IDs make replay after a lost reply safe.
        try:
            return self.request(endpoint, data)
        except (http.client.RemoteDisconnected, BrokenPipeError, ConnectionResetError, ssl.SSLEOFError):
            return self.request(endpoint, data)

    def request(self, endpoint, data):
        try:
            self.conn.request("POST", "/v1/pc/" + endpoint, data, self.headers)
            response = self.conn.getresponse()
            if response.status != 200:
                if response.status in (400, 413):
                    raise ValueError(f"HTTP {response.status}: record rejected")
                raise OSError(f"HTTP {response.status}")
            return json.load(response)
        except (OSError, ValueError, http.client.HTTPException):
            self.conn.close()
            raise

    def close(self):
        self.conn.close()
