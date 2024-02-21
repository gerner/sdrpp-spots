#! /bin/bash

set -eu -o pipefail

SCRIPTNAME=$0
function printusage {
    echo "usage $(basename $SCRIPTNAME) [<option> ...]"
    echo ""
    echo "options:"
    echo "    --start    start daemon (must come before other options)"
    echo "    --stop     stop daemon"
    echo "    --host     which host to send spots to. default localhost"
    echo "    --port     which port to send spots to. default 6124."
    echo "    --src      where to get spots from (pota, hamqth)"
    echo "    --help,-h  print this help message"
}

PIDFILE=/tmp/spots.pid
set -- `getopt -n$0 -u -a --longoptions="host port source start stop" "h" "$@"`

host="localhost"
port=6214
period=30
src=""

while [ $# -gt 0 ]; do
    case $1 in
        --start)
            shift
            echo "starting $SCRIPTNAME $@"
            start-stop-daemon --start -p ${PIDFILE} -b -m -x $SCRIPTNAME -- `echo $@ | sed 's/ -- / /'`
            exit $?
            ;;
        --stop)
            shift
            start-stop-daemon --stop -p ${PIDFILE}
            exit $?
            ;;
        --host) host=$2;shift;;
        --port) port=$2;shift;;
        --source) src=$2;shift;;
        --help|-h)
            printusage
            exit 0
        --) shift;break;;
    esac
    shift
done

if [ -z "$src" ]; then
    echo "source is required"
    printusage
    exit 1
fi

while true; do
    if [[ "$src" == "pota" ]]; then
        curl "https://api.pota.app/spot" | jq -r '.[] | ["DX", .spotter, .frequency, .activator, .comments, (.spotTime+"Z" | fromdateiso8601 | strftime("%H%M %Y-%m-%d"))] | @tsv' | nc -N -w 5 $host $port > /dev/null
    elif [[ "$src" == "hamqth" ]]; then
        curl 'https://www.hamqth.com/dxc_csv.php?limit=200&band=10m' | tr '^' '\t' | awk '{printf("DX\t%s\n", $0) }' | nc -N -w 5 $host $port > /dev/null
    else
        echo "unknown source $src"
        printusage
        exit 1
    fi
    sleep ${period}
done

