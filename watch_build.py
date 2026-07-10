import os, sys, time, json, zipfile, shutil, urllib.request

SHA = "1507499defdc9545a6e098c4b533c18fe3979fc5"
TAG = "v0.1.53"
EXTRACT = os.path.expanduser("~/car53")
REPO = "clashrauto/clashauto"

def api(path):
    req = urllib.request.Request("https://api.github.com" + path)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "clashwatch")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)

def log(msg):
    print(msg, flush=True)

log(f"watching runs for {SHA[:7]} -> {TAG}")
deadline = time.time() + 60 * 25
while time.time() < deadline:
    try:
        data = api(f"/repos/{REPO}/actions/runs?head_sha={SHA}&per_page=20")
    except Exception as e:
        log(f"poll err: {e}"); time.sleep(30); continue
    runs = data.get("workflow_runs", [])
    if not runs:
        log("no runs yet"); time.sleep(30); continue
    alldone = all(r["status"] == "completed" for r in runs)
    log("  " + " | ".join(f"{r['name']}:{r['status']}/{r['conclusion']}" for r in runs))
    if alldone:
        break
    time.sleep(40)

log("fetching release " + TAG)
rel = None
for _ in range(20):
    try:
        rel = api(f"/repos/{REPO}/releases/tags/{TAG}"); break
    except Exception as e:
        log(f"release not ready: {e}"); time.sleep(20)
if not rel:
    log("RELEASE NOT FOUND"); sys.exit(1)

asset = None
for a in rel["assets"]:
    n = a["name"].lower()
    if "win" in n and ("x64" in n or "amd64" in n) and "portable" in n and n.endswith(".zip"):
        asset = a; break
if not asset:
    for a in rel["assets"]:
        n = a["name"].lower()
        if "win" in n and ("x64" in n or "amd64" in n) and n.endswith(".zip"):
            asset = a; break
if not asset:
    log("assets: " + ", ".join(a["name"] for a in rel["assets"]))
    log("NO MATCHING ASSET"); sys.exit(1)

log("downloading " + asset["name"])
zpath = os.path.expanduser("~/car53.zip")
req = urllib.request.Request(asset["browser_download_url"]); req.add_header("User-Agent", "clashwatch")
with urllib.request.urlopen(req, timeout=300) as r, open(zpath, "wb") as f:
    f.write(r.read())

if os.path.isdir(EXTRACT):
    shutil.rmtree(EXTRACT, ignore_errors=True)
os.makedirs(EXTRACT, exist_ok=True)
with zipfile.ZipFile(zpath) as z:
    z.extractall(EXTRACT)
log("extracted to " + EXTRACT)
log("DONE")
