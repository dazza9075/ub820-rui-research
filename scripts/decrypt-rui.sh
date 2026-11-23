#!/bin/bash
# Decrypt a Panasonic DP-UB820 RUI .js file.
# Scheme: AES-128-CBC, NO padding. KEY is PER-FILE (captured live via libjshook.so
# EVP_DecryptInit hook when the engine rebuilds that file's .js.cache).
# e.g. home.js: KEY = <your captured 32-hex key>, IV = 00000000000000000000000000000000.
# Usage: decrypt-rui.sh <encrypted.js> <key-hex-32> [<iv-hex-32=zeros>]
KEY="$2"; IV="${3:-00000000000000000000000000000000}"
openssl enc -d -aes-128-cbc -K "$KEY" -iv "$IV" -nopad -in "$1"
