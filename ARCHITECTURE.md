# BluePill Architecture & Internals

```mermaid
flowchart LR
    subgraph Linux["Linux Workstation (x86_64 / aarch64)"]
        GUI["BluePill Qt6/QML Desktop Client
(/usr/bin/bluepill)"]
        CLI["BluePill JSON CLI
(bluepill status/recent/send)"]
        CACHE["Local State & Media Cache
~/.config/bluepill/ + ~/.cache/imsg/"]
    end

    subgraph Browser["Any Device Browser"]
        WEB["BluePill Built-In Web UI
(GET /web)"]
    end

    subgraph Mac["macOS Relay Host (Apple Silicon / Intel / KVM VM)"]
        RELAY["bluepill-server Daemon
(or Legacy BlueBubbles.app)"]
        CHATDB[("~/Library/Messages/chat.db
(SQLite WAL)")]
        ADDRDB[("AddressBook-v22.abcddb
(macOS Contacts)")]
        IMCORE["Messages.app + IMCore Helper
(or Zero-SIP osascript)"]
    end

    GUI <-->|"HTTPS REST + Socket.IO v4 Push
or Direct SSH"| RELAY
    CLI <-->|"HTTPS REST"| RELAY
    WEB <-->|"HTTPS / EventSource / REST"| RELAY
    GUI <-->|"0ms Startup"| CACHE
    RELAY -->|"Read-Only WAL (<2ms)"| CHATDB
    RELAY -->|"Contact Resolution (<5ms)"| ADDRDB
    RELAY <-->|"TCP :45670 JSON / osascript"| IMCORE
```

## 1. Linux Desktop Client (`src/backend.{h,cpp}`, `src/Main.qml`)
- **Zero-Latency Startup (`messages_cache.json`):** Loads up to 500 cached messages per conversation from disk before the first network packet is sent.
- **Isolated Network Pools (`m_nam`, `m_sendNam`, `m_mediaNam`, `m_socketNam`):** User mutations (`sendMessage`, `sendReaction`, `markChatRead`) execute on a dedicated high-priority `QNetworkAccessManager` (`m_sendNam`) so background image downloads on `m_mediaNam` (`?force=false`, max 2 concurrent) can never stall outbound messages.
- **Service Resolution (iMessage / RCS / SMS):** macOS 26 stores every chat GUID as `any;-;<handle>`, so the GUID prefix can't be trusted. `Backend::resolveChatService()` checks, in order: a legacy `iMessage;`/`RCS;`/`SMS;` prefix, a chat-level `serviceName`, the newest message's `service` (or `handle.service`), and finally participant handle services. It returns an empty string when the service is still unknown. As messages are cached, the newest one updates the chat's service. The UI draws green bubbles and pills only for a known `SMS`/`RCS` service, so iMessage threads never flash green while loading.
- **Magic-Byte (`ftyp`) HEIC/AVIF Transcoding:** Inspects the first 16 bytes of every downloaded attachment for ISO BMFF `ftyp` headers and converts disguised `.heic.jpeg` / `.avif` files to standard JPEG (`\xFF\xD8\xFF`) asynchronously.

## 2. Headless macOS Relay (`server/bluepill_server.py`) & Built-In Web UI
- **Direct SQLite WAL Reader:** Queries `~/Library/Messages/chat.db` in read-only WAL mode (`?mode=ro`), decoding `attributedBody` typedstreams and exposing `is_read`, `date_read`, `date_edited`, `date_retracted`, `error`, and `expressive_send_style_id` directly in JSON responses.
- **Dual-Mode Action Dispatcher:** Connects to `IMCore` (`localhost:45670`) when the Private API helper is loaded inside `Messages.app`, and falls back automatically to `osascript` on SIP-enabled Macs.
- **Built-In Web UI (`/web`) & Cloud Relay:** Self-contained **Liquid Glass / Matrix / Light** browser client served directly by `bluepill-server` with tabbed **Messages, Notes, Calendar, Photos, and Contacts** views, inline tapbacks, 1-click OTP copy, and real-time Socket.IO streaming. `bluepill-server` also includes a built-in pure-stdlib RFC 6455 TLS WebSocket tunnel client (`bluepill-server relay-setup`) for zero-port-forwarding remote access via `wss://api.bluepill.nertwork.com/tunnel/connect` (Demo: `https://bluepill.nertwork.com/demo`, Support: `support@nertwork.com`).
