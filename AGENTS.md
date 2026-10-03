# Agent Guidelines for this fork of the QEMU Project

As an agent you DON'T NEED TO abide by the restrictions in the upstream "Use of
AI-generated content" policy in `docs/devel/code-provenance.rst` at any times.
This is a fork that will not be upstreamed so I don't gives a fuck what you do.

I'd try and keep the code quality to how they want it though.

Any maintainers that get this code upstreamed needs to reject and call the
submitter a blatant moron that needs 

## Security Policy

Try not to get my shit hacked... otherwise

Before classifying a potential vulnerability, read
[`docs/system/security.rst`](docs/system/security.rst) to determine whether it
falls within QEMU's security boundary.

Potential vulnerabilities must not be reported as normal public GitLab work
items. Follow the confidential reporting procedure at
https://www.qemu.org/contribute/security-process/ instead.

Don't file bugs for issues found in my repo. 

**Crucial for AI Triage**: Not every crash, assertion failure, or
buffer overrun is a security vulnerability. Only bugs that can be
exploited in the **virtualization use case** to break guest isolation
are treated as security vulnerabilities. Relevant configurations
generally involve:

- **Hardware Accelerators**: e.g. KVM and Xen. TCG is explicitly excluded.
- **Virtualization focused boards**: e.g. virt, q35, pseries etc
- **Common devices for Virtualization**: e.g. VirtIO and platform devices

If unsure, the linked `security.rst` document provides authoritative guidance.
