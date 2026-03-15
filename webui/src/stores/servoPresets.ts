/**
 * Servo position preset store — localStorage-persisted saved positions.
 *
 * Each servo can have up to 20 named position presets that persist
 * across page refreshes. No backend required (Phase 1 pattern).
 * Follows the same localStorage persistence pattern as `ui.ts`.
 *
 * Task: W17
 */

import { create } from 'zustand'
import { toast } from './toast'

export interface ServoPreset {
  id: string
  name: string
  position: number
}

interface PresetStore {
  /** Map of servo ID → array of presets */
  presets: Record<number, ServoPreset[]>
  /** Add a preset for a servo. Returns true if added, false if limit reached. */
  addPreset: (servoId: number, preset: Omit<ServoPreset, 'id'>) => boolean
  /** Remove a preset by ID. */
  removePreset: (servoId: number, presetId: string) => void
  /** Rename a preset. */
  renamePreset: (servoId: number, presetId: string, name: string) => void
  /** Clear all presets for a servo. */
  clearPresets: (servoId: number) => void
}

const STORAGE_KEY = 'sb_servo_presets'

/** Max presets per servo — prevents unbounded localStorage growth. */
const MAX_PRESETS_PER_SERVO = 20

let nextPresetId = 0

function loadPresets(): Record<number, ServoPreset[]> {
  try {
    const stored = localStorage.getItem(STORAGE_KEY)
    if (stored) {
      const parsed = JSON.parse(stored)
      if (typeof parsed === 'object' && parsed !== null && !Array.isArray(parsed)) {
        // Validate each entry — reject corrupt data rather than crashing
        const validated: Record<number, ServoPreset[]> = {}
        for (const [key, value] of Object.entries(parsed)) {
          const numKey = Number(key)
          if (Number.isFinite(numKey) && Array.isArray(value)) {
            validated[numKey] = (value as ServoPreset[]).filter(
              (p) =>
                typeof p === 'object' &&
                p !== null &&
                typeof p.id === 'string' &&
                typeof p.name === 'string' &&
                typeof p.position === 'number' &&
                Number.isFinite(p.position) &&
                p.position >= 0 &&
                p.position <= 4095
            )
          }
        }
        return validated
      }
    }
  } catch {
    // Corrupt or unavailable — start fresh
  }
  return {}
}

function persistPresets(presets: Record<number, ServoPreset[]>): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(presets))
  } catch {
    // localStorage unavailable — presets still work in-memory
  }
}

export const usePresetStore = create<PresetStore>((set, get) => ({
  presets: loadPresets(),

  addPreset: (servoId, preset) => {
    const existing = get().presets[servoId] ?? []
    if (existing.length >= MAX_PRESETS_PER_SERVO) {
      toast.warning(`Maximum ${MAX_PRESETS_PER_SERVO} presets per servo reached`)
      return false
    }
    const id = `preset-${Date.now()}-${++nextPresetId}`
    const updated = {
      ...get().presets,
      [servoId]: [...existing, { ...preset, id }],
    }
    persistPresets(updated)
    set({ presets: updated })
    return true
  },

  removePreset: (servoId, presetId) => {
    const existing = get().presets[servoId] ?? []
    const updated = {
      ...get().presets,
      [servoId]: existing.filter((p) => p.id !== presetId),
    }
    persistPresets(updated)
    set({ presets: updated })
  },

  renamePreset: (servoId, presetId, name) => {
    const existing = get().presets[servoId] ?? []
    const updated = {
      ...get().presets,
      [servoId]: existing.map((p) =>
        p.id === presetId ? { ...p, name } : p
      ),
    }
    persistPresets(updated)
    set({ presets: updated })
  },

  clearPresets: (servoId) => {
    const updated = { ...get().presets }
    delete updated[servoId]
    persistPresets(updated)
    set({ presets: updated })
  },
}))
