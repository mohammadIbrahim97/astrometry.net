# Tools

This directory contains additional tools that are not necessary to run
or build Astrometry.net, but might still be useful.

## `add-distractors.sh`
Adds distractors to an image.
Size and amount of these distractors are configurable.
Optionally, an image file to be used as a distractor can be supplied.
A file specifying the positions of the distractors can also be supplied.

This can be useful to automatically generate test data to test the resilience
of Astrometry.net against images with lots of distractors.

Usage: `add-distractors.sh
    ( <DOT-SIZE> | <DOT-SRC-FILE> )
    ( <NUM-DISTRACTORS> | <DISTRACTORS-POS-FILE> )
    <SRC-IMG> <DEST-IMG>`

When the first argument is an integer, it is used as the size,
in pixels, of the distractors. In this case, the distractors are white dots -
more specifically, radial gradients, which means they are fully opaque at their
centre while fading into full transparency on the outside.
When the first argument is not an integer, it is used as the path
to the image that will be overlaid onto the original image
instead of the white dots.

When the second argument is an integer, it is used as the amount of distractors
to add to the image.
In this case, the positions of the distractors are generated randomly.
When the second argument is not an integer, it is used as the path
to the file specifying the positions of the distractors.
In this case, each line in the file will cause a distractor to be placed.
Lines must have the format `([+][0-9]+){2}`.
For example, the line `+500+200` would place a distractor at X 500 Y 200.

### Prerequisites:
- [ImageMagick](https://imagemagick.org/) for the `convert` command
- `/dev/urandom`

### Examples:
- `tools/add-distractors.sh 12 32
demo/apod5.jpg demo/apod5-with-distractors.png`
- `tools/add-distractors.sh 12 tools/add-distractors-demo-positions.txt
demo/apod5.jpg demo/apod5-with-distractors.png`

The script is POSIX compatible.
