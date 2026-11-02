"""Issue TLS certificates for the Mosquitto broker and the back-end client from the project CA.

    python tools/gen_broker_cert.py --host 192.168.1.10 --host broker.local

Writes secrets/broker/{server_key.pem,server_cert.pem,ca_cert.pem} for
tools/mosquitto/mosquitto.conf, and secrets/backend/{client_key.pem,client_cert.pem}
(CN "kws-backend", full access in tools/mosquitto/acl) for tools/kws_ctl.py.
Each --host may be an IP address or DNS name and must match the host in the
devices' broker URI.
"""
import argparse
import datetime
import ipaddress
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

ROOT = Path(__file__).resolve().parent.parent
SECRETS = ROOT / "secrets"


def san(host):
    try:
        return x509.IPAddress(ipaddress.ip_address(host))
    except ValueError:
        return x509.DNSName(host)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", action="append", required=True)
    ap.add_argument("--days", type=int, default=825)
    args = ap.parse_args()

    ca_key = serialization.load_pem_private_key((SECRETS / "ca_key.pem").read_bytes(), password=None)
    ca_cert = x509.load_pem_x509_certificate((SECRETS / "ca_cert.pem").read_bytes())
    key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, args.host[0])]))
            .issuer_name(ca_cert.subject).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(days=args.days))
            .add_extension(x509.SubjectAlternativeName([san(h) for h in args.host]), critical=False)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .sign(ca_key, hashes.SHA256()))

    out = SECRETS / "broker"
    out.mkdir(parents=True, exist_ok=True)
    (out / "server_key.pem").write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                                            serialization.PrivateFormat.TraditionalOpenSSL,
                                                            serialization.NoEncryption()))
    (out / "server_cert.pem").write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (out / "ca_cert.pem").write_bytes((SECRETS / "ca_cert.pem").read_bytes())
    print(f"wrote {out.relative_to(ROOT)}/server_cert.pem for {', '.join(args.host)}")

    backend = SECRETS / "backend"
    if not (backend / "client_cert.pem").exists():
        backend.mkdir(parents=True, exist_ok=True)
        bkey = ec.generate_private_key(ec.SECP256R1())
        bcert = (x509.CertificateBuilder()
                 .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "kws-backend")]))
                 .issuer_name(ca_cert.subject).public_key(bkey.public_key())
                 .serial_number(x509.random_serial_number())
                 .not_valid_before(now - datetime.timedelta(minutes=5))
                 .not_valid_after(now + datetime.timedelta(days=args.days))
                 .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
                 .sign(ca_key, hashes.SHA256()))
        (backend / "client_key.pem").write_bytes(bkey.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption()))
        (backend / "client_cert.pem").write_bytes(bcert.public_bytes(serialization.Encoding.PEM))
        print(f"wrote {backend.relative_to(ROOT)}/client_cert.pem (CN kws-backend)")


if __name__ == "__main__":
    main()
