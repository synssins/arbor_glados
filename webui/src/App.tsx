import { Routes, Route, Navigate } from 'react-router-dom'
import Layout from './components/Layout'
import Dashboard from './views/Dashboard'
import Nodes from './views/Nodes'
import ServoControl from './views/ServoControl'
import RobotControl from './views/RobotControl'
import KlipperStatus from './views/KlipperStatus'
import Sensors from './views/Sensors'
import Settings from './views/Settings'
import ConfigBrowser from './views/ConfigBrowser'
import Login from './views/Login'
import FirstBootWizard from './views/FirstBootWizard'
import ApiDocs from './views/ApiDocs'
import { useIsExpert } from './stores/ui'

/**
 * Route guard — redirects to dashboard when an expert-only route
 * is accessed in Student mode.
 */
function ExpertRoute({ children }: { children: React.ReactNode }) {
  const isExpert = useIsExpert()
  if (!isExpert) return <Navigate to="/" replace />
  return <>{children}</>
}

export default function App() {
  return (
    <Routes>
      <Route path="/setup" element={<FirstBootWizard />} />
      <Route path="/login" element={<Login />} />
      <Route element={<Layout />}>
        <Route path="/" element={<Dashboard />} />
        <Route path="/nodes" element={<Nodes />} />
        <Route path="/servos" element={<ServoControl />} />
        <Route path="/robots" element={<RobotControl />} />
        <Route path="/klipper" element={<ExpertRoute><KlipperStatus /></ExpertRoute>} />
        <Route path="/sensors" element={<Sensors />} />
        <Route path="/settings" element={<Settings />} />
        <Route path="/config" element={<ExpertRoute><ConfigBrowser /></ExpertRoute>} />
        <Route path="/docs" element={<ExpertRoute><ApiDocs /></ExpertRoute>} />
      </Route>
    </Routes>
  )
}
