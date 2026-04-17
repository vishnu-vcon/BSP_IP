#!/usr/bin/env python3
import subprocess
import json
import time
import os
import sys

# D-Bus Constants
BUS_NAME = "com.camera.UnifiedEngine"
BUS_PATH = "/com/camera/UnifiedEngine"
IFACE    = "com.camera.UnifiedEngine"
CONFIG_PATH = "config/user_config.json"

def run_dbus(method, params_json):
    """Executes a gdbus call and returns the response string."""
    cmd = [
        "gdbus", "call", "--session",
        "--dest", BUS_NAME,
        "--object-path", BUS_PATH,
        "--method", f"{IFACE}.{method}",
        params_json
    ]
    try:
        result = subprocess.check_output(cmd, stderr=subprocess.STDOUT).decode('utf-8')
        # gdbus returns ('{"status":"..."}',) - we need to extract the string
        if "('" in result:
            return result.split("('")[1].split("',")[0].replace("\\\"", "\"")
        return result
    except subprocess.CalledProcessError as e:
        return f"Error: {e.output.decode('utf-8')}"

def check_persistence(lens, tier, expected_codec, expected_res):
    """Checks if the JSON file matches expectations."""
    if not os.path.exists(CONFIG_PATH):
        return False, "Config file missing"
    
    with open(CONFIG_PATH, 'r') as f:
        data = json.load(f)
        try:
            branch = data['user_overrides'][lens][tier]
            if branch['encoder'] == expected_codec and branch['resolution'] == expected_res:
                return True, "Match"
            return False, f"Mismatch: Found {branch['encoder']} {branch['resolution']}"
        except KeyError:
            return False, "Key missing in JSON"

def test_reconfiguration(lens, tier, codec, res):
    print(f"[*] Testing {lens}/{tier} -> {codec} ({res})... ", end="", flush=True)
    
    # 1. Send Request
    payload = json.dumps({lens: {tier: {"encoder": codec, "resolution": res}}})
    resp_raw = run_dbus("ConfigureLens", f"'{payload}'")
    
    # 2. Verify D-Bus Response
    try:
        resp = json.loads(resp_raw)
        if resp.get("status") != "configured":
            print(f"FAILED (D-Bus error: {resp.get('message')})")
            return False
    except:
        print(f"FAILED (Invalid Response: {resp_raw})")
        return False

    # 3. Verify Persistence
    ok, msg = check_persistence(lens, tier, codec, res)
    if ok:
        print("PASSED")
    else:
        print(f"FAILED (Persistence: {msg})")
    return ok

if __name__ == "__main__":
    print("=== SmartIP Engine Real-Time Validator ===")
    
    # Test 1: Codec Swap (Static Rebuild Path)
    s1 = test_reconfiguration("lens1", "sub", "h265", "1280x720")
    time.sleep(1)
    
    # Test 2: Resolution Change (Dynamic or Static Path)
    s2 = test_reconfiguration("lens1", "sub", "h265", "1920x1080")
    time.sleep(1)
    
    # Test 3: Overlay Toggle
    print("[*] Testing Overlay Toggle... ", end="", flush=True)
    payload = json.dumps({"lens1": {"sub": {"overlay": False}}})
    run_dbus("ConfigureLens", f"'{payload}'")
    with open(CONFIG_PATH, 'r') as f:
        data = json.load(f)
        if data['user_overrides']['lens1']['sub']['overlay'] == False:
            print("PASSED")
        else:
            print("FAILED")

    print("\n--- Validation Complete ---")
