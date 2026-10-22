# The UI font

`font-20x20.png` is the font every screen in tinrom uses: outlined glyphs
drawn in 20x20 cells, fifteen to a row, printable ASCII from the space onwards
and then some accented letters tinrom does not use yet. ps1-python uses the
same design as a 10x10 sheet, `ps1python/font.tim`.

It is from the Superpowers asset packs - <http://superpowers-html5.com/> - and
is in the public domain under Creative Commons Zero:

> Copyright/Attribution Notice: Creative Commons Zero:
> http://creativecommons.org/publicdomain/zero/1.0/ Attribution is not
> required but appreciated. Placing a link to http://superpowers-html5.com/
> somewhere would be awesome :)

So it is credited anyway: here, in `NOTICE.md`, and on tinrom's About ->
Credits screen, with the link.

## How it is used

`tools/convertfont.py` converts it at build time (see `cmake/assets.cmake` and
the call in `CMakeLists.txt`):

- the screen is 640x240 (`src/hal/video.h`), its pixels half as wide as they
  are tall, so every cell keeps all 20 of its columns and is scaled down only
  to 10 rows. The sheet is drawn at double size but not only in 2x2 blocks -
  its rounded corners and joins are single pixels - and those columns are
  what the wide screen shows that a 320-wide one could not. A row that falls
  between two comes out as a shade between the outline and the body. The PS1
  draws sprites one texel to one pixel, so this is the only place a font can
  be resized;
- each glyph is cropped to the columns it uses and packed into a 256x45 4bpp
  sheet, and moves the pen on by two less than its width, so neighbours share
  their two-column outline;
- the backtick cell is empty, so `` ` `` borrows the apostrophe.

`font-20x20.png` itself is kept as it came. A different size is `--size` and
`--width` in `CMakeLists.txt`; the compiler then says which constants in
`src/ui/font.h` to change, though the UI's layout is built around 10-row
text. A different font is another PNG or TIM laid out the same way.

## The terminal font

`term4x8.txt` is the terminal's font: 4x8, all 256 CP437 characters, drawn
for tinrom and under its MIT licence. It is text - eight rows of `#` and `.`
per character - so it can be edited in any editor. `tools/convertbitfont.py`
turns it into a C table at build time. Letters are three pixels wide with the
fourth column as the gap; box drawing and blocks use all four, so that they
join up.
