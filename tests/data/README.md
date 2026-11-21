# Synthetic test data

The project distributes these decoder and PDF fixtures under its [MIT licence](../../LICENSE).

Run `python3 tests/data/generate_layers.py` with libjpeg-turbo's `cjpeg` on PATH
to rebuild the 50 × 30 colour gradient. The PNG adds a varying alpha channel.
The JPEG uses the same RGB pixels at quality 85.

The PDF seeds were built with pypdf from blank pages, text labels and vector
shapes. Each exercises a different part of PDF import or preservation.

| File in `pdf-seeds/` | Contents |
|---|---|
| rich.pdf | Five labelled pages with crops, rotations, links, outlines and page labels |
| inherited.pdf | Page boxes and rotation inherited from the page tree |
| incremental.pdf | A second revision of rich.pdf with updated metadata |
| form.pdf | A text-field widget added to rich.pdf |
| appearances.pdf | Tagged pages with stamp, widget and rotated annotation appearances |
| acroform.pdf | Interactive form fields and widget appearances |
