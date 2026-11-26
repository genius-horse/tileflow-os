#!/usr/bin/env python3
"""Exercise the built TileFlow server and optionally compare its scheduling policies."""

import argparse
from contextlib import contextmanager
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import random
import signal
import socket
import statistics
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zlib


ROOT = Path(__file__).resolve().parents[1]
DEMO_DELAY = 600
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(label, predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = predicate()
        if found:
            return found
        time.sleep(0.025)
    raise RuntimeError(f"Timed out waiting for {label}")


def png(width, height, seed):
    stride = width * 3
    count = stride * height
    pixels = random.Random(seed).getrandbits(8 * count).to_bytes(count, "little")
    rows = b"".join(b"\x00" + pixels[i:i + stride] for i in range(0, count, stride))

    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload)))

    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 1)) + chunk(b"IEND", b""))


def image_size(data):
    check(data.startswith(PNG_SIGNATURE) and data[12:16] == b"IHDR", "Expected a PNG image")
    return struct.unpack(">II", data[16:24])


class Client:
    def __init__(self, port):
        self.url = f"http://127.0.0.1:{port}"
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
        )

    def call(self, path, *, method="GET", body=None, headers=None, status=200):
        request = urllib.request.Request(self.url + path, data=body,
                                         headers=headers or {}, method=method)
        try:
            response = self.opener.open(request, timeout=15)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read()
            check(response.code == status,
                  f"{method} {path} returned HTTP {response.code}; expected {status}: {data[:240]!r}")
            return data

    def login(self, name, *, password=None, status=200):
        body = json.dumps({"username": name, "password": password or name + "-demo"}).encode()
        return json.loads(self.call("/api/login", method="POST", body=body,
                                    headers={"Content-Type": "application/json"}, status=status))

    def submit(self, filename, image, op, priority):
        boundary = "tileflow-" + uuid.uuid4().hex
        fields = []
        for name, value in [("op", op), ("priority", priority)]:
            fields.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
                          f'\r\n\r\n{value}\r\n'.encode())
        fields.append(f'--{boundary}\r\nContent-Disposition: form-data; name="image"; '
                      f'filename="{filename}"\r\nContent-Type: image/png\r\n\r\n'.encode())
        fields.extend((image, f"\r\n--{boundary}--\r\n".encode()))
        return json.loads(self.call("/api/jobs", method="POST", body=b"".join(fields),
                                    headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
                                    status=201))["id"]

    def state(self, since=0):
        return json.loads(self.call(f"/api/state?since={since}"))


def available_port():
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        return reserved.getsockname()[1]


def stop_process(process):
    if process.poll() is None:
        process.terminate()
    try:
        return process.wait(timeout=15)
    except subprocess.TimeoutExpired as error:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        raise RuntimeError("Server did not stop after SIGTERM") from error


@contextmanager
def server(binary, web, disk, delay, policy="priority"):
    port = available_port()
    environ = os.environ.copy()
    for name in ("admin", "alice", "bob"):
        environ[f"TILEFLOW_{name.upper()}_PASSWORD"] = name + "-demo"
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            [str(binary), "--image", str(disk), "--web", str(web), "--port", str(port),
             "--workers", "4", "--tile-size", "256", "--demo-delay-ms", str(delay), "--policy", policy],
            env=environ, cwd=disk.parent, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        failed = False

        def healthy():
            check(process.poll() is None, f"Server exited with status {process.returncode}")
            try:
                return json.loads(Client(port).call("/api/health"))["status"] == "ok"
            except urllib.error.URLError:
                return False

        try:
            wait_for("server health", healthy, timeout=15)
            yield port, process.pid
        except BaseException:
            failed = True
            raise
        finally:
            try:
                status = stop_process(process)
                if not failed:
                    check(status == 0, f"Server shutdown returned status {status}")
            finally:
                log.seek(0, os.SEEK_END)
                log.seek(max(0, log.tell() - 4096))
                output = log.read().decode("utf-8", "replace")
                if failed and output:
                    print(f"Server output:\n{output}", flush=True)


def jobs_by_id(state):
    return {job["id"]: job for job in state["jobs"]}


def matched_event(state, kind, job=None):
    return next((event for event in state["events"]
                 if event["type"] == kind and (job is None or event["job"] == job)), None)


def workers_running(state):
    jobs = jobs_by_id(state)
    active = []
    for worker in state["workers"]:
        if worker["job"] is None:
            continue
        tile = jobs[worker["job"]]["tiles"][worker["tile"]]
        attempt = tile["attempts"][-1]
        check(tile["status"] == "running" and attempt["status"] == "running" and
              tile["pid"] == attempt["pid"] == worker["pid"], "Inconsistent worker assignment in /api/state")
        active.append({**worker, "attempt": attempt["attempt"], "start": attempt["start"]})
    return active


def four_busy_workers(state):
    workers = workers_running(state)
    if len(workers) == 4 and len({worker["pid"] for worker in workers if worker["pid"] > 0}) == 4:
        return workers
    return None


def choose_worker(admin, exclude=()):
    def freshly_assigned():
        workers = sorted(workers_running(admin.state()), key=lambda worker: -worker["start"])
        now = time.time() * 1000
        return next((worker for worker in workers if worker["pid"] not in exclude and
                     now - worker["start"] < DEMO_DELAY / 2), None)

    return wait_for("a busy worker with a new attempt", freshly_assigned)


def ended_attempt(state, worker):
    return next((event for event in state["events"] if event["type"] == "worker_exit" and
                 all(event[k] == worker[k] for k in ("pid", "job", "tile", "attempt"))), None)


def finish(admin, job_ids):
    def all_done():
        state = admin.state()
        jobs = [jobs_by_id(state)[job_id] for job_id in job_ids]
        failures = [job["error"] for job in jobs if job["status"] == "failed"]
        check(not failures, f"Jobs failed: {failures}")
        return state if all(job["status"] == "done" for job in jobs) else None

    return wait_for("all submitted jobs to finish", all_done, timeout=90)


def recovery_evidence(state, worker):
    job, tile, pid, attempt = (worker[k] for k in ("job", "tile", "pid", "attempt"))
    events = [event for event in state["events"] if event["job"] == job and event["tile"] == tile]
    exit_event = ended_attempt(state, worker)
    requeue = next((event for event in events if event["type"] == "requeue" and
                    event["pid"] == pid and event["attempt"] == attempt), None)
    commits = [event for event in events if event["type"] == "commit"]
    check(exit_event is not None and requeue is not None and len(commits) == 1,
          f"Expected a worker exit, requeue and unique commit for job {job} tile {tile}")
    commit = commits[0]
    retry = next((event for event in events if event["type"] == "dispatch" and
                  event["attempt"] == commit["attempt"]), None)
    attempts = jobs_by_id(state)[job]["tiles"][tile]["attempts"]
    check(retry is not None and exit_event["seq"] < requeue["seq"] < retry["seq"] < commit["seq"] and
          commit["pid"] != pid and commit["attempt"] != attempt and
          any(item["attempt"] == attempt and item["status"] == "failed" for item in attempts) and
          attempts[-1]["status"] == "done",
          f"Job {job} tile {tile} did not finish on a new process and attempt")
    return {"exit": exit_event, "requeue": requeue, "retry": retry, "commit": commit, "attempts": attempts}


def run_cli(*args):
    process = subprocess.run(args, capture_output=True, text=True, timeout=45)
    check(process.returncode == 0,
          f"{args[1]} failed with {process.returncode}: {process.stdout}\n{process.stderr}")
    return process.stdout.strip()


def acceptance(binary, web, fixture, directory):
    sample = fixture.read_bytes()
    check(image_size(sample) == (1025, 769), "The acceptance fixture must be the 1025×769 demo PNG")
    small = png(129, 97, 9)
    disk = directory / "acceptance.img"
    with server(binary, web, disk, DEMO_DELAY) as (port, parent):
        clients = {name: Client(port) for name in ("alice", "bob", "admin")}
        alice, bob, admin = (clients[name] for name in ("alice", "bob", "admin"))
        anonymous = Client(port)
        anonymous.call("/api/state", status=401)
        anonymous.login("alice", password="incorrect", status=401)
        for name, client in clients.items():
            role = "admin" if name == "admin" else "user"
            check(client.login(name)["user"] == {"username": name, "role": role}, f"Wrong role for {name}")

        blur = alice.submit("demo-blur.png", sample, "blur", "normal")
        busy = wait_for("four distinct simultaneously busy workers", lambda: four_busy_workers(admin.state()))
        for worker in busy:
            proc_status = Path(f"/proc/{worker['pid']}/status").read_text()
            ppid = next(line.split()[1] for line in proc_status.splitlines() if line.startswith("PPid:"))
            check(int(ppid) == parent, f"PID {worker['pid']} is not a child of the tested server")
        invert = bob.submit("demo-invert.png", sample, "invert", "normal")
        before_high = admin.state()
        pending = {entry["job"] for entry in before_high["queues"]["normal"]}
        check({blur, invert} <= pending and four_busy_workers(before_high),
              "Both normal jobs must have queued work while all four workers are busy")
        check(before_high["disk"]["total_blocks"] > before_high["disk"]["free_blocks"],
              "Admin disk state did not show allocated blocks")
        high = bob.submit("urgent-gray.png", small, "grayscale", "high")

        def overlap():
            state = admin.state()
            running = workers_running(state)
            if high in {worker["job"] for worker in running} and len({worker["job"] for worker in running}) >= 2:
                return state, running
            return None

        overlap_state, overlap_workers = wait_for("high and normal jobs running on distinct workers", overlap)
        high_submitted = matched_event(overlap_state, "submitted", high)
        first_dispatch = next(event for event in overlap_state["events"]
                              if event["type"] == "dispatch" and event["seq"] > high_submitted["seq"])
        check(first_dispatch["job"] == high and overlap_state["queues"]["normal"],
              "The next dispatch must take the high job while normal work remains queued")
        print("Observed four live worker PIDs and next-dispatch priority during a normal backlog", flush=True)

        check({job["id"] for job in alice.state()["jobs"]} == {blur}, "Alice saw another user's jobs")
        bob_view = bob.state()
        check({job["id"] for job in bob_view["jobs"]} == {invert, high} and not bob_view["workers"] and
              all(event["job"] in {invert, high} for event in bob_view["events"]),
              "Bob saw admin worker state or another user's events")
        bob.call(f"/api/jobs/{blur}/source", status=404)
        anonymous.call(f"/api/jobs/{blur}/result", status=401)
        check(alice.call(f"/api/jobs/{blur}/source") == sample and
              admin.call(f"/api/jobs/{invert}/source") == sample, "Authorized source download differs from upload")

        admin_target = choose_worker(admin)
        target_route = f"/api/workers/{admin_target['pid']}/stop"
        alice.call(target_route, method="POST", status=403)
        admin.call(target_route, method="POST", status=202)
        wait_for("admin-stopped attempt to exit", lambda: ended_attempt(admin.state(), admin_target))

        terminal_target = choose_worker(admin, exclude={admin_target["pid"]})
        os.kill(terminal_target["pid"], signal.SIGKILL)
        wait_for("externally killed attempt to exit", lambda: ended_attempt(admin.state(), terminal_target))
        state = finish(admin, (blur, invert, high))
        recovery = {"admin_stop": recovery_evidence(state, admin_target),
                    "external_sigkill": recovery_evidence(state, terminal_target)}
        print("Verified admin stop and external SIGKILL each produced a retry and one committed tile", flush=True)

        submissions = [(blur, alice, "alice", "blur", sample), (invert, bob, "bob", "invert", sample),
                       (high, bob, "bob", "grayscale", small)]
        results = {}
        for job_id, client, owner, operation, source in submissions:
            job = jobs_by_id(state)[job_id]
            tile_ids = [event["tile"] for event in state["events"]
                        if event["job"] == job_id and event["type"] == "commit"]
            check(job["done"] == job["total"] and sorted(tile_ids) == list(range(job["total"])),
                  f"Job {job_id} did not commit each tile exactly once")
            image = client.call(f"/api/jobs/{job_id}/result")
            check(image_size(image) == image_size(source), f"Job {job_id} changed the image dimensions")
            input_path, output_path, serial_path = (directory / f"{job_id}-{suffix}.png"
                                                     for suffix in ("input", "result", "serial"))
            input_path.write_bytes(source)
            output_path.write_bytes(image)
            run_cli(binary, "--serial", operation, input_path, serial_path)
            comparison = run_cli(binary, "--compare", output_path, serial_path)
            results[job_id] = {"owner": owner, "operation": operation, "tiles": job["total"],
                               "sha256": hashlib.sha256(image).hexdigest(), "comparison": comparison}
        bob.call(f"/api/jobs/{blur}/result", status=404)
        bob.call(f"/api/jobs/{blur}/tiles/0", status=404)
        check(image_size(alice.call(f"/api/jobs/{blur}/tiles/0")) == (256, 256), "Owner tile download failed")
        print("Verified owner permissions and pixel equality with serial output for all three operations", flush=True)

    with server(binary, web, disk, 0) as (port, _):
        restored = {name: Client(port) for name in ("alice", "bob", "admin")}
        for name, client in restored.items():
            client.login(name)
        jobs = jobs_by_id(restored["admin"].state())
        for job_id, record in results.items():
            check(jobs[job_id]["status"] == "done", f"Job {job_id} was not restored as done")
            downloaded = restored[record["owner"]].call(f"/api/jobs/{job_id}/result")
            check(hashlib.sha256(downloaded).hexdigest() == record["sha256"],
                  f"Job {job_id} changed after restarting the server")
        restored["bob"].call(f"/api/jobs/{blur}/result", status=404)
    print("Verified downloads and ownership survived restart from the same disk image", flush=True)

    return {"busy_workers": busy, "overlap_workers": overlap_workers, "normal_queue_before_high":
            before_high["queues"]["normal"], "high_submitted": high_submitted,
            "dispatch_after_high": first_dispatch, "recovery": recovery, "results": results,
            "restart_verified": True, "events": state["events"]}


def benchmark(binary, web, directory, repeats):
    load_size = (3073, 2049)
    ordinary_image, urgent_image = png(*load_size, 42), png(129, 97, 9)
    results = {"priority": [], "fifo": []}
    for trial in range(repeats):
        # Alternate order so either policy can benefit from a warmed-up host.
        policies = ("fifo", "priority") if trial % 2 else ("priority", "fifo")
        for policy in policies:
            disk = directory / f"{policy}-{trial}.img"
            with server(binary, web, disk, 0, policy) as (port, _):
                admin, alice, bob = (Client(port) for _ in range(3))
                for name, client in (("admin", admin), ("alice", alice), ("bob", bob)):
                    client.login(name)
                ordinary = alice.submit("benchmark-normal.png", ordinary_image, "blur", "normal")
                ready = admin.state()
                check(ready["policy"] == policy and ready["demo_delay_ms"] == 0 and
                      ready["queues"]["normal"], "Expected a normal-job backlog with no demo delay")
                urgent = bob.submit("benchmark-high.png", urgent_image, "grayscale", "high")
                state = finish(admin, (ordinary, urgent))
                submitted = matched_event(state, "submitted", urgent)
                first_after = next(event for event in state["events"]
                                   if event["type"] == "dispatch" and event["seq"] > submitted["seq"])
                expected = urgent if policy == "priority" else ordinary
                check(first_after["job"] == expected, f"Dispatch order did not match policy {policy}")
                started = matched_event(state, "dispatch", urgent)
                ended = matched_event(state, "job_done", urgent)
                beginning = matched_event(state, "submitted", ordinary)
                ordinary_end = matched_event(state, "job_done", ordinary)["time"]
                end = max(ended["time"], ordinary_end)
                result = {"trial": trial + 1, "high_dispatch_wait_ms": started["time"] - submitted["time"],
                          "high_turnaround_ms": ended["time"] - submitted["time"],
                          "normal_turnaround_ms": ordinary_end - beginning["time"],
                          "batch_turnaround_ms": end - beginning["time"],
                          "normal_tiles_waiting_before_high": len(ready["queues"]["normal"])}
                results[policy].append(result)
                print(f"{policy} trial {trial + 1}: {json.dumps(result)}", flush=True)
            disk.unlink()
    metrics = ("high_dispatch_wait_ms", "high_turnaround_ms",
               "normal_turnaround_ms", "batch_turnaround_ms")
    medians = {policy: {metric: statistics.median(r[metric] for r in records) for metric in metrics}
               for policy, records in results.items()}
    print(f"Medians: {json.dumps(medians)}", flush=True)
    return {"normal_dimensions": load_size, "high_dimensions": [129, 97], "workers": 4,
            "tile_size": 256, "demo_delay_ms": 0, "trials": results, "medians": medians}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build" / "tileflow")
    parser.add_argument("--web", type=Path, default=ROOT / "web")
    parser.add_argument("--fixture", type=Path, help="Acceptance PNG, defaults to web/demo.png")
    parser.add_argument("--evidence", type=Path, help="Write raw observations and events to JSON")
    parser.add_argument("--benchmark", action="store_true", help="Compare FIFO and priority with demo delay disabled")
    parser.add_argument("--repeats", type=int, default=3, help="Benchmark trials per policy")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    binary, web = args.binary.resolve(), args.web.resolve()
    fixture = args.fixture or (web / "demo.png")
    with tempfile.TemporaryDirectory(prefix="tileflow-acceptance-") as directory:
        directory = Path(directory)
        if args.benchmark:
            evidence = benchmark(binary, web, directory, args.repeats)
        else:
            evidence = acceptance(binary, web, fixture, directory)
    if args.evidence:
        args.evidence.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
