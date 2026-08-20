# lscpu

**a `/bin` program.**

## Synopsis

    lscpu

## Description

CPU identity and features, over `SYS_CPU_INFO`: vendor, brand, family/
model/stepping, measured or CPUID-reported speed, and which protection
features are actually ENABLED (SSE, NX, SMEP, SMAP).

Enabled rather than merely supported, which is the distinction worth
having: a CPU advertising SMAP that the kernel never turned on offers
no protection, and only one of those two numbers says so.
