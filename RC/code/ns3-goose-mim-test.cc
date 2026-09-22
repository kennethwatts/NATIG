/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright 2026 Kenneth Watts
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
 * Minimal GOOSE publish/subscribe + rogue-publisher attack test.
 *
 * Unlike Modbus/MMS's point-to-point client/server test topologies,
 * GOOSE needs a shared medium so multiple subscribers can all receive
 * one publisher's multicast frame -- this uses a CSMA network with one
 * publisher, one legitimate subscriber, and one rogue publisher (an
 * "Insider" node) all attached to the same segment.
 *
 * Expected result: the subscriber should see MMXU1.TotW.mag.f's real
 * value (100) via the legitimate publisher's burst-then-heartbeat
 * traffic, then see it forged to 9999 once the rogue publisher's
 * attack window opens (it wins acceptance by advancing stNum faster
 * than the real publisher's heartbeat), then see it return to 100
 * after the attack window closes and the real publisher's next
 * heartbeat/change re-asserts the genuine state.
 *
 * Portions of this file were drafted with AI assistance (Claude,
 * Anthropic) and reviewed/adapted by the author.
 *
 * Author: Kenneth Watts (ken.watts@gmail.com)
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/csma-module.h"
#include "ns3/goose-application-helper-new.h"
#include "ns3/goose-application-new.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("GooseMIMTest");

int
main (int argc, char *argv[])
{
  double simTimeSeconds = 30.0;
  std::string pointsFile = "points_goose_mim_test.csv";
  std::string attackConfFile = "attack_config_goose_test.json";

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation duration in seconds", simTimeSeconds);
  cmd.AddValue ("pointsFile", "Path to the GOOSE points CSV file", pointsFile);
  cmd.AddValue ("attackConfFile", "Path to the rogue-publisher attack config JSON file", attackConfFile);
  cmd.Parse (argc, argv);

  LogComponentEnable ("GooseMIMTest", LOG_LEVEL_INFO);
  LogComponentEnable ("GooseApplicationNew", LOG_LEVEL_ALL);

  NS_LOG_INFO ("GOOSE MIM test: simTime=" << simTimeSeconds << "s, pointsFile=" << pointsFile
               << ", attackConfFile=" << attackConfFile);

  // -- Shared CSMA segment: publisher (0), subscriber (1), rogue publisher (2) --
  NodeContainer nodes;
  nodes.Create (3);

  CsmaHelper csma;
  csma.SetChannelAttribute ("DataRate", StringValue ("100Mbps"));
  csma.SetChannelAttribute ("Delay", TimeValue (NanoSeconds (500)));
  NetDeviceContainer devices = csma.Install (nodes);

  InternetStackHelper internetStack;
  internetStack.Install (nodes);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer interfaces = ipv4.Assign (devices);

  Ipv4Address publisherAddress = interfaces.GetAddress (0);
  Ipv4Address rogueAddress = interfaces.GetAddress (2);
  Ipv4Address multicastGroup ("239.1.2.3");
  uint16_t multicastPort = 102;

  // A default multicast route on each *sender's* interface is what
  // actually lets an outbound packet addressed to the multicast group
  // get routed onto the shared CSMA segment at all -- receivers need
  // no equivalent per-socket "join" call on a single shared segment
  // (see goose-application-new.cc's makeMulticastConnection for why).
  // Both the legitimate publisher (node 0) and the rogue publisher
  // (node 2) send multicast traffic, so both need this.
  Ipv4StaticRoutingHelper multicastRouting;
  multicastRouting.SetDefaultMulticastRoute (nodes.Get (0), devices.Get (0));
  multicastRouting.SetDefaultMulticastRoute (nodes.Get (2), devices.Get (2));

  // -- Legitimate publisher --
  GooseApplicationHelperNew goosePublisher ("ns3::UdpSocketFactory",
                                             InetSocketAddress (publisherAddress, multicastPort));
  goosePublisher.SetAttribute ("LocalPort", UintegerValue (multicastPort));
  goosePublisher.SetAttribute ("RemoteAddress", AddressValue (multicastGroup));
  goosePublisher.SetAttribute ("RemotePort", UintegerValue (multicastPort));
  goosePublisher.SetAttribute ("isMaster", BooleanValue (true));
  goosePublisher.SetAttribute ("Name", StringValue ("GoosePublisher1"));
  goosePublisher.SetAttribute ("PointsFilename", StringValue (pointsFile));
  goosePublisher.SetAttribute ("GooseID", StringValue ("GCB1"));
  goosePublisher.SetAttribute ("BurstCount", UintegerValue (3));
  goosePublisher.SetAttribute ("BurstIntervalMs", DoubleValue (4.0));
  goosePublisher.SetAttribute ("HeartbeatIntervalMs", DoubleValue (2000.0));
  goosePublisher.SetAttribute ("JitterMinNs", DoubleValue (1000));
  goosePublisher.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<GooseApplicationNew> publisher =
    goosePublisher.Install (nodes.Get (0), std::string ("GoosePublisher1"));
  Simulator::Schedule (MilliSeconds (5), &GooseApplicationNew::attack_data, publisher, 0);

  // -- Subscriber --
  GooseApplicationHelperNew gooseSubscriber ("ns3::UdpSocketFactory",
                                              InetSocketAddress (interfaces.GetAddress (1), multicastPort));
  gooseSubscriber.SetAttribute ("LocalPort", UintegerValue (multicastPort));
  gooseSubscriber.SetAttribute ("RemoteAddress", AddressValue (multicastGroup));
  gooseSubscriber.SetAttribute ("RemotePort", UintegerValue (multicastPort));
  gooseSubscriber.SetAttribute ("isMaster", BooleanValue (false));
  gooseSubscriber.SetAttribute ("Name", StringValue ("GooseSubscriber1"));
  gooseSubscriber.SetAttribute ("PointsFilename", StringValue (pointsFile));
  gooseSubscriber.SetAttribute ("GooseID", StringValue ("GCB1"));

  Ptr<GooseApplicationNew> subscriber =
    gooseSubscriber.Install (nodes.Get (1), std::string ("GooseSubscriber1"));

  // -- Rogue publisher ("Insider" in the name triggers the same --
  // -- rogue/attack dispatch path as MIM does for the other protocols) --
  GooseApplicationHelperNew gooseRogue ("ns3::UdpSocketFactory",
                                         InetSocketAddress (rogueAddress, multicastPort));
  gooseRogue.SetAttribute ("LocalPort", UintegerValue (multicastPort));
  gooseRogue.SetAttribute ("RemoteAddress2", AddressValue (multicastGroup));
  gooseRogue.SetAttribute ("RemotePort", UintegerValue (multicastPort));
  gooseRogue.SetAttribute ("isMaster", BooleanValue (false));
  gooseRogue.SetAttribute ("Name", StringValue ("GooseInsiderMIM1"));
  gooseRogue.SetAttribute ("PointsFilename", StringValue (pointsFile));
  gooseRogue.SetAttribute ("GooseID", StringValue ("GCB1"));

  gooseRogue.SetAttribute ("mitmFlag", BooleanValue (true));
  gooseRogue.SetAttribute ("AttackConf", StringValue (attackConfFile));
  gooseRogue.SetAttribute ("ID", UintegerValue (1));
  gooseRogue.SetAttribute ("NodeID", StringValue ("Node1"));
  gooseRogue.SetAttribute ("PointID", StringValue ("MMXU1.TotW.mag.f"));
  gooseRogue.SetAttribute ("Value_attck", StringValue ("9999"));
  gooseRogue.SetAttribute ("RealVal", StringValue ("100"));
  gooseRogue.SetAttribute ("AttackStartTime", StringValue ("10"));
  gooseRogue.SetAttribute ("AttackEndTime", StringValue ("20"));

  Ptr<GooseApplicationNew> rogue =
    gooseRogue.Install (nodes.Get (2), std::string ("GooseInsiderMIM1"));
  // freq is the rogue's own re-forge interval in ms (see attack_data's
  // mitm_flag dispatch) -- must be a real positive interval, not 0,
  // since 0ms would reschedule with no delay and spin a tight,
  // real-time-devouring loop without simulated time ever advancing.
  Simulator::Schedule (MilliSeconds (5), &GooseApplicationNew::attack_data, rogue, 500);

  Simulator::Stop (Seconds (simTimeSeconds));
  NS_LOG_INFO ("Starting simulation...");
  Simulator::Run ();
  Simulator::Destroy ();
  NS_LOG_INFO ("Simulation complete.");

  return 0;
}
