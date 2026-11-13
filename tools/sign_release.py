"""Release signing for V3X (Ed25519, RFC 8032; pure Python, no pip packages).

    python tools/sign_release.py keygen --out-dir DIR [--name NAME]
        new key pair: DIR/NAME.seed.b64 (PRIVATE: base64 of the 32-byte seed, the value of the
        GitHub secret V3X_SIGNING_KEY) and DIR/NAME.pub.txt (public key hex + key id). Never put DIR
        inside a git checkout. Prints the public key and the line to paste into installer/release_keys.h.
    python tools/sign_release.py sign FILE [--out FILE.sig] [--allow-key HEX]
        signs FILE (compat.json) with the seed in env V3X_SIGNING_KEY. Refuses unless the matching
        public key is one of installer/release_keys.h (or given with --allow-key, for dev dry runs),
        then verifies the written signature again.
    python tools/sign_release.py verify FILE [SIG] [--pub HEX ...]
        checks SIG (default FILE.sig) against installer/release_keys.h (or the --pub keys).
    python tools/sign_release.py pubkey
        public key + key id of env V3X_SIGNING_KEY.
    python tools/sign_release.py selftest
        RFC 8032 test vectors.

Signature file (FILE.sig, ASCII, one line):  v3x-sig 1 <key id> <base64 of the 64-byte signature>
over the exact bytes of FILE. key id = first 8 bytes of SHA-512(public key), 16 lowercase hex digits.
Verified by installer/release_sig.h (V3X-Setup.exe --verify-release, plugins/updater) and by
installer/install.ps1 (its own PowerShell Ed25519). Exit code 0 = ok, 1 = failure, 2 = usage.
"""
import argparse
import base64
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
KEYS_H = os.path.join(REPO, "installer", "release_keys.h")
# Public TEST key for the installer tests (make_dist.py --trust-key, test_installer.py). Its seed is
# in this file, so everyone can sign with it: it must never be in release_keys.h or install.ps1
# (test_installer.py fails if it is).
TEST_SEED = hashlib.sha256(b"V3X PUBLIC TEST KEY - everyone knows this seed, never trust it in a release").digest()

# ------------------------------------------------------------------ Ed25519 (RFC 8032 section 5.1)
P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _add(a, b):
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    A = (y1 - x1) * (y2 - x2) % P
    B = (y1 + x1) * (y2 + x2) % P
    C = 2 * t1 * t2 * D % P
    Dd = 2 * z1 * z2 % P
    E, F, G, H = B - A, Dd - C, Dd + C, B + A
    return (E * F % P, G * H % P, F * G % P, E * H % P)


def _mul(s, pt):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _add(q, pt)
        pt = _add(pt, pt)
        s >>= 1
    return q


def _eq(a, b):
    x1, y1, z1, _ = a
    x2, y2, z2, _ = b
    return (x1 * z2 - x2 * z1) % P == 0 and (y1 * z2 - y2 * z1) % P == 0


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * pow(5, P - 2, P) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)


def _compress(pt):
    x, y, z, _ = pt
    zi = pow(z, P - 2, P)
    x, y = x * zi % P, y * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(b):
    if len(b) != 32:
        return None
    y = int.from_bytes(b, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _sha512_int(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little")


def _expand(seed):
    if len(seed) != 32:
        raise ValueError("an Ed25519 seed is 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    a, _ = _expand(seed)
    return _compress(_mul(a, G))


def sign(seed, msg):
    a, prefix = _expand(seed)
    A = _compress(_mul(a, G))
    r = _sha512_int(prefix, msg) % L
    R = _compress(_mul(r, G))
    k = _sha512_int(R, A, msg) % L
    s = (r + k * a) % L
    return R + int.to_bytes(s, 32, "little")


def verify(pub, msg, sig):
    if len(pub) != 32 or len(sig) != 64:
        return False
    A = _decompress(pub)
    if A is None:
        return False
    Rb = sig[:32]
    R = _decompress(Rb)
    if R is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= L:
        return False
    k = _sha512_int(Rb, pub, msg) % L
    return _eq(_mul(s, G), _add(R, _mul(k, A)))


# ------------------------------------------------------------------ V3X formats
def key_id(pub):
    return hashlib.sha512(pub).digest()[:8].hex()


def sig_line(pub, sig):
    return "v3x-sig 1 %s %s\n" % (key_id(pub), base64.b64encode(sig).decode("ascii"))


def parse_sig(text):
    m = re.fullmatch(r"v3x-sig 1 ([0-9a-f]{16}) ([A-Za-z0-9+/]{86}==)\r?\n?", text)
    if not m:
        return None
    sig = base64.b64decode(m.group(2), validate=True)
    return (m.group(1), sig) if len(sig) == 64 else None


def header_keys(path=KEYS_H):
    """public keys (hex) in installer/release_keys.h: every non-empty V3X_RELEASE_KEY_* string"""
    keys = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r'\s*#define\s+V3X_RELEASE_KEY_(CURRENT|NEXT)\s+"([0-9a-fA-F]*)"', line)
        if m and m.group(2):
            if len(m.group(2)) != 64:
                raise SystemExit("release_keys.h: %s is not 64 hex digits" % m.group(1))
            keys.append(m.group(2).lower())
    return keys


def seed_from_env():
    v = os.environ.get("V3X_SIGNING_KEY", "").strip()
    if not v:
        raise SystemExit("sign_release: env V3X_SIGNING_KEY is empty (base64 of the 32-byte seed)")
    try:
        seed = base64.b64decode(v, validate=True)
    except Exception:
        raise SystemExit("sign_release: V3X_SIGNING_KEY is not base64")
    if len(seed) != 32:
        raise SystemExit("sign_release: V3X_SIGNING_KEY must decode to 32 bytes (got %d)" % len(seed))
    return seed


def selftest():
    # RFC 8032 section 7.1, TEST 1, 2, 3 (secret key, public key, message, signature)
    vec = [
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ]
    ok = True
    for sk, pk, msg, sg in vec:
        seed, msg_b = bytes.fromhex(sk), bytes.fromhex(msg)
        good = public_key(seed).hex() == pk and sign(seed, msg_b).hex() == sg and verify(bytes.fromhex(pk), msg_b, bytes.fromhex(sg))
        bad = bytearray(bytes.fromhex(sg))
        bad[0] ^= 1
        good = good and not verify(bytes.fromhex(pk), msg_b, bytes(bad)) and not verify(bytes.fromhex(pk), msg_b + b"x", bytes.fromhex(sg))
        print("%s RFC 8032 vector pk=%s..." % ("ok  " if good else "FAIL", pk[:16]))
        ok = ok and good
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--out-dir", required=True)
    k.add_argument("--name", default="v3x-release")
    s = sub.add_parser("sign")
    s.add_argument("file")
    s.add_argument("--out")
    s.add_argument("--allow-key", action="append", default=[])
    v = sub.add_parser("verify")
    v.add_argument("file")
    v.add_argument("sig", nargs="?")
    v.add_argument("--pub", action="append", default=[])
    sub.add_parser("pubkey")
    sub.add_parser("selftest")
    a = ap.parse_args(argv)

    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "keygen":
        out = os.path.abspath(a.out_dir)
        if os.path.commonpath([out.lower(), REPO.lower()]) == REPO.lower():
            print("sign_release: refusing to write a private key inside the repository", file=sys.stderr)
            return 1
        os.makedirs(out, exist_ok=True)
        sp = os.path.join(out, a.name + ".seed.b64")
        if os.path.exists(sp):
            print("sign_release: %s exists; not overwritten" % sp, file=sys.stderr)
            return 1
        seed = os.urandom(32)
        pub = public_key(seed)
        fd = os.open(sp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(base64.b64encode(seed).decode("ascii") + "\n")
        with open(os.path.join(out, a.name + ".pub.txt"), "w") as f:
            f.write("public_key %s\nkey_id %s\n" % (pub.hex(), key_id(pub)))
        print("private seed: %s  (GitHub secret V3X_SIGNING_KEY = its content; keep it offline)" % sp)
        print("public key:   %s" % pub.hex())
        print("key id:       %s" % key_id(pub))
        print('release_keys.h line: #define V3X_RELEASE_KEY_CURRENT "%s"' % pub.hex())
        return 0
    if a.cmd == "pubkey":
        pub = public_key(seed_from_env())
        print("public_key %s\nkey_id %s" % (pub.hex(), key_id(pub)))
        return 0
    if a.cmd == "sign":
        seed = seed_from_env()
        pub = public_key(seed)
        allowed = header_keys() + [x.lower() for x in a.allow_key]
        if pub.hex() not in allowed:
            print("sign_release: the key in V3X_SIGNING_KEY (%s, id %s) is not in installer/release_keys.h; "
                  "releases signed with it would be refused" % (pub.hex(), key_id(pub)), file=sys.stderr)
            return 1
        data = open(a.file, "rb").read()
        sig = sign(seed, data)
        if not verify(pub, data, sig):
            print("sign_release: self-check of the new signature failed", file=sys.stderr)
            return 1
        out = a.out or a.file + ".sig"
        with open(out, "w", newline="\n") as f:
            f.write(sig_line(pub, sig))
        print("sign_release: %s signed with key %s -> %s" % (a.file, key_id(pub), out))
        return 0
    if a.cmd == "verify":
        data = open(a.file, "rb").read()
        sp = a.sig or a.file + ".sig"
        parsed = parse_sig(open(sp, "rb").read().decode("ascii", "replace"))
        if not parsed:
            print("sign_release: %s is not a v3x-sig line" % sp, file=sys.stderr)
            return 1
        kid, sig = parsed
        keys = [x.lower() for x in a.pub] or header_keys()
        for hx in keys:
            pub = bytes.fromhex(hx)
            if key_id(pub) == kid:
                if verify(pub, data, sig):
                    print("sign_release: signature ok (key %s)" % kid)
                    return 0
                print("sign_release: BAD signature (key %s)" % kid, file=sys.stderr)
                return 1
        print("sign_release: unknown key id %s" % kid, file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
