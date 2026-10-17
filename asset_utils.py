# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 TheSameEli
"""Deterministic hashes used by the DMI compiler and its regression checks."""

def fingerprint(im):
    data = bytearray(im.convert('RGBA').tobytes())
    for i in range(0, len(data), 4):
        if not data[i + 3]:
            data[i:i+3] = b'\0\0\0'
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def alpha_fingerprint(im):
    value = 14695981039346656037
    for byte in bytes((im.width, im.height)) + im.getchannel('A').tobytes():
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value
