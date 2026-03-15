/**
 * React Error Boundary — catches unhandled render errors and shows a
 * friendly fallback UI instead of a white page.
 *
 * Must be a class component (React requirement for getDerivedStateFromError).
 * Includes a standalone E-Stop button so hardware safety is maintained
 * even when the main UI crashes.
 *
 * Task: W17
 */

import { Component, type ErrorInfo, type ReactNode } from 'react'
import { toast } from '../stores/toast'
import { emergency } from '../api/client'

interface Props {
  children: ReactNode
  /** Optional custom fallback UI. If omitted, the default error card renders. */
  fallback?: ReactNode
}

interface State {
  hasError: boolean
  error: Error | null
  stopping: boolean
}

export default class ErrorBoundary extends Component<Props, State> {
  constructor(props: Props) {
    super(props)
    this.state = { hasError: false, error: null, stopping: false }
  }

  static getDerivedStateFromError(error: Error): Partial<State> {
    return { hasError: true, error }
  }

  componentDidCatch(error: Error, errorInfo: ErrorInfo): void {
    // eslint-disable-next-line no-console
    console.error('[ErrorBoundary] Uncaught error:', error, errorInfo)
    toast.error('Something went wrong', {
      detail: error.message,
      duration: 0, // sticky — user must dismiss
    })
  }

  private handleReset = () => {
    this.setState({ hasError: false, error: null })
  }

  private handleEmergencyStop = async () => {
    this.setState({ stopping: true })
    try {
      await emergency.stop()
      toast.success('Emergency stop executed')
    } catch (e) {
      toast.error('Emergency stop failed', { detail: (e as Error).message })
    } finally {
      this.setState({ stopping: false })
    }
  }

  render() {
    if (this.state.hasError) {
      if (this.props.fallback) {
        return this.props.fallback
      }

      return (
        <div
          className="min-h-screen flex items-center justify-center bg-gray-50 p-6"
          role="alert"
          tabIndex={-1}
          ref={(el) => el?.focus()}
        >
          <div className="max-w-md w-full bg-white rounded-lg shadow-lg p-8 text-center">
            <div className="text-4xl mb-4" aria-hidden="true">
              ⚠️
            </div>
            <h1 className="text-xl font-bold text-gray-900 mb-2">
              Something went wrong
            </h1>
            <p className="text-sm text-gray-600 mb-4">
              An unexpected error occurred. You can try again or reload the page.
            </p>
            {this.state.error && (
              <details className="mb-4 text-left">
                <summary className="text-xs text-gray-500 cursor-pointer hover:text-gray-700">
                  Show error details
                </summary>
                <pre className="bg-gray-100 rounded p-3 text-xs text-red-700 overflow-auto max-h-32 mt-2">
                  {this.state.error.message}
                </pre>
              </details>
            )}
            <div className="flex gap-3 justify-center mb-4">
              <button
                onClick={this.handleReset}
                className="btn-primary"
              >
                Try Again
              </button>
              <button
                onClick={() => window.location.reload()}
                className="btn-secondary"
              >
                Reload Page
              </button>
            </div>

            {/* E-Stop — always available even during UI crash */}
            <div className="border-t border-gray-200 pt-4">
              <button
                onClick={this.handleEmergencyStop}
                disabled={this.state.stopping}
                className="btn bg-red-700 text-white hover:bg-red-800 focus:ring-red-500 w-full"
                aria-label="Emergency stop — immediately disable all servo torque"
                type="button"
              >
                {this.state.stopping ? 'STOPPING...' : '🛑 EMERGENCY STOP'}
              </button>
            </div>
          </div>
        </div>
      )
    }

    return this.props.children
  }
}
