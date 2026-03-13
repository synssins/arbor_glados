/**
 * Sensors view — servo temperatures, endstop status.
 * Task: W10
 */

import { useEffect } from 'react'
import { useServoStore } from '../stores/servo'
import { useSensorStore } from '../stores/sensor'

function ServoTemperature() {
  const servos = useServoStore((s) => s.servos)
  const scannedIds = useServoStore((s) => s.scannedIds)
  const scanning = useServoStore((s) => s.scanning)
  const scan = useServoStore((s) => s.scan)

  useEffect(() => {
    if (scannedIds.length === 0 && !scanning) {
      scan()
    }
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  if (scannedIds.length === 0) {
    return (
      <div className="card">
        <h3 className="font-semibold mb-2">Servo Temperatures</h3>
        <p className="text-sm text-gray-500">
          {scanning ? 'Scanning for servos...' : 'No servos found. Scan the bus from the Servos page.'}
        </p>
      </div>
    )
  }

  return (
    <div className="card space-y-4">
      <h3 className="font-semibold">Servo Temperatures</h3>
      <div className="space-y-2">
        {scannedIds.map((id) => {
          const s = servos.get(id)
          const temp = s?.temperature ?? null

          const tempColor =
            temp !== null
              ? temp > 60
                ? 'bg-red-500'
                : temp > 40
                  ? 'bg-yellow-500'
                  : 'bg-green-500'
              : 'bg-gray-300'

          const tempPercent = temp !== null
            ? Math.min(100, Math.max(0, ((temp + 10) / 80) * 100))
            : 0

          return (
            <div key={id} className="flex items-center gap-3">
              <span className="text-sm font-medium w-20">Servo #{id}</span>
              <div className="flex-1">
                <div className="h-3 bg-gray-100 rounded-full overflow-hidden">
                  <div
                    className={`h-full rounded-full transition-all duration-500 ${tempColor}`}
                    style={{ width: `${tempPercent}%` }}
                  />
                </div>
              </div>
              <span className="text-sm font-mono w-16 text-right">
                {temp !== null ? `${temp}°C` : '—'}
              </span>
            </div>
          )
        })}
      </div>
      <div className="flex justify-between text-xs text-gray-400">
        <span>-10°C</span>
        <span>70°C</span>
      </div>
    </div>
  )
}

function EndstopPanel() {
  const endstops = useSensorStore((s) => s.endstops)
  const sensors = useSensorStore((s) => s.sensors)

  // Also check fetched sensor data for endstop info
  const endstopSensor = sensors.find((s) => s.type === 'endstop')
  const endstopState = endstopSensor?.state as
    | { endstops?: { index: number; pin: number; triggered: boolean }[] }
    | undefined

  const displayEndstops =
    endstops.length > 0
      ? endstops
      : endstopState?.endstops ?? []

  if (displayEndstops.length === 0) {
    return (
      <div className="card">
        <h3 className="font-semibold mb-2">Endstops</h3>
        <p className="text-sm text-gray-500">No endstops configured</p>
      </div>
    )
  }

  return (
    <div className="card">
      <h3 className="font-semibold mb-3">Endstops</h3>
      <div className="space-y-2">
        {displayEndstops.map((es) => (
          <div
            key={es.index}
            className="flex items-center justify-between py-2 border-b border-gray-100 last:border-0"
          >
            <div className="flex items-center gap-3">
              <div
                className={`w-4 h-4 rounded-full border-2 ${
                  es.triggered
                    ? 'bg-red-500 border-red-600'
                    : 'bg-gray-200 border-gray-300'
                }`}
              />
              <span className="text-sm font-medium">Endstop #{es.index}</span>
              <span className="text-xs text-gray-400">GPIO {es.pin}</span>
            </div>
            <span
              className={`text-sm font-medium ${
                es.triggered ? 'text-red-600' : 'text-gray-500'
              }`}
            >
              {es.triggered ? 'TRIGGERED' : 'Open'}
            </span>
          </div>
        ))}
      </div>
    </div>
  )
}

export default function Sensors() {
  const fetchAll = useSensorStore((s) => s.fetchAll)

  useEffect(() => {
    fetchAll()
  }, [fetchAll])

  return (
    <div className="space-y-6">
      <h2 className="text-xl font-bold">Sensors</h2>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
        <ServoTemperature />
        <EndstopPanel />
      </div>
    </div>
  )
}
