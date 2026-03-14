import { Routes, Route } from 'react-router-dom'
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
import ApiDocs from './views/ApiDocs'

export default function App() {
  return (
    <Routes>
      <Route path="/login" element={<Login />} />
      <Route element={<Layout />}>
        <Route path="/" element={<Dashboard />} />
        <Route path="/nodes" element={<Nodes />} />
        <Route path="/servos" element={<ServoControl />} />
        <Route path="/robots" element={<RobotControl />} />
        <Route path="/klipper" element={<KlipperStatus />} />
        <Route path="/sensors" element={<Sensors />} />
        <Route path="/settings" element={<Settings />} />
        <Route path="/config" element={<ConfigBrowser />} />
        <Route path="/docs" element={<ApiDocs />} />
      </Route>
    </Routes>
  )
}
