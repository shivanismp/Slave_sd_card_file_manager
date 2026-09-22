import http.server
import socketserver
import urllib.parse
import os
import io
import shutil
import requests
import subprocess
import zipfile
import random
import string
import base64

PORT = 8000

# ====================== BROKER / CERT MANAGER CONFIG ======================
# The cert manager is LAN-only. Put the API key in the environment rather
# than in this file:  set CERTMGR_API_KEY=...
CERT_MGR_URL = os.environ.get("CERTMGR_URL", "http://192.168.1.226:8010")
CERT_MGR_KEY  = "51F8X8y3LM5tLOzm0Fm2b72WYQ2YzkHCeKbrAQ+wgZY="
# CERT_MGR_KEY = os.environ.get("CERTMGR_API_KEY", "")

MQTT_BROKER_HOST = "mqtt.shapet.online"
MQTT_BROKER_PORT = 8883

LITTLEFS_DIR = "littlefs"
MQTT_DIR = os.path.join(LITTLEFS_DIR, "mqtt")
CERTS_DIR = os.path.join(MQTT_DIR, "certs")
BACKUP_DIR = "backup"

REGISTER_NAMES = [
    "HOLD_ADDR_BAR_L12V_MIN", "HOLD_ADDR_BAR_L12V_MAX",
    "HOLD_ADDR_BAR_L12A_MIN", "HOLD_ADDR_BAR_L12A_MAX",
    "HOLD_ADDR_BAR_L23V_MIN", "HOLD_ADDR_BAR_L23V_MAX",
    "HOLD_ADDR_BAR_L23A_MIN", "HOLD_ADDR_BAR_L23A_MAX",
    "HOLD_ADDR_BAR_L31V_MIN", "HOLD_ADDR_BAR_L31V_MAX",
    "HOLD_ADDR_BAR_L31A_MIN", "HOLD_ADDR_BAR_L31A_MAX",
    "HOLD_ADDR_BAR_LAVGV_MIN", "HOLD_ADDR_BAR_LAVGV_MAX",
    "HOLD_ADDR_BAR_LAVGA_MIN", "HOLD_ADDR_BAR_LAVGA_MAX",
    "HOLD_ADDR_BAR_FREQ_MIN", "HOLD_ADDR_BAR_FREQ_MAX",
    "HOLD_ADDR_BAR_KW_MIN", "HOLD_ADDR_BAR_KW_MAX",
    "HOLD_ADDR_BAR_PF_MIN", "HOLD_ADDR_BAR_PF_MAX"
]


# ====================== SERIAL GENERATOR ======================
def generate_serial(length=10, min_digits=2, min_letters=2):
    digits = random.choices(string.digits, k=min_digits)
    letters = random.choices(string.ascii_uppercase, k=min_letters)
    remaining = random.choices(
        string.ascii_uppercase + string.digits,
        k=length - min_digits - min_letters
    )
    serial_chars = digits + letters + remaining
    random.shuffle(serial_chars)
    return ''.join(serial_chars)


# ====================== HELPER FUNCTIONS ======================
def write_with_backup(file_path, content):
    with open(file_path, "w") as f:
        f.write(content)
    print(f"Created: {file_path}")

    base, _ = os.path.splitext(file_path)
    bak_path = base + ".bak"
    with open(bak_path, "w") as f:
        f.write(content)
    print(f"Backup created: {bak_path}")


# ====================== CERTIFICATE MANAGER ======================
class CertManagerError(RuntimeError):
    pass


def _headers():
    if not CERT_MGR_KEY:
        raise CertManagerError(
            "CERTMGR_API_KEY is not set. Run:  set CERTMGR_API_KEY=your-key"
        )
    return {"X-API-Key": CERT_MGR_KEY, "Content-Type": "application/json"}


def check_cert_manager():
    """Confirm the service is up and able to sign before doing anything else."""
    try:
        r = requests.get(f"{CERT_MGR_URL}/api/health", timeout=10)
        r.raise_for_status()
        health = r.json()
    except requests.RequestException as exc:
        raise CertManagerError(f"Cannot reach the cert manager: {exc}")

    if not health.get("ca_key_present"):
        raise CertManagerError(
            "The CA key is not on the server, so nothing can be signed. "
            "Plug in the CA key and restart the service."
        )
    return health


def serial_already_issued(serial):
    r = requests.get(
        f"{CERT_MGR_URL}/api/certs/{serial}", headers=_headers(), timeout=15
    )
    return r.status_code == 200


def issue_certificate(serial, machine_type="", machine_model="",
                      capacity="", customer="", site=""):
    """Ask the cert manager for an identity and get the bundle back.

    inline_bundle collects the one-time key in the same call, so the key
    never sits waiting on the server.
    """
    payload = {
        "cn": serial,
        "customer": customer,
        "site": site,
        "model": f"{machine_type} {machine_model} {capacity}".strip(),
        "tags": [t for t in (machine_type, machine_model, capacity) if t],
        "note": "provisioned by config generator",
        "inline_bundle": True,
    }

    print(f"Requesting certificate for {serial} ...")
    r = requests.post(
        f"{CERT_MGR_URL}/api/certs", json=payload, headers=_headers(), timeout=60
    )

    if r.status_code == 409:
        raise CertManagerError(
            f"{serial} already has a live certificate. Revoke it first, or use "
            f"a different serial."
        )
    if not r.ok:
        try:
            raise CertManagerError(r.json().get("error", r.text))
        except ValueError:
            raise CertManagerError(f"{r.status_code}: {r.text[:200]}")

    data = r.json()
    blob = base64.b64decode(data["bundle_zip_base64"])
    print(f"Certificate issued. Serial {data['serial'][:16]}...")
    return blob, data


def extract_bundle(blob, dest_dir):
    """Unpack the bundle.

    The zip already contains ShapetRootCA.pem, device.crt, private.key and a
    .bak copy of each, so the layout matches what the firmware reads.
    """
    os.makedirs(dest_dir, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        for name in z.namelist():
            target = os.path.join(dest_dir, os.path.basename(name))
            with open(target, "wb") as f:
                f.write(z.read(name))
            print(f"Written: {target}")


def revoke_certificate(serial):
    r = requests.post(
        f"{CERT_MGR_URL}/api/certs/{serial}/revoke", headers=_headers(), timeout=30
    )
    if not r.ok:
        raise CertManagerError(r.json().get("error", r.text))
    print(f"Revoked {serial}")
    return r.json()


# ====================== BACKUP / FLASH ======================
def create_zip_backup(thing_name):
    os.makedirs(BACKUP_DIR, exist_ok=True)
    zip_name = os.path.join(BACKUP_DIR, f"{thing_name}.zip")
    with zipfile.ZipFile(zip_name, "w") as zipf:
        for root, _, files in os.walk(LITTLEFS_DIR):
            for file in files:
                full_path = os.path.join(root, file)
                zipf.write(full_path, os.path.relpath(full_path, LITTLEFS_DIR))
    print(f"ZIP backup created: {zip_name}")
    print("This backup holds the private key. Keep it somewhere controlled, "
          "or delete it once the device is flashed.")


def flash_esp32_device():
    print("Flashing ESP32...")
    cmd = r'"C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat" && idf.py flash'
    result = subprocess.run(cmd, shell=True)
    print("Flash completed with return code:", result.returncode)


# ====================== HTML PAGE ======================
HTML_PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Config Generator</title>
    <link rel="stylesheet" href="style.css">
    <script>
        function toggleCustom(selectId, inputId) {
            let select = document.getElementById(selectId);
            let input = document.getElementById(inputId);
            if (select.value === "Custom") {
                input.classList.remove("hidden");
            } else {
                input.classList.add("hidden");
            }
        }
    </script>
</head>
<body>
    <div class="container">
        <header>
            <h1>ESP32 Machine Config Generator</h1>
            <p>Configure your device • Issue broker credentials • Ready for production</p>
        </header>

        <form method="POST">
            <div class="section">
                <h2>Machine Information</h2>

                <div class="form-group">
                    <label>Machine Type</label>
                    <select name="machine_type" id="machine_type" onchange="toggleCustom('machine_type', 'machine_type_custom')">
                        <option value="Prime-Melt Series">Prime-Melt Series</option>
                        <option value="Power Heat">Power Heat</option>
                        <option value="Chiller">Chiller</option>
                        <option value="Custom">Custom</option>
                    </select>
                    <input type="text" id="machine_type_custom" name="machine_type_custom" class="hidden" placeholder="Enter custom machine type">
                </div>

                <div class="form-group">
                    <label>Machine Model</label>
                    <select name="machine_model" id="machine_model" onchange="toggleCustom('machine_model', 'machine_model_custom')">
                        <option value="Jewellery">Jewellery</option>
                        <option value="Industrial">Industrial</option>
                        <option value="Custom">Custom</option>
                    </select>
                    <input type="text" id="machine_model_custom" name="machine_model_custom" class="hidden" placeholder="Enter custom model">
                </div>

                <div class="form-group">
                    <label>Capacity</label>
                    <select name="capacity" id="capacity" onchange="toggleCustom('capacity', 'capacity_custom')">
                        <option value="2KG">2KG</option>
                        <option value="4KG">4KG</option>
                        <option value="15KW">15KW</option>
                        <option value="25KW">25KW</option>
                        <option value="45KW">45KW</option>
                        <option value="Custom">Custom</option>
                    </select>
                    <input type="text" id="capacity_custom" name="capacity_custom" class="hidden" placeholder="Enter custom capacity">
                </div>

                <div class="form-group">
                    <label>Serial number — leave blank to generate one</label>
                    <input type="text" name="serial_override" placeholder="e.g. 7K2MQ9XB4T">
                </div>

                <div class="form-group">
                    <label>Customer</label>
                    <input type="text" name="customer" placeholder="Optional">
                </div>

                <div class="form-group">
                    <label>Site</label>
                    <input type="text" name="site" placeholder="Optional">
                </div>

                <div class="form-group">
                    <label>Issue broker certificate</label>
                    <select name="issue_cert">
                        <option value="Yes">Yes</option>
                        <option value="No">No</option>
                    </select>
                </div>

                <div class="form-group">
                    <label>Flash ESP32</label>
                    <select name="flash_esp32">
                        <option value="Yes">Yes</option>
                        <option value="No">No</option>
                    </select>
                </div>
            </div>

            <div class="section">
                <h2>Holding Register Values</h2>
                <div class="register-grid">
"""

for reg in REGISTER_NAMES:
    HTML_PAGE += f"""
                    <div class="form-group">
                        <label>{reg}</label>
                        <input type="number" name="{reg}" step="any" placeholder="Enter value">
                    </div>
    """

HTML_PAGE += """
                </div>
            </div>

            <button type="submit" class="submit-btn">
                Generate Config & Setup Device
            </button>
        </form>
    </div>
</body>
</html>
"""


def result_page(title, colour, rows, extra=""):
    body = "".join(f"<p><strong>{k}:</strong> {v}</p>" for k, v in rows)
    return f"""
    <html>
    <body style="font-family:Arial;padding:40px;background:#f8fafc;">
        <div style="max-width:600px;margin:50px auto;background:white;padding:40px;
                    border-radius:12px;box-shadow:0 10px 30px rgba(0,0,0,0.1);">
            <h2 style="color:{colour};">{title}</h2>
            {body}
            {extra}
            <a href="/" style="display:inline-block;margin-top:20px;padding:12px 24px;
               background:#3b82f6;color:white;text-decoration:none;border-radius:8px;">
               &larr; Go Back</a>
        </div>
    </body>
    </html>
    """


# ====================== HTTP HANDLER ======================
class ConfigHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-type", "text/html")
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode())
        else:
            super().do_GET()

    def _respond(self, html):
        self.send_response(200)
        self.send_header("Content-type", "text/html")
        self.end_headers()
        self.wfile.write(html.encode())

    def do_POST(self):
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        data = urllib.parse.parse_qs(post_data.decode())

        def field(name, default=""):
            return data.get(name, [default])[0].strip()

        machine_type = field("machine_type")
        if machine_type == "Custom":
            machine_type = field("machine_type_custom")

        machine_model = field("machine_model")
        if machine_model == "Custom":
            machine_model = field("machine_model_custom")

        capacity = field("capacity")
        if capacity == "Custom":
            capacity = field("capacity_custom")

        customer = field("customer")
        site = field("site")
        issue_cert = field("issue_cert", "No")
        flash_choice = field("flash_esp32", "No")

        # The serial IS the MQTT identity: it becomes the certificate CN, the
        # broker username, and the topic prefix. They must all agree.
        serial_no = field("serial_override") or generate_serial()

        prefix_map = {
            "Prime-Melt Series": "MLTR",
            "Power Heat": "INDT",
            "Chiller": "CHLR",
        }
        machine_prefix = prefix_map.get(machine_type, "GEN")
        thing_name = f"{machine_prefix}-{serial_no}"

        print("\n=== New Device ===")
        print(f"Name   : {thing_name}")
        print(f"Serial : {serial_no}")

        os.makedirs(LITTLEFS_DIR, exist_ok=True)
        os.makedirs(MQTT_DIR, exist_ok=True)
        os.makedirs(CERTS_DIR, exist_ok=True)
        os.makedirs(BACKUP_DIR, exist_ok=True)

        machine_info = (
            f"THING_NAME: {thing_name}\n"
            f"MACHINE_TYPE: {machine_type}\n"
            f"MACHINE_MODEL: {machine_model}\n"
            f"CAPACITY: {capacity}\n"
        )
        write_with_backup(os.path.join(LITTLEFS_DIR, "machine_info.txt"), machine_info)

        write_with_backup(
            os.path.join(LITTLEFS_DIR, "aws_config.txt"),
            f"{issue_cert}\n{flash_choice}\n"
        )

        write_with_backup(os.path.join(MQTT_DIR, "mqtt_ser.txt"), serial_no)

        # Kept under the original filename so the firmware needs no change;
        # it now holds the broker hostname rather than an AWS endpoint.
        write_with_backup(
            os.path.join(MQTT_DIR, "mqtt_broker_host.txt"), MQTT_BROKER_HOST
        )
        write_with_backup(
            os.path.join(MQTT_DIR, "mqtt_broker.txt"),
            f"{MQTT_BROKER_HOST}\n{MQTT_BROKER_PORT}\n"
        )

        topics = [
            f"{serial_no}/MBM/REQ",
            f"{serial_no}/MBM/RES",
            f"{serial_no}/MBM/STAT",
            f"{serial_no}/MBM/TELEMETRY",
            f"{serial_no}/MBM/STATUS",
        ]
        write_with_backup(os.path.join(MQTT_DIR, "topics.txt"), "\n".join(topics))

        cert_serial = None
        if issue_cert == "Yes":
            try:
                check_cert_manager()
                blob, info = issue_certificate(
                    serial_no,
                    machine_type=machine_type,
                    machine_model=machine_model,
                    capacity=capacity,
                    customer=customer,
                    site=site,
                )
                extract_bundle(blob, CERTS_DIR)
                cert_serial = info["serial"]
                create_zip_backup(thing_name)
                print("Certificate setup completed.")
            except CertManagerError as exc:
                print(f"Certificate issue failed: {exc}")
                self._respond(result_page(
                    "Certificate could not be issued", "#dc2626",
                    [("Serial", serial_no), ("Reason", str(exc))],
                    "<p>Config files were written, but the device has no "
                    "credentials and will not connect.</p>"
                ))
                return
        else:
            print("Skipping certificate issue.")

        if flash_choice == "Yes":
            flash_esp32_device()

        rows = [
            ("Machine name", thing_name),
            ("Serial number", serial_no),
            ("Broker", f"{MQTT_BROKER_HOST}:{MQTT_BROKER_PORT}"),
            ("Topic prefix", f"{serial_no}/"),
        ]
        if cert_serial:
            rows.append(("Certificate serial", cert_serial[:24] + "..."))

        self._respond(result_page(
            "Configuration generated", "#10b981", rows
        ))


# ====================== ADD STATUS TOPIC TO EXISTING FILES ======================
serial_path = os.path.join(MQTT_DIR, "mqtt_ser.txt")
topics_path = os.path.join(MQTT_DIR, "topics.txt")
backup_path = os.path.join(MQTT_DIR, "topics.bak")

if os.path.isfile(serial_path):
    with open(serial_path, "r", encoding="utf-8") as f:
        existing_serial = f.read().strip()

    source_path = topics_path if os.path.isfile(topics_path) else backup_path

    if existing_serial and os.path.isfile(source_path):
        with open(source_path, "r", encoding="utf-8") as f:
            existing_topics = [line.strip() for line in f if line.strip()]

        status_topic = f"{existing_serial}/MBM/STATUS"
        if status_topic not in existing_topics:
            existing_topics.append(status_topic)

        write_with_backup(topics_path, "\n".join(existing_topics) + "\n")
        print("STATUS topic:", status_topic)
        print("Updated file:", os.path.abspath(topics_path))
    else:
        print("Topic update skipped: empty serial or no topic files.")
else:
    print("Serial file not found:", os.path.abspath(serial_path))


# ====================== START SERVER ======================
if not CERT_MGR_KEY:
    print("WARNING: CERTMGR_API_KEY is not set. Certificate issuing will fail.")
    print("         set CERTMGR_API_KEY=your-key   before starting this script.")

print(f"Cert manager : {CERT_MGR_URL}")
print(f"Broker       : {MQTT_BROKER_HOST}:{MQTT_BROKER_PORT}")
print(f"Server running at http://localhost:{PORT}")

with socketserver.TCPServer(("", PORT), ConfigHandler) as httpd:
    httpd.serve_forever()
