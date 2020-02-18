#!/usr/bin/env python3

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2020 John Kacur <jkacur@redhat.com>

import subprocess, signal, argparse, re, glob, sys

parser = argparse.ArgumentParser(description='Get a snapshot of running instances of cyclictest')
parser.add_argument('-l', '--list', action='store_true', help='list the main pid(s) of running instances of cyclictest')
parser.add_argument('-s', '--snapshot', nargs='*', metavar='pid', help='take a snapshot of running instances of cyclictest')
parser.add_argument('-p', '--print', nargs='*', metavar='pid', help='print the snapshots')
args = parser.parse_args()


class Snapshot:

    def __init__(self):
        self.pids = []
        self.shm_files = []
        self.refresh()

    def refresh(self):
        self.pids = []
        self.shm_files = glob.glob('/dev/shm/cyclictest*')
        self.shm_files.sort()
        for shm_file in self.shm_files:
            pid = re.search('[0-9]*$', shm_file).group()
            self.pids += [pid]

    # Send USR2 to all running instances of cyclictest or just to
    # a specific pid (spid) if specified
    def take_snapshot(self, spids=None):
        for pid in self.pids:
            if (spids == None) or (pid in spids):
                # print("kill -s USR2 ", pid)
                subprocess.run(["kill", "-s", "USR2", pid])

    def print_pids(self):
        for pid in self.pids:
            print(pid)

    # Print the data in /dev/shm/cyclictest*
    def print(self, spids=None):
        if spids == None:
            for shm_file in self.shm_files:
                with open(shm_file, 'r') as f:
                    data = f.read()
                print(data)
        else:
            for spid in spids:
                if spid in self.pids:
                    shm_file = '/dev/shm/cyclictest' + spid
                    with open(shm_file, 'r') as f:
                        data = f.read()
                    print(data)

snapshot = Snapshot()

if args.list:
    snapshot.print_pids()

if args.snapshot != None:
    if len(args.snapshot) == 0:
        snapshot.take_snapshot()
    else:
        snapshot.take_snapshot(args.snapshot)

if args.print != None:
    if len(args.print) == 0:
        snapshot.print()
    else:
        snapshot.print(args.print)

if len(sys.argv) == 1:
    snapshot.take_snapshot()
    snapshot.print()
