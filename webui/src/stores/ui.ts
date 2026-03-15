/**
 * UI preferences store — Student/Expert mode toggle.
 *
 * Persists mode selection in localStorage so it survives page refreshes.
 * Default: 'student' (grade-school STEMMA audience).
 *
 * Task: W16
 */

import { create } from 'zustand'

export type UIMode = 'student' | 'expert'

const STORAGE_KEY = 'sb_ui_mode'

function loadMode(): UIMode {
  try {
    const stored = localStorage.getItem(STORAGE_KEY)
    if (stored === 'expert') return 'expert'
  } catch {
    // localStorage unavailable — use default
  }
  return 'student'
}

function persistMode(mode: UIMode): void {
  try {
    localStorage.setItem(STORAGE_KEY, mode)
  } catch {
    // localStorage unavailable — mode still works in-memory
  }
}

interface UIState {
  mode: UIMode
  setMode: (mode: UIMode) => void
  toggleMode: () => void
}

export const useUIStore = create<UIState>((set, get) => ({
  mode: loadMode(),

  setMode: (mode) => {
    persistMode(mode)
    set({ mode })
  },

  toggleMode: () => {
    const next = get().mode === 'student' ? 'expert' : 'student'
    persistMode(next)
    set({ mode: next })
  },
}))

/** Convenience hook — returns true when Expert mode is active. */
export function useIsExpert(): boolean {
  return useUIStore((s) => s.mode === 'expert')
}
