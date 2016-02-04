#!/usr/bin/python3
import argparse
import json
import socket
import struct

def connect_to_plugin(host, port):
  skt = socket.create_connection((host, port))
  
  while True:
    header = skt.recv(4)
    hdr_ver, reserved, data_len = struct.unpack(">BBH", header)
    assert hdr_ver == 1
    data = bytes()
    while len(data) < data_len:
      data += skt.recv(data_len - len(data))
    
    obj = json.loads(data.decode("utf-8"))
    
    if obj["state"] == "drive":
      print("Speed: %d" % obj["telemetry"]["truck"]["speed"])

def main():
  parser = argparse.ArgumentParser(description="ETS2 Dashboard v2 Test")
  parser.add_argument("--host", default="localhost", help="Host to connect to (default: localhost)")
  parser.add_argument("--port", type=int, default=21212, help="Port to connect to (default: 21212)")
  args = parser.parse_args()
  connect_to_plugin(args.host, args.port)

if __name__ == "__main__":
  main()
