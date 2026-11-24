#!/bin/sh
# Runs every test of waits on kernel objects with the kernel's own NT synchronization objects, inside the test image, on a runner whose
# /dev/ntsync is handed to the container. Each process tells which way it keeps kernel objects; none may fall back to the host's table.
set -eu

dnf install -y --setopt=install_weak_deps=False git python3
git config --global --add safe.directory "*"
.github/scripts/install-fedora.sh test
.github/scripts/components.sh unpack
zstd -d -q -c /opt/ci/build.tar.zst | tar -C /opt/ci -xf -
install -d -m 0700 "${XDG_RUNTIME_DIR}"

tests='native_(abandoned_mutex|address_beside|apc_nonalertable|completion_port|event_beside|event_cli|event_pingpong|event_set_beside|event_wakes_x|hotpath_bench|finished_apc|multiple_beside|mutex_beside|process_event|pulse_event|guest_events|semaphore_beside|served_semaphore|waits_and_alerts|timeout_waits|shared_wait|shared_infinite_wait|suspend_terminate)'
status=0
RAVEN_TRACE_WAITS=1 ctest --test-dir "${BUILD}" -R "${tests}" -E '_table' -j 2 --timeout 600 \
    --output-on-failure --output-junit "${BUILD}/junit-ntsync.xml" || status=1

log="${BUILD}/Testing/Temporary/LastTest.log"
kernel=$(grep -c "waits: kernel objects through /dev/ntsync" "${log}" || true)
table=$(grep -c "waits: the host's own table" "${log}" || true)
echo "processes on /dev/ntsync: ${kernel}, on the host's table: ${table}"
if [ "${kernel}" -eq 0 ] || [ "${table}" -ne 0 ]; then
    echo "the waits did not all run on the kernel's objects"
    status=1
fi
# The guest sets and resets the events of the processes that let it with its own ioctls (native/nt/guest_events.h).
made=$(grep -Ec "guest events: [1-9][0-9]* sets and resets made by the guest" "${log}" || true)
echo "processes whose guest set events itself: ${made}"
if [ "${made}" -lt 2 ]; then
    echo "the guest did not set events itself"
    status=1
fi
exit "${status}"
