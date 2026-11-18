# Security policy

## Supported versions

Only the latest commit on `main` is supported; there are no releases to back-port fixes to.

## Reporting a vulnerability

Please do **not** open a public issue. Report it privately through GitHub: the repository's
**Security** tab, **Report a vulnerability**
([GitHub's guide](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)).
Say what is affected, how to reproduce it, and what an attacker could do with it.

This is a one-person spare-time project: expect an answer within a couple of weeks. Once it is
fixed, the report can be published as a GitHub security advisory, with credit to you if you want
it.

## Scope

In scope: the renderer (`renderer/`), the tool (`rexnative/`: it extracts archives, disc images
and Xbox Live packages, downloads title updates, and copies games to a Steam Deck over SSH), the
SDK patches (`sdk/`) and the scripts (`scripts/`, `deck/`).

Out of scope, to be reported to their own projects: the ReXGlue SDK itself
([rexglue-sdk](https://github.com/rexglue/rexglue-sdk)), NVIDIA's and AMD's SDKs and DLLs,
Proton and vkd3d-proton, and the games.
