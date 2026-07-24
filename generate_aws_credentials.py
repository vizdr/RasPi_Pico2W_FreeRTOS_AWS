#!/usr/bin/env python3
"""Regenerates aws_credentials.c (gitignored) from the PEM files in certs/.

Run this again whenever you rotate the AWS IoT certificate/key:
    python3 generate_aws_credentials.py
"""

# Where the null-terminator rule below (see c_array()) comes from - straight from mbedtls's
# own API documentation, not something invented for this script:
#
#   mbedtls_x509_crt_parse() (used for aws_root_ca_pem and aws_device_cert_pem), documented
#   in mbedtls/x509_crt.h:
#     "buflen: The size of buf, including the terminating NULL byte in case of PEM encoded
#      data."
#
#   mbedtls_pk_parse_key() (used for aws_private_key_pem), documented in mbedtls/pk.h, even
#   more explicit:
#     "key: ... For PEM, the buffer must contain a null-terminated string."
#     "keylen: ... For PEM data, this includes the terminating null byte, so keylen must be
#      equal to strlen(key) + 1."
#
# The reason: mbedtls's PEM parser treats the buffer as a C string and scans it for the
# "-----BEGIN...-----"/"-----END...-----" markers, which needs a real null terminator to
# bound the scan safely. DER is a binary, length-prefixed format with no such scanning, so
# mbedtls explicitly says NOT to include a null byte there - these two rules only apply to
# PEM input, which is what all three files here are.

import pathlib

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
CERTS_DIR = SCRIPT_DIR / "certs"
OUT_C = SCRIPT_DIR / "aws_credentials.c"

FILES = [
    ("aws_root_ca_pem", CERTS_DIR / "AmazonRootCA1.pem"),
    ("aws_device_cert_pem", CERTS_DIR / "pico2w-VZ-210726-freertos.cert.pem"),
    ("aws_private_key_pem", CERTS_DIR / "pico2w-VZ-210726-freertos.private.key"),
]


def c_array(name: str, data: bytes) -> str:
    # Normalize the ending, then append exactly one newline + a null byte. The null
    # terminator matters: mbedtls parses PEM buffers as C strings (scanning for
    # "-----END...-----"), so it requires the terminator to be included in the reported
    # length - unlike DER buffers, which must NOT have one. rstrip first avoids ending up
    # with multiple trailing newlines if the source file already had one.
    data = data.rstrip(b"\n") + b"\n\0"

    lines = [f"const unsigned char {name}[] = {{"]
    # 16 bytes per line is purely cosmetic (keeps the generated file human-scannable
    # instead of one huge line); the last chunk is just shorter if len(data) isn't a
    # multiple of 16, since Python slicing doesn't error on running past the end.
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk))
    lines.append("};")
    # len(data) here is the length AFTER the \n\0 was appended above, so this constant
    # automatically includes the null terminator - matching what
    # mbedtls_x509_crt_parse()/mbedtls_pk_parse_key() expect for a PEM buffer.
    lines.append(f"const unsigned int {name}_len = {len(data)};")
    return "\n".join(lines)


def main():
    parts = ['#include "aws_credentials.h"', ""]
    for name, path in FILES:
        if not path.exists():
            raise SystemExit(f"missing {path} - copy your AWS IoT credentials into certs/ first")
        parts.append(c_array(name, path.read_bytes()))
        parts.append("")
    OUT_C.write_text("\n".join(parts))
    print(f"wrote {OUT_C}")


if __name__ == "__main__":
    main()
