# Security policy

Raven runs Windows programs on Linux. A flaw that lets a guest program escape supervision, read or write host files it was
not given, or run host code is a vulnerability. So is one in the tools that build, provision or package Raven.

## Reporting

Report vulnerabilities privately through
[GitHub's private reporting](https://github.com/mirkobrombin/raven/security/advisories/new), not in a public issue or pull
request. Include the Raven revision, how to reproduce it, and what an attacker gains.

You will get an answer within seven days. Once the issue is confirmed, a fix and an advisory are prepared together; the
advisory credits you unless you prefer otherwise. Please give the fix up to 90 days before disclosing details publicly, and
tell us if you plan to publish sooner.

## Supported versions

Raven has no stable releases yet. Fixes land on the main branch.

## Scope

Raven is not a sandbox. Programs run with the user's permissions, and the host files Raven maps into a prefix are reachable
on purpose. Reports about what a program can do with those permissions are welcome as ordinary issues.
