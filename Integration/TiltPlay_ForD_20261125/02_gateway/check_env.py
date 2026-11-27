"""Pre-flight check for the gateway: python / deps / ports / serial / config.
Prints plain ASCII so it is safe inside cmd.exe.
"""
import json, socket, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
G = ROOT / "gateway"
sys.path[:0] = [str(G / "python"), str(G / ".vendor")]

print("python      :", sys.version.split()[0], "|", sys.executable)

try:
    import serial  # noqa
    from serial.tools import list_ports
    print("pyserial    : ok")
except Exception as exc:
    print("pyserial    : FAILED ->", exc)

try:
    import paho.mqtt.client as mqtt  # noqa
    print("paho-mqtt   : ok")
except Exception as exc:
    print("paho-mqtt   : FAILED ->", exc)

try:
    from tiltplay.bridge import BridgeConfig
    cfg = BridgeConfig.load(G / "config" / "bridge.json")
    print("config      : udp=%s control=%s hz=%s sign=%s level_gate=%s limit=%s" % (
        cfg.udp_port, cfg.control_port, cfg.udp_hz,
        cfg.forward_output_sign, cfg.require_level_recenter,
        cfg.recenter_level_max_deg))
except Exception as exc:
    print("config      : FAILED ->", exc)
    sys.exit(1)

for port in (7001, 7002):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", port))
        print("port %d     : free" % port)
    except OSError as exc:
        print("port %d     : BUSY (%s)" % (port, exc))
    finally:
        s.close()

try:
    ports = list(list_ports.comports())
    if not ports:
        print("serial ports: none found (board not plugged in?)")
    else:
        for p in ports:
            print("serial port :", p.device, "-", p.description)
except Exception as exc:
    print("serial ports: FAILED ->", exc)
