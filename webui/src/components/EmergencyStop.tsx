/**
 * Emergency stop button — always visible, all pages.
 * Task: W06
 */

import { useState } from 'react'
import { emergency } from '../api/client'

export default function EmergencyStop() {
  const [stopping, setStopping] = useState(false)
  const [result, setResult] = useState<string | null>(null)

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
        onClick={handleStop}
        disabled={stopping}
        className="bg-red-600 hover:bg-red-700 active:bg-red-800 text-white font-bold px-4 py-2 rounded-lg shadow-lg transition-all text-sm uppercase tracking-wide disabled:opacity-50 ring-2 ring-red-400 ring-offset-1"
      >
        {stopping ? '...' : 'E-STOP'}
      </button>
      {result && (
        <div className="absolute right-0 top-full mt-1 bg-gray-900 text-white text-xs px-2 py-1 rounded whitespace-nowrap z-50">
          {result}
        </div>
      )}
    </div>
  )
}
