/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2016 SEBASTIEN DERONNE
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Sebastien Deronne <sebastien.deronne@gmail.com>
 */

#include "ns3/command-line.h"
#include "ns3/config.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/string.h"
#include "ns3/enum.h"
#include "ns3/log.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/spectrum-wifi-helper.h"
#include "ns3/ssid.h"
#include "ns3/mobility-helper.h"
#include "ns3/internet-stack-helper.h"
#include "ns3/ipv4-address-helper.h"
#include "ns3/udp-client-server-helper.h"
#include "ns3/packet-sink-helper.h"
#include "ns3/on-off-helper.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/packet-sink.h"
#include "ns3/yans-wifi-channel.h"
#include "ns3/multi-model-spectrum-channel.h"
#include "ns3/wifi-acknowledgment.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/random-variable-stream.h"
#include "ns3/multi-user-scheduler.h"
#include <unistd.h>
#include "ns3/rr-multi-user-scheduler.h"
#include <string>

// This is a simple example in order to show how to configure an IEEE 802.11ax Wi-Fi network.
//
// It outputs the UDP or TCP goodput for every HE MCS value, which depends on the MCS value (0 to 11),
// the channel width (20, 40, 80 or 160 MHz) and the guard interval (800ns, 1600ns or 3200ns).
// The PHY bitrate is constant over all the simulation run. The user can also specify the distance between
// the access point and the station: the larger the distance the smaller the goodput.
//
// The simulation assumes a configurable number of stations in an infrastructure network:
//
//  STA     AP
//    *     *
//    |     |
//   n1     n2
//
// Packets in this simulation belong to BestEffort Access Class (AC_BE).
// By selecting an acknowledgment sequence for DL MU PPDUs, it is possible to aggregate a
// Round Robin scheduler to the AP, so that DL MU PPDUs are sent by the AP via DL OFDMA.

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("he-wifi-network");

static void PrintRxByte(uint64_t *prevbyte, Ptr<PacketSink> packetsink, uint32_t payloadSize);
static void PrintProgress(int value)
{
  std::cerr << "[" << value << "/100]\r" << std::flush;
}

int main (int argc, char *argv[])
{
  bool udp {false};
  bool useRts {false};
  bool useExtendedBlockAck {false};
  double simulationTime {30}; //seconds
  double distance {1.0}; //meters
  double downlink{false};
  double frequency {5}; //whether 2.4, 5 or 6 GHz
  std::size_t nStations {1};
  std::string dlAckSeqType {"MU-BAR"};
  bool enableUlOfdma {true};
  bool enableBsrp {true};
  int mcs {-1}; // -1 indicates an unset value
  uint32_t payloadSize = 500; // must fit in the max TX duration when transmitting at MCS 0 over an RU of 26 tones
  std::string phyModel {"Spectrum"};
  double minExpectedThroughput {0};
  double maxExpectedThroughput {0};
  double maxNetworkRadius{50};
  int maxAccessDevices{18};
  int bitRateVariable{1500};
  int warmUpTime{40};
  std::string csvOption = "default";

  CommandLine cmd (__FILE__);
  cmd.AddValue ("frequency", "Whether working in the 2.4, 5 or 6 GHz band (other values gets rejected)", frequency);
  cmd.AddValue ("downlink", "Generate downlink flows if set to 1, uplink flows otherwise", downlink);
  cmd.AddValue ("distance", "Distance in meters between the station and the access point", distance);
  cmd.AddValue ("simulationTime", "Simulation time in seconds", simulationTime);
  cmd.AddValue ("udp", "UDP if set to 1, TCP otherwise", udp);
  cmd.AddValue ("useRts", "Enable/disable RTS/CTS", useRts);
  cmd.AddValue ("useExtendedBlockAck", "Enable/disable use of extended BACK", useExtendedBlockAck);
  cmd.AddValue ("nStations", "Number of non-AP HE stations", nStations);
  cmd.AddValue ("dlAckType", "Ack sequence type for DL OFDMA (NO-OFDMA, ACK-SU-FORMAT, MU-BAR, AGGR-MU-BAR)",
                dlAckSeqType);
  cmd.AddValue ("enableUlOfdma", "Enable UL OFDMA (useful if DL OFDMA is enabled and TCP is used)", enableUlOfdma);
  cmd.AddValue ("enableBsrp", "Enable BSRP (useful if DL and UL OFDMA are enabled and TCP is used)", enableBsrp);
  cmd.AddValue ("mcs", "if set, limit testing to a specific MCS (0-11)", mcs);
  cmd.AddValue ("payloadSize", "The application payload size in bytes", payloadSize);
  cmd.AddValue ("phyModel", "PHY model to use when OFDMA is disabled (Yans or Spectrum). If OFDMA is enabled then Spectrum is automatically selected", phyModel);
  cmd.AddValue ("minExpectedThroughput", "if set, simulation fails if the lowest throughput is below this value", minExpectedThroughput);
  cmd.AddValue ("maxExpectedThroughput", "if set, simulation fails if the highest throughput is above this value", maxExpectedThroughput);
  cmd.AddValue ("maxAccessDevices", "the maximum number of stations taht can be granted an RU", maxAccessDevices);
  cmd.AddValue ("bitRateVariable", "the maximum number of stations taht can be granted an RU", bitRateVariable);
  cmd.AddValue("warmUpTime","Set the time when clients start transmission. It is for Association time",warmUpTime);
  cmd.AddValue ("option","Set csv file name",csvOption);
  
  cmd.Parse (argc,argv);

  if (useRts)
    {
      Config::SetDefault ("ns3::WifiRemoteStationManager::RtsCtsThreshold", StringValue ("0"));
    }

  if (dlAckSeqType == "ACK-SU-FORMAT")
    {
      Config::SetDefault ("ns3::WifiDefaultAckManager::DlMuAckSequenceType",
                          EnumValue (WifiAcknowledgment::DL_MU_BAR_BA_SEQUENCE));
    }
  else if (dlAckSeqType == "MU-BAR")
    {
      Config::SetDefault ("ns3::WifiDefaultAckManager::DlMuAckSequenceType",
                          EnumValue (WifiAcknowledgment::DL_MU_TF_MU_BAR));
    }
  else if (dlAckSeqType == "AGGR-MU-BAR")
    {
      Config::SetDefault ("ns3::WifiDefaultAckManager::DlMuAckSequenceType",
                          EnumValue (WifiAcknowledgment::DL_MU_AGGREGATE_TF));
    }
  else if (dlAckSeqType != "NO-OFDMA")
    {
      NS_ABORT_MSG ("Invalid DL ack sequence type (must be NO-OFDMA, ACK-SU-FORMAT, MU-BAR or AGGR-MU-BAR)");
    }

  if (phyModel != "Yans" && phyModel != "Spectrum")
    {
      NS_ABORT_MSG ("Invalid PHY model (must be Yans or Spectrum)");
    }
  if (dlAckSeqType != "NO-OFDMA")
    {
      // SpectrumWifiPhy is required for OFDMA
      phyModel = "Spectrum";
    }

  double prevThroughput [150];
  for (uint32_t l = 0; l < 150; l++)
    {
      prevThroughput[l] = 0;
    }
  
  // std::cout << "NODE" << "\t\t" << "pos.x" <<"\t\t" << "pos.y" << "\n";

  // Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
  // Ptr<UniformDiscPositionAllocator> discStaPos = CreateObject<UniformDiscPositionAllocator>();
  // //AP's position.
  // positionAlloc->Add (Vector (0.0, 0.0, 0.0));
  // std::cout << "AP" << "\t\t" << "0.0" <<"\t\t" << "0.0" << "\n";

  // //STA`s position. add by ryu.
  // Ptr<UniformRandomVariable> rand = CreateObject<UniformRandomVariable> (); //ランダム値を生成
  // for(int i=0;i<nStations;i++){
  //   discStaPos->SetRho(rand->GetValue(0.0,maxNetworkRadius));
  //   discStaPos->SetZ(0.0);
  //   Vector v = discStaPos->GetNext();
  //   positionAlloc->Add (v);
  //   std::cout << "STA" << i << "\t\t" << v.x <<"\t\t" << v.y << "\n";
  // }

  //modify maxAccessDevices;
  warmUpTime = (nStations+4)/5;
  maxAccessDevices = nStations;
  std::string ulName =(enableBsrp) ? "UONRA" : "UORA";
  std::stringstream csvName;
  csvName << "Sta" << nStations << "_Warm" << warmUpTime << "_Sim" << simulationTime <<"_Rate" << payloadSize*8*bitRateVariable/1000000 << "M_payload" << payloadSize << "_" << ulName; 
  std::string fileName = "./data/"+csvName.str() + "_"+csvOption+".csv";
  
  std::ofstream ofs(fileName);
  ofs << "index,IP Address,candidate,Success Receive to AP, total Packet Size,Average Duration of Transmission" << std::endl;
  std::cout << "Number of Station" << "\t\t" <<"MCS value" << "\t\t" << "Channel width" << "\t\t" << "GI" << "\t\t\t" << "Throughput" << '\n';
  int minMcs = 0;
  int maxMcs = 11;
  if (mcs >= 0 && mcs <= 11)
    {
      minMcs = mcs;
      maxMcs = mcs;
    }
  for (int mcs = minMcs; mcs <= maxMcs; mcs++)
    {
      uint8_t index = 0;
      double previous = 0;
      uint8_t maxChannelWidth = frequency == 2.4 ? 40 : 160;
      uint8_t maxChannelWidthTest = 80;
      sleep(2);
      Ptr<RrMultiUserScheduler> rrMuUser = nullptr;
      // for (int nSta=50; nSta<=200; nSta++) //MHz
      //   {
          int channelWidth  =80;
          int gi = 3200;
              if (!udp)
                {
                  Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (payloadSize));
                }

              NodeContainer wifiStaNodes;
              wifiStaNodes.Create (nStations);
              NodeContainer wifiApNode;
              wifiApNode.Create (1);

              NetDeviceContainer apDevice, staDevices;
              WifiMacHelper mac;
              WifiHelper wifi;
              std::string channelStr ("{0, " + std::to_string (channelWidth) + ", ");

              if (frequency == 6)
                {
                  wifi.SetStandard (WIFI_STANDARD_80211ax);
                  channelStr += "BAND_6GHZ, 0}";
                  Config::SetDefault ("ns3::LogDistancePropagationLossModel::ReferenceLoss", DoubleValue (48));
                }
              else if (frequency == 5)
                {
                  wifi.SetStandard (WIFI_STANDARD_80211ax);
                  channelStr += "BAND_5GHZ, 0}";
                }
              else if (frequency == 2.4)
                {
                  wifi.SetStandard (WIFI_STANDARD_80211ax);
                  channelStr += "BAND_2_4GHZ, 0}";
                  Config::SetDefault ("ns3::LogDistancePropagationLossModel::ReferenceLoss", DoubleValue (40));
                }
              else
                {
                  std::cout << "Wrong frequency value!" << std::endl;
                  return 0;
                }

              std::ostringstream oss;
              oss << "HeMcs" << mcs;
              wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager","DataMode", StringValue (oss.str ()),
                                            "ControlMode", StringValue (oss.str ()));

              Ssid ssid = Ssid ("ns3-80211ax");
              if (phyModel == "Spectrum")
                {
                  /*
                  * SingleModelSpectrumChannel cannot be used with 802.11ax because two
                  * spectrum models are required: one with 78.125 kHz bands for HE PPDUs
                  * and one with 312.5 kHz bands for, e.g., non-HT PPDUs (for more details,
                  * see issue #408 (CLOSED))
                  */
                  Ptr<MultiModelSpectrumChannel> spectrumChannel = CreateObject<MultiModelSpectrumChannel> ();
                  SpectrumWifiPhyHelper phy;
                  phy.SetPcapDataLinkType (WifiPhyHelper::DLT_IEEE802_11_RADIO);
                  phy.SetChannel (spectrumChannel);

                  mac.SetType ("ns3::StaWifiMac",
                              "Ssid", SsidValue (ssid));
                  phy.Set ("ChannelSettings", StringValue (channelStr));
                  staDevices = wifi.Install (phy, mac, wifiStaNodes);

                  if (dlAckSeqType != "NO-OFDMA")
                    {
                      mac.SetMultiUserScheduler ("ns3::RrMultiUserScheduler",
                                                "EnableUlOfdma", BooleanValue (enableUlOfdma),
                                                "EnableBsrp", BooleanValue (enableBsrp),
                                                "NStations",UintegerValue(nStations));
                      
                    }
                  mac.SetType ("ns3::ApWifiMac",
                              "EnableBeaconJitter", BooleanValue (false),
                              "Ssid", SsidValue (ssid));
                  
                // mac.SetAckManager ("ns3::WifiDefaultAckManager", "DlMuAckSequenceType", EnumValue (m_dlMuAckType));
                  apDevice = wifi.Install (phy, mac, wifiApNode);
                }
              else
                {
                  YansWifiChannelHelper channel = YansWifiChannelHelper::Default ();
                  YansWifiPhyHelper phy;
                  phy.SetPcapDataLinkType (WifiPhyHelper::DLT_IEEE802_11_RADIO);
                  phy.SetChannel (channel.Create ());

                  mac.SetType ("ns3::StaWifiMac",
                              "Ssid", SsidValue (ssid));
                  phy.Set ("ChannelSettings", StringValue (channelStr));
                  staDevices = wifi.Install (phy, mac, wifiStaNodes);

                  mac.SetType ("ns3::ApWifiMac",
                              "EnableBeaconJitter", BooleanValue (false),
                              "Ssid", SsidValue (ssid));
                  apDevice = wifi.Install (phy, mac, wifiApNode);
                }

              RngSeedManager::SetSeed (1);
              RngSeedManager::SetRun (1);
              int64_t streamNumber = 100;
              streamNumber += wifi.AssignStreams (apDevice, streamNumber);
              streamNumber += wifi.AssignStreams (staDevices, streamNumber);

              // Set guard interval and MPDU buffer size
              Config::Set ("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/HeConfiguration/GuardInterval", TimeValue (NanoSeconds (gi)));
              Config::Set ("/NodeList/*/DeviceList/*/$ns3::WifiNetDevice/HeConfiguration/MpduBufferSize", UintegerValue (useExtendedBlockAck ? 256 : 64));

              // mobility.
              MobilityHelper mobility;
            Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
            Ptr<UniformDiscPositionAllocator> discStaPos = CreateObject<UniformDiscPositionAllocator>();
            //AP's position.
            positionAlloc->Add (Vector (0.0, 0.0, 0.0));
            // std::cout << "AP" << "\t\t" << "0.0" <<"\t\t" << "0.0" << "\n";

            //STA`s position. add by ryu.
            Ptr<UniformRandomVariable> rand = CreateObject<UniformRandomVariable> (); //ランダム値を生成
            // std::cout << "number of STA: " << nStations << std::endl;
            for(int i=0;i<nStations;i++){
              discStaPos->SetRho(rand->GetValue(0.0,maxNetworkRadius));
              discStaPos->SetZ(0.0);
              Vector v = discStaPos->GetNext();
              positionAlloc->Add (v);
              std::cout << "STA" << i << "\t\t" << v.x <<"\t\t" << v.y << "\n";
            }
              
              mobility.SetPositionAllocator (positionAlloc);

              mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");

              mobility.Install (wifiApNode);
              mobility.Install (wifiStaNodes);

              /* Internet stack*/
              InternetStackHelper stack;
              stack.Install (wifiApNode);
              stack.Install (wifiStaNodes);

              Ipv4AddressHelper address;
              address.SetBase ("192.168.1.0", "255.255.255.0");
              Ipv4InterfaceContainer staNodeInterfaces;
              Ipv4InterfaceContainer apNodeInterface;

              staNodeInterfaces = address.Assign (staDevices);
              apNodeInterface = address.Assign (apDevice);

              /* Setting applications */
              //uplink: client:STA, server:AP,
              //udp/tcpsocketFactory , AP address
              ApplicationContainer serverApp;
                auto serverNodes = downlink ? std::ref(wifiStaNodes) : std::ref(wifiApNode);
                Ipv4InterfaceContainer serverInterfaces;
                NodeContainer clientNodes;
                for (std::size_t i = 0; i < nStations; i++)
                {
                    serverInterfaces.Add(downlink ? staNodeInterfaces.Get(i)
                                                  : apNodeInterface.Get(0));
                    clientNodes.Add(downlink ? wifiApNode.Get(0) : wifiStaNodes.Get(i));
                }
 
                if (udp)
                {
                    // UDP flow
                    uint16_t port = 9;
                    UdpServerHelper server(port);
                    serverApp = server.Install(serverNodes.get());
                    serverApp.Start(Seconds(0.0));
                    serverApp.Stop(Seconds(simulationTime + warmUpTime));
 
                    for (std::size_t i = 0; i < nStations; i++)
                    {
                        UdpClientHelper client(serverInterfaces.GetAddress(i), port);
                        client.SetAttribute("MaxPackets", UintegerValue(4294967295U));
                        client.SetAttribute("Interval", TimeValue(Time("0.00001"))); // packets/s
                        client.SetAttribute("PacketSize", UintegerValue(payloadSize));
                        ApplicationContainer clientApp = client.Install(clientNodes.Get(i));
                        clientApp.Start(Seconds(warmUpTime));
                        clientApp.Stop(Seconds(simulationTime + warmUpTime));
                    }
                }
                else
                {
                    // TCP flow
                    uint16_t port = 50000;
                    Address localAddress(InetSocketAddress(Ipv4Address::GetAny(), port));
                    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory", localAddress);
                    serverApp = packetSinkHelper.Install(serverNodes.get());
                    serverApp.Start(Seconds(0.0));
                    serverApp.Stop(Seconds(simulationTime + warmUpTime));
 
                    for (std::size_t i = 0; i < nStations; i++)
                    {
                        OnOffHelper onoff("ns3::TcpSocketFactory", Ipv4Address::GetAny());
                        onoff.SetAttribute("OnTime",
                                           StringValue("ns3::ConstantRandomVariable[Constant=1]"));
                        onoff.SetAttribute("OffTime",
                                           StringValue("ns3::ConstantRandomVariable[Constant=0]"));
                        onoff.SetAttribute("PacketSize", UintegerValue(payloadSize));
                        onoff.SetAttribute("DataRate", DataRateValue(payloadSize*8*bitRateVariable)); // bit/s
                        AddressValue remoteAddress(
                            InetSocketAddress(serverInterfaces.GetAddress(i), port));
                        onoff.SetAttribute("Remote", remoteAddress);
                        ApplicationContainer clientApp = onoff.Install(clientNodes.Get(i));
                        clientApp.Start(Seconds(warmUpTime));
                        clientApp.Stop(Seconds(simulationTime + warmUpTime));
                    }
                }
            
            std::cout << "AP adress" << "\t\t" << apNodeInterface.GetAddress(0,0) << std::endl;
            for(int i=0;i<nStations;i++){
              std::cout << "STA"<< i <<" adress" << "\t\t" << staNodeInterfaces.GetAddress(i,0) << std::endl;  
            }
            // for(auto sta_ptr = staDevices.Begin();sta_ptr!=staDevices.End();sta_ptr++){
            //   std::cout << "Sta::Address: " << sta_ptr->GetAddress() << ". ID: " << sta_ptr->GetNode()->GetId() << std::endl;
            // }
            
            // for (int i=0; i<wifiStaNodes.GetN(); i++) {
            //   std::cout << "ID: " << wifiStaNodes.Get(i)->GetId() << ", mac address: " << wifiStaNodes.Get(i)->GetDevice(0)->GetAddress() << std::endl;
            // }

              Simulator::Schedule (Seconds (0), &Ipv4GlobalRoutingHelper::PopulateRoutingTables);

              uint64_t rByte=0;
              for(int i=1;i<(simulationTime+warmUpTime)/10;i++){
                Simulator::Schedule (Seconds(i*10),&PrintRxByte,&rByte,DynamicCast<PacketSink> (serverApp.Get (0)),payloadSize);
                // rByte = 8*payloadSize * DynamicCast<PacketSink> (serverApp.Get (0))->GetTotalRx ();
              }
              for(int i=0 ; i<101 ; i++)
              {
                Simulator::Schedule (Seconds((simulationTime+warmUpTime)/100*i),&PrintProgress,i);
              }

              Simulator::Stop (Seconds (simulationTime + warmUpTime));
              Simulator::Run ();

              // When multiple stations are used, there are chances that association requests collide
              // and hence the throughput may be lower than expected. Therefore, we relax the check
              // that the throughput cannot decrease by introducing a scaling factor (or tolerance)
              double tolerance = 0.10;
              uint64_t rxBytes = 0;
              typedef std::pair<Ipv4Address, Time> averageDelay;
              std::vector <averageDelay> v;
              if (udp)
                {
                  for (uint32_t i = 0; i < serverApp.GetN (); i++)
                    {
                      rxBytes += payloadSize * DynamicCast<UdpServer> (serverApp.Get (i))->GetReceived ();
                    }
                }
              else
                {
                  for (uint32_t i = 0; i < serverApp.GetN (); i++)
                    {
                      // ofs << i << "," << DynamicCast<PacketSink> (serverApp.Get (i))->GetTotalRx ()*8 << "," <<payloadSize << "," << DynamicCast<PacketSink> (serverApp.Get (i))->GetReceiveCount() << std::endl;
                      rxBytes += DynamicCast<PacketSink> (serverApp.Get (i))->GetTotalRx ();
                      // for(int staIndex=0;staIndex<wifiStaNodes.GetN();staIndex++){
                      //   auto addr = staNodeInterfaces.GetAddress(staIndex,0);
                      //   std::cout << "sta"<< staIndex << "'s address:" << addr << ". delay:" << DynamicCast<PacketSink> (serverApp.Get (i))->GetAverageDelay(addr) << std::endl;
                      //   v.push_back({addr,DynamicCast<PacketSink> (serverApp.Get (i))->GetAverageDelay(addr)});
                      // }
                      
                    }
                }
              double throughput = (rxBytes * 8) / (simulationTime * 1000000.0); //Mbit/s
              int basicNum = mac.GetUplinkNum(0);
              int bsrpNum = mac.GetUplinkNum(1);
              int conflictStaNum = mac.GetConflictNum();
              int maxCandidates = mac.GetMaxCandidatesNum();
            for (int i=0; i<wifiStaNodes.GetN(); i++) {
              Address addr = wifiStaNodes.Get(i)->GetDevice(0)->GetAddress();
              auto candidateInfo = mac.GetCandidateInfo(Mac48Address::ConvertFrom(addr));
              auto ipAddr= staNodeInterfaces.GetAddress(i,0);
              // auto v_ptr = std::find_if(v.begin(),v.end(),
              //                     [&ipAddr] (averageDelay pair)
              //               { return pair.first == ipAddr; });
              // std::cout << ipAddr << ((v_ptr!=v.end()) ? "true" : "false") << std::endl;
              ofs << i << "," <<  ipAddr << "," <<  candidateInfo.at(0)<< "," <<  candidateInfo.at(1)<< "," <<  candidateInfo.at(2) /*<<"," << v_ptr->second */<< std::endl;
            }
              Simulator::Destroy ();
              // std::cout << "Number of Station" << "\t\t" <<"MCS value" << "\t\t" << "Channel width" << "\t\t" << "GI" << "\t\t\t" << "Throughput" << '\n'; 
              std::cout << nStations << "\t\t" <<mcs << "\t\t" << channelWidth << "\t\t" << gi << "\t\t" << throughput  << "\t\t" << basicNum << "\t\t" << bsrpNum << "\t\t" << conflictStaNum << "\t\t"  << maxCandidates << std::endl;
              // std::cout << nStations << "\t\t" <<mcs << "\t\t" << channelWidth << "\t\t" << gi << "\t\t" << throughput   << std::endl;

              //test first element
              if (mcs == 0 && channelWidth == 20 && gi == 3200)
                {
                  if (throughput * (1 + tolerance) < minExpectedThroughput)
                    {
                      NS_LOG_ERROR ("Obtained throughput " << throughput << " is not expected!");
                      exit (1);
                    }
                }
              //test last element
              if (mcs == 11 && channelWidth == 160 && gi == 800)
                {
                  if (maxExpectedThroughput > 0 && throughput > maxExpectedThroughput * (1 + tolerance))
                    {
                      NS_LOG_ERROR ("Obtained throughput " << throughput << " is not expected!");
                      exit (1);
                    }
                }
              //test previous throughput is smaller (for the same mcs)
              if (throughput * (1 + tolerance) > previous)
                {
                  previous = throughput;
                }
              else if (throughput > 0)
                {
                  NS_LOG_ERROR ("Obtained throughput " << throughput << " is not expected!");
                  exit (1);
                }
              //test previous throughput is smaller (for the same channel width and GI)
              if (throughput * (1 + tolerance) > prevThroughput [index])
                {
                  prevThroughput [index] = throughput;
                }
              else if (throughput > 0)
                {
                  NS_LOG_ERROR ("Obtained throughput " << throughput << " is not expected!");
                  exit (1);
                }
              index++;
              // Simulator::Destroy();
              // gi /= 2;
          // }
         
      }
    
  
  return 0;
}

static void PrintRxByte(uint64_t *prevbyte, Ptr<PacketSink> packetSink, uint32_t payloadSize)
{
  uint64_t nowbyte = packetSink->GetTotalRx ();
  std::cout << "PrintRxByte..." << Simulator::Now() << ". byte = " << (nowbyte-*prevbyte) << std::endl;
  *prevbyte = nowbyte;
}
