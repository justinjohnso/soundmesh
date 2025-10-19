Import("env")

# Get the PIOENV (e.g., "tx" or "rx")
pio_env = env["PIOENV"]

# Define the source file based on the environment
if pio_env == "tx":
    app_sources = "tx/main.c"
    message = "Building TX firmware"
else:
    app_sources = "rx/main.c"
    message = "Building RX firmware"

print(message)

# Generate src/CMakeLists.txt
cmake_content = f"""
idf_component_register(SRCS "{app_sources}")
"""

with open("src/CMakeLists.txt", "w") as f:
    f.write(cmake_content)
