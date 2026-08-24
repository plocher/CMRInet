// CMRInet.h — umbrella header for the CMRInet library.
//
// VALIDATION: Design v1.1 D1: all public types live in namespace
// CMRInet and are spelled by the naming grammar (e.g.
// CMRInet::CMRIPacket).

#pragma once

#include "CMRIFrameCodec.h"
#include "CMRIHost.h"
#include "CMRIPacket.h"
#include "CMRISerialPort.h"
#include "CMRITime.h"
#include "CMRITransport.h"
#include "ConformanceFault.h"
#include "MockCMRITransport.h"
#include "RemoteNodeHandle.h"
#include "SerialCMRITransport.h"
#include "StreamCMRISerialPort.h"  // Arduino-only; empty on desktop
