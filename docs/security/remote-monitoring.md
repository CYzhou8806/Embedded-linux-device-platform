# Remote monitoring over mutual TLS

Plan.md V2/M7 asks for a "mutual-TLS remote monitoring interface". This is
it: `device-monitor` ([`userspace/device-monitor/`](../../userspace/device-monitor/))
serves device-service's machine state to authenticated clients, and to
nobody else.

| | Status |
| --- | --- |
| Server: TLS 1.3 only, client certificate required, clientAuth purpose, CRL | ✅ tested on the host, 21 checks (`tests/test_mtls.sh`) |
| Certificates from keys in the HSM token: Device CA (02) for the device, a new Operator CA (06) for clients, CRL | ✅ tested on a scratch SoftHSM token, 11 checks (`security/monitoring/test-monitor-pki.sh`) |
| Read-only by construction: the control socket refuses state-changing commands from it | ✅ unit-tested (`Commands.*` in device-service's tests) |
| Packaged: recipe, sandboxed unit, in `device-platform-image-ab` | ✅ built with bitbake; `systemd-analyze security` 1.5 |
| On the board | ✅ 2026-10-02, image 1.3.2: Operator CA (key 06) created in the real token, device key generated on the board, workstation certificate issued; see §6 |

## 1. Shape

```
  operator workstation                         Raspberry Pi 5 (A/B image)
 ┌──────────────────┐   TLS 1.3, both sides   ┌───────────────────────────────────────────┐
 │ curl / Prometheus│◄──────────────────────► │ device-monitor   network: yes  hardware: no│
 │ ops.crt (Operator│   present certificates  │  DynamicUser, +group acq, key via          │
 │ CA), trusts the  │                         │  LoadCredential                            │
 │ Device CA        │                         │        │ "status" only (read-only peer)     │
 └──────────────────┘                         │        ▼ /run/device-service/control.sock  │
                                              │ device-service   network: no   hardware: yes│
                                              │  user acq, PrivateNetwork=yes               │
                                              └───────────────────────────────────────────┘
```

The design decision is the split into two processes. device-service touches
the hardware and has no network at all: its sandbox has
`PrivateNetwork=yes` ([hardening](hardening.md), F11). Adding a TLS listener
to it would have meant giving the process that drives the SPI bus a network
stack, and putting OpenSSL's parser in the same address space as the
acquisition loop. Instead the network-facing process can do exactly one
thing to the device: ask for its status.

That restriction is enforced on the device-service side, not by trusting
the monitor. Every connection to the control socket carries the peer's
`SO_PEERCRED`. `status` is answered for any peer that could connect (root
and the `acq` group, through the socket's 0660 mode and its directory's
0750). `pause`, `resume`, `start`, `calibrate` and `reset` are refused
unless the peer is root or device-service's own uid. device-monitor runs
under a `DynamicUser` uid with `acq` as a supplementary group, so it is
always in the first set and never in the second. A compromised monitor
gets the status JSON and nothing more.

## 2. What a client sees

```
GET /status    the machine state, counters, last fault and last calibration (JSON)
GET /metrics   the same in Prometheus format: device_state{state="…"} 0/1,
               device_samples_read_total, device_sequence_gaps_total,
               device_sample_age_microseconds, device_faults_total, device_clock_drift_ppm …
GET /healthz   200 while Running, 503 otherwise
```

Anything but GET is answered 405. There is no write path over the network
at all. Pausing or resetting a device is done on the device, through
`device-ctl` as root, which means an SSH session with the admin key
([hardening](hardening.md) §5).

## 3. Trust: two CAs, one job each

| key | CA | vouches for | lives |
| --- | --- | --- | --- |
| 02 | Device CA (existing) | "this TLS key is device *N*": the server certificate, `serverAuth` | HSM token |
| 06 | **Operator CA** (new) | "this key may look at devices": client certificates, `clientAuth` | HSM token |

Both keys are P-256, generated in the token and never extractable
(`monitor-pki.sh init-operator-ca`). They are separate for the same reason
the boot, update and device keys are separate: each one's compromise has
its own blast radius. A leaked operator certificate is revoked through the
Operator CA's CRL without touching any device, and the Operator CA can't
mint a device identity.

**The device's TLS key is made on the device** (`provision-monitor.sh`): it
is generated on the encrypted `/data` partition, only its CSR goes to the
host, and the CSR's signature is checked before signing (proof of
possession). The test makes sure a CSR with a broken signature is refused.
The key is a file, because the Pi 5 has no TEE (F12). The design for a
board that has one is the OP-TEE TA in [optee](optee.md).

On the device, the four files sit in `/data/monitor`, root 0600. The
monitor's uid can't read them. systemd copies them into a ramfs that only
this service can see (`LoadCredential=`), at
`/run/credentials/device-monitor.service/`.

## 4. What is refused, and why the log says so

Each line is a check in `tests/test_mtls.sh`, run against a throwaway PKI:

| client | result | device-monitor's log |
| --- | --- | --- |
| no certificate | refused | `peer did not return a certificate` |
| certificate from a CA nobody trusts | refused | `unable to get local issuer certificate` |
| Operator-CA certificate, revoked | refused | `certificate revoked` |
| Operator-CA certificate with `serverAuth` only | refused | `unsuitable certificate purpose` |
| TLS 1.2 | refused | `unsupported protocol` |
| valid client, but the device's certificate from the wrong CA | the *client* refuses | `tlsv1 alert unknown ca` |
| any client, while the device's CRL has expired | refused | `CRL has expired` |
| valid client, POST | 405 | access line with the client's CN |

The expired-CRL line is a deliberate choice and has an operational cost.
Revocation fails closed, so a device whose CRL is past `nextUpdate`
refuses everyone, including legitimate operators. A fresh CRL has to
reach every device before the old one expires (`CRL_DAYS`, 30 by
default). [`refresh-monitor-crl.sh`](../../security/monitoring/refresh-monitor-crl.sh)
does that. It issues a fresh CRL from the token, pushes it to each device,
restarts the monitor, and reads `/metrics` back with an operator
certificate to confirm the device enforces the new expiry. On the board
the gauge went back to a full 30 days. What is not built is running it
on a schedule; that is a cron job on the signing host. It is the same
problem the update keyring has ([update-and-provisioning](update-and-provisioning.md) §6).
What is built is the warning. `/metrics` exports
`device_monitor_crl_expiry_seconds` and
`device_monitor_certificate_expiry_seconds`, read once at startup, so a
scraper can alert days before either deadline instead of finding out
when every connection is refused. The test checks both against the test
PKI's 7- and 30-day lifetimes.

## 5. Limits

- **Time.** Certificate validity and CRL freshness depend on the clock,
  and the board has no RTC. Before NTP, the clock floor keeps it at or
  after the image's build time ([update-and-provisioning](update-and-provisioning.md) §7),
  which is enough to reject an expired certificate. It is not enough to
  reject one that expired after the image was built. That needs NTP.
- **One connection at a time**, each bounded by `timeout_ms` (5 s). This
  is enough for a scraper. A client that opens connections and stalls can
  hold the queue, so this is not a public-internet service, and nothing
  here rate-limits.
- **No session resumption.** Every connection does a full handshake, so a
  CRL update takes effect on the next connection. That costs one ECDSA
  signature per scrape, which is irrelevant at monitoring rates.

## 6. On the board (2026-10-02)

`provision-monitor.sh RaspberryPi5-prod raspberrypi5 192.168.178.172`.
The device key was generated on the board, into `/data/monitor` (root
0600). The certificate carries `DNS:raspberrypi5, IP:192.168.178.172`.
From the workstation:

| request | result | board's access log |
| --- | --- | --- |
| `GET /status`, `/metrics`, `/healthz` with `ops-workstation` | 200, 200, 200 (`device_state{state="Running"} 1`, CRL expiry 30 days, certificate 2 years) | `CN=ops-workstation "GET /status HTTP/1.1" 200` … |
| no client certificate | refused (`tlsv13 alert certificate required`) | `peer did not return a certificate` |
| `POST /status` | 405 | `CN=ops-workstation "POST /status HTTP/1.1" 405` |

And the property the two-process design exists for. A process with
exactly device-monitor's identity (`systemd-run -p DynamicUser=yes -p
SupplementaryGroups=acq device-ctl …`) reads `status`, but its `pause`
gets `{"error":"permission denied: 'pause' changes state"}`, and the
device stays `Running`.

The provisioning script needed three fixes on its first real run, all of
the same kind as [case 14](../debugging/case-14-update-rejected-by-its-own-health-check.md):
no `install` applet, no root login (everything through `sudo`), and a glob
that the admin shell expanded inside a directory it cannot read.
