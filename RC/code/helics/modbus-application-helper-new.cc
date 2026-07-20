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
 * Modbus application helper implementation.
 *
 * Ported near-verbatim from Dnp3ApplicationHelperNew.cc -- same
 * standard ns-3 ObjectFactory pattern, no DNP3-library dependency.
 *
 * Portions of this file were drafted with AI assistance (Claude,
 * Anthropic) and reviewed/adapted by the author.
 *
 * Author: Kenneth Watts (ken.watts@gmail.com)
 */

#include "modbus-application-helper-new.h"
#include "ns3/string.h"
#include "ns3/inet-socket-address.h"
#include "ns3/names.h"
#include "ns3/modbus-application-new.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"

namespace ns3 {

ModbusApplicationHelperNew::ModbusApplicationHelperNew (std::string protocol, Address address)
{
  // BUG FIX: this used to also call m_factory.Set("Protocol", ...).
  // Unlike "LocalAddress" below (a real attribute -- see
  // ModbusApplicationNew::GetTypeId()), "Protocol" was never registered
  // there at all. ObjectFactory::Set() on an unknown attribute name is
  // fatal, aborting the very first time this constructor ever actually
  // ran. ModbusApplicationNew doesn't need a stored "Protocol" value --
  // it always uses TCP explicitly via TypeId::LookupByName in
  // makeTcpConnection, gated by the separate EnableTCP attribute -- so
  // the protocol string has nothing to be set against. Dropping just
  // that one Set() call; the parameter stays unused (kept only so
  // every existing call site's two-argument construction still
  // compiles unchanged).
  (void) protocol;
  m_factory.SetTypeId ("ns3::ModbusApplicationNew");
  m_factory.Set ("LocalAddress", AddressValue (address));
}

void
ModbusApplicationHelperNew::SetAttribute (std::string name, const AttributeValue &value)
{
  m_factory.Set (name, value);
}

ApplicationContainer
ModbusApplicationHelperNew::Install (Ptr<Node> node) const
{
  return ApplicationContainer (InstallPriv (node));
}

ApplicationContainer
ModbusApplicationHelperNew::Install (std::string nodeName) const
{
  Ptr<Node> node = Names::Find<Node> (nodeName);
  return ApplicationContainer (InstallPriv (node));
}

ApplicationContainer
ModbusApplicationHelperNew::Install (NodeContainer c) const
{
  ApplicationContainer apps;
  for (NodeContainer::Iterator i = c.Begin (); i != c.End (); ++i)
    {
      apps.Add (InstallPriv (*i));
    }

  return apps;
}

Ptr<ModbusApplicationNew>
ModbusApplicationHelperNew::Install (Ptr<Node> node, const std::string &name)
{
  return InstallPriv (node, name);
}

Ptr<ModbusApplicationNew>
ModbusApplicationHelperNew::InstallPriv (Ptr<Node> node) const
{
  Ptr<ModbusApplicationNew> app = m_factory.Create<ModbusApplicationNew> ();
  BooleanValue ptr;
  app->GetAttribute ("isMaster", ptr);
  StringValue name;
  app->GetAttribute ("Name", name);
  app->SetEndpointName (name.Get (), false);

  node->AddApplication (app);

  return app;
}

Ptr<ModbusApplicationNew>
ModbusApplicationHelperNew::InstallPriv (Ptr<Node> node, const std::string &name)
{
  Ptr<ModbusApplicationNew> app = m_factory.Create<ModbusApplicationNew> ();
  BooleanValue ptr;
  app->GetAttribute ("isMaster", ptr);

  app->SetEndpointName (name, false);
  node->AddApplication (app);

  return app;
}

} // namespace ns3
