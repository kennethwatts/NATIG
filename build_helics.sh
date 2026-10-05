#!/bin/bash

cd /rd2c/ns-3-dev/contrib/
rm -r helics
git clone --depth 1 --branch HELICS-v2.x-waf https://github.com/GMLC-TDC/helics-ns3.git; mv helics-ns3 helics
cp -r /rd2c/PUSH/NATIG/RC/code/helics/helics-helper* /rd2c/ns-3-dev/contrib/helics/helper/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/dnp3-application-helper-new.* /rd2c/ns-3-dev/contrib/helics/helper/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/dnp3-application-new* /rd2c/ns-3-dev/contrib/helics/model/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/modbus-application-helper-new.* /rd2c/ns-3-dev/contrib/helics/helper/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/modbus-application-new* /rd2c/ns-3-dev/contrib/helics/model/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/mms-application-helper-new.* /rd2c/ns-3-dev/contrib/helics/helper/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/mms-application-new* /rd2c/ns-3-dev/contrib/helics/model/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/goose-application-helper-new.* /rd2c/ns-3-dev/contrib/helics/helper/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/goose-application-new* /rd2c/ns-3-dev/contrib/helics/model/
# The regular dnp3-application-new.cc/.h (copied by the dnp3-application-new* glob above) is
# used for Docker too; the Docker-specific variant was retired (see build_ns3.sh note).
cp -r /rd2c/PUSH/NATIG/RC/code/helics/wscript /rd2c/ns-3-dev/contrib/helics/
cp -r /rd2c/PUSH/NATIG/RC/code/helics/helics-simulator-impl.cc /rd2c/ns-3-dev/contrib/helics/model/
