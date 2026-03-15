/**
 * Settings view — device name, WiFi, LED, OTA, pin configuration, restart.
 * Student mode shows Device Name, WiFi, LED, OTA, Backup/Restore only.
 * Expert mode adds Pin Config, raw JSON config, File Manager, Restart button.
 * Task: W09, W16
 */

import { useEffect, useState } from 'react'
import { system, otaUpload, led, type WifiNetwork, type WifiState, type FileEntry } from '../api/client'
import { useSystemStore } from '../stores/system'
import { useIsExpert } from '../stores/ui'

export default function Settings() {
  const isExpert = useIsExpert()
  const [config, setConfig] = useState<Record<string, unknown> | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)
  const [saveMsg, setSaveMsg] = useState<string | null>(null)

  useEffect(() => {
    loadConfig()
  }, [])

  const loadConfig = async () => {
    setLoading(true)
    try {
      const cfg = await system.config()
      setConfig(cfg)
      setError(null)
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setLoading(false)
    }
  }

  const handleRestart = async () => {
    if (!confirm('Restart the node? This will briefly disconnect all clients.')) return
    try {
      await system.restart()
    } catch {
      // Expected — connection drops during restart
    }
  }

  const handleSave = async () => {
    if (!config) return
    setSaving(true)
    setSaveMsg(null)
    try {
      const result = await system.updateConfig(config)
      setSaveMsg(result.detail)
    } catch (e) {
      setSaveMsg(`Error: ${(e as Error).message}`)
    } finally {
      setSaving(false)
    }
  }

  if (loading) return <div className="text-gray-500">Loading configuration...</div>
  if (error) return <div className="text-red-600">Error: {error}</div>

  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <h2 className="text-xl font-bold">Settings</h2>
        {isExpert && (
          <button onClick={handleRestart} className="btn-secondary">
            Restart Node
          </button>
        )}
      </div>

      {saveMsg && (
        <div className="bg-blue-50 text-blue-700 text-sm px-3 py-2 rounded">
          {saveMsg}
        </div>
      )}

      <DeviceName />
      <WifiConfig />
      <LedControl />
      {isExpert && <OtaUpdate />}
      {isExpert && <BackupRestore />}
      {isExpert && <FileManager />}

      {/* Configuration (Expert only) */}
      {isExpert && config && (
        <div className="card">
          <div className="flex items-center justify-between mb-3">
            <h3 className="text-sm font-semibold text-gray-700">
              Current Configuration
            </h3>
            <button onClick={handleSave} disabled={saving} className="btn-primary">
              {saving ? 'Saving...' : 'Save Config'}
            </button>
          </div>
          <pre className="bg-gray-50 rounded p-3 text-xs overflow-auto max-h-96 font-mono">
            {JSON.stringify(config, null, 2)}
          </pre>
        </div>
      )}

      {/* Pin Configuration (Expert only) */}
      {isExpert && config && typeof config === 'object' && 'pins' in config && (
        <PinConfig
          pins={config.pins as Record<string, unknown>}
          onChange={(pins) => setConfig({ ...config, pins })}
        />
      )}
    </div>
  )
}

// ── Device Name ──

function DeviceName() {
  const info = useSystemStore((s) => s.info)
  const fetchInfo = useSystemStore((s) => s.fetchInfo)
  const [name, setName] = useState('')
  const [renaming, setRenaming] = useState(false)
  const [msg, setMsg] = useState<string | null>(null)

  useEffect(() => {
    if (info?.display_name) {
      setName(info.display_name)
    }
  }, [info?.display_name])

  const handleRename = async () => {
    if (!name.trim()) return
    setRenaming(true)
    setMsg(null)
    try {
      const result = await system.setName(name.trim())
      setMsg(`Renamed to "${result.name}"`)
      fetchInfo()
    } catch (e) {
      setMsg(`Error: ${(e as Error).message}`)
    } finally {
      setRenaming(false)
    }
  }

  return (
    <div className="card">
      <h3 className="text-sm font-semibold text-gray-700 mb-3">Device Name</h3>
      <div className="flex items-center gap-2">
        <input
          type="text"
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder="Device name"
          className="input flex-1"
          onKeyDown={(e) => e.key === 'Enter' && handleRename()}
          aria-label="Device name"
        />
        <button onClick={handleRename} disabled={renaming} className="btn-primary">
          {renaming ? 'Renaming...' : 'Rename'}
        </button>
      </div>
      {msg && (
        <div className="text-sm text-gray-600 mt-2">{msg}</div>
      )}
    </div>
  )
}

// ── WiFi Configuration ──

function WifiConfig() {
  const [wifi, setWifi] = useState<WifiState | null>(null)
  const [networks, setNetworks] = useState<WifiNetwork[]>([])
  const [scanning, setScanning] = useState(false)
  const [selectedSsid, setSelectedSsid] = useState<string | null>(null)
  const [password, setPassword] = useState('')
  const [connecting, setConnecting] = useState(false)
  const [msg, setMsg] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    loadWifi()
  }, [])

  const loadWifi = async () => {
    try {
      const state = await system.wifi()
      setWifi(state)
    } catch {
      // WiFi endpoint may not be available
    }
  }

  const handleScan = async () => {
    setScanning(true)
    setError(null)
    try {
      const result = await system.wifiScan()
      setNetworks(result.networks)
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setScanning(false)
    }
  }

  const handleConnect = async () => {
    if (!selectedSsid) return
    setConnecting(true)
    setMsg(null)
    setError(null)
    try {
      const result = await system.wifiConnect(selectedSsid, password)
      setMsg(result.detail)
      setSelectedSsid(null)
      setPassword('')
      // Reload WiFi state after a brief delay
      setTimeout(loadWifi, 2000)
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setConnecting(false)
    }
  }

  const signalBars = (rssi: number): string => {
    if (rssi >= -50) return '████'
    if (rssi >= -60) return '███░'
    if (rssi >= -70) return '██░░'
    if (rssi >= -80) return '█░░░'
    return '░░░░'
  }

  return (
    <div className="card space-y-4">
      <h3 className="text-sm font-semibold text-gray-700">WiFi Configuration</h3>

      {wifi && (
        <div className="grid grid-cols-2 md:grid-cols-4 gap-3 text-sm">
          <div>
            <span className="text-gray-500">Mode</span>
            <div className="font-medium uppercase">{wifi.mode}</div>
          </div>
          <div>
            <span className="text-gray-500">SSID</span>
            <div className="font-medium">{wifi.ssid || '—'}</div>
          </div>
          <div>
            <span className="text-gray-500">IP</span>
            <div className="font-medium font-mono">{wifi.ip || '—'}</div>
          </div>
          <div>
            <span className="text-gray-500">Status</span>
            <div className="flex items-center gap-1">
              <div className={`w-2 h-2 rounded-full ${wifi.connected ? 'bg-green-500' : 'bg-gray-400'}`} />
              <span className="font-medium">{wifi.connected ? 'Connected' : 'Disconnected'}</span>
            </div>
          </div>
        </div>
      )}

      <div className="flex items-center gap-2">
        <button onClick={handleScan} disabled={scanning} className="btn-secondary">
          {scanning ? 'Scanning...' : 'Scan Networks'}
        </button>
      </div>

      {error && (
        <div className="bg-red-50 text-red-700 text-sm px-3 py-2 rounded">{error}</div>
      )}
      {msg && (
        <div className="bg-blue-50 text-blue-700 text-sm px-3 py-2 rounded">{msg}</div>
      )}

      {networks.length > 0 && (
        <div className="space-y-1">
          <div className="text-xs text-gray-500 mb-2">Available Networks ({networks.length})</div>
          {networks.map((net) => (
            <div key={net.ssid}>
              <button
                onClick={() => setSelectedSsid(selectedSsid === net.ssid ? null : net.ssid)}
                className={`w-full flex items-center justify-between px-3 py-2 rounded text-sm transition-colors ${
                  selectedSsid === net.ssid
                    ? 'bg-servo-50 border border-servo-200'
                    : 'hover:bg-gray-50 border border-transparent'
                }`}
              >
                <span className="font-medium">{net.ssid}</span>
                <div className="flex items-center gap-3">
                  <span className="font-mono text-xs text-gray-500">{signalBars(net.rssi)}</span>
                  <span className="text-xs text-gray-500">{net.rssi} dBm</span>
                  <span className="text-xs text-gray-500">{net.auth}</span>
                </div>
              </button>
              {selectedSsid === net.ssid && (
                <div className="flex items-center gap-2 px-3 py-2 ml-4">
                  <input
                    type="password"
                    value={password}
                    onChange={(e) => setPassword(e.target.value)}
                    placeholder="Password"
                    className="input flex-1"
                    onKeyDown={(e) => e.key === 'Enter' && handleConnect()}
                    aria-label="WiFi password"
                  />
                  <button onClick={handleConnect} disabled={connecting} className="btn-primary">
                    {connecting ? 'Connecting...' : 'Connect'}
                  </button>
                </div>
              )}
            </div>
          ))}
          <div className="bg-yellow-50 text-yellow-700 text-xs px-3 py-2 rounded mt-2">
            Warning: Connecting to a new network will change the device IP address. You may need to reconnect.
          </div>
        </div>
      )}
    </div>
  )
}

// ── LED Control ──

function LedControl() {
  const [identify, setIdentify] = useState(false)
  const [msg, setMsg] = useState<string | null>(null)

  useEffect(() => {
    loadLedState()
  }, [])

  const loadLedState = async () => {
    try {
      const state = await led.state()
      setIdentify(state.identify)
    } catch {
      // LED endpoint may not be available
    }
  }

  const handleIdentifyToggle = async () => {
    const newVal = !identify
    try {
      await led.identify(newVal)
      setIdentify(newVal)
      setMsg(null)
    } catch (e) {
      setMsg(`Error: ${(e as Error).message}`)
    }
  }

  const handleFlashDown = async () => {
    try {
      await led.flash(true)
    } catch {
      // ignore
    }
  }

  const handleFlashUp = async () => {
    try {
      await led.flash(false)
    } catch {
      // ignore
    }
  }

  return (
    <div className="card space-y-4">
      <h3 className="text-sm font-semibold text-gray-700">LED Control</h3>
      <div className="flex items-center gap-6">
        <div className="flex items-center gap-3">
          <span className="text-sm text-gray-600">Identify</span>
          <button
            onClick={handleIdentifyToggle}
            role="switch"
            aria-checked={identify}
            aria-label={`${identify ? 'Disable' : 'Enable'} LED identify mode`}
            className={`relative inline-flex h-6 w-11 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-servo-500 ${
              identify ? 'bg-servo-600' : 'bg-gray-300'
            }`}
          >
            <span
              className={`inline-block h-4 w-4 transform rounded-full bg-white transition-transform ${
                identify ? 'translate-x-6' : 'translate-x-1'
              }`}
            />
          </button>
        </div>
        <button
          onMouseDown={handleFlashDown}
          onMouseUp={handleFlashUp}
          onMouseLeave={handleFlashUp}
          className="btn-secondary"
        >
          Flash
        </button>
      </div>
      {msg && (
        <div className="text-sm text-red-600">{msg}</div>
      )}
    </div>
  )
}

// ── OTA Firmware Update ──

function OtaUpdate() {
  const [file, setFile] = useState<File | null>(null)
  const [uploading, setUploading] = useState(false)
  const [progress, setProgress] = useState(0)
  const [msg, setMsg] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const handleUpload = async () => {
    if (!file) return
    if (!confirm('Upload and flash new firmware? The device will restart after flashing.')) return
    setUploading(true)
    setProgress(0)
    setMsg(null)
    setError(null)
    try {
      const result = await otaUpload(file, setProgress)
      if (result.ok) {
        setMsg('Upload complete. Restarting...')
        setTimeout(() => {
          window.location.reload()
        }, 10000)
      } else {
        setError(result.detail || 'Upload failed')
      }
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setUploading(false)
    }
  }

  return (
    <div className="card space-y-4">
      <h3 className="text-sm font-semibold text-gray-700">OTA Firmware Update</h3>
      <div className="flex items-center gap-2">
        <input
          type="file"
          accept=".bin"
          onChange={(e) => setFile(e.target.files?.[0] ?? null)}
          className="text-sm text-gray-600"
          disabled={uploading}
          aria-label="Select firmware binary file"
        />
        <button
          onClick={handleUpload}
          disabled={!file || uploading}
          className="btn-primary"
        >
          {uploading ? 'Uploading...' : 'Upload & Flash'}
        </button>
      </div>
      {uploading && (
        <div>
          <div className="h-3 bg-gray-100 rounded-full overflow-hidden">
            <div
              className="h-full bg-servo-600 rounded-full transition-all duration-300"
              style={{ width: `${progress}%` }}
            />
          </div>
          <div className="text-xs text-gray-500 mt-1">{progress}%</div>
        </div>
      )}
      {msg && (
        <div className="bg-blue-50 text-blue-700 text-sm px-3 py-2 rounded">{msg}</div>
      )}
      {error && (
        <div className="bg-red-50 text-red-700 text-sm px-3 py-2 rounded">{error}</div>
      )}
    </div>
  )
}

// ── Pin Configuration ──

function PinConfig({
  pins,
  onChange,
}: {
  pins: Record<string, unknown>
  onChange: (pins: Record<string, unknown>) => void
}) {
  const pinFields = [
    { key: 'servo_bus_tx', label: 'Servo Bus TX' },
    { key: 'servo_bus_rx', label: 'Servo Bus RX' },
    { key: 'servo_bus_dir', label: 'Servo Bus DIR' },
    { key: 'i2c_sda', label: 'I2C SDA' },
    { key: 'i2c_scl', label: 'I2C SCL' },
    { key: 'spi_mosi', label: 'SPI MOSI' },
    { key: 'spi_miso', label: 'SPI MISO' },
    { key: 'spi_sclk', label: 'SPI SCLK' },
    { key: 'oled_sda', label: 'OLED SDA' },
    { key: 'oled_scl', label: 'OLED SCL' },
    { key: 'ws2812_data', label: 'WS2812 Data' },
    { key: 'onewire', label: '1-Wire (DS18B20)' },
  ]

  const handlePinChange = (key: string, value: string) => {
    const num = value === '' ? -1 : parseInt(value)
    onChange({ ...pins, [key]: isNaN(num) ? -1 : num })
  }

  return (
    <div className="card">
      <h3 className="text-sm font-semibold text-gray-700 mb-3">
        Pin Configuration
      </h3>
      <p className="text-xs text-gray-500 mb-4">
        All pins are configurable. Use -1 for "not assigned". Avoid GPIO 6-11 (flash).
      </p>
      <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-4 gap-3">
        {pinFields.map(({ key, label }) => (
          <div key={key}>
            <label className="block text-xs text-gray-500 mb-0.5">{label}</label>
            <input
              type="number"
              min={-1}
              max={39}
              value={pins[key] !== undefined ? String(pins[key]) : '-1'}
              onChange={(e) => handlePinChange(key, e.target.value)}
              className="input text-sm"
            />
          </div>
        ))}
      </div>
    </div>
  )
}

// ── Backup & Restore ──

function BackupRestore() {
  const [restoring, setRestoring] = useState(false)
  const [msg, setMsg] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const handleBackup = async () => {
    setError(null)
    try {
      const config = await system.backup()
      const blob = new Blob([JSON.stringify(config, null, 2)], { type: 'application/json' })
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = `arbor_config_${new Date().toISOString().slice(0, 10)}.json`
      a.click()
      URL.revokeObjectURL(url)
    } catch (e) {
      setError((e as Error).message)
    }
  }

  const handleRestore = async () => {
    setError(null)
    setMsg(null)
    const input = document.createElement('input')
    input.type = 'file'
    input.accept = '.json'
    input.onchange = async () => {
      const file = input.files?.[0]
      if (!file) return
      if (!confirm('Restore configuration from this file? Current settings will be overwritten.')) return
      setRestoring(true)
      try {
        const text = await file.text()
        const config = JSON.parse(text)
        const result = await system.restore(config)
        setMsg(result.detail)
      } catch (e) {
        setError((e as Error).message)
      } finally {
        setRestoring(false)
      }
    }
    input.click()
  }

  return (
    <div className="card space-y-4">
      <h3 className="text-sm font-semibold text-gray-700">Backup & Restore</h3>
      <p className="text-xs text-gray-500">
        Export all settings (servos, WiFi, pins) as JSON. Import to restore after a reset or migrate to another board.
      </p>
      <div className="flex items-center gap-2">
        <button onClick={handleBackup} className="btn-secondary">
          Download Backup
        </button>
        <button onClick={handleRestore} disabled={restoring} className="btn-secondary">
          {restoring ? 'Restoring...' : 'Restore from File'}
        </button>
      </div>
      {msg && (
        <div className="bg-blue-50 text-blue-700 text-sm px-3 py-2 rounded">{msg}</div>
      )}
      {error && (
        <div className="bg-red-50 text-red-700 text-sm px-3 py-2 rounded">{error}</div>
      )}
    </div>
  )
}

// ── File Manager ──

function FileManager() {
  const [files, setFiles] = useState<FileEntry[]>([])
  const [storage, setStorage] = useState({ total: 0, used: 0, free: 0 })
  const [loading, setLoading] = useState(false)
  const [msg, setMsg] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const loadFiles = async () => {
    setLoading(true)
    try {
      const result = await system.files()
      setFiles(result.files)
      setStorage({ total: result.total_bytes, used: result.used_bytes, free: result.free_bytes })
      setError(null)
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => { loadFiles() }, [])

  const handleDownload = async (name: string) => {
    try {
      const content = await system.fileDownload(name)
      const blob = new Blob([content], { type: 'application/octet-stream' })
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = name
      a.click()
      URL.revokeObjectURL(url)
    } catch (e) {
      setError((e as Error).message)
    }
  }

  const handleUpload = async () => {
    setError(null)
    setMsg(null)
    const input = document.createElement('input')
    input.type = 'file'
    input.accept = '.json,.txt,.csv'
    input.onchange = async () => {
      const file = input.files?.[0]
      if (!file) return
      try {
        const content = await file.text()
        const result = await system.fileUpload(file.name, content)
        setMsg(`Uploaded ${result.filename} (${result.size} bytes)`)
        loadFiles()
      } catch (e) {
        setError((e as Error).message)
      }
    }
    input.click()
  }

  const handleDelete = async (name: string) => {
    if (!confirm(`Delete ${name}?`)) return
    try {
      await system.fileDelete(name)
      setMsg(`Deleted ${name}`)
      loadFiles()
    } catch (e) {
      setError((e as Error).message)
    }
  }

  const fmt = (bytes: number) =>
    bytes < 1024 ? `${bytes} B` : `${(bytes / 1024).toFixed(1)} KB`

  return (
    <div className="card space-y-4">
      <div className="flex items-center justify-between">
        <h3 className="text-sm font-semibold text-gray-700">File Manager</h3>
        <div className="flex items-center gap-2">
          <span className="text-xs text-gray-500">
            {fmt(storage.used)} / {fmt(storage.total)} ({fmt(storage.free)} free)
          </span>
          <button onClick={handleUpload} className="btn-secondary text-xs">
            Upload File
          </button>
        </div>
      </div>

      {loading && <div className="text-sm text-gray-500">Loading...</div>}

      {files.length > 0 && (
        <div className="divide-y divide-gray-100">
          {files.map((f) => (
            <div key={f.name} className="flex items-center justify-between py-2">
              <div>
                <span className="text-sm font-mono">{f.name}</span>
                <span className="text-xs text-gray-500 ml-2">{fmt(f.size)}</span>
              </div>
              <div className="flex items-center gap-1">
                <button
                  onClick={() => handleDownload(f.name)}
                  className="text-xs text-servo-600 hover:text-servo-800 px-2 py-1"
                >
                  Download
                </button>
                <button
                  onClick={() => handleDelete(f.name)}
                  className="text-xs text-red-500 hover:text-red-700 px-2 py-1"
                >
                  Delete
                </button>
              </div>
            </div>
          ))}
        </div>
      )}

      {!loading && files.length === 0 && (
        <div className="text-sm text-gray-500">No config files on device</div>
      )}

      {msg && (
        <div className="bg-blue-50 text-blue-700 text-sm px-3 py-2 rounded">{msg}</div>
      )}
      {error && (
        <div className="bg-red-50 text-red-700 text-sm px-3 py-2 rounded">{error}</div>
      )}
    </div>
  )
}
