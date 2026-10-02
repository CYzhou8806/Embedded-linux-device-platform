# Case 12: pseudo "inode mismatch": Kbuild's clean reached the packaging directories

**Platform:** Yocto Scarthgap build host (Ubuntu 24.04), `custom-acq-driver` recipe ([`yocto/meta-device-platform/recipes-kernel/custom-acq-driver/`](../../yocto/meta-device-platform/recipes-kernel/custom-acq-driver/)), both build directories (6.6 and 6.12 kernels)
**Occurred:** recurring since the security work (2026-09-24); root-caused on 2026-10-01 while adding the M8 tracepoints

## Symptom

Every time the kernel changed, or the driver's `do_configure` ran again
for any other reason, the next build failed in `do_package`:

```
ERROR: Task (.../custom-acq-driver_1.0.bb:do_package) failed with exit code '134'
```

`pseudo.log` in the recipe's work directory:

```
inode mismatch: '.../custom-acq-driver/1.0/package/usr/lib/modules/6.12.93-v8-16k/updates/custom_acq.ko.xz'
    ino 24436070 in db, 18260350 in request.
path mismatch [2 links]: ino 18260350 db '.../sstate-build-package/packages-split/...custom_acq.ko.xz'
    req '.../sstate-build-package/package/...custom_acq.ko.xz'.
```

The workaround was `bitbake -c clean custom-acq-driver`, which throws
away pseudo's database with everything else. It was recorded as "can be
done but wasn't" in the security review notes, because the workaround
always worked.

## What pseudo is complaining about

pseudo is the fakeroot that `do_install` and `do_package` run under. It
keeps a database of the file ownership and modes it has faked, keyed by
path *and* inode. If a file at a known path suddenly has a different
inode, something replaced it behind pseudo's back. pseudo can't tell
whether its record is stale or the new file is, and it aborts rather
than guess, hence 134 (SIGABRT).

So the question was which process deletes `custom_acq.ko.xz` outside of
pseudo between two runs of `do_package`.

## Finding it

Only this recipe was affected, never device-service or devbus, which are
also built by this layer. Two things set it apart: it is a kernel module
(`inherit module`), and it set `S = "${WORKDIR}"`. That made the source
directory the same directory that holds `package/`, `packages-split/`,
`sstate-build-package/` and `pseudo/`.

`module.bbclass`'s `do_configure` runs `make clean`, and the configure log
showed what that means here:

```
make -C .../work-shared/raspberrypi5/kernel-source M=.../custom-acq-driver/1.0 clean
```

`M` is `$S`, which is the whole work directory. The kernel's own clean
rule for external modules (`Makefile`, line 1967 in this tree):

```make
@find $(or $(KBUILD_EXTMOD), .) $(RCS_FIND_IGNORE) \
	\( -name '*.[aios]' -o -name '*.rsi' -o -name '*.ko' -o -name '.*.cmd' \
	-o -name '*.ko.*' \
	...
```

That is a recursive `find` over all of `M`, deleting every `*.ko` and
`*.ko.*`. It deletes the compressed modules from the previous build's
`package/`, `packages-split/` and `sstate-build-package/` trees. It runs in
`do_configure`, which is not under pseudo, so pseudo never learns the
files are gone. The next `do_package` writes new files at the same paths
with new inodes, and pseudo aborts.

That accounts for all the facts: why only this recipe, why only after
`do_configure` re-runs (a kernel change invalidates it), and why
`-c clean` fixes it (no old files, no old database).

## Fix

Unpack the sources into their own subdirectory, so `M` contains only the
driver:

```bitbake
SRC_URI = "file://custom_acq.c;subdir=custom-acq \
           file://custom_acq_trace.h;subdir=custom-acq \
           file://Makefile;subdir=custom-acq \
          "
S = "${WORKDIR}/custom-acq"
```

**Verified** with the sequence that used to fail every time. On the old
recipe, a build after the 6.12 kernel had changed failed with exactly the
log above. On the new recipe: clean build, then `bitbake -f -c configure
custom-acq-driver` (force the `make clean`), then a full build again, and
`do_package` passed. In the 6.6 build directory only a clean build was
run, which also passed. The forced-reconfigure repeat was done on 6.12 only.

## What generalises

- **`S = "${WORKDIR}"` is a trap whenever the build system cleans
  recursively.** Here the build system is Kbuild, which has no idea it is
  sitting inside a Yocto work directory. Newer Yocto releases deprecate
  `S = "${WORKDIR}"` for this class of reason.
- **A workaround that always works stops anyone asking why.** The
  `-c clean` habit hid a deterministic, explainable bug for a week. The
  facts that pointed at the cause (only this recipe, only after configure)
  were there from the first occurrence.
