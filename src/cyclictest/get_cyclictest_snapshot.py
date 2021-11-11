#!/usr/bin/env python3
""" Program to get a snapshot of a running instance of cyclictest """

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2020 John Kacur <jkacur@redhat.com>

import subprocess
import argparse
import re
import glob
import sys

parser = argparse.ArgumentParser(description='Get a snapshot of running instances of cyclictest')
parser.add_argument('-l', '--list', action='store_true', help='list the main pid(s) of running instances of cyclictest')
parser.add_argument('-s', '--snapshot', nargs='*', metavar='pid', help='take a snapshot of running instances of cyclictest')
parser.add_argument('-p', '--print', nargs='*', metavar='pid', help='print the snapshots')
args = parser.parse_args()

class Snapshot:
    """ Class for getting a snapshot of a running cyclictest instance """

    warned = False

    @classmethod
    def print_warning(cls):
        """ print a warning one time only even if called multiple times """
        if not cls.warned:
            cls.warned = True
            print("No cyclictest instance found")

    def __init__(self):
        self.pids = []
        self.shm_files = []
        self.refresh()

    def refresh(self):
        """ Create a list of running cyclictest instances. """
        self.pids = []
        self.shm_files = glob.glob('/dev/shm/cyclictest*')
        self.shm_files.sort()
        for shm_file in self.shm_files:
            pid = re.search('[0-9]*$', shm_file).group()
            self.pids += [pid]

    def take_snapshot(self, spids=None):
        """ Send USR2 to all running instances of cyclictest,
            or just to a specific pid (spids) if specified. """
        if spids is None:
            if not self.pids:
                Snapshot.print_warning()
            for pid in self.pids:
                subprocess.run(["kill", "-s", "USR2", pid])
        else:
            for pid in spids:
                subprocess.run(["kill", "-s", "USR2", pid])

    def print_pids(self):
        """ Print the list of pids of running cyclictest instances. """
        if not self.pids:
            Snapshot.print_warning()
        for pid in self.pids:
            print(pid)

    def print(self, spids=None):
        """ Print the data in /dev/shm/cyclictest* """
        if spids is None:
            if not self.shm_files:
                Snapshot.print_warning()
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
                else:
                    Snapshot.print_warning()

snapshot = Snapshot()

if args.list:
    snapshot.print_pids()

if args.snapshot is not None:
    if args.snapshot:
        snapshot.take_snapshot(args.snapshot)
    else:
        snapshot.take_snapshot()

if args.print is not None:
    if args.print:
        snapshot.print(args.print)
    else:
        snapshot.print()

if len(sys.argv) == 1:
    snapshot.take_snapshot()
    snapshot.print()
