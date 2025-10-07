cmake_minimum_required(VERSION 3.16)

# This is a multi-app workspace. Build from one of the app directories instead:
#   - apps/node
#   - apps/tx
#   - apps/rx
# Example:
#   idf.py -C apps/node set-target esp32s3 build

message(FATAL_ERROR "Not an IDF project root. Run idf.py from apps/node, apps/tx, or apps/rx.")