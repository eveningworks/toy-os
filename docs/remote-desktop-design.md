# Remote desktop: VNC and RDP servers

Another computer sees this screen and uses its keyboard and mouse.
Chosen from mockups on 2026-10-08: **both protocols**, a Settings page
with **one card per protocol**, a **corner notice** that asks before an
unknown viewer gets in, a **trusted-viewer list**, and the connected
sessions as a **shared widget**.

## What real systems do

- **Windows** ships an RDP server in the OS (TermService); Settings >
  System > Remote Desktop is one switch, a Network Level Authentication
  option and the port. RDP is always TLS, and NLA authenticates (CredSSP,
  NTLM or Kerberos) before a session exists.
- **macOS Screen Sharing** is VNC (RFB) with Apple's own authentication
  added beside a plain "VNC password" option.
- **GNOME** (gnome-remote-desktop) moved to RDP only in 46; **KDE** has
  krfb (VNC) and KRdp (RDP, Plasma 6.1). Both are SEPARATE PROCESSES that
  get the screen from the compositor (PipeWire screencast) and inject
  input through it (the RemoteDesktop portal, libei). krfb asks before
  accepting a connection and keeps a tray indicator while one is open.
- **X11's x11vnc** reads the framebuffer and injects with XTest, mapping
  a VNC keysym back to a keycode through the current keymap.

toy-os follows the shape: a separate ring-3 server (`/bin/remoted`),
the screen from the compositor, the prompt and indicator in the
compositor. It differs on INPUT -- next section.

## The layers

```
   viewer (TigerVNC, mstsc ...)
          |  TCP
   /bin/remoted ------------------- listener: config, `from`, spawn
   remoted session vnc PEER ------- one per viewer: RFB, DES, ZRLE
      |  screen                  |  keys + pointer
      v                          v
   toywm: WIN_REQ_SCREENSHOT   SYS_INPUT_INJECT  [new]
   (lib/ushot.h, unchanged)      |
                                 v
                        kernel input core (kernel/input.h)
                        keyboard_key_event / mouse  -- as a device
                                 |
                                 v
                        WIN_EV_RAW_* -> toywm, like any keyboard
```

## Decisions

**Input enters the kernel's input core, not the compositor** (Linux's
uinput, not libei). A key has to meet the layout, Caps Lock, dead keys,
the Ctrl encoding, the by-position stream games read and the key tap --
all in the kernel's keyboard path. Injecting above it means a second
encoder in toywm, and two encoders drift. The cost is a privilege check
the kernel has no model for: `SYS_INPUT_INJECT` answers only a process
spawned from `/bin/remoted` (the spawn-path identity rule the compositor
already uses for screensavers). Remote buttons sit in their own mask OR'd
with the devices' (`mouse_inject_buttons()`), and everything the server
holds is released when it exits.

**VNC sends characters, so the kernel finds the key**
(`keyboard_layout_find()`): the key and level that type a Latin-1
character on the active layout, Shift and AltGr pressed around it when
the level needs them -- x11vnc's approach. RDP sends scancodes
(`INPUT_INJECT_SCANCODE`) and needs no lookup.

**The screen is pulled and compared, not damage-tracked.** A session
captures through the existing screenshot request and compares 64x64
tiles with the frame the viewer has -- TigerVNC's comparing update
tracker. One request, no new compositor protocol; the cost is a
full-screen copy and compare per check (~30 a second while a viewer
waits). A damage hint from the compositor is the optimisation if it is
ever measured to matter.

**One process per viewer**, inetd's model: the stranger's bytes are
parsed in a process that holds nothing else, and each connection has its
own reader, which this TCP stack's retransmission timers need.

## Stages

1. **BUILT (2026-10-08)** -- `SYS_INPUT_INJECT`, `/bin/remoted`, VNC: RFB
   3.8/3.7/3.3, VNC Authentication, Raw and ZRLE at 8/16/32 bits, keys
   and pointer and wheel, `/etc/remote.conf`, the service.
   `tools/vnc_test.py`.
2. **Settings, the prompt, the indicator.** System Settings > Network >
   Remote Desktop (S2: a card per protocol). The P2 corner notice (Allow /
   View only / Deny, an "Always allow this address" box, denied after
   30 s) -- a `WIN_NOTICE_*` kind the compositor owns, answered back to
   the session over the client channel. The tray's Remote activity
   indicator lights for a desktop session (a kernel record taken from the
   SOCKET's peer, so a program cannot claim one), and its flyout gains
   View only and Disconnect. The connected-sessions list is a shared
   `uui_` widget.
3. **TLS on the server side.** mbedtls with `MBEDTLS_SSL_SRV_C`, a key
   and self-signed certificate made on first start, its fingerprint shown
   in Settings. VNC gains VeNCrypt (TigerVNC and Remmina encrypt; macOS
   falls back to the plain password).
4. **RDP.** X.224, MCS/GCC, capability exchange, licensing, fast-path
   input, bitmap updates (interleaved RLE or planar), over stage 3's TLS.
   NLA (CredSSP with NTLMv2) is what the Windows client asks for by
   default and is its own piece of work.

## Known limits

- **Plain VNC is DES over 8 characters, unencrypted** -- stage 3 is the
  answer, and the settings say so where the password is typed.
- **A character the layout has no key for is dropped**, as x11vnc drops
  it; a viewer on a different layout types what its keysyms say only
  where this layout can.
- **A fullscreen program holding a scanout lease cannot be shown**: the
  compositor refuses the capture (-EBUSY) and the viewer keeps its last
  frame.
- **A resolution change ends the session**; DesktopSize is read from the
  viewer but not yet sent.
