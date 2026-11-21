# Building the echoroot image

How to rebuild `sdcard.img` from this repository in 2026, what the build
depends on, and how to add software to the image.

**Status:** done on 2026-11-20 on Linux Mint 22.3 with Docker 29. The result
was compared with an image built from the same commit in June 2024: the same
7,749 files, every Amazon-supplied file byte-identical (kernel, DSP firmware,
`asrd`, `ledd`, `libpryon.so`, ...). 59 files differ, all of them recompiled
Buildroot programs plus the regenerated password hash. The new image has not
been booted on an Echo.

## Quick start

```sh
git submodule update --init          # buildroot fork, u-boot, porcupine
make -C rootfs -j8 download          # 306 Amazon packages -> rootfs/dl (148 MB)
docker/build.sh make -C rootfs/br2echo olddefconfig   # once; see "Changes"
docker/build.sh                      # full build
```

Result: `sdcard/images/sdcard.img` (2.0 GB). Write it to a card as described
in `page/SETUP.md`.

First build: about 80 minutes on 16 cores, most of it Buildroot (167
packages). Disk: about 15 GB, of which 9.8 GB is `rootfs/br2echo`. A rebuild
after changing only the rootfs merge or the image takes about 30 seconds.

## What the build does

| Stage | Where | Output |
|---|---|---|
| Fetch Amazon's firmware packages | `rootfs/Makefile` (`download`, `extract`) | `rootfs/dl/*.ipk`, unpacked into `rootfs/orig` |
| Build a newer base system and toolchain | Buildroot 2017.05.2 fork, config in `rootfs/br2echo/.config` | `rootfs/br2echo/target`, `rootfs/br2echo/host` |
| Merge both | `rootfs/Makefile` (`merge`) | `rootfs/out`: Amazon's tree with glibc 2.23, busybox, init scripts and selected programs from Buildroot copied over it |
| Build the two bootloaders | `u-boot` (2010.06 fork), configs `misto_sd` and `misto_int` | `u-boot/MISTO_SD/u-boot.bin` for the card, `u-boot/MISTO_INT/u-boot.bin` for internal flash |
| Build the voice demos | `voice/Makefile` | `porcupine_demo_shm`, `porcupine_send_shm` |
| Post-step | top-level `Makefile` | Porcupine library and models, the internal u-boot and the demos copied into `rootfs/out` |
| Assemble the image | `sdcard/Makefile`, `sdcard/genimage.cfg` | 16 MB FAT (`MLO`, `u-boot.bin`) plus two identical 1000 MB ext3 partitions |

The first-stage loader `x-load/MLO` is a prebuilt binary in the repository and
is not rebuilt.

## Inputs and how to keep them

| Input | Source | Preserved by |
|---|---|---|
| 306 Amazon `.ipk` packages | `https://d1s31zyz7dcc2d.cloudfront.net/` (URLs in `rootfs/Makefile`) | `rootfs/dl` |
| Buildroot fork | `github.com/AndreRH/buildroot`, commit `9725cbd` | git submodule |
| u-boot fork | `gitlab.com/AndreRH/u-boot-echo`, commit `e1ab914` | git submodule |
| Porcupine | `github.com/Picovoice/porcupine`, commit `b99e652` | git submodule |
| 155 source archives for Buildroot packages | upstream sites, with `sources.buildroot.net` as fallback | `rootfs/buildroot/dl` (554 MB) |
| Build environment | Ubuntu 18.04 packages | Docker image `echoroot-build` |

All of these were still downloadable on 2026-11-20. The Amazon package server
is the only source that cannot be replaced; the packages contain the kernel,
the DSP firmware and the wake-word engine. Keep copies outside this machine:

```sh
tar cf echoroot-inputs.tar rootfs/dl rootfs/buildroot/dl .git/modules
docker save echoroot-build | xz > echoroot-build-image.tar.xz
```

With `rootfs/dl` and `rootfs/buildroot/dl` in place the build needs no network
access apart from creating the Docker image.

`rootfs/dl` is listed in `.gitignore`. The packages are Amazon's proprietary
firmware; do not commit them.

The archives do not contain this repository itself. Commit the changes listed
under "Changes made to the repository" and keep the repository somewhere safe
as well; `docker/` and the `rootfs/Makefile` fix exist nowhere else.

### Restoring from the archives

On a machine with Docker and a clone of this repository:

```sh
cd echoroot
tar xf /path/to/echoroot-inputs.tar            # rootfs/dl, rootfs/buildroot/dl, .git/modules
git submodule update --init                    # checks out from .git/modules, no network
xz -dc /path/to/echoroot-build-image.tar.xz | docker load
docker/build.sh                                # full build
```

Notes:

- Extract the tar at the top of the repository; its paths are relative to it.
- `git submodule update --init` finds the submodule data in `.git/modules` and
  does not fetch. Checked in a scratch clone with networking disabled, with
  `rootfs/buildroot/dl` already in place.
- `docker/build.sh` uses an existing `echoroot-build` image as it is. It only
  runs `docker build`, which needs Ubuntu's 18.04 package servers, when the
  image is missing or `REBUILD_IMAGE=1` is set.
- The image contains a `build` user with the user and group ID of whoever
  created it (1000 here). If your ID on the new machine differs, the build
  cannot write to the tree. Either rebuild the image there, or run the
  container with `--user "$(id -u):$(id -g)" -e HOME=/tmp` added to the
  `docker run` line in `docker/build.sh` (untested).
- With both archives restored, the build should need no network: Buildroot
  finds its source archives in `rootfs/buildroot/dl` and the Amazon packages
  are in `rootfs/dl`. A complete rebuild with networking disabled has not
  been tested.

The package URLs contain a hash-like directory name. It is not the MD5 of the
file, so it cannot be used to verify downloads. All 306 files were checked to
be valid Debian package archives.

The submodule for Buildroot is pinned some commits behind the fork's
branch head. The build uses the pinned commit, as recorded in this repository.

## Build environment

`docker/Dockerfile` defines the environment and `docker/build.sh` runs a
command in it (default `make`):

```sh
docker/build.sh                    # full build
docker/build.sh make -C rootfs     # rootfs only
docker/build.sh bash               # shell in the container
```

Why a container, and why these choices:

- **Ubuntu 18.04.** Buildroot 2017.05 does not build its host tools on a
  current distribution. 18.04 is the newest release whose glibc (2.27) still
  works unpatched, and it has the `gcc-7` ARM cross compiler that
  u-boot 2010.06 needs. Mint 22.3 has neither.
- **Mounted at `/sata/src/echoroot`.** `rootfs/br2echo/Makefile` is generated
  by Buildroot, is committed, and contains the absolute path of the original
  author's checkout. Mounting the tree at that path avoids editing it.
- **Runs as your user.** Files in the tree keep your ownership. Files inside
  the image are still owned by root, because `sdcard/Makefile` creates the
  filesystem under `fakeroot`.
- **`CROSS_COMPILE=arm-linux-gnueabihf-`** is set for u-boot. Buildroot
  ignores it and uses its own toolchain.

## Changes made to the repository

| File | Change | Why |
|---|---|---|
| `docker/Dockerfile`, `docker/build.sh` | New | Build environment |
| `rootfs/Makefile` | `ar --output=DIR x` replaced by `cd DIR && ar x` | `--output` needs binutils 2.34; Ubuntu 18.04 has 2.30 |
| `rootfs/br2echo/.config` | Refreshed with `olddefconfig` | See below |
| `rootfs/br2echo/Makefile` | Header comment only | Rewritten by `olddefconfig` |
| `.gitignore` | Build outputs added | `rootfs/dl`, `rootfs/orig`, `rootfs/out`, Buildroot output, images, logs |

The `.config` refresh: the committed file was written on a machine where
Buildroot could not detect the host compiler version. With a detectable
compiler, new questions appear and a non-interactive build stops with
`Console input/output is redirected`. `olddefconfig` answers them with the
defaults, which leaves every newly visible package disabled. It also drops two
`BR2_PACKAGE_MPD_VERSION` lines that the pinned Buildroot commit no longer
knows. No package selection changed.

## Problems found and their fixes

| Symptom | Cause | Fix |
|---|---|---|
| `ar: unrecognized option '--output'` | binutils older than 2.34 | Makefile change above |
| `Console input/output is redirected. Run 'make oldconfig'` | Stale `.config`, see above | `docker/build.sh make -C rootfs/br2echo olddefconfig` |
| samba4 fails with `arm-binfmt-P: Could not open '/lib/ld-linux.so.3'` | The Buildroot fork expects `compile_et` on the build machine; without it samba4 builds one for ARM and tries to run it | `comerr-dev` in the Dockerfile |
| `sh: 1: mkdosfs: not found` in the last step | `genimage` calls FAT tools from the build machine | `dosfstools` and `mtools` in the Dockerfile |
| `/etc/group` in the image has a duplicated `sshd` line | The merge step hard-links `rootfs/orig` into `rootfs/out` and then appends to files, so repeated merges modify `orig` | Run `make -C rootfs clean` before a release build |

The samba4 failure message is specific to machines with `qemu-user-static`
registered, as this one is. Elsewhere the same cause would show as an
"Exec format error".

## Release build

For an image you intend to flash, start the merge from a clean state:

```sh
docker/build.sh sh -c 'make -C rootfs clean && make'
```

`make -C rootfs clean` removes `rootfs/orig` and `rootfs/out` only. It keeps
the downloaded packages and the Buildroot output, so this takes about 30
seconds.

## What a fresh image does not contain

A fresh image is generic. Compared with a card that has been set up:

- `/etc/network/interfaces` has only the loopback interface. The `wlan0` block
  from `page/SETUP.md` has to be added.
- `/etc/wpa_supplicant.conf` has no network. Add one with `wpa_passphrase`.
- The root password is `echo`.

Both ext3 partitions are identical. `install.sh` copies the third partition to
the Echo's internal flash, so settings made on the second partition do not
carry over unless they are also made on the third.

## Adding software

Constraints that apply to anything added:

- Kernel 2.6.37, so glibc 2.23 at most. Software that needs newer kernel
  interfaces will not run.
- ARMv7 Cortex-A8, soft-float calling convention (`arm-linux-gnueabi`).
  Hard-float binaries do not load without tricks.
- About 200 MB of RAM for Linux; the rest is reserved for the DSP.
- About 600 MB free on the 1000 MB root partition.

### A program of your own

The cross compiler is `rootfs/br2echo/host/usr/bin/arm-buildroot-linux-gnueabi-gcc`
(gcc 6.5), with headers and libraries for everything Buildroot built,
including ALSA, PulseAudio, libcurl, OpenSSL, speexdsp and D-Bus.

```sh
docker/build.sh rootfs/br2echo/host/usr/bin/arm-buildroot-linux-gnueabi-gcc \
    -O2 -o hello hello.c
scp hello root@<echo-ip>:/usr/bin/
```

Checked: a test program built this way runs against the image's root
filesystem under `qemu-arm` and reports glibc 2.23.

Libraries that exist only in Amazon's part of the image (`libshmbuffer.so`,
`liblipc.so`, `libpryon.so`) have no headers. Link against the copies in
`rootfs/orig/usr/local/lib` and declare the functions yourself.

To include the program in the image, follow the pattern of the voice demos:
build it in a rule in the top-level `Makefile` and copy it into `rootfs/out`
in the `rootfs/out/.stamp_post` rule.

### A package that Buildroot knows

```sh
docker/build.sh make -C rootfs/br2echo menuconfig    # select the package
docker/build.sh make -C rootfs/br2echo               # build it
```

Then add `cp` lines for its files to the `merge` rule in `rootfs/Makefile`.
Buildroot's whole target tree is not copied into the image, only the files
named there. Buildroot 2017.05 package versions are from 2017.

### Changing files in the image

- Init scripts and configuration live in `rootfs/additional` and are copied in
  by the `merge` rule.
- For one-off changes on a card, mount its second partition on a PC.

### Faster iteration

During development, copy binaries to the running Echo with `scp` and rebuild
the image only for releases.

## Layout of build outputs

| Path | Content | Size |
|---|---|---|
| `rootfs/dl` | Amazon packages | 148 MB |
| `rootfs/orig` | Unpacked Amazon tree | 277 MB |
| `rootfs/buildroot/dl` | Buildroot source archives | 554 MB |
| `rootfs/br2echo` | Buildroot output: `host` (toolchain), `target`, `build` | 9.8 GB |
| `rootfs/out` | Merged root filesystem | 89 MB |
| `u-boot/MISTO_SD`, `u-boot/MISTO_INT` | Bootloader builds | small |
| `sdcard/images` | `sdcard.img` and its parts | 3.0 GB |
| `build.log*` | Logs from the builds on 2026-11-20 | |

`rootfs/br2echo/build` can be deleted to save space once the build works; the
toolchain in `rootfs/br2echo/host` and the files in `target` are what later
steps use. Deleting it means a full rebuild if a Buildroot package changes.
