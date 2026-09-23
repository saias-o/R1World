"""A public data service says "not now" far more often than it says "no".

The world downloads its tiles from two shared, free services while the player
stands on the map waiting, and both answer 429/502/503 under load and recover
within seconds. Before this, one such answer became a bare `HTTP Error 503` in
front of the player and put a 60-second cooldown in front of every other tile.

So the contract these tests hold is a distinction, not a number: a transient
status is retried, a permanent one is raised at once, and the wait never becomes
long enough that the game looks hung. No socket is opened here -- `urlopen` is
replaced, which is the only way to test a retry policy without asking a public
mirror to fail on cue.
"""

from __future__ import annotations

import io
import json
import unittest
import urllib.error
from unittest import mock

from r1 import sources


class _Response(io.BytesIO):
    """The context-manager shape `urlopen` returns, and nothing more."""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def _ok(payload=None):
    return _Response(json.dumps(payload if payload is not None else {"elevation": [12.0]}).encode())


def _http(code, headers=None):
    return urllib.error.HTTPError(
        "https://example.invalid", code, "boom", headers or {}, None
    )


class RequestRetryTests(unittest.TestCase):
    def _run(self, outcomes):
        """Drive `_request_json` through a scripted sequence of urlopen results."""
        waits: list[float] = []
        calls = iter(outcomes)

        def fake_urlopen(request, timeout=None):
            outcome = next(calls)
            if isinstance(outcome, Exception):
                raise outcome
            return outcome

        with mock.patch.object(sources.urllib.request, "urlopen", fake_urlopen):
            result = sources._request_json(
                "https://example.invalid", sleep=waits.append
            )
        return result, waits

    def test_a_transient_status_is_retried_until_it_succeeds(self) -> None:
        result, waits = self._run([_http(503), _http(429), _ok()])
        self.assertEqual(result, {"elevation": [12.0]})
        self.assertEqual(len(waits), 2, "both failures should have been waited out")

    def test_the_wait_backs_off(self) -> None:
        _, waits = self._run([_http(502), _http(502), _ok()])
        self.assertLess(waits[0], waits[1], "a second failure must wait longer")

    def test_a_permanent_status_is_raised_immediately(self) -> None:
        # 400 means the query is wrong and will be wrong next time too; retrying
        # only delays the error the caller has to handle anyway.
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self._run([_http(400), _ok()])
        self.assertEqual(caught.exception.code, 400)

    def test_403_is_raised_rather_than_hidden(self) -> None:
        # A mirror that forbids the request is not overloaded, it is refusing;
        # the caller's job is then to move to the next endpoint, not to wait.
        with self.assertRaises(urllib.error.HTTPError):
            self._run([_http(403), _ok()])

    def test_a_timeout_is_retried(self) -> None:
        result, waits = self._run([TimeoutError("read timed out"), _ok()])
        self.assertEqual(result, {"elevation": [12.0]})
        self.assertEqual(len(waits), 1)

    def test_the_last_attempt_raises_instead_of_looping_forever(self) -> None:
        with self.assertRaises(urllib.error.HTTPError):
            self._run([_http(503)] * sources.RETRY_ATTEMPTS)

    def test_retry_after_is_honoured_but_capped(self) -> None:
        # A mirror asking for an hour is one to give up on, not one to wait for
        # with a player stood in front of an empty map.
        _, waits = self._run([_http(429, {"Retry-After": "3600"}), _ok()])
        self.assertEqual(waits, [sources.MAX_RETRY_WAIT])

        _, waits = self._run([_http(429, {"Retry-After": "5"}), _ok()])
        self.assertEqual(waits, [5.0])

        # The HTTP-date form is legal and not worth parsing; it must fall back
        # to the backoff rather than crash on a float() it cannot do.
        _, waits = self._run([_http(429, {"Retry-After": "Wed, 21 Oct 2026 07:28:00 GMT"}), _ok()])
        self.assertEqual(len(waits), 1)
        self.assertGreater(waits[0], 0.0)


class EndpointListTests(unittest.TestCase):
    def test_no_regional_extract_is_listed(self) -> None:
        """A mirror that answers 200/empty outside its region loses a city silently.

        `overpass.osm.ch` measurably does this: a Paris bbox returns 200 with no
        elements, which `cook` would store as a valid, building-less tile. The
        cache has no way to tell that apart from genuinely empty countryside, so
        the guard has to be here, before the request is ever made.
        """
        for endpoint in sources.OSM_ENDPOINTS:
            self.assertNotIn("osm.ch", endpoint)

    def test_more_than_one_endpoint(self) -> None:
        self.assertGreaterEqual(len(sources.OSM_ENDPOINTS), 2)
