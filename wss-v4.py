#!/usr/bin/env python3
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

import argparse
import os
import time

def parse_args():
    parser = argparse.ArgumentParser('Working set size estimator using Multi-Generational LRU',
                                     formatter_class=argparse.RawTextHelpFormatter)

    parser.add_argument('-c', '--cgroup', required=True, type=str,
                        help='cgroup_name')
    parser.add_argument('-q', '--quiet', required=False, action='store_true',
                        help='quiet mode')
    parser.add_argument('-i', '--interval', required=False, type=int,
                        default=10, help='interval_secs')
    parser.add_argument('-f', '--forever', required=False, action='store_true',
                        help='run forever') 
    parser.add_argument('-b', '--breakdown', required=False, action='store_true',
                        help='breakdown by generation')
    parser.add_argument('-o', '--omit_oldest', required=False, action='store_true',
                        help='omit oldest generation of pages (coldest pages) from WSS estimate')
    parser.add_argument('-d', '--debug', required=False, action='store_true',
                        help='debug mode')

    args = parser.parse_args()
    return args

def parse_lru_gen(cgroup_name):
    cgroup_id = 0
    cgroup_gens = 0
    cgroup_lastgen = []
    cgroup_nodes = 0
    cgroup_anon = []
    cgroup_file = []
    try:
        with open('/sys/kernel/debug/lru_gen', mode='r') as f:
            lines = f.readlines()

            id = 0
            i = 1
            memindex = 0
            for line in lines:
                line = line.strip()
                data = line.split()
                if (len(data) < 2):
                    continue
                if (data[0] == 'memcg'):
                    # done since we already found our cgroup
                    if (id > 0):
                        break
                    if (data[2] != cgroup_name):
                        continue
                    id = int(data[1])
                    cgroup_id = id
                    cgroup_anon = []
                    cgroup_file = []
                    cgroup_nodes = 0
                    continue

                # we are dealing with memcg data for our cgroup
                # either of form
                # node N
                #  gen age_in_msec anon_pages file_pages
                if (id > 0):
                   # skip node lines
                   if (data[0] == 'node'):
                       memindex = 0
                       cgroup_gens = 0
                       cgroup_nodes = cgroup_nodes + 1
                       continue
                   if (len(data) < 4):
                       continue
                   lastgen = int(data[0])
                   # gather gen, anon, file data
                   if (len(cgroup_lastgen) < cgroup_nodes):
                       cgroup_lastgen.append(lastgen)
                   else:
                       cgroup_lastgen[cgroup_nodes - 1] = lastgen
                   cgroup_gens = cgroup_gens + 1
                   canon=int(data[2])
                   cfile=int(data[3])
                   if (len(cgroup_anon) <= memindex):
                       cgroup_anon.append(canon)
                   else:
                       cgroup_anon[memindex] = cgroup_anon[memindex] + canon
                   if (len(cgroup_file) <= memindex):
                       cgroup_file.append(cfile)
                   else:
                       cgroup_file[memindex] = cgroup_file[memindex] + cfile
                   memindex = memindex + 1
                   continue
                # Look for line of form
                # memcg id cgroupname
                if (data[0] != 'memcg'):
                    continue
                # reset id
                if (id > 0):
                    id = 0
                    cgroup_nodes = 0
                if (data[2] != cgroup_name):
                    continue
                id = int(data[1])
                cgroup_id = id
                cgroup_anon = []
                cgroup_file = []
    except IOError:
            print('Could not read lru gen file; ensure run via sudo + CONFIG_LRU_GEN is enabled for your kernel')
    f.close()
    if cgroup_id == 0:
        print('Could not find cgroup ' + cgroup_name)
        exit(1)
    return cgroup_id, cgroup_nodes, cgroup_gens, cgroup_lastgen, cgroup_anon, cgroup_file

def init_lru_gen():
    try:
        with open('/sys/kernel/mm/lru_gen/enabled', 'w') as f:
            f.write('y')
            f.close()
    except IOError:
            print('Could not write lru gen file; ensure run via sudo + CONFIG_LRU_GEN is enabled for your kernel')

def write_lru_gen(args, cgroup_id, node, lastgen):
    try:
        with open('/sys/kernel/debug/lru_gen', 'w') as f:
            cmd = '+' + str(cgroup_id) + ' ' + str(node) + ' ' + str(lastgen)
            if (args.debug):
                print('Writing ', cmd)
            f.write(cmd)
            f.close()
    except IOError:
            print('Could not write lru gen file; ensure run via sudo + CONFIG_LRU_GEN is enabled for your kernel')

def main(args):
    if (args.cgroup[0] != '/'):
        cgroup_name = '/' + args.cgroup
    else:
        cgroup_name = args.cgroup
    pagesize = 4096
    mb = 1024 * 1024
    init_lru_gen()
    cgroup_id, cgroup_nodes, cgroup_gens, cgroup_lastgen, cgroup_anon, cgroup_file = parse_lru_gen(cgroup_name)
    if (args.debug):
        print("cgroup", cgroup, "id", cgroup_id, "#nodes", cgroup_nodes,
              "lastgens", cgroup_lastgen, "anon pages:", cgroup_anon,
              "file pages:", cgroup_file)
    cols=[ 'Est(s)', 'Ref(MB)', 'Ref(Pages)']
    if (args.quiet == False):
        print(f'{cols[0]:>7} {cols[1]:>10} {cols[2]:>20}')

    while True:
        start = time.time()
        for i in range(cgroup_gens):
            for j in range(cgroup_nodes):
                write_lru_gen(args, cgroup_id, j, cgroup_lastgen[j])
            time.sleep(args.interval/cgroup_gens)
        _, _, _, cgroup_lastgen, cgroup_anon, cgroup_file = parse_lru_gen(cgroup_name)
        end = time.time()
        time_secs = end - start
        if (args.debug):
            print("cgroup", cgroup_name, "id", cgroup_id,
                  "anon_pages:", cgroup_anon,
                  "file_pages:", cgroup_file)
        time_taken = str(round(time_secs, 4))
        if (args.breakdown):
            for i in range(cgroup_gens):
                if (args.omit_oldest) and (i == 0):
                    continue
                gen_total = cgroup_anon[i] + cgroup_file[i]
                gen_pages = str(gen_total)
                gen_mb = str(round(gen_total*pagesize/mb, 2))
                print(f'{time_taken:>7} {gen_mb:>10} {gen_pages:>20}')
        else:
            total_anon = sum(cgroup_anon)
            total_file = sum(cgroup_file)
            if (args.omit_oldest):
                total_anon = total_anon - cgroup_anon[0]
                total_file = total_file - cgroup_file[0]
            total = total_anon + total_file
            total_pages = str(total)
            total_mb = str(round(total*pagesize/mb, 2))
            print(f'{time_taken:>7} {total_mb:>10} {total_pages:>20}')
        if (args.forever == False):
            break

if __name__ == '__main__':
    args = parse_args()
    main(args)
