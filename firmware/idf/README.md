# Meshnet Audio (ESP-IDF + ESP-ADF)

This workspace contains transmitter, receiver, and dual-mode node apps for broadcasting audio across an ESP-WIFI-MESH using Opus.

- Board: XIAO-ESP32-S3 (PSRAM recommended)
- SDKs: ESP-IDF v5.2.x, ESP-ADF v2.6.x (as submodule or via ADF_PATH)

## Layout

- apps/
  - tx/ : USB/Line-in -> Opus -> Mesh
  - rx/ : Mesh -> Opus -> USB/I2S out
  - node/: Dual-mode app; reads a GPIO to switch TX/RX
- components/
  - mesh_stream/: ADF element for mesh send/receive with framing + jitter buffer
  - ctrl_plane/: Simple role/control messages over mesh

## Setup (macOS)

1) Install ESP-IDF 5.2.x and export env
2) Clone or init ESP-ADF and set ADF_PATH to this workspace's esp-adf (or provide externally)
3) Configure board and USB permissions as needed

## Build

From this directory:

- Default (dual-mode node):
  idf.py set-target esp32s3 build

- Build TX:
  idf.py -DPROJECT=tx set-target esp32s3 build

- Build RX:
  idf.py -DPROJECT=rx set-target esp32s3 build

## Notes

- USB audio used for quick validation; I2S line-in/out paths will be enabled via configs
- Mesh, ctrl_plane, and mesh_stream are currently stubs; functionality will be implemented incrementally
