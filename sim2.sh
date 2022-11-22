#!/bin/bash

for i in `seq $1 $2`
do
    
    ./ns3 run "MBTA --mcs=8 --nStations=${i} --enableBsrp=false"
    
done