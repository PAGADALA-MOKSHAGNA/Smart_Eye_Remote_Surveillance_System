# smart_eye_streamlit.py
"""
SmartEye Streamlit dashboard (ESP32 DevKit + ESP32-CAM)
- Parses ESP32 /status JSON with fields:
  { "panActive": bool, "pirState": "HIGH"/"LOW", "irState": "DETECTED"/"CLEAR",
    "distanceCm": number, "lastPIRAcceptedAt": number, "now": number, "servoAngle": number }
- Fetches a camera snapshot from CAMERA_SNAPSHOT_URL (single JPEG)
- Displays prominent anomaly banner when PIR/IR/distance indicate anomaly
- Robust configuration: accepts st.secrets, env vars, or sidebar overrides
"""

import os
import requests
from io import BytesIO
from PIL import Image, UnidentifiedImageError
import streamlit as st
from datetime import datetime

st.set_page_config(page_title="SmartEye — ESP32 Dashboard", layout="wide")

# ---------------------------
# Configuration (robust)
# ---------------------------
# Try Streamlit secrets first (safe: won't crash if secrets missing)
try:
    _secrets = st.secrets
except Exception:
    _secrets = {}

SENSOR_API_URL = _secrets.get("SENSOR_API_URL") or os.getenv("SENSOR_API_URL")
CAMERA_SNAPSHOT_URL = _secrets.get("CAMERA_SNAPSHOT_URL") or os.getenv("CAMERA_SNAPSHOT_URL")

# Sidebar overrides (useful during development)
st.sidebar.title("Connection")
SENSOR_API_URL = st.sidebar.text_input("Sensor API URL (ESP32 /status)", value=SENSOR_API_URL)
CAMERA_SNAPSHOT_URL = st.sidebar.text_input("Camera snapshot URL (ESP32-CAM /capture)", value=CAMERA_SNAPSHOT_URL)

st.sidebar.markdown("---")
st.sidebar.header("Anomaly thresholds")
DISTANCE_ANOMALY_THRESHOLD_CM = st.sidebar.number_input("Distance anomaly threshold (cm)", min_value=1, max_value=1000, value=20)
AUTO_REFRESH = st.sidebar.checkbox("Auto-refresh (every 3 s)", value=True)
REFRESH_INTERVAL_SECS = st.sidebar.slider("Refresh interval (seconds)", min_value=1, max_value=30, value=3)

# Anomaly acknowledgment state
if "anomaly_ack" not in st.session_state:
    st.session_state["anomaly_ack"] = False
if "last_sensor_snapshot" not in st.session_state:
    st.session_state["last_sensor_snapshot"] = None

# Auto-refresh
if AUTO_REFRESH:
    st.experimental_rerun() if False else None  # placeholder - we will use st.button to refresh in this simple file
    # Note: If you prefer auto re-run use streamlit_autorefresh package:
    # from streamlit_autorefresh import st_autorefresh
    # st_autorefresh(interval=REFRESH_INTERVAL_SECS * 1000, key="autorefresh")

# ---------------------------
# Helpers
# ---------------------------
def fetch_sensor_json(url, timeout=2.5):
    """Return (dict, error_string)"""
    try:
        r = requests.get(url, timeout=timeout)
        r.raise_for_status()
        return r.json(), None
    except Exception as e:
        return None, str(e)

def fetch_camera_bytes(url, timeout=3.0):
    try:
        r = requests.get(url, timeout=timeout, stream=True)
        r.raise_for_status()
        return r.content, None
    except Exception as e:
        return None, str(e)

def infer_anomaly_from_status(status_json, distance_thresh_cm):
    """
    Given the exact status JSON from your ESP32 DevKit, infer anomaly.
    Returns (anomaly_bool, reason_str)
    JSON layout expected:
      { "panActive": false, "pirState": "LOW", "irState": "CLEAR",
        "distanceCm": 90, "lastPIRAcceptedAt": 16518, "now": 453660, "servoAngle": 28 }
    """
    if not isinstance(status_json, dict):
        return False, None

    # PIR: strings "HIGH"/"LOW"
    pir = status_json.get("pirState")
    if isinstance(pir, str) and pir.upper() == "HIGH":
        return True, "Motion detected (PIR = HIGH)"

    # IR: strings "DETECTED"/"CLEAR" (your code uses active LOW => "DETECTED")
    ir = status_json.get("irState")
    if isinstance(ir, str) and ir.upper() in ("DETECTED", "DETECT", "LOW", "1"):
        return True, "IR sensor detected object"

    # Distance
    try:
        dist = float(status_json.get("distanceCm", -1))
        if dist >= 0 and dist < float(distance_thresh_cm):
            return True, f"Object too close: {dist:.1f} cm < {distance_thresh_cm} cm"
    except Exception:
        pass

    # Optional: panActive true may indicate ongoing response but not anomaly by itself
    return False, None

# ---------------------------
# UI: header and controls
# ---------------------------
st.title("🔭 SmartEye — ESP32 DevKit Live Status")
st.markdown("Displays the current `/status` JSON from the ESP32 DevKit and a snapshot from the ESP32-CAM. Anomaly banner appears automatically.")

col_top_left, col_top_right = st.columns([3, 1])
with col_top_right:
    if st.button("Manual refresh"):
        # clear ack so anomaly reappears if still active
        st.session_state["anomaly_ack"] = False

# ---------------------------
# Fetch sensor data
# ---------------------------
sensor_data, sensor_err = fetch_sensor_json(SENSOR_API_URL)

if sensor_err:
    st.error(f"Failed to fetch sensor data from `{SENSOR_API_URL}`: {sensor_err}")
    st.stop()

# store last snapshot of sensor to session
st.session_state["last_sensor_snapshot"] = sensor_data

# Parse and display key metrics
pan_active = bool(sensor_data.get("panActive", False))
pir_state = sensor_data.get("pirState", "UNKNOWN")
ir_state = sensor_data.get("irState", "UNKNOWN")
servo_angle = sensor_data.get("servoAngle", None)
distance_cm = sensor_data.get("distanceCm", None)
last_pir_accepted = sensor_data.get("lastPIRAcceptedAt", None)
esp_now = sensor_data.get("now", None)

metric_cols = st.columns(4)
with metric_cols[0]:
    st.metric("PIR (motion)", "Triggered" if str(pir_state).upper() == "HIGH" else "No")
with metric_cols[1]:
    st.metric("IR", f"{ir_state}")
with metric_cols[2]:
    st.metric("Distance (cm)", f"{distance_cm}" if distance_cm is not None else "N/A")
with metric_cols[3]:
    st.metric("Pan active", "Yes" if pan_active else "No")

st.markdown("**Full `/status` JSON (raw)**")
st.json(sensor_data)

# ---------------------------
# Fetch and show camera image (if available)
# ---------------------------
st.subheader("Camera snapshot")
img_bytes, img_err = fetch_camera_bytes(CAMERA_SNAPSHOT_URL)
if img_err:
    st.warning(f"Could not fetch camera snapshot from `{CAMERA_SNAPSHOT_URL}`: {img_err}")
else:
    try:
        img = Image.open(BytesIO(img_bytes))
        st.image(img, caption=f"Snapshot @ {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}", use_column_width=True)
    except UnidentifiedImageError:
        st.warning("Fetched camera content could not be decoded as an image.")
    except Exception as e:
        st.warning(f"Error decoding image: {e}")

# ---------------------------
# Infer anomaly and show banner
# ---------------------------
anomaly_flag, anomaly_reason = infer_anomaly_from_status(sensor_data, DISTANCE_ANOMALY_THRESHOLD_CM)

# Provide explicit override: some deployments set PIR as boolean 1/0 or use 'DETECTED' string; our infer function covers common cases.
if anomaly_flag and not st.session_state["anomaly_ack"]:
    st.markdown(
        f"""
        <div style="border:3px solid #ff4b4b; padding:14px; border-radius:8px; background:#fff3f3">
          <h2 style="color:#b30000; margin:0;">🚨 ANOMALY DETECTED</h2>
          <div style="font-size:14px; margin-top:6px;">{anomaly_reason or 'Sensor-reported anomaly'}</div>
          <div style="margin-top:8px;">
            <form action="#">
              <!-- Empty form just to align buttons -->
            </form>
          </div>
        </div>
        """,
        unsafe_allow_html=True,
    )

    # Provide acknowledge button
    if st.button("Acknowledge / Clear banner"):
        st.session_state["anomaly_ack"] = True
else:
    if st.session_state["anomaly_ack"]:
        st.success("Anomaly acknowledged by operator (banner cleared).")
    else:
        st.info("No anomaly detected by current sensor readings.")

# ---------------------------
# Extra diagnostics & controls
# ---------------------------
with st.expander("Diagnostics & Controls"):
    st.write("Parsed fields from `/status`:")
    st.write(f"- panActive: {pan_active}")
    st.write(f"- pirState: {pir_state}")
    st.write(f"- irState: {ir_state}")
    st.write(f"- distanceCm: {distance_cm}")
    st.write(f"- servoAngle: {servo_angle}")
    st.write(f"- lastPIRAcceptedAt: {last_pir_accepted}")
    st.write(f"- now (ESP millis-like): {esp_now}")

    st.write("---")
    st.write("Quick actions (these assume your ESP32 implements corresponding endpoints):")
    col_a, col_b, col_c = st.columns(3)
    with col_a:
        if st.button("Trigger /trigger"):
            try:
                r = requests.get(SENSOR_API_URL.replace("/status", "/trigger"), timeout=2.5)
                st.write("Trigger result:", r.text)
            except Exception as e:
                st.warning("Trigger failed: " + str(e))
    with col_b:
        if st.button("Stop /stop"):
            try:
                r = requests.get(SENSOR_API_URL.replace("/status", "/stop"), timeout=2.5)
                st.write("Stop result:", r.text)
            except Exception as e:
                st.warning("Stop failed: " + str(e))
    with col_c:
        if st.button("Refresh sensor now"):
            st.experimental_rerun()

st.caption("SmartEye — ESP32 DevKit status viewer. Adapted to your device JSON: {\"panActive\":false,\"pirState\":\"LOW\",\"irState\":\"CLEAR\",\"distanceCm\":90,...}")
