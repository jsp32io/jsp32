"""Local certificate authorities and TLS servers; no external network access."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import socket
import ssl
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID


def certificate(name, issuer=None, expired=False):
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    now = datetime.now(timezone.utc)
    builder = (x509.CertificateBuilder().subject_name(subject)
               .issuer_name(issuer[1].subject if issuer else subject)
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(now - timedelta(days=3))
               .not_valid_after(now - timedelta(days=1) if expired else now + timedelta(days=30))
               .add_extension(x509.BasicConstraints(ca=issuer is None, path_length=None), True))
    if issuer:
        builder = (builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(name)]), False)
                   .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False))
    cert = builder.sign(issuer[0] if issuer else key, hashes.SHA256())
    return key, cert


def pem(cert):
    return cert.public_bytes(serialization.Encoding.PEM).decode()


class TLSServer:
    def __init__(self, directory: Path, name, cert=None, mode="echo", tls13=False):
        self.mode = mode
        self.context = None
        if cert:
            key, certificate = cert
            cert_file, key_file = directory / (name + ".pem"), directory / (name + ".key")
            cert_file.write_text(pem(certificate))
            key_file.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                                  serialization.PrivateFormat.PKCS8,
                                                  serialization.NoEncryption()))
            self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.context.maximum_version = ssl.TLSVersion.TLSv1_3 if tls13 else ssl.TLSVersion.TLSv1_2
            if tls13:
                self.context.minimum_version = ssl.TLSVersion.TLSv1_3
            self.context.load_cert_chain(cert_file, key_file)
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(16)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.stopping = threading.Event()
        self.sni = []
        if self.context:
            self.context.set_servername_callback(lambda conn, name, ctx: self.sni.append(name))
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stopping.is_set():
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self.handle, args=(client,), daemon=True).start()

    def handle(self, client):
        client.settimeout(3)
        try:
            if self.mode == "stall":
                time.sleep(0.5)
                return
            client = self.context.wrap_socket(client, server_side=True)
            if self.mode == "duplex":
                client.sendall(b"hello")
            if self.mode in ("normal", "truncated"):
                client.sendall(b"abcdefghij")
                if self.mode == "truncated":
                    fd = client.detach()
                    socket.socket(fileno=fd).close()
                    return
                client.unwrap().close()
                return
            while not self.stopping.is_set():
                data = client.recv(16384)
                if not data:
                    client.unwrap().close()
                    return
                client.sendall(data)
        except (OSError, ssl.SSLError):
            pass
        finally:
            client.close()

    def close(self):
        self.stopping.set()
        self.listener.close()
        self.thread.join(timeout=1)


def fixtures(directory):
    system = certificate("Test System Root")
    private = certificate("Test Private Root")
    other = certificate("Test Other Root")
    (directory / "system-ca.pem").write_text(pem(system[1]))
    servers = {
        "system": TLSServer(directory, "system", certificate("service.test", system)),
        "private": TLSServer(directory, "private", certificate("service.test", private)),
        "expired": TLSServer(directory, "expired", certificate("service.test", private, expired=True)),
        "normal": TLSServer(directory, "normal", certificate("service.test", private), "normal"),
        "truncated": TLSServer(directory, "truncated", certificate("service.test", private), "truncated"),
        "stall": TLSServer(directory, "stall", mode="stall"),
        "tls13": TLSServer(directory, "tls13", certificate("service.test", system), tls13=True),
        "duplex": TLSServer(directory, "duplex", certificate("service.test", system), "duplex"),
    }
    data = {name: server.port for name, server in servers.items()}
    data.update(ca=pem(private[1]), otherCA=pem(other[1]))
    return servers, data
