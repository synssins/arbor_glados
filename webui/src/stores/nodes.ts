/**
 * Node management store — node list, probe, add/remove.
 * Follows pattern of system.ts, servo.ts, sensor.ts.
 */

import { create } from 'zustand'
import { nodes as nodesApi, type NodeSummary, type ProbeResult, type NodeAddRequest } from '../api/client'

interface NodeStore {
  nodes: NodeSummary[]
  loading: boolean
  error: string | null

  // Probe state
  probing: boolean
  probeResult: ProbeResult | null
  probeError: string | null

  // Actions
  fetchNodes: () => Promise<void>
  probeNode: (host: string, port: number) => Promise<void>
  addNode: (node: NodeAddRequest) => Promise<boolean>
  removeNode: (id: string) => Promise<boolean>
  clearProbe: () => void
  clearError: () => void
}

export const useNodeStore = create<NodeStore>((set) => ({
  nodes: [],
  loading: false,
  error: null,
  probing: false,
  probeResult: null,
  probeError: null,

  fetchNodes: async () => {
    set({ loading: true, error: null })
    try {
      const result = await nodesApi.list()
      set({ nodes: result.nodes, loading: false })
    } catch (e) {
      set({ error: (e as Error).message, loading: false })
    }
  },

  probeNode: async (host: string, port: number) => {
    set({ probing: true, probeResult: null, probeError: null })
    try {
      const result = await nodesApi.probe({ host, port, timeout_seconds: 5 })
      set({ probeResult: result, probing: false })
    } catch (e) {
      set({ probeError: (e as Error).message, probing: false })
    }
  },

  addNode: async (node: NodeAddRequest) => {
    try {
      await nodesApi.add(node)
      // Refresh the list
      const result = await nodesApi.list()
      set({ nodes: result.nodes, error: null })
      return true
    } catch (e) {
      set({ error: (e as Error).message })
      return false
    }
  },

  removeNode: async (id: string) => {
    try {
      await nodesApi.remove(id)
      set((s) => ({ nodes: s.nodes.filter((n) => n.id !== id), error: null }))
      return true
    } catch (e) {
      set({ error: (e as Error).message })
      return false
    }
  },

  clearProbe: () => set({ probeResult: null, probeError: null }),
  clearError: () => set({ error: null }),
}))
