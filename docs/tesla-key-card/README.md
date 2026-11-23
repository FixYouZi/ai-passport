<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Tesla Key Card for AI Passport

This feature targets an independently operating BLE virtual key on the
ESP32-C3 AI Passport. The phone is not part of the daily unlock path.

## Trust and enrollment model

1. The user enters a 17-character VIN and optionally assigns a display model.
   The model label is user metadata; it is not proof of vehicle identity.
2. The firmware normalizes the VIN and derives Tesla's advertised BLE local
   name from the first eight bytes of SHA-1(VIN). A scan match is only a
   discovery hint. It does not authenticate a vehicle.
3. The device generates its own P-256 key pair. The private key must remain on
   the device and must never be logged, exported in a QR code, or sent to a
   companion service.
4. The public key is enrolled through Tesla's supported vehicle authorization
   flow. The owner uses an existing authorized key card/key fob as prompted by
   the vehicle and confirms enrollment on the vehicle display.
5. Only after successful enrollment may the firmware establish an authenticated
   session and issue supported local BLE commands.

The BLE MAC address is transient radio metadata. It can rotate and must not be
used as the durable vehicle identity or as proof of key enrollment.

## Board constraints

The current board is ESP32-C3, 8 MB flash, no PSRAM, ST7789P3 240x320 display,
and three ADC-ladder buttons. Its NTAG213 is a passive tag and there is no
MCU-facing NFC reader in the documented BSP. It cannot read, clone, or emulate
the Tesla factory key card. Factory-card approval therefore remains a physical
vehicle-side step.

The existing BLE demo is a non-connectable peripheral advertiser. This feature
needs NimBLE central/observer support, a scanner, GATT client, bounded worker
tasks, and a lifecycle that releases radio resources on page exit. The board
has one configured BLE connection and tight RAM headroom; scan and protocol
buffers must remain bounded.

## Security requirements

- Never persist or log the VIN unless the user explicitly chooses to save it.
- Protect private-key storage with an assessed ESP-IDF secure-storage design.
  Flash/NVS encryption and secure boot provisioning are separate requirements;
  do not claim hardware-backed key isolation without a secure element.
- Do not expose a "bound" state based only on a matching advertisement or MAC.
- Fail closed on invalid VIN, failed signature verification, expired/replayed
  session data, GATT errors, and incomplete enrollment.
- Require an explicit user action for unlock; do not silently unlock on scan.
- Do not embed Tesla account tokens, private keys, or credentials in source.

## Current implementation scope

The first code increment provides VIN normalization and the advertised-name
formatter, with host tests. It does not yet implement BLE scanning, key
generation/storage, Tesla protobuf transport, enrollment, or lock/unlock.
Those require separate protocol, memory, and on-device validation milestones.
