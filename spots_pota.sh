#! /bin/bash

set -eu -o pipefail

host="$1"
port="$2"

curl "https://api.pota.app/spot" | jq -r '.[] | ["DX", .spotter, .frequency, .activator, .comments, (.spotTime+"Z" | fromdateiso8601 | strftime("%H%M %Y-%m-%d"))] | @tsv' | nc -N $host $port

