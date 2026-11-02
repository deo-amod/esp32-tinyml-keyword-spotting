"""Manufacturing provisioning: per-device identity, MQTT certificate and BLE credentials.

    python tools/provision_device.py --broker-uri mqtts://broker.local:8883 [--port COM5]

For one device this script:
  1. creates an ECDSA P-256 key and a client certificate signed by secrets/ca_cert.pem
  2. creates a random BLE provisioning password and its SRP6a salt/verifier
     (only the verifier goes on the device; print the password on the device label)
  3. builds the `fctry` NVS partition image with ESP-IDF's nvs_partition_gen.py
  4. optionally flashes it to the device at the fctry offset (--port)

Output goes to secrets/devices/<device_id>/ (git-ignored). Requires IDF_PATH (for
ESP-IDF's SRP6a implementation) and `pip install -r tools/requirements.txt`.
"""
import argparse
import csv
import datetime
import json
import os
import secrets
import subprocess
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

ROOT = Path(__file__).resolve().parent.parent
SECRETS = ROOT / "secrets"
FCTRY_OFFSET = "0x1B000"  # must match firmware/partitions.csv
FCTRY_SIZE = "0x6000"
PROV_USER = "kws"


def idf_path():
    p = os.environ.get("IDF_PATH")
    if not p:
        sys.exit("IDF_PATH is not set: run this from an ESP-IDF environment")
    return Path(p)


def srp_salt_verifier(username, password):
    esp_prov = idf_path() / "tools/esp_prov"
    sys.path[:0] = [str(esp_prov), str(esp_prov / "security")]
    import srp6a  # noqa: E402  (ESP-IDF's implementation, matches the firmware)
    return srp6a.generate_salt_and_verifier(username, password, len_s=16)


def device_certificate(device_id, ca_key, ca_cert, years):
    key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, device_id)]))
            .issuer_name(ca_cert.subject).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(days=365 * years))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]), critical=False)
            .sign(ca_key, hashes.SHA256()))
    key_pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                serialization.NoEncryption())
    return key_pem, cert.public_bytes(serialization.Encoding.PEM)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device-id", help="default: kws-<random hex>")
    ap.add_argument("--broker-uri", required=True, help="e.g. mqtts://192.168.1.10:8883")
    ap.add_argument("--cert-years", type=int, default=5)
    ap.add_argument("--port", help="serial port: flash the fctry partition after generating it")
    args = ap.parse_args()
    if not args.broker_uri.startswith(("mqtts://", "wss://")):
        sys.exit("broker URI must use TLS (mqtts:// or wss://)")

    ca_key = serialization.load_pem_private_key((SECRETS / "ca_key.pem").read_bytes(), password=None)
    ca_cert = x509.load_pem_x509_certificate((SECRETS / "ca_cert.pem").read_bytes())
    device_id = args.device_id or f"kws-{secrets.token_hex(6)}"
    out = SECRETS / "devices" / device_id
    out.mkdir(parents=True, exist_ok=False)

    key_pem, cert_pem = device_certificate(device_id, ca_key, ca_cert, args.cert_years)
    (out / "client_key.pem").write_bytes(key_pem)
    (out / "client_cert.pem").write_bytes(cert_pem)
    (out / "ca_cert.pem").write_bytes((SECRETS / "ca_cert.pem").read_bytes())

    password = secrets.token_urlsafe(9)
    salt, verifier = srp_salt_verifier(PROV_USER, password)
    (out / "prov_salt.bin").write_bytes(salt)
    (out / "prov_verifier.bin").write_bytes(verifier)

    rows = [
        ("kws", "namespace", "", ""),
        ("device_id", "data", "string", device_id),
        ("broker_uri", "data", "string", args.broker_uri),
        ("prov_user", "data", "string", PROV_USER),
        ("ca_cert", "file", "string", str(out / "ca_cert.pem")),
        ("client_cert", "file", "string", str(out / "client_cert.pem")),
        ("client_key", "file", "string", str(out / "client_key.pem")),
        ("prov_salt", "file", "binary", str(out / "prov_salt.bin")),
        ("prov_verifier", "file", "binary", str(out / "prov_verifier.bin")),
    ]
    csv_path = out / "fctry.csv"
    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["key", "type", "encoding", "value"])
        w.writerows(rows)

    bin_path = out / "fctry.bin"
    subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", str(csv_path), str(bin_path),
                    FCTRY_SIZE], check=True)

    label = {"device_id": device_id, "ble_name_prefix": "KWS_", "prov_username": PROV_USER,
             "prov_password": password, "pop_scheme": "Security 2 (SRP6a)"}
    (out / "label.json").write_text(json.dumps(label, indent=2) + "\n")
    print(f"\nDevice {device_id} -> {out}")
    print(f"  BLE provisioning: username '{PROV_USER}', password '{password}' (print on the device label)")

    if args.port:
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32", "-p", args.port, "write_flash",
                        FCTRY_OFFSET, str(bin_path)], check=True)
        print(f"  fctry partition written at {FCTRY_OFFSET}")
    else:
        print(f"  flash with: python -m esptool --chip esp32 -p <PORT> write_flash {FCTRY_OFFSET} {bin_path}")


if __name__ == "__main__":
    main()
