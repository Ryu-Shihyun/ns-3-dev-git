#!/bin/bash

for i in `seq $1 $2`
do
    if [ $((${i} % 10)) -eq 0 ];then
        ./ns3 run "MBTA --mcs=8 --nStations=${i} --enableBsrp=true"
    fi
done