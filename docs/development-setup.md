# Setting up another machine (or a fork)

Everything a build needs is in the repository. This is the part that is
NOT -- the packages, the two group memberships, the SSH key, and the one
step that fails silently.

It lived in `README.md` until 2026-09-07 and moved here when that file
became a front page rather than a manual. `CLAUDE.md` points at it by
title.

---

Everything that matters is in the repository — the build, the tools in
`tools/`, and the docs — so a clone plus the packages above builds and
tests. Three things are **not** in the clone, and one of them fails
silently.

```bash
# 1. Packages (Arch/CachyOS; see the table above for other distributions)
sudo pacman -S --needed base-devel nasm grub xorriso mtools qemu-full python

# Optional. None are needed to build; each removes a rederive cost and
# docs/tools.md explains what for. ccache is the one worth having first,
# because the gate starts with `make clean` every time (2.37s -> 0.40s).
sudo pacman -S --needed ccache bear ruff shellcheck github-cli python-pillow docker

# 2. KVM, if you want tools/kvm_soak.py and the timing bugs TCG hides.
#    Docker is only for tools/qemu_matrix.py.
sudo usermod -aG kvm "$USER"
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
#    LOG OUT AND BACK IN -- group changes do not reach a running session.
#    Then: [ -w /dev/kvm ] && echo "KVM ok"

# 3. An SSH key, if you intend to push. (`gh auth login` can generate and
#    upload one for you instead -- choose SSH when it asks.)
ssh-keygen -t ed25519 -f ~/.ssh/github_key -C 'toy-os dev box'
cat >> ~/.ssh/config <<EOF

Host github.com
    HostName github.com
    User git
    IdentityFile ~/.ssh/github_key
    IdentitiesOnly yes
EOF
chmod 600 ~/.ssh/config ~/.ssh/github_key
cat ~/.ssh/github_key.pub     # add to GitHub -> Settings -> SSH keys
ssh -T git@github.com         # should greet you by name

# 4. Clone (substitute your fork's URL if you have one)
git clone git@github.com:eveningworks/toy-os.git
cd toy-os

# 5. THE COMMIT IDENTITY -- the step that fails silently.
#    It is per-repository, so the clone did NOT bring one, and commits
#    would use your GLOBAL identity: your real name and address. This
#    project scrubbed exactly that out of every prior commit with a
#    history rewrite, and nothing in git warns you beforehand.
git config --local user.name  'toy-os'
git config --local user.email 'noreply@toy-os.local'
#    On a fork, your own name and address are fine -- the point is that
#    it is a CHOICE rather than a leak. tools/preflight.sh refuses to run
#    until SOME local identity is set, which is where the check lives
#    because .git/hooks is not cloned either.

# 6. Confirm the machine before trusting a result from it
bash tools/preflight.sh                        # build + boot + both suites
python3 tools/gui_regress.py --logs /tmp/gui   # every GUI tool, as one table

# 7. gh, only for releases and `gh workflow run` -- not for push
gh auth login                                  # choose SSH as the protocol
```

**What you do not copy.** `disk.img` is gitignored and reseeded by
`make iso`; `build/`, `toy-os.iso` and `compile_commands.json` are all
regenerated. Nothing needs a GitHub token in the environment — the whole
build and test path is offline, so you can work for a week without
authenticating and only need it to push.

**On a dedicated box.** KVM wants bare metal rather than a nested VM —
`tools/kvm_soak.py` exists for what TCG cannot show. And the host's QEMU
version is a real variable: `tools/qemu_matrix.py` exists because a
virtio-blk defect was invisible on one version and reproduced every time
on another, so keeping two machines on the same distribution means a
difference between them is your code rather than your toolchain.

## A Claude Code cloud session (Ubuntu 24.04)

The steps above are for the maintainer's bare-metal CachyOS box. A
session on claude.ai/code runs in a throwaway Ubuntu 24.04 container
instead, and differs in seven ways:

- **Nothing is installed.** gcc, binutils, make, gdb, ruff and python3
  are there; nasm, QEMU and GRUB are not. Put this in the cloud
  environment's **Setup script** (network access `Trusted` reaches the
  Ubuntu mirrors and PyPI) so every session starts ready:

  ```bash
  #!/bin/bash
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq -o Acquire::Retries=3
  apt-get install -y -qq -o Acquire::Retries=3 \
    nasm grub-pc-bin grub-common xorriso mtools libxkbcommon-tools \
    qemu-system-x86 ccache shellcheck
  python3 -m pip install -q --root-user-action=ignore pillow
  ```

- **Pillow comes from pip, not apt.** `python3` is a separate build in
  `/usr/local`, which cannot load apt's `python3-pil`; the gate's
  `genttf.py --check` fails on the import.
- **The commit identity is the container's, not yours.** Its global git
  config says `Claude <noreply@anthropic.com>`, and step 5 above has not
  run in a fresh clone. Set it in the environment's **Environment
  variables**, which override any config:

  ```
  GIT_AUTHOR_NAME=toy-os
  GIT_AUTHOR_EMAIL=noreply@toy-os.local
  GIT_COMMITTER_NAME=toy-os
  GIT_COMMITTER_EMAIL=noreply@toy-os.local
  ```

  `preflight.sh` still wants a LOCAL identity, so run step 5 as well.
- **binutils is 2.42**, older than CachyOS's. It cannot link userland
  above 4 GiB unless GOT loads are left unrelaxed, which is what
  `-Wa,-mrelax-relocations=no` in `USERLAND_CFLAGS` is for; the
  Makefile comment beside it has the mechanism.
- **Everything runs TCG.** There is no `/dev/kvm` and no Docker daemon,
  so `kvm_soak.py`, `vm.py --kvm` and `qemu_matrix.py` cannot run, and
  the cloud box answers nothing about the two bug classes TCG hides.
  With four cores `gui_regress.py` runs two jobs at a time.
- **Outbound HTTPS goes through a policy proxy**, which refuses
  ftp.gnu.org, sourceware.org and GitHub archive downloads. The normal
  build is offline and never notices; `EXTRAS=1` fetches from the
  network and has not been tried there.
- **The container is reclaimed when the session ends**, so unpushed work
  is lost. A session pushes to its own `claude/...` branch rather than
  `main`, and has no `gh` -- the maintainer merges. No other QEMU runs in
  the container, so the "ask the user to close their QEMU" rule in
  `CLAUDE.md` does not apply there.
