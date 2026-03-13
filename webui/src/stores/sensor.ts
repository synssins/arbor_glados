/**
 * Sensor state store — temperature, endstops.
 * Task: W04
 */

import { create } from 'zustand'
import { sensor, type SensorEntry } from '../api/client'

interface SensorStore {
  sensors: SensorEntry[]
  temperature: number | null
  endstops: { index: number; pin: number; triggered: boolean }[]
  loading: boolean
  error: string | null

  fetchAll: () => Promise<void>
  updateFromTempEvent: (data: unknown) => void
  updateFromEndstopEvent: (data: unknown) => void
}

export const useSensorStore = create<SensorStore>((set) => ({
  sensors: [],
  temperature: null,
  endstops: [],
  loading: false,
  error: null,

  fetchAll: async () => {
    set({ loading: true })
    try {
      const result = await sensor.all()
      set({ sensors: result.sensors, loading: false, error: null })
    } catch (e) {
      set({ error: (e as Error).message, loading: false })
    }
  },

  updateFromTempEvent: (data) => {
    const d = data as { temperature?: number }
    if (d.temperature !== undefined) {
      set({ temperature: d.temperature })
    }
  },

  updateFromEndstopEvent: (data) => {
    const d = data as { endstop?: number; pin?: number; triggered?: boolean }
    if (d.endstop !== undefined) {
      set((s) => {
        const endstops = [...s.endstops]
        const idx = endstops.findIndex((e) => e.index === d.endstop)
        const entry = {
          index: d.endstop!,
          pin: d.pin ?? 0,
          triggered: d.triggered ?? false,
        }
        if (idx >= 0) {
          endstops[idx] = entry
        } else {
          endstops.push(entry)
        }
        return { endstops }
      })
    }
  },
}))
