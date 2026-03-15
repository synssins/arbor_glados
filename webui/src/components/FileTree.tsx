/**
 * FileTree — recursive directory tree with expand/collapse.
 * Task: Config File Browser
 */

import type { FileItem } from '../api/client'

interface Props {
  items: FileItem[]
  currentPath: string
  onNavigate: (path: string) => void
  onOpenFile: (path: string) => void
  openFilePath?: string
}

function joinPath(base: string, name: string): string {
  return base ? `${base}/${name}` : name
}

function FileIcon({ type, name }: { type: 'file' | 'directory'; name: string }) {
  if (type === 'directory') {
    return (
      <svg className="w-4 h-4 text-amber-500 flex-shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
        <path strokeLinecap="round" strokeLinejoin="round" d="M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z" />
      </svg>
    )
  }

  // File icon color by extension
  const ext = name.split('.').pop()?.toLowerCase() || ''
  let color = 'text-gray-400'
  if (['cfg', 'conf', 'ini'].includes(ext)) color = 'text-blue-500'
  else if (['yaml', 'yml'].includes(ext)) color = 'text-green-500'
  else if (['json'].includes(ext)) color = 'text-amber-600'
  else if (['md', 'txt'].includes(ext)) color = 'text-gray-500'

  return (
    <svg className={`w-4 h-4 ${color} flex-shrink-0`} fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
      <path strokeLinecap="round" strokeLinejoin="round" d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z" />
    </svg>
  )
}

function formatSize(bytes: number): string {
  if (bytes === 0) return ''
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1048576).toFixed(1)} MB`
}

export default function FileTree({ items, currentPath, onNavigate, onOpenFile, openFilePath }: Props) {
  return (
    <div className="text-sm" role="list" aria-label="File browser">
      {/* Parent directory link */}
      {currentPath && (
        <button
          onClick={() => {
            const parts = currentPath.split('/')
            parts.pop()
            onNavigate(parts.join('/'))
          }}
          className="flex items-center gap-2 px-2 py-1.5 w-full text-left hover:bg-gray-100 rounded text-gray-500"
          role="listitem"
          aria-label="Navigate to parent directory"
        >
          <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
            <path strokeLinecap="round" strokeLinejoin="round" d="M11 17l-5-5m0 0l5-5m-5 5h12" />
          </svg>
          <span>..</span>
        </button>
      )}

      {items.map((item) => {
        const fullPath = joinPath(currentPath, item.name)
        const isOpen = openFilePath === fullPath

        return (
          <button
            key={item.name}
            role="listitem"
            aria-label={item.type === 'directory' ? `Open folder ${item.name}` : `Open file ${item.name}`}
            aria-current={isOpen ? 'true' : undefined}
            onClick={() => {
              if (item.type === 'directory') {
                onNavigate(fullPath)
              } else {
                onOpenFile(fullPath)
              }
            }}
            className={`flex items-center gap-2 px-2 py-1.5 w-full text-left rounded transition-colors ${
              isOpen
                ? 'bg-servo-100 text-servo-700'
                : 'hover:bg-gray-100 text-gray-700'
            }`}
          >
            <FileIcon type={item.type} name={item.name} />
            <span className="flex-1 truncate">{item.name}</span>
            {item.type === 'file' && item.size > 0 && (
              <span className="text-xs text-gray-500 flex-shrink-0">{formatSize(item.size)}</span>
            )}
          </button>
        )
      })}

      {items.length === 0 && (
        <p className="text-gray-500 text-xs px-2 py-4 text-center">Empty directory</p>
      )}
    </div>
  )
}
