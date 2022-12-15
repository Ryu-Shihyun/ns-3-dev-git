/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright 2007 University of Washington
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
 * Author:  Tom Henderson (tomhend@u.washington.edu)
 */
#include "ns3/address.h"
#include "ns3/address-utils.h"
#include "ns3/log.h"
#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/node.h"
#include "ns3/socket.h"
#include "ns3/udp-socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-socket-factory.h"
#include "packet-sink.h"
#include "ns3/boolean.h"
#include "ns3/ipv4-packet-info-tag.h"
#include "ns3/ipv6-packet-info-tag.h"
#include <fstream>
#include <string>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("PacketSink");

NS_OBJECT_ENSURE_REGISTERED (PacketSink);

TypeId
PacketSink::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::PacketSink")
    .SetParent<Application> ()
    .SetGroupName("Applications")
    .AddConstructor<PacketSink> ()
    .AddAttribute ("Local",
                   "The Address on which to Bind the rx socket.",
                   AddressValue (),
                   MakeAddressAccessor (&PacketSink::m_local),
                   MakeAddressChecker ())
    .AddAttribute ("Protocol",
                   "The type id of the protocol to use for the rx socket.",
                   TypeIdValue (UdpSocketFactory::GetTypeId ()),
                   MakeTypeIdAccessor (&PacketSink::m_tid),
                   MakeTypeIdChecker ())
    .AddAttribute ("EnableSeqTsSizeHeader",
                   "Enable optional header tracing of SeqTsSizeHeader",
                   BooleanValue (false),
                   MakeBooleanAccessor (&PacketSink::m_enableSeqTsSizeHeader),
                   MakeBooleanChecker ())
    .AddTraceSource ("Rx",
                     "A packet has been received",
                     MakeTraceSourceAccessor (&PacketSink::m_rxTrace),
                     "ns3::Packet::AddressTracedCallback")
    .AddTraceSource ("RxWithAddresses", "A packet has been received",
                     MakeTraceSourceAccessor (&PacketSink::m_rxTraceWithAddresses),
                     "ns3::Packet::TwoAddressTracedCallback")
    .AddTraceSource ("RxWithSeqTsSize",
                     "A packet with SeqTsSize header has been received",
                     MakeTraceSourceAccessor (&PacketSink::m_rxTraceWithSeqTsSize),
                     "ns3::PacketSink::SeqTsSizeCallback")
  ;
  return tid;
}

PacketSink::PacketSink ()
{
  NS_LOG_FUNCTION (this);
  m_socket = 0;
  m_totalRx = 0;
}

PacketSink::~PacketSink()
{
  NS_LOG_FUNCTION (this);
}

uint64_t PacketSink::GetTotalRx () const
{
  NS_LOG_FUNCTION (this);
  return m_totalRx;
}

Ptr<Socket>
PacketSink::GetListeningSocket (void) const
{
  NS_LOG_FUNCTION (this);
  return m_socket;
}

std::list<Ptr<Socket> >
PacketSink::GetAcceptedSockets (void) const
{
  NS_LOG_FUNCTION (this);
  return m_socketList;
}

void PacketSink::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_socket = 0;
  m_socketList.clear ();

  // chain up
  Application::DoDispose ();
}


// Application Methods
void PacketSink::StartApplication ()    // Called at time specified by Start
{
  NS_LOG_FUNCTION (this);
  // Create the socket if not already
  if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode (), m_tid);
      if (m_socket->Bind (m_local) == -1)
        {
          NS_FATAL_ERROR ("Failed to bind socket");
        }
      m_socket->Listen ();
      m_socket->ShutdownSend ();
      if (addressUtils::IsMulticast (m_local))
        {
          Ptr<UdpSocket> udpSocket = DynamicCast<UdpSocket> (m_socket);
          if (udpSocket)
            {
              // equivalent to setsockopt (MCAST_JOIN_GROUP)
              udpSocket->MulticastJoinGroup (0, m_local);
            }
          else
            {
              NS_FATAL_ERROR ("Error: joining multicast on a non-UDP socket");
            }
        }
    }

  if (InetSocketAddress::IsMatchingType (m_local))
    {
      m_localPort = InetSocketAddress::ConvertFrom (m_local).GetPort ();
    }
  else if (Inet6SocketAddress::IsMatchingType (m_local))
    {
      m_localPort = Inet6SocketAddress::ConvertFrom (m_local).GetPort ();
    }
  else
    {
      m_localPort = 0;
    }
  m_socket->SetRecvCallback (MakeCallback (&PacketSink::HandleRead, this));
  m_socket->SetRecvPktInfo (true);
  m_socket->SetAcceptCallback (
    MakeNullCallback<bool, Ptr<Socket>, const Address &> (),
    MakeCallback (&PacketSink::HandleAccept, this));
  m_socket->SetCloseCallbacks (
    MakeCallback (&PacketSink::HandlePeerClose, this),
    MakeCallback (&PacketSink::HandlePeerError, this));
}

void PacketSink::StopApplication ()     // Called at time specified by Stop
{
  NS_LOG_FUNCTION (this);
  while(!m_socketList.empty ()) //these are accepted sockets, close them
    {
      Ptr<Socket> acceptedSocket = m_socketList.front ();
      m_socketList.pop_front ();
      acceptedSocket->Close ();
    }
  if (m_socket)
    {
      m_socket->Close ();
      m_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
    }
}

void PacketSink::HandleRead (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
  Ptr<Packet> packet;
  Address from;
  Address localAddress;
  while ((packet = socket->RecvFrom (from)))
    {
      if (packet->GetSize () == 0)
        { //EOF
          break;
        }
      m_totalRx += packet->GetSize ();
      if (InetSocketAddress::IsMatchingType (from))
        {
          NS_LOG_INFO ("At time " << Simulator::Now ().As (Time::S)
                       << " packet sink received "
                       <<  packet->GetSize () << " bytes from "
                       << InetSocketAddress::ConvertFrom(from).GetIpv4 ()
                       << " port " << InetSocketAddress::ConvertFrom (from).GetPort ()
                       << " total Rx " << m_totalRx << " bytes");
          //BEGIN: log for
          std::cout << "At time " << Simulator::Now ().As (Time::S)
                       << " packet sink received "
                       <<  packet->GetSize () << " bytes from "
                       << InetSocketAddress::ConvertFrom(from).GetIpv4 ()
                       << " port " << InetSocketAddress::ConvertFrom (from).GetPort ()
                       << " total Rx " << m_totalRx << " bytes" << std::endl;
          
          uint8_t *buffer = new uint8_t[packet->GetSize()];
          packet->CopyData(buffer,packet->GetSize());
          std::string str = std::string(buffer, buffer + packet->GetSize());
          int first = 0;
          int last = str.find_first_of(',');
          std::cout << str << std::endl;
          std::vector<std::string> result;
          while (first < str.size()) {
            result.push_back(str.substr(first, last - first));
            first = last + 1;
            last = str.find_first_of(',', first);
            if (last == std::string::npos) last = str.size();
          }
          
          auto ip_from = InetSocketAddress::ConvertFrom(from).GetIpv4 ();
          Time delay = Simulator::Now() - Time(result.at(1));
          
          //SUB BEGIN: waste code but it`s called many time
          // auto itr = std::find_if(m_delayInfo.begin(),m_delayInfo.end(),
          //                 [&ip_from] (addrPair pair)
          //                   { return pair.first == ip_from; });
          // std::cout << "ip_from: " <<  ip_from << ((itr!=m_delayInfo.end()) ? "true" : "false") << std::endl;
          // if(itr == m_delayInfo.end())
          // {
          //   delayPair dp = {delay,1};
          //   addrPair ap = {ip_from,dp};
          //   m_delayInfo.push_back(ap);
          // }
          // else
          // {
          //   Time data = ((itr->second.first) * (itr->second.second) + delay)/(itr->second.second + 1);
          //   itr->second.first = delay;
          //   itr->second.second += 1;
          //   std::cout << "delay:" << itr->second.first << ". count:" << itr->second.second << std::endl;
          // }
          //SUB END: waste code but it`s called many time

          //SUB BEGIN: record delay
          std::ofstream  writting_file;
          std::string filename = "./data/delayData.csv";
          writting_file.open(filename, std::ios::app);
          writting_file << ip_from << "," << delay.GetNanoSeconds() << std::endl;
          writting_file.close();

          //END: log for
        }
      else if (Inet6SocketAddress::IsMatchingType (from))
        {
          NS_LOG_INFO ("At time " << Simulator::Now ().As (Time::S)
                       << " packet sink received "
                       <<  packet->GetSize () << " bytes from "
                       << Inet6SocketAddress::ConvertFrom(from).GetIpv6 ()
                       << " port " << Inet6SocketAddress::ConvertFrom (from).GetPort ()
                       << " total Rx " << m_totalRx << " bytes");
          //BEGIN: log for
          std::cout << "At time " << Simulator::Now ().As (Time::S)
                       << " packet sink received "
                       <<  packet->GetSize () << " bytes from "
                       << Inet6SocketAddress::ConvertFrom(from).GetIpv6 ()
                       << " port " << Inet6SocketAddress::ConvertFrom (from).GetPort ()
                       << " total Rx " << m_totalRx << " bytes" << std::endl;

          // uint8_t *buffer = new uint8_t[packet->GetSize()];
          // packet->CopyData(buffer,packet->GetSize());
          // std::string str = std::string(buffer, buffer + packet->GetSize());
          // int first = 0;
          // int last = str.find_first_of(',');
          // std::vector<std::string> result;
          // while (first < str.size()) {
          //   result.push_back(str.substr(first, last - first));
          //   first = last + 1;
          //   last = str.find_first_of(',', first);
          //   if (last == std::string::npos) last = str.size();
          // }

          // Time delay = Simulator::Now() - Time(result.at(1));
          // auto itr = std::find_if(m_delayInfo.begin(),m_delayInfo.end(),
          //                 [&from] (addrPair pair)
          //                   { return pair.first == from; });
          // if(itr == m_delayInfo.end())
          // {
          //   delayPair dp = {delay,1};
          //   addrPair ap = {InetSocketAddress::ConvertFrom(from).GetIpv4 (),dp};
          //   m_delayInfo.push_back(ap);
          // }
          // else
          // {
          //   Time data = ((itr->second.first) * (itr->second.second) + delay)/(itr->second.second + 1);
          //   itr->second.first = delay;
          //   itr->second.second += 1;
          // }

          //END: log for
        }

      if (!m_rxTrace.IsEmpty () || !m_rxTraceWithAddresses.IsEmpty () ||
          (!m_rxTraceWithSeqTsSize.IsEmpty () && m_enableSeqTsSizeHeader))
        {
          Ipv4PacketInfoTag interfaceInfo;
          Ipv6PacketInfoTag interface6Info;
          if (packet->RemovePacketTag (interfaceInfo))
            {
              localAddress = InetSocketAddress (interfaceInfo.GetAddress (), m_localPort);
            }
          else if (packet->RemovePacketTag (interface6Info))
            {
              localAddress = Inet6SocketAddress (interface6Info.GetAddress (), m_localPort);
            }
          else
            {
              socket->GetSockName (localAddress);
            }
          m_rxTrace (packet, from);
          m_rxTraceWithAddresses (packet, from, localAddress);

          if (!m_rxTraceWithSeqTsSize.IsEmpty () && m_enableSeqTsSizeHeader)
            {
              PacketReceived (packet, from, localAddress);
            }
        }
    }
}

void
PacketSink::PacketReceived (const Ptr<Packet> &p, const Address &from,
                            const Address &localAddress)
{
  SeqTsSizeHeader header;
  Ptr<Packet> buffer;

  auto itBuffer = m_buffer.find (from);
  if (itBuffer == m_buffer.end ())
    {
      itBuffer = m_buffer.insert (std::make_pair (from, Create<Packet> (0))).first;
    }

  buffer = itBuffer->second;
  buffer->AddAtEnd (p);
  buffer->PeekHeader (header);

  NS_ABORT_IF (header.GetSize () == 0);

  while (buffer->GetSize () >= header.GetSize ())
    {
      NS_LOG_DEBUG ("Removing packet of size " << header.GetSize () << " from buffer of size " << buffer->GetSize ());
      Ptr<Packet> complete = buffer->CreateFragment (0, static_cast<uint32_t> (header.GetSize ()));
      buffer->RemoveAtStart (static_cast<uint32_t> (header.GetSize ()));

      complete->RemoveHeader (header);

      m_rxTraceWithSeqTsSize (complete, from, localAddress, header);

      if (buffer->GetSize () > header.GetSerializedSize ())
        {
          buffer->PeekHeader (header);
        }
      else
        {
          break;
        }
    }
}

void PacketSink::HandlePeerClose (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
}

void PacketSink::HandlePeerError (Ptr<Socket> socket)
{
  NS_LOG_FUNCTION (this << socket);
}

void PacketSink::HandleAccept (Ptr<Socket> s, const Address& from)
{
  NS_LOG_FUNCTION (this << s << from);
  s->SetRecvCallback (MakeCallback (&PacketSink::HandleRead, this));
  m_socketList.push_back (s);
}
//BEGIN: log for
// Time PacketSink::GetAverageDelay (Ipv4Address addr)
// {
//   auto itr = std::find_if(m_delayInfo.begin(),m_delayInfo.end(),
//                           [&addr] (addrPair pair)
//                             { return pair.first == addr; });
//   return itr->second.first;  
// }
//END: log for
} // Namespace ns3
