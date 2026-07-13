#!/bin/bash

usage() {
  echo "USAGE: $0 <solve-field-executable-path> <arcsecperpix-min> <arcsecperpix-max>"
  exit
}

if [ $# -ne 3 ]; then usage; fi
if ! eval "$1" >/dev/null; then
  echo "$1 returned an error. Is the path correct?"
  exit
fi

numreg="^([0-9]+(\.[0-9]+)?|\.[0-9]+)$"

if ! [[ "$2" =~ $numreg ]] || ! [[ "$3" =~ $numreg ]]; then
  echo "<arcsecperpix-min> and <arcsecperpix-max> need to be numeric values."
  usage
fi

thisdir="$(realpath "$(dirname "$0")")"
tmpfn="$thisdir/astrometry_streaming_tmp.txt"

if ! touch "$tmpfn"; then
  echo "Cannot create $tmpfn. Please make sure permissions are set correctly."
  exit
fi

trap 'jobs -p | xargs -r kill' EXIT

read -r to_be_solved
echo "$to_be_solved" > "$tmpfn"

while true; do
  fn="$(cat "$tmpfn")"

  # While this is an inefficient loop that does active waiting,
  # if will only have to wait when after having solved an image, a new image hasn't yet arrived.
  # This will happen very rarely, if at all, so the performance loss is negligible.
  if [ "$fn" = "$last_tried" ]; then
    sleep 0.125
    continue
  fi
  output="$($1 "$fn" --overwrite -p -l 5 -z 2 -L $2 -H $3)"
  if [ -f "${fn%.*}.solved" ]; then
    echo "$output" | grep "Field center: (RA,Dec) ="
    distline="$(echo "$output" | grep -n -m 1 "brightest distractors are" | cut -d: -f1)"
    echo "$output" | tail -n +$distline
  else
    echo "Could not solve $fn."
  fi
  last_tried="$fn"

  # TODO: Clean
done &

while true; do
  read -r infn
  bs="0.3" # Mock
  # bs="$(python3 "$thisdir/blur_score.py $infn")"
  # TODO: Connect with actual blur score
  echo "Blurscore for $infn is $bs" >&2
  if (( $(echo "$bs < 0.7" | bc -l) )); then
    echo "Acceptable" >&2
    echo "$infn" > "$tmpfn"
  else
    echo "Unacceptable" >&2
  fi
done
