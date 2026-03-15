/**
 * Login page.
 * Task: W07
 */

import { useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router-dom'
import { auth } from '../api/client'

export default function Login() {
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [accessCode, setAccessCode] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [mode, setMode] = useState<'accesscode' | 'login'>('accesscode')
  const navigate = useNavigate()

  const handleSubmit = async (e: FormEvent) => {
    e.preventDefault()
    setError(null)

    if (mode === 'accesscode') {
      if (!accessCode.trim()) {
        setError('Please enter your access code')
        return
      }
      localStorage.setItem('sb_token', accessCode.trim())
      navigate('/')
      return
    }

    try {
      const result = await auth.login(username, password)
      localStorage.setItem('sb_token', result.token)
      navigate('/')
    } catch (e) {
      setError((e as Error).message)
    }
  }

  return (
    <div className="min-h-screen flex items-center justify-center bg-gray-50">
      <div className="card w-full max-w-sm">
        <h1 className="text-xl font-bold text-center mb-6 text-servo-700">
          Arbor
        </h1>

        <div className="flex rounded-md bg-gray-100 p-1 mb-4">
          <button
            className={`flex-1 text-sm py-1.5 rounded ${mode === 'accesscode' ? 'bg-white shadow-sm font-medium' : 'text-gray-500'}`}
            onClick={() => setMode('accesscode')}
          >
            Access Code
          </button>
          <button
            className={`flex-1 text-sm py-1.5 rounded ${mode === 'login' ? 'bg-white shadow-sm font-medium' : 'text-gray-500'}`}
            onClick={() => setMode('login')}
          >
            Login
          </button>
        </div>

        <form onSubmit={handleSubmit} className="space-y-4">
          {mode === 'accesscode' ? (
            <div>
              <label
                htmlFor="access-code-input"
                className="block text-sm font-medium text-gray-700 mb-1"
              >
                Access Code
              </label>
              <input
                id="access-code-input"
                type="password"
                value={accessCode}
                onChange={(e) => setAccessCode(e.target.value)}
                className="input"
                placeholder="sb_..."
                autoFocus
              />
            </div>
          ) : (
            <>
              <div>
                <label
                  htmlFor="username-input"
                  className="block text-sm font-medium text-gray-700 mb-1"
                >
                  Username
                </label>
                <input
                  id="username-input"
                  type="text"
                  value={username}
                  onChange={(e) => setUsername(e.target.value)}
                  className="input"
                  autoFocus
                />
              </div>
              <div>
                <label
                  htmlFor="password-input"
                  className="block text-sm font-medium text-gray-700 mb-1"
                >
                  Password
                </label>
                <input
                  id="password-input"
                  type="password"
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                  className="input"
                />
              </div>
            </>
          )}

          {error && (
            <p role="alert" className="text-sm text-red-600">{error}</p>
          )}

          <button type="submit" className="btn-primary w-full">
            Connect
          </button>
        </form>
      </div>
    </div>
  )
}
