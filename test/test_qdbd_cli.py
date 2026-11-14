import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

QDBD = sys.argv.pop(1)


def free_port():
    with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as s:
        s.bind(("::", 0))
        return s.getsockname()[1]


def run(*args):
    return subprocess.run([QDBD, *args], capture_output=True, text=True, timeout=20)


def listens(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


TABLES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "regtest", "cases", "decimal_parquet", "tables")


def query(http_port, sql):
    req = urllib.request.Request(f"http://127.0.0.1:{http_port}/api/run",
                                 data=sql.encode())
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


class TestQdbdCli(unittest.TestCase):
    def test_node_ports_define_the_listen_ports(self):
        ports = [free_port() for _ in range(4)]
        nodes = [f"127.0.0.1:{ports[0]}:1", f"127.0.0.1:{ports[1]}:2"]
        procs = []
        try:
            for node_id, http_port in ((1, ports[2]), (2, ports[3])):
                args = [QDBD, "--node", nodes[0], "--node", nodes[1], "--node-id", str(node_id),
                        "--leader-id", "1", "--http-port", str(http_port)]
                procs.append(subprocess.Popen(args, stderr=subprocess.DEVNULL))
            self.assertTrue(listens(ports[0]), "node 1 must listen on its --node port")
            self.assertTrue(listens(ports[1]), "node 2 must listen on its --node port")
            for p in procs:
                self.assertIsNone(p.poll(), "a node exited")
        finally:
            for p in procs:
                p.kill()
                p.wait()

    def test_jit_cache_dir_is_filled_by_the_first_query_and_hit_by_the_second(self):
        ports = [free_port() for _ in range(4)]
        nodes = [f"127.0.0.1:{ports[0]}:1", f"127.0.0.1:{ports[1]}:2"]
        sql = "SELECT id % 7 AS k, SUM(d15) AS total FROM decimals GROUP BY id % 7"
        procs = []
        with tempfile.TemporaryDirectory() as tmp:
            cache = os.path.join(tmp, "missing", "jit")  # created by the daemon
            try:
                for node_id, http_port in ((1, ports[2]), (2, ports[3])):
                    args = [QDBD, "--node", nodes[0], "--node", nodes[1], "--node-id", str(node_id),
                            "--leader-id", "1", "--http-port", str(http_port), "--data", TABLES,
                            f"--jit:cache_dir={cache}"]
                    procs.append(subprocess.Popen(args, stderr=subprocess.DEVNULL))
                for port in ports:
                    self.assertTrue(listens(port))

                def files():
                    return {os.path.join(d, f): os.stat(os.path.join(d, f)).st_mtime_ns
                            for d, _, names in os.walk(cache) for f in names}

                bodies = []
                snapshots = []
                for _ in range(2):
                    status, body = None, ""
                    for _ in range(20):  # the compute actors may still be joining
                        status, body = query(ports[2], sql)
                        if status != 503:
                            break
                        time.sleep(0.25)
                    self.assertEqual(status, 200, body)
                    bodies.append(sorted(body.splitlines()))
                    snapshots.append(files())
                self.assertTrue(snapshots[0], "the first query must write cache files")
                self.assertEqual(snapshots[0], snapshots[1], "the second query must only hit the cache")
                self.assertEqual(bodies[0], bodies[1])
            finally:
                for p in procs:
                    p.kill()
                    p.wait()

    def test_queries_do_not_share_pruned_table_sources(self):
        ports = [free_port() for _ in range(4)]
        nodes = [f"127.0.0.1:{ports[0]}:1", f"127.0.0.1:{ports[1]}:2"]
        procs = []
        try:
            for node_id, http_port in ((1, ports[2]), (2, ports[3])):
                args = [QDBD, "--node", nodes[0], "--node", nodes[1], "--node-id", str(node_id),
                        "--leader-id", "1", "--http-port", str(http_port), "--data", TABLES]
                procs.append(subprocess.Popen(args, stderr=subprocess.DEVNULL))
            for port in ports:
                self.assertTrue(listens(port))
            # The first query prunes "decimals" to id and d15; the second reads more of them.
            for sql, lines in (
                ("SELECT id % 7 AS k, SUM(d15) AS total FROM decimals GROUP BY id % 7", None),
                ("SELECT a.id, b.s FROM decimals a JOIN decimals b ON a.id = b.id", 257),  # 256 rows and the header
            ):
                status, body = None, ""
                for _ in range(20):  # the compute actors may still be joining
                    status, body = query(ports[2], sql)
                    if status != 503:
                        break
                    time.sleep(0.25)
                self.assertEqual(status, 200, body)
                if lines is not None:
                    self.assertEqual(len(body.splitlines()), lines, body[:200])
        finally:
            for p in procs:
                p.kill()
                p.wait()

    def test_bad_command_lines_fail_with_a_message(self):
        for args, text in (
            (["--bogus"], "Unknown argument: --bogus"),
            (["stray"], "Unknown argument: stray"),
            (["--http-port", "x"], "Invalid value 'x' for --http-port"),
            (["--port", "31337"], "Unknown argument: --port"),
            (["--http-port", "70000"], "Invalid value '70000' for --http-port"),
            (["--node", "127.0.0.1:1"], "Invalid node format"),
            (["--data"], "--data requires a value"),
            (["--config"], "--config requires a value"),
            (["--transport:window_chunks=0"], "an integer >= 1"),
        ):
            with self.subTest(args=args):
                r = run(*args)
                self.assertEqual(r.returncode, 1, r.stderr)
                self.assertIn(text, r.stderr)

    def test_bind_failure_is_reported(self):
        with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as busy:
            busy.bind(("::", 0))
            busy.listen()
            busy_port = busy.getsockname()[1]
            r = run("--node", f"127.0.0.1:{free_port()}:1", "--http-port", str(busy_port))
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("Cannot listen", r.stderr)

    def test_help_lists_every_option(self):
        r = run("--help")
        self.assertEqual(r.returncode, 0)
        for flag in ("--http-port", "--node-id", "--leader-id", "--node", "--data", "--config"):
            with self.subTest(flag=flag):
                self.assertRegex(r.stdout, rf"(?m)^\s*{flag}(\s|$)")
        self.assertIn("required", r.stdout)

    def test_unregistered_node_id_exits_with_one(self):
        for args in ([], ["--node", f"127.0.0.1:{free_port()}:2"]):
            with self.subTest(args=args):
                r = run(*args)
                self.assertEqual(r.returncode, 1, r.stderr)
                self.assertIn("is not registered", r.stderr)


if __name__ == "__main__":
    unittest.main()
