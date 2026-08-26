# Debugging fbdev-multi on amdgpu

Symptom being chased: with `fbdev-multi` selected as the default DRM client,
no window manager works.

Commands here are bare shell, meant to be run on the test machine.

Relevant commits:

- `812462ea3de2` fix fbdev-multi never reaching a committable modeset array
- `269193394427` constrain the fbdev-multi re-probe to the allocated buffers

---

## Step 0 - build and install

    make -j$(nproc) && make modules_install && make install

Keep the old working kernel in the bootloader. Every step below assumes it can
be selected from the boot menu.

---

## Step 1 - is it fbdev-multi at all?

Two boots of the *same* kernel, changing only the command line. In GRUB press
`e`, edit the `linux` line, boot with `Ctrl-X`.

| Boot | Command line addition          | If the WM works                                      |
|------|--------------------------------|------------------------------------------------------|
| A    | `drm_client_lib.active=fbdev`       | fbdev-multi is the cause, not the rest of the commit |
| B    | `drm_client_lib.active=fbdev-multi` | reproduces the breakage                              |

The module parameter overrides the Kconfig default at runtime, so no rebuild is
needed between the two boots.

**If boot A also fails, the problem is not this client.** Stop here and say so;
everything below is scoped to fbdev-multi.

---

## Step 2 - capture the boot state

Boot B, then *before* starting the window manager:

    dmesg | grep -iE 'drm|amdgpu|fbdev|fb[0-9]' > /tmp/boot.log
    ls -l /dev/fb*
    cat /proc/fb

Expected on a working dual-monitor setup:

- two entries in `/proc/fb`
- two `fb%d: amdgpudrmfb frame buffer device (fbdev-multi)` lines in dmesg

One entry where two were expected means a sibling was never created. Zero means
the primary never got a buffer.

---

## Step 3 - the lines that matter

    dmesg | grep -iE 'CRTC set but no FB|Invalid source coordinates|no fb device|atomic'

| Match                                | Meaning                                                              |
|--------------------------------------|----------------------------------------------------------------------|
| `CRTC set but no FB`                 | the bug the first commit was meant to fix is still present            |
| `Invalid source coordinates`         | the `-ENOSPC` src-rect path: offsets or buffer sizing                 |
| `fbdev-multi: [CRTC:..] has no fb device` | a driven CRTC got no sibling; expected only for a connector that was dark at boot |
| nothing                              | the client is not failing commits; the WM breakage is elsewhere       |

If all of them are empty, reboot with `drm.debug=0x4` added to the command line
and run the grep again. The atomic check failures only print at that level.

---

## Step 4 - start the WM from a tty

Do **not** start it from a display manager, the error will be lost.

    sway -d 2>&1 | tee /tmp/sway.log

For X:

    startx 2>&1 | tee /tmp/x.log

then also collect `~/.local/share/xorg/Xorg.0.log`.

---

## Step 5 - if it hard-hangs

The original author's comment in `drm_fbdev_multi_sibling_set_par()`
(`drivers/gpu/drm/clients/drm_fbdev_multi_client.c`) records that sway wedged
two kworkers in `amdgpu_dm_atomic_commit_tail()` ->
`drm_atomic_helper_wait_for_flip_done()`, unkillable, reboot required. If that
is what happens, dmesg dies with the machine, so set up capture **before**
starting the WM.

Magic SysRq, screen photographed:

    sudo sysctl -w kernel.sysrq=1

On hang: `Alt+SysRq+w` (blocked tasks), then `Alt+SysRq+t` (all tasks).

Netconsole to a second machine, much better if available:

    sudo modprobe netconsole netconsole=6666@<this-ip>/,6666@<other-ip>/<other-mac>

and on the other machine:

    nc -u -l 6666

---

## Step 6 - what to send back

- `/tmp/boot.log`
- the Step 3 grep output
- `/tmp/sway.log` or the Xorg log
- for a hang: the SysRq photo or the netconsole capture

That is enough to tell whether this is the commit, the client's design, or
something unrelated.

---

## Note on expectations

There is no known path in the applied commits that commits to hardware while a
compositor holds the device, and compositor-plus-fbdev-multi on amdgpu was
documented as hanging by the original author before any of this was touched.

Step 1 is the cheap test that settles which it is. Do that one first.

## Note on the Kconfig default

`CONFIG_DRM_CLIENT_DEFAULT_FBDEV_MULTI` makes fbdev-multi the client for every
DRM device on the machine. Until this is proven, prefer leaving the default at
`fbdev` and opting in per boot with `drm_client_lib.active=fbdev-multi`.
