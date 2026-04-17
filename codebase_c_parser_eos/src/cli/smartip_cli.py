#!/usr/bin/env python3
"""
SMART IP Edge CLI Client (Interactive Menu)
=============================================
Menu-driven CLI for the SMART IP Edge Control Plane REST API.
Uses only stdlib (urllib) — no external dependencies.

Usage:
    python3 smartip_cli.py
    python3 smartip_cli.py --no-tls
    python3 smartip_cli.py --host 192.168.1.160 --port 8443
"""

import json
import os
import ssl
import time
import urllib.error
import urllib.request
import urllib.parse
import argparse
import logging

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)8s] [%(name)s] %(message)s"
)
log = logging.getLogger("smartip_cli")

# ── Config ────────────────────────────────────────────────────────────

TOKEN_FILE = os.path.expanduser("~/.smartip_camera_token")
DEFAULT_HOST = "127.0.0.1"
DEFAULT_HTTPS_PORT = 8443
DEFAULT_HTTP_PORT = 8080


class SmartIPCLI:
    """Interactive menu-driven CLI client for SMART IP Edge."""

    def __init__(self, host: str, port: int, use_tls: bool):
        self.host = host
        self.port = port
        self.use_tls = use_tls
        self.token = None
        self.user = None
        self.role = None

        self._setup_base_url()
        self._load_token()

    def _setup_base_url(self):
        scheme = "https" if self.use_tls else "http"
        self.base_url = f"{scheme}://{self.host}:{self.port}/api/v1"

    # ── HTTP Helpers ──────────────────────────────────────────────────

    def _ssl_ctx(self):
        if not self.use_tls:
            return None
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx

    def _request(self, path: str, method: str = "GET", data: dict = None, timeout: int = 10) -> dict:
        url = f"{self.base_url}{path}"
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"

        body = json.dumps(data).encode("utf-8") if data else None
        req = urllib.request.Request(url, data=body, headers=headers, method=method)

        try:
            with urllib.request.urlopen(req, context=self._ssl_ctx(), timeout=timeout) as resp:
                raw = resp.read()
                resp_data = raw.decode("utf-8", errors="replace")
                if not resp_data or not resp_data.strip():
                    return {"status": "ok"}
                try:
                    return json.loads(resp_data)
                except json.JSONDecodeError:
                    return {"status": "ok", "raw_response": resp_data}
        except urllib.error.HTTPError as e:
            err_data = e.read().decode("utf-8")
            try:
                error_body = json.loads(err_data)
            except Exception:
                error_body = {"error": e.reason}
            return {"_http_error": e.code, **error_body}
            return {"_connection_error": str(e.reason)}
        except Exception as e:
            return {"_request_error": str(e)}

    # ── Token Persistence ─────────────────────────────────────────────

    def _save_token(self):
        with open(TOKEN_FILE, "w") as f:
            json.dump({"token": self.token, "user": self.user, "role": self.role}, f)

    def _load_token(self):
        if os.path.exists(TOKEN_FILE):
            try:
                with open(TOKEN_FILE, "r") as f:
                    data = json.load(f)
                self.token = data.get("token")
                self.user = data.get("user")
                self.role = data.get("role")
            except Exception:
                self.token = None

    def _check_logged_in(self) -> bool:
        if not self.token:
            print("\n  ✗ Not logged in. Select option 1 to login first.")
            return False
        return True

    # ── Menu Actions ──────────────────────────────────────────────────

    def do_login(self):
        print("\n── Login ──")
        username = input("  Username: ").strip()
        password = input("  Password: ").strip()

        if not username or not password:
            print("  ✗ Username and password required.")
            return

        result = self._request("/auth/login", method="POST", data={
            "username": username,
            "password": password,
        })

        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_connection_error" in result:
            print(f"  ✗ Connection failed: {result['_connection_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Login failed: {result.get('error', 'Unknown error')}")
            return

        self.token = result.get("token")
        
        # Stage 2: Check for 2FA requirements
        status = result.get("status")
        if status == "otp_required":
            print("\n  ! Email OTP required. Please check your email.")
            otp = input("  Enter OTP: ").strip()
            result = self._request("/auth/verify-otp", method="POST", data={
                "username": username,
                "otp": otp
            })
            if "_http_error" in result:
                print(f"  ✗ OTP verification failed: {result.get('error', 'Unknown error')}")
                return
            self.token = result.get("token")
            status = result.get("status") # Check if TOTP follows OTP

        if status == "totp_required":
            print("\n  ! TOTP required. Please check your authenticator app.")
            totp = input("  Enter Code: ").strip()
            result = self._request("/auth/verify-totp", method="POST", data={
                "username": username,
                "totp_code": totp
            })
            if "_http_error" in result:
                print(f"  ✗ TOTP verification failed: {result.get('error', 'Unknown error')}")
                return
            self.token = result.get("token")

        if not self.token:
            print(f"  ✗ Login failed: {result.get('error', 'No token received')}")
            return

        try:
            import base64
            payload_b64 = self.token.split('.')[0]
            payload_b64 += '=' * (-len(payload_b64) % 4)
            payload = json.loads(base64.urlsafe_b64decode(payload_b64))
            self.user = payload.get("user", username)
            self.role = payload.get("role", "unknown")
        except Exception:
            self.user = username
            self.role = "unknown"

        self._save_token()
        print(f"  ✓ Logged in as '{self.user}' (role: {self.role})")

    def do_health(self):
        print("\n── Health Check ──")
        result = self._request("/system/health")
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_connection_error" in result:
            print(f"  ✗ Connection failed: {result['_connection_error']}")
            return
        print(f"  Status: {result.get('status', '?')}")

    def do_status(self):
        if not self._check_logged_in():
            return
        print("\n── Engine Status ──")
        result = self._request("/system/status")

        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return

        print(f"  Total RTSP Clients: {result.get('total_clients', 0)}")
        lenses = result.get("lenses", {})
        for lid, lcfg in lenses.items():
            print(f"\n    Lens: {lid} ({lcfg.get('device')})")
            branches = lcfg.get("branches", {})
            for tier, bcfg in branches.items():
                state = bcfg.get('state', 'idle')
                print(f"      - {tier:5s}: {bcfg.get('mount', '?')} | {bcfg.get('resolution','?')} | "
                      f"{bcfg.get('fps','?')}fps | {bcfg.get('encoder','?')} | {state}")
        print()

    def do_configure_lens(self):
        if not self._check_logged_in():
            return
        print("\n── Configure Lens ──")
        print("    1. lens1")
        print("    2. lens2")
        l_idx = input("  Select Lens (1-2) [1]: ").strip() or "1"
        lens_id = "lens1" if l_idx == "1" else "lens2"

        print(f"\n  ── Branch Selection for {lens_id} ──")
        print("    1. main + sub (Standard Dual Stream)")
        print("    2. main only")
        print("    3. sub only")
        print("    4. third (Plain MJPEG 360p@5fps — no AI/overlay)")
        print("    5. main + sub + third (All branches)")
        try:
            b_idx = input("  Select (1-5) [1]: ").strip() or "1"
        except (ValueError, IndexError):
            print("  ✗ Invalid choice.")
            return

        params = {}

        # Determine which branches to configure
        branches_to_configure = []
        if b_idx == "1":
            branches_to_configure = ["main", "sub"]
        elif b_idx == "2":
            branches_to_configure = ["main"]
        elif b_idx == "3":
            branches_to_configure = ["sub"]
        elif b_idx == "4":
            branches_to_configure = ["third"]
        elif b_idx == "5":
            branches_to_configure = ["main", "sub", "third"]
        else:
            print("  ✗ Invalid choice.")
            return

        for branch in branches_to_configure:
            if branch == "third":
                # Third branch: fixed plain config, no user input needed
                params["third"] = {}
                print(f"\n  ✓ Third branch: 640x360, 5fps, MJPEG (plain, no AI)")
                continue

            print(f"\n  ── {branch.upper()} Stream Parameters ──")

            # Resolution
            print("  ── Resolution ──")
            print("    1. 1080p  (1920x1080)")
            print("    2. 720p   (1280x720)")
            print("    3. 480p   (640x480)")
            print("    4. 360p   (640x360)")
            print("    5. Custom")
            default_r = "1" if branch == "main" else "2"
            r_idx = input(f"  Select (1-5) [{default_r}]: ").strip() or default_r
            r_map = {"1": "1920x1080", "2": "1280x720", "3": "640x480", "4": "640x360"}
            if r_idx in r_map:
                res = r_map[r_idx]
            elif r_idx == "5":
                res = input("  Enter WxH (e.g. 1024x768): ").strip() or "1920x1080"
            else:
                res = r_map.get(default_r, "1920x1080")

            # FPS
            print("  ── Framerate (FPS) ──")
            print("    1.  30 FPS")
            print("    2.  25 FPS")
            print("    3.  15 FPS")
            print("    4.  10 FPS")
            print("    5.   5 FPS")
            print("    6. Custom")
            default_f = "2" if branch == "main" else "4"
            f_idx = input(f"  Select (1-6) [{default_f}]: ").strip() or default_f
            f_map = {"1": "30", "2": "25", "3": "15", "4": "10", "5": "5"}
            if f_idx in f_map:
                fps = int(f_map[f_idx])
            elif f_idx == "6":
                custom_f = input("  Enter FPS (1-60): ").strip()
                fps = int(custom_f) if custom_f.isdigit() else 25
            else:
                fps = int(f_map.get(default_f, "25"))

            # Encoder
            print("  ── Encoder ──")
            print("    1. H.264 (hardware)")
            print("    2. H.265 (hardware)")
            e_idx = input("  Select (1-2) [1]: ").strip() or "1"
            enc = "h265" if e_idx == "2" else "h264"

            params[branch] = {"resolution": res, "fps": fps, "encoder": enc}

        # Cairo (AI overlay) — only for main/sub, NOT third
        has_ai_branches = any(b in branches_to_configure for b in ["main", "sub"])
        if has_ai_branches:
            print("\n  ── AI Overlay (Cairo) ──")
            print("    Enable AI bounding box drawing on main/sub streams.")
            ai_choice = input("  Enable AI Overlays? (y/n) [y]: ").strip().lower() or "y"
            params["cairo"] = (ai_choice in ("y", "yes"))

            print("\n  ── NTP Clock Overlay ──")
            ntp_choice = input("  Enable Clock Overlay? (y/n) [n]: ").strip().lower() or "n"
            ntp_on = ntp_choice in ("y", "yes")
            for b in branches_to_configure:
                if b != "third":
                    params[b]["ntp_overlay"] = ntp_on

        lens_q = urllib.parse.quote(lens_id)
        result = self._request(f"/lenses/{lens_q}/config", method="PATCH", data=params, timeout=30)
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return

        print(f"\n  ✓ Lens '{lens_id}' configured!")
        branches = result.get("branches", {})
        for tier, info in branches.items():
            state = info.get('state', '?')
            state_icon = {"unchanged": "═", "updated": "↻", "new": "✦", "rebuilt": "⟳"}.get(state, "?")
            line = f"    {state_icon} {tier:5s} → {info.get('mount', '?')}  "
            line += f"[{info.get('resolution', '?')}, {info.get('fps', '?')}fps, {info.get('encoder', '?')}]"
            print(line)
        print(f"\n  RTSP base: rtsp://{self.host}:8554")

    def do_recording(self):
        if not self._check_logged_in():
            return
        print("\n── Recording ──")
        print("    1. Start Continuous Recording")
        print("    2. Start AI-Triggered Recording (Event Mode)")
        print("    3. Start Scheduled Recording")
        print("    4. Stop Recording")
        choice = input("  Select (1-4): ").strip()

        lens = input("  Lens (lens1/lens2) [lens1]: ").strip() or "lens1"

        if choice == "1":
            params = {"lens": lens, "branch": "main", "mode": "continuous", "output_dir": "/data"}
            print("\n  (Press Enter to skip any parameter and use defaults)")

            fps_str = input("  New FPS [skip]: ").strip()
            if fps_str and fps_str.isdigit(): params["fps"] = int(fps_str)

            print("  ── Resolution ──\n    1. 1080p\n    2. 720p\n    3. 480p\n    4. Custom\n    5. Skip")
            r_idx = input("  Select (1-5) [5]: ").strip() or "5"
            r_map = {"1": "1920x1080", "2": "1280x720", "3": "640x480"}
            if r_idx in r_map: params["resolution"] = r_map[r_idx]
            elif r_idx == "4":
                res = input("  Enter WxH: ").strip()
                if res: params["resolution"] = res

            print("  ── Encoder ──\n    1. H.264\n    2. H.265\n    3. Skip")
            e_idx = input("  Select (1-3) [3]: ").strip() or "3"
            e_map = {"1": "h264", "2": "h265"}
            if e_idx in e_map: params["encoder"] = e_map[e_idx]

            lens_q = urllib.parse.quote(lens)
            result = self._request(f"/lenses/{lens_q}/recording/main", method="POST", data=params)

        elif choice == "2":
            # AI-Triggered Event Recording
            print("\n  ── AI-Triggered Recording (Event) ──")
            print("  AI recording automatically saves 720p 25fps H.264 files when AI detections occur.")
            params = {
                "lens": lens,
                "branch": "ai",
                "mode": "event",
                "output_dir": "/data"
            }
            timeout_str = input("  Idle Timeout Secs (stop recording if no events) [120]: ").strip()
            if timeout_str and timeout_str.isdigit(): params["idle_timeout"] = int(timeout_str)
            
            lens_q = urllib.parse.quote(lens)
            result = self._request(f"/lenses/{lens_q}/recording/ai", method="POST", data=params)

        elif choice == "3":
            # Scheduled Recording
            print("\n  ── Scheduled Recording ──")
            print("    1. Once (Date & Time)")
            print("    2. Daily (Time range)")
            s_idx = input("  Select Schedule Type (1-2) [1]: ").strip() or "1"
            
            branch = input("  Branch to schedule (main/sub) [main]: ").strip() or "main"
            params = {
                "lens": lens,
                "branch": branch,
                "mode": "scheduled",
                "output_dir": "/data"
            }
            
            if s_idx == "2":
                params["schedule_type"] = "daily"
                start_time = input("  Start Time (HH:MM:SS) [10:00:00]: ").strip() or "10:00:00"
                end_time = input("  End Time (HH:MM:SS) [12:00:00]: ").strip() or "12:00:00"
                params["start_time"] = start_time
                params["end_time"] = end_time
            else:
                params["schedule_type"] = "once"
                import datetime
                now = datetime.datetime.now()
                end = now + datetime.timedelta(hours=1)
                def_start = now.strftime("%Y-%m-%d %H:%M:%S")
                def_end = end.strftime("%Y-%m-%d %H:%M:%S")
                start_time = input(f"  Start Time (YYYY-MM-DD HH:MM:SS) [{def_start}]: ").strip() or def_start
                end_time = input(f"  End Time (YYYY-MM-DD HH:MM:SS) [{def_end}]: ").strip() or def_end
                params["schedule_start_time"] = start_time
                params["schedule_end_time"] = end_time

            lens_q = urllib.parse.quote(lens)
            branch_q = urllib.parse.quote(branch)
            result = self._request(f"/lenses/{lens_q}/recording/{branch_q}", method="POST", data=params)

        elif choice == "4":
            branch = input("  Branch to stop (main/sub/ai) [main]: ").strip() or "main"
            params = {"lens": lens, "branch": branch}
            lens_q = urllib.parse.quote(lens)
            branch_q = urllib.parse.quote(branch)
            result = self._request(f"/lenses/{lens_q}/recording/{branch_q}", method="DELETE", data=params)
        else:
            print("  ✗ Invalid choice.")
            return

        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return
        print(f"  ✓ {json.dumps(result, indent=2)}")

    def do_snapshot(self):
        if not self._check_logged_in():
            return
        print("\n── Take Snapshot ──")
        lens = input("  Lens (lens1/lens2) [lens1]: ").strip() or "lens1"
        lens_q = urllib.parse.quote(lens)
        result = self._request(f"/lenses/{lens_q}/snapshot", method="POST")
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return
        print(f"  ✓ Snapshot saved: {result.get('file', '?')}")

    def do_dynamic_update(self):
        if not self._check_logged_in():
            return
        print("\n── Dynamic Stream Update (Live) ──")
        print("  (Changes apply instantly to running streams — no reconnect needed)")
        print("  NOTE: Only main/sub branches support dynamic updates.")
        lens = input("  Lens (lens1/lens2) [lens1]: ").strip() or "lens1"
        print("    1. main")
        print("    2. sub")
        try:
            b_idx = input("  Select Branch (1-2) [1]: ").strip() or "1"
            branch = ["main", "sub"][int(b_idx)-1]
        except (ValueError, IndexError):
            print("  ✗ Invalid branch choice.")
            return

        params = {}
        print("\n  (Press Enter to skip any parameter you don't want to change)")

        # FPS
        fps_str = input("  New FPS [skip]: ").strip()
        if fps_str and fps_str.isdigit():
            params["fps"] = int(fps_str)

        # Resolution
        print("  ── Resolution ──")
        print("    1. 1080p  (1920x1080)")
        print("    2. 720p   (1280x720)")
        print("    3. 480p   (640x480)")
        print("    4. Custom")
        print("    5. Skip")
        r_idx = input("  Select (1-5) [5]: ").strip() or "5"
        r_map = {"1": "1920x1080", "2": "1280x720", "3": "640x480"}
        if r_idx in r_map:
            params["resolution"] = r_map[r_idx]
        elif r_idx == "4":
            res = input("  Enter WxH: ").strip()
            if res:
                params["resolution"] = res

        # Bitrate
        print("  ── Bitrate ──")
        print("    1. 4 Mbps")
        print("    2. 2 Mbps")
        print("    3. 1 Mbps")
        print("    4. Custom")
        print("    5. Skip")
        br_idx = input("  Select (1-5) [5]: ").strip() or "5"
        br_map = {"1": 4000000, "2": 2000000, "3": 1000000}
        if br_idx in br_map:
            params["bitrate"] = br_map[br_idx]
        elif br_idx == "4":
            cust = input("  Enter bitrate in bps: ").strip()
            if cust.isdigit():
                params["bitrate"] = int(cust)

        # NTP Overlay
        print("  ── NTP Clock Overlay ──")
        print("    1. Enable")
        print("    2. Disable")
        print("    3. Skip")
        n_idx = input("  Select (1-3) [3]: ").strip() or "3"
        if n_idx == "1":
            params["ntp_overlay"] = True
        elif n_idx == "2":
            params["ntp_overlay"] = False

        if not params:
            print("  ✗ No parameters selected. Nothing to update.")
            return

        lens_q = urllib.parse.quote(lens)
        branch_q = urllib.parse.quote(branch)
        result = self._request(f"/lenses/{lens_q}/streams/{branch_q}/params", method="PATCH", data=params)
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return
        print(f"  ✓ {json.dumps(result, indent=2)}")

    def do_set_log_level(self):
        if not self._check_logged_in():
            return
        print("\n── Set Log Level ──")
        print("  1. DEBUG")
        print("  2. INFO")
        print("  3. WARNING")
        print("  4. ERROR")
        lchoice = input("  Select (1-4) [2]: ").strip() or "2"
        level_map = {"1": "DEBUG", "2": "INFO", "3": "WARNING", "4": "ERROR"}
        level = level_map.get(lchoice, "INFO")

        result = self._request("/system/log_level", method="PUT", data={"level": level})
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return
        print(f"  ✓ Log level set to {level}")

    def do_logout(self):
        if os.path.exists(TOKEN_FILE):
            os.remove(TOKEN_FILE)
        self.token = None
        self.user = None
        self.role = None
        print("\n  ✓ Logged out.")

    def do_ai_load_model(self):
        if not self._check_logged_in():
            return
        print("\n── AI: Load Model ──")
        lens = input("  Lens (lens1/lens2) [lens1]: ").strip() or "lens1"
        print("  Available models:")
        print("    1. helmet")
        print("    2. triple_riding")
        print("    3. overspeed")
        print("    4. person")
        m_idx = input("  Select (1-4) [1]: ").strip() or "1"
        m_map = {"1": "helmet", "2": "triple_riding", "3": "overspeed", "4": "person"}
        model = m_map.get(m_idx, "helmet")
        result = self._request("/ai/load", method="POST", data={"lens": lens, "model": model}, timeout=15)
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return
        print(f"  ✓ {json.dumps(result, indent=2)}")

    def do_ai_stop_model(self):
        if not self._check_logged_in():
            return
        print("\n── AI: Stop Model ──")
        lens = input("  Lens (lens1/lens2) [lens1]: ").strip() or "lens1"
        result = self._request("/ai/stop", method="POST", data={"lens": lens})
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return
        print(f"  ✓ {json.dumps(result, indent=2)}")

    def do_ai_config(self):
        if not self._check_logged_in():
            return
        print("\n── AI: Configuration ──")
        lens = input("  Lens (lens1/lens2) [lens1]: ").strip() or "lens1"

        # 1. Global AI Config (Snapshots & Threshold)
        ai_config = {"lens": lens}
        print("\n  (Press Enter to skip any parameter)")

        t_str = input("  Detection Threshold (0.0-1.0) [skip]: ").strip()
        if t_str:
            try: ai_config["threshold"] = float(t_str)
            except ValueError: print("  ✗ Invalid threshold value")

        st_str = input("  Snapshot/Recording Threshold (0.0-1.0) [skip]: ").strip()
        if st_str:
            try: ai_config["snapshot_threshold"] = float(st_str)
            except ValueError: print("  ✗ Invalid snapshot threshold value")

        print("  AI Snapshots:")
        print("    1. Enable")
        print("    2. Disable")
        print("    3. Skip")
        s_idx = input("  Select (1-3) [3]: ").strip() or "3"
        if s_idx == "1": ai_config["snapshots"] = True
        elif s_idx == "2": ai_config["snapshots"] = False

        if len(ai_config) > 1:
            res = self._request("/ai/config", method="POST", data=ai_config)
            if "_request_error" in res:
                print(f"  ✗ AI Config request failed: {res['_request_error']}")
            elif "_http_error" in res:
                print(f"  ✗ AI Config error: {res.get('error','')}")
            else:
                print("  ✓ AI settings updated.")

        # 2. AI-Triggered Recording (Linking AI Alerts to Recorder)
        print("\n  ── AI-Triggered Recording ──")
        print("    1. Enable (Arm recorder on Violation Alerts)")
        print("    2. Disable (Stop Event Recorder)")
        print("    3. Skip")
        r_idx = input("  Select (1-3) [3]: ").strip() or "3"
        
        if r_idx == "1":
            # Arm the recorder in 'event' mode
            rec_params = {
                "branch": "main", 
                "mode": "event", 
                "output_dir": "/data",
                "idle_timeout": 60,
                "max_segment_sec": 300
            }
            lens_q = urllib.parse.quote(lens)
            res = self._request(f"/lenses/{lens_q}/recording", method="POST", data=rec_params)
            if "_request_error" in res:
                print(f"  ✗ Failed to arm AI recording (request error): {res['_request_error']}")
            elif "_http_error" in res:
                print(f"  ✗ Failed to arm AI recording: {res.get('error','')}")
            else:
                print(f"  ✓ AI-Triggered Recording ARMED for {lens} (waiting for alerts).")
        elif r_idx == "2":
            lens_q = urllib.parse.quote(lens)
            res = self._request(f"/lenses/{lens_q}/recording", method="DELETE")
            if "_request_error" in res:
                print(f"  ✗ Failed to disarm AI recording (request error): {res['_request_error']}")
            elif "_http_error" in res:
                print(f"  ✗ Failed to disarm AI recording: {res.get('error','')}")
            else:
                print(f"  ✓ AI-Triggered Recording DISARMED for {lens}.")

        # 2. Per-Stream Overlay Toggle
        print("\n  ── Bounding Box Overlay (Per-Stream) ──")
        print("    1. Enable overlay")
        print("    2. Disable overlay")
        print("    3. Skip")
        o_idx = input("  Select (1-3) [3]: ").strip() or "3"

        if o_idx in ["1", "2"]:
            enabled = (o_idx == "1")
            print("\n    Target Branch:")
            print("      1. All Branches (main + sub)")
            print("      2. main only")
            print("      3. sub only")
            b_idx = input("    Select (1-3) [1]: ").strip() or "1"
            branch = None
            if b_idx == "2": branch = "main"
            elif b_idx == "3": branch = "sub"

            overlay_data = {"enabled": enabled, "lens": lens}
            if branch: overlay_data["branch"] = branch

            lens_q = urllib.parse.quote(lens)
            result = self._request(f"/lenses/{lens_q}/overlay", method="PATCH", data=overlay_data)
            if "_request_error" in result:
                print(f"  ✗ Overlay request failed: {result['_request_error']}")
            elif "_http_error" in result:
                print(f"  ✗ Overlay error: {result.get('error', '')}")
            else:
                target = branch if branch else "all streams"
                count = result.get("count", "?")
                print(f"  ✓ Overlay {'ON' if enabled else 'OFF'} for {target} ({count} overlays affected)")

    def do_ai_status(self):
        if not self._check_logged_in():
            return
        print("\n── AI: Status ──")
        result = self._request("/ai/status")
        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return
        print(f"  Model   : {result.get('current_model', 'None')}")
        print(f"  Lens    : {result.get('current_lens', 'None')}")
        print(f"  Threshold: {result.get('ai_threshold', '?')}")
        print(f"  Snapshots: {'Enabled' if result.get('snapshots_enabled') else 'Disabled'}")

    def do_print_urls(self):
        if not self._check_logged_in():
            return
        print("\n── Active Stream URLs ──")
        result = self._request("/system/status")

        if "_request_error" in result:
            print(f"  ✗ Request failed: {result['_request_error']}")
            return
        if "_http_error" in result:
            print(f"  ✗ Error {result['_http_error']}: {result.get('error', '')}")
            return

        lenses = result.get("lenses", {})
        if not lenses:
            print("  No active lenses found. Configure a lens first.")
            return

        scheme = "https" if self.use_tls else "http"
        for lid, lcfg in lenses.items():
            print(f"\n  [ {lid.upper()} ]")
            branches = lcfg.get("branches", {})
            for tier, bcfg in branches.items():
                print(f"    {tier}:")
                mount = bcfg.get('mount', '').lstrip('/')
                print(f"      RTSP : rtsp://{self.host}:8554/{mount}")
                lid_q = urllib.parse.quote(lid)
                tier_q = urllib.parse.quote(tier)
                hls_url = f"{scheme}://{self.host}:{self.port}/api/v1/stream/{lid_q}/{tier_q}/playlist.m3u8?token={self.token}"
                print(f"      HLS  : {hls_url}")
        print()

        print()
    
    # ── Sub-Menus ──────────────────────────────────────────────────────
    
    def user_menu(self):
        while True:
            print("\n── User Management ──")
            print("  1. List Users")
            print("  2. Add User")
            print("  3. Delete User")
            print("  0. Back")
            c = input("\n  Select: ").strip()
            if c == "1":
                res = self._request("/users")
                print(json.dumps(res, indent=2))
            elif c == "2":
                u = input("  Username: ").strip()
                p = input("  Password: ").strip()
                r = input("  Role (admin/operator/viewer): ").strip()
                e = input("  Email: ").strip()
                res = self._request("/users", method="POST", data={"username":u,"password":p,"role":r,"email":e})
                print(json.dumps(res, indent=2))
            elif c == "3":
                u = input("  Username to delete: ").strip()
                res = self._request(f"/users/{u}", method="DELETE")
                print(json.dumps(res, indent=2))
            elif c == "0": break

    def device_menu(self):
        while True:
            print("\n── Device Control ──")
            print("  1. Reboot")
            print("  2. Factory Reset (Wipe All)")
            print("  3. Network Reset")
            print("  4. Config Reset (Keep Users)")
            print("  0. Back")
            c = input("\n  Select: ").strip()
            if c == "1":
                self._request("/device/reboot", method="POST")
                print("  ✓ Reboot command sent.")
            elif c == "2":
                self._request("/device/factory-reset", method="POST", data={"confirm": True})
                print("  ✓ Factory reset initiated.")
            elif c == "3":
                self._request("/device/network-reset", method="POST")
                print("  ✓ Network reset initiated.")
            elif c == "0": break

    def security_menu(self):
        while True:
            print("\n── Security & 2FA ──")
            print("  1. Setup TOTP (Authenticator App)")
            print("  2. Change Password")
            print("  3. Forgot Password / Reset")
            print("  0. Back")
            c = input("\n  Select: ").strip()
            if c == "1":
                res = self._request("/auth/setup-totp", method="POST")
                if "qr_uri" in res:
                    print(f"\n  ✓ TOTP Setup Initiated!")
                    print(f"  QR URI: {res['qr_uri']}")
                    print("  Scan this in Google Authenticator / Authy.")
                else: print(json.dumps(res, indent=2))
            elif c == "2":
                old = input("  Old Password: ").strip()
                new = input("  New Password: ").strip()
                res = self._request("/auth/change-password", method="POST", data={"old_password":old, "new_password":new})
                print(json.dumps(res, indent=2))
            elif c == "0": break

    def smtp_menu(self):
        while True:
            print("\n── SMTP Alerts Configuration ──")
            print("  1. View Current Settings")
            print("  2. Update Settings")
            print("  0. Back")
            c = input("\n  Select: ").strip()
            if c == "1":
                res = self._request("/system/smtp")
                print(json.dumps(res, indent=2))
            elif c == "2":
                cfg = {}
                h = input("  Host [skip]: ").strip(); 
                if h: cfg["host"] = h
                p = input("  Port [skip]: ").strip(); 
                if p: cfg["port"] = int(p)
                u = input("  User [skip]: ").strip(); 
                if u: cfg["user"] = u
                pw = input("  Pass [skip]: ").strip(); 
                if pw: cfg["pass"] = pw
                f = input("  From Email [skip]: ").strip(); 
                if f: cfg["from"] = f
                ssl_c = input("  Use SSL? (y/n) [y]: ").strip().lower()
                if ssl_c: cfg["use_ssl"] = (ssl_c != "n")
                res = self._request("/system/smtp", method="PUT", data=cfg)
                print(json.dumps(res, indent=2))
            elif c == "0": break

    # ── Main Menu Loop ────────────────────────────────────────────────

    def run(self):
        print("═" * 50)
        print("  SMART IP Edge Camera CLI")
        print(f"  Server: {self.base_url}")
        print("═" * 50)

        while True:
            if self.token:
                status = f"Logged in as '{self.user}' ({self.role})"
            else:
                status = "Not logged in"

            print(f"\n  [{status}]")
            print()
            print("  1. Login")
            print("  2. Health Check")
            print("  3. Engine Status")
            print("  4. Configure Lens (Setup Streams)")
            print("  5. Dynamic Stream Update (Live FPS/Res/Bitrate)")
            print("  6. Recording (Start/Stop)")
            print("  7. Take Snapshot")
            print("  8. Set Log Level")
            print("  9. Print Stream URLs")
            print(" 10. AI: Load Model")
            print(" 11. AI: Stop Model")
            print(" 12. AI: Config (Threshold/Overlay)")
            print(" 13. AI: Status")
            print(" 14. User Management")
            print(" 15. Device Control")
            print(" 16. Security & 2FA")
            print(" 17. SMTP Alerts Config")
            print(" 18. Logout")
            print("  0. Exit")
            print()

            try:
                choice = input("  Select option: ").strip()
            except (EOFError, KeyboardInterrupt):
                print("\n  Goodbye!")
                break

            actions = {
                "1": self.do_login,
                "2": self.do_health,
                "3": self.do_status,
                "4": self.do_configure_lens,
                "5": self.do_dynamic_update,
                "6": self.do_recording,
                "7": self.do_snapshot,
                "8": self.do_set_log_level,
                "9": self.do_print_urls,
                "10": self.do_ai_load_model,
                "11": self.do_ai_stop_model,
                "12": self.do_ai_config,
                "13": self.do_ai_status,
                "14": self.user_menu,
                "15": self.device_menu,
                "16": self.security_menu,
                "17": self.smtp_menu,
                "18": self.do_logout,
            }

            if choice == "0":
                print("\n  Goodbye!")
                break
            elif choice in actions:
                actions[choice]()
            else:
                print("\n  ✗ Invalid option. Try again.")


# ── Entry Point ───────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="SMART IP Edge Camera CLI")
    parser.add_argument("--host", default=DEFAULT_HOST, help="Control Plane host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=None, help="Control Plane port")
    parser.add_argument("--no-tls", action="store_true", help="Use HTTP instead of HTTPS")
    args = parser.parse_args()

    use_tls = not args.no_tls
    port = args.port or (DEFAULT_HTTPS_PORT if use_tls else DEFAULT_HTTP_PORT)

    cli = SmartIPCLI(host=args.host, port=port, use_tls=use_tls)
    cli.run()


if __name__ == "__main__":
    main()
