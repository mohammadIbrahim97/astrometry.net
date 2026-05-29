#!/bin/bash

printhelp() {
  echo ""
  echo "Usage: $0 <options>"
  echo ""
  echo "Required options:"
  echo "  -i <in-dir>: Directory containing all files to be processed."
  echo "  -n <name-scheme>: Shell pattern of input files to match. See documentation for find and its -name option."
  echo "  -o <out-file>: Path to the file with the generated data."
  echo ""
  echo "Optional options:"
  echo "  -l <scale-lower> [default: 0]"
  echo "  -h <scale-higher> [default: 999999]"
  echo "  -c <cpulimit> [default: 300]"
  echo "  -d <downsample> [default: 0]"
  echo "  -r <number-of-runs> [default: 1] NOT YET IMPLEMENTED"
  echo "  -e <command-string>: Allows to call external tools and add their data to the generated file."
  echo "    <command-string> must consist of two parts separated by a colon (:)."
  echo "    The first part is a command to be run, while the second part is the corresponding field name."
  echo "    For each input file found, the given command will be run with the file's path as its argument,"
  echo "      and the command's output will be added to the data generated per file under its fieldname."
  echo "    The time this command takes will not be included in the solving time."
  echo "    Example: ... -e \"./laplacian.sh:laplace-blur\""
  echo "    This option can be present multiple times."
  echo ""
  echo "Example:"
  echo "  $0 -i demo/ -n \"*.jpg\" -o stats.json"
  exit 1
}

intreg="^[0-9]+$"
numreg="^([0-9]+(\.[0-9]+)?|\.[0-9]+)$"

execind=0

while getopts "i:n:o:l:h:c:d:r:e:" opt; do
  case $opt in
    i)
      if ! [ -d "$OPTARG" ]; then
        echo "ERROR: Input directory \"$OPTARG\" is not a directory."; printhelp
      fi
      indir="$OPTARG"
      ;;
    n)
      namescheme="$OPTARG"
      ;;
    o)
      if [ -e "$OPTARG" ]; then
        echo "ERROR: Output file \"$OPTARG\" already exists."; printhelp
      fi
      outfile="$OPTARG"
      ;;
    l)
      if ! [[ "$OPTARG" =~ $numreg ]] ; then
         echo "ERROR: Lower scale bound (-l) needs to be a number."; printhelp
      fi
      scalelow="$OPTARG"
      ;;
    h)
      if ! [[ "$OPTARG" =~ $numreg ]] ; then
        echo "ERROR: Upper scale bound (-h) needs to be a number."; printhelp
      fi
      scalehigh="$OPTARG"
      ;;
    c)
      if ! [[ "$OPTARG" =~ $intreg ]] ; then
        echo "ERROR: CPU limit (-c) needs to be an integer."; printhelp
      fi
      cpulimit="$OPTARG"
      ;;
    d)
      if ! [[ "$OPTARG" =~ $intreg ]] ; then
        echo "ERROR: Downsampling degree (-d) needs to be an integer."; printhelp
      fi
      downsample="$OPTARG"
      ;;
    r)
      if ! [[ "$OPTARG" =~ $intreg ]] ; then
        echo "ERROR: Amount of repeats (-r) needs to be an integer."; printhelp
      fi
      repeats="$OPTARG"
      ;;
    e)
      if [[ "$OPTARG" != *":"* ]]; then
        echo "ERROR: -e requires format \"command:fieldname\" (provided: \"$OPTARG\")."
        printhelp
      fi
      cmds[execind]="${OPTARG%:*}"
      fieldnames[execind]="${OPTARG##*:}"
      execind=$((execind+1))
      ;;
    \?)
      printhelp
      ;;
  esac
done

if [ -z "$indir" ]; then
  echo "Input directory (-i) is required."; printhelp
fi

if [ -z "$namescheme" ]; then
  echo "File name scheme (-n) is required."; printhelp
fi

if [ -z "$outfile" ]; then
  echo "Output file location (-o) is required."; printhelp
fi

if [ -n "$scalelow" ] && [ -n "$scalehigh" ]; then
  if (( $(echo "$scalelow > $scalehigh" | bc -l) )); then
    echo "ERROR: Upper scale bound (-h) needs to be greater than lower scale bound (-l)."
    printhelp
  fi
fi

if [ -z "$scalehigh" ]; then scalehigh=999999; fi
if [ -z "$scalelow" ]; then scalelow=0; fi
if [ -z "$cpulimit" ]; then cpulimit=300; fi
if [ -z "$downsample" ]; then downsample=0; fi
if [ -z "$repeats" ]; then repeats=1; fi

if [ "$EUID" -eq 0 ] && [ -z "$AN_ALLOW_ROOT" ]; then
  echo "Script is run at root, but AN_ALLOW_ROOT is not set."
  echo "Please set AN_ALLOW_ROOT to run this script as root."
  exit 1
fi

# Returns value of NDISTRACT field in .match file
ndistract () {
  tmpmatchfn="$1.match.tmp"

  subtable -i "$1.match" -c "NDISTRACT" -o "$tmpmatchfn" 1>/dev/null
  tablist -r "$tmpmatchfn" | xargs
  rm "$tmpmatchfn"
}

# Outputs the number of unrecognized objects
# that are brighter than the dimmest correspondence.
# This is not always equal to NDISTRACT.
bright_unrecognized() {
  tmpcorrfn="$1.corr.tmp"

  subtable -i "$1.corr" -c "field_id" -o "$tmpcorrfn" 1>/dev/null
  corr_field_ids=$(tablist -r "$tmpcorrfn")
  rm "$tmpcorrfn"

  lastid="$(echo "$corr_field_ids" | tail -n 1 | xargs)"
  corr_field_n="$(echo "$corr_field_ids" | wc -l)"
  expected_n=$((lastid+1))
  echo $((expected_n - corr_field_n))
}

mydir="$(dirname "$0")"
cleanscript="$mydir/clean.sh"

solved=0
notsolved=0
timetaken=0
shopt -s lastpipe

printf "{\n  \"downsample\": %s,\n  \"scale-low\": %s,\n  \"scale-high\": %s,\n  \"cpulimit\": %s,\n  \"repeats\": %s"\
  "$downsample" "$scalelow" "$scalehigh" "$cpulimit" "$repeats" >> "$outfile"
printf ",\n  \"images\": [" >> "$outfile"

inputfiles=$(find "$indir" -mindepth 1 -maxdepth 1 -type f -name "$namescheme" | sort)
while read -r file; do
  echo "Solving $file..."

  printf "\n    {" >> "$outfile"

  # Execute all commands from -e
  for i in $(seq 0 $((execind-1))); do
    execout="$(${cmds[i]} "$file")"
    printf "\n      \"%s\": \"%s\"," "${fieldnames[i]}" "$execout" >> "$outfile"
  done

  # Gather data
  t0=$( echo $EPOCHREALTIME | tr -dc "0-9")
  output="$(solve-field --overwrite --no-plots --downsample "$downsample"\
  --scale-units arcsecperpix --scale-low "$scalelow" --scale-high "$scalehigh" --cpulimit "$cpulimit" "$file" 2>/dev/null)"
  t1=$( echo $EPOCHREALTIME | tr -dc "0-9")
  td=$(((t1 - t0) / 1000))

  noext="${file%.*}"
  numsources="$(listhead "$noext.axy" | grep "NAXIS2" | xargs | cut -d " " -f3)"

  if [ -f "$noext.solved" ]; then
    echo "Solved $file in $td ms."
    solved=$((solved+1))
    timetaken=$((timetaken + td))
    numcorrs="$(listhead "$noext.corr" | grep "NAXIS2" | xargs | cut -d " " -f3)"
    numbrightunrecognized="$(bright_unrecognized "$noext")"
    ndistract=$(ndistract "$noext")
    pxscale="$(listhead "$noext.wcs" | grep "COMMENT scale: .* arcsec/pix" | cut -d " " -f3)"
    {
      printf "\n      \"file\": \"%s\"," "$(basename "$file")"
      printf "\n      \"solved\": true,"
      printf "\n      \"time\": %s," $td
      printf "\n      \"arcsec/px\": %s," "$pxscale"
      printf "\n      \"nsrc\": %s," "$numsources"
      printf "\n      \"ncorr\": %s," "$numcorrs"
      printf "\n      \"ndistract\": %s," "$ndistract"
      printf "\n      \"nbrighter\": %s" "$numbrightunrecognized"
    } >> "$outfile"
  else
    echo "Could not solve $file."
    notsolved=$((notsolved+1))
    {
      printf "\n      \"file\": \"%s\"," "$(basename "$file")"
      printf "\n      \"solved\": false,"
      printf "\n      \"nsrc\": %s" "$numsources"
    } >> "$outfile"
  fi

  printf "\n    }," >> "$outfile"

  echo "Cleaning up..."
  $cleanscript "$indir" "$(basename "$noext")" 1>/dev/null

done <<< "$inputfiles"

truncate -s-1 "$outfile" # Remove the last , in the array
printf "\n  ],\n  \"solved\": %s,\n  \"avgtime\": %s,\n  \"notsolved\": %s\n}\n"\
  "$solved" "$((timetaken / solved))" "$notsolved" >> "$outfile"
