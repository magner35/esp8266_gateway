#!/usr/bin/env python3
"""Упаковка OTA-образов SKE-02 в подписанные/шифрованные пакеты .skpak.

Шлюз принимает прошивки через веб ТОЛЬКО в этом формате:
  - Ed25519-подпись заголовка (приватный ключ - здесь, публичный зашит
    в прошивке шлюза, src/ota_sec.c);
  - AES-128-CTR шифрование полезной нагрузки (ключ зашит там же) -
    образ не виден сниферу в LAN;
  - SHA-256 открытых данных для контроля целостности.

Формат пакета (152-байтный заголовок, little-endian):
   0   8  magic "SKE-OTA1"
   8   1  тип: 1 = образ ESP (firmware.bin), 2 = hex прибора,
            3 = ЕДИНЫЙ образ: [u32 esp_len][esp bin][avr hex]
            (принимается прошивкой с поддержкой bundle)
   9   3  reserved (0)
  12   4  u32 размер ОТКРЫТЫХ данных
  16  16  nonce AES-CTR (уникален на пакет)
  32  32  SHA-256 открытых данных
  64  24  note: версия/дата, ascii, zero-padded (не подписывается);
            для bundle формируется "M<версия прибора>" - шлюз сравнивает
            с версией прибора и пропускает часть при совпадении
  88  64  подпись Ed25519 полей 0..63 (magic..sha256)
 152  ..  AES-128-CTR(payload); начальный счётчик = nonce,
           инкремент всего 128-битного блока big-endian (mbedTLS-совместимо)

Ключи (tools/keys/, НЕ в git):
  ota_sign.pem  приватный Ed25519 (PEM)
  ota_sign.pub  публичный Ed25519 (raw 32 байта)
  ota_aes.key   ключ AES-128 (raw 16 байт)

Команды:
  genkeys                 создать ключи + напечатать C-массивы для ota_sec.c
  pack <in> <esp|avr> <out.skpak> [--note "строка"]
  pack-all <bin> <hex> <out.skpak> [--mver "1.2.3"] [--note "строка"]
                          ЕДИНЫЙ образ (шлюз+прибор)
  selftest                упаковка->расшифровка->проверка подписи/порчи
"""

import argparse
import hashlib
import os
import struct
import sys

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    sys.exit("нет модуля cryptography: pip install cryptography")

HDR = 152
MAGIC = b"SKE-OTA1"
T_ESP, T_AVR, T_BUNDLE = 1, 2, 3

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = os.path.join(ROOT, "tools", "keys")
F_SIGN = os.path.join(KEYS, "ota_sign.pem")
F_PUB = os.path.join(KEYS, "ota_sign.pub")
F_AES = os.path.join(KEYS, "ota_aes.key")


def c_array(name: str, data: bytes) -> str:
    rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 8])
            for i in range(0, len(data), 8)]
    body = ",\n    ".join(rows)
    return f"static const uint8_t {name}[] = {{\n    {body},\n}};"


def load_keys():
    if not (os.path.exists(F_SIGN) and os.path.exists(F_AES)):
        sys.exit("нет ключей: запусти 'ota_pack.py genkeys' (tools/keys/)")
    priv = serialization.load_pem_private_key(
        open(F_SIGN, "rb").read(), password=None)
    aes = open(F_AES, "rb").read()
    assert len(aes) == 16, "битый AES-ключ"
    return priv, aes


def genkeys():
    os.makedirs(KEYS, exist_ok=True)
    priv = Ed25519PrivateKey.generate()
    open(F_SIGN, "wb").write(priv.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    pub = priv.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    open(F_PUB, "wb").write(pub)
    aes = os.urandom(16)
    open(F_AES, "wb").write(aes)
    print(f"ключи: {KEYS}")
    print(f"  {os.path.basename(F_SIGN)} - приватный, ХРАНИ/БЭКАПИРУЙ вне репо")
    print(f"  {os.path.basename(F_PUB)}  публичный (raw 32)")
    print(f"  {os.path.basename(F_AES)}  AES-128 (raw 16)")
    print("\nВставь в src/ota_sec.c:\n")
    print(c_array("kEdPub", pub))
    print()
    print(c_array("kAesKey", aes))


def _write(outp, hdr48, note, sig, body):
    with open(outp, "wb") as f:
        f.write(hdr48 + note.encode("utf-8")[:23].ljust(24, bytes([0]))
                + sig + body)


def pack(inp: str, ptype: int, outp: str, note: str):
    priv, aes = load_keys()
    plain = open(inp, "rb").read()
    if ptype == T_ESP and plain[0] != 0xE9:
        sys.exit("образ ESP должен начинаться с 0xE9 (esptool image)")
    if ptype == T_AVR and plain[0:1] != b":":
        sys.exit("файл прибора должен быть intel hex")
    nonce = os.urandom(16)
    sha = hashlib.sha256(plain).digest()
    hdr48 = (MAGIC + bytes([ptype]) + bytes([0, 0, 0])
             + struct.pack("<I", len(plain)) + nonce + sha)
    sig = priv.sign(hdr48)
    enc = Cipher(algorithms.AES(aes), modes.CTR(nonce)).encryptor()
    body = enc.update(plain) + enc.finalize()
    _write(outp, hdr48, note, sig, body)
    print(f"{outp}: тип {'esp' if ptype == T_ESP else 'avr'}, "
          f"открытых {len(plain)} Б, пакет {HDR + len(plain)} Б, "
          f"note \"{note}\"")


def pack_bundle(bin_path: str, hex_path: str, outp: str, note: str,
                mver: str = ""):
    """единый образ: [u32 LE esp_len][esp bin][avr hex] под одной подписью"""
    priv, aes = load_keys()
    esp = open(bin_path, "rb").read()
    avr = open(hex_path, "rb").read()
    if esp[0] != 0xE9:
        sys.exit("образ ESP должен начинаться с 0xE9")
    if avr[0:1] != b":":
        sys.exit("файл прибора должен быть intel hex")
    if not note and mver:
        note = "M" + mver   # шлюз сравнит с версией прибора
    plain = struct.pack("<I", len(esp)) + esp + avr
    nonce = os.urandom(16)
    sha = hashlib.sha256(plain).digest()
    hdr48 = (MAGIC + bytes([T_BUNDLE]) + bytes([0, 0, 0])
             + struct.pack("<I", len(plain)) + nonce + sha)
    sig = priv.sign(hdr48)
    enc = Cipher(algorithms.AES(aes), modes.CTR(nonce)).encryptor()
    body = enc.update(plain) + enc.finalize()
    _write(outp, hdr48, note, sig, body)
    print(f"{outp}: ЕДИНЫЙ образ, esp {len(esp)} Б + avr {len(avr)} Б, "
          f"пакет {HDR + len(plain)} Б, note \"{note}\"")


def _decrypt_body(nonce, body):
    dec = Cipher(algorithms.AES(open(F_AES, 'rb').read()),
                 modes.CTR(nonce)).decryptor()
    return dec.update(body) + dec.finalize()


def selftest():
    priv, aes = load_keys()
    from cryptography.exceptions import InvalidSignature
    plain = b"\xE9" + os.urandom(5000)
    nonce = os.urandom(16)
    sha = hashlib.sha256(plain).digest()
    hdr48 = (MAGIC + bytes([T_ESP]) + bytes([0, 0, 0])
             + struct.pack("<I", len(plain)) + nonce + sha)
    sig = priv.sign(hdr48)
    enc = Cipher(algorithms.AES(aes), modes.CTR(nonce)).encryptor()
    pkg = hdr48 + b"selftest".ljust(24, bytes([0])) + sig + \
        enc.update(plain) + enc.finalize()

    pub_obj = priv.public_key()
    h = pkg[:64]
    pub_obj.verify(pkg[88:152], h)
    dec = _decrypt_body(pkg[16:32], pkg[HDR:])
    assert dec == plain, "CTR-расшифровка не совпала"
    assert hashlib.sha256(dec).digest() == pkg[32:64]
    print("selftest: подпись/шифрование/хеш OK")

    bad = bytearray(pkg)
    bad[HDR + 100] ^= 1
    dec2 = _decrypt_body(bytes(bad[16:32]), bytes(bad[HDR:]))
    assert hashlib.sha256(dec2).digest() != pkg[32:64]
    print("selftest: порча payload отвергается хешем OK")

    other = Ed25519PrivateKey.generate()
    try:
        fake = other.sign(h)
        pub_obj.verify(fake, h)
        sys.exit("ЧУЖАЯ подпись прошла?!")
    except InvalidSignature:
        print("selftest: чужая подпись отвергается OK")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("genkeys")
    sp = sub.add_parser("pack")
    sp.add_argument("input")
    sp.add_argument("ptype", choices=["esp", "avr"])
    sp.add_argument("output")
    sp.add_argument("--note", default="")
    sb = sub.add_parser("pack-all")
    sb.add_argument("binfile")
    sb.add_argument("hexfile")
    sb.add_argument("output")
    sb.add_argument("--note", default="")
    sb.add_argument("--mver", default="",
                    help="версия прошивки прибора (для автопропуска)")
    sub.add_parser("selftest")
    a = p.parse_args()
    if a.cmd == "genkeys":
        genkeys()
    elif a.cmd == "pack":
        pack(a.input, T_ESP if a.ptype == "esp" else T_AVR, a.output, a.note)
    elif a.cmd == "pack-all":
        pack_bundle(a.binfile, a.hexfile, a.output, a.note, a.mver)
    elif a.cmd == "selftest":
        selftest()


if __name__ == "__main__":
    main()
