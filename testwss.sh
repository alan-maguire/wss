#!/usr/bin bash

# Compare wss-v2 and wss-v3 numbers for the same process; in the
# wss-v2 case we trace on a per-process basis, for wss-v3 we use
# the cgroup

TESTMEM=
CGROUP_BASE=${CGROUP_BASE:-/sys/fs/cgroup}
CGROUP_MEMORY=$CGROUP_BASE
# cgroup v2 is a shared hierarchy; handle cgroup v1 here.
if [[ -d ${CGROUP_BASE}/memory ]]; then
	CGROUP_MEMORY=${CGROUP_BASE}/memory
fi
CGROUP_TEST=${CGROUP_MEMORY}/foo
mkdir -p ${CGROUP_TEST}

# do not emit headers from wss scripts
export QUIET=1
for p in 1024 2048 4096 ; do
	expected_mb=$(( $p / 256 ))
	echo "Testing for $p pages ($expected_mb Mb)"
	cgexec -g memory:foo ./testmem $p 1 0 >/dev/null 2>&1 &
	TESTMEM=$!
	#echo $TESTMEM > ${CGROUP_TEST}/cgroup.procs
	V2OUT=$(./wss-v2 $TESTMEM 10 | awk '{print $1 "\t" $2}')
	V3OUT=$(./wss-v3 $CGROUP_TEST 10 | awk '{print $1 "\t" $2}')
	V4OUT=$(./wss-v4.py -q -c $CGROUP_TEST | awk '{print $1 "\t" $2}')
	kill $TESTMEM 2>/dev/null
	wait $TESTMEM 2>/dev/null
	echo "v2 Est(s), Mb (per-pid):	$V2OUT"
	echo "v3 Est(s), Mb (per-cgroup):	$V3OUT"
	echo "v4 Est(s), Mb (per-cgroup):	$V4OUT"
	sleep 0.5
done

rmdir $CGROUP_TEST

