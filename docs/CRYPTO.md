# RUI JavaScript encryption (DP-UB820 / A8640EU)

The resident UI is a WebKit+V8 app rendered by the `AjaxCeplay` engine binary. Its JavaScript
modules under `/dtvrec/data/rui/ace001/{jslib,bdav,...}` come in two forms:
- `*.js`      — the source, **AES-encrypted** (high entropy).
- `*.js.cache`— pre-compiled engine bytecode, magic **`0xDEAD0007`** (NOT encrypted, but packed;
  string/identifier tables are UTF-16, readable with `strings -e l`).

## The cipher
- **AES-128-CBC, IV = 16 zero bytes, no padding.**
- Implemented with stock OpenSSL (`libcrypto.so.1.0.0`) in the normal world (NOT the TEE).
- A native JS class **`CipherKey`** with method **`setKey(key16)`** (both found via Ghidra:
  class-name + method-name strings next to a `JS_InitClass`-style registration) hands the engine
  the key; the engine then `EVP_DecryptInit`s with `EVP_aes_128_ecb`/`cbc` and decrypts the module.
- **Keys are per-module but drawn as 16-byte windows from a key stream** — they overlap, so a
  modest set of captured windows decrypts many files.

## Why you must *trigger* a decrypt
At runtime the engine loads the pre-built `*.js.cache`, so it **never decrypts `.js`** during normal
use (keys are only used at factory cache-build). Two ways to force it on your own device:
- **Delete the `*.js.cache`** (that have a `.js` sibling) in a custom firmware → the engine must
  decrypt the `.js` to rebuild the cache on load.
- Then **hook OpenSSL** (`LD_PRELOAD` on `EVP_DecryptInit`/`EVP_DecryptInit_ex`) inside `AjaxCeplay`
  and log key+iv as each module loads. Navigate menus/apps/disc to load every module.

## Decrypt
`openssl enc -d -aes-128-cbc -K <keyhex> -iv 00000000000000000000000000000000 -nopad -in <file.js>`
Try each captured key against each file; the one that yields clean JS is the match.

## Notes
- There's a second, unrelated crypto subsystem for auth/config params: `EVP_BytesToKey`
  (password-derived, CBC) with constant passphrases. Different code path, not the module key.
- The DRM keys (PlayReady/Widevine/AACS) live in the TrustZone secure world and are **not**
  reachable from normal-world root. Out of scope.
