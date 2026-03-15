/**
 * Emergency stop button — 64×64px circular, always visible, all pages.
 * Sized for easy targeting by grade-school students.
 * Task: W06
 */

import { useState, useEffect, useRef } from 'react'
import { emergency } from '../api/client'

export default function EmergencyStop() {
  const [stopping, setStopping] = useState(false)
  const [result, setResult] = useState<string | null>(null)
  const dismissTimer = useRef<ReturnType<typeof setTimeout> | null>(null)

  // Auto-dismiss result after 5 seconds
  useEffect(() => {
    if (result) {
      dismissTimer.current = setTimeout(() => setResult(null), 5000)
    }
    return () => {
      if (dismissTimer.current) clearTimeout(dismissTimer.current)
    }
  }, [result])

  const handleStop = async () => {
    setStopping(true)
    setResult(null)
    try {
      const res = await emergency.stop()
      setResult(
        res.stopped
          ? `Stopped in ${(res.elapsed_us / 1000).toFixed(1)}ms`
          : 'Stop command sent but no servos confirmed',
      )
    } catch (e) {
      setResult(`Error: ${(e as Error).message}`)
    } finally {
      setStopping(false)
    }
  }

  return (
    <div className="relative">
      <button
        type="button"
        onClick={handleStop}
        disabled={stopping}
        aria-label="Emergency stop — immediately disable all servo torque"
        className="w-16 h-16 rounded-full bg-red-700 hover:bg-red-800 active:bg-red-900 text-white font-bold shadow-lg transition-all text-sm uppercase disabled:opacity-50 ring-4 ring-red-300 ring-offset-2 flex items-center justify-center focus:outline-none focus-visible:ring-yellow-700"
      >
        {stopping ? 'STOPPING' : 'E-STOP'}
      </button>
      {/* Always-present live region for screen reader announcements */}
      <div
        role="alert"
        className={
          result
            ? 'absolute right-0 top-full mt-1 bg-gray-900 text-white text-xs px-2 py-1 rounded whitespace-nowrap z-50'
            : 'sr-only'
        }
      >
        {result}
      </div>
    </div>
  )
}
