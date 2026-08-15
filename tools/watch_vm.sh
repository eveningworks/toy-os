#!/usr/bin/env bash
# Attach a VIEW-ONLY VNC viewer to a headless toy-os VM, so a test run
# can be watched live without interfering with it.
#
# View-only is the whole point, not a preference. A connected viewer's
# real mouse motion goes into the same emulated PS/2 device the
# synthetic input uses, and the two fighting looks exactly like a flaky
# test rather than like interference -- see CLAUDE.md's QMP section.
# Remmina's quick-connect URI (`remmina -c vnc://host:port`) has no
# view-only option at all, so this writes a saved profile with
# viewonly=1 and launches that instead.
#
# The display number derives from the VM slot, the same way vm.py's
# does: slot N is VNC :5+N, i.e. TCP port 5900+5+N.
#
#   tools/watch_vm.sh          # slot 0 (vm.py start, qmp_test.py's default)
#   tools/watch_vm.sh 2        # slot 2 (vm.py --instance 2)
#   tools/watch_vm.sh 1 3      # gui_regress.py runs slots 0..3 -- one
#                              # viewer per argument
#
# Attaching or detaching mid-run is free; nothing in the VM notices.
set -euo pipefail

slots=("$@")
[ ${#slots[@]} -eq 0 ] && slots=(0)

profile_dir="${XDG_DATA_HOME:-$HOME/.local/share}/remmina"
mkdir -p "$profile_dir"

for slot in "${slots[@]}"; do
    case "$slot" in
        ''|*[!0-9]*) echo "watch_vm: slot must be a number, got '$slot'" >&2; exit 2 ;;
    esac
    display=$((5 + slot))
    port=$((5900 + display))
    profile="$profile_dir/toyos-vm-slot$slot.remmina"

    # Rewritten every run: cheap, and it keeps viewonly=1 true even if
    # the profile was edited in Remmina's UI at some point.
    cat > "$profile" <<EOF
[remmina]
name=toy-os VM slot $slot (:$display, view-only)
protocol=VNC
server=localhost:$port
viewonly=1
disableclipboard=1
disableserverinput=1
colordepth=32
quality=9
window_maximize=0
EOF

    # Warn, don't refuse: Remmina reconnects, so attaching before the
    # VM is up is a legitimate thing to do.
    if command -v ss >/dev/null 2>&1 && ! ss -ltn 2>/dev/null | grep -q ":$port "; then
        echo "watch_vm: nothing listening on port $port yet -- is slot $slot's VM up?" >&2
        if [ "$slot" = 0 ]; then
            echo "watch_vm:   python3 tools/vm.py start" >&2
        else
            echo "watch_vm:   python3 tools/vm.py --instance $slot start" >&2
        fi
    fi

    echo "watch_vm: slot $slot -> vnc://localhost:$port (view-only)"
    remmina -c "$profile" &
done

wait
