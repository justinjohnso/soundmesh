Import("env")

from pathlib import Path

# Get the PIOENV (e.g., "src" or "out")
pio_env = env["PIOENV"]
project_dir = Path(env["PROJECT_DIR"])

# Define the source file based on the environment
if pio_env == "src":
    app_sources = "src/main.c"
    message = "Building SRC firmware"
elif pio_env == "out":
    app_sources = "out/main.c"
    message = "Building OUT firmware"
else:
    raise ValueError(f"Unsupported environment '{pio_env}'. Expected 'src' or 'out'.")

print(message)

# PlatformIO's ESP-IDF builder expects a generated sdkconfig.<env> cache file.
# Keep it synchronized with sdkconfig.<env>.defaults so stack/memory config updates
# are applied reliably across builds.
sdkconfig_cache = project_dir / f"sdkconfig.{pio_env}"
sdkconfig_defaults = project_dir / f"sdkconfig.{pio_env}.defaults"
if not sdkconfig_defaults.exists():
    raise FileNotFoundError(f"Missing {sdkconfig_defaults}")

defaults_text = sdkconfig_defaults.read_text()
if (not sdkconfig_cache.exists()) or (sdkconfig_cache.read_text() != defaults_text):
    sdkconfig_cache.write_text(defaults_text)
    print(f"[sdkconfig] synchronized {sdkconfig_cache.name} from {sdkconfig_defaults.name}")

# Generate src/CMakeLists.txt
cmake_content = f"""
idf_component_register(SRCS "{app_sources}")
"""

with open("src/CMakeLists.txt", "w") as f:
    f.write(cmake_content)

# Config files are used individually for now
