#!/bin/bash

path_pipe=${1:-"/tmp/vmsplice.pipe"}
path_qmp=${2:-"/tmp/qmp.sock"}
path_target_qmp=${3:-"/tmp/qmp-target.sock"}

do_qmp() {
    cmd=$1
    args=$2

    cmd_negotiate="{ 'execute': 'qmp_capabilities' }"

    echo -e "$cmd_negotiate\n" \
          "{ 'execute': '$cmd', 'arguments': $args }" \
        | socat stdio,ignoreeof unix-connect:$path_qmp,ignoreeof
}

#do_qmp migrate-local "{'path':'$path_pipe'}" 
do_qmp migrate "{'uri':'unix:/tmp/migrate.sock'}"
