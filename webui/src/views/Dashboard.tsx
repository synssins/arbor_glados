/**
 * Dashboard view — node health, system info, recent events.
 * Task: W08
 */

import { useSystemStore } from '../stores/system'

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  return `${(bytes / 1024).toFixed(1)} KB`
}

export default function Dashboard() {
  const info = useSystemStore((s) => s.info)
  const health = useSystemStore((s) => s.health)
  const loading = useSystemStore((s) => s.loading)
  const error = useSystemStore((s) => s.error)
  const wsConnected = useSystemStore((s) => s.wsConnected)

  if (loading && !info) {
    return <div className="text-gray-500">Loading...</div>
  }

  if (error && !info) {
    return <div className="text-red-600">Error: {error}</div>
  }

  return (
    <div className="space-y-6">
      <h2 className="text-xl font-bold">Dashboard</h2>

      {/* Status cards */}
      <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-4">
        <div className="card">
          <div className="text-sm text-gray-500 mb-1">Status</div>
          <div className="flex items-center gap-2">
            <div
              className={`w-3 h-3 rounded-full ${
                health?.status === 'healthy'
                  ? 'bg-green-500'
                  : health?.status === 'degraded'
                    ? 'bg-yellow-500'
                    : 'bg-red-500'
              }`}
            />
            <span className="text-lg font-semibold capitalize">
              {health?.status ?? 'unknown'}
            </span>
          </div>
        </div>

        <div className="card">
          <div className="text-sm text-gray-500 mb-1">Free Heap</div>
          <div className="text-lg font-semibold">
            {info ? formatBytes(info.free_heap) : '—'}
          </div>
          <div className="text-xs text-gray-500">
            min: {info ? formatBytes(info.min_free_heap) : '—'}
          </div>
        </div>

        <div className="card">
          <div className="text-sm text-gray-500 mb-1">Device</div>
          <div className="text-lg font-semibold">
            {info?.display_name || 'Arbor'}
          </div>
          <div className="text-xs text-gray-500">
            {info?.board_id ? `ID: ${info.board_id}` : ''}
          </div>
        </div>

        <div className="card">
          <div className="text-sm text-gray-500 mb-1">WebSocket</div>
          <div className="flex items-center gap-2">
            <div
              className={`w-3 h-3 rounded-full ${wsConnected ? 'bg-green-500' : 'bg-gray-400'}`}
            />
            <span className="text-lg font-semibold">
              {wsConnected ? 'Connected' : 'Disconnected'}
            </span>
          </div>
          <div className="text-xs text-gray-500">
            {info ? `${info.ws_clients} client(s)` : ''}
          </div>
        </div>
      </div>

      {/* System info */}
      {info && (
        <div className="card">
          <h3 className="text-sm font-semibold text-gray-700 mb-3">System Info</h3>
          <dl className="grid grid-cols-2 md:grid-cols-4 gap-3 text-sm">
            <div>
              <dt className="text-gray-500">Platform</dt>
              <dd className="font-medium">{info.platform}</dd>
            </div>
            <div>
              <dt className="text-gray-500">Firmware</dt>
              <dd className="font-medium">{info.firmware_version}</dd>
            </div>
            <div>
              <dt className="text-gray-500">IDF</dt>
              <dd className="font-medium">{info.idf_version}</dd>
            </div>
            <div>
              <dt className="text-gray-500">Cores</dt>
              <dd className="font-medium">{info.cores}</dd>
            </div>
          </dl>
        </div>
      )}

      {/* Plugins */}
      {info?.plugins && info.plugins.length > 0 && (
        <div className="card">
          <h3 className="text-sm font-semibold text-gray-700 mb-3">Plugins</h3>
          <div className="space-y-2">
            {info.plugins.map((p) => (
              <div
                key={p.name}
                className="flex items-center justify-between text-sm py-1.5 border-b border-gray-100 last:border-0"
              >
                <div className="flex items-center gap-2">
                  <div
                    className={`w-2 h-2 rounded-full ${
                      p.health === 'healthy'
                        ? 'bg-green-500'
                        : p.health === 'degraded'
                          ? 'bg-yellow-500'
                          : p.health === 'unhealthy'
                            ? 'bg-red-500'
                            : 'bg-gray-400'
                    }`}
                  />
                  <span className="font-medium">{p.name}</span>
                  <span className="text-gray-500">v{p.version}</span>
                </div>
                <span className="text-gray-500 capitalize">{p.health}</span>
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Health components */}
      {health?.components && health.components.length > 0 && (
        <div className="card">
          <h3 className="text-sm font-semibold text-gray-700 mb-3">
            Health Components
          </h3>
          <div className="space-y-1">
            {health.components.map((c, i) => (
              <div key={i} className="flex items-center gap-2 text-sm">
                <div
                  className={`w-2 h-2 rounded-full ${
                    c.status === 'healthy'
                      ? 'bg-green-500'
                      : c.status === 'degraded'
                        ? 'bg-yellow-500'
                        : 'bg-red-500'
                  }`}
                />
                <span>{c.message || c.status}</span>
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  )
}
