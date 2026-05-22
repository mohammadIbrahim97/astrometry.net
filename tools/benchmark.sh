#!/bin/bash

if [ $# -lt 7 ]; then
  echo "Usage: $0 <DIR> <NAME-SCHEME> <DOWNSAMPLE-DEGREE> <SCALE-LOW> <SCALE-HIGH> <CPULIMIT> <OUT-FILE>"
  echo "  Example: $0 demo \"*.jpg\" 2 0 999999 30 demo/statistics.json"
  exit
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

statsfile="$7"
if [ -e "$statsfile" ]; then
  echo "$statsfile already exists."
  exit
fi

mydir="$(dirname "$0")"
cleanscript="$mydir/clean.sh"

downsample="$3"
scalelow="$4"
scalehigh="$5"
cpulimit="$6"

solved=0
notsolved=0
timetaken=0
shopt -s lastpipe

printf "{\n  \"downsample\": %s,\n  \"scale-low\": %s,\n  \"scale-high\": %s,\n  \"cpulimit\": %s"\
  "$downsample" "$scalelow" "$scalehigh" "$cpulimit" >> "$statsfile"
printf ",\n  \"images\": [" >> "$statsfile"

find "$1" -mindepth 1 -maxdepth 1 -type f -name "$2" | while read -r file; do
  echo "Solving $file..."

  # Gather data
  t0=$( echo $EPOCHREALTIME | tr -dc "0-9")
  output="$(solve-field --overwrite --no-plots --downsample "$downsample"\
  --scale-units arcsecperpix --scale-low "$scalelow" --scale-high "$scalehigh" --cpulimit "$cpulimit" "$file" 2>/dev/null)"
  t1=$( echo $EPOCHREALTIME | tr -dc "0-9")
  td=$(((t1 - t0) / 1000))

  noext="${file%.*}"
  numsources="$(listhead "$noext.axy" | grep "NAXIS2" | xargs | cut -d " " -f3)"

  # Write data
  printf "\n    {" >> "$statsfile"

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
    } >> "$statsfile"
  else
    echo "Could not solve $file."
    notsolved=$((notsolved+1))
    {
      printf "\n      \"file\": \"%s\"," "$(basename "$file")"
      printf "\n      \"solved\": false,"
      printf "\n      \"nsrc\": %s" "$numsources"
    } >> "$statsfile"
  fi

  printf "\n    }," >> "$statsfile"

  echo "Cleaning up..."
  $cleanscript "$1" "$(basename "$noext")" 1>/dev/null

done

truncate -s-1 "$statsfile" # Remove the last , in the array
printf "\n  ],\n  \"solved\": %s,\n  \"avgtime\": %s,\n  \"notsolved\": %s\n}\n"\
  "$solved" "$((timetaken / solved))" "$notsolved" >> "$statsfile"
