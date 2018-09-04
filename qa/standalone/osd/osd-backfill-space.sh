#!/usr/bin/env bash
#
# Copyright (C) 2017 Red Hat <contact@redhat.com>
#
# Author: David Zafman <dzafman@redhat.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Library Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library Public License for more details.
#

source $CEPH_ROOT/qa/standalone/ceph-helpers.sh

function run() {
    local dir=$1
    shift

    export CEPH_MON="127.0.0.1:7180" # git grep '\<7180\>' : there must be only one
    export CEPH_ARGS
    CEPH_ARGS+="--fsid=$(uuidgen) --auth-supported=none "
    CEPH_ARGS+="--mon-host=$CEPH_MON "
    CEPH_ARGS+="--osd_min_pg_log_entries=5 --osd_max_pg_log_entries=10 "
    CEPH_ARGS+="--fake_statfs_for_testing=1228800 "
    CEPH_ARGS+="--osd_max_backfills=10 "
    export objects=200
    export poolprefix=test

    local funcs=${@:-$(set | sed -n -e 's/^\(TEST_[0-9a-z_]*\) .*/\1/p')}
    for func in $funcs ; do
        setup $dir || return 1
        $func $dir || return 1
        teardown $dir || return 1
    done
}


function get_num_in_state() {
    local state=$1
    local expression
    expression+="select(contains(\"${state}\"))"
    ceph --format json pg dump pgs 2>/dev/null | \
        jq "[.[] | .state | $expression] | length"
}


function wait_for_state() {
    local state=$1
    local num_in_state=-1
    local cur_in_state
    local -a delays=($(get_timeout_delays $2 5))
    local -i loop=0

    flush_pg_stats || return 1
    while test $(get_num_pgs) == 0 ; do
	sleep 1
    done

    while true ; do
        cur_in_state=$(get_num_in_state ${state})
        test $cur_in_state = "0" && break
        if test $cur_in_state != $num_in_state ; then
            loop=0
            num_in_state=$cur_in_state
        elif (( $loop >= ${#delays[*]} )) ; then
            ceph pg dump pgs
            return 1
        fi
        ceph pg dump pgs
        sleep ${delays[$loop]}
        loop+=1
    done
    return 0
}


function wait_for_backfill() {
    local timeout=$1
    wait_for_state backfilling $timeout
}


function wait_for_active() {
    local timeout=$1
    wait_for_state activating $timeout
}


function TEST_backfill_test_simple() {
    local dir=$1
    local pools=2
    local OSDS=3

    run_mon $dir a || return 1
    run_mgr $dir x || return 1
    export CEPH_ARGS

    for osd in $(seq 0 $(expr $OSDS - 1))
    do
      run_osd $dir $osd || return 1
    done

    for p in $(seq 1 $pools)
    do
      create_pool "${poolprefix}$p" 1 1
      ceph osd pool set "${poolprefix}$p" size 1
    done

    wait_for_clean || return 1

    # This won't work is if the 2 pools primary and only osds
    # are the same.

    dd if=/dev/urandom of=$dir/datafile bs=1024 count=4
    for o in $(seq 1 $objects)
    do
      for p in $(seq 1 $pools)
      do
	rados -p "${poolprefix}$p" put obj$o $dir/datafile
      done
    done

    for p in $(seq 1 $pools)
    do
      ceph osd pool set "${poolprefix}$p" size 2
    done

    wait_for_backfill 60
    wait_for_active 60

    ERRORS=0
    if [ "$(ceph pg dump pgs | grep +backfill_toofull | wc -l)" != "1" ];
    then
      echo "One pool should have been in backfill_toofull"
      ERRORS="$(expr $ERRORS + 1)"
    fi

    expected="$(expr $pools - 1)"
    if [ "$(ceph pg dump pgs | grep active+clean | wc -l)" != "$expected" ];
    then
      echo "$expected didn't finish backfill"
      ERRORS="$(expr $ERRORS + 1)"
    fi

    ceph pg dump pgs

    if [ $ERRORS != "0" ];
    then
      return 1
    fi

    for i in $(seq 1 $pools)
    do
      delete_pool "${poolprefix}$i"
    done
    kill_daemons $dir || return 1
}


function TEST_backfill_test_multi() {
    local dir=$1
    local pools=4
    local OSDS=10

    run_mon $dir a || return 1
    run_mgr $dir x || return 1
    export CEPH_ARGS

    for osd in $(seq 0 $(expr $OSDS - 1))
    do
      run_osd $dir $osd || return 1
    done

    for p in $(seq 1 $pools)
    do
      create_pool "${poolprefix}$p" 1 1
      ceph osd pool set "${poolprefix}$p" size 1
    done

    wait_for_clean || return 1

    dd if=/dev/urandom of=$dir/datafile bs=1024 count=4
    for o in $(seq 1 $objects)
    do
      for p in $(seq 1 $pools)
      do
	rados -p "${poolprefix}$p" put obj$o $dir/datafile
      done
    done

    for p in $(seq 1 $pools)
    do
      ceph osd pool set "${poolprefix}$p" size 2
    done
    wait_for_backfill 60
    wait_for_active 60

    ERRORS=0
    if [ "$(ceph pg dump pgs | grep +backfill_toofull | wc -l)" != "1" ];
    then
      echo "One pool should have been in backfill_toofull"
      ERRORS="$(expr $ERRORS + 1)"
    fi

    expected="$(expr $pools - 1)"
    if [ "$(ceph pg dump pgs | grep active+clean | wc -l)" != "$expected" ];
    then
      echo "$expected didn't finish backfill"
      ERRORS="$(expr $ERRORS + 1)"
    fi

    ceph pg dump pgs

    if [ $ERRORS != "0" ];
    then
      return 1
    fi

    for i in $(seq 1 $pools)
    do
      delete_pool "${poolprefix}$i"
    done
    kill_daemons $dir || return 1
}


function TEST_backfill_test_sametarget() {
    local dir=$1
    local pools=10
    local OSDS=5

    run_mon $dir a || return 1
    run_mgr $dir x || return 1
    export CEPH_ARGS

    for osd in $(seq 0 $(expr $OSDS - 1))
    do
      run_osd $dir $osd || return 1
    done

    for p in $(seq 1 $pools)
    do
      create_pool "${poolprefix}$p" 1 1
      ceph osd pool set "${poolprefix}$p" size 2
    done
    sleep 5

    wait_for_clean || return 1

    ceph pg dump pgs

    # Find 2 pools with a pg that distinct primaries but second
    # replica on the same osd.
    local PG1
    local POOLNUM1
    local pool1
    local chk_osd1
    local chk_osd2

    local PG2
    local POOLNUM2
    local pool2
    for p in $(seq 1 $pools)
    do
      ceph pg map ${p}.0 --format=json | jq '.acting[]' > $dir/acting
      local test_osd1=$(head -1 $dir/acting)
      local test_osd2=$(tail -1 $dir/acting)
      if [ $p = "1" ];
      then
        PG1="${p}.0"
        POOLNUM1=$p
        pool1="${poolprefix}$p"
        chk_osd1=$test_osd1
        chk_osd2=$test_osd2
      elif [ $chk_osd1 != $test_osd1 -a $chk_osd2 = $test_osd2 ];
      then
        PG2="${p}.0"
        POOLNUM2=$p
        pool2="${poolprefix}$p"
        break
      fi
    done
    rm -f $dir/acting

    if [ "$pool2" = "" ];
    then
      echo "Failure to find appropirate PGs"
      return 1
    fi

    for p in $(seq 1 $pools)
    do
      if [ $p != $POOLNUM1 -a $p != $POOLNUM2 ];
      then
        delete_pool ${poolprefix}$p
      fi
    done

    ceph osd pool set $pool1 size 1
    ceph osd pool set $pool2 size 1

    wait_for_clean || return 1

    dd if=/dev/urandom of=$dir/datafile bs=1024 count=4
    for i in $(seq 1 $objects)
    do
	rados -p $pool1 put obj$i $dir/datafile
        rados -p $pool2 put obj$i $dir/datafile
    done

    ceph osd pool set $pool1 size 2
    ceph osd pool set $pool2 size 2
    wait_for_backfill 60
    wait_for_active 60

    ERRORS=0
    if [ "$(ceph pg dump pgs | grep +backfill_toofull | wc -l)" != "1" ];
    then
      echo "One pool should have been in backfill_toofull"
      ERRORS="$(expr $ERRORS + 1)"
    fi

    if [ "$(ceph pg dump pgs | grep active+clean | wc -l)" != "1" ];
    then
      echo "One didn't finish backfill"
      ERRORS="$(expr $ERRORS + 1)"
    fi

    ceph pg dump pgs

    if [ $ERRORS != "0" ];
    then
      return 1
    fi

    delete_pool $pool1
    delete_pool $pool2
    kill_daemons $dir || return 1
}


main osd-backfill-space "$@"

# Local Variables:
# compile-command: "make -j4 && ../qa/run-standalone.sh osd-backfill-space.sh"
# End:
