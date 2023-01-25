/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2020 Universita' degli Studi di Napoli Federico II
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
 * Author: Stefano Avallone <stavallo@unina.it>
 */

#include "ns3/log.h"
#include "rr-multi-user-scheduler.h"
#include "ns3/wifi-mac-queue.h"
#include "ns3/wifi-protection.h"
#include "ns3/wifi-acknowledgment.h"
#include "ns3/wifi-psdu.h"
#include "he-frame-exchange-manager.h"
#include "he-configuration.h"
#include "he-phy.h"
#include "ns3/rng-seed-manager.h"
#include <algorithm>
#include <numeric>
#include <fstream>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("RrMultiUserScheduler");

NS_OBJECT_ENSURE_REGISTERED (RrMultiUserScheduler);

//BEGIN: My Propose
std::map<int /*staId*/, bool /*will_be_qosnull*/> m_will_be_qos_null;
std::map<int /*staId*/, int /*Buffer Status*/> m_bsr;
std::vector <int > m_bsrpList;
std::vector <int> m_zerobsr;
//END: My Propose

TypeId
RrMultiUserScheduler::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::RrMultiUserScheduler")
    .SetParent<MultiUserScheduler> ()
    .SetGroupName ("Wifi")
    .AddConstructor<RrMultiUserScheduler> ()
    .AddAttribute ("NStations",
                   "The maximum number of stations that can be granted an RU in a DL MU OFDMA transmission",
                   UintegerValue (4),
                   MakeUintegerAccessor (&RrMultiUserScheduler::m_nStations),
                  //  MakeUintegerChecker<uint8_t> (1, 74))
                   MakeUintegerChecker<uint8_t> (1, 1000))
    .AddAttribute ("EnableTxopSharing",
                   "If enabled, allow A-MPDUs of different TIDs in a DL MU PPDU.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&RrMultiUserScheduler::m_enableTxopSharing),
                   MakeBooleanChecker ())
    .AddAttribute ("ForceDlOfdma",
                   "If enabled, return DL_MU_TX even if no DL MU PPDU could be built.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&RrMultiUserScheduler::m_forceDlOfdma),
                   MakeBooleanChecker ())
    .AddAttribute ("EnableUlOfdma",
                   "If enabled, return UL_MU_TX if DL_MU_TX was returned the previous time.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&RrMultiUserScheduler::m_enableUlOfdma),
                   MakeBooleanChecker ())
    .AddAttribute ("EnableBsrp",
                   "If enabled, send a BSRP Trigger Frame before an UL MU transmission.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&RrMultiUserScheduler::m_enableBsrp),
                   MakeBooleanChecker ())
    .AddAttribute ("UlPsduSize",
                   "The default size in bytes of the solicited PSDU (to be sent in a TB PPDU)",
                  //  UintegerValue (500),//Default
                   UintegerValue (600),
                   
                   MakeUintegerAccessor (&RrMultiUserScheduler::m_ulPsduSize),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("UseCentral26TonesRus",
                   "If enabled, central 26-tone RUs are allocated, too, when the "
                   "selected RU type is at least 52 tones.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&RrMultiUserScheduler::m_useCentral26TonesRus),
                   MakeBooleanChecker ())
    .AddAttribute ("MaxCredits",
                   "Maximum amount of credits a station can have. When transmitting a DL MU PPDU, "
                   "the amount of credits received by each station equals the TX duration (in "
                   "microseconds) divided by the total number of stations. Stations that are the "
                   "recipient of the DL MU PPDU have to pay a number of credits equal to the TX "
                   "duration (in microseconds) times the allocated bandwidth share",
                   TimeValue (Seconds (1)),
                   MakeTimeAccessor (&RrMultiUserScheduler::m_maxCredits),
                   MakeTimeChecker ())
    .AddAttribute ("NQosNull",
                   "Threshold of the border number of will_be_qosnull to decide sending bsrp(proposal), "
                   "If m_will_be_qos_null has more trues than the threshold, send bsrp",
                   IntegerValue (1),
                   MakeIntegerAccessor (&RrMultiUserScheduler::m_threshold1),
                   MakeIntegerChecker<int> ())
  ;
  return tid;
}

RrMultiUserScheduler::RrMultiUserScheduler ()
{
  NS_LOG_FUNCTION (this);
}

RrMultiUserScheduler::~RrMultiUserScheduler ()
{
  NS_LOG_FUNCTION_NOARGS ();
}

void
RrMultiUserScheduler::DoInitialize (void)
{
  NS_LOG_FUNCTION (this);
  NS_ASSERT (m_apMac);
  m_apMac->TraceConnectWithoutContext ("AssociatedSta",
                                       MakeCallback (&RrMultiUserScheduler::NotifyStationAssociated, this));
  m_apMac->TraceConnectWithoutContext ("DeAssociatedSta",
                                       MakeCallback (&RrMultiUserScheduler::NotifyStationDeassociated, this));
  for (const auto& ac : wifiAcList)
    {
      m_staListDl.insert ({ac.first, {}});
    }
  for(int i=1; i<= m_nStations ; i++)
    {
      m_will_be_qos_null[i] = false;
      m_bsr[i] = 0;
    }
  MultiUserScheduler::DoInitialize ();
}

void
RrMultiUserScheduler::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_staListDl.clear ();
  m_staListUl.clear ();
  m_candidates.clear ();
  m_txParams.Clear ();
  m_apMac->TraceDisconnectWithoutContext ("AssociatedSta",
                                          MakeCallback (&RrMultiUserScheduler::NotifyStationAssociated, this));
  m_apMac->TraceDisconnectWithoutContext ("DeAssociatedSta",
                                          MakeCallback (&RrMultiUserScheduler::NotifyStationDeassociated, this));
  MultiUserScheduler::DoDispose ();
}

MultiUserScheduler::TxFormat
RrMultiUserScheduler::SelectTxFormat (void)
{
  NS_LOG_FUNCTION (this);
  // ----- BEGIN MY CODE ------
  // m_edca->SetIsDlMuTx(false);
  // ----- END MY CODE ------

  Ptr<const WifiMpdu> mpdu = m_edca->PeekNextMpdu (SINGLE_LINK_OP_ID);

  if (mpdu && !GetWifiRemoteStationManager ()->GetHeSupported (mpdu->GetHeader ().GetAddr1 ()))
    {
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun SU_TX" << std::endl;

      return SU_TX;
    }

  if (m_enableUlOfdma && m_enableBsrp && (GetLastTxFormat () == DL_MU_TX || !mpdu))
    {
      TxFormat txFormat = TrySendingBsrpTf ();
      if (txFormat != DL_MU_TX)
        {
          return txFormat;
        }
    }
  else if (m_enableUlOfdma && ((GetLastTxFormat () == DL_MU_TX)
                               || (m_trigger.GetType () == TriggerFrameType::BSRP_TRIGGER)
                               || !mpdu))
    {
      TxFormat txFormat = TrySendingBasicTf ();

      if (txFormat != DL_MU_TX)
        {
          return txFormat;
        }
    }

  return TrySendingDlMuPpdu ();
}

template <class Func>
WifiTxVector
RrMultiUserScheduler::GetTxVectorForUlMu (Func canbeSolicited)
{
  NS_LOG_FUNCTION (this);

  // determine RUs to allocate to stations
  auto count = std::min<std::size_t> (m_nStations, m_staListUl.size ());
  std::size_t nCentral26TonesRus;
  HeRu::GetEqualSizedRusForStations (m_allowedWidth, count, nCentral26TonesRus);
  NS_ASSERT (count >= 1);

  if (!m_useCentral26TonesRus)
    {
      nCentral26TonesRus = 0;
    }

  Ptr<HeConfiguration> heConfiguration = m_apMac->GetHeConfiguration ();
  NS_ASSERT (heConfiguration);

  WifiTxVector txVector;
  txVector.SetPreambleType (WIFI_PREAMBLE_HE_TB);
  txVector.SetChannelWidth (m_allowedWidth);
  txVector.SetGuardInterval (heConfiguration->GetGuardInterval ().GetNanoSeconds ());
  txVector.SetBssColor (heConfiguration->GetBssColor ());

  //BEGIN: My Propose
  // if(!m_bsrpList.empty())
  // {
  //   auto max_ptr = std::max_element(m_staListUl.begin(),m_staListUl.end(),[](const auto sta1,const auto sta2){ return sta1.credits < sta2.credits;});
  //   for(auto staId : m_bsrpList)
  //   {
  //     std::cout << "+++ m_bsrpList:" << staId << std::endl;
  //     auto itr = std::find_if(m_staListUl.begin(),m_staListUl.end(),[&staId](auto &sta){
  //       return sta.aid == staId;
  //     });
  //     if(itr != m_staListUl.end())
  //     {
  //       itr->credits = max_ptr->credits;

  //       // m_apMac->SetBufferStatus(itr->aid,itr->address,255);
  //     }
  //   }
  //   m_staListUl.sort ([] (const MasterInfo& a, const MasterInfo& b)
  //               { return a.credits > b.credits; });

  // }
  //END: My Propose


  // iterate over the associated stations until an enough number of stations is identified
  auto staIt = m_staListUl.begin ();
  m_candidates.clear ();

  while (staIt != m_staListUl.end ()
         && txVector.GetHeMuUserInfoMap ().size () < std::min<std::size_t> (m_nStations, count + nCentral26TonesRus))
    {
      NS_LOG_DEBUG ("Next candidate STA (MAC=" << staIt->address << ", AID=" << staIt->aid << ")");
      std::cout << "Next candidate STA(MAC=" << staIt->address << ", AID=" << staIt->aid << ")" << std::endl;
      if (!canbeSolicited (*staIt))
        {
          NS_LOG_DEBUG ("Skipping station based on provided function object");
          std::cout << "Skipping station based on provided function object" << std::endl;
          staIt++;
          continue;
        }

      uint8_t tid = 0;
      while (tid < 8)
        {
          // check that a BA agreement is established with the receiver for the
          // considered TID, since ack sequences for UL MU require block ack
          if (m_heFem->GetBaAgreementEstablished (staIt->address, tid))
            {
              break;
            }
          ++tid;
        }
      if (tid == 8)
        {
          NS_LOG_DEBUG ("No Block Ack agreement established with " << staIt->address);
          staIt++;
          continue;
        }

      // prepare the MAC header of a frame that would be sent to the candidate station,
      // just for the purpose of retrieving the TXVECTOR used to transmit to that station
      WifiMacHeader hdr (WIFI_MAC_QOSDATA);
      hdr.SetAddr1 (staIt->address);
      hdr.SetAddr2 (m_apMac->GetAddress ());
      WifiTxVector suTxVector = GetWifiRemoteStationManager ()->GetDataTxVector (hdr, m_allowedWidth);
      txVector.SetHeMuUserInfo (staIt->aid,
                                {HeRu::RuSpec (), // assigned later by FinalizeTxVector
                                suTxVector.GetMode (),
                                suTxVector.GetNss ()});
      m_candidates.push_back ({staIt, nullptr});

      // move to the next station in the list
      staIt++;
    }

  if (txVector.GetHeMuUserInfoMap ().empty ())
    {
      NS_LOG_DEBUG ("No suitable station");
      return txVector;
    }
  //BEGIN: log for
  std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << std::endl;
  //END: log for
  
  FinalizeTxVector (txVector);
  return txVector;
}

MultiUserScheduler::TxFormat
RrMultiUserScheduler::TrySendingBsrpTf (void)
{
  NS_LOG_FUNCTION (this);

  if (m_staListUl.empty ())
    {
      NS_LOG_DEBUG ("No HE stations associated: return SU_TX");
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun SU_TX. No HE stations associated" << std::endl;
      return TxFormat::SU_TX;
    }
  //BEGIN: Default
  // WifiTxVector txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; });
  //END: Default
  
  //BEGIN: My Code Ru Random Assign
  // WifiTxVector txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; },true);
  //END:  My Code Ru Random Assign
  //BEGIN: My Propose
  // WifiTxVector txVector;
  // if(m_isDoneUl)
  // {
  //   int actual, theoretical;
  //   actual = m_heFem->GetBpsSets().at(0);
  //   theoretical = m_heFem->GetBpsSets().at(1);
    
  //   auto qosNullStas = m_heFem->GetQosNullStas();

  //   auto staIt = m_staListUl.begin ();
  //   int nullcount=0;
  //   int index=0;
  //   while (staIt != m_staListUl.end ()
  //         && index < std::min<std::size_t> (m_nStations, 37))
  //     {

  //       uint8_t tid = 0;
  //       while (tid < 8)
  //         {
  //           // check that a BA agreement is established with the receiver for the
  //           // considered TID, since ack sequences for UL MU require block ack
  //           if (m_heFem->GetBaAgreementEstablished (staIt->address, tid))
  //             {
  //               break;
  //             }
  //           ++tid;
  //         }
  //       if (tid == 8)
  //         {
  //         staIt++;
  //           continue;
  //         }

  //       // prepare the MAC header of a frame that would be sent to the candidate station,
  //       // just for the purpose of retrieving the TXVECTOR used to transmit to that station
  //       // WifiMacHeader hdr (WIFI_MAC_QOSDATA);
  //       // hdr.SetAddr1 (staIt->address);
  //       // hdr.SetAddr2 (m_apMac->GetAddress ());
  //       // WifiTxVector suTxVector = GetWifiRemoteStationManager ()->GetDataTxVector (hdr, m_allowedWidth);
  //       // txVector.SetHeMuUserInfo (staIt->aid,
  //       //                           {HeRu::RuSpec (), // assigned later by FinalizeTxVector
  //       //                           suTxVector.GetMode (),
  //       //                           suTxVector.GetNss ()});
  //       // m_candidates.push_back ({staIt, nullptr});
  //       auto itr = std::find(qosNullStas.begin(),qosNullStas.end(),staIt->address);
  //       if(itr != qosNullStas.end()) nullcount++;

  //       // move to the next station in the list
  //       index++;
  //       staIt++;
  //     }
  //   if(actual/theoretical*1.0 < (5472000+36000+260000)*1.0/(5472000+36000+260000+476000) && nullcount >0 ) //
  //   {
  //     std::cout << "** Start UONRA. actual/theoretical:" << actual/theoretical*1.0 << ". nullcount" << nullcount << std::endl;
  //     txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; });
  //   }
  //   else
  //   {
  //     std::cout << "** Start UORA actual/theoretical:" << actual/theoretical*1.0<< ". nullcount" << nullcount << std::endl;
  //     return TrySendingBasicTf();
  //     // txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; });
  //   }
  // }
  // else
  // {
  //   std::cout << "** Default" << std::endl;
  //   txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; });
  // }
  
  //END: My Propose

  //BEGIN: My Propose v2
  WifiTxVector txVector;
  m_bsrpList.clear();
  if(m_isDoneUl)
  {
    int count_true=0;
    UpdateWillBeQosNull();
    m_zerobsr.clear();
    std::ofstream writting_2;
    std::stringstream filename;
    filename << "./data/WillBeQosNull.csv";
    writting_2.open(filename.str(), std::ios::app);
    for(int i=1; i<= m_nStations ; i++)
    {
      //std::string address = "00:00:00:00:00:";
      bool isQosnull=false;
      if(m_bsr[0]==0)
      {
        m_zerobsr.push_back(i);
      }
      // if(i<10) address += "0";
      // address += i;
      // Mac48Address addr = Mac48Address::Mac48Address(address)
      if(m_will_be_qos_null[i])
      {
        count_true++;
        m_bsrpList.push_back(i);
        isQosnull = true;
      } 
      writting_2 << "," << ((isQosnull)?"true" : "false");
    }
    writting_2 << "," << Simulator::Now()<< std::endl;
    writting_2.close();
      
    // if(count_true >= m_threshold1 ) //
    // {
      std::cout << "** Start UONRA. count_true:" <<count_true << ". m_threshold1:" << m_threshold1 << std::endl;
    //   txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; });
    // }
    // else
    // {
    //   std::cout << "** Start UORA. count_true:" <<count_true << ". m_threshold1:" << m_threshold1 << std::endl;
    //   return TrySendingBasicTf();
    // }
    txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; });//AT: Default
  }
  else
  {
    std::cout << "** Default" << std::endl;
    txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; });
  }
  
  //END:My Propose v2

  if (txVector.GetHeMuUserInfoMap ().empty ())
    {
      NS_LOG_DEBUG ("No suitable station found");
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun DlMuTX" << std::endl;
      return TxFormat::DL_MU_TX;
    }

  m_trigger = CtrlTriggerHeader (TriggerFrameType::BSRP_TRIGGER, txVector);
  txVector.SetGuardInterval (m_trigger.GetGuardInterval ());

  auto item = GetTriggerFrame (m_trigger);
  m_triggerMacHdr = item->GetHeader ();

  m_txParams.Clear ();
  // set the TXVECTOR used to send the Trigger Frame
  m_txParams.m_txVector = m_apMac->GetWifiRemoteStationManager ()->GetRtsTxVector (m_triggerMacHdr.GetAddr1 ());

  if (!m_heFem->TryAddMpdu (item, m_txParams, m_availableTime))
    {
      // sending the BSRP Trigger Frame is not possible, hence return NO_TX. In
      // this way, no transmission will occur now and the next time we will
      // try again sending a BSRP Trigger Frame.
      NS_LOG_DEBUG ("Remaining TXOP duration is not enough for BSRP TF exchange");
      return NO_TX;
    }

  // Compute the time taken by each station to transmit 8 QoS Null frames
  Time qosNullTxDuration = Seconds (0);
  for (const auto& userInfo : m_trigger)
    {
      Time duration = WifiPhy::CalculateTxDuration (GetMaxSizeOfQosNullAmpdu (m_trigger),
                                                    txVector,
                                                    m_apMac->GetWifiPhy ()->GetPhyBand (),
                                                    userInfo.GetAid12 ());
      qosNullTxDuration = Max (qosNullTxDuration, duration);
    }

  if (m_availableTime != Time::Min ())
    {
      // TryAddMpdu only considers the time to transmit the Trigger Frame
      NS_ASSERT (m_txParams.m_protection && m_txParams.m_protection->protectionTime != Time::Min ());
      NS_ASSERT (m_txParams.m_acknowledgment && m_txParams.m_acknowledgment->acknowledgmentTime.IsZero ());
      NS_ASSERT (m_txParams.m_txDuration != Time::Min ());

      if (m_txParams.m_protection->protectionTime
          + m_txParams.m_txDuration     // BSRP TF tx time
          + m_apMac->GetWifiPhy ()->GetSifs ()
          + qosNullTxDuration
          > m_availableTime)
        {
          NS_LOG_DEBUG ("Remaining TXOP duration is not enough for BSRP TF exchange");
          return NO_TX;
        }
    }

  uint16_t ulLength;
  std::tie (ulLength, qosNullTxDuration) = HePhy::ConvertHeTbPpduDurationToLSigLength (qosNullTxDuration,
                                                                                       m_trigger.GetHeTbTxVector (m_trigger.begin ()->GetAid12 ()),
                                                                                       m_apMac->GetWifiPhy ()->GetPhyBand ());
  NS_LOG_DEBUG ("Duration of QoS Null frames: " << qosNullTxDuration.As (Time::MS));
  m_trigger.SetUlLength (ulLength);
  std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun UlMuTX" << std::endl;
  m_isNotAfterBsrp = false;//AT: MY CODE
  return UL_MU_TX;
}

MultiUserScheduler::TxFormat
RrMultiUserScheduler::TrySendingBasicTf (void)
{
  NS_LOG_FUNCTION (this);

  if (m_staListUl.empty ())
    {
      NS_LOG_DEBUG ("No HE stations associated: return SU_TX");
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun SU_TX" << std::endl; 
      return TxFormat::SU_TX;
    }

  // check if an UL OFDMA transmission is possible after a DL OFDMA transmission
  NS_ABORT_MSG_IF (m_ulPsduSize == 0, "The UlPsduSize attribute must be set to a non-null value");
  m_bsrpList.clear();
  //BEGIN: log for
  for(auto info : m_staListUl)
  {
    std::cout << "Sta:" << info.address << ". maxBufferStatus:" << int(m_apMac->GetMaxBufferStatus(info.address)) << std::endl;
  }
  std::ofstream writting;
  std::stringstream filename;
  filename << "./data/MaxBufferStatus.csv";
  writting.open(filename.str(), std::ios::app);
  for(int i=1; i<= m_nStations ; i++)
  {
    auto itr = std::find_if(m_staListUl.begin(),m_staListUl.end(),[&i](auto &sta){
      return sta.aid == i;
    });
    int mbs = int(m_apMac->GetMaxBufferStatus(itr->address));
    // std::cerr << << "," << mbs << std::endl;
    writting << "," << mbs;
  }
  writting << "," << Simulator::Now()<< std::endl;
  writting.close();
  //END: log for
  // only consider stations that do not have reported a null queue size
  //BEGIN: Default
  WifiTxVector txVector = GetTxVectorForUlMu ([this](const MasterInfo& info)
                                               { return m_apMac->GetMaxBufferStatus (info.address) > 0; });
  //END: Default

  //BEGIN: Ru Random Assign for UORA
  
  // WifiTxVector txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; },m_isNotAfterBsrp);

  // WifiTxVector txVector;
  // if(m_isRuRand) // My propose
  // {
  //   txVector = GetTxVectorForUlMu ([](const MasterInfo&){ return true; },m_isNotAfterBsrp);
  //   // std::cerr << "It`s My Propose!" << std::endl;
  // }
  // else // Default
  // {
  //   txVector = GetTxVectorForUlMu ([this](const MasterInfo& info)
  //                                              { return m_apMac->GetMaxBufferStatus (info.address) > 0; });
  // }

  m_isNotAfterBsrp = true;
  //END: Ru Random Assing for UORA
  if (txVector.GetHeMuUserInfoMap ().empty ())
    {
      NS_LOG_DEBUG ("No suitable station found");
      // m_edca->SetIsDlMuTx(true);
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun DlMuTX. No suitable station found" << std::endl;
      return TxFormat::DL_MU_TX;
    }

  uint32_t maxBufferSize = 0;

  for (const auto& candidate : txVector.GetHeMuUserInfoMap ())
    {
      auto staIt = m_apMac->GetStaList ().find (candidate.first);
      NS_ASSERT (staIt != m_apMac->GetStaList ().end ());
      uint8_t queueSize = m_apMac->GetMaxBufferStatus (staIt->second);
      if (queueSize == 255)
        {
          NS_LOG_DEBUG ("Buffer status of station " << staIt->second << " is unknown");
          maxBufferSize = std::max (maxBufferSize, m_ulPsduSize);
        }
      else if (queueSize == 254)
        {
          NS_LOG_DEBUG ("Buffer status of station " << staIt->second << " is not limited");
          maxBufferSize = 0xffffffff;
        }
      else
        {
          NS_LOG_DEBUG ("Buffer status of station " << staIt->second << " is " << +queueSize);
          maxBufferSize = std::max (maxBufferSize, static_cast<uint32_t> (queueSize * 256));
        }
    }

  if (maxBufferSize == 0)
    {
      // m_edca->SetIsDlMuTx(true);
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun DlMuTX. maxBufferSize is 0" << std::endl;
      return DL_MU_TX;
    }

  m_trigger = CtrlTriggerHeader (TriggerFrameType::BASIC_TRIGGER, txVector);
  txVector.SetGuardInterval (m_trigger.GetGuardInterval ());

  auto item = GetTriggerFrame (m_trigger);
  m_triggerMacHdr = item->GetHeader ();

  // compute the maximum amount of time that can be granted to stations.
  // This value is limited by the max PPDU duration
  Time maxDuration = GetPpduMaxTime (txVector.GetPreambleType ());

  m_txParams.Clear ();
  // set the TXVECTOR used to send the Trigger Frame
  m_txParams.m_txVector = m_apMac->GetWifiRemoteStationManager ()->GetRtsTxVector (m_triggerMacHdr.GetAddr1 ());

  if (!m_heFem->TryAddMpdu (item, m_txParams, m_availableTime))
    {
      // an UL OFDMA transmission is not possible, hence return NO_TX. In
      // this way, no transmission will occur now and the next time we will
      // try again performing an UL OFDMA transmission.
      NS_LOG_DEBUG ("Remaining TXOP duration is not enough for UL MU exchange");
      return NO_TX;
    }

  if (m_availableTime != Time::Min ())
    {
      // TryAddMpdu only considers the time to transmit the Trigger Frame
      NS_ASSERT (m_txParams.m_protection && m_txParams.m_protection->protectionTime != Time::Min ());
      NS_ASSERT (m_txParams.m_acknowledgment && m_txParams.m_acknowledgment->acknowledgmentTime != Time::Min ());
      NS_ASSERT (m_txParams.m_txDuration != Time::Min ());

      maxDuration = Min (maxDuration, m_availableTime
                                      - m_txParams.m_protection->protectionTime
                                      - m_txParams.m_txDuration
                                      - m_apMac->GetWifiPhy ()->GetSifs ()
                                      - m_txParams.m_acknowledgment->acknowledgmentTime);
      if (maxDuration.IsNegative ())
        {
          NS_LOG_DEBUG ("Remaining TXOP duration is not enough for UL MU exchange");
          return NO_TX;
        }
    }

  // Compute the time taken by each station to transmit a frame of maxBufferSize size
  Time bufferTxTime = Seconds (0);
  for (const auto& userInfo : m_trigger)
    {
      Time duration = WifiPhy::CalculateTxDuration (maxBufferSize, txVector,
                                                    m_apMac->GetWifiPhy ()->GetPhyBand (),
                                                    userInfo.GetAid12 ());
      bufferTxTime = Max (bufferTxTime, duration);
    }

  if (bufferTxTime < maxDuration)
    {
      // the maximum buffer size can be transmitted within the allowed time
      maxDuration = bufferTxTime;
    }
  else
    {
      // maxDuration may be a too short time. If it does not allow any station to
      // transmit at least m_ulPsduSize bytes, give up the UL MU transmission for now
      Time minDuration = Seconds (0);
      for (const auto& userInfo : m_trigger)
        {
          Time duration = WifiPhy::CalculateTxDuration (m_ulPsduSize, txVector,
                                                        m_apMac->GetWifiPhy ()->GetPhyBand (),
                                                        userInfo.GetAid12 ());
          minDuration = (minDuration.IsZero () ? duration : Min (minDuration, duration));
        }

      if (maxDuration < minDuration)
        {
          // maxDuration is a too short time, hence return NO_TX. In this way,
          // no transmission will occur now and the next time we will try again
          // performing an UL OFDMA transmission.
          NS_LOG_DEBUG ("Available time " << maxDuration.As (Time::MS) << " is too short");
          return NO_TX;
        }
    }

  // maxDuration is the time to grant to the stations. Finalize the Trigger Frame
  uint16_t ulLength;
  std::tie (ulLength, maxDuration) = HePhy::ConvertHeTbPpduDurationToLSigLength (maxDuration,
                                                                                  txVector,
                                                                                  m_apMac->GetWifiPhy ()->GetPhyBand ());
  NS_LOG_DEBUG ("TB PPDU duration: " << maxDuration.As (Time::MS));
  m_trigger.SetUlLength (ulLength);
  // set Preferred AC to the AC that gained channel access
  for (auto& userInfo : m_trigger)
    {
      userInfo.SetBasicTriggerDepUserInfo (0, 0, m_edca->GetAccessCategory ());
    }

  UpdateCredits (m_staListUl, maxDuration, txVector);
  std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun UlMuTX. " << std::endl;
  return UL_MU_TX;
}

void
RrMultiUserScheduler::NotifyStationAssociated (uint16_t aid, Mac48Address address)
{
  NS_LOG_FUNCTION (this << aid << address);

  if (GetWifiRemoteStationManager ()->GetHeSupported (address))
    {
      for (auto& staList : m_staListDl)
        {
          staList.second.push_back (MasterInfo {aid, address, 0.0});
        }
      m_staListUl.push_back (MasterInfo {aid, address, 0.0});
    }
}

void
RrMultiUserScheduler::NotifyStationDeassociated (uint16_t aid, Mac48Address address)
{
  NS_LOG_FUNCTION (this << aid << address);

  if (GetWifiRemoteStationManager ()->GetHeSupported (address))
    {
      for (auto& staList : m_staListDl)
        {
          staList.second.remove_if ([&aid, &address] (const MasterInfo& info)
                                    { return info.aid == aid && info.address == address; });
        }
      m_staListUl.remove_if ([&aid, &address] (const MasterInfo& info)
                             { return info.aid == aid && info.address == address; });
    }
}

MultiUserScheduler::TxFormat
RrMultiUserScheduler::TrySendingDlMuPpdu (void)
{
  NS_LOG_FUNCTION (this);

  AcIndex primaryAc = m_edca->GetAccessCategory ();

  if (m_staListDl[primaryAc].empty ())
    {
      NS_LOG_DEBUG ("No HE stations associated: return SU_TX");
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun SU_TX. No HE stations associtated" << std::endl; 
      return TxFormat::SU_TX;
    }

  std::size_t count = std::min (static_cast<std::size_t> (m_nStations), m_staListDl[primaryAc].size ());
  std::size_t nCentral26TonesRus;
  HeRu::RuType ruType = HeRu::GetEqualSizedRusForStations (m_allowedWidth, count,
                                                           nCentral26TonesRus);
  NS_ASSERT (count >= 1);

  if (!m_useCentral26TonesRus)
    {
      nCentral26TonesRus = 0;
    }

  uint8_t currTid = wifiAcList.at (primaryAc).GetHighTid ();

  Ptr<WifiMpdu> mpdu = m_edca->PeekNextMpdu (SINGLE_LINK_OP_ID);

  if (mpdu && mpdu->GetHeader ().IsQosData ())
    {
      currTid = mpdu->GetHeader ().GetQosTid ();
    }

  // determine the list of TIDs to check
  std::vector<uint8_t> tids;

  if (m_enableTxopSharing)
    {
      for (auto acIt = wifiAcList.find (primaryAc); acIt != wifiAcList.end (); acIt++)
        {
          uint8_t firstTid = (acIt->first == primaryAc ? currTid : acIt->second.GetHighTid ());
          tids.push_back (firstTid);
          tids.push_back (acIt->second.GetOtherTid (firstTid));
        }
    }
  else
    {
      tids.push_back (currTid);
    }

  Ptr<HeConfiguration> heConfiguration = m_apMac->GetHeConfiguration ();
  NS_ASSERT (heConfiguration);

  m_txParams.Clear ();
  m_txParams.m_txVector.SetPreambleType (WIFI_PREAMBLE_HE_MU);
  m_txParams.m_txVector.SetChannelWidth (m_allowedWidth);
  m_txParams.m_txVector.SetGuardInterval (heConfiguration->GetGuardInterval ().GetNanoSeconds ());
  m_txParams.m_txVector.SetBssColor (heConfiguration->GetBssColor ());

  // The TXOP limit can be exceeded by the TXOP holder if it does not transmit more
  // than one Data or Management frame in the TXOP and the frame is not in an A-MPDU
  // consisting of more than one MPDU (Sec. 10.22.2.8 of 802.11-2016).
  // For the moment, we are considering just one MPDU per receiver.
  Time actualAvailableTime = (m_initialFrame ? Time::Min () : m_availableTime);

  // iterate over the associated stations until an enough number of stations is identified
  auto staIt = m_staListDl[primaryAc].begin ();
  m_candidates.clear ();

  while (staIt != m_staListDl[primaryAc].end ()
         && m_candidates.size () < std::min (static_cast<std::size_t> (m_nStations), count + nCentral26TonesRus))
    {
      NS_LOG_DEBUG ("Next candidate STA (MAC=" << staIt->address << ", AID=" << staIt->aid << ")");
      std::cout << "Next candidate STA(MAC=" << staIt->address << ", AID=" << staIt->aid << "). DL_MU_TX" << std::endl;

      HeRu::RuType currRuType = (m_candidates.size () < count ? ruType : HeRu::RU_26_TONE);

      // check if the AP has at least one frame to be sent to the current station
      for (uint8_t tid : tids)
        {
          AcIndex ac = QosUtilsMapTidToAc (tid);
          NS_ASSERT (ac >= primaryAc);
          // check that a BA agreement is established with the receiver for the
          // considered TID, since ack sequences for DL MU PPDUs require block ack
          if (m_apMac->GetQosTxop (ac)->GetBaAgreementEstablished (staIt->address, tid))
            {
              mpdu = m_apMac->GetQosTxop (ac)->PeekNextMpdu (SINGLE_LINK_OP_ID, tid, staIt->address);

              // we only check if the first frame of the current TID meets the size
              // and duration constraints. We do not explore the queues further.
              if (mpdu)
                {
                  // Use a temporary TX vector including only the STA-ID of the
                  // candidate station to check if the MPDU meets the size and time limits.
                  // An RU of the computed size is tentatively assigned to the candidate
                  // station, so that the TX duration can be correctly computed.
                  WifiTxVector suTxVector = GetWifiRemoteStationManager ()->GetDataTxVector (mpdu->GetHeader (), m_allowedWidth),
                               txVectorCopy = m_txParams.m_txVector;

                  m_txParams.m_txVector.SetHeMuUserInfo (staIt->aid,
                                                         {{currRuType, 1, false},
                                                          suTxVector.GetMode (),
                                                          suTxVector.GetNss ()});

                  if (!m_heFem->TryAddMpdu (mpdu, m_txParams, actualAvailableTime))
                    {
                      NS_LOG_DEBUG ("Adding the peeked frame violates the time constraints");
                      m_txParams.m_txVector = txVectorCopy;
                    }
                  else
                    {
                      // the frame meets the constraints
                      NS_LOG_DEBUG ("Adding candidate STA (MAC=" << staIt->address << ", AID="
                                    << staIt->aid << ") TID=" << +tid);
                      m_candidates.push_back ({staIt, mpdu});
                      break;    // terminate the for loop
                    }
                }
              else
                {
                  NS_LOG_DEBUG ("No frames to send to " << staIt->address << " with TID=" << +tid);
                }
            }
        }

      // move to the next station in the list
      staIt++;
    }

  if (m_candidates.empty ())
    {
      if (m_forceDlOfdma)
        {
          NS_LOG_DEBUG ("The AP does not have suitable frames to transmit: return NO_TX");
          return NO_TX;
        }
      NS_LOG_DEBUG ("The AP does not have suitable frames to transmit: return SU_TX");
      std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun SU_TX. The AP does not have suitable frames to transmit" << std::endl; 
      return SU_TX;
    }
  std::cout << "Time:" << Simulator::Now() << ". Fucntion:" <<__func__ << ". retrun DlMuTX" << std::endl;
  return TxFormat::DL_MU_TX;
}

//BEGIN Default
void
RrMultiUserScheduler::FinalizeTxVector (WifiTxVector& txVector)
{
  // Do not log txVector because GetTxVectorForUlMu() left RUs undefined and
  // printing them will crash the simulation
  NS_LOG_FUNCTION (this);
  NS_ASSERT (txVector.GetHeMuUserInfoMap ().size () == m_candidates.size ());

  // compute how many stations can be granted an RU and the RU size
  std::size_t nRusAssigned = m_candidates.size ();
  std::size_t nCentral26TonesRus;
  HeRu::RuType ruType = HeRu::GetEqualSizedRusForStations (m_allowedWidth, nRusAssigned,
                                                           nCentral26TonesRus);
  //BEGIN: log for
  std::cout << "nRusAssigned:" << nRusAssigned << std::endl; 
  //END: log for
  NS_LOG_DEBUG (nRusAssigned << " stations are being assigned a " << ruType << " RU");

  if (!m_useCentral26TonesRus || m_candidates.size () == nRusAssigned)
    {
      nCentral26TonesRus = 0;
    }
  else
    {
      nCentral26TonesRus = std::min (m_candidates.size () - nRusAssigned, nCentral26TonesRus);
      NS_LOG_DEBUG (nCentral26TonesRus << " stations are being assigned a 26-tones RU");
    }

  // re-allocate RUs based on the actual number of candidate stations
  WifiTxVector::HeMuUserInfoMap heMuUserInfoMap;
  std::swap (heMuUserInfoMap, txVector.GetHeMuUserInfoMap ());

  auto candidateIt = m_candidates.begin (); // iterator over the list of candidate receivers
  auto ruSet = HeRu::GetRusOfType (m_allowedWidth, ruType);
  auto ruSetIt = ruSet.begin ();
  auto central26TonesRus = HeRu::GetCentral26TonesRus (m_allowedWidth, ruType);
  auto central26TonesRusIt = central26TonesRus.begin ();

  for (std::size_t i = 0; i < nRusAssigned + nCentral26TonesRus; i++)
    {
      NS_ASSERT (candidateIt != m_candidates.end ());
      auto mapIt = heMuUserInfoMap.find (candidateIt->first->aid);
      NS_ASSERT (mapIt != heMuUserInfoMap.end ());
      std::cout << "Assign RU. staId:" << mapIt->first << ". RuSet:" << *ruSetIt << std::endl;
      //BEGIN: Inspection No Bsrp Fixed-RU
      // txVector.SetHeMuUserInfo (mapIt->first,
      //                           {(i < nRusAssigned ? *ruSetIt : *central26TonesRusIt++),
      //                            mapIt->second.mcs, mapIt->second.nss});
      //END: Inspection No Bsrp Fixed-RU

      //BEGIN: Default
      txVector.SetHeMuUserInfo (mapIt->first,
                                {(i < nRusAssigned ? *ruSetIt++ : *central26TonesRusIt++),
                                 mapIt->second.mcs, mapIt->second.nss});
      //END: Default
      candidateIt++;
       
    }

  // remove candidates that will not be served
  m_candidates.erase (candidateIt, m_candidates.end ());
}
//END: Default

void
RrMultiUserScheduler::UpdateCredits (std::list<MasterInfo>& staList, Time txDuration,
                                     const WifiTxVector& txVector)
{
  NS_LOG_FUNCTION (this << txDuration.As (Time::US) << txVector);

  // find how many RUs have been allocated for each RU type
  std::map<HeRu::RuType, std::size_t> ruMap;
  for (const auto& userInfo : txVector.GetHeMuUserInfoMap ())
    {
      ruMap.insert ({userInfo.second.ru.GetRuType (), 0}).first->second++;
    }

  // The amount of credits received by each station equals the TX duration (in
  // microseconds) divided by the number of stations.
  double creditsPerSta = txDuration.ToDouble (Time::US) / staList.size ();
  // Transmitting stations have to pay a number of credits equal to the TX duration
  // (in microseconds) times the allocated bandwidth share.
  double debitsPerMhz = txDuration.ToDouble (Time::US)
                        / std::accumulate (ruMap.begin (), ruMap.end (), 0,
                                           [](uint16_t sum, auto pair)
                                           { return sum + pair.second * HeRu::GetBandwidth (pair.first); });
  std::cout << "Time:" << Simulator::Now() <<". Function:" << __func__ << std::endl;
  // assign credits to all stations
  for (auto& sta : staList)
    {
      sta.credits += creditsPerSta;
      sta.credits = std::min (sta.credits, m_maxCredits.ToDouble (Time::US));
      std::cout << "sta:"<< sta.address << ". aid:"<<sta.aid << ". sta.credits:" << sta.credits << std::endl;
    }

  // subtract debits to the selected stations
  for (auto& candidate : m_candidates)
    {
      auto mapIt = txVector.GetHeMuUserInfoMap ().find (candidate.first->aid);
      NS_ASSERT (mapIt != txVector.GetHeMuUserInfoMap ().end ());
     std::cout << "candidate addr:" << candidate.first->address << ", credits:" << candidate.first->credits ;
      candidate.first->credits -= debitsPerMhz * HeRu::GetBandwidth (mapIt->second.ru.GetRuType ());
      std::cout << ". debitsPerMhz:" << debitsPerMhz << ". new credits:" << candidate.first->credits << ". band:" << HeRu::GetBandwidth (mapIt->second.ru.GetRuType ()) << std::endl; 

    }

  // sort the list in decreasing order of credits
  staList.sort ([] (const MasterInfo& a, const MasterInfo& b)
                { return a.credits > b.credits; });
  for(auto& sta: staList)
  {
    std::cout << "sorted: sta:" << sta.address << std::endl;
  }

}

MultiUserScheduler::DlMuInfo
RrMultiUserScheduler::ComputeDlMuInfo (void)
{
  NS_LOG_FUNCTION (this);

  if (m_candidates.empty ())
    {
      return DlMuInfo ();
    }
  //BEGIN: log for
  std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << std::endl;
  //END: log for
  DlMuInfo dlMuInfo;
  std::swap (dlMuInfo.txParams.m_txVector, m_txParams.m_txVector);
  FinalizeTxVector (dlMuInfo.txParams.m_txVector);

  m_txParams.Clear ();
  Ptr<WifiMpdu> mpdu;

  // Compute the TX params (again) by using the stored MPDUs and the final TXVECTOR
  Time actualAvailableTime = (m_initialFrame ? Time::Min () : m_availableTime);

  for (const auto& candidate : m_candidates)
    {
      mpdu = candidate.second;
      NS_ASSERT (mpdu);

      [[maybe_unused]] bool ret = m_heFem->TryAddMpdu (mpdu, dlMuInfo.txParams, actualAvailableTime);
      NS_ASSERT_MSG (ret, "Weird that an MPDU does not meet constraints when "
                          "transmitted over a larger RU");
    }

  // We have to complete the PSDUs to send
  Ptr<WifiMacQueue> queue;
  Mac48Address receiver;

  for (const auto& candidate : m_candidates)
    {
      // Let us try first A-MSDU aggregation if possible
      mpdu = candidate.second;
      NS_ASSERT (mpdu);
      uint8_t tid = mpdu->GetHeader ().GetQosTid ();
      receiver = mpdu->GetHeader ().GetAddr1 ();
      NS_ASSERT (receiver == candidate.first->address);

      NS_ASSERT (mpdu->IsQueued ());
      Ptr<WifiMpdu> item = mpdu;

      if (!mpdu->GetHeader ().IsRetry ())
        {
          // this MPDU must have been dequeued from the AC queue and we can try
          // A-MSDU aggregation
          item = m_heFem->GetMsduAggregator ()->GetNextAmsdu (mpdu, dlMuInfo.txParams, m_availableTime);

          if (!item)
            {
              // A-MSDU aggregation failed or disabled
              item = mpdu;
            }
          m_apMac->GetQosTxop (QosUtilsMapTidToAc (tid))->AssignSequenceNumber (item);
        }

      // Now, let's try A-MPDU aggregation if possible
      std::vector<Ptr<WifiMpdu>> mpduList = m_heFem->GetMpduAggregator ()->GetNextAmpdu (item, dlMuInfo.txParams, m_availableTime);

      if (mpduList.size () > 1)
        {
          // A-MPDU aggregation succeeded, update psduMap
          dlMuInfo.psduMap[candidate.first->aid] = Create<WifiPsdu> (std::move (mpduList));
        }
      else
        {
          dlMuInfo.psduMap[candidate.first->aid] = Create<WifiPsdu> (item, true);
        }
    }

  AcIndex primaryAc = m_edca->GetAccessCategory ();
  UpdateCredits (m_staListDl[primaryAc], dlMuInfo.txParams.m_txDuration, dlMuInfo.txParams.m_txVector);

  NS_LOG_DEBUG ("Next station to serve has AID=" << m_staListDl[primaryAc].front ().aid);

  return dlMuInfo;
}

MultiUserScheduler::UlMuInfo
RrMultiUserScheduler::ComputeUlMuInfo (void)
{
  return UlMuInfo {m_trigger, m_triggerMacHdr, std::move (m_txParams)};
}

//BEGIN: My Code My Code Ru Random Assign
template <class Func>
WifiTxVector
RrMultiUserScheduler::GetTxVectorForUlMu (Func canbeSolicited,bool isBsrp)
{
  NS_LOG_FUNCTION (this);

  // determine RUs to allocate to stations
  auto count = std::min<std::size_t> (m_nStations, m_staListUl.size ());
  std::size_t nCentral26TonesRus;
  HeRu::GetEqualSizedRusForStations (m_allowedWidth, count, nCentral26TonesRus,isBsrp);
  NS_ASSERT (count >= 1);

  if (!m_useCentral26TonesRus)
    {
      nCentral26TonesRus = 0;
    }

  Ptr<HeConfiguration> heConfiguration = m_apMac->GetHeConfiguration ();
  NS_ASSERT (heConfiguration);

  WifiTxVector txVector;
  txVector.SetPreambleType (WIFI_PREAMBLE_HE_TB);
  txVector.SetChannelWidth (m_allowedWidth);
  txVector.SetGuardInterval (heConfiguration->GetGuardInterval ().GetNanoSeconds ());
  txVector.SetBssColor (heConfiguration->GetBssColor ());

  // iterate over the associated stations until an enough number of stations is identified
  auto staIt = m_staListUl.begin ();
  m_candidates.clear ();
 std::cout << "isBsrp:" << ((isBsrp)? "true" : "false") << std::endl;
  while (staIt != m_staListUl.end ()
         && txVector.GetHeMuUserInfoMap ().size () < std::min<std::size_t> (m_nStations, count + nCentral26TonesRus))
    {
      NS_LOG_DEBUG ("Next candidate STA (MAC=" << staIt->address << ", AID=" << staIt->aid << ")");
      std::cout << "Next candidate STA(MAC=" << staIt->address << ", AID=" << staIt->aid << ")" << std::endl;
      if (!canbeSolicited (*staIt))
        {
          NS_LOG_DEBUG ("Skipping station based on provided function object");
          std::cout << "Skipping station based on provided function object" << std::endl;
          staIt++;
          continue;
        }

      uint8_t tid = 0;
      while (tid < 8)
        {
          // check that a BA agreement is established with the receiver for the
          // considered TID, since ack sequences for UL MU require block ack
          if (m_heFem->GetBaAgreementEstablished (staIt->address, tid))
            {
              break;
            }
          ++tid;
        }
      if (tid == 8)
        {
          NS_LOG_DEBUG ("No Block Ack agreement established with " << staIt->address);
          staIt++;
          continue;
        }

      // prepare the MAC header of a frame that would be sent to the candidate station,
      // just for the purpose of retrieving the TXVECTOR used to transmit to that station
      WifiMacHeader hdr (WIFI_MAC_QOSDATA);
      hdr.SetAddr1 (staIt->address);
      hdr.SetAddr2 (m_apMac->GetAddress ());
      WifiTxVector suTxVector = GetWifiRemoteStationManager ()->GetDataTxVector (hdr, m_allowedWidth);
      txVector.SetHeMuUserInfo (staIt->aid,
                                {HeRu::RuSpec (), // assigned later by FinalizeTxVector
                                suTxVector.GetMode (),
                                suTxVector.GetNss ()});
      m_candidates.push_back ({staIt, nullptr});

      // move to the next station in the list
      staIt++;
    }

  if (txVector.GetHeMuUserInfoMap ().empty ())
    {
      NS_LOG_DEBUG ("No suitable station");
      return txVector;
    }
  //BEGIN: log for
  std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << std::endl;
  //END: log for
  
  FinalizeTxVector (txVector, isBsrp);
  return txVector;
}

void
RrMultiUserScheduler::FinalizeTxVector (WifiTxVector& txVector, bool isBsrp)
{
  // Do not log txVector because GetTxVectorForUlMu() left RUs undefined and
  // printing them will crash the simulation
  NS_LOG_FUNCTION (this);
  NS_ASSERT (txVector.GetHeMuUserInfoMap ().size () == m_candidates.size ());
  std::cout << "Finalize isBsrp! ";
  // compute how many stations can be granted an RU and the RU size
  std::size_t nRusAssigned = m_candidates.size ();
  std::size_t nCentral26TonesRus;
  HeRu::RuType ruType = HeRu::GetEqualSizedRusForStations (m_allowedWidth, nRusAssigned,
                                                           nCentral26TonesRus, isBsrp);
  //BEGIN: log for
  std::cout << "nRusAssigned:" << nRusAssigned << std::endl; 
  //END: log for
  NS_LOG_DEBUG (nRusAssigned << " stations are being assigned a " << ruType << " RU");

  if (!m_useCentral26TonesRus || m_candidates.size () == nRusAssigned)
    {
      nCentral26TonesRus = 0;
    }
  else
    {
      nCentral26TonesRus = std::min (m_candidates.size () - nRusAssigned, nCentral26TonesRus);
      NS_LOG_DEBUG (nCentral26TonesRus << " stations are being assigned a 26-tones RU");
    }

  // re-allocate RUs based on the actual number of candidate stations
  WifiTxVector::HeMuUserInfoMap heMuUserInfoMap;
  std::swap (heMuUserInfoMap, txVector.GetHeMuUserInfoMap ());

  auto candidateIt = m_candidates.begin (); // iterator over the list of candidate receivers
  auto ruSet = HeRu::GetRusOfType (m_allowedWidth, ruType);
  auto ruSetIt = ruSet.begin ();
  auto ruSetSize = ruSet.size();
  auto central26TonesRus = HeRu::GetCentral26TonesRus (m_allowedWidth, ruType);
  auto central26TonesRusIt = central26TonesRus.begin ();
  if(nRusAssigned <= ruSetSize)
  {
    for (std::size_t i = 0; i < nRusAssigned + nCentral26TonesRus; i++)
      {
        NS_ASSERT (candidateIt != m_candidates.end ());
        auto mapIt = heMuUserInfoMap.find (candidateIt->first->aid);
        NS_ASSERT (mapIt != heMuUserInfoMap.end ());
        std::cout << "Assign RU. staId:" << mapIt->first << ". RuSet:" << *ruSetIt << "RuSetSize:"<< ruSetSize << std::endl;
        //BEGIN: Inspection No Bsrp Fixed-RU
        // txVector.SetHeMuUserInfo (mapIt->first,
        //                           {(i < nRusAssigned ? *ruSetIt : *central26TonesRusIt++),
        //                            mapIt->second.mcs, mapIt->second.nss});
        //END: Inspection No Bsrp Fixed-RU

        //BEGIN: Default
        txVector.SetHeMuUserInfo (mapIt->first,
                                  {(i < nRusAssigned ? *ruSetIt++ : *central26TonesRusIt++),
                                  mapIt->second.mcs, mapIt->second.nss});
        //END: Default
        candidateIt++;
        
      }
  }
  else
  {
    RngSeedManager::SetSeed (1);
    RngSeedManager::SetRun (1);
    Ptr<UniformRandomVariable> rand = CreateObject<UniformRandomVariable> ();
    int ruIndex;
    for(std::size_t i = 0; i < nRusAssigned + nCentral26TonesRus; i++)
    {
      auto mapIt = heMuUserInfoMap.find (candidateIt->first->aid);
      ruIndex = rand->GetInteger(0,ruSetSize-1);
      std::cout << "Assign RU. staId:" << mapIt->first << ". RuSet:" << ruSet.at(ruIndex) << "RuSetSize:"<< ruSetSize << std::endl;
      std::cout << "mapIt->second.mcs:" << mapIt->second.mcs << ". mapIt->second.nss:" << mapIt->second.nss << std::endl;
      txVector.SetHeMuUserInfo (mapIt->first,{ruSet.at(ruIndex),mapIt->second.mcs, mapIt->second.nss});
      candidateIt++;
    }
    

  }

  // remove candidates that will not be served
  m_candidates.erase (candidateIt, m_candidates.end ());
}

//END: My Code Ru Random Assign

//BEGIN: My Propose
bool
RrMultiUserScheduler::isEnableBsrp()
{
  return m_enableBsrp;
}
void
RrMultiUserScheduler::SetEnableBsrp(bool isBsrp)
{
  m_enableBsrp = isBsrp;
}
void
RrMultiUserScheduler::SwitchRuAssignMode(bool sw)
{
  m_isRuRand = sw;
}

void 
RrMultiUserScheduler::UpdateBsr(int staId, int byte)
{
  std::cout <<"Function:" << __func__ << ", staId:" << staId << ", byte:" << byte << std::endl;
  if(byte <0)
  {
    if(m_bsr[staId]>0)
    {
      m_bsr[staId] += byte;
      if(m_bsr[staId]<=0)
      {
        m_will_be_qos_null[staId] = true;
      }
    }
  }
  else
  {
    m_bsr[staId] = byte;
    
    m_will_be_qos_null[staId] = false;
      
  }
  
}

void
RrMultiUserScheduler::UpdateWillBeQosNull()
{
  auto qosNullStas = m_heFem->GetQosNullStas();
  std::ofstream writting_3;
  std::stringstream filename;
  filename << "./data/MyBsr.csv";
  writting_3.open(filename.str(), std::ios::app);
  for(int i=1; i<= m_nStations ; i++)
  {
    writting_3 << "," << m_bsr[i] ;
    auto addr_ptr = std::find_if(m_staListUl.begin(),m_staListUl.end(),[&i](auto &sta){
      return sta.aid == i;
    });
    auto itr = std::find(qosNullStas.begin(),qosNullStas.end(),addr_ptr->address);
    auto zero_ptr = std::find(m_zerobsr.begin(),m_zerobsr.end(),i);

    if(itr != qosNullStas.end())
    {
      m_will_be_qos_null[i] = true;
    }
    else if(zero_ptr != m_zerobsr.end())
    {
      m_will_be_qos_null[i] = false;
    } 
  }
  writting_3 << "," << Simulator::Now()<< std::endl;
  writting_3.close();
}

//END: My Propose

} //namespace ns3
