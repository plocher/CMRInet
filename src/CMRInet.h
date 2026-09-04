// CMRInet.h — umbrella header for the CMRInet library.
//
// VALIDATION: Design v1.1 D1: all public types live in namespace
// CMRInet and are spelled by the naming grammar (e.g.
// CMRInet::CMRIPacket).

#pragma once

#include "CMRIFrameCodec.h"
#include "CMRIHost.h"
#include "CMRINode.h"
#include "CMRIPacket.h"
#include "CMRITime.h"
#include "CMRITransport.h"
#include "ConformanceFault.h"
#include "IOBuffer.h"
#include "RemoteNodeHandle.h"
#include "NodeInit.h"
// Transport implementations live under transport/. Include the one
// your sketch chooses (e.g. transport/serialESP32.h); the umbrella
// carries only the seam (CMRITransport.h), not implementations.
