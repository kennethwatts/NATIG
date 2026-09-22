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
 * Minimal Modbus MIM (false data injection) attack test.
 *
 * Based on ns3-modbus-minimal-test.cc (which validated the normal
 * request/response path), with one change: the outstation's name
 * contains "MIM", causing HandleRead to dispatch incoming requests to
 * handle_MIM instead of handle_normal, and mitmFlag/AttackConf/
 * NodeID/PointID/Value_attck are set to configure a false-data-
 * injection attack against TestRegister1 for the entire simulation
 * window.
 *
 * IMPORTANT: handle_MIM's node/point matching works by searching for
 * "NodeID$PointID" as a substring within the points file's point
 * names (see initConfig/handle_MIM). The points file used here
 * (points_modbus_mim_test.csv) uses "Node1$TestRegister1" naming to
 * satisfy this -- our original points_modbus_test.csv (no "$") would
 * never match and the attack would silently never apply.
 *
 * Expected result: TestRegister1's real value is 100. With the attack
 * active for the whole run (PointStart=0, PointStop=30, chance=1.0),
 * every register-1 poll should return 132000 (the injected value, from
 * Value_attck) instead of 100. 132000 matches a real configured FDI
 * attack magnitude seen in recorder output, and exceeds a raw 16-bit
 * register's 65535 range -- exercising the analog register scale fix
 * (m_registerScale/GetRegisterScale in modbus-application-new.cc):
 * handle_MIM's ModbusApplication::handle_MIM log line should show the
 * injected value reconstructing from the scaled register readback to
 * within +-2*scale of 132000.
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

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("ModbusMIMTest");

int
main (int argc, char *argv[])
{
  double simTimeSeconds = 30.0;
  int pollIntervalMs = 2000;
  std::string pointsFile = "points_modbus_mim_test.csv";
  std::string attackConfFile = "attack_config_test.json";

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation duration in seconds", simTimeSeconds);
  cmd.AddValue ("pollInterval", "Master poll interval in milliseconds", pollIntervalMs);
  cmd.AddValue ("pointsFile", "Path to the Modbus points CSV file", pointsFile);
  cmd.AddValue ("attackConfFile", "Path to the MIM attack config JSON file", attackConfFile);
  cmd.Parse (argc, argv);

  LogComponentEnable ("ModbusMIMTest", LOG_LEVEL_INFO);
  LogComponentEnable ("ModbusApplicationNew", LOG_LEVEL_ALL);

  NS_LOG_INFO ("Modbus MIM test: simTime=" << simTimeSeconds
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

  Ipv4Address masterAddress = interfaces.GetAddress (0);
  Ipv4Address outstationAddress = interfaces.GetAddress (1);

  uint16_t masterPort = 5020;
  uint16_t outstationPort = 5020;

  // -- Outstation (server), renamed to trigger handle_MIM dispatch --
  ModbusApplicationHelperNew modbusOutstation ("ns3::TcpSocketFactory",
                                                InetSocketAddress (outstationAddress, outstationPort));
  modbusOutstation.SetAttribute ("LocalPort", UintegerValue (outstationPort));
  modbusOutstation.SetAttribute ("RemoteAddress", AddressValue (masterAddress));
  modbusOutstation.SetAttribute ("RemotePort", UintegerValue (masterPort));
  modbusOutstation.SetAttribute ("isMaster", BooleanValue (false));
  modbusOutstation.SetAttribute ("Name", StringValue ("ModbusOutstationMIM1"));
  modbusOutstation.SetAttribute ("PointsFilename", StringValue (pointsFile));
  modbusOutstation.SetAttribute ("UnitId", UintegerValue (1));
  modbusOutstation.SetAttribute ("EnableTCP", BooleanValue (true));
  modbusOutstation.SetAttribute ("JitterMinNs", DoubleValue (1000));
  modbusOutstation.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  // -- Attack configuration --
  modbusOutstation.SetAttribute ("mitmFlag", BooleanValue (true));
  modbusOutstation.SetAttribute ("AttackConf", StringValue (attackConfFile));
  modbusOutstation.SetAttribute ("ID", UintegerValue (1)); // MIM_ID, matches JSON array index 1
  modbusOutstation.SetAttribute ("NodeID", StringValue ("Node1"));
  modbusOutstation.SetAttribute ("PointID", StringValue ("TestRegister1"));
  modbusOutstation.SetAttribute ("Value_attck", StringValue ("132000"));
  modbusOutstation.SetAttribute ("RealVal", StringValue ("100"));

  Ptr<ModbusApplicationNew> outstation =
    modbusOutstation.Install (nodes.Get (1), std::string ("ModbusOutstationMIM1"));

  // -- Master (client), unchanged --
  ModbusApplicationHelperNew modbusMaster ("ns3::TcpSocketFactory",
                                            InetSocketAddress (masterAddress, masterPort));
  modbusMaster.SetAttribute ("LocalPort", UintegerValue (masterPort));
  modbusMaster.SetAttribute ("RemoteAddress", AddressValue (outstationAddress));
  modbusMaster.SetAttribute ("RemotePort", UintegerValue (outstationPort));
  modbusMaster.SetAttribute ("isMaster", BooleanValue (true));
  modbusMaster.SetAttribute ("Name", StringValue ("ModbusMaster1"));
  modbusMaster.SetAttribute ("PointsFilename", StringValue (pointsFile));
  modbusMaster.SetAttribute ("UnitId", UintegerValue (1));
  modbusMaster.SetAttribute ("EnableTCP", BooleanValue (true));
  modbusMaster.SetAttribute ("JitterMinNs", DoubleValue (1000));
  modbusMaster.SetAttribute ("JitterMaxNs", DoubleValue (10000));

  Ptr<ModbusApplicationNew> master =
    modbusMaster.Install (nodes.Get (0), std::string ("ModbusMaster1"));

  Simulator::Schedule (MilliSeconds (1005), &ModbusApplicationNew::periodic_poll,
                        master, pollIntervalMs);

  Simulator::Stop (Seconds (simTimeSeconds));
  NS_LOG_INFO ("Starting simulation...");
  Simulator::Run ();
  Simulator::Destroy ();
  NS_LOG_INFO ("Simulation complete.");

  return 0;
}
