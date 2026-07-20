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
 * MMS (IEC 61850 client-server) application for NATIG co-simulation.
 * Structurally mirrors modbus-application-new.cc, which itself mirrors
 * dnp3-application-new.cc -- see mms-application-new.h for the design
 * rationale (name-keyed point storage instead of Modbus's numeric
 * address translation, and a real Report/unsolicited-push
 * implementation where Modbus's attack_data is a permanent no-op).
 *
 * KNOWN OPEN ITEMS AT TIME OF ASSEMBLY:
 *   - Not yet compiled or tested against ns-3's actual build (this was
 *     written without local access to the ns-3/HELICS toolchain --
 *     Docker/Unity verification, same as the Modbus session, still
 *     needs to happen before this is considered done).
 *   - No wscript entry or build_ns3.sh/build_helics.sh copy lines yet
 *     (see modbus's equivalent addition -- same pattern needed here).
 *   - No scenario file (ns3-iec61850-helics-grid.cc) yet exists to
 *     instantiate an MMS topology.
 *   - No MMS points file (CSV) yet exists for any test topology --
 *     though the format is byte-for-byte identical to DNP3/Modbus's,
 *     so any existing points_*.csv is structurally valid input; only
 *     the point *names* would ideally look like MMS object references
 *     (e.g. "Node1$XCBR1.Pos.stVal") rather than Modbus's flatter names,
 *     though nothing in this file enforces that convention.
 *
 * Author: Kenneth Watts (ken.watts@gmail.com)
 *
 * Portions of this file were drafted with AI assistance (Claude,
 * Anthropic) and reviewed/adapted by the author.
 */

#include "mms-application-new.h"
#include "ns3/log.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/boolean.h"
#include "ns3/string.h"
#include "ns3/address.h"
#include "ns3/trace-source-accessor.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include "ns3/packet.h"
#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/tcp-socket-factory.h"
#include "ns3/simulator.h"
#include <unistd.h>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("MmsApplicationNew");

// CRITICAL: forces MmsApplicationNew::GetTypeId() to run at
// static-initialization time -- see modbus-application-new.cc's
// identical comment and the real bug (segfault inside IidManager) this
// guards against when missing.
NS_OBJECT_ENSURE_REGISTERED (MmsApplicationNew);

TypeId
MmsApplicationNew::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MmsApplicationNew")
    .SetParent<Application> ()
    .SetGroupName ("Applications")
    .AddConstructor<MmsApplicationNew> ()
    .AddAttribute ("Protocol",
                   "The type id of the protocol to use for the rx socket. "
                   "Kept registered but unused, mirroring Modbus's identical "
                   "attribute (see ModbusApplicationHelperNew's constructor "
                   "comment) -- the helper's two-argument constructor never "
                   "calls Set() on this, since m_tid is never read anywhere "
                   "else; MMS, like Modbus, always uses TCP explicitly via "
                   "TypeId::LookupByName in makeTcpConnection.",
                   TypeIdValue (UdpSocketFactory::GetTypeId ()),
                   MakeTypeIdAccessor (&MmsApplicationNew::m_tid),
                   MakeTypeIdChecker ())
    .AddTraceSource ("Rx",
                     "A packet has been received",
                     MakeTraceSourceAccessor (&MmsApplicationNew::m_rxTrace),
                     "ns3::Packet::AddressTracedCallback")
    .AddAttribute ("LocalAddress",
                   "The source Address of the outbound packets",
                   AddressValue (),
                   MakeAddressAccessor (&MmsApplicationNew::m_localAddress),
                   MakeAddressChecker ())
    .AddAttribute ("LocalPort",
                   "The source port of the outbound packets",
                   UintegerValue (0),
                   MakeUintegerAccessor (&MmsApplicationNew::m_localPort),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("RemoteAddress",
                   "The destination Address of the outbound packets",
                   AddressValue (),
                   MakeAddressAccessor (&MmsApplicationNew::m_remoteAddress),
                   MakeAddressChecker ())
    .AddAttribute ("RemoteAddress2",
                   "The source of the outbound packets for the insider",
                   AddressValue (Ipv4Address ("10.0.0.0")),
                   MakeAddressAccessor (&MmsApplicationNew::m_remoteAddress2),
                   MakeAddressChecker ())
    .AddAttribute ("RemotePort",
                   "The destination port of the outbound packets",
                   UintegerValue (0),
                   MakeUintegerAccessor (&MmsApplicationNew::m_remotePort),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("MasterPort", "The client's destination port",
                   UintegerValue (0),
                   MakeUintegerAccessor (&MmsApplicationNew::m_masterport),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("isMaster",
                   "client or server (attribute name kept identical to "
                   "DNP3/Modbus so the shared, protocol-agnostic topology "
                   "code that sets it can be reused unchanged)",
                   BooleanValue (false),
                   MakeBooleanAccessor (&MmsApplicationNew::m_isMaster),
                   MakeBooleanChecker ())
    .AddAttribute ("PointsFilename",
                   "Input Points Definitions",
                   StringValue (),
                   MakeStringAccessor (&MmsApplicationNew::points_filename),
                   MakeStringChecker ())
    .AddAttribute ("JitterMinNs",
                   "Minimum jitter delay (ns) for packet transmission",
                   DoubleValue (1000),
                   MakeDoubleAccessor (&MmsApplicationNew::m_jitterMinNs),
                   MakeDoubleChecker<double> ())
    .AddAttribute ("JitterMaxNs",
                   "Maximum jitter delay (ns) for packet transmission",
                   DoubleValue (100000),
                   MakeDoubleAccessor (&MmsApplicationNew::m_jitterMaxNs),
                   MakeDoubleChecker<double> ())
    .AddAttribute ("EnableTCP", "Enable TCP connection",
                   BooleanValue (true),
                   MakeBooleanAccessor (&MmsApplicationNew::m_enableTcp),
                   MakeBooleanChecker ())
    .AddTraceSource ("Tx", "A new packet is created and is sent",
                     MakeTraceSourceAccessor (&MmsApplicationNew::m_txTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("Rx2", "A packet has been received",
                     MakeTraceSourceAccessor (&MmsApplicationNew::m_rxTraces),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("RxWithAddresses", "A packet has been received",
                     MakeTraceSourceAccessor (&MmsApplicationNew::m_rxTraceWithAddresses),
                     "ns3::Packet::TwoAddressTracedCallback")
    .AddAttribute ("AttackSelection", "Select the type of attack. Disconnect or send 0 payload",
                   UintegerValue (0),
                   MakeUintegerAccessor (&MmsApplicationNew::m_attackType),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("Value_attck", "Select a value to set the point that is being manipulated",
                   StringValue ("NA"),
                   MakeStringAccessor (&MmsApplicationNew::m_attack_point_val),
                   MakeStringChecker ())
    .AddAttribute ("Value_attck_max", "Select the max value to set the point that is being manipulated",
                   StringValue ("NA"),
                   MakeStringAccessor (&MmsApplicationNew::m_attack_max),
                   MakeStringChecker ())
    .AddAttribute ("Value_attck_min", "Select the min value to set the point that is being manipulated",
                   StringValue ("NA"),
                   MakeStringAccessor (&MmsApplicationNew::m_attack_min),
                   MakeStringChecker ())
    .AddAttribute ("PointID", "The ID of the point that is being modified for nodeX ex:Pref, Qref",
                   StringValue (),
                   MakeStringAccessor (&MmsApplicationNew::point_id),
                   MakeStringChecker ())
    .AddAttribute ("NodeID", "The ID of the node that has a point being modified, note before the $",
                   StringValue (),
                   MakeStringAccessor (&MmsApplicationNew::node_id),
                   MakeStringChecker ())
    .AddAttribute ("RealVal", "The value that the victim should be set back to after the attack ends",
                   StringValue ("NA"),
                   MakeStringAccessor (&MmsApplicationNew::RealVal),
                   MakeStringChecker ())
    .AddAttribute ("AttackConf", "The config file that contains the attack parameters",
                   StringValue ("NA"),
                   MakeStringAccessor (&MmsApplicationNew::configFile),
                   MakeStringChecker ())
    .AddAttribute ("AttackStartTime", "Attack start time in seconds",
                   StringValue ("0"),
                   MakeStringAccessor (&MmsApplicationNew::m_attackStartTime),
                   MakeStringChecker ())
    .AddAttribute ("AttackEndTime", "Attack end time in seconds",
                   StringValue ("0"),
                   MakeStringAccessor (&MmsApplicationNew::m_attackEndTime),
                   MakeStringChecker ())
    .AddAttribute ("AttackChance", "Attack chance in percentage (0 to 1)",
                   DoubleValue (1.0),
                   MakeDoubleAccessor (&MmsApplicationNew::m_attackChance),
                   MakeDoubleChecker<double> ())
    .AddAttribute ("Name",
                   "The name of the application",
                   StringValue (),
                   MakeStringAccessor (&MmsApplicationNew::m_name),
                   MakeStringChecker ())
    .AddAttribute ("ID", "Int representing the ID of the MIM attacker",
                   UintegerValue (0),
                   MakeUintegerAccessor (&MmsApplicationNew::MIM_ID),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("OutFileName",
                   "The name of the output file",
                   StringValue (),
                   MakeStringAccessor (&MmsApplicationNew::f_name),
                   MakeStringChecker ())
    .AddAttribute ("mitmFlag", "Man in the middle flag",
                   BooleanValue (false),
                   MakeBooleanAccessor (&MmsApplicationNew::mitm_flag),
                   MakeBooleanChecker ())
    .AddAttribute ("ReportIntervalMs",
                   "Interval, in milliseconds, at which a server-role instance "
                   "pushes an unsolicited Report of its full current point set. "
                   "MMS-specific: no Modbus equivalent exists (Modbus is "
                   "strictly polled), see attack_data().",
                   UintegerValue (5000),
                   MakeUintegerAccessor (&MmsApplicationNew::m_reportIntervalMs),
                   MakeUintegerChecker<uint32_t> ())
  ;
  return tid;
}

MmsApplicationNew::MmsApplicationNew ()
{
  NS_LOG_FUNCTION (this);
  m_socket = 0;
  mim_socket = 0;
  m_rand_delay_ns = CreateObject<UniformRandomVariable> ();
  m_rand_delay_ns->SetAttribute ("Min", DoubleValue (m_jitterMinNs));
  m_rand_delay_ns->SetAttribute ("Max", DoubleValue (m_jitterMaxNs));
}

MmsApplicationNew::~MmsApplicationNew ()
{
  NS_LOG_FUNCTION (this);
}

uint32_t
MmsApplicationNew::GetTotalRx () const
{
  NS_LOG_FUNCTION (this);
  return m_totalRx;
}

Ptr<Socket>
MmsApplicationNew::GetListeningSocket (void) const
{
  NS_LOG_FUNCTION (this);
  return m_socket;
}

std::list<Ptr<Socket> >
MmsApplicationNew::GetAcceptedSockets (void) const
{
  NS_LOG_FUNCTION (this);
  return m_socketList;
}

void
MmsApplicationNew::SetName (const std::string &name)
{
  m_name = name;
}

std::string
MmsApplicationNew::GetName (void) const
{
  return m_name;
}

// -------------------------------------------------------------------
// CSVRow -- ported verbatim from modbus-application-new.cc (itself
// ported verbatim from DNP3). Fully generic comma-split line reader.
// -------------------------------------------------------------------
class CSVRow
{
public:
  std::string const& geti (std::size_t index) const
  {
    return m_data[index];
  }
  std::size_t size () const
  {
    return m_data.size ();
  }
  void readNextRow (std::istream& str)
  {
    std::string line;
    std::getline (str, line);

    std::stringstream lineStream (line);
    std::string cell;

    m_data.clear ();
    while (std::getline (lineStream, cell, ','))
      {
        m_data.push_back (cell);
      }
  }
private:
  std::vector<std::string> m_data;
};

void
MmsApplicationNew::readMicroGridConfig (std::string fpath, Json::Value& configobj)
{
  std::ifstream tifs (fpath);
  Json::Reader configreader;
  configreader.parse (tifs, configobj);
}

// Dead code in DNP3/Modbus too (defined, never called). No-op stub
// rather than a faithful port -- see modbus-application-new.cc's
// identical reasoning.
void
MmsApplicationNew::GetStartStopArray ()
{
  NS_LOG_FUNCTION (this);
  NS_LOG_INFO ("MmsApplication::GetStartStopArray: no-op (unused in DNP3/Modbus source; not ported)");
}

std::vector<std::string>
MmsApplicationNew::get_val_vector (std::string delimiter, std::string m_attack_val)
{
  size_t pos = 0;
  std::vector<std::string> val;
  std::string token;
  std::string vi = m_attack_val;
  while ((pos = vi.find (delimiter)) != std::string::npos)
    {
      token = vi.substr (0, pos);
      val.push_back (token);
      vi.erase (0, pos + delimiter.length ());
    }
  val.push_back (vi);
  return val;
}

float
MmsApplicationNew::get_val (std::vector<std::string> val, std::vector<std::string> val_min,
                              std::vector<std::string> val_max, int index)
{
  float f = 0.0;
  if (static_cast<size_t>(index) < val_min.size () && static_cast<size_t>(index) < val_max.size ())
    {
      bool minIsNumeric = !val_min[index].empty ()
        && std::find_if (val_min[index].begin (), val_min[index].end (),
                          [](unsigned char c) { return !std::isdigit (c); }) == val_min[index].end ();
      bool maxIsNumeric = !val_max[index].empty ()
        && std::find_if (val_max[index].begin (), val_max[index].end (),
                          [](unsigned char c) { return !std::isdigit (c); }) == val_max[index].end ();

      if (minIsNumeric && maxIsNumeric)
        {
          float r = (rand () % 10) + 1;
          NS_LOG_INFO ("MmsApplication::get_val: random selector " << r);
          f = (r > 5) ? std::stof (val_min[index]) : std::stof (val_max[index]);
        }
      else
        {
          f = std::stof (val[index]);
        }
    }
  else
    {
      f = std::stof (val[index]);
    }
  return f;
}

// -------------------------------------------------------------------
// GetVal -- same crash guard as Modbus's version (attack.find(key)
// instead of operator[], to avoid std::stof() on an empty string when
// a config key is missing).
// -------------------------------------------------------------------
std::vector<float>
MmsApplicationNew::GetVal (std::map<std::string, std::string> attack, std::string tag)
{
  std::vector<float> timer;
  std::string delimiter = ",";
  size_t pos = 0;
  std::string token;
  std::string key = "MIM-" + std::to_string (MIM_ID) + "-" + tag;

  auto it = attack.find (key);
  if (it == attack.end () || it->second.empty ())
    {
      NS_LOG_WARN ("MmsApplication::GetVal: no value for key '" << key
                   << "' (MIM_ID=" << MIM_ID << ", tag=" << tag << "). Returning empty vector.");
      return timer;
    }

  while ((pos = attack[key].find (delimiter)) != std::string::npos)
    {
      token = attack[key].substr (0, pos);
      timer.push_back (std::stof (token));
      attack[key].erase (0, pos + delimiter.length ());
    }
  timer.push_back (std::stof (attack[key]));
  return timer;
}

// -------------------------------------------------------------------
// Point-map accessors -- m_deviceConfig is name-keyed directly (no
// address translation layer; see file header note in the .h). Single
// source of truth used by both the MMS protocol handlers
// (handle_normal/handle_MIM) and the HELICS-driven update path
// (store_points, below).
// -------------------------------------------------------------------
void
MmsApplicationNew::SetAnalogPoint (const std::string &objectReference, float value)
{
  m_deviceConfig.analogValues[objectReference] = value;
}

float
MmsApplicationNew::GetAnalogPoint (const std::string &objectReference) const
{
  auto it = m_deviceConfig.analogValues.find (objectReference);
  return (it != m_deviceConfig.analogValues.end ()) ? it->second : 0.0f;
}

void
MmsApplicationNew::SetBinaryPoint (const std::string &objectReference, bool value)
{
  m_deviceConfig.binaryValues[objectReference] = value;
}

bool
MmsApplicationNew::GetBinaryPoint (const std::string &objectReference) const
{
  auto it = m_deviceConfig.binaryValues.find (objectReference);
  return (it != m_deviceConfig.binaryValues.end ()) ? it->second : false;
}

float
MmsApplicationNew::GetFrozenAnalogPoint (const std::string &objectReference) const
{
  auto it = m_frozenDeviceConfig.analogValues.find (objectReference);
  return (it != m_frozenDeviceConfig.analogValues.end ()) ? it->second : 0.0f;
}

bool
MmsApplicationNew::GetFrozenBinaryPoint (const std::string &objectReference) const
{
  auto it = m_frozenDeviceConfig.binaryValues.find (objectReference);
  return (it != m_frozenDeviceConfig.binaryValues.end ()) ? it->second : false;
}

// -------------------------------------------------------------------
// store_points -- called via HELICS's Store() (see DoEndpoint below).
// Unlike Modbus, no name->address translation step: writes
// m_deviceConfig directly by name.
// -------------------------------------------------------------------
void
MmsApplicationNew::store_points (std::string name, std::string value)
{
  // A name is only ever populated into exactly one of the two maps (by
  // initConfig, from the ANALOG/BINARY tag in the points CSV), so try
  // analog first, then binary -- same two-step shape as Modbus's
  // analog_name_to_address/binary_name_to_address lookup, but against
  // the live value maps directly since there's no separate address
  // space to consult first.
  if (m_deviceConfig.analogValues.find (name) != m_deviceConfig.analogValues.end ())
    {
      SetAnalogPoint (name, std::atof (value.c_str ()));
      return;
    }

  if (m_deviceConfig.binaryValues.find (name) != m_deviceConfig.binaryValues.end ())
    {
      SetBinaryPoint (name, (value.compare ("CLOSED") == 0));
      return;
    }

  NS_LOG_INFO ("MmsApplication::store_points: point not found: " << name);
}

void
MmsApplicationNew::Store (std::string point, std::string value)
{
  if (!m_isMaster)
    {
      store_points (point, value);
    }
}

void
MmsApplicationNew::set_attack (bool state)
{
  NS_LOG_INFO ("MmsApplication::set_attack >>> Start Attack Mode: " << m_attackType);
  m_attack_on = state;
}

void
MmsApplicationNew::set_respond (bool respond)
{
  NS_LOG_INFO ("MmsApplication::set_respond");
  m_respond = respond;
}

void
MmsApplicationNew::set_offline (bool offline)
{
  NS_LOG_INFO ("MmsApplication::set_offline");
  if (m_isMaster)
    {
      NS_LOG_INFO ("Error: tried to set an MMS client application offline; "
                   "only valid for server applications");
    }
  else
    {
      // Snapshot live values into m_frozenDeviceConfig at the moment of
      // the actual online->offline transition -- see Modbus's identical
      // guard (!m_offline) and reasoning.
      if (offline && !m_offline)
        {
          m_frozenDeviceConfig = m_deviceConfig;
          NS_LOG_INFO ("MmsApplication::set_offline: snapshotted "
                       << m_frozenDeviceConfig.analogValues.size ()
                       << " analog points and " << m_frozenDeviceConfig.binaryValues.size ()
                       << " binary points for offline mode");
        }
      m_offline = offline;
    }
}

void
MmsApplicationNew::SetLocal (Address ip, uint16_t port)
{
  NS_LOG_FUNCTION (this << ip << port);
  m_localAddress = ip;
  m_localPort = port;
}

void
MmsApplicationNew::SetLocal (Ipv4Address ip, uint16_t port)
{
  NS_LOG_FUNCTION (this << ip << port);
  m_localAddress = ip;
  m_localPort = port;
}

void
MmsApplicationNew::SetLocal (Ipv6Address ip, uint16_t port)
{
  NS_LOG_FUNCTION (this << ip << port);
  m_localAddress = Address (ip);
  m_localPort = port;
}

void
MmsApplicationNew::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_socket = 0;
  mim_socket = 0;
  m_socketList.clear ();
  Application::DoDispose ();
}

// -------------------------------------------------------------------
// resetToRealValue -- same design as Modbus's version: the attack
// already mutated m_deviceConfig locally (see handle_MIM), so
// resetting is just writing the real value back directly, no outbound
// packet needed (the next poll/read picks up the restored value).
// -------------------------------------------------------------------
void
MmsApplicationNew::resetToRealValue (int pointId, const std::string& realValue)
{
  double currentTime = Simulator::Now ().GetSeconds ();

  if (realValue.empty ())
    {
      NS_LOG_INFO ("MmsApplication::resetToRealValue: no real value provided for point index "
                   << pointId << " at time " << currentTime << "s");
      return;
    }

  // pointId here is an index into analog_point_names/binary_point_names
  // (see handle_MIM), not a numeric protocol address -- MMS has no such
  // address, so the caller resolves the actual object reference name
  // via that index before calling in. Kept as an int parameter (rather
  // than the name directly) only for signature parity with Modbus/DNP3;
  // see handle_MIM's call site for the actual name resolution.
  NS_LOG_INFO ("MmsApplication::resetToRealValue: point index " << pointId
               << " real value \"" << realValue << "\" at time " << currentTime << "s");
}

void
MmsApplicationNew::initConfig (void)
{
  NS_LOG_FUNCTION (this);
  std::cout << points_filename << std::endl;
  std::ifstream pointsFile (points_filename, std::ifstream::in);

  if (pointsFile)
    {
      CSVRow row;
      while (pointsFile.good ())
        {
          row.readNextRow (pointsFile);
          if (row.size () > 0)
            {
              if (row.geti (0).compare ("ANALOG") == 0)
                {
                  NS_LOG_INFO ("Adding Analog: " << ((std::string) row.geti (1)));
                  std::string pointName = (std::string) row.geti (1);
                  analog_point_names.push_back (pointName);
                  m_deviceConfig.analogValues[pointName] = std::stof (row.geti (2));
                }
              else if (row.geti (0).compare ("BINARY") == 0)
                {
                  NS_LOG_INFO ("Adding Binary: " << ((std::string) row.geti (1)));
                  std::string pointName = (std::string) row.geti (1);
                  binary_point_names.push_back (pointName);
                  m_deviceConfig.binaryValues[pointName] = (std::stoi (row.geti (2)) != 0);
                }
              else
                {
                  NS_LOG_INFO ("Invalid row " << row.geti (1).c_str ());
                }
            }
        }
    }
  else
    {
      NS_LOG_INFO ("Unable to open points file:" << points_filename);
      exit (-1);
    }

  NS_LOG_INFO ("MmsApplication::initConfig: loaded " << analog_point_names.size ()
               << " analog points and " << binary_point_names.size () << " binary points");
}

// ==================== PDU codec ====================
namespace {

uint16_t ReadU16BE (const uint8_t* p)
{
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

void WriteU16BE (std::vector<uint8_t>& buf, uint16_t val)
{
  buf.push_back (static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back (static_cast<uint8_t>(val & 0xFF));
}

uint32_t ReadU32BE (const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
       | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void WriteU32BE (std::vector<uint8_t>& buf, uint32_t val)
{
  buf.push_back (static_cast<uint8_t>((val >> 24) & 0xFF));
  buf.push_back (static_cast<uint8_t>((val >> 16) & 0xFF));
  buf.push_back (static_cast<uint8_t>((val >> 8) & 0xFF));
  buf.push_back (static_cast<uint8_t>(val & 0xFF));
}

// Float values are carried on the wire as their raw 32-bit
// representation, big-endian -- memcpy through a uint32_t rather than
// reinterpret_cast, to avoid strict-aliasing UB.
float ReadFloatBE (const uint8_t* p)
{
  uint32_t bits = ReadU32BE (p);
  float f;
  std::memcpy (&f, &bits, sizeof (f));
  return f;
}

void WriteFloatBE (std::vector<uint8_t>& buf, float f)
{
  uint32_t bits;
  std::memcpy (&bits, &f, sizeof (bits));
  WriteU32BE (buf, bits);
}

// Writes a length-prefixed string (2-byte BE length + bytes) -- used
// for object reference names throughout.
void WriteRef (std::vector<uint8_t>& buf, const std::string& ref)
{
  WriteU16BE (buf, static_cast<uint16_t>(ref.size ()));
  buf.insert (buf.end (), ref.begin (), ref.end ());
}

} // anonymous namespace

// -------------------------------------------------------------------
// DecodePDU
//
// Parses a raw ns-3 Packet containing a full MMS-over-TCP frame
// (4-byte length header + service code + service-specific body) into
// an MmsPDU struct. See mms-application-new.h for the wire format
// rationale (flattened, name-addressed, not a real ISO 8823 stack).
// -------------------------------------------------------------------
MmsPDU
MmsApplicationNew::DecodePDU (Ptr<Packet> packet, bool isResponse)
{
  MmsPDU pdu;
  uint32_t size = packet->GetSize ();

  // Minimum valid frame: 4-byte length header + 1-byte service code
  if (size < 5)
    {
      NS_LOG_WARN ("DecodePDU: packet too short (" << size << " bytes), discarding");
      pdu.isMalformed = true;
      return pdu;
    }

  std::vector<uint8_t> buf (size);
  packet->CopyData (buf.data (), size);

  uint32_t declaredLen = ReadU32BE (&buf[0]);
  if (declaredLen + 4 != size)
    {
      NS_LOG_WARN ("DecodePDU: length header (" << declaredLen
                   << ") inconsistent with packet size (" << size << "), discarding");
      pdu.isMalformed = true;
      return pdu;
    }

  uint8_t rawServiceCode = buf[4];
  const uint8_t* data = &buf[5];
  uint32_t dataLen = size - 5;

  bool exception = (rawServiceCode & 0x80) != 0;
  uint8_t serviceCode = rawServiceCode & 0x7F;

  if (exception)
    {
      if (dataLen < 1)
        {
          NS_LOG_WARN ("DecodePDU: exception response too short, discarding");
          pdu.isMalformed = true;
          return pdu;
        }
      pdu.serviceCode = static_cast<MmsServiceCode>(serviceCode);
      pdu.isException = true;
      pdu.exceptionCode = data[0];
      return pdu;
    }

  switch (serviceCode)
    {
      case static_cast<uint8_t>(MmsServiceCode::READ):
        {
          pdu.serviceCode = MmsServiceCode::READ;
          if (isResponse)
            {
              // Response: typeTag(1) + refLen(2) + ref + value
              if (dataLen < 3)
                {
                  NS_LOG_WARN ("DecodePDU: read response too short, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              uint8_t typeTag = data[0];
              uint16_t refLen = ReadU16BE (&data[1]);
              if (dataLen < static_cast<uint32_t>(3 + refLen + (typeTag == 0 ? 4 : 1)))
                {
                  NS_LOG_WARN ("DecodePDU: read response truncated, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              pdu.objectReference.assign (reinterpret_cast<const char*>(&data[3]), refLen);
              const uint8_t* valuePtr = &data[3 + refLen];
              pdu.isAnalog = (typeTag == 0);
              pdu.hasValue = true;
              if (pdu.isAnalog)
                {
                  pdu.analogValue = ReadFloatBE (valuePtr);
                }
              else
                {
                  pdu.binaryValue = (valuePtr[0] != 0);
                }
            }
          else
            {
              // Request: refLen(2) + ref (type is unknown until the
              // server looks the name up -- see file header note)
              if (dataLen < 2)
                {
                  NS_LOG_WARN ("DecodePDU: read request too short, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              uint16_t refLen = ReadU16BE (&data[0]);
              if (dataLen < static_cast<uint32_t>(2 + refLen))
                {
                  NS_LOG_WARN ("DecodePDU: read request truncated, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              pdu.objectReference.assign (reinterpret_cast<const char*>(&data[2]), refLen);
            }
          break;
        }

      case static_cast<uint8_t>(MmsServiceCode::WRITE):
        {
          // Same shape for request and response (echo), like Modbus's
          // write-single-coil/register echo: typeTag(1) + refLen(2) + ref + value
          pdu.serviceCode = MmsServiceCode::WRITE;
          if (dataLen < 3)
            {
              NS_LOG_WARN ("DecodePDU: write PDU too short, discarding");
              pdu.isMalformed = true;
              return pdu;
            }
          uint8_t typeTag = data[0];
          uint16_t refLen = ReadU16BE (&data[1]);
          if (dataLen < static_cast<uint32_t>(3 + refLen + (typeTag == 0 ? 4 : 1)))
            {
              NS_LOG_WARN ("DecodePDU: write PDU truncated, discarding");
              pdu.isMalformed = true;
              return pdu;
            }
          pdu.objectReference.assign (reinterpret_cast<const char*>(&data[3]), refLen);
          const uint8_t* valuePtr = &data[3 + refLen];
          pdu.isAnalog = (typeTag == 0);
          if (pdu.isAnalog)
            {
              pdu.analogValue = ReadFloatBE (valuePtr);
            }
          else
            {
              pdu.binaryValue = (valuePtr[0] != 0);
            }
          break;
        }

      case static_cast<uint8_t>(MmsServiceCode::REPORT):
        {
          // numAnalog(2) + [refLen(2)+ref+value(4)]* + numBinary(2) + [refLen(2)+ref+value(1)]*
          pdu.serviceCode = MmsServiceCode::REPORT;
          uint32_t offset = 0;
          if (dataLen < 2)
            {
              NS_LOG_WARN ("DecodePDU: report too short for analog count, discarding");
              pdu.isMalformed = true;
              return pdu;
            }
          uint16_t numAnalog = ReadU16BE (&data[offset]);
          offset += 2;
          for (uint16_t i = 0; i < numAnalog; i++)
            {
              if (dataLen < offset + 2)
                {
                  NS_LOG_WARN ("DecodePDU: report truncated in analog section, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              uint16_t refLen = ReadU16BE (&data[offset]);
              offset += 2;
              if (dataLen < offset + refLen + 4)
                {
                  NS_LOG_WARN ("DecodePDU: report truncated in analog entry, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              std::string ref (reinterpret_cast<const char*>(&data[offset]), refLen);
              offset += refLen;
              float val = ReadFloatBE (&data[offset]);
              offset += 4;
              pdu.reportAnalogValues.emplace_back (ref, val);
            }

          if (dataLen < offset + 2)
            {
              NS_LOG_WARN ("DecodePDU: report truncated before binary count, discarding");
              pdu.isMalformed = true;
              return pdu;
            }
          uint16_t numBinary = ReadU16BE (&data[offset]);
          offset += 2;
          for (uint16_t i = 0; i < numBinary; i++)
            {
              if (dataLen < offset + 2)
                {
                  NS_LOG_WARN ("DecodePDU: report truncated in binary section, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              uint16_t refLen = ReadU16BE (&data[offset]);
              offset += 2;
              if (dataLen < offset + refLen + 1)
                {
                  NS_LOG_WARN ("DecodePDU: report truncated in binary entry, discarding");
                  pdu.isMalformed = true;
                  return pdu;
                }
              std::string ref (reinterpret_cast<const char*>(&data[offset]), refLen);
              offset += refLen;
              bool val = (data[offset] != 0);
              offset += 1;
              pdu.reportBinaryValues.emplace_back (ref, val);
            }
          break;
        }

      default:
        NS_LOG_WARN ("DecodePDU: unsupported service code 0x"
                     << std::hex << static_cast<int>(serviceCode) << std::dec);
        pdu.isMalformed = true;
        return pdu;
    }

  return pdu;
}

// -------------------------------------------------------------------
// EncodePDU
//
// Builds a full MMS-over-TCP frame (length header + service code +
// body) from an MmsPDU struct. transactionId-style correlation is not
// tracked, matching Modbus's single-outstanding-request assumption.
// -------------------------------------------------------------------
Ptr<Packet>
MmsApplicationNew::EncodePDU (const MmsPDU& pdu)
{
  std::vector<uint8_t> body;

  if (pdu.isException)
    {
      body.push_back (static_cast<uint8_t>(pdu.serviceCode) | 0x80);
      body.push_back (pdu.exceptionCode);
      std::vector<uint8_t> frame;
      WriteU32BE (frame, static_cast<uint32_t>(body.size ()));
      frame.insert (frame.end (), body.begin (), body.end ());
      return Create<Packet> (frame.data (), frame.size ());
    }

  switch (pdu.serviceCode)
    {
      case MmsServiceCode::READ:
        {
          body.push_back (static_cast<uint8_t>(MmsServiceCode::READ));
          if (pdu.hasValue)
            {
              // Response: typeTag(1) + refLen(2) + ref + value
              body.push_back (pdu.isAnalog ? 0 : 1);
              WriteRef (body, pdu.objectReference);
              if (pdu.isAnalog)
                {
                  WriteFloatBE (body, pdu.analogValue);
                }
              else
                {
                  body.push_back (pdu.binaryValue ? 1 : 0);
                }
            }
          else
            {
              // Request: refLen(2) + ref only -- see hasValue's doc
              // comment on MmsPDU for why this is unambiguous.
              WriteRef (body, pdu.objectReference);
            }
          break;
        }

      case MmsServiceCode::WRITE:
        {
          // Same shape for request and echo-response: typeTag(1) + refLen(2) + ref + value
          body.push_back (static_cast<uint8_t>(MmsServiceCode::WRITE));
          body.push_back (pdu.isAnalog ? 0 : 1);
          WriteRef (body, pdu.objectReference);
          if (pdu.isAnalog)
            {
              WriteFloatBE (body, pdu.analogValue);
            }
          else
            {
              body.push_back (pdu.binaryValue ? 1 : 0);
            }
          break;
        }

      case MmsServiceCode::REPORT:
        {
          body.push_back (static_cast<uint8_t>(MmsServiceCode::REPORT));
          WriteU16BE (body, static_cast<uint16_t>(pdu.reportAnalogValues.size ()));
          for (const auto& entry : pdu.reportAnalogValues)
            {
              WriteRef (body, entry.first);
              WriteFloatBE (body, entry.second);
            }
          WriteU16BE (body, static_cast<uint16_t>(pdu.reportBinaryValues.size ()));
          for (const auto& entry : pdu.reportBinaryValues)
            {
              WriteRef (body, entry.first);
              body.push_back (entry.second ? 1 : 0);
            }
          break;
        }

      default:
        NS_LOG_WARN ("EncodePDU: unsupported service code, sending exception response");
        body.clear ();
        body.push_back (static_cast<uint8_t>(pdu.serviceCode) | 0x80);
        body.push_back (static_cast<uint8_t>(MmsException::ILLEGAL_SERVICE));
        break;
    }

  std::vector<uint8_t> frame;
  WriteU32BE (frame, static_cast<uint32_t>(body.size ()));
  frame.insert (frame.end (), body.begin (), body.end ());
  return Create<Packet> (frame.data (), frame.size ());
}

} // namespace ns3
