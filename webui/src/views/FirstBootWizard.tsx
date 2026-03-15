/**
 * First-boot setup wizard.
 *
 * Shown when the system is in provisioning mode (no access codes exist).
 * Guides the user through initial setup:
 *   1. Welcome — explains what Arbor is
 *   2. Create Access Code — generates first admin code to lock down system
 *   3. Done — shows code, redirects to dashboard
 *
 * Remediation:
 *   - "API Key" → "Access Code" (grade-school appropriate)
 *   - Provisioning guard: redirects away if not in provisioning mode
 *   - Immediate localStorage persist after key creation (survives refresh)
 *   - Double-click guard via loading ref
 *   - localStorage wrapped in try-catch with user warning on failure
 *   - Full ARIA: progress indicator with role="list", form labels, error role,
 *     copy button, loading state role="status"
 *   - Simplified language for STEMMA classroom audience
 *   - Provisioning warning elevated to amber box (safety visibility)
 *   - WCAG AA contrast: inactive steps use text-gray-700 on bg-gray-200
 *   - Back button has visible focus ring
 *
 * Task: W15
 */

import { useState, useEffect, useRef } from 'react'
import { useNavigate } from 'react-router-dom'
import { auth, system } from '../api/client'
import type { ApiKeyCreateResult } from '../api/client'

type Step = 'welcome' | 'create-code' | 'done'

const STEP_LABELS = ['Welcome', 'Create Code', 'Done'] as const
const STEPS: Step[] = ['welcome', 'create-code', 'done']

function safeLocalStorageSet(key: string, value: string): boolean {
  try {
    localStorage.setItem(key, value)
    return true
  } catch {
    return false
  }
}

export default function FirstBootWizard() {
  const [step, setStep] = useState<Step>('welcome')
  const [codeName, setCodeName] = useState('My Access Code')
  const [createdKey, setCreatedKey] = useState<ApiKeyCreateResult | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [storageWarning, setStorageWarning] = useState(false)
  const [loading, setLoading] = useState(false)
  const [copied, setCopied] = useState(false)
  const [guardChecked, setGuardChecked] = useState(false)
  const creatingRef = useRef(false)
  const copyTimeoutRef = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)
  const navigate = useNavigate()

  // Guard: redirect away if not in provisioning mode
  useEffect(() => {
    let cancelled = false
    system.info().then((info) => {
      if (cancelled) return
      if (!info.provisioning_mode) {
        navigate('/', { replace: true })
      } else {
        setGuardChecked(true)
      }
    }).catch(() => {
      // If we can't reach the API, show the wizard anyway
      if (!cancelled) setGuardChecked(true)
    })
    return () => { cancelled = true }
  }, [navigate])

  // Cleanup copy feedback timeout on unmount
  useEffect(() => {
    return () => clearTimeout(copyTimeoutRef.current)
  }, [])

  const currentStepIndex = STEPS.indexOf(step)

  const handleCreateCode = async () => {
    // Double-click guard
    if (creatingRef.current) return
    if (!codeName.trim()) {
      setError('Please enter a name for your access code')
      return
    }
    setError(null)
    setLoading(true)
    creatingRef.current = true
    try {
      const result = await auth.createApiKey(codeName.trim())
      setCreatedKey(result)
      // Persist immediately so the code survives a page refresh
      const saved = safeLocalStorageSet('sb_token', result.plaintext_key)
      if (!saved) {
        setStorageWarning(true)
      }
      setStep('done')
    } catch (e) {
      setError((e as Error).message)
    } finally {
      setLoading(false)
      creatingRef.current = false
    }
  }

  const handleCopyCode = async () => {
    if (createdKey) {
      try {
        await navigator.clipboard.writeText(createdKey.plaintext_key)
        setCopied(true)
        clearTimeout(copyTimeoutRef.current)
        copyTimeoutRef.current = setTimeout(() => setCopied(false), 2000)
      } catch {
        // Fallback: select the text for manual copy
        const el = document.getElementById('access-code-display')
        if (el) {
          const range = document.createRange()
          range.selectNodeContents(el)
          window.getSelection()?.removeAllRanges()
          window.getSelection()?.addRange(range)
        }
      }
    }
  }

  const handleFinish = () => {
    if (createdKey) {
      // Ensure token is stored (may already be set from creation step)
      safeLocalStorageSet('sb_token', createdKey.plaintext_key)
    }
    navigate('/')
  }

  // Don't render until provisioning guard has checked
  if (!guardChecked) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gray-50">
        <p role="status" aria-live="polite" className="text-sm text-gray-500">
          Loading...
        </p>
      </div>
    )
  }

  return (
    <div className="min-h-screen flex items-center justify-center bg-gray-50">
      <div className="card w-full max-w-md">
        {/* Progress indicator */}
        <nav
          aria-label="Setup progress"
          className="flex items-center justify-center gap-2 mb-6"
        >
          <ol role="list" className="flex items-center gap-2">
            {STEPS.map((s, i) => (
              <li key={s} className="flex items-center gap-2">
                <div
                  aria-label={`Step ${i + 1}: ${STEP_LABELS[i]}${step === s ? ' (current)' : currentStepIndex > i ? ' (completed)' : ''}`}
                  aria-current={step === s ? 'step' : undefined}
                  className={`w-8 h-8 rounded-full flex items-center justify-center text-sm font-medium ${
                    step === s
                      ? 'bg-servo-600 text-white'
                      : currentStepIndex > i
                        ? 'bg-servo-100 text-servo-700'
                        : 'bg-gray-200 text-gray-700'
                  }`}
                >
                  {i + 1}
                </div>
                {i < 2 && (
                  <div
                    aria-hidden="true"
                    className={`w-8 h-0.5 ${
                      currentStepIndex > i ? 'bg-servo-300' : 'bg-gray-200'
                    }`}
                  />
                )}
              </li>
            ))}
          </ol>
        </nav>

        {/* Step 1: Welcome */}
        {step === 'welcome' && (
          <div className="space-y-4">
            <h1 className="text-2xl font-bold text-center text-servo-700">
              Welcome to Arbor
            </h1>
            <p className="text-sm text-gray-600 text-center">
              Arbor controls your servos and motors. Let&apos;s get your
              system ready to use!
            </p>
            <div className="bg-blue-50 rounded-lg p-3 text-sm text-blue-800">
              <p className="font-medium mb-1">What we&apos;ll do:</p>
              <ol className="list-decimal list-inside space-y-1 text-blue-700">
                <li>Create your access code (like a password)</li>
                <li>Lock your system so only you can use it</li>
              </ol>
            </div>
            <div className="bg-amber-50 border border-amber-200 rounded-lg p-3 text-sm text-amber-800">
              <p className="font-medium">Your system is currently open</p>
              <p className="text-xs mt-1">
                Anyone on this network can control your servos right now.
                Complete setup quickly to lock it down.
              </p>
            </div>
            <button
              onClick={() => setStep('create-code')}
              className="btn-primary w-full"
            >
              Get Started
            </button>
          </div>
        )}

        {/* Step 2: Create Access Code */}
        {step === 'create-code' && (
          <div className="space-y-4">
            <h1 className="text-xl font-bold text-center text-servo-700">
              Create Your Access Code
            </h1>
            <p className="text-sm text-gray-600 text-center">
              This code is like a password for Arbor. Keep it somewhere safe —
              you&apos;ll need it to log in.
            </p>
            <div>
              <label
                htmlFor="code-name-input"
                className="block text-sm font-medium text-gray-700 mb-1"
              >
                Code Name
              </label>
              <input
                id="code-name-input"
                type="text"
                value={codeName}
                onChange={(e) => setCodeName(e.target.value)}
                className="input"
                placeholder="e.g., Teacher Code, Lab Computer"
                autoFocus
                aria-describedby="code-name-hint"
              />
              <p id="code-name-hint" className="text-xs text-gray-500 mt-1">
                A friendly name so you can tell your codes apart later.
              </p>
            </div>

            {error && (
              <div
                role="alert"
                className="bg-red-50 text-red-700 text-sm p-2 rounded"
              >
                {error}
              </div>
            )}

            <div className="flex gap-2">
              <button
                onClick={() => setStep('welcome')}
                className="flex-1 py-2 text-sm rounded border border-gray-300 hover:bg-gray-50 transition-colors focus:outline-none focus:ring-2 focus:ring-offset-2 focus:ring-gray-400"
              >
                Back
              </button>
              <button
                onClick={handleCreateCode}
                disabled={loading}
                aria-busy={loading}
                className="flex-1 btn-primary"
              >
                {loading ? 'Creating...' : 'Create Code'}
              </button>
            </div>
          </div>
        )}

        {/* Step 3: Done */}
        {step === 'done' && createdKey && (
          <div className="space-y-4">
            <h1 className="text-xl font-bold text-center text-green-700">
              You&apos;re All Set!
            </h1>
            <p className="text-sm text-gray-600 text-center">
              Your access code has been created. Copy it now and save it
              somewhere safe — this is the only time it will be shown.
            </p>

            <div className="bg-gray-50 border border-gray-200 rounded-lg p-3">
              <div className="flex justify-between items-center mb-2">
                <span className="text-xs font-medium text-gray-500">
                  {createdKey.name}
                </span>
                <button
                  onClick={handleCopyCode}
                  aria-label={copied ? 'Access code copied' : 'Copy access code to clipboard'}
                  className="text-xs text-servo-700 hover:text-servo-900 font-medium"
                >
                  {copied ? 'Copied!' : 'Copy'}
                </button>
              </div>
              <code
                id="access-code-display"
                role="region"
                aria-label="Your access code"
                className="block text-xs font-mono bg-white p-2 rounded border break-all select-all"
              >
                {createdKey.plaintext_key}
              </code>
            </div>

            {storageWarning && (
              <div
                role="alert"
                className="bg-red-50 border border-red-200 rounded-lg p-3 text-sm text-red-800"
              >
                <p className="font-medium">Could not save automatically</p>
                <p className="text-xs mt-1">
                  Your browser blocked saving. Copy your code now — you
                  will need to paste it on the login page.
                </p>
              </div>
            )}

            <div className="bg-amber-50 border border-amber-200 rounded-lg p-3 text-sm text-amber-800">
              <p className="font-medium">Important:</p>
              <p className="text-xs mt-1">
                Write down this code or save it in a safe place. If you
                lose it, you&apos;ll need to create a new one from the
                server.
              </p>
            </div>

            <button
              onClick={handleFinish}
              className="btn-primary w-full"
            >
              Enter Arbor
            </button>
          </div>
        )}
      </div>
    </div>
  )
}
