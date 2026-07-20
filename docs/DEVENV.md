# ESP32AntTest — Development Environment

Operational notes on the dev VM and host setup: how the boards and toolchains
are wired into the workstation. This is the environment layer, not the
project's own hardware/architecture (for those see
[HARDWARE.md](HARDWARE.md) / [SPEC.md](SPEC.md)).

## VM overview

- QEMU/KVM guest, managed via **virt-manager** (libvirt).
- A host-shared folder is mounted at `/home/elliot/projects/share/lib/` —
  bulky/reusable tooling lives there to conserve VM disk (see "Shared dev
  resources" in [AGENTS.md](../AGENTS.md)): ESP-IDF v5.2.3 at `.../esp/esp-idf`, etc.
- ESP32 boards are passed through host → VM as USB devices so `idf.py` can
  flash/monitor them from inside the guest.

## USB passthrough: the re-enumeration trap

**Symptom (inside the VM):** the ESP32-C3's serial port (`/dev/ttyACM0`)
disappears mid-session — `ls /dev/ttyACM0` finds nothing, `lsusb` shows no
Espressif device, and `idf.py flash` fails with `Could not open /dev/ttyACM0
… No such device`. The guest kernel log shows a clean disconnect:

```
kernel: usb 1-4: USB disconnect, device number 3
```

**Root cause:** the ESP32-C3-Zero/SuperMini exposes its USB JTAG/serial debug
unit **natively** (no UART bridge; appears as `/dev/ttyACM*`, vendor:product
`303a:1001`). A hard reset of the chip — including the RTS pulse esptool
issues at the end of every `idf.py flash` — drops the USB device and
re-enumerates it. On re-enumeration the **device number** (`device=N` in the
USB `bus/device` pair) often increments (3 → 4 → 5 …).

virt-manager's *Add Hardware → USB Host Device* writes the *current* bus and
device number into the libvirt domain XML as a host `<address>` element
inside `<source>`, so the match is pinned to that one enumeration:

```xml
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor  id='0x303a'/>
    <product id='0x1001'/>
    <address bus='1' device='4'/>   <!-- ← pinned to host bus/device; breaks on re-enumeration -->
  </source>
  <address type='usb' bus='0' port='4'/>   <!-- guest-side virtual port; stable, unrelated -->
</hostdev>
```

After re-enumeration libvirt is still looking for host `bus='1' device='4'`;
the re-enumerated device is `device='5'`; the match fails and the device
vanishes from the guest until you re-add it.

> **Two `<address>` lines, only one is the problem.** The `<address>` inside
> `<source>` (no `type=` attribute) is the *host* bus/device — the one that
> rolls. The `<address type='usb' ...>` *after* `</source>` is the
> *guest-side* virtual port and is stable; leave it alone.

## Current setup (what works, and what you manage by hand)

The accepted operating mode for this project:

1. **Pass the board through via virt-manager** (*Add Hardware → USB Host
   Device*). This writes the vendor/product + current host `<address>` into
   the domain XML and attaches it live.
2. **Hand-edit the `<source>` to add `startupPolicy='optional'`** (the GUI
   doesn't expose it). On the host:

   ```bash
   virsh edit <domain>
   ```

   ```xml
   <source startupPolicy='optional'>
     <vendor  id='0x303a'/>
     <product id='0x1001'/>
     <address bus='1' device='96'/>
   </source>
   ```

   `startupPolicy='optional'` means the VM boots even if the board is
   unplugged (libvirt marks the hostdev `absent='yes'` at boot); when the
   board *is* plugged in, it attaches normally. This is purely a boot-time
   robustness tweak — it does **not** affect runtime behavior.

3. **When a flash-induced reset re-enumerates the device and `/dev/ttyACM0`
   vanishes mid-session:** re-add it from the host. Either:
   - virt-manager: remove the stale USB host device → *Add Hardware → USB
     Host Device* (picks up the new device number), or
   - `virsh reboot <domain>` (libvirt re-resolves vendor/product → bus/device
     fresh on VM start, so the new number is picked up automatically).

   This is manual friction, but infrequent for a dev setup, and it's the
   reliable path.

> **Why not just drop the host `<address>` from `<source>` (vendor/product
> only)?** That *is* a valid libvirt form and does help at VM-restart
> re-resolution — but it does **not** help with in-flight re-enumeration:
> libvirt binds to a concrete host device object at attach time and does not
> re-resolve when that object disappears mid-session. Since virt-manager
> rewrites the whole `<hostdev>` (and re-pins the `<address>`) whenever you
> re-add via the GUI, keeping the `startupPolicy='optional'` + re-add-on-reset
> workflow is simpler than chasing a vendor/product-only config the GUI keeps
> overwriting. If you mostly edit the XML by hand, dropping the host
> `<address>` is a reasonable refinement — see the "Tried and abandoned" note
> on udev hotplug below for the full picture.

## Tried and abandoned: udev-rule-driven hotplug

To eliminate the manual re-add, we attempted a host udev rule that calls
`virsh attach-device`/`detach-device --live` on the board's `add`/`remove`
(and `bind`/`unbind`) events. **This did not work reliably and is not
recommended** — recorded here so future-self doesn't burn time re-deriving it.

**Failure modes observed:**

- **Attach/detach feedback loop.** When libvirt attaches a USB device to the
  guest, it *detaches it from the host driver* — which itself fires host
  `remove`/`unbind` events. A `detach-device` rule keyed on those events then
  yanks the device back to the host, which re-binds the host `cdc_acm`
  driver, which fires `add`/`bind` → attach → host-unbind → detach → … The
  device ends up in a ~0.8 s reset loop and visible on **neither** side
  stably. Gating the detach rule on `ENV{BUSNUM}=="?*"` (present only on
  genuine physical events) is *not* sufficient to prevent this.
- **Collision with the persistent (boot) hostdev.** When the device is also
  in the VM's persistent config (our case, for `startupPolicy`), the udev
  `add`/`bind` attach fights with the boot-time attach for the same device.
- **Modern udev actions.** Kernels since ~4.12 (2017) emit `bind`/`unbind`
  events in addition to `add`/`remove`; a rule that only matches the latter
  silently does nothing on a modern host. Any hand-rolled rule must handle
  `ACTION=="add|bind"` and `ACTION=="remove|unbind"`.

**If this ever needs to be made fully automatic,** the maintained-shape
upgrade path is the [`olavmrk/usb-libvirt-hotplug`](https://github.com/olavmrk/usb-libvirt-hotplug)
script (a udev-driven `virsh attach/detach-device` wrapper). Caveats: the
upstream project is effectively abandoned (last push 2022, open PRs
unmerged), and it **does not work on modern kernels as-is** — it rejects
`bind`/`unbind` actions and exits 1. Apply PR #6 ("Tolerate new udev actions
bind/unbind") and use a tempfile for the device XML (issue #5) instead of
`/dev/stdin`. Even then, for a device *also* in the persistent config, the
feedback-loop hazard above applies — that use case is genuinely finicky and
not worth the complexity for this single-board dev setup.

## Two boards, no collision

When the WROOM-32 is added, its USB-UART bridge is a different vendor
entirely (CP2102 → `10c4:ea60`, or CH340/CH341 → `1a86:7523`; confirm with
`lsusb` on the host when you plug it in). So matching each board by
vendor/product is unambiguous — no host bus/device address needed to
disambiguate.
