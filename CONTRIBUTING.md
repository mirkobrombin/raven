# Contributing to Raven

Please check existing issues and pull requests before starting. Discuss large changes with the maintainers first.

Keep each pull request focused on one problem. Explain what changes, why it is needed, and how you tested it. Use English for code, comments, commit messages and pull requests.

## Commit messages

Use this format:

```text
type: describe the change
```

Use a lowercase type:

- `feat`: add functionality.
- `fix`: correct a bug.
- `docs`: change documentation.
- `test`: add or correct tests.
- `perf`: reduce a measured cost without changing the intended behavior.
- `refactor`: reorganize code without changing behavior.
- `build`: change build or provisioning tools.
- `ci`: change automation.
- `chore`: other maintenance.

Write a short, specific subject. Add a body only when the reason or tradeoff is not clear from the diff. Pull request titles follow the same format.

## Code and verification

Follow the style of the surrounding code. Keep unrelated cleanup out of the pull request.

For bug fixes, include a test that reproduces the failure when practical. For Windows API changes, cover error cases and both 32-bit and 64-bit callers where applicable.

Include the build options, test commands and results in the pull request. Report skipped and failing tests. Do not suppress failures to make a check pass.

For performance changes, provide reproducible before-and-after measurements using the same workload and settings. Distinguish game menus from gameplay and hardware rendering from software rendering.

## Use of generative AI

AI tools may assist with contributions. You remain responsible for the submitted work: read it, understand it, check its sources and test it.

For each commit with substantive AI assistance, include:

```text
fix: handle the missing registry value

Assisted-by: <tool>:<model-version>
AI-Scope: <what the tool produced and the prompt or a short summary>
```

Single-line completions, renames and formatting do not require these trailers.

AI output is not evidence of correctness or permission to reuse code. Maintainers review contributions before merging. Pull requests that omit required disclosures must be corrected before they can be accepted.

Do not add personal agent instructions, chat transcripts or tool configuration files to the repository.

## Windows materials and research

Do not use proprietary Windows source code, leaked materials, or disassembly or decompilation of proprietary Windows components to write a contribution. Do not submit code derived from those materials.

When using AI tools, keep those materials outside the tools' accessible workspace. Do not upload Windows installations, system binaries, instruction dumps or decompiled code, or let an agent retrieve or inspect them.

Public API documentation, published specifications and appropriately licensed SDK materials may be used under their terms.

Behavioral tests against legally obtained Windows software are allowed. Keep execution in a separate test environment and provide observations such as inputs, return values and logs, without exposing proprietary implementation code.

These rules govern contribution materials and tool access. Contributors are not asked to certify the contents of a model's training data.

## Existing implementations and licenses

Wine, ReactOS and other open source implementations may be consulted or reused when their licenses permit the intended use.

For copied or adapted code:

- Identify the upstream project, source URL, revision and affected files.
- Preserve copyright notices and license texts.
- Record your modifications and give credit to the original authors.
- Explain which Raven files contain derived code and which license applies.

Raven's original code is MIT-licensed. Imported and derived code retains its applicable license obligations; it does not become MIT-licensed because it is placed in this repository.

Credit alone does not grant permission to copy code. GPL and LGPL requirements can also affect linking and distribution. Discuss imports with maintainers before integrating them, and document their origin and terms under `native/upstream` where appropriate.

Only submit work you have the right to contribute. If provenance or license compatibility is uncertain, resolve it before submitting.
