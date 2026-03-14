/**
 * File browser state store — roots, directory listing, file editing.
 * Task: Config File Browser
 */

import { create } from 'zustand'
import { files, type FileRoot, type FileItem } from '../api/client'

interface OpenFile {
  rootId: string
  path: string
  content: string
  originalContent: string
  readonly: boolean
}

interface FileStore {
  roots: FileRoot[]
  activeRootId: string | null
  currentPath: string
  items: FileItem[]
  openFile: OpenFile | null
  loading: boolean
  saving: boolean
  error: string | null

  fetchRoots: () => Promise<void>
  selectRoot: (rootId: string) => Promise<void>
  navigateTo: (path: string) => Promise<void>
  openFilePath: (path: string) => Promise<void>
  setContent: (content: string) => void
  saveFile: () => Promise<boolean>
  saveAndRestart: () => Promise<boolean>
  deleteFile: (path: string) => Promise<void>
  closeFile: () => void
}

export const useFileStore = create<FileStore>((set, get) => ({
  roots: [],
  activeRootId: null,
  currentPath: '',
  items: [],
  openFile: null,
  loading: false,
  saving: false,
  error: null,

  fetchRoots: async () => {
    set({ loading: true, error: null })
    try {
      const result = await files.roots()
      set({ roots: result.roots, loading: false })
      // Auto-select first root if none selected
      if (result.roots.length > 0 && !get().activeRootId) {
        await get().selectRoot(result.roots[0].id)
      }
    } catch (e) {
      set({ error: (e as Error).message, loading: false })
    }
  },

  selectRoot: async (rootId) => {
    set({ activeRootId: rootId, currentPath: '', openFile: null, error: null })
    await get().navigateTo('')
  },

  navigateTo: async (path) => {
    const { activeRootId } = get()
    if (!activeRootId) return

    set({ loading: true, error: null, currentPath: path })
    try {
      const result = await files.list(activeRootId, path)
      set({ items: result.items, loading: false })
    } catch (e) {
      set({ error: (e as Error).message, loading: false, items: [] })
    }
  },

  openFilePath: async (path) => {
    const { activeRootId } = get()
    if (!activeRootId) return

    set({ loading: true, error: null })
    try {
      const result = await files.read(activeRootId, path)
      set({
        openFile: {
          rootId: activeRootId,
          path: result.path,
          content: result.content,
          originalContent: result.content,
          readonly: result.readonly,
        },
        loading: false,
      })
    } catch (e) {
      set({ error: (e as Error).message, loading: false })
    }
  },

  setContent: (content) => {
    const { openFile } = get()
    if (openFile) {
      set({ openFile: { ...openFile, content } })
    }
  },

  saveFile: async () => {
    const { openFile } = get()
    if (!openFile || openFile.readonly) return false

    set({ saving: true, error: null })
    try {
      await files.write(openFile.rootId, openFile.path, openFile.content)
      set({
        saving: false,
        openFile: { ...openFile, originalContent: openFile.content },
      })
      return true
    } catch (e) {
      set({ error: (e as Error).message, saving: false })
      return false
    }
  },

  saveAndRestart: async () => {
    const saved = await get().saveFile()
    if (!saved) return false

    const { openFile } = get()
    if (!openFile) return false

    try {
      await files.restart(openFile.rootId)
      return true
    } catch (e) {
      set({ error: `Saved but restart failed: ${(e as Error).message}` })
      return false
    }
  },

  deleteFile: async (path) => {
    const { activeRootId } = get()
    if (!activeRootId) return

    set({ error: null })
    try {
      await files.delete(activeRootId, path)
      // Refresh directory listing
      await get().navigateTo(get().currentPath)
      // Close file if it was the deleted one
      const { openFile } = get()
      if (openFile && openFile.path === path) {
        set({ openFile: null })
      }
    } catch (e) {
      set({ error: (e as Error).message })
    }
  },

  closeFile: () => set({ openFile: null }),
}))
