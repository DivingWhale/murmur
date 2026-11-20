# Campus Wi-Fi (WPA2-Enterprise / PEAP)

How Murmur joins the campus network. The firmware agent should read this before writing the Wi-Fi code.

## What is known

| Item | Value | Source |
|---|---|---|
| Security | WPA2-Enterprise, AES (CCMP) | Owner's Windows profile |
| Outer method | PEAP ("Microsoft: Protected EAP") | Owner's Windows profile |
| Inner method | EAP-MSCHAPv2 (**to confirm**, Windows' default for PEAP) | Assumed |
| SSID | **to fill in** | |
| 2.4 GHz available | **to confirm**. The ESP32-S3 has no 5 GHz radio | |
| CA certificate / server name | **to fill in** (see below) | |
| Username format | **to confirm**: `user`, `user@school.edu` or `DOMAIN\user` | |
| Anonymous (outer) identity | **to confirm**, often `anonymous@school.edu` | |
| Murmur → Pi reachable | **to test**. Many campus networks block device-to-device traffic | |

## Reading the missing values from Windows

Run this in PowerShell or Command Prompt on the laptop that already connects:

```bat
netsh wlan show interfaces
netsh wlan export profile name="<SSID>" folder=.
```

- `show interfaces` gives the **SSID** and the radio band (`Channel` below 14 means 2.4 GHz).
- The exported XML contains:
  - **`<ServerNames>`**: the RADIUS server name to check against.
  - **`<TrustedRootCA>`**: the thumbprint (SHA-1) of the CA certificate.
  - **`<EapType>26</EapType>`** inside the PEAP block, which confirms **MSCHAPv2**.
  - The **password is not exported**. Good.

To export the CA certificate itself:
1. Run `certmgr.msc` (or `certlm.msc`).
2. Go to Trusted Root Certification Authorities → Certificates.
3. Find the certificate whose thumbprint matches `<TrustedRootCA>`.
4. Export it as **Base-64 X.509 (.CER)**. That file is the `ca.pem` below.

The GUI route gives the same information: Wi-Fi properties → Security → **Settings**. It shows "Verify the server's identity", "Connect to these servers" and the trusted root CA.

## Firmware (ESP-IDF 5.x)

PEAP is supported through the enterprise client (`esp_eap_client.h`):

```c
esp_eap_client_set_identity((uint8_t*)anon_id, strlen(anon_id));     // outer identity
esp_eap_client_set_username((uint8_t*)user, strlen(user));           // inner identity
esp_eap_client_set_password((uint8_t*)pass, strlen(pass));
esp_eap_client_set_ca_cert(ca_pem, ca_pem_len);                      // validate the server
esp_eap_client_set_domain_name(server_name);                         // match <ServerNames>
esp_wifi_sta_enterprise_enable();
// wifi_config_t: ssid set, no password, threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE
```

Rules:
- **Always validate the server**, with both the CA and the server name. Skipping validation lets anyone with a fake access point collect the owner's university password.
- **The clock must be right for validation to work.** Certificate checks fail when the clock is wrong, for example after the battery runs flat. Keep the time in the board's RTC if it has one, set it over USB when provisioning, and refresh it at each sync. If validation fails because the time is wrong, show a clear error. Never fall back to "no validation".
- **Credentials live in encrypted storage** (NVS with flash encryption), never in source code or the repo. `.gitignore` already blocks `wifi_credentials.*`, `*.pem` and `secrets.h`.
- **Connect only after a recording or on wake, then turn Wi-Fi off** (`docs/product-design.md`). An enterprise login takes a few seconds, so allow up to about 15 s before treating it as a failure.

## Getting credentials onto the device

There's no keyboard, so provisioning happens over USB serial from the computer, for example:

```sh
murmur wifi set --ssid "<SSID>" --eap peap --phase2 mschapv2 \
  --identity "<user>" --anonymous "<anon>" --ca ca.pem --server "<server name>"
# prompts for the password; never pass it as an argument (shell history)
murmur wifi test      # connect, report the result and Pi reachability, disconnect
```

The device's settings screen shows status only (network name, connected or not, last sync, **Sync now**). It never shows or edits the password.

## Risks to check early

1. **Personal password on a gadget.** PEAP with MSCHAPv2 uses the owner's university password. Ask IT whether there's a **device or IoT network**, or app-specific passwords. If there is, use that instead.
2. **Device-to-device traffic blocked.** If Murmur can't reach the Pi on campus Wi-Fi, use another route: the Pi on a network the owner controls, the Pi running its own small network, or a relay the Pi reaches from outside.
3. **No 2.4 GHz.** If the campus network is 5 GHz only, the ESP32-S3 can't join it at all.
