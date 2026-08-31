# Universal CLI subscriptions

The CLI accepts subscription feeds independently of Amnezia Premium.

## Sources and encodings

Use exactly one source:

```text
--data <payload>
--file <path>
--url <http-or-https-url>
```

Use `--encoding auto|plain|base64|hex`. Auto detection is the default. The URL form uses the same decoder, so open text, Base64, and hexadecimal endpoints do not need separate commands.

Supported feed layouts:

- One share URI or native configuration.
- One share URI per line.
- A JSON array containing strings or configuration objects.
- A JSON object with an `items`, `configs`, `servers`, or `nodes` array.
- Multiline configurations separated by a line containing exactly `---`.

Each entry is passed to Amnezia's existing import controller. The current upstream importer accepts Amnezia, OpenVPN, WireGuard, AWG, VLESS, VMess, Trojan, Shadowsocks, SSD, and Xray JSON configurations.

## Commands

```bash
amnezia subscription inspect --url https://example.test/sub --encoding auto
amnezia subscription import --file subscription.txt --encoding plain --dry-run
amnezia subscription import --data "$PAYLOAD" --encoding base64
```

Import validation is atomic by default. If any entry is unsupported or invalid, no entries are imported. `--allow-partial` imports only the valid entries. Downloads use normal TLS verification, permit only HTTP and HTTPS, follow at most five non-downgrade redirects, and have a 4 MiB limit.

## 32x device IDs

```bash
amnezia subscription device-id
```

A 32x ID has the form `32x` followed by 64 hexadecimal characters. It is a domain-separated SHA-256 digest of the operating system machine identifier. The raw machine identifier is never printed or placed in a subscription.

## 16x subscriptions

Create a subscription for a particular 32x ID:

```bash
amnezia subscription pack --device 32x... --file subscription.txt --out customer.16x
amnezia subscription import --file customer.16x
```

Version 1 uses this wire representation:

```text
16x || base64url(version || nonce || authentication_tag || ciphertext)
```

- Version: one byte with value `1`.
- Nonce: 12 random bytes.
- Authentication tag: 16 bytes.
- Cipher: AES-256-GCM.
- AAD: `amnezia-16x-v1`.
- Key: SHA-256 over a domain separator and the normalized 32x ID.

The 32x ID is not stored in the token. A copied 16x token cannot be decrypted on a device with another 32x ID. The 32x value should still be treated as account data: anyone holding both the 16x token and its 32x ID can derive the same decryption key.
