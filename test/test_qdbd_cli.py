import socket
import subprocess
import sys
import time
import unittest

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

    def test_explicit_port_wins_over_node_port(self):
        node_port, port, http_port = free_port(), free_port(), free_port()
        p = subprocess.Popen([QDBD, "--node", f"127.0.0.1:{node_port}:1", "--port", str(port),
                              "--http-port", str(http_port)], stderr=subprocess.DEVNULL)
        try:
            self.assertTrue(listens(port))
        finally:
            p.kill()
            p.wait()

    def test_bad_command_lines_fail_with_a_message(self):
        for args, text in (
            (["--bogus"], "Unknown argument: --bogus"),
            (["stray"], "Unknown argument: stray"),
            (["--http-port", "x"], "Invalid value 'x' for --http-port"),
            (["--port", "70000"], "Invalid value '70000' for --port"),
            (["--node", "127.0.0.1:1"], "Invalid node format"),
            (["--data"], "--data requires a value"),
            (["--config"], "--config requires a value"),
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
        for flag in ("--http-port", "--port", "--node-id", "--leader-id", "--node", "--data", "--config"):
            self.assertIn(flag, r.stdout)


if __name__ == "__main__":
    unittest.main()
