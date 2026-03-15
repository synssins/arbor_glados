/**
 * Add PWM Servo Dialog — form to configure and add a new PWM servo.
 * Task: PWM Servo Support
 */

import { useState, useMemo, useEffect, useRef } from 'react'
import { usePwmServoStore } from '../stores/pwmServo'
import type { PwmServoConfig } from '../api/client'

interface Props {
  open: boolean
  onClose: () => void
  nodeIds: string[]
}

// ESP32-WROOM-32 PWM-capable pins
const ESP32_PWM_PINS = [0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27]
const ESP32_INPUT_ONLY = [34, 35, 36, 39]
const ESP32_RESERVED = [6, 7, 8, 9, 10, 11]

export default function AddPwmServoDialog({ open, onClose, nodeIds }: Props) {
  const addServo = usePwmServoStore((s) => s.addServo)
  const boardProfiles = usePwmServoStore((s) => s.boardProfiles)
  const existingServos = usePwmServoStore((s) => s.servos)

  const [name, setName] = useState('')
  const [controllerType, setControllerType] = useState<'esp32' | 'klipper'>('esp32')
  const [nodeId, setNodeId] = useState(nodeIds[0] || '')
  const [pin, setPin] = useState(2)
  const [klipperName, setKlipperName] = useState('')
  const [minPulse, setMinPulse] = useState(500)
  const [maxPulse, setMaxPulse] = useState(2500)
  const [invert, setInvert] = useState(false)
  const [pullUp, setPullUp] = useState(false)
  const [pullDown, setPullDown] = useState(false)
  const [submitting, setSubmitting] = useState(false)
  const [error, setError] = useState('')
  const dialogRef = useRef<HTMLDivElement>(null)

  // Focus trap + Escape handler (WCAG 2.1.2 / 2.4.3)
  useEffect(() => {
    if (!open) return

    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        onClose()
        return
      }
      if (e.key === 'Tab') {
        const dialog = dialogRef.current
        if (!dialog) return
        const focusable = dialog.querySelectorAll<HTMLElement>(
          'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'
        )
        if (focusable.length === 0) return
        const first = focusable[0]
        const last = focusable[focusable.length - 1]
        if (e.shiftKey) {
          if (document.activeElement === first) {
            e.preventDefault()
            last.focus()
          }
        } else {
          if (document.activeElement === last) {
            e.preventDefault()
            first.focus()
          }
        }
      }
    }

    // Auto-focus first input when dialog opens
    const timer = setTimeout(() => {
      const dialog = dialogRef.current
      if (dialog) {
        const first = dialog.querySelector<HTMLElement>('input, select, textarea')
        first?.focus()
      }
    }, 0)

    document.addEventListener('keydown', handleKeyDown)
    return () => {
      document.removeEventListener('keydown', handleKeyDown)
      clearTimeout(timer)
    }
  }, [open, onClose])

  // Determine next available channel
  const nextChannel = useMemo(() => {
    const used = new Set(existingServos.map((s) => s.channel))
    for (let i = 0; i < 8; i++) {
      if (!used.has(i)) return i
    }
    return -1
  }, [existingServos])

  // Get available PWM pins from board profile or fallback to defaults
  const pwmPins = useMemo(() => {
    // Try to get from board profile
    const profile = boardProfiles['esp32-wroom-32']
    if (profile && profile.pwm_capable_pins.length > 0) {
      return profile.pwm_capable_pins
    }
    return ESP32_PWM_PINS
  }, [boardProfiles])

  // Filter out pins already used by other PWM servos
  const usedPins = useMemo(() => {
    return new Set(existingServos.map((s) => s.pin))
  }, [existingServos])

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault()
    if (nextChannel < 0) {
      setError('All 8 PWM channels are in use')
      return
    }
    if (!name.trim()) {
      setError('Name is required')
      return
    }
    if (controllerType === 'klipper' && !klipperName.trim()) {
      setError('Klipper servo name is required')
      return
    }

    setSubmitting(true)
    setError('')

    const config: Partial<PwmServoConfig> = {
      channel: nextChannel,
      name: name.trim(),
      pin,
      controller_type: controllerType,
      node_id: controllerType === 'esp32' ? nodeId || null : null,
      klipper_name: controllerType === 'klipper' ? klipperName.trim() : null,
      min_pulse_us: minPulse,
      max_pulse_us: maxPulse,
      invert,
      pull_up: pullUp,
      pull_down: pullDown,
    }

    const ok = await addServo(config)
    setSubmitting(false)

    if (ok) {
      // Reset form
      setName('')
      setKlipperName('')
      setInvert(false)
      setPullUp(false)
      setPullDown(false)
      onClose()
    } else {
      setError('Failed to add servo')
    }
  }

  if (!open) return null

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/30"
      role="dialog"
      aria-modal="true"
      aria-labelledby="add-pwm-dialog-title"
      onClick={(e) => { if (e.target === e.currentTarget) onClose() }}
    >
      <div ref={dialogRef} className="bg-white rounded-lg shadow-xl w-full max-w-md mx-4 max-h-[90vh] overflow-y-auto">
        <form onSubmit={handleSubmit}>
          <div className="px-6 py-4 border-b border-gray-200">
            <h3 id="add-pwm-dialog-title" className="text-lg font-semibold">Add PWM Servo</h3>
            <p className="text-xs text-gray-500 mt-1">
              Channel {nextChannel >= 0 ? nextChannel : 'none available'} · 50 Hz · 14-bit
            </p>
          </div>

          <div className="px-6 py-4 space-y-4">
            {/* Name */}
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Name</label>
              <input
                type="text"
                value={name}
                onChange={(e) => setName(e.target.value)}
                placeholder="e.g. Pan Servo"
                className="input w-full"
                maxLength={64}
                required
              />
            </div>

            {/* Controller Type */}
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Controller</label>
              <div className="flex gap-2">
                <button
                  type="button"
                  onClick={() => setControllerType('esp32')}
                  aria-pressed={controllerType === 'esp32'}
                  className={`flex-1 px-3 py-2 rounded-md text-sm font-medium border transition-colors ${
                    controllerType === 'esp32'
                      ? 'bg-servo-100 border-servo-300 text-servo-700'
                      : 'bg-white border-gray-200 text-gray-600 hover:bg-gray-50'
                  }`}
                >
                  ESP32 Node
                </button>
                <button
                  type="button"
                  onClick={() => setControllerType('klipper')}
                  aria-pressed={controllerType === 'klipper'}
                  className={`flex-1 px-3 py-2 rounded-md text-sm font-medium border transition-colors ${
                    controllerType === 'klipper'
                      ? 'bg-servo-100 border-servo-300 text-servo-700'
                      : 'bg-white border-gray-200 text-gray-600 hover:bg-gray-50'
                  }`}
                >
                  Klipper
                </button>
              </div>
            </div>

            {/* ESP32 Node selection */}
            {controllerType === 'esp32' && (
              <>
                <div>
                  <label className="block text-sm font-medium text-gray-700 mb-1">Node</label>
                  {nodeIds.length > 0 ? (
                    <select
                      value={nodeId}
                      onChange={(e) => setNodeId(e.target.value)}
                      className="input w-full"
                    >
                      {nodeIds.map((nid) => (
                        <option key={nid} value={nid}>{nid}</option>
                      ))}
                    </select>
                  ) : (
                    <p className="text-sm text-amber-600">No ESP32 nodes connected</p>
                  )}
                </div>

                {/* GPIO Pin */}
                <div>
                  <label className="block text-sm font-medium text-gray-700 mb-1">
                    GPIO Pin
                    <span className="text-xs text-gray-500 ml-1">(PWM-capable)</span>
                  </label>
                  <select
                    value={pin}
                    onChange={(e) => setPin(parseInt(e.target.value))}
                    className="input w-full"
                  >
                    {pwmPins.map((p) => {
                      const inUse = usedPins.has(p)
                      return (
                        <option key={p} value={p} disabled={inUse}>
                          GPIO {p}{inUse ? ' (in use)' : ''}
                        </option>
                      )
                    })}
                  </select>
                  <p className="text-[10px] text-gray-500 mt-1">
                    Input-only: {ESP32_INPUT_ONLY.join(', ')} · Reserved: {ESP32_RESERVED.join(', ')}
                  </p>
                </div>
              </>
            )}

            {/* Klipper servo name */}
            {controllerType === 'klipper' && (
              <div>
                <label className="block text-sm font-medium text-gray-700 mb-1">
                  Klipper Servo Name
                </label>
                <input
                  type="text"
                  value={klipperName}
                  onChange={(e) => setKlipperName(e.target.value)}
                  placeholder="e.g. servo0"
                  className="input w-full"
                  required={controllerType === 'klipper'}
                />
                <p className="text-[10px] text-gray-500 mt-1">
                  Must match [servo ...] section in printer.cfg
                </p>
              </div>
            )}

            {/* Pulse Width Range */}
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-sm font-medium text-gray-700 mb-1">Min Pulse (μs)</label>
                <input
                  type="number"
                  min={100}
                  max={3000}
                  value={minPulse}
                  onChange={(e) => setMinPulse(parseInt(e.target.value) || 500)}
                  className="input w-full"
                />
              </div>
              <div>
                <label className="block text-sm font-medium text-gray-700 mb-1">Max Pulse (μs)</label>
                <input
                  type="number"
                  min={100}
                  max={3000}
                  value={maxPulse}
                  onChange={(e) => setMaxPulse(parseInt(e.target.value) || 2500)}
                  className="input w-full"
                />
              </div>
            </div>

            {/* GPIO Options */}
            {controllerType === 'esp32' && (
              <div className="space-y-2">
                <label className="block text-sm font-medium text-gray-700">GPIO Options</label>
                <div className="flex gap-4">
                  <label className="flex items-center gap-2 text-sm text-gray-600">
                    <input
                      type="checkbox"
                      checked={invert}
                      onChange={(e) => setInvert(e.target.checked)}
                      className="rounded border-gray-300"
                    />
                    Invert
                  </label>
                  <label className="flex items-center gap-2 text-sm text-gray-600">
                    <input
                      type="checkbox"
                      checked={pullUp}
                      onChange={(e) => {
                        setPullUp(e.target.checked)
                        if (e.target.checked) setPullDown(false)
                      }}
                      className="rounded border-gray-300"
                    />
                    Pull-up
                  </label>
                  <label className="flex items-center gap-2 text-sm text-gray-600">
                    <input
                      type="checkbox"
                      checked={pullDown}
                      onChange={(e) => {
                        setPullDown(e.target.checked)
                        if (e.target.checked) setPullUp(false)
                      }}
                      className="rounded border-gray-300"
                    />
                    Pull-down
                  </label>
                </div>
              </div>
            )}

            {error && (
              <div role="alert" className="text-sm text-red-600 bg-red-50 px-3 py-2 rounded">{error}</div>
            )}
          </div>

          {/* Actions */}
          <div className="px-6 py-4 border-t border-gray-200 flex justify-end gap-2">
            <button
              type="button"
              onClick={onClose}
              className="btn-secondary"
              disabled={submitting}
            >
              Cancel
            </button>
            <button
              type="submit"
              className="btn-primary"
              disabled={submitting || nextChannel < 0}
            >
              {submitting ? 'Adding...' : 'Add Servo'}
            </button>
          </div>
        </form>
      </div>
    </div>
  )
}
