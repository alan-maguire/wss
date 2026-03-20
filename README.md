# Working set size estimation on Linux redux

Here we revisit [the work Brendan did](./README_orig.md)
on working-set size estimation, comparing the approach used there
to approaches based on newer kernel features.  Specifically we
will compare

1. the idle page tracking approach Brendan used in [wss-v2.c](https://github.com/brendangregg/wss/blob/master/wss-v2.c) -
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
resident in memory.  These are not always identical, particularly when we are
interested in application behaviour in a specific time window.

Keeping track of RSS is easier for the kernel since it is involved in
adding/removing memory from a process, whereas WSS is dictated by
application memory access patterns that will change over time, and
for these the kernel updates data that is harder to collate like page
accessed bits.  It would be expensive for the kernel to constantly ask
"How much memory is each task using right now?", and of course different
users would want different definitions of "right now".

[testmem.c](./testmem.c) is a simple program that allows us to manipulate the
RSS and WSS of a workload, either together or separately.

It simply allocates a specified number of pages via mmap(), then touches a
subset of pages (every 1, every 4, etc).  It can be run in a loop with
a specified delay; for example to allocate 65536 pages and touch every one:

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

The allocation is in a separate `mmap()`ing; in this case in

```
7fcf47bff000-7fcf57c00000 rw-p 00000000 00:00 0
```

We see that the size is

```
0x7fcf57c00000 - 0x7fcf47bff000 = 0x10001000
```

This is 268439552 in decimal, which divided by PAGE_SIZE (4096 in this case)
is 65537; i.e. this matches our allocation size of 65536 pages.

Note that when `mmap()`ing in `testmem` we used

```
mem = mmap(NULL, pagesize * numpages, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_POPULATE, -1, 0);
```

We wanted an anonymous (non file-backed) mapping that is pre-faulted
(`MAP_POPULATE`); this will trigger a resident set size (RSS) of ~65536 pages.
Critically the WSS does not have to match the RSS forever; we can alter
testmem behaviour to only touch every 4th page after initial pre-faulting.
This will allow us to see if WSS measurements capture the WSS/RSS distinction.

Let us see how we measure WSS for `testmem` run in a memory cgroup.  `wss-v3`
is a program - based on Brendan's `wss-v2` - which measures working set size
for a memory cgroup (instead of a process).  The internals are described
below, but for now all we need to know is it can measure working set size.

All commands below are run as root.
```
$ mkdir /sys/fs/cgroup/memory/foo
$ cgexec -g memory:foo ./testmem 65536 1 0 > /dev/null 2>&1 &
[1] 449627
$ ./wss-v3 /sys/fs/cgroup/memory/foo 10
Watching '/sys/fs/cgroup/memory/foo'(inode 23651) page references during 10.00 seconds...
Est(s)     Ref(MB)           Ref(Pages)         Total(Pages)
11.122      256.00                65693                65703
$ pkill -TERM testmem
$ rmdir /sys/fs/cgroup/memory/foo
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
$ mkdir /sys/fs/cgroup/memory/foo
$ cgexec -g memory:foo ./testmem 65536 4 0 > /dev/null 2>&1
$ ./wss-v3 /sys/fs/cgroup/memory/foo 10
Watching '/sys/fs/cgroup/memory/foo'(inode 23683) page references during 10.00 seconds...
Est(s)     Ref(MB)           Ref(Pages)         Total(Pages)
11.823       64.00                16542                16552
```

And that is what we see.  So we see wss-v3 can distinguish resident set
size - memory allocated - from working set size - memory being used.

Note that we can see RSS easily via the second value in `/proc/<pid>/statm`
or indeed via `/proc/<pid>/status`:

```
$ cat /proc/$(pgrep testmem)/statm
66616 65843 287 1 0 65581 0
```

Above we see RSS of 65843; so this illustrates that RSS is not the
same as WSS for this workload.

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
per generation and also breaks down results by cgroup.
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
$ mkdir /sys/fs/cgroup/memory/foo
$ tail -6 /sys/kernel/debug/lru_gen
memcg    85 /foo
 node     0
          0       5922          0           0 
          1       5922          0           0 
          2       5922          0           0 
          3       5922          0           0 
$ cgexec -g memory:foo ./testmem 65536 4 0 > /dev/null 2>&1 &
[1] 58982
$ tail -6 /sys/kernel/debug/lru_gen
memcg    85 /foo
 node     0
          0      22039          0           0 
          1      22039          0           0 
          2      22039      65559           0 
          3      22039          0           0 
```

We see that for generation 2, there were 65559 anonymous (non file-backed)
accesses; this corresponds to our mmap()ed memory.
The oldest generation (0) contains the coldest pages, while generation
3 contains the hottest.

But here comes the interesting part; we can trigger a new
generation!  By doing the following:

```
$ echo "+85 0 3" > /sys/kernel/debug/lru_gen
```

we are saying create a new generation max_gen_nr+1 (where max_gen_nr is 3
in this case) for the cgroup (85), node id 0.

Now the lru_gen output looks like this:

```
$ tail -6 /sys/kernel/debug/lru_gen
memcg    85 /foo
 node     0
          1     207652          0           0 
          2     207652         20           0 
          3     207652      65539           0 
          4       1624          0           0 
```

Notice what has happened to the age_in_ms column; we while generations
1-3 have the same age (207652), we have now created a younger generation.
We will wait a number of seconds and do the same again:

```
$ echo "+85 0 4" > /sys/kernel/debug/lru_gen
memcg    85 /foo
 node     0
          2     232082          9           0 
          3     232082      49154           0 
          4      26054      16396           0 
          5       1680          0           0 
```

Now here is where things get interesting; we now have two new generations
of age_in_ms 1680 (hottest) and 26054ms.  But critically what we see is
that the cold pages associated with the initial `mmap()` have been split
out from the measurements, and the hotter pages associated with the 
page access loop (1/4 of the total) are now in generation 4.  So we are
isolating hot and cold pages!

This is why waiting for a period between creating new generations is
crtical; it allows us to separate pages into hot/cold and isolate
working set from resident set.

Let us go again:

```
$ echo "+85 0 5" > /sys/kernel/debug/lru_gen
$ tail -6 /sys/kernel/debug/lru_gen
memcg    85 /foo
 node     0
          3     268370      49163           0 
          4      62342          0           0 
          5      37968      16396           0 
          6       7475          0           0 
```

Now we see the coldest pages are in the oldest generation and have
not been accessed in 268370ms (268 seconds).  So not only can we
assess working set size, but by spawning generations regularly we
can understand the time dynamics of memory use.

Contrast this with the RSS measurement; it has none of this
detail:

```
$ cat /proc/$(pgrep testmem)/statm
66649 65895 335 1 0 65614 0
```

So we see the 65895 is roughly equal to the LRU sum across generations,
but we are collapsing the results across time and losing the temporal access
patterns which inform our WSS estimate.

So this sketches out the technique; for the a multi-generational
LRU with N generations, age out the current set of N generations;
after doing so the latest generations will then give us an indication 
of current working set size.

Note that some systems will have multiple NUMA nodes and in that
case we would need to create generations for each node.

[wss-v4.py](./wss-v4.py) implements this approach; given a
cgroup name ('-c cgroup') it will age out the current
set of generations gradually over an interval.  It uses the number
of generations as the basis for figuring out when to age out,
so for example for a 10 second interval with 4 generations it will
age out a generation every 2.5 seconds.  Different intervals are
configurable via '-i interval_in_seconds', and we can breakdown
by generations with the '-b' argument.  The full options are as
follows:

```
$ ./wss-v4.py --help
usage: Working set size estimator using Multi-Generational LRU
       [-h] -c CGROUP [-q] [-i INTERVAL] [-f] [-b] [-o] [-d]

optional arguments:
  -h, --help            show this help message and exit
  -c CGROUP, --cgroup CGROUP
                        cgroup_name
  -q, --quiet           quiet mode
  -i INTERVAL, --interval INTERVAL
                        interval_secs
  -f, --forever         run forever
  -b, --breakdown       breakdown by generation
  -o, --omit_oldest     omit oldest generation of pages (coldest pages) from WSS estimate
  -d, --debug           debug mode
```

So for example, we can run it while we have our testmem program
running:

```
$ cgexec -g memory:foo ./testmem 65536 4 0 >/dev/null 2>&1 &
[1] 900006
$ tail -6 /sys/kernel/debug/lru_gen
memcg    85 /foo
 node     0
          0      16456          0           0 
          1      16456          0           0 
          2      16456      65560           0 
          3      16456          0           0 
$ ./wss-v4.py -c foo -o
 Est(s)    Ref(MB)           Ref(Pages)
10.0365      64.05                16397

```

Importantly, the `-o` option is used here; it omits the oldest
generation (coldest pages) from the WSS calcuation; since we
know those pages are > 10 secs old we do not want to consider
them as part of WSS.  We can configure the interval to be larger
of course via the `-i interval` option.

We can breakdown accesses by generation too; this is useful in
exploring access dynamics over the course of the 10 sec interval:

```
$ ./wss-v4.py -c foo -b
 Est(s)    Ref(MB)           Ref(Pages)
10.0448     192.04                49163
10.0448        0.0                    0
10.0448      64.05                16397
10.0448        0.0                    0
```

As mentioned above, we spawn a new generation each 2.5 seconds
in the default 10 second interval, so we get a picture here
not only of which pages were accessed, but also when.  In the case
of our program which is a tight loop accessing pages, all fall into
the same generation while the original page accesses from the
`mmap()` are in the coldest generation.  Generations are ordered -
as in /sys/kernel/debug/lru_gen - from oldest to most recent.

We noted above that it would be expensive for the kernel to constantly
ask how much memory processes are using, but we see here that we get
this answer for multi-generational LRU as a byproduct of something the
kernel does anyway - aging out old pages.  And we are in control of
the aging, so for a given cgroup we can observe the changing memory
access patterns over a chosen time window.

## How accurate is it?

We see above that the accuracy is not as fine-grained as idle page tracking;
we get ~16400 page accesses for our testmem program where given the synthetic
workload we should see closer to 16384.  However that is pretty close!

So we age out the generations in order to discover if (what appear to be) cold
pages are in fact still in use, and using this technique we can discover the
working set size, and indeed how it changes over time.

And that is key; we get a picture of WSS over time as well as at a
particular moment.

## What are the overheads?

The documentation states that multi-generational LRU can induce overheads
for some workloads which can be mitigated by enabling clearing accessed
bits on leaf/non-leaf page tables (flags 0x2, 0x4 for
/sys/kernel/mmu/lru_gen/enabled).  As a result we switched all flags on,
and saw high accuracy regardless.

When we forcibly age out the generations to update our working set size
estimate, page reclaim needs to run on the oldest generation, so there
are overheads there too.  However given that reclaim is targeted to a
specific cgroup and that multi-generational LRU utilizes page table-based
reclaim to avoid overheads these should be reasonably small.

Notice that the time taken for aging out the generations in wss-v4.py
is only very slightly more than the interval time (10sec):

```
$ ./wss-v4.py -c foo -o
 Est(s)    Ref(MB)           Ref(Pages)
  10.01         64                16408
```

It only takes an additional 10msec - aside from the wait time of 10sec -
to age out the generations and collect the multigenerational LRU info.

Contrast this with the idle page tracking approach utilized by
[wss-v3.c](./wss-v3.c) which takes over a second
to rescan idle page tables, map from page to cgroup etc:

```
$ ./wss-v3 /sys/fs/cgroup/memory/foo 10
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
[was introduced in the 4.20 kernel](https://kernelnewbies.org/Linux_4.20#Pressure_stall_information_for_better_overview_of_system_load),
with monitoring added later in 5.1.

PSI covers CPU, I/O, interrupt and memory pressure.  Here we will be concerned
with memory pressure exclusively, but much of this applies to the other
pressure metrics.

PSI is measured in the scheduler as time tasks spend waiting for a resource;
CPU, memory or I/O. It is essentially a measure of "lost time"; time we
could have been doing productive work but could not due to resource issues.

The toplevel systemwide PSI metrics are exported in files in /proc/pressure .

Here we attempt a kernel build while running stress-ng and examine overall
system PSI stats (in `/proc/pressure/memory`):


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
for 0.1% of the interval (1sec), while we see that all tasks are waiting
for 0.05% of the 10 second time window.

The total= is the absolute stall time in microseconds; it allows us to see
effects too small to make their way into the averages.

The equivalent per-cgroup stats are in `memory.pressure` in the cgroup directory.

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
pages of memory and touches a configurable subset of them.  For example

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
overheads and accuracy.  This is done for 4096 and 65536 pages, and
we expect to see 16Mb, 256Mb for accessed pages.  We also collect
RSS measured from `/proc/$(pgrep testmem)/statm`.

First we measure for RSS == WSS for 4096 and 65536 pages, then we
measure for WSS == 1/4 of RSS (where we access every 4th page).
In the latter case we expect to see 4Mb/64Mb for WSS (1/4 of the
RSS):

```
Testing wss-v2 for 1/1 of 4096 pages (16 Mb WSS, 16 Mb RSS)
wss-v2 results:
Est(s) WSS(Mb)  RSS(Mb)
10.589	16.00   17
Testing wss-v3 for 1/1 of 4096 pages (16 Mb WSS, 16 Mb RSS)
wss-v3 results:
Est(s) WSS(Mb)  RSS(Mb)
10.659	16.00   17
Testing wss-v4 for 1/1 of 4096 pages (16 Mb WSS, 16 Mb RSS)
wss-v4 results:
Est(s) WSS(Mb)  RSS(Mb)
10.0136	16.02   17
Testing wss-v2 for 1/1 of 65536 pages (256 Mb WSS, 256 Mb RSS)
wss-v2 results:
Est(s) WSS(Mb)  RSS(Mb)
10.649	256.00   257
Testing wss-v3 for 1/1 of 65536 pages (256 Mb WSS, 256 Mb RSS)
wss-v3 results:
Est(s) WSS(Mb)  RSS(Mb)
10.740	256.00   257
Testing wss-v4 for 1/1 of 65536 pages (256 Mb WSS, 256 Mb RSS)
wss-v4 results:
Est(s) WSS(Mb)  RSS(Mb)
10.0496	256.02   257
Testing wss-v2 for 1/4 of 4096 pages (4 Mb WSS, 16 Mb RSS)
wss-v2 results:
Est(s) WSS(Mb)  RSS(Mb)
10.444	4.00   17
Testing wss-v3 for 1/4 of 4096 pages (4 Mb WSS, 16 Mb RSS)
wss-v3 results:
Est(s) WSS(Mb)  RSS(Mb)
10.495	4.00   17
Testing wss-v4 for 1/4 of 4096 pages (4 Mb WSS, 16 Mb RSS)
wss-v4 results:
Est(s) WSS(Mb)  RSS(Mb)
10.012	4.02   17
Testing wss-v2 for 1/4 of 65536 pages (64 Mb WSS, 256 Mb RSS)
wss-v2 results:
Est(s) WSS(Mb)  RSS(Mb)
10.631	64.00   257
Testing wss-v3 for 1/4 of 65536 pages (64 Mb WSS, 256 Mb RSS)
wss-v3 results:
Est(s) WSS(Mb)  RSS(Mb)
10.897	64.00   257
Testing wss-v4 for 1/4 of 65536 pages (64 Mb WSS, 256 Mb RSS)
wss-v4 results:
Est(s) WSS(Mb)  RSS(Mb)
10.0368	64.02   257

```

So we see that wss-v2, wss-v3 and wss-v4 give almost identical results
for working set size; multi-generational LRU is out by 0.2Mb.
They all distinguish RSS from WSS; where we access 1/4 of pages,
all wss estimates notice this.

In terms of overhead it takes an additional 0.5 seconds to collect
the results for wss-v2 and wss-v3; this relates to the overheads
associated with idle page tracking read discussed above.  Contrast
this with wss-v4 however; since the multi-generational LRU tracking
is how we age pages and the aggregation of page ages into generations
is done in-kernel for us, the overheads are much lower - between
10-50msec.

## Comparing PSI to WSS estimation

PSI is not a method for estimating WSS, rather it is a method for
determining if the workload is waiting for resources.  As such it
is not possible to do an apples-with-apples comparison to the other
WSS estimation techniques.  Rather what we would like to see is how
PSI measurements respond to various ranges of memory pressure and 
compare the data gathered to that from our WSS estimators.  This will
allow us to see how a specified amount of memory pressure is quantified
in PSI, and how that relates to the measured WSS.

We will limit our cgroup memory utilization such that we set a hard
upper limit (`memory.max`) of 400Mb, while we will set our memory
reclaim limit (`memory.high`) to 220Mb.

```
$ mkdir /sys/fs/cgroup/foo
$ cd /sys/fs/cgroup/foo
$ echo 400M > memory.max
$ echo 220M > memory.high
```

We will first run our testmem program with a WSS of 65536 pages
(256Mb) so that we see how exceeding memory.high impacts on PSI
measurements.  In one window run `testmem` to touch every page
every second, running forever (-1):

```
 cgexec -g memory:foo ./testmem 65536 1 1 -1 
   Est(us)        PagesAccessed                  Set                Unset
    677909                65536                65536                    0
   4312142                65536                    0                65536
```

While in another:

```
$ while true; do echo; cat memory.pressure ; sleep 1; done

some avg10=16.91 avg60=12.65 avg300=6.77 total=108268241
full avg10=16.91 avg60=12.65 avg300=6.76 total=107974404

some avg10=17.47 avg60=12.89 avg300=6.87 total=108285216
full avg10=17.47 avg60=12.89 avg300=6.85 total=107991379

```

Note that some and all are equal since there is only a single task
in the cgroup.

So we see that 16/17% of the time is spent waiting on memory reclaim
since we have breached memory.high.

Let us check that a lower WSS - where we touch 1 in 4 pages - is
reflected in PSI info.  Note we must let the avg300 window clear out
by waiting 5 minutes before measuring to avoid cross-contamination from
previous experiments, or create a fresh cgroup.

```
$  cgexec -g memory:foo ./testmem 65536 4 1 -1
   Est(us)        PagesAccessed                  Set                Unset
     29064                16384                16384                    0
       244                16384                    0                16384
       229                16384                16384                    0
       205                16384                    0                16384
       234                16384                16384                    0
       214                16384                    0                16384

```

In another window:

```
$ while true; do echo; cat memory.pressure ; sleep 1; done

some avg10=1.08 avg60=0.19 avg300=0.04 total=124762
full avg10=1.08 avg60=0.19 avg300=0.04 total=124762

some avg10=1.08 avg60=0.19 avg300=0.04 total=124762
full avg10=1.08 avg60=0.19 avg300=0.04 total=124762

some avg10=0.88 avg60=0.18 avg300=0.04 total=124762
full avg10=0.88 avg60=0.18 avg300=0.04 total=124762

...

some avg10=0.00 avg60=0.08 avg300=0.02 total=124762
full avg10=0.00 avg60=0.08 avg300=0.02 total=124762

```

We see over time the average falls as the total time spent
waiting comprises the intial - above memory.high - 256Mb.

So we see that over time PSI does reflect WSS since memory
reclaim is driven by actual memory usage over time - i.e.
working set size.

# Summary

So we have learnt

1. Working Set Size (WSS) is an important measure of the memory
   your application is actually using (rather than memory it has
   reserved).

2. There are a number of ways to measure it, both directly via
   idle page tracking and multi-generational LRU, and indirectly
   via memory pressure.

3. Each approach has its tradeoffs, but the above suggests that
   multi-generational LRU is a valuable low-overhead approach for
   WSS estimation which allows for fine-grained tracking, while
   PSI is a great approach if the question you want to answer is
   "is my workload stalled?"

# References

- [Brendan's original WSS repository](https://github.com/brendangregg/wss)
- [Idle Page Tracking](https://docs.kernel.org/admin-guide/mm/idle_page_tracking.html)
- [Multi-Generational LRU](https://docs.kernel.org/admin-guide/mm/multigen_lru.html)
- [Pressure Stall Information](https://docs.kernel.org/accounting/psi.html)

