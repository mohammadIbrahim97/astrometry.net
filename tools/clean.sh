#!/bin/sh

if [ $# -lt 1 ]; then
    echo "Usage: $0 <PATH>"
    exit
fi

find "$1" -mindepth 1 -maxdepth 1 \( -name "*.axy" -o -name "*.corr" -o -name "*.match"\
    -o -name "*.new" -o -name "*.rdls" -o -name "*.wcs" -o -name "*.solved"\
    -o -name "*-indx*" -o -name "*-ngc*" -o -name "*-objs*" \) \
    -exec echo {} \; -exec rm {} \;
