// ConformanceFault.h — the shared conformance fault vocabulary.
//
// One flat fault enum, plus two free classifiers that derive its axes.
// Naming a fault therefore cannot produce an invalid (layer,
// attribution) pair, and call sites ask `layerOf(f)` rather than
// chaining field tests of their own.
//
// VALIDATION: Design v1.3 D14: faults are classified on two axes —
// layer and attribution — not one flag. Attribution is derived from the
// (Host assumption, Node assumption, observation) tuple: assumptions
// differ means disagreement, assumptions agree and the observation
// contradicts them means defect.
//
// Role-neutral by construction. The Host records faults it observes in
// a remote Node; a Node records faults it observes in its Host (the
// RemoteHost* family, Design v1.2 D1). Nothing here carries poll
// vocabulary, so both roles share one vocabulary rather than inventing
// two: the polled engine's ReplyRejectReason maps *into* this, and is
// not promoted to it.
//
// VALIDATION: Design v1.2 D1: product-layer types are strategy-neutral
// and carry no protocol qualifier. This vocabulary is also
// role-neutral, so it takes no perspective qualifier either.
//
// Strategy-neutral, owned by no engine, in the same spirit as
// RemoteNodeHandle.h and CMRITime.h.

#pragma once

#include <stdint.h>

namespace CMRInet {

/// Where in the units ladder a fault was detected.
///
/// The rungs are not equally portable, and that is the point. The image
/// rung is strategy-invariant: geometry disagreements exist under any
/// carrier, including a push strategy with no packets at all. The
/// packet rung exists only for the polled strategy — it survives an
/// MQTT *carrier* (Design D11, which keeps the packet seam) but not a
/// semantic gateway (D12, which does not). The framing rung is
/// serial-specific and is replaced, not merely renamed, by a message
/// carrier's own delivery faults.
///
/// Tagging the layer is what keeps a carrier swap from silently
/// producing a vocabulary that is half meaningless.
///
/// Layer is where a fault was *detected*. That is not the same question
/// as who it is attributable to, and the two only collapse when the
/// fault's name already settles the attribution.
// VALIDATION: Design v1.3 D14: layer is indexed by the units ladder;
// the image rung is strategy-invariant, the bottom rung is
// carrier-specific. Detection and attribution are separate layers.
enum class ConformanceLayer : uint8_t {
  kNone,     ///< only for ConformanceFault::kNone
  kImage,    ///< geometry, NI/NO — survives every strategy
  kPacket,   ///< address, message type, body structure — polled strategy
  kFraming,  ///< escaping, truncation, inter-byte abort — serial carrier
};

/// Who a fault is attributable to, and therefore how it gets fixed.
///
/// This distinction is the practical payload of the taxonomy: a defect
/// needs a firmware change, a disagreement needs a configuration
/// change. Filing one as the other sends the operator to the wrong
/// place.
///
/// Deliberately two-valued. There is no "environment" or
/// "indeterminate" attribution: the first would import a link-integrity
/// verdict into the conformance domain, and the second would conflate
/// *who is at fault* with *how well we know* — both read as "not my
/// problem". A finding that is neither a disagreement nor a defect is
/// not a conformance finding at all, and belongs to link health
/// (LinkStatistics, CMRIFrameDecoder::Statistics).
// VALIDATION: Design v1.3 D14: a defect is the counterparty violating
// the spec; a disagreement is two correctly-behaving endpoints
// configured inconsistently. Attribution stays two-valued.
enum class FaultAttribution : uint8_t {
  kNone,          ///< only for ConformanceFault::kNone
  kDisagreement,  ///< both endpoints behaving correctly, configured apart
  kDefect,        ///< the counterparty violated the spec
};

/// The specific conformance faults this library can name.
///
/// Enumerator names lead with their layer so a reader can classify one
/// by eye, and so the classifiers below stay obviously exhaustive.
///
/// Admission test: a fault may live here, and be classified statically,
/// only if its name already encodes the assumption comparison.
/// "Geometry mismatch" does — it *means* the assumptions differed.
/// "Unexpected type" does — it *means* they agreed and the content
/// violated them. "Truncated" does not.
///
/// That is why the framing rung is empty. A malformed frame is a
/// faithful observation, but at the framing rung a Node that stopped
/// mid-frame is indistinguishable from a frame the wire corrupted, so
/// the fact carries no causation. Separating the two needs history, and
/// history lives one layer up: framing answers yes/no, analysis calls
/// the shot. Until that analysis layer exists, framing facts stay where
/// they already are, in LinkStatistics and
/// CMRIFrameDecoder::Statistics.
///
/// When analysis does promote a framing fact to a name here, it will
/// have already excluded the abort-limit disagreement (D13) and the
/// link-integrity reading, so what remains is a defect by construction
/// — which is exactly what makes the promoted name pass the admission
/// test above.
enum class ConformanceFault : uint8_t {
  kNone,  ///< no fault; the "nothing observed yet" value

  /// A reply's body length disagrees with the declared input geometry.
  /// The Node is doing exactly what it was built to do, so this is a
  /// disagreement about configuration, not misbehaviour.
  /// VALIDATION: Interop v1.1 2.2.8: commit to the application only on
  /// a valid frame. A geometry mismatch is not valid data.
  kImageGeometryMismatch,

  /// A reply carried an address other than the one addressed.
  ///
  /// Attributed as a disagreement rather than a defect. The Host cannot
  /// distinguish a mis-addressed Node from another Node answering out
  /// of turn, but in the fielded population the dominant cause is an
  /// addressing mismatch between a DIP-switched Node and the Host's
  /// table — and the remedy in both readings is to go check addressing,
  /// not to change firmware.
  kPacketUnexpectedAddress,

  /// A reply carried a message type the exchange does not permit there.
  /// Unsolicited traffic outside an exchange is a separate, legitimate
  /// case (interop E8) and is not a fault.
  kPacketUnexpectedType,
};

/// The layer at which `fault` is detected.
inline ConformanceLayer layerOf(ConformanceFault fault) {
  switch (fault) {
    case ConformanceFault::kNone:
      return ConformanceLayer::kNone;
    case ConformanceFault::kImageGeometryMismatch:
      return ConformanceLayer::kImage;
    case ConformanceFault::kPacketUnexpectedAddress:
    case ConformanceFault::kPacketUnexpectedType:
      return ConformanceLayer::kPacket;
  }
  return ConformanceLayer::kNone;
}

/// Who `fault` is attributable to.
inline FaultAttribution attributionOf(ConformanceFault fault) {
  switch (fault) {
    case ConformanceFault::kNone:
      return FaultAttribution::kNone;
    case ConformanceFault::kImageGeometryMismatch:
    case ConformanceFault::kPacketUnexpectedAddress:
      return FaultAttribution::kDisagreement;
    case ConformanceFault::kPacketUnexpectedType:
      return FaultAttribution::kDefect;
  }
  return FaultAttribution::kNone;
}

/// Human-readable names, for telemetry and bench tooling.
inline const char* conformanceFaultString(ConformanceFault fault) {
  switch (fault) {
    case ConformanceFault::kNone:                   return "none";
    case ConformanceFault::kImageGeometryMismatch:  return "image geometry mismatch";
    case ConformanceFault::kPacketUnexpectedAddress:return "packet unexpected address";
    case ConformanceFault::kPacketUnexpectedType:   return "packet unexpected type";
  }
  return "unknown";
}

inline const char* conformanceLayerString(ConformanceLayer layer) {
  switch (layer) {
    case ConformanceLayer::kNone:    return "none";
    case ConformanceLayer::kImage:   return "image";
    case ConformanceLayer::kPacket:  return "packet";
    case ConformanceLayer::kFraming: return "framing";
  }
  return "unknown";
}

inline const char* faultAttributionString(FaultAttribution attribution) {
  switch (attribution) {
    case FaultAttribution::kNone:         return "none";
    case FaultAttribution::kDisagreement: return "disagreement";
    case FaultAttribution::kDefect:       return "defect";
  }
  return "unknown";
}

}  // namespace CMRInet
