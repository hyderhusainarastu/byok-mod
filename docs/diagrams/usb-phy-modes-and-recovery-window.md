# USB PHY ownership and the boot recovery window

The ESP32-S3 in this hardware exposes two independent USB controllers — USB-Serial-JTAG and USB-OTG — onto one shared internal PHY and one physical USB-C port, with no external mux, so whichever controller last calls `usb_new_phy()` owns the pads until the next call. On stock firmware that hand-over happens automatically at 4.14 s into every normal boot (measured teardown at 4.242–4.243 s across three trials), closing the USB-Serial-JTAG link that `esptool` needs for driver-free download-mode recovery; three other boot arms (Disk Mode, USB console, missing SD card) never make that call and so never lose the link. The modified firmware turns the accidental stock window into a deliberate one: it hands the PHY back to USB-Serial-JTAG as the third action of boot and holds off starting the USB-OTG CDC link until a configured minimum uptime (5000 ms as shipped), because a warm restart on this chip does not reset the PHY mux or the USB peripherals — without that explicit hand-back, a crash-looping image would come back with no console and no recovery path short of a physical power cycle.

## Facts and confidence grades

| # | Fact | Confidence | Source document |
|---|---|---|---|
| 1 | USB-Serial-JTAG and USB-OTG share one internal PHY onto GPIO19/20 (D-/D+); GPIO19/20 are never claimed by any `gpio_config()` in the image | CONFIRMED | firmware/common/hw_config.h |
| 2 | `usb_new_phy()` has exactly two callers in the stock image: `tinyusb_driver_install` (Disk Mode) and `usb_host_install` (normal-mode host) | CONFIRMED | docs/architecture.md |
| 3 | In normal mode, USB-Serial-JTAG enumerates as 303A:1001 and is dropped at 4.242 / 4.242 / 4.243 s across three trials, with near-zero jitter | CONFIRMED | docs/architecture.md |
| 4 | Boot-mode word values: 0 = normal, 1 = Disk Mode, 3 = USB console, 4 = SD missing; the word lives in RAM only and is never persisted | CONFIRMED | firmware/common/hw_config.h |
| 5 | Disk Mode presents a single composite device: VID 0x303A, PID 0x4002, class EF/02/01, one 08/06/50 bulk-only MSC interface, full-speed, strings "TinyUSB" / "TinyUSB Device" / "123456", SCSI product "TEST MSC Storage" | CONFIRMED | docs/usb-and-boot-modes.md |
| 6 | USB console mode (entered by holding the button sampled on GPIO15 at boot) keeps the PHY on USB-Serial-JTAG because `usb_host_install` is never reached on that arm; the persistent window itself has not been directly observed | strongly indicated | firmware/common/hw_config.h; docs/architecture.md |
| 7 | A missing microSD card sets boot-mode 4 before the boot-mode fork, logs a shutdown message, and never reaches `usb_host_install` | strongly indicated | docs/architecture.md |
| 8 | The modified firmware's CDC-ACM interface uses a self-assigned PID (0x83B8) chosen to avoid the stock USB-Serial-JTAG PID (0x1001) and stock Disk-Mode PID (0x4002); it is not a registered PID | CONFIRMED for the code path | firmware/s3/README.md |
| 9 | The TUSB320 CC-role controller's `MODE_SELECT` register is never written in any boot mode, so its role is strap-fixed and identical across all modes | CONFIRMED | docs/architecture.md |
| 10 | Detailed boot-log timeline from reset through the 4.14 s hand-over (PSRAM, console, assets mount, Lua load, I2C buses, RTC, microSD mount, update check, display bring-up) | CONFIRMED | docs/architecture.md |
| 11 | The display bring-up gap (1040→4080 ms, ~3.04 s) accounts for about 74% of the pre-hand-over window | CONFIRMED | docs/architecture.md |
| 12 | `esptool`'s `USBJTAGSerialReset` strategy drives the chip into ROM download mode over DTR/RTS with no GPIO0/BOOT pin involved, as long as USB-Serial-JTAG (not the application) owns the pads | CONFIRMED for the mechanism | docs/architecture.md; docs/usb-and-boot-modes.md |
| 13 | The modified firmware hands the PHY back to USB-Serial-JTAG as the third action of `app_main()`, immediately after the power latch and the stock GPIO parity block | CONFIRMED | firmware/s3/main/app_main.c |
| 14 | On this chip, `esp_restart()` resets neither the RTC sub-system that holds the PHY mux nor the USB peripherals (`SYSTEM_USB_RST`/`SYSTEM_USB_DEVICE_RST` are deliberately omitted from the reset path) | CONFIRMED | firmware/s3/main/app_main.c |
| 15 | The USB-OTG CDC link is gated behind a configured minimum uptime (`CONFIG_BYOK_USB_CDC_MIN_UPTIME_MS`), shipped at 5000 ms | CONFIRMED | firmware/s3/main/app_main.c; CHANGELOG.md |

Diagram file: `usb-phy-modes-and-recovery-window.svg` (same directory).
