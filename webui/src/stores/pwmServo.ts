/**
 * PWM Servo state store — manually configured PWM servos.
 * Task: PWM Servo Support
 */

import { create } from 'zustand'
import { pwmServo, type PwmServoConfig, type BoardProfile } from '../api/client'

interface PwmServoStore {
  servos: PwmServoConfig[]
  boardProfiles: Record<string, BoardProfile>
  loading: boolean
  error: string | null

  fetchServos: () => Promise<void>
  fetchBoardProfiles: () => Promise<void>
  addServo: (config: Partial<PwmServoConfig>) => Promise<boolean>
  removeServo: (channel: number) => Promise<void>
  setPosition: (channel: number, position: number) => Promise<void>
  updateConfig: (channel: number, config: Partial<PwmServoConfig>) => Promise<void>
}

export const usePwmServoStore = create<PwmServoStore>((set, get) => ({
  servos: [],
  boardProfiles: {},
  loading: false,
  error: null,

  fetchServos: async () => {
    set({ loading: true, error: null })
    try {
      const result = await pwmServo.list()
      set({ servos: result.servos, loading: false })
    } catch (e) {
      set({ error: (e as Error).message, loading: false })
    }
  },

  fetchBoardProfiles: async () => {
    try {
      const result = await pwmServo.boardProfiles()
      set({ boardProfiles: result.profiles })
    } catch (e) {
      // Non-critical, board profiles are optional
      console.warn('Failed to fetch board profiles:', e)
    }
  },

  addServo: async (config) => {
    set({ error: null })
    try {
      const result = await pwmServo.add(config)
      if (result.ok) {
        // Refresh the list
        await get().fetchServos()
        return true
      }
      return false
    } catch (e) {
      set({ error: (e as Error).message })
      return false
    }
  },

  removeServo: async (channel) => {
    try {
      await pwmServo.remove(channel)
      set((s) => ({
        servos: s.servos.filter((srv) => srv.channel !== channel),
        error: null,
      }))
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  setPosition: async (channel, position) => {
    try {
      await pwmServo.setPosition(channel, position)
      set({ error: null })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  updateConfig: async (channel, config) => {
    try {
      await pwmServo.updateConfig(channel, config)
      await get().fetchServos()
      set({ error: null })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },
}))
