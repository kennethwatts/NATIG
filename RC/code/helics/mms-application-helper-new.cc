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
 * MMS application helper implementation.
 *
 * Ported near-verbatim from ModbusApplicationHelperNew.cc -- same
 * standard ns-3 ObjectFactory pattern, no external protocol-library
 * dependency.
 *
 * Portions of this file were drafted with AI assistance (Claude,
 * Anthropic) and reviewed/adapted by the author.
 *
 * Author: Kenneth Watts (ken.watts@gmail.com)
 */

#include "mms-application-helper-new.h"
#include "ns3/string.h"
#include "ns3/inet-socket-address.h"
#include "ns3/names.h"
#include "ns3/mms-application-new.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"

namespace ns3 {

MmsApplicationHelperNew::MmsApplicationHelperNew (std::string protocol, Address address)
{
  // Deliberately NOT calling m_factory.Set ("Protocol", ...) here.
  // This exact call cost real debugging time on the Modbus side (bug
  // #6 in modbus_tcp_session_summary.md): "Protocol" is a registered
  // attribute on MmsApplicationNew (kept only for helper-constructor
  // signature parity), but it's declared as a TypeIdValue, not a
  // StringValue -- passing a StringValue against a TypeIdChecker is
  // fatal at runtime with an unhelpful ObjectFactory error, and this
  // was the first time this exact two-argument constructor would ever
  // run. m_tid is never read anywhere else in MmsApplicationNew --
  // socket type is decided via the hardcoded
  // TypeId::LookupByName("ns3::TcpSocketFactory") calls in
  // makeTcpConnection, gated by the separate EnableTCP boolean
  // attribute -- so there is nothing for the protocol string to be set
  // against. Applying that lesson from the start here rather than
  // re-discovering it. The parameter stays unused (kept only so every
  // call site's two-argument construction still compiles unchanged).
  (void) protocol;
  m_factory.SetTypeId ("ns3::MmsApplicationNew");
  m_factory.Set ("LocalAddress", AddressValue (address));
}

void
MmsApplicationHelperNew::SetAttribute (std::string name, const AttributeValue &value)
{
  m_factory.Set (name, value);
}

ApplicationContainer
MmsApplicationHelperNew::Install (Ptr<Node> node) const
{
  return ApplicationContainer (InstallPriv (node));
}

ApplicationContainer
MmsApplicationHelperNew::Install (std::string nodeName) const
{
  Ptr<Node> node = Names::Find<Node> (nodeName);
  return ApplicationContainer (InstallPriv (node));
}

ApplicationContainer
MmsApplicationHelperNew::Install (NodeContainer c) const
{
  ApplicationContainer apps;
  for (NodeContainer::Iterator i = c.Begin (); i != c.End (); ++i)
    {
      apps.Add (InstallPriv (*i));
    }

  return apps;
}

Ptr<MmsApplicationNew>
MmsApplicationHelperNew::Install (Ptr<Node> node, const std::string &name)
{
  return InstallPriv (node, name);
}

Ptr<MmsApplicationNew>
MmsApplicationHelperNew::InstallPriv (Ptr<Node> node) const
{
  Ptr<MmsApplicationNew> app = m_factory.Create<MmsApplicationNew> ();
  BooleanValue ptr;
  app->GetAttribute ("isMaster", ptr);
  StringValue name;
  app->GetAttribute ("Name", name);
  app->SetEndpointName (name.Get (), false);

  node->AddApplication (app);

  return app;
}

Ptr<MmsApplicationNew>
MmsApplicationHelperNew::InstallPriv (Ptr<Node> node, const std::string &name)
{
  Ptr<MmsApplicationNew> app = m_factory.Create<MmsApplicationNew> ();
  BooleanValue ptr;
  app->GetAttribute ("isMaster", ptr);

  app->SetEndpointName (name, false);
  node->AddApplication (app);

  return app;
}

} // namespace ns3
