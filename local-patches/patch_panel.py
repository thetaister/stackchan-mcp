# STACKCHAN-LOCAL-PATCH panel: screen panels. load_avatar_set accepts mode="panel" (one 320x240 RGB565
# image, 153,600 bytes) plus "seconds"; the gateway stages it like a face set and sends avatar_set_fetch
# with mode="panel" + duration_ms. Needs the custom firmware (thetaister/stackchan-mcp). Idempotent.
import pathlib, shutil, sys

TAG = "STACKCHAN-LOCAL-PATCH panel"

EDITS = {
    "capture_server.py": [
        ('    if mode not in ("layered", "matrix"):\n        raise ValueError(f"unknown avatar mode: {mode}")',
         '    if mode not in ("layered", "matrix", "panel"):  # STACKCHAN-LOCAL-PATCH panel\n        raise ValueError(f"unknown avatar mode: {mode}")'),
    ],
    "esp32_client.py": [
        ('        expected_size: int,\n        timeout: float = 60.0,\n    ) -> dict[str, Any]:\n        """Send avatar_set_fetch notification',
         '        expected_size: int,\n        timeout: float = 60.0,\n        extra: dict[str, Any] | None = None,  # STACKCHAN-LOCAL-PATCH panel\n    ) -> dict[str, Any]:\n        """Send avatar_set_fetch notification'),
        ('            "expected_size": expected_size,\n        }\n        try:\n            await self._ws.send(json.dumps(msg))',
         '            "expected_size": expected_size,\n        }\n        if extra:  # STACKCHAN-LOCAL-PATCH panel\n            msg.update(extra)\n        try:\n            await self._ws.send(json.dumps(msg))'),
    ],
    "gateway.py": [
        ('        mode: str,\n        timeout: float = 60.0,\n    ) -> dict:\n        """Stage an avatar set',
         '        mode: str,\n        timeout: float = 60.0,\n        seconds: float | None = None,  # STACKCHAN-LOCAL-PATCH panel\n    ) -> dict:\n        """Stage an avatar set'),
        ('            "matrix":  90 * kimg_bytes,   # 3_456_000\n        }.get(mode)',
         '            "matrix":  90 * kimg_bytes,   # 3_456_000\n            "panel":   320 * 240 * 2,     # 153_600  STACKCHAN-LOCAL-PATCH panel\n        }.get(mode)'),
        ('            expected_size=len(payload),\n            timeout=timeout,\n        )\n        # Surface the staging metadata',
         '            expected_size=len(payload),\n            timeout=timeout,\n            extra=({"duration_ms": int(max(1.0, min(float(seconds or 10.0), 600.0)) * 1000)}\n                   if mode == "panel" else None),  # STACKCHAN-LOCAL-PATCH panel\n        )\n        # Surface the staging metadata'),
    ],
    "stdio_server.py": [
        ('        if mode not in ("layered", "matrix"):\n            return [',
         '        if mode not in ("layered", "matrix", "panel"):  # STACKCHAN-LOCAL-PATCH panel\n            return ['),
        ('        result = await gateway.load_avatar_set(archive_path, mode, timeout)',
         '        result = await gateway.load_avatar_set(archive_path, mode, timeout, seconds=arguments.get("seconds"))  # STACKCHAN-LOCAL-PATCH panel'),
        ('                            "enum": ["layered", "matrix"],',
         '                            "enum": ["layered", "matrix", "panel"],  # STACKCHAN-LOCAL-PATCH panel'),
        ('                    "required": ["archive_path", "mode"],',
         '                    # STACKCHAN-LOCAL-PATCH panel\n                    "required": ["archive_path", "mode"],'),
    ],
}

# "seconds" property for the tool schema, inserted right after the "timeout" property of load_avatar_set.
SECONDS_OLD = '''                            "default": 60.0,
                            "minimum": 5.0,
                            "maximum": 300.0,
                        },
                    },
                    # STACKCHAN-LOCAL-PATCH panel
                    "required": ["archive_path", "mode"],'''
SECONDS_NEW = '''                            "default": 60.0,
                            "minimum": 5.0,
                            "maximum": 300.0,
                        },
                        "seconds": {
                            "type": "number",
                            "description": "panel mode only: how long the panel stays on screen (default 10).",
                        },
                    },
                    # STACKCHAN-LOCAL-PATCH panel
                    "required": ["archive_path", "mode"],'''


def main(pkg: pathlib.Path) -> bool:
    ok = True
    for name, edits in EDITS.items():
        p = pkg / name
        s = p.read_text(encoding="utf-8")
        if TAG in s:
            print(f"  already patched: {name} (panel)")
            continue
        new_s = s
        for old, new in edits:
            if new_s.count(old) != 1:
                print(f"  ERROR: anchor not found in {name} (panel): {old.splitlines()[0][:70]!r}")
                ok = False
                break
            new_s = new_s.replace(old, new)
        else:
            if name == "stdio_server.py":
                if new_s.count(SECONDS_OLD) != 1:
                    print("  ERROR: seconds anchor not found in stdio_server.py (panel)")
                    ok = False
                    continue
                new_s = new_s.replace(SECONDS_OLD, SECONDS_NEW)
            shutil.copy2(p, p.with_suffix(".py.panel.orig"))
            p.write_text(new_s, encoding="utf-8")
            print(f"  patched: {name} (panel)")
    return ok


if __name__ == "__main__":
    if len(sys.argv) > 1:
        pkg = pathlib.Path(sys.argv[1])
    else:
        import stackchan_mcp
        pkg = pathlib.Path(stackchan_mcp.__file__).parent
    sys.exit(0 if main(pkg) else 1)
