# uptime

**a `/bin` program.**

**Category:** System information

## Synopsis

    uptime

## Description

How long the machine has been up, as a duration. `/bin/uptime`, over `SYS_MONOTONIC_NS` — uptime is an INTERVAL, and the RTC can step, so wall clock is not an implementation of it. The builtin printed raw ticks, which answers "is the timer running" rather than "how long has this been up".