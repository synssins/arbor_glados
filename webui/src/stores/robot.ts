/**
 * Robot state store — profiles, joint positions, motion status.
 * Task: Robotics Phase 4
 */

import { create } from 'zustand'
import {
  robots,
  klipper,
  type RobotProfile,
  type RobotState,
  type MotionResult,
  type JointMoveCommand,
  type CartesianPose,
  type VelocityCommand,
  type KlipperStatus,
} from '../api/client'

interface RobotStore {
  profiles: RobotProfile[]
  selectedId: string | null
  states: Map<string, RobotState>
  loading: boolean
  error: string | null
  lastResult: MotionResult | null
  klipperStatus: KlipperStatus | null

  // Profile CRUD
  fetchProfiles: () => Promise<void>
  createProfile: (profile: RobotProfile) => Promise<void>
  deleteProfile: (id: string) => Promise<void>
  selectRobot: (id: string | null) => void

  // Motion
  moveJoints: (id: string, cmd: JointMoveCommand) => Promise<void>
  moveCartesian: (id: string, pose: CartesianPose) => Promise<void>
  drive: (id: string, cmd: VelocityCommand) => Promise<void>
  homeRobot: (id: string) => Promise<void>
  stopRobot: (id: string) => Promise<void>
  fetchState: (id: string) => Promise<void>

  // Klipper
  fetchKlipperStatus: () => Promise<void>
  sendGcode: (script: string) => Promise<string | null>
}

export const useRobotStore = create<RobotStore>((set, get) => ({
  profiles: [],
  selectedId: null,
  states: new Map(),
  loading: false,
  error: null,
  lastResult: null,
  klipperStatus: null,

  fetchProfiles: async () => {
    set({ loading: true, error: null })
    try {
      const result = await robots.list()
      set({ profiles: result.robots, loading: false })
    } catch (e) {
      set({ error: (e as Error).message, loading: false })
    }
  },

  createProfile: async (profile) => {
    try {
      await robots.create(profile)
      await get().fetchProfiles()
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  deleteProfile: async (id) => {
    try {
      await robots.delete(id)
      set((s) => ({
        profiles: s.profiles.filter((p) => p.id !== id),
        selectedId: s.selectedId === id ? null : s.selectedId,
      }))
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  selectRobot: (id) => {
    set({ selectedId: id, lastResult: null })
    if (id) get().fetchState(id)
  },

  moveJoints: async (id, cmd) => {
    try {
      const result = await robots.moveJoints(id, cmd)
      set({ lastResult: result, error: null })
      await get().fetchState(id)
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  moveCartesian: async (id, pose) => {
    try {
      const result = await robots.moveCartesian(id, pose)
      set({ lastResult: result, error: null })
      await get().fetchState(id)
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  drive: async (id, cmd) => {
    try {
      const result = await robots.drive(id, cmd)
      set({ lastResult: result, error: null })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  homeRobot: async (id) => {
    try {
      const result = await robots.home(id)
      set({ lastResult: result, error: null })
      await get().fetchState(id)
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  stopRobot: async (id) => {
    try {
      const result = await robots.stop(id)
      set({ lastResult: result, error: null })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  fetchState: async (id) => {
    try {
      const state = await robots.state(id)
      set((s) => {
        const states = new Map(s.states)
        states.set(id, state)
        return { states, error: null }
      })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  fetchKlipperStatus: async () => {
    try {
      const status = await klipper.status()
      set({ klipperStatus: status, error: null })
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  sendGcode: async (script) => {
    try {
      const result = await klipper.gcode(script)
      set({ error: null })
      return JSON.stringify(result)
    } catch (e) {
      set({ error: (e as Error).message })
      return null
    }
  },
}))
