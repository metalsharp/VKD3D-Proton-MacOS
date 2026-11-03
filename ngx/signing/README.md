# Signing the lane's NGX core

NVIDIA's NGX library, which DLSS games link, loads its core (`nvngx.dll`, from the folder its registry values name)
only if the file's Authenticode signature chains to a trusted root. Unsigned, it fails with `CERT_E_UNTRUSTEDROOT`
and the game reports DLSS as unsupported.

So the lane's `nvngx.dll` is signed with the Mullion NGX key, as Mullion's is, and the lane's DXGI (DXVK,
`src/dxgi/dxgi_ngx.cpp`) adds that key's certificate, `mullion-ngx.pem` here (self-issued, code signing only), to
the prefix's `Root` store when it shows the game an NVIDIA GPU. The trust is the prefix's only.

- `scripts/build-ngx-arm64x.sh` signs with `NGX_SIGNING_KEY_FILE`, else `~/.metalsharp/ngx-signing/key.pem`.
- The release workflow signs with the repository secret `NGX_SIGNING_KEY` (the key, PEM); a run without it leaves
  `nvngx.dll` unsigned, with a warning.

By hand: `osslsigncode sign -certs ngx/signing/mullion-ngx.pem -key <key.pem> -n "Mullion NGX" -h sha256 -in
nvngx.dll -out nvngx-signed.dll`.
