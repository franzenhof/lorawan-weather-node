Import("env")

import os
import subprocess
from pathlib import Path


def get_version_string():
    forced_version = os.getenv("FW_VERSION")
    if forced_version:
        return forced_version.strip()

    try:
        result = subprocess.run(
            ["git", "describe", "--tags", "--dirty", "--always"],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()
    except Exception:
        return "dev"


version_string = get_version_string().replace('"', '\\"')
header_path = Path(env.get("PROJECT_DIR")) / "include" / "generated_version.h"
header_path.write_text(
    "#pragma once\n"
    f"#define FIRMWARE_VERSION \"{version_string}\"\n",
    encoding="utf-8",
)