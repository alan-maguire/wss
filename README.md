# Working set size estimation on Linux redux

Here we revisit [the work Brendan did](./README_orig.md)
on working-set size estimation, comparing the approach used there
to approaches based on newer kernel features.  Specifically we
will compare

1. the idle page tracking approach Brendan used in [wss-v2.c](./wss-v2.c) -
  tweaked to [work on a per-cgroup basis](./wss-v3.c) rather than
  per-process; vs
2. [multi generational LRU](https://docs.kernel.org/admin-guide/mm/multigen_lru.html)
  as a means for analyzing working set size; vs
3. [pressure stall information (PSI)](https://docs.kernel.org/accounting/psi.html)
  as a means for analyzing impact on working set of memory issues.

All methods are carried out on a per-cgroup basis because

- containers/VMs use cgroups and we want to have a method that
  can capture their memory utilization; and
- we can simply lauch a process in a cgroup via cgexec if we
  want to test that, so we can still support fine-grained tracking of
  workloads if needed.
  
# Resident Set Size (RSS) versus Working Set Size (WSS)

Working set size (WSS) is the amount of memory a process or cgroup _uses_;
resident set size (RSS) is the amount of memory a process or cgroup has
allocated.  These are not always identical, particularly when we are
interested in application behaviour in a specific time window.

[testmem.c](./testmem.c) is a simple program that allows us to manipulate the
RSS and WSS of a workload, either together or separately.

It simply allocates a specified number of pages, then touches a subset of
them (every 1, every 4, etc).  It can be run in a loop with a specified delay;
for example to allocate 65536 pages and touch every one:

```
$ ./testmem 65536 1
  Est(us)        PagesAccessed
   163547                65536 
```
We accessed 65536 pages in 163547 microseconds.  In this case WSS == RSS.

We can see the allocation by examining /proc/$(pgrep testmem)/map

```
00400000-00401000 r-xp 00000000 fc:03 284017028                          /home/src/github/wss/testmem
00600000-00601000 r--p 00000000 fc:03 284017028                          /home/src/github/wss/testmem
00601000-00602000 rw-p 00001000 fc:03 284017028                          /home/src/github/wss/testmem
30e39000-30e5a000 rw-p 00000000 00:00 0                                  [heap]
7fcf47bff000-7fcf57c00000 rw-p 00000000 00:00 0 
7fcf57c00000-7fcf57dcd000 r-xp 00000000 fc:01 134351078                  /usr/lib64/libc-2.28.so
7fcf57dcd000-7fcf57fcd000 ---p 001cd000 fc:01 134351078                  /usr/lib64/libc-2.28.so
7fcf57fcd000-7fcf57fd1000 r--p 001cd000 fc:01 134351078                  /usr/lib64/libc-2.28.so
7fcf57fd1000-7fcf57fd3000 rw-p 001d1000 fc:01 134351078                  /usr/lib64/libc-2.28.so
7fcf57fd3000-7fcf57fd7000 rw-p 00000000 00:00 0 
7fcf58000000-7fcf5802f000 r-xp 00000000 fc:01 134342814                  /usr/lib64/ld-2.28.so
7fcf5822f000-7fcf58230000 r--p 0002f000 fc:01 134342814                  /usr/lib64/ld-2.28.so
7fcf58230000-7fcf58232000 rw-p 00030000 fc:01 134342814                  /usr/lib64/ld-2.28.so
7fcf582f2000-7fcf582f4000 rw-p 00000000 00:00 0 
7fcf58308000-7fcf5830c000 r--p 00000000 00:00 0                          [vvar]
7fcf5830c000-7fcf5830e000 r--p 00000000 00:00 0                          [vvar_vclock]
7fcf5830e000-7fcf58310000 r-xp 00000000 00:00 0                          [vdso]
7fff57531000-7fff57553000 rw-p 00000000 00:00 0                          [stack]
ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0                  [vsyscall]
```

You might expect it to be in the `[heap]` but in fact the allocation is
a separate `mmap()`ing; in this case in

```
7fcf47bff000-7fcf57c00000 rw-p 00000000 00:00 0
```

The reason for this is in the malloc man page:


```
       Normally,  malloc()  allocates  memory  from  the heap, and adjusts the size of the heap as required, using sbrk(2).
       When allocating blocks of memory larger than MMAP_THRESHOLD bytes, the glibc malloc() implementation  allocates  the
       memory  as  a private anonymous mapping using mmap(2).  MMAP_THRESHOLD is 128 kB by default, but is adjustable using
       mallopt(3).  Prior to Linux 4.7 allocations performed using mmap(2) were  unaffected  by  the  RLIMIT_DATA  resource
       limit; since Linux 4.7, this limit is also enforced for allocations performed using mmap(2).
```

So in our case (since the allocation was more than `MMAP_THRESHOLD`)
we got a separate `mmap()`ed region; to verify we will check the size of the
largest anonymous `mmap()`ed region:

```
0x7fcf57c00000 - 0x7fcf47bff000 = 0x10001000
```

This is 268439552 in decimal, which divided by PAGE_SIZE (4096 in this case)
is 65537; i.e. this matches our allocation size of 65536 pages.

Let us see how we measure WSS for `testmem` run in a memory cgroup.  `wss-v3`
is a program - based on Brendan's `wss-v2` - which measures working set size
for a memory cgroup (instead of a process).  The internals are described
below, but for now all we need to know is it can measure working set size.

```
$ sudo mkdir /sys/fs/cgroup/memory/foo
$ sudo cgexec -g memory:foo ./testmem 65536 1 0 > /dev/null 2>&1 &
[1] 449627
$ sudo ./wss-v3 /sys/fs/cgroup/memory/foo 10
Watching '/sys/fs/cgroup/memory/foo'(inode 23651) page references during 10.00 seconds...
Est(s)     Ref(MB)           Ref(Pages)         Total(Pages)
11.122      256.00                65693                65703
$ sudo pkill -TERM testmem
$ sudo rmdir /sys/fs/cgroup/memory/foo
```

As expected, we saw ~256Mb (65536 pages) being used.

If we now configure testmem differently, it will

1. allocate 65536 pages (RSS)
2. touch every 4
3. wait a specified number of seconds (in this case 0)
3. touch every 4 pages again

and so on.

In this case our WSS measurements should be around 1/4 the size of our RSS,
since we only touch 1 in 4 pages.


```
$ sudo mkdir /sys/fs/cgroup/memory/foo
$ sudo cgexec -g memory:foo ./testmem 65536 4 0 > /dev/null 2>&1
$ sudo ./wss-v3 /sys/fs/cgroup/memory/foo 10
Watching '/sys/fs/cgroup/memory/foo'(inode 23683) page references during 10.00 seconds...
Est(s)     Ref(MB)           Ref(Pages)         Total(Pages)
11.823       64.00                16542                16552
```

And that is what we see.  So we see wss-v3 can distinguish resident set
size - memory allocated - from working set size - memory being used.

Now we will discuss various methods for assessing working set size/effects.
We will start with the method used for wss-v2 and wss-v3 - idle page tracking.

# 1. Idle Page Tracking

## What is it?

Idle page tracking provides a way for us to mark memory pages as idle,
and then we can later check if the page was used.  The idle bit associated
with the page will be cleared, and that will tell us the page was used since
we marked it as idle.

## How do I use it?

[Idle page tracking](https://docs.kernel.org/admin-guide/mm/idle_page_tracking.html)
requires a kernel built with

```
CONFIG_PAGE_IDLE_FLAG=y
CONFIG_IDLE_PAGE_TRACKING=y
```

With these in place the kernel exports an idle page tracking interface
in `/sys/kernel/mm/page_idle/bitmap` . The bitmap uses one bit per
physical page, indexed by page frame number (pfn).  We can set the bits
in the idle map by writing all 1s to every bit, and then later read the
idle map back to see which pages were accessed.  If a page was accessed,
the associated idle map bit is set to 0.

To map from cgroup to page frame number we can can use `/proc/kpagecgroup`;
it is a file indexed by page frame number, with each indexed value containing
the cgroup directory inode number associated with that page frame number.

So for example if we have cgroup `/sys/fs/cgroup/memory/foo`, we can get its
inode via `ls -i`

```
$ ls -i /sys/fs/cgroup/memory/
    3 cgroup.clone_children                   9 memory.max_usage_in_bytes
   16 cgroup.event_control                   34 memory.memsw.failcnt
    2 cgroup.procs                           33 memory.memsw.limit_in_bytes
    4 cgroup.sane_behavior                   32 memory.memsw.max_usage_in_bytes
23683 foo                                    31 memory.memsw.usage_in_bytes
```

So it has inode number 23683; now we know every page frame number in
/proc/kpagecgroup with that value belongs to that cgroup.

So putting these two pieces together, we can

- mark all pages as idle by writing to idle page map
- wait for interval
- after interval, read back idle page map into memory; then
- examine all pfns associated with the cgroup to see if associated idle
  bits were switched off; this gives us the number of pages accessed in the
  interval
- repeat

This is what the main loop in [wss-v3.c](./wss-v3.c) does.

[wss-v2.c](./wss-v2.c) was similar, except it had to utilize mappings
in `/proc/<pid>/map`, translating them into pfns to determine per-process
page utilization.

## How accurate is it?

Accuracy is high here, idle page tracking gets us to page-level granularity
for the interval measured.  We can see the numbers above match very closely
the projected working set size based on a synthetic workload.  There are some
caveats to consider with idle page tracking:


## What are the overheads?

There are overheads associated with

- writing the page idle map
- reading the page idle map
- reading the page-to-cgroup mappings

Unfortunately we need to set/read idle flags on all memory with this approach as
pages may be added to the workload dynamically during its operation via
`malloc()`, loading libraries etc.  Similarly we need to read the entire
kpagecgroup map to ensure we find all pfns associated with the cgroup.

The read/write of the idle page map requires read()ing/write()ing to
/sys/kernel/mm/page_idle/bitmap in 8 byte chunks, and there are
(total_memory_pages/64) of these. For a 1Tb memory system, this equates to
`(10^12/4096/8) = 30517578` read/write system calls.  Similarly a read
of kpagecgroup will be expensive since there are 8 bytes per page; thankfully
in that case we can do larger chunked reads.

# 2. Multi-generational LRU

## What is it?

[Multi-generational LRU](https://docs.kernel.org/admin-guide/mm/multigen_lru.html)
is a Least Recently Used (LRU) method for optimizing page reclaim, especially
when we are under memory pressure.

In general with page accesses we need to represent how recently the page has
been accessed, as this can be used to drive page reclaim when under pressure.

Multi-generational LRU incorporates the concept of generations into recency
measure; older generations are considered cold pages potentially eligible
for reclaim.  If a page is accessed it is promoted to a younger generation.

So if we can observe the generation stats for a set of pages, we can
determine the working set size.

Happily LRU already collects stats by generation on a per-cgroup basis,
so we do not need to do extra work to extract these.

## How do I use it?

It requires the kernel to be built with

```
CONFIG_LRU_GEN=y
CONFIG_LRU_GEN_ENABLED=y
```

`/sys/kernel/mm/lru_gen` should be present if this is the case.

First to enable muti-generational LRU, the documentation tells us to

```
$ echo y >/sys/kernel/mm/lru_gen/enabled
```

It provides breakdowns of page accesses, both anonymous and file-backed
per generation and also breaks down results by cgroup.  There is a catch
though which we need to explain before we move on to taking measurements.

There are a few different modes of multi-gen LRU operation, and for accurate
working set size estimation we need to understand them.  As well as
supporting 'y', we can specify any combination of:

- 0x1: switch multi-gen LRU on
- 0x2: large batch access bit clearing for leaf page tables
- 0x4: clear access bit in non-leaf page tables as well

For WSS estimation we observed good accuracy with all modes enabled,
and given that the overheads are reduced with modes 0x2/0x4 enabled,
using 'y' makes sense here.

```
$ echo y >/sys/kernel/mm/lru_gen/enabled
```

Below we observe cgroup stats before and after running `testmem` accessing every
4th page of 65536 pages (i.e. 16384 pages).  No other processes were running
for the cgroup.

```
$ tail -6 /sys/kernel/debug/lru_gen
memcg   110 /foo
 node     0
        104     209928          0           4 
        105     147816          0           0 
        106     144446          0           0 
        107     141762          0           0 
$ cgexec -g memory:foo ./testmem 65536 4 0 > /dev/null 2>&1 &
[1] 862190
$ tail -6 /sys/kernel/debug/lru_gen
memcg   110 /foo
 node     0
        104     253197          0           4 
        105     191085          0           0 
        106     187715      16408           0 
        107     185031          0           0 
```

We see that for generation 106, there were 16408 anonymous (non file-backed)
accesses; this matches closely our expected ~16384 (65536/4).

The oldest generation (104) contains the coldest pages, while generation
107 contains the hottest.  We can also see this by looking at the second
column which tells us that the pages in the generation have been accessed
in the last n msec; so for gen 104 they have been accessed in the last
253197msec, generation 105 in the last 191085mec, etc.

But here comes the interesting part; we can trigger a new
generation!  By doing the following:

```
$ echo "+110 0 107 0 0" > /sys/kernel/debug/lru_gen
```

we are saying create a new generation max_gen_nr+1 (where max_gen_nr is 107
in this case) for the cgroup (110), node id 0. The trailing 0s specify
respectively

- do not force a scan of the anon pages when swap is off
- reduce overhead with an associated reduction in accuracy

Now the lru_gen output looks like this:

```
$ tail -6 /sys/kernel/debug/lru_gen
memcg   110 /foo
 node     0
        105     385350          0           4 
        106     381980      16406           0 
        107     379296          2           0 
        108       1776          0           0 

```

Now generation 106 is approaching being the oldest generation; if we again
add a new generation:

```
$ echo "+110 0 108 0 0" > /sys/kernel/debug/lru_gen
$ tail -6 /sys/kernel/debug/lru_gen
memcg   110 /foo
 node     0
        106     398559      16401           4 
        107     395875          2           0 
        108      18355          5           0 
        109       1100          0           0 
```

And with one more generation added, the pages age out:

```
$ echo "+110 0 109 0 0" > /sys/kernel/debug/lru_gen
$ tail -5 /sys/kernel/debug/lru_gen
memcg   110 /foo
 node     0
        107     404750          2           4 
        108      27230          0           0 
        109       9975          5           0 
        110       2113      16401           0 
```

Now we see the latest generation 110 has the page accesses associated
with our program since the old accesses aged out and page reclaim
ran on them, discovering they were still in use. Hence the promotion
to the latest generation.

So this sketches out the technique; for the a multi-generational
LRU with N generations, age out the current set of N generations;
after doing so the latest generation will then give us an indication 
of current working set size.

[wss-v4.py](./wss-v4.py) implements this approach; given a
cgroup path ('-c cgroup_path') it will age out the current
set of generations gradually over an interval.  It uses the number
of generations as the basis for figuring out when to age out,
so for example for a 10 second interval with 4 generations it will
age out a generation every 2.5 seconds.  Different intervals are
configurable via '-i interval_in_seconds', and we can breakdown
by generations with the '-b' argument.  The full options are as
follows:

```
$ $ sudo ./wss-v4.py --help
usage: Workings set size estimator using Multi-Generational LRU
       [-h] -c CGROUP [-i INTERVAL] [-f] [-b] [-d]

optional arguments:
  -h, --help            show this help message and exit
  -c CGROUP, --cgroup CGROUP
                        cgroup_path
  -i INTERVAL, --interval INTERVAL
                        interval_secs
  -f, --forever         run forever
  -b, --breakdown       breakdown by generation
  -d, --debug           debug mode
```

So for example, we can run it while we have our testmem program
running:

```
$ cgexec -g memory:foo ./testmem 65536 4 0 >/dev/null 2>&1 &
[1] 900006
$ tail -6 /sys/kernel/debug/lru_gen
memcg   110 /foo
 node     0
        242     317484          0           0 
        243     314982          0           0 
        244     312480      16792           4 
        245     309978          0           0 
$ sudo ./wss-v4.py -c /sys/fs/cgroup/memory/foo 
 Est(s)    Ref(MB)           Ref(Pages)                  Gen
10.0111         66                16796             246->249
```

We can breakdown accesses by generation too; this is useful in
exploring access dynamics over the course of the 10 sec interval:

```
$ sudo ./wss-v4.py -c /sys/fs/cgroup/memory/foo -b
 Est(s)    Ref(MB)           Ref(Pages)                  Gen
10.0105          0                    4                  250
10.0105          0                    0                  251
10.0105         66                16790                  252
10.0105          0                    2                  253
```

As mentioned above, we spawn a new generation each 2.5 seconds
in the default 10 second interval, so we get a picture here
not only of which pages were accessed, but also when.  In the case
of our program which is a tight loop accessing pages, nearly all
fall into the same generation.

## How accurate is it?

We see above that the accuracy is not as fine-grained as idle page tracking;
we get ~16400 page accesses for our testmem program where given the synthetic
workload we should see closer to 16384.  However that is pretty close!

We might ask this: why are the pages accessed by our application not
always in the latest generation? XXX check this! From reading the documentation
it appears that multi-generational LRU does its best to avoid walking
each page, starting with page table entries (PTEs) when assessing
recency.  Fine-grained checking only happens on reclaim when pages age
out; in our case we then discover they are still in use and get promoted.

So we age out the generations in order to discover if (what appear to be) cold
pages are in fact still in use, and using this technique we can discover the
working set size.

## What are the overheads?

The documentation states that multi-generational LRU can induce overheads
for some workloads which can be mitigated by enabling clearing accessed
bits on leaf/non-leaf page tables (flags 0x2, 0x4 for
/sys/kernel/mmu/lru_gen/enabled).  As a result we switched all flags on,
and saw high accuracy regardless.

Similarly when aging out generations, the final parameter (force_scan)
can be set to 0 to reduce overhead, again having a smaller impact on accuracy.
We did not observe any differences in accuracy for force_scan=1 versus 0,
so in our explorations we stuck with force_scan=0.

When we forcibly age out the generations to update our working set size
estimate, page reclaim needs to run on the oldest generation, so there
are overheads there too.  However given that reclaim is targeted to a
specific cgroup and that multi-generational LRU utilizes page table-based
reclaim to avoid overheads these should be reasonably small.

Notice that the time taken for aging out the generations in wss-v4.py
is only very slightly more than the interval time (10sec):

```
$ sudo ./wss-v4.py -c /sys/fs/cgroup/memory/foo 
 Est(s)    Ref(MB)           Ref(Pages)                  Gen
  10.01         64                16408             242->245
```

It only takes an additional 10msec - aside from the wait time of 10sec -
to age out the generations and collect the multigenerational LRU info.

Contrast this with the idle page tracking approach utilized by
[wss-v3.c](./wss-v3.c) which takes over a second
to rescan idle page tables, map from page to cgroup etc:

```
$ sudo ./wss-v3 /sys/fs/cgroup/memory/foo 10
Watching '/sys/fs/cgroup/memory/foo'(inode 23683) page references during 10.00 seconds...
Est(s)     Ref(MB)           Ref(Pages)         Total(Pages)
11.823       64.00                16542                16552
```

We also do not get a sense of the time dynamics of accesses with the
idle page tracking approach; with idle page tracking we just know that
it was accessed sometime between when we set idle flags and checked them.

# 3. Pressure Stall Information (PSI)

## What is it?

[Pressure Stall Information](https://docs.kernel.org/accounting/psi.html)
was introduced in the 5.1 kernel with

```
commit 0e94682b73bfa6c44c98af7a26771c9c08c055d5
Author: Suren Baghdasaryan <surenb@google.com>
Date:   Tue May 14 15:41:15 2019 -0700

    psi: introduce psi monitor
    
    Psi monitor aims to provide a low-latency short-term pressure detection
    mechanism configurable by users.  It allows users to monitor psi metrics
    growth and trigger events whenever a metric raises above user-defined
    threshold within user-defined time window.
```  

PSI covers CPU, I/O, interrupt and memory pressure.  Here we will be concerned
with memory pressure exclusively, but much of this applies to the other
pressure metrics.

PSI is measured in the scheduler as time tasks spend waiting for a resource;
CPU, memory or I/O. It is essentially a measure of "lost time"; time we
could have been doing productive work but could not due to resource issues.

The toplevel systemwide PSI metrics are exported in files in /proc/pressure .

Here we attempt a kernel build while running stress-ng


```
$ /usr/bin/stress-ng --vm 2 --vm-bytes=90% --vm-keep > /dev/null 2>&1 &
$ ls /proc/pressure
cpu  io  irq  memory

$ cat /proc/pressure/memory 
some avg10=0.10 avg60=0.05 avg300=0.01 total=127256
full avg10=0.05 avg60=0.02 avg300=0.00 total=79899

```

Each file has a line for "full" which shows stall measurements for all non-idle
tasks, and a line for "some" which shows the share of time some tasks are
stalled.  10, 60 and 300sec cumulative averages are shown. The value is
the proportion of the time wasted; so for avg10=0.1 some task is waiting
for 10% of the interval (1sec), while we see that all tasks are waiting
for 5% of the 10 second time window.

The total= is the absolute stall time in microseconds; it allows us to see
effects too small to make their way into the averages.

Given that the definition of productive varies across the categories, we must
ask what does a productive task mean in the context of memory?  It is defined
as a running, non-reclaiming task.  So we can be left waiting on swap-in,
page cache refaults and page reclaim.

So SOME will have non-zero values if the number of delayed tasks in an interval
is non-zero, while FULL requires both delayed tasks _and_ no productive tasks
for an interval.  Siginificant FULL time for memory is recognized as a good
trigger for moving resources to other machines since jobs on the machine
are not making progress.

The reason the "some" line is important is if multiple tasks
are running and one is blocked, forward progress is still being made in the
case of the unblocked tasks, but we are still stalled on that specific task;
i.e. "full" will be 0, while "some" will reflect that stalled task.

## How do I use it?

First, to enable it in a Linux kernel config, `CONFIG_PSI=y` must be set.  If
`CONFIG_PSI_DEFAULT_DISABLED=y`, it is necessary to pass `psi=1` on the
boot command line to switch it on.

PSI covers CPU, I/O and memory pressure.  Here we will be concerned with
memory pressure exclusively.

Note that pressure stall will not directly answer the question "how big
is the working set size of my workload?" Instead it will answer the question
"is my workload being impacted by memory pressure?" That is often the key
question we want to answer for a system, and it is often why we want to
estimate working set size in the first place. 

The upshot of this is we will not get an apples-with-apples comparison
between PSI and methods that directly determine WSS.  So when benchmarking
the pertinent question is this - is PSI as effective at diagnosing memory
pressure for workloads as estimating workload WSS and determining if it
will place the system under memory pressure?

## What are the overheads?

Overheads were estimated at approximately 1%, but work done since it landed
initially have reduced that somewhat.

# Benchmarking the approaches

Here we will compare the various approaches.

## Per-process versus per-cgroup idle page tracking versus multi-gen LRU

First however we want to make sure that the wss-v2 (per-pid) and wss-v3
(per-cgroup) give the same results using the idle page tracking approach,
and compare these with the wss-v4 multi-gen LRU approach.

We test this via testwss.sh, which
creates a cgroup for the [testmem](./testmem.c) program; it allocates
memory and touches a configurable subset of them.  For example

```
$ ./testmem 1024 4
```

allocates 1024 pages and touches every 4 of them.  By providing an additional
sleep time argument it will run forever; for example

```
$ ./testmem 2048 1 0
```

will allocate 2048 pages in a loop, touching every one and not sleep()ing
between iterations.

Using the testmem program run in a memory cgroup, we can compare per-process
results from wss-v2 to per-cgroup from wss-v3 to the wss-v4 per-cgroup
multi-gen LRU tracking approach.  For each approach we collect the
Est(s) estimated time taken to monitor _and_ collect the data, and also
collect the estimate in Mb.  This will allow us to compare collection
overheads and accuracy.  This is done for 1024, 2048 and 4096 pages, and
we expect to see 4, 8 and 16Mb for accessed pages.

```
$ sudo bash testwss.sh 
Testing for 1024 pages (4 Mb)
v2 Est(s), Mb (per-pid):	11.607 4.00
v3 Est(s), Mb (per-cgroup):	11.630 4.00
v4 Est(s), Mb (per-cgroup):	10.0129 4.1
Testing for 2048 pages (8 Mb)
v2 Est(s), Mb (per-pid):	11.908 8.00
v3 Est(s), Mb (per-cgroup):	11.832 8.00
v4 Est(s), Mb (per-cgroup):	10.0118 8.09
Testing for 4096 pages (16 Mb)
v2 Est(s), Mb (per-pid):	11.943 16.00
v3 Est(s), Mb (per-cgroup):	11.929 16.00
v4 Est(s), Mb (per-cgroup):	10.0084 16.09
```

So we see that both wss-v2 and wss-v3 give identical results when doing
idle page tracking per-process versus per-cgroup.  Multi-generational
LRU gives a slight over-estimate, but notably the overhead in results
collection is much smaller - approximately 10msec - versus 1.6 seconds
to collect and cross-reference idle page info.

## Comparing PSI to WSS estimation

PSI is not a method for estimating WSS, rather it is a method for
determining if the workload is waiting for resources.  As such it
is not possible to do an apples-with-apples comparison to the other
WSS estimation techniques.  Rather what we would like to see is how
PSI measurements respond to various ranges of memory pressure and 
compare the data gathered to that from our WSS estimators.  This will
allow us to see how a specified amount of memory pressure is quantified
in PSI, and how that relates to the measured WSS.

XXX to do
