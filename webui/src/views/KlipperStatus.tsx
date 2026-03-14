/**
 * Klipper status and G-code console view.
 * Task: Robotics Phase 4
 */

import { useEffect, useState, useRef } from 'react'
import { useRobotStore } from '../stores/robot'

export default function KlipperStatus() {
  const klipperStatus = useRobotStore((s) => s.klipperStatus)
  const fetchKlipperStatus = useRobotStore((s) => s.fetchKlipperStatus)
  const sendGcode = useRobotStore((s) => s.sendGcode)
  const error = useRobotStore((s) => s.error)

  const [gcodeInput, setGcodeInput] = useState('')
  const [history, setHistory] = useState<{ cmd: string; result: string; ts: number }[]>([])
  const logRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    fetchKlipperStatus()
    const interval = setInterval(fetchKlipperStatus, 10000)
    return () => clearInterval(interval)
  }, [fetchKlipperStatus])

  useEffect(() => {
    if (logRef.current) {
      logRef.current.scrollTop = logRef.current.scrollHeight
    }
  }, [history])

  const handleSend = async () => {
    const cmd = gcodeInput.trim()
    if (!cmd) return

    const result = await sendGcode(cmd)
    setHistory((h) => [
      ...h,
      {
        cmd,
        result: result ?? 'Error',
        ts: Date.now(),
      },
    ])
    setGcodeInput('')
  }

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      handleSend()
    }
  }

  const printerInfo = klipperStatus?.result

  return (
    <div className="space-y-6">
      <h2 className="text-xl font-bold">Klipper Control</h2>

      {/* Status card */}
      <div className="card p-4">
        <h3 className="font-semibold text-sm text-gray-600 uppercase mb-3">Printer Status</h3>
        {printerInfo ? (
          <div className="grid grid-cols-2 md:grid-cols-3 gap-3">
            <div>
              <div className="text-xs text-gray-500">State</div>
              <div className={`font-semibold ${
                printerInfo.state === 'ready' ? 'text-green-600' :
                printerInfo.state === 'standby' ? 'text-yellow-600' :
                'text-red-600'
              }`}>
                {printerInfo.state}
              </div>
            </div>
            {printerInfo.state_message && (
              <div className="col-span-2">
                <div className="text-xs text-gray-500">Message</div>
                <div className="text-sm">{printerInfo.state_message}</div>
              </div>
            )}
            <div>
              <div className="text-xs text-gray-500">Hostname</div>
              <div className="text-sm font-mono">{printerInfo.hostname}</div>
            </div>
            <div>
              <div className="text-xs text-gray-500">Version</div>
              <div className="text-sm font-mono">{printerInfo.software_version}</div>
            </div>
            {printerInfo.cpu_info && (
              <div>
                <div className="text-xs text-gray-500">CPU</div>
                <div className="text-sm font-mono truncate" title={printerInfo.cpu_info}>
                  {printerInfo.cpu_info}
                </div>
              </div>
            )}
          </div>
        ) : (
          <div className="text-sm text-gray-400">
            {error ? `Error: ${error}` : 'Loading...'}
          </div>
        )}
        <button onClick={fetchKlipperStatus}
          className="mt-3 text-xs text-blue-600 hover:underline">
          Refresh
        </button>
      </div>

      {/* G-code console */}
      <div className="card p-4">
        <h3 className="font-semibold text-sm text-gray-600 uppercase mb-3">G-code Console</h3>

        {/* Log output */}
        <div ref={logRef}
          className="bg-gray-900 text-green-400 font-mono text-xs p-3 rounded h-64 overflow-y-auto mb-3">
          {history.length === 0 ? (
            <div className="text-gray-500">
              Enter G-code commands below. Examples:
              <br />  MANUAL_STEPPER STEPPER=stepper_x MOVE=10
              <br />  SET_SERVO SERVO=servo0 ANGLE=90
              <br />  M114 (report position)
              <br />  STATUS
            </div>
          ) : (
            history.map((entry, i) => (
              <div key={i} className="mb-2">
                <div className="text-yellow-400">&gt; {entry.cmd}</div>
                <div className="text-gray-300 whitespace-pre-wrap">{entry.result}</div>
              </div>
            ))
          )}
        </div>

        {/* Input */}
        <div className="flex gap-2">
          <input
            type="text"
            value={gcodeInput}
            onChange={(e) => setGcodeInput(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Enter G-code command..."
            className="flex-1 border rounded px-3 py-2 text-sm font-mono"
          />
          <button onClick={handleSend}
            className="px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 text-sm">
            Send
          </button>
        </div>
      </div>

      {/* Quick actions */}
      <div className="card p-4">
        <h3 className="font-semibold text-sm text-gray-600 uppercase mb-3">Quick Commands</h3>
        <div className="flex flex-wrap gap-2">
          {[
            { label: 'Status', cmd: 'STATUS' },
            { label: 'Home All', cmd: 'G28' },
            { label: 'Motors Off', cmd: 'M84' },
            { label: 'Report Position', cmd: 'M114' },
            { label: 'Emergency Stop', cmd: 'M112' },
            { label: 'Firmware Restart', cmd: 'FIRMWARE_RESTART' },
          ].map(({ label, cmd }) => (
            <button key={cmd}
              onClick={async () => {
                const result = await sendGcode(cmd)
                setHistory((h) => [...h, { cmd, result: result ?? 'Error', ts: Date.now() }])
              }}
              className={`px-3 py-1 rounded text-sm ${
                cmd === 'M112' ? 'bg-red-600 text-white hover:bg-red-700' :
                'bg-gray-200 hover:bg-gray-300'
              }`}
            >
              {label}
            </button>
          ))}
        </div>
      </div>
    </div>
  )
}
