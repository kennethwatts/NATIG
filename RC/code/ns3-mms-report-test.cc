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
 * MMS unsolicited Report (attack_data) test.
 *
 * Exercises the one capability that has no DNP3/Modbus equivalent at
 * all in this codebase: a server-role instance spontaneously pushing
 * its full current point set to connected clients, on a timer,
 * without ever being polled (see MmsApplicationNew::attack_data and
 * the design note on MmsServiceCode::REPORT). Deliberately isolated
 * from the MIM attack path (see ns3-mms-mim-test.cc) so a failure here
 * points at the Report mechanism itself rather than at attack
 * injection.
 *
 * No client-side periodic_poll is scheduled here at all -- if the
 * client's perf.txt / Record()'d packet count is nonzero and the log
 * shows "received unsolicited Report" entries, the server must have
 * pushed unprompted, since nothing ever asked it to.
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

NS_LOG_COMPONENT_DEFINE ("MmsReportTest");

int
main (int argc, char *argv[])
{
  double simTimeSeconds = 15.0;
  int reportIntervalMs = 3000;
  std::string pointsFile = "points_mms_mim_test.csv";

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation duration in seconds", simTimeSeconds);
  cmd.AddValue ("reportInterval", "Server unsolicited Report interval in milliseconds", reportIntervalMs);
  cmd.AddValue ("pointsFile", "Path to the MMS points CSV file", pointsFile);
  cmd.Parse (argc, argv);

  LogComponentEnable ("MmsReportTest", LOG_LEVEL_INFO);
  LogComponentEnable ("MmsApplicationNew", LOG_LEVEL_ALL);

  NS_LOG_INFO ("MMS Report test: simTime=" << simTimeSeconds
               << "s, reportInterval=" << reportIntervalMs << "ms, pointsFile=" << pointsFile);

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

  uint16_t clientPort = 102;
  uint16_t serverPort = 102;

  MmsApplicationHelperNew mmsServer ("ns3::TcpSocketFactory",
                                     InetSocketAddress (serverAddress, serverPort));
  mmsServer.SetAttribute ("LocalPort", UintegerValue (serverPort));
  mmsServer.SetAttribute ("RemoteAddress", AddressValue (clientAddress));
  mmsServer.SetAttribute ("RemotePort", UintegerValue (clientPort));
  mmsServer.SetAttribute ("isMaster", BooleanValue (false));
  mmsServer.SetAttribute ("Name", StringValue ("MmsReportServer1"));
  mmsServer.SetAttribute ("PointsFilename", StringValue (pointsFile));
  mmsServer.SetAttribute ("EnableTCP", BooleanValue (true));
  mmsServer.SetAttribute ("JitterMinNs", DoubleValue (1000));
  mmsServer.SetAttribute ("JitterMaxNs", DoubleValue (10000));
  mmsServer.SetAttribute ("ReportIntervalMs", UintegerValue (reportIntervalMs));

  Ptr<MmsApplicationNew> server =
    mmsServer.Install (nodes.Get (1), std::string ("MmsReportServer1"));

  MmsApplicationHelperNew mmsClient ("ns3::TcpSocketFactory",
                                     InetSocketAddress (clientAddress, clientPort));
  mmsClient.SetAttribute ("LocalPort", UintegerValue (clientPort));
  mmsClient.SetAttribute ("RemoteAddress", AddressValue (serverAddress));
  mmsClient.SetAttribute ("RemotePort", UintegerValue (serverPort));
  mmsClient.SetAttribute ("isMaster", BooleanValue (true));
  mmsClient.SetAttribute ("Name", StringValue ("MmsReportClient1"));
  mmsClient.SetAttribute ("PointsFilename", StringValue (pointsFile));
  mmsClient.SetAttribute ("EnableTCP", BooleanValue (true));
  mmsClient.SetAttribute ("JitterMinNs", DoubleValue (1000));
  mmsClient.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<MmsApplicationNew> client =
    mmsClient.Install (nodes.Get (0), std::string ("MmsReportClient1"));

  // No periodic_poll scheduled for the client -- see file header note.
  // attack_data is kicked off externally, the same way periodic_poll is
  // externally kicked off for the client role in every other NATIG
  // protocol test/topology file; it reschedules itself thereafter.
  Simulator::Schedule (MilliSeconds (1005), &MmsApplicationNew::attack_data,
                        server, reportIntervalMs);

  Simulator::Stop (Seconds (simTimeSeconds));
  NS_LOG_INFO ("Starting simulation...");
  Simulator::Run ();
  Simulator::Destroy ();
  NS_LOG_INFO ("Simulation complete.");

  return 0;
}
