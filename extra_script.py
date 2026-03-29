Import("env")

import atexit
from pathlib import Path

# Get the PIOENV (e.g., "src" or "out")
pio_env = env["PIOENV"]
project_dir = Path(env["PROJECT_DIR"])
build_dir = project_dir / ".pio" / "build" / pio_env
compile_db_path = project_dir / ".pio" / "build" / "compile_commands.json"
env.Replace(COMPILATIONDB_PATH=str(compile_db_path))

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

# Keep root clean from stale generated artifacts.
root_artifacts = [
    project_dir / "compile_commands.json",
    project_dir / f"sdkconfig.{pio_env}",
]
for artifact in root_artifacts:
    if artifact.exists():
        artifact.unlink()
        print(f"[cleanup] removed stale {artifact.name}")

# Track sdkconfig defaults in build workspace so config updates still force
# deterministic reconfigure behavior without creating root-level cache files.
sdkconfig_defaults = project_dir / f"sdkconfig.{pio_env}.defaults"
sdkconfig_stamp = build_dir / "sdkconfig.defaults.snapshot"
if not sdkconfig_defaults.exists():
    raise FileNotFoundError(f"Missing {sdkconfig_defaults}")

defaults_text = sdkconfig_defaults.read_text()
needs_reconfigure = False
if (not sdkconfig_stamp.exists()) or (sdkconfig_stamp.read_text() != defaults_text):
    build_dir.mkdir(parents=True, exist_ok=True)
    sdkconfig_stamp.write_text(defaults_text)
    needs_reconfigure = True
    print(f"[sdkconfig] detected defaults update for {pio_env}; forcing reconfigure")

# Generate src/CMakeLists.txt with environment-specific sources
cmake_content = f'idf_component_register(SRCS "{app_sources}")\n'
cmake_path = project_dir / "src" / "CMakeLists.txt"

# Check if content changed — if so, force CMake reconfigure
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


def cleanup_root_artifacts(source, target, env):
    for artifact in root_artifacts:
        if artifact.exists():
            artifact.unlink()
            print(f"[cleanup] removed generated {artifact.name}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", cleanup_root_artifacts)
env.AddPostAction("envdump", cleanup_root_artifacts)
atexit.register(lambda: cleanup_root_artifacts(None, None, None))
