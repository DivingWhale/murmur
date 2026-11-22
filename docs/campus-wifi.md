# Campus Wi-Fi (WPA2-Enterprise / PEAP)

How Murmur joins the campus network. The firmware agent should read this before writing the Wi-Fi code.

## What is known

| Item | Value | Source |
|---|---|---|
| Security | WPA2-Enterprise, AES (CCMP) | Owner's Windows profile |
| Outer method | PEAP ("Microsoft: Protected EAP") | Owner's Windows profile |
| Inner method | EAP-MSCHAPv2 | Windows WLAN profile, outer EAP type 25 and inner type 26 |
| SSID | `HKUSTGZ` | Active Windows WLAN connection |
| 2.4 GHz available | Channels 1 and 6 observed at the test location on 2026-11-20 | Windows scan of the same SSID while the laptop was on 5 GHz |
| Authentication server | `nce.hkust-gz.edu.cn` | Campus guide and a server certificate matching the Windows profile |
| Trust anchor | UCA Global G2 Root, SHA-256 `9BEA11C976FE014764C1BE56A6F914B5A560317ABD9988393382E5161AA0493C` | Windows-built chain for the matching leaf and SHECA's certificate repository |
| Username format | Campus email prefix, without the domain | Campus guide and a successful Windows WLAN authentication event |
| Outer identity | Windows profile specifies no separate anonymous identity. Use the campus username for the initial board test. | Windows PEAP profile |
| Murmur → Pi reachable | The handheld received a campus IP address, opened TCP port 22 on the Pi and received its SSH greeting. | Verified over campus Wi-Fi on 2026-11-21. The sync API remains a separate test. |

The [campus network guide, §§3.1 and 3.3.3](https://itdid.hkust-gz.edu.cn/uploadfile/2025/06/17/15-21-37/Guidelines%20for%20Campus%20Network%20Usage.pdf) shows the server name on an older certificate. On 2026-11-20, a SHA-1 thumbprint in the laptop's WLAN profile matched the publicly logged leaf for that same name, issued by SHECA DV TLS RSA CA A1 and valid through 2027-01-01. Windows built its chain to UCA Global G2 Root. Validate the expected name and chain on the handheld so a renewal under this CA continues to work. The issuer publishes the root in its [certificate repository](https://www.sheca.com/repository).

## Reading the missing values from Windows

Run this in PowerShell or Command Prompt on the laptop that already connects:

```bat
netsh wlan show interfaces
netsh wlan export profile name="<SSID>" folder=.
```

- `show interfaces` gives the connected SSID and band. Use `netsh wlan show networks mode=bssid` to check the bands the same SSID offers nearby.
- The exported XML contains:
  - **`<ServerNames>`** can specify a RADIUS server name. This profile leaves it empty.
  - **`<TrustedRootCA>`** entries contain SHA-1 thumbprints. One entry in this profile matches the server leaf. Identify the certificate before using it as a trust anchor.
  - **`<Type>26</Type>`** inside the nested EAP block confirms **MSCHAPv2**.
  - The **password is not exported**. Good.

To export the CA certificate itself:
1. Run `certmgr.msc` (or `certlm.msc`).
2. Go to Trusted Root Certification Authorities → Certificates.
3. Locate UCA Global G2 Root and verify the SHA-256 digest of its DER encoding against the table above.
4. Export it as **Base-64 X.509 (.CER)**. That file is the `ca.pem` below.

The GUI route gives the same information: Wi-Fi properties → Security → **Settings**. It shows "Verify the server's identity", "Connect to these servers" and the trusted root CA.

## Firmware

The handheld uses PEAP on HKUSTGZ. The Arduino 2.0.17 build binds the expected RADIUS DNS name to the supplicant's mbedTLS session and checks certificate dates against the device clock. The build restricts the outer EAP method to PEAP. The public CA certificate in `firmware/assets/hkustgz-root.der` has the fingerprint listed above.

The handheld preview builds with Arduino-ESP32 2.0.17. Its ESP32-S3 SDK exposes a CA setter in `esp_wpa2.h` but has no public setter for the expected server name. The example below targets the [ESP-IDF 5.4.2 enterprise client](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/api-reference/network/esp_wifi.html#_CPPv430esp_eap_client_set_domain_namePKc). Check the return value of each call and stop before connecting if certificate or name validation cannot be configured.

```c
esp_eap_client_set_identity((uint8_t*)anon_id, strlen(anon_id));     // outer identity
esp_eap_client_set_username((uint8_t*)user, strlen(user));           // inner identity
esp_eap_client_set_password((uint8_t*)pass, strlen(pass));
esp_eap_client_set_ca_cert(ca_pem, ca_pem_len);                      // validate the server
esp_eap_client_set_domain_name(server_name);                         // expected RADIUS DNS name
esp_wifi_sta_enterprise_enable();
// wifi_config_t: ssid set, no password, threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE
```

Rules:
- **Always validate the server**, with both the CA and the server name. Skipping validation lets anyone with a fake access point collect the owner's university password.
- **Cold-start time recovery.** A fully dated, successful login stores the exact SHA-256 leaf fingerprint and the date it was checked in NVS. With the clock unset, the supplicant can establish a time-only session using that exact pin, the configured CA chain, DNS name, and the remembered date. Present-day expiry is unknown during this step. A changed certificate or missing pin requires trusted time from the owner. On receiving SNTP time, the handheld disconnects and repeats PEAP with full current-date checks before exposing the connection to normal application traffic.
- **The development unit splits the profile across two media.** The internal NVS holds a random 256-bit key. The TF card holds an AES-GCM encrypted and authenticated copy of the campus username and password. This development setup uses no eFuse. Someone with both the card and an image of internal flash can recover the credentials.
- **Connect only after a recording or on wake, then turn Wi-Fi off** (`docs/product-design.md`). An enterprise login takes a few seconds, so allow up to about 15 s before treating it as a failure. The firmware gives up joining after 30 s.
  - *Automatic sessions* start at boot, after a recording is saved, from Settings → Wi-Fi, and on wake. Wake only connects if the clock is unset or the last session was more than 30 minutes ago. The radio switches off 2 s after network time arrives, or 20 s after DHCP, whichever comes first. The Pi upload will run inside this window once it exists. After a failed login, automatic sessions on wake and after recordings wait 2, 4, 8 … up to 60 minutes before trying again; a successful login resets this, and Settings → Wi-Fi always tries at once. `wifi status` shows the remaining wait as `auto_retry_s`.
  - *Manual sessions* (`wifi join`, `wifi save`, `wifi connect` over USB) stay up until `wifi off`, for at most 5 minutes, so the helper can probe the Pi.

## Getting credentials onto the device

From the repository root, start a manual Wi-Fi session with the project Python environment. On macOS or Linux:

```sh
work/venv/bin/python firmware/tools/join_campus.py --pi <Pi campus IP>
```

On Windows:

```powershell
$piAddress = Read-Host "Enter the Pi's campus IP address"
.\work\venv\Scripts\python.exe firmware/tools/join_campus.py --pi $piAddress
```

The helper finds Murmur by USB ID and sets its clock. On first use, enter the campus username and hidden password in the local terminal. After successful authentication and DHCP, the device writes a new encrypted file under `/murmur/wifi-a.enc` or `wifi-b.enc`, verifies the write, then commits the matching key and file selection to NVS. Future runs reuse the saved profile. Pass `--replace` to change the credentials. The optional `--pi` test opens port 22 from the handheld and reads the Pi's SSH greeting.

Leave the original TF card inserted for standalone login. An online session triggers SNTP. Settings → Clock shows network time status. The serial command `wifi clock` reports the last SNTP update and whether the board used the cold-start recovery path this boot. If recovery cannot authenticate the stored certificate or reach a time server, Settings → Clock and the USB helper can supply the date. If the card was missing, insert it and select Settings → Wi-Fi to retry. Updating the application partition preserves the NVS key and card files.

Over USB, `wifi profile` reports whether the device has a saved key, `wifi status` reports the connection state, and `wifi off` ends the session. `wifi forget` disconnects and removes the active key from NVS. An encrypted file can remain on the card. Provision again to create a fresh key. `wifi auto` starts the same self-ending session as Settings → Wi-Fi.

### Diagnosing a hang

A hung main loop reboots the handheld after 15 s (task watchdog). Afterwards, `wifi diag` prints `reset=<esp_reset_reason> loop_phase=<n> wifi_step=<n>` from breadcrumbs kept in RTC memory. A power-on reset clears them.

| `loop_phase` | Main loop was in | `wifi_step` | Wi-Fi code was |
|---|---|---|---|
| 1 | recording service | 1 | stopping SNTP and disconnecting |
| 2 | buttons | 2 | removing the EAP configuration |
| 3 | touch | 4 | switching the radio off (`WiFi.mode(WIFI_OFF)`) |
| 4 | serial commands | 10 | configuring and starting a login |
| 5 | Wi-Fi service | 20 | reading the saved profile from the card |
| 6 | screen update | 0 | none of the above |
| 7 | LVGL timers | | |
| 8 | idle delay | | |

To reproduce a hang, run `work/venv/bin/python firmware/tools/wifi_stress.py` (Windows: `.\work\venv\Scripts\python.exe`). It cycles automatic, manual and interrupted sessions, and prints `wifi diag` if the device reboots.

## Risks to check early

1. **Personal password on a gadget.** PEAP with MSCHAPv2 uses the owner's university password. Ask IT whether there's a **device or IoT network**, or app-specific passwords. If there is, use that instead.
2. **Device-to-device traffic blocked.** If Murmur can't reach the Pi on campus Wi-Fi, use another route: the Pi on a network the owner controls, the Pi running its own small network, or a relay the Pi reaches from outside.
