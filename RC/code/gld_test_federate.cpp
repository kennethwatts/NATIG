/*
 * Minimal standalone HELICS federate, standing in for GridLAB-D.
 *
 * Built as a plain C++ program (NOT an ns-3 scratch program), compiled
 * directly against the same HELICS C++ library our ns-3 module links
 * against. This exists because helics_app player produced silent,
 * undiagnosable failures (no output even at --loglevel=7) when given
 * a combined endpoints+messages JSON config -- see project discussion.
 *
 * API usage here is deliberately copied from our OWN already-working
 * code, not guessed fresh:
 *   - FederateInfo/CombinationFederate setup: see helics-helper.cc's
 *     HelicsHelper::SetupFederate()
 *   - sendMessage's exact 4-argument signature (endpoint, dest string,
 *     data pointer, data size): see modbus-application-new.cc's DoRead()
 *
 * Sends one message to our ns-3 Modbus outstation's endpoint
 * (ns3/ModbusHelicsTest1, confirmed via ground-truth diagnostic to be
 * the actual registered name) at simulated t=2.0s, matching our points
 * file's naming convention (Node1$TestRegister1).
 */

#include "helics/helics.hpp"
#include <iostream>
#include <memory>
#include <string>

int
main (int argc, char **argv)
{
  helics::FederateInfo fi{};
  fi.broker = "";
  fi.brokerPort = 23500;
  fi.coreType = helics::coreTypeFromString ("zmq");
  fi.setProperty (helics_property_time_delta, helics::loadTimeFromString ("1ns"));

  std::cout << "Creating federate 'gld_test_federate'..." << std::endl;
  auto fed = std::make_shared<helics::CombinationFederate> ("gld_test_federate", fi);
  std::cout << "Federate created." << std::endl;

  helics::Endpoint ep = fed->registerEndpoint ("fout");
  std::cout << "Registered endpoint: " << ep.getName () << std::endl;

  std::cout << "Entering executing mode..." << std::endl;
  fed->enterExecutingMode ();
  std::cout << "Entered executing mode." << std::endl;

  helics::Time granted = fed->requestTime (2.0);
  std::cout << "Granted time: " << static_cast<double>(granted) << std::endl;

  std::string message = "{\"Node1\": {\"TestRegister1\": \"777\"}}";
  std::string dest = "ns3/ModbusHelicsTest1";
  fed->sendMessage (ep, dest, message.data (), message.size ());
  std::cout << "Sent message to " << dest << ": " << message << std::endl;

  granted = fed->requestTime (10.0);
  std::cout << "Final granted time: " << static_cast<double>(granted) << std::endl;

  fed->finalize ();
  std::cout << "Finalized." << std::endl;

  return 0;
}
