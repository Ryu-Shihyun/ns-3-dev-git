#!/bin/bash

./ns3 run "MBTA --mcs=8 --nStations=70 --enableBsrp=false --simulationTime=20 --bitRateVariable=7500 --payloadSize=500  --option="UORA_RP""> ./scratch/MBTA/log_sta70_slot0_sim20_rate30M_UORA_RP.txt
./ns3 run "MBTA --mcs=8 --nStations=70 --enableBsrp=false --simulationTime=20 --bitRateVariable=250 --payloadSize=500  --option="UORA_RP""> ./scratch/MBTA/log_sta70_slot0_sim20_rate1M_UORA_RP.txt
./ns3 run "MBTA --mcs=8 --nStations=70 --enableBsrp=true --simulationTime=20 --bitRateVariable=7500 --payloadSize=500  --option="UONRA_RP""> ./scratch/MBTA/log_sta70_slot0_sim20_rate30M_UONRA_RP.txt
./ns3 run "MBTA --mcs=8 --nStations=70 --enableBsrp=true --simulationTime=20 --bitRateVariable=250 --payloadSize=500  --option="UONRA_RP""> ./scratch/MBTA/log_sta70_slot0_sim20_rate1M_UONRA_RP.txt
