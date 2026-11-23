# Security Policy

## Supported versions

Security fixes are handled against the latest `main` branch and the latest
published release. The current published release is `v1.0`.

Older release assets may contain upstream vkd3d-proton, MoltenVK, SPIRV-Cross,
Wine, or toolchain components with their own security lifecycles. Those issues
should also be reported to the appropriate upstream project when applicable.

## Reporting a vulnerability

Please do **not** report security vulnerabilities in a public issue or pull
request. Use GitHub's private vulnerability reporting for this repository:

<https://github.com/metalsharp/MetalVulkan/security/advisories/new>

If private reporting is unavailable, contact the maintainers through the
project profile at <https://github.com/metalsharp> and do not include exploit
details in a public thread.

Include, when possible:

- affected release, commit, or asset checksum;
- macOS version, Apple GPU, Wine version, and runtime route;
- a minimal reproduction or proof of impact;
- logs, stack traces, and relevant environment variables; and
- whether the issue is in workspace code or an upstream component.

Please allow maintainers time to investigate and coordinate an upstream or
release response before public disclosure. Do not upload secrets, personal
data, or proprietary game files with a report.

## Runtime integrity concerns

For a suspicious release asset, first verify `SHA256SUMS`, the GitHub release
source, and the loaded module paths. Report mismatches privately using the
process above.
