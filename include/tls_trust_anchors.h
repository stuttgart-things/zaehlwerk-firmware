#pragma once

// The trust anchors for https:// zaehlwerk addresses: every root the board
// accepts a server certificate from, as one PEM bundle. Today that is the
// stuttgart-things edge root CA and nothing else, because zaehlwerk is only
// ever reached through the internal Gateway, never on a public name.
//
// Adding a root later (ISRG Root X1, say, for a Let's Encrypt name) means
// appending its PEM block inside the same string: mbedTLS reads every
// certificate in the bundle. Nothing else changes. Do not add one without a
// reason written into ADR-0008: every root here can vouch for any zaehlwerk
// address.
//
// The edge root. The Gateway in front of zaehlwerk on the table's
// box and in the lab presents a certificate that chains to it
// (*.edge.sthings.lab, *.edge-tt-test1.4sthings.tiab.ssc.sva.de, through the
// "edge intermediate CA (cert-manager)").
//
// A public certificate, not a secret: it holds no key. Copied verbatim from
// stuttgart-things/harvester, clusters/edge/edge-root-ca.crt (identical to
// clusters/sthings-edge/edge-root-ca.crt), the same file the zaehlwerk piezo
// mock trusts as its only root (clusters/edge/lab/esp-mock/piezo-a.yaml,
// SSL_CERT_FILE).
//
//   subject  O=stuttgart-things, CN=stuttgart-things edge root CA
//   key      EC P-384
//   valid    2026-10-03 .. 2046-10-03
//   SHA-256  42:11:ED:62:04:23:CB:05:14:30:98:70:3E:AF:3D:B3:AD:76:6E:76:E6:82:B3:3F:B4:01:A9:64:6E:F2:1D:54
//
// To check it against the source:
//   sed -n '/^-----BEGIN/,/^-----END/p' include/tls_trust_anchors.h |
//     openssl x509 -noout -fingerprint -sha256
//
// If the edge root is ever replaced, replace this block with the new PEM and
// update the lines above. The board never falls back to an unverified
// connection: without a matching chain there is no request.
static const char TLS_TRUST_ANCHORS_PEM[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIICBzCCAY2gAwIBAgIUfpd1w2gpB8pOTFjKb+D6preo3TcwCgYIKoZIzj0EAwMw
QzEZMBcGA1UECgwQc3R1dHRnYXJ0LXRoaW5nczEmMCQGA1UEAwwdc3R1dHRnYXJ0
LXRoaW5ncyBlZGdlIHJvb3QgQ0EwHhcNMjYxMDAzMTkyNjQ3WhcNNDYxMDAzMTky
NjQ3WjBDMRkwFwYDVQQKDBBzdHV0dGdhcnQtdGhpbmdzMSYwJAYDVQQDDB1zdHV0
dGdhcnQtdGhpbmdzIGVkZ2Ugcm9vdCBDQTB2MBAGByqGSM49AgEGBSuBBAAiA2IA
BAFQC3ZSa7Frps1YBdaM0sAHm5W/hUdRJcZ9K3Ckh+FRsSLlUvmVrWlETQQ5EKFe
SnqhlgSRvMxALu/oXYEM9pdBHufiHcL2McZf7CZrUAx9iaRrelb3ZaXiXPsA2MPo
oaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYE
FJXawBe9oxr/pve4BsU+MUfZ6FpMMAoGCCqGSM49BAMDA2gAMGUCMF/y9gKSXmX4
wTDEj+zl+/jSMTcI1EdrTNFxg7lt1yuYVNWoBYK03mM+NxSKX+exUAIxANHFbsLm
00Q8VmbBVQWCBJ8dk7fwGjA0o3jNrOHvftcULr7UBrELrN1D0bTm3T5aXg==
-----END CERTIFICATE-----
)PEM";
