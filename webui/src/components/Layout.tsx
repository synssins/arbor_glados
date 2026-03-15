/**
 * Main layout — navbar with sidebar nav, connection status, emergency stop,
 * and Student/Expert mode toggle.
 * Task: W05, W16
 */

import { useEffect, useMemo } from 'react'
import { Outlet, NavLink, useNavigate } from 'react-router-dom'
import { useSystemStore } from '../stores/system'
import { useServoStore } from '../stores/servo'
import { useSensorStore } from '../stores/sensor'
import { useUIStore } from '../stores/ui'
import { ws } from '../api/ws'
import EmergencyStop from './EmergencyStop'
import ConnectionStatus from './ConnectionStatus'
import ModeToggle from './ModeToggle'

interface NavItem {
  to: string
  label: string
  /** If true, only visible in Expert mode */
  expertOnly?: boolean
}

const navItems: NavItem[] = [
  { to: '/', label: 'Dashboard' },
  { to: '/nodes', label: 'Nodes' },
  { to: '/servos', label: 'Servos' },
  { to: '/robots', label: 'Robots' },
  { to: '/klipper', label: 'Klipper', expertOnly: true },
  { to: '/sensors', label: 'Sensors' },
  { to: '/settings', label: 'Settings' },
  { to: '/config', label: 'Config', expertOnly: true },
  { to: '/docs', label: 'API Docs', expertOnly: true },
]

interface ExternalLink {
  href: string
  label: string
  expertOnly?: boolean
}

const externalLinks: ExternalLink[] = [
  { href: '/health', label: 'Health', expertOnly: true },
  { href: '/api/docs', label: 'Swagger', expertOnly: true },
  { href: '/moonraker/', label: 'Moonraker', expertOnly: true },
]

export default function Layout() {
  const info = useSystemStore((s) => s.info)
  const fetchInfo = useSystemStore((s) => s.fetchInfo)
  const fetchHealth = useSystemStore((s) => s.fetchHealth)
  const setWsConnected = useSystemStore((s) => s.setWsConnected)
  const updateServo = useServoStore((s) => s.updateFromEvent)
  const updateTemp = useSensorStore((s) => s.updateFromTempEvent)
  const updateEndstop = useSensorStore((s) => s.updateFromEndstopEvent)
  const navigate = useNavigate()
  const mode = useUIStore((s) => s.mode)
  const isExpert = mode === 'expert'

  // Filter nav items based on mode
  const visibleNavItems = useMemo(
    () => navItems.filter((item) => !item.expertOnly || isExpert),
    [isExpert],
  )
  const visibleExternalLinks = useMemo(
    () => externalLinks.filter((link) => !link.expertOnly || isExpert),
    [isExpert],
  )

  // Auto-redirect to setup wizard if system is in provisioning mode
  useEffect(() => {
    if (info?.provisioning_mode) {
      navigate('/setup', { replace: true })
    }
  }, [info, navigate])

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
      {/* Skip to main content — visible only on keyboard focus (WCAG 2.4.1) */}
      <a
        href="#main-content"
        className="sr-only focus:not-sr-only focus:fixed focus:z-50 focus:top-2 focus:left-2 focus:bg-white focus:px-4 focus:py-2 focus:rounded-md focus:shadow-lg focus:text-servo-700 focus:font-medium focus:ring-2 focus:ring-servo-500 focus:outline-none"
      >
        Skip to main content
      </a>

      {/* Navbar */}
      <header className="bg-white border-b border-gray-200 px-4 py-3 flex items-center justify-between sticky top-0 z-40">
        <div className="flex items-center gap-6">
          <h1 className="text-lg font-bold text-servo-700">{info?.display_name || 'Arbor'}</h1>
          <nav className="flex gap-1">
            {visibleNavItems.map((item) => (
              <NavLink
                key={item.to}
                to={item.to}
                className={({ isActive }) =>
                  `px-3 py-1.5 rounded-md text-sm font-medium transition-colors focus:outline-none focus:ring-2 focus:ring-servo-500 focus:ring-offset-1 ${
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
            <span className="text-xs text-gray-500 font-mono">v{info.firmware_version}</span>
          )}

          {/* External links divider — only show if there are visible links */}
          {visibleExternalLinks.length > 0 && (
            <>
              <div className="h-5 w-px bg-gray-200" />
              <div className="flex gap-2">
                {visibleExternalLinks.map((link) => (
                  <a
                    key={link.href}
                    href={link.href}
                    target="_blank"
                    rel="noopener noreferrer"
                    className="text-xs text-gray-500 hover:text-servo-600 transition-colors flex items-center gap-0.5"
                  >
                    {link.label}
                    <svg className="w-2.5 h-2.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
                      <path strokeLinecap="round" strokeLinejoin="round" d="M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14" />
                    </svg>
                  </a>
                ))}
              </div>
            </>
          )}
        </div>
        <div className="flex items-center gap-4">
          <ModeToggle />
          <ConnectionStatus />
          <EmergencyStop />
        </div>
      </header>

      {/* Content */}
      <main id="main-content" className="flex-1 p-6" tabIndex={-1}>
        <Outlet />
      </main>
    </div>
  )
}
