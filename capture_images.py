# download_esp32_snapshots.py
import os
import time
import requests
from datetime import datetime

ESP_IP = "10.223.247.45"    # <-- change to the IP printed by your ESP32 serial monitor
URL = f"http://{ESP_IP}/capture"
SAVE_DIR = r"D:\Smart Eye Images"  # change to desired folder on your laptop
INTERVAL_SEC = 3  # download every N seconds
TIMEOUT = 30      # seconds for HTTP request timeout

os.makedirs(SAVE_DIR, exist_ok=True)
print("Saving snapshots to:", SAVE_DIR)
print("Polling", URL, "every", INTERVAL_SEC, "seconds")

def save_image(content):
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"img_{timestamp}.jpg"
    path = os.path.join(SAVE_DIR, filename)
    with open(path, "wb") as f:
        f.write(content)
    print("Saved", filename)

while True:
    try:
        r = requests.get(URL, timeout=TIMEOUT)
        if r.status_code == 200 and r.headers.get('Content-Type','').startswith('image'):
            save_image(r.content)
        else:
            print("Unexpected response:", r.status_code, r.headers.get('Content-Type'))
    except Exception as e:
        print("Error fetching:", e)
    time.sleep(INTERVAL_SEC)
