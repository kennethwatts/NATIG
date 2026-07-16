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
 * Modbus HELICS integration test.
 *
 * Adds REAL HelicsHelper broker setup to our already-validated 2-node
 * topology (see ns3-modbus-minimal-test.cc), to test whether the
 * outstation's SetEndpointName/DoEndpoint/Store() path correctly
 * receives and processes a real HELICS message from an external
 * federate (a helics_app player standing in for GridLAB-D, since
 * replicating GridLAB-D's real, massive auto-generated HELICS
 * publication config is out of scope for this test -- see project
 * discussion).
 *
 * This test does NOT attempt to replicate GridLAB-D's actual variable
 * naming or publication structure. It uses our own arbitrary point
 * name (Node1$TestRegister1) sent via a player-app message; since
 * DoEndpoint's variable-name dispatch has a fallback branch that still
 * calls Store() for unrecognized names (logging a warning), this
 * should work without needing to fake real GridLAB-D conventions.
 *
 * MOST LIKELY POINT OF FRICTION: the exact addressable name of a
 * locally-registered (non-global) HELICS endpoint from another
 * federate's perspective. Our best understanding is "ns3/<name>"
 * (federate name "ns3" is HelicsHelper's default, "/" is the default
 * separator) -- but this is unverified and may need adjustment based
 * on what actually happens when run.
 *
 * Run alongside (separately, as background processes):
 *   helics_broker --federates=2 --port=23500
 *   helics_app player player_input_modbus_test.txt --stop 10
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
#include "ns3/modbus-application-helper-new.h"
#include "ns3/modbus-application-new.h"
#include "ns3/helics-helper.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("ModbusHelicsTest");

int
main (int argc, char *argv[])
{
  double simTimeSeconds = 10.0;
  std::string pointsFile = "points_modbus_mim_test.csv"; // reuses the
                                                          // $-convention
                                                          // points file
                                                          // from the MIM
                                                          // test

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation duration in seconds", simTimeSeconds);
  cmd.AddValue ("pointsFile", "Path to the Modbus points CSV file", pointsFile);

  // -- Set up the real HELICS federate --
  // Port must match the broker's --port argument.
  HelicsHelper helicsHelper (23500);
  helicsHelper.SetupCommandLine (cmd);
  cmd.Parse (argc, argv);

  LogComponentEnable ("ModbusHelicsTest", LOG_LEVEL_INFO);
  LogComponentEnable ("ModbusApplicationNew", LOG_LEVEL_ALL);

  NS_LOG_INFO ("Modbus HELICS test: simTime=" << simTimeSeconds << "s, pointsFile=" << pointsFile);

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

  Ipv4Address masterAddress = interfaces.GetAddress (0);
  Ipv4Address outstationAddress = interfaces.GetAddress (1);

  uint16_t masterPort = 5020;
  uint16_t outstationPort = 5020;

  // -- Outstation (server), with a real HELICS endpoint registered --
  // via Install()'s automatic SetEndpointName call.
  ModbusApplicationHelperNew modbusOutstation ("ns3::TcpSocketFactory",
                                                InetSocketAddress (outstationAddress, outstationPort));
  modbusOutstation.SetAttribute ("LocalPort", UintegerValue (outstationPort));
  modbusOutstation.SetAttribute ("RemoteAddress", AddressValue (masterAddress));
  modbusOutstation.SetAttribute ("RemotePort", UintegerValue (masterPort));
  modbusOutstation.SetAttribute ("isMaster", BooleanValue (false));
  modbusOutstation.SetAttribute ("Name", StringValue ("ModbusHelicsTest1"));
  modbusOutstation.SetAttribute ("PointsFilename", StringValue (pointsFile));
  modbusOutstation.SetAttribute ("UnitId", UintegerValue (1));
  modbusOutstation.SetAttribute ("EnableTCP", BooleanValue (true));
  modbusOutstation.SetAttribute ("JitterMinNs", DoubleValue (1000));
  modbusOutstation.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<ModbusApplicationNew> outstation =
    modbusOutstation.Install (nodes.Get (1), std::string ("ModbusHelicsTest1"));

  // -- Master (client), unchanged from the validated minimal test --
  ModbusApplicationHelperNew modbusMaster ("ns3::TcpSocketFactory",
                                            InetSocketAddress (masterAddress, masterPort));
  modbusMaster.SetAttribute ("LocalPort", UintegerValue (masterPort));
  modbusMaster.SetAttribute ("RemoteAddress", AddressValue (outstationAddress));
  modbusMaster.SetAttribute ("RemotePort", UintegerValue (outstationPort));
  modbusMaster.SetAttribute ("isMaster", BooleanValue (true));
  modbusMaster.SetAttribute ("Name", StringValue ("ModbusMasterHelicsTest1"));
  modbusMaster.SetAttribute ("PointsFilename", StringValue (pointsFile));
  modbusMaster.SetAttribute ("UnitId", UintegerValue (1));
  modbusMaster.SetAttribute ("EnableTCP", BooleanValue (true));
  modbusMaster.SetAttribute ("JitterMinNs", DoubleValue (1000));
  modbusMaster.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<ModbusApplicationNew> master =
    modbusMaster.Install (nodes.Get (0), std::string ("ModbusMasterHelicsTest1"));

  Simulator::Stop (Seconds (simTimeSeconds));
  NS_LOG_INFO ("Starting simulation...");
  Simulator::Run ();
  Simulator::Destroy ();
  NS_LOG_INFO ("Simulation complete.");

  return 0;
}
