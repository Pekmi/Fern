import io
import sys
import getpass
import paramiko
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

HOST     = "pekmi.me"
USER     = "ubuntu"
REMOTE   = "/home/ubuntu/fern-cloud"
LOCAL    = Path(__file__).parent

def run(ssh, cmd):
    _, stdout, stderr = ssh.exec_command(cmd)
    exit_code = stdout.channel.recv_exit_status()
    out = stdout.read().decode("utf-8", errors="replace").strip()
    err = stderr.read().decode("utf-8", errors="replace").strip()
    return exit_code, out, err

def upload_file(ssh, local_path: Path, remote_path: str):
    data = local_path.read_bytes()
    remote_dir = remote_path.rsplit("/", 1)[0]
    run(ssh, f"mkdir -p {remote_dir}")

    transport = ssh.get_transport()
    chan = transport.open_session()
    chan.exec_command(f"cat > {remote_path}")
    chan.sendall(data)
    chan.shutdown_write()
    exit_code = chan.recv_exit_status()
    chan.close()
    if exit_code != 0:
        raise RuntimeError(f"Upload failed for {remote_path} (code {exit_code})")

def main():
    password = getpass.getpass(prompt=f"Password for {USER}@{HOST}: ")
    print(f"Connecting to {USER}@{HOST}...")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(HOST, username=USER, password=password, timeout=15)

    files_to_upload = [
        "package.json",
        "index.js",
        ".env"
    ]

    print("\nUploading files...")
    for filename in files_to_upload:
        local_path = LOCAL / filename
        if not local_path.exists():
            print(f"  [SKIP] {filename} (not found locally)")
            continue
            
        remote_path = f"{REMOTE}/FernCloudServer/{filename}"
        upload_file(ssh, local_path, remote_path)
        print(f"  ✓ {filename}")

    print("\nInstalling dependencies and restarting PM2...")
    cmd = f"cd {REMOTE}/FernCloudServer && npm install && pm2 restart fern-cloud || pm2 start index.js --name fern-cloud"
    code, out, err = run(ssh, cmd)
    
    if code == 0:
        print("  ✓ Server restarted successfully")
    else:
        print(f"  ✗ Issue restarting server (code {code})")
        if out: print(f"    stdout: {out}")
        if err: print(f"    stderr: {err}")

    ssh.close()
    print("\nDeployment complete.")

if __name__ == "__main__":
    main()