#!/bin/bash

if [ $# -lt 5 ]; then
  echo "Usage: $0 <DIR> <NAME-SCHEME> <DOWNSAMPLE-DEGREE> <SCALE-LOW> <SCALE-HIGH> <CPULIMIT> <OUT-FILE>"
  echo "  Example: $0 demo \"*.jpg\" 2 0 999999 30 demo/statistics.json"
  exit
fi

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

  # All of these rely on the console output of solve-field and are thus incredibly fragile, but they work for now.
  numsources="$(echo "$output" | grep "simplexy: found .* sources" | cut -d " " -f3)"

  # Write data
  printf "\n    {" >> "$statsfile"

  noext="${file%.*}"
  if [ -f "$noext.solved" ]; then
    echo "Solved $file in $td ms."
    solved=$((solved+1))
    timetaken=$((timetaken + td))
    numcorrs="$(echo "$output" | grep "correspondences" | cut -d " " -f1)"
    numbrightdistractors="$(echo "$output" | grep "brighter" | cut -d " " -f1)"
    pxscale="$(echo "$output" | grep "pixel scale" | cut -d " " -f8)"
    {
      printf "\n      \"file\": \"%s\"," "$(basename "$file")"
      printf "\n      \"solved\": true,"
      printf "\n      \"time\": %s," $td
      printf "\n      \"arcsec/px\": %s," "$pxscale"
      printf "\n      \"nsrc\": %s," "$numsources"
      printf "\n      \"ncorr\": %s," "$numcorrs"
      printf "\n      \"nbrighter\": %s" "$numbrightdistractors"
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
