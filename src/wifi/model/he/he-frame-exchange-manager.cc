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
#include "ns3/abort.h"
#include "he-frame-exchange-manager.h"
#include "he-configuration.h"
#include "ns3/recipient-block-ack-agreement.h"
#include "ns3/ap-wifi-mac.h"
#include "ns3/sta-wifi-mac.h"
#include "multi-user-scheduler.h"
#include "ns3/snr-tag.h"
#include "he-phy.h"
#include <algorithm>
#include <functional>
#include <fstream>

#undef NS_LOG_APPEND_CONTEXT
#define NS_LOG_APPEND_CONTEXT std::clog << "[link=" << +m_linkId << "][mac=" << m_self << "] "

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("HeFrameExchangeManager");

NS_OBJECT_ENSURE_REGISTERED (HeFrameExchangeManager);

// ----- BEGIN: MY CODE -----
int m_numBasic=0;
int m_numBsrp=0;
int m_nConflict =0;
int m_wins;
int m_ruNum;
int m_bsrpSuccessNum;
bool m_isArbi;

struct CandidateInfo {
  Mac48Address addr;
  int cCount;
  int sCount;
  int byte;
  int bsrpTfReceiveCount;
  int QosNullReceivedCount;
};
std::vector<CandidateInfo> m_successes; 
int m_candidate;
int m_max_candidate;

struct BusyTone {
  uint16_t staId;
  uint8_t arbitrationNum;
  CtrlTriggerHeader trigger;
  WifiMacHeader hdr;
  bool isWin;
};
struct HeRuMap {
  HeRu::RuSpec ru;
  std::vector<BusyTone> bt;
};
std::vector<HeRuMap> m_staRuInfo;

std::vector<std::pair<Mac48Address,int>> m_UlSuccessStas;
int bsrpCand;
int bsrpSucc;

std::vector<float> m_wasteRuRates;
int m_wasteRuCount;
int m_dlCount = -1;
int m_slot = 4;
float m_succRate = 0.0;

std::vector<Mac48Address> m_qosNullStas;
int m_BARu;
int m_sumByte;
int m_maxByte;

// ----- END: MY CODE -----

TypeId
HeFrameExchangeManager::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HeFrameExchangeManager")
    .SetParent<VhtFrameExchangeManager> ()
    .AddConstructor<HeFrameExchangeManager> ()
    .SetGroupName ("Wifi")
  ;
  return tid;
}

HeFrameExchangeManager::HeFrameExchangeManager ()
  : m_triggerFrameInAmpdu (false)
{
  NS_LOG_FUNCTION (this);
}

HeFrameExchangeManager::~HeFrameExchangeManager ()
{
  NS_LOG_FUNCTION_NOARGS ();
}

uint16_t
HeFrameExchangeManager::GetSupportedBaBufferSize (void) const
{
  NS_ASSERT (m_mac->GetHeConfiguration ());
  if (m_mac->GetHeConfiguration ()->GetMpduBufferSize () > 64)
    {
      return 256;
    }
  return 64;
}

void
HeFrameExchangeManager::SetWifiMac (const Ptr<WifiMac> mac)
{
  m_apMac = DynamicCast<ApWifiMac> (mac);
  m_staMac = DynamicCast<StaWifiMac> (mac);
  VhtFrameExchangeManager::SetWifiMac (mac);
}

void
HeFrameExchangeManager::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_apMac = 0;
  m_staMac = 0;
  m_psduMap.clear ();
  m_txParams.Clear ();
  m_muScheduler = 0;
  m_multiStaBaEvent.Cancel ();
  VhtFrameExchangeManager::DoDispose ();
}

void
HeFrameExchangeManager::SetMultiUserScheduler (const Ptr<MultiUserScheduler> muScheduler)
{
  NS_ASSERT (m_mac);
  NS_ABORT_MSG_IF (!m_apMac,
                   "A Multi-User Scheduler can only be aggregated to an AP");
  NS_ABORT_MSG_IF (!m_apMac->GetHeConfiguration (),
                   "A Multi-User Scheduler can only be aggregated to an HE AP");
  m_muScheduler = muScheduler;
}

bool
HeFrameExchangeManager::StartFrameExchange (Ptr<QosTxop> edca, Time availableTime, bool initialFrame)
{
  NS_LOG_FUNCTION (this << edca << availableTime << initialFrame);

  MultiUserScheduler::TxFormat txFormat = MultiUserScheduler::SU_TX;
  Ptr<const WifiMpdu> mpdu;
 

  /*
   * We consult the Multi-user Scheduler (if available) to know the type of transmission to make if:
   * - there is no pending BlockAckReq to transmit
   * - either the AC queue is empty (the scheduler might select an UL MU transmission)
   *   or the next frame in the AC queue is a non-broadcast QoS data frame addressed to
   *   a receiver with which a BA agreement has been already established
   */
  
  if (m_muScheduler
      && !edca->GetBaManager ()->GetBar (false)
      && (!(mpdu = edca->PeekNextMpdu (m_linkId))
          || (mpdu->GetHeader ().IsQosData ()
              && !mpdu->GetHeader ().GetAddr1 ().IsGroup ()
              && edca->GetBaAgreementEstablished (mpdu->GetHeader ().GetAddr1 (), mpdu->GetHeader ().GetQosTid ()))))
    {
      txFormat = m_muScheduler->NotifyAccessGranted (edca, availableTime, initialFrame, m_allowedWidth);
    }
  //BEGIN: CHECK SU_TX
  // std::cout << "!edca->getBAmanager()->GetBar(false):" << !edca->GetBaManager ()->GetBar (false) 
  //           << ".!(mpdu = edca->PeekNextMpdu (m_linkId)):" << (mpdu == 0)  
  //           << ".mpdu->GetHeader ().IsQosData ()" << mpdu->GetHeader ().IsQosData () 
  //           << ".!mpdu->GetHeader ().GetAddr1 ().IsGroup ()"<< !mpdu->GetHeader ().GetAddr1 ().IsGroup ()
  //           <<".edca->GetBaAgreementEstablished (mpdu->GetHeader ().GetAddr1 (), mpdu->GetHeader ().GetQosTid ())" <<edca->GetBaAgreementEstablished (mpdu->GetHeader ().GetAddr1 (), mpdu->GetHeader ().GetQosTid ()) <<std::endl;

  //END: CHECK SU_TX
  std::cout << "m_self:" << m_self << std::endl;
  std::cout << "availableTime:" << availableTime << std::endl;

  if (txFormat == MultiUserScheduler::SU_TX)
    {
      // std::cout << "!edca->getBAmanager()->GetBar(false):" << !edca->GetBaManager ()->GetBar (false) << ".!(mpdu = edca->PeekNextMpdu (m_linkId)):" <<!(mpdu = edca->PeekNextMpdu (m_linkId))  << std::endl; 
  
      std::cout << "Time:" << Simulator::Now() << ". SU_TX" << std::endl;
      return VhtFrameExchangeManager::StartFrameExchange (edca, availableTime, initialFrame);
    }

  if (txFormat == MultiUserScheduler::DL_MU_TX)
    {
      //BEGIN: MY CODE
      m_dlCount++;
      
      if(m_candidate > m_max_candidate)
      {
        m_max_candidate = m_candidate;
      }
      
      //Sub BEGIN: My Proposed
      //check rate of success sta;
      // int unSuccessNum=0;
      auto ul_ptr = std::max_element(m_UlSuccessStas.begin(),m_UlSuccessStas.end(),[](const auto& a, const auto& b) {
        return a.second < b.second;
      });
      int maxByte = 0;
      if(ul_ptr != m_UlSuccessStas.end()) maxByte = ul_ptr->second;
      int padding=0;
      int sumByte=0;
      for(const auto&sta : m_UlSuccessStas)
      {
        // if(sta.sCount*1.0/sta.cCount <= 0.2)
        // {
        //   unSuccessNum++;
        // }
        // unSuccessNum += sta.sCount;
        padding += maxByte - sta.second;
        sumByte += sta.second;
      }
      


      
      std::ofstream writting;
      std::stringstream filename;
      filename << "./data/CountOfPadding.csv";
      std::string status = "normal";
      writting.open(filename.str(), std::ios::app);
      
      //if unSuccessNum > 20 ? switch UONRA or MBTA
      float nSTA = 100.0;
      // UONRA
      if(m_UlSuccessStas.size()>0)
      {
        if(sumByte*1.0/(maxByte*GetNRuForUl()) < (5472000+36000+260000)*1.0/(5472000+36000+260000+476000))
        {
          // m_muScheduler->SetEnableBsrp(true);
          status =  "maybe bsrp";
        }
        else if(m_isbsrp)
        {
          status = "uonra";

        }
        else
        {
          // m_muScheduler->SetEnableBsrp(false);
          status = "uora";
        }
      }
      

      // MBTA
      
      // if((unSuccessNum/nSTA) - m_succRate <=0.2 && !m_muScheduler->isEnableBsrp())
      // {
      //   m_slot = 4;
      //   status = "MBTA";
      //   m_muScheduler->SwitchRuAssignMode(true);
      // }
      // else
      // {
      //   m_slot=0;
      //   m_muScheduler->SwitchRuAssignMode(false);
      // }
      // m_succRate = (unSuccessNum/nSTA);

      writting << (m_dlCount) << "," << padding << "," << status << "," << m_UlSuccessStas.size() << ","<< m_ruNum << "," << padding*1.0/m_UlSuccessStas.size() << "," << sumByte  << "," << maxByte*GetNRuForUl()<< "," << sumByte*1.0/(maxByte*GetNRuForUl()) << std::endl;
      writting.close();

      
      //Sub End: My Proposed
      std::cout << "Time:" << Simulator::Now() << ". DL_MU_TX" << std::endl;
      std::cout << "empty:" << m_muScheduler->GetDlMuInfo ().psduMap.empty () << std::endl;
      m_candidate = 0;
      // m_isbsrp = false; // added 10/28
      m_staRuInfo.clear();
      if(bsrpCand!=bsrpSucc)
      {
        std::cout << "Loss in BSRP!! BsrpCand:" << bsrpCand << ". BsrpSucc:" << bsrpSucc << std::endl;
      }
      else
      {
        std::cout << "successBsrp count" << bsrpCand << std::endl;
      }
      bsrpCand=0;
      bsrpSucc=0;
      for(const auto &s : m_UlSuccessStas)
      {
        SetUlSuccesses(s.first);
      }
      int wrc  = m_ruNum - m_UlSuccessStas.size();
      m_wasteRuCount += wrc;
      if(m_ruNum >0) m_wasteRuRates.push_back(wrc/m_ruNum);
      // m_UlSuccessStas.clear();
      m_bsrpSuccessNum = 0;
      m_isArbi = true;
      //END: MY CODE
      if (m_muScheduler->GetDlMuInfo ().psduMap.empty ())
        {
          NS_LOG_DEBUG ("The Multi-user Scheduler returned DL_MU_TX with empty psduMap, do not transmit");
          return false;
        }

      SendPsduMapWithProtection (m_muScheduler->GetDlMuInfo ().psduMap,
                                 m_muScheduler->GetDlMuInfo ().txParams);
      return true;
    }

  if (txFormat == MultiUserScheduler::UL_MU_TX)
    {
      auto packet = Create<Packet> ();
      //BEGIN: MY CODE
      std::cout << "Time:" << Simulator::Now() << ". UL_MU_TX" << std::endl;
      CtrlTriggerHeader& trigger_ptr = m_muScheduler->GetUlMuInfo ().trigger;
      (!m_isbsrp) ? m_numBasic++ : m_numBsrp++;
      trigger_ptr.SetMbtaIndicator(m_isArbi);
      trigger_ptr.SetArbitrationSlots(m_slot);
      m_muScheduler->SetIsDoneUl(false);
      //END: MY CODE
      
      packet->AddHeader (m_muScheduler->GetUlMuInfo ().trigger);
      auto trigger = Create<WifiMpdu> (packet, m_muScheduler->GetUlMuInfo ().macHdr);
      SendPsduMapWithProtection (WifiPsduMap {{SU_STA_ID, GetWifiPsdu (trigger,
                                                                       m_muScheduler->GetUlMuInfo ().txParams.m_txVector)}},
                                 m_muScheduler->GetUlMuInfo ().txParams);
      return true;
    }

  return false;
}

bool
HeFrameExchangeManager::SendMpduFromBaManager (Ptr<QosTxop> edca, Time availableTime, bool initialFrame)
{
  NS_LOG_FUNCTION (this << edca << availableTime << initialFrame);
  std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << std::endl; 
  // First, check if there is a BAR to be transmitted
  Ptr<const WifiMpdu> peekedItem = edca->GetBaManager ()->GetBar (false);
  
  if (!peekedItem )
    {
      NS_LOG_DEBUG ("Block Ack Manager returned no frame to send");
      return false;
    }
  //BEGIN: log for
  std::cout << __func__ << ". type:" << peekedItem->GetHeader().GetTypeString() 
           << ". To:" << peekedItem->GetHeader().GetAddr1() << ". From:" << peekedItem->GetHeader().GetAddr2() 
           << ". byte:" << peekedItem->GetPacket()->GetSize() <<std::endl;  
  //END: log for
  if (peekedItem->GetHeader ().IsBlockAckReq ())
    {
      // BlockAckReq are handled by the HT FEM
      return HtFrameExchangeManager::SendMpduFromBaManager (edca, availableTime, initialFrame);
    }

  NS_ASSERT (peekedItem->GetHeader ().IsTrigger ());
  m_triggerFrame = Copy (edca->GetBaManager ()->GetBar ());

  SendPsduMap ();
  return true;
}

void
HeFrameExchangeManager::SendPsduMapWithProtection (WifiPsduMap psduMap, WifiTxParameters& txParams)
{
  NS_LOG_FUNCTION (this << &txParams);
  std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << std::endl;
    for(auto itr = psduMap.begin();itr!=psduMap.end();itr++){
      std::cout << "type:"<< itr->second->GetHeader(0).GetTypeString() << ". to:" << itr->second->GetAddr1() 
                << ". sender:" << m_self << ". byte = " << itr->second->GetPacket()->GetSize() << std::endl; 
    }
  m_psduMap = std::move (psduMap);
  m_txParams = std::move (txParams);
  //BEGIN: log for
  
  //END: log for

#ifdef NS3_BUILD_PROFILE_DEBUG
  // If protection is required, the MPDUs must be stored in some queue because
  // they are not put back in a queue if the MU-RTS/CTS exchange fails
  if (m_txParams.m_protection->method != WifiProtection::NONE)
    {
      for (const auto& psdu : psduMap)
        {
          for (const auto& mpdu : *PeekPointer (psdu.second))
            {
              NS_ASSERT (mpdu->GetHeader ().IsCtl () || !mpdu->GetHeader ().HasData () || mpdu->IsQueued ());
            }
        }
    }
#endif

  // Make sure that the acknowledgment time has been computed, so that SendMuRts()
  // can reuse this value.
  NS_ASSERT (m_txParams.m_acknowledgment);

  if (m_txParams.m_acknowledgment->acknowledgmentTime == Time::Min ())
    {
      CalculateAcknowledgmentTime (m_txParams.m_acknowledgment.get ());
    }

  // Set QoS Ack policy
  for (auto& psdu : m_psduMap)
    {
      WifiAckManager::SetQosAckPolicy (psdu.second, m_txParams.m_acknowledgment.get ());
    }

  if (m_txParams.m_protection->method == WifiProtection::RTS_CTS)
    {
      NS_ABORT_MSG_IF (m_psduMap.size () > 1, "Cannot use RTS/CTS with MU PPDUs");
      SendRts (m_txParams);
    }
  else if (m_txParams.m_protection->method == WifiProtection::NONE)
    {
      SendPsduMap ();
    }
  else
    {
      NS_ABORT_MSG ("Unknown or prohibited protection type: " << m_txParams.m_protection.get ());
    }
}

Ptr<WifiPsdu>
HeFrameExchangeManager::GetPsduTo (Mac48Address to, const WifiPsduMap& psduMap)
{
  auto it = std::find_if (psduMap.begin (), psduMap.end (),
                          [&to] (std::pair<uint16_t, Ptr<WifiPsdu>> psdu)
                            { return psdu.second->GetAddr1 () == to; });
  if (it != psduMap.end ())
    {
      return it->second;
    }
  return nullptr;
}

void
HeFrameExchangeManager::CtsTimeout (Ptr<WifiMpdu> rts, const WifiTxVector& txVector)
{
  NS_LOG_FUNCTION (this << *rts << txVector);

  if (m_psduMap.empty ())
    {
      // A CTS Timeout occurred when protecting a single PSDU that is not included
      // in a DL MU PPDU is handled by the parent classes
      VhtFrameExchangeManager::CtsTimeout (rts, txVector);
      return;
    }

  NS_ABORT_MSG_IF (m_psduMap.size () > 1, "RTS/CTS cannot be used to protect an MU PPDU");
  DoCtsTimeout (m_psduMap.begin ()->second);
  m_psduMap.clear ();
}

void
HeFrameExchangeManager::SendPsduMap (void)
{
  NS_LOG_FUNCTION (this);

  NS_ASSERT (m_txParams.m_acknowledgment);
  NS_ASSERT (!m_txTimer.IsRunning ());

  WifiTxTimer::Reason timerType = WifiTxTimer::NOT_RUNNING;  // no timer
  WifiTxVector* responseTxVector = nullptr;
  Ptr<WifiMpdu> mpdu = nullptr;
  Ptr<WifiPsdu> psdu = nullptr;
  WifiTxVector txVector;
  std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << "m_self:" << m_self << std::endl;
  // Compute the type of TX timer to set depending on the acknowledgment method

  /*
   * Acknowledgment via a sequence of BlockAckReq and BlockAck frames
   */
  if (m_txParams.m_acknowledgment->method == WifiAcknowledgment::DL_MU_BAR_BA_SEQUENCE)
    {
      WifiDlMuBarBaSequence* acknowledgment = static_cast<WifiDlMuBarBaSequence*> (m_txParams.m_acknowledgment.get ());

      // schedule the transmission of required BlockAckReq frames
      for (const auto& psdu : m_psduMap)
        {
          if (acknowledgment->stationsSendBlockAckReqTo.find (psdu.second->GetAddr1 ())
              != acknowledgment->stationsSendBlockAckReqTo.end ())
            {
              // the receiver of this PSDU will receive a BlockAckReq
              std::set<uint8_t> tids = psdu.second->GetTids ();
              NS_ABORT_MSG_IF (tids.size () > 1, "Acknowledgment method incompatible with a Multi-TID A-MPDU");
              uint8_t tid = *tids.begin ();

              NS_ASSERT (m_edca);
              m_edca->ScheduleBar (m_mac->GetQosTxop (tid)->PrepareBlockAckRequest (psdu.second->GetAddr1 (), tid));
            }
        }

      if (!acknowledgment->stationsReplyingWithNormalAck.empty ())
        {
          // a station will reply immediately with a Normal Ack
          timerType = WifiTxTimer::WAIT_NORMAL_ACK_AFTER_DL_MU_PPDU;
          responseTxVector = &acknowledgment->stationsReplyingWithNormalAck.begin ()->second.ackTxVector;
          psdu = GetPsduTo (acknowledgment->stationsReplyingWithNormalAck.begin ()->first, m_psduMap);
          NS_ASSERT (psdu->GetNMpdus () == 1);
          mpdu = *psdu->begin ();
        }
      else if (!acknowledgment->stationsReplyingWithBlockAck.empty ())
        {
          // a station will reply immediately with a Block Ack
          timerType = WifiTxTimer::WAIT_BLOCK_ACK;
          responseTxVector = &acknowledgment->stationsReplyingWithBlockAck.begin ()->second.blockAckTxVector;
          psdu = GetPsduTo (acknowledgment->stationsReplyingWithBlockAck.begin ()->first, m_psduMap);
        }
      // else no station will reply immediately
    }
  /*
   * Acknowledgment via a MU-BAR Trigger Frame sent as single user frame
   */
  else if (m_txParams.m_acknowledgment->method == WifiAcknowledgment::DL_MU_TF_MU_BAR)
    {
      WifiDlMuTfMuBar* acknowledgment = static_cast<WifiDlMuTfMuBar*> (m_txParams.m_acknowledgment.get ());

      if (!m_triggerFrame)
        {
          // we are transmitting the DL MU PPDU and have to schedule the
          // transmission of a MU-BAR Trigger Frame.
          // Create a TRIGVECTOR by "merging" all the BlockAck TXVECTORs
          std::map<uint16_t, CtrlBAckRequestHeader> recipients;

          NS_ASSERT (!acknowledgment->stationsReplyingWithBlockAck.empty ());
          auto staIt = acknowledgment->stationsReplyingWithBlockAck.begin ();
          m_trigVector = staIt->second.blockAckTxVector;
          while (staIt != acknowledgment->stationsReplyingWithBlockAck.end ())
            {
              NS_ASSERT (m_apMac);
              uint16_t staId = m_apMac->GetAssociationId (staIt->first, m_linkId);

              m_trigVector.SetHeMuUserInfo (staId, staIt->second.blockAckTxVector.GetHeMuUserInfo (staId));
              recipients.emplace (staId, staIt->second.barHeader);

              staIt++;
            }
          // set the Length field of the response TXVECTOR, which is needed to correctly
          // set the UL Length field of the MU-BAR Trigger Frame
          m_trigVector.SetLength (acknowledgment->ulLength);

          NS_ASSERT (m_edca);
          m_edca->ScheduleBar (PrepareMuBar (m_trigVector, recipients));
        }
      else
        {
          // we are transmitting the MU-BAR following the DL MU PPDU after a SIFS.
          // m_psduMap and m_txParams are still the same as when the DL MU PPDU was sent.
          // record the set of stations expected to send a BlockAck frame
          m_staExpectTbPpduFrom.clear ();
          for (auto& station : acknowledgment->stationsReplyingWithBlockAck)
            {
              m_staExpectTbPpduFrom.insert (station.first);
            }

          Ptr<WifiPsdu> triggerPsdu = GetWifiPsdu (m_triggerFrame, acknowledgment->muBarTxVector);
          Time txDuration = m_phy->CalculateTxDuration (triggerPsdu->GetSize (),
                                                        acknowledgment->muBarTxVector,
                                                        m_phy->GetPhyBand ());
          // update acknowledgmentTime to correctly set the Duration/ID
          acknowledgment->acknowledgmentTime -= (m_phy->GetSifs () + txDuration);
          m_triggerFrame->GetHeader ().SetDuration (GetPsduDurationId (txDuration, m_txParams));

          responseTxVector = &acknowledgment->stationsReplyingWithBlockAck.begin ()->second.blockAckTxVector;
          Time timeout = txDuration + m_phy->GetSifs () + m_phy->GetSlot ()
                         + m_phy->CalculatePhyPreambleAndHeaderDuration (*responseTxVector);

          m_txTimer.Set (WifiTxTimer::WAIT_BLOCK_ACKS_IN_TB_PPDU, timeout,
                         &HeFrameExchangeManager::BlockAcksInTbPpduTimeout, this, &m_psduMap,
                         &m_staExpectTbPpduFrom, m_staExpectTbPpduFrom.size ());
          m_channelAccessManager->NotifyAckTimeoutStartNow (timeout);

          ForwardPsduDown (triggerPsdu, acknowledgment->muBarTxVector);

          // Pass TRIGVECTOR to HE PHY (equivalent to PHY-TRIGGER.request primitive)
          auto hePhy = StaticCast<HePhy> (m_phy->GetPhyEntity (WIFI_MOD_CLASS_HE));
          hePhy->SetTrigVector (m_trigVector, timeout);

          return;
        }
    }
  /*
   * Acknowledgment requested by MU-BAR TFs aggregated to PSDUs in the DL MU PPDU
   */
  else if (m_txParams.m_acknowledgment->method == WifiAcknowledgment::DL_MU_AGGREGATE_TF)
    {
      WifiDlMuAggregateTf* acknowledgment = static_cast<WifiDlMuAggregateTf*> (m_txParams.m_acknowledgment.get ());

      // record the set of stations expected to send a BlockAck frame
      m_staExpectTbPpduFrom.clear ();

      m_trigVector = acknowledgment->stationsReplyingWithBlockAck.begin ()->second.blockAckTxVector;

      for (auto& station : acknowledgment->stationsReplyingWithBlockAck)
        {
          m_staExpectTbPpduFrom.insert (station.first);
          // check that the station that is expected to send a BlockAck frame is
          // actually the receiver of a PSDU
          auto psduMapIt = std::find_if (m_psduMap.begin (), m_psduMap.end (),
                                         [&station] (std::pair<uint16_t, Ptr<WifiPsdu>> psdu)
                                           { return psdu.second->GetAddr1 () == station.first; });

          NS_ASSERT (psduMapIt != m_psduMap.end ());
          // add a MU-BAR Trigger Frame to the PSDU
          std::vector<Ptr<WifiMpdu>> mpduList (psduMapIt->second->begin (), psduMapIt->second->end ());
          NS_ASSERT (mpduList.size () == psduMapIt->second->GetNMpdus ());
          // set the Length field of the response TXVECTOR, which is needed to correctly
          // set the UL Length field of the MU-BAR Trigger Frame
          station.second.blockAckTxVector.SetLength (acknowledgment->ulLength);
          mpduList.push_back (PrepareMuBar (station.second.blockAckTxVector,
                                            {{psduMapIt->first, station.second.barHeader}}));
          psduMapIt->second = Create<WifiPsdu> (std::move (mpduList));
          m_trigVector.SetHeMuUserInfo (psduMapIt->first,
                                        station.second.blockAckTxVector.GetHeMuUserInfo (psduMapIt->first));
        }

      timerType = WifiTxTimer::WAIT_BLOCK_ACKS_IN_TB_PPDU;
      responseTxVector = &acknowledgment->stationsReplyingWithBlockAck.begin ()->second.blockAckTxVector;
      m_trigVector.SetLength (acknowledgment->ulLength);
    }
  /*
   * Basic Trigger Frame starting an UL MU transmission
   */
  else if (m_txParams.m_acknowledgment->method == WifiAcknowledgment::UL_MU_MULTI_STA_BA)
    {
      // the PSDU map being sent must contain a (Basic) Trigger Frame
      NS_ASSERT (m_psduMap.size () == 1 && m_psduMap.begin ()->first == SU_STA_ID
                 && (mpdu = *m_psduMap.begin ()->second->begin ())->GetHeader ().IsTrigger ());
      std::cout << "Time:" << Simulator::Now() << ". BASIC TF" << std::endl;

      WifiUlMuMultiStaBa* acknowledgment = static_cast<WifiUlMuMultiStaBa*> (m_txParams.m_acknowledgment.get ());

      // record the set of stations solicited by this Trigger Frame
      m_staExpectTbPpduFrom.clear ();

      for (const auto& station : acknowledgment->stationsReceivingMultiStaBa)
        {
          m_staExpectTbPpduFrom.insert (station.first.first);
        }

      // Reset stationsReceivingMultiStaBa, which will be filled as soon as
      // TB PPDUs are received
      acknowledgment->stationsReceivingMultiStaBa.clear ();
      acknowledgment->baType.m_bitmapLen.clear ();

      // Add a SIFS and the TB PPDU duration to the acknowledgment time of the
      // Trigger Frame, so that its Duration/ID is correctly computed
      NS_ASSERT (m_muScheduler);
      Time tbPpduDuration = HePhy::ConvertLSigLengthToHeTbPpduDuration (m_muScheduler->GetUlMuInfo ().trigger.GetUlLength (),
                                                                        acknowledgment->tbPpduTxVector,
                                                                        m_phy->GetPhyBand ());
      acknowledgment->acknowledgmentTime += m_mac->GetWifiPhy ()->GetSifs ()
                                            + tbPpduDuration;

      timerType = WifiTxTimer::WAIT_TB_PPDU_AFTER_BASIC_TF;
      responseTxVector = &acknowledgment->tbPpduTxVector;
      m_trigVector = GetTrigVector (m_muScheduler->GetUlMuInfo ().trigger);
      
      //BEGIN: MY CODE
      m_BARu=0;
      m_qosNullStas.clear();
      m_UlSuccessStas.clear();
      //END: MY CODE
    }
  /*
   * BSRP Trigger Frame
   */
  else if (m_txParams.m_acknowledgment->method == WifiAcknowledgment::NONE
           && !m_txParams.m_txVector.IsUlMu ()
           && m_psduMap.size () == 1 && m_psduMap.begin ()->first == SU_STA_ID
           && (*m_psduMap.begin ()->second->begin ())->GetHeader ().IsTrigger ())
    {
      CtrlTriggerHeader& trigger = m_muScheduler->GetUlMuInfo ().trigger;
      NS_ASSERT (trigger.IsBsrp ());
      NS_ASSERT (m_apMac);
      //BEGIN: log for
      std::cout << "Time:" << Simulator::Now() << ". BSRP TF" << std::endl;
      m_isbsrp = false;
      //END: log for
      // record the set of stations solicited by this Trigger Frame
      m_staExpectTbPpduFrom.clear ();

      for (const auto& userInfo : trigger)
        {
          auto staIt = m_apMac->GetStaList ().find (userInfo.GetAid12 ());
          NS_ASSERT (staIt != m_apMac->GetStaList ().end ());
          m_staExpectTbPpduFrom.insert (staIt->second);
        }

      // Add a SIFS and the TB PPDU duration to the acknowledgment time of the
      // Trigger Frame, so that its Duration/ID is correctly computed
      WifiNoAck* acknowledgment = static_cast<WifiNoAck*> (m_txParams.m_acknowledgment.get ());
      txVector = trigger.GetHeTbTxVector (trigger.begin ()->GetAid12 ());
      acknowledgment->acknowledgmentTime += m_mac->GetWifiPhy ()->GetSifs ()
                                            + HePhy::ConvertLSigLengthToHeTbPpduDuration (trigger.GetUlLength (),
                                                                                          txVector,
                                                                                          m_phy->GetPhyBand ());

      timerType = WifiTxTimer::WAIT_QOS_NULL_AFTER_BSRP_TF;
      responseTxVector = &txVector;
      m_trigVector = GetTrigVector (m_muScheduler->GetUlMuInfo ().trigger);
    }
  /*
   * TB PPDU solicited by a Basic Trigger Frame
   */
  else if (m_txParams.m_txVector.IsUlMu ()
           && m_txParams.m_acknowledgment->method == WifiAcknowledgment::ACK_AFTER_TB_PPDU)
    {
      NS_ASSERT (m_psduMap.size () == 1);
      timerType = WifiTxTimer::WAIT_BLOCK_ACK_AFTER_TB_PPDU;
      NS_ASSERT (m_staMac && m_staMac->IsAssociated ());
      txVector = GetWifiRemoteStationManager ()->GetBlockAckTxVector (m_psduMap.begin ()->second->GetAddr1 (),
                                                                             m_txParams.m_txVector);
      responseTxVector = &txVector;
    }
  /*
   * QoS Null frames solicited by a BSRP Trigger Frame
   */
  else if (m_txParams.m_txVector.IsUlMu ()
           && m_txParams.m_acknowledgment->method == WifiAcknowledgment::NONE)
    {
      // No response is expected, so do nothing.
    }
  else
    {
      NS_ABORT_MSG ("Unable to handle the selected acknowledgment method ("
                    << m_txParams.m_acknowledgment.get () << ")");
    }

  // create a map of Ptr<const WifiPsdu>, as required by the PHY
  WifiConstPsduMap psduMap;
  for (const auto& psdu : m_psduMap)
    {
      psduMap.emplace (psdu.first, psdu.second);
    }

  Time txDuration;
  if (m_txParams.m_txVector.IsUlMu ())
    {
      txDuration = HePhy::ConvertLSigLengthToHeTbPpduDuration (m_txParams.m_txVector.GetLength (),
                                                               m_txParams.m_txVector,
                                                               m_phy->GetPhyBand ());
    }
  else
    {
      txDuration = m_phy->CalculateTxDuration (psduMap, m_txParams.m_txVector, m_phy->GetPhyBand ());

      // Set Duration/ID
      Time durationId = GetPsduDurationId (txDuration, m_txParams);
      for (auto& psdu : m_psduMap)
        {
          psdu.second->SetDuration (durationId);
        }
    }
  // std::cout << "Function:" << __func__ <<  std::endl;
  // if(m_self != 0) std::cout << "m_self:" << m_self << std::endl;
  // if(timerType != 0) std::cout << "timerType:" << timerType << std::endl;
  if (timerType == WifiTxTimer::NOT_RUNNING)
    {
      std::cout << "Not_RUNNING" << std::endl;
      std::cout << "txDuration:" << txDuration << std::endl;
      if (!m_txParams.m_txVector.IsUlMu ())
        {
          std::cout << "is not ulMU" << std::endl;
          Simulator::Schedule (txDuration, &HeFrameExchangeManager::TransmissionSucceeded, this);
        }
    }
  else
    {
      Time timeout = txDuration + m_phy->GetSifs () + m_phy->GetSlot ()
                     + m_phy->CalculatePhyPreambleAndHeaderDuration (*responseTxVector);
      m_channelAccessManager->NotifyAckTimeoutStartNow (timeout);
     std::cout << "txDuration:" << txDuration << ". sifs:" << m_phy->GetSifs() << ". slot" << m_phy->GetSlot()
               << ". preamble and header:" <<  m_phy->CalculatePhyPreambleAndHeaderDuration (*responseTxVector) << std::endl;
      // start timer
      switch (timerType)
        {
          case WifiTxTimer::WAIT_NORMAL_ACK_AFTER_DL_MU_PPDU:
            NS_ASSERT (mpdu);
            std::cout << "WAIT_NORMAL_ACK_AFTER_DL_MU_PPDU."<< m_self <<  std::endl;
            m_txTimer.Set (timerType, timeout, &HeFrameExchangeManager::NormalAckTimeout,
                          this, mpdu, m_txParams.m_txVector);
            break;
          case WifiTxTimer::WAIT_BLOCK_ACK:
            NS_ASSERT (psdu);
            std::cout << "WAIT_BLOCK_ACK."<< m_self<< std::endl;
            m_txTimer.Set (timerType, timeout, &HeFrameExchangeManager::BlockAckTimeout,
                          this, psdu, m_txParams.m_txVector);
            break;
          case WifiTxTimer::WAIT_BLOCK_ACKS_IN_TB_PPDU:
          std::cout << "WAIT_BLOCK_ACKS_IN_TB_PPDU."<< m_self <<  std::endl;
            m_txTimer.Set (timerType, timeout, &HeFrameExchangeManager::BlockAcksInTbPpduTimeout, this,
                          &m_psduMap, &m_staExpectTbPpduFrom, m_staExpectTbPpduFrom.size ());
            break;
          case WifiTxTimer::WAIT_TB_PPDU_AFTER_BASIC_TF:
          case WifiTxTimer::WAIT_QOS_NULL_AFTER_BSRP_TF:
           std::cout << "WAIT_TB_PPDU_AFTER_BASIC_TF or WAIT_QOS_NULL_AFTER_BSRP_TF."<< m_self <<  std::endl;
            m_txTimer.Set (timerType, timeout, &HeFrameExchangeManager::TbPpduTimeout, this,
                          &m_psduMap, &m_staExpectTbPpduFrom, m_staExpectTbPpduFrom.size ());
            break;
          case WifiTxTimer::WAIT_BLOCK_ACK_AFTER_TB_PPDU:
           std::cout << "WAIT_BLOCK_ACK_AFTER_TB_PPDU."<< m_self <<  std::endl;
            m_txTimer.Set (timerType, timeout, &HeFrameExchangeManager::BlockAckAfterTbPpduTimeout,
                          this, m_psduMap.begin ()->second, m_txParams.m_txVector);
            break;
          default:
            NS_ABORT_MSG ("Unknown timer type: " << timerType);
            break;
        }
    }

  // transmit the map of PSDUs
  ForwardPsduMapDown (psduMap, m_txParams.m_txVector);

  if (timerType == WifiTxTimer::WAIT_BLOCK_ACKS_IN_TB_PPDU
      || timerType == WifiTxTimer::WAIT_TB_PPDU_AFTER_BASIC_TF
      || timerType == WifiTxTimer::WAIT_QOS_NULL_AFTER_BSRP_TF)
    {
      // Pass TRIGVECTOR to HE PHY (equivalent to PHY-TRIGGER.request primitive)
      auto hePhy = StaticCast<HePhy> (m_phy->GetPhyEntity (WIFI_MOD_CLASS_HE));
      hePhy->SetTrigVector (m_trigVector, m_txTimer.GetDelayLeft ());
    }
  else if (timerType == WifiTxTimer::NOT_RUNNING
           && m_txParams.m_txVector.IsUlMu ())
    {
      // clear m_psduMap after sending QoS Null frames following a BSRP Trigger Frame
      Simulator::Schedule (txDuration, &WifiPsduMap::clear, &m_psduMap);
    }
}

void
HeFrameExchangeManager::ForwardPsduMapDown (WifiConstPsduMap psduMap, WifiTxVector& txVector)
{
  NS_LOG_FUNCTION (this << psduMap << txVector);

  for (const auto& psdu : psduMap)
    {
      std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << ". type:" << psdu.second->GetHeader(0).GetTypeString() 
                << ". addr1:"<<  psdu.second->GetAddr1() << ". addr2:" << psdu.second->GetAddr2() <<  ". byte:" << psdu.second->GetPacket()->GetSize();
      if(psdu.second->GetHeader(0).GetType()!= WifiMacType::WIFI_MAC_QOSDATA_NULL) std::cout <<  std::endl;  
      NS_LOG_DEBUG ("Transmitting: [STAID=" << psdu.first << ", " << *psdu.second << "]");
    }
  NS_LOG_DEBUG ("TXVECTOR: " << txVector);
  for (const auto& psdu : psduMap)
    {
      NotifyTxToEdca (psdu.second);
    }
  if (psduMap.size () > 1 || psduMap.begin ()->second->IsAggregate () || psduMap.begin ()->second->IsSingle ())
    {
      txVector.SetAggregation (true);
    }

  m_phy->Send (psduMap, txVector);
}

Ptr<WifiMpdu>
HeFrameExchangeManager::PrepareMuBar (const WifiTxVector& responseTxVector,
                                      std::map<uint16_t, CtrlBAckRequestHeader> recipients) const
{
  NS_LOG_FUNCTION (this << responseTxVector);
  NS_ASSERT (responseTxVector.GetHeMuUserInfoMap ().size () == recipients.size ());
  NS_ASSERT (!recipients.empty ());

  CtrlTriggerHeader muBar (TriggerFrameType::MU_BAR_TRIGGER, responseTxVector);
  SetTargetRssi (muBar);
  // Set the CS Required subfield to true, unless the UL Length subfield is less
  // than or equal to 418 (see Section 26.5.2.5 of 802.11ax-2021)
  muBar.SetCsRequired (muBar.GetUlLength () > 418);

  // Add the Trigger Dependent User Info subfield to every User Info field
  for (auto& userInfo : muBar)
    {
      auto recipientIt = recipients.find (userInfo.GetAid12 ());
      NS_ASSERT (recipientIt != recipients.end ());

      // Store the BAR in the Trigger Dependent User Info subfield
      userInfo.SetMuBarTriggerDepUserInfo (recipientIt->second);
    }

  Ptr<Packet> bar = Create<Packet> ();
  bar->AddHeader (muBar);
  Mac48Address rxAddress;
  // "If the Trigger frame has one User Info field and the AID12 subfield of the
  // User Info contains the AID of a STA, then the RA field is set to the address
  // of that STA". Otherwise, it is set to the broadcast address (Sec. 9.3.1.23 -
  // 802.11ax amendment draft 3.0)
  if (muBar.GetNUserInfoFields () > 1)
    {
      rxAddress = Mac48Address::GetBroadcast ();
    }
  else
    {
      NS_ASSERT (m_apMac);
      rxAddress = m_apMac->GetStaList ().at (recipients.begin ()->first);
    }

  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_TRIGGER);
  hdr.SetAddr1 (rxAddress);
  hdr.SetAddr2 (m_self);
  hdr.SetDsNotTo ();
  hdr.SetDsNotFrom ();
  hdr.SetNoRetry ();
  hdr.SetNoMoreFragments ();

  return Create<WifiMpdu> (bar, hdr);
}

void
HeFrameExchangeManager::CalculateAcknowledgmentTime (WifiAcknowledgment* acknowledgment) const
{
  NS_LOG_FUNCTION (this << acknowledgment);
  NS_ASSERT (acknowledgment);

  /*
   * Acknowledgment via a sequence of BlockAckReq and BlockAck frames
   */
  if (acknowledgment->method == WifiAcknowledgment::DL_MU_BAR_BA_SEQUENCE)
    {
      WifiDlMuBarBaSequence* dlMuBarBaAcknowledgment = static_cast<WifiDlMuBarBaSequence*> (acknowledgment);

      Time duration = Seconds (0);

      // normal ack or implicit BAR policy can be used for (no more than) one receiver
      NS_ABORT_IF (dlMuBarBaAcknowledgment->stationsReplyingWithNormalAck.size ()
                   + dlMuBarBaAcknowledgment->stationsReplyingWithBlockAck.size () > 1);

      if (!dlMuBarBaAcknowledgment->stationsReplyingWithNormalAck.empty ())
        {
          const auto& info = dlMuBarBaAcknowledgment->stationsReplyingWithNormalAck.begin ()->second;
          duration += m_phy->GetSifs ()
                      + m_phy->CalculateTxDuration (GetAckSize (), info.ackTxVector, m_phy->GetPhyBand ());
        }

      if (!dlMuBarBaAcknowledgment->stationsReplyingWithBlockAck.empty ())
        {
          const auto& info = dlMuBarBaAcknowledgment->stationsReplyingWithBlockAck.begin ()->second;
          duration += m_phy->GetSifs ()
                      + m_phy->CalculateTxDuration (GetBlockAckSize (info.baType),
                                                    info.blockAckTxVector, m_phy->GetPhyBand ());
        }

      for (const auto& stations : dlMuBarBaAcknowledgment->stationsSendBlockAckReqTo)
        {
          const auto& info = stations.second;
          duration += m_phy->GetSifs ()
                      + m_phy->CalculateTxDuration (GetBlockAckRequestSize (info.barType),
                                                    info.blockAckReqTxVector, m_phy->GetPhyBand ())
                      + m_phy->GetSifs ()
                      + m_phy->CalculateTxDuration (GetBlockAckSize (info.baType),
                                                    info.blockAckTxVector, m_phy->GetPhyBand ());
        }

      dlMuBarBaAcknowledgment->acknowledgmentTime = duration;
    }
  /*
   * Acknowledgment via a MU-BAR Trigger Frame sent as single user frame
   */
  else if (acknowledgment->method == WifiAcknowledgment::DL_MU_TF_MU_BAR)
    {
      WifiDlMuTfMuBar* dlMuTfMuBarAcknowledgment = static_cast<WifiDlMuTfMuBar*> (acknowledgment);

      Time duration = Seconds (0);

      for (const auto& stations : dlMuTfMuBarAcknowledgment->stationsReplyingWithBlockAck)
        {
          // compute the TX duration of the BlockAck response from this receiver.
          const auto& info = stations.second;
          NS_ASSERT (info.blockAckTxVector.GetHeMuUserInfoMap ().size () == 1);
          uint16_t staId = info.blockAckTxVector.GetHeMuUserInfoMap ().begin ()->first;
          Time currBlockAckDuration = m_phy->CalculateTxDuration (GetBlockAckSize (info.baType),
                                                                  info.blockAckTxVector,
                                                                  m_phy->GetPhyBand (),
                                                                  staId);
          // update the max duration among all the Block Ack responses
          if (currBlockAckDuration > duration)
            {
              duration = currBlockAckDuration;
            }
        }

      // The computed duration may not be coded exactly in the L-SIG length, hence determine
      // the exact duration corresponding to the value that will be coded in this field.
      WifiTxVector& txVector = dlMuTfMuBarAcknowledgment->stationsReplyingWithBlockAck.begin ()->second.blockAckTxVector;
      std::tie (dlMuTfMuBarAcknowledgment->ulLength, duration) = HePhy::ConvertHeTbPpduDurationToLSigLength (duration, txVector, m_phy->GetPhyBand ());

      uint32_t muBarSize = GetMuBarSize (dlMuTfMuBarAcknowledgment->barTypes);
      if (dlMuTfMuBarAcknowledgment->muBarTxVector.GetModulationClass () >= WIFI_MOD_CLASS_VHT)
        {
          // MU-BAR TF will be sent as an S-MPDU
          muBarSize = MpduAggregator::GetSizeIfAggregated (muBarSize, 0);
        }
      dlMuTfMuBarAcknowledgment->acknowledgmentTime = m_phy->GetSifs ()
                                                      + m_phy->CalculateTxDuration (muBarSize,
                                                                                    dlMuTfMuBarAcknowledgment->muBarTxVector,
                                                                                    m_phy->GetPhyBand ())
                                                      + m_phy->GetSifs () + duration;
    }
  /*
   * Acknowledgment requested by MU-BAR TFs aggregated to PSDUs in the DL MU PPDU
   */
  else if (acknowledgment->method == WifiAcknowledgment::DL_MU_AGGREGATE_TF)
    {
      WifiDlMuAggregateTf* dlMuAggrTfAcknowledgment = static_cast<WifiDlMuAggregateTf*> (acknowledgment);

      Time duration = Seconds (0);

      for (const auto& stations : dlMuAggrTfAcknowledgment->stationsReplyingWithBlockAck)
        {
          // compute the TX duration of the BlockAck response from this receiver.
          const auto& info = stations.second;
          NS_ASSERT (info.blockAckTxVector.GetHeMuUserInfoMap ().size () == 1);
          uint16_t staId = info.blockAckTxVector.GetHeMuUserInfoMap ().begin ()->first;
          Time currBlockAckDuration = m_phy->CalculateTxDuration (GetBlockAckSize (info.baType),
                                                                  info.blockAckTxVector,
                                                                  m_phy->GetPhyBand (),
                                                                  staId);
          // update the max duration among all the Block Ack responses
          if (currBlockAckDuration > duration)
            {
              duration = currBlockAckDuration;
            }
        }

      // The computed duration may not be coded exactly in the L-SIG length, hence determine
      // the exact duration corresponding to the value that will be coded in this field.
      WifiTxVector& txVector = dlMuAggrTfAcknowledgment->stationsReplyingWithBlockAck.begin ()->second.blockAckTxVector;
      std::tie (dlMuAggrTfAcknowledgment->ulLength, duration) = HePhy::ConvertHeTbPpduDurationToLSigLength (duration, txVector, m_phy->GetPhyBand ());
      dlMuAggrTfAcknowledgment->acknowledgmentTime = m_phy->GetSifs () + duration;
    }
  /*
   * Basic Trigger Frame starting an UL MU transmission
   */
  else if (acknowledgment->method == WifiAcknowledgment::UL_MU_MULTI_STA_BA)
    {
      WifiUlMuMultiStaBa* ulMuMultiStaBa = static_cast<WifiUlMuMultiStaBa*> (acknowledgment);

      Time duration = m_phy->CalculateTxDuration (GetBlockAckSize (ulMuMultiStaBa->baType),
                                                  ulMuMultiStaBa->multiStaBaTxVector,
                                                  m_phy->GetPhyBand ());
      ulMuMultiStaBa->acknowledgmentTime = m_phy->GetSifs () + duration;
    }
  /*
   * TB PPDU solicired by a Basic or BSRP Trigger Frame
   */
  else if (acknowledgment->method == WifiAcknowledgment::ACK_AFTER_TB_PPDU)
    {
      // The station solicited by the Trigger Frame does not have to account
      // for the actual acknowledgment time since it is given the PPDU duration
      // through the Trigger Frame
      acknowledgment->acknowledgmentTime = Seconds (0);
    }
  else
    {
      VhtFrameExchangeManager::CalculateAcknowledgmentTime (acknowledgment);
    }
}

Time
HeFrameExchangeManager::GetTxDuration (uint32_t ppduPayloadSize, Mac48Address receiver,
                                       const WifiTxParameters& txParams) const
{
  if (!txParams.m_txVector.IsMu ())
    {
      return VhtFrameExchangeManager::GetTxDuration (ppduPayloadSize, receiver, txParams);
    }

  NS_ASSERT_MSG (!txParams.m_txVector.IsDlMu () || m_apMac, "DL MU can be done by an AP");
  NS_ASSERT_MSG (!txParams.m_txVector.IsUlMu () || m_staMac, "UL MU can be done by a STA");

  if (txParams.m_acknowledgment
      && txParams.m_acknowledgment->method == WifiAcknowledgment::DL_MU_AGGREGATE_TF)
    {
      // we need to account for the size of the aggregated MU-BAR Trigger Frame
      WifiDlMuAggregateTf* acknowledgment = static_cast<WifiDlMuAggregateTf*> (txParams.m_acknowledgment.get ());

      const auto& info = acknowledgment->stationsReplyingWithBlockAck.find (receiver);
      NS_ASSERT (info != acknowledgment->stationsReplyingWithBlockAck.end ());

      ppduPayloadSize = MpduAggregator::GetSizeIfAggregated (info->second.muBarSize, ppduPayloadSize);
    }

  uint16_t staId = (txParams.m_txVector.IsDlMu () ? m_apMac->GetAssociationId (receiver, m_linkId)
                                                  : m_staMac->GetAssociationId ());
  Time psduDuration = m_phy->CalculateTxDuration (ppduPayloadSize, txParams.m_txVector,
                                                  m_phy->GetPhyBand (), staId);

  return std::max (psduDuration, txParams.m_txDuration);
}

void
HeFrameExchangeManager::TbPpduTimeout (WifiPsduMap* psduMap,
                                       const std::set<Mac48Address>* staMissedTbPpduFrom,
                                       std::size_t nSolicitedStations)
{
  NS_LOG_FUNCTION (this << psduMap << staMissedTbPpduFrom->size () << nSolicitedStations);

  NS_ASSERT (psduMap);
  NS_ASSERT (psduMap->size () == 1 && psduMap->begin ()->first == SU_STA_ID
              && psduMap->begin ()->second->GetHeader (0).IsTrigger ());

  // This method is called if some station(s) did not send a TB PPDU
  NS_ASSERT (!staMissedTbPpduFrom->empty ());
  NS_ASSERT (m_edca);

  if (staMissedTbPpduFrom->size () == nSolicitedStations)
    {
      // no station replied, the transmission failed
      m_edca->UpdateFailedCw (m_linkId);

      TransmissionFailed ();
    }
  else if (!m_multiStaBaEvent.IsRunning ())
    {
      m_edca->ResetCw (m_linkId);
      TransmissionSucceeded ();
    }

  m_psduMap.clear ();
}

void
HeFrameExchangeManager::BlockAcksInTbPpduTimeout (WifiPsduMap* psduMap,
                                                  const std::set<Mac48Address>* staMissedBlockAckFrom,
                                                  std::size_t nSolicitedStations)
{
  NS_LOG_FUNCTION (this << psduMap << nSolicitedStations);

  NS_ASSERT (psduMap);
  NS_ASSERT (m_txParams.m_acknowledgment
             && (m_txParams.m_acknowledgment->method == WifiAcknowledgment::DL_MU_AGGREGATE_TF
                 || m_txParams.m_acknowledgment->method == WifiAcknowledgment::DL_MU_TF_MU_BAR));

  // This method is called if some station(s) did not send a BlockAck frame in a TB PPDU
  NS_ASSERT (!staMissedBlockAckFrom->empty ());

  bool resetCw;

  if (staMissedBlockAckFrom->size () == nSolicitedStations)
    {
      // no station replied, the transmission failed
      // call ReportDataFailed to increase SRC/LRC
      GetWifiRemoteStationManager ()->ReportDataFailed (*psduMap->begin ()->second->begin ());
      resetCw = false;
    }
  else
    {
      // the transmission succeeded
      resetCw = true;
    }

  m_triggerFrame = nullptr;  // this is strictly needed for DL_MU_TF_MU_BAR only

  for (const auto& sta : *staMissedBlockAckFrom)
    {
      Ptr<WifiPsdu> psdu = GetPsduTo (sta, *psduMap);
      NS_ASSERT (psdu);
      // If the QSRC[AC] or the QLRC[AC] has reached dot11ShortRetryLimit or dot11LongRetryLimit
      // respectively, CW[AC] shall be reset to CWmin[AC] (sec. 10.22.2.2 of 802.11-2016).
      // We should get that psduResetCw is the same for all PSDUs, but the handling of QSRC/QLRC
      // needs to be aligned to the specifications.
      bool psduResetCw;
      MissedBlockAck (psdu, m_txParams.m_txVector, psduResetCw);
      resetCw = resetCw || psduResetCw;
    }

  NS_ASSERT (m_edca);

  if (resetCw)
    {
      m_edca->ResetCw (m_linkId);
    }
  else
    {
      m_edca->UpdateFailedCw (m_linkId);
    }

  if (staMissedBlockAckFrom->size () == nSolicitedStations)
    {
      // no station replied, the transmission failed
      TransmissionFailed ();
    }
  else
    {
      TransmissionSucceeded ();
    }
  m_psduMap.clear ();
}

void
HeFrameExchangeManager::BlockAckAfterTbPpduTimeout (Ptr<WifiPsdu> psdu, const WifiTxVector& txVector)
{
  NS_LOG_FUNCTION (this << *psdu << txVector);

  bool resetCw;

  // call ReportDataFailed to increase SRC/LRC
  GetWifiRemoteStationManager ()->ReportDataFailed (*psdu->begin ());

  MissedBlockAck (psdu, m_txParams.m_txVector, resetCw);

  // This is a PSDU sent in a TB PPDU. An HE STA resumes the EDCA backoff procedure
  // without modifying CW or the backoff counter for the associated EDCAF, after
  // transmission of an MPDU in a TB PPDU regardless of whether the STA has received
  // the corresponding acknowledgment frame in response to the MPDU sent in the TB PPDU
  // (Sec. 10.22.2.2 of 11ax Draft 3.0)
  m_psduMap.clear ();
}

void
HeFrameExchangeManager::NormalAckTimeout (Ptr<WifiMpdu> mpdu, const WifiTxVector& txVector)
{
  NS_LOG_FUNCTION (this << *mpdu << txVector);

  VhtFrameExchangeManager::NormalAckTimeout (mpdu, txVector);

  // If a Normal Ack is missed in response to a DL MU PPDU requiring acknowledgment
  // in SU format, we have to set the Retry flag for all transmitted MPDUs that have
  // not been acknowledged nor discarded and clear m_psduMap since the transmission failed.
  for (auto& psdu : m_psduMap)
    {
      for (auto& mpdu : *PeekPointer (psdu.second))
        {
          if (mpdu->IsQueued ())
            {
              mpdu->GetHeader ().SetRetry ();
            }
        }
    }
  m_psduMap.clear ();
}

void
HeFrameExchangeManager::BlockAckTimeout (Ptr<WifiPsdu> psdu, const WifiTxVector& txVector)
{
  NS_LOG_FUNCTION (this << *psdu << txVector);

  VhtFrameExchangeManager::BlockAckTimeout (psdu, txVector);

  // If a Block Ack is missed in response to a DL MU PPDU requiring acknowledgment
  // in SU format, we have to set the Retry flag for all transmitted MPDUs that have
  // not been acknowledged nor discarded and clear m_psduMap since the transmission failed.
  for (auto& psdu : m_psduMap)
    {
      for (auto& mpdu : *PeekPointer (psdu.second))
        {
          if (mpdu->IsQueued ())
            {
              mpdu->GetHeader ().SetRetry ();
            }
        }
    }
  m_psduMap.clear ();
}

WifiTxVector
HeFrameExchangeManager::GetTrigVector (const CtrlTriggerHeader& trigger) const
{
  WifiTxVector v;
  v.SetPreambleType (WifiPreamble::WIFI_PREAMBLE_HE_TB);
  v.SetChannelWidth (trigger.GetUlBandwidth ());
  v.SetGuardInterval (trigger.GetGuardInterval ());
  v.SetLength (trigger.GetUlLength ());
  for (const auto& userInfoField : trigger)
    {
      v.SetHeMuUserInfo (userInfoField.GetAid12 (),
                         {userInfoField.GetRuAllocation (),
                          HePhy::GetHeMcs (userInfoField.GetUlMcs ()),
                          userInfoField.GetNss ()});
    }
  return v;
}

WifiTxVector
HeFrameExchangeManager::GetHeTbTxVector (CtrlTriggerHeader trigger, Mac48Address triggerSender) const
{
  NS_ASSERT (triggerSender != m_self); //TxPower information is used only by STAs, it is useless for the sending AP (which can directly use CtrlTriggerHeader::GetHeTbTxVector)
  NS_ASSERT (m_staMac);
  uint16_t staId = m_staMac->GetAssociationId ();
  auto userInfoIt = trigger.FindUserInfoWithAid (staId);
  NS_ASSERT (userInfoIt != trigger.end ());

  WifiTxVector v = trigger.GetHeTbTxVector (staId);

  Ptr<HeConfiguration> heConfiguration = m_mac->GetHeConfiguration ();
  NS_ASSERT_MSG (heConfiguration, "This STA has to be an HE station to send an HE TB PPDU");
  v.SetBssColor (heConfiguration->GetBssColor ());

  if (userInfoIt->IsUlTargetRssiMaxTxPower ())
    {
      NS_LOG_LOGIC ("AP requested using the max transmit power (" << m_phy->GetTxPowerEnd () << " dBm)");
      v.SetTxPowerLevel (m_phy->GetNTxPower ());
      return v;
    }

  uint8_t powerLevel = GetWifiRemoteStationManager ()->GetDefaultTxPowerLevel ();
  /**
   * Get the transmit power to use for an HE TB PPDU
   * considering:
   * - the transmit power used by the AP to send the Trigger Frame (TF),
   *   obtained from the AP TX Power subfield of the Common Info field
   *   of the TF.
   * - the target uplink RSSI expected by the AP for the triggered HE TB PPDU,
   *   obtained from the UL Target RSSI subfield of the User Info field
   *   of the TF.
   * - the RSSI of the PPDU containing the TF, typically logged by the
   *   WifiRemoteStationManager upon reception of the TF from the AP.
   *
   * It is assumed that path loss is symmetric (i.e. uplink path loss is
   * equivalent to the measured downlink path loss);
   *
   * Refer to section 27.3.14.2 (Power pre-correction) of 802.11ax Draft 4.0 for more details.
   */
  int8_t pathLossDb = trigger.GetApTxPower () - static_cast<int8_t> (GetWifiRemoteStationManager ()->GetMostRecentRssi (triggerSender)); //cast RSSI to be on equal footing with AP Tx power information
  double reqTxPowerDbm = static_cast<double> (userInfoIt->GetUlTargetRssi () + pathLossDb);

  //Convert the transmit power to a power level
  uint8_t numPowerLevels = m_phy->GetNTxPower ();
  if (numPowerLevels > 1)
    {
      double stepDbm = (m_phy->GetTxPowerEnd () - m_phy->GetTxPowerStart ()) / (numPowerLevels - 1);
      powerLevel = static_cast<uint8_t> (ceil ((reqTxPowerDbm - m_phy->GetTxPowerStart ()) / stepDbm)); //better be slightly above so as to satisfy target UL RSSI
      if (powerLevel > numPowerLevels)
        {
          powerLevel = numPowerLevels; //capping will trigger warning below
        }
    }
  if (reqTxPowerDbm > m_phy->GetPowerDbm (powerLevel))
    {
      NS_LOG_WARN ("The requested power level (" << reqTxPowerDbm << "dBm) cannot be satisfied (max: " << m_phy->GetTxPowerEnd () << "dBm)");
    }
  v.SetTxPowerLevel (powerLevel);
  NS_LOG_LOGIC ("UL power control: "
                << "input {pathLoss=" << pathLossDb << "dB, reqTxPower=" << reqTxPowerDbm << "dBm}"
                << " output {powerLevel=" << +powerLevel << " -> " << m_phy->GetPowerDbm (powerLevel) << "dBm}"
                << " PHY power capa {min=" << m_phy->GetTxPowerStart () << "dBm, max=" << m_phy->GetTxPowerEnd () << "dBm, levels:" << +numPowerLevels << "}");

  return v;
}

void
HeFrameExchangeManager::SetTargetRssi (CtrlTriggerHeader& trigger) const
{
  NS_LOG_FUNCTION (this);
  NS_ASSERT (m_apMac);

  trigger.SetApTxPower (static_cast<int8_t> (m_phy->GetPowerDbm (GetWifiRemoteStationManager ()->GetDefaultTxPowerLevel ())));
  for (auto& userInfo : trigger)
    {
      const auto staList = m_apMac->GetStaList ();
      auto itAidAddr = staList.find (userInfo.GetAid12 ());
      NS_ASSERT (itAidAddr != staList.end ());
      int8_t rssi = static_cast<int8_t> (GetWifiRemoteStationManager ()->GetMostRecentRssi (itAidAddr->second));
      rssi = (rssi >= -20) ? -20 : ((rssi <= -110) ? -110 : rssi); //cap so as to keep within [-110; -20] dBm
      userInfo.SetUlTargetRssi (rssi);
    }
}

void
HeFrameExchangeManager::SendMultiStaBlockAck (const WifiTxParameters& txParams)
{
  NS_LOG_FUNCTION (this << &txParams);

  NS_ASSERT (m_apMac);
  NS_ASSERT (txParams.m_acknowledgment
             && txParams.m_acknowledgment->method == WifiAcknowledgment::UL_MU_MULTI_STA_BA);
  WifiUlMuMultiStaBa* acknowledgment = static_cast<WifiUlMuMultiStaBa*> (txParams.m_acknowledgment.get ());

  NS_ASSERT (!acknowledgment->stationsReceivingMultiStaBa.empty ());

  CtrlBAckResponseHeader blockAck;
  blockAck.SetType (acknowledgment->baType);

  Mac48Address receiver;

  for (const auto& staInfo : acknowledgment->stationsReceivingMultiStaBa)
    {
      receiver = staInfo.first.first;
      uint8_t tid = staInfo.first.second;
      std::size_t index = staInfo.second;

      blockAck.SetAid11 (m_apMac->GetAssociationId (receiver, m_linkId), index);
      blockAck.SetTidInfo (tid, index);

      if (tid == 14)
        {
          // All-ack context
          NS_LOG_DEBUG ("Multi-STA Block Ack: Sending All-ack to=" << receiver);
          blockAck.SetAckType (true, index);
          continue;
        }

      if (acknowledgment->baType.m_bitmapLen.at (index) == 0)
        {
          // Acknowledgment context
          NS_LOG_DEBUG ("Multi-STA Block Ack: Sending Ack to=" << receiver);
          blockAck.SetAckType (true, index);
        }
      else
        {
          // Block acknowledgment context
          blockAck.SetAckType (false, index);

          auto addressTidPair = staInfo.first;
          auto agreementIt = m_agreements.find (addressTidPair);
          NS_ASSERT (agreementIt != m_agreements.end ());
          agreementIt->second.FillBlockAckBitmap (&blockAck, index);
          NS_LOG_DEBUG ("Multi-STA Block Ack: Sending Block Ack with seq=" << blockAck.GetStartingSequence (index)
                        << " to=" << receiver << " tid=" << +tid);
        }
    }

  WifiMacHeader hdr;
  hdr.SetType (WIFI_MAC_CTL_BACKRESP);
  hdr.SetAddr1 (acknowledgment->stationsReceivingMultiStaBa.size () == 1 ? receiver : Mac48Address::GetBroadcast ());
  hdr.SetAddr2 (m_self);
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();

  Ptr<Packet> packet = Create<Packet> ();
  packet->AddHeader (blockAck);
  Ptr<WifiPsdu> psdu = GetWifiPsdu (Create<WifiMpdu> (packet, hdr),
                                    acknowledgment->multiStaBaTxVector);

  // The Duration/ID field in a BlockAck frame transmitted in response to a frame
  // carried in HE TB PPDU is set according to the multiple protection settings
  // (Sec. 9.2.5.7 of 802.11ax D3.0)
  Time txDuration = m_phy->CalculateTxDuration (GetBlockAckSize (acknowledgment->baType),
                                                acknowledgment->multiStaBaTxVector,
                                                m_phy->GetPhyBand ());
  WifiTxParameters params;
  // if the TXOP limit is null, GetPsduDurationId returns the acknowledgment time,
  // hence we set an method with acknowledgment time equal to zero.
  params.m_acknowledgment = std::unique_ptr<WifiAcknowledgment> (new WifiNoAck);
  psdu->SetDuration (GetPsduDurationId (txDuration, params));

  psdu->GetPayload (0)->AddPacketTag (m_muSnrTag);

  ForwardPsduDown (psdu, acknowledgment->multiStaBaTxVector);

  // continue with the TXOP if time remains
  m_psduMap.clear ();
  m_edca->ResetCw (m_linkId);
  m_muSnrTag.Reset ();
  std::cout << "Time:" << Simulator::Now() << ". Function:" << __func__ << ". txDuration:" << txDuration << std::endl;
  Simulator::Schedule (txDuration, &HeFrameExchangeManager::TransmissionSucceeded, this);
}

void
HeFrameExchangeManager::ReceiveBasicTrigger (const CtrlTriggerHeader& trigger, const WifiMacHeader& hdr)
{
  NS_LOG_FUNCTION (this << trigger << hdr);
  NS_ASSERT (trigger.IsBasic ());
  NS_ASSERT (m_staMac && m_staMac->IsAssociated ());

  NS_LOG_DEBUG ("Received a Trigger Frame (basic variant) soliciting a transmission");
  std::cout << "Time:" << Simulator::Now() << ". " << __func__ << std::endl;

  if (trigger.GetCsRequired () && hdr.GetAddr2 () != m_txopHolder && m_navEnd > Simulator::Now ())
    {
      NS_LOG_DEBUG ("Carrier Sensing required and channel busy, do nothing");
      return;
    }

  // Starting from the Preferred AC indicated in the Trigger Frame, check if there
  // is either a pending BlockAckReq frame or a data frame that can be transmitted
  // in the allocated time and is addressed to a station with which a Block Ack
  // agreement has been established.

  // create the sequence of TIDs to check
  std::vector<uint8_t> tids;
  uint16_t staId = m_staMac->GetAssociationId ();
  AcIndex preferredAc = trigger.FindUserInfoWithAid (staId)->GetPreferredAc ();
  auto acIt = wifiAcList.find (preferredAc);
  for (uint8_t i = 0; i < 4; i++)
    {
      NS_ASSERT (acIt != wifiAcList.end ());
      tids.push_back (acIt->second.GetHighTid ());
      tids.push_back (acIt->second.GetLowTid ());

      acIt++;
      if (acIt == wifiAcList.end ())
        {
          acIt = wifiAcList.begin ();
        }
    }

  Ptr<WifiPsdu> psdu;
  WifiTxParameters txParams;
  WifiTxVector tbTxVector = GetHeTbTxVector (trigger, hdr.GetAddr2 ());
  Time ppduDuration = HePhy::ConvertLSigLengthToHeTbPpduDuration (trigger.GetUlLength (),
                                                                  tbTxVector,
                                                                  m_phy->GetPhyBand ());

  for (const auto& tid : tids)
    {
      Ptr<QosTxop> edca = m_mac->GetQosTxop (tid);
      std::cout <<"TID:" << int(tid) << std::endl;
      if (!edca->GetBaAgreementEstablished (hdr.GetAddr2 (), tid))
        {
          // no Block Ack agreement established for this TID
          std::cout <<"sta:"<< m_staMac->GetAddress() << ". no Block Ack agreement established for this TID:" << int(tid) << std::endl;
          continue;
        }

      txParams.Clear ();
      txParams.m_txVector = tbTxVector;

      // first, check if there is a pending BlockAckReq frame
      if (Ptr<const WifiMpdu> mpdu;
          (mpdu = edca->GetBaManager ()->GetBar (false, tid, hdr.GetAddr2 ()))
          && TryAddMpdu (mpdu, txParams, ppduDuration))
        {
          NS_LOG_DEBUG ("Sending a BAR within a TB PPDU");
          std::cout <<"sta:"<< m_staMac->GetAddress() << ". Sending a BAR within a TB PPDU" << std::endl;
          
          psdu = Create<WifiPsdu> (edca->GetBaManager ()->GetBar (true, tid, hdr.GetAddr2 ()), true);
          break;
        }

      // otherwise, check if a suitable data frame is available
      if (Ptr<WifiMpdu> mpdu;
          (mpdu = edca->PeekNextMpdu (m_linkId, tid, hdr.GetAddr2 ())))
        {
          Ptr<WifiMpdu> item = edca->GetNextMpdu (m_linkId, mpdu, txParams, ppduDuration, false);
          std::cout << "peekNextMpdu" << std::endl;
          if (item)
            {
              // try A-MPDU aggregation
              std::cout <<"sta:"<< m_staMac->GetAddress() << ". try A-MPDU aggregation" << std::endl;
              std::vector<Ptr<WifiMpdu>> mpduList = m_mpduAggregator->GetNextAmpdu (item, txParams,
                                                                                            ppduDuration);
              psdu = (mpduList.size () > 1 ? Create<WifiPsdu> (std::move (mpduList))
                                           : Create<WifiPsdu> (item, true));
              break;
            }
        }
    }

  if (psdu)
    {
      psdu->SetDuration (hdr.GetDuration () - m_phy->GetSifs () - ppduDuration);
      SendPsduMapWithProtection (WifiPsduMap {{staId, psdu}}, txParams);
    }
  else
    {
      // send QoS Null frames
      std::cout <<"sta:"<< m_staMac->GetAddress() << ". Send QoS Null Frame" << std::endl;
      SendQosNullFramesInTbPpdu (trigger, hdr);
    }
}

void
HeFrameExchangeManager::SendQosNullFramesInTbPpdu (const CtrlTriggerHeader& trigger, const WifiMacHeader& hdr)
{
  NS_LOG_FUNCTION (this << trigger << hdr);
  NS_ASSERT (trigger.IsBasic () || trigger.IsBsrp ());
  NS_ASSERT (m_staMac && m_staMac->IsAssociated ());

  NS_LOG_DEBUG ("Requested to send QoS Null frames");
  std::cout << "Time:" << Simulator::Now() << ". " << __func__ << std::endl;

  if (trigger.GetCsRequired () && hdr.GetAddr2 () != m_txopHolder && m_navEnd > Simulator::Now ())
    {
      NS_LOG_DEBUG ("Carrier Sensing required and channel busy (TA=" << hdr.GetAddr2 ()
                    << ", TxopHolder=" << m_txopHolder << ", NAV end=" << m_navEnd.As (Time::S)
                    << "), do nothing");
      return;
    }

  WifiMacHeader header;
  header.SetType (WIFI_MAC_QOSDATA_NULL);
  header.SetAddr1 (hdr.GetAddr2 ());
  header.SetAddr2 (m_self);
  header.SetAddr3 (hdr.GetAddr2 ());
  header.SetDsTo ();
  header.SetDsNotFrom ();
  // TR3: Sequence numbers for transmitted QoS (+)Null frames may be set
  // to any value. (Table 10-3 of 802.11-2016)
  header.SetSequenceNumber (0);
  // Set the EOSP bit so that NotifyTxToEdca will add the Queue Size
  header.SetQosEosp ();

  WifiTxParameters txParams;
  txParams.m_txVector = GetHeTbTxVector (trigger, hdr.GetAddr2 ());
  txParams.m_protection = std::unique_ptr<WifiProtection> (new WifiNoProtection);
  txParams.m_acknowledgment = std::unique_ptr<WifiAcknowledgment> (new WifiNoAck);

  Time ppduDuration = HePhy::ConvertLSigLengthToHeTbPpduDuration (trigger.GetUlLength (),
                                                                  txParams.m_txVector,
                                                                  m_phy->GetPhyBand ());
  header.SetDuration (hdr.GetDuration () - m_phy->GetSifs () - ppduDuration);

  Ptr<WifiMpdu> mpdu;
  std::vector<Ptr<WifiMpdu>> mpduList;
  uint8_t tid = 0;
  header.SetQosTid (tid);

  while (tid < 8
         && IsWithinSizeAndTimeLimits (txParams.GetSizeIfAddMpdu (mpdu = Create<WifiMpdu> (Create<Packet> (),
                                                                                                   header)),
                                       hdr.GetAddr2 (), txParams, ppduDuration))
    {
      if (!m_mac->GetQosTxop (tid)->GetBaAgreementEstablished (hdr.GetAddr2 (), tid))
        {
          NS_LOG_DEBUG ("Skipping tid=" << +tid << " because no agreement established");
          header.SetQosTid (++tid);
          continue;
        }

      NS_LOG_DEBUG ("Aggregating a QoS Null frame with tid=" << +tid);
      // We could call TryAddMpdu instead of IsWithinSizeAndTimeLimits above in order to
      // get the TX parameters updated automatically. However, aggregating the QoS Null
      // frames might fail because MPDU aggregation is disabled by default for VO
      // and BK. Therefore, we skip the check on max A-MPDU size and only update the
      // TX parameters below.
      txParams.m_acknowledgment = GetAckManager ()->TryAddMpdu (mpdu, txParams);
      txParams.AddMpdu (mpdu);
      UpdateTxDuration (mpdu->GetHeader ().GetAddr1 (), txParams);
      mpduList.push_back (mpdu);
      header.SetQosTid (++tid);
    }

  if (mpduList.empty ())
    {
      NS_LOG_DEBUG ("Not enough time to send a QoS Null frame");
      return;
    }

  Ptr<WifiPsdu> psdu = (mpduList.size () > 1 ? Create<WifiPsdu> (std::move (mpduList))
                                             : Create<WifiPsdu> (mpduList.front (), true));
  uint16_t staId = m_staMac->GetAssociationId ();
  SendPsduMapWithProtection (WifiPsduMap {{staId, psdu}}, txParams);
}

void
HeFrameExchangeManager::SetTxopHolder (Ptr<const WifiPsdu> psdu, const WifiTxVector& txVector)
{
  NS_LOG_FUNCTION (this << psdu << txVector);

  if (psdu->GetHeader (0).IsTrigger ())
    {
      m_txopHolder = psdu->GetAddr2 ();
    }
  else if (!txVector.IsUlMu ())  // the sender of a TB PPDU is not the TXOP holder
    {
      VhtFrameExchangeManager::SetTxopHolder (psdu, txVector);
    }
}

void
HeFrameExchangeManager::ReceiveMpdu (Ptr<const WifiMpdu> mpdu, RxSignalInfo rxSignalInfo,
                                     const WifiTxVector& txVector, bool inAmpdu)
{
  //BEGIN: MY CODE
  if(m_txTimer.IsRunning()){
    
    std::cout << "Time:" << Simulator::Now()<<". " << __func__ << std::endl;
    std::cout << "type: " << mpdu->GetHeader().GetTypeString() << std::endl;
    std::cout << "txTimer: " << m_txTimer.GetReasonString(m_txTimer.GetReason()) << std::endl;
    
    if(m_txTimer.GetReason()==WifiTxTimer::WAIT_TB_PPDU_AFTER_BASIC_TF)
    {
      if(mpdu->GetHeader().GetType()==WifiMacType::WIFI_MAC_QOSDATA)
      {
        uint16_t staId = txVector.GetHeMuUserInfoMap ().begin ()->first;
        Mac48Address sender = mpdu->GetHeader().GetAddr2();
        std::cout << "sender:" << sender << ". byte = " << mpdu->GetPacketSize() << ". Recorde!" << std::endl;
        UpdateSuccesses(sender,mpdu->GetPacketSize());
        SearchUlSuccessSta(sender);
        AddUlSuccessStaSize(sender,mpdu->GetPacketSize());
        m_muScheduler->SetIsDoneUl(true);
        int queueSize = std::ceil (mpdu->GetPacketSize() / 256.0);
        m_muScheduler->UpdateBsr(staId,-1*queueSize);
      }
      else if(mpdu->GetHeader().GetType()==WifiMacType::WIFI_MAC_QOSDATA_NULL)
      {
        uint16_t staId = txVector.GetHeMuUserInfoMap ().begin ()->first;
        m_qosNullStas.push_back(mpdu->GetHeader().GetAddr2());
        m_muScheduler->UpdateBsr(staId, int(mpdu->GetHeader().GetQosQueueSize ()));
      }
      else
      {
        m_BARu++;
        std::cout << "m_BARu!" << std::endl;
      }
      
    }
    else if(m_txTimer.GetReason()==WifiTxTimer::WAIT_QOS_NULL_AFTER_BSRP_TF && mpdu->GetHeader().GetType()==WifiMacType::WIFI_MAC_QOSDATA_NULL)
    {
      uint16_t staId = txVector.GetHeMuUserInfoMap ().begin ()->first;
      SetQosNullReceived(mpdu->GetHeader().GetAddr2());
      m_muScheduler->UpdateBsr(staId, int(mpdu->GetHeader().GetQosQueueSize ()));
      bsrpSucc++;
    }
    else
    {
      std::cout << "sender:" << mpdu->GetHeader().GetAddr2() << ". receiver: " << m_self << ". byte = " << mpdu->GetPacketSize() << std::endl;
      
    }
  }
  //END: MY CODE
  // The received MPDU is either broadcast or addressed to this station
  NS_ASSERT (mpdu->GetHeader ().GetAddr1 ().IsGroup ()
             || mpdu->GetHeader ().GetAddr1 () == m_self);

  const WifiMacHeader& hdr = mpdu->GetHeader ();

  if (txVector.IsUlMu () && m_txTimer.IsRunning ()
      && m_txTimer.GetReason () == WifiTxTimer::WAIT_TB_PPDU_AFTER_BASIC_TF)
    {
      Mac48Address sender = hdr.GetAddr2 ();
      NS_ASSERT (m_txParams.m_acknowledgment
                  && m_txParams.m_acknowledgment->method == WifiAcknowledgment::UL_MU_MULTI_STA_BA);
      WifiUlMuMultiStaBa* acknowledgment = static_cast<WifiUlMuMultiStaBa*> (m_txParams.m_acknowledgment.get ());
      std::size_t index = acknowledgment->baType.m_bitmapLen.size ();

      if (m_staExpectTbPpduFrom.find (sender) == m_staExpectTbPpduFrom.end ())
        {
          NS_LOG_WARN ("Received a TB PPDU from an unexpected station: " << sender);
          return;
        }

      if (hdr.IsBlockAckReq ())
        {
          NS_LOG_DEBUG ("Received a BlockAckReq in a TB PPDU from " << sender);

          CtrlBAckRequestHeader blockAckReq;
          mpdu->GetPacket ()->PeekHeader (blockAckReq);
          NS_ABORT_MSG_IF (blockAckReq.IsMultiTid (), "Multi-TID BlockAckReq not supported");
          uint8_t tid = blockAckReq.GetTidInfo ();
          auto agreementIt = m_agreements.find ({sender, tid});
          NS_ASSERT (agreementIt != m_agreements.end ());
          agreementIt->second.NotifyReceivedBar (blockAckReq.GetStartingSequence ());

          // Block Acknowledgment context
          acknowledgment->stationsReceivingMultiStaBa.emplace (std::make_pair (sender, tid), index);
          acknowledgment->baType.m_bitmapLen.push_back (GetBlockAckType (sender, tid).m_bitmapLen.at (0));
          uint16_t staId = txVector.GetHeMuUserInfoMap ().begin ()->first;
          m_muSnrTag.Set (staId, rxSignalInfo.snr);
        }
      else if (hdr.IsQosData () && !inAmpdu && hdr.GetQosAckPolicy () == WifiMacHeader::NORMAL_ACK)
        {
          NS_LOG_DEBUG ("Received an S-MPDU in a TB PPDU from " << sender << " (" << *mpdu << ")");

          uint8_t tid = hdr.GetQosTid ();
          auto agreementIt = m_agreements.find ({sender, tid});
          NS_ASSERT (agreementIt != m_agreements.end ());
          agreementIt->second.NotifyReceivedMpdu (mpdu);

          // Acknowledgment context of Multi-STA Block Acks
          acknowledgment->stationsReceivingMultiStaBa.emplace (std::make_pair (sender, tid), index);
          acknowledgment->baType.m_bitmapLen.push_back (0);
          uint16_t staId = txVector.GetHeMuUserInfoMap ().begin ()->first;
          m_muSnrTag.Set (staId, rxSignalInfo.snr);
        }
      else if (!(hdr.IsQosData () && !hdr.HasData () && !inAmpdu))
        {
          // The other case handled by this function is when we receive a QoS Null frame
          // that is not in an A-MPDU. For all other cases, the reception is handled by
          // parent classes. In particular, in case of a QoS data frame in A-MPDU, we
          // have to wait until the A-MPDU reception is completed, but we let the
          // parent classes notify the Block Ack agreement of the reception of this MPDU
          VhtFrameExchangeManager::ReceiveMpdu (mpdu, rxSignalInfo, txVector, inAmpdu);
          return;
        }

      // Schedule the transmission of a Multi-STA BlockAck frame if needed
      if (!acknowledgment->stationsReceivingMultiStaBa.empty () && !m_multiStaBaEvent.IsRunning ())
        {
          
          m_multiStaBaEvent = Simulator::Schedule (m_phy->GetSifs (),
                                                   &HeFrameExchangeManager::SendMultiStaBlockAck,
                                                   this, std::cref (m_txParams));
        }

      // remove the sender from the set of stations that are expected to send a TB PPDU
      m_staExpectTbPpduFrom.erase (sender);

      if (m_staExpectTbPpduFrom.empty ())
        {
          // we do not expect any other BlockAck frame
          m_txTimer.Cancel ();
          m_channelAccessManager->NotifyAckTimeoutResetNow ();

          if (!m_multiStaBaEvent.IsRunning ())
            {
              // all of the stations that replied with a TB PPDU sent QoS Null frames.
              NS_LOG_DEBUG ("Continue the TXOP");
              m_psduMap.clear ();
              m_edca->ResetCw (m_linkId);
              TransmissionSucceeded ();
            }
        }

      // the received TB PPDU has been processed
      return;
    }

  if (txVector.IsUlMu () && m_txTimer.IsRunning ()
      && m_txTimer.GetReason () == WifiTxTimer::WAIT_QOS_NULL_AFTER_BSRP_TF
      && !inAmpdu) // if in A-MPDU, processing is done at the end of A-MPDU reception
    {
      Mac48Address sender = hdr.GetAddr2 ();

      if (m_staExpectTbPpduFrom.find (sender) == m_staExpectTbPpduFrom.end ())
        {
          NS_LOG_WARN ("Received a TB PPDU from an unexpected station: " << sender);
          return;
        }
      if (!(hdr.IsQosData () && !hdr.HasData ()))
        {
          NS_LOG_WARN ("No QoS Null frame in the received MPDU");
          return;
        }

      NS_LOG_DEBUG ("Received a QoS Null frame in a TB PPDU from " << sender);

      // remove the sender from the set of stations that are expected to send a TB PPDU
      m_staExpectTbPpduFrom.erase (sender);

      if (m_staExpectTbPpduFrom.empty ())
        {
          // we do not expect any other response
          m_txTimer.Cancel ();
          m_channelAccessManager->NotifyAckTimeoutResetNow ();

          NS_ASSERT (m_edca);
          m_psduMap.clear ();
          m_edca->ResetCw (m_linkId);
          TransmissionSucceeded ();
        }

      // the received TB PPDU has been processed
      return;
    }

  if (hdr.IsCtl ())
    {
      if (hdr.IsCts () && m_txTimer.IsRunning () && m_txTimer.GetReason () == WifiTxTimer::WAIT_CTS
          && m_psduMap.size () == 1)
        {
          NS_ABORT_MSG_IF (inAmpdu, "Received CTS as part of an A-MPDU");
          NS_ASSERT (hdr.GetAddr1 () == m_self);

          Mac48Address sender = m_psduMap.begin ()->second->GetAddr1 ();
          NS_LOG_DEBUG ("Received CTS from=" << sender);

          SnrTag tag;
          mpdu->GetPacket ()->PeekPacketTag (tag);
          GetWifiRemoteStationManager ()->ReportRxOk (sender, rxSignalInfo, txVector);
          GetWifiRemoteStationManager ()->ReportRtsOk (m_psduMap.begin ()->second->GetHeader (0),
                                                              rxSignalInfo.snr, txVector.GetMode (), tag.Get ());

          m_txTimer.Cancel ();
          m_channelAccessManager->NotifyCtsTimeoutResetNow ();
          Simulator::Schedule (m_phy->GetSifs (), &HeFrameExchangeManager::SendPsduMap, this);
        }
      else if (hdr.IsAck () && m_txTimer.IsRunning ()
          && m_txTimer.GetReason () == WifiTxTimer::WAIT_NORMAL_ACK_AFTER_DL_MU_PPDU)
        {
          NS_ASSERT (hdr.GetAddr1 () == m_self);
          NS_ASSERT (m_txParams.m_acknowledgment);
          NS_ASSERT (m_txParams.m_acknowledgment->method == WifiAcknowledgment::DL_MU_BAR_BA_SEQUENCE);

          WifiDlMuBarBaSequence* acknowledgment = static_cast<WifiDlMuBarBaSequence*> (m_txParams.m_acknowledgment.get ());
          NS_ASSERT (acknowledgment->stationsReplyingWithNormalAck.size () == 1);
          NS_ASSERT (m_apMac);
          uint16_t staId = m_apMac->GetAssociationId (acknowledgment->stationsReplyingWithNormalAck.begin ()->first, m_linkId);
          auto it = m_psduMap.find (staId);
          NS_ASSERT (it != m_psduMap.end ());
          NS_ASSERT (it->second->GetAddr1 () == acknowledgment->stationsReplyingWithNormalAck.begin ()->first);
          SnrTag tag;
          mpdu->GetPacket ()->PeekPacketTag (tag);
          ReceivedNormalAck (*it->second->begin (), m_txParams.m_txVector, txVector, rxSignalInfo, tag.Get ());
          m_psduMap.clear ();
        }
      // TODO the PHY should not pass us a non-TB PPDU if we are waiting for a
      // TB PPDU. However, processing the PHY header is done by the PHY entity
      // corresponding to the modulation class of the PPDU being received, hence
      // it is not possible to check if a valid TRIGVECTOR is stored when receiving
      // PPDUs of older modulation classes. Therefore, we check here that we are
      // actually receiving a TB PPDU.
      else if (hdr.IsBlockAck () && txVector.IsUlMu () && m_txTimer.IsRunning ()
               && m_txTimer.GetReason () == WifiTxTimer::WAIT_BLOCK_ACKS_IN_TB_PPDU)
        {
          Mac48Address sender = hdr.GetAddr2 ();
          NS_LOG_DEBUG ("Received BlockAck in TB PPDU from=" << sender);

          SnrTag tag;
          mpdu->GetPacket ()->PeekPacketTag (tag);

          // notify the Block Ack Manager
          CtrlBAckResponseHeader blockAck;
          mpdu->GetPacket ()->PeekHeader (blockAck);
          uint8_t tid = blockAck.GetTidInfo ();
          std::pair<uint16_t,uint16_t> ret = GetBaManager (tid)->NotifyGotBlockAck (blockAck, hdr.GetAddr2 (),
                                                                                    {tid});
          GetWifiRemoteStationManager ()->ReportAmpduTxStatus (hdr.GetAddr2 (), ret.first, ret.second,
                                                                      rxSignalInfo.snr, tag.Get (), m_txParams.m_txVector);

          // remove the sender from the set of stations that are expected to send a BlockAck
          if (m_staExpectTbPpduFrom.erase (sender) == 0)
            {
              NS_LOG_WARN ("Received a BlockAck from an unexpected stations: " << sender);
              return;
            }

          if (m_staExpectTbPpduFrom.empty ())
            {
              // we do not expect any other BlockAck frame
              m_txTimer.Cancel ();
              m_channelAccessManager->NotifyAckTimeoutResetNow ();
              m_triggerFrame = nullptr;  // this is strictly needed for DL_MU_TF_MU_BAR only

              m_edca->ResetCw (m_linkId);
              m_psduMap.clear ();
              TransmissionSucceeded ();
            }
        }
      else if (hdr.IsBlockAck () && m_txTimer.IsRunning ()
               && m_txTimer.GetReason () == WifiTxTimer::WAIT_BLOCK_ACK_AFTER_TB_PPDU)
        {
          CtrlBAckResponseHeader blockAck;
          mpdu->GetPacket ()->PeekHeader (blockAck);

          NS_ABORT_MSG_IF (!blockAck.IsMultiSta (),
                           "A Multi-STA BlockAck is expected after a TB PPDU");
          NS_LOG_DEBUG ("Received a Multi-STA BlockAck from=" << hdr.GetAddr2 ());

          NS_ASSERT (m_staMac && m_staMac->IsAssociated ());
          if (hdr.GetAddr2 () != m_bssid)
            {
              NS_LOG_DEBUG ("The sender is not the AP we are associated with");
              return;
            }

          uint16_t staId = m_staMac->GetAssociationId ();
          std::vector<uint32_t> indices = blockAck.FindPerAidTidInfoWithAid (staId);

          if (indices.empty ())
            {
              NS_LOG_DEBUG ("No Per AID TID Info subfield intended for me");
              return;
            }

          MuSnrTag tag;
          mpdu->GetPacket ()->PeekPacketTag (tag);

          // notify the Block Ack Manager
          for (const auto& index : indices)
            {
              uint8_t tid = blockAck.GetTidInfo (index);

              if (blockAck.GetAckType (index) && tid < 8)
                {
                  // Acknowledgment context
                  NS_ABORT_IF (m_psduMap.empty () || m_psduMap.begin ()->first != staId);
                  GetBaManager (tid)->NotifyGotAck (*m_psduMap.at (staId)->begin ());
                }
              else
                {
                  // Block Acknowledgment or All-ack context
                  if (blockAck.GetAckType (index) && tid == 14)
                    {
                      // All-ack context, we need to determine the actual TID(s) of the PSDU
                      NS_ASSERT (indices.size () == 1);
                      NS_ABORT_IF (m_psduMap.empty () || m_psduMap.begin ()->first != staId);
                      std::set<uint8_t> tids = m_psduMap.at (staId)->GetTids ();
                      NS_ABORT_MSG_IF (tids.size () > 1, "Multi-TID A-MPDUs not supported yet");
                      tid = *tids.begin ();
                    }

                  std::pair<uint16_t,uint16_t> ret = GetBaManager (tid)->NotifyGotBlockAck (blockAck,
                                                                                            hdr.GetAddr2 (),
                                                                                            {tid}, index);
                  GetWifiRemoteStationManager ()->ReportAmpduTxStatus (hdr.GetAddr2 (), ret.first,
                                                                              ret.second, rxSignalInfo.snr,
                                                                              tag.Get (staId),  m_txParams.m_txVector);
                }

              if (m_psduMap.at (staId)->GetHeader (0).IsQosData ()
                  && (blockAck.GetAckType (index)  // Ack or All-ack context
                      || std::any_of (blockAck.GetBitmap (index).begin (),
                                      blockAck.GetBitmap (index).end (),
                                      [](uint8_t b) { return b != 0; })))
                {
                  NS_ASSERT (m_psduMap.at (staId)->GetHeader (0).HasData ());
                  NS_ASSERT (m_psduMap.at (staId)->GetHeader (0).GetQosTid () == tid);
                  // the station has received a response from the AP for the HE TB PPDU
                  // transmitted in response to a Basic Trigger Frame and at least one
                  // MPDU was acknowledged. Therefore, it needs to update the access
                  // parameters if it received an MU EDCA Parameter Set element.
                  m_mac->GetQosTxop (tid)->StartMuEdcaTimerNow (m_linkId);
                }
            }

          // cancel the timer
          m_txTimer.Cancel ();
          m_channelAccessManager->NotifyAckTimeoutResetNow ();
          m_psduMap.clear ();
        }
      else if (hdr.IsBlockAck () && m_txTimer.IsRunning ()
               && m_txTimer.GetReason () == WifiTxTimer::WAIT_BLOCK_ACK)
        {
          // this BlockAck frame may have been sent in response to a DL MU PPDU with
          // acknowledgment in SU format or one of the consequent BlockAckReq frames.
          // We clear the PSDU map and let parent classes continue processing this frame.
          m_psduMap.clear ();
          VhtFrameExchangeManager::ReceiveMpdu (mpdu, rxSignalInfo, txVector, inAmpdu);
        }
      else if (hdr.IsTrigger ())
        {
          // Trigger Frames are only processed by STAs
          if (!m_staMac)
            {
              return;
            }

          // A Trigger Frame in an A-MPDU is processed when the A-MPDU is fully received
          if (inAmpdu)
            {
              m_triggerFrameInAmpdu = true;
              return;
            }

          CtrlTriggerHeader trigger;
          mpdu->GetPacket ()->PeekHeader (trigger);

          if (hdr.GetAddr1 () != m_self
              && (!hdr.GetAddr1 ().IsBroadcast ()
                  || !m_staMac->IsAssociated ()
                  || hdr.GetAddr2 () != m_bssid     // not sent by the AP this STA is associated with
                  || trigger.FindUserInfoWithAid (m_staMac->GetAssociationId ()) == trigger.end ()))
            {
              // not addressed to us
              return;
            }

          uint16_t staId = m_staMac->GetAssociationId ();

          if (trigger.IsMuBar ())
            {
              Mac48Address sender = hdr.GetAddr2 ();
              NS_LOG_DEBUG ("Received MU-BAR Trigger Frame from=" << sender);
              GetWifiRemoteStationManager ()->ReportRxOk (sender, rxSignalInfo, txVector);

              auto userInfoIt = trigger.FindUserInfoWithAid (staId);
              NS_ASSERT (userInfoIt != trigger.end ());
              CtrlBAckRequestHeader blockAckReq = userInfoIt->GetMuBarTriggerDepUserInfo ();
              NS_ABORT_MSG_IF (blockAckReq.IsMultiTid (), "Multi-TID BlockAckReq not supported");
              uint8_t tid = blockAckReq.GetTidInfo ();

              auto agreementIt = m_agreements.find ({sender, tid});

              if (agreementIt == m_agreements.end ())
                {
                  NS_LOG_DEBUG ("There's not a valid agreement for this BlockAckReq");
                  return;
                }

              agreementIt->second.NotifyReceivedBar (blockAckReq.GetStartingSequence ());

              NS_LOG_DEBUG ("Schedule Block Ack in TB PPDU");
              Simulator::Schedule (m_phy->GetSifs (), &HeFrameExchangeManager::SendBlockAck, this,
                                  agreementIt->second, hdr.GetDuration (),
                                  GetHeTbTxVector (trigger, hdr.GetAddr2 ()), rxSignalInfo.snr);
            }
          else if (trigger.IsBasic ())
            {
              //BEGIN: log for
              uint16_t staId = m_staMac->GetAssociationId ();
              WifiTxVector tbTxVector = GetHeTbTxVector (trigger, hdr.GetAddr2 ());
              auto ru =tbTxVector.GetHeMuUserInfo(staId).ru;
              std::cout << "Time:" << Simulator::Now() << ". RECEIVE BASIC. sta addr: " << m_self << ". staId:" << staId << ".ru:" << ru << std::endl;
              // if(trigger.GetMbtaIndicator()) SetSuccesses(m_self);
              SetSuccesses(m_self);
               m_ruNum = HeRu::GetNRus(80,ru.GetRuType());
              //END: log for

              //BEGIN: MY CODE
              if(trigger.GetArbitrationSlots()>0 && trigger.GetMbtaIndicator())
              {
                m_slot = trigger.GetArbitrationSlots();
                //Ptr<NormalRandomVariable> rand = CreateObject<NormalRandomVariable> ();
                uint8_t arbitrationNum = m_rand->GetInteger(0,std::pow(2,m_slot)-1);
                HeRuMap sri;
                std::vector<HeRuMap>::iterator itr;
                uint16_t staId = m_staMac->GetAssociationId();
                WifiTxVector tbTxVector = GetHeTbTxVector (trigger, hdr.GetAddr2 ());
                auto ru =tbTxVector.GetHeMuUserInfo(staId).ru;
                
                itr = std::find_if(m_staRuInfo.begin(), m_staRuInfo.end(),[&](const HeRuMap &i)->bool {
                                  return i.ru == ru;
                });
                BusyTone busyTone = {staId, arbitrationNum,trigger,hdr,false};
                if(itr==m_staRuInfo.end())
                {
                  sri.ru=ru;
                  sri.bt.push_back(busyTone);
                  m_staRuInfo.insert(m_staRuInfo.end(),sri);
                }
                else
                {
                  itr->bt.push_back(busyTone);
                }
                std::cout << "sta addr: " << m_self << ". staId:" << staId << std::endl;

                Simulator::Schedule(m_phy->GetSifs(), &HeFrameExchangeManager::SendBusyTone,
                                    this, trigger, hdr, staId,ru, true);
              }
              else
              {
                Simulator::Schedule (m_phy->GetSifs (), &HeFrameExchangeManager::ReceiveBasicTrigger,
                                   this, trigger, hdr);
              }
              //END: MY CODE
              //BEGIN: Default
              // Simulator::Schedule (m_phy->GetSifs (), &HeFrameExchangeManager::ReceiveBasicTrigger,
              //                      this, trigger, hdr);
              //END: Default
            }
          else if (trigger.IsBsrp ())
            {
              
              // BEGIN: MY CODE
                uint16_t staId = m_staMac->GetAssociationId ();
                WifiTxVector tbTxVector = GetHeTbTxVector (trigger, hdr.GetAddr2 ());
                auto ru =tbTxVector.GetHeMuUserInfo(staId).ru;
                 std::cout << "Time:" << Simulator::Now()<<". RECEIVE BSRP. sta addr: " << m_self << ". staId:" << staId << ".ru:" << ru << std::endl;
                m_ruNum = HeRu::GetNRus(80,ru.GetRuType());
                m_candidate++;
                m_isbsrp=true;
                m_isArbi=false;
                bsrpCand++;
                SetBsrpTfReceive(m_self);
                // END: My Code
              if(trigger.GetArbitrationSlots()>0) {
                
                
                // save info for arbitration phase
                m_slot = trigger.GetArbitrationSlots();
                // Ptr<NormalRandomVariable> rand = CreateObject<NormalRandomVariable> (); //ランダム値を生成
                uint8_t arbitrationNum = m_rand->GetInteger(0,std::pow(2,m_slot) -1);
                HeRuMap sri;
                std::vector<HeRuMap>::iterator itr;
                
                itr = std::find_if(m_staRuInfo.begin(),m_staRuInfo.end(),[&](const HeRuMap &i)->bool { return i.ru == ru; });
                BusyTone busyTone = {staId, arbitrationNum,trigger, hdr, false};
                if(itr==m_staRuInfo.end()){
                  sri.ru=ru;
                  sri.bt.push_back(busyTone);
                  m_staRuInfo.insert(m_staRuInfo.end(), sri);
                } else {
                  itr->bt.push_back(busyTone);
                }
               
                Simulator::Schedule(m_phy->GetSifs (), &HeFrameExchangeManager::SendBusyTone,
                                   this, trigger, hdr,staId,ru,false);
              }else{
                //BEGIN: Inspection no BSRP
                Simulator::Schedule (m_phy->GetSifs (), &HeFrameExchangeManager::SendQosNullFramesInTbPpdu,
                                   this, trigger, hdr);
                //END: Inspection no BSRP
              }
              // BEGIN: DEFAULT CODE
              // Simulator::Schedule (m_phy->GetSifs (), &HeFrameExchangeManager::SendQosNullFramesInTbPpdu,
              //                      this, trigger, hdr);
              // END: DEFAULT CODE
            }
        }
      else
        {
          // the received control frame cannot be handled here
          VhtFrameExchangeManager::ReceiveMpdu (mpdu, rxSignalInfo, txVector, inAmpdu);
        }

      // the received control frame has been processed
      return;
    }

  // the received frame cannot be handled here
  VhtFrameExchangeManager::ReceiveMpdu (mpdu, rxSignalInfo, txVector, inAmpdu);;
}

// ----- BEGIN: MY CODE -----
void
HeFrameExchangeManager::SendBusyTone(const CtrlTriggerHeader& trigger, const WifiMacHeader& hdr,uint8_t staId, HeRu::RuSpec ru, bool isBasic)
{
    std::cout << "SendBusyTone..." << Simulator::Now() << std::endl; // added by ryu 10/20
    auto ru_ptr = std::find_if(m_staRuInfo.begin(),m_staRuInfo.end(),[&](const HeRuMap &i)->bool {
                                  return i.ru == ru;
                            });
    int same_max =0;
    int staCount=0;
    if(ru_ptr->bt.size()<=1){
      ru_ptr->bt.at(0).isWin = true;
      m_wins++; // test by ryu 2022/11/22
      std::cout << "staId:"<< int(staId) <<". ru:"<<ru <<". count:" << 1 << ". same_max_arbi_num:" << 0<< std::endl;
      if(isBasic){
        ReceiveBasicTriggerAfterA(trigger, hdr,staId,ru);
      }else{
        SendQosNullFramesInTbPpduAfterA(trigger, hdr,staId,ru);
      }
      
      return;
    }
    auto my_ptr = std::find_if(ru_ptr->bt.begin(),ru_ptr->bt.end(),[&](const BusyTone &i)->bool {
                                  return i.staId == staId;
                            });
    uint8_t max_arbi_num = 0;
    
    for (auto bt_ptr = ru_ptr->bt.begin(); bt_ptr != ru_ptr->bt.end(); bt_ptr++) {
      // std::cout << "staId: "<<bt_ptr->staId<<std::endl;
      staCount++;
      if (max_arbi_num < bt_ptr->arbitrationNum) {
        max_arbi_num = bt_ptr->arbitrationNum;
        same_max=1;
      }else if(max_arbi_num == bt_ptr->arbitrationNum){
        same_max++;
      }
    }
    // std::cout << "loop end." << std::endl;

    std::cout << "staId:"<< int(staId) <<". ru:"<<ru <<". count:" << staCount << ". my_arbi_num:" << int(my_ptr->arbitrationNum)<< ". same_max_arbi_num:" << same_max << std::endl; 
    if(max_arbi_num == my_ptr->arbitrationNum){
      std::cout << "win staId: "<<my_ptr->staId<<std::endl;
      m_wins++; // test by ryu 2022/11/22
      my_ptr->isWin=true;
    }
    if(same_max>1){
      m_nConflict++;
    }

    if(isBasic){
      ReceiveBasicTriggerAfterA(trigger, hdr,staId,ru);
    }else{
      SendQosNullFramesInTbPpduAfterA(trigger, hdr,staId,ru);
    }
   
}

void
HeFrameExchangeManager::ReceiveBasicTriggerAfterA (const CtrlTriggerHeader& trigger, const WifiMacHeader& hdr, uint16_t staId,HeRu::RuSpec ru)
{
  NS_LOG_FUNCTION (this << trigger << hdr);
  NS_ASSERT (trigger.IsBasic ());
  NS_ASSERT (m_staMac && m_staMac->IsAssociated ());

  NS_LOG_DEBUG ("Received a Trigger Frame (basic variant) soliciting a transmission");
  std::cout << "Time:" << Simulator::Now() << ". " << __func__ << std::endl;

  if (trigger.GetCsRequired () && hdr.GetAddr2 () != m_txopHolder && m_navEnd > Simulator::Now ())
    {
      NS_LOG_DEBUG ("Carrier Sensing required and channel busy, do nothing");
      return;
    }
  auto ru_ptr = std::find_if(m_staRuInfo.begin(),m_staRuInfo.end(),[&](const HeRuMap &i)->bool {
                                  return i.ru == ru;
                            });
  
  auto my_ptr = std::find_if(ru_ptr->bt.begin(),ru_ptr->bt.end(),[&](const BusyTone &i)->bool {
                                  return i.staId == staId;
                            });
  if(!my_ptr->isWin){
    std::cout << "this sta " << staId << "is looser" << std::endl;
    return;
  }
  // Starting from the Preferred AC indicated in the Trigger Frame, check if there
  // is either a pending BlockAckReq frame or a data frame that can be transmitted
  // in the allocated time and is addressed to a station with which a Block Ack
  // agreement has been established.

  // create the sequence of TIDs to check
  std::vector<uint8_t> tids;
  //uint16_t staId = m_staMac->GetAssociationId ();
  AcIndex preferredAc = trigger.FindUserInfoWithAid (staId)->GetPreferredAc ();
  auto acIt = wifiAcList.find (preferredAc);
  for (uint8_t i = 0; i < 4; i++)
    {
      NS_ASSERT (acIt != wifiAcList.end ());
      tids.push_back (acIt->second.GetHighTid ());
      tids.push_back (acIt->second.GetLowTid ());

      acIt++;
      if (acIt == wifiAcList.end ())
        {
          acIt = wifiAcList.begin ();
        }
    }

  Ptr<WifiPsdu> psdu;
  WifiTxParameters txParams;
  WifiTxVector tbTxVector = GetHeTbTxVector (trigger, hdr.GetAddr2 ());
  Time ppduDuration = HePhy::ConvertLSigLengthToHeTbPpduDuration (trigger.GetUlLength (),
                                                                  tbTxVector,
                                                                  m_phy->GetPhyBand ());

  for (const auto& tid : tids)
    {
      Ptr<QosTxop> edca = m_mac->GetQosTxop (tid);
      std::cout <<"TID:" << int(tid) << std::endl;
      if (!edca->GetBaAgreementEstablished (hdr.GetAddr2 (), tid))
        {
          // no Block Ack agreement established for this TID
          std::cout <<"sta:"<< m_staMac->GetAddress() << ". no Block Ack agreement established for this TID:" << int(tid) << std::endl;
          continue;
        }

      txParams.Clear ();
      txParams.m_txVector = tbTxVector;

      // first, check if there is a pending BlockAckReq frame
      if (Ptr<const WifiMpdu> mpdu;
          (mpdu = edca->GetBaManager ()->GetBar (false, tid, hdr.GetAddr2 ()))
          && TryAddMpdu (mpdu, txParams, ppduDuration))
        {
          NS_LOG_DEBUG ("Sending a BAR within a TB PPDU");
          std::cout <<"sta:"<< m_staMac->GetAddress() << ". Sending a BAR within a TB PPDU" << std::endl;
          
          psdu = Create<WifiPsdu> (edca->GetBaManager ()->GetBar (true, tid, hdr.GetAddr2 ()), true);
          break;
        }

      // otherwise, check if a suitable data frame is available
      if (Ptr<WifiMpdu> mpdu;
          (mpdu = edca->PeekNextMpdu (m_linkId, tid, hdr.GetAddr2 ())))
        {
          Ptr<WifiMpdu> item = edca->GetNextMpdu (m_linkId, mpdu, txParams, ppduDuration, false);
          std::cout << "peekNextMpdu" << std::endl;
          if (item)
            {
              // try A-MPDU aggregation
              std::cout <<"sta:"<< m_staMac->GetAddress() << ". try A-MPDU aggregation" << std::endl;
              std::vector<Ptr<WifiMpdu>> mpduList = m_mpduAggregator->GetNextAmpdu (item, txParams,
                                                                                            ppduDuration);
              psdu = (mpduList.size () > 1 ? Create<WifiPsdu> (std::move (mpduList))
                                           : Create<WifiPsdu> (item, true));
              break;
            }
        }
    }

  if (psdu)
    {
      psdu->SetDuration (hdr.GetDuration () - m_phy->GetSifs () - ppduDuration);
      SendPsduMapWithProtection (WifiPsduMap {{staId, psdu}}, txParams);
    }
  else
    {
      // send QoS Null frames
      std::cout <<"sta:"<< m_staMac->GetAddress() << ". Send QoS Null Frame" << std::endl;
      SendQosNullFramesInTbPpdu (trigger, hdr);
    }
}

void
HeFrameExchangeManager::SendQosNullFramesInTbPpduAfterA (const CtrlTriggerHeader& trigger, const WifiMacHeader& hdr, uint16_t staId,HeRu::RuSpec ru)
{
  NS_LOG_FUNCTION (this << trigger << hdr);
  NS_ASSERT (trigger.IsBasic () || trigger.IsBsrp ());
  NS_ASSERT (m_staMac && m_staMac->IsAssociated ());

  NS_LOG_DEBUG ("Requested to send QoS Null frames");
  
  if (trigger.GetCsRequired () && hdr.GetAddr2 () != m_txopHolder && m_navEnd > Simulator::Now ())
    {
      // std::cout << "carrier sensing required and channel bysy" << std::endl;
      NS_LOG_DEBUG ("Carrier Sensing required and channel busy (TA=" << hdr.GetAddr2 ()
                    << ", TxopHolder=" << m_txopHolder << ", NAV end=" << m_navEnd.As (Time::S)
                    << "), do nothing");
      return;
    }
  auto ru_ptr = std::find_if(m_staRuInfo.begin(),m_staRuInfo.end(),[&](const HeRuMap &i)->bool {
                                  return i.ru == ru;
                            });
  
  auto my_ptr = std::find_if(ru_ptr->bt.begin(),ru_ptr->bt.end(),[&](const BusyTone &i)->bool {
                                  return i.staId == staId;
                            });
  if(!my_ptr->isWin){
    std::cout << "this sta " << staId << "is looser" << std::endl;
    return;
  }
  std::cout << "set ppdu after qos null" << std::endl;
  WifiMacHeader header;
  header.SetType (WIFI_MAC_QOSDATA_NULL);
  header.SetAddr1 (hdr.GetAddr2 ());
  header.SetAddr2 (m_self);
  header.SetAddr3 (hdr.GetAddr2 ());
  header.SetDsTo ();
  header.SetDsNotFrom ();
  // TR3: Sequence numbers for transmitted QoS (+)Null frames may be set
  // to any value. (Table 10-3 of 802.11-2016)
  header.SetSequenceNumber (0);
  // Set the EOSP bit so that NotifyTxToEdca will add the Queue Size
  header.SetQosEosp ();

  WifiTxParameters txParams;
  txParams.m_txVector = GetHeTbTxVector (trigger, hdr.GetAddr2 ());
  txParams.m_protection = std::unique_ptr<WifiProtection> (new WifiNoProtection);
  txParams.m_acknowledgment = std::unique_ptr<WifiAcknowledgment> (new WifiNoAck);

  Time ppduDuration = HePhy::ConvertLSigLengthToHeTbPpduDuration (trigger.GetUlLength (),
                                                                  txParams.m_txVector,
                                                                  m_phy->GetPhyBand ());
  header.SetDuration (hdr.GetDuration () - m_phy->GetSifs () - ppduDuration);

  // Ptr<WifiMacQueueItem> mpdu;
  // std::vector<Ptr<WifiMacQueueItem>> mpduList;
  
  Ptr<WifiMpdu> mpdu;
  std::vector<Ptr<WifiMpdu>> mpduList;
  uint8_t tid = 0;
  header.SetQosTid (tid);

  while (tid < 8
         && IsWithinSizeAndTimeLimits (txParams.GetSizeIfAddMpdu (mpdu = Create<WifiMpdu> (Create<Packet> (),
                                                                                                   header)),
                                       hdr.GetAddr2 (), txParams, ppduDuration))
    {
      NS_LOG_DEBUG ("Aggregating a QoS Null frame with tid=" << +tid);
      // We could call TryAddMpdu instead of IsWithinSizeAndTimeLimits above in order to
      // get the TX parameters updated automatically. However, aggregating the QoS Null
      // frames might fail because MPDU aggregation is disabled by default for VO
      // and BK. Therefore, we skip the check on max A-MPDU size and only update the
      // TX parameters below.
      txParams.m_acknowledgment = GetAckManager ()->TryAddMpdu (mpdu, txParams);
      txParams.AddMpdu (mpdu);
      UpdateTxDuration (mpdu->GetHeader ().GetAddr1 (), txParams);
      mpduList.push_back (mpdu);
      header.SetQosTid (++tid);
    }

  if (mpduList.empty ())
    {
      std::cout << "not enough time to send a QoS Null frame" << std::endl;
      NS_LOG_DEBUG ("Not enough time to send a QoS Null frame");
      return;
    }
  // std::cout << m_txTimer.IsRunning() << std::endl;
  Ptr<WifiPsdu> psdu = (mpduList.size () > 1 ? Create<WifiPsdu> (std::move (mpduList))
                                             : Create<WifiPsdu> (mpduList.front (), true));
  // uint16_t staId = m_staMac->GetAssociationId ();
  
  SendPsduMapWithProtection (WifiPsduMap {{staId, psdu}}, txParams);
}
int
HeFrameExchangeManager::GetNBasic(void)
{
  return m_numBasic;
}
int
HeFrameExchangeManager::GetNBsrp(void)
{
  return m_numBsrp;
}

int
HeFrameExchangeManager::GetNConflict(void)
{
  return m_nConflict;
}

int
HeFrameExchangeManager::GetMaxNCandidates(void)
{
  return m_max_candidate;
}

void
HeFrameExchangeManager::SetSuccesses(Mac48Address addr)
{
  auto itr = std::find_if(m_successes.begin(),m_successes.end(),[&addr](CandidateInfo ci){
    return ci.addr == addr;
  });
  if(itr == m_successes.end())
  {
    m_successes.push_back({addr,1,0,0,0,0});
  }else{
    itr->cCount += 1;
  }
}

void
HeFrameExchangeManager::UpdateSuccesses(Mac48Address addr,int byte)
{
  auto itr = std::find_if(m_successes.begin(),m_successes.end(),[&addr](CandidateInfo ci){
    return ci.addr == addr;
  });
  if(itr != m_successes.end())
  {
    // itr->sCount += 1;
    itr->byte += byte;
  }
}

std::vector<int>
HeFrameExchangeManager::GetCandidatesInfo(Mac48Address addr)
{
  auto itr = std::find_if(m_successes.begin(),m_successes.end(),[&addr](CandidateInfo ci){
    return ci.addr == addr;
  });
  if(itr != m_successes.end())
  {
    return {itr->cCount,itr->sCount,itr->byte,itr->bsrpTfReceiveCount,itr->QosNullReceivedCount};
  }else{
    return {0,0,0,0,0};
  }
}

void 
HeFrameExchangeManager::SetBsrpTfReceive(Mac48Address addr)
{
  auto itr = std::find_if(m_successes.begin(),m_successes.end(),[&addr](CandidateInfo ci){
    return ci.addr == addr;
  });
  if(itr == m_successes.end())
  {
    m_successes.push_back({addr,0,0,0,1,0});
  }else{
    itr->bsrpTfReceiveCount += 1;
  }
}

void 
HeFrameExchangeManager::SetQosNullReceived(Mac48Address addr)
{
  auto itr = std::find_if(m_successes.begin(),m_successes.end(),[&addr](CandidateInfo ci){
    return ci.addr == addr;
  });
  if(itr != m_successes.end())
  {
    itr->QosNullReceivedCount += 1;
  }
}

void 
HeFrameExchangeManager::SetUlSuccesses(Mac48Address addr)
{
  auto itr = std::find_if(m_successes.begin(),m_successes.end(),[&addr](CandidateInfo ci){
    return ci.addr == addr;
  });
  if(itr != m_successes.end())
  {
    itr->sCount += 1;
  }
}
void 
HeFrameExchangeManager::SearchUlSuccessSta(Mac48Address addr)
{
  auto itr = std::find_if(m_UlSuccessStas.begin(),m_UlSuccessStas.end(),[&addr](std::pair<Mac48Address,int> pair){
    return pair.first == addr;
  });
  if(itr == m_UlSuccessStas.end())
  {
    m_UlSuccessStas.push_back({addr,0});
  }
}
void 
HeFrameExchangeManager::AddUlSuccessStaSize(Mac48Address addr,int byte)
{
  auto itr = std::find_if(m_UlSuccessStas.begin(),m_UlSuccessStas.end(),[&addr](std::pair<Mac48Address,int> pair){
    return pair.first == addr;
  });
  if(itr == m_UlSuccessStas.end())
  {
    m_UlSuccessStas.push_back({addr,byte});
  }else
  {
    itr->second += byte;
  }
}


float
HeFrameExchangeManager::GetWasteRuRate(void)
{
  float sumRate=0;
  for(const auto &rate : m_wasteRuRates)
  {
    sumRate += rate;
  }
  return sumRate/m_wasteRuRates.size();
}

int
HeFrameExchangeManager::GetWasteRuCount(void)
{
  return m_wasteRuCount;
}

std::vector<int>
HeFrameExchangeManager::GetBpsSets(void)
{
  return {m_sumByte, m_maxByte*GetNRuForUl()};
}

std::vector<Mac48Address>
HeFrameExchangeManager::GetQosNullStas(void)
{
  return m_qosNullStas;
}

int 
HeFrameExchangeManager::GetNRuForUl(void)
{
  return m_ruNum - m_BARu;
}

// ----- END: MY CODE -----

void
HeFrameExchangeManager::EndReceiveAmpdu (Ptr<const WifiPsdu> psdu, const RxSignalInfo& rxSignalInfo,
                                         const WifiTxVector& txVector, const std::vector<bool>& perMpduStatus)
{
  std::set<uint8_t> tids = psdu->GetTids ();

  if (txVector.IsUlMu () && m_txTimer.IsRunning ()
      && m_txTimer.GetReason () == WifiTxTimer::WAIT_TB_PPDU_AFTER_BASIC_TF)
    {
      Mac48Address sender = psdu->GetAddr2 ();
      NS_ASSERT (m_txParams.m_acknowledgment
                  && m_txParams.m_acknowledgment->method == WifiAcknowledgment::UL_MU_MULTI_STA_BA);
      WifiUlMuMultiStaBa* acknowledgment = static_cast<WifiUlMuMultiStaBa*> (m_txParams.m_acknowledgment.get ());
      std::size_t index = acknowledgment->baType.m_bitmapLen.size ();

      if (m_staExpectTbPpduFrom.find (sender) == m_staExpectTbPpduFrom.end ())
        {
          NS_LOG_WARN ("Received a TB PPDU from an unexpected station: " << sender);
          return;
        }

      NS_LOG_DEBUG ("Received an A-MPDU in a TB PPDU from " << sender << " (" << *psdu << ")");

      if (std::any_of (tids.begin (), tids.end (),
                       [&psdu](uint8_t tid)
                         { return psdu->GetAckPolicyForTid (tid) == WifiMacHeader::NORMAL_ACK; }))
        {
          if (std::all_of (perMpduStatus.cbegin (), perMpduStatus.cend (), [](bool v) { return v; }))
            {
              // All-ack context
              acknowledgment->stationsReceivingMultiStaBa.emplace (std::make_pair (sender, 14), index);
              acknowledgment->baType.m_bitmapLen.push_back (0);
            }
          else
            {
              // Block Acknowledgment context
              std::size_t i = 0;
              for (const auto& tid : tids)
                {
                  acknowledgment->stationsReceivingMultiStaBa.emplace (std::make_pair (sender, tid), index + i++);
                  acknowledgment->baType.m_bitmapLen.push_back (GetBlockAckType (sender, tid).m_bitmapLen.at (0));
                }
            }
          uint16_t staId = txVector.GetHeMuUserInfoMap ().begin ()->first;
          m_muSnrTag.Set (staId, rxSignalInfo.snr);
        }

      // Schedule the transmission of a Multi-STA BlockAck frame if needed
      if (!acknowledgment->stationsReceivingMultiStaBa.empty () && !m_multiStaBaEvent.IsRunning ())
        {
          m_multiStaBaEvent = Simulator::Schedule (m_phy->GetSifs (),
                                                   &HeFrameExchangeManager::SendMultiStaBlockAck,
                                                   this, std::cref (m_txParams));
        }

      // remove the sender from the set of stations that are expected to send a TB PPDU
      m_staExpectTbPpduFrom.erase (sender);

      if (m_staExpectTbPpduFrom.empty ())
        {
          // we do not expect any other BlockAck frame
          m_txTimer.Cancel ();
          m_channelAccessManager->NotifyAckTimeoutResetNow ();

          if (!m_multiStaBaEvent.IsRunning ())
            {
              // all of the stations that replied with a TB PPDU sent QoS Null frames.
              NS_LOG_DEBUG ("Continue the TXOP");
              m_psduMap.clear ();
              m_edca->ResetCw (m_linkId);
              TransmissionSucceeded ();
            }
        }

      // the received TB PPDU has been processed
      return;
    }

  if (txVector.IsUlMu () && m_txTimer.IsRunning ()
      && m_txTimer.GetReason () == WifiTxTimer::WAIT_QOS_NULL_AFTER_BSRP_TF)
    {
      Mac48Address sender = psdu->GetAddr2 ();

      if (m_staExpectTbPpduFrom.find (sender) == m_staExpectTbPpduFrom.end ())
        {
          NS_LOG_WARN ("Received a TB PPDU from an unexpected station: " << sender);
          return;
        }
      if (std::none_of (psdu->begin (), psdu->end (), [](Ptr<WifiMpdu> mpdu)
                                                      { return mpdu->GetHeader ().IsQosData ()
                                                               && !mpdu->GetHeader ().HasData ();
                                                      }))
        {
          NS_LOG_WARN ("No QoS Null frame in the received PSDU");
          return;
        }

      NS_LOG_DEBUG ("Received QoS Null frames in a TB PPDU from " << sender);

      // remove the sender from the set of stations that are expected to send a TB PPDU
      m_staExpectTbPpduFrom.erase (sender);

      if (m_staExpectTbPpduFrom.empty ())
        {
          // we do not expect any other response
          m_txTimer.Cancel ();
          m_channelAccessManager->NotifyAckTimeoutResetNow ();

          NS_ASSERT (m_edca);
          m_psduMap.clear ();
          m_edca->ResetCw (m_linkId);
          TransmissionSucceeded ();
        }

      // the received TB PPDU has been processed
      return;
    }

  if (m_triggerFrameInAmpdu)
    {
      // the received A-MPDU contains a Trigger Frame. It is now time to handle it.
      auto psduIt = psdu->begin ();
      while (psduIt != psdu->end ())
        {
          if ((*psduIt)->GetHeader ().IsTrigger ())
            {
              ReceiveMpdu (*psduIt, rxSignalInfo, txVector, false);
            }
          psduIt++;
        }

      m_triggerFrameInAmpdu = false;
      return;
    }

  // the received frame cannot be handled here
  VhtFrameExchangeManager::EndReceiveAmpdu (psdu, rxSignalInfo, txVector, perMpduStatus);
}

} //namespace ns3
