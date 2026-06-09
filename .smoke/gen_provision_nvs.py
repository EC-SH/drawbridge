#!/usr/bin/env python3
"""Generate an NVS provisioning image for Pocket-Dial smoke testing.

Replicates AdminAuth::hashPin() (src/Helpers/AdminAuth.cpp) exactly so the
injected credential verifies against the on-device verifyPin():

    round 0      : SHA256( salt_ascii || pin_ascii )
    rounds 1..N-1: SHA256( salt_ascii || prev_digest )      (N = 50000)
    stored hash  : lowercase hex of the final 32-byte digest
    stored salt  : the 32-char hex salt string (ASCII, NOT decoded)

Writes an NVS CSV (namespace "storage") with:
    provisioned = 1   (u8)   -> display build boots straight to operational
    wifi_mode   = 2   (u8)   -> Standalone AP "esp32-sipserver" @ 192.168.4.1
    admin_salt  = <hex>      -> so verifyPin() can recompute
    admin_hash  = <hex>      -> salted/iterated digest of the chosen PIN

Then turn the CSV into a 0x6000 binary with the IDF tool:
    python %IDF_PATH%/components/nvs_flash/nvs_partition_generator/\
        nvs_partition_gen.py generate nvs_prov.csv nvs_prov.bin 0x6000

and flash it to the nvs partition (offset 0x9000):
    esptool -p COM4 write_flash 0x9000 nvs_prov.bin

This both restores the board to a usable provisioned state AND proves the
salt+hash credential scheme Fix #1 relies on (dashboard login PIN below).
"""
import hashlib
import sys

HASH_ITERATIONS = 50000          # AdminAuth::kHashIterations
SALT_HEX = "a1b2c3d4e5f60718293a4b5c6d7e8f90"  # 32 hex chars (128-bit), fixed for repeatability
PIN = sys.argv[1] if len(sys.argv) > 1 else "1234"

def hash_pin(salt_hex: str, pin: str, iters: int = HASH_ITERATIONS) -> str:
    salt = salt_hex.encode("ascii")
    digest = hashlib.sha256(salt + pin.encode("ascii")).digest()   # round 0
    for _ in range(1, iters):                                      # rounds 1..N-1
        digest = hashlib.sha256(salt + digest).digest()
    return digest.hex()

def main():
    admin_hash = hash_pin(SALT_HEX, PIN)
    csv = (
        "key,type,encoding,value\n"
        "storage,namespace,,\n"
        "provisioned,data,u8,1\n"
        "wifi_mode,data,u8,2\n"
        "admin_salt,data,string,%s\n"
        "admin_hash,data,string,%s\n"
    ) % (SALT_HEX, admin_hash)
    with open("nvs_prov.csv", "w", newline="\n") as f:
        f.write(csv)
    print("PIN           :", PIN)
    print("admin_salt    :", SALT_HEX)
    print("admin_hash    :", admin_hash)
    print("wrote         : nvs_prov.csv  (provisioned=1, wifi_mode=2=Standalone AP)")

if __name__ == "__main__":
    main()
