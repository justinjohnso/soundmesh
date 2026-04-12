from __future__ import annotations

import argparse
import urllib.error
import unittest
from unittest import mock

from tools import usb_compound_autoflash as autoflash


class UsbCompoundAutoflashTests(unittest.TestCase):
    def test_validate_https_firmware_url_accepts_valid_https_bin(self) -> None:
        url = "https://example.com/releases/soundmesh.bin"
        self.assertEqual(autoflash.validate_https_firmware_url(url), url)

    def test_validate_https_firmware_url_rejects_non_https_and_non_bin(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            autoflash.validate_https_firmware_url("http://example.com/fw.bin")
        with self.assertRaises(argparse.ArgumentTypeError):
            autoflash.validate_https_firmware_url("https://example.com/fw.txt")

    def test_normalize_base_url_accepts_and_normalizes(self) -> None:
        self.assertEqual(
            autoflash.normalize_base_url("http://192.168.4.1/"),
            "http://192.168.4.1",
        )
        self.assertEqual(
            autoflash.normalize_base_url("https://portal.local:8443"),
            "https://portal.local:8443",
        )

    def test_normalize_base_url_rejects_paths_and_queries(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            autoflash.normalize_base_url("https://portal.local/api")
        with self.assertRaises(argparse.ArgumentTypeError):
            autoflash.normalize_base_url("https://portal.local?x=1")

    def test_make_auth_headers_maps_modes(self) -> None:
        self.assertEqual(
            autoflash.make_auth_headers("abc", "bearer"),
            {"Authorization": "Bearer abc"},
        )
        self.assertEqual(
            autoflash.make_auth_headers("abc", "x-token"),
            {"X-SoundMesh-Token": "abc"},
        )
        self.assertEqual(autoflash.make_auth_headers(None, "bearer"), {})

    def test_parse_args_supports_dry_run_flag(self) -> None:
        args = autoflash.parse_args(
            [
                "--base-url",
                "http://192.168.4.1",
                "--firmware-url",
                "https://example.com/fw.bin",
                "--token",
                "abc123",
                "--dry-run",
            ]
        )
        self.assertTrue(args.dry_run)
        self.assertEqual(args.base_url, "http://192.168.4.1")

    def test_parse_args_requires_base_url(self) -> None:
        with self.assertRaises(SystemExit):
            autoflash.parse_args(
                [
                    "--firmware-url",
                    "https://example.com/fw.bin",
                    "--token",
                    "abc123",
                ]
            )

    def test_parse_args_requires_token(self) -> None:
        with self.assertRaises(SystemExit):
            autoflash.parse_args(
                [
                    "--base-url",
                    "http://192.168.4.1",
                    "--firmware-url",
                    "https://example.com/fw.bin",
                ]
            )

    def test_parse_args_rejects_poll_interval_over_timeout(self) -> None:
        with self.assertRaises(SystemExit):
            autoflash.parse_args(
                [
                    "--base-url",
                    "http://192.168.4.1",
                    "--firmware-url",
                    "https://example.com/fw.bin",
                    "--token",
                    "abc123",
                    "--poll-interval",
                    "5",
                    "--timeout",
                    "1",
                ]
            )

    def test_poll_until_done_success_after_active_then_last_ok(self) -> None:
        statuses = [
            autoflash.OtaStatus(
                in_progress=True,
                phase="downloading",
                last_ok=False,
                last_err=0,
            ),
            autoflash.OtaStatus(
                in_progress=False,
                phase="idle",
                last_ok=True,
                last_err=0,
            ),
        ]
        with mock.patch.object(autoflash, "fetch_ota_status", side_effect=statuses):
            result = autoflash.poll_until_done(
                base_url="http://192.168.4.1",
                headers={},
                request_timeout_s=1.0,
                poll_interval_s=0.0,
                timeout_s=2.0,
            )
        self.assertEqual(result, autoflash.EXIT_OK)

    def test_poll_until_done_success_when_immediately_unreachable_then_idle_last_ok(self) -> None:
        statuses = [
            urllib.error.URLError("device rebooting"),
            autoflash.OtaStatus(
                in_progress=False,
                phase="idle",
                last_ok=True,
                last_err=0,
            ),
        ]
        with mock.patch.object(autoflash, "fetch_ota_status", side_effect=statuses):
            result = autoflash.poll_until_done(
                base_url="http://192.168.4.1",
                headers={},
                request_timeout_s=1.0,
                poll_interval_s=0.0,
                timeout_s=2.0,
            )
        self.assertEqual(result, autoflash.EXIT_OK)

    def test_poll_until_done_does_not_use_stale_last_ok_after_late_disconnect(self) -> None:
        statuses = [
            autoflash.OtaStatus(
                in_progress=False,
                phase="idle",
                last_ok=True,
                last_err=0,
            ),
            urllib.error.URLError("transient disconnect"),
        ]

        def fake_fetch(*_args: object, **_kwargs: object) -> autoflash.OtaStatus:
            if statuses:
                next_item = statuses.pop(0)
                if isinstance(next_item, Exception):
                    raise next_item
                return next_item
            return autoflash.OtaStatus(
                in_progress=False,
                phase="idle",
                last_ok=True,
                last_err=0,
            )

        with mock.patch.object(autoflash, "fetch_ota_status", side_effect=fake_fetch):
            result = autoflash.poll_until_done(
                base_url="http://192.168.4.1",
                headers={},
                request_timeout_s=1.0,
                poll_interval_s=0.0,
                timeout_s=0.01,
            )
        self.assertEqual(result, autoflash.EXIT_TIMEOUT)

    def test_poll_until_done_aborts_immediately_on_poll_auth_rejection(self) -> None:
        for code in (401, 403):
            with self.subTest(code=code):
                http_error = urllib.error.HTTPError(
                    url="http://192.168.4.1/api/ota",
                    code=code,
                    msg="auth rejected",
                    hdrs=None,
                    fp=None,
                )
                with (
                    mock.patch.object(autoflash, "fetch_ota_status", side_effect=http_error) as fetch_mock,
                    mock.patch("builtins.print") as print_mock,
                ):
                    result = autoflash.poll_until_done(
                        base_url="http://192.168.4.1",
                        headers={},
                        request_timeout_s=1.0,
                        poll_interval_s=10.0,
                        timeout_s=120.0,
                    )
                self.assertEqual(result, autoflash.EXIT_RUNTIME_ERROR)
                self.assertEqual(fetch_mock.call_count, 1)
                print_mock.assert_any_call(
                    f"[ota] FAILED: auth rejected while polling /api/ota (HTTP {code}); aborting."
                )

    def test_poll_until_done_returns_runtime_error_on_non_auth_http_error(self) -> None:
        http_error = urllib.error.HTTPError(
            url="http://192.168.4.1/api/ota",
            code=500,
            msg="server exploded",
            hdrs=None,
            fp=None,
        )
        with (
            mock.patch.object(autoflash, "fetch_ota_status", side_effect=http_error) as fetch_mock,
            mock.patch("builtins.print") as print_mock,
        ):
            result = autoflash.poll_until_done(
                base_url="http://192.168.4.1",
                headers={},
                request_timeout_s=1.0,
                poll_interval_s=10.0,
                timeout_s=120.0,
            )
        self.assertEqual(result, autoflash.EXIT_RUNTIME_ERROR)
        self.assertEqual(fetch_mock.call_count, 1)
        print_mock.assert_any_call(
            "[ota] FAILED: unexpected HTTP error while polling /api/ota (HTTP 500): server exploded"
        )


if __name__ == "__main__":
    unittest.main()
