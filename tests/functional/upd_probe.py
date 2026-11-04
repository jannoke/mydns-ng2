# Send a minimal DNS UPDATE (zone section only) for example.test and print the reply header.
import socket, struct, sys
zone = b"".join(bytes([len(l)]) + l for l in b"example.test".split(b".")) + b"\x00"
pkt = struct.pack(">HHHHHH", 0x1234, 0x2800, 1, 0, 0, 0) + zone + struct.pack(">HH", 6, 1)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
s.sendto(pkt, ("127.0.0.1", 53))
try:
    r, _ = s.recvfrom(4096)
    idn, flags = struct.unpack(">HH", r[:4])
    print("reply id=%#x rcode=%d len=%d" % (idn, flags & 0xF, len(r)))
except socket.timeout:
    print("NO REPLY")
