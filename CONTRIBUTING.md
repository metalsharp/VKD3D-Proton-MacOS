# Contributing

Thanks for helping improve VKD3D-Proton macOS. Contributions are welcome in
the workspace code, build interface, documentation, probes, and reproducible
validation tooling.

## Before you start

1. Read the [development guide](docs/Development.md).
2. Check existing issues and pull requests for related work.
3. For security-sensitive bugs, follow [SECURITY.md](SECURITY.md) instead of
   opening a public issue.

The public repository intentionally excludes large source clones, generated
build directories, staged DLLs, and release archives. Do not add those files
to a pull request.

## Local setup

```bash
git clone https://github.com/metalsharp/vkd3d-proton-macos.git
cd vkd3d-proton-macos
make docs-check
```

For runtime changes, install the tools in
[docs/requirements.md](docs/requirements.md), place fresh source trees under
`sources/`, and use the root Makefile. The validated runtime is x86_64 PE
under Wine/Rosetta with a universal MoltenVK dylib.

## What to include

- Keep changes focused and explain the reason for a behavior or build change.
- Update the relevant documentation and Make targets when the workflow
  changes.
- Add or update a deterministic probe for promoted GPU behavior.
- Record source revisions, binary hashes, environment details, and complete
  probe output in `artifacts/evidence/` when changing a runtime lane.
- Preserve upstream licenses and identify changes to vendored components.
- Never use feature-level forcing or CPU fallback output as acceptance proof.

## Validation

For documentation or Makefile changes:

```bash
make test
git diff --check
```

For runtime changes, run the complete ladder and regression gate described in
[docs/validation.md](docs/validation.md). A capability advertisement or a
successful shader translation alone is not sufficient; promoted features need
GPU-executed, deterministic readback evidence.

## Pull requests

- Use a clear title describing the change.
- Explain scope, affected layers, validation performed, and known limits.
- Link the relevant evidence or issue.
- Keep generated artifacts and personal paths out of commits.
- Be ready to revise documentation and tests as part of review.

The `main` branch is protected. Changes require an approving review from the
repository owner through [`.github/CODEOWNERS`](.github/CODEOWNERS); direct
pushes are blocked for ordinary contributors. The repository owner may use
the GitHub administrator bypass (`gh pr merge --admin`) when an exceptional
direct merge is required.

By contributing, you agree that your work can be distributed under the
repository's [AGPL-3.0 License](LICENSE). Changes to an upstream project in
`patches/` are distributed under that project's license.
