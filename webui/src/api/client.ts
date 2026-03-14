/**
 * Typed API client for Arbor.
 * Wraps fetch with auth headers, error handling, and typed responses.
 * Task: W02
 */

const API_BASE = '/api/v1'

export class ApiError extends Error {
  constructor(
    public status: number,
    public detail: string,
  ) {
    super(`${status}: ${detail}`)
    this.name = 'ApiError'
  }
}

function getAuthHeaders(): HeadersInit {
  const token = localStorage.getItem('sb_token')
  const headers: HeadersInit = { 'Content-Type': 'application/json' }
  if (token) {
    headers['Authorization'] = `Bearer ${token}`
  }
  return headers
}

async function request<T>(
  method: string,
  path: string,
  body?: unknown,
): Promise<T> {
  const url = `${API_BASE}${path}`
  const res = await fetch(url, {
    method,
    headers: getAuthHeaders(),
    body: body ? JSON.stringify(body) : undefined,
  })

  if (!res.ok) {
    let detail = res.statusText
    try {
      const err = await res.json()
      detail = err.detail || detail
    } catch { /* ignore parse errors */ }
    throw new ApiError(res.status, detail)
  }

  return res.json()
}

// ── System ──

export interface SystemInfo {
  platform: string
  firmware_version: string
  project_name: string
  idf_version: string
  cores: number
  free_heap: number
  min_free_heap: number
  plugin_count: number
  ws_clients: number
  plugins: PluginInfo[]
  display_name?: string
  board_id?: string
  ip_addr?: string
  ap_mode?: boolean
  ssid?: string
}

export interface PluginInfo {
  name: string
  version: string
  initialized: boolean
  health: 'healthy' | 'degraded' | 'unhealthy' | 'unknown' | 'not_initialized'
  health_message?: string
}

export interface HealthStatus {
  status: 'healthy' | 'degraded' | 'unhealthy'
  free_heap: number
  provisioned: boolean
  components: { status: string; message?: string }[]
}

export interface WifiState {
  mode: 'ap' | 'sta'
  ip: string
  ssid: string
  password?: string
  board_id: string
  connected: boolean
}

export interface WifiNetwork {
  ssid: string
  rssi: number
  auth: string
}

export interface LedState {
  identify: boolean
  count: number
}

export interface FileEntry {
  name: string
  size: number
}

export const system = {
  info: () => request<SystemInfo>('GET', '/system/info'),
  health: () => request<HealthStatus>('GET', '/system/health'),
  config: () => request<Record<string, unknown>>('GET', '/system/config'),
  updateConfig: (config: Record<string, unknown>) =>
    request<{ detail: string }>('PUT', '/system/config', config),
  restart: () => request<{ detail: string }>('POST', '/system/restart'),
  ota: () => request<OTAStatus>('GET', '/system/ota'),
  wifi: () => request<WifiState>('GET', '/system/wifi'),
  wifiScan: () => request<{ networks: WifiNetwork[]; count: number }>('GET', '/system/wifi/scan'),
  wifiConnect: (ssid: string, password: string) => request<{ detail: string }>('POST', '/system/wifi/connect', { ssid, password }),
  setName: (name: string) => request<{ ok: boolean; name: string }>('PUT', '/system/name', { name }),
  backup: () => request<Record<string, unknown>>('GET', '/system/backup'),
  restore: (config: Record<string, unknown>) =>
    request<{ ok: boolean; detail: string; restart_required: boolean }>('POST', '/system/restore', config),
  files: () => request<{ files: FileEntry[]; total_bytes: number; used_bytes: number; free_bytes: number }>('GET', '/system/files'),
  fileDownload: async (name: string): Promise<string> => {
    const token = localStorage.getItem('sb_token')
    const headers: HeadersInit = {}
    if (token) headers['Authorization'] = `Bearer ${token}`
    const res = await fetch(`${API_BASE}/system/files/${encodeURIComponent(name)}`, { headers })
    if (!res.ok) throw new ApiError(res.status, res.statusText)
    return res.text()
  },
  fileUpload: (name: string, content: string) => {
    const token = localStorage.getItem('sb_token')
    const headers: HeadersInit = { 'Content-Type': 'application/octet-stream' }
    if (token) headers['Authorization'] = `Bearer ${token}`
    return fetch(`${API_BASE}/system/files/${encodeURIComponent(name)}`, {
      method: 'POST', headers, body: content,
    }).then(r => r.json()) as Promise<{ ok: boolean; filename: string; size: number }>
  },
  fileDelete: (name: string) => request<{ ok: boolean; detail: string }>('DELETE', `/system/files/${encodeURIComponent(name)}`),
}

export function otaUpload(file: File, onProgress?: (pct: number) => void): Promise<{ok: boolean; detail: string}> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest()
    xhr.open('POST', `${API_BASE}/system/ota/upload`)
    const token = localStorage.getItem('sb_token')
    if (token) xhr.setRequestHeader('Authorization', `Bearer ${token}`)
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) onProgress(Math.round((e.loaded / e.total) * 100))
    }
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        resolve(JSON.parse(xhr.responseText))
      } else {
        reject(new ApiError(xhr.status, xhr.responseText))
      }
    }
    xhr.onerror = () => reject(new Error('Upload failed'))
    xhr.send(file)
  })
}

export interface OTAStatus {
  running: { label: string; address: number; size: number; type: string }
  next_update: { label: string }
  version: string
  ota_supported: boolean
  partitions: { label: string; size: number; is_running: boolean }[]
}

// ── Servo ──

export interface ServoState {
  id: number
  position: number
  speed: number
  load: number
  temperature: number
  voltage: number
  torque_on: boolean
}

export interface SyncMove {
  id: number
  position: number
}

export const servo = {
  state: (id: number) => request<ServoState>('GET', `/servo/${id}/state`),
  setPosition: (id: number, position: number) =>
    request<{ ok: boolean }>('PUT', `/servo/${id}/position`, { position }),
  setSpeed: (id: number, speed: number) =>
    request<{ ok: boolean }>('PUT', `/servo/${id}/speed`, { speed }),
  setTorque: (id: number, enabled: boolean) =>
    request<{ ok: boolean }>('PUT', `/servo/${id}/torque`, { enabled }),
  sync: (moves: SyncMove[]) =>
    request<{ ok: boolean; count: number }>('POST', '/servo/sync', { moves }),
  scan: () => request<{ found_ids: number[]; count: number }>('GET', '/servo/scan'),
  setId: (currentId: number, newId: number) =>
    request<{ ok: boolean; old_id: number; new_id: number; verified: boolean }>('POST', '/servo/set-id', { current_id: currentId, new_id: newId }),
  backup: (id: number) =>
    request<{ ok: boolean; id: number; eeprom: number[] }>('GET', `/servo/${id}/backup`),
  restore: (id: number, eeprom: number[], includeId?: boolean, newId?: number) =>
    request<{ ok: boolean; id: number; bytes_written: number }>('POST', `/servo/${id}/restore`, {
      eeprom, include_id: includeId ?? false,
      ...(newId !== undefined ? { new_id: newId } : {}),
    }),
  readRegisters: (id: number) =>
    request<{ ok: boolean; data: number[] }>('GET', `/servo/${id}/registers`),
  writeRegister: (id: number, addr: number, data: number[], unlockEeprom?: boolean) =>
    request<{ ok: boolean }>('PUT', `/servo/${id}/register`, { addr, data, unlock_eeprom: unlockEeprom ?? false }),
  factoryReset: (id: number) =>
    request<{ ok: boolean }>('POST', `/servo/${id}/factory-reset`),
}

// ── LED ──

export const led = {
  state: () => request<LedState>('GET', '/led'),
  identify: (enabled: boolean) => request<{ ok: boolean }>('POST', '/led/identify', { enabled }),
  flash: (on: boolean) => request<{ ok: boolean }>('POST', '/led/flash', { on }),
  off: () => request<{ ok: boolean }>('POST', '/led/off'),
}

// ── Sensor ──

export interface SensorEntry {
  id: string
  type: 'temperature' | 'endstop'
  state: Record<string, unknown>
}

export interface TempReading {
  temperature: number
  timestamp_us: number
}

export const sensor = {
  all: () => request<{ sensors: SensorEntry[] }>('GET', '/sensors'),
  reading: (id: string) => request<Record<string, unknown>>('GET', `/sensor/${id}/reading`),
  history: (id: string, limit?: number) =>
    request<{ readings: TempReading[]; sensor_id: string }>(
      'GET',
      `/sensor/${id}/history${limit ? `?limit=${limit}` : ''}`,
    ),
}

// ── Emergency ──

export interface EmergencyStopResult {
  stopped: boolean
  elapsed_us: number
  deadline_met: boolean
  plugins: { plugin: string; stopped: boolean }[]
}

export const emergency = {
  stop: () => request<EmergencyStopResult>('POST', '/emergency-stop'),
}

// ── Modules ──

export const modules = {
  list: () => request<{ modules: PluginInfo[]; count: number }>('GET', '/modules'),
}

// ── Nodes ──

export interface NodeSummary {
  id: string
  type: string
  transport: string
  host?: string
  port?: number
  serial_port?: string
  baud?: number
  status: 'connected' | 'disconnected' | 'unknown'
}

export interface ProbeResult {
  reachable: boolean
  health?: { status: string }
  health_error?: string
  info?: {
    platform: string
    firmware_version?: string
    arbor_version?: string
    board_id?: string
    ip_addr?: string
    node_count?: number
    loaded_plugins?: {
      name: string
      version: string
      capabilities: string[]
    }[]
    plugins?: {
      name: string
      version: string
      health: string
      capabilities?: string[]
    }[]
  }
  info_error?: string
  config?: Record<string, unknown>
  config_error?: string
  servo_scan?: {
    found_ids: number[]
    count: number
  }
  servo_scan_error?: string
}

export interface NodeAddRequest {
  id: string
  type: string
  transport: 'wifi' | 'uart' | 'usb' | 'ethernet'
  transport_config: {
    host?: string
    network_port?: number
    port?: string
    baud?: number
  }
}

export const nodes = {
  list: () => request<{ nodes: NodeSummary[]; count: number }>('GET', '/nodes'),
  probe: (params: { host: string; port: number; timeout_seconds?: number }) =>
    request<ProbeResult>('POST', '/nodes/probe', params),
  add: (node: NodeAddRequest) =>
    request<{ ok: boolean; node_id: string; detail: string }>('POST', '/nodes', node),
  remove: (id: string) =>
    request<{ ok: boolean; detail: string }>('DELETE', `/nodes/${id}`),
}

// ── Auth ──

export const auth = {
  login: (username: string, password: string) =>
    request<{ token: string }>('POST', '/auth/login', { username, password }),
  logout: () => request<void>('POST', '/auth/logout'),
}
