"""iotdash store ...: the app store that comes with the SDK (store/, see store/README.md).

  iotdash store dev [--api | --web]        the API (:9310) and the web app (:5310) on this machine
  iotdash store test                       the API's tests
  iotdash store deploy [--dry-run] [--config FILE]   your store on AWS (store/scripts/deploy.sh)
  iotdash store deploy-web [--config FILE]           its web app (store/scripts/deploy-web.sh)
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import iotdash as I

STORE = I.ROOT / "store"
BACKEND = STORE / "backend"
FRONTEND = STORE / "frontend"


def _python313():
    for c in ("python3.13", "/opt/homebrew/opt/python@3.13/bin/python3.13"):
        p = shutil.which(c) or (c if Path(c).exists() else None)
        if p:
            return p
    raise SystemExit("the store's API needs Python 3.13 (the Lambda runtime)")


def ensure_api_env():
    """backend/.venv with the local requirements (run_local.sh does the same when it starts)."""
    py = BACKEND / ".venv" / "bin" / "python"
    if not py.exists():
        print("store: setting up the API's Python environment (backend/.venv)")
        subprocess.run([_python313(), "-m", "venv", str(BACKEND / ".venv")], check=True)
        subprocess.run([str(py), "-m", "pip", "install", "-q", "-r", "requirements.local.txt"], cwd=BACKEND, check=True)
    return py


def ensure_web_env():
    if not shutil.which("npm"):
        raise SystemExit("the store's web app needs Node.js (npm)")
    if not (FRONTEND / "node_modules").exists():
        print("store: installing the web app's packages (frontend/node_modules)")
        subprocess.run(["npm", "install", "--no-audit", "--no-fund"], cwd=FRONTEND, check=True)


def dev(api=True, web=True):
    if api and not (BACKEND / ".env.local").exists():
        raise SystemExit(f"no {BACKEND / '.env.local'}: cp store/backend/.env.example store/backend/.env.local, then edit it")
    procs = []
    try:
        if api:
            procs.append(subprocess.Popen(["./run_local.sh"], cwd=BACKEND))
        if web:
            ensure_web_env()
            procs.append(subprocess.Popen(["npm", "run", "dev"], cwd=FRONTEND))
        print("store: Ctrl+C stops it", flush=True)
        while all(p.poll() is None for p in procs):
            try:
                procs[0].wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass
    except KeyboardInterrupt:
        pass  # the terminal sent it to the children too: the API saves its data as it exits
    finally:
        for p in procs:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.terminate()


def test(extra=()):
    py = ensure_api_env()
    raise SystemExit(subprocess.run([str(py), "-m", "pytest", "-q", "tests", *extra], cwd=BACKEND).returncode)


def deploy(script, config=None, dry_run=False):
    env = dict(os.environ)
    if config:
        env["IOTSTORE_CONFIG"] = str(Path(config).expanduser().resolve())
    cmd = [str(STORE / "scripts" / script)] + (["--dry-run"] if dry_run else [])
    raise SystemExit(subprocess.run(cmd, env=env).returncode)


def add_commands(sub):
    s = sub.add_parser("store", help="the app store (store/): run it here, test it, deploy it to AWS")
    s.add_argument("what", choices=["dev", "test", "deploy", "deploy-web"])
    s.add_argument("--api", action="store_true", help="dev: only the API")
    s.add_argument("--web", action="store_true", help="dev: only the web app")
    s.add_argument("--dry-run", action="store_true", help="deploy: only print the changes")
    s.add_argument("--config", help="deploy: the store's settings (default store/deploy.env)")


def main(args):
    if args.what == "dev":
        dev(api=not args.web or args.api, web=not args.api or args.web)
    elif args.what == "test":
        test()
    elif args.what == "deploy":
        deploy("deploy.sh", args.config, args.dry_run)
    elif args.what == "deploy-web":
        deploy("deploy-web.sh", args.config)
    else:
        sys.exit(2)
