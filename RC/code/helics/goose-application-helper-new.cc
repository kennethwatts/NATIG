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
 * GOOSE application helper implementation.
 *
 * Ported near-verbatim from ModbusApplicationHelperNew.cc/
 * MmsApplicationHelperNew.cc -- same standard ns-3 ObjectFactory
 * pattern, no external protocol-library dependency.
 *
 * Portions of this file were drafted with AI assistance (Claude,
 * Anthropic) and reviewed/adapted by the author.
 *
 * Author: Kenneth Watts (ken.watts@gmail.com)
 */

#include "goose-application-helper-new.h"
#include "ns3/string.h"
#include "ns3/inet-socket-address.h"
#include "ns3/names.h"
#include "ns3/goose-application-new.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"

namespace ns3 {

GooseApplicationHelperNew::GooseApplicationHelperNew (std::string protocol, Address address)
{
  // Deliberately NOT calling m_factory.Set ("Protocol", ...) -- see
  // ModbusApplicationHelperNew's and MmsApplicationHelperNew's
  // identical comment and the real debugging cost this exact call
  // caused on Modbus (bug #6 in modbus_tcp_session_summary.md).
  // "Protocol" is a registered TypeIdValue attribute kept only for
  // helper-constructor signature parity; m_tid is never read anywhere
  // else in GooseApplicationNew.
  (void) protocol;
  m_factory.SetTypeId ("ns3::GooseApplicationNew");
  m_factory.Set ("LocalAddress", AddressValue (address));
}

void
GooseApplicationHelperNew::SetAttribute (std::string name, const AttributeValue &value)
{
  m_factory.Set (name, value);
}

ApplicationContainer
GooseApplicationHelperNew::Install (Ptr<Node> node) const
{
  return ApplicationContainer (InstallPriv (node));
}

ApplicationContainer
GooseApplicationHelperNew::Install (std::string nodeName) const
{
  Ptr<Node> node = Names::Find<Node> (nodeName);
  return ApplicationContainer (InstallPriv (node));
}

ApplicationContainer
GooseApplicationHelperNew::Install (NodeContainer c) const
{
  ApplicationContainer apps;
  for (NodeContainer::Iterator i = c.Begin (); i != c.End (); ++i)
    {
      apps.Add (InstallPriv (*i));
    }

  return apps;
}

Ptr<GooseApplicationNew>
GooseApplicationHelperNew::Install (Ptr<Node> node, const std::string &name)
{
  return InstallPriv (node, name);
}

Ptr<GooseApplicationNew>
GooseApplicationHelperNew::InstallPriv (Ptr<Node> node) const
{
  Ptr<GooseApplicationNew> app = m_factory.Create<GooseApplicationNew> ();
  BooleanValue ptr;
  app->GetAttribute ("isMaster", ptr);
  StringValue name;
  app->GetAttribute ("Name", name);
  app->SetEndpointName (name.Get (), false);

  node->AddApplication (app);

  return app;
}

Ptr<GooseApplicationNew>
GooseApplicationHelperNew::InstallPriv (Ptr<Node> node, const std::string &name)
{
  Ptr<GooseApplicationNew> app = m_factory.Create<GooseApplicationNew> ();
  BooleanValue ptr;
  app->GetAttribute ("isMaster", ptr);

  app->SetEndpointName (name, false);
  node->AddApplication (app);

  return app;
}

} // namespace ns3
