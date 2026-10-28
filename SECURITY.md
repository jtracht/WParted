<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Security policy

## Supported versions

WParted is in alpha. Only the latest pre-release receives fixes.

## Reporting a vulnerability

Please **do not** open a public issue for vulnerabilities. Report privately to the maintainer via
GitHub's private vulnerability reporting (https://github.com/jtracht/WParted/security), or by e-mail to the address on the
maintainer's GitHub profile (https://github.com/jtracht). Include affected version, environment and a minimal
reproduction. We aim to acknowledge reports within 7 days.

Data-loss bugs (a disk changed when it should not have) are treated as security issues.

## Design

See [docs/architecture/security-architecture.md](docs/architecture/security-architecture.md). WParted
sends no telemetry and performs no automatic update checks.
