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
 * MMS HELICS integration test.
 *
 * Mirrors ns3-modbus-helics-test.cc: adds a real HelicsHelper broker
 * setup to a 2-node client/server topology, to confirm the server's
 * SetEndpointName/DoEndpoint/Store() path correctly receives and
 * processes a real HELICS message from an external federate (a
 * helics_app player standing in for GridLAB-D -- see that file's
 * identical reasoning on why replicating GridLAB-D's real publication
 * config is out of scope here).
 *
 * Uses the same arbitrary point-naming approach as the Modbus test:
 * DoEndpoint's variable-name dispatch has a fallback branch that still
 * calls Store() for unrecognized names, so this doesn't need to fake
 * real GridLAB-D conventions to exercise the endpoint/Store path.
 *
 * MOST LIKELY POINT OF FRICTION: same as the Modbus test -- the exact
 * addressable name of a locally-registered (non-global) HELICS
 * endpoint from another federate's perspective ("ns3/<name>", believed
 * but unverified).
 *
 * Run alongside (separately, as background processes):
 *   helics_broker --federates=2 --port=23501
 *   helics_app player player_input_mms_test.txt --stop 10
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
#include "ns3/helics-helper.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("MmsHelicsTest");

int
main (int argc, char *argv[])
{
  double simTimeSeconds = 10.0;
  std::string pointsFile = "points_mms_mim_test.csv"; // reuses the
                                                       // $-convention
                                                       // points file
                                                       // from the MIM
                                                       // test

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation duration in seconds", simTimeSeconds);
  cmd.AddValue ("pointsFile", "Path to the MMS points CSV file", pointsFile);

  // -- Set up the real HELICS federate --
  // Port must match the broker's --port argument. Distinct from the
  // Modbus test's port (23500) so both tests' brokers can coexist if
  // ever run at the same time.
  HelicsHelper helicsHelper (23501);
  helicsHelper.SetupCommandLine (cmd);
  cmd.Parse (argc, argv);

  LogComponentEnable ("MmsHelicsTest", LOG_LEVEL_INFO);
  LogComponentEnable ("MmsApplicationNew", LOG_LEVEL_ALL);

  NS_LOG_INFO ("MMS HELICS test: simTime=" << simTimeSeconds << "s, pointsFile=" << pointsFile);

  NS_LOG_INFO ("Calling helicsHelper.SetupApplicationFederate()");
  helicsHelper.SetupApplicationFederate ();
  NS_LOG_INFO ("Federate name: " << helics_federate->getName ());

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

  // -- Server, with a real HELICS endpoint registered via Install()'s --
  // -- automatic SetEndpointName call. --
  MmsApplicationHelperNew mmsServer ("ns3::TcpSocketFactory",
                                     InetSocketAddress (serverAddress, serverPort));
  mmsServer.SetAttribute ("LocalPort", UintegerValue (serverPort));
  mmsServer.SetAttribute ("RemoteAddress", AddressValue (clientAddress));
  mmsServer.SetAttribute ("RemotePort", UintegerValue (clientPort));
  mmsServer.SetAttribute ("isMaster", BooleanValue (false));
  mmsServer.SetAttribute ("Name", StringValue ("MmsHelicsTest1"));
  mmsServer.SetAttribute ("PointsFilename", StringValue (pointsFile));
  mmsServer.SetAttribute ("EnableTCP", BooleanValue (true));
  mmsServer.SetAttribute ("JitterMinNs", DoubleValue (1000));
  mmsServer.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<MmsApplicationNew> server =
    mmsServer.Install (nodes.Get (1), std::string ("MmsHelicsTest1"));

  // -- Client, unchanged from the MIM test --
  MmsApplicationHelperNew mmsClient ("ns3::TcpSocketFactory",
                                     InetSocketAddress (clientAddress, clientPort));
  mmsClient.SetAttribute ("LocalPort", UintegerValue (clientPort));
  mmsClient.SetAttribute ("RemoteAddress", AddressValue (serverAddress));
  mmsClient.SetAttribute ("RemotePort", UintegerValue (serverPort));
  mmsClient.SetAttribute ("isMaster", BooleanValue (true));
  mmsClient.SetAttribute ("Name", StringValue ("MmsClientHelicsTest1"));
  mmsClient.SetAttribute ("PointsFilename", StringValue (pointsFile));
  mmsClient.SetAttribute ("EnableTCP", BooleanValue (true));
  mmsClient.SetAttribute ("JitterMinNs", DoubleValue (1000));
  mmsClient.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<MmsApplicationNew> client =
    mmsClient.Install (nodes.Get (0), std::string ("MmsClientHelicsTest1"));

  Simulator::Stop (Seconds (simTimeSeconds));
  NS_LOG_INFO ("Starting simulation...");
  Simulator::Run ();
  Simulator::Destroy ();
  NS_LOG_INFO ("Simulation complete.");

  return 0;
}
