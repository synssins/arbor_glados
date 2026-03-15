/**
 * Toast notification renderer — fixed-position overlay.
 *
 * Renders active toasts from the toast store with auto-dismiss timers,
 * accessible ARIA live regions, and level-appropriate styling.
 * Positioned below the navbar to avoid obscuring the E-Stop button.
 *
 * Task: W17
 */

import { useCallback, useEffect, useRef } from 'react'
import { useToastStore, type Toast, type ToastLevel } from '../stores/toast'

// ── Level-specific styles ──

const LEVEL_STYLES: Record<ToastLevel, { bg: string; border: string; text: string; icon: string }> = {
  info:    { bg: 'bg-blue-50',   border: 'border-blue-200',   text: 'text-blue-800',   icon: 'ℹ' },
  success: { bg: 'bg-green-50',  border: 'border-green-200',  text: 'text-green-800',  icon: '✓' },
  warning: { bg: 'bg-yellow-50', border: 'border-yellow-200', text: 'text-yellow-800', icon: '!' },
  error:   { bg: 'bg-red-50',    border: 'border-red-200',    text: 'text-red-800',    icon: '✕' },
}

const ICON_BG: Record<ToastLevel, string> = {
  info:    'bg-blue-200 text-blue-700',
  success: 'bg-green-200 text-green-700',
  warning: 'bg-yellow-200 text-yellow-700',
  error:   'bg-red-200 text-red-700',
}

// ── Toast item ──

function ToastItem({ toast: t }: { toast: Toast }) {
  const dismiss = useToastStore((s) => s.dismiss)
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const remainingRef = useRef(t.duration)
  const startRef = useRef(Date.now())

  const startTimer = useCallback(() => {
    if (timerRef.current) clearTimeout(timerRef.current)
    if (remainingRef.current > 0) {
      startRef.current = Date.now()
      timerRef.current = setTimeout(() => dismiss(t.id), remainingRef.current)
    }
  }, [dismiss, t.id])

  const pauseTimer = useCallback(() => {
    if (timerRef.current) {
      clearTimeout(timerRef.current)
      timerRef.current = null
      remainingRef.current -= Date.now() - startRef.current
      if (remainingRef.current < 0) remainingRef.current = 0
    }
  }, [])

  useEffect(() => {
    startTimer()
    return () => {
      if (timerRef.current) clearTimeout(timerRef.current)
    }
  }, [startTimer])

  const style = LEVEL_STYLES[t.level]

  return (
    <div
      role={t.level === 'error' ? 'alert' : 'status'}
      className={`${style.bg} ${style.border} border rounded-lg shadow-md px-4 py-3 flex items-start gap-3 w-80 max-w-[calc(100vw-2rem)] toast-slide-in`}
      onMouseEnter={pauseTimer}
      onMouseLeave={startTimer}
      onFocus={pauseTimer}
      onBlur={startTimer}
    >
      {/* Icon badge */}
      <span
        className={`flex-shrink-0 w-5 h-5 rounded-full flex items-center justify-center text-xs font-bold ${ICON_BG[t.level]}`}
        aria-hidden="true"
      >
        {style.icon}
      </span>

      {/* Content */}
      <div className="flex-1 min-w-0">
        <p className={`text-sm font-medium ${style.text}`}>{t.message}</p>
        {t.detail && (
          <p className={`text-xs mt-0.5 ${style.text} break-words`}>
            {t.detail}
          </p>
        )}
      </div>

      {/* Dismiss button — min 44×44px touch target for grade-school students */}
      <button
        onClick={() => dismiss(t.id)}
        className={`flex-shrink-0 ${style.text} opacity-70 hover:opacity-100 transition-opacity text-lg leading-none min-w-[44px] min-h-[44px] flex items-center justify-center -mr-2 -mt-1`}
        aria-label="Dismiss notification"
      >
        ×
      </button>
    </div>
  )
}

// ── Container ──

export default function ToastContainer() {
  const toasts = useToastStore((s) => s.toasts)

  if (toasts.length === 0) return null

  return (
    <section
      className="fixed top-24 right-4 z-50 flex flex-col gap-2 pointer-events-none"
      aria-label="Notifications"
    >
      {toasts.map((t) => (
        <div key={t.id} className="pointer-events-auto">
          <ToastItem toast={t} />
        </div>
      ))}
    </section>
  )
}
