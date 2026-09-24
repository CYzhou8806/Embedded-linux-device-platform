SUMMARY = "Zero-copy shared-memory pub/sub middleware for this device"
DESCRIPTION = "devbus: one publisher, many subscribers, over a POSIX \
shared-memory segment - loan/send instead of copying, per-subscriber \
lock-free queues and drop policies, crash reclaim via pidfd. Packaged \
from source unmodified (userspace/devbus/ at the repo root), not copied \
into this layer. See userspace/devbus/README.md and \
docs/devbus-experiments.md."
LICENSE = "CLOSED"

inherit cmake

# Same arrangement as device-service: point back at the single copy of
# the source under userspace/ (4 levels up from this recipe) rather than
# duplicating it inside the layer.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../userspace:"

# devbus-bench also compares against device-service's RingBuffer and
# includes it through ../device-service/include. Fetching that directory too
# recreates the same relative layout under ${WORKDIR} - without it the
# recipe fails in do_compile, which a development-host build (where the two
# trees are always side by side) never shows.
SRC_URI = "file://devbus \
           file://device-service/include \
          "

S = "${WORKDIR}/devbus"

# GTest isn't in the image. The tests are run on the development host and
# cross-compiled separately for on-target runs (experiments/rt-kernel/
# Containerfile), not built as part of the image.
EXTRA_OECMAKE = "-DBUILD_TESTING=OFF"

# Until this recipe existed, the binaries were scp'd to /tmp by hand and
# were gone after every reboot (the Yocto rootfs has no compiler, so
# rebuilding on target was never an option either). This is what that
# replaces.
#
# CMakeLists.txt's install(TARGETS ...) covers devbus-ls, devbus-bench,
# acq-bridge and the library; the two example programs are deliberately
# not installed - they exist to demonstrate the API on a development
# host, and acq-bridge is the one that carries real data.
