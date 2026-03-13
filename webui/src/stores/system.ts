/**
 * System state store — connection, health, info.
 * Task: W04
 */

import { create } from 'zustand'
import { system, type SystemInfo, type HealthStatus } from '../api/client'

interface SystemState {
  info: SystemInfo | null
  health: HealthStatus | null
  wsConnected: boolean
  loading: boolean
  error: string | null

  fetchInfo: () => Promise<void>
  fetchHealth: () => Promise<void>
  setWsConnected: (connected: boolean) => void
}

export const useSystemStore = create<SystemState>((set) => ({
  info: null,
  health: null,
  wsConnected: false,
  loading: false,
  error: null,

  fetchInfo: async () => {
    set({ loading: true, error: null })
    try {
      const info = await system.info()
      set({ info, loading: false })
    } catch (e) {
      set({ error: (e as Error).message, loading: false })
    }
  },

  fetchHealth: async () => {
    try {
      const health = await system.health()
      set({ health })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  setWsConnected: (connected) => set({ wsConnected: connected }),
}))
