# DELL-XPS-OLEDBrightness

A macOS kernel extension that enables native brightness control for the OLED display on the **Dell XPS 15 7590** Hackintosh. Works with the macOS brightness slider, keyboard brightness keys, and a CLI tool.

## Supported Hardware

| Component | Model |
|-----------|-------|
| Laptop | Dell XPS 15 7590 |
| Display | Samsung ATNA56WR04 — 15.6" 4K OLED (3840x2160) |
| GPU | Intel UHD 630 (CoffeeLake, PCI ID `8086:3e9b`) |
| Bootloader | OpenCore |

**The kext may work on other similar Dell models. Test at your own risk.**

## Prerequisites

Requires **WhateverGreen**

## Installation

Drag prebuilt kext into your OC Kexts folder, then enable under **Kernel -> Add**

## The Problem

The Dell XPS 15 7590's 4K OLED panel has no LED backlight. macOS controls LCD brightness by writing to the Intel GPU's BLC (BackLight Control) PWM registers via `AppleBacklight` and WhateverGreen. This works for IPS/LCD panels that have a physical backlight, but OLED panels control luminance per-pixel through an embedded timing controller (TCON) — the BLC PWM writes have no visible effect.

Without this kext, the macOS brightness slider and keyboard brightness keys do nothing on the OLED panel.

## How It Works

The Samsung ATNA56WR04 OLED panel supports **Intel HDR TCON brightness control** via the eDP AUX channel. The panel's TCON accepts brightness commands as a nits value written to **DPCD register 0x354** (the Intel HDR TCON brightness register). These values were discovered through dumping the DPCD binary values in Linux, although it seems easy, **this process was not trivial**, it required a lot of trial and error as the display requires additional prep before writing brightness values to the screen.

This kext bridges macOS brightness changes to the OLED panel through the following mechanism:

```
macOS Brightness Slider / Keyboard Keys
         │
         ▼
WhateverGreen intercepts the brightness change
and writes a duty cycle value to BLC_PWM_DUTY (GPU register 0xC8258)
         │
         ▼
OLEDBrightness kext polls BLC_PWM_DUTY at 30Hz
         │
         ▼
Detects change → scales duty cycle to nits (0 – 440)
         │
         ▼
Writes nits to DPCD 0x354 via AUX Channel A (GPU MMIO registers)
         │
         ▼
Samsung ATNA56WR04 TCON adjusts panel luminance
```
**RANT: This is a convoluted process that's very uncommon for laptops, which made brightness control in this laptop previously thought to be impossible, it still boggles my mind to why Dell chose this method to change the brightness on the fucking screen instead to just writing a PWM value**

### AUX Brightness Initialization

On startup, the kext performs the Intel HDR TCON enable sequence (matching the Linux i915 driver's `intel_dp_aux_hdr_set_backlight`):

1. **Write Intel Source OUI** to DPCD 0x300 (`00 AA 01`) — identifies the source as an Intel GPU
2. **Set ctrl bit 4** at DPCD 0x344 — enables AUX-based brightness control on the TCON
3. **Read initial brightness** from DPCD 0x354 to determine the panel's max nits (440 for this panel)

### BLC PWM Register Layout (Cannon Point PCH)

| Register | Address | Purpose |
|----------|---------|---------|
| BLC_PWM_CTL1 | 0xC8250 | Enable bit (bit 31) |
| BLC_PWM_FREQ | 0xC8254 | Max duty cycle / period (WhateverGreen sets to 120000) |
| BLC_PWM_DUTY | 0xC8258 | Current brightness duty cycle (written by WhateverGreen) |

### Verify (Optional step if not working)

After booting, check that the kext loaded:
```bash
ioreg -rn OLEDBrightnessDriver -d1 | grep -E 'status|brightness-method|max-nits'
```

Expected output:
```
"status" = "active"
"brightness-method" = "DPCD-0x354-AUX-nits"
"max-nits" = 440
```

### CLI Tool (Optional, for testing)

Copy the CLI tool somewhere in your PATH:
```bash
sudo cp Build/oled_brightness /usr/local/bin/
```

Usage:
```bash
sudo oled_brightness get           # Show current brightness
sudo oled_brightness set 50%       # Set to 50%
sudo oled_brightness set 32768     # Set to raw value (0-65535)
sudo oled_brightness max           # Show max value (65535)
```

## Building

### Requirements

- macOS
- Xcode Command Line Tools
- KDK for your version of MacOS

### Build

```bash
# Build everything
./Scripts/build.sh

# Build only kext
./Scripts/build.sh kext

# Build only CLI tool
./Scripts/build.sh cli

# Clean
./Scripts/build.sh clean
```
