/**
 * Config Browser — Mainsail-style config file browser and editor.
 * Browse, edit, save, and restart services from config files.
 * Task: Config File Browser
 */

import { useEffect, useState, useCallback, useRef } from 'react'
import { useFileStore } from '../stores/files'
import FileTree from '../components/FileTree'

export default function ConfigBrowser() {
  const roots = useFileStore((s) => s.roots)
  const activeRootId = useFileStore((s) => s.activeRootId)
  const currentPath = useFileStore((s) => s.currentPath)
  const items = useFileStore((s) => s.items)
  const openFile = useFileStore((s) => s.openFile)
  const loading = useFileStore((s) => s.loading)
  const saving = useFileStore((s) => s.saving)
  const error = useFileStore((s) => s.error)

  const fetchRoots = useFileStore((s) => s.fetchRoots)
  const selectRoot = useFileStore((s) => s.selectRoot)
  const navigateTo = useFileStore((s) => s.navigateTo)
  const openFilePath = useFileStore((s) => s.openFilePath)
  const setContent = useFileStore((s) => s.setContent)
  const saveFile = useFileStore((s) => s.saveFile)
  const saveAndRestart = useFileStore((s) => s.saveAndRestart)
  const closeFile = useFileStore((s) => s.closeFile)

  const textareaRef = useRef<HTMLTextAreaElement>(null)
  const [confirmRestart, setConfirmRestart] = useState(false)

  useEffect(() => {
    fetchRoots()
  }, [fetchRoots])

  // Keyboard shortcut: Ctrl+S to save
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 's' && openFile && !openFile.readonly) {
        e.preventDefault()
        saveFile()
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [openFile, saveFile])

  const isDirty = openFile ? openFile.content !== openFile.originalContent : false

  const activeRoot = roots.find((r) => r.id === activeRootId)

  // Breadcrumb parts
  const breadcrumbs = currentPath ? currentPath.split('/') : []

  const handleSaveAndRestart = useCallback(async () => {
    if (!confirmRestart) {
      setConfirmRestart(true)
      setTimeout(() => setConfirmRestart(false), 5000)
      return
    }
    await saveAndRestart()
    setConfirmRestart(false)
  }, [confirmRestart, saveAndRestart])

  return (
    <div className="space-y-4">
      <h2 className="text-xl font-bold">Config Browser</h2>

      {error && (
        <div role="alert" className="bg-red-50 text-red-700 text-sm px-3 py-2 rounded">{error}</div>
      )}

      {/* Root tabs */}
      {roots.length > 0 && (
        <div className="flex gap-1 border-b border-gray-200" role="tablist" aria-label="File root tabs">
          {roots.map((root) => (
            <button
              key={root.id}
              role="tab"
              aria-selected={activeRootId === root.id}
              onClick={() => selectRoot(root.id)}
              className={`px-4 py-2 text-sm font-medium border-b-2 transition-colors ${
                activeRootId === root.id
                  ? 'border-servo-600 text-servo-700'
                  : 'border-transparent text-gray-500 hover:text-gray-700 hover:border-gray-300'
              }`}
            >
              {root.label}
              {root.readonly && (
                <span className="ml-1 text-[10px] text-gray-500">(read-only)</span>
              )}
            </button>
          ))}
        </div>
      )}

      {roots.length === 0 && !loading && (
        <div className="text-center text-gray-500 py-12 bg-gray-50 rounded-lg">
          <p className="font-medium">No file roots configured</p>
          <p className="text-xs mt-1">
            Add <code className="bg-gray-200 px-1 rounded">file_roots</code> to your arbor.yaml config
          </p>
        </div>
      )}

      {/* Two-panel layout */}
      {activeRootId && (
        <div className="flex gap-4 min-h-[600px]">
          {/* Left panel: File tree */}
          <div className="w-72 flex-shrink-0 border border-gray-200 rounded-lg bg-white overflow-hidden flex flex-col">
            {/* Breadcrumb */}
            <div className="px-3 py-2 border-b border-gray-100 bg-gray-50 text-xs">
              <div className="flex items-center gap-1 text-gray-500 overflow-x-auto">
                <button
                  onClick={() => navigateTo('')}
                  className="hover:text-servo-600 flex-shrink-0"
                >
                  {activeRoot?.label || '/'}
                </button>
                {breadcrumbs.map((part, i) => (
                  <span key={i} className="flex items-center gap-1 flex-shrink-0">
                    <span>/</span>
                    <button
                      onClick={() => navigateTo(breadcrumbs.slice(0, i + 1).join('/'))}
                      className="hover:text-servo-600"
                    >
                      {part}
                    </button>
                  </span>
                ))}
              </div>
            </div>

            {/* File list */}
            <div className="flex-1 overflow-y-auto p-1">
              {loading && !openFile ? (
                <p className="text-xs text-gray-500 px-2 py-4 text-center">Loading...</p>
              ) : (
                <FileTree
                  items={items}
                  currentPath={currentPath}
                  onNavigate={navigateTo}
                  onOpenFile={openFilePath}
                  openFilePath={openFile?.path}
                />
              )}
            </div>
          </div>

          {/* Right panel: Editor */}
          <div className="flex-1 border border-gray-200 rounded-lg bg-white overflow-hidden flex flex-col">
            {openFile ? (
              <>
                {/* Editor header */}
                <div className="px-4 py-2 border-b border-gray-100 bg-gray-50 flex items-center justify-between">
                  <div className="flex items-center gap-2 min-w-0">
                    <span className="text-sm font-mono truncate">{openFile.path}</span>
                    {isDirty && (
                      <span className="text-[10px] text-amber-600 bg-amber-50 px-1.5 py-0.5 rounded flex-shrink-0">
                        unsaved
                      </span>
                    )}
                    {openFile.readonly && (
                      <span className="text-[10px] text-gray-500 bg-gray-100 px-1.5 py-0.5 rounded flex-shrink-0">
                        read-only
                      </span>
                    )}
                  </div>
                  <div className="flex items-center gap-2 flex-shrink-0">
                    {!openFile.readonly && (
                      <>
                        <button
                          onClick={saveFile}
                          disabled={saving || !isDirty}
                          className="btn-secondary text-xs"
                        >
                          {saving ? 'Saving...' : 'Save'}
                        </button>
                        {activeRoot?.has_restart && (
                          <button
                            onClick={handleSaveAndRestart}
                            disabled={saving}
                            className={`text-xs px-3 py-1.5 rounded-md font-medium transition-colors ${
                              confirmRestart
                                ? 'bg-red-600 text-white hover:bg-red-700'
                                : 'bg-amber-500 text-white hover:bg-amber-600'
                            }`}
                          >
                            {confirmRestart ? 'Confirm Restart?' : 'Save & Restart'}
                          </button>
                        )}
                      </>
                    )}
                    <button onClick={closeFile} className="text-gray-500 hover:text-gray-600" aria-label="Close file">
                      <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
                        <path strokeLinecap="round" strokeLinejoin="round" d="M6 18L18 6M6 6l12 12" />
                      </svg>
                    </button>
                  </div>
                </div>

                {/* Editor area */}
                <div className="flex-1 relative">
                  <textarea
                    ref={textareaRef}
                    value={openFile.content}
                    onChange={(e) => setContent(e.target.value)}
                    readOnly={openFile.readonly}
                    className="w-full h-full p-4 font-mono text-sm resize-none focus:outline-none bg-white"
                    style={{ tabSize: 4 }}
                    spellCheck={false}
                    aria-label={`Editing ${openFile.path}${openFile.readonly ? ' (read-only)' : ''}`}
                  />
                </div>

                {/* Status bar */}
                <div className="px-4 py-1 border-t border-gray-100 bg-gray-50 flex items-center justify-between text-[10px] text-gray-500">
                  <span>
                    {openFile.content.split('\n').length} lines · {new Blob([openFile.content]).size} bytes
                  </span>
                  <span className="font-mono">Ctrl+S to save</span>
                </div>
              </>
            ) : (
              <div className="flex-1 flex items-center justify-center text-gray-500">
                <div className="text-center">
                  <svg className="w-12 h-12 mx-auto mb-3 text-gray-300" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={1}>
                    <path strokeLinecap="round" strokeLinejoin="round" d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z" />
                  </svg>
                  <p className="text-sm">Select a file to edit</p>
                </div>
              </div>
            )}
          </div>
        </div>
      )}
    </div>
  )
}
