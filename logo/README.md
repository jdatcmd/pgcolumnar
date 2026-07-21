# pgColumnar logo

Brand assets for pgColumnar. The mark is four vertical bars of varying height
that read as a column store; the tallest bar carries the amber accent. All files
are self-contained SVG with no external fonts or images.

| File | Use |
| --- | --- |
| `pgcolumnar-logo.svg` | Horizontal lockup (mark plus wordmark) for light backgrounds. |
| `pgcolumnar-logo-dark.svg` | Horizontal lockup for dark backgrounds. |
| `pgcolumnar-mark.svg` | Mark only, for favicons and avatars. Holds its shape down to 16 px. |

## Palette

| Role | Value |
| --- | --- |
| Bar blue | `#5B8DEF` to `#2B5FD0` (vertical gradient) |
| Bar blue on dark | `#7AA2F7` to `#3B6FE0` |
| Accent amber | `#F7B733` to `#F59E0B` |
| Wordmark ink | `#1F2A44` (light background), `#F2F4F8` (dark background) |

The wordmark is set in a system sans-serif stack (`Segoe UI`, `Roboto`,
`Helvetica`, `Arial`). To lock the wordmark independent of installed fonts,
convert its text to paths in a vector editor.

## Theme-aware embedding

Use the `<picture>` element to serve the light or dark lockup by the viewer's
color scheme:

```html
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="logo/pgcolumnar-logo-dark.svg">
  <img src="logo/pgcolumnar-logo.svg" alt="pgColumnar" width="340">
</picture>
```
