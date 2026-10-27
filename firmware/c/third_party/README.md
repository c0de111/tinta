# Third-Party Components

The components bundled in this directory are not original to inki and retain their
respective upstream licenses. The surrounding firmware is licensed under Apache-2.0;
none of the components below impose copyleft obligations on that firmware code.

| Component      | License                       | Origin / where the license lives                         |
|----------------|-------------------------------|----------------------------------------------------------|
| `cjson`        | MIT                           | `cjson/LICENSE`                                           |
| `ds3231`       | MIT                           | `ds3231/LICENSE`                                          |
| `e-Paper`      | MIT                           | Waveshare; grant in each source file header              |
| `Config`       | MIT                           | Waveshare; grant in each source file header              |
| `GUI`          | MIT                           | Waveshare; grant in each source file header              |
| `Fonts`        | BSD-3-Clause                  | STMicroelectronics (2014); grant in each file header     |
| `fonts_custom` | Ubuntu Font Licence 1.0 (UFL) | Bitmap tables generated from the Ubuntu Mono typeface    |
| `miniz`        | Unlicense / public domain     | Rich Geldreich; statement at the end of the miniz source |
| `qrcodegen`    | MIT                           | Project Nayuki; grant in each source file header         |
| `st25dv`       | BSD-3-Clause                  | [STMicroelectronics/stm32-st25dv]; `st25dv/LICENSE.md`   |

[STMicroelectronics/stm32-st25dv]: https://github.com/STMicroelectronics/stm32-st25dv

## Notes

- `fonts_custom` are bitmap font tables generated from the **Ubuntu Mono** typeface
  (Copyright Canonical Ltd.), distributed under the **Ubuntu Font Licence 1.0**. The UFL
  text is at `fonts_custom/LICENSE`; attribution is in `fonts_custom/CUSTOM.md`.
- `st25dv` is the STM32Cube BSP component driver. Its `LICENSE.md` (BSD-3-Clause) was
  restored from the upstream repository; the driver source headers reference it.
- Waveshare (`e-Paper`, `Config`, `GUI`) and `miniz` carry their grants inline in the
  source rather than as separate LICENSE files.
