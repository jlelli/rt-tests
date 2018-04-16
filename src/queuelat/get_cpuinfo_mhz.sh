#!/bin/bash

mhz=`cat /proc/cpuinfo  | grep "cpu MHz" | uniq | cut -f 3 -d " "`
echo $mhz

