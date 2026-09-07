# CMRInet references

Part of the [CMRInet](README.md) guide set.

## The standard

- [docs/lcs-9.10.1_cmrinet_v1.1.pdf](docs/lcs-9.10.1_cmrinet_v1.1.pdf)
  — NMRA LCS-9.10.1 v1.1 (December 2014), the CMRInet specification.
- [docs/cmrinet-interop-profile-and-errata.md](docs/cmrinet-interop-profile-and-errata.md)
  — "CMRInet as fielded". The normative interop rules for this library
  and the proposed LCS-9.10.1 errata, each cited to its evidence.

## Host software

- [JMRI documents its CMRI Host
  support](https://www.jmri.org/help/en/html/hardware/cmri/CMRI.shtml)
  in the JMRI help pages. The Node software in this library works with
  JMRI as the Host.

## Field literature

Bruce Chubb's *C/MRI User's Manual* and the two volumes of the
*Railroaders Handbook* are the standard field references for CMRI
practice. Get them from [JLC
Enterprises](https://www.jlcenterprises.net/pages/downloads).

## Node types and geometry

The Node type travels in the I packet's NDP byte. Fielded map
(SPS/JMRI):

- `C` — CPNODE. 16 to 144 I/O using 8-bit cards. The I body carries
  two option bytes, the NI/NO card counts, and six 0xFF pad bytes.
- `M` — SMINI.
- `N` — USIC.
- `X` — SUSIC.

NI and NO count 8-bit cards and include a CPNODE's two onboard input
and two onboard output ports. JMRI caps a Node's reply at 118 input
data bytes; keep the reported input geometry at or below that. Full
body layouts and limits: the interop profile, errata E3, E4, and E7.
