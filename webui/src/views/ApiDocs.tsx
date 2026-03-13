/**
 * API Docs view — reference for Node REST API endpoints.
 * Task: W11
 */

export default function ApiDocs() {
  return (
    <div className="space-y-6">
      <h2 className="text-xl font-bold">API Documentation</h2>

      <div className="card">
        <h3 className="text-sm font-semibold text-gray-700 mb-3">
          Node API (ESP32)
        </h3>
        <p className="text-sm text-gray-600 mb-3">
          The ESP32 firmware exposes a REST API that the control server proxies.
          When connected directly to a node, use these endpoints:
        </p>

        <div className="space-y-3 text-sm">
          <Section title="System">
            <Endpoint method="GET" path="/api/v1/system/info" desc="System information" />
            <Endpoint method="GET" path="/api/v1/system/health" desc="Health check (public)" />
            <Endpoint method="GET" path="/api/v1/system/config" desc="Get configuration" />
            <Endpoint method="PUT" path="/api/v1/system/config" desc="Update configuration" />
            <Endpoint method="POST" path="/api/v1/system/restart" desc="Restart node" />
            <Endpoint method="GET" path="/api/v1/system/ota" desc="OTA partition status" />
            <Endpoint method="POST" path="/api/v1/system/ota/upload" desc="Upload firmware binary (multipart)" />
            <Endpoint method="GET" path="/api/v1/system/wifi" desc="WiFi state (mode, IP, SSID)" />
            <Endpoint method="GET" path="/api/v1/system/wifi/scan" desc="Scan for WiFi networks" />
            <Endpoint method="POST" path="/api/v1/system/wifi/connect" desc="Connect to WiFi network" />
            <Endpoint method="PUT" path="/api/v1/system/name" desc="Set device display name" />
          </Section>

          <Section title="Servo">
            <Endpoint method="GET" path="/api/v1/servo/{id}/state" desc="Read servo state (position, speed, load, temp, voltage)" />
            <Endpoint method="PUT" path="/api/v1/servo/{id}/position" desc="Set goal position" />
            <Endpoint method="PUT" path="/api/v1/servo/{id}/speed" desc="Set goal speed" />
            <Endpoint method="PUT" path="/api/v1/servo/{id}/torque" desc="Enable/disable torque" />
            <Endpoint method="GET" path="/api/v1/servo/{id}/registers" desc="Read all registers (50 bytes)" />
            <Endpoint method="PUT" path="/api/v1/servo/{id}/register" desc="Write register(s) {addr, data[], unlock_eeprom?}" />
            <Endpoint method="GET" path="/api/v1/servo/{id}/backup" desc="Backup full EEPROM (50 bytes)" />
            <Endpoint method="POST" path="/api/v1/servo/{id}/restore" desc="Restore EEPROM from backup {eeprom[], include_id?}" />
            <Endpoint method="POST" path="/api/v1/servo/sync" desc="Sync multi-servo move" />
            <Endpoint method="GET" path="/api/v1/servo/scan" desc="Scan bus for servos (pings 0-253)" />
            <Endpoint method="POST" path="/api/v1/servo/set-id" desc="Reassign servo ID {current_id, new_id}" />
          </Section>

          <Section title="LED">
            <Endpoint method="GET" path="/api/v1/led" desc="LED state" />
            <Endpoint method="POST" path="/api/v1/led/identify" desc="Enable/disable identify blink" />
            <Endpoint method="POST" path="/api/v1/led/flash" desc="Flash LED on/off" />
            <Endpoint method="POST" path="/api/v1/led/off" desc="Turn off all LEDs" />
          </Section>

          <Section title="Sensor">
            <Endpoint method="GET" path="/api/v1/sensors" desc="All sensor readings" />
            <Endpoint method="GET" path="/api/v1/sensor/{id}/reading" desc="Single sensor reading" />
            <Endpoint method="GET" path="/api/v1/sensor/{id}/history" desc="Sensor history" />
          </Section>

          <Section title="Emergency">
            <Endpoint method="POST" path="/api/v1/emergency-stop" desc="Emergency stop (no auth)" />
          </Section>
        </div>
      </div>
    </div>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div>
      <h4 className="font-medium text-gray-700 mb-1">{title}</h4>
      <div className="space-y-1 ml-2">{children}</div>
    </div>
  )
}

function Endpoint({ method, path, desc }: { method: string; path: string; desc: string }) {
  const color = {
    GET: 'bg-green-100 text-green-700',
    PUT: 'bg-blue-100 text-blue-700',
    POST: 'bg-yellow-100 text-yellow-700',
    DELETE: 'bg-red-100 text-red-700',
  }[method] ?? 'bg-gray-100 text-gray-700'

  return (
    <div className="flex items-center gap-2">
      <span className={`text-xs font-mono font-bold px-1.5 py-0.5 rounded ${color}`}>
        {method}
      </span>
      <code className="text-xs font-mono text-gray-600">{path}</code>
      <span className="text-xs text-gray-400">— {desc}</span>
    </div>
  )
}
