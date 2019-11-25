#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2018 Marcelo Tosatti <mtosatti@redhat.com>

mhz=`cat /proc/cpuinfo  | grep "cpu MHz" | uniq | cut -f 3 -d " "`
echo $mhz

