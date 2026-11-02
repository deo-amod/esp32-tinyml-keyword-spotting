"""Generate the project's signing keys and development CA into secrets/ (git-ignored).

    python tools/gen_keys.py

Creates, only if missing:
    secrets/model_signing_key.pem        ECDSA P-256, signs .kwsm models
    secrets/secure_boot_signing_key.pem  ECDSA P-256, Secure Boot v2 (release builds)
    secrets/ca_key.pem, ca_cert.pem      project CA for MQTT broker and device certificates
and (re)writes the model-signing public key to
    firmware/components/kws_model/keys/model_signing_pub.pem  (committed)

Production: keep these keys in an HSM or KMS. This script is for development and pilots.
"""
import datetime
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parent.parent
SECRETS = ROOT / "secrets"
PUBKEY_OUT = ROOT / "firmware/components/kws_model/keys/model_signing_pub.pem"


def _key(path):
    if path.exists():
        print(f"keep    {path.relative_to(ROOT)}")
        return serialization.load_pem_private_key(path.read_bytes(), password=None)
    key = ec.generate_private_key(ec.SECP256R1())
    path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                       serialization.PrivateFormat.PKCS8,
                                       serialization.NoEncryption()))
    print(f"created {path.relative_to(ROOT)}")
    return key


def main():
    SECRETS.mkdir(exist_ok=True)
    model_key = _key(SECRETS / "model_signing_key.pem")
    _key(SECRETS / "secure_boot_signing_key.pem")
    ca_key = _key(SECRETS / "ca_key.pem")

    PUBKEY_OUT.parent.mkdir(parents=True, exist_ok=True)
    PUBKEY_OUT.write_bytes(model_key.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    print(f"wrote   {PUBKEY_OUT.relative_to(ROOT)}")

    ca_cert = SECRETS / "ca_cert.pem"
    if ca_cert.exists():
        return
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "esp32-tinyml-keyword-spotting development CA")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now).not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
            .add_extension(x509.KeyUsage(digital_signature=False, content_commitment=False,
                                         key_encipherment=False, data_encipherment=False,
                                         key_agreement=False, key_cert_sign=True, crl_sign=True,
                                         encipher_only=False, decipher_only=False), critical=True)
            .sign(ca_key, hashes.SHA256()))
    ca_cert.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    print(f"created {ca_cert.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
