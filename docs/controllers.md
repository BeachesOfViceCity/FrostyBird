# Playing with a DualSense

Connect a PS5 DualSense with a USB **data** cable, or pair it once through
Linux Bluetooth settings. For pairing, hold **Create + PS** until the light
flashes and select the wireless controller. Once connected, run `./Play`.
At an Xbox **A** prompt, press **Cross (✕)**.

| DualSense | Xbox gamepad input |
| --- | --- |
| Cross / Circle / Square / Triangle | A / B / X / Y |
| Options / Create | Menu (Start) / View (Back) |
| D-pad | D-pad, including diagonals |
| Left and right sticks, L3 / R3 | Two analog sticks and their click buttons |
| L1 / R1 | Left / right shoulder |
| L2 / R2 | Independent analog triggers |

Standard low- and high-frequency rumble are routed back to the controller
that requested them. This is an Xbox gamepad interface: it does not expose
the PlayStation touchpad, gyro or adaptive-trigger effects.

## Reuse in other titles

The path is **controller → SDL2 gamepad mapping → WineBus → XInput**.
With mapping enabled, Wine presents one XInput-compatible DualSense or
DualSense Edge per physical pad. A raw Sony HID collection alone is not an
XInput device. Explicit WineBus HID overrides still take precedence. The
[portable WineBus patch](../patches/wine-mapped-dualsense.patch) includes
the Linux `hid-playstation` layouts for the Edge; older SDL2 Bluetooth
database entries use different button and axis numbers.

Any XInput client using this Wine build gets the mapped device. For an Xbox
GameInput v0 client, stage `build/guest/frosty-gameinput.dll` as
`GameInput.dll`, as the launcher already does. It exposes connected XInput
slots as separate devices, reports their identity and rumble capabilities,
and supplies immutable readings, reading callbacks, and connect/disconnect
notifications. The local GDK player recognises the same seat IDs. Keyboard
navigation still works when no pad is attached. There are no executable
names, title symbols, game assets, or game-specific action bindings in the
input path. Compatibility with a particular future title depends on the API
it uses; GTA 6 has not been tested.

## Verification

The player's Wine prefix detected the real Bluetooth controller as Sony
`054c:0ce6`, once in XInput slot 0 and once through GameInput as
`DualSense Wireless Controller`, with both rumble motors advertised.
Before the WineBus change, the same connected device yielded **zero XInput
slots**. For a five-second live reading display, run:

```bash
WINEPREFIX="$PWD/build/codec-test-prefix" WINEDEBUG=-all \
  build/wine64/wine build/guest/xs-controller-check.exe 054c 0ce6 --watch
```

The standalone conformance check uses a Linux uinput pad with the Edge
layout and an independent PE client compiled against the GameInput header.
USB and Bluetooth fixtures both passed button presses/releases, diagonals,
opposite stick extremes, independent triggers, historical readings and
disconnect/reconnect. A 25% / 75% rumble request reached the Linux device
as **16384 / 49151**; a stop request returned both motors to zero.
Evidence is in `build/controller-validation/physical-bluetooth.log` and
`build/controller-validation/layout-03/`.

To rebuild the local Wine and guest components, then repeat the fixture
check when the player is closed:

```bash
make -C build/wine64 -j6 dlls/winebus.sys/x86_64-windows/winebus.sys dlls/winebus.sys/winebus.so
cmake --build build/guest --target frosty_input frosty_local_grt xs-controller-check
python3 tools/controller_validation.py --out build/controller-validation/my-layout-check
```

The fixture requires write access to `/dev/uinput`. Playing with a physical
controller does not.
