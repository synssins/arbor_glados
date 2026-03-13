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
  const [apiKey, setApiKey] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [mode, setMode] = useState<'login' | 'apikey'>('apikey')
  const navigate = useNavigate()

  const handleSubmit = async (e: FormEvent) => {
    e.preventDefault()
    setError(null)

    if (mode === 'apikey') {
      if (!apiKey.trim()) {
        setError('Please enter an API key')
        return
      }
      localStorage.setItem('sb_token', apiKey.trim())
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
            className={`flex-1 text-sm py-1.5 rounded ${mode === 'apikey' ? 'bg-white shadow-sm font-medium' : 'text-gray-500'}`}
            onClick={() => setMode('apikey')}
          >
            API Key
          </button>
          <button
            className={`flex-1 text-sm py-1.5 rounded ${mode === 'login' ? 'bg-white shadow-sm font-medium' : 'text-gray-500'}`}
            onClick={() => setMode('login')}
          >
            Login
          </button>
        </div>

        <form onSubmit={handleSubmit} className="space-y-4">
          {mode === 'apikey' ? (
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">
                API Key
              </label>
              <input
                type="password"
                value={apiKey}
                onChange={(e) => setApiKey(e.target.value)}
                className="input"
                placeholder="sb_..."
                autoFocus
              />
            </div>
          ) : (
            <>
              <div>
                <label className="block text-sm font-medium text-gray-700 mb-1">
                  Username
                </label>
                <input
                  type="text"
                  value={username}
                  onChange={(e) => setUsername(e.target.value)}
                  className="input"
                  autoFocus
                />
              </div>
              <div>
                <label className="block text-sm font-medium text-gray-700 mb-1">
                  Password
                </label>
                <input
                  type="password"
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                  className="input"
                />
              </div>
            </>
          )}

          {error && (
            <p className="text-sm text-red-600">{error}</p>
          )}

          <button type="submit" className="btn-primary w-full">
            Connect
          </button>
        </form>
      </div>
    </div>
  )
}
