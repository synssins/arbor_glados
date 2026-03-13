/**
 * Connection status indicator for navbar.
 * Task: W05
 */

import { useSystemStore } from '../stores/system'

export default function ConnectionStatus() {
  const wsConnected = useSystemStore((s) => s.wsConnected)
  const health = useSystemStore((s) => s.health)

  const statusColor = wsConnected
    ? health?.status === 'healthy'
      ? 'bg-green-500'
      : health?.status === 'degraded'
        ? 'bg-yellow-500'
        : 'bg-red-500'
    : 'bg-gray-400'

  const statusText = wsConnected
    ? health?.status ?? 'connected'
    : 'disconnected'

  return (
    <div className="flex items-center gap-2 text-sm">
      <div className={`w-2.5 h-2.5 rounded-full ${statusColor}`} />
      <span className="text-gray-600 capitalize">{statusText}</span>
    </div>
  )
}
