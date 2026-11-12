#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Update notice test (needs root; meant for GitHub Actions). Runs USBeam's real service (without USB
# drivers) and its real page in a browser, with GitHub's answer about the newest release faked:
#   - a newer version shows the update bar and asks the app for one notification
#   - not more than one notification a day for the same version
#   - "Skip this version" hides it, is saved (also after reopening), and stays quiet for that version
#   - a newer version after that is announced again
#   - "Update now" hands the right download to the app
#   - the version this PC already has: nothing
import json, os, subprocess, sys, time, urllib.request
from playwright.sync_api import sync_playwright

fails = 0
def check(ok, what):
    global fails
    print(("OK: " if ok else "FAIL: ") + what)
    if not ok: fails += 1

os.makedirs("/opt/usbeam", exist_ok=True)
open("/opt/usbeam/installed", "w").write("0.0.0\n")            # as installed from the AppImage
svc = subprocess.Popen(["build/usbeam", "--service"], env=dict(os.environ, USBEAM_UITEST="1"),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
token = ""
for _ in range(50):
    try: token = open("/run/usbeam/ui-token").read().strip()
    except OSError: pass
    if token: break
    time.sleep(0.2)
if not token: print("FAIL: the service didn't start"); svc.kill(); print(svc.stdout.read()); sys.exit(1)

def state():
    req = urllib.request.Request("http://127.0.0.1:3242/api/state", headers={"X-USBeam-Token": token})
    return json.loads(urllib.request.urlopen(req, timeout=5).read())
mine = state()["version"]
print("this build:", mine)

latest = {"v": "9.9.9"}
def release(route):
    v = latest["v"]
    body = {"tag_name": "v" + v, "assets": [
        {"name": f"USBeam-{v}-x86_64.AppImage", "browser_download_url": f"https://github.com/EliSantana1/USBeam/releases/download/v{v}/USBeam-{v}-x86_64.AppImage", "digest": "sha256:" + "ab" * 32},
        {"name": f"USBeam-Setup-{v}.exe", "browser_download_url": f"https://github.com/EliSantana1/USBeam/releases/download/v{v}/USBeam-Setup-{v}.exe", "digest": "sha256:" + "cd" * 32}]}
    route.fulfill(status=200, content_type="application/json", body=json.dumps(body))

with sync_playwright() as p:
    b = p.chromium.launch()
    page = b.new_page()
    page.route("https://api.github.com/**", release)
    def open_page():
        page.goto(f"http://127.0.0.1:3242/?t={token}")
        page.wait_for_function("typeof S !== 'undefined' && S && S.version")
        page.evaluate("window.__links = []; window.appLink = (w, p) => window.__links.push([w, p || {}]);")
    def check_now():
        page.evaluate("autoUpdates()")
        page.wait_for_timeout(800)
    bar_text = lambda: page.evaluate("document.getElementById('updbar').hidden ? '' : document.getElementById('updbar').innerText")
    notes = lambda: [l for l in page.evaluate("window.__links") if l[0] == "notify"]

    print("1. A newer version")
    open_page(); check_now()
    check("USBeam 9.9.9 is available" in bar_text(), "the update bar shows it")
    check(notes() == [["notify", {"version": "9.9.9"}]], "the app is asked for one notification")
    check_now()
    check(len(notes()) == 1, "no second notification for it the same day")

    print("2. Update now")
    page.click("#updnow")
    ups = [l for l in page.evaluate("window.__links") if l[0] == "update"]
    check(len(ups) == 1 and ups[0][1].get("version") == "9.9.9" and ups[0][1].get("url", "").endswith("USBeam-9.9.9-x86_64.AppImage")
          and ups[0][1].get("sha256") == "ab" * 32, "the app gets this PC's download, with its checksum")

    print("3. Skip this version")
    open_page(); check_now()
    page.click("#updskip")
    page.wait_for_timeout(1500)
    page.evaluate("window.__links = []")
    check(bar_text() == "", "the bar goes away")
    check(state()["prefs"]["skip_version"] == "9.9.9", "the choice is saved")
    check_now()
    check(bar_text() == "" and not notes(), "that version stays quiet")
    open_page(); check_now()
    check(bar_text() == "" and not notes(), "also after opening USBeam again")

    print("4. A newer version after a skipped one")
    latest["v"] = "9.9.10"
    check_now()
    check("USBeam 9.9.10 is available" in bar_text(), "the bar is back for it")
    check(notes() == [["notify", {"version": "9.9.10"}]], "and it is announced")

    print("5. The version this PC already has")
    latest["v"] = mine
    open_page(); check_now()
    check(bar_text() == "" and not notes(), "nothing is shown")

    print("6. Gear menu → Check for updates still shows a skipped version")
    latest["v"] = "9.9.9"
    page.evaluate("checkUpdates()")
    page.wait_for_timeout(800)
    check("9.9.9 is available" in page.evaluate("document.getElementById('updstatus').innerText"), "it's offered there")
    b.close()

svc.terminate()
try: svc.wait(10)
except subprocess.TimeoutExpired: svc.kill()
print("PASS: update notice test" if not fails else "Update notice test FAILED")
sys.exit(1 if fails else 0)
