#!/usr/bin/bash
# SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
#
# Copyright (c) 2025, Oracle and/or its affiliates.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public
# License v2 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public
# License along with this program; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 021110-1307, USA.
#

# Compare wss-v2, wss-v3 wss-v4 numbers for the same workload; in the
# wss-v2 case we trace on a per-process basis, for wss-v3/v4 we use
# the cgroup

TESTMEM=
CGROUP_BASE=${CGROUP_BASE:-/sys/fs/cgroup}
CGROUP_MEMORY=$CGROUP_BASE
# cgroup v2 is a shared hierarchy; handle cgroup v1 here.
if [[ -d ${CGROUP_BASE}/memory ]]; then
	CGROUP_MEMORY=${CGROUP_BASE}/memory
fi
export CGROUP_NAME=testwss.$$
CGROUP_TEST=${CGROUP_MEMORY}/${CGROUP_NAME}

test()
{
	pages=$1
	proportion=$2
	wsscmd=$3
	rss_mb=$(($pages / 256 ))
	expected_mb=$(($pages / ($proportion * 256)))

	echo "Testing $wsscmd for 1/$proportion of $pages pages ($expected_mb Mb WSS, $rss_mb Mb RSS)"
	mkdir ${CGROUP_TEST}
	cgexec -g memory:${CGROUP_NAME} ./testmem $pages $proportion 0 > /dev/null 2>&1 &
	TESTMEM=$!
	sleep 1
	case $wsscmd in
	"wss-v2")	WSSCMD="./wss-v2 $TESTMEM 10";;
	"wss-v3")	WSSCMD="./wss-v3 $CGROUP_TEST 10";;
	"wss-v4")	WSSCMD="./wss-v4.py -q -c $CGROUP_TEST -i 10";;
	esac
	OUT=$($WSSCMD | awk '{print $1 "\t" $2}')
	RSSPAGES=$(cat /proc/${TESTMEM}/statm |awk '{print $2}')
	RSS=$(( $RSSPAGES * 16 / 4096 ))
	kill $TESTMEM 2>/dev/null
	wait $TESTMEM 2>/dev/null
	sleep 0.2
	rmdir ${CGROUP_TEST}
	echo "$wsscmd results:"
	echo "Est(s) WSS(Mb)  RSS(Mb)"
	echo "$OUT   $RSS"
}

# do not emit headers from wss c progs
export QUIET=1
for q in 1 4; do
    for p in 4096 65536; do
	test $p $q wss-v2
	test $p $q wss-v3
	test $p $q wss-v4
    done
done
