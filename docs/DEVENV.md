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

**Cause:** the ESP32-C3-Zero/SuperMini exposes its USB JTAG/serial debug
unit **natively** (no UART bridge; appears as `/dev/ttyACM*`, vendor:product
`303a:1001`). A hard reset of the chip — including the RTS pulse esptool
issues at the end of every `idf.py flash` — drops the USB device and
re-enumerates it. On re-enumeration the **device number** (`device=N` in the
USB `bus/device` pair) often increments (3 → 4 → 5 …).

virt-manager's *Add Hardware → USB Host Device* writes the *current* bus and
device number into the libvirt domain XML as an `<address>` element, so the
match is pinned to that one enumeration:

```xml
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor  id='0x303a'/>
    <product id='0x1001'/>
    <address bus='1' device='4'/>   <!-- ← pinned; breaks on re-enumeration -->
  </source>
</hostdev>
```

After re-enumeration libvirt is still looking for `bus='1' device='4'`; the
re-enumerated device is `device='5'`; the match fails and the device
vanishes from the guest until you re-add it.

### Workaround (GUI)

In virt-manager: remove the stale USB host device, then *Add Hardware → USB
Host Device* again (it picks up the current device number). This is what the
GUI forces you into, and it must be repeated whenever the number rolls.

### Proper fix (libvirt XML the GUI doesn't expose)

On the **host** (not inside the VM — libvirtd is host-side), edit the domain
XML and drop the `<address>` so the match is by vendor/product only. Then
re-enumeration (a new device number) no longer breaks it.

```bash
# on the HOST
virsh list --all                 # find the domain name
virsh edit <domain-name>
```

Find the `<hostdev>` for the board and delete its `<address …/>` line,
leaving:

```xml
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor  id='0x303a'/>
    <product id='0x1001'/>
  </source>
</hostdev>
```

Restart the VM (or `virsh attach-device --live` with the same fragment) to
apply. With `managed='yes'`, libvirt detaches the device from the host while
the VM runs and returns it when the VM stops; matching by vendor/product
alone means the board shows up in the guest regardless of which device
number it lands on.

> **Runtime re-attach caveat.** A boot-time hostdev is reliably re-bound on
> VM start. For a seamless re-attach *while the VM is running* after an
> in-flight reset (so `idf.py flash` → hard reset → port reappears with no
> intervention), modern libvirt's udev/nodedev integration usually re-binds
> matching devices. If you find the port still doesn't come back
> automatically, add a host udev rule to (re)attach on the device's `add`
> event:
>
> ```
> # /etc/udev/rules.d/90-espressif-c3.rules   (on the HOST)
> ACTION=="add",    SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="1001", \
>   RUN+="/usr/bin/virsh attach-device <domain> /etc/libvirt/usb/espc3.xml --live"
> ACTION=="remove", SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="1001", \
>   RUN+="/usr/bin/virsh detach-device <domain> /etc/libvirt/usb/espc3.xml --live"
> ```
>
> where `/etc/libvirt/usb/espc3.xml` is the vendor/product-only `<hostdev>`
> fragment above. **Try the plain vendor/product hostdev first** — you may
> well not need the udev rule at all.

### Two boards, no collision

When the WROOM-32 is added, its USB-UART bridge is a different vendor
entirely (CP2102 → `10c4:ea60`, or CH340/CH341 → `1a86:7523`; confirm with
`lsusb` on the host when you plug it in). So matching each board by
vendor/product is unambiguous — no bus/device address needed to
disambiguate.
