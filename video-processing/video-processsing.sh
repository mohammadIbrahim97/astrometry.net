#!/bin/bash

DEFAULT_TIME_LIMIT=5
DEFAULT_BLUR_THRESHOLD="0.8"

usage() {
  echo "USAGE: $0 <options>"
  echo ""
  echo "Required options:"
  echo "  -i | --input-dir | --input-directory <input-dir>"
  echo "    Path to the directory to monitor for new images to solve."
  echo "  --index-dir | --index-directory <index-dir>"
  echo "    Path to the directory containing the indices for the solver."
  echo "  --index-base-name <index-base-name>"
  echo "    Common part of the names of the index files inside <index-dir>."
  echo "    For example, if you have several index file that are named index_05.fits,"
  echo "    index_06.fits, index_07.fits, etc, this option's argument should be \"index_\"."
  echo "    These numbers must have two digits, and the file extension is assumed to be .fits."
  echo ""
  echo "Optional options:"
  echo "  --source-extractor-config <path>"
  echo "    Path to the configuration file for source-extractor."
  echo "    If this option is given, astromtry will use source-extractor;"
  echo "    otherwise, it will use image2xy."
  echo "  --time-limit <seconds>"
  echo "    Total time limit for the solver. Does not include source extraction."
  echo "    Default: $DEFAULT_TIME_LIMIT seconds."
  echo "  --scale-low <scale>"
  echo "    Lower bound of image scale estimate, in degrees."
  echo "    For example, if you have images that are between 90 and 120 arcminutes wide,"
  echo "    then you should pass --scale-low 1.5 --scale-high 2."
  echo "    This affects what range of index files will be used for solving;"
  echo "    see https://astrometry.net/doc/build-index.html#building-index-files."
  echo "  --scale-high <scale>"
  echo "    Upper bound of image scale estimate. See above."
  echo "  --blur-score-path <path>"
  echo "    Path to the executable to calculate the blur-score of any incoming image."
  echo "    Technically, this could be any executable that takes a file as an argument"
  echo "    and returns a number."
  echo "  --blur-threshold <threshold>"
  echo "    Any file with a blur score above this number will be excluded."
  echo "    Will have no effect unless --blur-score-path is also present."
  echo "    Default: $DEFAULT_BLUR_THRESHOLD."
  echo "  --verbose | -v"
  echo "    Print additional information."
}

rounded_index_scale() {
  # These formulae result from the way index scales work.
  # See https://astrometry.net/doc/build-index.html#building-index-files
  # (subsection: Index scale)
  exact=$(echo "2*l(8*$1)/l(2)" | bc -l)

  # This is not exact rounding (+0.5), but instead aplies a slight bias
  # towards lower index scales, as those, while having a larger file size,
  # seem to be checked more quickly by the solver
  rounded=$(echo "($exact+0.4)/1" | bc)
  echo "$rounded"
}

# Usage example: Clean "demo/" "apod*"
clean() {
  find "$1" -mindepth 1 -maxdepth 1 \( -name "$2.axy" -o -name "$2.corr" -o -name "$2.match"\
      -o -name "$2.new" -o -name "$2.rdls" -o -name "$2.wcs" -o -name "$2.solved"\
      -o -name "$2-indx.*" -o -name "$2-ngc.*" -o -name "$2-objs.*" \) \
      -exec rm {} \;
}

# This requires GNU getopt. On Mac OS X and FreeBSD, you have to install this separately.
args=$(getopt -o hi:v --long help:,input-directory:,input-dir:,index-dir:,index-directory:,index-base-name:,\
source-extractor-config:,time-limit:,scale-low:,scale-high:,blur-score-path:,blur-threshold:,verbose \
-n "$0" -- "$@")

if [ $? != 0 ] ; then usage; exit 255; fi

eval set -- "$args"

NUMREG="^([0-9]+(\.[0-9]+)?|\.[0-9]+)$"

input_dir=
index_dir=
index_base_name=
source_extractor_config=
time_limit=$DEFAULT_TIME_LIMIT
scale_low=
scale_high=
blur_score_path=
blur_threshold=$DEFAULT_BLUR_THRESHOLD
verbose=

while true; do
  case "$1" in
    --help | -h )
      usage; exit 0
      ;;
    --input-directory | --input-dir | -i )
      input_dir="$(realpath -m "$2")"
      if ! [ -d "$input_dir" ]; then
        echo "ERROR: Input directory \"$input_dir\" is not a directory."
        usage; exit 255
      fi
      shift 2 ;;
    --index-directory | --index-dir )
      index_dir="$(realpath -m "$2")"
      if ! [ -d "$index_dir" ]; then
        echo "ERROR: Index directory \"$index_dir\" is not a directory."
        usage; exit 255
      fi
      shift 2 ;;
    --index-base-name )
      # Sanity checks will be done later
      index_base_name="$2"
      shift 2 ;;
    --source-extractor-config )
      # The config might still be invalid, but that's for source-extractor to handle
      source_extractor_config="$(realpath -m "$2")"
      if ! [ -f "$source_extractor_config" ]; then
        echo "ERROR: Configuration file \"$source_extractor_config\" doesn't exist."
        usage; exit 255
      fi
      shift 2 ;;
    --time-limit )
      if ! [[ "$2" =~ $NUMREG ]] ; then
        echo "ERROR: Time limit (--time-limit) needs to be a number."
        usage; exit 255
      fi
      time_limit=$2
      shift 2 ;;
    --scale-low )
      # Again, sanity checks are done ltaer
      if ! [[ "$2" =~ $NUMREG ]] ; then
        echo "ERROR: Lower scale bound (--scale-low) needs to be a number."
        usage; exit 255
      fi
      scale_low=$2
      shift 2 ;;
    --scale-high )
      if ! [[ "$2" =~ $NUMREG ]] ; then
        echo "ERROR: Upper scale bound (--scale-high) needs to be a number."
        usage; exit 255
      fi
      scale_high=$2
      shift 2 ;;
    --blur-score-path )
      blur_score_path="$(realpath -m "$2")"
      if ! [ -f "$blur_score_path" ]; then
        echo "ERROR: Blur score executable at \"$blur_score_path doesn't exist."
        usage; exit 255
      fi
      shift 2 ;;
    --blur-threshold )
      if ! [[ "$2" =~ $NUMREG ]] ; then
        echo "ERROR: Blur score threshold (--blur-threshold) needs to be a number."
        usage; exit 255
      fi
      blur_threshold=$2
      shift 2 ;;
    --verbose | -v )
      verbose="set"
      shift ;;
    -- ) shift; break ;;
    * ) break ;;
  esac
done

if [[ -z "$input_dir" ]]; then
  echo "ERROR: Input directory (--input-directory) is required."
  usage; exit 255
fi
if [[ -z "$index_dir" ]]; then
  echo "ERROR: Index directory (--index-directory) is required."
  usage; exit 255
fi
if [[ -z "$index_base_name" ]]; then
  echo "ERROR: Base name scheme of index files (--index-base-name) is required."
  usage; exit 255
fi

if [[ -n $scale_low ]] && [[ -n $scale_high ]]; then
  if (( $(echo "$scale_low > $scale_high" | bc) )); then
    echo "ERROR: --scale-low must not be greater than --scale-high."
    usage; exit 255
  fi
fi

whole_command="solve-field -p --wall-limit $time_limit"

min_index_scale=0
max_index_scale=19
if [[ -n $scale_low ]]; then
  whole_command="$whole_command --scale-low $scale_low"
  # shellcheck disable=SC2086 # Values are numeric
  min_index_scale=$(("$(rounded_index_scale $scale_low)"-1))
fi
if [[ -n $scale_high ]]; then
  whole_command="$whole_command --scale-high $scale_high"
  # shellcheck disable=SC2086
  max_index_scale=$(("$(rounded_index_scale $scale_high)"+1))
fi
if [ $verbose ]; then
  echo "Index scales form $min_index_scale to $max_index_scale will be used."
fi

indices_found=0
for i in $(seq -f "%02g" $min_index_scale $max_index_scale); do
  filename="$index_dir/$index_base_name$i.fits"
  if [[ -f $filename ]]; then
    indices_found=1
    whole_command="$whole_command --index-file $filename"
    if [ $verbose ]; then
      echo "Found index file $filename."
    fi
  else
    echo "WARNING: Index file $filename not found."
  fi
done

if [ $indices_found -eq 0 ]; then
  echo "ERROR: No index files found in directory $index_dir"\
    "between scales $min_index_scale and $max_index_scale."
  exit 255
fi

if [[ -n $source_extractor_config ]]; then
  whole_command="$whole_command --use-source-extractor --source-extractor-config $source_extractor_config"
  whole_command="$whole_command --x-column X_IMAGE --y-column Y_IMAGE --sort-column MAG_AUTO --sort-ascending"
fi

tmp_dir=$(mktemp -d)
whole_command="$whole_command --dir $tmp_dir"
trap 'rm -rf -- "$tmp_dir"' EXIT

if [ $verbose ]; then
  echo "Whole command is: $whole_command"
fi

# -----------------------------------------------------------------------------

inotifywait -m "$input_dir" -e create -e moved_to | while read -r _ _ file; do
  if [ $verbose ]; then
    echo "New file '$file' appeared."
  fi
  solve_file=
  if [ -z "$blur_score_path" ]; then
    solve_file=1
  else
    bs="$($blur_score_path "$file")"
    if (( $(echo "$bs > $blur_threshold" | bc ) )); then
      if [ $verbose ]; then
        echo "Skipping $file because of blur score: Calculated $bs, threshold is $blur_threshold"
      fi
    else
      solve_file=1
    fi
  fi

  if [ $solve_file ]; then
    echo "Calculating..."
    output="$(eval "$whole_command $input_dir/$file 2>/dev/null")"
    noext="${file%.*}"
    if [ -f "$tmp_dir/$noext.solved" ]; then
      echo "$output" | grep "Field center: (RA,Dec) ="
      distline="$(echo "$output" | grep -n -m 1 "brightest distractors are" | cut -d: -f1)"
      echo "$output" | tail -n +$distline
    else
      echo "Could not solve $file."
    fi
    clean "$tmp_dir" "$noext"
  fi
done
