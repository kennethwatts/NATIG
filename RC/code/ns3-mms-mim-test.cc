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
 * Minimal MMS request/response + MIM (false data injection) attack test.
 *
 * Validates the normal READ/WRITE path (client polls every point in
 * points_mms_mim_test.csv every pollInterval ms) and the attack path in
 * one scenario: the server's name contains "MIM", causing HandleRead to
 * dispatch incoming requests to handle_MIM instead of handle_normal, and
 * mitmFlag/AttackConf/NodeID/PointID/Value_attck are set to configure a
 * false-data-injection attack against MMXU1.TotW.mag.f for a bounded
 * window.
 *
 * Unlike Modbus's equivalent test, handle_MIM's point matching here
 * compares the MIM-configured "NodeID$PointID" substring directly
 * against the request's object reference -- no numeric address is
 * involved at any point (see mms-application-new.h's design note).
 *
 * Expected result: MMXU1.TotW.mag.f's real value is 100. With the
 * attack active for PointStart=10/PointStop=20 (chance=1.0), every read
 * of that point in that window should return 9999 (Value_attck)
 * instead of 100; outside that window it should read back 100.
 *
 * Portions of this file were drafted with AI assistance (Claude,
 * Anthropic) and reviewed/adapted by the author.
 *
 * Author: Kenneth Watts (ken.watts@gmail.com)
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/mms-application-helper-new.h"
#include "ns3/mms-application-new.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("MmsMIMTest");

int
main (int argc, char *argv[])
{
  double simTimeSeconds = 30.0;
  int pollIntervalMs = 2000;
  std::string pointsFile = "points_mms_mim_test.csv";
  std::string attackConfFile = "attack_config_mms_test.json";

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation duration in seconds", simTimeSeconds);
  cmd.AddValue ("pollInterval", "Client poll interval in milliseconds", pollIntervalMs);
  cmd.AddValue ("pointsFile", "Path to the MMS points CSV file", pointsFile);
  cmd.AddValue ("attackConfFile", "Path to the MIM attack config JSON file", attackConfFile);
  cmd.Parse (argc, argv);

  LogComponentEnable ("MmsMIMTest", LOG_LEVEL_INFO);
  LogComponentEnable ("MmsApplicationNew", LOG_LEVEL_ALL);

  NS_LOG_INFO ("MMS MIM test: simTime=" << simTimeSeconds
               << "s, pollInterval=" << pollIntervalMs << "ms, pointsFile=" << pointsFile
               << ", attackConfFile=" << attackConfFile);

  NodeContainer nodes;
  nodes.Create (2);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("5Mbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("2ms"));
  NetDeviceContainer devices = p2p.Install (nodes);

  InternetStackHelper internetStack;
  internetStack.Install (nodes);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer interfaces = ipv4.Assign (devices);

  Ipv4Address clientAddress = interfaces.GetAddress (0);
  Ipv4Address serverAddress = interfaces.GetAddress (1);

  uint16_t clientPort = 102; // MMS's conventional TCP port in real deployments
  uint16_t serverPort = 102;

  // -- Server, renamed to trigger handle_MIM dispatch --
  MmsApplicationHelperNew mmsServer ("ns3::TcpSocketFactory",
                                      InetSocketAddress (serverAddress, serverPort));
  mmsServer.SetAttribute ("LocalPort", UintegerValue (serverPort));
  mmsServer.SetAttribute ("RemoteAddress", AddressValue (clientAddress));
  mmsServer.SetAttribute ("RemotePort", UintegerValue (clientPort));
  mmsServer.SetAttribute ("isMaster", BooleanValue (false));
  mmsServer.SetAttribute ("Name", StringValue ("MmsServerMIM1"));
  mmsServer.SetAttribute ("PointsFilename", StringValue (pointsFile));
  mmsServer.SetAttribute ("EnableTCP", BooleanValue (true));
  mmsServer.SetAttribute ("JitterMinNs", DoubleValue (1000));
  mmsServer.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  // -- Attack configuration --
  mmsServer.SetAttribute ("mitmFlag", BooleanValue (true));
  mmsServer.SetAttribute ("AttackConf", StringValue (attackConfFile));
  mmsServer.SetAttribute ("ID", UintegerValue (1)); // MIM_ID, matches JSON array index 1
  mmsServer.SetAttribute ("NodeID", StringValue ("Node1"));
  mmsServer.SetAttribute ("PointID", StringValue ("MMXU1.TotW.mag.f"));
  mmsServer.SetAttribute ("Value_attck", StringValue ("9999"));
  mmsServer.SetAttribute ("RealVal", StringValue ("100"));

  Ptr<MmsApplicationNew> server =
    mmsServer.Install (nodes.Get (1), std::string ("MmsServerMIM1"));

  // -- Client, unchanged --
  MmsApplicationHelperNew mmsClient ("ns3::TcpSocketFactory",
                                      InetSocketAddress (clientAddress, clientPort));
  mmsClient.SetAttribute ("LocalPort", UintegerValue (clientPort));
  mmsClient.SetAttribute ("RemoteAddress", AddressValue (serverAddress));
  mmsClient.SetAttribute ("RemotePort", UintegerValue (serverPort));
  mmsClient.SetAttribute ("isMaster", BooleanValue (true));
  mmsClient.SetAttribute ("Name", StringValue ("MmsClient1"));
  mmsClient.SetAttribute ("PointsFilename", StringValue (pointsFile));
  mmsClient.SetAttribute ("EnableTCP", BooleanValue (true));
  mmsClient.SetAttribute ("JitterMinNs", DoubleValue (1000));
  mmsClient.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<MmsApplicationNew> client =
    mmsClient.Install (nodes.Get (0), std::string ("MmsClient1"));

  Simulator::Schedule (MilliSeconds (1005), &MmsApplicationNew::periodic_poll,
                        client, pollIntervalMs);

  Simulator::Stop (Seconds (simTimeSeconds));
  NS_LOG_INFO ("Starting simulation...");
  Simulator::Run ();
  Simulator::Destroy ();
  NS_LOG_INFO ("Simulation complete.");

  return 0;
}
