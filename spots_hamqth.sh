#!/bin/bash

set -eu -o pipefail
set -x

host="$1"
port="$2"

curl 'https://www.hamqth.com/dxc_csv.php?limit=200&band=10m' | tr '^' '\t' | awk '{printf("DX\t%s\n", $0) }' | nc -N -w 5 $host $port
