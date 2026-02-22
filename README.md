# SUN2000 Modbus TCP Proxy (ESP32-C3)

Transparent Modbus TCP Proxy for Huawei SUN2000 Inverters.

This project creates a 1:1 Modbus TCP bridge between a client (e.g. Home Assistant, Node-RED, Modbus Poll) and a Huawei inverter.

The ESP32 acts as:

- Modbus TCP Server (Proxy)
- Modbus TCP Client (to Huawei inverter)

The goal is a stable, transparent forwarder that:
- Keeps TCP alive
- Handles Huawei dongle timing issues
- Avoids inverter crashes due to aggressive polling
- Mirrors registers without address translation

---

## Hardware

- ESP32-C3 (tested with Nologo ESP32C3 SuperMini)
- WiFi connection
- Huawei SUN2000 inverter (Modbus TCP enabled)

---

## Architecture

Client → ESP32 Proxy → Huawei Inverter

The proxy:
- Maintains persistent TCP connection to inverter
- Implements grace timing after connect
- Adds poll gap delays
- Handles Modbus timeout recovery
- Queues write operations safely

---

## Configuration

Create a file:
