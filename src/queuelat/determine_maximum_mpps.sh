#!/bin/bash

#  A script to determine the maximum mpps. Logic:
#  Increase mpps in 0.5 units 
# 
# NOTE: please set "PREAMBLE" to the command line you use for 
# 
PREAMBLE="taskset -c 2 chrt -f 1"
MAXLAT="20000"
CYCLES_PER_PACKET="300"
OUTFILE=/usr/tmp/outfile

echo "Determining maximum mpps the machine can handle"
echo "Will take a few minutes to determine mpps value"
echo "And 10 minutes run to confirm the final mpps value is stable"

for mpps in `seq 3 3 50`; do
	echo testing $mpps Mpps

	OUTFILE=`mktemp`
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f `sh get_cpuinfo_mhz.sh` -p "$mpps" -t 30 > $OUTFILE

	exceeded=`grep exceeded $OUTFILE`
	if [ ! -z "$exceeded" ]; then
		echo mpps failed: $mpps
		break;
	fi
	echo success
done
echo first loop mpps: $mpps

first_mpps=$(($mpps - 1))
for mpps in `seq $first_mpps -1 3`; do
	echo testing $mpps Mpps

	OUTFILE=`mktemp`
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f `sh get_cpuinfo_mhz.sh` -p "$mpps" -t 30 > $OUTFILE

	exceeded=`grep exceeded $OUTFILE`
	if [ -z "$exceeded" ]; then
		echo mpps success $mpps
		break;
	fi
	echo failure
done

second_mpps=`echo "$mpps + 0.3" | bc`
echo second loop mpps: $mpps

for mpps in `seq $second_mpps 0.3 $first_mpps`; do
	echo testing $mpps Mpps

	OUTFILE=`mktemp`
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f `sh get_cpuinfo_mhz.sh` -p "$mpps" -t 30 > $OUTFILE

	exceeded=`grep exceeded $OUTFILE`
	if [ ! -z "$exceeded" ]; then
		echo mpps failure $mpps
		break;
	fi
	echo success
done

echo third loop mpps: $mpps
third_mpps=`echo "$mpps -0.1" | bc`

for mpps in `seq $third_mpps -0.1 3`; do
	echo testing $mpps Mpps

	OUTFILE=`mktemp`
	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f `sh get_cpuinfo_mhz.sh` -p "$mpps" -t 30 > $OUTFILE

	exceeded=`grep exceeded $OUTFILE`
	if [ -z "$exceeded" ]; then
		echo mpps success $mpps
		break;
	fi
	echo failure
done

export queuelat_failure=1
while [ $queuelat_failure == 1 ]; do

	export queuelat_failure=0

	echo -n "Starting 10 runs of 30 seconds with "
	echo "$mpps Mpps"

	for i in `seq 1 10`; do 
		$PREAMBLE ./queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f `get_cpuinfo_mhz.sh` -p "$mpps" -t 30 > $OUTFILE
		exceeded=`grep exceeded $OUTFILE`

		if [ ! -z "$exceeded" ]; then
			echo "mpps failure (run $i) $mpps"
			export queuelat_failure=1
			mpps=`echo $mpps - 0.1 | bc`
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

	$PREAMBLE queuelat -m $MAXLAT -c $CYCLES_PER_PACKET -f `get_cpuinfo_mhz.sh` -p "$mpps" -t 600 > $OUTFILE
	exceeded=`grep exceeded $OUTFILE`

	if [ ! -z "$exceeded" ]; then
		echo "mpps failure (run $i) $mpps"
		export queuelat_failure=1
		export mpps=`echo $mpps - 0.1 | bc`
		continue
	fi
	echo "run $i success"
done

echo Final mpps is: $mpps

unset queuelat_failure
unset mpps
