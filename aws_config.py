import http.server
import socketserver
import urllib.parse
import os
import shutil
import requests
import subprocess
import json
import zipfile
import random
import string

PORT = 8000

# ====================== AWS CONFIG ======================
AWS_CLI = r"C:\Program Files\Amazon\AWSCLIV2\aws.exe"
LITTLEFS_DIR = "littlefs"
MQTT_DIR = os.path.join(LITTLEFS_DIR, "mqtt")
CERTS_DIR = os.path.join(MQTT_DIR, "certs")
BACKUP_DIR = "backup"

POLICY_NAME = "MACHINE_AUTO_ID_GENERATE"
ROOT_CA_URL = "https://www.amazontrust.com/repository/AmazonRootCA1.pem"
AWS_ENDPOINT = "a3958f6g61vd15-ats.iot.ap-south-1.amazonaws.com"

# ====================== SERIAL GENERATOR ======================
def generate_serial(length=10, min_digits=2, min_letters=2):
    digits = random.choices(string.digits, k=min_digits)
    letters = random.choices(string.ascii_uppercase, k=min_letters)
    remaining = random.choices(string.ascii_uppercase + string.digits, k=length - min_digits - min_letters)
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

# ====================== AWS FUNCTIONS ======================
def create_thing(thing_name):
    print("Creating AWS IoT Thing...")
    cmd = [AWS_CLI, "iot", "create-thing", "--thing-name", thing_name]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("THING CREATION FAILED!")
        print(result.stderr)
        return False
    print("Thing created successfully!")
    return True

def create_keys_and_certificate():
    print("Generating certificates...")
    os.makedirs("certs", exist_ok=True)
    cmd = [AWS_CLI, "iot", "create-keys-and-certificate", "--set-as-active",
           "--certificate-pem-outfile", "certs/device.pem.crt",
           "--public-key-outfile", "certs/public.pem.key",
           "--private-key-outfile", "certs/private.pem.key"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("Certificate creation failed!")
        print(result.stderr)
        return None
    return json.loads(result.stdout)

def create_policy_if_missing():
    cmd = [AWS_CLI, "iot", "get-policy", "--policy-name", POLICY_NAME]
    if subprocess.run(cmd, capture_output=True).returncode == 0:
        return
    print("Creating policy...")
    policy_document = {"Version": "2012-10-17", "Statement": [{"Effect": "Allow", "Action": ["iot:Connect","iot:Publish","iot:Subscribe","iot:Receive"], "Resource": "*"}]}
    with open("policy.json", "w") as f:
        json.dump(policy_document, f)
    create_cmd = [AWS_CLI, "iot", "create-policy", "--policy-name", POLICY_NAME, "--policy-document", "file://policy.json"]
    subprocess.run(create_cmd, capture_output=True)

def attach_policy(certificate_arn):
    cmd = [AWS_CLI, "iot", "attach-policy", "--policy-name", POLICY_NAME, "--target", certificate_arn]
    subprocess.run(cmd, capture_output=True)

def attach_thing_principal(thing_name, certificate_arn):
    cmd = [AWS_CLI, "iot", "attach-thing-principal", "--thing-name", thing_name, "--principal", certificate_arn]
    subprocess.run(cmd, capture_output=True)

def download_root_ca():
    print("Downloading Amazon Root CA...")
    response = requests.get(ROOT_CA_URL, timeout=10)
    if response.status_code == 200:
        with open(os.path.join(CERTS_DIR, "AmazonRootCA1.pem"), "wb") as f:
            f.write(response.content)
        print("Root CA downloaded")
    else:
        print("Failed to download Root CA")

def prepare_certificates():
    shutil.copyfile("certs/device.pem.crt", os.path.join(CERTS_DIR, "device.crt"))
    shutil.copyfile("certs/private.pem.key", os.path.join(CERTS_DIR, "private.key"))
    print("Certificates prepared")

def create_zip_backup(thing_name):
    zip_name = os.path.join(BACKUP_DIR, f"{thing_name}.zip")
    with zipfile.ZipFile(zip_name, "w") as zipf:
        for root, _, files in os.walk(LITTLEFS_DIR):
            for file in files:
                full_path = os.path.join(root, file)
                zipf.write(full_path, os.path.relpath(full_path, LITTLEFS_DIR))
    print(f"ZIP Backup created: {zip_name}")

def flash_esp32_device():
    print("Flashing ESP32...")
    cmd = r'"C:\Espressif\frameworks\esp-idf-v5.5.2\export.bat" && idf.py flash'
    result = subprocess.run(cmd, shell=True)
    print("Flash completed with return code:", result.returncode)

# ====================== MODERN HTML PAGE ======================
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
            <p>Configure your device • Generate AWS IoT credentials • Ready for production</p>
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
                    <label>Generate AWS Certificates</label>
                    <select name="aws_generate">
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

            <button type="submit" class="submit-btn">
                Generate Config & Setup Device
            </button>
        </form>
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

    def do_POST(self):
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        data = urllib.parse.parse_qs(post_data.decode())

        machine_type = data.get("machine_type", [""])[0]
        if machine_type == "Custom":
            machine_type = data.get("machine_type_custom", [""])[0]

        machine_model = data.get("machine_model", [""])[0]
        if machine_model == "Custom":
            machine_model = data.get("machine_model_custom", [""])[0]

        capacity = data.get("capacity", [""])[0]
        if capacity == "Custom":
            capacity = data.get("capacity_custom", [""])[0]

        aws_generate = data.get("aws_generate", ["No"])[0]
        flash_choice = data.get("flash_esp32", ["No"])[0]

        serial_no = generate_serial()
        prefix_map = {"Prime-Melt Series": "MLTR", "Power Heat": "INDT", "Chiller": "CHLR", "Custom": "GEN"}
        machine_prefix = prefix_map.get(machine_type, "GEN")
        thing_name = f"{machine_prefix}-{serial_no}"

        print(f"\n=== New Device Created ===")
        print(f"Thing Name : {thing_name}")
        print(f"Serial     : {serial_no}")

        os.makedirs(LITTLEFS_DIR, exist_ok=True)
        os.makedirs(MQTT_DIR, exist_ok=True)
        os.makedirs(CERTS_DIR, exist_ok=True)
        os.makedirs(BACKUP_DIR, exist_ok=True)

        machine_info = f"THING_NAME: {thing_name}\nMACHINE_TYPE: {machine_type}\nMACHINE_MODEL: {machine_model}\nCAPACITY: {capacity}\n"
        write_with_backup(os.path.join(LITTLEFS_DIR, "machine_info.txt"), machine_info)

        aws_info = f"{aws_generate}\n{flash_choice}\n"
        write_with_backup(os.path.join(LITTLEFS_DIR, "aws_config.txt"), aws_info)

        write_with_backup(os.path.join(MQTT_DIR, "mqtt_ser.txt"), serial_no)
        write_with_backup(os.path.join(MQTT_DIR, "mqtt_aws_endpoint.txt"), AWS_ENDPOINT)

        topics = [f"{serial_no}/MBM/REQ", f"{serial_no}/MBM/RES", f"{serial_no}/MBM/STAT", f"{serial_no}/MBM/TELEMETRY"]
        write_with_backup(os.path.join(MQTT_DIR, "topics.txt"), "\n".join(topics))

        if aws_generate == "Yes":
            if create_thing(thing_name):
                create_policy_if_missing()
                cert_data = create_keys_and_certificate()
                if cert_data:
                    cert_arn = cert_data.get("certificateArn")
                    attach_policy(cert_arn)
                    attach_thing_principal(thing_name, cert_arn)
                    download_root_ca()
                    prepare_certificates()
                    create_zip_backup(thing_name)
                    print("✅ AWS IoT Setup Completed Successfully!")
                else:
                    print("⚠️ Certificate generation failed.")
            else:
                print("⚠️ Thing creation failed.")
        else:
            print("Skipping AWS certificate generation.")

        if flash_choice == "Yes":
            flash_esp32_device()

        response = f"""
        <html>
        <body style="font-family:Arial;padding:40px;background:#f8fafc;">
            <div style="max-width:600px;margin:50px auto;background:white;padding:40px;border-radius:12px;box-shadow:0 10px 30px rgba(0,0,0,0.1);">
                <h2 style="color:#10b981;">✅ Configuration Generated Successfully!</h2>
                <p><strong>Thing Name:</strong> {thing_name}</p>
                <p><strong>Serial Number:</strong> {serial_no}</p>
                <a href="/" style="display:inline-block;margin-top:20px;padding:12px 24px;background:#3b82f6;color:white;text-decoration:none;border-radius:8px;">← Go Back</a>
            </div>
        </body>
        </html>
        """;

        self.send_response(200)
        self.send_header("Content-type", "text/html")
        self.end_headers()
        self.wfile.write(response.encode())


# ====================== START SERVER ======================
print(f"🚀 Server running at http://localhost:{PORT}")
with socketserver.TCPServer(("", PORT), ConfigHandler) as httpd:
    httpd.serve_forever()
