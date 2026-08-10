# Real-Time Video Processing
This subdirectory contains a wrapper utility around `solve-field` to make it
easier to work with continuous video streams from sources like cameras.

It monitors a directory for new files and runs `solve-field` on each of them,
with optimizations like using the lower and upper scale bounds to limit the
amount of index files used.  It also supports skipping
some of the incoming images based on a measure  called the blur score,
which is provided by an external program
(path given via `--blur-score-path`) that is run on the file
before `solve-field` is called.

## Setup
### Dependencies
```shell
sudo apt-get install git make gcc pkg-config \
libbz2-dev zlib1g-dev libcfitsio-dev libcairo2-dev \
libgsl-dev libjpeg-dev libpng-dev \
netpbm libnetpbm-dev \
python3-numpy python3-fitsio \
inotify-tools source-extractor
```

### Compiling the program
Clone the repository - if you just want to compile the program, you can clone
it with `--depth 1` and choose your target branch using `--branch`.
After this, `cd` into the repository and run `sudo make && sudo make install`.
By default, this will install Astrometry in `/usr/local/astrometry`,
but the location the `astrometry` directory is created in can be changed
by setting the `INSTALL_DIR` environment variable.
Make sure to add the `bin/` directory of that location to your PATH.

### Configuring `source-extractor`
Run `source-extractor -d` to get a default configuration -
you might as well `cat` the output into a file directly. For the purpose of
this guide, this file will henceforth be referred to as `se.conf`.
Create a new file and inside `se.conf`, set `PARAMETERS_NAME` to its file name.
This file needs to contain the following three lines:
```
X_IMAGE
Y_IMAGE
MAG_AUTO
```
Inside `se.conf`, set `CATALOG_TYPE` to `FITS_1.0`.
If you want to use a convolution filter, create a new file
(referred to as `se.conv` inside this guide) and inside `se.conf`,
set `FILTER_NAME` to its filename.  Otherwise, set `FILTER` to `N`.

If you're using a filter, you'll need to describe the filter inside `se.conv`.
I've found that the following 3x3 filter works well for most images:
```
CONV NORM
1 2 1
2 4 2
1 2 1
```
For gaussian filters of different sizes, refer to the files in the
[SourceExtractor repository](https://github.com/astromatic/sextractor/tree/master/config).
You might want to use larger filters the higher the resolution of your images is.

The following changes inside of `se.conf` are not required,
but I've found that they work well:
- Change `DETECT_THRESH` and `ANALYSIS_THRESH` to `5`.
    This results in fewer sources being detected and thus,
    speeds up both source-detection and solving. However, for low-quality
    images, you might want to decrease these values.
- Change `DETECT_MINAREA` to `1` (especially if using a large filter).

By default, all file paths in `se.conf` are relative to the current
working directory (not the parent directory of `se.conf`).
To avoid confusion when calling the program from multiple places,
consider using absolulte file paths.

### Index Files
The utility automatically chooses a set of index files based on the lower and
upper scale limits of your images.
You must pass the directory your index files are located in with `--index-dir`.
Each index file within this directory must be of the form `.*[0-9]{2}\.fits`.
If you don't have your own index files, you can get some from
[the Astrometry.net website](https://data.astrometry.net/).
However, make sure your index files follow the naming scheme from above.
The utility currently doesn't support indices that have their files split
across healpixes, so if you need to get indices, I recommend the 4100-series.

## Usage
A reasonable example using the 4100-series of index files
for solving images that are between 1 and 4 degrees wide:
```shell
video-processing/video-processsing.sh -i copy_to_here/ \
--index-directory /usr/local/astrometry/data/4100/ \
--index-base-name "index-41" \
--scale-low 1 --scale-high 4 \
--source-extractor-config se.conf \
--blur-score-path util/blur_score.py
```
For a description of what each option does, run the script with `-h`.

If you encounter any errors, add the `-v` flag to get more information.

You might get errors like the following when using `source-extractor`:
```
fitstable.c:972:read_array_into: Failed to read column from FITS file
Failed to read xylist field.
Suppressing solution output after solver execution failure
Failed to run_job()
solve-field.c:547:run_engine engine failed.  Command that failed was:
  ...
 ioutils.c:568:run_command_get_outputs Command failed: return value 1
```
If so, `source-extractor` did not extract any sources from the input image.
Try decreasing `DETECT_THRESH` and `ANALYSIS_THRESH`, or `DETECT_MINAREA`,
in your source extractor config file, or using a smaller filter.
