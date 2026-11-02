"""Operator CLI for esp32-tinyml-keyword-spotting devices over MQTT (mutual TLS as "kws-backend").

    pip install paho-mqtt>=2
    python tools/kws_ctl.py --broker 192.168.1.10 watch
    python tools/kws_ctl.py --broker 192.168.1.10 cmd kws-a1b2c3 ping
    python tools/kws_ctl.py --broker 192.168.1.10 cmd kws-a1b2c3 set_threshold --value 0.92
    python tools/kws_ctl.py --broker 192.168.1.10 cmd kws-a1b2c3 ota_model \
        --url https://updates.local/kws_model_v2.kwsm --file ~/kws-work/kws_model_v2.kwsm

Certificates default to secrets/backend/ and secrets/ca_cert.pem (tools/gen_broker_cert.py).
"""
import argparse
import hashlib
import json
import ssl
import sys
import time
import uuid
from pathlib import Path

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parent.parent
SECRETS = ROOT / "secrets"


def connect(args):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"kws-ctl-{uuid.uuid4().hex[:6]}")
    c.tls_set(ca_certs=str(args.ca), certfile=str(args.cert), keyfile=str(args.key),
              tls_version=ssl.PROTOCOL_TLS_CLIENT)
    c.connect(args.broker, args.port, keepalive=30)
    return c


def watch(args):
    c = connect(args)
    c.on_message = lambda _c, _u, m: print(f"{time.strftime('%H:%M:%S')} {m.topic} {m.payload.decode(errors='replace')}")
    c.subscribe(f"devices/{args.device or '+'}/#", qos=1)
    c.loop_forever()


def command(args):
    body = {"id": uuid.uuid4().hex[:8], "op": args.op}
    if args.value is not None:
        body["value"] = args.value
    if args.url:
        body["url"] = args.url
    if args.file:
        body["sha256"] = hashlib.sha256(Path(args.file).expanduser().read_bytes()).hexdigest()
    elif args.sha256:
        body["sha256"] = args.sha256

    c = connect(args)
    done = {}

    def on_message(_c, _u, m):
        r = json.loads(m.payload)
        if r.get("id") == body["id"]:
            print(json.dumps(r))
            if r.get("status") != "accepted":
                done["r"] = r

    c.on_message = on_message
    c.subscribe(f"devices/{args.device}/cmd/result", qos=1)
    c.loop_start()
    time.sleep(0.5)
    c.publish(f"devices/{args.device}/cmd", json.dumps(body), qos=1).wait_for_publish()
    deadline = time.time() + args.timeout
    while "r" not in done and time.time() < deadline:
        time.sleep(0.2)
    c.loop_stop()
    if "r" not in done:
        sys.exit(f"no result within {args.timeout} s")
    sys.exit(0 if done["r"].get("status") == "ok" else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--broker", required=True)
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--ca", type=Path, default=SECRETS / "ca_cert.pem")
    ap.add_argument("--cert", type=Path, default=SECRETS / "backend/client_cert.pem")
    ap.add_argument("--key", type=Path, default=SECRETS / "backend/client_key.pem")
    sub = ap.add_subparsers(dest="action", required=True)
    w = sub.add_parser("watch")
    w.add_argument("device", nargs="?")
    c = sub.add_parser("cmd")
    c.add_argument("device")
    c.add_argument("op", choices=["ping", "get_health", "set_threshold", "ota_firmware", "ota_model",
                                  "reboot", "forget_wifi"])
    c.add_argument("--value", type=float)
    c.add_argument("--url")
    c.add_argument("--sha256")
    c.add_argument("--file", help="local copy of the model, to compute sha256")
    c.add_argument("--timeout", type=float, default=180)
    args = ap.parse_args()
    watch(args) if args.action == "watch" else command(args)


if __name__ == "__main__":
    main()
