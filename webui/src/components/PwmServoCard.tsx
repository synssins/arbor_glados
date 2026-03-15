/**
 * PWM Servo Card — position slider, pin info, config, remove.
 * Task: PWM Servo Support
 */

import { useState, useCallback, useRef, useEffect } from 'react'
import { usePwmServoStore } from '../stores/pwmServo'
import type { PwmServoConfig } from '../api/client'

interface Props {
  servo: PwmServoConfig
}

export default function PwmServoCard({ servo }: Props) {
  const setPosition = usePwmServoStore((s) => s.setPosition)
  const removeServo = usePwmServoStore((s) => s.removeServo)
  const [position, setLocalPosition] = useState(500)
  const [showConfig, setShowConfig] = useState(false)
  const [confirming, setConfirming] = useState(false)
  const throttleRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  const handleSlider = useCallback(
    (e: React.ChangeEvent<HTMLInputElement>) => {
      const val = parseInt(e.target.value, 10)
      setLocalPosition(val)
      // Throttle position sends to avoid flooding
      if (throttleRef.current) clearTimeout(throttleRef.current)
      throttleRef.current = setTimeout(() => {
        setPosition(servo.channel, val)
      }, 50)
    },
    [servo.channel, setPosition],
  )

  useEffect(() => {
    return () => {
      if (throttleRef.current) clearTimeout(throttleRef.current)
    }
  }, [])

  const handleRemove = useCallback(() => {
    if (confirming) {
      removeServo(servo.channel)
      setConfirming(false)
    } else {
      setConfirming(true)
      setTimeout(() => setConfirming(false), 3000)
    }
  }, [confirming, servo.channel, removeServo])

  const pulseRange = `${servo.min_pulse_us}–${servo.max_pulse_us}μs`
  const controllerLabel =
    servo.controller_type === 'klipper'
      ? `Klipper (${servo.klipper_name || '?'})`
      : `ESP32 (${servo.node_id || '?'})`

  return (
    <div className="border border-gray-200 rounded-lg p-4 bg-white" role="group" aria-label={`PWM Servo ${servo.name}`}>
      {/* Header */}
      <div className="flex items-center justify-between mb-3">
        <div>
          <h3 className="font-semibold text-sm">{servo.name}</h3>
          <span className="text-xs text-gray-500 font-mono">
            CH{servo.channel} · GPIO {servo.pin} · {controllerLabel}
          </span>
        </div>
        <div className="flex items-center gap-2">
          <button
            onClick={() => setShowConfig(!showConfig)}
            className="text-xs text-gray-500 hover:text-gray-700 px-2 py-1 rounded hover:bg-gray-100"
            aria-expanded={showConfig}
            aria-label={`${showConfig ? 'Hide' : 'Show'} config for ${servo.name}`}
          >
            {showConfig ? 'Hide' : 'Config'}
          </button>
          <button
            onClick={handleRemove}
            className={`text-xs px-2 py-1 rounded ${
              confirming
                ? 'bg-red-600 text-white'
                : 'text-red-500 hover:text-red-700 hover:bg-red-50'
            }`}
            aria-label={confirming ? `Confirm removal of ${servo.name}` : `Remove ${servo.name}`}
          >
            {confirming ? 'Confirm?' : 'Remove'}
          </button>
        </div>
      </div>

      {/* Position slider */}
      <div className="space-y-1">
        <div className="flex items-center justify-between text-xs text-gray-500">
          <span>Position</span>
          <span className="font-mono">{position}</span>
        </div>
        <input
          type="range"
          min={0}
          max={1000}
          value={position}
          onChange={handleSlider}
          className="w-full h-2 bg-gray-200 rounded-lg appearance-none cursor-pointer accent-servo-600"
          aria-label={`Position for ${servo.name}`}
          aria-valuemin={0}
          aria-valuemax={1000}
          aria-valuenow={position}
        />
        <div className="flex justify-between text-[10px] text-gray-500">
          <span>0</span>
          <span>500</span>
          <span>1000</span>
        </div>
      </div>

      {/* Quick positions */}
      <div className="flex gap-1 mt-2">
        {[0, 250, 500, 750, 1000].map((pos) => (
          <button
            key={pos}
            onClick={() => {
              setLocalPosition(pos)
              setPosition(servo.channel, pos)
            }}
            className="flex-1 text-xs py-1 rounded bg-gray-100 hover:bg-servo-100 text-gray-600 hover:text-servo-700 transition-colors"
            aria-label={`Set ${servo.name} to position ${pos}`}
          >
            {pos}
          </button>
        ))}
      </div>

      {/* Config details (collapsible) */}
      {showConfig && (
        <div className="mt-3 pt-3 border-t border-gray-100 space-y-1 text-xs text-gray-600">
          <div className="grid grid-cols-2 gap-x-4 gap-y-1">
            <span className="text-gray-500">Pulse Range</span>
            <span className="font-mono">{pulseRange}</span>
            <span className="text-gray-500">Controller</span>
            <span>{servo.controller_type}</span>
            <span className="text-gray-500">GPIO Pin</span>
            <span className="font-mono">{servo.pin}</span>
            <span className="text-gray-500">Invert</span>
            <span>{servo.invert ? 'Yes' : 'No'}</span>
            <span className="text-gray-500">Pull Up</span>
            <span>{servo.pull_up ? 'Yes' : 'No'}</span>
            <span className="text-gray-500">Pull Down</span>
            <span>{servo.pull_down ? 'Yes' : 'No'}</span>
          </div>
        </div>
      )}
    </div>
  )
}
