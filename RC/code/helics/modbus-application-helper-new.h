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
 * Modbus application helper.
 *
 * Ported near-verbatim from Dnp3ApplicationHelperNew: this is a
 * standard ns-3 ObjectFactory-based helper pattern with no
 * DNP3-library dependency, so it transfers directly with type names
 * swapped.
 *
 * Portions of this file were drafted with AI assistance (Claude,
 * Anthropic) and reviewed/adapted by the author.
 *
 * Author: Kenneth Watts (ken.watts@gmail.com)
 */
#ifndef MODBUS_HELICS_HELPER_H
#define MODBUS_HELICS_HELPER_H

#include "ns3/object-factory.h"
#include "ns3/ipv4-address.h"
#include "ns3/node-container.h"
#include "ns3/application-container.h"
#include "ns3/modbus-application-new.h"

namespace ns3 {

/**
 * \ingroup applications
 * \brief A helper to make it easier to instantiate a ModbusApplicationNew
 * on a set of nodes.
 */
class ModbusApplicationHelperNew
{
public:
  /**
   * \param protocol the name of the protocol to use to receive traffic
   *        (a typical value would be ns3::TcpSocketFactory).
   * \param address the local address to bind to.
   */
  ModbusApplicationHelperNew (std::string protocol, Address address);

  void SetAttribute (std::string name, const AttributeValue &value);

  ApplicationContainer Install (NodeContainer c) const;
  ApplicationContainer Install (Ptr<Node> node) const;
  ApplicationContainer Install (std::string nodeName) const;
  Ptr<ModbusApplicationNew> Install (Ptr<Node> node, const std::string &name);

private:
  Ptr<ModbusApplicationNew> InstallPriv (Ptr<Node> node) const;
  Ptr<ModbusApplicationNew> InstallPriv (Ptr<Node> node, const std::string &name);

  ObjectFactory m_factory; //!< Object factory.
};

} // namespace ns3

#endif /* MODBUS_HELICS_HELPER_H */
