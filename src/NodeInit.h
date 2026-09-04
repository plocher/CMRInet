// NodeInit.h — Host-side node type and INIT body builders.
//
// Each official NDP has its own initialization parameter block and a
// pure builder that produces the I-message data body (pre-escape).
// CMRIHost stores the type+init on the slot and calls the builder from
// buildInitPacket_().
//
// NDP map (fielded SPS/JMRI): C=CPNODE, M=SMINI, N=USIC, X=SUSIC.
// VALIDATION: docs/research/node-type-init-bodies.md
// VALIDATION: Interop v1.1 E3 (CPNODE body), E4 (dH/dL units)

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace CMRInet {

/// Official node definition parameter (NDP) characters as fielded.
enum class NodeType : uint8_t {
  kCpnode = 'C',  ///< CPNODE (cpNode family); JMRI dialect I body
  kSmini = 'M',   ///< SMINI
  kUsic = 'N',    ///< USIC (24-bit cards)
  kSusic = 'X',   ///< SUSIC (32-bit cards)
};

inline char nodeTypeNdp(NodeType t) {
  return static_cast<char>(t);
}

inline const char* nodeTypeString(NodeType t) {
  switch (t) {
    case NodeType::kCpnode: return "CPNODE";
    case NodeType::kSmini: return "SMINI";
    case NodeType::kUsic: return "USIC";
    case NodeType::kSusic: return "SUSIC";
  }
  return "UNKNOWN";
}

/// True if `ndp` is a NodeType this library can build an I body for.
inline bool isSupportedNodeTypeNdp(char ndp) {
  return ndp == 'C' || ndp == 'M' || ndp == 'N' || ndp == 'X';
}

inline bool nodeTypeFromNdp(char ndp, NodeType& out) {
  switch (ndp) {
    case 'C': out = NodeType::kCpnode; return true;
    case 'M': out = NodeType::kSmini; return true;
    case 'N': out = NodeType::kUsic; return true;
    case 'X': out = NodeType::kSusic; return true;
    default: return false;
  }
}

// ---- Per-type INIT parameter blocks --------------------------------------

/// CPNODE ('C') init — interop E3 / JMRI dialect.
struct CpnodeInit {
  uint16_t inputBytes = 0;   ///< NI
  uint16_t outputBytes = 0;  ///< NO
  uint8_t opts1 = 0;         ///< USECMRIX|SENDEOT|USEBCC
  uint8_t opts2 = 0;         ///< reserved
};

/// SMINI ('M') init — SPS INITSMINI / JMRI SMINI.
/// Image geometry is fixed at 3 input bytes and 6 output bytes.
struct SminiInit {
  static constexpr uint16_t kInputBytes = 3;
  static constexpr uint16_t kOutputBytes = 6;
  static constexpr uint8_t kMaxNs = 24;
  static constexpr uint8_t kCtCount = 6;

  /// Number of 2-lead searchlight pairs (0..24). NS==0 omits CT bytes.
  uint8_t ns = 0;
  /// CT(1..6) when ns > 0; ignored when ns == 0.
  uint8_t ct[kCtCount] = {};
};

/// USIC ('N') or SUSIC ('X') init — SPS INITUSIC / JMRI.
/// I-body shape is identical; bits-per-card differs for geometry checks.
struct UsicFamilyInit {
  static constexpr uint8_t kMaxNs = 16;

  uint8_t ns = 1;              ///< number of 4-card sets (1..16)
  uint8_t ct[kMaxNs] = {};     ///< CT(1..ns)
  uint16_t inputBytes = 0;     ///< Host image size (not on classic I body)
  uint16_t outputBytes = 0;    ///< Host image size
};

/// Maximum I data-body length any supported builder emits.
/// SUSIC worst case: 4 + 16 CT = 20. CPNODE = 13.
constexpr size_t kMaxInitBodyBytes = 20;

/// Result of building one I data body (logical bytes, before DLE escape).
struct InitBody {
  uint8_t data[kMaxInitBodyBytes] = {};
  uint8_t length = 0;
};

/// Builder outcomes for add-time / build-time validation.
enum class InitBuildStatus : uint8_t {
  kOk,
  kBadNs,
  kBadGeometry,
  kUnsupportedType,
};

// ---- Builders (pure) -----------------------------------------------------

/// CPNODE body: <'C'> <dH> <dL> <opts1> <opts2> <NI> <NO> <0xFF x6>.
/// NI/NO are truncated to uint8_t on the wire (JMRI fielded practice for
/// the single-byte fields); callers must keep them ≤ 255 for a faithful
/// I body (Host image ceilings may still be larger for bench knobs).
inline InitBuildStatus buildCpnodeInitBody(const CpnodeInit& init,
                                           uint8_t dH, uint8_t dL,
                                           InitBody& out) {
  if (init.inputBytes > 255u || init.outputBytes > 255u) {
    return InitBuildStatus::kBadGeometry;
  }
  out.length = 13;
  out.data[0] = static_cast<uint8_t>(NodeType::kCpnode);
  out.data[1] = dH;
  out.data[2] = dL;
  out.data[3] = init.opts1;
  out.data[4] = init.opts2;
  out.data[5] = static_cast<uint8_t>(init.inputBytes);
  out.data[6] = static_cast<uint8_t>(init.outputBytes);
  for (uint8_t i = 7; i < 13; ++i) {
    out.data[i] = 0xFF;
  }
  return InitBuildStatus::kOk;
}

/// SMINI body: <'M'> <dH> <dL> <NS> [CT x6 if NS>0].
inline InitBuildStatus buildSminiInitBody(const SminiInit& init,
                                          uint8_t dH, uint8_t dL,
                                          InitBody& out) {
  if (init.ns > SminiInit::kMaxNs) {
    return InitBuildStatus::kBadNs;
  }
  out.data[0] = static_cast<uint8_t>(NodeType::kSmini);
  out.data[1] = dH;
  out.data[2] = dL;
  out.data[3] = init.ns;
  if (init.ns == 0) {
    out.length = 4;
    return InitBuildStatus::kOk;
  }
  out.length = 10;
  memcpy(out.data + 4, init.ct, SminiInit::kCtCount);
  return InitBuildStatus::kOk;
}

/// USIC/SUSIC body: <NDP> <dH> <dL> <NS> <CT x NS>.
inline InitBuildStatus buildUsicFamilyInitBody(NodeType type,
                                               const UsicFamilyInit& init,
                                               uint8_t dH, uint8_t dL,
                                               InitBody& out) {
  if (type != NodeType::kUsic && type != NodeType::kSusic) {
    return InitBuildStatus::kUnsupportedType;
  }
  if (init.ns < 1 || init.ns > UsicFamilyInit::kMaxNs) {
    return InitBuildStatus::kBadNs;
  }
  out.length = static_cast<uint8_t>(4u + init.ns);
  out.data[0] = static_cast<uint8_t>(type);
  out.data[1] = dH;
  out.data[2] = dL;
  out.data[3] = init.ns;
  memcpy(out.data + 4, init.ct, init.ns);
  return InitBuildStatus::kOk;
}

/// Sketch-facing "add one remote node" artifact: UA + type + that type's INIT.
///
/// Prefer this over a flat field bag. The active `init` arm is selected by
/// `type`; the other arms are inactive and must not be read.
///
/// C++11 note: the union needs an explicit default ctor because the init
/// structs use default member initializers (non-trivial). Helpers below
/// build fully-formed rows without designated-initializer syntax.
struct HostNodeSpec {
  uint8_t UA = 0;
  NodeType type = NodeType::kCpnode;
  union Init {
    CpnodeInit cpnode;
    SminiInit smini;
    UsicFamilyInit usic;
    Init() : cpnode() {}
  } init;

  HostNodeSpec() = default;
};

inline HostNodeSpec hostNodeCpnode(uint8_t ua, const CpnodeInit& init) {
  HostNodeSpec spec;
  spec.UA = ua;
  spec.type = NodeType::kCpnode;
  spec.init.cpnode = init;
  return spec;
}

inline HostNodeSpec hostNodeSmini(uint8_t ua, const SminiInit& init) {
  HostNodeSpec spec;
  spec.UA = ua;
  spec.type = NodeType::kSmini;
  spec.init.smini = init;
  return spec;
}

inline HostNodeSpec hostNodeUsic(uint8_t ua, const UsicFamilyInit& init) {
  HostNodeSpec spec;
  spec.UA = ua;
  spec.type = NodeType::kUsic;
  spec.init.usic = init;
  return spec;
}

inline HostNodeSpec hostNodeSusic(uint8_t ua, const UsicFamilyInit& init) {
  HostNodeSpec spec;
  spec.UA = ua;
  spec.type = NodeType::kSusic;
  spec.init.usic = init;
  return spec;
}


}  // namespace CMRInet
