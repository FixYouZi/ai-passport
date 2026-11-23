<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Tesla card-key UI fonts

[`fonts/tesla_symbols.txt`](fonts/tesla_symbols.txt) is the character inventory for
the Tesla card-key application: ASCII `0x20-0x7E` plus every non-ASCII code point
used in display string literals under `main/` and in runtime input such as
license plates (all province abbreviations plus the special plate characters,
300 code points). The compiled sources
`fonts/tesla_font_16.c`, `fonts/tesla_font_20.c`, and
`fonts/tesla_font_26.c` were generated with `lv_font_conv` 1.5.3 from Microsoft
YaHei Regular (face 0 of the system `msyh.ttc`). `fonts/tesla_font_lock.c` is a
36 px subset with only the 11 characters used by the large lock-state label
(minimal home layout, scheme C). The subset keeps flash usage small.
**Microsoft YaHei is a
proprietary Windows font and must not be redistributed**: the generated bitmap
sources are for local/personal firmware only. Replace the source with Noto Sans SC
(SIL OFL 1.1) before publishing a firmware image or this repository publicly.
The font file itself is not committed; only the generated bitmap sources are.

- Sizes and format: 16/20/26 px (full inventory) and 36 px (lock-state subset),
  4 bpp, `--format lvgl --no-compress`, LVGL 9.
- Generation (repeat for each size, bash syntax; `msyh-face0.ttf` is the face-0
  extraction of `C:\Windows\Fonts\msyh.ttc`):

  ```bash
  lv_font_conv --font msyh-face0.ttf --range 0x20-0x7E \
    --symbols "$(cat assets/fonts/tesla_symbols.txt)" --size 16 --bpp 4 \
    --format lvgl --no-compress --lv-font-name tesla_font_16 \
    --lv-include lvgl.h --output assets/fonts/tesla_font_16.c
  ```

  The lock subset uses `--size 36` with
  `--symbols "$(cat assets/fonts/tesla_lock_symbols.txt)"`; extend that file when
  the lock wording changes.

- Destination: compiled into the `main` component and referenced as
  `tesla_font_16/20/26` and `tesla_font_lock`.
- Coverage gate: `python tools/check_glyphs.py` verifies every display literal is
  covered after UI text changes. Regenerate the fonts when it fails.
- Flash impact: the four sources total about 1.22 MB; the linked application image
  is about 1.71 MB, leaving roughly 78 % of the 8 MB application partition free.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |
| [`images/car-model-3.png`](images/car-model-3.png) | 216 × 100, PNG RGB | Photoreal 2024 Model 3 (Highland facelift, Standard/RWD) front-view image for the Tesla card-key home screen: slim horizontal headlamps, smooth badge-free nose, single wide lower intake. Generated from a front-view reference with the built-in image tool; the background is removed by brightness thresholding plus edge flood fill, the largest connected region keeps only the car (dropping the corner watermark), and the body is scaled to 96 px high and composited on the `#14181F` panel background. Converted to RGB565 (`car_model_3`) in [`main/car_art.c`](../main/car_art.c) and selected when the saved model is Model 3. |
| [`images/car-model-y.png`](images/car-model-y.png) | 216 × 100, PNG RGB | Photoreal Model Y front-view image (taller body) for the same home screen, processed the same way and converted to `car_model_y`. Both images are static; the vehicle lock/closure status is shown in the text card above the image. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).
