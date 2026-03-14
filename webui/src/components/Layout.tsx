/**
 * Main layout — navbar with sidebar nav, connection status, emergency stop.
 * Task: W05
 */

import { useEffect } from 'react'
import { Outlet, NavLink } from 'react-router-dom'
import { useSystemStore } from '../stores/system'
import { useServoStore } from '../stores/servo'
import { useSensorStore } from '../stores/sensor'
import { ws } from '../api/ws'
import EmergencyStop from './EmergencyStop'
import ConnectionStatus from './ConnectionStatus'

const navItems = [
  { to: '/', label: 'Dashboard' },
  { to: '/nodes', label: 'Nodes' },
  { to: '/servos', label: 'Servos' },
  { to: '/robots', label: 'Robots' },
  { to: '/klipper', label: 'Klipper' },
  { to: '/sensors', label: 'Sensors' },
  { to: '/settings', label: 'Settings' },
  { to: '/docs', label: 'API Docs' },
]

const externalLinks = [
  { href: '/health', label: 'Health' },
  { href: '/api/docs', label: 'Swagger' },
  { href: '/moonraker/', label: 'Moonraker' },
]

export default function Layout() {
  const info = useSystemStore((s) => s.info)
  const fetchInfo = useSystemStore((s) => s.fetchInfo)
  const fetchHealth = useSystemStore((s) => s.fetchHealth)
  const setWsConnected = useSystemStore((s) => s.setWsConnected)
  const updateServo = useServoStore((s) => s.updateFromEvent)
  const updateTemp = useSensorStore((s) => s.updateFromTempEvent)
  const updateEndstop = useSensorStore((s) => s.updateFromEndstopEvent)

  useEffect(() => {
    // Fetch initial data
    fetchInfo()
    fetchHealth()

    // Connect WebSocket
    ws.onConnectionChange(setWsConnected)
    ws.connect()

    // Subscribe to events
    const unsubs = [
      ws.subscribe('servo.*', updateServo),
      ws.subscribe('sensor.temperature', updateTemp),
      ws.subscribe('sensor.endstop', updateEndstop),
    ]

    // Poll health every 10s
    const healthInterval = setInterval(fetchHealth, 10000)

    return () => {
      unsubs.forEach((u) => u())
      clearInterval(healthInterval)
      ws.disconnect()
    }
  }, [fetchInfo, fetchHealth, setWsConnected, updateServo, updateTemp, updateEndstop])

  return (
    <div className="min-h-screen flex flex-col">
      {/* Navbar */}
      <header className="bg-white border-b border-gray-200 px-4 py-3 flex items-center justify-between sticky top-0 z-40">
        <div className="flex items-center gap-6">
          <h1 className="text-lg font-bold text-servo-700">{info?.display_name || 'Arbor'}</h1>
          <nav className="flex gap-1">
            {navItems.map((item) => (
              <NavLink
                key={item.to}
                to={item.to}
                className={({ isActive }) =>
                  `px-3 py-1.5 rounded-md text-sm font-medium transition-colors ${
                    isActive
                      ? 'bg-servo-100 text-servo-700'
                      : 'text-gray-600 hover:text-gray-900 hover:bg-gray-100'
                  }`
                }
              >
                {item.label}
              </NavLink>
            ))}
          </nav>
          {info?.firmware_version && (
            <span className="text-xs text-gray-400 font-mono">v{info.firmware_version}</span>
          )}

          {/* External links divider */}
          <div className="h-5 w-px bg-gray-200" />
          <div className="flex gap-2">
            {externalLinks.map((link) => (
              <a
                key={link.href}
                href={link.href}
                target="_blank"
                rel="noopener noreferrer"
                className="text-xs text-gray-400 hover:text-servo-600 transition-colors flex items-center gap-0.5"
              >
                {link.label}
                <svg className="w-2.5 h-2.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
                  <path strokeLinecap="round" strokeLinejoin="round" d="M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14" />
                </svg>
              </a>
            ))}
          </div>
        </div>
        <div className="flex items-center gap-4">
          <ConnectionStatus />
          <EmergencyStop />
        </div>
      </header>

      {/* Content */}
      <main className="flex-1 p-6">
        <Outlet />
      </main>
    </div>
  )
}
