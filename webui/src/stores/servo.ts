/**
 * Servo state store — positions, torque, scan results.
 * Task: W04
 */

import { create } from 'zustand'
import { servo, type ServoState, type SyncMove } from '../api/client'

interface ServoStore {
  servos: Map<number, ServoState>
  scannedIds: number[]
  scanning: boolean
  error: string | null

  fetchState: (id: number) => Promise<void>
  setPosition: (id: number, position: number) => Promise<void>
  setSpeed: (id: number, speed: number) => Promise<void>
  setTorque: (id: number, enabled: boolean) => Promise<void>
  syncMove: (moves: SyncMove[]) => Promise<void>
  scan: () => Promise<void>
  updateFromEvent: (data: unknown) => void
}

export const useServoStore = create<ServoStore>((set, get) => ({
  servos: new Map(),
  scannedIds: [],
  scanning: false,
  error: null,

  fetchState: async (id) => {
    try {
      const state = await servo.state(id)
      set((s) => {
        const servos = new Map(s.servos)
        servos.set(id, state)
        return { servos, error: null }
      })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  setPosition: async (id, position) => {
    try {
      await servo.setPosition(id, position)
      // Optimistic update
      set((s) => {
        const servos = new Map(s.servos)
        const existing = servos.get(id)
        if (existing) {
          servos.set(id, { ...existing, position })
        }
        return { servos }
      })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  setSpeed: async (id, speed) => {
    try {
      await servo.setSpeed(id, speed)
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  setTorque: async (id, enabled) => {
    try {
      await servo.setTorque(id, enabled)
      set((s) => {
        const servos = new Map(s.servos)
        const existing = servos.get(id)
        if (existing) {
          servos.set(id, { ...existing, torque_on: enabled })
        }
        return { servos }
      })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  syncMove: async (moves) => {
    try {
      await servo.sync(moves)
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  scan: async () => {
    set({ scanning: true, error: null })
    try {
      const result = await servo.scan()
      set({ scannedIds: result.found_ids, scanning: false })
      // Fetch state for each found servo
      for (const id of result.found_ids) {
        get().fetchState(id)
      }
    } catch (e) {
      set({ error: (e as Error).message, scanning: false })
    }
  },

  updateFromEvent: (data) => {
    const d = data as { id?: number; position?: number }
    if (d.id !== undefined) {
      set((s) => {
        const servos = new Map(s.servos)
        const existing = servos.get(d.id!)
        if (existing && d.position !== undefined) {
          servos.set(d.id!, { ...existing, position: d.position })
        }
        return { servos }
      })
    }
  },
}))
