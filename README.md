# Working set size estimation redux

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
  want to test that, so we can support fine-grained tracking of
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
We accessed 65536 pages in 736 microseconds.  In this case WSS == RSS.

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
3. wait 10s
4. touch every 4 pages again

and so on.

In that case, for step 1, WSS again matches RSS for a narrow window.
But by step 4, our WSS measurements should be around 1/4 the size of our RSS,
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

Here we will 

## What are the overheads?

# 3. Multi-generational LRU

## What is it?

## How do I use it?

## What are the overheads?

# 3. Pressure Stall Information (PSI)

## What is it?

Pressure Stall Information was introduced in the 5.1 kernel with

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

## What are the overheads?


# Benchmarking the approaches

Here we will compare the various approaches.

## Per-process versus per-cgroup idle page tracking

First however we want to make sure that the wss-v2 (per-pid) and wss-v3
(per-cgroup) give the same results using the idle page tracking approach.

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
results from wss-v2 to per-cgroup from wss-v3 for the same workload:

```
$ sudo bash testwss.sh 
Testing for 1024 pages (4 Mb)
v2 Mb (per-pid):	4.00
v3 Mb (per-cgroup):	4.00
Testing for 2048 pages (8 Mb)
v2 Mb (per-pid):	8.00
v3 Mb (per-cgroup):	8.00
Testing for 4096 pages (16 Mb)
v2 Mb (per-pid):	16.00
v3 Mb (per-cgroup):	16.00
```

So we see that both wss-v2 and wss-v3 give identical results when doing
idle page tracking per-process versus per-cgroup.

## Per-cgroup idle page tracking versus multi-generational LRU

