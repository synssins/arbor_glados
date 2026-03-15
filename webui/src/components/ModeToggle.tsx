/**
 * Student/Expert mode toggle button for the Layout header.
 *
 * Displays current mode with a toggle switch.
 * Student mode hides advanced/dangerous sections for classroom use.
 * Expert mode shows everything.
 *
 * Remediation:
 *   - Toggle size h-6 w-11 / thumb h-4 w-4 matches LED Identify toggle
 *     in Settings.tsx for consistency and WCAG touch target size (44px via labels)
 *   - Inactive label uses text-gray-500 (4.6:1 contrast on white) instead of
 *     text-gray-400 (2.9:1) for WCAG AA compliance
 *
 * Task: W16
 */

import { useUIStore } from '../stores/ui'

export default function ModeToggle() {
  const mode = useUIStore((s) => s.mode)
  const toggleMode = useUIStore((s) => s.toggleMode)
  const isExpert = mode === 'expert'

  return (
    <div className="flex items-center gap-2">
      <span
        className={`text-xs font-medium ${
          !isExpert ? 'text-servo-700' : 'text-gray-500'
        }`}
      >
        Student
      </span>
      <button
        onClick={toggleMode}
        role="switch"
        aria-checked={isExpert}
        aria-label={`Switch to ${isExpert ? 'Student' : 'Expert'} mode`}
        className={`relative inline-flex h-6 w-11 items-center rounded-full transition-colors focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-servo-500 ${
          isExpert ? 'bg-servo-600' : 'bg-gray-300'
        }`}
      >
        <span
          className={`inline-block h-4 w-4 transform rounded-full bg-white transition-transform shadow-sm ${
            isExpert ? 'translate-x-6' : 'translate-x-1'
          }`}
        />
      </button>
      <span
        className={`text-xs font-medium ${
          isExpert ? 'text-servo-700' : 'text-gray-500'
        }`}
      >
        Expert
      </span>
    </div>
  )
}
