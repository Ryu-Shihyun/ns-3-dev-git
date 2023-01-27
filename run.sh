#!/bin/bash

./ns3 run "MBTA --mcs=8 --nStations=100 --enableBsrp=true --simulationTime=20 --bitRateVariable=7500 --payloadSize=500  --option="UONRA_packet500v2""> ./scratch/MBTA/log_sta40_slot0_sim20_rate30M_UONRA_packet500v2.txt
./ns3 run "MBTA --mcs=8 --nStations=100 --enableBsrp=true --simulationTime=20 --bitRateVariable=250 --payloadSize=500  --option="UONRA_packet500v2""> ./scratch/MBTA/log_sta40_slot0_sim20_rate1M_UONRA_pakcet500v2.txt
#./ns3 run "MBTA --mcs=8 --nStations=70 --enableBsrp=true --simulationTime=20 --bitRateVariable=7500 --payloadSize=500  --option="UONRA_RP""> ./scratch/MBTA/log_sta70_slot0_sim20_rate30M_UONRA_RP.txt
#./ns3 run "MBTA --mcs=8 --nStations=70 --enableBsrp=true --simulationTime=20 --bitRateVariable=250 --payloadSize=500  --option="UONRA_RP""> ./scratch/MBTA/log_sta70_slot0_sim20_rate1M_UONRA_RP.txt
