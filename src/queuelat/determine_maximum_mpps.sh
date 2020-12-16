#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2018 Marcelo Tosatti <mtosatti@redhat.com>

#  A script to determine the maximum mpps. Logic:
#  Increase mpps in 0.5 units 
# 
MAXLAT="20000"
CYCLES_PER_PACKET="300"
OUTFILE=/usr/tmp/outfile
PRIO=1
CPULIST=0
SCHED=""

usage()
{
	echo "Usage:"
	echo "$(basename $0) [OPTIONS]"
	echo
	echo "-a cpulist"
	echo "	List of processors to run on. The default is processor 0"
	echo "	Numbers are separated by commas and may include ranges. Eg. 0,3,7-11"
	echo "-m maxlat"
	echo "	maximum latency in nanoseconds. The default is 20000"
	echo "	if the maximum is exceeded, that run of queuelat quits"
	echo "-n cycles"
	echo "	Estimated number of cycles it takes to process one packet"
	echo "	The default is 300"
	echo "-f"
	echo "	Set the scheduling policy to SCHED_FIFO."
	echo "	This is the default if not specified"
	echo "-r"
	echo "	Set the scheduling policy to SCHED_RR".
	echo "-p priority"
	echo "	default priority = 1. Valid numbers are from 1 to 99"
	echo "-h"
	echo "	help"
	echo "	print this help message and exit"
	exit
}

get_cpuinfo_mhz()
{
	grep "cpu MHz" /proc/cpuinfo | cut -f 3 -d " " | sort -rn | head -n1
}

# Check that the scheduling policy hasn't already been set
# Exit with an error message if it has
check_sched()
{
	if [ "${SCHED}" != "" ]; then
		echo "Specify -f or -r, but not both"
		usage
	fi
}

# Process command line options
while getopts ":a:frp:m:n:h" opt; do
	case ${opt} in
		a ) CPULIST="${OPTARG}" ;;
		m ) MAXLAT="${OPTARG}" ;;
		n ) CYCLES_PER_PACKET="${OPTARG}" ;;
		f ) check_sched; SCHED="-f" ;;
		r ) check_sched; SCHED="-r" ;;
		p ) PRIO="${OPTARG}" ;;
		h ) usage ;;
		* ) echo "no such option"; usage ;;
	esac
done

shift $((OPTIND -1 ))

# If the user hasn't specified a scheduling policy
# then set it to the default SCHED_FIFO
if [ "${SCHED}" == "" ]; then
	SCHED="-f"
fi

# Error checking that the user entered a priority between 1 and 99
if [[ "${PRIO}" -lt "1" ]] || [[ "${PRIO}" -gt "99" ]]; then
	echo "PRIO must be a number between 1 and 99"
	usage
fi

PREAMBLE="taskset -c ${CPULIST} chrt ${SCHED} ${PRIO}"

echo "Determining maximum mpps the machine can handle"
echo "Will take a few minutes to determine mpps value"
echo "And 10 minutes run to confirm the final mpps value is stable"

for mpps in $(seq 3 3 50); do
	echo testing "$mpps" Mpps

	OUTFILE=$(mktemp)
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f "$(get_cpuinfo_mhz)" -p "$mpps" -t 30 > "$OUTFILE"

	exceeded=$(grep exceeded "$OUTFILE")
	if [ ! -z "$exceeded" ]; then
		echo mpps failed: "$mpps"
		break;
	fi
	echo success
done
echo first loop mpps: "$mpps"

first_mpps=$(($mpps - 1))
for mpps in $(seq $first_mpps -1 3); do
	echo testing "$mpps" Mpps

	OUTFILE=$(mktemp)
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f "$(get_cpuinfo_mhz)" -p "$mpps" -t 30 > "$OUTFILE"

	exceeded=$(grep exceeded "$OUTFILE")
	if [ -z "$exceeded" ]; then
		echo mpps success "$mpps"
		break;
	fi
	echo failure
done

second_mpps=$(echo "$mpps + 0.3" | bc)
echo second loop mpps: "$mpps"

for mpps in $(seq "$second_mpps" 0.3 $first_mpps); do
	echo testing "$mpps" Mpps

	OUTFILE=$(mktemp)
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f "$(get_cpuinfo_mhz)" -p "$mpps" -t 30 > "$OUTFILE"

	exceeded=$(grep exceeded "$OUTFILE")
	if [ ! -z "$exceeded" ]; then
		echo mpps failure "$mpps"
		break;
	fi
	echo success
done

echo third loop mpps: "$mpps"
third_mpps=$(echo "$mpps -0.1" | bc)

for mpps in $(seq "$third_mpps" -0.1 3); do
	echo testing "$mpps" Mpps

	OUTFILE=$(mktemp)
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f "$(get_cpuinfo_mhz)" -p "$mpps" -t 30 > "$OUTFILE"

	exceeded=$(grep exceeded "$OUTFILE")
	if [ -z "$exceeded" ]; then
		echo mpps success "$mpps"
		break;
	fi
	echo failure
done

export queuelat_failure=1
while [ $queuelat_failure == 1 ]; do

	export queuelat_failure=0

	echo -n "Starting 10 runs of 30 seconds with "
	echo "$mpps Mpps"

	for i in $(seq 1 10); do
		$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f "$(get_cpuinfo_mhz)" -p "$mpps" -t 30 > "$OUTFILE"
		exceeded=$(grep exceeded "$OUTFILE")

		if [ ! -z "$exceeded" ]; then
			echo "mpps failure (run $i) $mpps"
			export queuelat_failure=1
			mpps=$(echo "$mpps" - 0.1 | bc)
			export mpps
			break
		fi
		echo "run $i success"
	done

done

export queuelat_failure=1
while [ $queuelat_failure == 1 ]; do

	export queuelat_failure=0

	echo -n "Starting 10 minutes run with "
	echo "$mpps Mpps"

	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f "$(get_cpuinfo_mhz)" -p "$mpps" -t 600 > "$OUTFILE"
	exceeded=$(grep exceeded "$OUTFILE")

	if [ ! -z "$exceeded" ]; then
		echo "mpps failure (run $i) $mpps"
		export queuelat_failure=1
		mpps=$(echo "$mpps" - 0.1 | bc)
		export mpps
		continue
	fi
	echo "run $i success"
done

echo Final mpps is: "$mpps"

unset queuelat_failure
unset mpps
