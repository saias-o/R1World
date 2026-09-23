"""Telemetry must never be able to stop the world.

Measured failure, not a hypothetical one: on Windows `os.replace` refuses to
replace a file another process currently has open, and the game opens
`status.json` every 250 ms while it waits for a spawn. On a nine-tile teleport
the two crossed after the eighth tile, the PermissionError propagated out of
`serve()`, and the worker died. Nothing cooked another tile after that, and the
player watched the counter sit at "1 / 9" with no error anywhere on screen —
the worst shape a failure can take, because it looks like slowness.

Two properties fix it and are held here: the replace is retried, because the
reader holds its handle for microseconds; and a status write that fails anyway
is swallowed, because a status line is something a player reads and not
something the world depends on.
"""

from __future__ import annotations

import json
import os
import shutil
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from r1 import world_service


class AtomicJsonTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.path = self.root / "status.json"

    def test_a_transient_sharing_violation_is_retried(self) -> None:
        real = os.replace
        calls = {"n": 0}

        def flaky(src, dst):
            calls["n"] += 1
            if calls["n"] < 3:
                raise PermissionError(5, "Access is denied")
            return real(src, dst)

        with mock.patch.object(world_service.os, "replace", flaky), \
             mock.patch.object(world_service.time, "sleep", lambda _: None):
            world_service.atomic_json(self.path, {"state": "ready"})

        self.assertEqual(calls["n"], 3)
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8")), {"state": "ready"})

    def test_it_gives_up_rather_than_spinning_forever(self) -> None:
        def always(src, dst):
            raise PermissionError(5, "Access is denied")

        with mock.patch.object(world_service.os, "replace", always), \
             mock.patch.object(world_service.time, "sleep", lambda _: None):
            with self.assertRaises(PermissionError):
                world_service.atomic_json(self.path, {"state": "ready"})

    def test_no_partial_file_is_ever_visible(self) -> None:
        # The reader is a game polling this path four times a second. It must
        # see the old document or the new one, never half of either, which is
        # the whole reason for the temp-file-and-replace in the first place.
        world_service.atomic_json(self.path, {"state": "preparing"})
        seen = []
        real = os.replace

        def observe(src, dst):
            seen.append(json.loads(Path(dst).read_text(encoding="utf-8")))
            return real(src, dst)

        with mock.patch.object(world_service.os, "replace", observe):
            world_service.atomic_json(self.path, {"state": "ready"})
        self.assertEqual(seen, [{"state": "preparing"}], "the old document must stay whole")
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8")), {"state": "ready"})


class ReportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)

    def test_a_failed_status_write_does_not_reach_the_caller(self) -> None:
        """This is the regression that froze the counter.

        `report` is called from inside the cooking loop. If it can raise, one
        unlucky write ends the worker and every tile after it is never cooked.
        """
        def always(path, value):
            raise PermissionError(5, "Access is denied")

        with mock.patch.object(world_service, "atomic_json", always):
            world_service.report(self.root, {"key": "v3_1_1", "state": "ready"})

    def test_a_working_status_write_still_lands(self) -> None:
        world_service.report(self.root, {"key": "v3_1_1", "state": "ready"})
        self.assertEqual(
            json.loads((self.root / "status.json").read_text(encoding="utf-8")),
            {"key": "v3_1_1", "state": "ready"},
        )

    def test_the_cooking_loop_reports_through_report_and_never_atomic_json(self) -> None:
        """A future edit that writes the status directly reintroduces the crash.

        Cheap to check and worth checking: the whole fix is that these two
        writes are best effort, and that property lives at the call site.
        """
        source = Path(world_service.__file__).read_text(encoding="utf-8")
        loop = source.split("def serve(", 1)[1]
        self.assertNotIn('atomic_json(session / "status.json"', loop)
        self.assertIn('report(session,', loop)

    def test_heartbeat_continues_while_cooking_is_blocked(self) -> None:
        writes = []
        refreshed = threading.Event()

        def publish(path, value):
            writes.append(value)
            if len(writes) >= 2:
                refreshed.set()

        def slow_requests(session):
            self.assertTrue(refreshed.wait(4), "slow cooking must not look like a dead worker")

        with mock.patch.object(world_service, "atomic_json", publish), \
             mock.patch.object(world_service, "serve_requests", slow_requests):
            world_service.serve(self.root)
        self.assertGreaterEqual(len(writes), 2)

    def test_heartbeat_stops_when_request_loop_fails(self) -> None:
        before = set(threading.enumerate())
        with mock.patch.object(world_service, "serve_requests", side_effect=RuntimeError("broken")):
            with self.assertRaises(RuntimeError):
                world_service.serve(self.root)
        self.assertEqual(set(threading.enumerate()), before)
