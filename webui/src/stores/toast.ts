/**
 * Toast notification store — centralized notification system.
 *
 * Zero external dependencies — built on Zustand (already installed).
 * Provides both React hook access (useToastStore) and imperative
 * `toast.*` helpers for use inside stores, API clients, and WS handlers.
 *
 * Task: W17
 */

import { create } from 'zustand'

export type ToastLevel = 'info' | 'success' | 'warning' | 'error'

export interface Toast {
  id: string
  level: ToastLevel
  message: string
  /** Optional secondary detail text (smaller, below message). */
  detail?: string
  /** Auto-dismiss delay in ms. 0 = sticky (user must dismiss). */
  duration: number
  createdAt: number
}

interface ToastState {
  toasts: Toast[]
  /** Add a toast. Returns the toast id for programmatic dismissal. */
  add: (level: ToastLevel, message: string, opts?: { detail?: string; duration?: number }) => string
  /** Dismiss a single toast by id. */
  dismiss: (id: string) => void
  /** Clear all toasts. */
  clear: () => void
}

let nextId = 0

const DEFAULT_DURATIONS: Record<ToastLevel, number> = {
  info: 4000,
  success: 3000,
  warning: 6000,
  error: 8000,
}

/** Max toasts shown at once — oldest are evicted first. */
const MAX_TOASTS = 10

export const useToastStore = create<ToastState>((set) => ({
  toasts: [],

  add: (level, message, opts) => {
    const id = `toast-${++nextId}`
    const duration = opts?.duration ?? DEFAULT_DURATIONS[level]
    const t: Toast = {
      id,
      level,
      message,
      detail: opts?.detail,
      duration,
      createdAt: Date.now(),
    }
    set((state) => ({
      toasts: [...state.toasts.slice(-(MAX_TOASTS - 1)), t],
    }))
    return id
  },

  dismiss: (id) => {
    set((state) => ({
      toasts: state.toasts.filter((t) => t.id !== id),
    }))
  },

  clear: () => set({ toasts: [] }),
}))

// ── Imperative helpers (usable outside React components) ──

export const toast = {
  info: (message: string, opts?: { detail?: string; duration?: number }) =>
    useToastStore.getState().add('info', message, opts),
  success: (message: string, opts?: { detail?: string; duration?: number }) =>
    useToastStore.getState().add('success', message, opts),
  warning: (message: string, opts?: { detail?: string; duration?: number }) =>
    useToastStore.getState().add('warning', message, opts),
  error: (message: string, opts?: { detail?: string; duration?: number }) =>
    useToastStore.getState().add('error', message, opts),
}
