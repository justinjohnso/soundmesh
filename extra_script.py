Import("env")

from pathlib import Path
import shutil

# Get the PIOENV (e.g., "src" or "out")
pio_env = env["PIOENV"]
project_dir = Path(env["PROJECT_DIR"])
build_dir = project_dir / ".pio" / "build" / pio_env

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

# Generate src/CMakeLists.txt with environment-specific sources
cmake_content = f'idf_component_register(SRCS "{app_sources}")\n'
cmake_path = project_dir / "src" / "CMakeLists.txt"

# Check if content changed — if so, force CMake reconfigure
needs_reconfigure = False
if cmake_path.exists():
    existing = cmake_path.read_text()
    if existing != cmake_content:
        needs_reconfigure = True
        print(f"[cmake] Source changed: was '{existing.strip()}', now '{cmake_content.strip()}'")
else:
    needs_reconfigure = True

cmake_path.write_text(cmake_content)

# Force CMake reconfigure by removing CMakeCache if source file changed
# This handles switching between src/out environments cleanly
if needs_reconfigure:
    cmake_cache = build_dir / "CMakeCache.txt"
    if cmake_cache.exists():
        cmake_cache.unlink()
        print(f"[cmake] Removed {cmake_cache} to force reconfigure")
